#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int commands[2],events[2],failures;
static const char *self;
static void require(int okay,const char *what)
{if(!okay){printf("DUMP ERROR %s errno=%d\n",what,errno);fflush(stdout);_exit(99);}}
static void send_byte(int fd){require(write(fd,"x",1)==1,"pipe-write");}
static void take_byte(int fd){char c;require(read(fd,&c,1)==1,"pipe-read");}
static void drop(void)
{require(!setresgid(1000,1000,1000)&&!setresuid(1000,1000,1000),"drop");}
static void owner(pid_t pid,const char *name,unsigned expected)
{
    char path[80];struct stat info;
    snprintf(path,sizeof(path),"/proc/%d/status",pid);require(!stat(path,&info),"status-stat");
    printf("DUMP %s uid=%u gid=%u expected=%u\n",name,info.st_uid,info.st_gid,expected);
    failures+=(info.st_uid!=expected||info.st_gid!=expected);
    snprintf(path,sizeof(path),"/proc/%d",pid);require(!stat(path,&info),"directory-stat");
    require(info.st_uid==1000&&info.st_gid==1000,"directory-owner");fflush(stdout);
}
static void wait_zombie(pid_t child)
{siginfo_t info;require(!waitid(P_PID,child,&info,WEXITED|WNOWAIT)&&info.si_pid==child,"waitid-zombie");}
static void reap(pid_t child)
{int status;require(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status),"child-exit");}
static void ordinary(unsigned dump)
{
    require(!pipe(commands)&&!pipe(events),"pipes");fflush(stdout);pid_t child=fork();require(child>=0,"fork");
    if(!child){close(commands[1]);close(events[0]);drop();require(!prctl(PR_SET_DUMPABLE,dump),"set-dumpable");send_byte(events[1]);take_byte(commands[0]);_exit(0);}
    close(commands[0]);close(events[1]);take_byte(events[0]);owner(child,dump?"live-one":"live-zero",dump?1000:0);
    send_byte(commands[1]);wait_zombie(child);owner(child,dump?"zombie-one":"zombie-zero",dump?1000:0);reap(child);
    close(commands[1]);close(events[0]);
}
static void *survivor(void *unused)
{
    (void)unused;char path[80],text[1024];snprintf(path,sizeof(path),"/proc/%d/stat",getpid());
    unsigned attempt;
    for(attempt=0;attempt<100000;attempt++){
        int fd=open(path,O_RDONLY);require(fd>=0,"leader-stat-open");ssize_t n=read(fd,text,sizeof(text)-1);close(fd);require(n>0,"leader-stat-read");text[n]=0;
        char *end=strrchr(text,')');if(end&&end[1]==' '&&end[2]=='Z')break;sched_yield();
    }
    require(attempt<100000,"leader-is-zombie");
    require(!prctl(PR_SET_DUMPABLE,0),"survivor-zero");send_byte(events[1]);take_byte(commands[0]);
    require(!prctl(PR_SET_DUMPABLE,1),"survivor-one");send_byte(events[1]);take_byte(commands[0]);
    return 0;
}
static void leader(void)
{
    require(!pipe(commands)&&!pipe(events),"pipes");fflush(stdout);pid_t child=fork();require(child>=0,"fork");
    if(!child){close(commands[1]);close(events[0]);drop();pthread_t thread;require(!pthread_create(&thread,0,survivor,0),"pthread-create");syscall(SYS_exit,0);_exit(99);}
    close(commands[0]);close(events[1]);take_byte(events[0]);owner(child,"departed-leader-zero",0);send_byte(commands[1]);
    take_byte(events[0]);owner(child,"departed-leader-one",1000);send_byte(commands[1]);
    wait_zombie(child);owner(child,"group-zombie-one",1000);reap(child);close(commands[1]);close(events[0]);
}
static void vfork_case(void)
{
    fflush(stdout);pid_t child=fork();require(child>=0,"vfork-container");
    if(!child){drop();require(!prctl(PR_SET_DUMPABLE,1),"vfork-setup");pid_t nested=vfork();require(nested>=0,"vfork");
        if(!nested){if(prctl(PR_SET_DUMPABLE,0))_exit(99);_exit(0);}
        reap(nested);require(prctl(PR_GET_DUMPABLE)==0,"vfork-shares-state");
        nested=vfork();require(nested>=0,"vfork-exec");
        if(!nested){execl(self,self,"--probe",(char *)0);_exit(99);}
        reap(nested);require(prctl(PR_GET_DUMPABLE)==0,"exec-private-state");_exit(0);
    }
    reap(child);puts("DUMP vfork-share-and-exec PASS");
}
int main(int argc,char **argv)
{
    self=argv[0];if(argc>1&&!strcmp(argv[1],"--probe"))return prctl(PR_GET_DUMPABLE)==1?0:99;
    require(!mkdir("/proc",0555)||errno==EEXIST,"proc-dir");require(!mount("proc","/proc","proc",0,0)||errno==EBUSY,"proc-mount");
    ordinary(1);ordinary(0);leader();vfork_case();printf("DUMP END failures=%d\n",failures);return failures?1:0;
}
