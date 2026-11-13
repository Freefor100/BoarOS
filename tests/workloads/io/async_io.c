#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if(!(x)){dprintf(2,"ASYNC FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);} } while(0)
static int signo,signal_fd;
static sigset_t blocked;
static struct signalfd_siginfo next_signal(void)
{
    struct pollfd p={.fd=signal_fd,.events=POLLIN};
    CHECK(poll(&p,1,5000)==1 && (p.revents&POLLIN));
    struct signalfd_siginfo info;CHECK(read(signal_fd,&info,sizeof(info))==sizeof(info));
    return info;
}
static void empty(void)
{ struct signalfd_siginfo info;errno=0;CHECK(read(signal_fd,&info,sizeof(info))==-1 && errno==EAGAIN); }
static void enable(int fd)
{
    CHECK(!fcntl(fd,F_SETOWN,getpid()) && !fcntl(fd,F_SETSIG,signo));
    int flags=fcntl(fd,F_GETFL);CHECK(flags>=0 && !fcntl(fd,F_SETFL,flags|O_ASYNC));
}
static void input_signal(int fd)
{ struct signalfd_siginfo info=next_signal();CHECK(info.ssi_signo==(unsigned)signo && info.ssi_code==POLL_IN && info.ssi_fd==fd && (info.ssi_band&POLLIN)); }
static void owners_and_pipe(void)
{
    int p[2];CHECK(!pipe(p));CHECK(fcntl(p[0],F_GETOWN)==0 && fcntl(p[0],F_GETSIG)==0);
    struct f_owner_ex owner={.type=F_OWNER_TID,.pid=(pid_t)syscall(SYS_gettid)},out={0};
    CHECK(!fcntl(p[0],F_SETOWN_EX,&owner) && !fcntl(p[0],F_GETOWN_EX,&out));
    CHECK(out.type==owner.type && out.pid==owner.pid);
    owner.type=99;errno=0;CHECK(fcntl(p[0],F_SETOWN_EX,&owner)==-1 && errno==EINVAL);
    errno=0;CHECK(fcntl(p[0],F_SETSIG,65)==-1 && errno==EINVAL);
    enable(p[0]);int original=p[0],alias=dup(p[0]);CHECK(alias>=0);
    CHECK(!fcntl(alias,F_SETFL,fcntl(alias,F_GETFL)|O_NONBLOCK));
    CHECK(!close(p[0]) && write(p[1],"ab",2)==2);input_signal(original);
    char data[2];CHECK(read(alias,data,2)==2 && !memcmp(data,"ab",2));
    CHECK(write(p[1],"c",1)==1 && write(p[1],"d",1)==1);
    input_signal(original);input_signal(original);CHECK(read(alias,data,2)==2);
    CHECK(!fcntl(alias,F_SETFL,fcntl(alias,F_GETFL)&~O_ASYNC));
    CHECK(write(p[1],"e",1)==1);empty();CHECK(read(alias,data,1)==1);
    CHECK(!close(alias) && !close(p[1]));
    int file=open("/init",O_RDONLY);CHECK(file>=0);enable(file);
    CHECK(!(fcntl(file,F_GETFL)&O_ASYNC) && !close(file));empty();
    puts("ASYNC CASE owners-pipe PASS");
}
static volatile sig_atomic_t handler_count,handler_fd,handler_band;
static void handler(int sig,siginfo_t *info,void *context)
{ (void)context;if(sig==SIGUSR1){handler_fd=info->si_fd;handler_band=(sig_atomic_t)info->si_band;handler_count++;} }
static void consumers_and_quota(void)
{
    int p[2];CHECK(!pipe(p));enable(p[0]);
    CHECK(write(p[1],"w",1)==1);siginfo_t info={0};struct timespec timeout={5,0};
    sigset_t one;sigemptyset(&one);sigaddset(&one,signo);
    CHECK(sigtimedwait(&one,&info,&timeout)==signo && info.si_fd==p[0] && info.si_code==POLL_IN);
    char byte;CHECK(read(p[0],&byte,1)==1);
    struct sigaction action={.sa_sigaction=handler,.sa_flags=SA_SIGINFO};sigemptyset(&action.sa_mask);
    CHECK(!sigaction(SIGUSR1,&action,NULL) && !fcntl(p[0],F_SETSIG,SIGUSR1));
    CHECK(write(p[1],"h",1)==1);sigemptyset(&one);sigaddset(&one,SIGUSR1);
    CHECK(!sigprocmask(SIG_UNBLOCK,&one,NULL));
    CHECK(handler_count==1 && handler_fd==p[0] && (handler_band&POLLIN));
    CHECK(!sigprocmask(SIG_BLOCK,&one,NULL) && read(p[0],&byte,1)==1);
    CHECK(!fcntl(p[0],F_SETSIG,signo));
    struct rlimit saved,zero={0,0};CHECK(!getrlimit(RLIMIT_SIGPENDING,&saved));zero.rlim_max=saved.rlim_max;
    CHECK(!setrlimit(RLIMIT_SIGPENDING,&zero) && write(p[1],"q",1)==1);
    struct signalfd_siginfo got=next_signal();CHECK(got.ssi_signo==SIGIO && got.ssi_code==SI_KERNEL);
    CHECK(!setrlimit(RLIMIT_SIGPENDING,&saved) && read(p[0],&byte,1)==1);
    CHECK(!fcntl(p[0],F_SETFL,fcntl(p[0],F_GETFL)&~O_ASYNC));
    CHECK(!close(p[0]) && !close(p[1]));empty();
    puts("ASYNC CASE consumers-quota PASS");
}
static void sockets_and_inotify(void)
{
    int s[2];CHECK(!socketpair(AF_UNIX,SOCK_DGRAM,0,s));enable(s[1]);
    int owner=0,wanted=getpid();CHECK(!ioctl(s[1],0x8903,&owner) && owner==wanted);
    CHECK(!ioctl(s[1],0x8901,&wanted) && fcntl(s[1],F_GETOWN)==wanted);
    CHECK(send(s[0],"s",1,0)==1);input_signal(s[1]);char byte;CHECK(recv(s[1],&byte,1,0)==1);
    CHECK(!fcntl(s[1],F_SETFL,fcntl(s[1],F_GETFL)&~O_ASYNC) && !close(s[0]) && !close(s[1]));empty();
    const char *path="/tmp/async-watch";CHECK(!mkdir(path,0700) || errno==EEXIST);
    int fd=inotify_init1(IN_NONBLOCK|IN_CLOEXEC);CHECK(fd>=0 && inotify_add_watch(fd,path,IN_CREATE)>=0);enable(fd);
    int a=open("/tmp/async-watch/a",O_CREAT|O_WRONLY,0600),b=open("/tmp/async-watch/b",O_CREAT|O_WRONLY,0600);CHECK(a>=0 && b>=0);
    input_signal(fd);input_signal(fd);
    char events[512];CHECK(read(fd,events,sizeof(events))>0);
    CHECK(!fcntl(fd,F_SETFL,fcntl(fd,F_GETFL)&~O_ASYNC) && !close(fd));
    CHECK(!close(a)&&!close(b)&&!unlink("/tmp/async-watch/a")&&!unlink("/tmp/async-watch/b")&&!rmdir(path));empty();
    puts("ASYNC CASE socket-inotify PASS");
}
struct remote {int fd,cpu,observed;};
static void *remote_writer(void *argument)
{
    struct remote *r=argument;cpu_set_t set;CPU_ZERO(&set);CPU_SET(r->cpu,&set);
    CHECK(!sched_setaffinity(0,sizeof(set),&set));r->observed=sched_getcpu();CHECK(write(r->fd,"r",1)==1);return NULL;
}
struct identity_thread { int notify,ready,release; };
static void *nonleader_owner(void *argument)
{
    struct identity_thread *t=argument;
    CHECK(!syscall(SYS_setresuid,1000,1000,0));
    struct f_owner_ex owner={.type=F_OWNER_PID,.pid=(pid_t)syscall(SYS_gettid)};
    CHECK(!fcntl(t->notify,F_SETOWN_EX,&owner) && write(t->ready,"r",1)==1);
    char byte;CHECK(read(t->release,&byte,1)==1);
    CHECK(!syscall(SYS_setresuid,0,0,0));return NULL;
}
static void continued_notification(void)
{
    pid_t intermediate=fork();CHECK(intermediate>=0);
    if(!intermediate) {
        CHECK(!setresuid(1000,1000,1000));
        sigset_t set;sigemptyset(&set);sigaddset(&set,SIGCHLD);CHECK(!sigprocmask(SIG_BLOCK,&set,NULL));
        struct sigaction action={.sa_handler=SIG_DFL,.sa_flags=SA_NOCLDSTOP};sigemptyset(&action.sa_mask);
        CHECK(!sigaction(SIGCHLD,&action,NULL));
        int notify[2],release[2];CHECK(!pipe(notify) && !pipe(release));
        pid_t child=fork();CHECK(child>=0);
        if(!child){CHECK(!raise(SIGSTOP));char byte;CHECK(read(release[0],&byte,1)==1);_exit(0);}
        int status;CHECK(waitpid(child,&status,WUNTRACED)==child && WIFSTOPPED(status));
        action.sa_flags=0;CHECK(!sigaction(SIGCHLD,&action,NULL));
        CHECK(!fcntl(notify[0],F_SETOWN,child) && !fcntl(notify[0],F_SETSIG,SIGCONT));
        CHECK(!fcntl(notify[0],F_SETFL,fcntl(notify[0],F_GETFL)|O_ASYNC));
        CHECK(write(notify[1],"c",1)==1);
        siginfo_t info;struct timespec timeout={5,0};
        CHECK(sigtimedwait(&set,&info,&timeout)==SIGCHLD && info.si_code==CLD_CONTINUED);
        CHECK(write(release[1],"x",1)==1 && waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
        CHECK(!close(notify[0])&&!close(notify[1])&&!close(release[0])&&!close(release[1]));_exit(0);
    }
    int status;CHECK(waitpid(intermediate,&status,0)==intermediate && WIFEXITED(status) && !WEXITSTATUS(status));
}
static void identities_and_remote(void)
{
    int p[2];CHECK(!pipe(p));enable(p[0]);pid_t parent=getpid(),child=fork();CHECK(child>=0);
    if(!child){CHECK(!setresuid(1000,1000,1000));CHECK(!fcntl(p[0],F_SETOWN,parent));CHECK(write(p[1],"x",1)==1);_exit(0);}
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));empty();
    char byte;CHECK(read(p[0],&byte,1)==1);
    struct f_owner_ex target={.type=F_OWNER_TID,.pid=(pid_t)syscall(SYS_gettid)};
    CHECK(!fcntl(p[0],F_SETOWN_EX,&target));
    cpu_set_t available;CHECK(!sched_getaffinity(0,sizeof(available),&available));int last=0;
    for(int cpu=0;cpu<CPU_SETSIZE;cpu++)if(CPU_ISSET(cpu,&available))last=cpu;
    struct remote r={p[1],last,-1};pthread_t thread;CHECK(!pthread_create(&thread,NULL,remote_writer,&r));
    CHECK(!pthread_join(thread,NULL) && r.observed==last);input_signal(p[0]);CHECK(read(p[0],&byte,1)==1);
    int ready[2],release[2];CHECK(!pipe(ready) && !pipe(release));
    struct identity_thread differing={p[0],ready[1],release[0]};
    CHECK(!pthread_create(&thread,NULL,nonleader_owner,&differing));
    CHECK(read(ready[0],&byte,1)==1 && write(p[1],"n",1)==1);input_signal(p[0]);
    CHECK(read(p[0],&byte,1)==1 && write(release[1],"x",1)==1 && !pthread_join(thread,NULL));
    CHECK(!close(ready[0])&&!close(ready[1])&&!close(release[0])&&!close(release[1]));
    CHECK(!fcntl(p[0],F_SETOWN,-getpgrp()) && fcntl(p[0],F_GETOWN)==-getpgrp());
    CHECK(!fcntl(p[0],F_SETFL,fcntl(p[0],F_GETFL)&~O_ASYNC) && !close(p[0])&&!close(p[1]));empty();
    continued_notification();
    printf("ASYNC CASE identities-remote PASS cpu=%d\n",r.observed);
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);signo=SIGRTMIN+4;
    sigemptyset(&blocked);sigaddset(&blocked,signo);sigaddset(&blocked,SIGIO);sigaddset(&blocked,SIGUSR1);
    CHECK(!sigprocmask(SIG_BLOCK,&blocked,NULL));signal_fd=signalfd(-1,&blocked,SFD_NONBLOCK|SFD_CLOEXEC);CHECK(signal_fd>=0);
    owners_and_pipe();consumers_and_quota();sockets_and_inotify();identities_and_remote();
    CHECK(!close(signal_fd));puts("ASYNC PASS cases=4");return 0;
}
