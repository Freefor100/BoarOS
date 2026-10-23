#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/signalfd.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/mman.h>
#include <time.h>

/* Raw IDs stay separate from libc's opaque timer_t and callback bookkeeping. */
struct native_event { uint64_t value;int32_t signal,notify;int32_t tid;uint8_t padding[44]; };
_Static_assert(sizeof(struct native_event)==64,"native sigevent");
static unsigned failures;
#define CHECK(x) do{if(!(x)){printf("POSIX FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);failures++;}}while(0)
static int error(long value,int code){return value==-1&&errno==code;}
static int create(int clock,const struct native_event *event,int *id)
{return syscall(SYS_timer_create,clock,event,id);}
static int remove_timer(int id){return syscall(SYS_timer_delete,id);}
static int get_timer(int id,struct itimerspec *value){return syscall(SYS_timer_gettime,id,value);}
static void objects(void)
{
    struct native_event none={.notify=SIGEV_NONE};
    for(int clock=0;clock<4;clock++) {
        int id=-1;struct itimerspec value;memset(&value,0x55,sizeof(value));
        CHECK(!create(clock,&none,&id));if(id<0)return;
        CHECK(id>=0&&!get_timer(id,&value)&&!value.it_value.tv_sec&&!value.it_value.tv_nsec&&
            !value.it_interval.tv_sec&&!value.it_interval.tv_nsec);
        CHECK(!syscall(SYS_timer_getoverrun,id));CHECK(!remove_timer(id));
        CHECK(error(get_timer(id,&value),EINVAL)&&error(remove_timer(id),EINVAL));
    }
    int id;CHECK(!create(CLOCK_MONOTONIC,0,&id)&&!remove_timer(id));
    struct native_event event={.signal=SIGRTMIN,.notify=SIGEV_SIGNAL};
    CHECK(!create(CLOCK_MONOTONIC,&event,&id)&&!remove_timer(id));
    event.notify=SIGEV_THREAD;CHECK(!create(CLOCK_MONOTONIC,&event,&id)&&!remove_timer(id));
    event.notify=4;event.tid=syscall(SYS_gettid);CHECK(!create(CLOCK_MONOTONIC,&event,&id)&&!remove_timer(id));
    event.tid=INT32_MAX;CHECK(error(create(CLOCK_MONOTONIC,&event,&id),EINVAL));
    event.notify=SIGEV_SIGNAL;event.signal=0;CHECK(error(create(CLOCK_MONOTONIC,&event,&id),EINVAL));
    event.notify=SIGEV_NONE;CHECK(!create(CLOCK_MONOTONIC,&event,&id)&&!remove_timer(id));
    CHECK(error(create(INT32_MAX,&none,&id),EINVAL));
    puts("POSIX CASE objects OK");
}
static void faults_quota(void)
{
    struct native_event none={.notify=SIGEV_NONE};int id=-1,other=-1;
    CHECK(error(create(1,(void *)UINTPTR_MAX,&id),EFAULT));
    CHECK(error(create(1,&none,(void *)UINTPTR_MAX),EFAULT));
    struct rlimit original;CHECK(!getrlimit(RLIMIT_SIGPENDING,&original));
    struct rlimit one={1,original.rlim_max};CHECK(!setrlimit(RLIMIT_SIGPENDING,&one));
    CHECK(!create(1,&none,&id));CHECK(error(create(1,&none,&other),EAGAIN));
    CHECK(!remove_timer(id)&&!create(1,&none,&other)&&!remove_timer(other));
    CHECK(error(create(1,&none,(void *)UINTPTR_MAX),EFAULT));
    CHECK(!create(1,&none,&id)&&!remove_timer(id));
    CHECK(!setrlimit(RLIMIT_SIGPENDING,&original));
    puts("POSIX CASE faults-quota OK");
}
static int member_id=-1;
static void *creator(void *argument)
{(void)argument;struct native_event none={.notify=SIGEV_NONE};CHECK(!create(1,&none,&member_id));return 0;}
static void lifecycle(void)
{
    pthread_t member;CHECK(!pthread_create(&member,0,creator,0)&&!pthread_join(member,0));
    struct itimerspec value;CHECK(!get_timer(member_id,&value));
    pid_t child=fork();CHECK(child>=0);if(!child){if(!error(get_timer(member_id,&value),EINVAL))_exit(61);
        struct native_event none={.notify=SIGEV_NONE};int id;
        if(create(1,&none,&id))_exit(62);
        char number[16];snprintf(number,sizeof(number),"%d",id);
        execl("/init","/init","--posix-exec",number,(char *)0);_exit(63);}
    int status;CHECK(waitpid(child,&status,0)==child&&!status);
    CHECK(!get_timer(member_id,&value)&&!remove_timer(member_id));
    puts("POSIX CASE lifecycle OK");
}
static int set_timer(int id,int flags,const struct itimerspec *value,struct itimerspec *old)
{return syscall(SYS_timer_settime,id,flags,value,old);}
static uint64_t clock_ns(int clock)
{struct timespec t;CHECK(!clock_gettime(clock,&t));return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static struct itimerspec setting(uint64_t value,uint64_t interval)
{return (struct itimerspec){.it_value={value/1000000000,value%1000000000},.it_interval={interval/1000000000,interval%1000000000}};}
static void wall(void)
{
    struct native_event none={.notify=SIGEV_NONE};int id;CHECK(!create(1,&none,&id));
    struct itimerspec in=setting(1000000000,5000000),out;
    CHECK(!set_timer(id,0,&in,&out)&&!out.it_value.tv_sec&&!out.it_value.tv_nsec);
    CHECK(!get_timer(id,&out)&&(out.it_value.tv_sec||out.it_value.tv_nsec)&&out.it_interval.tv_nsec==5000000);
    in=setting(2000000000,0);CHECK(error(set_timer(id,0,&in,(void *)UINTPTR_MAX),EFAULT));
    CHECK(!get_timer(id,&out)&&out.it_value.tv_sec>=1&&!out.it_interval.tv_nsec);
    in=setting(0,999);CHECK(!set_timer(id,0,&in,0)&&!get_timer(id,&out));
    /* Linux's lazy SIGEV_NONE query retains the prior one-shot expiry. */
    CHECK(!out.it_interval.tv_nsec&&out.it_value.tv_sec>=1);
    in=setting(1,0);in.it_value.tv_nsec=-1;CHECK(error(set_timer(id,0,&in,0),EINVAL));
    CHECK(error(syscall(SYS_timer_settime,id,0,0,0),EINVAL));
    in=setting(0,0);CHECK(!set_timer(id,2,&in,0)&&!set_timer(id,-1,&in,0));
    CHECK(!remove_timer(id));puts("POSIX CASE wall OK");
}
static void queued_notifications(void)
{
    int sig=SIGUSR1,ids[2];sigset_t set,old;sigemptyset(&set);sigaddset(&set,sig);CHECK(!sigprocmask(SIG_BLOCK,&set,&old));
    struct native_event event={.notify=SIGEV_SIGNAL,.signal=sig};
    for(unsigned i=0;i<2;i++){event.value=111+i;CHECK(!create(1,&event,&ids[i]));}
    struct itimerspec in=setting(clock_ns(1)-1000000,0);
    for(unsigned i=0;i<2;i++)CHECK(!set_timer(ids[i],TIMER_ABSTIME,&in,0));
    unsigned seen=0;
    for(unsigned i=0;i<2;i++) {
        siginfo_t info;CHECK(sigwaitinfo(&set,&info)==sig&&info.si_code==SI_TIMER);
        CHECK(info.si_value.sival_int>=111&&info.si_value.sival_int<=112);
        if(info.si_value.sival_int>=111&&info.si_value.sival_int<=112)seen|=1U<<(info.si_value.sival_int-111);
    }
    CHECK(seen==3);for(unsigned i=0;i<2;i++)CHECK(!remove_timer(ids[i]));
    CHECK(!sigprocmask(SIG_SETMASK,&old,0));puts("POSIX CASE shared-standard-signal OK");
}
static void periodic(void)
{
    int sig=SIGRTMIN+3,id;sigset_t set,old;sigemptyset(&set);sigaddset(&set,sig);CHECK(!sigprocmask(SIG_BLOCK,&set,&old));
    struct native_event event={.notify=SIGEV_SIGNAL,.signal=sig,.value=UINT64_C(0xabcdef1234567890)};
    CHECK(!create(1,&event,&id));struct timespec resolution;CHECK(!clock_getres(1,&resolution));
    uint64_t interval=(uint64_t)resolution.tv_sec*1000000000+resolution.tv_nsec;if(interval<1000000)interval=1000000;
    struct itimerspec in=setting(clock_ns(1)-50000000,interval);
    CHECK(!set_timer(id,TIMER_ABSTIME,&in,0));siginfo_t info;
    CHECK(sigwaitinfo(&set,&info)==sig&&info.si_code==SI_TIMER&&(uintptr_t)info.si_value.sival_ptr==event.value);
    long over=syscall(SYS_timer_getoverrun,id);
    CHECK(info.si_timerid==id&&info.si_overrun>=(int)(50000000/interval)-1&&over==info.si_overrun);
    int fd=signalfd(-1,&set,0);CHECK(fd>=0);struct signalfd_siginfo out;
    CHECK(read(fd,&out,128)==128&&out.ssi_tid==(unsigned)id&&out.ssi_ptr==event.value&&out.ssi_code==SI_TIMER);
    CHECK(!remove_timer(id)&&!close(fd));struct timespec zero={0};
    while(sigtimedwait(&set,&info,&zero)>0){}
    CHECK(!sigprocmask(SIG_SETMASK,&old,0));puts("POSIX CASE periodic-overrun OK");
}
static void deadline_passed(void)
{
    int fd=timerfd_create(CLOCK_MONOTONIC,0);struct itimerspec in=setting(2000000,0);uint64_t count;
    CHECK(fd>=0&&!timerfd_settime(fd,0,&in,0)&&read(fd,&count,8)==8&&count==1&&!close(fd));
}
static void stale_and_ignored(void)
{
    int sig=SIGRTMIN+6,id;sigset_t set,old;sigemptyset(&set);sigaddset(&set,sig);CHECK(!sigprocmask(SIG_BLOCK,&set,&old));
    struct native_event event={.notify=0,.signal=sig,.value=93};CHECK(!create(1,&event,&id));
    struct itimerspec in=setting(clock_ns(1)-1000,1000000);CHECK(!set_timer(id,1,&in,0));deadline_passed();
    in=setting(60000000000ULL,0);CHECK(!set_timer(id,0,&in,0));siginfo_t info;struct timespec zero={0};
    CHECK(error(sigtimedwait(&set,&info,&zero),EAGAIN));CHECK(!remove_timer(id));
    CHECK(!create(1,&event,&id));in=setting(clock_ns(1)-1000,0);CHECK(!set_timer(id,1,&in,0));deadline_passed();
    sigset_t pending;CHECK(!sigpending(&pending)&&sigismember(&pending,sig)==1);CHECK(!remove_timer(id));
    CHECK(error(sigtimedwait(&set,&info,&zero),EAGAIN));
    /* User SI_TIMER payload is ordinary queue data, even if its ID matches. */
    CHECK(!create(1,&event,&id));memset(&info,0,sizeof(info));info.si_code=SI_TIMER;info.si_timerid=id;info.si_value.sival_int=77;
    CHECK(!syscall(SYS_rt_sigqueueinfo,getpid(),sig,&info));CHECK(sigwaitinfo(&set,&info)==sig&&info.si_code==SI_TIMER);
    struct itimerspec out;CHECK(!get_timer(id,&out)&&!out.it_value.tv_sec&&!out.it_value.tv_nsec&&!remove_timer(id));
    CHECK(!sigprocmask(SIG_SETMASK,&old,0));
    struct sigaction ignore={.sa_handler=SIG_IGN},previous;sigemptyset(&ignore.sa_mask);CHECK(!sigaction(sig,&ignore,&previous));
    CHECK(!create(1,&event,&id));in=setting(1,1000);CHECK(!set_timer(id,0,&in,0));deadline_passed();
    CHECK(!sigprocmask(SIG_BLOCK,&set,&old));deadline_passed();
    CHECK(error(sigtimedwait(&set,&info,&zero),EAGAIN));
    CHECK(!sigaction(sig,&previous,0));CHECK(sigwaitinfo(&set,&info)==sig&&info.si_code==SI_TIMER);
    CHECK(!remove_timer(id)&&!sigaction(sig,&previous,0));while(sigtimedwait(&set,&info,&zero)>0){}
    CHECK(!sigprocmask(SIG_SETMASK,&old,0));puts("POSIX CASE stale-ignore-forged OK");
}
static int pending_exec_check(int id,int sig)
{
    struct itimerspec out;sigset_t set;sigemptyset(&set);sigaddset(&set,sig);siginfo_t info;struct timespec zero={0};
    if(!error(get_timer(id,&out),EINVAL)||sigtimedwait(&set,&info,&zero)!=sig||info.si_code!=SI_QUEUE||info.si_value.sival_int!=7)return 65;
    if(!error(sigtimedwait(&set,&info,&zero),EAGAIN))return 66;
    struct rlimit limit;if(getrlimit(RLIMIT_SIGPENDING,&limit))return 67;limit.rlim_cur=1;
    struct native_event none={.notify=SIGEV_NONE};
    return setrlimit(RLIMIT_SIGPENDING,&limit)||create(1,&none,&id)||remove_timer(id)?68:0;
}
static void exec_pending(void)
{
    pid_t child=fork();CHECK(child>=0);
    if(!child) {
        int sig=SIGRTMIN+9,id;sigset_t set;sigemptyset(&set);sigaddset(&set,sig);if(sigprocmask(SIG_BLOCK,&set,0))_exit(69);
        struct native_event event={.notify=0,.signal=sig,.value=8};if(create(1,&event,&id))_exit(70);
        siginfo_t info={0};info.si_code=SI_QUEUE;info.si_value.sival_int=7;
        if(syscall(SYS_rt_sigqueueinfo,getpid(),sig,&info))_exit(71);
        struct itimerspec in=setting(clock_ns(1)-1000,0);if(set_timer(id,1,&in,0))_exit(72);deadline_passed();
        char timer[16],signal[16];snprintf(timer,sizeof(timer),"%d",id);snprintf(signal,sizeof(signal),"%d",sig);
        execl("/init","/init","--posix-pending-exec",timer,signal,(char *)0);_exit(73);
    }
    int status;CHECK(waitpid(child,&status,0)==child&&!status);puts("POSIX CASE exec-pending OK");
}
static int handler_pipe[2],handler_id;
static volatile sig_atomic_t handler_bad;
static void timer_handler(int sig,siginfo_t *info,void *context)
{
    (void)context;if(sig!=SIGRTMIN+5||info->si_code!=SI_TIMER||info->si_timerid!=handler_id||info->si_value.sival_int!=19)handler_bad=1;
    if(write(handler_pipe[1],"H",1)!=1)handler_bad=1;
}
static void callback(union sigval value)
{if(value.sival_int!=29||write(handler_pipe[1],"T",1)!=1)handler_bad=1;}
static void callbacks(void)
{
    CHECK(!pipe(handler_pipe));struct sigaction action={.sa_sigaction=timer_handler,.sa_flags=SA_SIGINFO|SA_RESTART},old;
    sigemptyset(&action.sa_mask);CHECK(!sigaction(SIGRTMIN+5,&action,&old));
    struct native_event event={.notify=4,.signal=SIGRTMIN+5,.value=19,.tid=syscall(SYS_gettid)};
    CHECK(!create(1,&event,&handler_id));struct itimerspec in=setting(1000000,0);
    CHECK(!set_timer(handler_id,0,&in,0));char byte;CHECK(read(handler_pipe[0],&byte,1)==1&&byte=='H'&&!handler_bad);
    CHECK(!remove_timer(handler_id)&&!sigaction(SIGRTMIN+5,&old,0));
    struct sigevent threaded={.sigev_notify=SIGEV_THREAD,.sigev_notify_function=callback,.sigev_value.sival_int=29};
    timer_t timer;CHECK(!timer_create(CLOCK_MONOTONIC,&threaded,&timer));
    CHECK(!timer_settime(timer,0,&in,0));CHECK(read(handler_pipe[0],&byte,1)==1&&byte=='T'&&!handler_bad);
    CHECK(!timer_delete(timer));CHECK(!close(handler_pipe[0])&&!close(handler_pipe[1]));puts("POSIX CASE handler-libc-thread OK");
}
static void timeout_exit(int sig){(void)sig;_exit(124);}
int main(int argc,char **argv)
{
    if(argc==4&&!strcmp(argv[1],"--posix-pending-exec"))return pending_exec_check(atoi(argv[2]),atoi(argv[3]));
    if(argc==3&&!strcmp(argv[1],"--posix-exec")){
        struct itimerspec value;return error(get_timer(atoi(argv[2]),&value),EINVAL)?0:64;}
    setvbuf(stdout,0,_IONBF,0);signal(SIGALRM,timeout_exit);alarm(20);
    struct native_event none={.notify=SIGEV_NONE};int entry=-1;
    CHECK(!create(1,&none,&entry));if(entry<0)return 1;CHECK(!remove_timer(entry));
    objects();faults_quota();lifecycle();wall();queued_notifications();periodic();stale_and_ignored();exec_pending();callbacks();
    printf("POSIX END failures=%u\n",failures);return failures?1:0;
}
