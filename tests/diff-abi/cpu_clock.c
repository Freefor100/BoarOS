#include "abi.h"

static long clock_id(long pid,unsigned kind,int thread)
{ return (int)(~(unsigned)pid<<3 | kind | (thread ? 4U : 0U)); }
static long clock_read(long id,long *t) {return SC2(113,id,t);}
static unsigned long clock_value(const long *t) {return t[0]*1000000000UL+t[1];}
static unsigned long wall_ns(void)
{long t[2];abi_require(!clock_read(1,t));return clock_value(t);}
static void clock_burn(void)
{
    unsigned long end=wall_ns()+20000000UL,v=1;
    do {for(int i=0;i<100000;i++) {v=(v<<3)^(v>>7)^17;__asm__ volatile("" : "+r"(v));}}
    while(wall_ns()<end);
}
struct clock_worker {int ready,release;long tid,alias_time,alias_res;};
static unsigned char clock_stack[16384] __attribute__((aligned(16)));
extern long abi_clone_entry(long,void *,int *,void (*)(void *),void *);
static void clock_worker(void *opaque)
{
    struct clock_worker *w=opaque;long t[2];w->tid=SC0(178);clock_burn();
    w->alias_time=clock_read(clock_id(w->tid,2,0),t);
    w->alias_res=SC2(114,clock_id(w->tid,2,0),t);
    __atomic_store_n(&w->ready,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&w->release,__ATOMIC_ACQUIRE))SC0(124);
}
void abi_cpu_clock_cases(void)
{
    long t[2]={-1,-1},before[2],after[2];long pid=SC0(172),tid=SC0(178);
    long ret=clock_read(2,t);
    abi_record("cpu-clock.process",ret,-1,-1,0,0,0);abi_require(!ret);
    abi_record("cpu-clock.layout",t[0]>=0 && t[1]>=0 && t[1]<1000000000L,-1,-1,0,0,0);
    abi_record("cpu-clock.thread",clock_read(3,t),-1,-1,0,0,0);
    int valid=1;
    for(unsigned kind=0;kind<3;kind++)for(int thread=0;thread<2;thread++) {
        valid &= !clock_read(clock_id(0,kind,thread),t);
        valid &= !clock_read(clock_id(thread ? tid : pid,kind,thread),t);
    }
    abi_record("cpu-clock.encoded",valid,-1,-1,0,0,0);
    abi_require(!clock_read(3,before));clock_burn();abi_require(!clock_read(3,after));
    abi_record("cpu-clock.compute",clock_value(after)>clock_value(before),-1,-1,0,0,0);
    abi_require(!clock_read(3,before));unsigned long start=wall_ns();long delay[2]={0,100000000};
    abi_require(!SC2(101,delay,0) && !clock_read(3,after));
    abi_record("cpu-clock.sleep",clock_value(after)-clock_value(before)<(wall_ns()-start)/2,-1,-1,0,0,0);
    valid=1;
    for(int id=2;id<=3;id++) {valid &= !SC2(114,id,t) && t[0]==0 && t[1]==1;}
    abi_record("cpu-clock.resolution",valid,-1,-1,0,0,0);
    valid=1;
    for(unsigned kind=0;kind<3;kind++) {
        valid &= !SC2(114,clock_id(pid,kind,0),t) && t[0]==0 && t[1]>0 && t[1]<=1000000000L;
        if(kind==2) valid &= t[1]==1;
    }
    abi_record("cpu-clock.encoded-resolution",valid,-1,-1,0,0,0);
    abi_record("cpu-clock.res-null",SC2(114,2,0),-1,-1,0,0,0);
    abi_record("cpu-clock.signed32",clock_read(0x100000002UL,t),-1,-1,0,0,0);
    abi_record("cpu-clock.negative32",clock_read((unsigned)clock_id(pid,2,0),t),-1,-1,0,0,0);
    abi_record("cpu-clock.badptr",clock_read(2,(void *)-1),-1,-1,0,0,0);
    abi_record("cpu-clock.res-badptr",SC2(114,3,(void *)-1),-1,-1,0,0,0);
    abi_record("cpu-clock.invalid-kind",clock_read(clock_id(pid,3,0),(void *)-1),-1,-1,0,0,0);
    abi_record("cpu-clock.invalid-res-null",SC2(114,clock_id(pid,3,0),0),-1,-1,0,0,0);
    abi_record("cpu-clock.missing",clock_read(clock_id(0x0fffffff,2,0),t),-1,-1,0,0,0);
    struct clock_worker worker={0};int child_tid=0;
    long worker_tid=abi_clone_entry(0x10f00,clock_stack+sizeof(clock_stack),&child_tid,clock_worker,&worker);abi_require(worker_tid>0);
    while(!__atomic_load_n(&worker.ready,__ATOMIC_ACQUIRE))SC0(124);
    abi_record("cpu-clock.same-group-thread",clock_read(clock_id(worker.tid,2,1),t),-1,-1,0,0,0);
    abi_record("cpu-clock.caller-tid-process",worker.alias_time,-1,-1,0,0,0);
    abi_record("cpu-clock.caller-tid-process-res",worker.alias_res,-1,-1,0,0,0);
    abi_record("cpu-clock.foreign-tid-process",clock_read(clock_id(worker.tid,2,0),t),-1,-1,0,0,0);
    __atomic_store_n(&worker.release,1,__ATOMIC_RELEASE);
    while(SC2(130,worker_tid,0)==0)SC0(124);
    abi_record("cpu-clock.exited-thread",clock_read(clock_id(worker_tid,2,1),t),-1,-1,0,0,0);
    int fds[2];abi_require(!SC2(59,fds,0));
    long child=SC5(220,17,0,0,0,0);abi_require(child>=0);
    if(!child) {char byte;abi_require(SC3(63,fds[0],&byte,1)==1);clock_burn();abi_exit(57);}
    abi_record("cpu-clock.other-process",clock_read(clock_id(child,2,0),t),-1,-1,0,0,0);
    abi_record("cpu-clock.foreign-thread",clock_read(clock_id(child,2,1),t),-1,-1,0,0,0);
    char byte=1;abi_require(SC3(64,fds[1],&byte,1)==1);unsigned char info[128];
    abi_require(!SC5(95,1,child,info,4|0x1000000,0));
    abi_record("cpu-clock.zombie",clock_read(clock_id(child,2,0),t),-1,-1,0,0,0);
    abi_require(SC4(260,child,0,0,0)==child && !SC1(57,fds[0]) && !SC1(57,fds[1]));
    abi_record("cpu-clock.reaped",clock_read(clock_id(child,2,0),t),-1,-1,0,0,0);
}
