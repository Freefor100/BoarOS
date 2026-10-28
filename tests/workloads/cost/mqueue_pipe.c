#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define CHECK(v) do { if(!(v)) { dprintf(2,"MQPIPE FAIL line=%d errno=%d\n",__LINE__,errno);exit(1); } } while(0)
enum { PAIRS=4, ACTORS=8, FORMAL_ROUNDS=4096, STREAM_BLOCK=65536, STREAM_BYTES=16*1024*1024 };
static unsigned cpu_ids[4],cpu_count,smoke,windows;
static uint64_t all_checks;
static uint64_t queue_clock(clockid_t id)
{ struct timespec t;CHECK(!clock_gettime(id,&t));return (uint64_t)t.tv_sec*1000000000U+(uint64_t)t.tv_nsec; }
static uint64_t now(void) { return queue_clock(CLOCK_MONOTONIC); }
static void queue_gate(pthread_barrier_t *gate)
{ int n=pthread_barrier_wait(gate);CHECK(!n || n==PTHREAD_BARRIER_SERIAL_THREAD); }
static int queue_compare(const void *a,const void *b)
{ uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;return (x>y)-(x<y); }
static void queue_pin(unsigned role)
{
    cpu_set_t mask,actual;CPU_ZERO(&mask);CPU_SET(cpu_ids[role%cpu_count],&mask);
    CHECK(!sched_setaffinity(0,sizeof(mask),&mask));CPU_ZERO(&actual);
    CHECK(!sched_getaffinity(0,sizeof(actual),&actual) && !memcmp(&mask,&actual,sizeof(mask)));
}
/* Reuse the existing WAIT/WAKE workers, sequence checks and sample positions.
 * Only the old three-window reporter is unused by this combined workload. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../../accounting/futex.h"
#pragma GCC diagnostic pop

static void metric(const char *name,uint64_t wall,uint64_t cpu,uint64_t *samples,
    unsigned count,uint64_t work,uint64_t bytes,uint64_t checks,unsigned workers)
{
    CHECK(wall && checks);
    if(count)qsort(samples,count,sizeof(*samples),queue_compare);
    struct rusage usage;CHECK(!getrusage(RUSAGE_SELF,&usage));
    printf("MQPIPE RESULT name=%s wall_ns=%llu cpu_ns=%llu p50_ns=%llu p95_ns=%llu max_ns=%llu work=%llu bytes=%llu checks=%llu rss_kib=%ld samples=%u workers=%u\n",
        name,(unsigned long long)wall,(unsigned long long)cpu,
        (unsigned long long)(count?samples[count/2]:0),
        (unsigned long long)(count?samples[count*95/100]:0),
        (unsigned long long)(count?samples[count-1]:0),
        (unsigned long long)work,(unsigned long long)bytes,(unsigned long long)checks,
        usage.ru_maxrss,count,workers);
    windows++;all_checks+=checks;
}
struct clock_span { uint64_t begin,end,cpu; };
static void spans(struct clock_span *s,unsigned count,uint64_t *wall,uint64_t *cpu)
{
    uint64_t first=UINT64_MAX,last=0;*cpu=0;
    for(unsigned i=0;i<count;i++) {
        CHECK(s[i].begin && s[i].end>=s[i].begin);
        if(s[i].begin<first)first=s[i].begin;
        if(s[i].end>last)last=s[i].end;
        *cpu+=s[i].cpu;
    }
    *wall=last-first;
}

/* The unregistered-stack getpid/tgkill work is the fixed body from
 * accounting/signal_stack.h, with physical CPUs imported from the allowed set. */
struct legacy_actor {
    pthread_barrier_t *gate;unsigned id,getpids,signals;
    struct clock_span spans[2];uint64_t samples[4096];
};
static _Thread_local volatile sig_atomic_t received_signals;
static void ordinary_signal(int number) { CHECK(number==SIGUSR1);received_signals++; }
static void *legacy_run(void *opaque)
{
    struct legacy_actor *a=opaque;queue_pin(a->id);
    pid_t pid=getpid(),tid=(pid_t)syscall(SYS_gettid);received_signals=0;
    queue_gate(a->gate);uint64_t cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID);a->spans[0].begin=now();
    for(unsigned i=0;i<a->getpids;i++)CHECK(syscall(SYS_getpid)==pid);
    a->spans[0].end=now();a->spans[0].cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID)-cpu;
    queue_gate(a->gate);cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID);a->spans[1].begin=now();
    for(unsigned i=0;i<a->signals;i++) {
        uint64_t begin=now();CHECK(!syscall(SYS_tgkill,pid,tid,SIGUSR1));
        a->samples[i]=now()-begin;CHECK((unsigned)received_signals==i+1);
    }
    a->spans[1].end=now();a->spans[1].cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID)-cpu;
    CHECK((unsigned)received_signals==a->signals);return NULL;
}
static void legacy_signal_windows(void)
{
    unsigned getpids=smoke?4096:1000000,signals=smoke?64:4096;
    struct sigaction action={.sa_handler=ordinary_signal};sigemptyset(&action.sa_mask);
    CHECK(!sigaction(SIGUSR1,&action,NULL));sigset_t set;sigemptyset(&set);sigaddset(&set,SIGUSR1);
    CHECK(!pthread_sigmask(SIG_UNBLOCK,&set,NULL));
    struct legacy_actor *a=calloc(cpu_count,sizeof(*a));CHECK(a);pthread_t threads[4];pthread_barrier_t gate;
    CHECK(!pthread_barrier_init(&gate,NULL,cpu_count+1));
    for(unsigned i=0;i<cpu_count;i++) {
        a[i].gate=&gate;a[i].id=i;a[i].getpids=getpids/cpu_count;a[i].signals=signals/cpu_count;
        /* Touch sample storage before releasing the timed workers. */
        memset(a[i].samples,0,sizeof(a[i].samples));
        CHECK(!pthread_create(&threads[i],NULL,legacy_run,&a[i]));
    }
    queue_gate(&gate);queue_gate(&gate);
    for(unsigned i=0;i<cpu_count;i++)CHECK(!pthread_join(threads[i],NULL));
    struct clock_span s[4];uint64_t wall,cpu,samples[4096];
    for(unsigned i=0;i<cpu_count;i++)s[i]=a[i].spans[0];
    spans(s,cpu_count,&wall,&cpu);
    metric("legacy-getpid",wall,cpu,NULL,0,getpids,0,getpids,cpu_count);
    for(unsigned i=0;i<cpu_count;i++) {
        s[i]=a[i].spans[1];memcpy(samples+i*(signals/cpu_count),a[i].samples,(signals/cpu_count)*sizeof(*samples));
    }
    spans(s,cpu_count,&wall,&cpu);metric("legacy-signal",wall,cpu,samples,signals,signals,0,signals,cpu_count);
    CHECK(!pthread_barrier_destroy(&gate));free(a);
}
static void legacy_futex_window(void)
{
    struct futex_perf_work *w=calloc(1,sizeof(*w));CHECK(w);w->cpus=cpu_count;
    memset(w->samples,0,sizeof(w->samples));CHECK(!pthread_barrier_init(&w->gate,NULL,ACTORS+1));
    pthread_t threads[ACTORS];struct futex_perf_actor actors[ACTORS];
    for(unsigned i=0;i<ACTORS;i++) {actors[i]=(struct futex_perf_actor){w,i};CHECK(!pthread_create(&threads[i],NULL,futex_perf_run,&actors[i]));}
    queue_gate(&w->gate);uint64_t begin=now();queue_gate(&w->gate);queue_gate(&w->gate);uint64_t wall=now()-begin,cpu=0;
    for(unsigned i=0;i<ACTORS;i++) {CHECK(w->completed[i]==1024);cpu+=w->cpu[i];CHECK(!pthread_join(threads[i],NULL));}
    for(unsigned i=0;i<PAIRS;i++)CHECK(w->pairs[i].sequence==2048 &&
        w->pairs[i].request==futex_perf_value(i,1023) && w->pairs[i].reply==~futex_perf_value(i,1023));
    metric("legacy-futex",wall,cpu,w->samples,FORMAL_ROUNDS,FORMAL_ROUNDS,FORMAL_ROUNDS*16U,FORMAL_ROUNDS*2U,ACTORS);
    CHECK(!pthread_barrier_destroy(&w->gate));free(w);
}

struct stream_pair { int fd[2];unsigned id,rounds;pthread_barrier_t *gate;unsigned char *pattern; };
struct stream_actor { struct stream_pair *pair;unsigned side;struct clock_span span;uint64_t samples[64]; };
static void exact(int fd,void *buffer,size_t count,int writing)
{
    unsigned char *p=buffer;
    while(count) {ssize_t n=writing?write(fd,p,count):read(fd,p,count);if(n<0 && errno==EINTR)continue;CHECK(n>0 && (size_t)n<=count);p+=n;count-=(size_t)n;}
}
static void *stream_run(void *opaque)
{
    struct stream_actor *a=opaque;struct stream_pair *p=a->pair;queue_pin(p->id+a->side);
    unsigned char *buffer=malloc(STREAM_BLOCK);CHECK(buffer);memcpy(buffer,p->pattern,STREAM_BLOCK);
    queue_gate(p->gate);uint64_t cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID);a->span.begin=now();
    for(unsigned i=0;i<p->rounds;i++) {
        uint64_t sequence=((uint64_t)p->id<<32)|i,begin=now();
        if(!a->side) {memcpy(buffer,&sequence,sizeof(sequence));exact(p->fd[0],buffer,STREAM_BLOCK,1);}
        else {
            exact(p->fd[1],buffer,STREAM_BLOCK,0);uint64_t actual;memcpy(&actual,buffer,sizeof(actual));
            CHECK(actual==sequence && !memcmp(buffer+8,p->pattern+8,STREAM_BLOCK-8));a->samples[i]=now()-begin;
        }
    }
    a->span.end=now();a->span.cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID)-cpu;free(buffer);return NULL;
}
static void stream_window(const char *name,int socket_kind,unsigned capacity)
{
    unsigned bytes=smoke?256*1024:STREAM_BYTES,rounds=bytes/PAIRS/STREAM_BLOCK;
    struct stream_pair p[PAIRS];struct stream_actor a[ACTORS]={0};pthread_t threads[ACTORS];pthread_barrier_t gate;
    CHECK(rounds>0 && rounds<=64 && !pthread_barrier_init(&gate,NULL,ACTORS+1));
    for(unsigned i=0;i<PAIRS;i++) {
        p[i]=(struct stream_pair){.id=i,.rounds=rounds,.gate=&gate};
        CHECK(!(socket_kind?socketpair(AF_UNIX,SOCK_STREAM,0,p[i].fd):pipe(p[i].fd)));
        /* exact() uses writer slot 0 and reader slot 1 for both backends. */
        if(!socket_kind) {int swap=p[i].fd[0];p[i].fd[0]=p[i].fd[1];p[i].fd[1]=swap;}
        if(capacity)CHECK(fcntl(p[i].fd[0],F_SETPIPE_SZ,(int)capacity)==(int)capacity);
        p[i].pattern=malloc(STREAM_BLOCK);CHECK(p[i].pattern);memset(p[i].pattern,0x30+(int)i,STREAM_BLOCK);
    }
    for(unsigned i=0;i<ACTORS;i++) {a[i].pair=&p[i/2];a[i].side=i%2;CHECK(!pthread_create(&threads[i],NULL,stream_run,&a[i]));}
    queue_gate(&gate);for(unsigned i=0;i<ACTORS;i++)CHECK(!pthread_join(threads[i],NULL));
    struct clock_span s[ACTORS];uint64_t samples[256],wall,cpu;
    for(unsigned i=0;i<ACTORS;i++)s[i]=a[i].span;
    for(unsigned i=0;i<PAIRS;i++)memcpy(samples+i*rounds,a[2*i+1].samples,rounds*sizeof(*samples));
    spans(s,ACTORS,&wall,&cpu);metric(name,wall,cpu,samples,PAIRS*rounds,PAIRS*rounds,bytes,PAIRS*rounds,ACTORS);
    for(unsigned i=0;i<PAIRS;i++) {CHECK(!close(p[i].fd[0]) && !close(p[i].fd[1]));free(p[i].pattern);}
    CHECK(!pthread_barrier_destroy(&gate));
}

struct mq_pair { mqd_t fd[2];unsigned id,size,rounds,epoll;pthread_barrier_t *gate;unsigned char pattern[4096]; };
struct mq_actor { struct mq_pair *pair;unsigned side;struct clock_span span;uint64_t samples[1024]; };
static void mq_send_checked(mqd_t fd,const unsigned char *buffer,unsigned size,unsigned priority)
{ int r;do {r=mq_send(fd,(const char *)buffer,size,priority);}while(r<0 && errno==EINTR);CHECK(!r); }
static void mq_receive_checked(mqd_t fd,unsigned char *buffer,unsigned size,unsigned priority,uint64_t sequence,const unsigned char *pattern,int ep,unsigned tag)
{
    if(ep>=0) {struct epoll_event event;int n;do {n=epoll_wait(ep,&event,1,-1);}while(n<0 && errno==EINTR);CHECK(n==1 && event.data.u64==tag && event.events==EPOLLIN);}
    unsigned actual_priority=UINT32_MAX;ssize_t n;
    do {n=mq_receive(fd,(char *)buffer,size,&actual_priority);}while(n<0 && errno==EINTR);
    uint64_t actual;memcpy(&actual,buffer,sizeof(actual));
    CHECK(n==(ssize_t)size && actual_priority==priority && actual==sequence && !memcmp(buffer+8,pattern+8,size-8));
}
static void *mq_run(void *opaque)
{
    struct mq_actor *a=opaque;struct mq_pair *p=a->pair;queue_pin(p->id+a->side);
    unsigned char buffer[4096];memcpy(buffer,p->pattern,sizeof(buffer));int ep=-1;unsigned tag=p->id*2+a->side+1;
    if(p->epoll) {ep=epoll_create1(EPOLL_CLOEXEC);CHECK(ep>=0);struct epoll_event event={.events=EPOLLIN,.data.u64=tag};CHECK(!epoll_ctl(ep,EPOLL_CTL_ADD,(int)p->fd[a->side],&event));}
    queue_gate(p->gate);uint64_t cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID);a->span.begin=now();
    for(unsigned i=0;i<p->rounds;i++) {
        uint64_t value=UINT64_C(0xabcdef1200000000)+(uint64_t)p->id*1024+i,begin=now();
        unsigned priority=(p->id*7+i)%32,reply_priority=(priority+11)%32;
        if(!a->side) {
            memcpy(buffer,&value,sizeof(value));mq_send_checked(p->fd[1],buffer,p->size,priority);
            mq_receive_checked(p->fd[0],buffer,p->size,reply_priority,~value,p->pattern,ep,tag);
            a->samples[i]=now()-begin;
        } else {
            mq_receive_checked(p->fd[1],buffer,p->size,priority,value,p->pattern,ep,tag);
            value=~value;memcpy(buffer,&value,sizeof(value));mq_send_checked(p->fd[0],buffer,p->size,reply_priority);
        }
    }
    a->span.end=now();a->span.cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID)-cpu;
    if(ep>=0)CHECK(!close(ep));
    return NULL;
}
static void mqueue_window(unsigned size,unsigned epoll_mode)
{
    unsigned total=smoke?64:FORMAL_ROUNDS,rounds=total/PAIRS;
    struct mq_pair *p=calloc(PAIRS,sizeof(*p));struct mq_actor *a=calloc(ACTORS,sizeof(*a));CHECK(p && a);
    pthread_t threads[ACTORS];pthread_barrier_t gate;CHECK(!pthread_barrier_init(&gate,NULL,ACTORS+1));
    for(unsigned i=0;i<PAIRS;i++) {
        p[i].id=i;p[i].size=size;p[i].rounds=rounds;p[i].epoll=epoll_mode;p[i].gate=&gate;
        for(unsigned j=0;j<sizeof(p[i].pattern);j++)p[i].pattern[j]=(unsigned char)(0x51+i*37+j*13);
        for(unsigned side=0;side<2;side++) {
            char name[96];CHECK(snprintf(name,sizeof(name),"/mqpipe-%ld-%u-%u-%u",(long)getpid(),windows,i,side)>0);
            struct mq_attr attr={.mq_maxmsg=8,.mq_msgsize=size};
            p[i].fd[side]=mq_open(name,O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600,&attr);
            CHECK(p[i].fd[side]!=(mqd_t)-1 && !mq_unlink(name));
        }
        /* Priority selection is checked with an actually queued set before
         * timing; round trips then verify sequence, content and metadata. */
        const unsigned priorities[3]={1,9,5},order[3]={1,2,0};unsigned char buffer[4096];
        memcpy(buffer,p[i].pattern,sizeof(buffer));
        for(unsigned j=0;j<3;j++) {uint64_t value=j;memcpy(buffer,&value,sizeof(value));mq_send_checked(p[i].fd[1],buffer,size,priorities[j]);}
        for(unsigned j=0;j<3;j++)mq_receive_checked(p[i].fd[1],buffer,size,priorities[order[j]],order[j],p[i].pattern,-1,0);
    }
    for(unsigned i=0;i<ACTORS;i++) {a[i].pair=&p[i/2];a[i].side=i%2;CHECK(!pthread_create(&threads[i],NULL,mq_run,&a[i]));}
    queue_gate(&gate);for(unsigned i=0;i<ACTORS;i++)CHECK(!pthread_join(threads[i],NULL));
    struct clock_span s[ACTORS];uint64_t samples[FORMAL_ROUNDS],wall,cpu;
    for(unsigned i=0;i<ACTORS;i++)s[i]=a[i].span;
    for(unsigned i=0;i<PAIRS;i++)memcpy(samples+i*rounds,a[i*2].samples,rounds*sizeof(*samples));
    spans(s,ACTORS,&wall,&cpu);char name[48];CHECK(snprintf(name,sizeof(name),"mq-%s-%u",epoll_mode?"epoll":"block",size)>0);
    metric(name,wall,cpu,samples,total,total,(uint64_t)total*size*2,(uint64_t)total*2+PAIRS*3,ACTORS);
    for(unsigned i=0;i<PAIRS;i++)CHECK(!mq_close(p[i].fd[0]) && !mq_close(p[i].fd[1]));
    CHECK(!pthread_barrier_destroy(&gate));free(a);free(p);
}

struct resize_actor { int fd[2];unsigned id,rounds,page;pthread_barrier_t *gate;struct clock_span span;uint64_t samples[256]; };
static void *resize_run(void *opaque)
{
    struct resize_actor *a=opaque;queue_pin(a->id);queue_gate(a->gate);
    uint64_t cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID);a->span.begin=now();
    for(unsigned i=0;i<a->rounds;i++) {uint64_t begin=now();CHECK(fcntl(a->fd[0],F_SETPIPE_SZ,1024*1024)==1024*1024);CHECK(fcntl(a->fd[0],F_SETPIPE_SZ,(int)a->page)==(int)a->page);a->samples[i]=now()-begin;}
    a->span.end=now();a->span.cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID)-cpu;return NULL;
}
static void resize_window(unsigned page)
{
    unsigned total=smoke?16:1024,rounds=total/PAIRS;struct resize_actor a[PAIRS]={0};pthread_t threads[PAIRS];pthread_barrier_t gate;
    CHECK(!pthread_barrier_init(&gate,NULL,PAIRS+1));
    for(unsigned i=0;i<PAIRS;i++) {
        a[i].id=i;a[i].rounds=rounds;a[i].page=page;a[i].gate=&gate;
        CHECK(!pipe(a[i].fd) && fcntl(a[i].fd[0],F_SETPIPE_SZ,(int)page)==(int)page);
        CHECK(!pthread_create(&threads[i],NULL,resize_run,&a[i]));
    }
    queue_gate(&gate);for(unsigned i=0;i<PAIRS;i++)CHECK(!pthread_join(threads[i],NULL));
    struct clock_span s[PAIRS];uint64_t samples[1024],wall,cpu;
    for(unsigned i=0;i<PAIRS;i++) {s[i]=a[i].span;memcpy(samples+i*rounds,a[i].samples,rounds*sizeof(*samples));}
    spans(s,PAIRS,&wall,&cpu);metric("pipe-resize",wall,cpu,samples,total,total,0,(uint64_t)total*2,PAIRS);
    for(unsigned i=0;i<PAIRS;i++)CHECK(!close(a[i].fd[0]) && !close(a[i].fd[1]));
    CHECK(!pthread_barrier_destroy(&gate));
}
int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0);char configured_mode[16]="all",profile[16]="formal";const char *mode=configured_mode;unsigned requested=0;
    if(argc==1) {FILE *config=fopen("/mqueue-pipe-cost","r");if(config) {int n=fscanf(config,"%15s %u %15s",configured_mode,&requested,profile);CHECK(n==2 || n==3);CHECK(!fclose(config));CHECK(!strcmp(profile,"formal") || !strcmp(profile,"smoke"));smoke=!strcmp(profile,"smoke");}else CHECK(errno==ENOENT);}
    for(int i=1;i<argc;i++) {
        if(!strcmp(argv[i],"--smoke"))smoke=1;
        else if(!strcmp(argv[i],"--cpus")) {CHECK(++i<argc);char *end;unsigned long value=strtoul(argv[i],&end,10);CHECK(*argv[i] && !*end && value<=4);requested=(unsigned)value;}
        else {CHECK(!strcmp(argv[i],"legacy") || !strcmp(argv[i],"features") || !strcmp(argv[i],"all"));mode=argv[i];}
    }
    CHECK(!strcmp(mode,"legacy") || !strcmp(mode,"features") || !strcmp(mode,"all"));
    cpu_set_t allowed;CPU_ZERO(&allowed);CHECK(!sched_getaffinity(0,sizeof(allowed),&allowed));
    unsigned found=0;for(unsigned i=0;i<CPU_SETSIZE && found<4;i++)if(CPU_ISSET(i,&allowed))cpu_ids[found++]=i;
    CHECK(found);cpu_count=requested?requested:(found>=4?4:found>=2?2:1);
    CHECK((cpu_count==1 || cpu_count==2 || cpu_count==4) && cpu_count<=found);queue_pin(0);
    printf("MQPIPE BEGIN version=1 mode=%s profile=%s cpus=%u cpu0=%u cpu1=%d cpu2=%d cpu3=%d\n",mode,smoke?"smoke":"formal",cpu_count,cpu_ids[0],cpu_count>1?(int)cpu_ids[1]:-1,cpu_count>2?(int)cpu_ids[2]:-1,cpu_count>3?(int)cpu_ids[3]:-1);
    if(strcmp(mode,"features")) {legacy_signal_windows();legacy_futex_window();stream_window("legacy-pipe-default",0,0);stream_window("legacy-unix",1,0);}
    if(strcmp(mode,"legacy")) {
        for(unsigned ep=0;ep<2;ep++)for(unsigned size=64;size<=4096;size*=64)mqueue_window(size,ep);
        long page=sysconf(_SC_PAGESIZE);CHECK(page>0 && page<=1024*1024);
        stream_window("pipe-default",0,0);stream_window("pipe-onepage",0,(unsigned)page);stream_window("pipe-1m",0,1024*1024);resize_window((unsigned)page);
    }
    CHECK(windows==(!strcmp(mode,"all")?13U:!strcmp(mode,"legacy")?5U:8U));
    printf("MQPIPE END windows=%u checks=%llu\n",windows,(unsigned long long)all_checks);return 0;
}
