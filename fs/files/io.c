#include "private.h"
#include "../open_file_internal.h"
#include "../pipe_internal.h"
#include "../uaccess_iov_internal.h"

#include <kernel/console.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/open_file.h>
#include <kernel/page.h>
#include <kernel/page_cache.h>
#include <kernel/uaccess.h>
#include <kernel/vfs.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define KERNEL_FILES_MAX_RW_COUNT \
    ((uint64_t)INT32_MAX & ~(uint64_t)BOAROS_PAGE_MASK)
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

static enum kernel_files_status read_pinned(
    struct kernel_files *files,
    struct kernel_mm *mm,
    struct kernel_open_file_description *description,
    const struct kernel_uaccess_iovec *iov,
    size_t iov_count,
    uint64_t count,
    int64_t *linux_result)
{
    uint64_t request;
    uint64_t total = 0U;
    struct kernel_uaccess_iov_cursor cursor = {iov, iov_count, 0U, 0U};

    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_CONSOLE) {
        return kernel_files_read_console(files, mm, iov, iov_count, count,
                                         linux_result);
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
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_REGULAR &&
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
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_REGULAR) {
        kernel_vfs_file_accessed(&description->file);
    }
    request = count > KERNEL_FILES_MAX_RW_COUNT
                  ? KERNEL_FILES_MAX_RW_COUNT
                  : count;
    while (total < request) {
        uint64_t file_offset = kernel_open_file_offset(description);
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
    status = kernel_files_pin(files, fd, &description, linux_result);
    if (status != KERNEL_FILES_STATUS_OK) {
        return status;
    }
    if (*linux_result != 0) {
        files->record->statistics.read_failures++;
        return KERNEL_FILES_STATUS_OK;
    }
    status = read_pinned(files, mm, description, &iov, 1U, count,
                         linux_result);
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
    status = kernel_files_pin(files, fd, &description, linux_result);
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
    if ((kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_REGULAR ||
         kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_PIPE) &&
        !kernel_open_file_readable(description)) {
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
    status = read_pinned(files, mm, description, iov, (size_t)iovcnt,
                         total, linux_result);
    dispatched = 1;
out:
    if (iov != local &&
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

enum kernel_files_status kernel_files_pread(
    struct kernel_files *files,
    struct kernel_mm *mm,
    int64_t fd,
    uint64_t user_buffer,
    uint64_t count,
    int64_t offset,
    int64_t *linux_result)
{
    struct kernel_open_file_description *description;
    uint64_t position;
    uint64_t request;
    uint64_t total = 0U;
    uint64_t file_size;

    if (!kernel_files_is_live(files) || mm == 0 || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    files->record->statistics.read_calls++;
    description = kernel_files_lookup_description(files, fd);
    if (description == 0) {
        files->record->statistics.read_failures++;
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_REGULAR &&
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
    if (kernel_open_file_kind(description) ==
            KERNEL_OPEN_FILE_KIND_CONSOLE ||
        kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_PIPE) {
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
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_REGULAR) {
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
    while (total < request) {
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
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_PIPE) {
        return access_mode != 0U;
    }
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_CONSOLE) {
        return 1;
    }
    return 0;
}

static enum kernel_files_status write_request(
    struct kernel_files *files, struct kernel_mm *mm,
    struct kernel_open_file_description *description,
    const struct kernel_uaccess_iovec *iov, size_t iov_count,
    uint64_t count, int64_t *linux_result)
{
    uint64_t total = 0U;
    unsigned char staging[KERNEL_FILES_WRITE_STAGING];

    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_PIPE) {
        return kernel_pipe_writev(description->pipe, mm, iov, iov_count,
                                   count, description->open_flags, linux_result)
                       == KERNEL_PIPE_STATUS_OK
                   ? KERNEL_FILES_STATUS_OK : KERNEL_FILES_STATUS_STATE;
    }
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_REGULAR) {
        int is_append = (description->open_flags & KERNEL_FILES_O_APPEND) != 0U;
        uint64_t file_offset = kernel_open_file_offset(description);

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
                if (chunk > sizeof(staging)) {
                    chunk = sizeof(staging);
                }
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
                    if (written != 0U) {
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
                        description->offset = file_offset;
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
    for (size_t index = 0U; index < iov_count && total < count; index++) {
        uint64_t offset = 0U;

        while (offset < iov[index].length && total < count) {
            size_t chunk = iov[index].length - offset;
            size_t copied = 0U;
            enum kernel_uaccess_status status;

            if (chunk > count - total) {
                chunk = count - total;
            }
            if (chunk > sizeof(staging)) {
                chunk = sizeof(staging);
            }
            status = kernel_copy_from_user(mm, staging,
                                            iov[index].base + offset,
                                            chunk, &copied);
            for (size_t byte = 0U; byte < copied; byte++) {
                kernel_console_putc((char)staging[byte]);
            }
            total += copied;
            offset += copied;
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

static void account_write(struct kernel_files *files, int64_t result)
{
    if (result >= 0) {
        files->record->statistics.bytes_written += (uint64_t)result;
    } else {
        files->record->statistics.write_failures++;
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
    files->record->statistics.write_calls++;
    status = kernel_files_pin(files, fd, &description, linux_result);
    if (status != KERNEL_FILES_STATUS_OK) {
        return status;
    }
    if (*linux_result != 0) {
        *linux_result = -KERNEL_EBADF;
        account_write(files, *linux_result);
        return KERNEL_FILES_STATUS_OK;
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
        KERNEL_UACCESS_STATUS_OK) {
        *linux_result = -KERNEL_EFAULT;
        account_write(files, *linux_result);
        return release_io_description(files, &description,
                                      KERNEL_FILES_STATUS_OK);
    }
    status = write_request(files, mm, description, &iov, 1U, count, linux_result);
    if (status == KERNEL_FILES_STATUS_OK) {
        account_write(files, *linux_result);
    }
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
    status = kernel_files_pin(files, fd, &description, linux_result);
    if (status != KERNEL_FILES_STATUS_OK) {
        return status;
    }
    if (*linux_result != 0) {
        *linux_result = -KERNEL_EBADF;
        account_write(files, *linux_result);
        return KERNEL_FILES_STATUS_OK;
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
            KERNEL_UACCESS_STATUS_OK) {
            *linux_result = -KERNEL_EFAULT;
            goto out;
        }
        total += iov[index].length;
    }
    if (total > KERNEL_FILES_MAX_RW_COUNT) {
        total = KERNEL_FILES_MAX_RW_COUNT;
    }
    status = write_request(files, mm, description, iov, iovcnt, total,
                           linux_result);
out:
    if (iov != local &&
        kernel_files_release_allocation(files, iov) ==
            KERNEL_FILES_STATUS_STATE) {
        status = KERNEL_FILES_STATUS_STATE;
    }
    if (status == KERNEL_FILES_STATUS_OK) {
        account_write(files, *linux_result);
    }
    return release_io_description(files, &description, status);
}

enum kernel_files_status kernel_files_lseek(
    struct kernel_files *files,
    int64_t fd,
    int64_t offset,
    uint64_t whence,
    int64_t *linux_result)
{
    struct kernel_open_file_description *description;
    int64_t current;
    int64_t target;

    if (!kernel_files_is_live(files) || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    description = kernel_files_lookup_description(files, fd);
    if (description == 0) {
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_open_file_kind(description) ==
            KERNEL_OPEN_FILE_KIND_CONSOLE ||
        kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_PIPE) {
        *linux_result = -KERNEL_ESPIPE;
        return KERNEL_FILES_STATUS_OK;
    }
    current = (int64_t)kernel_open_file_offset(description);
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
    *linux_result = target;
    return KERNEL_FILES_STATUS_OK;
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
    status = kernel_files_pin(files, fd, &description, linux_result);
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
    description = kernel_files_lookup_description(files, fd);
    if (description == 0) {
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
