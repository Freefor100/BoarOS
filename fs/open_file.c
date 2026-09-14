#include "open_file_internal.h"
#include "pipe_internal.h"
#include "vfs_internal.h"
#include "files/epoll_internal.h"

#include <kernel/console.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/open_file.h>
#include <kernel/page_cache.h>
#include <kernel/vfs.h>

#include <stddef.h>
#include <stdint.h>

static int open_file_live(
    const struct kernel_open_file_description *file);

enum kernel_open_file_status kernel_open_file_create(
    struct kernel_heap *heap,
    struct kernel_vfs_mount *mount,
    const char *path,
    struct kernel_open_file_description **owner,
    int *linux_result)
{
    struct kernel_open_file_description *file;
    enum kernel_heap_status heap_status;
    int result;

    if (heap == 0 || mount == 0 || path == 0 || owner == 0 ||
        *owner != 0 || linux_result == 0) {
        return KERNEL_OPEN_FILE_STATUS_INVALID_ARGUMENT;
    }
    heap_status = kernel_heap_allocate_zeroed(heap,
                                              1U,
                                              sizeof(*file),
                                              (void **)&file);
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        if (heap_status == KERNEL_HEAP_STATUS_EMPTY) {
            *linux_result = -KERNEL_ENOMEM;
            return KERNEL_OPEN_FILE_STATUS_OK;
        }
        return KERNEL_OPEN_FILE_STATUS_STATE;
    }
    result = kernel_vfs_open(mount, path, &file->file);
    if (result != 0) {
        file->heap = heap;
        file->vfs_closed = 1U;
        if (kernel_heap_release(heap, file) != KERNEL_HEAP_STATUS_OK) {
            *owner = file;
            return KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED;
        }
        *linux_result = result;
        return KERNEL_OPEN_FILE_STATUS_OK;
    }
    file->heap = heap;
    file->references = 1U;
    *owner = file;
    *linux_result = 0;
    return KERNEL_OPEN_FILE_STATUS_OK;
}

enum kernel_open_file_status kernel_open_file_create_mode(
    struct kernel_heap *heap,
    struct kernel_vfs_mount *mount,
    const char *path,
    uint32_t mode,
    struct kernel_open_file_description **owner,
    int *linux_result)
{
    struct kernel_open_file_description *file;
    enum kernel_heap_status heap_status;
    int result;

    if (heap == 0 || mount == 0 || path == 0 || owner == 0 ||
        *owner != 0 || linux_result == 0) {
        return KERNEL_OPEN_FILE_STATUS_INVALID_ARGUMENT;
    }
    heap_status = kernel_heap_allocate_zeroed(heap,
                                              1U,
                                              sizeof(*file),
                                              (void **)&file);
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        if (heap_status == KERNEL_HEAP_STATUS_EMPTY) {
            *linux_result = -KERNEL_ENOMEM;
            return KERNEL_OPEN_FILE_STATUS_OK;
        }
        return KERNEL_OPEN_FILE_STATUS_STATE;
    }
    result = kernel_vfs_create(mount, path, mode, &file->file);
    if (result != 0) {
        file->heap = heap;
        file->vfs_closed = 1U;
        if (kernel_heap_release(heap, file) != KERNEL_HEAP_STATUS_OK) {
            *owner = file;
            return KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED;
        }
        *linux_result = result;
        return KERNEL_OPEN_FILE_STATUS_OK;
    }
    file->heap = heap;
    file->references = 1U;
    file->kind = KERNEL_OPEN_FILE_KIND_REGULAR;
    *owner = file;
    *linux_result = 0;
    return KERNEL_OPEN_FILE_STATUS_OK;
}

enum kernel_open_file_status kernel_open_file_create_executable(
    struct kernel_heap *heap,
    struct kernel_vfs_mount *mount,
    const char *path,
    struct kernel_open_file_description **owner,
    int *linux_result)
{
    struct kernel_open_file_description *file;
    enum kernel_heap_status heap_status;
    int result;

    if (heap == 0 || mount == 0 || path == 0 || owner == 0 ||
        *owner != 0 || linux_result == 0) {
        return KERNEL_OPEN_FILE_STATUS_INVALID_ARGUMENT;
    }
    heap_status = kernel_heap_allocate_zeroed(heap,
                                              1U,
                                              sizeof(*file),
                                              (void **)&file);
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        if (heap_status == KERNEL_HEAP_STATUS_EMPTY) {
            *linux_result = -KERNEL_ENOMEM;
            return KERNEL_OPEN_FILE_STATUS_OK;
        }
        return KERNEL_OPEN_FILE_STATUS_STATE;
    }
    result = kernel_vfs_open_executable(mount, path, &file->file);
    if (result != 0) {
        file->heap = heap;
        file->vfs_closed = 1U;
        if (kernel_heap_release(heap, file) != KERNEL_HEAP_STATUS_OK) {
            *owner = file;
            return KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED;
        }
        *linux_result = result;
        return KERNEL_OPEN_FILE_STATUS_OK;
    }
    file->heap = heap;
    file->references = 1U;
    *owner = file;
    *linux_result = 0;
    return KERNEL_OPEN_FILE_STATUS_OK;
}

enum kernel_open_file_status kernel_open_file_create_console(
    struct kernel_heap *heap,
    struct kernel_open_file_description **owner)
{
    struct kernel_open_file_description *file;
    enum kernel_heap_status heap_status;

    if (heap == 0 || owner == 0 || *owner != 0) {
        return KERNEL_OPEN_FILE_STATUS_INVALID_ARGUMENT;
    }
    heap_status = kernel_heap_allocate_zeroed(heap,
                                              1U,
                                              sizeof(*file),
                                              (void **)&file);
    if (heap_status == KERNEL_HEAP_STATUS_EMPTY) {
        return KERNEL_OPEN_FILE_STATUS_NO_MEMORY;
    }
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        return KERNEL_OPEN_FILE_STATUS_STATE;
    }
    file->heap = heap;
    file->references = 1U;
    file->kind = KERNEL_OPEN_FILE_KIND_CONSOLE;
    *owner = file;
    return KERNEL_OPEN_FILE_STATUS_OK;
}

enum kernel_open_file_status kernel_open_file_create_pipe(
    struct kernel_heap *heap,
    struct kernel_pipe *pipe,
    uint32_t endpoint,
    uint64_t flags,
    struct kernel_open_file_description **owner)
{
    struct kernel_open_file_description *file;
    enum kernel_heap_status heap_status;
    enum kernel_pipe_status pipe_status;

    if (heap == 0 || pipe == 0 || owner == 0 || *owner != 0 ||
        (endpoint != KERNEL_PIPE_ENDPOINT_READ &&
         endpoint != KERNEL_PIPE_ENDPOINT_WRITE)) {
        return KERNEL_OPEN_FILE_STATUS_INVALID_ARGUMENT;
    }
    heap_status = kernel_heap_allocate_zeroed(heap,
                                              1U,
                                              sizeof(*file),
                                              (void **)&file);
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        return heap_status == KERNEL_HEAP_STATUS_EMPTY
                   ? KERNEL_OPEN_FILE_STATUS_NO_MEMORY
                   : KERNEL_OPEN_FILE_STATUS_STATE;
    }
    /* Keep an allocated description self-owned even if endpoint acquisition
     * fails and heap cleanup has to be retried by the file table. */
    file->heap = heap;
    file->vfs_closed = 1U;
    pipe_status = kernel_pipe_acquire_endpoint(pipe, endpoint);
    if (pipe_status != KERNEL_PIPE_STATUS_OK) {
        if (kernel_heap_release(heap, file) != KERNEL_HEAP_STATUS_OK) {
            *owner = file;
            return KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED;
        }
        return pipe_status == KERNEL_PIPE_STATUS_NO_MEMORY
                   ? KERNEL_OPEN_FILE_STATUS_NO_MEMORY
                   : KERNEL_OPEN_FILE_STATUS_STATE;
    }
    file->references = 1U;
    file->kind = KERNEL_OPEN_FILE_KIND_PIPE;
    file->file.mode = KERNEL_VFS_S_IFIFO | UINT32_C(0000600);
    file->open_flags = (uint32_t)flags |
                       (endpoint == KERNEL_PIPE_ENDPOINT_WRITE ? 1U : 0U);
    file->pipe = pipe;
    file->pipe_endpoint = (uint8_t)endpoint;
    file->vfs_closed = 0U;
    *owner = file;
    return KERNEL_OPEN_FILE_STATUS_OK;
}

enum kernel_open_file_status kernel_open_file_create_epoll(
    struct kernel_heap *heap,
    struct kernel_epoll *epoll,
    uint32_t flags,
    struct kernel_open_file_description **owner)
{
    struct kernel_open_file_description *file;
    enum kernel_heap_status heap_status;

    if (heap == 0 || epoll == 0 || owner == 0 || *owner != 0) {
        return KERNEL_OPEN_FILE_STATUS_INVALID_ARGUMENT;
    }
    heap_status = kernel_heap_allocate_zeroed(heap,
                                              1U,
                                              sizeof(*file),
                                              (void **)&file);
    if (heap_status == KERNEL_HEAP_STATUS_EMPTY) {
        return KERNEL_OPEN_FILE_STATUS_NO_MEMORY;
    }
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        return KERNEL_OPEN_FILE_STATUS_STATE;
    }
    file->heap = heap;
    file->references = 1U;
    file->kind = KERNEL_OPEN_FILE_KIND_EPOLL;
    file->epoll = epoll;
    file->open_flags = flags;
    file->vfs_closed = 0U;
    epoll->file = file;
    *owner = file;
    return KERNEL_OPEN_FILE_STATUS_OK;
}

enum kernel_open_file_kind kernel_open_file_kind(
    const struct kernel_open_file_description *file)
{
    if (file == 0) {
        return KERNEL_OPEN_FILE_KIND_REGULAR;
    }
    switch (file->kind) {
    case KERNEL_OPEN_FILE_KIND_DIRECTORY:
        return KERNEL_OPEN_FILE_KIND_DIRECTORY;
    case KERNEL_OPEN_FILE_KIND_CONSOLE:
        return KERNEL_OPEN_FILE_KIND_CONSOLE;
    case KERNEL_OPEN_FILE_KIND_PIPE:
        return KERNEL_OPEN_FILE_KIND_PIPE;
    case KERNEL_OPEN_FILE_KIND_EPOLL:
        return KERNEL_OPEN_FILE_KIND_EPOLL;
    default:
        return KERNEL_OPEN_FILE_KIND_REGULAR;
    }
}

int kernel_open_file_supports_epoll(
    const struct kernel_open_file_description *file)
{
    if (file == 0) {
        return 0;
    }
    switch (file->kind) {
    case KERNEL_OPEN_FILE_KIND_PIPE:
    case KERNEL_OPEN_FILE_KIND_CONSOLE:
    case KERNEL_OPEN_FILE_KIND_EPOLL:
        return 1;
    default:
        return 0;
    }
}

enum kernel_open_file_status kernel_open_file_acquire(
    struct kernel_open_file_description *file)
{
    if (file == 0 || file->heap == 0 || file->references == 0U ||
        file->references == UINT32_MAX || file->vfs_closed) {
        return KERNEL_OPEN_FILE_STATUS_STATE;
    }
    file->references++;
    return KERNEL_OPEN_FILE_STATUS_OK;
}

enum kernel_open_file_status kernel_open_file_release(
    struct kernel_open_file_description **owner)
{
    struct kernel_open_file_description *file;
    enum kernel_pipe_status pipe_status;

    if (owner == 0 || *owner == 0) {
        return KERNEL_OPEN_FILE_STATUS_INVALID_ARGUMENT;
    }
    file = *owner;
    /* A detached last reference is kept at zero until cleanup drains it. */
    if (file->heap == 0 || file->references == UINT32_MAX) {
        return KERNEL_OPEN_FILE_STATUS_STATE;
    }
    if (file->references > 1U) {
        file->references--;
        *owner = 0;
        return KERNEL_OPEN_FILE_STATUS_OK;
    }
    if (file->references == 1U) {
        file->references = 0U;
    }
    if (file->ep_items != 0) {
        kernel_epoll_notify_file_release(file);
    }
    if (!file->vfs_closed) {
        if (file->kind == KERNEL_OPEN_FILE_KIND_CONSOLE) {
            file->vfs_closed = 1U;
        } else if (file->kind == KERNEL_OPEN_FILE_KIND_PIPE) {
            if (file->pipe_endpoint_closed == 0U) {
                pipe_status = kernel_pipe_release_endpoint(
                    file->pipe,
                    file->pipe_endpoint);
                if (pipe_status != KERNEL_PIPE_STATUS_OK) {
                    return pipe_status == KERNEL_PIPE_STATUS_CLEANUP_REQUIRED
                                ? KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED
                                : KERNEL_OPEN_FILE_STATUS_STATE;
                }
                file->pipe_endpoint_closed = 1U;
                file->pipe = 0;
            }
            file->vfs_closed = 1U;
        } else if (file->kind == KERNEL_OPEN_FILE_KIND_EPOLL) {
            if (file->epoll != 0) {
                kernel_epoll_destroy(file->epoll);
                file->epoll = 0;
            }
            file->vfs_closed = 1U;
        } else if (kernel_vfs_close(&file->file) != 0) {
            return KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED;
        } else {
            file->vfs_closed = 1U;
        }
    }
    if (kernel_heap_release(file->heap, file) !=
        KERNEL_HEAP_STATUS_OK) {
        return KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED;
    }
    *owner = 0;
    return KERNEL_OPEN_FILE_STATUS_OK;
}

enum kernel_open_file_status kernel_open_file_detach(
    struct kernel_open_file_description **owner)
{
    struct kernel_open_file_description *file;

    if (owner == 0 || *owner == 0) {
        return KERNEL_OPEN_FILE_STATUS_INVALID_ARGUMENT;
    }
    file = *owner;
    if (!open_file_live(file)) {
        return KERNEL_OPEN_FILE_STATUS_STATE;
    }
    /* A pipe endpoint or epoll descriptor's last live owner closes it immediately,
     * even when its allocation must survive on a cleanup list. */
    if ((file->kind == KERNEL_OPEN_FILE_KIND_PIPE ||
         file->kind == KERNEL_OPEN_FILE_KIND_EPOLL) &&
        file->references == 1U) {
        return kernel_open_file_release(owner);
    }
    file->references--;
    if (file->references != 0U) {
        *owner = 0;
        return KERNEL_OPEN_FILE_STATUS_OK;
    }
    return KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED;
}

static int open_file_live(const struct kernel_open_file_description *file)
{
    return file != 0 && file->heap != 0 && file->references != 0U &&
           !file->vfs_closed;
}

uint64_t kernel_open_file_size(
    const struct kernel_open_file_description *file)
{
    return open_file_live(file) ? kernel_vfs_file_size(&file->file) : 0U;
}

uint32_t kernel_open_file_mode(
    const struct kernel_open_file_description *file)
{
    return open_file_live(file) ? file->file.mode : 0U;
}

uint32_t kernel_open_file_flags(
    const struct kernel_open_file_description *file)
{
    return open_file_live(file) ? file->open_flags : 0U;
}

uint64_t kernel_open_file_offset(
    const struct kernel_open_file_description *file)
{
    return open_file_live(file) ? file->offset : 0U;
}

enum kernel_open_file_status kernel_open_file_seek(
    struct kernel_open_file_description *file,
    uint64_t offset)
{
    if (!open_file_live(file)) {
        return KERNEL_OPEN_FILE_STATUS_STATE;
    }
    file->offset = offset;
    return KERNEL_OPEN_FILE_STATUS_OK;
}

enum kernel_open_file_status kernel_open_file_advance(
    struct kernel_open_file_description *file,
    uint64_t bytes)
{
    if (!open_file_live(file) || bytes > UINT64_MAX - file->offset) {
        return KERNEL_OPEN_FILE_STATUS_STATE;
    }
    file->offset += bytes;
    return KERNEL_OPEN_FILE_STATUS_OK;
}

enum kernel_page_cache_status kernel_open_file_get_page(
    struct kernel_open_file_description *file,
    uint64_t page_index,
    uint64_t *physical_address,
    size_t *valid_bytes)
{
    if (!open_file_live(file) || file->file.mount == 0 ||
        file->file.mount->private_data == 0) {
        return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    }
    return kernel_page_cache_get(
        kernel_vfs_file_page_cache(&file->file),
        &file->file,
        page_index,
        physical_address,
        valid_bytes);
}

enum kernel_page_cache_status kernel_open_file_lookup_page(
    struct kernel_open_file_description *file,
    uint64_t page_index,
    uint64_t *physical_address,
    size_t *valid_bytes)
{
    if (!open_file_live(file) || file->file.mount == 0 ||
        file->file.mount->private_data == 0) {
        return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    }
    return kernel_page_cache_lookup(
        kernel_vfs_file_page_cache(&file->file),
        &file->file,
        page_index,
        physical_address,
        valid_bytes);
}

int kernel_open_file_pread(struct kernel_open_file_description *file,
                           uint64_t offset,
                           void *buffer,
                           size_t size,
                           size_t *bytes_read)
{
    return open_file_live(file)
               ? kernel_vfs_pread(&file->file,
                                  offset,
                                  buffer,
                                  size,
                                  bytes_read)
               : -KERNEL_EINVAL;
}

uint32_t kernel_open_file_poll(
    struct kernel_open_file_description *file,
    uint32_t requested_events,
    struct kernel_wait_queue **out_queue)
{
    if (out_queue != 0) {
        *out_queue = 0;
    }
    if (!open_file_live(file)) {
        return KERNEL_POLLNVAL;
    }
    switch (file->kind) {
    case KERNEL_OPEN_FILE_KIND_PIPE:
        return kernel_pipe_poll(file->pipe, file->pipe_endpoint, out_queue);
    case KERNEL_OPEN_FILE_KIND_CONSOLE:
        return kernel_console_poll(requested_events, out_queue);
    case KERNEL_OPEN_FILE_KIND_REGULAR:
    case KERNEL_OPEN_FILE_KIND_DIRECTORY:
        return KERNEL_POLLIN | KERNEL_POLLOUT | KERNEL_POLLRDNORM | KERNEL_POLLWRNORM;
    case KERNEL_OPEN_FILE_KIND_EPOLL:
        return kernel_epoll_poll(file->epoll, requested_events, out_queue);
    default:
        return KERNEL_POLLNVAL;
    }
}
