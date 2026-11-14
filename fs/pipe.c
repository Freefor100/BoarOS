#include <kernel/credentials.h>
#include <kernel/async_io.h>
#include "pipe_internal.h"
#include "vfs_objects.h"
#include "uaccess_iov_internal.h"

#include <arch/context.h>
#include <kernel/errno.h>
#include <kernel/files.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/irq.h>
#include <kernel/open_file.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/scheduler.h>
#include <kernel/signal.h>
#include <kernel/task.h>
#include <kernel/time.h>
#include <kernel/uaccess.h>
#include <kernel/wait_internal.h>

#include <stdint.h>
#include <string.h>

#define PIPE_META(name, pipe) KERNEL_RAW_SCOPE(name, &(pipe)->metadata_lock)
#define PIPE_MAX_UNPRIVILEGED_BYTES UINT32_C(1048576)
#define PIPE_USER_PAGES_SOFT UINT64_C(16384)
#define PIPE_USER_PAGES_HARD UINT64_C(0)

static int pipe_unprivileged(const struct kernel_credentials *cred)
{
    return !kernel_credentials_capable(cred,KERNEL_CAP_SYS_RESOURCE) &&
        !kernel_credentials_capable(cred,KERNEL_CAP_SYS_ADMIN);
}
static int pipe_over_hard_limit(uint64_t pages)
{ return PIPE_USER_PAGES_HARD && pages>PIPE_USER_PAGES_HARD; }

static uint64_t next_pipe_proc_identity = 1U;

uint64_t kernel_pipe_proc_identity(const struct kernel_pipe *pipe)
{
    return pipe ? pipe->proc_identity : 0U;
}

static struct kernel_vfs_timespec pipe_now(void)
{
    uint64_t ns = kernel_time_realtime_ns();
    return (struct kernel_vfs_timespec){(int64_t)(ns / 1000000000), (int64_t)(ns % 1000000000)};
}
int kernel_pipe_stat(const struct kernel_pipe *pipe, struct kernel_vfs_stat *stat)
{
    if (!pipe || !stat) return -KERNEL_EINVAL;
    PIPE_META(guard,(struct kernel_pipe *)pipe);
    *stat = (struct kernel_vfs_stat){.ino=pipe->proc_identity, .mode=pipe->mode,
        .nlink=1, .blksize=BOAROS_PAGE_SIZE, .uid=pipe->uid, .gid=pipe->gid,
        .atime=pipe->atime, .mtime=pipe->mtime, .ctime=pipe->ctime};
    return 0;
}
int kernel_pipe_set_mode(struct kernel_pipe *pipe, uint32_t mode)
{
    if (!pipe) return -KERNEL_EINVAL;
    struct kernel_vfs_timespec now=pipe_now();
    PIPE_META(guard,pipe);
    int error=kernel_vfs_chmod_allowed(pipe->uid,pipe->gid,&mode);
    if (error) return error;
    /* 共享端点只改权限；访问方向仍由每个OFD拥有。 */
    pipe->mode = KERNEL_VFS_S_IFIFO | (mode & 07777U);
    pipe->ctime = now;
    return 0;
}

int kernel_pipe_set_owner(struct kernel_pipe *pipe, uint32_t uid, uint32_t gid)
{
    if (!pipe) return -KERNEL_EINVAL;
    struct kernel_vfs_timespec now=pipe_now();
    PIPE_META(guard,pipe);
    int error=kernel_vfs_chown_allowed(pipe->uid,pipe->gid,pipe->mode,uid,gid);
    if (error) return error;
    uint32_t mode=kernel_vfs_chown_mode(pipe->mode,pipe->gid,gid==UINT32_MAX?pipe->gid:gid);
    if (uid != UINT32_MAX) pipe->uid = uid;
    if (gid != UINT32_MAX) pipe->gid = gid;
    pipe->mode = mode;
    pipe->ctime = now;
    return 0;
}

static void pipe_release_page(struct kernel_pipe *pipe,void *page)
{
    uint64_t physical;
    if (!page || !pipe->heap->physical_address(page,&physical) ||
        physical_page_release(pipe->allocator,physical)) __builtin_trap();
}

static enum kernel_pipe_status pipe_destroy(struct kernel_pipe *pipe)
{
    kernel_flock_destroy(&pipe->flock_identity);
    if (pipe->read_queue.initialized) {
        struct kernel_wait_queue *queues[]={&pipe->read_queue,&pipe->write_queue,&pipe->both_queue,&pipe->async_events};
        for (unsigned i=0;i<4;i++) {
            enum kernel_scheduler_status status=kernel_wait_queue_close(queues[i]);
            /* bootstrap可构造/关闭管道，此时没有可park任务；destroy仍核对借用归零。 */
            if (status!=KERNEL_SCHEDULER_STATUS_OK && status!=KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED)
                __builtin_trap();
            if (kernel_wait_queue_destroy(queues[i])) __builtin_trap();
        }
    }
    if (pipe->fifo_node) {
        if (pipe->fifo_node->fifo_pipe != pipe) __builtin_trap();
        pipe->fifo_node->fifo_pipe = 0;
    }
    for (uint32_t i=0;i<pipe->ring_size;i++)
        if (pipe->buffers[i].page) pipe_release_page(pipe,pipe->buffers[i].page);
    if (pipe->buffers!=pipe->inline_buffers) kernel_heap_release(pipe->heap,pipe->buffers);
    (void)kernel_uid_account_pipe_pages(pipe->account,pipe->ring_size,0);
    kernel_uid_account_put(pipe->account);
    kernel_heap_release(pipe->heap, pipe);
    return KERNEL_PIPE_STATUS_OK;
}

enum kernel_pipe_status kernel_pipe_create(
    struct kernel_heap *heap,
    struct kernel_pipe **owner)
{
    struct kernel_pipe *pipe;
    enum kernel_heap_status heap_status;

    if (heap == 0 || owner == 0 || *owner != 0) {
        return KERNEL_PIPE_STATUS_INVALID_ARGUMENT;
    }
    heap_status = kernel_heap_allocate_zeroed(heap,
                                              1U,
                                              sizeof(*pipe),
                                              (void **)&pipe);
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        return heap_status == KERNEL_HEAP_STATUS_EMPTY
                   ? KERNEL_PIPE_STATUS_NO_MEMORY
                   : KERNEL_PIPE_STATUS_STATE;
    }
    kernel_raw_lock_init(&pipe->metadata_lock,KERNEL_RAW_RANK_OBJECT);
    pipe->heap = heap;
    pipe->allocator = heap->page_allocator;
    pipe->mode = KERNEL_VFS_S_IFIFO | 0600U;
    const struct kernel_credentials *cred = kernel_credentials_current();
    pipe->uid = cred->fsuid;
    pipe->gid = cred->fsgid;
    pipe->buffers=pipe->inline_buffers;
    pipe->ring_size=KERNEL_PIPE_INLINE_SLOTS;
    if ((uint64_t)pipe->ring_size*BOAROS_PAGE_SIZE>PIPE_MAX_UNPRIVILEGED_BYTES &&
        !kernel_credentials_capable(cred,KERNEL_CAP_SYS_RESOURCE))
        pipe->ring_size=PIPE_MAX_UNPRIVILEGED_BYTES/BOAROS_PAGE_SIZE;
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
      pipe->account=kernel_uid_account_get_locked(cred->account); }
    uint64_t pages=kernel_uid_account_pipe_pages(pipe->account,0,pipe->ring_size);
    if (pages>PIPE_USER_PAGES_SOFT && pipe_unprivileged(cred)) {
        pages=kernel_uid_account_pipe_pages(pipe->account,pipe->ring_size,2);
        pipe->ring_size=2;
    }
    if (pipe_over_hard_limit(pages) && pipe_unprivileged(cred)) {
        (void)kernel_uid_account_pipe_pages(pipe->account,pipe->ring_size,0);
        kernel_uid_account_put(pipe->account);
        kernel_heap_release(heap,pipe);
        return KERNEL_PIPE_STATUS_NO_MEMORY;
    }
    pipe->atime = pipe->mtime = pipe->ctime = pipe_now();
    pipe->read_slot=pipe->merge_slot=KERNEL_PIPE_NO_SLOT;
    kernel_wait_queue_init(&pipe->read_queue);
    kernel_wait_queue_init(&pipe->write_queue);
    kernel_wait_queue_init(&pipe->both_queue);
    kernel_wait_queue_init(&pipe->async_events);
    /* 片段资格先于OFD/rank10、UNIX控制/rank10和VFS写操作/rank15；跨管道按key排列。 */
    kernel_mutex_init(&pipe->read_lock,9U,(uintptr_t)&pipe->read_lock);
    kernel_mutex_init(&pipe->write_lock,9U,(uintptr_t)&pipe->write_lock);
    uint64_t identity=__atomic_load_n(&next_pipe_proc_identity,__ATOMIC_RELAXED);
    for (;;) {
        if (!identity || identity==UINT64_MAX) __builtin_trap();
        if (__atomic_compare_exchange_n(&next_pipe_proc_identity,&identity,identity+1,
            1,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) break;
    }
    pipe->proc_identity=identity;
    *owner = pipe;
    return KERNEL_PIPE_STATUS_OK;
}

static void pipe_notify(struct kernel_pipe *pipe,int readers,int writers)
{
    KERNEL_IRQ_SCOPE(irq);
    struct kernel_wait_queue *queues[]={&pipe->read_queue,&pipe->write_queue,&pipe->both_queue};
    int notify[]={readers,writers,readers||writers};
    for (unsigned i=0;i<3;i++) if (notify[i]) {
        enum kernel_scheduler_status status=kernel_wait_queue_wake_all(queues[i]);
        if (status!=KERNEL_SCHEDULER_STATUS_OK && status!=KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED)
            __builtin_trap();
    }
}

static void pipe_async(struct kernel_pipe *pipe,unsigned reason)
{
    struct kernel_wait_cursor cursor={0};
    /* The caller owns the pipe through producer completion. The queue lock
     * captures subscriber borrows; no endpoint fields enter the event. */
    if (!kernel_wait_cursor_prepare_event(&pipe->async_events,&cursor,reason,NULL)) return;
    kernel_wait_cursor_wake(&cursor,1);
}

int64_t kernel_pipe_capacity(struct kernel_pipe *pipe,int command,uint64_t argument)
{
    if (!pipe) return -KERNEL_EBADF;
    if (command==1032) {
        PIPE_META(guard,pipe);
        return (int64_t)((uint64_t)pipe->ring_size*BOAROS_PAGE_SIZE);
    }
    if (command!=1031) return -KERNEL_EINVAL;
    /* Linux在64位目标上也只消费unsigned int参数。 */
    uint32_t size=(uint32_t)argument;
    if (size>(UINT32_C(1)<<31)) return -KERNEL_EINVAL;
    uint32_t rounded=BOAROS_PAGE_SIZE;
    while (rounded<size) rounded<<=1;
    uint32_t nr=rounded/BOAROS_PAGE_SIZE;
    if (rounded>PIPE_MAX_UNPRIVILEGED_BYTES &&
        !kernel_credentials_capable(kernel_credentials_current(),KERNEL_CAP_SYS_RESOURCE)) {
        /* 此快照可直接拒绝越限扩容；通过者仍在复制资格内复核容量。 */
        PIPE_META(guard,pipe);
        if (nr>pipe->ring_size) return -KERNEL_EPERM;
    }
    struct kernel_pipe_buffer local[KERNEL_PIPE_INLINE_SLOTS]={0};
    struct kernel_pipe_buffer *candidate=local,*retired=0;
    enum kernel_heap_status allocated=KERNEL_HEAP_STATUS_OK;
    /* 分配可回收并睡眠，不能占用复制资格；返回错误仍先判断权限和额度。 */
    if (nr>KERNEL_PIPE_INLINE_SLOTS) {
        candidate=0;
        allocated=kernel_heap_allocate_zeroed(pipe->heap,nr,sizeof(*candidate),(void **)&candidate);
        if (allocated && allocated!=KERNEL_HEAP_STATUS_EMPTY) __builtin_trap();
    }
    uintptr_t irq=arch_interrupt_save();
    int64_t result;
    int changed=0;
    {
        KERNEL_LOCK_SCOPE(reader);kernel_mutex_lock(&pipe->read_lock,&reader);
        KERNEL_LOCK_SCOPE(writer);kernel_mutex_lock(&pipe->write_lock,&writer);
        uint32_t old=pipe->ring_size;
        const struct kernel_credentials *cred=kernel_credentials_current();
        do {
            if (nr>old && rounded>PIPE_MAX_UNPRIVILEGED_BYTES &&
                !kernel_credentials_capable(cred,KERNEL_CAP_SYS_RESOURCE)) {
                result=-KERNEL_EPERM;break;
            }
            uint64_t pages=kernel_uid_account_pipe_pages(pipe->account,old,nr);
            if (nr>old && (pages>PIPE_USER_PAGES_SOFT || pipe_over_hard_limit(pages)) &&
                pipe_unprivileged(cred)) result=-KERNEL_EPERM;
            else if (allocated) result=-KERNEL_ENOMEM;
            else if (nr<pipe->slots) result=-KERNEL_EBUSY;
            else {
                if (pipe->read_slot!=KERNEL_PIPE_NO_SLOT || pipe->merge_slot!=KERNEL_PIPE_NO_SLOT)
                    __builtin_trap();
                /* 两个复制资格排除全部片段借用者；poll只观察后面的短状态发布。 */
                for (uint32_t i=0;i<pipe->slots;i++)
                    candidate[i]=pipe->buffers[(pipe->head+i)&(old-1U)];
                struct kernel_pipe_buffer *published=candidate;
                if (nr<=KERNEL_PIPE_INLINE_SLOTS) {
                    memcpy(pipe->inline_buffers,local,sizeof(local));
                    published=pipe->inline_buffers;
                }
                { PIPE_META(guard,pipe);
                  if (pipe->buffers!=pipe->inline_buffers) retired=pipe->buffers;
                  pipe->buffers=published;
                  pipe->ring_size=nr;
                  pipe->head=0;
                  pipe->tail=pipe->slots&(nr-1U); }
                if (published==candidate) candidate=0;
                result=(int64_t)((uint64_t)nr*BOAROS_PAGE_SIZE);
                changed=1;break;
            }
            (void)kernel_uid_account_pipe_pages(pipe->account,nr,old);
        } while (0);
    }
    if (retired) kernel_heap_release(pipe->heap,retired);
    if (candidate && candidate!=local) kernel_heap_release(pipe->heap,candidate);
    if (changed) pipe_notify(pipe,0,1);
    arch_interrupt_restore(irq);
    return result;
}
int kernel_pipe_check_task_domain(struct kernel_pipe *pipe)
{
    PIPE_META(guard, pipe);
    if (pipe->fifo_node || !pipe->owners) return -KERNEL_EBUSY;
    struct kernel_wait_queue *queues[] = {&pipe->read_queue, &pipe->write_queue, &pipe->both_queue};
    for (unsigned i = 0; i < 3; i++) {
        KERNEL_IRQ_LOCK_SCOPE(wait_guard, &queues[i]->lock);
        if (queues[i]->registrations || queues[i]->borrows || queues[i]->closed) return -KERNEL_EBUSY;
    }
    return 0;
}

enum kernel_pipe_status kernel_pipe_acquire_endpoint(struct kernel_pipe *pipe,uint32_t endpoint)
{
    if (!pipe || !pipe->heap || !pipe->allocator || endpoint<1 || endpoint>3)
        return KERNEL_PIPE_STATUS_INVALID_ARGUMENT;
    uintptr_t irq=arch_interrupt_save();
    {
        PIPE_META(guard,pipe);
        if (pipe->owners==UINT32_MAX ||
            ((endpoint & 1) && (pipe->readers==UINT32_MAX || pipe->reader_generation==UINT64_MAX)) ||
            ((endpoint & 2) && (pipe->writers==UINT32_MAX || pipe->writer_generation==UINT64_MAX))) __builtin_trap();
        pipe->owners++;
        if (endpoint & 1) { pipe->readers++; pipe->reader_generation++; }
        if (endpoint & 2) { pipe->writers++; pipe->writer_generation++; }
    }
    /* 新取得的endpoint owner覆盖通知；不得在raw内执行回调。 */
    pipe_notify(pipe,1,1);
    arch_interrupt_restore(irq);
    return KERNEL_PIPE_STATUS_OK;
}
enum kernel_pipe_status kernel_pipe_destroy_unowned(struct kernel_pipe *pipe)
{
    if (!pipe || !pipe->heap) return KERNEL_PIPE_STATUS_INVALID_ARGUMENT;
    { PIPE_META(guard,pipe);
      if (pipe->readers || pipe->writers || pipe->owners) return KERNEL_PIPE_STATUS_INVALID_ARGUMENT; }
    return pipe_destroy(pipe);
}
enum kernel_pipe_status kernel_pipe_release_endpoint(struct kernel_pipe *pipe,uint32_t endpoint)
{
    if (!pipe || !pipe->heap || endpoint<1 || endpoint>3) return KERNEL_PIPE_STATUS_INVALID_ARGUMENT;
    uintptr_t irq=arch_interrupt_save();
    int readers,writers,last;
    {
        PIPE_META(guard,pipe);
        if (!pipe->owners || ((endpoint & 1) && !pipe->readers) ||
            ((endpoint & 2) && !pipe->writers)) __builtin_trap();
        if (endpoint & 1) pipe->readers--;
        if (endpoint & 2) pipe->writers--;
        readers=!pipe->writers; writers=!pipe->readers;
    }
    /* 本次关闭的owner仍在owners里；另一个关闭者不能提前释放通知目标。 */
    pipe_notify(pipe,readers,writers);
    if (readers!=writers) {
        pipe_async(pipe,KERNEL_ASYNC_POLL_IN);
        pipe_async(pipe,KERNEL_ASYNC_POLL_OUT);
    }
    { PIPE_META(guard,pipe); if (!pipe->owners) __builtin_trap(); last=!--pipe->owners; }
    enum kernel_pipe_status status=last ? pipe_destroy(pipe) : KERNEL_PIPE_STATUS_OK;
    arch_interrupt_restore(irq);
    return status;
}
/* 打开中和已安装的OFD均拥有pipe；inode关联本身不延长生命周期。 */
int kernel_pipe_fifo_open(struct kernel_heap *heap,struct kernel_vfs_node *node,
    uint32_t flags,struct kernel_pipe **owner,uint8_t *endpoint,uint64_t *observed_writers)
{
    kernel_assert_service_context();
    struct kernel_pipe *candidate=0,*pipe;
    uintptr_t irq=arch_interrupt_save();
    pipe=node->fifo_pipe;
    if (pipe) {
        PIPE_META(guard,pipe);
        if (pipe->owners==UINT32_MAX) __builtin_trap();
        pipe->owners++;
    }
    arch_interrupt_restore(irq);
    if (!pipe) {
        enum kernel_pipe_status status=kernel_pipe_create(heap,&candidate);
        if (status) return status==KERNEL_PIPE_STATUS_NO_MEMORY ? -KERNEL_ENOMEM : -KERNEL_EIO;
        irq=arch_interrupt_save();
        pipe=node->fifo_pipe;
        if (!pipe) {
            pipe=candidate; candidate=0;
            node->fifo_pipe=pipe; pipe->fifo_node=node;
        }
        { PIPE_META(guard,pipe);
          if (pipe->owners==UINT32_MAX) __builtin_trap();
          pipe->owners++; }
        arch_interrupt_restore(irq);
        if (candidate) kernel_pipe_destroy_unowned(candidate);
    }
    irq=arch_interrupt_save();
    uint8_t direction=(flags & 3)==0 ? KERNEL_PIPE_ENDPOINT_READ :
        (flags & 3)==1 ? KERNEL_PIPE_ENDPOINT_WRITE : KERNEL_PIPE_ENDPOINT_BOTH;
    int result=0; uint64_t generation;
    *observed_writers=0;
    {
        PIPE_META(guard,pipe);
        generation=direction==KERNEL_PIPE_ENDPOINT_READ ? pipe->writer_generation : pipe->reader_generation;
        if (direction==KERNEL_PIPE_ENDPOINT_WRITE && (flags & KERNEL_PIPE_NONBLOCK) && !pipe->readers)
            result=-KERNEL_ENXIO;
        if (!result) {
            if (((direction & 1) && (pipe->readers==UINT32_MAX || pipe->reader_generation==UINT64_MAX)) ||
                ((direction & 2) && (pipe->writers==UINT32_MAX || pipe->writer_generation==UINT64_MAX))) __builtin_trap();
            if (direction & 1) { pipe->readers++; pipe->reader_generation++; }
            if (direction & 2) { pipe->writers++; pipe->writer_generation++; }
            if (direction==KERNEL_PIPE_ENDPOINT_READ && !pipe->writers && (flags & KERNEL_PIPE_NONBLOCK))
                *observed_writers=pipe->writer_generation;
        }
    }
    if (result) {
        int last;
        { PIPE_META(guard,pipe); if (!pipe->owners) __builtin_trap(); last=!--pipe->owners; }
        if (last) pipe_destroy(pipe);
        arch_interrupt_restore(irq); return result;
    }
    pipe_notify(pipe,1,1);
    while (direction!=KERNEL_PIPE_ENDPOINT_BOTH && !(flags & KERNEL_PIPE_NONBLOCK)) {
        struct kernel_wait_token token={0}; enum kernel_scheduler_status status;
        {
            PIPE_META(guard,pipe);
            int waiting=direction==KERNEL_PIPE_ENDPOINT_READ ?
                !pipe->writers && pipe->writer_generation==generation :
                !pipe->readers && pipe->reader_generation==generation;
            if (!waiting) break;
            status=kernel_wait_prepare(&pipe->both_queue,0,1,&token);
        }
        if (status) { result=-KERNEL_EIO; break; }
        enum kernel_wait_wake_reason reason;
        status=kernel_wait_park(&token,&reason);
        if (kernel_wait_finish(&token)) __builtin_trap();
        if (status) { result=-KERNEL_EIO; break; }
        if (reason==KERNEL_WAIT_SIGNALLED) {
            PIPE_META(guard,pipe);
            uint64_t current=direction==KERNEL_PIPE_ENDPOINT_READ ? pipe->writer_generation : pipe->reader_generation;
            if (current==generation) {
                kernel_raw_scope_release(&guard);
                kernel_signal_note_syscall_restart(kernel_task_current());
                result=-KERNEL_ERESTARTSYS; break;
            }
        }
    }
    if (result) {
        if (kernel_pipe_release_endpoint(pipe,direction)) __builtin_trap();
    } else { *owner=pipe; *endpoint=direction; }
    arch_interrupt_restore(irq);
    return result;
}

static uint32_t next_slot(const struct kernel_pipe *pipe,uint32_t slot)
{ return (slot+1U)&(pipe->ring_size-1U); }

static void *pipe_consume(struct kernel_pipe *pipe, uint64_t count)
{
    uint32_t slot = pipe->head;
    void *page=0;
    if (pipe->merge_slot==slot) __builtin_trap();
    pipe->buffers[slot].offset += (uint16_t)count;
    pipe->buffers[slot].length -= (uint16_t)count;
    pipe->bytes -= count;
    if (pipe->buffers[slot].length == 0U) {
        page=pipe->buffers[slot].page;pipe->buffers[slot].page=0;
        pipe->buffers[slot].offset = 0U;
        pipe->buffers[slot].can_merge=0;
        pipe->head = next_slot(pipe,slot);
        pipe->slots--;
    }
    return page;
}

static uint16_t pipe_tail_space(const struct kernel_pipe *pipe)
{
    uint32_t last;

    if (pipe->slots == 0U) return 0U;
    last = (pipe->tail+pipe->ring_size-1U)&(pipe->ring_size-1U);
    if (pipe->read_slot==last || !pipe->buffers[last].can_merge) return 0U;
    return (uint16_t)(BOAROS_PAGE_SIZE - pipe->buffers[last].offset -
                      pipe->buffers[last].length);
}

static int pipe_wait_ready(struct kernel_pipe *pipe,int writing,int nonblocking)
{
    for (;;) {
        struct kernel_wait_token token={0};
        {
            PIPE_META(guard,pipe);
            if (writing) {
                if (!pipe->readers) return -KERNEL_EPIPE;
                if (pipe->slots<pipe->ring_size) return 0;
            } else if (pipe->bytes || !pipe->writers) return 0;
            if (nonblocking) return -KERNEL_EAGAIN;
            if (kernel_wait_prepare(writing?&pipe->write_queue:&pipe->read_queue,0,1,&token)) __builtin_trap();
        }
        enum kernel_wait_wake_reason reason;
        if (kernel_wait_park(&token,&reason) || kernel_wait_finish(&token)) __builtin_trap();
        if (reason==KERNEL_WAIT_SIGNALLED) return -KERNEL_ERESTARTSYS;
    }
}
int64_t kernel_pipe_read_begin(struct kernel_pipe *pipe,size_t count,int nonblocking,
    struct kernel_pipe_read_borrow *borrow)
{
    if (!count || borrow->guard.lock) __builtin_trap();
    for (;;) {
        int error=pipe_wait_ready(pipe,0,nonblocking);
        if (error) return error;
        kernel_mutex_lock(&pipe->read_lock,&borrow->guard);
        for (;;) {
            PIPE_META(guard,pipe);
            if (!pipe->bytes) break;
            if (pipe->merge_slot==pipe->head) {
                kernel_raw_scope_release(&guard);
                KERNEL_LOCK_SCOPE(writer);kernel_mutex_lock(&pipe->write_lock,&writer);
                continue;
            }
            struct kernel_pipe_buffer *part=&pipe->buffers[pipe->head];
            if (!pipe->slots || !part->page || !part->length || pipe->read_slot!=KERNEL_PIPE_NO_SLOT) __builtin_trap();
            pipe->read_slot=pipe->head;borrow->pipe=pipe;
            borrow->length=count<pipe->bytes?count:(size_t)pipe->bytes;
            borrow->cursor_slot=pipe->head;borrow->cursor_position=0;
            return (int64_t)borrow->length;
        }
        int eof;
        { PIPE_META(guard,pipe);eof=!pipe->bytes && !pipe->writers; }
        kernel_lock_release(&borrow->guard);
        if (eof) return 0;
    }
}
void kernel_pipe_read_finish(struct kernel_pipe_read_borrow *borrow,size_t consumed)
{
    struct kernel_pipe *pipe=borrow->pipe;
    if (!pipe || consumed>borrow->length || !borrow->guard.lock) __builtin_trap();
    size_t left=consumed;
    int wake_writers=0;
    while (left) {
        void *page=NULL;
        {
            PIPE_META(guard,pipe);
            if (pipe->read_slot!=pipe->head || !pipe->slots) __builtin_trap();
            /* A tail append may have started before this multi-buffer reader
             * advanced to it. Its published prefix is stable, but do not
             * retire that slot until the writer ends its reservation. */
            if (pipe->merge_slot==pipe->head) {
                kernel_raw_scope_release(&guard);
                KERNEL_LOCK_SCOPE(writer);kernel_mutex_lock(&pipe->write_lock,&writer);
                continue;
            }
            size_t n=left<pipe->buffers[pipe->head].length?left:pipe->buffers[pipe->head].length;
            if (n==pipe->buffers[pipe->head].length && pipe->slots==pipe->ring_size) wake_writers=1;
            page=pipe_consume(pipe,n);left-=n;pipe->read_slot=pipe->head;
        }
        if (page) pipe_release_page(pipe,page);
    }
    { PIPE_META(guard,pipe);pipe->read_slot=KERNEL_PIPE_NO_SLOT; }
    kernel_lock_release(&borrow->guard);*borrow=(struct kernel_pipe_read_borrow){0};
    if (wake_writers) pipe_notify(pipe,0,1);
    pipe_async(pipe,KERNEL_ASYNC_POLL_OUT);
}
void kernel_pipe_read_fragment(struct kernel_pipe_read_borrow *borrow,size_t offset,
    const void **data,size_t *count)
{
    struct kernel_pipe *pipe=borrow->pipe;
    if (!pipe || !borrow->guard.lock || offset>=borrow->length) __builtin_trap();
    if (offset<borrow->cursor_position) {borrow->cursor_slot=pipe->head;borrow->cursor_position=0;}
    for (;;) {
        struct kernel_pipe_buffer part;
        { PIPE_META(guard,pipe);part=pipe->buffers[borrow->cursor_slot]; }
        if (!part.length || !part.page) __builtin_trap();
        size_t within=offset-borrow->cursor_position;
        if (within<part.length) {
            *data=part.page+part.offset+within;*count=part.length-within;
            if (*count>borrow->length-offset)*count=borrow->length-offset;
            return;
        }
        borrow->cursor_position+=part.length;borrow->cursor_slot=next_slot(pipe,borrow->cursor_slot);
    }
}
void kernel_pipe_read_copy(struct kernel_pipe_read_borrow *borrow,size_t offset,void *target,size_t count)
{
    if (offset>borrow->length || count>borrow->length-offset) __builtin_trap();
    unsigned char *out=target;
    while (count) {
        const void *data;size_t n;kernel_pipe_read_fragment(borrow,offset,&data,&n);
        if(n>count)n=count;
        memcpy(out,data,n);out+=n;offset+=n;count-=n;
    }
}
int kernel_pipe_write_begin(struct kernel_pipe *pipe,int nonblocking,struct kernel_pipe_write_borrow *borrow)
{
    if (borrow->guard.lock) __builtin_trap();
    for (;;) {
        int error=pipe_wait_ready(pipe,1,nonblocking);
        if (error) return error;
        kernel_mutex_lock(&pipe->write_lock,&borrow->guard);
        { PIPE_META(guard,pipe);
          error=!pipe->readers?-KERNEL_EPIPE:0;
          if (!error && pipe->slots<pipe->ring_size) {
              borrow->pipe=pipe;borrow->slots=pipe->ring_size-pipe->slots;return 0;
          }
        }
        kernel_lock_release(&borrow->guard);
        if (error) return error;
    }
}
int kernel_pipe_push_page(struct kernel_pipe_write_borrow *borrow,void *page,size_t offset,size_t length)
{
    struct kernel_pipe *pipe=borrow->pipe;
    if (!pipe || !borrow->guard.lock || !page || !length || length>BOAROS_PAGE_SIZE-offset || !borrow->slots) __builtin_trap();
    {
        PIPE_META(guard,pipe);
        if (!pipe->readers) return -KERNEL_EPIPE;
        if (pipe->slots==pipe->ring_size || pipe->buffers[pipe->tail].page) __builtin_trap();
        pipe->buffers[pipe->tail]=(struct kernel_pipe_buffer){page,(uint16_t)offset,(uint16_t)length,0};
        pipe->tail=next_slot(pipe,pipe->tail);pipe->slots++;pipe->bytes+=length;borrow->slots--;borrow->changed=1;
    }
    pipe_notify(pipe,1,0);
    return 0;
}
void kernel_pipe_write_finish(struct kernel_pipe_write_borrow *borrow)
{
    if (!borrow->pipe || !borrow->guard.lock) __builtin_trap();
    struct kernel_pipe *pipe=borrow->pipe;unsigned changed=borrow->changed;
    kernel_lock_release(&borrow->guard);*borrow=(struct kernel_pipe_write_borrow){0};
    if (changed) pipe_async(pipe,KERNEL_ASYNC_POLL_IN);
}

/* All copy qualifications are ordered by their actual keys, including reverse
 * A->B/B->A transfers and resize's read->write pair. Wait with none held. */
int64_t kernel_pipe_link(struct kernel_pipe *in,struct kernel_pipe *out,size_t count,
    int nonblocking,int consume,unsigned *broken)
{
    *broken=0;
    for (;;) {
        int error=pipe_wait_ready(in,0,nonblocking);
        if (!error) error=pipe_wait_ready(out,1,nonblocking);
        if (error) {*broken=error==-KERNEL_EPIPE;return error;}
        struct kernel_mutex *locks[]={&in->read_lock,&in->write_lock,&out->read_lock,&out->write_lock};
        for (unsigned i=1;i<4;i++) for (unsigned j=i;j && (uintptr_t)locks[j]<(uintptr_t)locks[j-1];j--) {
            struct kernel_mutex *swap=locks[j];locks[j]=locks[j-1];locks[j-1]=swap;
        }
        struct kernel_lock_guard guards[4]={{0}};
        for (unsigned i=0;i<4;i++) kernel_mutex_lock(locks[i],&guards[i]);
        uint64_t total=0;uint32_t index=in->head,remaining=in->slots;
        int retry=0,wake_writers=0;
        for (;;) {
            if (!count) break;
            struct kernel_pipe_buffer part={0};int eof=0,full=0;
            {
                PIPE_META(guard,in);
                eof=!in->bytes && !in->writers;
                if (remaining) part=in->buffers[index];
            }
            { PIPE_META(guard,out);
              if (!out->readers) {*broken=1;error=-KERNEL_EPIPE;break;}
              full=out->slots==out->ring_size;
            }
            if (!remaining || full || !count) {
                retry=!total && !eof && (!remaining || full);break;
            }
            size_t n=count<part.length?count:part.length;
            int move=consume && n==part.length;
            /* Physical refs have a lower raw rank than endpoint metadata. */
            if (!move) {
                uint64_t physical;
                if (!in->heap->physical_address(part.page,&physical) || in->allocator!=out->allocator ||
                    physical_page_acquire(in->allocator,physical)) __builtin_trap();
                part.can_merge=0;
            }
            int publish=0;
            {
                struct kernel_pipe *first=(uintptr_t)in<(uintptr_t)out?in:out,*second=first==in?out:in;
                PIPE_META(a,first);PIPE_META(b,second);
                if (!out->readers) {*broken=1;error=-KERNEL_EPIPE;}
                else {
                    if (in->read_slot!=KERNEL_PIPE_NO_SLOT || in->merge_slot!=KERNEL_PIPE_NO_SLOT ||
                        out->read_slot!=KERNEL_PIPE_NO_SLOT || out->merge_slot!=KERNEL_PIPE_NO_SLOT ||
                        out->buffers[out->tail].page) __builtin_trap();
                    part.length=(uint16_t)n;out->buffers[out->tail]=part;
                    out->tail=next_slot(out,out->tail);out->slots++;out->bytes+=n;
                    if (consume) {
                        if (move && in->slots==in->ring_size) wake_writers=1;
                        void *released=pipe_consume(in,n);
                        if (move && released!=part.page) __builtin_trap();
                    }
                    publish=1;
                }
            }
            if (!publish) {if (!move) pipe_release_page(out,part.page);break;}
            total+=n;count-=n;
            if (consume) {index=in->head;remaining=in->slots;}
            else {index=next_slot(in,index);remaining--;}
        }
        for (unsigned i=4;i;i--) kernel_lock_release(&guards[i-1]);
        if (total) {
            pipe_notify(out,1,0);pipe_async(out,KERNEL_ASYNC_POLL_IN);
            if (wake_writers) pipe_notify(in,0,1);
            if (consume) pipe_async(in,KERNEL_ASYNC_POLL_OUT);
        }
        if (total || error || !retry) return total?(int64_t)total:error;
    }
}
int64_t kernel_pipe_vmsplice(struct kernel_pipe *pipe,struct kernel_mm *mm,
    const struct kernel_uaccess_iovec *iov,size_t count,uint64_t bytes,int nonblocking,unsigned *broken)
{
    *broken=0;
    struct kernel_pipe_write_borrow borrow={0};
    int error=kernel_pipe_write_begin(pipe,nonblocking,&borrow);
    if (error) {*broken=error==-KERNEL_EPIPE;return error;}
    struct kernel_uaccess_iov_cursor cursor={iov,count,0,0};uint64_t total=0;
    while (total<bytes && borrow.slots) {
        uint64_t address=0;size_t size=kernel_uaccess_iov_span(&cursor,bytes-total,&address);
        if (!size) __builtin_trap();
        size_t offset=address&BOAROS_PAGE_MASK;
        if (size>BOAROS_PAGE_SIZE-offset) size=BOAROS_PAGE_SIZE-offset;
        struct kernel_mm_user_pin pin={0};
        enum kernel_mm_status status=kernel_uaccess_pin_resolved(mm,address,0,KERNEL_MM_READ,&pin,NULL);
        if (status) {
            if(status==KERNEL_MM_STATUS_NO_MEMORY)error=-KERNEL_ENOMEM;
            else if(status==KERNEL_MM_STATUS_NOT_MAPPED || status==KERNEL_MM_STATUS_ACCESS ||
                status==KERNEL_MM_STATUS_ADDRESS_SPACE || status==KERNEL_MM_STATUS_BUS_FAULT)error=-KERNEL_EFAULT;
            else __builtin_trap();
            break;
        }
        /* The queued reference is independent of the temporary MM pin: unmap,
         * exit and COW can end that mapping while the pipe remains readable. */
        void *page;
        if (mm->allocator!=pipe->allocator || physical_page_acquire(pipe->allocator,pin.physical_address) ||
            physical_page_resolve(pipe->allocator,pin.physical_address,&page)) __builtin_trap();
        kernel_mm_unpin_user_page(&pin);
        error=kernel_pipe_push_page(&borrow,page,offset,size);
        if (error) {pipe_release_page(pipe,page);break;}
        cursor.offset+=size;total+=size;
    }
    kernel_pipe_write_finish(&borrow);
    *broken=error==-KERNEL_EPIPE;
    return total?(int64_t)total:error;
}

enum kernel_pipe_status kernel_pipe_readv(struct kernel_pipe *pipe,struct kernel_mm *mm,
    const struct kernel_uaccess_iovec *iov,size_t iov_count,uint64_t count,
    uint32_t flags,int64_t *result)
{
    if (!pipe || !mm || !result) return KERNEL_PIPE_STATUS_INVALID_ARGUMENT;
    if ((flags&3)==1) {*result=-KERNEL_EBADF;return KERNEL_PIPE_STATUS_OK;}
    if (!count) {*result=0;return KERNEL_PIPE_STATUS_OK;}
    struct kernel_pipe_read_borrow borrow={0};
    int64_t ready=kernel_pipe_read_begin(pipe,count,!!(flags&KERNEL_PIPE_NONBLOCK),&borrow);
    if(ready<=0){pipe_async(pipe,KERNEL_ASYNC_POLL_OUT);*result=ready;return KERNEL_PIPE_STATUS_OK;}
    struct kernel_uaccess_iov_cursor cursor={iov,iov_count,0,0};
    uint64_t total=0;int error=0;
    while (total<borrow.length) {
        const void *data;size_t size,copied=0;
        kernel_pipe_read_fragment(&borrow,(size_t)total,&data,&size);
        enum kernel_uaccess_status status=kernel_copy_to_user_iov(mm,&cursor,data,size,&copied);
        if (!status && copied!=size) __builtin_trap();
        if (status) {if(status!=KERNEL_UACCESS_STATUS_FAULT)__builtin_trap();error=-KERNEL_EFAULT;break;}
        total+=copied;
    }
    kernel_pipe_read_finish(&borrow,(size_t)total);
    *result=total?(int64_t)total:error;
    return KERNEL_PIPE_STATUS_OK;
}
static enum kernel_pipe_status pipe_write_source_locked(struct kernel_pipe *pipe,struct kernel_mm *mm,
    const struct kernel_uaccess_iovec *iov,size_t iov_count,uint64_t count,uint32_t flags,
    struct kernel_pipe_result *result,const unsigned char *buffer)
{
    if (!pipe || (!mm && !buffer) || !result || !pipe->heap || !pipe->allocator)
        return KERNEL_PIPE_STATUS_INVALID_ARGUMENT;
    *result=(struct kernel_pipe_result){0};
    if (!(flags & 3)) { result->bytes=-KERNEL_EBADF; return KERNEL_PIPE_STATUS_OK; }
    if (!count) return KERNEL_PIPE_STATUS_OK;
    uintptr_t irq=arch_interrupt_save();
    KERNEL_LOCK_SCOPE(copy); kernel_mutex_lock(&pipe->write_lock,&copy);
    /* sendfile/splice来源始终占新片段，不能并入user write的尾片段。 */
    uint64_t total=0; uint16_t remainder=buffer ? 0 : (uint16_t)(count & BOAROS_PAGE_MASK);
    struct kernel_uaccess_iov_cursor cursor={iov,iov_count,0,0};
    while (total<count) {
        uint32_t slot; uint16_t offset; size_t chunk; int merging,wait=0;
        {
            PIPE_META(guard,pipe);
            if (!pipe->readers) {
                result->broken=1;
                result->bytes=total ? (int64_t)total : -KERNEL_EPIPE;
                kernel_raw_scope_release(&guard);
                kernel_lock_scope_release(&copy); arch_interrupt_restore(irq);
                return KERNEL_PIPE_STATUS_OK;
            }
            merging=!total && remainder && pipe->slots && pipe_tail_space(pipe)>=remainder;
            if (!merging && pipe->slots==pipe->ring_size) {
                if (flags & KERNEL_PIPE_NONBLOCK) {
                    result->bytes=total ? (int64_t)total : -KERNEL_EAGAIN;
                    kernel_raw_scope_release(&guard);
                    kernel_lock_scope_release(&copy); arch_interrupt_restore(irq);
                    return KERNEL_PIPE_STATUS_OK;
                }
                wait=1; slot=offset=0; chunk=0;
            } else {
                slot=merging ? (pipe->tail+pipe->ring_size-1U)&(pipe->ring_size-1U) : pipe->tail;
                offset=merging ? (uint16_t)(pipe->buffers[slot].offset+pipe->buffers[slot].length) : 0;
                uint64_t rest=count-total;
                chunk=merging ? remainder : rest<BOAROS_PAGE_SIZE ? (size_t)rest : BOAROS_PAGE_SIZE;
                if (merging) pipe->merge_slot=slot;
            }
        }
        if (wait) {
            /* No copy reservation exists while full. Reuse the condition+
             * registration step without carrying a wait token through faults. */
            kernel_lock_release(&copy);
            pipe_async(pipe,KERNEL_ASYNC_POLL_IN);
            int error=pipe_wait_ready(pipe,1,0);
            if (error) {
                result->broken=error==-KERNEL_EPIPE;
                result->bytes=total ? (int64_t)total : error;
                arch_interrupt_restore(irq); return KERNEL_PIPE_STATUS_OK;
            }
            kernel_mutex_lock(&pipe->write_lock,&copy); continue;
        }
        if (!pipe->buffers[slot].page) {
            uint64_t physical;void *page;
            enum physical_page_status allocated=physical_page_allocate(pipe->allocator,&physical);
            if (allocated==PHYSICAL_PAGE_STATUS_EMPTY) {
                result->bytes=total?(int64_t)total:-KERNEL_ENOMEM;
                kernel_lock_scope_release(&copy);arch_interrupt_restore(irq);
                return KERNEL_PIPE_STATUS_OK;
            }
            if (allocated || physical_page_resolve(pipe->allocator,physical,&page)) __builtin_trap();
            /* 槽先拥有尚未发布的页；用户fault或最后端点释放都能找到它。 */
            { PIPE_META(guard,pipe);pipe->buffers[slot].page=page; }
        }
        unsigned char *destination=pipe->buffers[slot].page+offset;
        size_t done=0; enum kernel_uaccess_status access=KERNEL_UACCESS_STATUS_OK;
        if (buffer) { memcpy(destination,buffer+total,chunk); done=chunk; }
        else access=kernel_copy_from_user_iov(mm,&cursor,destination,chunk,&done);
        if (access || done!=chunk) {
            void *page=0;
            { PIPE_META(guard,pipe);
              pipe->merge_slot=KERNEL_PIPE_NO_SLOT;
              if (!merging) {
                  page=pipe->buffers[slot].page;pipe->buffers[slot].page=0;
              }
            }
            if (page) pipe_release_page(pipe,page);
            kernel_lock_scope_release(&copy); arch_interrupt_restore(irq);
            if (access!=KERNEL_UACCESS_STATUS_FAULT) return KERNEL_PIPE_STATUS_STATE;
            result->bytes=total ? (int64_t)total : -KERNEL_EFAULT;
            return KERNEL_PIPE_STATUS_OK;
        }
        {
            PIPE_META(guard,pipe);
            if (!merging) {
                pipe->buffers[slot].offset=pipe->buffers[slot].length=0;
                if (!buffer) pipe->buffers[slot].can_merge=1;
                else pipe->buffers[slot].can_merge=0;
                pipe->tail=next_slot(pipe,slot); pipe->slots++;
            }
            pipe->buffers[slot].length+=(uint16_t)chunk; pipe->bytes+=chunk;
            pipe->merge_slot=KERNEL_PIPE_NO_SLOT;
        }
        total+=chunk;
        pipe_notify(pipe,1,0);
    }
    kernel_lock_scope_release(&copy); arch_interrupt_restore(irq);
    result->bytes=(int64_t)total;
    return KERNEL_PIPE_STATUS_OK;
}
static enum kernel_pipe_status pipe_write_source(struct kernel_pipe *pipe,struct kernel_mm *mm,
    const struct kernel_uaccess_iovec *iov,size_t iov_count,uint64_t count,uint32_t flags,
    struct kernel_pipe_result *result,const unsigned char *buffer)
{
    enum kernel_pipe_status status=pipe_write_source_locked(pipe,mm,iov,iov_count,count,flags,result,buffer);
    if (!status && count && (flags&3U)) pipe_async(pipe,KERNEL_ASYNC_POLL_IN);
    return status;
}
enum kernel_pipe_status kernel_pipe_writev(struct kernel_pipe *pipe,struct kernel_mm *mm,
    const struct kernel_uaccess_iovec *iov,size_t iov_count,uint64_t count,uint32_t flags,
    struct kernel_pipe_result *result)
{ return pipe_write_source(pipe,mm,iov,iov_count,count,flags,result,0); }
enum kernel_pipe_status kernel_pipe_write_buffer(struct kernel_pipe *pipe,const void *buffer,
    size_t count,uint32_t flags,struct kernel_pipe_result *result)
{
    if (!buffer) return KERNEL_PIPE_STATUS_INVALID_ARGUMENT;
    return pipe_write_source(pipe,0,0,0,count,flags,result,buffer);
}

uint32_t kernel_pipe_poll(
    struct kernel_pipe *pipe,
    uint32_t endpoint,
    uint64_t observed_writers,
    struct kernel_wait_queue **out_queue)
{
    uint32_t events = 0U;

    if (pipe == 0) {
        if (out_queue != 0) *out_queue = 0;
        return KERNEL_POLLNVAL;
    }

    PIPE_META(guard,pipe);
    if (out_queue != 0)
        *out_queue = endpoint == KERNEL_PIPE_ENDPOINT_BOTH
            ? &pipe->both_queue :
              endpoint == KERNEL_PIPE_ENDPOINT_READ ? &pipe->read_queue :
              endpoint == KERNEL_PIPE_ENDPOINT_WRITE ? &pipe->write_queue : 0;
    if (endpoint & KERNEL_PIPE_ENDPOINT_READ) {
        if (pipe->bytes > 0U) {
            events |= (KERNEL_POLLIN | KERNEL_POLLRDNORM);
        }
        if (pipe->writers == 0U && pipe->writer_generation != observed_writers) {
            events |= KERNEL_POLLHUP;
        }
    }
    if (endpoint & KERNEL_PIPE_ENDPOINT_WRITE) {
        if (pipe->readers == 0U) {
            events |= KERNEL_POLLERR;
            events |= (KERNEL_POLLOUT | KERNEL_POLLWRNORM);
        } else if (pipe->slots < pipe->ring_size) {
            events |= (KERNEL_POLLOUT | KERNEL_POLLWRNORM);
        }
    }
    if (endpoint < KERNEL_PIPE_ENDPOINT_READ ||
        endpoint > KERNEL_PIPE_ENDPOINT_BOTH) {
        if (out_queue != 0) *out_queue = 0;
        return KERNEL_POLLNVAL;
    }

    return events;
}
