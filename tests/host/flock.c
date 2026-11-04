#include "../../fs/flock.h"
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/cpu.h>
#include <kernel/scheduler.h>
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

void mm_test_attach(unsigned);
unsigned mm_test_parked(unsigned);
static struct kernel_heap heap;
static unsigned allocations, fail_allocation;
static unsigned interrupt_wait;
enum kernel_scheduler_status __real_kernel_wait_park(const struct kernel_wait_token *,enum kernel_wait_wake_reason *);
enum kernel_scheduler_status __wrap_kernel_wait_park(const struct kernel_wait_token *token,enum kernel_wait_wake_reason *reason)
{
    assert(!kernel_cpu_current()->raw_locks);
    if (interrupt_wait) { interrupt_wait=0;assert(!kernel_wait_notify(token,KERNEL_WAIT_SIGNALLED)); }
    return __real_kernel_wait_park(token,reason);
}
enum kernel_heap_status kernel_heap_allocate_zeroed(struct kernel_heap *h,size_t n,size_t s,void **out)
{
    assert(h==&heap && !kernel_cpu_current()->raw_locks);
    if (fail_allocation) return KERNEL_HEAP_STATUS_EMPTY;
    *out=calloc(n,s);assert(*out);
    __atomic_add_fetch(&allocations,1,__ATOMIC_RELAXED);
    return KERNEL_HEAP_STATUS_OK;
}
void kernel_heap_release(struct kernel_heap *h,void *p)
{
    assert(h==&heap && p && !kernel_cpu_current()->raw_locks);
    assert(__atomic_fetch_sub(&allocations,1,__ATOMIC_RELAXED));free(p);
}

static struct kernel_flock_identity inode;
static struct kernel_flock_owner first,second,third;
static unsigned acquired;
static void *waiter(void *unused)
{
    (void)unused;mm_test_attach(1);
    assert(!kernel_flock_apply(&inode,&second,&heap,2));
    __atomic_store_n(&acquired,1,__ATOMIC_RELEASE);
    kernel_flock_release(&second);
    return 0;
}

static void shared_conversion(void)
{
    assert(!kernel_flock_apply(&inode,&first,&heap,1));
    assert(!kernel_flock_apply(&inode,&second,&heap,1));
    assert(kernel_flock_apply(&inode,&third,&heap,2|4)==-KERNEL_EAGAIN);
    assert(kernel_flock_apply(&inode,&first,&heap,2|4)==-KERNEL_EAGAIN);
    assert(!first.context); /* Failed upgrade has released its old reader. */
    kernel_flock_release(&second);
    assert(!kernel_flock_apply(&inode,&third,&heap,2|4));
    assert(!kernel_flock_apply(&inode,&third,&heap,2));
    assert(kernel_flock_apply(&inode,&first,&heap,1|4)==-KERNEL_EAGAIN);
    assert(!kernel_flock_apply(&inode,&third,&heap,1));
    assert(!kernel_flock_apply(&inode,&first,&heap,1));
    kernel_flock_release(&first);kernel_flock_release(&third);
    kernel_flock_destroy(&inode);assert(!allocations);
}

static void registered_wait(void)
{
    assert(!kernel_flock_apply(&inode,&first,&heap,2));
    pthread_t thread;assert(!pthread_create(&thread,0,waiter,0));
    /* The real scheduler backend publishes COMMITTED, rather than a ready
     * print preceding the flock call. The incumbent still blocks progress. */
    while (!mm_test_parked(1)) sched_yield();
    assert(!__atomic_load_n(&acquired,__ATOMIC_ACQUIRE));
    kernel_flock_release(&first);
    assert(!pthread_join(thread,0) && acquired);
    kernel_flock_destroy(&inode);assert(!allocations);
}

static void interrupted_conversion(void)
{
    assert(!kernel_flock_apply(&inode,&first,&heap,1));
    assert(!kernel_flock_apply(&inode,&second,&heap,1));
    interrupt_wait=1;
    assert(kernel_flock_apply(&inode,&first,&heap,2)==-KERNEL_ERESTARTSYS);
    assert(!first.context);
    kernel_flock_release(&second);
    assert(!kernel_flock_apply(&inode,&third,&heap,2|4));
    kernel_flock_release(&third);kernel_flock_destroy(&inode);assert(!allocations);
}

static pthread_barrier_t start;
static void *contending_reader(void *argument)
{
    unsigned id=(unsigned)(uintptr_t)argument;mm_test_attach(id);
    int b=pthread_barrier_wait(&start);assert(!b||b==PTHREAD_BARRIER_SERIAL_THREAD);
    struct kernel_flock_owner owner={0};
    assert(!kernel_flock_apply(&inode,&owner,&heap,1));
    kernel_flock_release(&owner);return 0;
}

int main(void)
{
    mm_test_attach(0);
    assert(!kernel_flock_apply(&inode,&first,&heap,8) && !allocations);
    assert(kernel_flock_apply(&inode,&first,&heap,3)==-KERNEL_EINVAL && !allocations);
    fail_allocation=1;
    assert(kernel_flock_apply(&inode,&first,&heap,2)==-KERNEL_ENOMEM && !first.context);
    fail_allocation=0;
    shared_conversion();registered_wait();interrupted_conversion();
    struct kernel_flock_identity *anonymous=kernel_flock_anonymous_identity();
    struct kernel_flock_identity *console=kernel_flock_bootstrap_console_identity();
    assert(!kernel_flock_apply(anonymous,&first,&heap,2));
    assert(kernel_flock_apply(anonymous,&second,&heap,1|4)==-KERNEL_EAGAIN);
    assert(!kernel_flock_apply(console,&third,&heap,2) && !allocations);
    kernel_flock_release(&first);kernel_flock_release(&third);
    assert(!kernel_flock_apply(anonymous,&second,&heap,2));kernel_flock_release(&second);
    assert(!allocations);
    assert(!pthread_barrier_init(&start,0,8));pthread_t readers[8];
    for (unsigned i=0;i<8;i++) assert(!pthread_create(&readers[i],0,contending_reader,(void *)(uintptr_t)(i+2)));
    for (unsigned i=0;i<8;i++) assert(!pthread_join(readers[i],0));
    assert(!pthread_barrier_destroy(&start));
    assert(allocations==1);kernel_flock_destroy(&inode);assert(!allocations);
    puts("FLOCK HOST PASS: shared/exclusive conversion, registered wait, lazy publication and cleanup");
    return 0;
}
