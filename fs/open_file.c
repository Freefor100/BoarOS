#include "open_file_internal.h"
#include "record_lock.h"
#include "pipe_internal.h"
#include "vfs_internal.h"
#include "char_device_internal.h"
#include "files/epoll_internal.h"

#include <arch/riscv/context.h>
#include <kernel/console.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/open_file.h>
#include <kernel/page_cache.h>
#include <kernel/memory_object.h>
#include <kernel/tmpfs.h>
#include <kernel/socket.h>
#include <kernel/vfs.h>

#include <stddef.h>
#include <stdint.h>

static int open_file_live(
    const struct kernel_open_file_description *file);

static uint64_t next_socket_proc_identity = 1U;

struct kernel_vfs_path *kernel_open_file_path(
    const struct kernel_open_file_description *description)
{
    return open_file_live(description) ? description->file.path : 0;
}

static void release_record_locks(struct kernel_open_file_description *file)
{
    if (!file->record_locks) return;
    struct kernel_vfs_node *node = kernel_open_file_node(file);
    if (!node) __builtin_trap();
    kernel_record_lock_release(kernel_vfs_node_record_locks(node),
                               &file->record_locks, file->heap);
}

static enum kernel_open_file_status create_open_file(
    struct kernel_heap *heap,
    struct kernel_vfs_mount *mount,
    const char *path,
    struct kernel_open_file_description **owner,
    int *linux_result,
    int follow_final)
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
    result = follow_final
                 ? kernel_vfs_open(mount, path, &file->file)
                 : kernel_vfs_open_nofollow(mount, path, &file->file);
    if (result != 0) {
        file->heap = heap;
        file->vfs_closed = 1U;
        (void)kernel_heap_release(heap, file);
        *linux_result = result;
        return KERNEL_OPEN_FILE_STATUS_OK;
    }
    file->heap = heap;
    file->references = 1U;
    kernel_mutex_init(&file->offset_lock, 10, (uintptr_t)file);
    if (kernel_vfs_file_generated(&file->file))
        file->kind = KERNEL_OPEN_FILE_KIND_GENERATED;
    file->observed_writeback_error = kernel_vfs_error_sequence(&file->file);
    *owner = file;
    *linux_result = 0;
    return KERNEL_OPEN_FILE_STATUS_OK;
}

enum kernel_open_file_status kernel_open_file_create_at(
    struct kernel_heap *heap, struct kernel_vfs_path *start,
    struct kernel_vfs_path *root, const char *path,
    enum kernel_open_file_path_operation operation, uint32_t mode,
    struct kernel_open_file_description **owner, int *linux_result)
{
    struct kernel_open_file_description *file;
    if (!heap || !start || !root || !path || !owner || *owner || !linux_result)
        return KERNEL_OPEN_FILE_STATUS_INVALID_ARGUMENT;
    enum kernel_heap_status allocation = kernel_heap_allocate_zeroed(heap,
                                         1U, sizeof(*file), (void **)&file);
    if (allocation != KERNEL_HEAP_STATUS_OK) {
        if (allocation != KERNEL_HEAP_STATUS_EMPTY) return KERNEL_OPEN_FILE_STATUS_STATE;
        *linux_result = -KERNEL_ENOMEM;
        return KERNEL_OPEN_FILE_STATUS_OK;
    }
    int result;
    switch (operation) {
    case KERNEL_OPEN_PATH_CREATE:
        result = kernel_vfs_create_at(start, root, path, mode, &file->file);
        break;
    case KERNEL_OPEN_PATH_EXECUTABLE:
        result = kernel_vfs_open_executable_at(start, root, path, &file->file);
        break;
    case KERNEL_OPEN_PATH_FOLLOW:
    case KERNEL_OPEN_PATH_NOFOLLOW:
        result = kernel_vfs_open_at(start, root, path,
                    operation == KERNEL_OPEN_PATH_FOLLOW, &file->file);
        break;
    default: result = -KERNEL_EINVAL; break;
    }
    if (result) {
        if (kernel_heap_release(heap, file) != KERNEL_HEAP_STATUS_OK) __builtin_trap();
    } else {
        file->heap = heap;
        file->references = 1U;
        kernel_mutex_init(&file->offset_lock, 10, (uintptr_t)file);
        if (kernel_vfs_file_generated(&file->file))
            file->kind = KERNEL_OPEN_FILE_KIND_GENERATED;
        file->observed_writeback_error = kernel_vfs_error_sequence(&file->file);
        *owner = file;
    }
    *linux_result = result;
    return KERNEL_OPEN_FILE_STATUS_OK;
}

enum kernel_open_file_status kernel_open_file_create(
    struct kernel_heap *heap,
    struct kernel_vfs_mount *mount,
    const char *path,
    struct kernel_open_file_description **owner,
    int *linux_result)
{
    return create_open_file(heap, mount, path, owner, linux_result, 1);
}

enum kernel_open_file_status kernel_open_file_create_nofollow(
    struct kernel_heap *heap,
    struct kernel_vfs_mount *mount,
    const char *path,
    struct kernel_open_file_description **owner,
    int *linux_result)
{
    return create_open_file(heap, mount, path, owner, linux_result, 0);
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
        (void)kernel_heap_release(heap, file);
        *linux_result = result;
        return KERNEL_OPEN_FILE_STATUS_OK;
    }
    file->heap = heap;
    file->references = 1U;
    kernel_mutex_init(&file->offset_lock, 10, (uintptr_t)file);
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
        (void)kernel_heap_release(heap, file);
        *linux_result = result;
        return KERNEL_OPEN_FILE_STATUS_OK;
    }
    file->heap = heap;
    file->references = 1U;
    kernel_mutex_init(&file->offset_lock, 10, (uintptr_t)file);
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
    kernel_mutex_init(&file->offset_lock, 10, (uintptr_t)file);
    file->kind = KERNEL_OPEN_FILE_KIND_CONSOLE;
    file->device = kernel_char_device_lookup(UINT64_C(0x501));
    if (!file->device) __builtin_trap();
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
         endpoint != KERNEL_PIPE_ENDPOINT_WRITE &&
         endpoint != KERNEL_PIPE_ENDPOINT_BOTH)) {
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
    /* Keep the description self-contained until endpoint acquisition commits. */
    file->heap = heap;
    file->vfs_closed = 1U;
    pipe_status = kernel_pipe_acquire_endpoint(pipe, endpoint);
    if (pipe_status != KERNEL_PIPE_STATUS_OK) {
        (void)kernel_heap_release(heap, file);
        return pipe_status == KERNEL_PIPE_STATUS_NO_MEMORY
                   ? KERNEL_OPEN_FILE_STATUS_NO_MEMORY
                   : KERNEL_OPEN_FILE_STATUS_STATE;
    }
    file->references = 1U;
    kernel_mutex_init(&file->offset_lock, 10, (uintptr_t)file);
    file->kind = KERNEL_OPEN_FILE_KIND_PIPE;
    file->file.mode = KERNEL_VFS_S_IFIFO | UINT32_C(0000600);
    file->open_flags = (uint32_t)flags |
                       (endpoint == KERNEL_PIPE_ENDPOINT_WRITE ? 1U :
                        endpoint == KERNEL_PIPE_ENDPOINT_BOTH ? 2U : 0U);
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
    kernel_mutex_init(&file->offset_lock, 10, (uintptr_t)file);
    file->kind = KERNEL_OPEN_FILE_KIND_EPOLL;
    file->epoll = epoll;
    file->open_flags = flags;
    file->vfs_closed = 0U;
    epoll->file = file;
    *owner = file;
    return KERNEL_OPEN_FILE_STATUS_OK;
}

enum kernel_open_file_status kernel_open_file_create_socket(
    struct kernel_heap *heap, struct kernel_socket *socket,
    uint32_t flags, struct kernel_open_file_description **owner)
{
    struct kernel_open_file_description *file = 0;
    enum kernel_heap_status heap_status;
    if (heap == 0 || socket == 0 || owner == 0 || *owner != 0)
        return KERNEL_OPEN_FILE_STATUS_INVALID_ARGUMENT;
    heap_status = kernel_heap_allocate_zeroed(heap, 1U, sizeof(*file),
                                              (void **)&file);
    if (heap_status == KERNEL_HEAP_STATUS_EMPTY)
        return KERNEL_OPEN_FILE_STATUS_NO_MEMORY;
    if (heap_status != KERNEL_HEAP_STATUS_OK)
        return KERNEL_OPEN_FILE_STATUS_STATE;
    file->heap = heap;
    file->references = 1U;
    kernel_mutex_init(&file->offset_lock, 10, (uintptr_t)file);
    file->kind = KERNEL_OPEN_FILE_KIND_SOCKET;
    file->file.mode = KERNEL_VFS_S_IFSOCK | UINT32_C(0000600);
    file->open_flags = flags;
    file->socket = socket;
    uintptr_t irq = riscv_interrupt_save();
    if (!next_socket_proc_identity) __builtin_trap();
    file->proc_identity = next_socket_proc_identity++;
    riscv_interrupt_restore(irq);
    *owner = file;
    return KERNEL_OPEN_FILE_STATUS_OK;
}

uint64_t kernel_open_file_pseudo_identity(
    const struct kernel_open_file_description *file)
{
    if (!open_file_live(file)) return 0U;
    if (file->kind == KERNEL_OPEN_FILE_KIND_PIPE)
        return kernel_pipe_proc_identity(file->pipe);
    if (file->kind == KERNEL_OPEN_FILE_KIND_SOCKET)
        return file->proc_identity;
    return 0U;
}

int kernel_open_file_pseudo_stat(
    const struct kernel_open_file_description *file,
    struct kernel_vfs_stat *stat)
{
    if (!open_file_live(file) || !stat || kernel_open_file_path(file))
        return -KERNEL_ENOTSUP;
    uint32_t mode;
    uint64_t rdev = 0U;
    if (file->kind == KERNEL_OPEN_FILE_KIND_PIPE)
        mode = KERNEL_VFS_S_IFIFO | 0600U;
    else if (file->kind == KERNEL_OPEN_FILE_KIND_SOCKET)
        mode = KERNEL_VFS_S_IFSOCK | 0600U;
    else if (file->kind == KERNEL_OPEN_FILE_KIND_EPOLL)
        mode = 0600U;
    else if (file->kind == KERNEL_OPEN_FILE_KIND_CONSOLE) {
        mode = KERNEL_VFS_S_IFCHR | 0600U;
        rdev = UINT64_C(0x501);
    } else return -KERNEL_ENOTSUP;
    *stat = (struct kernel_vfs_stat){
        .mode = mode,
        .nlink = 1U,
        .rdev = rdev,
        .size = kernel_open_file_size(file),
        .blksize = BOAROS_PAGE_SIZE,
    };
    return 0;
}

int kernel_open_file_pipe_pin(
    const struct kernel_open_file_description *source, uint32_t flags,
    struct kernel_open_file_pipe_pin *pin)
{
    if (!open_file_live(source) || !pin || pin->pipe ||
        source->kind != KERNEL_OPEN_FILE_KIND_PIPE) return -KERNEL_EINVAL;
    unsigned mode = flags & 3U;
    if (mode > 2U) return -KERNEL_EINVAL;
    uint32_t endpoint = mode == 0U ? KERNEL_PIPE_ENDPOINT_READ :
                        mode == 1U ? KERNEL_PIPE_ENDPOINT_WRITE :
                                     KERNEL_PIPE_ENDPOINT_BOTH;
    if (kernel_pipe_acquire_endpoint(source->pipe, endpoint) !=
        KERNEL_PIPE_STATUS_OK) return -KERNEL_EIO;
    pin->pipe = source->pipe;
    pin->endpoint = (uint8_t)endpoint;
    return 0;
}

int kernel_open_file_pipe_finish(struct kernel_heap *heap,
    struct kernel_open_file_pipe_pin *pin, uint32_t flags,
    struct kernel_open_file_description **owner)
{
    if (!heap || !pin || !pin->pipe || !owner || *owner)
        return -KERNEL_EINVAL;
    enum kernel_open_file_status status = kernel_open_file_create_pipe(heap,
        pin->pipe, pin->endpoint, flags, owner);
    if (kernel_pipe_release_endpoint(pin->pipe, pin->endpoint) !=
        KERNEL_PIPE_STATUS_OK) __builtin_trap();
    pin->pipe = 0;
    pin->endpoint = 0U;
    return status == KERNEL_OPEN_FILE_STATUS_OK ? 0 :
           status == KERNEL_OPEN_FILE_STATUS_NO_MEMORY ? -KERNEL_ENOMEM :
                                                          -KERNEL_EIO;
}

struct kernel_socket *kernel_open_file_socket(
    struct kernel_open_file_description *file)
{
    return open_file_live(file) && file->kind == KERNEL_OPEN_FILE_KIND_SOCKET
               ? file->socket : 0;
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
    case KERNEL_OPEN_FILE_KIND_NULL:
        return KERNEL_OPEN_FILE_KIND_NULL;
    case KERNEL_OPEN_FILE_KIND_ZERO:
        return KERNEL_OPEN_FILE_KIND_ZERO;
    case KERNEL_OPEN_FILE_KIND_SOCKET:
        return KERNEL_OPEN_FILE_KIND_SOCKET;
    case KERNEL_OPEN_FILE_KIND_GENERATED:
        return KERNEL_OPEN_FILE_KIND_GENERATED;
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
    case KERNEL_OPEN_FILE_KIND_SOCKET:
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
        release_record_locks(file);
        file->references = 0U;
    }
    if (file->ep_items != 0) {
        kernel_epoll_notify_file_release(file);
    }
    if (file->generated_ready) kernel_open_file_reset_generated(file);
    if (!file->vfs_closed) {
        if (file->kind == KERNEL_OPEN_FILE_KIND_CONSOLE &&
            file->file.private_data == 0) {
            file->vfs_closed = 1U;
        } else if (file->kind == KERNEL_OPEN_FILE_KIND_PIPE) {
            if (file->pipe_endpoint_closed == 0U) {
                pipe_status = kernel_pipe_release_endpoint(
                    file->pipe,
                    file->pipe_endpoint);
                if (pipe_status != KERNEL_PIPE_STATUS_OK) {
                    return KERNEL_OPEN_FILE_STATUS_STATE;
                }
                file->pipe_endpoint_closed = 1U;
                file->pipe = 0;
            }
            file->vfs_closed = 1U;
        } else if (file->kind == KERNEL_OPEN_FILE_KIND_EPOLL) {
            if (file->epoll != 0) {
                enum kernel_files_status epoll_status =
                    kernel_epoll_destroy(file->epoll);
                if (epoll_status != KERNEL_FILES_STATUS_OK) {
                    return epoll_status == KERNEL_FILES_STATUS_CLEANUP_REQUIRED
                               ? KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED
                               : KERNEL_OPEN_FILE_STATUS_STATE;
                }
                file->epoll = 0;
            }
            file->vfs_closed = 1U;
        } else if (file->kind == KERNEL_OPEN_FILE_KIND_SOCKET) {
            kernel_socket_destroy(file->socket);
            file->socket = 0;
            file->vfs_closed = 1U;
        } else if (kernel_vfs_close(&file->file) != 0) {
            return KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED;
        } else {
            file->vfs_closed = 1U;
        }
    }
    (void)kernel_heap_release(file->heap, file);
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
    /* A pipe endpoint's last live owner closes it immediately. */
    if ((file->kind == KERNEL_OPEN_FILE_KIND_PIPE ||
         file->kind == KERNEL_OPEN_FILE_KIND_SOCKET) &&
        file->references == 1U) {
        return kernel_open_file_release(owner);
    }
    if (file->references == 1U) release_record_locks(file);
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

struct kernel_vfs_node *kernel_open_file_node(
    const struct kernel_open_file_description *description)
{
    return description != 0 &&
                   description->kind == KERNEL_OPEN_FILE_KIND_REGULAR
               ? kernel_vfs_file_node(&description->file) : 0;
}

uint64_t kernel_open_file_size(
    const struct kernel_open_file_description *file)
{
    if (open_file_live(file) && file->kind == KERNEL_OPEN_FILE_KIND_GENERATED)
        return file->generated_ready ? file->generated_length : 0U;
    return open_file_live(file) ? kernel_vfs_file_size(&file->file) : 0U;
}

int kernel_open_file_generate(struct kernel_open_file_description *file)
{
    if (!open_file_live(file) || file->kind != KERNEL_OPEN_FILE_KIND_GENERATED)
        return -KERNEL_EINVAL;
    if (file->generated_ready) return 0;
    int result = kernel_vfs_file_snapshot(&file->file, file->heap,
                                           &file->generated_data,
                                           &file->generated_length);
    if (result) {
        if (file->generated_data) kernel_open_file_reset_generated(file);
        return result;
    }
    file->generated_ready = 1U;
    return 0;
}

void kernel_open_file_reset_generated(struct kernel_open_file_description *file)
{
    if (!file || file->kind != KERNEL_OPEN_FILE_KIND_GENERATED) __builtin_trap();
    if (file->generated_data &&
        kernel_heap_release(file->heap, file->generated_data) !=
            KERNEL_HEAP_STATUS_OK) __builtin_trap();
    file->generated_data = 0;
    file->generated_length = 0U;
    file->generated_ready = 0U;
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

int kernel_open_file_readable(
    const struct kernel_open_file_description *file)
{
    uint32_t access_mode;

    if (!open_file_live(file)) {
        return 0;
    }
    access_mode = file->open_flags & 3U;
    switch (file->kind) {
    case KERNEL_OPEN_FILE_KIND_REGULAR:
    case KERNEL_OPEN_FILE_KIND_GENERATED:
    case KERNEL_OPEN_FILE_KIND_DIRECTORY:
    case KERNEL_OPEN_FILE_KIND_NULL:
    case KERNEL_OPEN_FILE_KIND_ZERO:
        return access_mode == 0U || access_mode == 2U;
    case KERNEL_OPEN_FILE_KIND_PIPE:
        return access_mode == 0U || access_mode == 2U;
    case KERNEL_OPEN_FILE_KIND_SOCKET:
        return 1;
    case KERNEL_OPEN_FILE_KIND_CONSOLE:
        return file->file.private_data == 0 ||
               access_mode == 0U || access_mode == 2U;
    default:
        return 0;
    }
}

int kernel_open_file_writable(
    const struct kernel_open_file_description *file)
{
    if (!open_file_live(file)) return 0;
    return (file->open_flags & 3U) == 1U ||
           (file->open_flags & 3U) == 2U;
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

int kernel_open_file_memory_backed(struct kernel_open_file_description *file)
{ return open_file_live(file) && kernel_vfs_file_memory(&file->file) != 0; }

static enum kernel_page_cache_status memory_page(struct kernel_open_file_description *file,
    uint64_t index, uint64_t *address, size_t *valid, int create, int *created)
{
    struct kernel_memory_object *object = kernel_vfs_file_memory(&file->file);
    uint64_t size = kernel_open_file_size(file);
    if (index > UINT64_MAX / BOAROS_PAGE_SIZE || index * BOAROS_PAGE_SIZE >= size)
        return KERNEL_PAGE_CACHE_STATUS_OUT_OF_RANGE;
    enum kernel_memory_object_status r = create ? kernel_memory_object_get_page(object,index,address,created)
                                              : kernel_memory_object_find_page(object,index,address);
    if (r == KERNEL_MEMORY_OBJECT_OK) {
        uint64_t remaining = size - index * BOAROS_PAGE_SIZE;
        *valid = remaining < BOAROS_PAGE_SIZE ? remaining : BOAROS_PAGE_SIZE;
        return KERNEL_PAGE_CACHE_STATUS_OK;
    }
    return r == KERNEL_MEMORY_OBJECT_NOT_FOUND ? KERNEL_PAGE_CACHE_STATUS_NOT_FOUND :
           r == KERNEL_MEMORY_OBJECT_NO_MEMORY ? KERNEL_PAGE_CACHE_STATUS_NO_MEMORY :
           r == KERNEL_MEMORY_OBJECT_NO_SPACE ? KERNEL_PAGE_CACHE_STATUS_IO : KERNEL_PAGE_CACHE_STATUS_STATE;
}

void kernel_open_file_memory_modified(struct kernel_open_file_description *file)
{
    if (!kernel_open_file_memory_backed(file)) __builtin_trap();
    uintptr_t irq = riscv_interrupt_save();
    kernel_tmpfs_memory_modified(&file->file);
    riscv_interrupt_restore(irq);
}

enum kernel_page_cache_status kernel_open_file_get_page_for_fault(
    struct kernel_open_file_description *file, uint64_t index,
    uint64_t *address, size_t *valid, int *created)
{
    if (!created || !address || !valid || !open_file_live(file) ||
        !file->file.mount || !file->file.mount->private_data)
        return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    *created = 0;
    if (kernel_open_file_memory_backed(file))
        return memory_page(file, index, address, valid, 1, created);
    return kernel_open_file_get_page(file, index, address, valid);
}

void kernel_open_file_discard_new_page(struct kernel_open_file_description *file,
    uint64_t index, uint64_t address)
{
    struct kernel_memory_object *object = kernel_vfs_file_memory(&file->file);
    if (!object) __builtin_trap();
    kernel_memory_object_discard_new_page(object, index, address);
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
    int created;
    if (kernel_open_file_memory_backed(file)) return memory_page(file,page_index,physical_address,valid_bytes,1,&created);
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
    if (kernel_open_file_memory_backed(file)) return memory_page(file,page_index,physical_address,valid_bytes,0,0);
    return kernel_page_cache_lookup(
        kernel_vfs_file_page_cache(&file->file),
        &file->file,
        page_index,
        physical_address,
        valid_bytes);
}

enum kernel_page_cache_status kernel_open_file_alias_attach(
    struct kernel_open_file_description *file, uint64_t page_index,
    uint64_t physical_address, struct kernel_page_cache_alias *alias,
    void *owner, uint64_t virtual_address,
    void (*rearm)(void *owner, uint64_t virtual_address))
{
    if (!open_file_live(file) || file->file.mount == 0 ||
        file->file.mount->private_data == 0)
        return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    return kernel_page_cache_alias_attach(
        kernel_vfs_file_page_cache(&file->file), &file->file, page_index,
        physical_address, alias, owner, virtual_address, rearm);
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

int kernel_open_file_sync_range(struct kernel_open_file_description *file,
    uint64_t start, uint64_t end)
{
    if (!open_file_live(file)) return -KERNEL_EBADF;
    return kernel_vfs_sync_range(&file->file, start, end,
                                 &file->observed_writeback_error);
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
    if (file->device) return file->device->poll(requested_events, out_queue);
    switch (file->kind) {
    case KERNEL_OPEN_FILE_KIND_PIPE:
        return kernel_pipe_poll(file->pipe, file->pipe_endpoint, out_queue);
    case KERNEL_OPEN_FILE_KIND_REGULAR:
    case KERNEL_OPEN_FILE_KIND_GENERATED:
    case KERNEL_OPEN_FILE_KIND_DIRECTORY:
        return KERNEL_POLLIN | KERNEL_POLLOUT | KERNEL_POLLRDNORM | KERNEL_POLLWRNORM;
    case KERNEL_OPEN_FILE_KIND_EPOLL:
        return kernel_epoll_poll(file->epoll, requested_events, out_queue);
    case KERNEL_OPEN_FILE_KIND_SOCKET:
        return kernel_socket_poll(file->socket, out_queue);
    default:
        return KERNEL_POLLNVAL;
    }
}
