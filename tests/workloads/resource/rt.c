#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"RT FAIL line=%d expr=%s errno=%d\n",__LINE__,#x,errno); _exit(1); } } while (0)
static unsigned cases;
static volatile sig_atomic_t signals;
static void pass(const char *name) { printf("RT CASE %s PASS\n",name);cases++; }
static void noted(int sig) { CHECK(sig==SIGXCPU);signals++; }
static uint64_t stamp(clockid_t clock) { struct timespec t;CHECK(!clock_gettime(clock,&t));return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec; }
static void wait_ok(pid_t child) {int status;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));}
static void limit(int kind,rlim_t soft,rlim_t hard) {struct rlimit v={soft,hard};CHECK(!setrlimit(kind,&v));}
/* Original musl inputs can export scheduler stubs; probe the kernel ABI. */
static int rt_setscheduler(pid_t pid,int kind,const struct sched_param *p)
{ return (int)syscall(SYS_sched_setscheduler,pid,kind,p); }
static int rt_setparam(pid_t pid,const struct sched_param *p)
{ return (int)syscall(SYS_sched_setparam,pid,p); }
static int rt_getscheduler(pid_t pid)
{ return (int)syscall(SYS_sched_getscheduler,pid); }
static void policy(int kind,int priority) {struct sched_param p={.sched_priority=priority};CHECK(!rt_setscheduler(0,kind,&p));}
static void reject_policy(int kind,int priority) {struct sched_param p={.sched_priority=priority};errno=0;CHECK(rt_setscheduler(0,kind,&p)==-1&&errno==EPERM);}
static void drop(unsigned uid) { CHECK(!setgroups(0,0)&&!setgid(uid)&&!setuid(uid)); }
static void state(void)
{
    struct rlimit p,t;CHECK(!getrlimit(RLIMIT_RTPRIO,&p)&&!getrlimit(RLIMIT_RTTIME,&t));
    CHECK(!p.rlim_cur&&!p.rlim_max&&t.rlim_cur==RLIM_INFINITY&&t.rlim_max==RLIM_INFINITY);
    pid_t child=fork();CHECK(child>=0);if(!child){limit(RLIMIT_RTPRIO,20,30);limit(RLIMIT_RTTIME,700000,900000);pid_t next=fork();CHECK(next>=0);if(!next){CHECK(!getrlimit(RLIMIT_RTPRIO,&p)&&p.rlim_cur==20&&p.rlim_max==30);CHECK(!getrlimit(RLIMIT_RTTIME,&t)&&t.rlim_cur==700000&&t.rlim_max==900000);_exit(0);}wait_ok(next);_exit(0);}wait_ok(child);
    pass("rt-default-set-fork");
}
static void unprivileged(void)
{
    pid_t child=fork();CHECK(child>=0);if(!child){limit(RLIMIT_RTPRIO,20,20);drop(1001);policy(SCHED_FIFO,10);policy(SCHED_FIFO,15);reject_policy(SCHED_FIFO,21);policy(SCHED_RR,15);struct sched_param p={.sched_priority=9};CHECK(!rt_setparam(0,&p));limit(RLIMIT_RTPRIO,0,20);reject_policy(SCHED_RR,10);policy(SCHED_RR,8);reject_policy(SCHED_FIFO,8);policy(SCHED_OTHER,0);reject_policy(SCHED_FIFO,1);_exit(0);}wait_ok(child);
    pass("rtprio-unprivileged-limit-policy");
}
static void reset_fork(void)
{
    pid_t child=fork();CHECK(child>=0);if(!child){limit(RLIMIT_RTPRIO,10,10);drop(1001);policy(SCHED_FIFO|SCHED_RESET_ON_FORK,5);reject_policy(SCHED_FIFO,5);pid_t next=fork();CHECK(next>=0);if(!next){CHECK(rt_getscheduler(0)==SCHED_OTHER);_exit(0);}wait_ok(next);CHECK(rt_getscheduler(0)==(SCHED_FIFO|SCHED_RESET_ON_FORK));policy(SCHED_OTHER|SCHED_RESET_ON_FORK,0);_exit(0);}wait_ok(child);
    pass("rtprio-reset-on-fork");
}
static void target_limit(void)
{
    int ready[2],done[2];CHECK(!pipe(ready)&&!pipe(done));pid_t child=fork();CHECK(child>=0);
    if(!child){close(ready[0]);close(done[1]);limit(RLIMIT_RTPRIO,7,7);drop(1001);CHECK(write(ready[1],"r",1)==1);char c;CHECK(read(done[0],&c,1)==1);CHECK(rt_getscheduler(0)==SCHED_FIFO);_exit(0);}
    close(ready[1]);close(done[0]);char c;CHECK(read(ready[0],&c,1)==1);pid_t actor=fork();CHECK(actor>=0);
    if(!actor){drop(1001);struct rlimit v;CHECK(!getrlimit(RLIMIT_RTPRIO,&v)&&!v.rlim_cur);struct sched_param p={.sched_priority=5};CHECK(!rt_setscheduler(child,SCHED_FIFO,&p));_exit(0);}wait_ok(actor);
    actor=fork();CHECK(actor>=0);if(!actor){drop(1002);struct sched_param p={.sched_priority=4};errno=0;CHECK(rt_setscheduler(child,SCHED_FIFO,&p)==-1&&errno==EPERM);_exit(0);}wait_ok(actor);
    CHECK(write(done[1],"d",1)==1);wait_ok(child);close(ready[0]);close(done[1]);pass("rtprio-target-limit-and-owner");
}
static void prepare_rt(rlim_t soft,rlim_t hard)
{
    limit(RLIMIT_CPU,3,3);limit(RLIMIT_RTTIME,soft,hard);
    struct sigaction action={.sa_handler=noted};CHECK(!sigaction(SIGXCPU,&action,0));policy(SCHED_FIFO,10);
}
static void soft(void)
{
    pid_t child=fork();CHECK(child>=0);if(!child){prepare_rt(30000,90000);while(!signals)__asm__ volatile("" ::: "memory");policy(SCHED_OTHER,0);struct rlimit v;CHECK(signals==1&&!getrlimit(RLIMIT_RTTIME,&v)&&v.rlim_cur==1030000&&v.rlim_max==90000);_exit(0);}wait_ok(child);pass("rttime-soft-signal-and-update");
}
static void hard(int yield)
{
    uint64_t start=stamp(CLOCK_MONOTONIC);pid_t child=fork();CHECK(child>=0);
    if(!child){prepare_rt(30000,80000);for(;;){if(yield)CHECK(!sched_yield());__asm__ volatile("" ::: "memory");}}
    int status;CHECK(waitpid(child,&status,0)==child&&WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);CHECK(stamp(CLOCK_MONOTONIC)-start<2000000000ULL);
    pass(yield?"rttime-yield-retains-budget":"rttime-hard-kill");
}
static void wait_blocked(pid_t child)
{
    char path[64],buf[256];snprintf(path,sizeof(path),"/proc/%d/stat",child);
    for(;;){int fd=open(path,O_RDONLY);CHECK(fd>=0);ssize_t n=read(fd,buf,sizeof(buf)-1);CHECK(n>0&&!close(fd));buf[n]=0;char *p=strrchr(buf,')');CHECK(p&&p[1]==' ');if(p[2]=='S')return;sched_yield();}
}
static void blocking(void)
{
    int request[2],reply[2];CHECK(!pipe(request)&&!pipe(reply));pid_t child=fork();CHECK(child>=0);
    if(!child){close(request[0]);close(reply[1]);prepare_rt(40000,80000);for(unsigned i=0;i<6;i++){uint64_t until=stamp(CLOCK_THREAD_CPUTIME_ID)+15000000;while(stamp(CLOCK_THREAD_CPUTIME_ID)<until)__asm__ volatile("" ::: "memory");CHECK(!signals&&write(request[1],"r",1)==1);char c;CHECK(read(reply[0],&c,1)==1);}policy(SCHED_OTHER,0);CHECK(!signals);_exit(0);}
    close(request[1]);close(reply[0]);for(unsigned i=0;i<6;i++){char c;CHECK(read(request[0],&c,1)==1);wait_blocked(child);CHECK(write(reply[1],"d",1)==1);}wait_ok(child);close(request[0]);close(reply[1]);pass("rttime-real-block-resets-budget");
}
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);alarm(15);mkdir("/proc",0755);CHECK(!mount("proc","/proc","proc",0,0)||errno==EBUSY);
    puts("RT BEGIN");state();unprivileged();reset_fork();target_limit();soft();hard(0);hard(1);blocking();
    printf("RT END cases=%u\n",cases);alarm(0);if(getpid()==1){sync();reboot(RB_POWER_OFF);}return 0;
}
