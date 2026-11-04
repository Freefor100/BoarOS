#include "flock.h"
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/irq.h>
#include <kernel/raw_lock.h>
#include <kernel/scheduler.h>
#include <kernel/wait_internal.h>

struct kernel_flock_context {
    struct kernel_raw_lock lock;
    struct kernel_wait_queue waiters;
    struct kernel_heap *heap;
    unsigned readers, writer;
};

/* These two real backing inodes are permanent, so their contexts need no heap
 * lifetime. All other inode contexts are allocated only on first flock. */
#define PERMANENT_CONTEXT { .lock={.rank=KERNEL_RAW_RANK_OBJECT, .initialized=KERNEL_RAW_INITIALIZED}, \
    .waiters={.initialized=KERNEL_WAIT_QUEUE_INITIALIZED} }
static struct kernel_flock_context anonymous_context=PERMANENT_CONTEXT;
static struct kernel_flock_context console_context=PERMANENT_CONTEXT;
static struct kernel_flock_identity anonymous_identity={.context=&anonymous_context};
static struct kernel_flock_identity console_identity={.context=&console_context};
struct kernel_flock_identity *kernel_flock_anonymous_identity(void)
{ return &anonymous_identity; }
struct kernel_flock_identity *kernel_flock_bootstrap_console_identity(void)
{ return &console_identity; }

static struct kernel_flock_context *context_get(struct kernel_flock_identity *id,
    struct kernel_heap *heap, int create)
{
    struct kernel_flock_context *context=__atomic_load_n(&id->context,__ATOMIC_ACQUIRE);
    if (context || !create) return context;
    struct kernel_flock_context *fresh=0;
    enum kernel_heap_status status=kernel_heap_allocate_zeroed(heap,1,sizeof(*fresh),(void **)&fresh);
    if (status==KERNEL_HEAP_STATUS_EMPTY) return 0;
    if (status) __builtin_trap();
    fresh->heap=heap;
    kernel_raw_lock_init(&fresh->lock,KERNEL_RAW_RANK_OBJECT);
    kernel_wait_queue_init(&fresh->waiters);
    if (__atomic_compare_exchange_n(&id->context,&context,fresh,0,__ATOMIC_RELEASE,__ATOMIC_ACQUIRE))
        return fresh;
    kernel_heap_release(heap,fresh);
    return context;
}

/* Caller holds the context's short protection; the OFD embeds the owner. */
static void remove_owner(struct kernel_flock_owner *owner)
{
    struct kernel_flock_context *context=owner->context;
    if (!context) __builtin_trap();
    if (owner->type==1) {
        if (!context->readers) __builtin_trap();
        context->readers--;
    } else {
        if (owner->type!=2 || !context->writer) __builtin_trap();
        context->writer=0;
    }
    *owner=(struct kernel_flock_owner){0};
}

static void notify(struct kernel_flock_context *context)
{
    KERNEL_IRQ_SCOPE(irq);
    if (kernel_wait_queue_wake_all(&context->waiters)) __builtin_trap();
}

int kernel_flock_apply(struct kernel_flock_identity *identity,
    struct kernel_flock_owner *owner,struct kernel_heap *heap,unsigned operation)
{
    unsigned type=operation&~4U;
    if (type!=1 && type!=2 && type!=8) return -KERNEL_EINVAL;
    struct kernel_flock_context *context=context_get(identity,heap,type!=8);
    if (!context) return type==8 ? 0 : -KERNEL_ENOMEM;
    for (;;) {
        struct kernel_wait_token token={0};
        struct kernel_wait_cursor changed={0};
        int wait=0,result=0;
        {
            KERNEL_RAW_SCOPE(guard,&context->lock);
            if (owner->context) {
                if (owner->context!=context) __builtin_trap();
                if (owner->type==type) return 0;
                /* Linux conversion first drops the old lock, including an
                 * upgrade that later loses to another reader or a signal. */
                remove_owner(owner);
                /* Capture existing waiters before registering this conversion;
                 * its own release notification must not wake its new wait. */
                (void)kernel_wait_cursor_prepare(&context->waiters,&changed);
            }
            if (type!=8) {
                if (context->writer || (type==2 && context->readers)) {
                    if (operation&4U) result=-KERNEL_EAGAIN;
                    else {
                        if (kernel_wait_prepare(&context->waiters,0,1,&token)) __builtin_trap();
                        wait=1;
                    }
                } else {
                    if (type==1) {
                        if (context->readers==UINT32_MAX) __builtin_trap();
                        context->readers++;
                    } else context->writer=1;
                    owner->context=context;owner->type=type;
                }
            }
        }
        /* The caller's backing-object reference covers notification and park;
         * no stack owner or wait token escapes its scheduler registration. */
        if (changed.queue) {
            KERNEL_IRQ_SCOPE(irq);
            kernel_wait_cursor_wake(&changed,1);
        }
        if (!wait) return result;
        enum kernel_wait_wake_reason reason;
        enum kernel_scheduler_status status=kernel_wait_park(&token,&reason);
        if (kernel_wait_finish(&token) || status) __builtin_trap();
        if (reason==KERNEL_WAIT_SIGNALLED) return -KERNEL_ERESTARTSYS;
    }
}

void kernel_flock_release(struct kernel_flock_owner *owner)
{
    struct kernel_flock_context *context=owner->context;
    if (!context) return;
    { KERNEL_RAW_SCOPE(guard,&context->lock);remove_owner(owner); }
    notify(context);
}

void kernel_flock_destroy(struct kernel_flock_identity *identity)
{
    struct kernel_flock_context *context=identity->context;
    if (!context) return;
    /* Only the final backing-object owner destroys this context. All OFDs,
     * waiting calls and synchronous notifications have already returned. */
    if (!context->heap || context->readers || context->writer) __builtin_trap();
    if (kernel_wait_queue_close(&context->waiters) ||
        kernel_wait_queue_destroy(&context->waiters)) __builtin_trap();
    identity->context=0;
    kernel_heap_release(context->heap,context);
}
