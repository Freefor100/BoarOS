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
#include "signalfd_cases.h"
#include "queued_threads.h"
#include "queued_lifecycle.h"
#include "queued_control.h"
/* Run only as the real guest PID 1, not as a normal native process. */
static volatile sig_atomic_t init_handled;
static void init_handler(int sig) { if(sig==SIGUSR1)init_handled++; }
static void init_signal_policy(void)
{
    CHECK(getpid()==1);
    CHECK(!kill(1,SIGTERM));
    CHECK(!kill(1,SIGKILL));
    CHECK(!syscall(SYS_tgkill,1,syscall(SYS_gettid),SIGSTOP));
    struct sigaction action={.sa_handler=init_handler},old_action;
    sigemptyset(&action.sa_mask);
    CHECK(!sigaction(SIGUSR1,&action,&old_action));
    CHECK(!kill(1,SIGUSR1) && init_handled==1);
    CHECK(!sigaction(SIGUSR1,&old_action,NULL));
    sigset_t set,old,pending_set;sigemptyset(&set);sigaddset(&set,SIGTERM);
    CHECK(!sigprocmask(SIG_BLOCK,&set,&old));
    CHECK(!kill(1,SIGTERM));
    CHECK(!sigpending(&pending_set) && sigismember(&pending_set,SIGTERM)==1);
    siginfo_t info;struct timespec zero={0};
    CHECK(sigtimedwait(&set,&info,&zero)==SIGTERM && info.si_pid==1);
    CHECK(!kill(1,SIGTERM));
    CHECK(!sigprocmask(SIG_SETMASK,&old,NULL));
    CHECK(!sigpending(&pending_set) && !sigismember(&pending_set,SIGTERM));
    pid_t child=fork();CHECK(child>=0);
    if(!child)_exit(kill(getppid(),SIGKILL)?91:0);
    if(child>0){int status;CHECK(waitpid(child,&status,0)==child && !status);}
    puts("QUEUED INIT defaults-handlers-blocked-child-kill PASS");
}
static pthread_t init_leader;
static void *init_exec_worker(void *unused)
{
    (void)unused;if(pthread_join(init_leader,NULL))_exit(92);
    execl("/init","/init","--init-policy-exec",(char *)NULL);_exit(93);
}

int main(int argc,char **argv)
{
    if(argc==2 && !strcmp(argv[1],"--init-policy-exec")){
        setvbuf(stdout,0,_IONBF,0);init_signal_policy();
        printf("QUEUED INIT nonleader-exec PASS\nQUEUED END failures=%u\n",failures);return failures?1:0;
    }
    if(argc>1&&!strcmp(argv[1],"--queued-tid-exec"))return queued_tid_exec_check(argc,argv);
    if(argc>1&&!strcmp(argv[1],"--queued-exec"))return queued_exec_check(argc,argv);
    setvbuf(stdout,0,_IONBF,0);alarm(15);
    FILE *performance=fopen("/queued-performance","r");if(performance){unsigned cpus;CHECK(fscanf(performance,"%u",&cpus)==1);
        int extra=fscanf(performance,"%u %u",&queue_perf_mode,&queue_perf_rounds);CHECK(extra==EOF||(extra==2&&queue_perf_mode<3&&queue_perf_rounds>0&&queue_perf_rounds<=1024));CHECK(!fclose(performance));
        queue_diagnostics=!access("/queued-diagnostics",F_OK);if(queue_diagnostics){CHECK(!mkdir("/proc",0555)||errno==EEXIST);CHECK(!mount("proc","/proc","proc",0,0)||errno==EBUSY);}
        queue_perf=1;alarm(120);queued_threads();CHECK(queue_cpus==cpus);printf("QUEUED-PERF END failures=%u\n",failures);return failures?1:0;}
    FILE *selection=fopen("/queued-select","r");if(selection){char name[16]={0};fscanf(selection,"%15s",name);fclose(selection);
        if(!strcmp(name,"fatal"))queued_fatal();else if(!strcmp(name,"times"))queued_child_times();else if(!strcmp(name,"life"))queued_lifecycle();else CHECK(0);
        printf("QUEUED END failures=%u\n",failures);return failures?1:0;}
    if(getpid()==1)init_signal_policy();
    queued_observe();pending();payload();import_bounds();send_limits();reserved_numbers();signal_fd_cases();queued_threads();queued_lifecycle();queued_child_times();queued_fatal();queued_competition();queued_control_cases();queued_observe_finish();
    if(getpid()==1 && !failures){
        pthread_t worker;init_leader=pthread_self();
        CHECK(!pthread_create(&worker,NULL,init_exec_worker,NULL));
        if(!failures)pthread_exit(NULL);
    }
    printf("QUEUED END failures=%u\n",failures);return failures?1:0;
}
