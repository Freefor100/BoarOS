#include "lwext4_port.h"

#include <kernel/heap.h>
#include <kernel/io_context.h>
#include <arch/context.h>
#include <kernel/irq.h>
#include <ext4.h>
#include <ext4_bcache.h>
#include <ext4_fs.h>
#include <ext4_journal.h>
#include <ext4_errno.h>
#include <misc/tree.h>

#include <stddef.h>

static struct kernel_heap *lwext4_heap;
static unsigned heap_users;
static struct kernel_wait_queue channels[64];
struct boaros_lwext4_resource {
    RB_ENTRY(boaros_lwext4_resource) index;
    struct kernel_mutex mutex;
    enum ext4_resource_kind kind;
    uint32_t key;
    /* Requests retain queued, active and replay-only qualifications. */
    uint32_t references;
};
static int resource_order(const struct boaros_lwext4_resource *a,
    const struct boaros_lwext4_resource *b)
{
    if (a->kind!=b->kind) return a->kind<b->kind?-1:1;
    return a->key<b->key?-1:a->key!=b->key;
}
RB_GENERATE_STATIC(boaros_lwext4_resource_tree,boaros_lwext4_resource,index,resource_order);

static void *resource_allocate(struct boaros_lwext4_domain *domain,size_t bytes)
{
    void *owner=NULL;
    KERNEL_NO_RECLAIM_IO;
    if (kernel_heap_allocate_zeroed(domain->heap,1,bytes,&owner)!=KERNEL_HEAP_STATUS_OK) return NULL;
    return owner;
}
static struct boaros_lwext4_resource *resource_find(struct boaros_lwext4_domain *domain,
    enum ext4_resource_kind kind,uint32_t key)
{
    struct boaros_lwext4_resource probe={.kind=kind,.key=key},*resource;
    {
        KERNEL_TASK_LOCK_SCOPE(guard,&domain->resource_index);
        resource=RB_FIND(boaros_lwext4_resource_tree,&domain->resources,&probe);
        if(resource) {
            if(resource->references==UINT32_MAX)__builtin_trap();
            resource->references++;
        }
    }
    if (resource) return resource;
    struct boaros_lwext4_resource *candidate=resource_allocate(domain,sizeof(*candidate));
    if (!candidate) return NULL;
    candidate->kind=kind;candidate->key=key;candidate->references=1;
    kernel_mutex_init(&candidate->mutex,kind==EXT4_RESOURCE_INODE?31:
        kind==EXT4_RESOURCE_ORPHAN?32:35,key);
    {
        KERNEL_TASK_LOCK_SCOPE(guard,&domain->resource_index);
        resource=RB_FIND(boaros_lwext4_resource_tree,&domain->resources,&probe);
        if (!resource) {
            if (RB_INSERT(boaros_lwext4_resource_tree,&domain->resources,candidate)) __builtin_trap();
            resource=candidate;candidate=NULL;
        } else {
            if(resource->references==UINT32_MAX)__builtin_trap();
            resource->references++;
        }
    }
    if (candidate) kernel_heap_release(domain->heap,candidate);
    return resource;
}
static void resource_put(struct boaros_lwext4_domain *domain,struct boaros_lwext4_resource *resource)
{
    int last;
    {
        KERNEL_TASK_LOCK_SCOPE(guard,&domain->resource_index);
        if(!resource->references)__builtin_trap();
        last=!--resource->references;
        if(last) {
            /* Find+borrow and final removal use the same index protection. */
            if(resource->mutex.lock.writer || resource->mutex.lock.readers ||
               resource->mutex.lock.pending_head || resource->mutex.lock.handoff_owner)__builtin_trap();
            RB_REMOVE(boaros_lwext4_resource_tree,&domain->resources,resource);
        }
    }
    if(last)kernel_heap_release(domain->heap,resource);
}
int boaros_lwext4_inode_get(struct boaros_lwext4_domain *domain,uint32_t inode,
    struct boaros_lwext4_resource **owner)
{
    if(!domain || !inode || !owner || *owner)__builtin_trap();
    *owner=resource_find(domain,EXT4_RESOURCE_INODE,inode);
    return *owner?EOK:ENOMEM;
}
void boaros_lwext4_inode_put(struct boaros_lwext4_domain *domain,
    struct boaros_lwext4_resource **owner)
{
    if(!domain || !owner)__builtin_trap();
    struct boaros_lwext4_resource *resource=*owner;
    if(!resource)return;
    if(resource->kind!=EXT4_RESOURCE_INODE)__builtin_trap();
    *owner=NULL;resource_put(domain,resource);
}
static void resource_request_free(struct boaros_lwext4_domain *domain,
    struct boaros_lwext4_resource_set *set,struct boaros_lwext4_resource_request *request)
{
    uintptr_t address=(uintptr_t)request;
    if(address>=(uintptr_t)set->requests && address<(uintptr_t)(set->requests+BOAROS_LWEXT4_INLINE_RESOURCES))
        *request=(struct boaros_lwext4_resource_request){0};
    else kernel_heap_release(domain->heap,request);
}
static void resource_requests_discard(struct boaros_lwext4_domain *domain,
    struct boaros_lwext4_resource_set *set)
{
    if (set->held_last) __builtin_trap();
    while (set->head) {
        struct boaros_lwext4_resource_request *request=set->head;
        set->head=request->next;
        if (request->guard.lock) __builtin_trap();
        resource_put(domain,request->resource);
        resource_request_free(domain,set,request);
    }
    set->tail=NULL;set->request_count=0;
}
static int resource_begin(struct boaros_lwext4_domain *domain,struct ext4_operation *operation,
    struct boaros_lwext4_resource_set **owner)
{
    struct kernel_io_context *context=kernel_io_context_inline();
    if (!operation || context->backend_domain!=domain || !context->backend_depth ||
        context->backend_state_depth || operation->fs!=domain->fs) __builtin_trap();
    struct boaros_lwext4_resource_set *set=&context->backend_resources;
    if (set->operation && set->operation!=operation) __builtin_trap();
    if (set->retry) return EAGAIN;
    if (!set->operation) {
        set->operation=operation;
        /* Complete abort keeps keys, not guards or private metadata. */
        for (struct boaros_lwext4_resource_request *request=set->head;request;request=request->next) {
            kernel_mutex_lock(&request->resource->mutex,&request->guard);
            set->held_last=request;
        }
    }
    *owner=set;return EOK;
}
static int resource_request_get(struct boaros_lwext4_domain *domain,
    struct boaros_lwext4_resource_set *set,enum ext4_resource_kind kind,
    uint32_t key,struct boaros_lwext4_resource_request **owner,int *created)
{
    struct boaros_lwext4_resource_request *before=set->head;
    struct boaros_lwext4_resource probe={.kind=kind,.key=key};
    while (before && resource_order(before->resource,&probe)<0) before=before->next;
    if (before && !resource_order(before->resource,&probe)) {
        *owner=before;*created=0;return EOK;
    }
    struct boaros_lwext4_resource_request *request;
    if(set->request_count==UINT32_MAX)__builtin_trap();
    /* 常见资格不进入 heap；超出部分只活到本次操作及其排序重放结束。 */
    if(set->request_count<BOAROS_LWEXT4_INLINE_RESOURCES)request=&set->requests[set->request_count];
    else request=resource_allocate(domain,sizeof(*request));
    if (!request) return ENOMEM;
    request->resource=resource_find(domain,kind,key);
    if (!request->resource) { resource_request_free(domain,set,request);return ENOMEM; }
    request->next=before;
    request->previous=before?before->previous:set->tail;
    if (request->previous) request->previous->next=request;else set->head=request;
    if (before) before->previous=request;else set->tail=request;
    set->request_count++;
    *owner=request;*created=1;return EOK;
}
static int resource_acquire(void *opaque,struct ext4_operation *operation,
    enum ext4_resource_kind kind,uint32_t key)
{
    struct boaros_lwext4_domain *domain=opaque;
    if (!__atomic_load_n(&domain->parallel,__ATOMIC_ACQUIRE)) return EOK;
    if(kind!=EXT4_RESOURCE_INODE && kind!=EXT4_RESOURCE_ORPHAN && kind!=EXT4_RESOURCE_BLOCK_GROUP)
        __builtin_trap();
    struct boaros_lwext4_resource_set *set;
    int result=resource_begin(domain,operation,&set),created;
    if(result)return result;
    struct boaros_lwext4_resource_request *request;
    result=resource_request_get(domain,set,kind,key,&request,&created);
    if(result)return result;
    if(!created) {
        if(!request->guard.lock && !request->private_inode)__builtin_trap();
        return EOK;
    }
    if (set->held_last && resource_order(request->resource,set->held_last->resource)<0) {
        set->retry=1;
        return EAGAIN;
    }
    kernel_mutex_lock(&request->resource->mutex,&request->guard);
    set->held_last=request;
    return EOK;
}
static int resource_claim_inode(void *opaque,struct ext4_operation *operation,uint32_t inode)
{
    struct boaros_lwext4_domain *domain=opaque;
    if(!__atomic_load_n(&domain->parallel,__ATOMIC_ACQUIRE))return EOK;
    struct boaros_lwext4_resource_set *set;
    int result=resource_begin(domain,operation,&set),created;
    if(result)return result;
    struct boaros_lwext4_resource_request *request;
    result=resource_request_get(domain,set,EXT4_RESOURCE_INODE,inode,&request,&created);
    if(result)return result;
    /* The allocator's bitmap reservation owns this unpublished inode. An
     * aborted attempt drops that ownership; replay takes its normal mutex. */
    if(created)request->private_inode=1;
    else if(!request->guard.lock && !request->private_inode)__builtin_trap();
    return EOK;
}
static void resource_release(void *opaque)
{
    struct boaros_lwext4_domain *domain=opaque;
    struct kernel_io_context *context=kernel_io_context_inline();
    struct boaros_lwext4_resource_set *set=&context->backend_resources;
    if (context->backend_domain!=domain || context->backend_state_depth) __builtin_trap();
    for (struct boaros_lwext4_resource_request *request=set->tail;request;request=request->previous) {
        kernel_lock_scope_release(&request->guard);
        request->private_inode=0;
    }
    set->held_last=NULL;set->operation=NULL;
    if (!set->retry) resource_requests_discard(domain,set);
}
static bool resource_retry(void *opaque)
{
    struct kernel_io_context *context=kernel_io_context_inline();
    struct boaros_lwext4_resource_set *set=&context->backend_resources;
    if (context->backend_domain!=opaque || set->held_last || set->operation) __builtin_trap();
    unsigned retry=set->retry;set->retry=0;
    return retry!=0;
}
static int state_backend_writer(struct boaros_lwext4_domain *domain,
    struct kernel_io_context *context)
{
    return context->backend_domain==domain && context->backend_depth && !context->backend_read &&
        context->backend_guard.lock==&domain->serial && context->backend_guard.owner==context &&
        context->backend_guard.write && domain->serial.writer==context;
}
static inline __attribute__((always_inline)) void state_check_owner(struct boaros_lwext4_domain *domain,
    struct kernel_io_context *context)
{
    struct kernel_lock_guard *guard=&context->backend_state_guard;
#if BOAROS_MAX_CPUS == 1 && !BOAROS_LOCK_DEBUG && !BOAROS_COST_DIAGNOSTICS
    /* 首层已验证真实 writer；backend 释放/暂停拒绝未结束的 state。
     * 递归只核对 domain，独立 guard 的最终释放仍检查 owner 和 LIFO。 */
    if(!guard->lock) {
        if(context->backend_domain==domain && context->backend_depth) return;
    } else if(guard->lock==&domain->serial || guard->lock==&domain->state.lock) return;
#else
    if(guard->lock==&domain->state.lock && guard->owner==context) return;
#endif
    __builtin_trap();
}
static inline __attribute__((always_inline)) void state_enter(void *opaque,int write)
{
    struct boaros_lwext4_domain *domain=opaque;
    struct kernel_io_context *context=kernel_io_context_inline();
    if (context->backend_state_depth) {
        state_check_owner(domain,context);
        /* A nested query borrows its writer, but a reader cannot upgrade
         * while other readers still borrow the same accepted state. */
        if(write && context->backend_state_guard.lock && !context->backend_state_guard.write)
            __builtin_trap();
    }
    else {
#if BOAROS_MAX_CPUS == 1 && !BOAROS_LOCK_DEBUG && !BOAROS_COST_DIAGNOSTICS
        if(context->backend_state_guard.lock) __builtin_trap();
        if(!__atomic_load_n(&domain->parallel,__ATOMIC_ACQUIRE)) {
            /* State scopes may allocate or perform synchronous journal I/O.
             * Reuse the real sleeping writer qualification, never a raw lock.
             * Readers still serialize state changes with one another. */
            if(state_backend_writer(domain,context)) goto Acquired;
            if(context->backend_domain!=domain || !context->backend_depth) {
                /* A caller outside the backend must also exclude its writer.
                 * Publication can finish while this caller waits on serial. */
                kernel_rwlock_write(&domain->serial,&context->backend_state_guard);
                if(!__atomic_load_n(&domain->parallel,__ATOMIC_ACQUIRE)) goto Acquired;
                kernel_lock_release(&context->backend_state_guard);
            }
        }
#endif
        if(write)kernel_mutex_lock(&domain->state,&context->backend_state_guard);
        else kernel_rwlock_read(&domain->state.lock,&context->backend_state_guard);
    }
#if BOAROS_MAX_CPUS == 1 && !BOAROS_LOCK_DEBUG && !BOAROS_COST_DIAGNOSTICS
Acquired:
#endif
    if (context->backend_state_depth==UINT32_MAX) __builtin_trap();
    context->backend_state_depth++;
}
void boaros_lwext4_state_lock(void *opaque)
{ state_enter(opaque,1); }
void boaros_lwext4_state_read_lock(void *opaque)
{ state_enter(opaque,0); }
void boaros_lwext4_state_unlock(void *opaque)
{
    struct boaros_lwext4_domain *domain=opaque;
    struct kernel_io_context *context=kernel_io_context_inline();
    if (!context->backend_state_depth) __builtin_trap();
    state_check_owner(domain,context);
    if (!--context->backend_state_depth) kernel_lock_scope_release(&context->backend_state_guard);
}
static void backend_enter(void *lock, int read)
{
    struct boaros_lwext4_domain *domain=lock;
    struct kernel_io_context *context = kernel_io_context_inline();
    if (context->backend_depth) {
        if (context->backend_domain != domain || (!read && context->backend_read)) __builtin_trap();
    } else {
        if (context->backend_domain && context->backend_domain!=domain) __builtin_trap();
        context->backend_domain=domain;
        if (!__atomic_load_n(&domain->parallel,__ATOMIC_ACQUIRE)) {
            if (read) kernel_rwlock_read(&domain->serial, &context->backend_guard);
            else kernel_rwlock_write(&domain->serial, &context->backend_guard);
            /* A waiter can have queued before the transition writer. Once it
             * owns the old lock, use the mode that writer actually published. */
            if (__atomic_load_n(&domain->parallel,__ATOMIC_ACQUIRE))
                kernel_lock_scope_release(&context->backend_guard);
        }
        context->backend_read = read;
    }
    context->backend_depth++;
}
static void backend_read(void *lock) { backend_enter(lock, 1); }
static void backend_write(void *lock) { backend_enter(lock, 0); }
void boaros_lwext4_unlock(void *lock)
{
    struct kernel_io_context *context = kernel_io_context_inline();
    struct boaros_lwext4_domain *domain=lock;
    if (!context->backend_depth || context->backend_domain!=domain) __builtin_trap();
    if (!--context->backend_depth) {
        struct boaros_lwext4_resource_set *set=&context->backend_resources;
        if (context->backend_state_depth || set->held_last || set->operation)
            __builtin_trap();
        kernel_lock_scope_release(&context->backend_guard);
        resource_requests_discard(domain,set);set->retry=0;
        context->backend_domain=NULL;context->backend_read=0;
    }
}
static bool backend_serial_writer(void *lock)
{ return state_backend_writer(lock,kernel_io_context_inline()); }
unsigned boaros_lwext4_pause(void)
{
    struct kernel_io_context *context = kernel_io_context_inline();
    unsigned depth = context->backend_depth;
    struct boaros_lwext4_resource_set *set=&context->backend_resources;
    if (!depth || context->backend_read || context->backend_state_depth || set->held_last)
        __builtin_trap();
    context->backend_depth = 0;
    kernel_lock_scope_release(&context->backend_guard);
    return depth;
}
void boaros_lwext4_resume(void *lock, unsigned depth)
{
    struct kernel_io_context *context = kernel_io_context_inline();
    if (!depth || context->backend_depth) __builtin_trap();
    backend_write(lock);
    context->backend_depth = depth;
}
void boaros_lwext4_lock_init(struct ext4_lock *callbacks,
    struct boaros_lwext4_domain *domain,struct kernel_heap *heap)
{
    callbacks->lock = backend_write;
    callbacks->unlock = boaros_lwext4_unlock;
    callbacks->read_lock = backend_read;
    callbacks->serial_writer = backend_serial_writer;
    callbacks->context = domain;
    callbacks->state_lock=boaros_lwext4_state_lock;
    callbacks->state_read_lock=boaros_lwext4_state_read_lock;
    callbacks->state_unlock=boaros_lwext4_state_unlock;
    /* UP keeps the mount writer; no independent resource consumer exists. */
    callbacks->resource_acquire=BOAROS_MAX_CPUS>1?resource_acquire:NULL;
    callbacks->resource_claim_inode=BOAROS_MAX_CPUS>1?resource_claim_inode:NULL;
    callbacks->resource_release=BOAROS_MAX_CPUS>1?resource_release:NULL;
    callbacks->resource_retry=BOAROS_MAX_CPUS>1?resource_retry:NULL;
    *domain=(struct boaros_lwext4_domain){.heap=heap};
    kernel_rwlock_init(&domain->serial,31,(uintptr_t)domain);
    kernel_mutex_init(&domain->state,40,(uintptr_t)domain);
    kernel_raw_lock_init(&domain->resource_index,KERNEL_RAW_RANK_OBJECT);
}
void boaros_lwext4_enable_parallel(struct boaros_lwext4_domain *domain,struct ext4_fs *fs)
{
    if (!domain || !fs || kernel_io_context_inline()->backend_depth) __builtin_trap();
    KERNEL_LOCK_SCOPE(serial);
    kernel_rwlock_write(&domain->serial,&serial);
    if (!domain || !fs || fs->read_only || fs->super_replay_required || !fs->orphan_index ||
        !fs->jbd_journal || !fs->jbd_journal->grouped || fs->jbd_journal->error ||
        fs->active_operations || domain->parallel ||
        kernel_io_context_inline()->backend_depth) __builtin_trap();
    domain->fs=fs;__atomic_store_n(&domain->parallel,1,__ATOMIC_RELEASE);
}
void boaros_lwext4_lock_fini(struct boaros_lwext4_domain *domain)
{
    if (kernel_io_context_inline()->backend_domain==domain) __builtin_trap();
    if(!RB_EMPTY(&domain->resources))__builtin_trap();
    if (domain->serial.writer || domain->serial.readers || domain->serial.pending_head ||
        domain->serial.handoff_owner || domain->state.lock.writer || domain->state.lock.readers ||
        domain->state.lock.pending_head || domain->state.lock.handoff_owner) __builtin_trap();
}
int boaros_lwext4_read_context(void)
{
    struct kernel_io_context *context = kernel_io_context_inline();
    return context->backend_depth && context->backend_read;
}
static struct kernel_wait_queue *channel(void *key)
{ return &channels[((uintptr_t)key >> 4) % 64]; }
void boaros_lwext4_wait(void *key)
{
    struct ext4_buf *buf=key;
    struct kernel_wait_token token;
    {
        KERNEL_TASK_LOCK_SCOPE(guard,&buf->bc->metadata);
        /* Completion may precede this callback. Check and register together. */
        if (!buf->loading) return;
        if (kernel_wait_prepare(channel(buf),0,0,&token)!=KERNEL_SCHEDULER_STATUS_OK)
            __builtin_trap();
    }
    enum kernel_wait_wake_reason reason;
    if (kernel_wait_park(&token,&reason)!=KERNEL_SCHEDULER_STATUS_OK ||
        kernel_wait_finish(&token)!=KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
}
void boaros_lwext4_wake(void *key)
{
    /* Caller holds the buffer-cache metadata lock and a producer reference. */
    struct kernel_wait_queue *queue=channel(key);
    if (__atomic_load_n(&queue->head,__ATOMIC_RELAXED)) {
        KERNEL_IRQ_SCOPE(irq);
        if (kernel_wait_queue_wake_all(queue)!=KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
    }
}

int boaros_lwext4_allocation_active(void)
{
    return kernel_io_context_inline()->allocation_depth != 0U ||
           kernel_io_context_inline()->backend_depth != 0U;
}

int boaros_lwext4_heap_bind(struct kernel_heap *heap)
{
    if (heap == 0 || (lwext4_heap != 0 && lwext4_heap != heap)) {
        return 0;
    }

    if (!lwext4_heap) {
        for (unsigned i = 0; i < 64; i++) kernel_wait_queue_init(&channels[i]);
    }
    if (heap_users == UINT32_MAX) __builtin_trap();
    heap_users++;
    lwext4_heap = heap;
    return 1;
}

void boaros_lwext4_heap_unbind(struct kernel_heap *heap)
{
    if (lwext4_heap != heap || !heap_users) __builtin_trap();
    if (!--heap_users) lwext4_heap = 0;
}

void *ext4_user_malloc(size_t size)
{
    void *pointer = 0;
    if (lwext4_heap == 0) return 0;
    kernel_io_context_inline()->allocation_depth++;
    enum kernel_heap_status status = kernel_heap_allocate(lwext4_heap, size, &pointer);
    kernel_io_context_inline()->allocation_depth--;
    return status == KERNEL_HEAP_STATUS_OK ? pointer : 0;
}

void *ext4_user_calloc(size_t count, size_t size)
{
    void *pointer = 0;

    if (lwext4_heap == 0) return 0;
    kernel_io_context_inline()->allocation_depth++;
    enum kernel_heap_status status = kernel_heap_allocate_zeroed(lwext4_heap,
                                    count,
                                    size,
                                    &pointer);
    kernel_io_context_inline()->allocation_depth--;
    return status == KERNEL_HEAP_STATUS_OK ? pointer : 0;
}

void *ext4_user_realloc(void *pointer, size_t size)
{
    void *result = 0;

    if (lwext4_heap == 0) return 0;
    kernel_io_context_inline()->allocation_depth++;
    enum kernel_heap_status status = kernel_heap_resize(lwext4_heap, pointer, size, &result);
    kernel_io_context_inline()->allocation_depth--;
    return status == KERNEL_HEAP_STATUS_OK ? result : 0;
}

void ext4_user_free(void *pointer)
{
    if (lwext4_heap != 0) {
        kernel_heap_release(lwext4_heap, pointer);
    }
}
