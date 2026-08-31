#include <kernel/errno.h>
#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/uaccess.h>
#include <kernel/vfs.h>

#include <stddef.h>
#include <stdint.h>

struct kernel_fs_context_record {
    struct kernel_vfs_mount *root_mount;
    char *cwd;
    uint32_t references;
};

static int empty_context(const struct kernel_fs_context *fs)
{
    return fs->state == KERNEL_FS_CONTEXT_EMPTY && fs->heap == 0 &&
           fs->record == 0;
}

static void finish_context(struct kernel_fs_context *fs,
                           enum kernel_fs_context_state state)
{
    fs->heap = 0;
    fs->record = 0;
    fs->state = state;
}

int kernel_fs_context_is_live(const struct kernel_fs_context *fs)
{
    return fs != 0 && fs->state == KERNEL_FS_CONTEXT_LIVE &&
           fs->heap != 0 && fs->record != 0 &&
           fs->record->references == 1U &&
           fs->record->root_mount != 0 && fs->record->cwd != 0;
}

static enum kernel_fs_context_status create_context(
    struct kernel_fs_context *fs,
    struct kernel_vfs_mount *root_mount,
    struct kernel_heap *heap,
    const char *cwd,
    size_t cwd_size)
{
    struct kernel_fs_context_record *record;
    char *copy;
    size_t index;
    enum kernel_heap_status heap_status;

    heap_status = kernel_heap_allocate_zeroed(heap,
                                              1U,
                                              sizeof(*record),
                                              (void **)&record);
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        return heap_status == KERNEL_HEAP_STATUS_EMPTY
                   ? KERNEL_FS_CONTEXT_STATUS_NO_MEMORY
                   : KERNEL_FS_CONTEXT_STATUS_STATE;
    }
    record->root_mount = root_mount;
    record->references = 1U;
    heap_status = kernel_heap_allocate(heap, cwd_size, (void **)&copy);
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        if (kernel_heap_release(heap, record) != KERNEL_HEAP_STATUS_OK) {
            fs->heap = heap;
            fs->record = record;
            fs->state = KERNEL_FS_CONTEXT_CLEANUP;
            return KERNEL_FS_CONTEXT_STATUS_CLEANUP_REQUIRED;
        }
        return heap_status == KERNEL_HEAP_STATUS_EMPTY
                   ? KERNEL_FS_CONTEXT_STATUS_NO_MEMORY
                   : KERNEL_FS_CONTEXT_STATUS_STATE;
    }
    for (index = 0U; index < cwd_size; index++) {
        copy[index] = cwd[index];
    }
    record->cwd = copy;
    fs->heap = heap;
    fs->record = record;
    fs->state = KERNEL_FS_CONTEXT_LIVE;
    return KERNEL_FS_CONTEXT_STATUS_OK;
}

enum kernel_fs_context_status kernel_fs_context_create(
    struct kernel_fs_context *fs,
    struct kernel_vfs_mount *root_mount,
    struct kernel_heap *heap)
{
    static const char root_path[] = "/";

    if (fs == 0 || root_mount == 0 || heap == 0) {
        return KERNEL_FS_CONTEXT_STATUS_INVALID_ARGUMENT;
    }
    if (!empty_context(fs)) {
        return KERNEL_FS_CONTEXT_STATUS_STATE;
    }
    return create_context(fs,
                          root_mount,
                          heap,
                          root_path,
                          sizeof(root_path));
}

static size_t text_length(const char *text)
{
    size_t length = 0U;

    while (text[length] != '\0') {
        length++;
    }
    return length;
}

enum kernel_fs_context_status kernel_fs_context_fork(
    struct kernel_fs_context *destination,
    const struct kernel_fs_context *source)
{
    size_t cwd_size;

    if (destination == 0 || source == 0 || destination == source) {
        return KERNEL_FS_CONTEXT_STATUS_INVALID_ARGUMENT;
    }
    if (!empty_context(destination) ||
        !kernel_fs_context_is_live(source)) {
        return KERNEL_FS_CONTEXT_STATUS_STATE;
    }
    cwd_size = text_length(source->record->cwd) + 1U;
    return create_context(destination,
                          source->record->root_mount,
                          source->heap,
                          source->record->cwd,
                          cwd_size);
}

enum kernel_fs_context_status kernel_fs_context_move(
    struct kernel_fs_context *destination,
    struct kernel_fs_context *source)
{
    if (destination == 0 || source == 0 || destination == source) {
        return KERNEL_FS_CONTEXT_STATUS_INVALID_ARGUMENT;
    }
    if (!empty_context(destination) ||
        (source->state != KERNEL_FS_CONTEXT_LIVE &&
         source->state != KERNEL_FS_CONTEXT_CLEANUP) ||
        source->heap == 0 || source->record == 0) {
        return KERNEL_FS_CONTEXT_STATUS_STATE;
    }
    *destination = *source;
    finish_context(source, KERNEL_FS_CONTEXT_MOVED);
    return KERNEL_FS_CONTEXT_STATUS_OK;
}

enum kernel_fs_context_status kernel_fs_context_resolve_user_path(
    const struct kernel_fs_context *fs,
    struct kernel_mm *mm,
    int64_t dirfd,
    uint64_t user_path,
    char *buffer,
    size_t capacity,
    struct kernel_vfs_mount **mount,
    int *linux_result)
{
    size_t path_length;
    enum kernel_uaccess_status access_status;

    if (!kernel_fs_context_is_live(fs) || mm == 0 || buffer == 0 ||
        capacity != KERNEL_FS_PATH_MAX || mount == 0 ||
        linux_result == 0) {
        return KERNEL_FS_CONTEXT_STATUS_INVALID_ARGUMENT;
    }
    access_status = kernel_copy_string_from_user(mm,
                                                 buffer,
                                                 user_path,
                                                 capacity,
                                                 &path_length);
    if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
        *linux_result = -KERNEL_EFAULT;
        return KERNEL_FS_CONTEXT_STATUS_OK;
    }
    if (access_status == KERNEL_UACCESS_STATUS_TOO_LONG) {
        *linux_result = -KERNEL_ENAMETOOLONG;
        return KERNEL_FS_CONTEXT_STATUS_OK;
    }
    if (access_status != KERNEL_UACCESS_STATUS_OK) {
        return KERNEL_FS_CONTEXT_STATUS_STATE;
    }
    return kernel_fs_context_resolve_kernel_path(fs,
                                                  dirfd,
                                                  buffer,
                                                  path_length,
                                                  buffer,
                                                  capacity,
                                                  mount,
                                                  linux_result);
}

enum kernel_fs_context_status kernel_fs_context_resolve_kernel_path(
    const struct kernel_fs_context *fs,
    int64_t dirfd,
    const char *path,
    size_t path_length,
    char *buffer,
    size_t capacity,
    struct kernel_vfs_mount **mount,
    int *linux_result)
{
    size_t cwd_length;
    size_t separator;
    size_t index;

    if (!kernel_fs_context_is_live(fs) || path == 0 || buffer == 0 ||
        capacity != KERNEL_FS_PATH_MAX || mount == 0 ||
        linux_result == 0) {
        return KERNEL_FS_CONTEXT_STATUS_INVALID_ARGUMENT;
    }
    if (path_length == 0U) {
        *linux_result = -KERNEL_ENOENT;
        return KERNEL_FS_CONTEXT_STATUS_OK;
    }
    if (path[0] == '/') {
        if (path_length + 1U > capacity) {
            *linux_result = -KERNEL_ENAMETOOLONG;
            return KERNEL_FS_CONTEXT_STATUS_OK;
        }
        if (path != buffer) {
            for (index = 0U; index <= path_length; index++) {
                buffer[index] = path[index];
            }
        }
        *mount = fs->record->root_mount;
        *linux_result = 0;
        return KERNEL_FS_CONTEXT_STATUS_OK;
    }
    if (dirfd != KERNEL_FS_AT_FDCWD) {
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FS_CONTEXT_STATUS_OK;
    }

    cwd_length = text_length(fs->record->cwd);
    separator = cwd_length != 0U &&
                        fs->record->cwd[cwd_length - 1U] == '/'
                    ? 0U
                    : 1U;
    if (cwd_length >= capacity || separator > capacity - cwd_length ||
        path_length + 1U > capacity - cwd_length - separator) {
        *linux_result = -KERNEL_ENAMETOOLONG;
        return KERNEL_FS_CONTEXT_STATUS_OK;
    }
    for (index = path_length + 1U; index != 0U; index--) {
        buffer[cwd_length + separator + index - 1U] = path[index - 1U];
    }
    for (index = 0U; index < cwd_length; index++) {
        buffer[index] = fs->record->cwd[index];
    }
    if (separator != 0U) {
        buffer[cwd_length] = '/';
    }
    *mount = fs->record->root_mount;
    *linux_result = 0;
    return KERNEL_FS_CONTEXT_STATUS_OK;
}

enum kernel_fs_context_status kernel_fs_context_release(
    struct kernel_fs_context *fs)
{
    if (fs == 0) {
        return KERNEL_FS_CONTEXT_STATUS_INVALID_ARGUMENT;
    }
    if ((fs->state != KERNEL_FS_CONTEXT_LIVE &&
         fs->state != KERNEL_FS_CONTEXT_CLEANUP) ||
        fs->heap == 0 || fs->record == 0 ||
        fs->record->references != 1U ||
        fs->record->root_mount == 0) {
        return KERNEL_FS_CONTEXT_STATUS_STATE;
    }
    fs->state = KERNEL_FS_CONTEXT_CLEANUP;
    if (fs->record->cwd != 0) {
        if (kernel_heap_release(fs->heap, fs->record->cwd) !=
            KERNEL_HEAP_STATUS_OK) {
            return KERNEL_FS_CONTEXT_STATUS_CLEANUP_REQUIRED;
        }
        fs->record->cwd = 0;
    }
    if (kernel_heap_release(fs->heap, fs->record) !=
        KERNEL_HEAP_STATUS_OK) {
        return KERNEL_FS_CONTEXT_STATUS_CLEANUP_REQUIRED;
    }
    finish_context(fs, KERNEL_FS_CONTEXT_RELEASED);
    return KERNEL_FS_CONTEXT_STATUS_OK;
}
