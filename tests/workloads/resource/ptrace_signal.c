#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { if(!(x)){dprintf(2,"PTRACE SIGNAL FAIL line=%d errno=%d %s\n",__LINE__,errno,#x);_exit(1);} } while(0)
static volatile sig_atomic_t delivered;
static sigset_t child_events;
static void unexpected(int sig){(void)sig;_exit(91);}
static void received(int sig,siginfo_t *info,void *context)
{ (void)context;CHECK(sig==SIGUSR2 && info->si_code==SI_QUEUE && info->si_value.sival_int==0x56473829);delivered++; }
static void byte_send(int fd,char c){CHECK(write(fd,&c,1)==1);}
static void byte_wait(int fd,char c){char actual;CHECK(read(fd,&actual,1)==1 && actual==c);}
static void child_notice(pid_t child,int code,int status)
{
    siginfo_t info;struct timespec timeout={5,0};
    CHECK(sigtimedwait(&child_events,&info,&timeout)==SIGCHLD);
    if(info.si_pid!=child || info.si_code!=code || info.si_status!=status)
        dprintf(2,"PTRACE SIGNAL notice pid=%d/%d code=%d/%d status=%#x/%#x\n",info.si_pid,child,info.si_code,code,info.si_status,status);
    CHECK(info.si_pid==child && info.si_uid==getuid() && info.si_code==code && info.si_status==status);
}
static unsigned stopped(pid_t child,unsigned signal)
{
    int status;CHECK(waitpid(child,&status,__WALL)==child);
    if(!WIFSTOPPED(status)||(unsigned)WSTOPSIG(status)!=signal)
        dprintf(2,"PTRACE SIGNAL wait expected=%u actual=%#x\n",signal,status);
    CHECK(WIFSTOPPED(status) && (unsigned)WSTOPSIG(status)==signal);
    child_notice(child,(status>>16)==128 ? CLD_STOPPED : CLD_TRAPPED,
        (status>>16)==128 ? 0 : (status>>8)&0x7f);
    return (unsigned)status;
}
int main(void)
{
    sigemptyset(&child_events);sigaddset(&child_events,SIGCHLD);CHECK(!sigprocmask(SIG_BLOCK,&child_events,0));
    int ready[2],go[2];CHECK(!pipe(ready)&&!pipe(go));pid_t child=fork();CHECK(child>=0);
    if(!child) {
        struct sigaction a={.sa_handler=unexpected,.sa_flags=SA_RESETHAND};sigemptyset(&a.sa_mask);sigaddset(&a.sa_mask,SIGUSR2);CHECK(!sigaction(SIGUSR1,&a,0));
        struct sigaction b={.sa_sigaction=received,.sa_flags=SA_SIGINFO};CHECK(!sigaction(SIGUSR2,&b,0));
        struct sigaction ignored={.sa_handler=SIG_IGN};CHECK(!sigaction(SIGURG,&ignored,0));
        sigset_t blocked;sigemptyset(&blocked);sigaddset(&blocked,SIGUSR2);CHECK(!sigprocmask(SIG_BLOCK,&blocked,0));
        byte_send(ready[1],'r');byte_wait(go[0],'g');
        CHECK(!raise(SIGUSR1)); /* tracer suppresses: no action/mask commit */
        struct sigaction observed;sigset_t mask;CHECK(!sigaction(SIGUSR1,0,&observed) && observed.sa_handler==unexpected && (observed.sa_flags&SA_RESETHAND));
        CHECK(!sigprocmask(SIG_SETMASK,0,&mask) && !sigismember(&mask,SIGUSR1) && sigismember(&mask,SIGUSR2));
        CHECK(!raise(SIGUSR1)); /* tracer replaces it with blocked SIGUSR2 */
        CHECK(!sigaction(SIGUSR1,0,&observed) && observed.sa_handler==unexpected);
        CHECK(!sigpending(&mask) && sigismember(&mask,SIGUSR2) && !delivered);
        CHECK(!sigprocmask(SIG_UNBLOCK,&blocked,0) && delivered==1);
        CHECK(!raise(SIGURG)); /* ignored disposition still produces a trace stop */
        CHECK(!raise(SIGUSR1)); /* SIGCONT during this stop cancels injected group-stop. */
        _exit(27);
    }
    byte_wait(ready[0],'r');CHECK(!ptrace(PTRACE_SEIZE,child,0,0));CHECK(!ptrace(PTRACE_INTERRUPT,child,0,0));stopped(child,SIGTRAP);
    CHECK(!ptrace(PTRACE_CONT,child,0,0));byte_send(go[1],'g');
    stopped(child,SIGUSR1);siginfo_t info;CHECK(!ptrace(PTRACE_GETSIGINFO,child,0,&info) && info.si_signo==SIGUSR1);
    CHECK(!ptrace(PTRACE_CONT,child,0,0));
    stopped(child,SIGUSR1);info=(siginfo_t){.si_signo=SIGUSR2,.si_code=SI_QUEUE};info.si_pid=getpid();info.si_uid=getuid();info.si_value.sival_int=0x56473829;
    CHECK(!ptrace(PTRACE_SETSIGINFO,child,0,&info));CHECK(!ptrace(PTRACE_CONT,child,0,(void *)(uintptr_t)SIGUSR2));
    stopped(child,SIGUSR2);CHECK(!ptrace(PTRACE_GETSIGINFO,child,0,&info) && info.si_code==SI_QUEUE && info.si_value.sival_int==0x56473829);
    CHECK(!ptrace(PTRACE_CONT,child,0,(void *)(uintptr_t)SIGUSR2));
    stopped(child,SIGURG);CHECK(!ptrace(PTRACE_CONT,child,0,0));
    stopped(child,SIGUSR1);CHECK(!kill(child,SIGCONT));
    CHECK(!ptrace(PTRACE_CONT,child,0,(void *)(uintptr_t)SIGSTOP));
    CHECK((stopped(child,SIGTRAP)>>16)==128U);CHECK(!ptrace(PTRACE_CONT,child,0,0));
    stopped(child,SIGCONT);CHECK(!ptrace(PTRACE_CONT,child,0,(void *)(uintptr_t)SIGCONT));
    int status;CHECK(waitpid(child,&status,__WALL)==child && WIFEXITED(status) && WEXITSTATUS(status)==27);
    child_notice(child,CLD_EXITED,27);
    CHECK(!close(ready[0])&&!close(ready[1])&&!close(go[0])&&!close(go[1]));
    puts("PTRACE SIGNAL suppression reset-mask blocked-replacement siginfo ignored continue-cancels-stop PASS");return 0;
}
