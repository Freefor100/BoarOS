#define _GNU_SOURCE
#include <signal.h>
#include <ucontext.h>
#include <stddef.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <time.h>
int la_signal_register_probe(void);
_Static_assert(sizeof(ucontext_t)==448,"musl LA ucontext size");
_Static_assert(offsetof(ucontext_t,uc_mcontext)==176,"musl LA mcontext offset");
_Static_assert(offsetof(mcontext_t,__extcontext)==272,"musl LA extension offset");
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"LA signal check line=%d errno=%d\n",__LINE__,errno);return 1;} } while(0)
static volatile sig_atomic_t seen,nested,depth,kind,expected_signal,expected_code;
static void *volatile address;
static void handler(int sig,siginfo_t *info,void *pointer)
{
    ucontext_t *context=pointer;
    if(((uintptr_t)context&15) || sig!=info->si_signo || !context->uc_mcontext.__pc || context->uc_stack.ss_flags!=SS_DISABLE) _exit(80);
    if(kind==1) {
        sigset_t mask;sigprocmask(SIG_SETMASK,0,&mask);
        if(sig!=SIGUSR1 || info->si_code!=SI_TKILL || info->si_pid!=getpid() || !sigismember(&mask,SIGUSR2)) _exit(81);
        seen++;return;
    }
    if(kind==2) {
        depth++;if(depth==1 && raise(SIGUSR1))_exit(82);
        if(depth==2)nested++;
        depth--;seen++;return;
    }
    if(kind==3) { unsigned magic=0xdead1234;memcpy(context->uc_mcontext.__extcontext,&magic,sizeof(magic));return; }
    if(kind==6) {
        const unsigned long *r=context->uc_mcontext.__gregs;
        if(sig!=SIGTRAP || info->si_code!=TRAP_BRKPT || r[3]!=r[7] || r[21]!=1365 || r[22]!=819) _exit(86);
        for(unsigned i=0;i<9;i++) if(r[12+i]!=256+i || r[23+i]!=512+i)_exit(87);
        seen++;context->uc_mcontext.__pc+=4;return;
    }
    if(sig!=expected_signal || info->si_code!=expected_code ||
       info->si_addr!=(expected_code==SI_KERNEL ? 0 : address ? address : (void *)context->uc_mcontext.__pc)) _exit(83);
    seen++;
    if((kind==4 || kind==5) && !(context->uc_mcontext.__flags&(1U<<31)))_exit(88);
    if(kind==4) {
        if(mmap(address,16384,PROT_READ|PROT_WRITE,MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS,-1,0)!=address) _exit(84);
    } else if(kind==5) {
        if(mprotect(address,16384,PROT_READ|PROT_WRITE)) _exit(85);
    } else context->uc_mcontext.__pc+=4;
}
static int fault(unsigned scenario)
{
    seen=0;address=0;kind=0;
    expected_signal=scenario>=10 ? SIGFPE : scenario==2 ? SIGBUS : scenario==3 ? SIGILL : scenario==4 ? SIGTRAP : SIGSEGV;
    expected_code=scenario==10 ? FPE_INTOVF : scenario==11 ? FPE_INTDIV : scenario==2 ? BUS_ADRERR : scenario==3 ? SI_KERNEL : scenario==4 ? TRAP_BRKPT : scenario==1 ? SEGV_ACCERR : SEGV_MAPERR;
    struct sigaction action={.sa_sigaction=handler,.sa_flags=SA_SIGINFO};sigemptyset(&action.sa_mask);
    if(scenario==5)action.sa_handler=SIG_DFL;
    if(scenario==6)action.sa_handler=SIG_IGN;
    CHECK(!sigaction(expected_signal,&action,0));
    if(scenario==7) {sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGSEGV);CHECK(!sigprocmask(SIG_BLOCK,&mask,0));}
    if(scenario==8) {__asm__ volatile("li.d $sp, 8;li.w $a7,139;syscall 0":::"memory");__builtin_unreachable();}
    if(scenario==9) {kind=3;action.sa_sigaction=handler;CHECK(!sigaction(SIGUSR1,&action,0));CHECK(!raise(SIGUSR1));return 91;}
    if(scenario==3) __asm__ volatile(".word 0":::"memory");
    else if(scenario==4) __asm__ volatile("break 0":::"memory");
    else if(scenario==10) __asm__ volatile("break 6":::"memory");
    else if(scenario==11) __asm__ volatile("break 7":::"memory");
    else if(scenario==2) {
        int fd=open("/signal-empty",O_CREAT|O_TRUNC|O_RDWR,0600);CHECK(fd>=0);
        address=mmap(0,16384,PROT_READ,MAP_PRIVATE,fd,0);CHECK(address!=MAP_FAILED && !close(fd));
        unsigned long value;__asm__ volatile("ld.d %0,%1,0":"=r"(value):"r"(address):"memory");(void)value;
        CHECK(!munmap(address,16384));
    } else {
        address=mmap(0,16384,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(address!=MAP_FAILED);
        kind=scenario==1 ? 5 : 4;
        if(scenario!=1)CHECK(!munmap(address,16384));
        *(volatile unsigned char *)address=0x6b;
        CHECK(*(volatile unsigned char *)address==0x6b && !munmap(address,16384));
    }
    CHECK(seen==1);return 0;
}
static void simple_handler(int sig) { (void)sig;seen++; }
static int waiting(void)
{
    for(int restart=0;restart<2;restart++) {
        int channel[2];CHECK(!pipe(channel));
        struct sigaction action={.sa_handler=simple_handler,.sa_flags=restart ? SA_RESTART : 0};
        CHECK(!sigaction(SIGUSR1,&action,0));seen=0;
        pid_t child=fork();CHECK(child>=0);
        if(!child) {
            struct timespec delay={0,20000000};
            if(nanosleep(&delay,0) || kill(getppid(),SIGUSR1) || nanosleep(&delay,0) || write(channel[1],"R",1)!=1)_exit(1);
            _exit(0);
        }
        char byte=0;errno=0;ssize_t count=read(channel[0],&byte,1);
        if(restart)CHECK(count==1 && byte=='R');
        else CHECK(count==-1 && errno==EINTR);
        int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status) && seen==1);
        CHECK(!close(channel[0]) && !close(channel[1]));
    }
    struct sigaction action={.sa_handler=simple_handler,.sa_flags=SA_RESTART};
    CHECK(!sigaction(SIGALRM,&action,0));seen=0;
    struct itimerval timer={{0,0},{0,20000}};
    CHECK(!setitimer(ITIMER_REAL,&timer,0));
    struct timespec requested={0,200000000},remaining={0};errno=0;
    CHECK(nanosleep(&requested,&remaining)==-1 && errno==EINTR && seen==1 && remaining.tv_sec==0 && remaining.tv_nsec>0 && remaining.tv_nsec<requested.tv_nsec);
    sigset_t blocked,old,temporary;sigemptyset(&blocked);sigaddset(&blocked,SIGUSR1);
    CHECK(!sigprocmask(SIG_BLOCK,&blocked,&old) && !raise(SIGUSR1));
    sigemptyset(&temporary);seen=0;errno=0;
    CHECK(sigsuspend(&temporary)==-1 && errno==EINTR && seen==1);
    CHECK(!sigprocmask(SIG_SETMASK,0,&temporary) && sigismember(&temporary,SIGUSR1) && !sigprocmask(SIG_SETMASK,&old,0));
    puts("LA signal wait/restart/suspend passed");return 0;
}
int main(void)
{
    CHECK(getauxval(AT_PAGESZ)==16384);
    struct sigaction action={.sa_sigaction=handler,.sa_flags=SA_SIGINFO};sigemptyset(&action.sa_mask);sigaddset(&action.sa_mask,SIGUSR2);
    kind=1;CHECK(!sigaction(SIGUSR1,&action,0) && !raise(SIGUSR1) && seen==1);
    sigset_t mask;CHECK(!sigprocmask(SIG_SETMASK,0,&mask) && !sigismember(&mask,SIGUSR1) && !sigismember(&mask,SIGUSR2));
    kind=2;seen=0;action.sa_flags|=SA_NODEFER;CHECK(!sigaction(SIGUSR1,&action,0) && !raise(SIGUSR1) && seen==2 && nested==1);
    puts("LA signal layout/mask/nesting passed");
    for(unsigned i=0;i<12;i++) {
        pid_t child=fork();CHECK(child>=0);if(!child)_exit(fault(i));
        int status=-1;CHECK(waitpid(child,&status,0)==child);
        if((i<5 || i>=10) && (!WIFEXITED(status) || WEXITSTATUS(status)))fprintf(stderr,"LA fault scenario=%u status=%x\n",i,status);
        if(i<5 || i>=10)CHECK(WIFEXITED(status) && !WEXITSTATUS(status));
        else CHECK(WIFSIGNALED(status) && WTERMSIG(status)==SIGSEGV);
    }
    puts("LA signal fault/recovery/badframe passed");
    kind=6;seen=0;action.sa_sigaction=handler;action.sa_flags=SA_SIGINFO;
    CHECK(!sigaction(SIGTRAP,&action,0) && !la_signal_register_probe() && seen==1);
    puts("LA signal integer registers passed");
    CHECK(!waiting());
    unlink("/signal-empty");return 0;
}
