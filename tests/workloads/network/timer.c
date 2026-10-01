#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"timer:%d: %s errno=%d\n",__LINE__,#x,errno); exit(1); } } while(0)
static volatile sig_atomic_t delivered;
static void handler(int sig) { (void)sig; delivered++; }
static void *exec_member(void *unused)
{
    (void)unused; execl("/init","timer","exec",NULL);_exit(3);
}
static void *surviving_member(void *unused)
{
    (void)unused; sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGALRM);
    siginfo_t info;CHECK(sigwaitinfo(&mask,&info)==SIGALRM && info.si_code==SI_KERNEL);_exit(0);
}
int main(int argc, char **argv)
{
    (void)argv; setbuf(stdout,NULL);
    struct itimerval timer={0}, state={0};
    if(argc>1) {
        CHECK(getitimer(ITIMER_REAL,&state)==0 && (state.it_value.tv_sec||state.it_value.tv_usec));
        sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGALRM);
        siginfo_t info;CHECK(sigwaitinfo(&mask,&info)==SIGALRM && info.si_code==SI_KERNEL);
        return 0;
    }
    CHECK(getitimer(ITIMER_REAL,&state)==0 && state.it_value.tv_sec==0 && state.it_value.tv_usec==0);
    CHECK(getitimer(3,&state)==-1 && errno==EINVAL);
    CHECK(syscall(SYS_getitimer,0,1)==-1 && errno==EFAULT);
    timer.it_value.tv_usec=1000000;
    CHECK(setitimer(ITIMER_REAL,&timer,NULL)==-1 && errno==EINVAL);
    struct sigaction action={.sa_handler=handler}; sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGALRM,&action,NULL)==0);
    int pipefd[2]; CHECK(pipe(pipefd)==0);
    timer=(struct itimerval){.it_value={0,20000}};
    CHECK(setitimer(ITIMER_REAL,&timer,NULL)==0);
    char byte; CHECK(read(pipefd[0],&byte,1)==-1 && errno==EINTR && delivered==1);
    CHECK(close(pipefd[0])==0 && close(pipefd[1])==0);
    timer=(struct itimerval){.it_interval={0,10000},.it_value={0,10000}};
    CHECK(setitimer(ITIMER_REAL,&timer,NULL)==0);
    while(delivered<4) pause();
    CHECK(syscall(SYS_setitimer,0,0,0)==0);
    CHECK(getitimer(ITIMER_REAL,&state)==0 && state.it_interval.tv_usec==0 && state.it_value.tv_usec==0);
    timer=(struct itimerval){.it_value={1,0}};
    CHECK(setitimer(ITIMER_REAL,&timer,NULL)==0);
    pid_t child=fork(); CHECK(child>=0);
    if(!child) {
        CHECK(getitimer(ITIMER_REAL,&state)==0 && state.it_value.tv_sec==0 && state.it_value.tv_usec==0);
        _exit(0);
    }
    int status; CHECK(waitpid(child,&status,0)==child && status==0);
    CHECK(getitimer(ITIMER_REAL,&state)==0 && (state.it_value.tv_sec||state.it_value.tv_usec));
    CHECK(setitimer(ITIMER_REAL,&(struct itimerval){0},NULL)==0);
    sigset_t mask; sigemptyset(&mask); sigaddset(&mask,SIGALRM);
    CHECK(sigprocmask(SIG_BLOCK,&mask,NULL)==0);
    timer=(struct itimerval){.it_value={0,10000}};
    CHECK(setitimer(ITIMER_REAL,&timer,NULL)==0);
    siginfo_t info;
    CHECK(sigwaitinfo(&mask,&info)==SIGALRM && info.si_code==SI_KERNEL && info.si_pid==0);
    timer=(struct itimerval){.it_value={1,0}};
    CHECK(syscall(SYS_setitimer,0,&timer,1)==-1 && errno==EFAULT);
    CHECK(getitimer(ITIMER_REAL,&state)==0 && (state.it_value.tv_sec||state.it_value.tv_usec));
    CHECK(setitimer(ITIMER_REAL,&(struct itimerval){0},NULL)==0);
    for(unsigned kind=0;kind<2;kind++) {
        child=fork();CHECK(child>=0);
        if(!child) {
            timer=(struct itimerval){.it_value={0,200000}};
            CHECK(setitimer(ITIMER_REAL,&timer,NULL)==0);
            pthread_t thread;CHECK(pthread_create(&thread,NULL,kind?surviving_member:exec_member,NULL)==0);
            if(kind) { syscall(SYS_exit,0);_exit(4); }
            for(;;) pause();
        }
        CHECK(waitpid(child,&status,0)==child && status==0);
    }
    CHECK(sigprocmask(SIG_UNBLOCK,&mask,NULL)==0);
    puts("NETWORK PASS timer: alarm, interrupt, periodic, fork, exec adoption, leader exit and kernel siginfo");
    return 0;
}
