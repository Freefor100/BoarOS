#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do {if(!(x)){dprintf(2,"IPCNS FAIL line=%d errno=%d %s\n",__LINE__,errno,#x);_exit(1);}}while(0)
#define ERROR(x,e) do {errno=0;CHECK((x)==-1&&errno==(e));}while(0)
union sem_argument {int val;struct semid_ds *buf;unsigned short *array;};
struct message {long type;char text[32];};
static key_t key;
static int semid,msgid,shmid,original_ns,procfd[3];
static size_t page;
static int *shared;
static mqd_t mq;
static char mqname[64];
static char clone_stack[65536] __attribute__((aligned(16)));
static unsigned cases;
static int unshare_ready[2],unshare_release[2];
static void passed(const char *name){printf("IPCNS CASE %s PASS\n",name);cases++;}
static void joined(pid_t child)
{int status;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));}
static void directory(const char *path)
{CHECK(!mkdir(path,0755)||errno==EEXIST);}
static int open_namespace(void)
{int fd=open("/ipc-proc/self/ns/ipc",O_RDONLY|O_CLOEXEC);CHECK(fd>=0);return fd;}
static int proc_key(int fd,key_t wanted)
{
    char text[8192];CHECK(lseek(fd,0,SEEK_SET)==0);ssize_t size=read(fd,text,sizeof(text)-1);CHECK(size>=0);text[size]=0;
    for(char *at=text;at&&*at;){long value;char *next=strchr(at,'\n');if(next)*next++=0;
        if(sscanf(at,"%ld",&value)==1&&value==(long)wanted)return 1;
        at=next;}
    return 0;
}
static int proc_open(unsigned index)
{
    const char *names[]={"/ipc-proc/sysvipc/sem","/ipc-proc/sysvipc/msg","/ipc-proc/sysvipc/shm"};
    int fd=open(names[index],O_RDONLY);CHECK(fd>=0);return fd;
}
static void isolation_child(void)
{
    CHECK(!close(unshare_ready[0])&&!close(unshare_release[1]));
    struct sembuf undo={0,-1,SEM_UNDO};CHECK(!semop(semid,&undo,1));
    CHECK(!unshare(CLONE_NEWIPC));
    char token=1;CHECK(write(unshare_ready[1],&token,1)==1&&read(unshare_release[0],&token,1)==1);
    CHECK(!close(unshare_ready[1])&&!close(unshare_release[0]));
    ERROR(semget(key,1,0),ENOENT);ERROR(msgget(key,0),ENOENT);ERROR(shmget(key,0,0),ENOENT);
    ERROR(mq_open(mqname,O_RDWR),ENOENT);
    for(unsigned i=0;i<3;i++){
        CHECK(proc_key(procfd[i],key));int fresh=proc_open(i);CHECK(!proc_key(fresh,key)&&!close(fresh));
        CHECK(proc_key(procfd[i],key));CHECK(!close(procfd[i]));
    }
    int s=semget(key,1,IPC_CREAT|IPC_EXCL|0600),m=msgget(key,IPC_CREAT|IPC_EXCL|0600);
    int h=shmget(key,page,IPC_CREAT|IPC_EXCL|0600);CHECK(s>=0&&m>=0&&h>=0);
    union sem_argument value={.val=19};CHECK(!semctl(s,0,SETVAL,value)&&semctl(s,0,GETVAL)==19);
    int *fresh=shmat(h,0,0);CHECK(fresh!=(void *)-1&&*shared==41);*shared=42;*fresh=99;
    pid_t child=fork();CHECK(child>=0);
    if(!child){
        CHECK(*shared==42&&*fresh==99);
        int *duplicate=mremap(shared,0,page,MREMAP_MAYMOVE);
        CHECK(duplicate!=MAP_FAILED&&duplicate!=shared&&*duplicate==42&&*shared==42);
        CHECK(!shmdt(shared)&&*duplicate==42&&!shmdt(duplicate)&&!shmdt(fresh));_exit(0);
    }
    joined(child);
    struct message item={.type=7,.text="new"};CHECK(!msgsnd(m,&item,4,0));
    CHECK(msgrcv(m,&item,sizeof(item.text),7,0)==4&&!strcmp(item.text,"new"));
    char data[64];CHECK(mq_receive(mq,data,sizeof(data),0)==5&&!strcmp(data,"root"));
    CHECK(!mq_send(mq,"child",6,3));
    mqd_t own=mq_open(mqname,O_CREAT|O_EXCL|O_RDWR,0600,&(struct mq_attr){.mq_maxmsg=2,.mq_msgsize=64});CHECK(own!=(mqd_t)-1);
    struct mq_attr attr;CHECK(!mq_getattr(own,&attr)&&!attr.mq_curmsgs);
    CHECK(!mq_close(own)&&!mq_unlink(mqname));
    CHECK(!shmctl(h,IPC_RMID,0));
    int *again=shmat(h,0,0);CHECK(again!=(void *)-1&&*again==99);
    CHECK(!shmdt(again)&&!shmdt(fresh)&&!shmdt(shared));
    struct shmid_ds ds;ERROR(shmctl(h,IPC_STAT,&ds),EINVAL);
    CHECK(!semctl(s,0,IPC_RMID)&&!msgctl(m,IPC_RMID,0));
    CHECK(!setns(original_ns,CLONE_NEWIPC));CHECK(semctl(semid,0,GETVAL)==5);
    CHECK(!mq_close(mq));_exit(0);
}
static int cloned_space(void *unused)
{
    (void)unused;CHECK(*shared==42);
    ERROR(semget(key,1,0),ENOENT);ERROR(msgget(key,0),ENOENT);ERROR(shmget(key,0,0),ENOENT);
    CHECK(!shmdt(shared));return 0;
}
static void clone_and_flags(void)
{
    pid_t child=clone(cloned_space,clone_stack+sizeof(clone_stack),CLONE_NEWIPC|SIGCHLD,0);CHECK(child>=0);joined(child);
    ERROR(clone(cloned_space,clone_stack+sizeof(clone_stack),CLONE_NEWIPC|CLONE_SYSVSEM|SIGCHLD,0),EINVAL);
    ERROR(setns(original_ns,CLONE_NEWUTS),EINVAL);
    ERROR(unshare(CLONE_NEWIPC|0x1000),EINVAL); /* CLONE_PIDFD is not an unshare operation. */
    CHECK(semctl(semid,0,GETVAL)==5);
    passed("clone-flags-and-inherited-attachments");
}
static void mount_context_child(void)
{
    CHECK(!unshare(CLONE_NEWIPC));int first=open_namespace();
    int context=(int)syscall(SYS_fsopen,"mqueue",1);CHECK(context>=0);
    CHECK(!unshare(CLONE_NEWIPC));int second=open_namespace();
    CHECK(!syscall(SYS_fsconfig,context,6,0,0,0));
    int tree=(int)syscall(SYS_fsmount,context,1,0);CHECK(tree>=0);
    CHECK(!syscall(SYS_move_mount,tree,"",AT_FDCWD,"/ipc-mq-old",4)&&!close(tree));
    int pathfd=open("/ipc-mq-old/captured",O_CREAT|O_EXCL|O_RDWR,0600);CHECK(pathfd>=0);
    ERROR(mq_open("/captured",O_RDWR),ENOENT);
    CHECK(!setns(first,CLONE_NEWIPC));mqd_t old=mq_open("/captured",O_RDWR);CHECK(old!=(mqd_t)-1);
    struct stat one,two;CHECK(!fstat(pathfd,&one)&&!fstat((int)old,&two)&&one.st_ino==two.st_ino);
    CHECK(!setns(second,CLONE_NEWIPC)&&!close(first)&&!close(context));
    ERROR(open("/ipc-mq-old/new",O_CREAT|O_RDWR,0600),EACCES);
    sigset_t blocked;sigemptyset(&blocked);sigaddset(&blocked,SIGUSR1);CHECK(!sigprocmask(SIG_BLOCK,&blocked,0));
    struct sigevent event={.sigev_notify=SIGEV_SIGNAL,.sigev_signo=SIGUSR1,.sigev_value.sival_int=1234};
    CHECK(!mq_notify(old,&event)&&!mq_send(old,"orphan",7,1));
    siginfo_t info;CHECK(sigtimedwait(&blocked,&info,&(struct timespec){0,0})==SIGUSR1);
    CHECK(info.si_code==SI_MESGQ&&info.si_value.sival_int==1234);
    char data[8192];CHECK(mq_receive(old,data,sizeof(data),0)==7&&!strcmp(data,"orphan"));
    CHECK(!unlink("/ipc-mq-old/captured")&&!close(pathfd));
    CHECK(!umount("/ipc-mq-old"));
    CHECK(!mq_send(old,"fd",3,0)&&mq_receive(old,data,sizeof(data),0)==3);
    CHECK(!mq_close(old)&&!close(second));_exit(0);
}
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);page=(size_t)sysconf(_SC_PAGESIZE);CHECK(page>0);
    directory("/ipc-proc");directory("/ipc-mq-old");CHECK(!mount("proc","/ipc-proc","proc",0,0));
    original_ns=open_namespace();key=(key_t)(0x43000000U+(unsigned)getpid());
    CHECK(snprintf(mqname,sizeof(mqname),"/ipcns-%u",(unsigned)getpid())>0);
    semid=semget(key,1,IPC_CREAT|IPC_EXCL|0600);msgid=msgget(key,IPC_CREAT|IPC_EXCL|0600);
    shmid=shmget(key,page,IPC_CREAT|IPC_EXCL|0600);CHECK(semid>=0&&msgid>=0&&shmid>=0);
    CHECK(!semctl(semid,0,SETVAL,(union sem_argument){.val=5}));
    shared=shmat(shmid,0,0);CHECK(shared!=(void *)-1);*shared=41;
    struct message item={.type=1,.text="original"};CHECK(!msgsnd(msgid,&item,9,0));
    mq=mq_open(mqname,O_CREAT|O_EXCL|O_RDWR,0600,&(struct mq_attr){.mq_maxmsg=2,.mq_msgsize=64});CHECK(mq!=(mqd_t)-1);
    CHECK(!mq_send(mq,"root",5,0));for(unsigned i=0;i<3;i++)procfd[i]=proc_open(i);
    CHECK(!pipe(unshare_ready)&&!pipe(unshare_release));
    pid_t child=fork();CHECK(child>=0);if(!child)isolation_child();
    CHECK(!close(unshare_ready[1])&&!close(unshare_release[0]));
    char token;CHECK(read(unshare_ready[0],&token,1)==1&&semctl(semid,0,GETVAL)==5);
    CHECK(write(unshare_release[1],&token,1)==1&&!close(unshare_ready[0])&&!close(unshare_release[1]));
    joined(child);
    CHECK(*shared==42&&semctl(semid,0,GETVAL)==5);
    CHECK(msgrcv(msgid,&item,sizeof(item.text),1,0)==9&&!strcmp(item.text,"original"));
    char data[64];CHECK(mq_receive(mq,data,sizeof(data),0)==6&&!strcmp(data,"child"));
    for(unsigned i=0;i<3;i++)CHECK(!close(procfd[i]));
    passed("isolation-semundo-shm-fork-mremap-proc-open-and-mq-fd");
    clone_and_flags();
    child=fork();CHECK(child>=0);if(!child)mount_context_child();joined(child);
    passed("fsopen-capture-orphan-mount-and-notification");
    CHECK(!mq_close(mq)&&!mq_unlink(mqname)&&!semctl(semid,0,IPC_RMID)&&!msgctl(msgid,IPC_RMID,0));
    CHECK(!shmdt(shared)&&!shmctl(shmid,IPC_RMID,0)&&!close(original_ns)&&!umount("/ipc-proc"));
    CHECK(!rmdir("/ipc-mq-old")&&!rmdir("/ipc-proc"));
    printf("IPCNS PASS cases=%u\n",cases);return 0;
}
