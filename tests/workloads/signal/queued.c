#define _GNU_SOURCE
#include <signal.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
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
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);alarm(15);pending();
    printf("QUEUED END failures=%u\n",failures);return failures?1:0;
}
