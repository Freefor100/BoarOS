#define _GNU_SOURCE
#include <signal.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
static volatile sig_atomic_t handled, handler_bad;
static uint64_t values[4];
static void handler(int signal,siginfo_t *info,void *context)
{
    (void)context;
    if(signal!=SIGRTMIN+1 || info->si_code!=SI_QUEUE || info->si_pid!=getpid() || handled>=4)handler_bad++;
    else values[handled++]=(uintptr_t)info->si_value.sival_ptr;
}
static int queued(int signal,uint64_t value)
{
    siginfo_t info={0};info.si_code=SI_QUEUE;info.si_pid=getpid();info.si_uid=getuid();info.si_value.sival_ptr=(void *)(uintptr_t)value;
    return syscall(SYS_rt_sigqueueinfo,getpid(),signal,&info);
}
static int error(long result,int code){return result==-1 && errno==code;}
static unsigned failures;
#define CHECK(x) do{if(!(x)){printf("QUEUED FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);failures++;}}while(0)
static void pending(void)
{
    int signal=SIGRTMIN+1;sigset_t set,old;sigemptyset(&set);sigaddset(&set,signal);
    CHECK(!sigprocmask(SIG_BLOCK,&set,&old));
    for(unsigned i=0;i<3;i++)CHECK(!kill(getpid(),signal));
    for(unsigned i=0;i<3;i++) {
        siginfo_t info={0};struct timespec zero={0};
        CHECK(sigtimedwait(&set,&info,&zero)==signal && info.si_pid==getpid() && info.si_code==SI_USER);
    }
    struct sigaction ignore={.sa_handler=SIG_IGN};sigemptyset(&ignore.sa_mask);
    CHECK(!sigaction(signal,&ignore,0) && !sigprocmask(SIG_SETMASK,&old,0));
    struct rlimit limit={0};CHECK(!getrlimit(RLIMIT_SIGPENDING,&limit));
    struct rlimit zero={0,limit.rlim_max};
    if(!setrlimit(RLIMIT_SIGPENDING,&zero)) {
        sigemptyset(&set);sigaddset(&set,SIGUSR1);CHECK(!sigprocmask(SIG_BLOCK,&set,&old));
        CHECK(!syscall(SYS_tgkill,getpid(),syscall(SYS_gettid),SIGUSR1));
        siginfo_t info={0};struct timespec timeout={0};
        CHECK(sigtimedwait(&set,&info,&timeout)==SIGUSR1 && info.si_code==SI_USER && !info.si_pid);
        CHECK(!sigprocmask(SIG_SETMASK,&old,0));
        CHECK(!setrlimit(RLIMIT_SIGPENDING,&limit));
    } else CHECK(0);
}
static void payload(void)
{
    int signal=SIGRTMIN+1;sigset_t set,old;sigemptyset(&set);sigaddset(&set,signal);CHECK(!sigprocmask(SIG_BLOCK,&set,&old));
    for(unsigned i=0;i<3;i++)CHECK(!queued(signal,UINT64_C(0x1234567800000000)+i));
    for(unsigned i=0;i<3;i++) {
        siginfo_t info={0};struct timespec zero={0};
        CHECK(sigtimedwait(&set,&info,&zero)==signal && info.si_code==SI_QUEUE &&
              (uintptr_t)info.si_value.sival_ptr==UINT64_C(0x1234567800000000)+i);
    }
    struct sigaction action={.sa_sigaction=handler,.sa_flags=SA_SIGINFO};sigemptyset(&action.sa_mask);CHECK(!sigaction(signal,&action,0));
    for(unsigned i=0;i<3;i++)CHECK(!queued(signal,UINT64_C(0xabcdef0100000000)+i));
    CHECK(!sigprocmask(SIG_SETMASK,&old,0));CHECK(handled==3 && !handler_bad);
    for(unsigned i=0;i<3;i++)CHECK(values[i]==UINT64_C(0xabcdef0100000000)+i);
    sigemptyset(&set);sigaddset(&set,SIGUSR1);CHECK(!sigprocmask(SIG_BLOCK,&set,&old));
    CHECK(!queued(SIGUSR1,77) && !queued(SIGUSR1,88));siginfo_t info={0};struct timespec zero={0};
    CHECK(sigtimedwait(&set,&info,&zero)==SIGUSR1 && (uintptr_t)info.si_value.sival_ptr==77);
    CHECK(error(sigtimedwait(&set,&info,&zero),EAGAIN));CHECK(!sigprocmask(SIG_SETMASK,&old,0));
}
static void import_bounds(void)
{
    size_t page=(size_t)sysconf(_SC_PAGESIZE);char *map=mmap(0,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(map!=MAP_FAILED && !mprotect(map+page,page,PROT_NONE));
    siginfo_t info={0};info.si_code=SI_QUEUE;info.si_pid=123;info.si_uid=234;info.si_value.sival_ptr=(void *)UINT64_C(0x1234567887654321);
    memcpy(map+page-48,&info,48);int signal=SIGRTMIN+2;sigset_t set,old;sigemptyset(&set);sigaddset(&set,signal);CHECK(!sigprocmask(SIG_BLOCK,&set,&old));
    CHECK(!syscall(SYS_rt_sigqueueinfo,getpid(),signal,map+page-48));
    struct timespec zero={0};siginfo_t out={0};
    CHECK(sigtimedwait(&set,&out,&zero)==signal && out.si_pid==123 && out.si_uid==234 && (uintptr_t)out.si_value.sival_ptr==UINT64_C(0x1234567887654321));
    info.si_code=-100;memcpy(map+page-48,&info,48);
    CHECK(error(syscall(SYS_rt_sigqueueinfo,getpid(),signal,map+page-48),EFAULT));
    ((char *)&info)[127]=1;CHECK(error(syscall(SYS_rt_sigqueueinfo,getpid(),signal,&info),E2BIG));
    ((char *)&info)[127]=0;CHECK(!syscall(SYS_rt_sigqueueinfo,getpid(),signal,&info));CHECK(sigtimedwait(&set,&out,&zero)==signal && out.si_code==-100);
    info.si_code=SI_KERNEL;
    CHECK(error(syscall(SYS_rt_sigqueueinfo,INT32_MAX,signal,&info),EPERM));
    CHECK(error(syscall(SYS_rt_tgsigqueueinfo,0,0,signal,&info),EINVAL));
    CHECK(error(syscall(SYS_rt_tgsigqueueinfo,getpid(),syscall(SYS_gettid),65,&info),EINVAL));
    CHECK(error(syscall(SYS_rt_tgsigqueueinfo,getpid(),syscall(SYS_gettid),signal,(void *)UINTPTR_MAX),EFAULT));
    info.si_code=SI_QUEUE;CHECK(error(syscall(SYS_rt_sigqueueinfo,0,signal,&info),ESRCH));
    CHECK(!syscall(SYS_rt_tgsigqueueinfo,getpid(),syscall(SYS_gettid),signal,&info));CHECK(sigtimedwait(&set,&out,&zero)==signal && out.si_code==SI_QUEUE);
    CHECK(!sigprocmask(SIG_SETMASK,&old,0) && !munmap(map,page*2));
}
static void send_limits(void)
{
    int signal=SIGRTMIN+3;sigset_t set,old;sigemptyset(&set);sigaddset(&set,signal);CHECK(!sigprocmask(SIG_BLOCK,&set,&old));
    struct rlimit limit;CHECK(!getrlimit(RLIMIT_SIGPENDING,&limit));struct rlimit zero={0,limit.rlim_max};CHECK(!setrlimit(RLIMIT_SIGPENDING,&zero));
    CHECK(error(queued(signal,7),EAGAIN));
    CHECK(error(syscall(SYS_tgkill,getpid(),syscall(SYS_gettid),signal),EAGAIN));
    CHECK(!kill(getpid(),signal));siginfo_t info={0};struct timespec timeout={0};
    CHECK(sigtimedwait(&set,&info,&timeout)==signal && info.si_code==SI_USER && !info.si_pid);
    CHECK(!setrlimit(RLIMIT_SIGPENDING,&limit));
    pid_t child=fork();CHECK(child>=0);if(!child)_exit(0);
    siginfo_t exited;CHECK(!waitid(P_PID,(id_t)child,&exited,WEXITED|WNOWAIT) && exited.si_pid==child);
    info=(siginfo_t){0};info.si_code=SI_QUEUE;
    CHECK(!syscall(SYS_rt_sigqueueinfo,child,signal,&info));
    CHECK(!syscall(SYS_rt_tgsigqueueinfo,child,child,signal,&info));
    int status;CHECK(waitpid(child,&status,0)==child && !status);
    CHECK(!sigprocmask(SIG_SETMASK,&old,0));
}
static void reserved_numbers(void)
{
    uint64_t mask=(UINT64_C(1)<<31)|(UINT64_C(1)<<63),old;
    CHECK(!syscall(SYS_rt_sigprocmask,SIG_BLOCK,&mask,&old,8));
    for(unsigned signal=32;signal<=64;signal+=32) {
        siginfo_t info={0},out={0};info.si_code=SI_QUEUE;info.si_value.sival_ptr=(void *)(uintptr_t)(UINT64_C(0x1234567800000000)+signal);
        CHECK(!syscall(SYS_rt_sigqueueinfo,getpid(),signal,&info));struct timespec timeout={0};
        CHECK(syscall(SYS_rt_sigtimedwait,&mask,&out,&timeout,8)==signal && out.si_value.sival_ptr==info.si_value.sival_ptr);
    }
    CHECK(!syscall(SYS_rt_sigprocmask,SIG_SETMASK,&old,0,8));
}
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);alarm(15);pending();payload();import_bounds();send_limits();reserved_numbers();
    printf("QUEUED END failures=%u\n",failures);return failures?1:0;
}
