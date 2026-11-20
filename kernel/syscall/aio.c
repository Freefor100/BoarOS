#include "private.h"
#include <kernel/aio.h>
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/files.h>
#include <kernel/heap.h>
#include <kernel/irq.h>
#include <kernel/memory_object.h>
#include <kernel/mm.h>
#include <kernel/open_file.h>
#include <kernel/pid.h>
#include <kernel/signal_fd.h>
#include <kernel/scheduler.h>
#include <kernel/signal.h>
#include <kernel/sync.h>
#include <kernel/task.h>
#include <kernel/time.h>
#include <kernel/uaccess.h>
#include <kernel/wait_internal.h>

enum { AIO_PREAD, AIO_PWRITE, AIO_FSYNC, AIO_FDSYNC, AIO_POLL=5, AIO_PREADV=7, AIO_PWRITEV };
struct aio_iocb {
    uint64_t data;
    uint32_t key, rw_flags;
    uint16_t opcode, priority;
    uint32_t fd;
    uint64_t buffer, count;
    int64_t offset;
    uint64_t reserved;
    uint32_t flags, resfd;
};
struct aio_event { uint64_t data, object; int64_t result, result2; };
struct aio_ring { uint32_t id, nr, head, tail, magic, compat, incompat, header_length; };
_Static_assert(sizeof(struct aio_iocb)==64 && sizeof(struct aio_ring)==32 && sizeof(struct aio_event)==32,"Linux AIO ABI");

struct aio_request {
    struct aio_request *next, **previous, *ready_next;
    struct kernel_aio_context *ctx;
    struct kernel_open_file_description *file, *eventfd;
    const struct kernel_credentials *credentials;
    struct kernel_aio_direct *direct;
    struct kernel_pid *signal_recipient;
    struct kernel_wait_node watch;
    struct aio_event event;
    unsigned opcode, events, cancelled, queued, busy, dirty;
};
struct aio_page { uint64_t physical; void *bytes; };
struct kernel_aio_context {
    struct kernel_aio_context *next;
    struct kernel_heap *heap;
    struct kernel_memory_object *object;
    uint64_t address, bytes;
    uint32_t charged, nr, tail, reserved, refs, dead, stop, worker_state, id, pages;
    struct kernel_raw_lock lock;
    struct kernel_mutex read_lock;
    struct kernel_thread_join worker;
    struct kernel_wait_queue completed, work, released;
    struct aio_request *active, *ready, **ready_tail;
    struct aio_page page[];
};

/* The registry serializes only context publication, lookup and quota changes.
 * A syscall holds a context reference until all inline operations have ended. */
static struct kernel_raw_lock registry={.rank=KERNEL_RAW_RANK_OBJECT,.initialized=KERNEL_RAW_INITIALIZED};
static uint64_t total_events, max_events=65536;
static uint32_t next_id;

uint64_t kernel_aio_quota(int maximum)
{
    KERNEL_RAW_SCOPE(guard,&registry);
    return maximum?max_events:total_events;
}
void kernel_aio_set_max(uint64_t value)
{
    KERNEL_RAW_SCOPE(guard,&registry);
    max_events=value;
}
static int copy_in(struct kernel_mm *mm,void *dst,uint64_t user,size_t length)
{
    size_t copied=0;
    return kernel_copy_from_user(mm,dst,user,length,&copied) || copied!=length ? -KERNEL_EFAULT : 0;
}
static int copy_out(struct kernel_mm *mm,uint64_t user,const void *src,size_t length)
{
    size_t copied=0;
    return kernel_copy_to_user(mm,user,src,length,&copied) || copied!=length ? -KERNEL_EFAULT : 0;
}
static void wake(struct kernel_wait_queue *queue)
{
    if (!kernel_wait_queue_registrations(queue)) return;
    KERNEL_IRQ_SCOPE(irq);
    if (kernel_wait_queue_wake_all(queue)) __builtin_trap();
}
static void put_file(struct kernel_open_file_description **file)
{
    if (*file && kernel_open_file_release(file)) __builtin_trap();
}
static struct kernel_aio_context *lookup(struct kernel_mm *mm,uint64_t address,int remove)
{
    uint32_t id;
    if (copy_in(mm,&id,address,sizeof(id))) return NULL;
    KERNEL_RAW_SCOPE(guard,&registry);
    for (struct kernel_aio_context **slot=kernel_mm_aio_contexts(mm); *slot; slot=&(*slot)->next) {
        struct kernel_aio_context *ctx=*slot;
        if (ctx->address!=address || ctx->id!=id) continue;
        if (remove) *slot=ctx->next;
        else ctx->refs++;
        return ctx;
    }
    return NULL;
}
int kernel_aio_ring_move(struct kernel_mm *mm,struct kernel_memory_object *object,
    uint64_t old_address,uint64_t address)
{
    KERNEL_RAW_SCOPE(guard,&registry);
    for (struct kernel_aio_context *ctx=*kernel_mm_aio_contexts(mm);ctx;ctx=ctx->next) {
        if (ctx->object==object && ctx->address==old_address) {
            ctx->address=address;
            return 0;
        }
    }
    return -KERNEL_EINVAL;
}
static void put_context(struct kernel_aio_context *ctx)
{
    /* A waiting destroy may free the queue immediately after the last put.
     * Pin the cursor while the reference is still protected. */
    struct kernel_wait_cursor cursor={0};
    int notify;
    {
        KERNEL_RAW_SCOPE(guard,&registry);
        if (!ctx->refs) __builtin_trap();
        notify=!--ctx->refs && kernel_wait_cursor_prepare(&ctx->released,&cursor);
    }
    if (notify) kernel_wait_cursor_wake(&cursor,1);
}
static struct aio_ring *header(struct kernel_aio_context *ctx)
{
    return ctx->page[0].bytes;
}
static struct aio_event *event_slot(struct kernel_aio_context *ctx,unsigned index)
{
    size_t offset=sizeof(struct aio_ring)+index*sizeof(struct aio_event);
    return (void *)((char *)ctx->page[offset/BOAROS_PAGE_SIZE].bytes+(offset&BOAROS_PAGE_MASK));
}
static unsigned head_index(struct kernel_aio_context *ctx)
{
    unsigned head=__atomic_load_n(&header(ctx)->head,__ATOMIC_ACQUIRE);
    return head<ctx->nr?head:head%ctx->nr;
}
static unsigned available(struct kernel_aio_context *ctx,unsigned head)
{
    return ctx->tail>=head?ctx->tail-head:ctx->nr-head+ctx->tail;
}
static int reserve(struct kernel_aio_context *ctx)
{
    KERNEL_RAW_SCOPE(guard,&ctx->lock);
    if (ctx->dead) return -KERNEL_EINVAL;
    if (available(ctx,head_index(ctx))+ctx->reserved>=ctx->nr-1) return -KERNEL_EAGAIN;
    ctx->reserved++;
    return 0;
}
static void complete(struct kernel_aio_context *ctx,const struct aio_event *event,
    struct kernel_open_file_description *eventfd)
{
    {
        KERNEL_RAW_SCOPE(guard,&ctx->lock);
        if (!ctx->reserved) __builtin_trap();
        ctx->reserved--;
        *event_slot(ctx,ctx->tail)=*event;
        if (++ctx->tail==ctx->nr) ctx->tail=0;
        __atomic_store_n(&header(ctx)->tail,ctx->tail,__ATOMIC_RELEASE);
    }
    if (eventfd) kernel_event_fd_signal(eventfd);
    wake(&ctx->completed);
}

/* Enqueue only the first indication. If the worker is checking readiness it
 * rechecks once for any concurrent publication before going idle. */
static void enqueue_locked(struct aio_request *r)
{
    r->dirty=1;
    if (r->queued || r->busy || !r->previous) return;
    r->queued=1;
    *r->ctx->ready_tail=r;
    r->ctx->ready_tail=&r->ready_next;
}
static void poll_wake(struct kernel_wait_node *node,uint32_t reason,uint64_t event)
{
    struct aio_request *r=node->context;
    (void)event;
    {
        KERNEL_RAW_SCOPE(guard,&r->ctx->lock);
        if (reason==KERNEL_WAIT_POLL_FREE) {
            kernel_wait_queue_remove(node);
            r->cancelled=1;
        }
        enqueue_locked(r);
    }
    wake(&r->ctx->work);
}
static void free_request(struct aio_request *r)
{
    if (r->signal_recipient) kernel_task_identity_put(r->signal_recipient);
    if (r->direct) kernel_files_aio_direct_free(r->direct);
    put_file(&r->file);
    put_file(&r->eventfd);
    if (r->credentials) kernel_credentials_put(r->credentials);
    kernel_heap_release(r->ctx->heap,r);
}
static struct aio_request *take_work(struct kernel_aio_context *ctx,int *finished)
{
    KERNEL_RAW_SCOPE(guard,&ctx->lock);
    struct aio_request *r=ctx->ready;
    if (r) {
        ctx->ready=r->ready_next;
        if (!ctx->ready) ctx->ready_tail=&ctx->ready;
        r->ready_next=NULL;
        r->queued=0;
    } else if (ctx->stop) r=ctx->active;
    if (r) {
        r->busy=1;
        r->dirty=0;
    } else *finished=ctx->stop;
    return r;
}
static void run_work(struct aio_request *r)
{
    struct kernel_aio_context *ctx=r->ctx;
    int poll=r->opcode==AIO_POLL;
    if (r->signal_recipient) {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        struct kernel_pid_member *member=r->signal_recipient->members[KERNEL_PID_TID];
        uint64_t mask=__atomic_load_n(kernel_signal_fd_mask(r->file),__ATOMIC_ACQUIRE);
        r->event.result=member && kernel_signal_fd_pending_locked(member->task,mask)?
            KERNEL_POLLIN&r->events:0;
    } else if (poll) r->event.result=kernel_open_file_poll(r->file,r->events,NULL)&r->events;
    else {
        const struct kernel_credentials *saved=kernel_credentials_override(r->credentials);
        r->event.result=r->direct?kernel_files_aio_direct_run(r->file,r->direct):
            kernel_files_aio_sync(r->file,r->opcode==AIO_FDSYNC);
        kernel_credentials_override(saved);
    }
    {
        KERNEL_RAW_SCOPE(guard,&ctx->lock);
        r->busy=0;
        if (poll && !r->event.result && !r->cancelled && !ctx->stop) {
            if (r->dirty) enqueue_locked(r);
            return;
        }
        /* POLLFREE may retire the source as soon as it detaches. Serialize
         * queue-pointer inspection with that callback, then drain its borrow
         * outside the context lock. */
        if (poll) kernel_wait_queue_remove(&r->watch);
        *r->previous=r->next;
        if (r->next) r->next->previous=r->previous;
        r->previous=NULL;
    }
    if (poll && kernel_wait_node_remove_sync(&r->watch)) __builtin_trap();
    complete(ctx,&r->event,r->eventfd);
    free_request(r);
}
static void worker_main(void *arg)
{
    struct kernel_aio_context *ctx=arg;
    for (;;) {
        int finished=0;
        struct aio_request *r=take_work(ctx,&finished);
        if (r) { run_work(r); continue; }
        if (finished) return;
        KERNEL_IRQ_SCOPE(irq);
        struct kernel_wait_token token;
        enum kernel_wait_wake_reason reason;
        if (kernel_wait_prepare(&ctx->work,0,0,&token)) __builtin_trap();
        int idle;
        {
            KERNEL_RAW_SCOPE(guard,&ctx->lock);
            idle=!ctx->ready && !ctx->stop;
        }
        if (idle && kernel_wait_park(&token,&reason)) __builtin_trap();
        if (kernel_wait_finish(&token)) __builtin_trap();
    }
}
static int ensure_worker(struct kernel_aio_context *ctx)
{
    {
        KERNEL_RAW_SCOPE(guard,&ctx->lock);
        if (ctx->worker_state==2) return 0;
        if (ctx->worker_state) return -KERNEL_EAGAIN;
        ctx->worker_state=1;
    }
    KERNEL_SERVICE_SCOPE(service);
    int error=kernel_thread_create_joinable(worker_main,ctx,&ctx->worker);
    {
        KERNEL_RAW_SCOPE(guard,&ctx->lock);
        ctx->worker_state=error?0:2;
    }
    return error?-KERNEL_EAGAIN:0;
}
static void release_ring(struct kernel_mm *mm,struct kernel_aio_context *ctx,int unmap)
{
    if (ctx->object) {
        if (unmap) kernel_mm_aio_unmap(mm,ctx->address,ctx->bytes,ctx->object);
        for (unsigned i=0;i<ctx->pages;i++)
            if (physical_page_release(mm->allocator,ctx->page[i].physical)) __builtin_trap();
        kernel_memory_object_release(&ctx->object);
    }
    kernel_heap_release(ctx->heap,ctx);
}
static int setup(struct kernel_mm *mm,struct kernel_heap *heap,unsigned requested,uint64_t output)
{
    uint64_t initial;
    if (copy_in(mm,&initial,output,sizeof(initial))) return -KERNEL_EFAULT;
    if (initial || !requested || requested>0x400000U) return -KERNEL_EINVAL;
    {
        KERNEL_RAW_SCOPE(guard,&registry);
        if (total_events>max_events || requested>max_events-total_events) return -KERNEL_EAGAIN;
        total_events+=requested;
    }
    uint64_t slots=requested<BOAROS_MAX_CPUS*4U?BOAROS_MAX_CPUS*4U:requested;
    uint64_t bytes=(sizeof(struct aio_ring)+(2*slots+2)*sizeof(struct aio_event)+BOAROS_PAGE_MASK)&~(uint64_t)BOAROS_PAGE_MASK;
    unsigned pages=bytes/BOAROS_PAGE_SIZE;
    struct kernel_aio_context *ctx=NULL;
    int error=-KERNEL_ENOMEM;
    if (kernel_heap_allocate_zeroed(heap,1,sizeof(*ctx)+pages*sizeof(struct aio_page),(void **)&ctx)) goto fail;
    ctx->heap=heap;
    ctx->bytes=bytes;
    ctx->charged=requested;
    ctx->nr=(bytes-sizeof(struct aio_ring))/sizeof(struct aio_event);
    ctx->ready_tail=&ctx->ready;
    kernel_raw_lock_init(&ctx->lock,KERNEL_RAW_RANK_OBJECT);
    kernel_mutex_init(&ctx->read_lock,4,(uintptr_t)ctx);
    kernel_wait_queue_init(&ctx->work);
    kernel_wait_queue_init(&ctx->released);
    kernel_wait_queue_init(&ctx->completed);
    if (kernel_mm_aio_map(mm,bytes,&ctx->address,&ctx->object)) goto fail;
    for (unsigned i=0;i<pages;i++) {
        int created;
        struct aio_page *page=&ctx->page[i];
        if (kernel_memory_object_get_page(ctx->object,i,&page->physical,&created)) goto fail;
        if (physical_page_resolve(mm->allocator,page->physical,&page->bytes)) __builtin_trap();
        ctx->pages++;
    }
    ctx->id=__atomic_fetch_add(&next_id,1,__ATOMIC_RELAXED);
    *header(ctx)=(struct aio_ring){.id=ctx->id,.nr=ctx->nr,.magic=0xa10a10a1U,.compat=1,.header_length=sizeof(struct aio_ring)};
    error=copy_out(mm,output,&ctx->address,sizeof(ctx->address));
    if (error) goto fail;
    {
        KERNEL_RAW_SCOPE(guard,&registry);
        struct kernel_aio_context **head=kernel_mm_aio_contexts(mm);
        ctx->next=*head;
        *head=ctx;
    }
    return 0;
fail:
    if (ctx) release_ring(mm,ctx,1);
    {KERNEL_RAW_SCOPE(guard,&registry);total_events-=requested;}
    return error;
}
static void destroy(struct kernel_mm *mm,struct kernel_aio_context *ctx,int unmap)
{
    {KERNEL_RAW_SCOPE(guard,&ctx->lock);ctx->dead=1;}
    wake(&ctx->completed);
    for (;;) {
        KERNEL_IRQ_SCOPE(irq);
        struct kernel_wait_token token;
        enum kernel_wait_wake_reason reason;
        if (kernel_wait_prepare(&ctx->released,0,0,&token)) __builtin_trap();
        unsigned refs;
        {KERNEL_RAW_SCOPE(guard,&registry);refs=ctx->refs;}
        if (refs && kernel_wait_park(&token,&reason)) __builtin_trap();
        if (kernel_wait_finish(&token)) __builtin_trap();
        if (!refs) break;
    }
    /* The last put can finish its cursor after our wait token has drained
     * its node. The cursor holds a separate borrow of the embedded queue. */
    if (kernel_wait_queue_close(&ctx->released)) __builtin_trap();
    for (;;) {
        enum kernel_scheduler_status status=kernel_wait_queue_destroy(&ctx->released);
        if (!status) break;
        if (status!=KERNEL_SCHEDULER_STATUS_BUSY) __builtin_trap();
        kernel_wait_backend_quiesce();
    }
    if (ctx->worker_state) {
        {KERNEL_RAW_SCOPE(guard,&ctx->lock);ctx->stop=1;}
        wake(&ctx->work);
        KERNEL_SERVICE_SCOPE(service);
        kernel_thread_join(&ctx->worker);
    }
    if (ctx->reserved || ctx->active) __builtin_trap();
    {KERNEL_RAW_SCOPE(guard,&registry);total_events-=ctx->charged;}
    release_ring(mm,ctx,unmap);
}
void kernel_aio_exit(struct kernel_mm *mm)
{
    /* Last scheduler-owned MM user; no more lookups or context creation. */
    struct kernel_aio_context **head=kernel_mm_aio_contexts(mm);
    while (*head) {
        struct kernel_aio_context *ctx=*head;
        *head=ctx->next;
        destroy(mm,ctx,0);
    }
}
static int pin_file(struct kernel_files *files,unsigned fd,struct kernel_open_file_description **out)
{
    int64_t result=0;
    if (kernel_files_pin(files,fd,out,&result)) __builtin_trap();
    return (int)result;
}
static int submit_deferred(struct kernel_aio_context *ctx,struct aio_iocb *cb,
    struct aio_event *event,struct kernel_open_file_description **file,
    struct kernel_open_file_description **eventfd,struct kernel_aio_direct *direct)
{
    int poll=cb->opcode==AIO_POLL;
    if (!direct && (cb->offset || cb->count || cb->rw_flags || (poll ? cb->buffer>UINT16_MAX : cb->buffer!=0)))
        return -KERNEL_EINVAL;
    unsigned mask=(unsigned)cb->buffer|KERNEL_POLLERR|KERNEL_POLLHUP;
    struct kernel_wait_queue *source=NULL;
    if (poll) {
        event->result=kernel_open_file_poll(*file,mask,&source)&mask;
        if (event->result) {complete(ctx,event,*eventfd);return 0;}
        if (!source) return -KERNEL_EINVAL;
    } else if (!direct) {
        enum kernel_open_file_kind kind=kernel_open_file_kind(*file);
        if (kind!=KERNEL_OPEN_FILE_KIND_REGULAR && kind!=KERNEL_OPEN_FILE_KIND_BLOCK &&
            kind!=KERNEL_OPEN_FILE_KIND_DIRECTORY) return -KERNEL_EINVAL;
    }
    int error=ensure_worker(ctx);
    if (error) return error;
    struct aio_request *r=NULL;
    if (kernel_heap_allocate_zeroed(ctx->heap,1,sizeof(*r),(void **)&r)) return -KERNEL_EAGAIN;
    r->ctx=ctx;
    r->direct=direct;
    r->file=*file; *file=NULL;
    r->eventfd=*eventfd; *eventfd=NULL;
    r->event=*event;
    r->opcode=cb->opcode;
    r->events=mask;
    if (poll) {
        if (kernel_open_file_kind(r->file)==KERNEL_OPEN_FILE_KIND_SIGNALFD) {
            kernel_pid_t tid;
            if (kernel_task_user_tid(kernel_task_current(),&tid) ||
                kernel_task_identity_get_user(tid,&r->signal_recipient)) {
                free_request(r);
                return -KERNEL_ESRCH;
            }
        }
        KERNEL_SERVICE_SCOPE_IF(service,kernel_open_file_poll_requires_cpu0(r->file));
        kernel_wait_node_init_callback(&r->watch,poll_wake,r);
        r->watch.event_mask=kernel_signal_fd_mask(r->file);
        /* Source selection can depend on the current endpoint generation. */
        KERNEL_IRQ_SCOPE(irq);
        event->result=kernel_open_file_poll(r->file,mask,&source)&mask;
        if (event->result || !source) {
            if (event->result) complete(ctx,event,r->eventfd);
            free_request(r);
            return event->result?0:-KERNEL_EINVAL;
        }
        kernel_wait_queue_add(source,&r->watch);
    } else r->credentials=kernel_credentials_get(kernel_credentials_current());
    {
        KERNEL_RAW_SCOPE(guard,&ctx->lock);
        r->next=ctx->active;
        if (r->next) r->next->previous=&r->next;
        ctx->active=r; r->previous=&ctx->active;
        /* First worker check closes the query/registration interval. */
        enqueue_locked(r);
    }
    wake(&ctx->work);
    return 0;
}
static int submit_one(struct kernel_aio_context *ctx,struct kernel_files *files,
    struct kernel_mm *mm,uint64_t user)
{
    struct aio_iocb cb;
    if (copy_in(mm,&cb,user,sizeof(cb))) return -KERNEL_EFAULT;
    if (cb.reserved || cb.count>INT64_MAX) return -KERNEL_EINVAL;
    int error=reserve(ctx);
    if (error) return error;
    struct kernel_open_file_description *file=NULL,*eventfd=NULL;
    struct aio_event event={.data=cb.data,.object=user};
    error=pin_file(files,cb.fd,&file);
    if (error) goto out;
    if (cb.flags&1) {
        error=pin_file(files,cb.resfd,&eventfd);
        if (error) goto out;
        if (kernel_open_file_kind(eventfd)!=KERNEL_OPEN_FILE_KIND_EVENTFD) {
            error=-KERNEL_EINVAL; goto out;
        }
    }
    uint32_t key=0;
    error=copy_out(mm,user+offsetof(struct aio_iocb,key),&key,sizeof(key));
    if (error) goto out;
    switch (cb.opcode) {
    case AIO_PREAD: case AIO_PWRITE: case AIO_PREADV: case AIO_PWRITEV:
        if (cb.flags&2) {
            unsigned cls=cb.priority>>13;
            if (cls>3 || (!cls && (cb.priority&7))) {error=-KERNEL_EINVAL;break;}
            const struct kernel_credentials *cred=kernel_credentials_current();
            if (cls==1 && !kernel_credentials_capable(cred,KERNEL_CAP_SYS_ADMIN) &&
                !kernel_credentials_capable(cred,KERNEL_CAP_SYS_NICE)) {error=-KERNEL_EPERM;break;}
        }
        struct kernel_aio_direct *direct=NULL;
        error=kernel_files_aio_rw(files,mm,&file,cb.opcode,cb.buffer,cb.count,
            cb.offset,cb.rw_flags,&event.result,&direct);
        if (direct) {
            error=submit_deferred(ctx,&cb,&event,&file,&eventfd,direct);
            if (error) kernel_files_aio_direct_free(direct);
        } else if (!error) complete(ctx,&event,eventfd);
        break;
    case AIO_POLL: case AIO_FSYNC: case AIO_FDSYNC:
        error=submit_deferred(ctx,&cb,&event,&file,&eventfd,NULL);
        break;
    default: error=-KERNEL_EINVAL;
    }
out:
    put_file(&file);
    put_file(&eventfd);
    if (error) { KERNEL_RAW_SCOPE(guard,&ctx->lock);ctx->reserved--; }
    return error;
}
static int64_t submit(struct kernel_aio_context *ctx,struct kernel_files *files,
    struct kernel_mm *mm,int64_t count,uint64_t array)
{
    if (count<0) return -KERNEL_EINVAL;
    if ((uint64_t)count>ctx->nr) count=ctx->nr;
    for (int64_t i=0;i<count;i++) {
        uint64_t entry;
        int error=copy_in(mm,&entry,array+i*sizeof(entry),sizeof(entry));
        if (!error) error=submit_one(ctx,files,mm,entry);
        if (error) return i?i:error;
    }
    return count;
}
static int cancel(struct kernel_aio_context *ctx,uint64_t user)
{
    {
        KERNEL_RAW_SCOPE(guard,&ctx->lock);
        struct aio_request *r=ctx->active;
        while (r && (r->opcode!=AIO_POLL || r->event.object!=user || r->cancelled)) r=r->next;
        if (!r) return -KERNEL_EINVAL;
        r->cancelled=1;
        enqueue_locked(r);
    }
    wake(&ctx->work);
    return -KERNEL_EINPROGRESS;
}
static int64_t read_ring(struct kernel_aio_context *ctx,struct kernel_mm *mm,
    uint64_t buffer,int64_t count)
{
    if (!count) return 0;
    KERNEL_LOCK_SCOPE(reader);
    kernel_mutex_lock(&ctx->read_lock,&reader);
    unsigned head, ready;
    {
        KERNEL_RAW_SCOPE(guard,&ctx->lock);
        head=head_index(ctx);
        ready=available(ctx,head);
        if (!ready && ctx->dead) return -KERNEL_EINVAL;
    }
    if ((uint64_t)count>ready) count=ready;
    int64_t copied=0;
    while (copied<count) {
        unsigned offset=(sizeof(struct aio_ring)+head*sizeof(struct aio_event))&BOAROS_PAGE_MASK;
        unsigned chunk=(BOAROS_PAGE_SIZE-offset)/sizeof(struct aio_event);
        if (chunk>ctx->nr-head) chunk=ctx->nr-head;
        if ((uint64_t)chunk>(uint64_t)(count-copied)) chunk=(unsigned)(count-copied);
        if (copy_out(mm,buffer+copied*sizeof(struct aio_event),event_slot(ctx,head),
                     chunk*sizeof(struct aio_event))) return -KERNEL_EFAULT;
        head+=chunk;
        if (head==ctx->nr) head=0;
        copied+=chunk;
    }
    if (copied) __atomic_store_n(&header(ctx)->head,head,__ATOMIC_RELEASE);
    return copied;
}
static int64_t get_events(struct kernel_aio_context *ctx,struct kernel_mm *mm,
    const uint64_t *a,uint64_t deadline,int immediate)
{
    int64_t min=(int64_t)a[1],max=(int64_t)a[2],done=0;
    if (min<0 || max<min) return -KERNEL_EINVAL;
    for (;;) {
        int64_t result=read_ring(ctx,mm,a[3]+(uint64_t)done*sizeof(struct aio_event),max-done);
        if (result<0) return done?done:result;
        done+=result;
        if (done>=min || immediate) return done;
        KERNEL_IRQ_SCOPE(irq);
        struct kernel_wait_token token;
        enum kernel_wait_wake_reason reason=KERNEL_WAIT_WOKEN;
        enum kernel_scheduler_status status=deadline?
            kernel_wait_prepare_monotonic(&ctx->completed,deadline,1,&token):
            kernel_wait_prepare(&ctx->completed,0,1,&token);
        if (status) __builtin_trap();
        int empty;
        { KERNEL_RAW_SCOPE(guard,&ctx->lock);empty=!ctx->dead && !available(ctx,head_index(ctx)); }
        if (empty && kernel_wait_park(&token,&reason)) __builtin_trap();
        if (kernel_wait_finish(&token)) __builtin_trap();
        immediate=reason!=KERNEL_WAIT_WOKEN;
    }
}
static int timeout_for(struct kernel_mm *mm,uint64_t address,uint64_t *deadline,int *immediate)
{
    if (!address) return 0;
    struct {int64_t sec,nsec;} ts;
    if (copy_in(mm,&ts,address,sizeof(ts))) return -KERNEL_EFAULT;
    /* Linux converts the relative timespec to ktime; nonpositive duration expires at once. */
    __int128 ns=(__int128)ts.sec*1000000000+ts.nsec;
    if (ns<=0) {*immediate=1;return 0;}
    ns+=kernel_time_monotonic_ns();
    if (ns>INT64_MAX) ns=INT64_MAX;
    *deadline=(uint64_t)ns;
    return 0;
}
enum kernel_syscall_status syscall_handle_native_aio(struct kernel_task *caller,
    const struct kernel_syscall_request *request,struct kernel_syscall_result *decoded)
{
    struct kernel_files *files;
    struct kernel_mm *mm;
    if (kernel_task_mm_borrow_mutable(caller,&mm) || kernel_task_files_borrow(caller,&files))
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    const uint64_t *a=request->arguments;
    unsigned nr=request->number;
    decoded->action=KERNEL_SYSCALL_ACTION_RETURN;
    if (!nr) { decoded->value=setup(mm,files->heap,(unsigned)a[0],a[1]);return 0; }
    int error=0,immediate=0,modified=0;
    uint64_t deadline=0, saved=0;
    if (nr==3) {
        uint32_t key;
        error=copy_in(mm,&key,a[1]+offsetof(struct aio_iocb,key),sizeof(key));
        if (!error && key) error=-KERNEL_EINVAL;
    } else if (nr==4 || nr==292) {
        error=timeout_for(mm,a[4],&deadline,&immediate);
        if (!error && nr==292 && a[5]) {
            struct {uint64_t addr,size;} sig;
            error=copy_in(mm,&sig,a[5],sizeof(sig));
            if (!error && sig.addr) {
                uint64_t mask;
                error=sig.size!=sizeof(mask)?-KERNEL_EINVAL:copy_in(mm,&mask,sig.addr,sizeof(mask));
                if (!error) {
                    if (kernel_signal_set_temporary_mask(caller,mask,&saved)) __builtin_trap();
                    modified=1;
                }
            }
        }
    }
    int64_t result=error;
    if (!error) {
        struct kernel_aio_context *ctx=lookup(mm,a[0],nr==1);
        result=-KERNEL_EINVAL;
        if (ctx) {
            if (nr==1) {destroy(mm,ctx,1);result=0;}
            else {
                if (nr==2) result=submit(ctx,files,mm,(int64_t)a[1],a[2]);
                else if (nr==3) result=cancel(ctx,a[1]);
                else result=get_events(ctx,mm,a,deadline,immediate);
                put_context(ctx);
            }
        }
    }
    if (nr==4 || nr==292) {
        int interrupted=kernel_signal_has_pending(caller);
        if (modified) kernel_signal_restore_temporary_mask(caller,saved,interrupted);
        if (interrupted && !result) {
            if (nr==292) {
                kernel_signal_note_nohandler_restart(caller);
                result=-KERNEL_ERESTARTSYS;
            } else result=-KERNEL_EINTR;
        }
    }
    decoded->value=result;
    return 0;
}
