#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/mman.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do{if(!(x)){dprintf(2,"PIDNS IPC FAIL %d %s errno=%d\n",__LINE__,#x,errno);_exit(1);}}while(0)
union semun {int val;struct semid_ds *buf;unsigned short *array;};
static int semid,msgid,shmid,inner=-1,nested,index_child,wire[2];
static pid_t creator,previous;
static int procfd[2],pidwire[2];
static pid_t child_outer;
static void *shared_mapping;
static int shared_child(void *unused)
{ (void)unused;shared_mapping=shmat(shmid,NULL,0);CHECK(shared_mapping!=(void *)-1);return 0; }
static struct {long type;char data;} message={1,'x'};
static void proc_ids(const char *path,int expected_id,pid_t local_first,pid_t local_second,int message_queue)
{
    for(unsigned reopen=0;reopen<2;reopen++){
        FILE *f;
        if(reopen)f=fopen(path,"r");
        else {int copy=dup(procfd[message_queue?0:1]);CHECK(copy>=0&&lseek(copy,0,SEEK_SET)==0);f=fdopen(copy,"r");}
        CHECK(f);char *line=NULL;size_t capacity=0;int found=0;
        pid_t first=local_first,second=local_second;
        if(!reopen&&nested&&child_outer){
            first=message_queue?(local_first?child_outer:(index_child?previous:creator)):creator;
            second=local_second?child_outer:(message_queue?(index_child?previous:creator):creator);
        }
        while(getline(&line,&capacity,f)>0){int id;long long a,b,n;
            int parsed=message_queue?sscanf(line,"%*s %d %*s %*s %lld %lld %lld",&id,&n,&a,&b):
                sscanf(line,"%*s %d %*s %*s %lld %lld",&id,&a,&b);
            if(parsed==(message_queue?4:3)&&id==expected_id){
                if(a!=first||b!=second)dprintf(2,"PIDNS IPC PROC reopened=%u actual=%lld/%lld expected=%d/%d\n",reopen,a,b,first,second);
                CHECK(a==first&&b==second);found++;
            }
        }
        CHECK(found==1&&!fclose(f));free(line);
    }
}
static void stat_ids(pid_t expected_sem,pid_t expected_cpid,pid_t expected_lpid)
{
    struct msqid_ds m;struct shmid_ds s;
    CHECK(semctl(semid,0,GETPID)==expected_sem);
    CHECK(!msgctl(msgid,IPC_STAT,&m)&&m.msg_lspid==expected_sem&&m.msg_lrpid==expected_sem);
    CHECK(!shmctl(shmid,IPC_STAT,&s));
    if(s.shm_cpid!=expected_cpid||s.shm_lpid!=expected_lpid)
        dprintf(2,"PIDNS IPC SHM observer=%d actual=%d/%d expected=%d/%d\n",getpid(),s.shm_cpid,s.shm_lpid,expected_cpid,expected_lpid);
    CHECK(s.shm_cpid==expected_cpid&&s.shm_lpid==expected_lpid);
    proc_ids("/proc/sysvipc/msg",msgid,expected_sem,expected_sem,1);
    proc_ids("/proc/sysvipc/shm",shmid,expected_cpid,expected_lpid,0);
}
static int child(void *unused)
{
    (void)unused;if(nested)CHECK(getpid()==1);
    CHECK(read(pidwire[0],&child_outer,sizeof(child_outer))==sizeof(child_outer));
    /* fork's inherited VMA open is performed by the creator, not the new child. */
    stat_ids(nested?0:(index_child?previous:creator),nested?0:creator,nested?0:creator);
    CHECK(!semctl(semid,0,SETVAL,(union semun){.val=2}));
    CHECK(!msgsnd(msgid,&message,1,0)&&msgrcv(msgid,&message,1,0,0)==1);
    void *a=shmat(shmid,NULL,0);CHECK(a!=(void *)-1);CHECK(!shmdt(a));
    stat_ids(getpid(),nested?0:creator,getpid());
    if(!index_child){inner=shmget(IPC_PRIVATE,4096,0600);CHECK(inner>=0);CHECK(write(wire[1],&inner,sizeof(inner))==sizeof(inner));}
    else {
        struct shmid_ds s;CHECK(!shmctl(inner,IPC_STAT,&s)&&s.shm_cpid==(nested?0:previous));
        CHECK(shmat(inner,NULL,0)!=(void *)-1); /* exit closes this mapping */
    }
    /* inherited shmid mapping remains: asynchronous MM cleanup must record us */
    return 0;
}
int main(int argc,char **argv)
{
    if(argc==2&&!strcmp(argv[1],"--exec-child"))return 0;
    nested=argc==2&&!strcmp(argv[1],"--nested");creator=getpid();
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    semid=semget(IPC_PRIVATE,1,0600);msgid=msgget(IPC_PRIVATE,0600);shmid=shmget(IPC_PRIVATE,3*page,0600);
    CHECK(semid>=0&&msgid>=0&&shmid>=0&&!pipe(wire)&&!pipe(pidwire));
    procfd[0]=open("/proc/sysvipc/msg",O_RDONLY|O_CLOEXEC);procfd[1]=open("/proc/sysvipc/shm",O_RDONLY|O_CLOEXEC);
    CHECK(procfd[0]>=0&&procfd[1]>=0);
    CHECK(!semctl(semid,0,SETVAL,(union semun){.val=1}));
    CHECK(!msgsnd(msgid,&message,1,0)&&msgrcv(msgid,&message,1,0,0)==1);
    void *mapping=shmat(shmid,NULL,0);CHECK(mapping!=(void *)-1);
    for(index_child=0;index_child<2;index_child++){
        void *stack=NULL;pid_t p;
        if(nested){stack=malloc(1U<<20);CHECK(stack);p=clone(child,(char *)stack+(1U<<20),CLONE_NEWPID|SIGCHLD,NULL);}
        else {p=fork();if(!p)return child(NULL);}
        CHECK(p>0);CHECK(write(pidwire[1],&p,sizeof(p))==sizeof(p));int status;CHECK(waitpid(p,&status,0)==p&&WIFEXITED(status)&&!WEXITSTATUS(status));free(stack);
        if(!index_child)CHECK(read(wire[0],&inner,sizeof(inner))==sizeof(inner));
        stat_ids(p,creator,p);
        struct shmid_ds s;CHECK(!shmctl(inner,IPC_STAT,&s)&&s.shm_cpid==(index_child?previous:p));
        CHECK(s.shm_lpid==(index_child?p:0));previous=p;
    }
    /* VMA split/merge account the caller; do not preserve the last exiter. */
    CHECK(!mprotect((char *)mapping+page,page,PROT_READ));
    stat_ids(previous,creator,creator);
    CHECK(!mprotect((char *)mapping+page,page,PROT_READ|PROT_WRITE));
    struct shmid_ds before,after;CHECK(!shmctl(shmid,IPC_STAT,&before));
    void *stack=malloc(1U<<20);CHECK(stack);
    pid_t p=clone(shared_child,(char *)stack+(1U<<20),CLONE_VM|SIGCHLD,NULL);CHECK(p>0);
    int status;CHECK(waitpid(p,&status,0)==p&&WIFEXITED(status)&&!WEXITSTATUS(status));free(stack);
    CHECK(!shmctl(shmid,IPC_STAT,&after)&&after.shm_nattch==before.shm_nattch+1&&after.shm_lpid==p);
    CHECK(!shmdt(shared_mapping));stat_ids(previous,creator,creator);
    p=fork();CHECK(p>=0);
    if(!p){
        CHECK(!shmctl(shmid,IPC_STAT,&before));
        execl("/does-not-exist-pidns-ipc","absent",(char *)NULL);CHECK(errno==ENOENT);
        CHECK(!shmctl(shmid,IPC_STAT,&after)&&after.shm_nattch==before.shm_nattch&&after.shm_lpid==before.shm_lpid);
        execl(argv[0],argv[0],"--exec-child",(char *)NULL);CHECK(0);
    }
    CHECK(waitpid(p,&status,0)==p&&WIFEXITED(status)&&!WEXITSTATUS(status));
    stat_ids(previous,creator,p);
    CHECK(!shmdt(mapping)&&!shmctl(inner,IPC_RMID,NULL)&&!shmctl(shmid,IPC_RMID,NULL));
    CHECK(!semctl(semid,0,IPC_RMID)&&!msgctl(msgid,IPC_RMID,NULL));CHECK(!close(wire[0])&&!close(wire[1])&&!close(procfd[0])&&!close(procfd[1])&&!close(pidwire[0])&&!close(pidwire[1]));
    puts(nested?"PIDNS IPC PASS nested sibling sem msg shm retained identity exit":"PIDNS IPC PASS sem msg shm retained identity exit");return 0;
}
