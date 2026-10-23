#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/signalfd.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(v) do {if(!(v)){dprintf(2,"POSIX-PERF FAIL line=%d errno=%d\n",__LINE__,errno);exit(1);}}while(0)
static uint64_t now(void)
{struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
#include "signal_stack.h"
#include "../workloads/signal/queued_threads.h"
/* The old comparison and new states deliberately share one ELF. The old
 * entry performs no native timer operation on the reference kernel. */
struct native_event {uint64_t value;int32_t signal,notify,tid;uint8_t padding[44];};
struct posix_worker {
    pthread_barrier_t *gate;
    unsigned id,cpus,state,clock;
    uint64_t begin,end,cpu,check,signals,overrun,next,latency[1024];
};
static _Thread_local struct posix_worker *current;
static _Thread_local unsigned bad;
static void handler(int sig,siginfo_t *info,void *unused)
{
    (void)unused;
    if(!current||sig!=SIGRTMIN+6||info->si_code!=SI_TIMER||
       (uintptr_t)info->si_value.sival_ptr!=current->id+1U||info->si_overrun<0){bad++;return;}
    uint64_t at=queue_clock(current->clock);
    uint64_t due=current->next+(unsigned)info->si_overrun*UINT64_C(10000000);
    if(current->signals<1024)current->latency[current->signals]=at>due?at-due:0;
    current->next=due+10000000;current->signals++;current->overrun+=(unsigned)info->si_overrun;
}
static void *compute(void *arg)
{
    struct posix_worker *w=arg;current=w;queue_pin(w->id%w->cpus);bad=0;
    sigset_t set;sigemptyset(&set);sigaddset(&set,SIGRTMIN+6);CHECK(!pthread_sigmask(SIG_BLOCK,&set,0));
    int id=-1;
    if(w->state) {
        struct native_event event={.value=w->id+1,.signal=SIGRTMIN+6,.notify=4,.tid=syscall(SYS_gettid)};
        CHECK(!syscall(SYS_timer_create,w->clock,&event,&id));
        if(w->state>=2){struct itimerspec in={.it_value={w->state==2?60:0,w->state==3?10000000:0},.it_interval={0,w->state==3?10000000:0}};
            w->next=queue_clock(w->clock)+10000000;
            CHECK(!syscall(SYS_timer_settime,id,0,&in,0));}
    }
    queue_gate(w->gate);CHECK(!pthread_sigmask(SIG_UNBLOCK,&set,0));
    w->cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID);w->begin=now();uint64_t value=1;
    for(unsigned i=0;i<25000000;i++){value=(value<<3)^(value>>7)^17;__asm__ volatile("":"+r"(value));}
    w->end=now();w->cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID)-w->cpu;w->check=value;
    CHECK(!pthread_sigmask(SIG_BLOCK,&set,0));
    if(w->state)CHECK(!syscall(SYS_timer_delete,id));
    siginfo_t info;struct timespec zero={0};while(sigtimedwait(&set,&info,&zero)>0){}
    CHECK(!bad&&(w->state==3?w->signals>0:w->signals==0));current=0;
    return 0;
}
static int new_cost(unsigned cpus)
{
    struct sigaction action={.sa_sigaction=handler,.sa_flags=SA_SIGINFO};sigemptyset(&action.sa_mask);CHECK(!sigaction(SIGRTMIN+6,&action,0));
    const char *states[]={"uncreated","disarmed","armed60s","periodic10ms"};
    for(unsigned clock=0;clock<4;clock++)for(unsigned state=0;state<4;state++) {
        pthread_barrier_t gate;CHECK(!pthread_barrier_init(&gate,0,5));struct posix_worker workers[4];pthread_t threads[4];
        for(unsigned i=0;i<4;i++){workers[i]=(struct posix_worker){.gate=&gate,.id=i,.cpus=cpus,.state=state,.clock=clock};CHECK(!pthread_create(&threads[i],0,compute,&workers[i]));}
        queue_gate(&gate);for(unsigned i=0;i<4;i++)CHECK(!pthread_join(threads[i],0));
        uint64_t begin=UINT64_MAX,end=0,cpu=0,signals=0,overrun=0,latency[4096];unsigned count=0;
        for(unsigned i=0;i<4;i++) {
            CHECK(workers[i].check==workers[0].check);
            if(workers[i].begin<begin)begin=workers[i].begin;
            if(workers[i].end>end)end=workers[i].end;
            cpu+=workers[i].cpu;signals+=workers[i].signals;overrun+=workers[i].overrun;
            for(unsigned j=0;j<workers[i].signals&&j<1024;j++)latency[count++]=workers[i].latency[j];
        }
        if(count)qsort(latency,count,sizeof(*latency),queue_compare);
        struct rusage usage;CHECK(!getrusage(RUSAGE_SELF,&usage));
        printf("POSIX-PERF clock=%u state=%s iterations=100000000 ns=%llu cpu_ns=%llu signals=%llu overrun=%llu p50=%llu p95=%llu max=%llu rss_kib=%ld check=%llu\n",
            clock,states[state],(unsigned long long)(end-begin),(unsigned long long)cpu,(unsigned long long)signals,(unsigned long long)overrun,
            (unsigned long long)(count?latency[count/2]:0),(unsigned long long)(count?latency[count*95/100]:0),(unsigned long long)(count?latency[count-1]:0),usage.ru_maxrss,(unsigned long long)workers[0].check);
        CHECK(!pthread_barrier_destroy(&gate));
    }
    puts("POSIX-PERF NEW END 16");return 0;
}
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);FILE *config=fopen("/account-posix","r");CHECK(config);unsigned cpus,kind;
    CHECK(fscanf(config,"%u %u",&kind,&cpus)==2&&!fclose(config)&&(cpus==1||cpus==2||cpus==4));
    if(kind)return new_cost(cpus);
    config=fopen("/account-altstack","r");CHECK(config&&!stack_cost_main(config));
    uint64_t affinity=(UINT64_C(1)<<cpus)-1;CHECK(!syscall(SYS_sched_setaffinity,0,8,&affinity));
    queue_perf=1;queue_perf_mode=1;queued_threads();puts("POSIX-PERF OLD END 3");return 0;
}
