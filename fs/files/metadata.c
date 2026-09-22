#include "../open_file_internal.h"
#include "private.h"

#include <kernel/errno.h>
#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/uaccess.h>
#include <kernel/vfs.h>

#include <string.h>

/* Return owned identity for a path or AT_EMPTY_PATH, preserving the fd/path
 * error before validating operation-specific values. Synthetic fds have no
 * filesystem path; they must never silently use the root mount. */
static int metadata_path(struct kernel_files *files,
                          const struct kernel_fs_context *fs,
                          int64_t dirfd, const char *name, uint32_t flags,
                          struct kernel_vfs_path **path)
{
    struct kernel_vfs_path *start = 0;
    if (!name[0] && (flags & KERNEL_FILES_AT_EMPTY_PATH)) {
        if (dirfd == KERNEL_FS_AT_FDCWD) start = kernel_fs_context_cwd(fs);
        else {
            struct kernel_open_file_description *file = kernel_files_lookup_description(files, dirfd);
            if (!file) return -KERNEL_EBADF;
            start = file->file.path;
            if (!start) return -KERNEL_ENOTSUP;
        }
        int result = kernel_vfs_path_acquire(start);
        if (!result) *path = start;
        return result;
    }
    int result = kernel_files_path_start(files, fs, dirfd, name, &start);
    if (result) return result;
    return kernel_vfs_path_resolve(start, kernel_fs_context_root(fs), name,
                    !(flags & KERNEL_FILES_AT_SYMLINK_NOFOLLOW), path);
}

static enum kernel_files_status copy_metadata_path(
    struct kernel_files *files, const struct kernel_fs_context *fs,
    struct kernel_mm *mm, int64_t dirfd, uint64_t user_path, uint32_t flags,
    struct kernel_vfs_path **path, int *result)
{
    char *name;
    size_t length;
    enum kernel_heap_status allocation = kernel_heap_allocate(files->heap,
                                          KERNEL_FS_PATH_MAX, (void **)&name);
    if (allocation != KERNEL_HEAP_STATUS_OK) {
        if (allocation != KERNEL_HEAP_STATUS_EMPTY) return KERNEL_FILES_STATUS_STATE;
        *result = -KERNEL_ENOMEM;
        return KERNEL_FILES_STATUS_OK;
    }
    enum kernel_uaccess_status access = kernel_copy_string_from_user(mm, name,
                                      user_path, KERNEL_FS_PATH_MAX, &length);
    if (access == KERNEL_UACCESS_STATUS_FAULT) *result = -KERNEL_EFAULT;
    else if (access == KERNEL_UACCESS_STATUS_TOO_LONG) *result = -KERNEL_ENAMETOOLONG;
    else if (access == KERNEL_UACCESS_STATUS_OK)
        *result = metadata_path(files, fs, dirfd, name, flags, path);
    if (kernel_files_release_allocation(files, name) != KERNEL_FILES_STATUS_OK)
        return KERNEL_FILES_STATUS_STATE;
    return access == KERNEL_UACCESS_STATUS_OK || access == KERNEL_UACCESS_STATUS_FAULT ||
           access == KERNEL_UACCESS_STATUS_TOO_LONG ? KERNEL_FILES_STATUS_OK : KERNEL_FILES_STATUS_STATE;
}

enum kernel_files_status kernel_files_utimensat(
    struct kernel_files *files, const struct kernel_fs_context *fs,
    struct kernel_mm *mm, int64_t dirfd, uint64_t user_path,
    uint64_t user_times, uint32_t flags, int64_t *linux_result)
{
    struct kernel_vfs_timespec times[2];
    struct kernel_vfs_path *path = 0;
    int result;
    if (!kernel_files_is_live(files) || !kernel_fs_context_is_live(fs) ||
        !mm || !linux_result) return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    if (user_times) {
        size_t copied = 0;
        enum kernel_uaccess_status access = kernel_copy_from_user(mm, times,
                                        user_times, sizeof(times), &copied);
        if (access == KERNEL_UACCESS_STATUS_FAULT) {
            *linux_result = -KERNEL_EFAULT;
            return KERNEL_FILES_STATUS_OK;
        }
        if (access != KERNEL_UACCESS_STATUS_OK || copied != sizeof(times))
            return KERNEL_FILES_STATUS_STATE;
        if (times[0].nanoseconds == KERNEL_VFS_UTIME_OMIT &&
            times[1].nanoseconds == KERNEL_VFS_UTIME_OMIT) {
            *linux_result = 0;
            return KERNEL_FILES_STATUS_OK;
        }
    }
    if (!user_path && dirfd != KERNEL_FS_AT_FDCWD) {
        struct kernel_open_file_description *file;
        if (flags) result = -KERNEL_EINVAL;
        else if (!(file = kernel_files_lookup_description(files, dirfd))) result = -KERNEL_EBADF;
        else if (!file->file.private_data) result = -KERNEL_ENOTSUP;
        else result = kernel_vfs_file_set_times(&file->file, user_times ? times : 0);
    } else {
        if (flags & ~(KERNEL_FILES_AT_SYMLINK_NOFOLLOW | KERNEL_FILES_AT_EMPTY_PATH)) {
            *linux_result = -KERNEL_EINVAL;
            return KERNEL_FILES_STATUS_OK;
        }
        enum kernel_files_status status = copy_metadata_path(files, fs, mm,
                                       dirfd, user_path, flags, &path, &result);
        if (status != KERNEL_FILES_STATUS_OK) return status;
        if (!result) result = kernel_vfs_path_set_times(path, user_times ? times : 0);
        if (path) (void)kernel_vfs_path_release(&path);
    }
    *linux_result = result;
    return KERNEL_FILES_STATUS_OK;
}

static enum kernel_files_status copy_statfs(struct kernel_mm *mm,
    struct kernel_vfs_mount *mount, uint64_t user_buffer, int64_t *linux_result)
{
    struct kernel_vfs_statfs native;
    struct kernel_linux_statfs stat;
    size_t copied = 0;
    if (!mount) { *linux_result = -KERNEL_ENOTSUP; return KERNEL_FILES_STATUS_OK; }
    int result = kernel_vfs_mount_statfs(mount, &native);
    if (result) { *linux_result = result; return KERNEL_FILES_STATUS_OK; }
    memset(&stat, 0, sizeof(stat));
    stat.f_type = (int64_t)native.type;
    stat.f_bsize = stat.f_frsize = (int64_t)native.block_size;
    stat.f_blocks = native.blocks;
    stat.f_bfree = native.free_blocks;
    stat.f_bavail = native.available_blocks;
    stat.f_files = native.inodes;
    stat.f_ffree = native.free_inodes;
    stat.f_fsid[0] = (int32_t)(uint32_t)native.fsid;
    stat.f_fsid[1] = (int32_t)(uint32_t)(native.fsid >> 32U);
    stat.f_namelen = (int64_t)native.name_length;
    stat.f_flags = (int64_t)native.flags;
    enum kernel_uaccess_status access = kernel_copy_to_user(mm, user_buffer,
                                                    &stat, sizeof(stat), &copied);
    if (access == KERNEL_UACCESS_STATUS_FAULT) *linux_result = -KERNEL_EFAULT;
    else if (access != KERNEL_UACCESS_STATUS_OK || copied != sizeof(stat))
        return KERNEL_FILES_STATUS_STATE;
    else *linux_result = 0;
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_statfs(
    struct kernel_files *files, const struct kernel_fs_context *fs,
    struct kernel_mm *mm, uint64_t user_path, uint64_t user_buffer,
    int64_t *linux_result)
{
    struct kernel_vfs_path *path = 0;
    int result;
    if (!kernel_files_is_live(files) || !kernel_fs_context_is_live(fs) ||
        !mm || !linux_result) return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    enum kernel_files_status status = copy_metadata_path(files, fs, mm,
                          KERNEL_FS_AT_FDCWD, user_path, 0, &path, &result);
    if (status != KERNEL_FILES_STATUS_OK) return status;
    if (result) { *linux_result = result; return KERNEL_FILES_STATUS_OK; }
    status = copy_statfs(mm, kernel_vfs_path_mount(path), user_buffer, linux_result);
    (void)kernel_vfs_path_release(&path);
    return status;
}

enum kernel_files_status kernel_files_fstatfs(
    struct kernel_files *files, struct kernel_mm *mm, int64_t fd,
    uint64_t user_buffer, int64_t *linux_result)
{
    if (!kernel_files_is_live(files) || !mm || !linux_result)
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    struct kernel_open_file_description *file = kernel_files_lookup_description(files, fd);
    if (!file) { *linux_result = -KERNEL_EBADF; return KERNEL_FILES_STATUS_OK; }
    return copy_statfs(mm, file->file.mount, user_buffer, linux_result);
}
