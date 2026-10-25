#include "private.h"
#include "../open_file_internal.h"
#include "../pipe_internal.h"
#include "../char_device_internal.h"
#include "../block_file_internal.h"
#include "../uaccess_iov_internal.h"
#include "../../mm/uaccess_internal.h"
#include "../vfs_internal.h"

#include <kernel/cost.h>
#include <kernel/credentials.h>
#include <kernel/cpu.h>
#include <kernel/errno.h>
#include <kernel/event_fd.h>
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
static enum kernel_files_status pipe_user_result(int64_t bytes,unsigned broken)
{
    struct kernel_task *task=kernel_task_current();
    if (kernel_task_has_user_context(task)) {
        if (broken && kernel_signal_send_task(task,13,0)!=KERNEL_SIGNAL_STATUS_OK)
            return KERNEL_FILES_STATUS_STATE;
        if (bytes==-KERNEL_ERESTARTSYS) kernel_signal_note_syscall_restart(task);
    }
    return KERNEL_FILES_STATUS_OK;
}

static enum kernel_files_status release_io_description(
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

/* Sysctl reads commit the offset only after the complete generated prefix was
 * copied. Unlike ordinary proc snapshots, every offset-zero read is fresh. */
static enum kernel_files_status control_read(
    struct kernel_files *files, struct kernel_mm *mm,
    struct kernel_open_file_description *description,
    struct kernel_uaccess_iov_cursor *cursor, uint64_t count,
    uint64_t offset, int advance, int64_t *linux_result)
{
    char buffer[32];
    size_t length = 0, copied = 0;
    int result = kernel_vfs_file_control(&description->file, 0, offset, buffer,
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
    if (advance && kernel_open_file_advance(description, length) != KERNEL_OPEN_FILE_STATUS_OK)
        return KERNEL_FILES_STATUS_STATE;
    KERNEL_FILES_NOTE(files, bytes_read, length);
    *linux_result = (int64_t)length;
    return KERNEL_FILES_STATUS_OK;
}

/* The backend builds a stable OFD snapshot before any user copy. A fault
 * advances only bytes copied, and seeking to zero starts a new snapshot. */
static enum kernel_files_status generated_read(
    struct kernel_files *files, struct kernel_mm *mm,
    struct kernel_open_file_description *description,
    struct kernel_uaccess_iov_cursor *cursor, uint64_t count,
    uint64_t offset, int advance, int64_t *linux_result)
{
    if (kernel_vfs_file_is_control(&description->file))
        return control_read(files, mm, description, cursor, count, offset, advance, linux_result);
    int result = kernel_open_file_generate(description);
    if (result) {
        *linux_result = result;
        KERNEL_FILES_NOTE(files, read_failures, 1);
        return KERNEL_FILES_STATUS_OK;
    }
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
    struct kernel_task_io_buffer buffer = {0};
    uint64_t total = 0;
    int error = 0;
    if (offset >= kernel_open_file_size(description)) { *linux_result = 0; return KERNEL_FILES_STATUS_OK; }
    if (kernel_task_io_buffer_acquire(&buffer, mm->allocator) != KERNEL_TASK_STATUS_OK) {
        *linux_result = -KERNEL_ENOMEM; KERNEL_FILES_NOTE(files, read_failures, 1);
        return KERNEL_FILES_STATUS_OK;
    }
    while (total < count) {
        size_t chunk = count - total > BOAROS_PAGE_SIZE ? BOAROS_PAGE_SIZE : (size_t)(count - total);
        size_t available = 0, copied = 0;
        error = kernel_open_file_pread(description, offset + total, buffer.data, chunk, &available);
        if (error || !available) break;
        enum kernel_uaccess_status status = kernel_copy_to_user_iov(mm, cursor, buffer.data, available, &copied);
        if (advance && kernel_open_file_advance(description, copied) != KERNEL_OPEN_FILE_STATUS_OK) __builtin_trap();
        total += copied;
        KERNEL_FILES_NOTE(files, read_chunks, 1);
        if (status == KERNEL_UACCESS_STATUS_FAULT) { error = -KERNEL_EFAULT; break; }
        if (status != KERNEL_UACCESS_STATUS_OK || copied != available) __builtin_trap();
    }
    kernel_task_io_buffer_release(&buffer);
    if (error) KERNEL_FILES_NOTE(files, read_failures, 1);
    KERNEL_FILES_NOTE(files, bytes_read, total);
    *linux_result = total ? (int64_t)total : error;
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
    return release_io_description(files,&description,status);
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
    return release_io_description(files, &description, KERNEL_FILES_STATUS_OK);
}

/* 缓存批次只覆盖剩余需求；复制前已经结束存储锁，整个批次的成功页都有独立引用。 */
static enum kernel_files_status disk_read(
    struct kernel_files *files, struct kernel_mm *mm,
    struct kernel_open_file_description *description,
    struct kernel_uaccess_iov_cursor *cursor, uint64_t request,
    uint64_t position, int advance, int64_t *linux_result
    )
{
    struct physical_page_allocator *data_allocator=kernel_open_file_page_cache(description)->allocator;
    uint64_t total = 0;
    int error = 0, done = 0;
    enum kernel_files_status status = KERNEL_FILES_STATUS_OK;
    while (total < request && !done) {
        size_t start = position & BOAROS_PAGE_MASK;
        size_t count = (start + request - total + BOAROS_PAGE_MASK) >> BOAROS_PAGE_SHIFT;
        if (count > KERNEL_PAGE_CACHE_READ_BATCH_MAX) count = KERNEL_PAGE_CACHE_READ_BATCH_MAX;
        struct kernel_page_cache_read_page pages[KERNEL_PAGE_CACHE_READ_BATCH_MAX];
        enum kernel_page_cache_status loaded;
        loaded = kernel_page_cache_get_batch(
            kernel_open_file_page_cache(description), &description->file,
            position >> BOAROS_PAGE_SHIFT, pages, count);
        if (loaded == KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT)
            return KERNEL_FILES_STATUS_STATE;
        struct kernel_uaccess_write_cursor user_copy={0};
        for (size_t i = 0; i < count; i++) {
            struct kernel_page_cache_read_page *part = &pages[i];
            KERNEL_FILES_NOTE(files, read_chunks, 1);
            if (part->status == KERNEL_PAGE_CACHE_STATUS_OUT_OF_RANGE ||
                (!part->status && start >= part->valid_bytes)) { done = 1; break; }
            if (part->status) {
                error = part->status == KERNEL_PAGE_CACHE_STATUS_NO_MEMORY ? -KERNEL_ENOMEM : -KERNEL_EIO;
                done = 1; break;
            }
            void *page;
            if (physical_page_map_owned(data_allocator, part->physical_address, &page)) {
                status = KERNEL_FILES_STATUS_STATE; done = 1; break;
            }
            size_t chunk = part->valid_bytes - start, copied = 0;
            if (chunk > request - total) chunk = request - total;
            enum kernel_uaccess_status access = kernel_copy_to_user_iov_cursor(mm, cursor,&user_copy,
                (unsigned char *)page + start, chunk, &copied);
            if (copied > chunk || (advance && kernel_open_file_advance(description, copied))) {
                status = KERNEL_FILES_STATUS_STATE; done = 1; break;
            }
            kernel_open_file_read_progress(description, position, copied);
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
        /* 包括错误或user fault之后未消费的成功页；不能靠下一次请求归还。 */
        kernel_uaccess_write_cursor_finish(&user_copy);
        for (size_t i = 0; i < count; i++) if (!pages[i].status)
            if (physical_page_release(data_allocator, pages[i].physical_address)) __builtin_trap();
    }
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
    struct kernel_socket_address *peer, uint32_t *message_size, struct kernel_socket_message *socket_message)
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
    if(kind==KERNEL_OPEN_FILE_KIND_PIDFD) {
        *linux_result=-KERNEL_EINVAL;return KERNEL_FILES_STATUS_OK;
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
    KERNEL_SERVICE_SCOPE_IF(service, kind != KERNEL_OPEN_FILE_KIND_REGULAR && kind != KERNEL_OPEN_FILE_KIND_SOCKET && kind != KERNEL_OPEN_FILE_KIND_PIPE);
    KERNEL_LOCK_SCOPE(offset_guard);
    if (kind == KERNEL_OPEN_FILE_KIND_REGULAR || kind == KERNEL_OPEN_FILE_KIND_GENERATED)
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
        struct kernel_socket_read_request read_request = {0};
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
        while (total < count || (message && total == 0)) {
            uint64_t remaining = count - total;
            received = kernel_socket_reserve_read(
                kernel_open_file_socket(description), kernel_task_current(),
                &read_request, description_owner,
                remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining,
                socket_flags & KERNEL_SOCKET_MSG_DONTWAIT, socket_message);
            if (received == -KERNEL_EAGAIN && total == 0U && !(socket_flags & KERNEL_SOCKET_MSG_DONTWAIT)) {
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
                    kernel_socket_wait_ready(kernel_open_file_socket(description), KERNEL_POLLIN, target, socket_flags);
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
            /* 本批已覆盖当时可读前缀；不追读新数据或消费下一次应返回的错误。 */
            break;
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
        while (total < request) {
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

        if (pipe_status != KERNEL_PIPE_STATUS_OK || pipe_user_result(*linux_result,0)) {
            return KERNEL_FILES_STATUS_STATE;
        }
        if (*linux_result > 0 && description->file.private_data)
            kernel_vfs_file_accessed(&description->file);
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
    if (count == 0U) {
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
        kernel_vfs_file_accessed(&description->file);
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
        return release_io_description(files,&description,KERNEL_FILES_STATUS_OK);
    }
    status = read_pinned(files, mm, &description, &iov, 1U, count, 1,
                         linux_result, 0U, 0, 0, NULL);
    return release_io_description(files, &description, status);
}

static enum kernel_files_status pread_iov_pinned(
    struct kernel_files *files, struct kernel_mm *mm,
    struct kernel_open_file_description **description_owner,
    const struct kernel_uaccess_iovec *iov, size_t iov_count,
    uint64_t count, int64_t offset, int scalar, int64_t *linux_result);

static enum kernel_files_status readv_request(
    struct kernel_files *files,
    struct kernel_mm *mm,
    int64_t fd,
    uint64_t user_iov,
    uint64_t iovcnt,
    int positioned, int64_t offset, int64_t *linux_result)
{
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
    KERNEL_SERVICE_SCOPE_IF(service, kernel_open_file_kind(description) != KERNEL_OPEN_FILE_KIND_REGULAR && kernel_open_file_kind(description) != KERNEL_OPEN_FILE_KIND_SOCKET && kernel_open_file_kind(description) != KERNEL_OPEN_FILE_KIND_PIPE && kernel_open_file_kind(description) != KERNEL_OPEN_FILE_KIND_EVENTFD && kernel_open_file_kind(description) != KERNEL_OPEN_FILE_KIND_TIMERFD && kernel_open_file_kind(description) != KERNEL_OPEN_FILE_KIND_SIGNALFD);
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
                         snapshot.total, 0, linux_result, 0U, 0, 0, NULL);
    dispatched = 1;
out:
    kernel_uaccess_iov_cleanup(&snapshot,files->heap);
    if (!dispatched && status == KERNEL_FILES_STATUS_OK &&
        *linux_result < 0) {
        KERNEL_FILES_NOTE(files, read_failures, 1);
    }
    return release_io_description(files, &description, status);
}


enum kernel_files_status kernel_files_readv(struct kernel_files *files,
    struct kernel_mm *mm, int64_t fd, uint64_t user_iov, uint64_t iovcnt,
    int64_t *linux_result)
{
    return readv_request(files, mm, fd, user_iov, iovcnt, 0, 0, linux_result);
}

enum kernel_files_status kernel_files_preadv(struct kernel_files *files,
    struct kernel_mm *mm, int64_t fd, uint64_t user_iov, uint64_t iovcnt,
    int64_t offset, int64_t *linux_result)
{
    return readv_request(files, mm, fd, user_iov, iovcnt, 1, offset, linux_result);
}

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
    KERNEL_SERVICE_SCOPE_IF(service, kind != KERNEL_OPEN_FILE_KIND_REGULAR && kind != KERNEL_OPEN_FILE_KIND_SOCKET && kind != KERNEL_OPEN_FILE_KIND_PIPE && kind != KERNEL_OPEN_FILE_KIND_EVENTFD && kind != KERNEL_OPEN_FILE_KIND_TIMERFD && kind != KERNEL_OPEN_FILE_KIND_SIGNALFD);
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
    if (count == 0U) {
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
                           linux_result, 0U, 0, 0, NULL);
    }
    if (kind == KERNEL_OPEN_FILE_KIND_REGULAR) {
        kernel_open_file_read_begin(description, (uint64_t)offset);
        kernel_vfs_file_accessed(&description->file);
    }
    request = count > KERNEL_FILES_MAX_RW_COUNT
                  ? KERNEL_FILES_MAX_RW_COUNT
                  : count;
    position = (uint64_t)offset;
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
    return release_io_description(files, &description, status);
}


static int description_writable(
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

static int64_t sendfile_output(struct kernel_open_file_description **output_owner,
                               const void *buffer, size_t size,
                               uint64_t position, uint64_t deadline, uint32_t socket_flags,
                               struct kernel_vfs_size_limit *limit, int first)
{
    struct kernel_open_file_description *output = *output_owner;
    enum kernel_open_file_kind kind = kernel_open_file_kind(output);
    if (kind == KERNEL_OPEN_FILE_KIND_PIPE) {
        struct kernel_pipe_result completed;
        if (kernel_pipe_write_buffer(output->pipe, buffer, size,
                kernel_open_file_flags(output), &completed) != KERNEL_PIPE_STATUS_OK ||
            pipe_user_result(completed.bytes,completed.broken)) __builtin_trap();
        int64_t result=completed.bytes;
        if (result > 0 && output->file.private_data &&
            !kernel_vfs_mount_is_readonly(output->file.mount)) {
            int error = kernel_vfs_file_modified(&output->file, 0, 0);
            if (error) return error;
        }
        return result;
    }
    if (kind == KERNEL_OPEN_FILE_KIND_SOCKET) {
        struct kernel_socket *socket = kernel_open_file_socket(output);
        struct kernel_socket_write_request request = {0};
        int tcp = kernel_socket_is_tcp(socket), result;
        if (tcp) kernel_socket_stream_begin(&request, output_owner);
        for (;;) {
            if (tcp) {
                result = kernel_socket_stream_reserve(&request, (uint32_t)size, socket_flags);
                if (result > 0) result = kernel_socket_stream_commit(&request, buffer, (uint32_t)result, socket_flags);
            } else result = kernel_socket_write_packet_buffer(output_owner, buffer, size, socket_flags);
            if (result != -KERNEL_EAGAIN || (socket_flags & KERNEL_SOCKET_MSG_DONTWAIT)) break;
            uint64_t now = deadline ? kernel_time_monotonic_ns() : 0;
            int waited = deadline && now >= deadline ? -KERNEL_EAGAIN :
                kernel_socket_wait_ready(socket, KERNEL_POLLOUT, deadline, socket_flags);
            if (waited != 0) { result = waited; break; }
        }
        if (tcp) kernel_socket_stream_finish(&request);
        else if (result == -KERNEL_EPIPE && kernel_socket_is_stream(socket) &&
                 !(socket_flags & KERNEL_SOCKET_MSG_NOSIGNAL))
            (void)kernel_signal_send_task(kernel_task_current(), 13U, 0);
        return result;
    }

    size_t written = 0;
    int error = 0;
    if (kind == KERNEL_OPEN_FILE_KIND_REGULAR) {
        size_t total = 0;
        /* splice会继续提交未完成的buffer；限额前缀成功后，剩余范围拒绝并产生SIGXFSZ。 */
        while (total < size) {
            size_t allowed = size - total;
            error = kernel_vfs_check_write_limit(limit, position + total, &allowed);
            if (error) break;
            if (first && !total) {
                error = kernel_vfs_file_modified(&output->file, position, 0);
                if (error) break;
            }
            /* 跨输入读取不持目标门闩；pwrite每批自取rank15。 */
            error = kernel_vfs_pwrite(&output->file, position + total,
                (const unsigned char *)buffer + total, allowed, &written);
            if (written > allowed) __builtin_trap();
            total += written;
            if (error || written < allowed) break;
        }
        return total ? (int64_t)total : error;
    } else if (output->device && output->device->write) {
        error = output->device->write(output->device_instance, kernel_task_current(), kernel_open_file_flags(output), buffer, size, &written);
    } else return -KERNEL_EINVAL;
    if (written > size) __builtin_trap();
    return written != 0U ? (int64_t)written : error;
}

static int64_t sendfile_pinned(struct kernel_files *files, struct kernel_mm *mm,
    struct kernel_open_file_description *input,
    struct kernel_open_file_description **output_owner,
    int explicit_offset, int64_t *position, uint64_t count)
{
    struct kernel_open_file_description *output = *output_owner;
    uint32_t socket_flags = output->kind == KERNEL_OPEN_FILE_KIND_SOCKET &&
        (kernel_open_file_flags(output) & KERNEL_FILES_O_NONBLOCK) ? KERNEL_SOCKET_MSG_DONTWAIT : 0;
    KERNEL_LOCK_SCOPE(first_guard);
    KERNEL_LOCK_SCOPE(second_guard);
    struct kernel_open_file_description *first =
        !explicit_offset && input->kind == KERNEL_OPEN_FILE_KIND_REGULAR ? input : 0;
    struct kernel_open_file_description *second =
        output->kind == KERNEL_OPEN_FILE_KIND_REGULAR && output != first ? output : 0;
    if (first != 0 && second != 0 && (uintptr_t)first > (uintptr_t)second) {
        struct kernel_open_file_description *swap = first; first = second; second = swap;
    }
    /* rank 10 的 OFD 锁按 key 取得；同一 OFD 只有一个锁 owner。 */
    if (first != 0) kernel_mutex_lock(&first->offset_lock, &first_guard);
    if (second != 0) kernel_mutex_lock(&second->offset_lock, &second_guard);
    uint64_t start = explicit_offset ? (uint64_t)*position : input->offset;
    uint64_t out_start = output->offset, total = 0;
    struct kernel_vfs_size_limit size_limit = {.maximum=kernel_task_current_fsize_limit()};
    int error = 0;
    if (count > (uint64_t)INT64_MAX || start > (uint64_t)INT64_MAX - count)
        return -KERNEL_EINVAL;
    if (count > KERNEL_FILES_MAX_RW_COUNT) count = KERNEL_FILES_MAX_RW_COUNT;
    uint64_t limit = input->kind == KERNEL_OPEN_FILE_KIND_REGULAR
        ? kernel_vfs_file_max_size(&input->file) : (uint64_t)INT64_MAX;
    if (output->kind == KERNEL_OPEN_FILE_KIND_REGULAR) {
        uint64_t out_limit = kernel_vfs_file_max_size(&output->file);
        if (limit > out_limit) limit = out_limit;
    }
    if (start > limit || (count != 0U && start == limit)) return -KERNEL_EOVERFLOW;
    if (count > limit - start) count = limit - start;
    if (output->kind != KERNEL_OPEN_FILE_KIND_PIPE) {
        if (out_start > (uint64_t)INT64_MAX - count) return -KERNEL_EINVAL;
        if (kernel_open_file_flags(output) & KERNEL_FILES_O_APPEND) return -KERNEL_EINVAL;
    }
    if (input->kind != KERNEL_OPEN_FILE_KIND_REGULAR) return -KERNEL_EINVAL;
    if (count) kernel_open_file_read_begin(input, start);
    kernel_vfs_file_accessed(&input->file);
    if (count == 0U || start >= kernel_open_file_size(input)) return 0;
    int datagram=kernel_socket_preserves_messages(kernel_open_file_socket(output));
    int direct=output->kind==KERNEL_OPEN_FILE_KIND_PIPE ||
        (output->kind==KERNEL_OPEN_FILE_KIND_SOCKET && !datagram);
    struct kernel_task_io_buffer source_buffer = {0};
    if (!direct && !datagram && kernel_task_io_buffer_acquire(&source_buffer, mm->allocator) != KERNEL_TASK_STATUS_OK)
        return -KERNEL_ENOMEM;
    size_t capacity = BOAROS_PAGE_SIZE;
    void *staging = source_buffer.data, *datagram_staging = 0;
    if (datagram) {
        /* Linux splice_to_socket batches at most sixteen target pages. */
        size_t batch=(size_t)BOAROS_PAGE_SIZE*16U;
        capacity=count>batch ? batch : (size_t)count;
        enum kernel_heap_status allocated = kernel_heap_allocate(files->heap, capacity, &datagram_staging);
        if (allocated != KERNEL_HEAP_STATUS_OK) {
            if (source_buffer.allocator) kernel_task_io_buffer_release(&source_buffer);
            if (allocated != KERNEL_HEAP_STATUS_EMPTY) __builtin_trap();
            return -KERNEL_ENOMEM;
        }
        staging = datagram_staging;
    }
    uint64_t timeout = output->kind == KERNEL_OPEN_FILE_KIND_SOCKET && !(socket_flags & KERNEL_SOCKET_MSG_DONTWAIT)
        ? kernel_socket_send_timeout(kernel_open_file_socket(output)) : 0;
    uint64_t deadline = 0;
    if (timeout != 0U) {
        uint64_t now = kernel_time_monotonic_ns();
        deadline = UINT64_MAX - now < timeout ? UINT64_MAX : now + timeout;
    }
    while (total < count) {
        size_t wanted = count - total > capacity
            ? capacity : (size_t)(count - total), available = 0;
        if (direct) {
            uint64_t position=start+total,physical;
            size_t offset=position&BOAROS_PAGE_MASK,valid;
            kernel_open_file_read_begin(input,position);
            int memory=kernel_open_file_memory_backed(input);
            enum kernel_page_cache_status loaded=memory
                ?kernel_open_file_lookup_page(input,position>>BOAROS_PAGE_SHIFT,&physical,&valid)
                :kernel_open_file_get_page(input,position>>BOAROS_PAGE_SHIFT,&physical,&valid);
            if (loaded==KERNEL_PAGE_CACHE_STATUS_NOT_FOUND && memory) {
                uint64_t size=kernel_open_file_size(input);
                if (position>=size) break;
                available=BOAROS_PAGE_SIZE-offset;
                if (available>wanted) available=wanted;
                if (available>size-position) available=(size_t)(size-position);
                /* 稀疏内存文件的洞只生成当前零片段，不为读取建立持久文件页。 */
                if (kernel_task_io_buffer_acquire(&source_buffer,mm->allocator)) {
                    error=-KERNEL_ENOMEM;break;
                }
                staging=source_buffer.data;
                memset(staging,0,available);
            } else if (loaded==KERNEL_PAGE_CACHE_STATUS_OK) {
                /* 页引用跨目标等待保活；TCP仍COPY进ACK前自有的协议版本。 */
                kernel_task_io_buffer_take_page(&source_buffer,files->heap->page_allocator,physical);
                if (offset<valid) {
                    available=valid-offset;
                    if (available>wanted) available=wanted;
                }
                staging=(unsigned char *)source_buffer.data+offset;
            } else {
                if (loaded!=KERNEL_PAGE_CACHE_STATUS_OUT_OF_RANGE)
                    error=loaded==KERNEL_PAGE_CACHE_STATUS_NO_MEMORY?-KERNEL_ENOMEM:-KERNEL_EIO;
                break;
            }
            kernel_open_file_read_progress(input,position,available);
        } else {
            /* 文件目标保留分离的读写版本，允许同inode及重叠区间复制。 */
            error=kernel_open_file_pread(input,start+total,staging,wanted,&available);
            if (available>wanted) __builtin_trap();
        }
        if (!available) {
            if (direct && source_buffer.allocator) kernel_task_io_buffer_release(&source_buffer);
            break;
        }
        int64_t sent = sendfile_output(output_owner, staging, available,
                                       out_start + total, deadline, socket_flags, &size_limit, total == 0);
        if (direct) kernel_task_io_buffer_release(&source_buffer);
        if (sent < 0) { error = (int)sent; break; }
        if ((uint64_t)sent > available) __builtin_trap();
        if (sent > 0 && output->kind == KERNEL_OPEN_FILE_KIND_REGULAR &&
            (kernel_open_file_flags(output) & KERNEL_FILES_O_DSYNC) != 0U) {
            int datasync = (kernel_open_file_flags(output) & KERNEL_FILES_O_SYNC) != KERNEL_FILES_O_SYNC;
            int synced = kernel_vfs_sync(&output->file, datasync, &output->observed_writeback_error);
            if (synced != 0) { error = synced; break; }
        }
        total += (uint64_t)sent;
        if ((size_t)sent < available || error != 0) break;
    }
    if (datagram_staging) kernel_heap_release(files->heap, datagram_staging);
    if (source_buffer.allocator) kernel_task_io_buffer_release(&source_buffer);
    kernel_files_size_limit_notify(&size_limit);
    if (total != 0U && error == -KERNEL_ERESTARTSYS)
        kernel_signal_clear_syscall_restart(kernel_task_current());
    if (total != 0U) {
        /* Linux 先发布输出位置再输入位置；NULL offset 的 alias 只推进一次。 */
        if (output->kind == KERNEL_OPEN_FILE_KIND_REGULAR && (explicit_offset || output != input))
            output->offset = out_start + total;
        if (explicit_offset) *position = (int64_t)(start + total);
        else input->offset = start + total;
        return (int64_t)total;
    }
    return error;
}

enum kernel_files_status kernel_files_sendfile(
    struct kernel_files *files, struct kernel_mm *mm, int64_t out_fd,
    int64_t in_fd, uint64_t user_offset, uint64_t count, int64_t *linux_result)
{
    KERNEL_SERVICE_SCOPE_IF(service, 0);
    COST_SCOPE(sendfile_cost, OPERATION_TICKS);
    struct kernel_open_file_description *input = 0, *output = 0;
    int64_t position = 0;
    size_t copied = 0;
    if (!kernel_files_is_live(files) || mm == 0 || linux_result == 0)
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    if (user_offset != 0U && (kernel_copy_from_user(mm, &position, user_offset,
            sizeof(position), &copied) != KERNEL_UACCESS_STATUS_OK || copied != sizeof(position))) {
        *linux_result = -KERNEL_EFAULT; return KERNEL_FILES_STATUS_OK;
    }
    enum kernel_files_status status = kernel_files_pin_data(files, in_fd, &input, linux_result);
    KERNEL_FILES_NOTE(files, read_calls, 1);
    KERNEL_FILES_NOTE(files, write_calls, 1);
    if (status != KERNEL_FILES_STATUS_OK || *linux_result != 0) goto done;
    if (!kernel_open_file_readable(input)) { *linux_result = -KERNEL_EBADF; goto done; }
    if (user_offset != 0U && (input->kind == KERNEL_OPEN_FILE_KIND_PIPE ||
            input->kind == KERNEL_OPEN_FILE_KIND_SOCKET || (input->device && !input->device->positioned))) {
        *linux_result = -KERNEL_ESPIPE; goto done;
    }
    uint64_t checked_position = (uint64_t)position;
    if (!user_offset) {
        /* Linux checks the input range before looking up out_fd. Snapshot the
         * shared offset here; sendfile_pinned locks and rechecks before I/O. */
        KERNEL_LOCK_SCOPE(snapshot);
        kernel_mutex_lock(&input->offset_lock, &snapshot);
        checked_position = input->offset;
    }
    if (position < 0 || count > (uint64_t)INT64_MAX ||
        checked_position > (uint64_t)INT64_MAX - count) {
        *linux_result = -KERNEL_EINVAL; goto done;
    }
    status = kernel_files_pin_data(files, out_fd, &output, linux_result);
    if (status != KERNEL_FILES_STATUS_OK || *linux_result != 0) goto done;
    if (!description_writable(output)) { *linux_result = -KERNEL_EBADF; goto done; }
    if (input->kind != KERNEL_OPEN_FILE_KIND_REGULAR ||
        (output->kind != KERNEL_OPEN_FILE_KIND_SOCKET && output->kind != KERNEL_OPEN_FILE_KIND_PIPE))
        kernel_service_scope_begin(&service);
    /* 两份 pin 由仍会恢复的 syscall 栈持有；退出先唤醒等待，正常展开再交还。 */
    *linux_result = sendfile_pinned(files, mm, input, &output, user_offset != 0U, &position, count);
    if (*linux_result > 0) {
        KERNEL_FILES_NOTE(files, bytes_read, (uint64_t)*linux_result);
        KERNEL_FILES_NOTE(files, bytes_written, (uint64_t)*linux_result);
    }
done:
    if (output != 0) status = release_io_description(files, &output, status);
    if (input != 0) status = release_io_description(files, &input, status);
    if (status != KERNEL_FILES_STATUS_OK) return status;
    /* 即使 do_sendfile 已失败也写回 offset；只读指针的 fault 覆盖先前结果。 */
    copied = 0;
    if (user_offset != 0U && (kernel_copy_to_user(mm, user_offset, &position,
            sizeof(position), &copied) != KERNEL_UACCESS_STATUS_OK || copied != sizeof(position)))
        *linux_result = -KERNEL_EFAULT;
    return KERNEL_FILES_STATUS_OK;
}

static enum kernel_files_status control_write(
    struct kernel_files *files, struct kernel_mm *mm,
    struct kernel_open_file_description *description,
    const struct kernel_uaccess_iovec *iov, size_t iov_count, uint64_t count,
    int positioned, uint64_t requested_offset, int64_t *linux_result)
{
    char *buffer = 0;
    size_t copied = 0, accepted = 0;
    int result = 0;
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
    if (!result) result = kernel_vfs_file_control(&description->file, 1,
        positioned ? requested_offset : kernel_open_file_offset(description),
        buffer, (size_t)count, &accepted);
    kernel_heap_release(files->heap, buffer);
    if (result) { *linux_result = result; return KERNEL_FILES_STATUS_OK; }
    if (!positioned && kernel_open_file_advance(description, accepted) != KERNEL_OPEN_FILE_STATUS_OK)
        return KERNEL_FILES_STATUS_STATE;
    *linux_result = (int64_t)accepted;
    return KERNEL_FILES_STATUS_OK;
}

static enum kernel_files_status regular_write_request(
    struct kernel_files *files,struct kernel_mm *mm,
    struct kernel_open_file_description *description,
    const struct kernel_uaccess_iovec *iov,size_t iov_count,
    uint64_t count,int positioned,uint64_t requested_offset,int64_t *linux_result)
{
    if (!count) { *linux_result=0; return KERNEL_FILES_STATUS_OK; }
    int append=(kernel_open_file_flags(description)&KERNEL_FILES_O_APPEND)!=0;
    uint64_t offset=positioned?requested_offset:kernel_open_file_offset(description);
    struct kernel_vfs_size_limit size_limit={.maximum=kernel_task_current_fsize_limit()};
    struct kernel_vfs_user_write_result completed={0};
    int error=kernel_vfs_write_user(&description->file,mm,iov,iov_count,
        (size_t)count,offset,append,&size_limit,&completed);
    kernel_files_size_limit_notify(&size_limit);
    if ((completed.access!=KERNEL_UACCESS_STATUS_OK &&
         completed.access!=KERNEL_UACCESS_STATUS_FAULT) || completed.written>count)
        return KERNEL_FILES_STATUS_STATE;
    KERNEL_FILES_NOTE(files,write_chunks,completed.chunks);
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
    int64_t *linux_result, unsigned char *staging, size_t capacity)
{
    uint64_t total = 0U;

    if (description->device && description->device->discard_writes) {
        size_t consumed = 0U;
        int result = description->device->write(description->device_instance, kernel_task_current(), kernel_open_file_flags(description), 0, (size_t)count, &consumed);
        if (consumed > count) return KERNEL_FILES_STATUS_STATE;
        *linux_result = result && !consumed ? result : (int64_t)consumed;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_GENERATED) {
        if (kernel_vfs_file_is_control(&description->file))
            return control_write(files, mm, description, iov, iov_count, count,
                                 positioned, requested_offset, linux_result);
        *linux_result = -KERNEL_EIO;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_PIPE) {
        struct kernel_pipe_result completed;
        if (kernel_pipe_writev(description->pipe,mm,iov,iov_count,count,
            kernel_open_file_flags(description),&completed)!=KERNEL_PIPE_STATUS_OK ||
            pipe_user_result(completed.bytes,completed.broken)) return KERNEL_FILES_STATUS_STATE;
        *linux_result=completed.bytes;
        if (*linux_result > 0 && description->file.private_data &&
            !kernel_vfs_mount_is_readonly(description->file.mount)) {
            int error = kernel_vfs_file_modified(&description->file, 0, 0);
            if (error) *linux_result = error;
        }
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
            int result = description->device->write(description->device_instance, kernel_task_current(), kernel_open_file_flags(description), staging, copied, &written);
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
struct tcp_user_batch {
    struct kernel_mm_user_pin pins[4];
    struct kernel_socket_write_segment segments[4];
};
_Static_assert(sizeof(struct tcp_user_batch) <= BOAROS_PAGE_SIZE, "TCP pin workspace");

static int tcp_write_user_batch(struct kernel_mm *mm,
    struct kernel_socket_write_request *request, struct kernel_task_io_buffer *workspace,
    uint64_t address, uint32_t size, uint32_t flags, int *fault)
{
    if (!workspace->allocator && kernel_task_io_buffer_acquire(workspace, mm->allocator) != KERNEL_TASK_STATUS_OK)
        return -KERNEL_ENOMEM;
    struct tcp_user_batch *batch = workspace->data;
    unsigned count = 0;
    uint32_t prepared = 0;
    int result = 0;
    while (prepared < size) {
        if (kernel_user_range_check(address + prepared, 1) != KERNEL_UACCESS_STATUS_OK) {
            *fault = 1;
            if (!prepared) result = -KERNEL_EFAULT;
            break;
        }
        struct kernel_mm_user_pin *pin = &batch->pins[count];
        *pin = (struct kernel_mm_user_pin){0};
        enum kernel_uaccess_status access;
        if (!count) access = kernel_uaccess_pin_user_page(mm, address, KERNEL_MM_READ, pin);
        else {
            /* A mapping editor may be draining our earlier pins. Never fault
             * or wait for its gate with that debt: submit the resident prefix. */
            COST_ADD(USER_RESOLUTIONS, 1); COST_IO_ADD(4, 1);
            enum kernel_mm_status status = kernel_mm_pin_user_page(mm, address + prepared, KERNEL_MM_READ, pin);
            if (status == KERNEL_MM_STATUS_NOT_MAPPED || status == KERNEL_MM_STATUS_ACCESS ||
                status == KERNEL_MM_STATUS_CONFLICT || status == KERNEL_MM_STATUS_ADDRESS_SPACE ||
                status == KERNEL_MM_STATUS_BUS_FAULT || status == KERNEL_MM_STATUS_NO_MEMORY) break;
            if (status != KERNEL_MM_STATUS_OK) __builtin_trap();
            access = KERNEL_UACCESS_STATUS_OK;
        }
        if (access != KERNEL_UACCESS_STATUS_OK) {
            if (access != KERNEL_UACCESS_STATUS_FAULT) __builtin_trap();
            *fault = 1;
            if (!prepared) result = -KERNEL_EFAULT;
            break;
        }
        uint32_t offset = (uint32_t)((address + prepared) & BOAROS_PAGE_MASK);
        uint32_t length = BOAROS_PAGE_SIZE - offset;
        if (length > size - prepared) length = size - prepared;
        batch->segments[count++] = (struct kernel_socket_write_segment){(const unsigned char *)pin->page + offset, length};
        prepared += length;
    }
    /* All page preparation precedes protocol ownership. Only this bounded
     * accepted batch pins user pages; EAGAIN returns them before socket wait. */
    if (prepared) result = kernel_socket_stream_commitv(request, batch->segments, count, flags);
    while (count) kernel_mm_unpin_user_page(&batch->pins[--count]);
    return result;
}

static __attribute__((noinline)) enum kernel_files_status tcp_write_request(
    struct kernel_files *files, struct kernel_mm *mm,
    struct kernel_open_file_description *description,
    const struct kernel_uaccess_iovec *iov, size_t iov_count, uint64_t count,
    uint32_t flags, int64_t *linux_result)
{
    struct kernel_open_file_description *borrowed = description;
    struct kernel_socket *socket = kernel_open_file_socket(description);
    struct kernel_socket_write_request request = {0};
    struct kernel_task_io_buffer buffer = {0};
    struct kernel_uaccess_iov_cursor cursor = {iov, iov_count, 0, 0};
    enum kernel_files_status status = KERNEL_FILES_STATUS_OK;
    uint64_t total = 0, target = 0, timeout = (flags & KERNEL_SOCKET_MSG_DONTWAIT) ? 0 : kernel_socket_send_timeout(socket);
    size_t staged = 0, consumed = 0;
    int fault = 0;
    const int direct = iov_count == 1;
    if (timeout) {
        uint64_t now = kernel_time_monotonic_ns();
        target = UINT64_MAX - now < timeout ? UINT64_MAX : now + timeout;
    }
    kernel_socket_stream_begin(&request, &description);
    do {
        size_t chunk = staged - consumed;
        if (!chunk && count) {
            chunk=count-total<BOAROS_PAGE_SIZE?(size_t)(count-total):BOAROS_PAGE_SIZE;
        }
        uint64_t address = 0;
        if (direct && count) {
            address = iov[0].base + cursor.offset;
            size_t limit = 4 * BOAROS_PAGE_SIZE - (address & BOAROS_PAGE_MASK);
            chunk = count - total < limit ? (size_t)(count - total) : limit;
        }
        int result = kernel_socket_stream_reserve(&request, (uint32_t)chunk, flags);
        size_t copied_now = 0;
        if (result >= 0) {
            if (!count) { *linux_result = 0; break; }
            if (!result || (size_t)result > chunk) { status = KERNEL_FILES_STATUS_STATE; break; }
            chunk = (size_t)result;
            if (direct) {
                result = tcp_write_user_batch(mm, &request, &buffer, address, (uint32_t)chunk,
                    flags | ((total + chunk < count) ? KERNEL_SOCKET_IO_MORE : 0), &fault);
            } else {
                if (staged == consumed) {
                    if (!buffer.allocator) {
                        if (kernel_task_io_buffer_acquire(&buffer, mm->allocator) != KERNEL_TASK_STATUS_OK) {
                            *linux_result = total ? (int64_t)total : -KERNEL_ENOMEM; break;
                        }
                        COST_IO_ADD(5, BOAROS_PAGE_SIZE);
                    }
                    KERNEL_FILES_NOTE(files, write_chunks, 1);
                    enum kernel_uaccess_status access = kernel_copy_from_user_iov(mm, &cursor,
                        buffer.data, chunk, &copied_now);
                    if ((access != KERNEL_UACCESS_STATUS_OK && access != KERNEL_UACCESS_STATUS_FAULT) ||
                        copied_now > chunk || (access == KERNEL_UACCESS_STATUS_OK && copied_now != chunk)) {
                        status = KERNEL_FILES_STATUS_STATE; break;
                    }
                    fault = access == KERNEL_UACCESS_STATUS_FAULT;
                    if (!copied_now) { *linux_result = total ? (int64_t)total : -KERNEL_EFAULT; break; }
                    staged = copied_now; consumed = 0; chunk = copied_now;
                }
                result = kernel_socket_stream_commit(&request,
                    (const unsigned char *)buffer.data + consumed, (uint32_t)chunk,
                    flags|((!fault && total+chunk<count)?KERNEL_SOCKET_IO_MORE:0));
            }
        }
        if (result == -KERNEL_EAGAIN && !(flags & KERNEL_SOCKET_MSG_DONTWAIT)) {
            if (!total) COST_ADD(STREAM_COPY_BLOCKED_BYTES, copied_now);
            uint64_t now = target ? kernel_time_monotonic_ns() : 0;
            result = target && now >= target ? -KERNEL_EAGAIN : kernel_socket_wait_ready(kernel_open_file_socket(borrowed),
                KERNEL_POLLOUT, target, flags);
            if (!result) continue; /* gather保留暂存尾部；单iov重新pin未接纳的来源。 */
            if (total && result == -KERNEL_ERESTARTSYS)
                kernel_signal_clear_syscall_restart(kernel_task_current());
        }
        if (result < 0) { *linux_result = total ? (int64_t)total : result; break; }
        if (!result || (size_t)result > (direct ? chunk : staged - consumed)) {
            status = KERNEL_FILES_STATUS_STATE; break;
        }
        if (direct) {
            cursor.offset += (size_t)result;
            KERNEL_FILES_NOTE(files, write_chunks, 1);
            COST_ADD(COPY_FROM_USER, result); COST_IO_ADD(3, result);
        } else consumed += (size_t)result;
        total += (uint64_t)result;
        *linux_result = (int64_t)total;
        if (fault || ((direct ? (size_t)result < chunk : consumed < staged) && (flags & KERNEL_SOCKET_MSG_DONTWAIT))) break;
    } while (total < count);
    kernel_socket_stream_finish(&request);
    if (buffer.allocator) kernel_task_io_buffer_release(&buffer);
    return status;
}

static __attribute__((noinline)) enum kernel_files_status non_socket_write_request(
    struct kernel_files *files, struct kernel_mm *mm,
    struct kernel_open_file_description *description,
    const struct kernel_uaccess_iovec *iov, size_t iov_count,
    uint64_t count, int positioned, uint64_t requested_offset,
    int64_t *linux_result, const struct kernel_lock_guard *borrowed)
{
    const enum kernel_open_file_kind kind=description->kind;
    KERNEL_LOCK_SCOPE(offset_guard);
    if (((!positioned || (kernel_open_file_flags(description)&KERNEL_FILES_O_DSYNC)) &&
         kind == KERNEL_OPEN_FILE_KIND_REGULAR) || kind == KERNEL_OPEN_FILE_KIND_GENERATED) {
        if (!regular_borrows_offset(description,borrowed))
            kernel_mutex_lock(&description->offset_lock, &offset_guard);
    }
    enum kernel_files_status status;
    if (kind==KERNEL_OPEN_FILE_KIND_REGULAR) {
        status=regular_write_request(files,mm,description,iov,iov_count,count,
            positioned,requested_offset,linux_result);
    } else {
        unsigned char small[KERNEL_FILES_WRITE_STAGING];
        unsigned char *staging = small;
        size_t capacity = sizeof(small);
        status=buffered_write_request(files,mm,description,iov,iov_count,count,
            positioned,requested_offset,linux_result,staging,capacity);
    }
    if (status == KERNEL_FILES_STATUS_OK) COST_IO_ADD(2, *linux_result > 0 ? (uint64_t)*linux_result : 0);
    if (status == KERNEL_FILES_STATUS_OK && *linux_result > 0 &&
        kind == KERNEL_OPEN_FILE_KIND_REGULAR &&
        (kernel_open_file_flags(description) & KERNEL_FILES_O_DSYNC) != 0U) {
        int datasync = (kernel_open_file_flags(description) & KERNEL_FILES_O_SYNC) !=
                        KERNEL_FILES_O_SYNC;
        int64_t sync_result = 0;
        const struct kernel_lock_guard *borrow = offset_guard.lock ? &offset_guard : borrowed;
        enum kernel_files_status synced = sync_pinned(description, datasync, &sync_result, borrow);
        if (synced) return synced;
        int error = (int)sync_result;
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
    int64_t *linux_result, uint32_t socket_flags,const struct kernel_lock_guard *borrowed, struct kernel_socket_message *socket_message)
{
    if (description->block) return kernel_block_file_io(description, mm, iov, iov_count,
        count, 1, positioned, requested_offset, linux_result);
    const enum kernel_open_file_kind kind = description->kind;
    if(kind==KERNEL_OPEN_FILE_KIND_PIDFD) {
        *linux_result=-KERNEL_EINVAL;return KERNEL_FILES_STATUS_OK;
    }
    if ((kind==KERNEL_OPEN_FILE_KIND_EVENTFD || kind==KERNEL_OPEN_FILE_KIND_TIMERFD)) {
        *linux_result=kernel_event_fd_io(description,mm,iov,iov_count,count,1);
        return KERNEL_FILES_STATUS_OK;
    }
    /* 固定本次调用的等待资格；后续fcntl只影响下一次读写。 */
    if (kind == KERNEL_OPEN_FILE_KIND_SOCKET &&
        (kernel_open_file_flags(description) & KERNEL_FILES_O_NONBLOCK))
        socket_flags |= KERNEL_SOCKET_MSG_DONTWAIT;
    KERNEL_SERVICE_SCOPE_IF(service, kind != KERNEL_OPEN_FILE_KIND_REGULAR && kind != KERNEL_OPEN_FILE_KIND_SOCKET && kind != KERNEL_OPEN_FILE_KIND_PIPE);
    COST_SCOPE(write_cost, OPERATION_TICKS);
    COST_IO_SCOPE(io_cost,
        kernel_socket_preserves_messages(kernel_open_file_socket(description)) ? 2 :
        kind == KERNEL_OPEN_FILE_KIND_REGULAR ? 0 : kind == KERNEL_OPEN_FILE_KIND_SOCKET ? 1 : 3);
    COST_IO_ADD(0, 1); COST_IO_ADD(1, count);
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
        iov,iov_count,count,positioned,requested_offset,linux_result,borrowed);
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
        enum kernel_files_status status = write_request(files, mm, *owner, iov, iov_count, count, 0, 0, linux_result, flags,NULL, socket_message);
        if (status == KERNEL_FILES_STATUS_OK) {
            if (*linux_result >= 0) KERNEL_FILES_NOTE(files, bytes_written, (uint64_t)*linux_result);
            else KERNEL_FILES_NOTE(files, write_failures, 1);
        }
        return status;
    }
    KERNEL_FILES_NOTE(files, read_calls, 1);
    return read_pinned(files, mm, owner, iov, iov_count, count, 0, linux_result, flags, peer, message_size, socket_message);
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
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_EPOLL ||
        description->kind==KERNEL_OPEN_FILE_KIND_PIDFD ||
        description->kind==KERNEL_OPEN_FILE_KIND_TIMERFD || description->kind==KERNEL_OPEN_FILE_KIND_SIGNALFD) {
        *linux_result = -KERNEL_EINVAL;
        account_write(files, *linux_result);
        return release_io_description(files, &description,
                                      KERNEL_FILES_STATUS_OK);
    }
    if (!description_writable(description)) {
        *linux_result = -KERNEL_EBADF;
        account_write(files, *linux_result);
        return release_io_description(files, &description,
                                      KERNEL_FILES_STATUS_OK);
    }
    /* Validate the original range before clipping; rejection cannot accept data. */
    if (kernel_user_range_check(user_buffer, (size_t)count) !=
            KERNEL_UACCESS_STATUS_OK ||
        scalar_empty_range_fault(user_buffer, count)) {
        *linux_result = -KERNEL_EFAULT;
        account_write(files, *linux_result);
        return release_io_description(files, &description,
                                      KERNEL_FILES_STATUS_OK);
    }
    if (count > KERNEL_FILES_MAX_RW_COUNT) count = KERNEL_FILES_MAX_RW_COUNT;
    iov.length = count;
    if (description->device && description->device->writev) {
        status = description->device->writev(description->device_instance,
            kernel_task_current(), files, &description, 0, mm, &iov, 1U,
            count, kernel_open_file_flags(description), linux_result);
    } else status = write_request(files, mm, description, &iov, 1U, count,
                           0, 0U, linux_result, 0U,NULL, NULL);
    if (status == KERNEL_FILES_STATUS_OK) {
        account_write(files, *linux_result);
    }
    return release_io_description(files, &description, status);
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
    if (kind == KERNEL_OPEN_FILE_KIND_PIDFD)
        *linux_result = -KERNEL_EINVAL;
    else if (kind == KERNEL_OPEN_FILE_KIND_EPOLL ||
        kind == KERNEL_OPEN_FILE_KIND_EVENTFD || kind == KERNEL_OPEN_FILE_KIND_TIMERFD || kind == KERNEL_OPEN_FILE_KIND_SIGNALFD ||
        kind == KERNEL_OPEN_FILE_KIND_SOCKET ||
        kind == KERNEL_OPEN_FILE_KIND_PIPE ||
        (description->device && !description->device->positioned))
        *linux_result = -KERNEL_ESPIPE;
    else if (!description_writable(description))
        *linux_result = -KERNEL_EBADF;
    else if (offset < 0)
        *linux_result = -KERNEL_EINVAL;
    else if (kind == KERNEL_OPEN_FILE_KIND_DIRECTORY)
        *linux_result = -KERNEL_EISDIR;
    else
        *linux_result = 0;
    if (*linux_result != 0) {
        account_write(files, *linux_result);
        return release_io_description(files, &description,
                                      KERNEL_FILES_STATUS_OK);
    }
    if (kernel_user_range_check(user_buffer, (size_t)count) !=
            KERNEL_UACCESS_STATUS_OK ||
        scalar_empty_range_fault(user_buffer, count)) {
        *linux_result = -KERNEL_EFAULT;
        account_write(files, *linux_result);
        return release_io_description(files, &description,
                                      KERNEL_FILES_STATUS_OK);
    }
    if (count > KERNEL_FILES_MAX_RW_COUNT) count = KERNEL_FILES_MAX_RW_COUNT;
    iov.length = count;
    status = write_request(files, mm, description, &iov, 1U, count,
                           1, (uint64_t)offset, linux_result, 0U,NULL, NULL);
    if (status == KERNEL_FILES_STATUS_OK) account_write(files, *linux_result);
    return release_io_description(files, &description, status);
}

static enum kernel_files_status writev_request(
    struct kernel_files *files, struct kernel_mm *mm, int64_t fd,
    uint64_t user_iov, uint64_t iovcnt, int positioned, int64_t offset,
    int64_t *linux_result)
{
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
    KERNEL_SERVICE_SCOPE_IF(service, kernel_open_file_kind(description) != KERNEL_OPEN_FILE_KIND_REGULAR && kernel_open_file_kind(description) != KERNEL_OPEN_FILE_KIND_SOCKET && kernel_open_file_kind(description) != KERNEL_OPEN_FILE_KIND_PIPE && kernel_open_file_kind(description) != KERNEL_OPEN_FILE_KIND_EVENTFD && kernel_open_file_kind(description) != KERNEL_OPEN_FILE_KIND_TIMERFD && kernel_open_file_kind(description) != KERNEL_OPEN_FILE_KIND_SIGNALFD);
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_EPOLL ||
        description->kind==KERNEL_OPEN_FILE_KIND_PIDFD ||
        description->kind==KERNEL_OPEN_FILE_KIND_TIMERFD || description->kind==KERNEL_OPEN_FILE_KIND_SIGNALFD) {
        *linux_result = positioned && description->kind!=KERNEL_OPEN_FILE_KIND_PIDFD
            ? -KERNEL_ESPIPE : -KERNEL_EINVAL;
        account_write(files, *linux_result);
        return release_io_description(files, &description,
                                      KERNEL_FILES_STATUS_OK);
    }
    if (positioned && ((description->device && !description->device->positioned) ||
        (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_EVENTFD || kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_TIMERFD || kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_SIGNALFD) ||
        kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_PIPE ||
        kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_SOCKET)) {
        *linux_result = -KERNEL_ESPIPE;
        goto out;
    }
    if (!description_writable(description)) {
        *linux_result = -KERNEL_EBADF;
        account_write(files, *linux_result);
        return release_io_description(files, &description,
                                      KERNEL_FILES_STATUS_OK);
    }
    if (iovcnt > 1024U) {
        *linux_result = -KERNEL_EINVAL;
        goto out;
    }
    status=kernel_uaccess_iov_import(&snapshot,files->heap,mm,user_iov,iovcnt,linux_result);
    if (status) goto out;
    if (*linux_result) goto out;
    if (!snapshot.total && description->kind==KERNEL_OPEN_FILE_KIND_EVENTFD) { *linux_result=0;goto out; }
    if (!positioned && description->device && description->device->writev) {
        status = description->device->writev(description->device_instance,
            kernel_task_current(), files, &description,
            &snapshot.allocation, mm, snapshot.view, snapshot.count,
            snapshot.total, kernel_open_file_flags(description), linux_result);
    } else status = write_request(files, mm, description, snapshot.view, snapshot.count, snapshot.total,
                           positioned, (uint64_t)offset, linux_result, 0U,NULL, NULL);
out:
    kernel_uaccess_iov_cleanup(&snapshot,files->heap);
    if (status == KERNEL_FILES_STATUS_OK) {
        account_write(files, *linux_result);
    }
    return release_io_description(files, &description, status);
}

enum kernel_files_status kernel_files_writev(struct kernel_files *files,
    struct kernel_mm *mm, int64_t fd, uint64_t user_iov, uint64_t iovcnt,
    int64_t *linux_result)
{
    return writev_request(files, mm, fd, user_iov, iovcnt, 0, 0, linux_result);
}

enum kernel_files_status kernel_files_pwritev(struct kernel_files *files,
    struct kernel_mm *mm, int64_t fd, uint64_t user_iov, uint64_t iovcnt,
    int64_t offset, int64_t *linux_result)
{
    return writev_request(files, mm, fd, user_iov, iovcnt, 1, offset, linux_result);
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
    int control = kernel_vfs_file_is_control(&description->file);
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
    default:
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    if (target < 0 || (description->block && (uint64_t)target > kernel_open_file_size(description))) {
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
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
        return release_io_description(files,&description,KERNEL_FILES_STATUS_OK);
    }
    if (whence > 4U) {
        *linux_result = -KERNEL_EINVAL;
        return release_io_description(files, &description, KERNEL_FILES_STATUS_OK);
    }
    status = lseek_pinned(description, offset, whence, linux_result,NULL);
    return release_io_description(files, &description, status);
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
    if (!description_writable(description)) {
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
    return release_io_description(files,&description,status);
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
    unsigned char record[(19U + 256U + 7U) & ~(size_t)7U];
    /* Fill the name directly in the outgoing dirent. Keeping a second
     * 256-byte copy consumes the single-page task stack on ext4 faults. */
    char *name = (char *)&record[19];
    uint64_t inode;
    uint8_t type;
    uint64_t request;
    uint64_t total = 0U;
    uint64_t position;
    uint64_t next_position;
    int fill_result;

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
    /* The descriptor offset is the backend cookie for the next record. */
    position = kernel_open_file_offset(description);
    do {
        size_t name_length;
        size_t record_length;
        size_t copied = 0U;
        uint16_t reported_length;
        uint16_t little;
        uint64_t cookie;
        enum kernel_uaccess_status access_status;

        fill_result = kernel_vfs_dir_entry(&description->file,
                                           position,
                                           &next_position,
                                           &inode,
                                           &type,
                                           name,
                                           256U);
        if (fill_result == 0) {
            if (kernel_open_file_seek(description, next_position) !=
                KERNEL_OPEN_FILE_STATUS_OK) {
                return KERNEL_FILES_STATUS_STATE;
            }
            break;
        }
        if (fill_result < 0) {
            /* Linux returns a completed prefix when a later directory
             * record fails; the failing record remains the next cookie. */
            *linux_result = total != 0U ? (int64_t)total : fill_result;
            KERNEL_FILES_NOTE(files, bytes_read, total);
            return KERNEL_FILES_STATUS_OK;
        }
        name_length = strlen(name);
        record_length = (19U + name_length + 1U + 7U) & ~(size_t)7U;
        if ((uint64_t)record_length > request - total) {
            /* Linux answers EINVAL when nothing fits at all. */
            *linux_result = total == 0U ? -KERNEL_EINVAL : (int64_t)total;
            KERNEL_FILES_NOTE(files, bytes_read, total);
            return KERNEL_FILES_STATUS_OK;
        }
        reported_length = (uint16_t)record_length;
        little = 1;
        if (*(unsigned char *)&little) {
            /* Little-endian host byte order for the fixed fields. */
            memcpy(&record[0], &inode, sizeof(inode));
            cookie = next_position;
            memcpy(&record[8], &cookie, sizeof(cookie));
        } else {
            for (uint8_t byte = 0U; byte < 8U; byte++) {
                record[byte] =
                    (unsigned char)((inode >> (8U * byte)) & 0xffU);
                record[8U + byte] =
                    (unsigned char)((next_position >> (8U * byte)) &
                                    0xffU);
            }
        }
        memcpy(&record[16], &reported_length, sizeof(reported_length));
        record[18] = type;
        record[19U + name_length] = '\0';
        for (size_t pad = 19U + name_length + 1U; pad < record_length;
             pad++) {
            record[pad] = 0U;
        }

        access_status = kernel_copy_to_user(mm,
                                            user_buffer + total,
                                            record,
                                            record_length,
                                            &copied);
        if (access_status == KERNEL_UACCESS_STATUS_FAULT ||
            copied != record_length) {
            *linux_result = total == 0U ? -KERNEL_EFAULT : (int64_t)total;
            KERNEL_FILES_NOTE(files, bytes_read, total);
            return KERNEL_FILES_STATUS_OK;
        }
        total += record_length;
        position = next_position;
        if (kernel_open_file_seek(description, position) !=
            KERNEL_OPEN_FILE_STATUS_OK) {
            return KERNEL_FILES_STATUS_STATE;
        }
    } while (total < request);
    KERNEL_FILES_NOTE(files, bytes_read, total);
    *linux_result = (int64_t)total;
    return KERNEL_FILES_STATUS_OK;
}

#if BOAROS_COST_DIAGNOSTICS
int kernel_files_cost_descriptor(struct kernel_files *files, int64_t fd)
{
    KERNEL_FILES_PIN_SCOPE(pin);
    struct kernel_open_file_description *description = kernel_files_hold_fd(files, fd, &pin);
    return description && kernel_vfs_file_is_cost(&description->file);
}
#endif
