#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifndef P_PIDFD
#define P_PIDFD 3
#endif
#define OPEN(pid,flags) syscall(434,(pid),(flags))
#define SEND(fd,sig,info,flags) syscall(424,(fd),(sig),(info),(flags))
#define GETFD(fd,target,flags) syscall(438,(fd),(target),(flags))
#define THREAD 0200U
#define CHECK(x) do { if(!(x)){fprintf(stderr,"PIDFD FAIL line=%d errno=%d %s\n",__LINE__,errno,#x);exit(1);} }while(0)
#define ERROR(x,e) do {errno=0;CHECK((x)==-1 && errno==(e));}while(0)
static unsigned cases;
static void pass(const char *s){cases++;printf("PIDFD CASE %s PASS\n",s);}
static void token(int fd,char value){CHECK(write(fd,&value,1)==1);}
static void expect(int fd,char value){char b;CHECK(read(fd,&b,1)==1 && b==value);}
static uint64_t now(void){struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000000000U+t.tv_nsec;}
static unsigned ready(int fd)
{struct pollfd p={fd,POLLIN,0};CHECK(poll(&p,1,0)>=0);return (unsigned)p.revents;}
static void join(pid_t p,int code)
{int s;CHECK(waitpid(p,&s,0)==p && WIFEXITED(s) && WEXITSTATUS(s)==code);}
static void fdinfo(int fd,int expected)
{
    char path[64],text[512];snprintf(path,sizeof(path),"/proc/self/fdinfo/%d",fd);
    int in=open(path,O_RDONLY);CHECK(in>=0);ssize_t n=read(in,text,sizeof(text)-1);CHECK(n>0);text[n]=0;CHECK(!close(in));
    char *p=strstr(text,"Pid:\t"),*ns=strstr(text,"NSpid:\t");
    CHECK(p && strtol(p+5,0,10)==expected);
    if(ns)CHECK(strtol(ns+7,0,10)==expected); /* Linux omits it without CONFIG_PID_NS. */
    CHECK(strstr(text,"pos:\t0\n") && strstr(text,"flags:\t") && strstr(text,"mnt_id:\t") && strstr(text,"ino:\t"));
}
static void abi(void)
{
    ERROR(OPEN(0,0),EINVAL);ERROR(OPEN(-1,0),EINVAL);
    ERROR(OPEN(getpid(),1),EINVAL);ERROR(OPEN(0,1),EINVAL);
    ERROR(SEND(-1,0,0,3),EINVAL);ERROR(SEND(-1,0,0,0),EBADF);
    ERROR(GETFD(-1,0,1),EINVAL);ERROR(GETFD(-1,0,0),EBADF);
    CHECK(!SEND(-10000,0,0,0) && !SEND(-10001,0,0,0));
    ERROR(GETFD(-10000,0,0),EBADF);
    int fd=(int)OPEN(getpid(),0),again=(int)OPEN(getpid(),0);CHECK(fd>=0 && again>=0);
    CHECK(fcntl(fd,F_GETFD)==FD_CLOEXEC);
    struct stat a,b;CHECK(!fstat(fd,&a) && !fstat(again,&b));
    CHECK(a.st_mode==0700 && b.st_mode==0700 && a.st_ino==b.st_ino);
    char path[64],link[64];snprintf(path,sizeof(path),"/proc/self/fd/%d",fd);
    ssize_t n=readlink(path,link,sizeof(link)-1);CHECK(n>0);link[n]=0;
    CHECK(!strcmp(link,"anon_inode:[pidfd]"));fdinfo(fd,getpid());
    char byte;ERROR(read(fd,&byte,1),EINVAL);ERROR(write(fd,&byte,1),EINVAL);
    ERROR(pread(fd,&byte,1,0),EINVAL);ERROR(lseek(fd,0,SEEK_SET),ESPIPE);
    ERROR(syscall(SYS_pwrite64,fd,UINTPTR_MAX,1,0),EINVAL);
    ERROR(GETFD(fd,INT32_MAX,0),EBADF);
    CHECK(!close(fd) && !close(again));pass("abi-self-stat-fdinfo");
}
static void lifecycle(int file)
{
    int gate[2];CHECK(!pipe(gate));pid_t child=fork();CHECK(child>=0);
    if(!child){CHECK(!close(gate[1]));expect(gate[0],'x');_exit(17);}
    CHECK(!close(gate[0]));
    int fd=(int)OPEN(child,O_NONBLOCK);CHECK(fd>=0 && !ready(fd));fdinfo(fd,child);
    siginfo_t info;memset(&info,0x5a,sizeof(info));
    ERROR(waitid(P_PIDFD,fd,&info,WEXITED),EAGAIN);CHECK(!info.si_pid && !info.si_signo);
    CHECK(!waitid(P_PIDFD,fd,&info,WEXITED|WNOHANG) && !info.si_pid);
    CHECK(!SEND(fd,0,0,0));
    int copy=(int)GETFD(fd,file,0);CHECK(copy>=0 && fcntl(copy,F_GETFD)==FD_CLOEXEC);
    CHECK(lseek(copy,2,SEEK_SET)==2 && lseek(file,0,SEEK_CUR)==2);
    char bytes[4]={0};CHECK(pread(copy,bytes,3,0)==3 && !memcmp(bytes,"ofd",3));CHECK(!close(copy));
    int ep=epoll_create1(EPOLL_CLOEXEC);CHECK(ep>=0);struct epoll_event event={.events=EPOLLIN,.data.u64=42};
    CHECK(!epoll_ctl(ep,EPOLL_CTL_ADD,fd,&event));
    token(gate[1],'x');CHECK(!close(gate[1]));
    CHECK(epoll_wait(ep,&event,1,5000)==1 && event.data.u64==42 && (event.events&EPOLLIN));
    CHECK((ready(fd)&(POLLIN|POLLHUP))==POLLIN);
    CHECK(!waitid(P_PIDFD,fd,&info,WEXITED|WNOWAIT) && info.si_pid==child && info.si_status==17);
    fdinfo(fd,child);
    CHECK(!waitid(P_PIDFD,fd,&info,WEXITED) && info.si_pid==child && info.si_status==17);
    CHECK((ready(fd)&(POLLIN|POLLHUP))==(POLLIN|POLLHUP));fdinfo(fd,-1);
    ERROR(SEND(fd,0,0,0),ESRCH);ERROR(GETFD(fd,file,0),ESRCH);
    ERROR(waitid(P_PIDFD,fd,&info,WEXITED),ECHILD);
    CHECK(!close(ep) && !close(fd));pass("getfd-shared-ofd-nonblock-wait-exit-reap");
}
static void zombie(void)
{
    pid_t child=fork();CHECK(child>=0);if(!child)_exit(9);
    siginfo_t info;CHECK(!waitid(P_PID,child,&info,WEXITED|WNOWAIT) && info.si_pid==child);
    int fd=(int)OPEN(child,0);CHECK(fd>=0 && (ready(fd)&POLLIN));CHECK(!SEND(fd,0,0,0));
    int thread=(int)OPEN(child,THREAD);CHECK(thread>=0 && !SEND(thread,SIGUSR1,0,0));
    join(child,9);CHECK(ready(fd)&POLLHUP);fdinfo(fd,-1);CHECK(!close(fd));
    ERROR(SEND(thread,SIGUSR1,0,0),ESRCH);CHECK(!close(thread));
    pass("open-zombie-and-retained-detached-identity");
}
struct worker {int ready,gate;};
static void *member(void *arg)
{
    struct worker *w=arg;int tid=(int)syscall(SYS_gettid);
    CHECK(!SEND(-10000,0,0,2));
    CHECK(write(w->ready,&tid,sizeof(tid))==sizeof(tid));expect(w->gate,'x');
    sigset_t set;sigemptyset(&set);sigaddset(&set,SIGUSR1);siginfo_t info;
    CHECK(sigwaitinfo(&set,&info)==SIGUSR1 && info.si_code==SI_USER);
    return 0;
}
static void leader_exit(int file)
{
    int reports[2],gate[2];CHECK(!pipe(reports) && !pipe(gate));
    pid_t child=fork();CHECK(child>=0);
    if(!child) {
        close(reports[0]);close(gate[1]);struct worker w={reports[1],gate[0]};pthread_t t;
        sigset_t blocked;sigemptyset(&blocked);sigaddset(&blocked,SIGUSR1);
        CHECK(!sigprocmask(SIG_BLOCK,&blocked,0));
        CHECK(!pthread_create(&t,0,member,&w));syscall(SYS_exit,0);_exit(127);
    }
    close(reports[1]);close(gate[0]);int tid;CHECK(read(reports[0],&tid,sizeof(tid))==sizeof(tid));close(reports[0]);
    int process=(int)OPEN(child,0),thread=(int)OPEN(tid,THREAD|O_NONBLOCK);CHECK(process>=0 && thread>=0);
    ERROR(OPEN(tid,0),ENOENT);
    char path[64],text[256];snprintf(path,sizeof(path),"/proc/%d/stat",child);uint64_t end=now()+UINT64_C(5000000000);
    for(;;) {
        int in=open(path,O_RDONLY);CHECK(in>=0);ssize_t n=read(in,text,sizeof(text)-1);CHECK(n>0);text[n]=0;close(in);
        char *name=strrchr(text,')');CHECK(name && name[1]==' ');
        if(name[2]=='Z')break;
        CHECK(now()<end && !sched_yield());
    }
    CHECK(!ready(process) && !ready(thread));
    int copy=(int)GETFD(thread,file,0);CHECK(copy>=0);CHECK(!close(copy));
    ERROR(GETFD(process,file,0),ESRCH);
    siginfo_t info;ERROR(waitid(P_PIDFD,thread,&info,WEXITED|WNOHANG),ECHILD);
    CHECK(!SEND(thread,0,0,0) && !SEND(thread,0,0,2));
    CHECK(!SEND(thread,SIGUSR1,0,2));token(gate[1],'x');close(gate[1]);
    CHECK(!waitid(P_PIDFD,process,&info,WEXITED) && info.si_pid==child);
    CHECK(ready(process)&POLLHUP);CHECK(ready(thread)&POLLHUP);
    CHECK(!close(thread) && !close(process));pass("thread-target-and-last-thread-process-readiness");
}
static void payload(void)
{
    int signal=SIGRTMIN+2;sigset_t set,old;sigemptyset(&set);sigaddset(&set,signal);
    CHECK(!sigprocmask(SIG_BLOCK,&set,&old));
    int fd=(int)OPEN(getpid(),0);CHECK(fd>=0);
    siginfo_t input={0},output={0};input.si_signo=signal;input.si_code=SI_QUEUE;
    input.si_pid=1234;input.si_uid=2345;input.si_value.sival_ptr=(void *)(uintptr_t)UINT64_C(0x123456789abcdef);
    CHECK(!SEND(fd,signal,&input,0));CHECK(sigwaitinfo(&set,&output)==signal &&
        output.si_pid==1234 && output.si_uid==2345 && output.si_value.sival_ptr==input.si_value.sival_ptr);
    input.si_signo=signal+1;ERROR(SEND(fd,signal,&input,0),EINVAL);
    CHECK(!close(fd) && !sigprocmask(SIG_SETMASK,&old,0));pass("queued-payload-and-scope-validation");
}
static void permissions(int file)
{
    pid_t parent=getpid(),child=fork();CHECK(child>=0);
    if(!child) {
        CHECK(!setgroups(0,0) && !setgid(1001) && !setuid(1001));
        int target=(int)OPEN(parent,0);CHECK(target>=0);
        ERROR(GETFD(target,file,0),EPERM);ERROR(SEND(target,0,0,0),EPERM);
        int own=(int)OPEN(getpid(),0);CHECK(own>=0);
        int copy=(int)GETFD(own,file,0);CHECK(copy>=0);
        CHECK(!close(copy) && !close(own) && !close(target));_exit(0);
    }
    join(child,0);pass("realcreds-numeric-user-and-existing-fd-rights");
}
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);alarm(45);
    if(access("/proc/self/stat",F_OK)) {if(mkdir("/proc",0755))CHECK(errno==EEXIST);CHECK(!mount("proc","/proc","proc",0,0));}
    int file=open("/pidfd-ofd",O_CREAT|O_RDWR|O_TRUNC,0600);CHECK(file>=0 && write(file,"ofd",3)==3);
    abi();lifecycle(file);zombie();leader_exit(file);payload();permissions(file);
    CHECK(!close(file) && !unlink("/pidfd-ofd"));printf("PIDFD END %u\n",cases);alarm(0);
    if(getpid()==1){sync();reboot(RB_POWER_OFF);}return 0;
}
