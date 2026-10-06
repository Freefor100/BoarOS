#include "private.h"
#include "../open_file_internal.h"
#include "../pipe_internal.h"
#include "../char_device_internal.h"
#include "../uaccess_iov_internal.h"
#include "../vfs_internal.h"

#include <kernel/cost.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/open_file.h>
#include <kernel/page.h>
#include <kernel/page_cache.h>
#include <kernel/scheduler.h>
#include <kernel/signal.h>
#include <kernel/socket.h>
#include <kernel/task.h>
#include <kernel/time.h>
#include <kernel/uaccess.h>
#include <kernel/vfs.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <arch/context.h>

#define KERNEL_FILES_WRITE_STAGING 64U

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
        return operation_status == KERNEL_FILES_STATUS_STATE
                   ? operation_status
                   : KERNEL_FILES_STATUS_CLEANUP_REQUIRED;
    }
    return KERNEL_FILES_STATUS_STATE;
}

static int socket_operation_ready(struct kernel_socket *socket, uint32_t events)
{
    uint32_t polled = kernel_socket_poll(socket, 0);
    /* 终止事件可对外发布，实际接收仍须等待独占reservation的owner交还。 */
    return events == KERNEL_POLLIN ? kernel_socket_receive_ready(socket) :
        (polled & (events | KERNEL_POLLERR | KERNEL_POLLHUP)) != 0U;
}

static int socket_wait_ready(struct kernel_open_file_description *description,
                             uint32_t events, uint64_t timeout_ns, uint32_t socket_flags)
{
    struct kernel_socket *socket = kernel_open_file_socket(description);
    uint64_t deadline = 0;
    uint64_t target_ns = 0;
    uintptr_t saved;
    if ((description->open_flags & KERNEL_FILES_O_NONBLOCK) != 0U ||
        (socket_flags & KERNEL_SOCKET_MSG_DONTWAIT) != 0U)
        return -KERNEL_EAGAIN;
    if (socket_operation_ready(socket, events))
        return (events & KERNEL_POLLOUT) ? kernel_socket_unix_wait_error(socket) : 0;
    if (timeout_ns != 0U) {
        uint64_t now = kernel_time_monotonic_ns();
        uint64_t target = UINT64_MAX - now < timeout_ns
                              ? UINT64_MAX : now + timeout_ns;
        target_ns = target;
        enum kernel_time_status status =
            kernel_time_deadline_from_monotonic(target, &deadline);
        if (status == KERNEL_TIME_STATUS_DEADLINE_PASSED)
            return -KERNEL_EAGAIN;
        if (status != KERNEL_TIME_STATUS_OK) return -KERNEL_EIO;
    }
    saved = arch_interrupt_save();
    while (!socket_operation_ready(socket, events)) {
        enum kernel_wait_wake_reason reason;
        uint64_t sleep_deadline = deadline;
        if (kernel_scheduler_block_current(kernel_socket_wait_queue(socket),
                                            sleep_deadline, 1, &reason) !=
            KERNEL_SCHEDULER_STATUS_OK) {
            arch_interrupt_restore(saved);
            return -KERNEL_EIO;
        }
        if (reason == KERNEL_WAIT_TIMEOUT) {
            if (target_ns != 0U &&
                kernel_time_monotonic_ns() >= target_ns) {
                arch_interrupt_restore(saved);
                return -KERNEL_EAGAIN;
            }
            continue;
        }
        if (reason == KERNEL_WAIT_SIGNALLED) {
            arch_interrupt_restore(saved);
            if (timeout_ns != 0U) return -KERNEL_EINTR;
            kernel_signal_note_syscall_restart(kernel_task_current());
            return -KERNEL_ERESTARTSYS;
        }
    }
    int error=(events & KERNEL_POLLOUT) ? kernel_socket_unix_wait_error(socket) : 0;
    arch_interrupt_restore(saved);
    return error;
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
        files->record->statistics.read_failures++;
        return KERNEL_FILES_STATUS_OK;
    }
    if (advance && kernel_open_file_advance(description, length) != KERNEL_OPEN_FILE_STATUS_OK)
        return KERNEL_FILES_STATUS_STATE;
    files->record->statistics.bytes_read += length;
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
        files->record->statistics.read_failures++;
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
        files->record->statistics.read_chunks++;
        if (access == KERNEL_UACCESS_STATUS_FAULT) {
            *linux_result = total ? (int64_t)total : -KERNEL_EFAULT;
            if (!total) files->record->statistics.read_failures++;
            files->record->statistics.bytes_read += total;
            return KERNEL_FILES_STATUS_OK;
        }
        if (access != KERNEL_UACCESS_STATUS_OK || copied != chunk)
            return KERNEL_FILES_STATUS_STATE;
    }
    files->record->statistics.bytes_read += total;
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
        *linux_result = -KERNEL_ENOMEM; files->record->statistics.read_failures++;
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
        files->record->statistics.read_chunks++;
        if (status == KERNEL_UACCESS_STATUS_FAULT) { error = -KERNEL_EFAULT; break; }
        if (status != KERNEL_UACCESS_STATUS_OK || copied != available) __builtin_trap();
    }
    kernel_task_io_buffer_release(&buffer);
    if (error) files->record->statistics.read_failures++;
    files->record->statistics.bytes_read += total;
    *linux_result = total ? (int64_t)total : error;
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
    enum kernel_open_file_kind kind = kernel_open_file_kind(description);
    *linux_result = kind == KERNEL_OPEN_FILE_KIND_REGULAR ||
                    kind == KERNEL_OPEN_FILE_KIND_DIRECTORY
        ? kernel_vfs_sync(&description->file, datasync,
                            &description->observed_writeback_error)
        : -KERNEL_EINVAL;
    return release_io_description(files, &description, KERNEL_FILES_STATUS_OK);
}

enum kernel_files_status kernel_files_syncfs(struct kernel_files *files,
    int64_t fd, int64_t *linux_result)
{
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

static enum kernel_files_status read_pinned(
    struct kernel_files *files,
    struct kernel_mm *mm,
    struct kernel_open_file_description *description,
    struct kernel_open_file_description **description_owner,
    const struct kernel_uaccess_iovec *iov,
    size_t iov_count,
    uint64_t count,
    int64_t *linux_result, uint32_t socket_flags,
    struct kernel_socket_address *peer, uint32_t *message_size)
{
    KERNEL_LOCK_SCOPE(offset_guard);
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_REGULAR ||
        kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_GENERATED)
        kernel_mutex_lock(&description->offset_lock, &offset_guard);
    uint64_t request;
    uint64_t total = 0U;
    struct kernel_uaccess_iov_cursor cursor = {iov, iov_count, 0U, 0U};

    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_EPOLL) {
        *linux_result = -KERNEL_EINVAL;
        files->record->statistics.read_failures++;
        return KERNEL_FILES_STATUS_OK;
    }
    if (!kernel_open_file_readable(description)) {
        *linux_result = -KERNEL_EBADF;
        files->record->statistics.read_failures++;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_SOCKET) {
        struct kernel_task_io_buffer buffer = {0};
        struct kernel_socket_read_request read_request = {0};
        int received = 0;
        enum kernel_files_status result = KERNEL_FILES_STATUS_OK;
        for (size_t index = 0; index < iov_count; index++) {
            if (kernel_user_range_check(iov[index].base, (size_t)iov[index].length)
                    != KERNEL_UACCESS_STATUS_OK) {
                *linux_result = -KERNEL_EFAULT;
                files->record->statistics.read_failures++;
                return KERNEL_FILES_STATUS_OK;
            }
        }
        int message = (socket_flags & KERNEL_SOCKET_IO_MESSAGE) != 0;
        /* read(0)可直接返回；recv(0)仍须检查协议状态和等待条件。 */
        int discard = kernel_socket_discard_receive(kernel_open_file_socket(description),socket_flags);
        if (count == 0U && !message) {
            *linux_result = 0;
            return KERNEL_FILES_STATUS_OK;
        }
        if (count != 0U && !discard && kernel_task_io_buffer_acquire(&buffer, mm->allocator) !=
                KERNEL_TASK_STATUS_OK) {
            *linux_result = -KERNEL_ENOMEM;
            files->record->statistics.read_failures++;
            return KERNEL_FILES_STATUS_OK;
        }
        while (total < count || (message && total == 0)) {
            uint64_t remaining = count - total;
            received = kernel_socket_reserve_read(
                kernel_open_file_socket(description), kernel_task_current(),
                &read_request, description_owner,
                remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining,
                (description->open_flags & KERNEL_FILES_O_NONBLOCK) ||
                    (socket_flags & KERNEL_SOCKET_MSG_DONTWAIT));
            if (received == -KERNEL_EAGAIN && total == 0U) {
                int waited = socket_wait_ready(description, KERNEL_POLLIN,
                    kernel_socket_receive_timeout(kernel_open_file_socket(description)), socket_flags);
                if (waited == 0) continue;
                received = waited;
            }
            if (received < 0 || read_request.socket == 0) break;
            int datagram = read_request.datagram;
            if (datagram) kernel_socket_read_info(&read_request, peer, message_size);
            uint32_t done = discard ? (uint32_t)received : 0;
            int fault = 0;
            while (done < (uint32_t)received) {
                size_t chunk = (uint32_t)received - done, copied = 0;
                if (chunk > BOAROS_PAGE_SIZE) chunk = BOAROS_PAGE_SIZE;
                kernel_socket_copy_read(&read_request, done, buffer.data, chunk);
                enum kernel_uaccess_status access = kernel_copy_to_user_iov(
                    mm, &cursor, buffer.data, chunk, &copied);
                files->record->statistics.read_chunks++;
                if (access != KERNEL_UACCESS_STATUS_OK || copied != chunk) {
                    fault = 1;
                    if (access != KERNEL_UACCESS_STATUS_FAULT)
                        result = KERNEL_FILES_STATUS_STATE;
                    break;
                }
                done += (uint32_t)chunk;
            }
            kernel_socket_finish_read(&read_request, fault);
            if (fault) { received = -KERNEL_EFAULT; break; }
            total += (uint32_t)received;
            if (datagram) break;
        }
        if (buffer.allocator) kernel_task_io_buffer_release(&buffer);
        *linux_result = total != 0U ? (int64_t)total : received;
        if (*linux_result < 0) files->record->statistics.read_failures++;
        else files->record->statistics.bytes_read += total;
        return result;
    }
    if (description->device) {
        const struct kernel_char_device *device = description->device;
        if (device->empty_range_fault && iov_count == 1U && count == 0U &&
            iov[0].base != 0U &&
            kernel_user_range_check(iov[0].base - 1U, 1U) !=
                KERNEL_UACCESS_STATUS_OK) {
            *linux_result = -KERNEL_EFAULT;
            files->record->statistics.read_failures++;
            return KERNEL_FILES_STATUS_OK;
        }
        for (size_t index = 0U; index < iov_count; index++) {
            if (kernel_user_range_check(iov[index].base,
                                        (size_t)iov[index].length) !=
                KERNEL_UACCESS_STATUS_OK) {
                files->record->statistics.read_failures++;
                *linux_result = -KERNEL_EFAULT;
                return KERNEL_FILES_STATUS_OK;
            }
        }
        if (device->readv) {
            enum kernel_files_status status = device->readv(description->device_instance,
                kernel_task_current(), files, description_owner, 0, mm, iov, iov_count,
                count > KERNEL_FILES_MAX_RW_COUNT ? KERNEL_FILES_MAX_RW_COUNT : count,
                description->open_flags, linux_result);
            if (*linux_result < 0) files->record->statistics.read_failures++;
            else files->record->statistics.bytes_read += (uint64_t)*linux_result;
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
            int result = device->read(description->device_instance, kernel_task_current(), description->open_flags, staging,
                                      chunk, &received);
            if (result) {
                *linux_result = total ? (int64_t)total : result;
                if (!total) files->record->statistics.read_failures++;
                files->record->statistics.bytes_read += total;
                return KERNEL_FILES_STATUS_OK;
            }
            if (received > chunk) return KERNEL_FILES_STATUS_STATE;
            if (!received) break;
            enum kernel_uaccess_status access = kernel_copy_to_user_iov(
                mm, &cursor, staging, received, &copied);
            total += copied;
            files->record->statistics.read_chunks++;
            if (access == KERNEL_UACCESS_STATUS_FAULT) {
                *linux_result = total ? (int64_t)total : -KERNEL_EFAULT;
                files->record->statistics.bytes_read += total;
                if (total == 0U) files->record->statistics.read_failures++;
                return KERNEL_FILES_STATUS_OK;
            }
            if (access != KERNEL_UACCESS_STATUS_OK || copied != received)
                return KERNEL_FILES_STATUS_STATE;
            if (device->read_once || received < chunk) break;
        }
        files->record->statistics.bytes_read += total;
        *linux_result = (int64_t)total;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_PIPE) {
        enum kernel_pipe_status pipe_status = kernel_pipe_readv(
            description->pipe,
            mm,
            iov, iov_count,
            count,
            description->open_flags,
            linux_result);

        if (pipe_status != KERNEL_PIPE_STATUS_OK) {
            return KERNEL_FILES_STATUS_STATE;
        }
        if (*linux_result > 0 && description->file.private_data)
            kernel_vfs_file_accessed(&description->file);
        if (*linux_result >= 0) {
            files->record->statistics.bytes_read += (uint64_t)*linux_result;
        } else {
            files->record->statistics.read_failures++;
        }
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_open_file_kind(description) ==
        KERNEL_OPEN_FILE_KIND_DIRECTORY) {
        files->record->statistics.read_failures++;
        *linux_result = -KERNEL_EISDIR;
        return KERNEL_FILES_STATUS_OK;
    }
    if ((kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_REGULAR ||
         kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_GENERATED) &&
        !kernel_open_file_readable(description)) {
        files->record->statistics.read_failures++;
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    for (size_t index = 0U; index < iov_count; index++) {
        if (kernel_user_range_check(iov[index].base,
                                    (size_t)iov[index].length) !=
            KERNEL_UACCESS_STATUS_OK) {
            files->record->statistics.read_failures++;
            *linux_result = -KERNEL_EFAULT;
            return KERNEL_FILES_STATUS_OK;
        }
    }
    if (count == 0U) {
        *linux_result = 0;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_GENERATED) {
        request = count > KERNEL_FILES_MAX_RW_COUNT
                      ? KERNEL_FILES_MAX_RW_COUNT : count;
        return generated_read(files, mm, description, &cursor, request,
                              kernel_open_file_offset(description), 1,
                              linux_result);
    }
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_REGULAR) {
        kernel_open_file_read_begin(description, kernel_open_file_offset(description));
        kernel_vfs_file_accessed(&description->file);
    }
    request = count > KERNEL_FILES_MAX_RW_COUNT
                  ? KERNEL_FILES_MAX_RW_COUNT
                  : count;
    if (kernel_open_file_memory_backed(description))
        return memory_read(files, mm, description, &cursor, request,
                           kernel_open_file_offset(description), 1, linux_result);
    while (total < request) {
        uint64_t file_offset = kernel_open_file_offset(description);
        kernel_open_file_read_begin(description, file_offset);
        uint64_t page_index = file_offset >> BOAROS_PAGE_SHIFT;
        size_t page_offset = (size_t)(file_offset & BOAROS_PAGE_MASK);
        size_t valid_bytes;
        size_t chunk;
        size_t copied = 0U;
        uint64_t physical_address;
        void *page;
        enum kernel_page_cache_status cache_status;
        enum kernel_uaccess_status access_status;

        cache_status = kernel_open_file_get_page(description,
                                                 page_index,
                                                 &physical_address,
                                                 &valid_bytes);
        files->record->statistics.read_chunks++;
        if (cache_status == KERNEL_PAGE_CACHE_STATUS_OUT_OF_RANGE) {
            break;
        }
        if (cache_status != KERNEL_PAGE_CACHE_STATUS_OK) {
            int result = cache_status ==
                                     KERNEL_PAGE_CACHE_STATUS_NO_MEMORY
                                 ? -KERNEL_ENOMEM
                                 : -KERNEL_EIO;

            files->record->statistics.read_failures++;
            *linux_result = total != 0U ? (int64_t)total : result;
            files->record->statistics.bytes_read += total;
            return KERNEL_FILES_STATUS_OK;
        }
        if (page_offset >= valid_bytes) {
            (void)physical_page_release(files->heap->page_allocator,
                                      physical_address);
            break;
        }
        chunk = valid_bytes - page_offset;
        if ((uint64_t)chunk > request - total) {
            chunk = (size_t)(request - total);
        }
        if (physical_page_resolve(files->heap->page_allocator,
                                  physical_address,
                                  &page) !=
            PHYSICAL_PAGE_STATUS_OK) {
            (void)physical_page_release(files->heap->page_allocator,
                                        physical_address);
            return KERNEL_FILES_STATUS_STATE;
        }
        access_status = kernel_copy_to_user_iov(mm, &cursor,
                                                (unsigned char *)page +
                                                    page_offset,
                                                chunk, &copied);
        if (kernel_open_file_advance(description, copied) !=
                KERNEL_OPEN_FILE_STATUS_OK ||
            physical_page_release(files->heap->page_allocator,
                                  physical_address) !=
                PHYSICAL_PAGE_STATUS_OK) {
            return KERNEL_FILES_STATUS_STATE;
        }
        kernel_open_file_read_progress(description, file_offset, copied);
        total += copied;
        if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
            files->record->statistics.read_failures++;
            *linux_result = total != 0U ? (int64_t)total
                                        : -KERNEL_EFAULT;
            files->record->statistics.bytes_read += total;
            return KERNEL_FILES_STATUS_OK;
        }
        if (access_status != KERNEL_UACCESS_STATUS_OK ||
            copied != chunk) {
            return KERNEL_FILES_STATUS_STATE;
        }
        if (page_offset + chunk == valid_bytes &&
            valid_bytes < BOAROS_PAGE_SIZE) {
            break;
        }
    }

    files->record->statistics.bytes_read += total;
    *linux_result = (int64_t)total;
    return KERNEL_FILES_STATUS_OK;
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
    files->record->statistics.read_calls++;
    status = kernel_files_pin_data(files, fd, &description, linux_result);
    if (status != KERNEL_FILES_STATUS_OK) {
        return status;
    }
    if (*linux_result != 0) {
        files->record->statistics.read_failures++;
        return KERNEL_FILES_STATUS_OK;
    }
    status = read_pinned(files, mm, description, &description, &iov, 1U, count,
                         linux_result, 0U, 0, 0);
    return release_io_description(files, &description, status);
}

enum kernel_files_status kernel_files_readv(
    struct kernel_files *files,
    struct kernel_mm *mm,
    int64_t fd,
    uint64_t user_iov,
    uint64_t iovcnt,
    int64_t *linux_result)
{
    struct kernel_uaccess_iovec local[8];
    struct kernel_uaccess_iovec *iov = local;
    struct kernel_open_file_description *description = 0;
    uint64_t total = 0U;
    size_t copied = 0U;
    enum kernel_files_status status;
    enum kernel_uaccess_status access;
    int dispatched = 0;

    if (!kernel_files_is_live(files) || mm == 0 || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    files->record->statistics.read_calls++;
    status = kernel_files_pin_data(files, fd, &description, linux_result);
    if (status != KERNEL_FILES_STATUS_OK) {
        return status;
    }
    if (*linux_result != 0) {
        files->record->statistics.read_failures++;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_EPOLL) {
        *linux_result = -KERNEL_EINVAL;
        status = KERNEL_FILES_STATUS_OK;
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
    if (iovcnt > sizeof(local) / sizeof(local[0])) {
        enum kernel_heap_status allocation =
            kernel_heap_allocate(files->heap, iovcnt * sizeof(*iov),
                                  (void **)&iov);

        if (allocation != KERNEL_HEAP_STATUS_OK) {
            if (allocation != KERNEL_HEAP_STATUS_EMPTY) {
                status = KERNEL_FILES_STATUS_STATE;
                goto out;
            }
            *linux_result = -KERNEL_ENOMEM;
            status = KERNEL_FILES_STATUS_OK;
            goto out;
        }
    }
    if (iovcnt != 0U) {
        access = kernel_copy_from_user(mm, iov, user_iov,
                                       iovcnt * sizeof(*iov), &copied);
        if (access != KERNEL_UACCESS_STATUS_OK ||
            copied != iovcnt * sizeof(*iov)) {
            *linux_result = -KERNEL_EFAULT;
            status = access == KERNEL_UACCESS_STATUS_FAULT
                         ? KERNEL_FILES_STATUS_OK : KERNEL_FILES_STATUS_STATE;
            goto out;
        }
    }
    for (size_t index = 0U; index < iovcnt; index++) {
        if (iov[index].length > (uint64_t)INT64_MAX) {
            *linux_result = -KERNEL_EINVAL;
            status = KERNEL_FILES_STATUS_OK;
            goto out;
        }
        /* Linux imports a sole iovec through import_ubuf (cap first),
         * whereas a multi-entry vector checks each original range first. */
        if (iovcnt == 1U && iov[index].length > KERNEL_FILES_MAX_RW_COUNT) {
            iov[index].length = KERNEL_FILES_MAX_RW_COUNT;
        }
        if (kernel_user_range_check(iov[index].base,
                                    (size_t)(iov[index].length == 0U
                                                 ? 1U : iov[index].length)) !=
            KERNEL_UACCESS_STATUS_OK) {
            *linux_result = -KERNEL_EFAULT;
            status = KERNEL_FILES_STATUS_OK;
            goto out;
        }
        if (iov[index].length > KERNEL_FILES_MAX_RW_COUNT - total) {
            iov[index].length = KERNEL_FILES_MAX_RW_COUNT - total;
        }
        total += iov[index].length;
    }
    if (total == 0U) {
        *linux_result = 0;
        status = KERNEL_FILES_STATUS_OK;
        goto out;
    }
    if (description->device && description->device->readv) {
        status = description->device->readv(description->device_instance,
            kernel_task_current(), files, &description,
            iov == local ? 0 : (void **)&iov, mm, iov, (size_t)iovcnt,
            total, description->open_flags, linux_result);
        if (*linux_result < 0) files->record->statistics.read_failures++;
        else files->record->statistics.bytes_read += (uint64_t)*linux_result;
    } else status = read_pinned(files, mm, description, &description,
                         iov, (size_t)iovcnt,
                         total, linux_result, 0U, 0, 0);
    dispatched = 1;
out:
    if (iov && iov != local &&
        kernel_files_release_allocation(files, iov) ==
            KERNEL_FILES_STATUS_STATE) {
        status = KERNEL_FILES_STATUS_STATE;
    }
    if (!dispatched && status == KERNEL_FILES_STATUS_OK &&
        *linux_result < 0) {
        files->record->statistics.read_failures++;
    }
    return release_io_description(files, &description, status);
}

static enum kernel_files_status pread_pinned(
    struct kernel_files *files,
    struct kernel_mm *mm,
    struct kernel_open_file_description *description,
    uint64_t user_buffer,
    uint64_t count,
    int64_t offset,
    int64_t *linux_result)
{
    uint64_t position;
    uint64_t request;
    uint64_t total = 0U;
    uint64_t file_size;

    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_EPOLL) {
        files->record->statistics.read_failures++;
        *linux_result = -KERNEL_ESPIPE;
        return KERNEL_FILES_STATUS_OK;
    }
    if ((kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_REGULAR ||
         kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_GENERATED) &&
        !kernel_open_file_readable(description)) {
        files->record->statistics.read_failures++;
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    if (offset < 0) {
        files->record->statistics.read_failures++;
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_open_file_kind(description) ==
            KERNEL_OPEN_FILE_KIND_DIRECTORY) {
        files->record->statistics.read_failures++;
        *linux_result = -KERNEL_EISDIR;
        return KERNEL_FILES_STATUS_OK;
    }
    if ((description->device && !description->device->positioned) ||
        kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_PIPE ||
        kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_SOCKET) {
        files->record->statistics.read_failures++;
        *linux_result = -KERNEL_ESPIPE;
        return KERNEL_FILES_STATUS_OK;
    }
    /* Linux vfs_read checks the caller's original range before MAX_RW_COUNT
     * clipping and before EOF. Its empty range permits the user limit itself
     * but rejects higher addresses; generic uaccess deliberately skips zeros. */
    if (kernel_user_range_check(user_buffer, (size_t)count) !=
            KERNEL_UACCESS_STATUS_OK ||
        (count == 0U && user_buffer != 0U &&
         kernel_user_range_check(user_buffer - 1U, 1U) !=
             KERNEL_UACCESS_STATUS_OK)) {
        files->record->statistics.read_failures++;
        *linux_result = -KERNEL_EFAULT;
        return KERNEL_FILES_STATUS_OK;
    }
    if (count == 0U) {
        *linux_result = 0;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_GENERATED) {
        KERNEL_LOCK_SCOPE(offset_guard);
        kernel_mutex_lock(&description->offset_lock, &offset_guard);
        struct kernel_uaccess_iovec iov = {user_buffer, count};
        struct kernel_uaccess_iov_cursor cursor = {&iov, 1U, 0U, 0U};
        uint64_t capped = count > KERNEL_FILES_MAX_RW_COUNT
                            ? KERNEL_FILES_MAX_RW_COUNT : count;
        return generated_read(files, mm, description, &cursor, capped,
                              (uint64_t)offset, 0, linux_result);
    }
    if (description->device && description->device->positioned) {
        const struct kernel_uaccess_iovec iov = {user_buffer, count};
        return read_pinned(files, mm, description, 0, &iov, 1U, count,
                           linux_result, 0U, 0, 0);
    }
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_REGULAR) {
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
        const struct kernel_uaccess_iovec iov = {user_buffer, request};
        struct kernel_uaccess_iov_cursor cursor = {&iov, 1, 0, 0};
        return memory_read(files, mm, description, &cursor, request, position, 0, linux_result);
    }
    while (total < request) {
        kernel_open_file_read_begin(description, position);
        uint64_t page_index = position >> BOAROS_PAGE_SHIFT;
        size_t page_offset = (size_t)(position & BOAROS_PAGE_MASK);
        size_t valid_bytes;
        size_t chunk;
        size_t copied = 0U;
        uint64_t physical_address;
        void *page;
        enum kernel_page_cache_status cache_status;
        enum kernel_uaccess_status access_status;

        cache_status = kernel_open_file_get_page(description,
                                                  page_index,
                                                  &physical_address,
                                                  &valid_bytes);
        files->record->statistics.read_chunks++;
        if (cache_status == KERNEL_PAGE_CACHE_STATUS_OUT_OF_RANGE) {
            break;
        }
        if (cache_status != KERNEL_PAGE_CACHE_STATUS_OK) {
            int result = cache_status == KERNEL_PAGE_CACHE_STATUS_NO_MEMORY
                             ? -KERNEL_ENOMEM
                             : -KERNEL_EIO;

            files->record->statistics.read_failures++;
            *linux_result = total != 0U ? (int64_t)total : result;
            files->record->statistics.bytes_read += total;
            return KERNEL_FILES_STATUS_OK;
        }
        if (page_offset >= valid_bytes) {
            (void)physical_page_release(files->heap->page_allocator,
                                      physical_address);
            break;
        }
        chunk = valid_bytes - page_offset;
        if ((uint64_t)chunk > request - total) {
            chunk = (size_t)(request - total);
        }
        if (physical_page_resolve(files->heap->page_allocator,
                                  physical_address,
                                  &page) != PHYSICAL_PAGE_STATUS_OK) {
            (void)physical_page_release(files->heap->page_allocator,
                                        physical_address);
            return KERNEL_FILES_STATUS_STATE;
        }
        access_status = kernel_copy_to_user(mm,
                                            user_buffer + total,
                                            (unsigned char *)page + page_offset,
                                            chunk,
                                            &copied);
        (void)physical_page_release(files->heap->page_allocator,
                                  physical_address);
        total += copied;
        kernel_open_file_read_progress(description, position, copied);
        position += copied;
        if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
            files->record->statistics.read_failures++;
            *linux_result = total != 0U ? (int64_t)total : -KERNEL_EFAULT;
            files->record->statistics.bytes_read += total;
            return KERNEL_FILES_STATUS_OK;
        }
        if (access_status != KERNEL_UACCESS_STATUS_OK || copied != chunk) {
            return KERNEL_FILES_STATUS_STATE;
        }
        if (page_offset + chunk == valid_bytes && valid_bytes < BOAROS_PAGE_SIZE) {
            break;
        }
    }
    files->record->statistics.bytes_read += total;
    *linux_result = (int64_t)total;
    return KERNEL_FILES_STATUS_OK;
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
    files->record->statistics.read_calls++;
    status = kernel_files_pin_data(files, fd, &description, linux_result);
    if (status != KERNEL_FILES_STATUS_OK) return status;
    if (*linux_result != 0) {
        files->record->statistics.read_failures++;
        return KERNEL_FILES_STATUS_OK;
    }
    status = pread_pinned(files, mm, description, user_buffer, count,
                          offset, linux_result);
    return release_io_description(files, &description, status);
}

static int description_writable(
    const struct kernel_open_file_description *description)
{
    uint32_t access_mode;

    if (description == 0) {
        return 0;
    }
    access_mode = description->open_flags & 3U;
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_REGULAR) {
        return access_mode == 1U || access_mode == 2U;
    }
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_GENERATED)
        return access_mode == 1U || access_mode == 2U;
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_PIPE) {
        return access_mode != 0U;
    }
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_SOCKET) {
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
                               uint64_t position, uint64_t deadline)
{
    struct kernel_open_file_description *output = *output_owner;
    enum kernel_open_file_kind kind = kernel_open_file_kind(output);
    if (kind == KERNEL_OPEN_FILE_KIND_PIPE) {
        int64_t result;
        if (kernel_pipe_write_buffer(output->pipe, buffer, size,
                output->open_flags, &result) != KERNEL_PIPE_STATUS_OK)
            __builtin_trap();
        if (result > 0 && output->file.private_data &&
            !kernel_vfs_mount_is_readonly(output->file.mount)) {
            int error = kernel_vfs_file_modified(&output->file, 0, 0);
            if (error) return error;
        }
        return result;
    }
    if (kind == KERNEL_OPEN_FILE_KIND_SOCKET) {
        struct kernel_socket *socket = kernel_open_file_socket(output);
        if (kernel_socket_is_datagram(socket))
            return kernel_socket_write_datagram_buffer(output_owner, buffer, size,
                (output->open_flags & KERNEL_FILES_O_NONBLOCK) ? KERNEL_SOCKET_MSG_DONTWAIT : 0U);
        for (;;) {
            int result = kernel_socket_write_buffer(socket, buffer, (uint32_t)size, 0);
            if (result != -KERNEL_EAGAIN) return result;
            uint64_t now = deadline ? kernel_time_monotonic_ns() : 0;
            int waited = deadline && now >= deadline ? -KERNEL_EAGAIN :
                socket_wait_ready(output, KERNEL_POLLOUT, deadline ? deadline - now : 0, 0);
            if (waited != 0) return waited;
        }
    }
    size_t written = 0;
    int error;
    if (kind == KERNEL_OPEN_FILE_KIND_REGULAR) {
        /* pwrite 每批自取 rank 15；跨输入读取持目标门闩会形成交叉 inode 等待。 */
        error = kernel_vfs_pwrite(&output->file, position, buffer, size, &written);
    } else if (output->device && output->device->write) {
        error = output->device->write(output->device_instance, kernel_task_current(), output->open_flags, buffer, size, &written);
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
        if (output->open_flags & KERNEL_FILES_O_APPEND) return -KERNEL_EINVAL;
    }
    if (input->kind != KERNEL_OPEN_FILE_KIND_REGULAR) return -KERNEL_EINVAL;
    if (count) kernel_open_file_read_begin(input, start);
    kernel_vfs_file_accessed(&input->file);
    if (count == 0U || start >= kernel_open_file_size(input)) return 0;
    struct kernel_task_io_buffer scratch = {0};
    if (kernel_task_io_buffer_acquire(&scratch, mm->allocator) != KERNEL_TASK_STATUS_OK)
        return -KERNEL_ENOMEM;
    size_t capacity = BOAROS_PAGE_SIZE;
    void *staging = scratch.data, *datagram_staging = 0;
    if (kernel_socket_is_datagram(kernel_open_file_socket(output))) {
        /* Linux splice_to_socket batches at most sixteen target pages. */
        size_t batch=(size_t)BOAROS_PAGE_SIZE*16U;
        capacity=count>batch ? batch : (size_t)count;
        enum kernel_heap_status allocated = kernel_heap_allocate(files->heap, capacity, &datagram_staging);
        if (allocated != KERNEL_HEAP_STATUS_OK) {
            kernel_task_io_buffer_release(&scratch);
            if (allocated != KERNEL_HEAP_STATUS_EMPTY) __builtin_trap();
            return -KERNEL_ENOMEM;
        }
        staging = datagram_staging;
    }
    uint64_t timeout = output->kind == KERNEL_OPEN_FILE_KIND_SOCKET
        ? kernel_socket_send_timeout(kernel_open_file_socket(output)) : 0;
    uint64_t deadline = 0;
    if (timeout != 0U) {
        uint64_t now = kernel_time_monotonic_ns();
        deadline = UINT64_MAX - now < timeout ? UINT64_MAX : now + timeout;
    }
    while (total < count) {
        size_t wanted = count - total > capacity
            ? capacity : (size_t)(count - total), available = 0;
        /* pread 交还 inode 数据锁后才写目标，允许同 inode 复制而不锁升级。 */
        error = kernel_open_file_pread(input, start + total, staging, wanted, &available);
        if (available > wanted) __builtin_trap();
        if (available == 0U) break;
        if (total == 0U && output->kind == KERNEL_OPEN_FILE_KIND_REGULAR) {
            int modified = kernel_vfs_file_modified(&output->file, out_start, 0);
            if (modified != 0) { error = modified; break; }
        }
        int64_t sent = sendfile_output(output_owner, staging, available,
                                       out_start + total, deadline);
        if (sent < 0) { error = (int)sent; break; }
        if ((uint64_t)sent > available) __builtin_trap();
        if (sent > 0 && output->kind == KERNEL_OPEN_FILE_KIND_REGULAR &&
            (output->open_flags & KERNEL_FILES_O_DSYNC) != 0U) {
            int datasync = (output->open_flags & KERNEL_FILES_O_SYNC) != KERNEL_FILES_O_SYNC;
            int synced = kernel_vfs_sync(&output->file, datasync, &output->observed_writeback_error);
            if (synced != 0) { error = synced; break; }
        }
        total += (uint64_t)sent;
        if ((size_t)sent < available || error != 0) break;
    }
    if (datagram_staging != 0 && kernel_heap_release(files->heap, datagram_staging) != KERNEL_HEAP_STATUS_OK)
        __builtin_trap();
    kernel_task_io_buffer_release(&scratch);
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
    files->record->statistics.read_calls++;
    files->record->statistics.write_calls++;
    if (status != KERNEL_FILES_STATUS_OK || *linux_result != 0) goto done;
    if (!kernel_open_file_readable(input)) { *linux_result = -KERNEL_EBADF; goto done; }
    if (user_offset != 0U && (input->kind == KERNEL_OPEN_FILE_KIND_PIPE ||
            input->kind == KERNEL_OPEN_FILE_KIND_SOCKET || (input->device && !input->device->positioned))) {
        *linux_result = -KERNEL_ESPIPE; goto done;
    }
    uint64_t checked_position = user_offset != 0U ? (uint64_t)position : input->offset;
    if (position < 0 || count > (uint64_t)INT64_MAX ||
        checked_position > (uint64_t)INT64_MAX - count) {
        *linux_result = -KERNEL_EINVAL; goto done;
    }
    status = kernel_files_pin_data(files, out_fd, &output, linux_result);
    if (status != KERNEL_FILES_STATUS_OK || *linux_result != 0) goto done;
    if (!description_writable(output)) { *linux_result = -KERNEL_EBADF; goto done; }
    /* 两份 pin 由仍会恢复的 syscall 栈持有；退出先唤醒等待，正常展开再交还。 */
    *linux_result = sendfile_pinned(files, mm, input, &output, user_offset != 0U, &position, count);
    if (*linux_result > 0) {
        files->record->statistics.bytes_read += (uint64_t)*linux_result;
        files->record->statistics.bytes_written += (uint64_t)*linux_result;
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
        if (kernel_heap_release(files->heap, buffer) != KERNEL_HEAP_STATUS_OK) __builtin_trap();
        if (access != KERNEL_UACCESS_STATUS_FAULT) return KERNEL_FILES_STATUS_STATE;
        *linux_result = -KERNEL_EFAULT;
        return KERNEL_FILES_STATUS_OK;
    }
    if (!result) result = kernel_vfs_file_control(&description->file, 1,
        positioned ? requested_offset : kernel_open_file_offset(description),
        buffer, (size_t)count, &accepted);
    if (kernel_heap_release(files->heap, buffer) != KERNEL_HEAP_STATUS_OK) __builtin_trap();
    if (result) { *linux_result = result; return KERNEL_FILES_STATUS_OK; }
    if (!positioned && kernel_open_file_advance(description, accepted) != KERNEL_OPEN_FILE_STATUS_OK)
        return KERNEL_FILES_STATUS_STATE;
    *linux_result = (int64_t)accepted;
    return KERNEL_FILES_STATUS_OK;
}

static enum kernel_files_status buffered_write_request(
    struct kernel_files *files, struct kernel_mm *mm,
    struct kernel_open_file_description *description,
    const struct kernel_uaccess_iovec *iov, size_t iov_count,
    uint64_t count, int positioned, uint64_t requested_offset,
    int64_t *linux_result, unsigned char *staging, size_t capacity,
    uint32_t socket_flags)
{
    uint64_t total = 0U;

    if (description->device && description->device->discard_writes) {
        size_t consumed = 0U;
        int result = description->device->write(description->device_instance, kernel_task_current(), description->open_flags, 0, (size_t)count, &consumed);
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
        if (kernel_pipe_writev(description->pipe, mm, iov, iov_count,
                count, description->open_flags, linux_result) != KERNEL_PIPE_STATUS_OK)
            return KERNEL_FILES_STATUS_STATE;
        if (*linux_result > 0 && description->file.private_data &&
            !kernel_vfs_mount_is_readonly(description->file.mount)) {
            int error = kernel_vfs_file_modified(&description->file, 0, 0);
            if (error) *linux_result = error;
        }
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_SOCKET) {
        struct kernel_socket *socket = kernel_open_file_socket(description);
        uint64_t timeout = kernel_socket_send_timeout(socket), target = 0;
        if (timeout) {
            uint64_t now = kernel_time_monotonic_ns();
            target = UINT64_MAX - now < timeout ? UINT64_MAX : now + timeout;
        }
        for (size_t index = 0U; index < iov_count && total < count; index++) {
            uint64_t offset = 0U;
            while (offset < iov[index].length && total < count) {
                size_t chunk = iov[index].length - offset;
                size_t copied = 0U;
                int sent;
                enum kernel_uaccess_status access;
                if (chunk > capacity) chunk = capacity;
                size_t page_left = BOAROS_PAGE_SIZE -
                    ((iov[index].base + offset) & BOAROS_PAGE_MASK);
                if (chunk > page_left) chunk = page_left;
                if (chunk > count - total) chunk = (size_t)(count - total);
                files->record->statistics.write_chunks++;
                access = kernel_copy_from_user(mm, staging,
                    iov[index].base + offset, chunk, &copied);
                if (access == KERNEL_UACCESS_STATUS_FAULT && copied == 0U) {
                    *linux_result = total != 0U ? (int64_t)total
                                                 : -KERNEL_EFAULT;
                    return KERNEL_FILES_STATUS_OK;
                }
                if (access != KERNEL_UACCESS_STATUS_OK &&
                    access != KERNEL_UACCESS_STATUS_FAULT)
                    return KERNEL_FILES_STATUS_STATE;
                size_t consumed = 0U;
                /* 等待和短发送只推进暂存游标，不能重新解析尚未发送的用户字节。 */
                while (consumed < copied) {
                    sent = kernel_socket_write_buffer(socket, (const unsigned char *)staging + consumed,
                                                        (uint32_t)(copied - consumed), socket_flags);
                    if (sent == -KERNEL_EAGAIN) {
                        uint64_t now = target ? kernel_time_monotonic_ns() : 0;
                        int waited = target && now >= target ? -KERNEL_EAGAIN :
                            socket_wait_ready(description, KERNEL_POLLOUT,
                                              target ? target-now : 0, socket_flags);
                        if (waited == 0) continue;
                        if (total && waited == -KERNEL_ERESTARTSYS)
                            kernel_signal_clear_syscall_restart(kernel_task_current());
                        sent = waited;
                    }
                    if (sent < 0) {
                        *linux_result = total != 0U ? (int64_t)total : sent;
                        return KERNEL_FILES_STATUS_OK;
                    }
                    if (sent == 0 || (size_t)sent > copied - consumed)
                        return KERNEL_FILES_STATUS_STATE;
                    consumed += (size_t)sent;
                    total += (uint64_t)sent;
                    offset += (uint64_t)sent;
                    /* fault或非阻塞仍返回本次已接受前缀。 */
                    if (access == KERNEL_UACCESS_STATUS_FAULT || (consumed < copied &&
                        ((description->open_flags & KERNEL_FILES_O_NONBLOCK) ||
                         (socket_flags & KERNEL_SOCKET_MSG_DONTWAIT)))) {
                        *linux_result = (int64_t)total;
                        return KERNEL_FILES_STATUS_OK;
                    }
                }
            }
        }
        *linux_result = (int64_t)total;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_REGULAR) {
        int is_append = (description->open_flags & KERNEL_FILES_O_APPEND) != 0U;
        uint64_t file_offset = positioned ? requested_offset :
                               kernel_open_file_offset(description);

        /* ext4 modifies mtime/ctime before usercopy, including a first-byte
         * fault. Zero-length requests and earlier validation failures skip it. */
        if (count != 0U) {
            int result = kernel_vfs_file_modified(&description->file,
                                                   file_offset, is_append);
            if (result != 0) {
                *linux_result = result;
                return KERNEL_FILES_STATUS_OK;
            }
        }
        for (size_t index = 0U; index < iov_count && total < count; index++) {
            uint64_t offset = 0U;

            while (offset < iov[index].length && total < count) {
                size_t chunk = (size_t)(iov[index].length - offset);
                size_t copied = 0U;
                size_t written = 0U;
                enum kernel_uaccess_status status;
                int vfs_result;

                if ((uint64_t)chunk > count - total) {
                    chunk = (size_t)(count - total);
                }
                if (chunk > capacity) {
                    chunk = capacity;
                }
                files->record->statistics.write_chunks++;
                status = kernel_copy_from_user(mm, staging,
                                                iov[index].base + offset,
                                                chunk, &copied);
                if ((status != KERNEL_UACCESS_STATUS_OK &&
                     status != KERNEL_UACCESS_STATUS_FAULT) ||
                    copied > chunk ||
                    (status == KERNEL_UACCESS_STATUS_OK && copied != chunk)) {
                    return KERNEL_FILES_STATUS_STATE;
                }
                if (copied == 0U) {
                    if (total != 0U) {
                        files->record->statistics.write_failures++;
                    }
                    *linux_result = total != 0U ? (int64_t)total : -KERNEL_EFAULT;
                    return KERNEL_FILES_STATUS_OK;
                }
                /* A user fault leaves a valid prefix in staging. Only the
                 * backend's committed bytes belong to the file or offset. */
                if (is_append) {
                    uint64_t new_offset = 0U;
                    vfs_result = kernel_vfs_append(&description->file,
                                                   staging,
                                                   copied,
                                                   &new_offset,
                                                   &written);
                    if (written > copied) {
                        return KERNEL_FILES_STATUS_STATE;
                    }
                    if (written != 0U && !positioned) {
                        description->offset = new_offset;
                        file_offset = new_offset;
                    }
                } else {
                    vfs_result = kernel_vfs_pwrite(&description->file,
                                                   file_offset,
                                                   staging,
                                                   copied,
                                                   &written);
                    if (written > copied) {
                        return KERNEL_FILES_STATUS_STATE;
                    }
                    if (written != 0U) {
                        file_offset += (uint64_t)written;
                        if (!positioned) description->offset = file_offset;
                    }
                }
                total += (uint64_t)written;
                offset += (uint64_t)written;
                if (vfs_result != 0) {
                    files->record->statistics.write_failures++;
                    *linux_result = total != 0U ? (int64_t)total : vfs_result;
                    return KERNEL_FILES_STATUS_OK;
                }
                if (status == KERNEL_UACCESS_STATUS_FAULT) {
                    if (total != 0U) {
                        files->record->statistics.write_failures++;
                    }
                    *linux_result = total != 0U ? (int64_t)total : -KERNEL_EFAULT;
                    return KERNEL_FILES_STATUS_OK;
                }
                if (written < copied) {
                    *linux_result = (int64_t)total;
                    return KERNEL_FILES_STATUS_OK;
                }
            }
        }
        *linux_result = (int64_t)total;
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
            int result = description->device->write(description->device_instance, kernel_task_current(), description->open_flags, staging, copied, &written);
            if (written > copied) return KERNEL_FILES_STATUS_STATE;
            total += written;
            offset += written;
            if (result || written < copied) {
                *linux_result = total ? (int64_t)total : result;
                return KERNEL_FILES_STATUS_OK;
            }
            if (status == KERNEL_UACCESS_STATUS_FAULT) {
                if (total != 0U) {
                    files->record->statistics.write_failures++;
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

static enum kernel_files_status tcp_write_request(
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
    uint64_t total = 0, target = 0, timeout = kernel_socket_send_timeout(socket);
    size_t staged = 0, consumed = 0;
    int fault = 0;
    if (timeout) {
        uint64_t now = kernel_time_monotonic_ns();
        target = UINT64_MAX - now < timeout ? UINT64_MAX : now + timeout;
    }
    kernel_socket_stream_begin(&request, &description);
    do {
        uint64_t address = 0;
        size_t chunk = staged - consumed;
        if (!chunk && count) {
            size_t limit = count - total < BOAROS_PAGE_SIZE ? (size_t)(count - total) : BOAROS_PAGE_SIZE;
            chunk = kernel_uaccess_iov_span(&cursor, limit, &address);
            if (!chunk) { status = KERNEL_FILES_STATUS_STATE; break; }
        }
        int result = kernel_socket_stream_reserve(&request, (uint32_t)chunk, flags);
        size_t copied_now = 0;
        if (result >= 0) {
            if (!count) { *linux_result = 0; break; }
            if (!result || (size_t)result > chunk) { status = KERNEL_FILES_STATUS_STATE; break; }
            chunk = (size_t)result;
            if (staged == consumed) {
                if (!buffer.allocator) {
                    if (kernel_task_io_buffer_acquire(&buffer, mm->allocator) != KERNEL_TASK_STATUS_OK) {
                        *linux_result = total ? (int64_t)total : -KERNEL_ENOMEM; break;
                    }
                    COST_IO_ADD(5, BOAROS_PAGE_SIZE);
                }
                files->record->statistics.write_chunks++;
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
                (const unsigned char *)buffer.data + consumed, (uint32_t)chunk, flags);
        }
        if (result == -KERNEL_EAGAIN) {
            if (!total) COST_ADD(STREAM_COPY_BLOCKED_BYTES, copied_now);
            uint64_t now = target ? kernel_time_monotonic_ns() : 0;
            result = target && now >= target ? -KERNEL_EAGAIN : socket_wait_ready(borrowed,
                KERNEL_POLLOUT, target ? target - now : 0, flags);
            if (!result) continue; /* 暂存尾部留在当前请求，下次只重取接纳预算。 */
            if (total && result == -KERNEL_ERESTARTSYS)
                kernel_signal_clear_syscall_restart(kernel_task_current());
        }
        if (result < 0) { *linux_result = total ? (int64_t)total : result; break; }
        if (!result || (size_t)result > staged - consumed) { status = KERNEL_FILES_STATUS_STATE; break; }
        consumed += (size_t)result; total += (uint64_t)result;
        *linux_result = (int64_t)total;
        if (fault || (consumed < staged && ((borrowed->open_flags & KERNEL_FILES_O_NONBLOCK) ||
            (flags & KERNEL_SOCKET_MSG_DONTWAIT)))) break;
    } while (total < count);
    kernel_socket_stream_finish(&request);
    if (buffer.allocator) kernel_task_io_buffer_release(&buffer);
    return status;
}

static enum kernel_files_status write_request(
    struct kernel_files *files, struct kernel_mm *mm,
    struct kernel_open_file_description *description,
    const struct kernel_uaccess_iovec *iov, size_t iov_count,
    uint64_t count, int positioned, uint64_t requested_offset,
    int64_t *linux_result, uint32_t socket_flags)
{
    COST_SCOPE(write_cost, OPERATION_TICKS);
    COST_IO_SCOPE(io_cost,
        kernel_socket_is_datagram(kernel_open_file_socket(description)) ? 2 :
        kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_REGULAR ? 0 :
        kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_SOCKET ? 1 : 3);
    COST_IO_ADD(0, 1); COST_IO_ADD(1, count);
    if (kernel_socket_is_datagram(kernel_open_file_socket(description))) {
        *linux_result = kernel_socket_write_datagram(&description, mm, iov,
            iov_count, count, socket_flags |
                ((description->open_flags & KERNEL_FILES_O_NONBLOCK) ? KERNEL_SOCKET_MSG_DONTWAIT : 0U), 0);
        COST_IO_ADD(2, *linux_result > 0 ? (uint64_t)*linux_result : 0);
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_socket_is_tcp(kernel_open_file_socket(description))) {
        enum kernel_files_status status = tcp_write_request(files, mm, description,
            iov, iov_count, count, socket_flags, linux_result);
        if (status == KERNEL_FILES_STATUS_OK) COST_IO_ADD(2, *linux_result > 0 ? (uint64_t)*linux_result : 0);
        return status;
    }
    KERNEL_LOCK_SCOPE(offset_guard);
    if ((!positioned && kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_REGULAR) ||
        kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_GENERATED)
        kernel_mutex_lock(&description->offset_lock, &offset_guard);
    KERNEL_LOCK_SCOPE(operation_guard);
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_REGULAR)
        kernel_vfs_file_write_lock(&description->file, &operation_guard);
    struct kernel_task_io_buffer buffer = {0};
    unsigned char small[KERNEL_FILES_WRITE_STAGING];
    unsigned char *staging = small;
    size_t capacity = sizeof(small);
    enum kernel_open_file_kind kind = kernel_open_file_kind(description);
    if (count != 0U && (kind == KERNEL_OPEN_FILE_KIND_REGULAR ||
                        kind == KERNEL_OPEN_FILE_KIND_SOCKET)) {
        if (kernel_task_io_buffer_acquire(&buffer, mm->allocator) !=
                KERNEL_TASK_STATUS_OK) {
            *linux_result = -KERNEL_ENOMEM;
            return KERNEL_FILES_STATUS_OK;
        }
        staging = buffer.data;
        capacity = BOAROS_PAGE_SIZE;
        COST_IO_ADD(5, BOAROS_PAGE_SIZE);
    }
    enum kernel_files_status status = buffered_write_request(files, mm,
        description, iov, iov_count, count, positioned, requested_offset,
        linux_result, staging, capacity, socket_flags);
    if (status == KERNEL_FILES_STATUS_OK) COST_IO_ADD(2, *linux_result > 0 ? (uint64_t)*linux_result : 0);
    if (buffer.allocator != 0) kernel_task_io_buffer_release(&buffer);
    if (status == KERNEL_FILES_STATUS_OK && *linux_result > 0 &&
        kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_REGULAR &&
        (description->open_flags & KERNEL_FILES_O_DSYNC) != 0U) {
        int datasync = (description->open_flags & KERNEL_FILES_O_SYNC) !=
                        KERNEL_FILES_O_SYNC;
        int error = kernel_vfs_sync(&description->file, datasync,
                                     &description->observed_writeback_error);
        /* generic_write_sync returns the error after accepted bytes have
         * advanced the OFD offset, including a prefix before usercopy fault. */
        if (error != 0) *linux_result = error;
    }
    return status;
}

enum kernel_files_status kernel_files_socket_iov_io(
    struct kernel_files *files, struct kernel_mm *mm,
    struct kernel_open_file_description **owner,
    const struct kernel_uaccess_iovec *iov, size_t iov_count, uint64_t count,
    uint32_t flags, int writing, struct kernel_socket_address *peer,
    uint32_t *message_size, int64_t *linux_result)
{
    if (!kernel_files_is_live(files) || mm == 0 || owner == 0 || *owner == 0 ||
        kernel_open_file_socket(*owner) == 0 || linux_result == 0)
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    if (count > KERNEL_FILES_MAX_RW_COUNT) count = KERNEL_FILES_MAX_RW_COUNT;
    if (writing) {
        files->record->statistics.write_calls++;
        enum kernel_files_status status = write_request(files, mm, *owner, iov, iov_count, count, 0, 0, linux_result, flags);
        if (status == KERNEL_FILES_STATUS_OK) {
            if (*linux_result >= 0) files->record->statistics.bytes_written += (uint64_t)*linux_result;
            else files->record->statistics.write_failures++;
        }
        return status;
    }
    files->record->statistics.read_calls++;
    return read_pinned(files, mm, *owner, owner, iov, iov_count, count, linux_result, flags, peer, message_size);
}
enum kernel_files_status kernel_files_socket_io(
    struct kernel_files *files, struct kernel_mm *mm,
    struct kernel_open_file_description **owner, uint64_t user_buffer,
    uint64_t count, uint32_t flags, int writing, int64_t *linux_result)
{
    const struct kernel_uaccess_iovec iov = {user_buffer, count};
    return kernel_files_socket_iov_io(files, mm, owner, &iov, 1, count, flags, writing, 0, 0, linux_result);
}

static void account_write(struct kernel_files *files, int64_t result)
{
    if (result >= 0) {
        files->record->statistics.bytes_written += (uint64_t)result;
    } else {
        files->record->statistics.write_failures++;
    }
}

static int memory_device_empty_range_fault(
    const struct kernel_open_file_description *description,
    uint64_t user_buffer, uint64_t count)
{
    return description->device && description->device->empty_range_fault &&
           count == 0U && user_buffer != 0U &&
           kernel_user_range_check(user_buffer - 1U, 1U) !=
               KERNEL_UACCESS_STATUS_OK;
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
    files->record->statistics.write_calls++;
    status = kernel_files_pin_data(files, fd, &description, linux_result);
    if (status != KERNEL_FILES_STATUS_OK) {
        return status;
    }
    if (*linux_result != 0) {
        *linux_result = -KERNEL_EBADF;
        account_write(files, *linux_result);
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_EPOLL) {
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
    if (count > KERNEL_FILES_MAX_RW_COUNT) {
        count = KERNEL_FILES_MAX_RW_COUNT;
    }
    if (kernel_user_range_check(user_buffer, (size_t)count) !=
            KERNEL_UACCESS_STATUS_OK ||
        memory_device_empty_range_fault(description, user_buffer, count)) {
        *linux_result = -KERNEL_EFAULT;
        account_write(files, *linux_result);
        return release_io_description(files, &description,
                                      KERNEL_FILES_STATUS_OK);
    }
    if (description->device && description->device->writev) {
        status = description->device->writev(description->device_instance,
            kernel_task_current(), files, &description, 0, mm, &iov, 1U,
            count, description->open_flags, linux_result);
    } else status = write_request(files, mm, description, &iov, 1U, count,
                           0, 0U, linux_result, 0U);
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
    files->record->statistics.write_calls++;
    status = kernel_files_pin_data(files, fd, &description, linux_result);
    if (status != KERNEL_FILES_STATUS_OK) return status;
    if (*linux_result != 0) {
        *linux_result = -KERNEL_EBADF;
        account_write(files, *linux_result);
        return KERNEL_FILES_STATUS_OK;
    }
    kind = kernel_open_file_kind(description);
    if (kind == KERNEL_OPEN_FILE_KIND_EPOLL ||
        kind == KERNEL_OPEN_FILE_KIND_SOCKET)
        *linux_result = -KERNEL_ESPIPE;
    else if (!description_writable(description))
        *linux_result = -KERNEL_EBADF;
    else if (offset < 0)
        *linux_result = -KERNEL_EINVAL;
    else if ((description->device && !description->device->positioned) ||
             kind == KERNEL_OPEN_FILE_KIND_PIPE)
        *linux_result = -KERNEL_ESPIPE;
    else if (kind == KERNEL_OPEN_FILE_KIND_DIRECTORY)
        *linux_result = -KERNEL_EISDIR;
    else
        *linux_result = 0;
    if (*linux_result != 0) {
        account_write(files, *linux_result);
        return release_io_description(files, &description,
                                      KERNEL_FILES_STATUS_OK);
    }
    if (count > KERNEL_FILES_MAX_RW_COUNT) count = KERNEL_FILES_MAX_RW_COUNT;
    if (kernel_user_range_check(user_buffer, (size_t)count) !=
            KERNEL_UACCESS_STATUS_OK ||
        memory_device_empty_range_fault(description, user_buffer, count)) {
        *linux_result = -KERNEL_EFAULT;
        account_write(files, *linux_result);
        return release_io_description(files, &description,
                                      KERNEL_FILES_STATUS_OK);
    }
    status = write_request(files, mm, description, &iov, 1U, count,
                           1, (uint64_t)offset, linux_result, 0U);
    if (status == KERNEL_FILES_STATUS_OK) account_write(files, *linux_result);
    return release_io_description(files, &description, status);
}

enum kernel_files_status kernel_files_writev(
    struct kernel_files *files, struct kernel_mm *mm, int64_t fd,
    uint64_t user_iov, uint64_t iovcnt, int64_t *linux_result)
{
    struct kernel_uaccess_iovec local[8];
    struct kernel_uaccess_iovec *iov = local;
    struct kernel_open_file_description *description = 0;
    uint64_t total = 0U;
    size_t copied = 0U;
    enum kernel_files_status status = KERNEL_FILES_STATUS_OK;
    enum kernel_uaccess_status access;

    if (!kernel_files_is_live(files) || mm == 0 || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    files->record->statistics.write_calls++;
    status = kernel_files_pin_data(files, fd, &description, linux_result);
    if (status != KERNEL_FILES_STATUS_OK) {
        return status;
    }
    if (*linux_result != 0) {
        *linux_result = -KERNEL_EBADF;
        account_write(files, *linux_result);
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_EPOLL) {
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
    if (iovcnt > 1024U) {
        *linux_result = -KERNEL_EINVAL;
        goto out;
    }
    if (iovcnt > sizeof(local) / sizeof(local[0])) {
        enum kernel_heap_status allocation =
            kernel_heap_allocate(files->heap, iovcnt * sizeof(*iov),
                                  (void **)&iov);

        if (allocation != KERNEL_HEAP_STATUS_OK) {
            if (allocation != KERNEL_HEAP_STATUS_EMPTY) {
                status = KERNEL_FILES_STATUS_STATE;
                goto out;
            }
            *linux_result = -KERNEL_ENOMEM;
            goto out;
        }
    }
    /* Import descriptors once before any output or blocking. This is also
     * what makes aggregate PIPE_BUF checks independent of iovec layout. */
    access = kernel_copy_from_user(mm, iov, user_iov, iovcnt * sizeof(*iov),
                                    &copied);
    if (access != KERNEL_UACCESS_STATUS_OK || copied != iovcnt * sizeof(*iov)) {
        *linux_result = -KERNEL_EFAULT;
        if (access != KERNEL_UACCESS_STATUS_FAULT) {
            status = KERNEL_FILES_STATUS_STATE;
        }
        goto out;
    }
    for (size_t index = 0U; index < iovcnt; index++) {
        if (iov[index].length > (uint64_t)INT64_MAX - total) {
            *linux_result = -KERNEL_EINVAL;
            goto out;
        }
        if (kernel_user_range_check(iov[index].base, iov[index].length) !=
                KERNEL_UACCESS_STATUS_OK ||
            memory_device_empty_range_fault(description, iov[index].base,
                                            iov[index].length)) {
            *linux_result = -KERNEL_EFAULT;
            goto out;
        }
        total += iov[index].length;
    }
    if (total > KERNEL_FILES_MAX_RW_COUNT) {
        total = KERNEL_FILES_MAX_RW_COUNT;
    }
    if (description->device && description->device->writev) {
        status = description->device->writev(description->device_instance,
            kernel_task_current(), files, &description,
            iov == local ? 0 : (void **)&iov, mm, iov, iovcnt,
            total, description->open_flags, linux_result);
    } else status = write_request(files, mm, description, iov, iovcnt, total,
                           0, 0U, linux_result, 0U);
out:
    if (iov && iov != local &&
        kernel_files_release_allocation(files, iov) ==
            KERNEL_FILES_STATUS_STATE) {
        status = KERNEL_FILES_STATUS_STATE;
    }
    if (status == KERNEL_FILES_STATUS_OK) {
        account_write(files, *linux_result);
    }
    return release_io_description(files, &description, status);
}

static enum kernel_files_status lseek_pinned(
    struct kernel_open_file_description *description, int64_t offset,
    uint64_t whence, int64_t *linux_result)
{
    int64_t current, target;
    KERNEL_LOCK_SCOPE(offset_guard);
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
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_EPOLL) {
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
    if (target < 0) {
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

enum kernel_files_status kernel_files_lseek(
    struct kernel_files *files, int64_t fd, int64_t offset,
    uint64_t whence, int64_t *linux_result)
{
    struct kernel_open_file_description *description = 0;
    if (!kernel_files_is_live(files) || !linux_result)
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    enum kernel_files_status status = kernel_files_pin_data(files, fd, &description, linux_result);
    if (status != KERNEL_FILES_STATUS_OK || *linux_result) return status;
    if (whence > 4U) {
        *linux_result = -KERNEL_EINVAL;
        return release_io_description(files, &description, KERNEL_FILES_STATUS_OK);
    }
    status = lseek_pinned(description, offset, whence, linux_result);
    return release_io_description(files, &description, status);
}

enum kernel_files_status kernel_files_ftruncate(
    struct kernel_files *files,
    int64_t fd,
    uint64_t length,
    int64_t *linux_result)
{
    struct kernel_open_file_description *description = 0;
    enum kernel_files_status status;
    int vfs_result;

    if (!kernel_files_is_live(files) || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    status = kernel_files_pin_data(files, fd, &description, linux_result);
    if (status != KERNEL_FILES_STATUS_OK) {
        return status;
    }
    if (*linux_result != 0) {
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_DIRECTORY) {
        *linux_result = -KERNEL_EISDIR;
        return release_io_description(files, &description, KERNEL_FILES_STATUS_OK);
    }
    if (kernel_open_file_kind(description) != KERNEL_OPEN_FILE_KIND_REGULAR) {
        *linux_result = -KERNEL_EINVAL;
        return release_io_description(files, &description, KERNEL_FILES_STATUS_OK);
    }
    if (!description_writable(description)) {
        *linux_result = -KERNEL_EINVAL;
        return release_io_description(files, &description, KERNEL_FILES_STATUS_OK);
    }
    vfs_result = kernel_vfs_ftruncate(&description->file, length);
    *linux_result = vfs_result;
    return release_io_description(files, &description, KERNEL_FILES_STATUS_OK);
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
    if (count == 0U) {
        *linux_result = 0;
        return KERNEL_FILES_STATUS_OK;
    }
    request = count > KERNEL_FILES_MAX_RW_COUNT
                  ? KERNEL_FILES_MAX_RW_COUNT
                  : count;

    KERNEL_LOCK_SCOPE(offset_guard);
    kernel_mutex_lock(&description->offset_lock, &offset_guard);
    /* The descriptor offset is the backend cookie for the next record. */
    position = kernel_open_file_offset(description);
    while (total < request) {
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
            files->record->statistics.bytes_read += total;
            return KERNEL_FILES_STATUS_OK;
        }
        name_length = strlen(name);
        record_length = (19U + name_length + 1U + 7U) & ~(size_t)7U;
        if ((uint64_t)record_length > request - total) {
            /* Linux answers EINVAL when nothing fits at all. */
            *linux_result = total == 0U ? -KERNEL_EINVAL : (int64_t)total;
            files->record->statistics.bytes_read += total;
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
            files->record->statistics.bytes_read += total;
            return KERNEL_FILES_STATUS_OK;
        }
        total += record_length;
        position = next_position;
        if (kernel_open_file_seek(description, position) !=
            KERNEL_OPEN_FILE_STATUS_OK) {
            return KERNEL_FILES_STATUS_STATE;
        }
    }
    files->record->statistics.bytes_read += total;
    *linux_result = (int64_t)total;
    return KERNEL_FILES_STATUS_OK;
}

#if BOAROS_COST_DIAGNOSTICS
int kernel_files_cost_descriptor(struct kernel_files *files, int64_t fd)
{
    struct kernel_open_file_description *description = kernel_files_lookup_description(files, fd);
    return description && kernel_vfs_file_is_cost(&description->file);
}
#endif
