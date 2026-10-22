#define BOAROS_HOST_IRQ_STATE 1
#include <kernel/task_state.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/wait_internal.h>
#include "../../kernel/signal_pending.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
_Thread_local uintptr_t network_host_irq=RISCV_SSTATUS_SIE;
struct kernel_raw_lock kernel_wait_domain={.rank=KERNEL_RAW_RANK_SCHEDULER,.initialized=KERNEL_RAW_INITIALIZED};
static unsigned live,fail;
static struct kernel_heap heap;
void kernel_cpu_flush_notifications(void){assert(!kernel_cpu_current()->outbound);}
enum kernel_heap_status kernel_heap_allocate_zeroed(struct kernel_heap *h,size_t n,size_t size,void **out)
{
    assert(h==&heap && !kernel_cpu_current()->raw_locks);
    if(__atomic_load_n(&fail,__ATOMIC_RELAXED))return KERNEL_HEAP_STATUS_EMPTY;
    *out=calloc(n,size);assert(*out);__atomic_fetch_add(&live,1,__ATOMIC_RELAXED);return 0;
}
void kernel_heap_release(struct kernel_heap *h,void *p)
{assert(h==&heap && p && !kernel_cpu_current()->raw_locks);free(p);assert(__atomic_fetch_sub(&live,1,__ATOMIC_RELAXED));}
static void init(struct kernel_task *t)
{*t=(struct kernel_task){0};t->group_leader=t;t->group_members=1;t->files.heap=&heap;t->sigpending_limit=(struct kernel_rlimit64){4096,4096};}
static uint64_t count(void)
{KERNEL_RAW_SCOPE(g,&kernel_wait_domain);return kernel_signal_pending_count_locked();}
static int post(struct kernel_task *t,int shared,unsigned sig,int code,uint64_t value,int supplied)
{
    struct signal_pending_prepared p={0};
    struct kernel_signal_wait_info info={.signal=sig,.code=code,.sender=42,.value=value};
    signal_pending_prepare(t,shared,&info,supplied,&p);int result;
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);result=signal_pending_post_locked(t,shared,&info,supplied,&p);}
    signal_pending_finish(&p);return result<0?result:0;
}
static struct kernel_signal_wait_info take(struct kernel_task *t,int shared,unsigned sig)
{
    struct signal_pending_prepared p={0};struct kernel_signal_wait_info info;
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);signal_pending_take_locked(t,shared,sig,&info,&p);}
    signal_pending_finish(&p);return info;
}
static pthread_barrier_t ready;
struct sender {struct kernel_task *task;unsigned index;};
static void *produce(void *arg)
{
    struct sender *s=arg;
    struct signal_pending_prepared p={0};
    struct kernel_signal_wait_info info={.signal=36,.code=-1,.sender=s->index,.value=1};
    signal_pending_prepare(s->task,0,&info,1,&p);
    int r=pthread_barrier_wait(&ready);assert(!r || r==PTHREAD_BARRIER_SERIAL_THREAD);
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);assert(!signal_pending_post_locked(s->task,0,&info,1,&p));}
    signal_pending_finish(&p);
    for(unsigned i=2;i<=32;i++) {
        info.value=i;signal_pending_prepare(s->task,0,&info,1,&p);
        {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);assert(!signal_pending_post_locked(s->task,0,&info,1,&p));}
        signal_pending_finish(&p);
    }
    return 0;
}
int main(void)
{
    struct kernel_task a,b;init(&a);init(&b);
    assert(!post(&a,0,10,0,0,0) && count()==1);
    assert(!post(&a,0,10,0,0,0) && count()==1);
    assert(take(&a,0,10).sender==42 && !count());
    a.sigpending_limit.current=2;
    assert(!post(&a,1,36,-1,UINT64_C(0x1234567887654321),1));
    assert(!post(&a,1,36,-1,2,1));
    b.sigpending_limit.current=2;
    assert(post(&b,0,36,-1,3,1)==-KERNEL_EAGAIN && count()==2);
    a.sigpending_limit.current=0;
    assert(take(&a,1,36).value==UINT64_C(0x1234567887654321));
    assert(take(&a,1,36).value==2 && !count());
    assert(!post(&a,0,10,-6,0,0));
    struct kernel_signal_wait_info info=take(&a,0,10);assert(!info.code && !info.sender && !count());
    assert(!post(&a,0,10,0,0,0) && count()==1);assert(take(&a,0,10).sender==42 && !count());
    a.sigpending_limit.current=4096;fail=1;
    assert(post(&b,0,36,-1,3,1)==-KERNEL_EAGAIN && !count());
    assert(!post(&b,0,36,0,0,0));info=take(&b,0,36);assert(!info.sender && !count());fail=0;
    assert(!pthread_barrier_init(&ready,0,2));pthread_t workers[2];struct sender senders[2]={{&a,1},{&a,2}};
    for(unsigned i=0;i<2;i++)assert(!pthread_create(&workers[i],0,produce,&senders[i]));
    for(unsigned i=0;i<2;i++)assert(!pthread_join(workers[i],0));
    assert(count()==64);unsigned sequence[3]={0};
    for(unsigned i=0;i<64;i++){info=take(&a,0,36);assert(info.sender>=1 && info.sender<=2 && info.value==++sequence[info.sender]);}
    assert(sequence[1]==32 && sequence[2]==32 && !count());assert(!pthread_barrier_destroy(&ready));
    assert(!post(&a,1,36,-1,7,1));
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);b.group_pending=a.group_pending;kernel_signal_pending_adopt_locked(&b,&a);}
    kernel_signal_pending_cleanup(&a);assert(count()==1);assert(take(&b,1,36).value==7 && !count());
    assert(!post(&b,0,37,-1,9,1));kernel_signal_pending_cleanup(&b);assert(!count() && !live);
    puts("signal pending FIFO/quota/abort/merge/lifetime passed");return 0;
}
