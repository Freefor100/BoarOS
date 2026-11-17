#define _GNU_SOURCE
#include <sched.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int nested;
static int go[2][2];
struct raw_event {uint64_t value;int signal,notify,tid;char padding[44];};
static clockid_t encoded(int pid,int thread){return (clockid_t)((~(uint32_t)pid<<3)|(thread?4:0)|2);}
static void *worker(void *arg)
{
    (void)arg;int tid=syscall(SYS_gettid),id=-1;struct timespec ts;
    assert(tid!=getpid());assert(!clock_gettime(encoded(tid,0),&ts));
    errno=0;assert(clock_getres(encoded(tid,0),&ts)==-1&&errno==EINVAL);
    struct raw_event e={.notify=1};
    errno=0;assert(syscall(SYS_timer_create,encoded(tid,0),&e,&id)==-1&&errno==EINVAL);
    assert(!syscall(SYS_timer_create,encoded(tid,1),&e,&id));assert(!syscall(SYS_timer_delete,id));
    assert(!syscall(SYS_timer_create,encoded(getpid(),0),&e,&id));assert(!syscall(SYS_timer_delete,id));
    e=(struct raw_event){.signal=SIGUSR1,.notify=4,.tid=tid,.value=0x987};
    assert(!syscall(SYS_timer_create,CLOCK_MONOTONIC,&e,&id));
    struct itimerspec interval={.it_value={.tv_nsec=10000}};
    assert(!syscall(SYS_timer_settime,id,0,&interval,NULL));
    sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGUSR1);siginfo_t info;
    assert(sigwaitinfo(&mask,&info)==SIGUSR1&&info.si_code==SI_TIMER&&info.si_value.sival_int==0x987);
    assert(!syscall(SYS_timer_delete,id));
    e.tid=0;errno=0;assert(syscall(SYS_timer_create,CLOCK_MONOTONIC,&e,&id)==-1&&errno==EINVAL);
    return NULL;
}
static int child(void *arg)
{
    int i=(int)(long)arg;char token;if(nested)assert(getpid()==1);
    assert(read(go[i][0],&token,1)==1);
    pthread_t t;assert(!pthread_create(&t,NULL,worker,NULL));assert(!pthread_join(t,NULL));return 0;
}
int main(int argc,char **argv)
{
    nested=argc==2&&!strcmp(argv[1],"--nested");assert(argc==1||nested);
    sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGUSR1);assert(!pthread_sigmask(SIG_BLOCK,&mask,NULL));
    pid_t children[2];void *stacks[2];
    for(int i=0;i<2;i++){assert(!pipe(go[i]));stacks[i]=malloc(65536);assert(stacks[i]);children[i]=clone(child,(char *)stacks[i]+65536,SIGCHLD|(nested?CLONE_NEWPID:0),(void *)(long)i);assert(children[i]>0);}
    for(int i=0;i<2;i++){
        struct timespec ts;assert(!clock_gettime(encoded(children[i],0),&ts));
        errno=0;assert(clock_gettime(encoded(children[i],1),&ts)==-1&&errno==EINVAL);
        struct raw_event e={.signal=SIGUSR1,.notify=4,.tid=children[i]};int id;
        errno=0;assert(syscall(SYS_timer_create,CLOCK_MONOTONIC,&e,&id)==-1&&errno==EINVAL);
        assert(write(go[i][1],"x",1)==1);
    }
    for(int i=0;i<2;i++){int status;assert(waitpid(children[i],&status,0)==children[i]&&WIFEXITED(status)&&!WEXITSTATUS(status));free(stacks[i]);assert(!close(go[i][0])&&!close(go[i][1]));}
    puts(nested?"PIDNS CLOCK PASS nested sibling clocks and thread recipient":"PIDNS CLOCK PASS process thread strict clocks and timer recipient");return 0;
}
