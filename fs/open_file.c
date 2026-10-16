#include "open_file_internal.h"
#include "record_lock.h"
#include "pipe_internal.h"
#include "vfs_internal.h"
#include "char_device_internal.h"
#include "files/epoll_internal.h"

#include <arch/context.h>
#include <arch/platform_io.h>
#include <kernel/console.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/open_file.h>
#include <kernel/page_cache.h>
#include <kernel/file_mapping.h>
#include <kernel/memory_object.h>
#include <kernel/tmpfs.h>
#include <kernel/socket.h>
#include <kernel/vfs.h>

#include <stddef.h>
#include <stdint.h>

static int open_file_live(
    const struct kernel_open_file_description *file);

static void reset_read_prediction(struct kernel_open_file_description *file)
{
#if BOAROS_PAGE_CACHE_READAHEAD_PAGES
    uint64_t cookie;
    {
        KERNEL_RAW_SCOPE(prediction,&file->offset_lock.lock.metadata);
        cookie=file->readahead_cookie;
        file->readahead_cookie = 0; file->read_sequential = 0;
    }
    if (cookie) kernel_page_cache_cancel_readahead(kernel_vfs_file_page_cache(&file->file),cookie);
#else
    (void)file;
#endif
}

#if BOAROS_PAGE_CACHE_READAHEAD_PAGES
void kernel_open_file_read_begin(struct kernel_open_file_description *file, uint64_t offset)
{
    if (!open_file_live(file)) return;
    uint64_t cancel=0;
    {
        KERNEL_RAW_SCOPE(prediction,&file->offset_lock.lock.metadata);
        if (file->read_sequential && file->read_end != offset) {
            cancel=file->readahead_cookie;
            file->readahead_cookie=0;file->read_sequential=0;
        }
    }
    if (cancel) kernel_page_cache_cancel_readahead(kernel_vfs_file_page_cache(&file->file),cancel);
}
void kernel_open_file_read_progress(struct kernel_open_file_description *file,
    uint64_t offset, size_t count)
{
    if (!count || !open_file_live(file) || file->kind != KERNEL_OPEN_FILE_KIND_REGULAR ||
        kernel_open_file_memory_backed(file) || kernel_vfs_file_generated(&file->file)) return;
    if (offset > UINT64_MAX - count) __builtin_trap();
    uint64_t cancel=0,cookie=0,end=offset+count;
    int queue;
    {
        KERNEL_RAW_SCOPE(prediction,&file->offset_lock.lock.metadata);
        int sequential=file->read_sequential && file->read_end==offset;
        if (!sequential) { cancel=file->readahead_cookie;file->readahead_cookie=0; }
        file->read_end=end;file->read_sequential=1;
        queue=sequential && !(end&BOAROS_PAGE_MASK);
        cookie=file->readahead_cookie;
    }
    struct kernel_page_cache *cache=kernel_vfs_file_page_cache(&file->file);
    if (cancel) kernel_page_cache_cancel_readahead(cache,cancel);
    if (queue) {
        uint64_t published=kernel_page_cache_readahead(cache,kernel_vfs_file_node(&file->file),
            end>>BOAROS_PAGE_SHIFT,cookie);
        int keep;
        {
            KERNEL_RAW_SCOPE(prediction,&file->offset_lock.lock.metadata);
            keep=file->read_sequential && file->read_end==end && file->readahead_cookie==cookie;
            if (keep) file->readahead_cookie=published;
        }
        /* 排队期间的seek或其他pread已改变预测；不能把旧计划重新挂回OFD。 */
        if (!keep && published) kernel_page_cache_cancel_readahead(cache,published);
    }
}
#endif

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
    file->observed_mount_error = kernel_vfs_mount_error_sequence(file->file.mount);
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
    case KERNEL_OPEN_PATH_ONLY:
    case KERNEL_OPEN_PATH_ONLY_NOFOLLOW:
        result = kernel_vfs_path_resolve(start, root, path,
                    operation == KERNEL_OPEN_PATH_ONLY, &file->file.path);
        if (!result) {
            file->kind = KERNEL_OPEN_FILE_KIND_PATH;
            file->file.mount = kernel_vfs_path_mount(file->file.path);
            file->file.mode = kernel_vfs_path_mode(file->file.path);
        }
        break;
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
        file->observed_mount_error = kernel_vfs_mount_error_sequence(file->file.mount);
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
    file->observed_mount_error = kernel_vfs_mount_error_sequence(file->file.mount);
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
    file->observed_mount_error = kernel_vfs_mount_error_sequence(file->file.mount);
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
    file->open_flags = 2U;
    if (kernel_char_device_open(file, file->open_flags)) {
        (void)kernel_heap_release(heap, file);
        return KERNEL_OPEN_FILE_STATUS_STATE;
    }
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
    uintptr_t irq = arch_interrupt_save();
    if (!next_socket_proc_identity) __builtin_trap();
    file->proc_identity = next_socket_proc_identity++;
    arch_interrupt_restore(irq);
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
        return kernel_pipe_stat(file->pipe, stat);
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
    case KERNEL_OPEN_FILE_KIND_RANDOM:
        return KERNEL_OPEN_FILE_KIND_RANDOM;
    case KERNEL_OPEN_FILE_KIND_URANDOM:
        return KERNEL_OPEN_FILE_KIND_URANDOM;
    case KERNEL_OPEN_FILE_KIND_PATH:
        return KERNEL_OPEN_FILE_KIND_PATH;
    case KERNEL_OPEN_FILE_KIND_RTC:
        return KERNEL_OPEN_FILE_KIND_RTC;
    default:
        return KERNEL_OPEN_FILE_KIND_REGULAR;
    }
}

int kernel_open_file_supports_epoll(
    const struct kernel_open_file_description *file)
{
    if (file && file->kind==KERNEL_OPEN_FILE_KIND_PIPE && file->pipe &&
        __atomic_load_n(&file->pipe->concurrent,__ATOMIC_ACQUIRE)) return 0;
    if (file == 0) {
        return 0;
    }
    switch (file->kind) {
    case KERNEL_OPEN_FILE_KIND_PIPE:
    case KERNEL_OPEN_FILE_KIND_CONSOLE:
    case KERNEL_OPEN_FILE_KIND_EPOLL:
    case KERNEL_OPEN_FILE_KIND_SOCKET:
    case KERNEL_OPEN_FILE_KIND_RANDOM:
        return 1;
    default:
        return 0;
    }
}

enum kernel_open_file_status kernel_open_file_acquire(
    struct kernel_open_file_description *file)
{
    if (file == 0 || file->heap == 0) return KERNEL_OPEN_FILE_STATUS_STATE;
    uint32_t references = __atomic_load_n(&file->references, __ATOMIC_RELAXED);
    for (;;) {
        if (!references || references == UINT32_MAX) return KERNEL_OPEN_FILE_STATUS_STATE;
        /* 已有owner覆盖存活，不能从裸指针复活清理中的OFD。 */
        if (__atomic_compare_exchange_n(&file->references, &references, references + 1,
                1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return KERNEL_OPEN_FILE_STATUS_OK;
    }
}

#if BOAROS_USER_SMP
int kernel_open_file_release_nonfinal(struct kernel_open_file_description **owner)
{
    struct kernel_open_file_description *file = *owner;
    uint32_t references = __atomic_load_n(&file->references, __ATOMIC_ACQUIRE);
    for (;;) {
        if (!references || references == UINT32_MAX) __builtin_trap();
        if (references == 1) return 0;
        if (__atomic_compare_exchange_n(&file->references, &references, references - 1,
            1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            *owner = NULL;
            return 1;
        }
    }
}
#endif

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
    if (file->heap == 0) {
        return KERNEL_OPEN_FILE_STATUS_STATE;
    }
    uint32_t references = __atomic_load_n(&file->references, __ATOMIC_ACQUIRE);
    for (;;) {
        if (references == UINT32_MAX) return KERNEL_OPEN_FILE_STATUS_STATE;
        if (!references) break; /* 原清理owner重试，未取得新引用。 */
        if (__atomic_compare_exchange_n(&file->references, &references, references - 1,
                1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) break;
    }
    if (references > 1U) {
        *owner = 0;
        return KERNEL_OPEN_FILE_STATUS_OK;
    }
    /* 匿名pipe没有后端、命名空间或观察者清理；最后endpoint可在受控CPU归还。
     * 其余对象的真实清理继续由CPU0服务域执行。 */
    if (!(file->kind==KERNEL_OPEN_FILE_KIND_PIPE && file->pipe &&
          !file->file.private_data && !file->pipe->fifo_node &&
          !file->ep_items && !file->device_opened && !file->record_locks &&
          !file->generated_ready)) kernel_assert_service_context();
    if (references == 1U) {
        reset_read_prediction(file);
        release_record_locks(file);
    }
    if (file->device_opened) {
        file->device_opened = 0;
        if (file->device->release) file->device->release(file->device_instance);
        file->device_instance = 0;
    }
    if (file->ep_items != 0) {
        kernel_epoll_notify_file_release(file);
    }
    if (file->generated_ready) kernel_open_file_reset_generated(file);
    if (!file->vfs_closed) {
        if (file->kind == KERNEL_OPEN_FILE_KIND_CONSOLE &&
            file->file.private_data == 0) {
            file->vfs_closed = 1U;
        } else if (file->kind == KERNEL_OPEN_FILE_KIND_PATH) {
            if (kernel_vfs_path_release(&file->file.path)) __builtin_trap();
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
            if (file->file.private_data && kernel_vfs_close(&file->file))
                return KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED;
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
    uint32_t references = __atomic_load_n(&file->references, __ATOMIC_ACQUIRE);
    for (;;) {
        if (!references || references == UINT32_MAX) return KERNEL_OPEN_FILE_STATUS_STATE;
        /* A pipe endpoint's last live owner closes it immediately. */
        if (references == 1 && (file->kind == KERNEL_OPEN_FILE_KIND_PIPE ||
                               file->kind == KERNEL_OPEN_FILE_KIND_SOCKET))
            return kernel_open_file_release(owner);
        if (__atomic_compare_exchange_n(&file->references, &references, references - 1,
                1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) break;
    }
    if (references == 1U) {
        reset_read_prediction(file);
        release_record_locks(file);
        if (file->device_opened) {
            file->device_opened = 0;
            if (file->device->release) file->device->release(file->device_instance);
            file->device_instance = 0;
        }
    }
    if (references > 1U) {
        *owner = 0;
        return KERNEL_OPEN_FILE_STATUS_OK;
    }
    return KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED;
}

static int open_file_live(const struct kernel_open_file_description *file)
{
    return file != 0 && file->heap != 0 && __atomic_load_n(&file->references, __ATOMIC_ACQUIRE) != 0U &&
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
    if (!open_file_live(file)) return 0U;
    if (file->kind == KERNEL_OPEN_FILE_KIND_PIPE && !file->file.private_data) {
        KERNEL_RAW_SCOPE(guard,&file->pipe->metadata_lock);
        return file->pipe->mode;
    }
    if (file->kind == KERNEL_OPEN_FILE_KIND_PATH)
        return kernel_vfs_path_mode(file->file.path);
    return file->file.mode;
}

uint32_t kernel_open_file_flags(
    const struct kernel_open_file_description *file)
{
    /* 创建/终端取得/fd标志不是OFD状态；F_GETFL结果可直接用于F_SETFL。 */
    const uint32_t open_only = 0100U | 0200U | 0400U | 01000U | 02000000U;
    return open_file_live(file) ? __atomic_load_n(&file->open_flags, __ATOMIC_RELAXED) & ~open_only : 0U;
}
void kernel_open_file_set_status_flags(struct kernel_open_file_description *file, uint32_t flags)
{
    if (!open_file_live(file)) __builtin_trap();
    const uint32_t mask=00002000U | 00004000U;
    uint32_t old=__atomic_load_n(&file->open_flags, __ATOMIC_RELAXED);
    while (!__atomic_compare_exchange_n(&file->open_flags, &old,
        (old & ~mask) | (flags & mask), 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

int kernel_open_file_readable(
    const struct kernel_open_file_description *file)
{
    uint32_t access_mode;

    if (!open_file_live(file)) {
        return 0;
    }
    access_mode = kernel_open_file_flags(file) & 3U;
    switch (file->kind) {
    case KERNEL_OPEN_FILE_KIND_REGULAR:
    case KERNEL_OPEN_FILE_KIND_GENERATED:
    case KERNEL_OPEN_FILE_KIND_DIRECTORY:
    case KERNEL_OPEN_FILE_KIND_NULL:
    case KERNEL_OPEN_FILE_KIND_ZERO:
    case KERNEL_OPEN_FILE_KIND_RANDOM:
    case KERNEL_OPEN_FILE_KIND_URANDOM:
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
    if (!open_file_live(file) || file->kind == KERNEL_OPEN_FILE_KIND_PATH) return 0;
    return (kernel_open_file_flags(file) & 3U) == 1U ||
           (kernel_open_file_flags(file) & 3U) == 2U;
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
    reset_read_prediction(file);
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
const struct kernel_vfs_file *kernel_open_file_vfs_borrow(const struct kernel_open_file_description *file)
{ return open_file_live(file) ? &file->file : 0; }
struct kernel_page_cache *kernel_open_file_page_cache(const struct kernel_open_file_description *file)
{ return open_file_live(file) ? kernel_vfs_file_page_cache(&file->file) : 0; }

int kernel_open_file_prepare_memory_page(struct kernel_open_file_description *file,
    uint64_t index, struct kernel_open_file_page_preparation *prepared)
{
    if (!prepared || prepared->pending || !kernel_open_file_memory_backed(file))
        return -KERNEL_EINVAL;
    struct kernel_vfs_node *node = kernel_open_file_node(file);
    KERNEL_LOCK_SCOPE(guard);
    kernel_vfs_node_lock(node, &guard, 0);
    int error = kernel_file_mapping_prepare(node);
    if (error) return error;
    uint64_t epoch;
    if (kernel_file_mapping_epoch(node, &epoch)) return -KERNEL_EBUSY;
    uint64_t size = kernel_open_file_size(file);
    if (index > UINT64_MAX / BOAROS_PAGE_SIZE || index * BOAROS_PAGE_SIZE >= size)
        return -KERNEL_EFAULT;
    uint64_t address;
    int created;
    enum kernel_memory_object_status status = kernel_memory_object_prepare_page(
        kernel_vfs_file_memory(&file->file), index, &address, &created);
    if (status != KERNEL_MEMORY_OBJECT_OK)
        return status == KERNEL_MEMORY_OBJECT_NO_MEMORY ? -KERNEL_ENOMEM :
               status == KERNEL_MEMORY_OBJECT_NO_SPACE ? -KERNEL_ENOSPC : -KERNEL_EIO;
    *prepared = (struct kernel_open_file_page_preparation){
        .address=address, .index=index, .epoch=epoch, .size=size, .pending=1, .created=created};
    return 0;
}

void kernel_open_file_finish_memory_page(struct kernel_open_file_description *file,
    struct kernel_open_file_page_preparation *prepared, int accepted)
{
    if (!prepared || !prepared->pending || !kernel_open_file_memory_backed(file))
        __builtin_trap();
    kernel_memory_object_finish_page(kernel_vfs_file_memory(&file->file),
        prepared->index, prepared->address, accepted);
    prepared->pending = 0;
}

enum kernel_open_file_status kernel_open_file_check_mapping_domain(
    const struct kernel_open_file_description *file,const struct kernel_heap *heap,
    const struct physical_page_allocator *allocator)
{
    if (!open_file_live(file) || file->kind != KERNEL_OPEN_FILE_KIND_REGULAR)
        return KERNEL_OPEN_FILE_STATUS_STATE;
    struct kernel_vfs_node *node=kernel_open_file_node(file);
    int error=kernel_open_file_memory_backed((struct kernel_open_file_description *)file) ?
        kernel_tmpfs_check_domain(node,heap,allocator) : kernel_ext4_check_mapping_domain(node,heap,allocator);
    return error ? KERNEL_OPEN_FILE_STATUS_STATE : KERNEL_OPEN_FILE_STATUS_OK;
}
enum kernel_open_file_status kernel_open_file_check_io_domain(
    const struct kernel_open_file_description *file,const struct kernel_heap *heap,
    const struct physical_page_allocator *allocator)
{
    if (!open_file_live(file) || file->kind!=KERNEL_OPEN_FILE_KIND_REGULAR || file->heap!=heap ||
        file->ep_items || file->record_locks || file->device_opened || file->generated_ready)
        return KERNEL_OPEN_FILE_STATUS_STATE;
    struct kernel_vfs_node *node=kernel_open_file_node(file);
    int error=kernel_open_file_memory_backed((struct kernel_open_file_description *)file) ?
        kernel_tmpfs_check_domain(node,heap,allocator) : kernel_ext4_check_io_domain(node,heap,allocator);
    return error?KERNEL_OPEN_FILE_STATUS_STATE:KERNEL_OPEN_FILE_STATUS_OK;
}
int kernel_open_file_prepare_mapping_page(struct kernel_open_file_description *file,
    uint64_t index,struct kernel_open_file_page_preparation *prepared)
{
    if (kernel_open_file_memory_backed(file)) return kernel_open_file_prepare_memory_page(file,index,prepared);
    if (!prepared || prepared->pending || !open_file_live(file)) return -KERNEL_EINVAL;
    struct kernel_vfs_node *node=kernel_open_file_node(file);
    struct kernel_page_cache *cache=kernel_vfs_file_page_cache(&file->file);
    if (!node || !kernel_page_cache_is_controlled(cache)) return -KERNEL_EINVAL;
    uint64_t epoch,size;
    {
        KERNEL_LOCK_SCOPE(io); kernel_vfs_node_lock(node,&io,0);
        int error=kernel_file_mapping_prepare(node);
        if (error) return error;
        if (kernel_file_mapping_epoch(node,&epoch)) return -KERNEL_EBUSY;
        size=kernel_open_file_size(file);
        if (index>UINT64_MAX/BOAROS_PAGE_SIZE || index*BOAROS_PAGE_SIZE>=size) return -KERNEL_EFAULT;
    }
    /* 不能携带inode读资格等CPU0再取得读资格：排队writer会形成等待环。 */
    uint64_t address; size_t valid;
    enum kernel_page_cache_status status=kernel_page_cache_get(cache,&file->file,index,&address,&valid);
    if (status) return status==KERNEL_PAGE_CACHE_STATUS_NO_MEMORY ? -KERNEL_ENOMEM :
        status==KERNEL_PAGE_CACHE_STATUS_STATE ? -KERNEL_EBUSY : -KERNEL_EIO;
    {
        KERNEL_LOCK_SCOPE(io); kernel_vfs_node_lock(node,&io,0);
        uint64_t latest;
        if (kernel_file_mapping_epoch(node,&latest) || latest!=epoch || index*BOAROS_PAGE_SIZE>=kernel_open_file_size(file)) {
            physical_page_release(cache->allocator,address); return -KERNEL_EBUSY;
        }
    }
    *prepared=(struct kernel_open_file_page_preparation){.address=address,.index=index,.epoch=epoch,.size=size,.pending=1};
    return 0;
}
void kernel_open_file_finish_mapping_page(struct kernel_open_file_description *file,
    struct kernel_open_file_page_preparation *prepared,int accepted)
{
    if (kernel_open_file_memory_backed(file)) { kernel_open_file_finish_memory_page(file,prepared,accepted); return; }
    if (!prepared || !prepared->pending || !open_file_live(file)) __builtin_trap();
    if (!accepted && physical_page_release(kernel_vfs_file_page_cache(&file->file)->allocator,prepared->address)) __builtin_trap();
    prepared->pending=0;
}

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
    uintptr_t irq = arch_interrupt_save();
    kernel_tmpfs_memory_modified(&file->file);
    arch_interrupt_restore(irq);
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
    int (*rearm)(void *owner, uint64_t virtual_address))
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
    if (!open_file_live(file)) return -KERNEL_EINVAL;
    if (bytes_read) *bytes_read = 0;
    if (size) kernel_open_file_read_begin(file, offset);
    int result = kernel_vfs_pread(&file->file, offset, buffer, size, bytes_read);
    if (bytes_read) kernel_open_file_read_progress(file, offset, *bytes_read);
    return result;
}

struct sync_range_call {
    struct kernel_open_file_description *file;
    uint64_t start,end;
    int result;
};
static void sync_range_cpu0(void *argument)
{
    struct sync_range_call *call=argument;
    /* 调用者在排队前取得OFD位置资格，executor只借用，不能等待自己的队首。 */
    call->result=kernel_vfs_sync_range(&call->file->file,call->start,call->end,
        &call->file->observed_writeback_error);
}
int kernel_open_file_sync_range(struct kernel_open_file_description *file,
    uint64_t start, uint64_t end)
{
    if (!open_file_live(file)) return -KERNEL_EBADF;
    if (kernel_open_file_io_requires_cpu0(file)) {
        struct kernel_io_context *io=kernel_io_context_current();
        if (io->locks || io->user_pins || io->cache_writes || kernel_cpu_current()->raw_locks ||
            !kernel_scheduler_can_sleep() || arch_external_interrupt_active()) return -KERNEL_EIO;
        KERNEL_LOCK_SCOPE(offset);
        kernel_mutex_lock(&file->offset_lock,&offset);
        struct sync_range_call call={file,start,end,-KERNEL_EIO};
        if (kernel_page_cache_run_cpu0_borrowing(kernel_open_file_page_cache(file),sync_range_cpu0,&call,&offset))
            return -KERNEL_EIO;
        return call.result;
    }
    KERNEL_LOCK_SCOPE(offset);
    if (!kernel_lock_held(&file->offset_lock.lock,1)) kernel_mutex_lock(&file->offset_lock,&offset);
    return kernel_vfs_sync_range(&file->file, start, end,
                                 &file->observed_writeback_error);
}

uint32_t kernel_open_file_poll(
    struct kernel_open_file_description *file,
    uint32_t requested_events,
    struct kernel_wait_queue **out_queue)
{
    if (file && file->kind == KERNEL_OPEN_FILE_KIND_PATH) {
        if (out_queue) *out_queue = 0;
        return KERNEL_POLLNVAL;
    }

    if (out_queue != 0) {
        *out_queue = 0;
    }
    if (!open_file_live(file)) {
        return KERNEL_POLLNVAL;
    }
    if (file->device) return file->device->poll ? file->device->poll(file->device_instance, kernel_open_file_flags(file), requested_events, out_queue) : 0U;
    switch (file->kind) {
    case KERNEL_OPEN_FILE_KIND_PIPE:
        return kernel_pipe_poll(file->pipe, file->pipe_endpoint, file->pipe_observed_writers, out_queue);
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
