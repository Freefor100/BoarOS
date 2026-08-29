#include <kernel/errno.h>
#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/uaccess.h>
#include <kernel/vfs.h>

#include <stddef.h>
#include <stdint.h>

static int empty_context(const struct kernel_fs_context *fs)
{
    return fs->state == KERNEL_FS_CONTEXT_EMPTY && fs->heap == 0 &&
           fs->root_mount == 0 && fs->cwd == 0;
}

static void finish_context(struct kernel_fs_context *fs,
                           enum kernel_fs_context_state state)
{
    fs->heap = 0;
    fs->root_mount = 0;
    fs->cwd = 0;
    fs->state = state;
}

int kernel_fs_context_is_live(const struct kernel_fs_context *fs)
{
    return fs != 0 && fs->state == KERNEL_FS_CONTEXT_LIVE &&
           fs->heap != 0 && fs->root_mount != 0 && fs->cwd != 0;
}

enum kernel_fs_context_status kernel_fs_context_create(
    struct kernel_fs_context *fs,
    struct kernel_vfs_mount *root_mount,
    struct kernel_heap *heap)
{
    char *cwd;
    enum kernel_heap_status heap_status;

    if (fs == 0 || root_mount == 0 || heap == 0) {
        return KERNEL_FS_CONTEXT_STATUS_INVALID_ARGUMENT;
    }
    if (!empty_context(fs)) {
        return KERNEL_FS_CONTEXT_STATUS_STATE;
    }
    heap_status = kernel_heap_allocate(heap, 2U, (void **)&cwd);
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        return heap_status == KERNEL_HEAP_STATUS_EMPTY
                   ? KERNEL_FS_CONTEXT_STATUS_NO_MEMORY
                   : KERNEL_FS_CONTEXT_STATUS_STATE;
    }
    cwd[0] = '/';
    cwd[1] = '\0';
    fs->heap = heap;
    fs->root_mount = root_mount;
    fs->cwd = cwd;
    fs->state = KERNEL_FS_CONTEXT_LIVE;
    return KERNEL_FS_CONTEXT_STATUS_OK;
}

static size_t text_length(const char *text)
{
    size_t length = 0U;

    while (text[length] != '\0') {
        length++;
    }
    return length;
}

enum kernel_fs_context_status kernel_fs_context_resolve_user_path(
    const struct kernel_fs_context *fs,
    const struct kernel_mm *mm,
    int64_t dirfd,
    uint64_t user_path,
    char *buffer,
    size_t capacity,
    struct kernel_vfs_mount **mount,
    int *linux_result)
{
    size_t path_length;
    size_t cwd_length;
    size_t separator;
    size_t index;
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
    if (path_length == 0U) {
        *linux_result = -KERNEL_ENOENT;
        return KERNEL_FS_CONTEXT_STATUS_OK;
    }
    if (buffer[0] == '/') {
        *mount = fs->root_mount;
        *linux_result = 0;
        return KERNEL_FS_CONTEXT_STATUS_OK;
    }
    if (dirfd != KERNEL_FS_AT_FDCWD) {
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FS_CONTEXT_STATUS_OK;
    }

    cwd_length = text_length(fs->cwd);
    separator = cwd_length != 0U && fs->cwd[cwd_length - 1U] == '/'
                    ? 0U
                    : 1U;
    if (cwd_length >= capacity || separator > capacity - cwd_length ||
        path_length + 1U > capacity - cwd_length - separator) {
        *linux_result = -KERNEL_ENAMETOOLONG;
        return KERNEL_FS_CONTEXT_STATUS_OK;
    }
    for (index = path_length + 1U; index != 0U; index--) {
        buffer[cwd_length + separator + index - 1U] = buffer[index - 1U];
    }
    for (index = 0U; index < cwd_length; index++) {
        buffer[index] = fs->cwd[index];
    }
    if (separator != 0U) {
        buffer[cwd_length] = '/';
    }
    *mount = fs->root_mount;
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
        fs->heap == 0 || fs->root_mount == 0 || fs->cwd == 0) {
        return KERNEL_FS_CONTEXT_STATUS_STATE;
    }
    fs->state = KERNEL_FS_CONTEXT_CLEANUP;
    if (kernel_heap_release(fs->heap, fs->cwd) !=
        KERNEL_HEAP_STATUS_OK) {
        return KERNEL_FS_CONTEXT_STATUS_CLEANUP_REQUIRED;
    }
    finish_context(fs, KERNEL_FS_CONTEXT_RELEASED);
    return KERNEL_FS_CONTEXT_STATUS_OK;
}
