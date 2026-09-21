#include "../open_file_internal.h"
#include "private.h"

#include <kernel/errno.h>
#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/open_file.h>
#include <kernel/page.h>
#include <kernel/uaccess.h>
#include <kernel/vfs.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define LINUX_O_ACCMODE UINT64_C(00000003)
#define LINUX_O_WRONLY UINT64_C(00000001)
#define LINUX_O_RDWR UINT64_C(00000002)
#define LINUX_O_CREAT UINT64_C(00000100)
#define LINUX_O_EXCL UINT64_C(00000200)
#define LINUX_O_TRUNC UINT64_C(00001000)
#define LINUX_O_APPEND UINT64_C(00002000)
#define LINUX_O_NONBLOCK UINT64_C(00004000)
#define LINUX_O_DSYNC UINT64_C(00010000)
#define LINUX_O_DIRECT UINT64_C(00040000)
#define LINUX_O_LARGEFILE UINT64_C(00100000)
#define LINUX_O_DIRECTORY UINT64_C(00200000)
#define LINUX_O_NOFOLLOW UINT64_C(00400000)
#define LINUX_O_NOATIME UINT64_C(01000000)
#define LINUX_O_CLOEXEC UINT64_C(02000000)
#define LINUX_O_SYNC UINT64_C(04010000)
#define LINUX_O_PATH UINT64_C(010000000)
#define LINUX_O_TMPFILE UINT64_C(020200000)

static int validate_open_flags(uint64_t flags, uint32_t *fd_flags)
{
    const uint64_t write_flags = LINUX_O_CREAT | LINUX_O_TRUNC |
                                 LINUX_O_APPEND | LINUX_O_EXCL;
    const uint64_t unsupported_flags =
        LINUX_O_DSYNC | LINUX_O_DIRECT |
        LINUX_O_NOATIME | LINUX_O_SYNC | LINUX_O_PATH |
        (LINUX_O_TMPFILE & ~LINUX_O_DIRECTORY);
    const uint64_t known_flags = LINUX_O_ACCMODE | write_flags |
        unsupported_flags | LINUX_O_NONBLOCK | LINUX_O_DIRECTORY |
        LINUX_O_NOFOLLOW |
        LINUX_O_LARGEFILE | LINUX_O_CLOEXEC;
    uint64_t access_mode = flags & LINUX_O_ACCMODE;

    if (access_mode == 3U || (flags & ~known_flags) != 0U) {
        return -KERNEL_EINVAL;
    }
    if ((flags & LINUX_O_DIRECTORY) != 0U && (flags & LINUX_O_CREAT) != 0U) {
        return -KERNEL_EINVAL;
    }
    if ((flags & unsupported_flags) != 0U) {
        return -KERNEL_ENOTSUP;
    }
    *fd_flags = (flags & LINUX_O_CLOEXEC) != 0U
                    ? KERNEL_FILES_FD_CLOEXEC
                    : 0U;
    return 0;
}

static enum kernel_files_status finish_path(struct kernel_files *files,
                                             char *path)
{
    return kernel_files_release_allocation(files, path) ==
                   KERNEL_FILES_STATUS_OK
               ? KERNEL_FILES_STATUS_OK
               : KERNEL_FILES_STATUS_STATE;
}

enum kernel_files_status kernel_files_openat(
    struct kernel_files *files,
    const struct kernel_fs_context *fs,
    struct kernel_mm *mm,
    int64_t dirfd,
    uint64_t user_path,
    uint64_t flags,
    uint64_t mode,
    int64_t *linux_result)
{
    struct kernel_open_file_description *description = 0;
    struct kernel_vfs_mount *mount;
    char *path;
    uint32_t fd;
    uint32_t fd_flags = 0U;
    int result;
    enum kernel_heap_status heap_status;
    enum kernel_open_file_status open_status;
    enum kernel_fs_context_status fs_status;
    enum kernel_files_status files_status;

    int created = 0;

    if (!kernel_files_is_live(files) || !kernel_fs_context_is_live(fs) ||
        mm == 0 || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    files->record->statistics.open_calls++;
    result = validate_open_flags(flags, &fd_flags);
    if (result != 0) {
        files->record->statistics.open_failures++;
        *linux_result = result;
        return KERNEL_FILES_STATUS_OK;
    }
    files_status = kernel_files_find_free_fd(files, &fd, &result);
    if (files_status != KERNEL_FILES_STATUS_OK) {
        return files_status;
    }
    if (result != 0) {
        files->record->statistics.open_failures++;
        *linux_result = result;
        return KERNEL_FILES_STATUS_OK;
    }
    heap_status = kernel_heap_allocate(files->heap,
                                       KERNEL_FS_PATH_MAX,
                                       (void **)&path);
    if (heap_status == KERNEL_HEAP_STATUS_EMPTY) {
        files->record->statistics.open_failures++;
        *linux_result = -KERNEL_ENOMEM;
        return KERNEL_FILES_STATUS_OK;
    }
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        return KERNEL_FILES_STATUS_STATE;
    }
    fs_status = kernel_fs_context_resolve_user_path(fs,
                                                    mm,
                                                    dirfd,
                                                    user_path,
                                                    path,
                                                    KERNEL_FS_PATH_MAX,
                                                    &mount,
                                                    &result);
    if (fs_status != KERNEL_FS_CONTEXT_STATUS_OK) {
        (void)finish_path(files, path);
        return KERNEL_FILES_STATUS_STATE;
    }
    if (result != 0) {
        files->record->statistics.open_failures++;
        if (finish_path(files, path) != KERNEL_FILES_STATUS_OK) {
            return KERNEL_FILES_STATUS_STATE;
        }
        *linux_result = result;
        return KERNEL_FILES_STATUS_OK;
    }
    if ((flags & LINUX_O_NOFOLLOW) != 0U ||
        (flags & (LINUX_O_CREAT | LINUX_O_EXCL)) ==
            (LINUX_O_CREAT | LINUX_O_EXCL)) {
        open_status = kernel_open_file_create_nofollow(files->heap,
                                                      mount,
                                                      path,
                                                      &description,
                                                      &result);
    } else {
        open_status = kernel_open_file_create(files->heap,
                                              mount,
                                              path,
                                              &description,
                                              &result);
    }
    if (result == -KERNEL_ELOOP &&
        (flags & (LINUX_O_CREAT | LINUX_O_EXCL)) ==
            (LINUX_O_CREAT | LINUX_O_EXCL)) {
        result = -KERNEL_EEXIST;
    }
    if (open_status == KERNEL_OPEN_FILE_STATUS_OK &&
        result == -KERNEL_ENOENT &&
        (flags & LINUX_O_CREAT) != 0U) {
        if (kernel_vfs_mount_is_readonly(mount)) {
            files->record->statistics.open_failures++;
            if (finish_path(files, path) != KERNEL_FILES_STATUS_OK) {
                return KERNEL_FILES_STATUS_STATE;
            }
            *linux_result = -KERNEL_EROFS;
            return KERNEL_FILES_STATUS_OK;
        }
        created = 1;
        open_status = kernel_open_file_create_mode(files->heap,
                                                   mount,
                                                   path,
                                                   (uint32_t)mode,
                                                   &description,
                                                   &result);
    }
    if (finish_path(files, path) != KERNEL_FILES_STATUS_OK) {
        if (description != 0) {
            kernel_files_queue_description(files, description);
            (void)kernel_files_drain_file_cleanup(files);
        }
        return KERNEL_FILES_STATUS_STATE;
    }
    if (open_status == KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED) {
        if (description != 0) {
            kernel_files_queue_description(files, description);
        }
        return KERNEL_FILES_STATUS_STATE;
    }
    if (open_status != KERNEL_OPEN_FILE_STATUS_OK) {
        return open_status == KERNEL_OPEN_FILE_STATUS_NO_MEMORY
                   ? KERNEL_FILES_STATUS_NO_MEMORY
                   : KERNEL_FILES_STATUS_STATE;
    }
    if (result != 0) {
        files->record->statistics.open_failures++;
        *linux_result = result;
        return KERNEL_FILES_STATUS_OK;
    }
    if (!created && (flags & LINUX_O_CREAT) != 0U &&
        (flags & LINUX_O_EXCL) != 0U) {
        files->record->statistics.open_failures++;
        kernel_files_queue_description(files, description);
        (void)kernel_files_drain_file_cleanup(files);
        *linux_result = -KERNEL_EEXIST;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_vfs_mount_is_readonly(mount) &&
        (kernel_open_file_mode(description) & KERNEL_VFS_S_IFMT) ==
            KERNEL_VFS_S_IFREG) {
        uint64_t access_mode = flags & LINUX_O_ACCMODE;
        if (access_mode == LINUX_O_WRONLY || access_mode == LINUX_O_RDWR ||
            (flags & LINUX_O_TRUNC) != 0U) {
            files->record->statistics.open_failures++;
            kernel_files_queue_description(files, description);
            (void)kernel_files_drain_file_cleanup(files);
            *linux_result = -KERNEL_EROFS;
            return KERNEL_FILES_STATUS_OK;
        }
    }
    if ((kernel_open_file_mode(description) & KERNEL_VFS_S_IFMT) ==
        KERNEL_VFS_S_IFDIR) {
        uint64_t access_mode = flags & LINUX_O_ACCMODE;

        if (access_mode == LINUX_O_WRONLY || access_mode == LINUX_O_RDWR ||
            (flags & LINUX_O_TRUNC) != 0U) {
            files->record->statistics.open_failures++;
            kernel_files_queue_description(files, description);
            (void)kernel_files_drain_file_cleanup(files);
            *linux_result = -KERNEL_EISDIR;
            return KERNEL_FILES_STATUS_OK;
        }
        description->kind = KERNEL_OPEN_FILE_KIND_DIRECTORY;
    } else if ((kernel_open_file_mode(description) & KERNEL_VFS_S_IFMT) ==
               KERNEL_VFS_S_IFREG) {
        uint64_t access_mode = flags & LINUX_O_ACCMODE;

        if ((flags & LINUX_O_DIRECTORY) != 0U) {
            files->record->statistics.open_failures++;
            kernel_files_queue_description(files, description);
            (void)kernel_files_drain_file_cleanup(files);
            *linux_result = -KERNEL_ENOTDIR;
            return KERNEL_FILES_STATUS_OK;
        }
        if (access_mode == LINUX_O_WRONLY || access_mode == LINUX_O_RDWR ||
            (flags & LINUX_O_TRUNC) != 0U) {
            int lease_result = kernel_vfs_file_acquire_write(&description->file);
            if (lease_result != 0) {
                files->record->statistics.open_failures++;
                kernel_files_queue_description(files, description);
                (void)kernel_files_drain_file_cleanup(files);
                *linux_result = lease_result;
                return KERNEL_FILES_STATUS_OK;
            }
        }
        if (!created && (flags & LINUX_O_TRUNC) != 0U) {
            int trunc_result = kernel_vfs_ftruncate(&description->file, 0U);

            if (trunc_result != 0) {
                files->record->statistics.open_failures++;
                kernel_files_queue_description(files, description);
                (void)kernel_files_drain_file_cleanup(files);
                *linux_result = trunc_result;
                return KERNEL_FILES_STATUS_OK;
            }
        }
        description->kind = KERNEL_OPEN_FILE_KIND_REGULAR;
    } else if ((kernel_open_file_mode(description) & KERNEL_VFS_S_IFMT) ==
               KERNEL_VFS_S_IFCHR) {
        struct kernel_vfs_stat stat;
        int stat_result = kernel_vfs_fstat(&description->file, &stat);

        if (stat_result == 0 && (flags & LINUX_O_DIRECTORY) != 0U)
            stat_result = -KERNEL_ENOTDIR;
        if (stat_result == 0) {
            switch (stat.rdev) {
            case UINT64_C(0x103):
                description->kind = KERNEL_OPEN_FILE_KIND_NULL;
                break;
            case UINT64_C(0x105):
                description->kind = KERNEL_OPEN_FILE_KIND_ZERO;
                break;
            case UINT64_C(0x501):
                description->kind = KERNEL_OPEN_FILE_KIND_CONSOLE;
                break;
            default:
                stat_result = -KERNEL_ENXIO;
                break;
            }
        }
        if (stat_result != 0) {
            files->record->statistics.open_failures++;
            kernel_files_queue_description(files, description);
            (void)kernel_files_drain_file_cleanup(files);
            *linux_result = stat_result;
            return KERNEL_FILES_STATUS_OK;
        }
    } else {
        files->record->statistics.open_failures++;
        kernel_files_queue_description(files, description);
        (void)kernel_files_drain_file_cleanup(files);
        *linux_result = -KERNEL_ENOTSUP;
        return KERNEL_FILES_STATUS_OK;
    }

    description->open_flags = (uint32_t)flags;
    files_status = kernel_files_install_new_owned_at(files,
                                                     fd,
                                                     fd_flags,
                                                     &description);
    if (files_status != KERNEL_FILES_STATUS_OK) {
        kernel_files_queue_description(files, description);
        (void)kernel_files_drain_file_cleanup(files);
        return files_status;
    }
    *linux_result = fd;
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_symlinkat(
    struct kernel_files *files,
    const struct kernel_fs_context *fs,
    struct kernel_mm *mm,
    uint64_t user_target,
    int64_t dirfd,
    uint64_t user_linkpath,
    int64_t *linux_result)
{
    struct kernel_vfs_mount *mount;
    char *storage;
    char *linkpath;
    size_t target_length;
    int result;
    enum kernel_heap_status heap_status;
    enum kernel_uaccess_status access_status;
    enum kernel_fs_context_status fs_status;

    if (!kernel_files_is_live(files) || !kernel_fs_context_is_live(fs) ||
        mm == 0 || linux_result == 0) return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    heap_status = kernel_heap_allocate(files->heap, 2U * KERNEL_FS_PATH_MAX,
                                       (void **)&storage);
    if (heap_status == KERNEL_HEAP_STATUS_EMPTY) {
        *linux_result = -KERNEL_ENOMEM;
        return KERNEL_FILES_STATUS_OK;
    }
    if (heap_status != KERNEL_HEAP_STATUS_OK) return KERNEL_FILES_STATUS_STATE;
    linkpath = storage + KERNEL_FS_PATH_MAX;
    access_status = kernel_copy_string_from_user(mm, storage, user_target,
                                                 KERNEL_FS_PATH_MAX,
                                                 &target_length);
    if (access_status != KERNEL_UACCESS_STATUS_OK) {
        result = access_status == KERNEL_UACCESS_STATUS_FAULT ? -KERNEL_EFAULT :
                 access_status == KERNEL_UACCESS_STATUS_TOO_LONG ?
                     -KERNEL_ENAMETOOLONG : 0;
        if (finish_path(files, storage) != KERNEL_FILES_STATUS_OK ||
            result == 0) return KERNEL_FILES_STATUS_STATE;
        *linux_result = result;
        return KERNEL_FILES_STATUS_OK;
    }
    fs_status = kernel_fs_context_resolve_user_path(fs, mm, dirfd,
                                                    user_linkpath, linkpath,
                                                    KERNEL_FS_PATH_MAX,
                                                    &mount, &result);
    if (fs_status != KERNEL_FS_CONTEXT_STATUS_OK) {
        (void)finish_path(files, storage);
        return KERNEL_FILES_STATUS_STATE;
    }
    if (result == 0) {
        result = target_length == 0U ? -KERNEL_ENOENT :
            kernel_vfs_symlink(mount, storage, linkpath);
    }
    if (finish_path(files, storage) != KERNEL_FILES_STATUS_OK)
        return KERNEL_FILES_STATUS_STATE;
    *linux_result = result;
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_readlinkat(
    struct kernel_files *files,
    const struct kernel_fs_context *fs,
    struct kernel_mm *mm,
    int64_t dirfd,
    uint64_t user_path,
    uint64_t user_buffer,
    uint64_t size,
    int64_t *linux_result)
{
    struct kernel_vfs_mount *mount;
    char *storage;
    char *buffer;
    size_t bytes_read = 0U;
    size_t copied = 0U;
    int result;
    enum kernel_heap_status heap_status;
    enum kernel_fs_context_status fs_status;
    enum kernel_uaccess_status access_status;

    if (!kernel_files_is_live(files) || !kernel_fs_context_is_live(fs) ||
        mm == 0 || linux_result == 0) return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    if (size == 0U) {
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    heap_status = kernel_heap_allocate(files->heap, 2U * KERNEL_FS_PATH_MAX,
                                       (void **)&storage);
    if (heap_status == KERNEL_HEAP_STATUS_EMPTY) {
        *linux_result = -KERNEL_ENOMEM;
        return KERNEL_FILES_STATUS_OK;
    }
    if (heap_status != KERNEL_HEAP_STATUS_OK) return KERNEL_FILES_STATUS_STATE;
    buffer = storage + KERNEL_FS_PATH_MAX;
    fs_status = kernel_fs_context_resolve_user_path(fs, mm, dirfd,
                                                    user_path, storage,
                                                    KERNEL_FS_PATH_MAX,
                                                    &mount, &result);
    if (fs_status != KERNEL_FS_CONTEXT_STATUS_OK) {
        (void)finish_path(files, storage);
        return KERNEL_FILES_STATUS_STATE;
    }
    if (result == 0) {
        size_t capacity = size < KERNEL_FS_PATH_MAX ? (size_t)size :
                          KERNEL_FS_PATH_MAX;
        result = kernel_vfs_readlink(mount, storage, buffer, capacity,
                                     &bytes_read);
    }
    if (result == 0) {
        access_status = kernel_copy_to_user(mm, user_buffer, buffer,
                                            bytes_read, &copied);
        if (access_status == KERNEL_UACCESS_STATUS_FAULT)
            result = -KERNEL_EFAULT;
        else if (access_status != KERNEL_UACCESS_STATUS_OK ||
                 copied != bytes_read) {
            (void)finish_path(files, storage);
            return KERNEL_FILES_STATUS_STATE;
        }
    }
    if (finish_path(files, storage) != KERNEL_FILES_STATUS_OK)
        return KERNEL_FILES_STATUS_STATE;
    *linux_result = result == 0 ? (int64_t)bytes_read : result;
    return KERNEL_FILES_STATUS_OK;
}

static void fill_linux_vfs_stat(struct kernel_linux_stat *stat,
                                const struct kernel_vfs_stat *vfs_stat)
{
    stat->st_dev = vfs_stat->dev;
    stat->st_ino = vfs_stat->ino;
    stat->st_mode = vfs_stat->mode;
    stat->st_nlink = vfs_stat->nlink;
    stat->st_uid = vfs_stat->uid;
    stat->st_gid = vfs_stat->gid;
    stat->st_rdev = vfs_stat->rdev;
    stat->st_size = (int64_t)vfs_stat->size;
    stat->st_blksize = (int32_t)vfs_stat->blksize;
    stat->st_blocks = (int64_t)vfs_stat->blocks;
    stat->st_atime = vfs_stat->atime.seconds;
    stat->st_atime_nsec = vfs_stat->atime.nanoseconds;
    stat->st_mtime = vfs_stat->mtime.seconds;
    stat->st_mtime_nsec = vfs_stat->mtime.nanoseconds;
    stat->st_ctime = vfs_stat->ctime.seconds;
    stat->st_ctime_nsec = vfs_stat->ctime.nanoseconds;
}

static int fill_linux_stat(
    struct kernel_linux_stat *stat,
    const struct kernel_open_file_description *description)
{
    uint64_t size = kernel_open_file_size(description);
    enum kernel_open_file_kind kind = kernel_open_file_kind(description);

    memset(stat, 0, sizeof(*stat));
    if (kind == KERNEL_OPEN_FILE_KIND_CONSOLE &&
        description->file.private_data == 0) {
        stat->st_mode = KERNEL_VFS_S_IFCHR | UINT32_C(0000600);
        stat->st_rdev = UINT64_C(0x501);
    } else if (kind == KERNEL_OPEN_FILE_KIND_PIPE) {
        stat->st_mode = KERNEL_VFS_S_IFIFO | UINT32_C(0000600);
    } else if (kind == KERNEL_OPEN_FILE_KIND_EPOLL) {
        /* Linux anon_inode_getfile supplies mode 0600 without type bits. */
        stat->st_mode = UINT32_C(0000600);
    } else {
        struct kernel_vfs_stat vfs_stat;
        int result = kernel_vfs_fstat(&description->file, &vfs_stat);

        if (result != 0) return result;
        fill_linux_vfs_stat(stat, &vfs_stat);
        return 0;
    }
    stat->st_nlink = 1U;
    stat->st_size = (int64_t)size;
    stat->st_blksize = (int32_t)BOAROS_PAGE_SIZE;
    return 0;
}

static int copy_stat_to_user(struct kernel_mm *mm,
                             uint64_t user_buffer,
                             const struct kernel_linux_stat *stat,
                             int64_t *linux_result)
{
    size_t copied = 0U;
    enum kernel_uaccess_status access_status =
        kernel_copy_to_user(mm,
                            user_buffer,
                            stat,
                            sizeof(*stat),
                            &copied);

    if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
        *linux_result = -KERNEL_EFAULT;
        return 0;
    }
    return access_status == KERNEL_UACCESS_STATUS_OK &&
                   copied == sizeof(*stat)
               ? 1
               : -1;
}

enum kernel_files_status kernel_files_fstat(
    struct kernel_files *files,
    struct kernel_mm *mm,
    int64_t fd,
    uint64_t user_buffer,
    int64_t *linux_result)
{
    struct kernel_open_file_description *description;
    struct kernel_linux_stat stat;
    int copy_result;
    int result;

    if (!kernel_files_is_live(files) || mm == 0 || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    description = kernel_files_lookup_description(files, fd);
    if (description == 0) {
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    result = fill_linux_stat(&stat, description);
    if (result != 0) {
        *linux_result = result;
        return KERNEL_FILES_STATUS_OK;
    }
    copy_result = copy_stat_to_user(mm, user_buffer, &stat, linux_result);
    if (copy_result < 0) {
        return KERNEL_FILES_STATUS_STATE;
    }
    if (copy_result > 0) {
        *linux_result = 0;
    }
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_fstatat(
    struct kernel_files *files,
    const struct kernel_fs_context *fs,
    struct kernel_mm *mm,
    int64_t dirfd,
    uint64_t user_path,
    uint64_t user_buffer,
    uint64_t flags,
    int64_t *linux_result)
{
    struct kernel_open_file_description *description = 0;
    struct kernel_vfs_mount *mount;
    struct kernel_linux_stat stat;
    char *path;
    size_t path_length;
    enum kernel_heap_status heap_status;
    enum kernel_fs_context_status fs_status;
    enum kernel_open_file_status open_status;
    int copy_result;
    int result;

    if (!kernel_files_is_live(files) || !kernel_fs_context_is_live(fs) ||
        mm == 0 || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    if ((flags & ~(KERNEL_FILES_AT_SYMLINK_NOFOLLOW |
                   KERNEL_FILES_AT_EMPTY_PATH)) != 0U) {
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    heap_status = kernel_heap_allocate(files->heap,
                                       KERNEL_FS_PATH_MAX,
                                       (void **)&path);
    if (heap_status == KERNEL_HEAP_STATUS_EMPTY) {
        *linux_result = -KERNEL_ENOMEM;
        return KERNEL_FILES_STATUS_OK;
    }
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        return KERNEL_FILES_STATUS_STATE;
    }
    {
        enum kernel_uaccess_status access_status =
            kernel_copy_string_from_user(mm,
                                         path,
                                         user_path,
                                         KERNEL_FS_PATH_MAX,
                                         &path_length);

        if (access_status != KERNEL_UACCESS_STATUS_OK) {
            (void)kernel_files_release_allocation(files, path);
            if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
                *linux_result = -KERNEL_EFAULT;
                return KERNEL_FILES_STATUS_OK;
            }
            if (access_status == KERNEL_UACCESS_STATUS_TOO_LONG) {
                *linux_result = -KERNEL_ENAMETOOLONG;
                return KERNEL_FILES_STATUS_OK;
            }
            return KERNEL_FILES_STATUS_STATE;
        }
    }
    if (path_length == 0U) {
        if ((flags & KERNEL_FILES_AT_EMPTY_PATH) == 0U) {
            (void)kernel_files_release_allocation(files, path);
            *linux_result = -KERNEL_ENOENT;
            return KERNEL_FILES_STATUS_OK;
        }
        description = kernel_files_lookup_description(files, dirfd);
        if (description == 0) {
            (void)kernel_files_release_allocation(files, path);
            *linux_result = -KERNEL_EBADF;
            return KERNEL_FILES_STATUS_OK;
        }
        if (kernel_open_file_acquire(description) !=
            KERNEL_OPEN_FILE_STATUS_OK) {
            (void)kernel_files_release_allocation(files, path);
            return KERNEL_FILES_STATUS_STATE;
        }
    } else {
        fs_status = kernel_fs_context_resolve_kernel_path(fs,
                                                          dirfd,
                                                          path,
                                                          path_length,
                                                          path,
                                                          KERNEL_FS_PATH_MAX,
                                                          &mount,
                                                          &result);
        if (fs_status != KERNEL_FS_CONTEXT_STATUS_OK) {
            (void)kernel_files_release_allocation(files, path);
            return KERNEL_FILES_STATUS_STATE;
        }
        if (result != 0) {
            (void)kernel_files_release_allocation(files, path);
            *linux_result = result;
            return KERNEL_FILES_STATUS_OK;
        }
        if ((flags & KERNEL_FILES_AT_SYMLINK_NOFOLLOW) != 0U) {
            struct kernel_vfs_stat vfs_stat;

            result = kernel_vfs_stat_path(mount, path, 0, &vfs_stat);
            if (kernel_files_release_allocation(files, path) !=
                KERNEL_FILES_STATUS_OK) return KERNEL_FILES_STATUS_STATE;
            if (result != 0) {
                *linux_result = result;
                return KERNEL_FILES_STATUS_OK;
            }
            memset(&stat, 0, sizeof(stat));
            fill_linux_vfs_stat(&stat, &vfs_stat);
            copy_result = copy_stat_to_user(mm, user_buffer, &stat,
                                            linux_result);
            if (copy_result < 0) return KERNEL_FILES_STATUS_STATE;
            if (copy_result > 0) *linux_result = 0;
            return KERNEL_FILES_STATUS_OK;
        }
        open_status = kernel_open_file_create(files->heap,
                                              mount,
                                              path,
                                              &description,
                                              &result);
        if (open_status != KERNEL_OPEN_FILE_STATUS_OK) {
            (void)kernel_files_release_allocation(files, path);
            if (open_status == KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED &&
                description != 0) {
                kernel_files_queue_description(files, description);
                *linux_result = -KERNEL_EIO;
                return KERNEL_FILES_STATUS_OK;
            }
            return open_status == KERNEL_OPEN_FILE_STATUS_NO_MEMORY
                       ? KERNEL_FILES_STATUS_NO_MEMORY
                       : KERNEL_FILES_STATUS_STATE;
        }
        if (result != 0) {
            /* A path lookup error owns no OFD when its temporary allocation
             * was released successfully. */
            if (description != 0 &&
                kernel_open_file_release(&description) !=
                    KERNEL_OPEN_FILE_STATUS_OK) {
                (void)kernel_files_release_allocation(files, path);
                return KERNEL_FILES_STATUS_STATE;
            }
            (void)kernel_files_release_allocation(files, path);
            *linux_result = result;
            return KERNEL_FILES_STATUS_OK;
        }
    }
    (void)kernel_files_release_allocation(files, path);
    result = fill_linux_stat(&stat, description);
    if (result != 0) {
        open_status = kernel_open_file_release(&description);
        if (open_status == KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED &&
            description != 0) {
            kernel_files_queue_description(files, description);
        } else if (open_status != KERNEL_OPEN_FILE_STATUS_OK) {
            return KERNEL_FILES_STATUS_STATE;
        }
        *linux_result = result;
        return KERNEL_FILES_STATUS_OK;
    }
    copy_result = copy_stat_to_user(mm, user_buffer, &stat, linux_result);
    open_status = kernel_open_file_release(&description);
    if (open_status == KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED &&
        description != 0) {
        kernel_files_queue_description(files, description);
    } else if (open_status != KERNEL_OPEN_FILE_STATUS_OK) {
        return KERNEL_FILES_STATUS_STATE;
    }
    if (copy_result < 0) {
        return KERNEL_FILES_STATUS_STATE;
    }
    if (copy_result > 0) {
        *linux_result = 0;
    }
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_mkdirat(
    struct kernel_files *files,
    const struct kernel_fs_context *fs,
    struct kernel_mm *mm,
    int64_t dirfd,
    uint64_t user_path,
    uint32_t mode,
    int64_t *linux_result)
{
    struct kernel_vfs_mount *mount;
    char *path;
    int result;
    enum kernel_heap_status heap_status;
    enum kernel_fs_context_status fs_status;

    if (!kernel_files_is_live(files) || !kernel_fs_context_is_live(fs) ||
        mm == 0 || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    heap_status = kernel_heap_allocate(files->heap,
                                       KERNEL_FS_PATH_MAX,
                                       (void **)&path);
    if (heap_status == KERNEL_HEAP_STATUS_EMPTY) {
        *linux_result = -KERNEL_ENOMEM;
        return KERNEL_FILES_STATUS_OK;
    }
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        return KERNEL_FILES_STATUS_STATE;
    }
    fs_status = kernel_fs_context_resolve_user_path(fs,
                                                    mm,
                                                    dirfd,
                                                    user_path,
                                                    path,
                                                    KERNEL_FS_PATH_MAX,
                                                    &mount,
                                                    &result);
    if (fs_status != KERNEL_FS_CONTEXT_STATUS_OK) {
        (void)finish_path(files, path);
        return KERNEL_FILES_STATUS_STATE;
    }
    if (result != 0) {
        if (finish_path(files, path) != KERNEL_FILES_STATUS_OK) {
            return KERNEL_FILES_STATUS_STATE;
        }
        *linux_result = result;
        return KERNEL_FILES_STATUS_OK;
    }
    result = kernel_vfs_mkdir(mount, path, mode);
    if (finish_path(files, path) != KERNEL_FILES_STATUS_OK) {
        return KERNEL_FILES_STATUS_STATE;
    }
    *linux_result = result;
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_unlinkat(
    struct kernel_files *files,
    const struct kernel_fs_context *fs,
    struct kernel_mm *mm,
    int64_t dirfd,
    uint64_t user_path,
    uint32_t flags,
    int64_t *linux_result)
{
    struct kernel_vfs_mount *mount;
    char *path;
    int result;
    enum kernel_heap_status heap_status;
    enum kernel_fs_context_status fs_status;

    if (!kernel_files_is_live(files) || !kernel_fs_context_is_live(fs) ||
        mm == 0 || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    if ((flags & ~KERNEL_FILES_AT_REMOVEDIR) != 0U) {
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    heap_status = kernel_heap_allocate(files->heap,
                                       KERNEL_FS_PATH_MAX,
                                       (void **)&path);
    if (heap_status == KERNEL_HEAP_STATUS_EMPTY) {
        *linux_result = -KERNEL_ENOMEM;
        return KERNEL_FILES_STATUS_OK;
    }
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        return KERNEL_FILES_STATUS_STATE;
    }
    fs_status = kernel_fs_context_resolve_user_path(fs,
                                                    mm,
                                                    dirfd,
                                                    user_path,
                                                    path,
                                                    KERNEL_FS_PATH_MAX,
                                                    &mount,
                                                    &result);
    if (fs_status != KERNEL_FS_CONTEXT_STATUS_OK) {
        (void)finish_path(files, path);
        return KERNEL_FILES_STATUS_STATE;
    }
    if (result != 0) {
        if (finish_path(files, path) != KERNEL_FILES_STATUS_OK) {
            return KERNEL_FILES_STATUS_STATE;
        }
        *linux_result = result;
        return KERNEL_FILES_STATUS_OK;
    }
    if ((flags & KERNEL_FILES_AT_REMOVEDIR) != 0U) {
        result = kernel_vfs_rmdir(mount, path);
    } else {
        result = kernel_vfs_unlink(mount, path);
    }
    if (finish_path(files, path) != KERNEL_FILES_STATUS_OK) {
        return KERNEL_FILES_STATUS_STATE;
    }
    *linux_result = result;
    return KERNEL_FILES_STATUS_OK;
}
