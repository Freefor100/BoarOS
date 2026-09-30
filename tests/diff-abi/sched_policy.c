#include "abi.h"
struct sched_time { long sec, ns; };
static unsigned long monotonic(void)
{
    struct sched_time t;
    abi_require(SC2(113,1,&t)==0);
    return (unsigned long)t.sec*1000000000UL+(unsigned long)t.ns;
}
static void wait_child(long pid)
{
    int status=-1;
    abi_require(SC4(260,pid,&status,0,0)==pid && status==0);
}
static void policy(long pid,int kind,int priority)
{ abi_require(SC3(119,pid,kind,&priority)==0); }

void abi_sched_policy_cases(void)
{
    int param=-1;
    abi_record("sched.get-other",SC1(120,0),-1,-1,0,0,0);
    long result=SC2(121,0,&param);
    abi_record("sched.param-other",result,-1,-1,0,&param,sizeof(param));
    abi_record("sched.priority-max-fifo",SC1(125,1),-1,-1,0,0,0);
    abi_record("sched.priority-min-rr",SC1(126,2),-1,-1,0,0,0);
    abi_record("sched.priority-max-batch",SC1(125,3),-1,-1,0,0,0);
    abi_record("sched.priority-min-idle",SC1(126,5),-1,-1,0,0,0);
    abi_record("sched.priority-invalid",SC1(125,4),-1,-1,0,0,0);
    param=100;abi_record("sched.priority-overflow",SC3(119,0,1,&param),-1,-1,0,0,0);
    param=1;abi_record("sched.other-priority-invalid",SC3(119,0,0,&param),-1,-1,0,0,0);
    abi_record("sched.param-null",SC2(121,0,0),-1,-1,0,0,0);
    abi_record("sched.param-fault",SC2(121,0,1),-1,-1,0,0,0);
    abi_record("sched.pid-negative",SC1(120,-1),-1,-1,0,0,0);
    unsigned long mask=1;
    abi_record("sched.affinity-cpu0",SC3(122,0,8,&mask),-1,-1,0,0,0);
    abi_record("sched.affinity-short",SC3(123,0,1,&mask),-1,-1,0,0,0);
    mask=2;abi_record("sched.affinity-offline",SC3(122,0,8,&mask),-1,-1,0,0,0);
    mask=0;result=SC3(123,0,8,&mask);
    abi_record("sched.affinity-get",result,-1,-1,0,&mask,sizeof(mask));
    struct sched_time interval;
    policy(0,1,20);
    result=SC2(127,0,&interval);
    abi_record("sched.fifo-interval",result,-1,-1,0,&interval,sizeof(interval));
    policy(0,2,20);
    result=SC2(127,0,&interval);
    abi_record("sched.rr-interval",result,-1,-1,0,&interval,sizeof(interval));
    policy(0,1|0x40000000,20);
    long child=SC5(220,17,0,0,0,0);
    abi_require(child>=0);
    if(!child) abi_exit(SC1(120,0)!=0);
    wait_child(child);
    abi_record("sched.reset-parent",SC1(120,0),-1,-1,0,0,0);
    policy(0,0,0);

    long address=SC6(222,0,4096,3,0x21,-1,0);
    abi_require(address>=0);
    volatile unsigned long *shared=(void *)address;
    shared[0]=0;
    policy(0,1,40);
    child=SC5(220,17,0,0,0,0);abi_require(child>=0);
    if(!child) {shared[0]=1;abi_exit(0);}
    unsigned long start=monotonic();
    while(monotonic()-start<20000000UL) { }
    long fifo_held=shared[0]==0;
    abi_require(SC0(124)==0);
    long yield_peer=shared[0]==1;
    wait_child(child);
    abi_record("sched.fifo-no-tick-rotation",fifo_held,-1,-1,0,0,0);
    abi_record("sched.fifo-yield-peer",yield_peer,-1,-1,0,0,0);

    shared[0]=0;
    child=SC5(220,17,0,0,0,0);abi_require(child>=0);
    if(!child) {shared[0]=1;abi_exit(0);}
    policy(child,1,60);
    long preempted=shared[0]==1;
    wait_child(child);
    abi_record("sched.higher-priority-wake",preempted,-1,-1,0,0,0);

    shared[0]=0;
    policy(0,2,40);
    child=SC5(220,17,0,0,0,0);abi_require(child>=0);
    if(!child) {shared[0]=1;abi_exit(0);}
    start=monotonic();
    while(!shared[0] && monotonic()-start<2000000000UL) { }
    long rr_rotated=shared[0]==1;
    wait_child(child);
    abi_record("sched.rr-equal-priority-rotation",rr_rotated,-1,-1,0,0,0);
    policy(0,0,0);
    abi_require(SC2(215,address,4096)==0);
}
