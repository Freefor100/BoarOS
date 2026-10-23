#include <kernel/credentials.h>
#include "pipe_internal.h"
#include "vfs_objects.h"
#include "uaccess_iov_internal.h"

#include <arch/context.h>
#include <kernel/errno.h>
#include <kernel/files.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
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
    if (pipe->read_queue.initialized) {
        struct kernel_wait_queue *queues[]={&pipe->read_queue,&pipe->write_queue,&pipe->both_queue};
        for (unsigned i=0;i<3;i++) {
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
    for (unsigned i=0;i<16;i++) if (pipe->pages[i]) pipe_release_page(pipe,pipe->pages[i]);
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
    pipe->atime = pipe->mtime = pipe->ctime = pipe_now();
    pipe->read_slot=pipe->merge_slot=16;
    kernel_wait_queue_init(&pipe->read_queue);
    kernel_wait_queue_init(&pipe->write_queue);
    kernel_wait_queue_init(&pipe->both_queue);
    /* read在前，只有同页合并时才按key顺序短暂等待write；writer不反向取read。 */
    kernel_mutex_init(&pipe->read_lock,15U,(uintptr_t)&pipe->read_lock);
    kernel_mutex_init(&pipe->write_lock,15U,(uintptr_t)&pipe->write_lock);
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
    struct kernel_wait_queue *queues[]={&pipe->read_queue,&pipe->write_queue,&pipe->both_queue};
    int notify[]={readers,writers,readers||writers};
    for (unsigned i=0;i<3;i++) if (notify[i]) {
        enum kernel_scheduler_status status=kernel_wait_queue_wake_all(queues[i]);
        if (status!=KERNEL_SCHEDULER_STATUS_OK && status!=KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED)
            __builtin_trap();
    }
}
int kernel_pipe_check_task_domain(struct kernel_pipe *pipe)
{
    PIPE_META(guard, pipe);
    if (pipe->fifo_node || !pipe->owners) return -KERNEL_EBUSY;
    KERNEL_RAW_SCOPE(wait_guard, &kernel_wait_domain);
    struct kernel_wait_queue *queues[] = {&pipe->read_queue, &pipe->write_queue, &pipe->both_queue};
    for (unsigned i = 0; i < 3; i++)
        if (queues[i]->registrations || queues[i]->borrows || queues[i]->closed) return -KERNEL_EBUSY;
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

static uint16_t next_slot(uint16_t slot)
{
    return (uint16_t)((slot + 1U) %
                      (KERNEL_PIPE_CAPACITY / BOAROS_PAGE_SIZE));
}

static void *pipe_consume(struct kernel_pipe *pipe, uint64_t count)
{
    uint16_t slot = pipe->head;
    void *page=0;
    if (pipe->merge_slot==slot) __builtin_trap();
    pipe->page_offset[slot] += (uint16_t)count;
    pipe->page_length[slot] -= (uint16_t)count;
    pipe->bytes -= count;
    if (pipe->page_length[slot] == 0U) {
        page=pipe->pages[slot];pipe->pages[slot]=0;
        pipe->page_offset[slot] = 0U;
        pipe->can_merge &= (uint16_t)~(1U << slot);
        pipe->head = next_slot(slot);
        pipe->slots--;
    }
    return page;
}

static uint16_t pipe_tail_space(const struct kernel_pipe *pipe)
{
    uint16_t last;

    if (pipe->slots == 0U) return 0U;
    last = (uint16_t)((pipe->tail +
                       KERNEL_PIPE_CAPACITY / BOAROS_PAGE_SIZE - 1U) %
                      (KERNEL_PIPE_CAPACITY / BOAROS_PAGE_SIZE));
    if (pipe->read_slot==last || !(pipe->can_merge & (1U << last))) return 0U;
    return (uint16_t)(BOAROS_PAGE_SIZE - pipe->page_offset[last] -
                      pipe->page_length[last]);
}

/* 已在对象raw保护内登记；释放复制资格后park，finish覆盖所有返回路径。 */
static enum kernel_scheduler_status pipe_park(struct kernel_lock_guard *copy,
    struct kernel_wait_token *token,enum kernel_wait_wake_reason *reason)
{
    kernel_lock_release(copy);
    enum kernel_scheduler_status status=kernel_wait_park(token,reason);
    if (kernel_wait_finish(token)) __builtin_trap();
    return status;
}
enum kernel_pipe_status kernel_pipe_readv(struct kernel_pipe *pipe,struct kernel_mm *mm,
    const struct kernel_uaccess_iovec *iov,size_t iov_count,uint64_t count,
    uint32_t flags,int64_t *result)
{
    if (!pipe || !mm || !result || !pipe->heap || !pipe->allocator)
        return KERNEL_PIPE_STATUS_INVALID_ARGUMENT;
    if ((flags & 3)==1) { *result=-KERNEL_EBADF; return KERNEL_PIPE_STATUS_OK; }
    if (!count) { *result=0; return KERNEL_PIPE_STATUS_OK; }
    uintptr_t irq=arch_interrupt_save();
    KERNEL_LOCK_SCOPE(copy); kernel_mutex_lock(&pipe->read_lock,&copy);
    uint64_t requested;
    for (;;) {
        struct kernel_wait_token token={0};
        enum kernel_scheduler_status status;
        {
            PIPE_META(guard,pipe);
            requested=count<pipe->bytes ? count : pipe->bytes;
            if (requested) break;
            if (!pipe->writers || (flags & KERNEL_PIPE_NONBLOCK)) {
                *result=!pipe->writers ? 0 : -KERNEL_EAGAIN;
                kernel_raw_scope_release(&guard);
                kernel_lock_scope_release(&copy); arch_interrupt_restore(irq);
                return KERNEL_PIPE_STATUS_OK;
            }
            status=kernel_wait_prepare(&pipe->read_queue,0,1,&token);
            /* 条件和登记由同一metadata保护；发布者只能在解锁后改变条件。 */
            if (pipe->bytes || !pipe->writers) __builtin_trap();
        }
        if (status) { kernel_lock_scope_release(&copy); arch_interrupt_restore(irq); return KERNEL_PIPE_STATUS_STATE; }
        enum kernel_wait_wake_reason reason;
        status=pipe_park(&copy,&token,&reason);
        if (status) { arch_interrupt_restore(irq); return KERNEL_PIPE_STATUS_STATE; }
        if (reason==KERNEL_WAIT_SIGNALLED) { *result=-KERNEL_ERESTARTSYS; arch_interrupt_restore(irq); return KERNEL_PIPE_STATUS_OK; }
        kernel_mutex_lock(&pipe->read_lock,&copy);
    }
    struct kernel_uaccess_iov_cursor cursor={iov,iov_count,0,0};
    uint64_t committed=0; int fault=0;
    while (committed<requested) {
        uint16_t slot,offset; size_t chunk;
        for (;;) {
            PIPE_META(guard,pipe);
            if (pipe->merge_slot==pipe->head) {
                kernel_raw_scope_release(&guard);
                KERNEL_LOCK_SCOPE(writer);kernel_mutex_lock(&pipe->write_lock,&writer);
                continue;
            }
            slot=pipe->head; offset=pipe->page_offset[slot];
            if (!pipe->slots || !pipe->page_length[slot]) __builtin_trap();
            uint64_t length=requested-committed;
            chunk=length<pipe->page_length[slot] ? (size_t)length : pipe->page_length[slot];
            pipe->read_slot=slot;
            break;
        }
        size_t done=0;
        enum kernel_uaccess_status access=kernel_copy_to_user_iov(mm,&cursor,
            pipe->pages[slot]+offset,chunk,&done);
        if (access || done!=chunk) {
            { PIPE_META(guard,pipe); pipe->read_slot=16; }
            if (access!=KERNEL_UACCESS_STATUS_FAULT) { kernel_lock_scope_release(&copy); arch_interrupt_restore(irq); return KERNEL_PIPE_STATUS_STATE; }
            fault=1; break;
        }
        void *page;
        { PIPE_META(guard,pipe);pipe->read_slot=16;page=pipe_consume(pipe,chunk); }
        if (page) pipe_release_page(pipe,page);
        committed+=chunk;
    }
    if (committed) pipe_notify(pipe,0,1);
    kernel_lock_scope_release(&copy); arch_interrupt_restore(irq);
    *result=committed ? (int64_t)committed : fault ? -KERNEL_EFAULT : 0;
    return KERNEL_PIPE_STATUS_OK;
}
static enum kernel_pipe_status pipe_write_source(struct kernel_pipe *pipe,struct kernel_mm *mm,
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
        uint16_t slot,offset; size_t chunk; int merging,wait=0;
        struct kernel_wait_token token={0}; enum kernel_scheduler_status status=KERNEL_SCHEDULER_STATUS_OK;
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
            if (!merging && pipe->slots==16) {
                if (flags & KERNEL_PIPE_NONBLOCK) {
                    result->bytes=total ? (int64_t)total : -KERNEL_EAGAIN;
                    kernel_raw_scope_release(&guard);
                    kernel_lock_scope_release(&copy); arch_interrupt_restore(irq);
                    return KERNEL_PIPE_STATUS_OK;
                }
                status=kernel_wait_prepare(&pipe->write_queue,0,1,&token);
                if (pipe->slots!=16 || !pipe->readers) __builtin_trap();
                wait=1; slot=offset=0; chunk=0;
            } else {
                slot=merging ? (uint16_t)((pipe->tail+15)%16) : pipe->tail;
                offset=merging ? (uint16_t)(pipe->page_offset[slot]+pipe->page_length[slot]) : 0;
                uint64_t rest=count-total;
                chunk=merging ? remainder : rest<BOAROS_PAGE_SIZE ? (size_t)rest : BOAROS_PAGE_SIZE;
                if (merging) pipe->merge_slot=slot;
            }
        }
        if (status) { kernel_lock_scope_release(&copy); arch_interrupt_restore(irq); return KERNEL_PIPE_STATUS_STATE; }
        if (wait) {
            enum kernel_wait_wake_reason reason;
            status=pipe_park(&copy,&token,&reason);
            if (status) { arch_interrupt_restore(irq); return KERNEL_PIPE_STATUS_STATE; }
            if (reason==KERNEL_WAIT_SIGNALLED) {
                result->bytes=total ? (int64_t)total : -KERNEL_ERESTARTSYS;
                arch_interrupt_restore(irq); return KERNEL_PIPE_STATUS_OK;
            }
            kernel_mutex_lock(&pipe->write_lock,&copy); continue;
        }
        if (!pipe->pages[slot]) {
            uint64_t physical;void *page;
            enum physical_page_status allocated=physical_page_allocate(pipe->allocator,&physical);
            if (allocated==PHYSICAL_PAGE_STATUS_EMPTY) {
                result->bytes=total?(int64_t)total:-KERNEL_ENOMEM;
                kernel_lock_scope_release(&copy);arch_interrupt_restore(irq);
                return KERNEL_PIPE_STATUS_OK;
            }
            if (allocated || physical_page_resolve(pipe->allocator,physical,&page)) __builtin_trap();
            /* 槽先拥有尚未发布的页；用户fault或最后端点释放都能找到它。 */
            { PIPE_META(guard,pipe);pipe->pages[slot]=page; }
        }
        unsigned char *destination=pipe->pages[slot]+offset;
        size_t done=0; enum kernel_uaccess_status access=KERNEL_UACCESS_STATUS_OK;
        if (buffer) { memcpy(destination,buffer+total,chunk); done=chunk; }
        else access=kernel_copy_from_user_iov(mm,&cursor,destination,chunk,&done);
        if (access || done!=chunk) {
            void *page=0;
            { PIPE_META(guard,pipe);
              pipe->merge_slot=16;
              if (!merging) {
                  page=pipe->pages[slot];pipe->pages[slot]=0;
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
                pipe->page_offset[slot]=pipe->page_length[slot]=0;
                if (!buffer) pipe->can_merge |= (uint16_t)(1U<<slot);
                else pipe->can_merge &= (uint16_t)~(1U<<slot);
                pipe->tail=next_slot(slot); pipe->slots++;
            }
            pipe->page_length[slot]+=(uint16_t)chunk; pipe->bytes+=chunk;
            pipe->merge_slot=16;
        }
        total+=chunk;
        pipe_notify(pipe,1,0);
    }
    kernel_lock_scope_release(&copy); arch_interrupt_restore(irq);
    result->bytes=(int64_t)total;
    return KERNEL_PIPE_STATUS_OK;
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
        } else if (pipe->slots < KERNEL_PIPE_CAPACITY / BOAROS_PAGE_SIZE) {
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
