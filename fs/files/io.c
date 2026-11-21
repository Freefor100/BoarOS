#include <kernel/aio.h>
#include "../vfs_notify.h"
#include "private.h"
#include "../open_file_internal.h"
#include "../pipe_internal.h"
#include "../char_device_internal.h"
#include "../block_file_internal.h"
#include "../uaccess_iov_internal.h"
#include "../../mm/uaccess_internal.h"
#include "../vfs_internal.h"
#include "../write_source.h"

#include <kernel/cost.h>
#include <kernel/credentials.h>
#include <kernel/cpu.h>
#include <kernel/errno.h>
#include <kernel/event_fd.h>
#include <kernel/mqueue.h>
#include <kernel/signal_fd.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/open_file.h>
#include <kernel/page.h>
#include <kernel/page_cache.h>
#include <kernel/scheduler.h>
#include <kernel/signal.h>
#include <kernel/socket.h>
#include <kernel/socket_control.h>
#include <kernel/task.h>
#include <kernel/time.h>
#include <kernel/uts.h>
#include <kernel/uaccess.h>
#include <kernel/vfs.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <arch/context.h>
#include <arch/platform_io.h>

#define KERNEL_FILES_WRITE_STAGING 64U

/* Scalar access_ok includes empty ranges; uaccess itself intentionally does not.
 * The user limit is a legal empty range, but an address above it is not. */
static int scalar_empty_range_fault(uint64_t address, uint64_t count)
{
    return count == 0U && address != 0U &&
        kernel_user_range_check(address - 1U, 1U) != KERNEL_UACCESS_STATUS_OK;
}

/* 调用现场持OFD pin及MM/files owner；callback结束之前iov与结果地址不退栈。
 * 队列只借用这些owner，不重查fd或再复制数据。 */
struct regular_call {
    const struct kernel_credentials *credentials;
    void (*execute)(void *);
    struct kernel_open_file_description *description;
    const struct kernel_lock_guard *offset_borrow, *operation_borrow;
    struct kernel_vfs_size_limit *size_limit;
    uint64_t length, whence;
    int64_t offset;
    int64_t *result;
    enum kernel_files_status status;
};
static void regular_worker(void *argument)
{
    struct regular_call *call=argument;
    const struct kernel_credentials *previous=kernel_credentials_override(call->credentials);
    call->execute(call);
    (void)kernel_credentials_override(previous);
}
static enum kernel_files_status regular_dispatch(struct regular_call *call,void (*execute)(void *))
{
    call->status=KERNEL_FILES_STATUS_STATE;
    call->credentials=kernel_credentials_current();call->execute=execute;
    struct kernel_page_cache *cache=kernel_open_file_page_cache(call->description);
    const struct kernel_lock_guard *borrow = call->offset_borrow ? call->offset_borrow : call->operation_borrow;
    enum kernel_page_cache_status status = kernel_page_cache_run_cpu0_borrowing(cache,regular_worker,call,borrow);
    return status?KERNEL_FILES_STATUS_STATE:call->status;
}
static int regular_caller_ready(void)
{
    struct kernel_io_context *io=kernel_io_context_current();
    return !io->locks && !io->user_pins && !io->cache_writes && !kernel_cpu_current()->raw_locks &&
        kernel_scheduler_can_sleep() && !arch_external_interrupt_active();
}
static int regular_borrows_offset(struct kernel_open_file_description *description,
    const struct kernel_lock_guard *borrowed)
{
    if (!borrowed) return 0;
    if (borrowed->lock!=&description->offset_lock.lock || !borrowed->write ||
        !borrowed->owner || borrowed->previous) __builtin_trap();
    return 1;
}
static void seek_cpu0(void *);
static void truncate_cpu0(void *);

void kernel_files_size_limit_notify(const struct kernel_vfs_size_limit *limit)
{
    if (limit->exceeded && kernel_signal_send_file_limit(kernel_task_current()) != KERNEL_SIGNAL_STATUS_OK)
        __builtin_trap();
}

/* 核心只报告接纳/断端；仅真实用户调用有SIGPIPE及syscall restart上下文。 */
enum kernel_files_status kernel_files_pipe_result(int64_t bytes,unsigned broken)
{
    struct kernel_task *task=kernel_task_current();
    if (kernel_task_has_user_context(task)) {
        if (broken && kernel_signal_send_task(task,13,0)!=KERNEL_SIGNAL_STATUS_OK)
            return KERNEL_FILES_STATUS_STATE;
        if (bytes==-KERNEL_ERESTARTSYS) kernel_signal_note_syscall_restart(task);
    }
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_release_io(
    struct kernel_files *files,
    struct kernel_open_file_description **owner,
    enum kernel_files_status operation_status)
{
    enum kernel_open_file_status release_status;

    release_status = kernel_open_file_release(owner);
    if (release_status == KERNEL_OPEN_FILE_STATUS_OK) {
        return operation_status;
    }
    if (release_status == KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED &&
        *owner != 0) {
        kernel_files_queue_description(files, *owner);
        *owner = 0;
        return operation_status;
    }
    return KERNEL_FILES_STATUS_STATE;
}

/* Sysctl reads commit the backend position only after the complete generated
 * prefix was copied. Every call reads the current configuration. */
static enum kernel_files_status control_read(
    struct kernel_files *files, struct kernel_mm *mm,
    struct kernel_open_file_description *description,
    struct kernel_uaccess_iov_cursor *cursor, uint64_t count,
    uint64_t offset, int advance, int64_t *linux_result)
{
    char buffer[KERNEL_UTS_FIELD_SIZE];
    size_t length = 0, copied = 0;
    int result = kernel_vfs_file_control_permission(&description->file, 0);
    if (!result) result = kernel_vfs_file_control(&description->file, 0, &offset, buffer,
        count < sizeof(buffer) ? (size_t)count : sizeof(buffer), &length);
    if (!result && length) {
        enum kernel_uaccess_status access = kernel_copy_to_user_iov(
            mm, cursor, buffer, length, &copied);
        if (access == KERNEL_UACCESS_STATUS_FAULT) result = -KERNEL_EFAULT;
        else if (access != KERNEL_UACCESS_STATUS_OK || copied != length)
            return KERNEL_FILES_STATUS_STATE;
    }
    if (result) {
        *linux_result = result;
        KERNEL_FILES_NOTE(files, read_failures, 1);
        return KERNEL_FILES_STATUS_OK;
    }
    if (advance && kernel_open_file_seek(description, offset) != KERNEL_OPEN_FILE_STATUS_OK)
        return KERNEL_FILES_STATUS_STATE;
    KERNEL_FILES_NOTE(files, bytes_read, length);
    *linux_result = (int64_t)length;
    return KERNEL_FILES_STATUS_OK;
}

/* The backend builds a stable OFD snapshot before any user copy. A fault
 * advances only bytes copied, and seeking to zero starts a new snapshot. */
static enum kernel_files_status mqueue_read_text(struct kernel_files *files,struct kernel_mm *mm,
    struct kernel_open_file_description *file,const struct kernel_uaccess_iovec *iov,
    size_t iov_count,uint64_t count,uint64_t offset,int advance,int scalar,int64_t *result)
{
    if(!kernel_open_file_readable(file)){*result=-KERNEL_EBADF;return KERNEL_FILES_STATUS_OK;}
    if(scalar&&(kernel_user_range_check(iov[0].base,(size_t)count)||scalar_empty_range_fault(iov[0].base,count))){
        *result=-KERNEL_EFAULT;return KERNEL_FILES_STATUS_OK;
    }
    char buffer[128];size_t n=0,copied=0;
    int error=kernel_mqueue_read(file,offset,buffer,count>sizeof(buffer)?sizeof(buffer):(size_t)count,&n);
    if(error){*result=error;return KERNEL_FILES_STATUS_OK;}
    struct kernel_uaccess_iov_cursor cursor={iov,iov_count,0,0};
    enum kernel_uaccess_status access=kernel_copy_to_user_iov(mm,&cursor,buffer,n,&copied);
    if(advance&&kernel_open_file_advance(file,copied))return KERNEL_FILES_STATUS_STATE;
    *result=copied?(int64_t)copied:access?-KERNEL_EFAULT:0;
    if(access)KERNEL_FILES_NOTE(files,read_failures,1);
    KERNEL_FILES_NOTE(files,bytes_read,copied);return KERNEL_FILES_STATUS_OK;
}

static enum kernel_files_status proc_page_stream_read(struct kernel_files *files, struct kernel_mm *mm,
    struct kernel_open_file_description *description, struct kernel_uaccess_iov_cursor *cursor,
    uint64_t count, uint64_t offset, int advance, int64_t *linux_result)
{
    int pagemap=description->proc_page_stream==KERNEL_PROC_PAGE_STREAM_PAGEMAP;
    struct kernel_mm_pagemap *view=pagemap ? description->proc_pagemap : NULL;
    uint64_t total=0, fault_prefix=0;
    int error = 0;
    if (pagemap && !kernel_mm_pagemap_live(view)) goto Out;
    if (offset % sizeof(uint64_t)) { error = -KERNEL_EINVAL; goto Out; }
    int show_pfn=pagemap && kernel_credentials_capable(description->credentials,KERNEL_CAP_SYS_ADMIN);
    uint64_t entries[KERNEL_MM_PAGEMAP_BATCH];
    /* The native proc files implement scalar reads. Vectored IO retains each
     * preceding completed segment when a later one fails alignment or copying. */
    for (size_t i = 0; i < cursor->count && total < count; ++i) {
        uint64_t length = cursor->iov[i].length;
        if (length > count - total) length = count - total;
        if (length % sizeof(uint64_t)) { error = -KERNEL_EINVAL; break; }
        uint64_t done = 0;
        while (done < length) {
            uint64_t remaining = (length - done) / sizeof(uint64_t);
            unsigned batch = remaining > KERNEL_MM_PAGEMAP_BATCH ? KERNEL_MM_PAGEMAP_BATCH : (unsigned)remaining;
            unsigned produced;
            uint64_t first=(offset+total+done)/sizeof(uint64_t);
            if (pagemap)
                error=kernel_mm_pagemap_read(view,first,batch,show_pfn,entries,&produced);
            else
                produced=physical_page_flags_snapshot(description->heap->page_allocator,first,entries,batch);
            if (error || !produced) break;
            size_t bytes = produced * sizeof(uint64_t), copied;
            if (kernel_copy_to_user(mm, cursor->iov[i].base + done, entries, bytes, &copied)) {
                /* kpageflags advances after each successfully copied record,
                 * even when the scalar read returns EFAULT. */
                if (!pagemap) fault_prefix=done+copied/sizeof(uint64_t)*sizeof(uint64_t);
                error = -KERNEL_EFAULT;
                break;
            }
            done += bytes;
        }
        if (error) break;
        total += done;
        if (done != length) break;
    }
Out:
    uint64_t consumed=total+fault_prefix;
    if (advance && consumed && kernel_open_file_advance(description,consumed))
        return KERNEL_FILES_STATUS_STATE;
    if (error) KERNEL_FILES_NOTE(files, read_failures, 1);
    KERNEL_FILES_NOTE(files, bytes_read, consumed);
    *linux_result = total ? (int64_t)total : error;
    return KERNEL_FILES_STATUS_OK;
}

static enum kernel_files_status generated_read(
    struct kernel_files *files, struct kernel_mm *mm,
    struct kernel_open_file_description *description,
    struct kernel_uaccess_iov_cursor *cursor, uint64_t count,
    uint64_t offset, int advance, int64_t *linux_result)
{
    if (description->proc_page_stream)
        return proc_page_stream_read(files, mm, description, cursor, count, offset, advance, linux_result);
    if (kernel_vfs_file_is_control(&description->file, 0))
        return control_read(files, mm, description, cursor, count, offset, advance, linux_result);
    if (!count) { *linux_result = 0; return KERNEL_FILES_STATUS_OK; }
    if (description->generated_ready && (!offset || offset != description->generated_read_offset))
        kernel_open_file_reset_generated(description);
    int result = kernel_open_file_generate(description);
    if (result) {
        *linux_result = result;
        KERNEL_FILES_NOTE(files, read_failures, 1);
        return KERNEL_FILES_STATUS_OK;
    }
    description->generated_read_offset = offset;
    uint64_t total = 0U;
    if (offset < description->generated_length) {
        uint64_t available = description->generated_length - offset;
        if (count > available) count = available;
    } else count = 0U;
    while (total < count) {
        size_t chunk = (size_t)(count - total);
        size_t copied = 0U;
        if (chunk > BOAROS_PAGE_SIZE) chunk = BOAROS_PAGE_SIZE;
        enum kernel_uaccess_status access = kernel_copy_to_user_iov(
            mm, cursor, description->generated_data + offset + total,
            chunk, &copied);
        if (advance && kernel_open_file_advance(description, copied) !=
                           KERNEL_OPEN_FILE_STATUS_OK)
            return KERNEL_FILES_STATUS_STATE;
        total += copied;
        description->generated_read_offset = offset + total;
        KERNEL_FILES_NOTE(files, read_chunks, 1);
        if (access == KERNEL_UACCESS_STATUS_FAULT) {
            *linux_result = total ? (int64_t)total : -KERNEL_EFAULT;
            if (!total) KERNEL_FILES_NOTE(files, read_failures, 1);
            KERNEL_FILES_NOTE(files, bytes_read, total);
            return KERNEL_FILES_STATUS_OK;
        }
        if (access != KERNEL_UACCESS_STATUS_OK || copied != chunk)
            return KERNEL_FILES_STATUS_STATE;
    }
    KERNEL_FILES_NOTE(files, bytes_read, total);
    *linux_result = (int64_t)total;
    return KERNEL_FILES_STATUS_OK;
}

static enum kernel_files_status memory_read(
    struct kernel_files *files, struct kernel_mm *mm,
    struct kernel_open_file_description *description,
    struct kernel_uaccess_iov_cursor *cursor, uint64_t count,
    uint64_t offset, int advance, int64_t *linux_result)
{
    struct kernel_uaccess_write_cursor output = {0};
    struct kernel_vfs_node *node = kernel_open_file_node(description);
    uint64_t total = 0;
    int error = 0;
    if (offset >= kernel_open_file_size(description)) { *linux_result = 0; return KERNEL_FILES_STATUS_OK; }
    while (total < count) {
        uint64_t destination;
        size_t chunk=kernel_uaccess_iov_span(cursor,(size_t)(count-total),&destination);
        if (!chunk) __builtin_trap();
        uint64_t destination_page=destination & ~(uint64_t)BOAROS_PAGE_MASK;
        if (!output.pin.registration || output.virtual_page!=destination_page) {
            kernel_uaccess_write_cursor_finish(&output);
            enum kernel_uaccess_status status=kernel_uaccess_pin_user_page(mm,destination,KERNEL_MM_WRITE,&output.pin);
            if (status==KERNEL_UACCESS_STATUS_FAULT) { error=-KERNEL_EFAULT;break; }
            if (status) __builtin_trap();
            output.virtual_page=destination_page;
        }
        KERNEL_LOCK_SCOPE(source);
        if (!kernel_vfs_node_try_read(node,&source)) {
            /* Truncate can own this inode while draining a destination pin
             * through an mmap alias. Return the pin before waiting for it. */
            kernel_uaccess_write_cursor_finish(&output);
            kernel_vfs_node_lock(node,&source,0);
            continue;
        }
        uint64_t position=offset+total,size=kernel_open_file_size(description);
        if (position>=size) break;
        size_t within=position & BOAROS_PAGE_MASK;
        size_t available=BOAROS_PAGE_SIZE-within;
        if (available>size-position) available=(size_t)(size-position);
        if (available>count-total) available=(size_t)(count-total);
        uint64_t physical=0;size_t valid=0;
        enum kernel_page_cache_status found=kernel_open_file_lookup_page(description,
            position>>BOAROS_PAGE_SHIFT,&physical,&valid);
        void *page=NULL;
        if (!found) {
            if (within>=valid || available>valid-within || physical_page_map_owned(mm->allocator,physical,&page))
                __builtin_trap();
        } else if (found!=KERNEL_PAGE_CACHE_STATUS_NOT_FOUND)
            error=found==KERNEL_PAGE_CACHE_STATUS_NO_MEMORY?-KERNEL_ENOMEM:-KERNEL_EIO;
        size_t copied=0;
        while (!error && copied<available) {
            chunk=kernel_uaccess_iov_span(cursor,available-copied,&destination);
            if (!chunk) __builtin_trap();
            /* Both page borrows stay valid across same-page iovec pieces.
             * End the source qualification before preparing a new user page. */
            if ((destination & ~(uint64_t)BOAROS_PAGE_MASK)!=output.virtual_page) break;
            void *target=(unsigned char *)output.pin.page+(destination&BOAROS_PAGE_MASK);
            if (page) memmove(target,(unsigned char *)page+within+copied,chunk);
            else memset(target,0,chunk);
            COST_ADD(COPY_TO_USER,chunk);
            cursor->offset+=chunk;copied+=chunk;
            KERNEL_FILES_NOTE(files,read_chunks,1);
        }
        kernel_lock_release(&source);
        if (physical && physical_page_release(mm->allocator,physical)) __builtin_trap();
        if (error) break;
        if (!copied) __builtin_trap();
        if (advance && kernel_open_file_advance(description,copied) != KERNEL_OPEN_FILE_STATUS_OK) __builtin_trap();
        total+=copied;
    }
    kernel_uaccess_write_cursor_finish(&output);
    if (error) KERNEL_FILES_NOTE(files, read_failures, 1);
    KERNEL_FILES_NOTE(files, bytes_read, total);
    *linux_result = total ? (int64_t)total : error;
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_sync_file_range(struct kernel_files *files,
    int64_t fd, int64_t offset, int64_t length, unsigned flags, int64_t *linux_result)
{
    if (!kernel_files_is_live(files) || !linux_result)
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    KERNEL_FILES_PIN_SCOPE(pin);
    struct kernel_open_file_description *file = NULL;
    enum kernel_files_status status = kernel_files_pin_data(files, fd, &file, linux_result);
    if (status || *linux_result)
        return status;
    pin = (struct kernel_files_pin_guard){files, file};
    enum kernel_open_file_kind kind = kernel_open_file_kind(file);
    if (flags & ~KERNEL_SYNC_FILE_RANGE_ALL || offset < 0 || length < 0 ||
        length > INT64_MAX - offset)
        *linux_result = -KERNEL_EINVAL;
    else if (kind == KERNEL_OPEN_FILE_KIND_BLOCK)
        *linux_result = 0; /* Block descriptions do not buffer writes. */
    else if (kind == KERNEL_OPEN_FILE_KIND_MQUEUE)
        *linux_result = 0; /* The regular inode has no data mapping. */
    else if (kind != KERNEL_OPEN_FILE_KIND_REGULAR && kind != KERNEL_OPEN_FILE_KIND_DIRECTORY &&
             kind != KERNEL_OPEN_FILE_KIND_GENERATED)
        *linux_result = -KERNEL_ESPIPE;
    else {
        uint64_t start = (uint64_t)offset & ~(uint64_t)BOAROS_PAGE_MASK;
        uint64_t end = length ? (uint64_t)(offset + length) : (uint64_t)INT64_MAX + 1U;
        end = (end + BOAROS_PAGE_MASK) & ~(uint64_t)BOAROS_PAGE_MASK;
        *linux_result = kernel_vfs_sync_file_range(&file->file, start, end, flags,
            &file->observed_writeback_error);
    }
    return KERNEL_FILES_STATUS_OK;
}

static enum kernel_files_status sync_pinned(struct kernel_open_file_description *description,
    int datasync,int64_t *linux_result,const struct kernel_lock_guard *borrowed)
{
    if (kernel_open_file_io_requires_cpu0(description) && !borrowed && !regular_caller_ready())
        return KERNEL_FILES_STATUS_STATE;
    enum kernel_open_file_kind kind=kernel_open_file_kind(description);
    *linux_result=kind==KERNEL_OPEN_FILE_KIND_BLOCK ? kernel_block_file_sync(description) :
        kind==KERNEL_OPEN_FILE_KIND_REGULAR || kind==KERNEL_OPEN_FILE_KIND_DIRECTORY ?
        kernel_vfs_sync_borrowing(&description->file,datasync,
            &description->observed_writeback_error,borrowed):-KERNEL_EINVAL;
    return KERNEL_FILES_STATUS_OK;
}
enum kernel_files_status kernel_files_sync(struct kernel_files *files,
    int64_t fd, int datasync, int64_t *linux_result)
{
    struct kernel_open_file_description *description = 0;
    if (!kernel_files_is_live(files) || linux_result == 0)
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    enum kernel_files_status status = kernel_files_pin_data(files, fd, &description,
                                                       linux_result);
    if (status != KERNEL_FILES_STATUS_OK || *linux_result != 0) return status;
    status=sync_pinned(description,datasync,linux_result,NULL);
    return kernel_files_release_io(files,&description,status);
}

enum kernel_files_status kernel_files_syncfs(struct kernel_files *files,
    int64_t fd, int64_t *linux_result)
{
    KERNEL_SERVICE_SCOPE(service);
    struct kernel_open_file_description *description = 0;
    if (!kernel_files_is_live(files) || !linux_result)
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    enum kernel_files_status status = kernel_files_pin_data(files, fd, &description, linux_result);
    if (status != KERNEL_FILES_STATUS_OK || *linux_result) return status;
    if (description->file.mount)
        *linux_result = kernel_vfs_sync_mount(description->file.mount,
                                               &description->observed_mount_error);
    else {
        /* 匿名pipe/socket/epoll及无路径console没有持久化后端。 */
        enum kernel_open_file_kind kind = kernel_open_file_kind(description);
        *linux_result = kind == KERNEL_OPEN_FILE_KIND_PIPE || kind == KERNEL_OPEN_FILE_KIND_SOCKET ||
                        kind == KERNEL_OPEN_FILE_KIND_EPOLL || kind == KERNEL_OPEN_FILE_KIND_CONSOLE
                            ? 0 : -KERNEL_ENOTSUP;
    }
    return kernel_files_release_io(files, &description, KERNEL_FILES_STATUS_OK);
}

static enum kernel_files_status direct_read(struct kernel_files *files,struct kernel_mm *mm,
    struct kernel_open_file_description *description,struct kernel_uaccess_iov_cursor *cursor,
    uint64_t request,uint64_t position,int advance,int64_t *result)
{
    size_t align=kernel_vfs_direct_alignment(&description->file);
    if (position>=kernel_open_file_size(description)) { *result=0;return 0; }
    if (!align || position%align || request%align) { *result=-KERNEL_EINVAL;return 0; }
    struct kernel_uaccess_iov_cursor check=*cursor;
    uint64_t left=request;
    while (left) {
        uint64_t address;size_t n=kernel_uaccess_iov_span(&check,left,&address);
        if (!n || address%align || n%align) { *result=-KERNEL_EINVAL;return 0; }
        check.offset+=n;left-=n;
    }
    struct kernel_task_io_buffer staging={0};
    if (kernel_task_io_buffer_acquire(&staging,mm->allocator)) { *result=-KERNEL_ENOMEM;return 0; }
    uint64_t total=0;int error=0;
    while (total<request) {
        size_t n=request-total,done=0,copied=0;
        if (n>BOAROS_PAGE_SIZE) n=BOAROS_PAGE_SIZE;
        error=kernel_vfs_direct_pread(&description->file,position+total,staging.data,n,&done);
        enum kernel_uaccess_status access=KERNEL_UACCESS_STATUS_OK;
        if (done) access=kernel_copy_to_user_iov(mm,cursor,staging.data,done,&copied);
        total+=copied;
        if (advance && copied && kernel_open_file_advance(description,copied)) __builtin_trap();
        KERNEL_FILES_NOTE(files,read_chunks,1);
        if (access) { error=-KERNEL_EFAULT;break; }
        if (error || done<n) break;
    }
    kernel_task_io_buffer_release(&staging);
    KERNEL_FILES_NOTE(files,bytes_read,total);
    *result=total?(int64_t)total:error;
    return 0;
}

/* 缓存批次只覆盖剩余需求；复制前已经结束存储锁，整个批次的成功页都有独立引用。 */
static enum kernel_files_status disk_read(
    struct kernel_files *files, struct kernel_mm *mm,
    struct kernel_open_file_description *description,
    struct kernel_uaccess_iov_cursor *cursor, uint64_t request,
    uint64_t position, int advance, int64_t *linux_result
    )
{
    if (kernel_open_file_flags(description)&KERNEL_FILES_O_DIRECT)
        return direct_read(files,mm,description,cursor,request,position,advance,linux_result);
    struct kernel_page_cache *cache=kernel_open_file_page_cache(description);
    struct kernel_vfs_node *node=kernel_open_file_node(description);
    uint64_t total = 0;
    int error = 0, done = 0;
    enum kernel_files_status status = KERNEL_FILES_STATUS_OK;
    size_t chunks=0;
    int no_reuse=kernel_open_file_read_no_reuse(description);
    while (total < request && !done) {
        /* As in kernel_vfs_pread, this snapshot only sizes the demand. The
         * cache still revalidates every requested page against live EOF. */
        uint64_t end=kernel_vfs_node_size(node);
        if (position>=end) break;
        uint64_t remaining=request-total;
        if (remaining>end-position) remaining=end-position;
        uint64_t batch_start=position;
        int cache_miss=0;
        size_t start = position & BOAROS_PAGE_MASK;
        size_t count = (start + remaining + BOAROS_PAGE_MASK) >> BOAROS_PAGE_SHIFT;
        if (count > KERNEL_PAGE_CACHE_READ_BATCH_MAX) count = KERNEL_PAGE_CACHE_READ_BATCH_MAX;
        struct kernel_page_cache_read_page pages[KERNEL_PAGE_CACHE_READ_BATCH_MAX];
        enum kernel_page_cache_status loaded;
        loaded = kernel_page_cache_get_batch(
            cache, &description->file,
            position >> BOAROS_PAGE_SHIFT, pages, count, no_reuse);
        if (loaded == KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT) {
            status=KERNEL_FILES_STATUS_STATE;
            break;
        }
        struct kernel_uaccess_write_cursor user_copy={0};
        for (size_t i = 0; i < count; i++) {
            struct kernel_page_cache_read_page *part = &pages[i];
            chunks++;
            if (part->status == KERNEL_PAGE_CACHE_STATUS_OUT_OF_RANGE ||
                (!part->status && start >= part->valid_bytes)) { done = 1; break; }
            if (part->status) {
                error = part->status == KERNEL_PAGE_CACHE_STATUS_NO_MEMORY ? -KERNEL_ENOMEM : -KERNEL_EIO;
                done = 1; break;
            }
            const unsigned char *page=kernel_page_cache_read_data(part);
            size_t chunk = part->valid_bytes - start, copied = 0;
            if (chunk > request - total) chunk = request - total;
            /* One scalar span has no later page/iovec within this window
             * that could reuse a retained destination pin. */
            enum kernel_uaccess_status access = count==1 && cursor->count==1 ?
                kernel_copy_to_user_iov(mm,cursor,page+start,chunk,&copied) :
                kernel_copy_to_user_iov_cursor(mm,cursor,&user_copy,page+start,chunk,&copied);
            /* A later user page can fault and the task may be reaped while
             * blocked. Keep the copied prefix visible to other FD owners. */
            if (copied > chunk || (advance && kernel_open_file_advance(description,copied))) {
                status = KERNEL_FILES_STATUS_STATE; done = 1; break;
            }
#if BOAROS_PAGE_CACHE_READAHEAD_PAGES
            if (copied && part->cache_miss) cache_miss=1;
#endif
            total += copied; position += copied;
            if (access == KERNEL_UACCESS_STATUS_FAULT) {
                error = -KERNEL_EFAULT; done = 1; break;
            }
            if (access || copied != chunk) {
                status = KERNEL_FILES_STATUS_STATE; done = 1; break;
            }
            if (part->valid_bytes < BOAROS_PAGE_SIZE) { done = 1; break; }
            start = 0;
        }
        /* 预测以整个需求批次的成功前缀为单位，避免将大读误作逐页小读。 */
        kernel_open_file_read_progress(description,batch_start,(size_t)(position-batch_start),cache_miss);
        /* 包括错误或user fault之后未消费的成功页；不能靠下一次请求归还。 */
        kernel_uaccess_write_cursor_finish(&user_copy);
        kernel_page_cache_put_batch(cache, pages, count);
    }
    if (chunks) KERNEL_FILES_NOTE(files,read_chunks,chunks);
    if (error) KERNEL_FILES_NOTE(files, read_failures, 1);
    KERNEL_FILES_NOTE(files, bytes_read, total);
    *linux_result = total ? (int64_t)total : error;
    return status;
}

static enum kernel_files_status read_pinned(
    struct kernel_files *files,
    struct kernel_mm *mm,
    struct kernel_open_file_description **description_owner,
    const struct kernel_uaccess_iovec *iov,
    size_t iov_count,
    uint64_t count, int scalar,
    int64_t *linux_result, uint32_t socket_flags,
    struct kernel_socket_address *peer, uint32_t *message_size, struct kernel_socket_message *socket_message, const struct kernel_lock_guard *borrowed_offset)
{
    struct kernel_open_file_description *description = *description_owner;
    if (description->block) {
        if (scalar && kernel_open_file_readable(description) &&
            (kernel_user_range_check(iov[0].base,count) || scalar_empty_range_fault(iov[0].base,count))) {
            *linux_result=-KERNEL_EFAULT;
            KERNEL_FILES_NOTE(files,read_failures,1);
            return KERNEL_FILES_STATUS_OK;
        }
        enum kernel_files_status status = kernel_block_file_io(description, mm, iov, iov_count,
            count, 0, 0, 0, linux_result);
        if (!status) {
            if (*linux_result < 0) KERNEL_FILES_NOTE(files, read_failures, 1);
            else KERNEL_FILES_NOTE(files, bytes_read, (uint64_t)*linux_result);
        }
        return status;
    }
    const enum kernel_open_file_kind kind = kernel_open_file_kind(description);
    if(kind==KERNEL_OPEN_FILE_KIND_MQUEUE){
        KERNEL_LOCK_SCOPE(guard);kernel_mutex_lock(&description->offset_lock,&guard);
        return mqueue_read_text(files,mm,description,iov,iov_count,count,
            kernel_open_file_offset(description),1,scalar,linux_result);
    }
    if(kind==KERNEL_OPEN_FILE_KIND_PIDFD) {
        *linux_result=-KERNEL_EINVAL;return KERNEL_FILES_STATUS_OK;
    }
    if (kind==KERNEL_OPEN_FILE_KIND_NAMESPACE) {
        *linux_result=(kernel_open_file_flags(description)&010000000U)?-KERNEL_EBADF:-KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kind==KERNEL_OPEN_FILE_KIND_SIGNALFD) {
        *linux_result=kernel_signal_fd_read(description,mm,iov,iov_count,count);
        if (*linux_result<0) KERNEL_FILES_NOTE(files,read_failures,1);
        else KERNEL_FILES_NOTE(files,bytes_read,(uint64_t)*linux_result);
        return KERNEL_FILES_STATUS_OK;
    }
    if ((kind==KERNEL_OPEN_FILE_KIND_EVENTFD || kind==KERNEL_OPEN_FILE_KIND_TIMERFD)) {
        *linux_result=kernel_event_fd_io(description,mm,iov,iov_count,count,0);
        if (*linux_result<0) KERNEL_FILES_NOTE(files,read_failures,1);
        else KERNEL_FILES_NOTE(files,bytes_read,(uint64_t)*linux_result);
        return KERNEL_FILES_STATUS_OK;
    }
    /* 固定本次调用的等待资格；后续fcntl只影响下一次读写。 */
    if (kind == KERNEL_OPEN_FILE_KIND_SOCKET &&
        (kernel_open_file_flags(description) & KERNEL_FILES_O_NONBLOCK))
        socket_flags |= KERNEL_SOCKET_MSG_DONTWAIT;
    KERNEL_SERVICE_SCOPE_IF(service, !description->proc_page_stream && kind != KERNEL_OPEN_FILE_KIND_INOTIFY &&
        kind != KERNEL_OPEN_FILE_KIND_REGULAR && kind != KERNEL_OPEN_FILE_KIND_SOCKET && kind != KERNEL_OPEN_FILE_KIND_PIPE);
    KERNEL_LOCK_SCOPE(offset_guard);
    if ((kind == KERNEL_OPEN_FILE_KIND_REGULAR || kind == KERNEL_OPEN_FILE_KIND_GENERATED) &&
        !regular_borrows_offset(description,borrowed_offset))
        kernel_mutex_lock(&description->offset_lock, &offset_guard);
    uint64_t request;
    uint64_t total = 0U;
    struct kernel_uaccess_iov_cursor cursor = {iov, iov_count, 0U, 0U};

    if (kind == KERNEL_OPEN_FILE_KIND_EPOLL) {
        *linux_result = -KERNEL_EINVAL;
        KERNEL_FILES_NOTE(files, read_failures, 1);
        return KERNEL_FILES_STATUS_OK;
    }
    if (!kernel_open_file_readable(description)) {
        *linux_result = -KERNEL_EBADF;
        KERNEL_FILES_NOTE(files, read_failures, 1);
        return KERNEL_FILES_STATUS_OK;
    }
    if (kind == KERNEL_OPEN_FILE_KIND_SOCKET) {
        if ((socket_flags & KERNEL_SOCKET_MSG_PEEK) && !socket_message) __builtin_trap();
        struct kernel_socket_read_request read_request = {
            .waitall = (socket_flags & KERNEL_SOCKET_MSG_WAITALL) &&
                kernel_socket_is_tcp(kernel_open_file_socket(description))
        };
        int received = 0;
        uint64_t target = 0;
        int timeout_captured = 0;
        enum kernel_files_status result = KERNEL_FILES_STATUS_OK;
        if (scalar && kernel_user_range_check(iov[0].base,count)) {
            *linux_result=-KERNEL_EFAULT;
            KERNEL_FILES_NOTE(files,read_failures,1);
            return KERNEL_FILES_STATUS_OK;
        }
        int message = (socket_flags & KERNEL_SOCKET_IO_MESSAGE) != 0;
        /* read(0)可直接返回；recv(0)仍须检查协议状态和等待条件。 */
        int discard = kernel_socket_discard_receive(kernel_open_file_socket(description),socket_flags);
        if (count == 0U && !message) {
            *linux_result = 0;
            return KERNEL_FILES_STATUS_OK;
        }
        if (read_request.waitall && !(socket_flags & KERNEL_SOCKET_MSG_DONTWAIT)) {
            uint64_t timeout = kernel_socket_receive_timeout(kernel_open_file_socket(description));
            if (timeout) {
                uint64_t now = kernel_time_monotonic_ns();
                target = UINT64_MAX - now < timeout ? UINT64_MAX : now + timeout;
            }
            timeout_captured = 1;
        }
        while (total < count || (message && total == 0)) {
            uint64_t remaining = count - total;
            received = kernel_socket_reserve_read(
                kernel_open_file_socket(description), kernel_task_current(),
                &read_request, description_owner,
                remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining,
                socket_flags & KERNEL_SOCKET_MSG_DONTWAIT, socket_message);
            if (received == -KERNEL_EAGAIN && (total == 0U || read_request.waitall) && !(socket_flags & KERNEL_SOCKET_MSG_DONTWAIT)) {
                /* ready后数据仍可能被另一reader取走，本次调用只取得一次预算。 */
                if (!timeout_captured) {
                    uint64_t timeout = kernel_socket_receive_timeout(kernel_open_file_socket(description));
                    if (timeout) {
                        uint64_t now = kernel_time_monotonic_ns();
                        target = UINT64_MAX - now < timeout ? UINT64_MAX : now + timeout;
                    }
                    timeout_captured = 1;
                }
                int waited = target && kernel_time_monotonic_ns() >= target ? -KERNEL_EAGAIN :
                    kernel_socket_wait_read(kernel_open_file_socket(description), &read_request, target, socket_flags);
                if (waited == 0) continue;
                received = waited;
            }
            if (received < 0 || read_request.socket == 0) break;
            int datagram = read_request.datagram;
            if (datagram || (peer && kernel_socket_is_unix(read_request.socket)))
                kernel_socket_read_info(&read_request, peer, datagram ? message_size : 0);
            int fault = !discard && kernel_socket_read_user(&read_request, mm, &cursor);
            KERNEL_FILES_NOTE(files, read_chunks, 1);
            total += kernel_socket_finish_read(&read_request, fault);
            if (fault) { received = -KERNEL_EFAULT; break; }
            /* Ordinary reads stop at the ready prefix. WAITALL releases the
             * reservation and receive credit before waiting for more data. */
            if (!read_request.waitall || !received) break;
        }
        *linux_result = total != 0U ? (int64_t)total : received;
        if (*linux_result < 0) KERNEL_FILES_NOTE(files, read_failures, 1);
        else KERNEL_FILES_NOTE(files, bytes_read, total);
        return result;
    }
    if (description->device) {
        const struct kernel_char_device *device = description->device;
        if (scalar && device->empty_range_fault && iov_count == 1U && count == 0U &&
            iov[0].base != 0U &&
            kernel_user_range_check(iov[0].base - 1U, 1U) !=
                KERNEL_UACCESS_STATUS_OK) {
            *linux_result = -KERNEL_EFAULT;
            KERNEL_FILES_NOTE(files, read_failures, 1);
            return KERNEL_FILES_STATUS_OK;
        }
        if (scalar && kernel_user_range_check(iov[0].base,count)) {
            *linux_result=-KERNEL_EFAULT;
            KERNEL_FILES_NOTE(files,read_failures,1);
            return KERNEL_FILES_STATUS_OK;
        }
        if (device->readv) {
            enum kernel_files_status status = device->readv(description->device_instance,
                kernel_task_current(), files, description_owner, 0, mm, iov, iov_count,
                count > KERNEL_FILES_MAX_RW_COUNT ? KERNEL_FILES_MAX_RW_COUNT : count,
                kernel_open_file_flags(description), linux_result);
            if (*linux_result < 0) KERNEL_FILES_NOTE(files, read_failures, 1);
            else KERNEL_FILES_NOTE(files, bytes_read, (uint64_t)*linux_result);
            return status;
        }
        unsigned char staging[64];
        request = count > KERNEL_FILES_MAX_RW_COUNT
                      ? KERNEL_FILES_MAX_RW_COUNT : count;
        while (total < request || (device->read_once && !total)) {
            if (device->interruptible_bulk && total >= 256U &&
                kernel_signal_has_pending(kernel_task_current())) break;
            size_t received = 0U;
            size_t copied = 0U;
            size_t chunk = request - total < sizeof(staging)
                               ? (size_t)(request - total) : sizeof(staging);
            int result = device->read(description->device_instance, kernel_task_current(), kernel_open_file_flags(description), staging,
                                      chunk, &received);
            if (result) {
                *linux_result = total ? (int64_t)total : result;
                if (!total) KERNEL_FILES_NOTE(files, read_failures, 1);
                KERNEL_FILES_NOTE(files, bytes_read, total);
                return KERNEL_FILES_STATUS_OK;
            }
            if (received > chunk) return KERNEL_FILES_STATUS_STATE;
            if (!received) break;
            enum kernel_uaccess_status access = kernel_copy_to_user_iov(
                mm, &cursor, staging, received, &copied);
            total += copied;
            KERNEL_FILES_NOTE(files, read_chunks, 1);
            if (access == KERNEL_UACCESS_STATUS_FAULT) {
                *linux_result = total ? (int64_t)total : -KERNEL_EFAULT;
                KERNEL_FILES_NOTE(files, bytes_read, total);
                if (total == 0U) KERNEL_FILES_NOTE(files, read_failures, 1);
                return KERNEL_FILES_STATUS_OK;
            }
            if (access != KERNEL_UACCESS_STATUS_OK || copied != received)
                return KERNEL_FILES_STATUS_STATE;
            if (device->read_once || received < chunk) break;
        }
        KERNEL_FILES_NOTE(files, bytes_read, total);
        *linux_result = (int64_t)total;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kind == KERNEL_OPEN_FILE_KIND_PIPE) {
        if (scalar && kernel_user_range_check(iov[0].base,count)) {
            *linux_result=-KERNEL_EFAULT;
            KERNEL_FILES_NOTE(files,read_failures,1);
            return KERNEL_FILES_STATUS_OK;
        }
        enum kernel_pipe_status pipe_status = kernel_pipe_readv(
            description->pipe,
            mm,
            iov, iov_count,
            count,
            kernel_open_file_flags(description),
            linux_result);

        if (pipe_status != KERNEL_PIPE_STATUS_OK || kernel_files_pipe_result(*linux_result,0)) {
            return KERNEL_FILES_STATUS_STATE;
        }
        if (*linux_result > 0 && description->file.private_data)
            kernel_vfs_file_accessed(&description->file,kernel_open_file_flags(description));
        if (*linux_result >= 0) {
            KERNEL_FILES_NOTE(files, bytes_read, (uint64_t)*linux_result);
        } else {
            KERNEL_FILES_NOTE(files, read_failures, 1);
        }
        return KERNEL_FILES_STATUS_OK;
    }
    if (kind == KERNEL_OPEN_FILE_KIND_DIRECTORY) {
        KERNEL_FILES_NOTE(files, read_failures, 1);
        *linux_result = -KERNEL_EISDIR;
        return KERNEL_FILES_STATUS_OK;
    }
    if ((kind == KERNEL_OPEN_FILE_KIND_REGULAR || kind == KERNEL_OPEN_FILE_KIND_GENERATED) &&
        !kernel_open_file_readable(description)) {
        KERNEL_FILES_NOTE(files, read_failures, 1);
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    if (scalar && (kernel_user_range_check(iov[0].base,count) ||
        scalar_empty_range_fault(iov[0].base,count))) {
        KERNEL_FILES_NOTE(files,read_failures,1);
        *linux_result=-KERNEL_EFAULT;
        return KERNEL_FILES_STATUS_OK;
    }
    if (count == 0U && !description->proc_page_stream) {
        *linux_result = 0;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kind == KERNEL_OPEN_FILE_KIND_GENERATED) {
        request = count > KERNEL_FILES_MAX_RW_COUNT
                      ? KERNEL_FILES_MAX_RW_COUNT : count;
        return generated_read(files, mm, description, &cursor, request,
                              kernel_open_file_offset(description), 1,
                              linux_result);
    }
    if (kind == KERNEL_OPEN_FILE_KIND_REGULAR) {
        kernel_open_file_read_begin(description, kernel_open_file_offset(description));
        kernel_vfs_file_accessed(&description->file,kernel_open_file_flags(description));
    }
    request = count > KERNEL_FILES_MAX_RW_COUNT
                  ? KERNEL_FILES_MAX_RW_COUNT
                  : count;
    if (kernel_open_file_memory_backed(description))
        return memory_read(files, mm, description, &cursor, request,
                           kernel_open_file_offset(description), 1, linux_result);
    return disk_read(files, mm, description, &cursor, request,
                     kernel_open_file_offset(description), 1, linux_result
                     );
}

enum kernel_files_status kernel_files_read(
    struct kernel_files *files,
    struct kernel_mm *mm,
    int64_t fd,
    uint64_t user_buffer,
    uint64_t count,
    int64_t *linux_result)
{
    struct kernel_uaccess_iovec iov = {user_buffer, count};
    struct kernel_open_file_description *description = 0;
    enum kernel_files_status status;

    if (!kernel_files_is_live(files) || mm == 0 || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    KERNEL_FILES_NOTE(files, read_calls, 1);
    status = kernel_files_pin_data(files, fd, &description, linux_result);
    if (status != KERNEL_FILES_STATUS_OK) {
        return status;
    }
    if (*linux_result != 0) {
        KERNEL_FILES_NOTE(files, read_failures, 1);
        return KERNEL_FILES_STATUS_OK;
    }
    if ((description->kind==KERNEL_OPEN_FILE_KIND_EVENTFD || description->kind==KERNEL_OPEN_FILE_KIND_TIMERFD || description->kind==KERNEL_OPEN_FILE_KIND_SIGNALFD) &&
        (kernel_user_range_check(user_buffer,(size_t)count) || scalar_empty_range_fault(user_buffer,count))) {
        *linux_result=-KERNEL_EFAULT;KERNEL_FILES_NOTE(files,read_failures,1);
        return kernel_files_release_io(files,&description,KERNEL_FILES_STATUS_OK);
    }
    status = read_pinned(files, mm, &description, &iov, 1U, count, 1,
                         linux_result, 0U, 0, 0, NULL, NULL);
    if (!status && *linux_result>0 && description)
        kernel_vfs_notify_file(&description->file,VFS_NOTIFY_ACCESS);
    return kernel_files_release_io(files, &description, status);
}

static enum kernel_files_status pread_iov_pinned(
    struct kernel_files *files, struct kernel_mm *mm,
    struct kernel_open_file_description **description_owner,
    const struct kernel_uaccess_iovec *iov, size_t iov_count,
    uint64_t count, int64_t offset, int scalar, int64_t *linux_result);

static int vector_request_error(struct kernel_open_file_description *file,
    uint64_t position,uint64_t count,uint32_t flags)
{
    if ((file->kind==KERNEL_OPEN_FILE_KIND_REGULAR || file->kind==KERNEL_OPEN_FILE_KIND_GENERATED) &&
        (position>INT64_MAX || count>INT64_MAX-position)) return -KERNEL_EINVAL;
    if (flags&~UINT32_C(0x1ff)) return -KERNEL_EOPNOTSUPP;
    if ((flags&(KERNEL_RWF_APPEND|KERNEL_RWF_NOAPPEND))==
        (KERNEL_RWF_APPEND|KERNEL_RWF_NOAPPEND)) return -KERNEL_EINVAL;
    if (flags&(KERNEL_RWF_NOWAIT|KERNEL_RWF_ATOMIC|KERNEL_RWF_DONTCACHE))
        return -KERNEL_EOPNOTSUPP;
    if ((flags&KERNEL_RWF_NOAPPEND) &&
        (kernel_open_file_flags(file)&KERNEL_FILES_O_APPEND) &&
        file->kind==KERNEL_OPEN_FILE_KIND_REGULAR && !kernel_vfs_file_noappend_allowed(&file->file))
        return -KERNEL_EPERM;
    return 0;
}

static enum kernel_files_status readv_request(
    struct kernel_files *files,
    struct kernel_mm *mm,
    int64_t fd,
    uint64_t user_iov,
    uint64_t iovcnt,
    int positioned, int64_t offset, uint32_t flags, int64_t *linux_result)
{
    KERNEL_LOCK_SCOPE(offset_guard);
    struct kernel_uaccess_iov_snapshot snapshot = {0};
    struct kernel_open_file_description *description = 0;
    enum kernel_files_status status;
    int dispatched = 0;

    if (!kernel_files_is_live(files) || mm == 0 || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    KERNEL_FILES_NOTE(files, read_calls, 1);
    status = kernel_files_pin_data(files, fd, &description, linux_result);
    if (status != KERNEL_FILES_STATUS_OK) {
        return status;
    }
    if (*linux_result != 0) {
        KERNEL_FILES_NOTE(files, read_failures, 1);
        return KERNEL_FILES_STATUS_OK;
    }
    KERNEL_SERVICE_SCOPE_IF(service, description->kind != KERNEL_OPEN_FILE_KIND_INOTIFY && kernel_open_file_kind(description) != KERNEL_OPEN_FILE_KIND_REGULAR && kernel_open_file_kind(description) != KERNEL_OPEN_FILE_KIND_SOCKET && kernel_open_file_kind(description) != KERNEL_OPEN_FILE_KIND_PIPE && kernel_open_file_kind(description) != KERNEL_OPEN_FILE_KIND_EVENTFD && kernel_open_file_kind(description) != KERNEL_OPEN_FILE_KIND_TIMERFD && kernel_open_file_kind(description) != KERNEL_OPEN_FILE_KIND_SIGNALFD && kernel_open_file_kind(description) != KERNEL_OPEN_FILE_KIND_MQUEUE);
    if (description->kind==KERNEL_OPEN_FILE_KIND_NAMESPACE) {
        *linux_result=(kernel_open_file_flags(description)&010000000U)?-KERNEL_EBADF:-KERNEL_EINVAL;
        status=KERNEL_FILES_STATUS_OK;
        goto out;
    }
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_EPOLL ||
        kernel_open_file_kind(description)==KERNEL_OPEN_FILE_KIND_PIDFD) {
        *linux_result = positioned && kernel_open_file_kind(description)!=KERNEL_OPEN_FILE_KIND_PIDFD
            ? -KERNEL_ESPIPE : -KERNEL_EINVAL;
        status = KERNEL_FILES_STATUS_OK;
        goto out;
    }
    if (positioned && ((description->device && !description->device->positioned) ||
        (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_EVENTFD || kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_TIMERFD || kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_SIGNALFD) ||
        kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_PIPE ||
        kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_SOCKET)) {
        *linux_result = -KERNEL_ESPIPE;
        goto out;
    }
    if (!kernel_open_file_readable(description)) {
        *linux_result = -KERNEL_EBADF;
        status = KERNEL_FILES_STATUS_OK;
        goto out;
    }
    if (iovcnt > 1024U) {
        *linux_result = -KERNEL_EINVAL;
        status = KERNEL_FILES_STATUS_OK;
        goto out;
    }
    status=kernel_uaccess_iov_import(&snapshot,files->heap,mm,user_iov,iovcnt,linux_result);
    if (status) goto out;
    if (*linux_result) goto out;
    if (snapshot.total == 0U) {
        *linux_result = 0;
        status = KERNEL_FILES_STATUS_OK;
        goto out;
    }
    if (!positioned && (description->kind==KERNEL_OPEN_FILE_KIND_REGULAR ||
        description->kind==KERNEL_OPEN_FILE_KIND_GENERATED))
        kernel_mutex_lock(&description->offset_lock,&offset_guard);
    *linux_result=vector_request_error(description,positioned?(uint64_t)offset:kernel_open_file_offset(description),
        snapshot.total,flags);
    if (*linux_result) goto out;
    if (positioned) {
        status = pread_iov_pinned(files, mm, &description, snapshot.view, snapshot.count,
                                  snapshot.total, offset, 0, linux_result);
    } else if (description->device && description->device->readv) {
        status = description->device->readv(description->device_instance,
            kernel_task_current(), files, &description,
            &snapshot.allocation, mm, snapshot.view, snapshot.count,
            snapshot.total, kernel_open_file_flags(description), linux_result);
        if (*linux_result < 0) KERNEL_FILES_NOTE(files, read_failures, 1);
        else KERNEL_FILES_NOTE(files, bytes_read, (uint64_t)*linux_result);
    } else status = read_pinned(files, mm, &description,
                         snapshot.view, snapshot.count,
                         snapshot.total, 0, linux_result, 0U, 0, 0, NULL,offset_guard.lock?&offset_guard:NULL);
    dispatched = 1;
    if (!status && *linux_result>0 && description)
        kernel_vfs_notify_file(&description->file,VFS_NOTIFY_ACCESS);
out:
    if (offset_guard.lock) kernel_lock_release(&offset_guard);
    kernel_uaccess_iov_cleanup(&snapshot,files->heap);
    if (!dispatched && status == KERNEL_FILES_STATUS_OK &&
        *linux_result < 0) {
        KERNEL_FILES_NOTE(files, read_failures, 1);
    }
    return kernel_files_release_io(files, &description, status);
}


enum kernel_files_status kernel_files_readv(struct kernel_files *files,
    struct kernel_mm *mm, int64_t fd, uint64_t user_iov, uint64_t iovcnt,
    int64_t *linux_result)
{
    return readv_request(files, mm, fd, user_iov, iovcnt, 0, 0, 0, linux_result);
}

enum kernel_files_status kernel_files_preadv(struct kernel_files *files,
    struct kernel_mm *mm, int64_t fd, uint64_t user_iov, uint64_t iovcnt,
    int64_t offset, int64_t *linux_result)
{
    return readv_request(files, mm, fd, user_iov, iovcnt, 1, offset, 0, linux_result);
}

enum kernel_files_status kernel_files_preadv2(struct kernel_files *files,struct kernel_mm *mm,
    int64_t fd,uint64_t iov,uint64_t count,int64_t offset,uint32_t flags,int64_t *result)
{ return readv_request(files,mm,fd,iov,count,offset!=-1,offset,flags,result); }

static enum kernel_files_status pread_iov_pinned(
    struct kernel_files *files,
    struct kernel_mm *mm,
    struct kernel_open_file_description **description_owner,
    const struct kernel_uaccess_iovec *iov, size_t iov_count,
    uint64_t count,
    int64_t offset, int scalar,
    int64_t *linux_result)
{
    struct kernel_open_file_description *description = *description_owner;
    /* pin覆盖本次请求；kind只在OFD发布前设置。 */
    const enum kernel_open_file_kind kind = description->kind;
    if(kind==KERNEL_OPEN_FILE_KIND_MQUEUE){
        if(offset<0){*linux_result=-KERNEL_EINVAL;return KERNEL_FILES_STATUS_OK;}
        return mqueue_read_text(files,mm,description,iov,iov_count,count,(uint64_t)offset,0,scalar,linux_result);
    }
    if (description->block) {
        if (offset < 0) { *linux_result = -KERNEL_EINVAL; return KERNEL_FILES_STATUS_OK; }
        if (scalar && kernel_open_file_readable(description) &&
            (kernel_user_range_check(iov[0].base,count) || scalar_empty_range_fault(iov[0].base,count))) {
            *linux_result=-KERNEL_EFAULT;
            KERNEL_FILES_NOTE(files,read_failures,1);
            return KERNEL_FILES_STATUS_OK;
        }
        enum kernel_files_status status = kernel_block_file_io(description, mm, iov, iov_count,
            count, 0, 1, offset, linux_result);
        if (!status) {
            if (*linux_result < 0) KERNEL_FILES_NOTE(files, read_failures, 1);
            else KERNEL_FILES_NOTE(files, bytes_read, (uint64_t)*linux_result);
        }
        return status;
    }
    KERNEL_SERVICE_SCOPE_IF(service, !description->proc_page_stream && kind != KERNEL_OPEN_FILE_KIND_REGULAR &&
        kind != KERNEL_OPEN_FILE_KIND_SOCKET && kind != KERNEL_OPEN_FILE_KIND_PIPE &&
        kind != KERNEL_OPEN_FILE_KIND_EVENTFD && kind != KERNEL_OPEN_FILE_KIND_TIMERFD &&
        kind != KERNEL_OPEN_FILE_KIND_SIGNALFD && kind != KERNEL_OPEN_FILE_KIND_MQUEUE);
    uint64_t position;
    uint64_t request;
    uint64_t file_size;

    if(kind==KERNEL_OPEN_FILE_KIND_PIDFD) {
        *linux_result=-KERNEL_EINVAL;return KERNEL_FILES_STATUS_OK;
    }
    if (kind == KERNEL_OPEN_FILE_KIND_EPOLL) {
        KERNEL_FILES_NOTE(files, read_failures, 1);
        *linux_result = -KERNEL_ESPIPE;
        return KERNEL_FILES_STATUS_OK;
    }
    if ((kind == KERNEL_OPEN_FILE_KIND_REGULAR ||
         kind == KERNEL_OPEN_FILE_KIND_GENERATED) &&
        !kernel_open_file_readable(description)) {
        KERNEL_FILES_NOTE(files, read_failures, 1);
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    if (offset < 0) {
        KERNEL_FILES_NOTE(files, read_failures, 1);
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kind ==
            KERNEL_OPEN_FILE_KIND_DIRECTORY) {
        KERNEL_FILES_NOTE(files, read_failures, 1);
        *linux_result = -KERNEL_EISDIR;
        return KERNEL_FILES_STATUS_OK;
    }
    if ((description->device && !description->device->positioned) ||
        (kind == KERNEL_OPEN_FILE_KIND_EVENTFD || kind == KERNEL_OPEN_FILE_KIND_TIMERFD || kind == KERNEL_OPEN_FILE_KIND_SIGNALFD) ||
        kind == KERNEL_OPEN_FILE_KIND_PIPE ||
        kind == KERNEL_OPEN_FILE_KIND_SOCKET) {
        KERNEL_FILES_NOTE(files, read_failures, 1);
        *linux_result = -KERNEL_ESPIPE;
        return KERNEL_FILES_STATUS_OK;
    }
    if (scalar && (kernel_user_range_check(iov[0].base, (size_t)count) !=
            KERNEL_UACCESS_STATUS_OK || scalar_empty_range_fault(iov[0].base, count))) {
        *linux_result = -KERNEL_EFAULT;
        KERNEL_FILES_NOTE(files, read_failures, 1);
        return KERNEL_FILES_STATUS_OK;
    }
    if (count == 0U && !description->proc_page_stream) {
        *linux_result = 0;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kind == KERNEL_OPEN_FILE_KIND_GENERATED) {
        KERNEL_LOCK_SCOPE(offset_guard);
        kernel_mutex_lock(&description->offset_lock, &offset_guard);
        struct kernel_uaccess_iov_cursor cursor = {iov, iov_count, 0U, 0U};
        uint64_t capped = count > KERNEL_FILES_MAX_RW_COUNT
                            ? KERNEL_FILES_MAX_RW_COUNT : count;
        return generated_read(files, mm, description, &cursor, capped,
                              (uint64_t)offset, 0, linux_result);
    }
    if (description->device && description->device->positioned) {
        return read_pinned(files, mm, description_owner, iov, iov_count, count, 0,
                           linux_result, 0U, 0, 0, NULL, NULL);
    }
    if (kind == KERNEL_OPEN_FILE_KIND_REGULAR) {
        kernel_open_file_read_begin(description, (uint64_t)offset);
        kernel_vfs_file_accessed(&description->file,kernel_open_file_flags(description));
    }
    request = count > KERNEL_FILES_MAX_RW_COUNT
                  ? KERNEL_FILES_MAX_RW_COUNT
                  : count;
    position = (uint64_t)offset;
    if (kernel_open_file_flags(description)&KERNEL_FILES_O_DIRECT) {
        struct kernel_uaccess_iov_cursor direct_cursor={iov,iov_count,0,0};
        return direct_read(files,mm,description,&direct_cursor,request,position,0,linux_result);
    }
    file_size = kernel_open_file_size(description);
    if (position >= file_size) {
        *linux_result = 0;
        return KERNEL_FILES_STATUS_OK;
    }
    if (request > file_size - position) {
        request = file_size - position;
    }
    if (kernel_open_file_memory_backed(description)) {
        struct kernel_uaccess_iov_cursor cursor = {iov, iov_count, 0, 0};
        return memory_read(files, mm, description, &cursor, request, position, 0, linux_result);
    }
    struct kernel_uaccess_iov_cursor cursor = {iov, iov_count, 0, 0};
    return disk_read(files, mm, description, &cursor, request, position, 0, linux_result
        );
}

enum kernel_files_status kernel_files_pread(
    struct kernel_files *files, struct kernel_mm *mm, int64_t fd,
    uint64_t user_buffer, uint64_t count, int64_t offset,
    int64_t *linux_result)
{
    struct kernel_open_file_description *description = 0;
    enum kernel_files_status status;

    if (!kernel_files_is_live(files) || mm == 0 || linux_result == 0)
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    KERNEL_FILES_NOTE(files, read_calls, 1);
    status = kernel_files_pin_data(files, fd, &description, linux_result);
    if (status != KERNEL_FILES_STATUS_OK) return status;
    if (*linux_result != 0) {
        KERNEL_FILES_NOTE(files, read_failures, 1);
        return KERNEL_FILES_STATUS_OK;
    }
    struct kernel_uaccess_iovec iov = {user_buffer, count};
    status = pread_iov_pinned(files, mm, &description, &iov, 1,
                              count, offset, 1, linux_result);
    if (!status && *linux_result>0 && description)
        kernel_vfs_notify_file(&description->file,VFS_NOTIFY_ACCESS);
    return kernel_files_release_io(files, &description, status);
}


int kernel_files_description_writable(
    const struct kernel_open_file_description *description)
{
    uint32_t access_mode;

    if (description == 0) {
        return 0;
    }
    const enum kernel_open_file_kind kind = description->kind;
    access_mode = kernel_open_file_flags(description) & 3U;
    if (kind == KERNEL_OPEN_FILE_KIND_REGULAR ||
        kind == KERNEL_OPEN_FILE_KIND_BLOCK) {
        return access_mode == 1U || access_mode == 2U;
    }
    if (kind == KERNEL_OPEN_FILE_KIND_GENERATED)
        return access_mode == 1U || access_mode == 2U;
    if (kind == KERNEL_OPEN_FILE_KIND_PIPE) {
        return access_mode != 0U;
    }
    if (kind == KERNEL_OPEN_FILE_KIND_SOCKET ||
        (kind == KERNEL_OPEN_FILE_KIND_EVENTFD || kind == KERNEL_OPEN_FILE_KIND_TIMERFD || kind == KERNEL_OPEN_FILE_KIND_SIGNALFD)) {
        return access_mode == 2U;
    }
    if (description->device) {
        if (description->kind == KERNEL_OPEN_FILE_KIND_CONSOLE &&
            description->file.private_data == 0) return 1;
        return access_mode == 1U || access_mode == 2U;
    }
    return 0;
}

static int control_write_as_opener(struct kernel_open_file_description *description,
    uint64_t *offset, char *buffer, size_t size, size_t *accepted)
{
    int opener_credentials = kernel_vfs_file_control_open_credentials(&description->file);
    const struct kernel_credentials *previous = opener_credentials ?
        kernel_credentials_override(description->credentials) : 0;
    int error = kernel_vfs_file_control(&description->file,1,offset,buffer,size,accepted);
    if (opener_credentials) kernel_credentials_override(previous);
    return error;
}

static enum kernel_files_status control_write(
    struct kernel_files *files, struct kernel_mm *mm,
    struct kernel_open_file_description *description,
    const struct kernel_uaccess_iovec *iov, size_t iov_count, uint64_t count,
    int positioned, uint64_t requested_offset, int64_t *linux_result)
{
    size_t prefix = kernel_vfs_file_control_prefix(&description->file);
    if (prefix) {
        /* Bounded legacy writers opt into pwrite explicitly (seq_file comm
         * does not). Each writev component invokes its callback independently;
         * ignored tail bytes
         * must not be faulted in, even though the whole component is consumed. */
        if (positioned && !(prefix & KERNEL_VFS_CONTROL_PREFIX_POSITIONED)) {
            *linux_result = -KERNEL_ESPIPE; return KERNEL_FILES_STATUS_OK;
        }
        prefix &= ~KERNEL_VFS_CONTROL_PREFIX_POSITIONED;
        char buffer[35];
        if (prefix >= sizeof(buffer)) __builtin_trap();
        uint64_t total = 0;
        for (size_t i = 0; i < iov_count; i++) {
            size_t segment = iov[i].length > count - total ? (size_t)(count - total) : (size_t)iov[i].length;
            if (!segment && count) continue;
            size_t imported = segment > prefix ? prefix : segment;
            size_t copied = 0, accepted = 0;
            enum kernel_uaccess_status access = kernel_copy_from_user(mm, buffer, iov[i].base, imported, &copied);
            int error = access == KERNEL_UACCESS_STATUS_FAULT ? -KERNEL_EFAULT : 0;
            if (access != KERNEL_UACCESS_STATUS_OK && !error) return KERNEL_FILES_STATUS_STATE;
            if (!error && copied != imported) return KERNEL_FILES_STATUS_STATE;
            uint64_t offset = positioned ? requested_offset : kernel_open_file_offset(description);
            if (!error) error = control_write_as_opener(description, &offset, buffer, imported, &accepted);
            if (error) { *linux_result = total ? (int64_t)total : error; return KERNEL_FILES_STATUS_OK; }
            if (accepted != imported) return KERNEL_FILES_STATUS_STATE;
            total += segment;
            if (total == count) break;
        }
        *linux_result = (int64_t)total;
        return KERNEL_FILES_STATUS_OK;
    }

    char *buffer = 0;
    size_t copied = 0, accepted = 0;
    int result = kernel_vfs_file_control_permission(&description->file, 1);
    if (result) { *linux_result = result; return KERNEL_FILES_STATUS_OK; }
    if (!count) { *linux_result = 0; return KERNEL_FILES_STATUS_OK; }
    enum kernel_heap_status allocation = kernel_heap_allocate(
        files->heap, (size_t)count, (void **)&buffer);
    if (allocation != KERNEL_HEAP_STATUS_OK) {
        if (allocation != KERNEL_HEAP_STATUS_EMPTY) return KERNEL_FILES_STATUS_STATE;
        *linux_result = -KERNEL_ENOMEM;
        return KERNEL_FILES_STATUS_OK;
    }
    struct kernel_uaccess_iov_cursor cursor = {iov, iov_count, 0, 0};
    enum kernel_uaccess_status access = kernel_copy_from_user_iov(mm, &cursor,
        buffer, (size_t)count, &copied);
    /* 先完整收集 writev；第二段 fault 不得发布首段配置或偏移。 */
    if (access != KERNEL_UACCESS_STATUS_OK || copied != count) {
        kernel_heap_release(files->heap, buffer);
        if (access != KERNEL_UACCESS_STATUS_FAULT) return KERNEL_FILES_STATUS_STATE;
        *linux_result = -KERNEL_EFAULT;
        return KERNEL_FILES_STATUS_OK;
    }
    uint64_t offset = positioned ? requested_offset : kernel_open_file_offset(description);
    result = control_write_as_opener(description, &offset,
        buffer, (size_t)count, &accepted);
    kernel_heap_release(files->heap, buffer);
    if (result) { *linux_result = result; return KERNEL_FILES_STATUS_OK; }
    if (!positioned && kernel_open_file_seek(description, offset) != KERNEL_OPEN_FILE_STATUS_OK)
        return KERNEL_FILES_STATUS_STATE;
    *linux_result = (int64_t)accepted;
    return KERNEL_FILES_STATUS_OK;
}

struct write_options { uint32_t flags;unsigned no_signal; };

static enum kernel_files_status regular_write_request(
    struct kernel_files *files,struct kernel_mm *mm,
    struct kernel_open_file_description *description,
    const struct kernel_uaccess_iovec *iov,size_t iov_count,
    uint64_t count,int positioned,uint64_t requested_offset,int64_t *linux_result,
    uint64_t *accepted_end,const struct write_options *options)
{
    if (!count) { *linux_result=0; return KERNEL_FILES_STATUS_OK; }
    int append=(options->flags&KERNEL_FILES_O_APPEND)!=0;
    uint64_t offset=positioned?requested_offset:kernel_open_file_offset(description);
    struct kernel_vfs_size_limit size_limit={.maximum=kernel_task_current_fsize_limit()};
    struct kernel_vfs_user_write_result completed={0};
    int error=kernel_vfs_write_user_mode(&description->file,mm,iov,iov_count,
        (size_t)count,offset,append,!!(options->flags&KERNEL_FILES_O_DIRECT),&size_limit,&completed);
    kernel_files_size_limit_notify(&size_limit);
    if ((completed.access!=KERNEL_UACCESS_STATUS_OK &&
         completed.access!=KERNEL_UACCESS_STATUS_FAULT) || completed.written>count)
        return KERNEL_FILES_STATUS_STATE;
    KERNEL_FILES_NOTE(files,write_chunks,completed.chunks);
    if (completed.written) *accepted_end=completed.end_offset;
    if (completed.written && !positioned) description->offset=completed.end_offset;
    if (error || completed.access==KERNEL_UACCESS_STATUS_FAULT) {
        if (completed.written) KERNEL_FILES_NOTE(files,write_failures,1);
        *linux_result=completed.written?(int64_t)completed.written:
            error?error:-KERNEL_EFAULT;
    } else *linux_result=(int64_t)completed.written;
    return KERNEL_FILES_STATUS_OK;
}

static enum kernel_files_status buffered_write_request(
    struct kernel_files *files, struct kernel_mm *mm,
    struct kernel_open_file_description *description,
    const struct kernel_uaccess_iovec *iov, size_t iov_count,
    uint64_t count, int positioned, uint64_t requested_offset,
    int64_t *linux_result, unsigned char *staging, size_t capacity,
    const struct write_options *options)
{
    uint64_t total = 0U;

    if (description->device && description->device->discard_writes) {
        size_t consumed = 0U;
        int result = description->device->write(description->device_instance, kernel_task_current(), options->flags, 0, (size_t)count, &consumed);
        if (consumed > count) return KERNEL_FILES_STATUS_STATE;
        *linux_result = result && !consumed ? result : (int64_t)consumed;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_GENERATED) {
        if (kernel_vfs_file_is_control(&description->file, 1))
            return control_write(files, mm, description, iov, iov_count, count,
                                 positioned, requested_offset, linux_result);
        *linux_result = -KERNEL_EIO;
        return KERNEL_FILES_STATUS_OK;
    }


    if (!description->device) return KERNEL_FILES_STATUS_STATE;
    for (size_t index = 0U; index < iov_count && total < count; index++) {
        uint64_t offset = 0U;

        while (offset < iov[index].length && total < count) {
            size_t chunk = iov[index].length - offset;
            size_t copied = 0U;
            enum kernel_uaccess_status status;

            if (chunk > count - total) {
                chunk = count - total;
            }
            if (chunk > capacity) {
                chunk = capacity;
            }
            status = kernel_copy_from_user(mm, staging,
                                            iov[index].base + offset,
                                            chunk, &copied);
            size_t written = 0U;
            int result = description->device->write(description->device_instance, kernel_task_current(), options->flags, staging, copied, &written);
            if (written > copied) return KERNEL_FILES_STATUS_STATE;
            total += written;
            offset += written;
            if (result || written < copied) {
                *linux_result = total ? (int64_t)total : result;
                return KERNEL_FILES_STATUS_OK;
            }
            if (status == KERNEL_UACCESS_STATUS_FAULT) {
                if (total != 0U) {
                    KERNEL_FILES_NOTE(files, write_failures, 1);
                }
                *linux_result = total != 0U ? (int64_t)total : -KERNEL_EFAULT;
                return KERNEL_FILES_STATUS_OK;
            }
            if (status != KERNEL_UACCESS_STATUS_OK || copied != chunk) {
                return KERNEL_FILES_STATUS_STATE;
            }
        }
    }
    *linux_result = (int64_t)total;
    return KERNEL_FILES_STATUS_OK;
}

static enum kernel_files_status unix_write_request(struct kernel_files *files, struct kernel_mm *mm,
    struct kernel_open_file_description *description,
    const struct kernel_uaccess_iovec *iov, size_t iov_count, uint64_t count,
    uint32_t flags, int64_t *linux_result, struct kernel_socket_message *socket_message)
{
    struct kernel_open_file_description *borrowed=description;
    struct kernel_socket *socket=kernel_open_file_socket(description);
    struct kernel_uaccess_iov_cursor cursor={iov,iov_count,0,0};
    uint64_t total=0,target=0,timeout=(flags & KERNEL_SOCKET_MSG_DONTWAIT)?0:kernel_socket_send_timeout(socket);
    if (timeout) {
        uint64_t now=kernel_time_monotonic_ns();
        target=UINT64_MAX-now<timeout?UINT64_MAX:now+timeout;
    }
    do {
        int fault=0;
        int result=kernel_socket_write_unix(&description,mm,&cursor,count-total,
            flags|KERNEL_SOCKET_MSG_DONTWAIT,&fault, socket_message);
        if (result==-KERNEL_EAGAIN && !(flags & KERNEL_SOCKET_MSG_DONTWAIT)) {
            uint64_t now=target?kernel_time_monotonic_ns():0;
            result=target && now>=target?-KERNEL_EAGAIN:kernel_socket_wait_ready(kernel_open_file_socket(borrowed),
                KERNEL_POLLOUT,target,flags);
            if (!result) continue;
            if (total && result==-KERNEL_ERESTARTSYS)
                kernel_signal_clear_syscall_restart(kernel_task_current());
        }
        if (result<0) {
            if (!total && result==-KERNEL_EPIPE && kernel_socket_is_stream(kernel_open_file_socket(borrowed)) &&
                !(flags&KERNEL_SOCKET_MSG_NOSIGNAL))
                (void)kernel_signal_send_task(kernel_task_current(),13U,0);
            *linux_result=total?(int64_t)total:result;
            return KERNEL_FILES_STATUS_OK;
        }
        total+=(uint64_t)result;
        if (result>0) KERNEL_FILES_NOTE(files,write_chunks,1);
        if (!result || fault) break;
    } while (total<count);
    *linux_result=(int64_t)total;
    return KERNEL_FILES_STATUS_OK;
}

/* The existing task scratch holds page references, not another payload copy. */
enum { TCP_USER_PINS_INLINE=4,
#if BOAROS_USER_SMP && BOAROS_PAGE_SHIFT == 12
       TCP_USER_PINS_MAX=9 /* Unaligned 32 KiB send window. */
#else
       TCP_USER_PINS_MAX=TCP_USER_PINS_INLINE
#endif
};
struct tcp_user_batch {
    struct kernel_mm_user_pin pins[TCP_USER_PINS_MAX];
    uint64_t pages[TCP_USER_PINS_MAX];
    struct kernel_socket_write_segment segments[TCP_USER_PINS_MAX];
};
_Static_assert(sizeof(struct tcp_user_batch) <= BOAROS_PAGE_SIZE, "TCP pin workspace");

static int tcp_write_user_batch(struct kernel_mm *mm,
    struct kernel_socket_write_request *request, struct kernel_task_io_buffer *workspace,
    struct kernel_uaccess_iov_cursor *cursor,uint32_t size,uint32_t flags,int *fault,unsigned limit)
{
    if (!workspace->allocator && kernel_task_io_buffer_acquire(workspace,mm->allocator)!=KERNEL_TASK_STATUS_OK)
        return -KERNEL_ENOMEM;
    struct tcp_user_batch *batch=workspace->data;
    struct kernel_uaccess_iov_cursor gather=*cursor;
    unsigned pins=0,segments=0;
    uint32_t prepared=0;
    int result=0;
    while (prepared<size) {
        uint64_t address;
        size_t length=kernel_uaccess_iov_span(&gather,size-prepared,&address);
        if (!length) __builtin_trap();
        uint64_t page=address & ~(uint64_t)BOAROS_PAGE_MASK;
        unsigned index=0;
        while (index<pins && batch->pages[index]!=page) index++;
        if (index==pins) {
            if (pins==limit || segments==limit) break;
            struct kernel_mm_user_pin *pin=&batch->pins[pins];
            *pin=(struct kernel_mm_user_pin){0};
            enum kernel_uaccess_status access;
            if (!pins) access=kernel_uaccess_pin_user_page(mm,address,KERNEL_MM_READ,pin);
            else {
                /* An editor can be draining earlier pins. Submit their prefix
                 * before faulting or waiting for the next mapping. */
                COST_ADD(USER_RESOLUTIONS,1);COST_IO_ADD(4,1);
                enum kernel_mm_status status=kernel_mm_pin_user_page(mm,address,KERNEL_MM_READ,pin);
                if (status==KERNEL_MM_STATUS_NOT_MAPPED || status==KERNEL_MM_STATUS_ACCESS ||
                    status==KERNEL_MM_STATUS_CONFLICT || status==KERNEL_MM_STATUS_ADDRESS_SPACE ||
                    status==KERNEL_MM_STATUS_BUS_FAULT || status==KERNEL_MM_STATUS_NO_MEMORY) break;
                if (status) __builtin_trap();
                access=KERNEL_UACCESS_STATUS_OK;
            }
            if (access) {
                if (access!=KERNEL_UACCESS_STATUS_FAULT) __builtin_trap();
                *fault=1;if (!prepared) result=-KERNEL_EFAULT;break;
            }
            batch->pages[pins++]=page;
        }
        const void *data=(const unsigned char *)batch->pins[index].page+(address&BOAROS_PAGE_MASK);
        struct kernel_socket_write_segment *last=segments?&batch->segments[segments-1]:NULL;
        uintptr_t page_address=(uintptr_t)batch->pins[index].page;
        /* Adjacent iovecs in one pinned page share a protocol fragment too. */
        if (last && (uintptr_t)last->data>=page_address &&
            (uintptr_t)last->data-page_address<BOAROS_PAGE_SIZE &&
            (const unsigned char *)last->data+last->size==data) last->size+=(uint32_t)length;
        else {
            if (segments==limit) break;
            batch->segments[segments++]=(struct kernel_socket_write_segment){data,(uint32_t)length};
        }
        prepared+=(uint32_t)length;gather.offset+=length;
    }
    flags|=(!*fault && prepared<size)?KERNEL_SOCKET_IO_MORE:0;
#if BOAROS_USER_SMP
    int detached=prepared && kernel_socket_stream_preparev(request,batch->segments,segments,prepared);
    if (prepared && !detached)
#else
    if (prepared)
#endif
        result=kernel_socket_stream_commitv(request,batch->segments,segments,flags);
    while (pins) kernel_mm_unpin_user_page(&batch->pins[--pins]);
#if BOAROS_USER_SMP
    /* The request now owns the final payload; even a busy shard no longer
     * makes the address-space editor wait for this send's user pins. */
    if (detached) result=kernel_socket_stream_publish(request,flags);
#endif
    /* The protocol may accept only part of the prepared vector. Descriptor
     * preparation never advances the caller's completed prefix. */
    if (result>0) {
        unsigned left=(unsigned)result;
        if (left>prepared) __builtin_trap();
        while (left) {
            uint64_t address;size_t length=kernel_uaccess_iov_span(cursor,left,&address);
            if (!length) __builtin_trap();
            cursor->offset+=length;left-=(unsigned)length;
        }
    }
    return result;
}

/* Coalesce only when one page would need more protocol fragments than the
 * direct workspace can retain. A contiguous vector keeps the pinned path. */
static int tcp_fragmented_prefix(struct kernel_uaccess_iov_cursor cursor,uint64_t remaining,unsigned fragments)
{
    if (cursor.count-cursor.index<=fragments) return 0;
    size_t limit=remaining<BOAROS_PAGE_SIZE?(size_t)remaining:BOAROS_PAGE_SIZE;
    uint64_t end=0;unsigned runs=0;
    while (limit) {
        uint64_t address;size_t length=kernel_uaccess_iov_span(&cursor,limit,&address);
        if (!length) __builtin_trap();
        if ((!runs || address!=end) && ++runs>fragments) return 1;
        end=address+length;cursor.offset+=length;limit-=length;
    }
    return 0;
}

static __attribute__((noinline)) enum kernel_files_status tcp_write_request(
    struct kernel_files *files,struct kernel_mm *mm,
    struct kernel_open_file_description *description,
    const struct kernel_uaccess_iovec *iov,size_t iov_count,uint64_t count,
    uint32_t flags,int64_t *linux_result)
{
    struct kernel_open_file_description *borrowed=description;
    struct kernel_socket *socket=kernel_open_file_socket(description);
    struct kernel_socket_write_request request={0};
    struct kernel_task_io_buffer buffer={0};
    struct kernel_uaccess_iov_cursor cursor={iov,iov_count,0,0};
    enum kernel_files_status status=KERNEL_FILES_STATUS_OK;
    uint64_t total=0,target=0,timeout=(flags&KERNEL_SOCKET_MSG_DONTWAIT)?0:kernel_socket_send_timeout(socket);
    int fault=0;
    size_t staged=0,consumed=0;
    unsigned pins=TCP_USER_PINS_INLINE;
#if BOAROS_USER_SMP && BOAROS_PAGE_SHIFT == 12
    if (kernel_socket_stream_has_shard(socket)) pins=TCP_USER_PINS_MAX;
#endif
    if (timeout) {
        uint64_t now=kernel_time_monotonic_ns();
        target=UINT64_MAX-now<timeout?UINT64_MAX:now+timeout;
    }
    kernel_socket_stream_begin(&request,&description);
    do {
        int compact=staged>consumed || tcp_fragmented_prefix(cursor,count-total,pins);
        size_t chunk=staged-consumed;
        if (!chunk && count) {
            uint64_t address;
            if (!kernel_uaccess_iov_span(&cursor,1,&address)) __builtin_trap();
            size_t limit=compact?BOAROS_PAGE_SIZE:pins*BOAROS_PAGE_SIZE-(address&BOAROS_PAGE_MASK);
            chunk=count-total<limit?(size_t)(count-total):limit;
        }
        int result=kernel_socket_stream_reserve(&request,(uint32_t)chunk,flags);
        if (result>=0) {
            if (!count) { *linux_result=0;break; }
            if (!result || (size_t)result>chunk) { status=KERNEL_FILES_STATUS_STATE;break; }
            chunk=(size_t)result;
            if (!compact) result=tcp_write_user_batch(mm,&request,&buffer,&cursor,(uint32_t)chunk,
                flags|((total+chunk<count)?KERNEL_SOCKET_IO_MORE:0),&fault,pins);
            else {
                if (staged==consumed) {
                    if (!buffer.allocator && kernel_task_io_buffer_acquire(&buffer,mm->allocator)!=KERNEL_TASK_STATUS_OK) {
                        *linux_result=total?(int64_t)total:-KERNEL_ENOMEM;break;
                    }
                    size_t copied=0;
                    enum kernel_uaccess_status access=kernel_copy_from_user_iov(mm,&cursor,buffer.data,chunk,&copied);
                    if ((access!=KERNEL_UACCESS_STATUS_OK && access!=KERNEL_UACCESS_STATUS_FAULT) ||
                        copied>chunk || (!access && copied!=chunk)) {status=KERNEL_FILES_STATUS_STATE;break;}
                    fault=access==KERNEL_UACCESS_STATUS_FAULT;
                    if (!copied) {*linux_result=total?(int64_t)total:-KERNEL_EFAULT;break;}
                    staged=copied;consumed=0;chunk=copied;
                }
                result=kernel_socket_stream_commit(&request,(const unsigned char *)buffer.data+consumed,
                    (uint32_t)chunk,flags|((!fault && total+chunk<count)?KERNEL_SOCKET_IO_MORE:0));
            }
        }
        if (result==-KERNEL_EAGAIN && !(flags&KERNEL_SOCKET_MSG_DONTWAIT)) {
            uint64_t now=target?kernel_time_monotonic_ns():0;
            result=target && now>=target?-KERNEL_EAGAIN:kernel_socket_wait_ready(
                kernel_open_file_socket(borrowed),KERNEL_POLLOUT,target,flags);
            if (!result) continue;
            if (total && result==-KERNEL_ERESTARTSYS) kernel_signal_clear_syscall_restart(kernel_task_current());
        }
        if (result<0) { *linux_result=total?(int64_t)total:result;break; }
        if (!result || (size_t)result>chunk) { status=KERNEL_FILES_STATUS_STATE;break; }
        if (compact) consumed+=(size_t)result;
        KERNEL_FILES_NOTE(files,write_chunks,1);
        /* Compact copying was already charged by the common uaccess helper. */
        if (!compact) {COST_ADD(COPY_FROM_USER,result);COST_IO_ADD(3,result);}
        total+=(uint64_t)result;*linux_result=(int64_t)total;
        if (fault || ((size_t)result<chunk && (flags&KERNEL_SOCKET_MSG_DONTWAIT))) break;
    } while (total<count);
    kernel_socket_stream_finish(&request);
    if (buffer.allocator) kernel_task_io_buffer_release(&buffer);
    return status;
}

static __attribute__((noinline)) enum kernel_files_status non_socket_write_request(
    struct kernel_files *files, struct kernel_mm *mm,
    struct kernel_open_file_description *description,
    const struct kernel_uaccess_iovec *iov, size_t iov_count,
    uint64_t count, int positioned, uint64_t requested_offset,
    int64_t *linux_result, const struct kernel_lock_guard *borrowed,
    const struct write_options *options)
{
    const enum kernel_open_file_kind kind=description->kind;
    KERNEL_LOCK_SCOPE(offset_guard);
    if (((!positioned || (options->flags&KERNEL_FILES_O_DSYNC)) &&
         kind == KERNEL_OPEN_FILE_KIND_REGULAR) || kind == KERNEL_OPEN_FILE_KIND_GENERATED) {
        if (!regular_borrows_offset(description,borrowed))
            kernel_mutex_lock(&description->offset_lock, &offset_guard);
    }
    enum kernel_files_status status;
    uint64_t accepted_end=0;
    if (kind==KERNEL_OPEN_FILE_KIND_REGULAR) {
        status=regular_write_request(files,mm,description,iov,iov_count,count,
            positioned,requested_offset,linux_result,&accepted_end,options);
    } else {
        unsigned char small[KERNEL_FILES_WRITE_STAGING];
        unsigned char *staging = small;
        size_t capacity = sizeof(small);
        status=buffered_write_request(files,mm,description,iov,iov_count,count,
            positioned,requested_offset,linux_result,staging,capacity,options);
    }
    if (status == KERNEL_FILES_STATUS_OK) COST_IO_ADD(2, *linux_result > 0 ? (uint64_t)*linux_result : 0);
    if (status == KERNEL_FILES_STATUS_OK && *linux_result > 0 &&
        kind == KERNEL_OPEN_FILE_KIND_REGULAR &&
        (options->flags & KERNEL_FILES_O_DSYNC) != 0U) {
        int datasync = (options->flags & KERNEL_FILES_O_SYNC) !=
                        KERNEL_FILES_O_SYNC;
        const struct kernel_lock_guard *borrow = offset_guard.lock ? &offset_guard : borrowed;
        int error = kernel_vfs_sync_range_borrowing(&description->file,
            accepted_end-(uint64_t)*linux_result,accepted_end,datasync,
            &description->observed_writeback_error,borrow);
        /* generic_write_sync returns the error after accepted bytes have
         * advanced the OFD offset, including a prefix before usercopy fault. */
        if (error != 0) *linux_result = error;
    }
    return status;
}

static enum kernel_files_status write_request(
    struct kernel_files *files, struct kernel_mm *mm,
    struct kernel_open_file_description *description,
    const struct kernel_uaccess_iovec *iov, size_t iov_count,
    uint64_t count, int positioned, uint64_t requested_offset,
    int64_t *linux_result, uint32_t socket_flags,const struct kernel_lock_guard *borrowed, struct kernel_socket_message *socket_message,uint32_t rwf)
{
    if(description->device && !description->device->write && !description->device->writev){
        *linux_result=-KERNEL_EINVAL;return KERNEL_FILES_STATUS_OK;
    }
    struct write_options options={.flags=kernel_open_file_flags(description),
        .no_signal=!!(rwf&KERNEL_RWF_NOSIGNAL)};
    if (rwf&KERNEL_RWF_NOAPPEND) options.flags&=~KERNEL_FILES_O_APPEND;
    else if (rwf&KERNEL_RWF_APPEND) options.flags|=KERNEL_FILES_O_APPEND;
    if (rwf&KERNEL_RWF_SYNC) options.flags|=KERNEL_FILES_O_SYNC;
    else if (rwf&KERNEL_RWF_DSYNC) options.flags|=KERNEL_FILES_O_DSYNC;
    if (description->block) {
        enum kernel_files_status status=kernel_block_file_io(description,mm,iov,iov_count,
            count,1,positioned,requested_offset,linux_result);
        if (!status && *linux_result>0 && (rwf&(KERNEL_RWF_SYNC|KERNEL_RWF_DSYNC)) &&
            !(kernel_open_file_flags(description)&KERNEL_FILES_O_DSYNC)) {
            int error=kernel_block_file_sync(description);if (error) *linux_result=error;
        }
        return status;
    }
    if (options.no_signal) socket_flags|=KERNEL_SOCKET_MSG_NOSIGNAL;
    const enum kernel_open_file_kind kind = description->kind;
    if(kind==KERNEL_OPEN_FILE_KIND_MQUEUE){
        *linux_result=kernel_open_file_writable(description)?-KERNEL_EINVAL:-KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    if(kind==KERNEL_OPEN_FILE_KIND_PIDFD) {
        *linux_result=-KERNEL_EINVAL;return KERNEL_FILES_STATUS_OK;
    }
    if ((kind==KERNEL_OPEN_FILE_KIND_EVENTFD || kind==KERNEL_OPEN_FILE_KIND_TIMERFD)) {
        *linux_result=kernel_event_fd_io(description,mm,iov,iov_count,count,1);
        return KERNEL_FILES_STATUS_OK;
    }
    /* 固定本次调用的等待资格；后续fcntl只影响下一次读写。 */
    if (kind == KERNEL_OPEN_FILE_KIND_SOCKET &&
        (options.flags & KERNEL_FILES_O_NONBLOCK))
        socket_flags |= KERNEL_SOCKET_MSG_DONTWAIT;
    KERNEL_SERVICE_SCOPE_IF(service, kind != KERNEL_OPEN_FILE_KIND_REGULAR && kind != KERNEL_OPEN_FILE_KIND_SOCKET && kind != KERNEL_OPEN_FILE_KIND_PIPE);
    COST_SCOPE(write_cost, OPERATION_TICKS);
    COST_IO_SCOPE(io_cost,
        kernel_socket_preserves_messages(kernel_open_file_socket(description)) ? 2 :
        kind == KERNEL_OPEN_FILE_KIND_REGULAR ? 0 : kind == KERNEL_OPEN_FILE_KIND_SOCKET ? 1 : 3);
    COST_IO_ADD(0, 1); COST_IO_ADD(1, count);
    /* Pipe copy owns its final pages; do not carry the device staging frame
     * through a file-backed user-page fault. */
    if (kind == KERNEL_OPEN_FILE_KIND_PIPE) {
        struct kernel_pipe_result completed;
        if (kernel_pipe_writev(description->pipe,mm,iov,iov_count,count,
            options.flags,&completed)!=KERNEL_PIPE_STATUS_OK ||
            kernel_files_pipe_result(completed.bytes,completed.broken && !options.no_signal)) return KERNEL_FILES_STATUS_STATE;
        *linux_result=completed.bytes;
        if (*linux_result>0 && description->file.private_data && !kernel_vfs_mount_is_readonly(description->file.mount)) {
            int error=kernel_vfs_file_modified(&description->file,0,0);if(error)*linux_result=error;
        }
        COST_IO_ADD(2,*linux_result>0?(uint64_t)*linux_result:0);
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_socket_preserves_messages(kernel_open_file_socket(description))) {
        *linux_result = kernel_socket_write_datagram(&description, mm, iov,
            iov_count, count, socket_flags, 0, socket_message);
        COST_IO_ADD(2, *linux_result > 0 ? (uint64_t)*linux_result : 0);
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_socket_is_tcp(kernel_open_file_socket(description))) {
        enum kernel_files_status status = tcp_write_request(files, mm, description,
            iov, iov_count, count, socket_flags, linux_result);
        if (status == KERNEL_FILES_STATUS_OK) COST_IO_ADD(2, *linux_result > 0 ? (uint64_t)*linux_result : 0);
        return status;
    }
    if (kind==KERNEL_OPEN_FILE_KIND_SOCKET) {
        enum kernel_files_status status=unix_write_request(files,mm,description,iov,iov_count,count,socket_flags,linux_result, socket_message);
        if (status==KERNEL_FILES_STATUS_OK) COST_IO_ADD(2,*linux_result>0?(uint64_t)*linux_result:0);
        return status;
    }
    enum kernel_files_status status=non_socket_write_request(files,mm,description,
        iov,iov_count,count,positioned,requested_offset,linux_result,borrowed,&options);
    return status;
}

enum kernel_files_status kernel_files_socket_iov_io(
    struct kernel_files *files, struct kernel_mm *mm,
    struct kernel_open_file_description **owner,
    const struct kernel_uaccess_iovec *iov, size_t iov_count, uint64_t count,
    uint32_t flags, int writing, struct kernel_socket_address *peer,
    uint32_t *message_size, int64_t *linux_result, struct kernel_socket_message *socket_message)
{
    if (!kernel_files_is_live(files) || mm == 0 || owner == 0 || *owner == 0 ||
        kernel_open_file_socket(*owner) == 0 || linux_result == 0)
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    if (count > KERNEL_FILES_MAX_RW_COUNT) count = KERNEL_FILES_MAX_RW_COUNT;
    if (writing) {
        KERNEL_FILES_NOTE(files, write_calls, 1);
        enum kernel_files_status status = write_request(files, mm, *owner, iov, iov_count, count, 0, 0, linux_result, flags,NULL, socket_message,0);
        if (status == KERNEL_FILES_STATUS_OK) {
            if (*linux_result >= 0) KERNEL_FILES_NOTE(files, bytes_written, (uint64_t)*linux_result);
            else KERNEL_FILES_NOTE(files, write_failures, 1);
        }
        return status;
    }
    KERNEL_FILES_NOTE(files, read_calls, 1);
    return read_pinned(files, mm, owner, iov, iov_count, count, 0, linux_result, flags, peer, message_size, socket_message, NULL);
}
enum kernel_files_status kernel_files_socket_io(
    struct kernel_files *files, struct kernel_mm *mm,
    struct kernel_open_file_description **owner, uint64_t user_buffer,
    uint64_t count, uint32_t flags, int writing, int64_t *linux_result)
{
    const struct kernel_uaccess_iovec iov = {user_buffer, count};
    return kernel_files_socket_iov_io(files, mm, owner, &iov, 1, count, flags, writing, 0, 0, linux_result, NULL);
}

static void account_write(struct kernel_files *files, int64_t result)
{
    if (result >= 0) {
        KERNEL_FILES_NOTE(files, bytes_written, (uint64_t)result);
    } else {
        KERNEL_FILES_NOTE(files, write_failures, 1);
    }
}

enum kernel_files_status kernel_files_write(
    struct kernel_files *files, struct kernel_mm *mm, int64_t fd,
    uint64_t user_buffer, uint64_t count, int64_t *linux_result)
{
    struct kernel_uaccess_iovec iov = {user_buffer, count};
    struct kernel_open_file_description *description = 0;
    enum kernel_files_status status;

    if (!kernel_files_is_live(files) || mm == 0 || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    KERNEL_FILES_NOTE(files, write_calls, 1);
    status = kernel_files_pin_data(files, fd, &description, linux_result);
    if (status != KERNEL_FILES_STATUS_OK) {
        return status;
    }
    if (*linux_result != 0) {
        *linux_result = -KERNEL_EBADF;
        account_write(files, *linux_result);
        return KERNEL_FILES_STATUS_OK;
    }
    if(description->kind==KERNEL_OPEN_FILE_KIND_MQUEUE){
        *linux_result=kernel_open_file_writable(description)?-KERNEL_EINVAL:-KERNEL_EBADF;
        account_write(files,*linux_result);
        return kernel_files_release_io(files,&description,KERNEL_FILES_STATUS_OK);
    }
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_EPOLL ||
        description->kind==KERNEL_OPEN_FILE_KIND_PIDFD ||
        description->kind==KERNEL_OPEN_FILE_KIND_TIMERFD || description->kind==KERNEL_OPEN_FILE_KIND_SIGNALFD) {
        *linux_result = -KERNEL_EINVAL;
        account_write(files, *linux_result);
        return kernel_files_release_io(files, &description,
                                      KERNEL_FILES_STATUS_OK);
    }
    if (!kernel_files_description_writable(description)) {
        *linux_result = -KERNEL_EBADF;
        account_write(files, *linux_result);
        return kernel_files_release_io(files, &description,
                                      KERNEL_FILES_STATUS_OK);
    }
    /* Validate the original range before clipping; rejection cannot accept data. */
    if (kernel_user_range_check(user_buffer, (size_t)count) !=
            KERNEL_UACCESS_STATUS_OK ||
        scalar_empty_range_fault(user_buffer, count)) {
        *linux_result = -KERNEL_EFAULT;
        account_write(files, *linux_result);
        return kernel_files_release_io(files, &description,
                                      KERNEL_FILES_STATUS_OK);
    }
    if (count > KERNEL_FILES_MAX_RW_COUNT) count = KERNEL_FILES_MAX_RW_COUNT;
    iov.length = count;
    if (description->device && description->device->writev) {
        status = description->device->writev(description->device_instance,
            kernel_task_current(), files, &description, 0, mm, &iov, 1U,
            count, kernel_open_file_flags(description), linux_result);
    } else status = write_request(files, mm, description, &iov, 1U, count,
                           0, 0U, linux_result, 0U,NULL, NULL,0);
    if (status == KERNEL_FILES_STATUS_OK) {
        account_write(files, *linux_result);
    }
    return kernel_files_release_io(files, &description, status);
}

enum kernel_files_status kernel_files_pwrite(
    struct kernel_files *files, struct kernel_mm *mm, int64_t fd,
    uint64_t user_buffer, uint64_t count, int64_t offset,
    int64_t *linux_result)
{
    struct kernel_uaccess_iovec iov = {user_buffer, count};
    struct kernel_open_file_description *description = 0;
    enum kernel_files_status status;
    enum kernel_open_file_kind kind;

    if (!kernel_files_is_live(files) || mm == 0 || linux_result == 0)
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    KERNEL_FILES_NOTE(files, write_calls, 1);
    status = kernel_files_pin_data(files, fd, &description, linux_result);
    if (status != KERNEL_FILES_STATUS_OK) return status;
    if (*linux_result != 0) {
        *linux_result = -KERNEL_EBADF;
        account_write(files, *linux_result);
        return KERNEL_FILES_STATUS_OK;
    }
    kind = description->kind;
    if(kind==KERNEL_OPEN_FILE_KIND_MQUEUE)
        *linux_result=kernel_open_file_writable(description)?-KERNEL_EINVAL:-KERNEL_EBADF;
    else if (kind == KERNEL_OPEN_FILE_KIND_PIDFD)
        *linux_result = -KERNEL_EINVAL;
    else if (kind == KERNEL_OPEN_FILE_KIND_EPOLL ||
        kind == KERNEL_OPEN_FILE_KIND_EVENTFD || kind == KERNEL_OPEN_FILE_KIND_TIMERFD || kind == KERNEL_OPEN_FILE_KIND_SIGNALFD ||
        kind == KERNEL_OPEN_FILE_KIND_SOCKET ||
        kind == KERNEL_OPEN_FILE_KIND_PIPE ||
        (description->device && !description->device->positioned))
        *linux_result = -KERNEL_ESPIPE;
    else if (!kernel_files_description_writable(description))
        *linux_result = -KERNEL_EBADF;
    else if (offset < 0)
        *linux_result = -KERNEL_EINVAL;
    else if (kind == KERNEL_OPEN_FILE_KIND_DIRECTORY)
        *linux_result = -KERNEL_EISDIR;
    else
        *linux_result = 0;
    if (*linux_result != 0) {
        account_write(files, *linux_result);
        return kernel_files_release_io(files, &description,
                                      KERNEL_FILES_STATUS_OK);
    }
    if (kernel_user_range_check(user_buffer, (size_t)count) !=
            KERNEL_UACCESS_STATUS_OK ||
        scalar_empty_range_fault(user_buffer, count)) {
        *linux_result = -KERNEL_EFAULT;
        account_write(files, *linux_result);
        return kernel_files_release_io(files, &description,
                                      KERNEL_FILES_STATUS_OK);
    }
    if (count > KERNEL_FILES_MAX_RW_COUNT) count = KERNEL_FILES_MAX_RW_COUNT;
    iov.length = count;
    status = write_request(files, mm, description, &iov, 1U, count,
                           1, (uint64_t)offset, linux_result, 0U,NULL, NULL,0);
    if (status == KERNEL_FILES_STATUS_OK) account_write(files, *linux_result);
    return kernel_files_release_io(files, &description, status);
}

static enum kernel_files_status writev_request(
    struct kernel_files *files, struct kernel_mm *mm, int64_t fd,
    uint64_t user_iov, uint64_t iovcnt, int positioned, int64_t offset,
    uint32_t flags,int64_t *linux_result)
{
    KERNEL_LOCK_SCOPE(offset_guard);
    struct kernel_uaccess_iov_snapshot snapshot = {0};
    struct kernel_open_file_description *description = 0;
    enum kernel_files_status status = KERNEL_FILES_STATUS_OK;

    if (!kernel_files_is_live(files) || mm == 0 || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    KERNEL_FILES_NOTE(files, write_calls, 1);
    status = kernel_files_pin_data(files, fd, &description, linux_result);
    if (status != KERNEL_FILES_STATUS_OK) {
        return status;
    }
    if (*linux_result != 0) {
        *linux_result = -KERNEL_EBADF;
        account_write(files, *linux_result);
        return KERNEL_FILES_STATUS_OK;
    }
    KERNEL_SERVICE_SCOPE_IF(service, kernel_open_file_kind(description) != KERNEL_OPEN_FILE_KIND_REGULAR && kernel_open_file_kind(description) != KERNEL_OPEN_FILE_KIND_SOCKET && kernel_open_file_kind(description) != KERNEL_OPEN_FILE_KIND_PIPE && kernel_open_file_kind(description) != KERNEL_OPEN_FILE_KIND_EVENTFD && kernel_open_file_kind(description) != KERNEL_OPEN_FILE_KIND_TIMERFD && kernel_open_file_kind(description) != KERNEL_OPEN_FILE_KIND_SIGNALFD && kernel_open_file_kind(description) != KERNEL_OPEN_FILE_KIND_MQUEUE);
    if(description->kind==KERNEL_OPEN_FILE_KIND_MQUEUE){
        *linux_result=kernel_open_file_writable(description)?-KERNEL_EINVAL:-KERNEL_EBADF;
        account_write(files,*linux_result);
        return kernel_files_release_io(files,&description,KERNEL_FILES_STATUS_OK);
    }
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_EPOLL ||
        description->kind==KERNEL_OPEN_FILE_KIND_PIDFD ||
        description->kind==KERNEL_OPEN_FILE_KIND_TIMERFD || description->kind==KERNEL_OPEN_FILE_KIND_SIGNALFD) {
        *linux_result = positioned && description->kind!=KERNEL_OPEN_FILE_KIND_PIDFD
            ? -KERNEL_ESPIPE : -KERNEL_EINVAL;
        account_write(files, *linux_result);
        return kernel_files_release_io(files, &description,
                                      KERNEL_FILES_STATUS_OK);
    }
    if (positioned && ((description->device && !description->device->positioned) ||
        (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_EVENTFD || kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_TIMERFD || kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_SIGNALFD) ||
        kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_PIPE ||
        kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_SOCKET ||
        (description->kind == KERNEL_OPEN_FILE_KIND_GENERATED &&
         kernel_vfs_file_control_prefix(&description->file) &&
         !(kernel_vfs_file_control_prefix(&description->file) & KERNEL_VFS_CONTROL_PREFIX_POSITIONED)))) {
        *linux_result = -KERNEL_ESPIPE;
        goto out;
    }
    if (!kernel_files_description_writable(description)) {
        *linux_result = -KERNEL_EBADF;
        account_write(files, *linux_result);
        return kernel_files_release_io(files, &description,
                                      KERNEL_FILES_STATUS_OK);
    }
    if (iovcnt > 1024U) {
        *linux_result = -KERNEL_EINVAL;
        goto out;
    }
    status=kernel_uaccess_iov_import(&snapshot,files->heap,mm,user_iov,iovcnt,linux_result);
    if (status) goto out;
    if (*linux_result) goto out;
    if (!snapshot.total) { *linux_result=0;goto out; }
    if (!positioned && (description->kind==KERNEL_OPEN_FILE_KIND_REGULAR ||
        description->kind==KERNEL_OPEN_FILE_KIND_GENERATED))
        kernel_mutex_lock(&description->offset_lock,&offset_guard);
    *linux_result=vector_request_error(description,positioned?(uint64_t)offset:kernel_open_file_offset(description),
        snapshot.total,flags);
    if (*linux_result) goto out;
    if (!positioned && description->device && description->device->writev) {
        status = description->device->writev(description->device_instance,
            kernel_task_current(), files, &description,
            &snapshot.allocation, mm, snapshot.view, snapshot.count,
            snapshot.total, kernel_open_file_flags(description), linux_result);
    } else status = write_request(files, mm, description, snapshot.view, snapshot.count, snapshot.total,
                           positioned, (uint64_t)offset, linux_result, 0U,offset_guard.lock?&offset_guard:NULL, NULL,flags);
out:
    if (offset_guard.lock) kernel_lock_release(&offset_guard);
    kernel_uaccess_iov_cleanup(&snapshot,files->heap);
    if (status == KERNEL_FILES_STATUS_OK) {
        account_write(files, *linux_result);
    }
    return kernel_files_release_io(files, &description, status);
}

enum kernel_files_status kernel_files_writev(struct kernel_files *files,
    struct kernel_mm *mm, int64_t fd, uint64_t user_iov, uint64_t iovcnt,
    int64_t *linux_result)
{
    return writev_request(files, mm, fd, user_iov, iovcnt, 0, 0, 0, linux_result);
}

enum kernel_files_status kernel_files_pwritev(struct kernel_files *files,
    struct kernel_mm *mm, int64_t fd, uint64_t user_iov, uint64_t iovcnt,
    int64_t offset, int64_t *linux_result)
{
    return writev_request(files, mm, fd, user_iov, iovcnt, 1, offset, 0, linux_result);
}

enum kernel_files_status kernel_files_pwritev2(struct kernel_files *files,struct kernel_mm *mm,
    int64_t fd,uint64_t iov,uint64_t count,int64_t offset,uint32_t flags,int64_t *result)
{ return writev_request(files,mm,fd,iov,count,offset!=-1,offset,flags,result); }

int64_t kernel_files_aio_sync(struct kernel_open_file_description *file,int data_only)
{
    int64_t result;
    if (sync_pinned(file,data_only,&result,NULL)) __builtin_trap();
    return result;
}

/* Keep the memory used by a single deferred transfer bounded. Larger
 * requests keep the existing streaming implementation. No user-page pin or
 * page-cache writer may survive into direct_transfer's mapping revocation. */
#define AIO_DIRECT_BYTES (64U * 1024U)
struct aio_destination_page {
    uint64_t owner;
    unsigned order;
    void *address;
    size_t length;
};
struct kernel_aio_direct {
    struct kernel_heap *heap;
    struct kernel_mm_record *destination;
    uint64_t offset;
    size_t size;
    unsigned pages, writing;
    void *staging;
    struct aio_destination_page spans[];
};

void kernel_files_aio_direct_free(struct kernel_aio_direct *direct)
{
    for (unsigned i=0;i<direct->pages;i++) {
        struct aio_destination_page *span=&direct->spans[i];
        if (physical_page_release_order(direct->heap->page_allocator,span->owner,span->order))
            __builtin_trap();
    }
    if (direct->destination) kernel_mm_aio_destination_end(direct->destination);
    if (direct->staging) kernel_heap_release(direct->heap,direct->staging);
    kernel_heap_release(direct->heap,direct);
}

static int capture_destination(struct kernel_aio_direct *direct,struct kernel_mm *mm,
    uint64_t address,size_t length)
{
    struct kernel_mm_user_pin pin={0};
    enum kernel_mm_status status=kernel_mm_pin_user_page(mm,address,KERNEL_MM_WRITE,&pin);
    if (status==KERNEL_MM_STATUS_NOT_MAPPED || status==KERNEL_MM_STATUS_ACCESS)
        status=kernel_mm_resolve_user_pin(mm,address,KERNEL_MM_WRITE,&pin);
    if (status) return 0;
    int captured=!pin.cache_write.entry;
    if (captured) {
        if (physical_page_acquire_order(mm->allocator,pin.owner_address,pin.order)) __builtin_trap();
        direct->spans[direct->pages++]=(struct aio_destination_page){
            .owner=pin.owner_address,.order=pin.order,
            .address=(char *)pin.page+(address&BOAROS_PAGE_MASK),.length=length};
    }
    kernel_mm_unpin_user_page(&pin);
    return captured;
}

static struct kernel_aio_direct *prepare_direct(struct kernel_open_file_description *file,
    struct kernel_mm *mm,const struct kernel_uaccess_iovec *iov,size_t elements,
    uint64_t count,uint64_t offset,int writing,uint32_t flags)
{
    size_t alignment=kernel_vfs_direct_alignment(&file->file);
    if (!count || count>AIO_DIRECT_BYTES || !alignment || offset%alignment || flags ||
        (writing && ((kernel_open_file_flags(file)&(KERNEL_FILES_O_APPEND|KERNEL_FILES_O_DSYNC)) ||
        kernel_task_current_fsize_limit()!=UINT64_MAX))) return NULL;
    size_t spans=0;
    for (size_t i=0;i<elements;i++) {
        if (iov[i].base%alignment || iov[i].length%alignment) return NULL;
        if (iov[i].length) spans+=1+((iov[i].base&BOAROS_PAGE_MASK)+iov[i].length-1)/BOAROS_PAGE_SIZE;
    }
    struct kernel_aio_direct *direct=NULL;
    size_t size=sizeof(*direct)+(writing?0:spans)*sizeof(struct aio_destination_page);
    if (kernel_heap_allocate_zeroed(file->heap,1,size,(void **)&direct)) return NULL;
    direct->heap=file->heap; direct->size=count; direct->offset=offset; direct->writing=writing;
    if ((writing || spans>1) && kernel_heap_allocate(file->heap,count,&direct->staging)) goto fail;
    if (writing) {
        size_t offset_in_buffer=0;
        for (size_t i=0;i<elements;i++) {
            size_t copied=0;
            if (kernel_copy_from_user(mm,(char *)direct->staging+offset_in_buffer,iov[i].base,
                    iov[i].length,&copied) || copied!=iov[i].length) goto fail;
            offset_in_buffer+=copied;
        }
    } else {
        if (!kernel_mm_aio_destination_begin(mm)) goto fail;
        direct->destination=mm->record;
        for (size_t i=0;i<elements;i++) {
            uint64_t address=iov[i].base,left=iov[i].length;
            while (left) {
                size_t length=BOAROS_PAGE_SIZE-(address&BOAROS_PAGE_MASK);
                if (length>left) length=left;
                if (!capture_destination(direct,mm,address,length)) goto fail;
                address+=length; left-=length;
            }
        }
    }
    return direct;
fail:
    kernel_files_aio_direct_free(direct);
    return NULL;
}

int64_t kernel_files_aio_direct_run(struct kernel_open_file_description *file,
    struct kernel_aio_direct *direct)
{
    size_t done=0;
    int error;
    if (direct->writing) {
        KERNEL_LOCK_SCOPE(operation);
        kernel_vfs_file_write_lock(&file->file,&operation);
        error=kernel_vfs_aio_direct_write(&file->file,direct->offset,direct->staging,
            direct->size,&done,&operation);
    } else {
        kernel_vfs_file_accessed(&file->file,kernel_open_file_flags(file));
        void *data=direct->staging?direct->staging:direct->spans[0].address;
        error=kernel_vfs_direct_pread(&file->file,direct->offset,data,direct->size,&done);
        if (direct->staging) {
            size_t copied=0;
            for (unsigned i=0;copied<done;i++) {
                size_t n=direct->spans[i].length;
                if (n>done-copied) n=done-copied;
                memcpy(direct->spans[i].address,(char *)data+copied,n);
                copied+=n;
            }
        }
        if (done) kernel_vfs_notify_file(&file->file,VFS_NOTIFY_ACCESS);
    }
    return done?(int64_t)done:error;
}

int kernel_files_aio_rw(struct kernel_files *files,struct kernel_mm *mm,
    struct kernel_open_file_description **owner,unsigned op,uint64_t buf,
    uint64_t count,int64_t offset,uint32_t flags,int64_t *result,struct kernel_aio_direct **deferred)
{
    struct kernel_open_file_description *file=*owner;
    int writing=op==1 || op==8, vector=op==7 || op==8;
    if (writing ? !kernel_files_description_writable(file) : !kernel_open_file_readable(file))
        return -KERNEL_EBADF;
    if (file->kind!=KERNEL_OPEN_FILE_KIND_REGULAR && !file->block) return -KERNEL_EINVAL;
    struct kernel_uaccess_iov_snapshot snapshot={0};
    struct kernel_uaccess_iovec scalar={buf,count};
    const struct kernel_uaccess_iovec *iov=&scalar;
    size_t elements=1;
    int error=0;
    if (vector) {
        if (count>1024) return -KERNEL_EINVAL;
        int64_t imported=0;
        if (kernel_uaccess_iov_import(&snapshot,files->heap,mm,buf,count,&imported)) __builtin_trap();
        if (imported) {error=(int)imported;goto out;}
        iov=snapshot.view; elements=snapshot.count; count=snapshot.total;
    } else {
        if (count>KERNEL_FILES_MAX_RW_COUNT) count=KERNEL_FILES_MAX_RW_COUNT;
        if (kernel_user_range_check(buf,count) || scalar_empty_range_fault(buf,count)) return -KERNEL_EFAULT;
        scalar.length=count;
    }
    if (offset<0 || count>(uint64_t)(INT64_MAX-offset)) {error=-KERNEL_EINVAL;goto out;}
    error=vector_request_error(file,(uint64_t)offset,count,flags);
    if (error) goto out;
    *result=0;
    if (file->kind==KERNEL_OPEN_FILE_KIND_REGULAR && (kernel_open_file_flags(file)&KERNEL_FILES_O_DIRECT)) {
        *deferred=prepare_direct(file,mm,iov,elements,count,(uint64_t)offset,writing,flags);
        if (*deferred) goto out;
    }
    if (count && writing) {
        if (write_request(files,mm,file,iov,elements,count,1,(uint64_t)offset,result,
                0,NULL,NULL,flags)) __builtin_trap();
        account_write(files,*result);
    } else if (count) {
        if (pread_iov_pinned(files,mm,owner,iov,elements,count,offset,!vector,result)) __builtin_trap();
        if (*result>0 && *owner) kernel_vfs_notify_file(&file->file,VFS_NOTIFY_ACCESS);
    }
out:
    kernel_uaccess_iov_cleanup(&snapshot,files->heap);
    return error;
}

static enum kernel_files_status lseek_pinned(
    struct kernel_open_file_description *description, int64_t offset,
    uint64_t whence, int64_t *linux_result,const struct kernel_lock_guard *borrowed)
{
    if (kernel_open_file_io_requires_cpu0(description) && whence >= 3U) {
        if (!regular_caller_ready()) return KERNEL_FILES_STATUS_STATE;
        KERNEL_LOCK_SCOPE(guard);
        kernel_mutex_lock(&description->offset_lock,&guard);
        struct regular_call call={.description=description,.offset=offset,.whence=whence,.result=linux_result,
            .offset_borrow=&guard};
        return regular_dispatch(&call,seek_cpu0);
    }
    int64_t current, target;
    KERNEL_LOCK_SCOPE(offset_guard);
    if (!regular_borrows_offset(description,borrowed))
        kernel_mutex_lock(&description->offset_lock, &offset_guard);
    if ((description->device && !description->device->positioned) ||
        kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_PIPE ||
        kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_SOCKET) {
        *linux_result = -KERNEL_ESPIPE;
        return KERNEL_FILES_STATUS_OK;
    }
    if (description->device && description->device->positioned) {
        (void)kernel_open_file_seek(description, 0U);
        *linux_result = 0;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_EPOLL ||
        (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_EVENTFD || kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_TIMERFD || kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_SIGNALFD)) {
        *linux_result = (int64_t)kernel_open_file_offset(description);
        return KERNEL_FILES_STATUS_OK;
    }
    current = (int64_t)kernel_open_file_offset(description);
    int control = kernel_vfs_file_is_control(&description->file, 0);
    if (control && (whence == 3U || whence == 4U)) {
        /* default_llseek treats sysctl i_size (zero) as the EOF hole. */
        if (offset >= 0) {
            *linux_result = -KERNEL_ENXIO;
            return KERNEL_FILES_STATUS_OK;
        }
        if (whence == 4U) offset = 0;
        whence = KERNEL_FILES_SEEK_SET;
    }
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_GENERATED &&
        !control && whence == KERNEL_FILES_SEEK_END) {
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    switch (whence) {
    case KERNEL_FILES_SEEK_SET:
        target = offset;
        break;
    case KERNEL_FILES_SEEK_CUR:
        if ((offset > 0 && current > INT64_MAX - offset) ||
            (offset < 0 && current < INT64_MIN - offset)) {
            *linux_result = -KERNEL_EINVAL;
            return KERNEL_FILES_STATUS_OK;
        }
        target = current + offset;
        break;
    case KERNEL_FILES_SEEK_END:
        current = (int64_t)kernel_open_file_size(description);
        if ((offset > 0 && current > INT64_MAX - offset) ||
            (offset < 0 && current < INT64_MIN - offset)) {
            *linux_result = -KERNEL_EINVAL;
            return KERNEL_FILES_STATUS_OK;
        }
        target = current + offset;
        break;
    case KERNEL_FILES_SEEK_DATA:
    case KERNEL_FILES_SEEK_HOLE: {
        if (kernel_open_file_kind(description)!=KERNEL_OPEN_FILE_KIND_REGULAR) {
            *linux_result=-KERNEL_EINVAL;
            return KERNEL_FILES_STATUS_OK;
        }
        uint64_t position;
        int error=kernel_vfs_seek_sparse(&description->file, (uint64_t)offset,
            whence==KERNEL_FILES_SEEK_HOLE, &position);
        if (error) {
            *linux_result=error;
            return KERNEL_FILES_STATUS_OK;
        }
        target=(int64_t)position;
        break;
    }
    default:
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    if (target < 0 || (description->block && (uint64_t)target > kernel_open_file_size(description))) {
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    if (description->kind == KERNEL_OPEN_FILE_KIND_GENERATED && !description->proc_page_stream && !control && target &&
        (uint64_t)target != description->generated_read_offset) {
        kernel_open_file_reset_generated(description);
        int error = kernel_open_file_generate(description);
        if (error) {
            (void)kernel_open_file_seek(description, 0);
            kernel_open_file_reset_generated(description);
            *linux_result = error;
            return KERNEL_FILES_STATUS_OK;
        }
        description->generated_read_offset = (uint64_t)target;
    }
    if (kernel_open_file_seek(description, (uint64_t)target) !=
        KERNEL_OPEN_FILE_STATUS_OK) {
        return KERNEL_FILES_STATUS_STATE;
    }
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_GENERATED &&
        target == 0)
        kernel_open_file_reset_generated(description);
    *linux_result = target;
    return KERNEL_FILES_STATUS_OK;
}

static void seek_cpu0(void *argument)
{
    struct regular_call *call=argument;
    call->status=lseek_pinned(call->description,call->offset,call->whence,call->result,call->offset_borrow);
}
enum kernel_files_status kernel_files_lseek(
    struct kernel_files *files, int64_t fd, int64_t offset,
    uint64_t whence, int64_t *linux_result)
{
    struct kernel_open_file_description *description = 0;
    if (!kernel_files_is_live(files) || !linux_result)
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    enum kernel_files_status status = kernel_files_pin_data(files, fd, &description, linux_result);
    if (status != KERNEL_FILES_STATUS_OK || *linux_result) return status;
    if(description->kind==KERNEL_OPEN_FILE_KIND_PIDFD) {
        *linux_result=-KERNEL_ESPIPE;
        return kernel_files_release_io(files,&description,KERNEL_FILES_STATUS_OK);
    }
    if (whence > 4U) {
        *linux_result = -KERNEL_EINVAL;
        return kernel_files_release_io(files, &description, KERNEL_FILES_STATUS_OK);
    }
    status = lseek_pinned(description, offset, whence, linux_result,NULL);
    return kernel_files_release_io(files, &description, status);
}

static enum kernel_files_status truncate_pinned(struct kernel_open_file_description *description,
    uint64_t length,int64_t *linux_result,const struct kernel_lock_guard *borrowed,
    struct kernel_vfs_size_limit *limit)
{
    if (kernel_vfs_file_metadata_requires_cpu0(&description->file)) {
        if (!regular_caller_ready()) return KERNEL_FILES_STATUS_STATE;
        KERNEL_LOCK_SCOPE(operation);
        kernel_vfs_file_write_lock(&description->file, &operation);
        struct regular_call call={.description=description,.length=length,.result=linux_result,
            .operation_borrow=&operation,.size_limit=limit};
        return regular_dispatch(&call,truncate_cpu0);
    }

    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_DIRECTORY) {
        *linux_result = -KERNEL_EISDIR;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_open_file_kind(description) != KERNEL_OPEN_FILE_KIND_REGULAR) {
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    if (!kernel_files_description_writable(description)) {
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    *linux_result=kernel_vfs_ftruncate_borrowing(&description->file,length,borrowed,limit);
    return KERNEL_FILES_STATUS_OK;
}
static void truncate_cpu0(void *argument)
{
    struct regular_call *call=argument;
    call->status=truncate_pinned(call->description,call->length,call->result,call->operation_borrow,call->size_limit);
}
enum kernel_files_status kernel_files_ftruncate(struct kernel_files *files,int64_t fd,
    uint64_t length,int64_t *linux_result)
{
    if (!kernel_files_is_live(files) || !linux_result) return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    struct kernel_open_file_description *description=0;
    enum kernel_files_status status=kernel_files_pin_data(files,fd,&description,linux_result);
    if (status || *linux_result) return status;
    struct kernel_vfs_size_limit limit={.maximum=kernel_task_current_fsize_limit()};
    status=truncate_pinned(description,length,linux_result,NULL,&limit);
    kernel_files_size_limit_notify(&limit);
    return kernel_files_release_io(files,&description,status);
}

enum kernel_files_status kernel_files_getdents(
    struct kernel_files *files,
    struct kernel_mm *mm,
    int64_t fd,
    uint64_t user_buffer,
    uint64_t count,
    int64_t *linux_result)
{
    struct kernel_open_file_description *description;
    unsigned char small[(19U + 256U + 7U) & ~(size_t)7U];
    struct kernel_task_io_buffer scratch = {0};
    uint64_t request, total = 0;
    enum kernel_files_status status = KERNEL_FILES_STATUS_OK;

    if (!kernel_files_is_live(files) || mm == 0 || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    KERNEL_FILES_PIN_SCOPE(pin_guard);
    description = kernel_files_hold_fd(files, fd, &pin_guard);
    if (description == 0) {
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_PATH) {
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_open_file_kind(description) !=
        KERNEL_OPEN_FILE_KIND_DIRECTORY) {
        *linux_result = -KERNEL_ENOTDIR;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_user_range_check(user_buffer, (size_t)count) !=
        KERNEL_UACCESS_STATUS_OK) {
        *linux_result = -KERNEL_EFAULT;
        return KERNEL_FILES_STATUS_OK;
    }
    request = count > KERNEL_FILES_MAX_RW_COUNT
                  ? KERNEL_FILES_MAX_RW_COUNT
                  : count;

    KERNEL_SERVICE_SCOPE_IF(service, kernel_vfs_namespace_requires_cpu0(description->file.mount));
    KERNEL_LOCK_SCOPE(offset_guard);
    kernel_mutex_lock(&description->offset_lock, &offset_guard);
    uint32_t open_flags = kernel_open_file_flags(description);
    const uint32_t *access_flags = &open_flags;
    /* Reuse the task's retained I/O page, not a page on the single-page
     * kernel stack. Allocation failure falls back to the same bounded path. */
    size_t capacity = sizeof(small);
    unsigned char *data = small;
    if (request > capacity &&
        kernel_task_io_buffer_acquire(&scratch, mm->allocator) == KERNEL_TASK_STATUS_OK) {
        data = scratch.data;
        capacity = BOAROS_PAGE_SIZE;
    }
    do {
        uint64_t remaining = request - total;
        uint64_t position = description->offset;
        struct kernel_vfs_dir_batch batch = {
            .data = data,
            .capacity = remaining < capacity ? (size_t)remaining : capacity,
            .position = position,
        };
        int fill_result = kernel_vfs_read_directory(&description->file, &batch, access_flags);
        access_flags = NULL;
        size_t copied = 0, complete = 0;
        enum kernel_uaccess_status access = KERNEL_UACCESS_STATUS_OK;
        /* Drop all namespace/backend locks before faulting user memory.
         * offset_lock still serializes dup/fork readers and directory seeks. */
        if (batch.used)
            access = kernel_copy_to_user(mm, user_buffer + total, data, batch.used, &copied);
        if ((access != KERNEL_UACCESS_STATUS_OK && access != KERNEL_UACCESS_STATUS_FAULT) ||
            copied > batch.used || (!access && copied != batch.used)) {
            status = KERNEL_FILES_STATUS_STATE;
            break;
        }
        if (access == KERNEL_UACCESS_STATUS_OK) {
            complete = batch.used;
            position = batch.position;
        } else {
            /* A fault can split any record. Only complete records count;
             * the failed record's cookie is retried even if bytes escaped. */
            while (complete < copied) {
                size_t length = (size_t)data[complete + 16] | ((size_t)data[complete + 17] << 8);
                if (length > copied - complete) break;
                position = 0;
                for (unsigned byte = 0; byte < 8; byte++)
                    position |= (uint64_t)data[complete + 8 + byte] << (8U * byte);
                complete += length;
            }
        }
        /* Directory cookies use the same offset lock as seek. Committing a
         * read here does not seek or reset regular-file read prediction. */
        description->offset = position;
        total += complete;
        if (access == KERNEL_UACCESS_STATUS_FAULT) {
            *linux_result = total ? (int64_t)total : -KERNEL_EFAULT;
            break;
        }
        if (fill_result <= 0) {
            *linux_result = total ? (int64_t)total : fill_result;
            if (!fill_result) kernel_vfs_notify_file(&description->file, VFS_NOTIFY_ACCESS);
            break;
        }
        if (!batch.used) {
            *linux_result = total ? (int64_t)total : -KERNEL_EINVAL;
            break;
        }
        if (total == request) {
            *linux_result = (int64_t)total;
            kernel_vfs_notify_file(&description->file, VFS_NOTIFY_ACCESS);
            break;
        }
        /* A short user buffer ends the call. A full scratch batch instead
         * continues from the last accepted opaque cookie, never from zero. */
        if (remaining <= capacity || request - total < 24U) {
            *linux_result = (int64_t)total;
            break;
        }
    } while (total < request);
    if (scratch.allocator) kernel_task_io_buffer_release(&scratch);
    KERNEL_FILES_NOTE(files, bytes_read, total);
    return status;
}

#if BOAROS_COST_DIAGNOSTICS
int kernel_files_cost_descriptor(struct kernel_files *files, int64_t fd)
{
    KERNEL_FILES_PIN_SCOPE(pin);
    struct kernel_open_file_description *description = kernel_files_hold_fd(files, fd, &pin);
    return description && kernel_vfs_file_is_cost(&description->file);
}
#endif
