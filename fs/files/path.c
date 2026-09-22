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
        LINUX_O_DIRECT |
        LINUX_O_NOATIME | LINUX_O_PATH |
        (LINUX_O_TMPFILE & ~LINUX_O_DIRECTORY);
    const uint64_t known_flags = LINUX_O_ACCMODE | write_flags |
        unsupported_flags | LINUX_O_NONBLOCK | LINUX_O_DIRECTORY |
        LINUX_O_NOFOLLOW |
        LINUX_O_LARGEFILE | LINUX_O_CLOEXEC | LINUX_O_SYNC;
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

/* Callers hold the file table and fs context throughout this serialized call. */
static int select_path_start(struct kernel_files *files,
                              const struct kernel_fs_context *fs,
                              int64_t dirfd, const char *path,
                              struct kernel_vfs_path **start)
{
    if (!path[0]) return -KERNEL_ENOENT;
    if (path[0] == '/') *start = kernel_fs_context_root(fs);
    else if (dirfd == KERNEL_FS_AT_FDCWD) *start = kernel_fs_context_cwd(fs);
    else {
        struct kernel_open_file_description *file =
            kernel_files_lookup_description(files, dirfd);
        if (!file) return -KERNEL_EBADF;
        if ((kernel_open_file_mode(file) & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR)
            return -KERNEL_ENOTDIR;
        *start = file->file.path;
    }
    return *start ? 0 : -KERNEL_EIO;
}

static enum kernel_fs_context_status copy_path_start(
    struct kernel_files *files, const struct kernel_fs_context *fs,
    struct kernel_mm *mm, int64_t dirfd, uint64_t user_path,
    char *buffer, size_t capacity, struct kernel_vfs_path **start,
    struct kernel_vfs_mount **mount, int *result)
{
    size_t length;
    enum kernel_uaccess_status status = kernel_copy_string_from_user(mm,
                                  buffer, user_path, capacity, &length);
    if (status == KERNEL_UACCESS_STATUS_FAULT) *result = -KERNEL_EFAULT;
    else if (status == KERNEL_UACCESS_STATUS_TOO_LONG) *result = -KERNEL_ENAMETOOLONG;
    else if (status != KERNEL_UACCESS_STATUS_OK) return KERNEL_FS_CONTEXT_STATUS_STATE;
    else {
        *result = select_path_start(files, fs, dirfd, buffer, start);
        if (!*result) *mount = kernel_vfs_path_mount(*start);
    }
    return KERNEL_FS_CONTEXT_STATUS_OK;
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
    struct kernel_vfs_path *start = 0;
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
    fs_status = copy_path_start(files, fs,
                                                    mm,
                                                    dirfd,
                                                    user_path,
                                                    path,
                                                    KERNEL_FS_PATH_MAX,
                                                    &start, &mount,
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
    enum kernel_open_file_path_operation operation =
        ((flags & LINUX_O_NOFOLLOW) ||
         (flags & (LINUX_O_CREAT | LINUX_O_EXCL)) == (LINUX_O_CREAT | LINUX_O_EXCL))
            ? KERNEL_OPEN_PATH_NOFOLLOW : KERNEL_OPEN_PATH_FOLLOW;
    open_status = kernel_open_file_create_at(files->heap, start,
                    kernel_fs_context_root(fs), path, operation, 0,
                    &description, &result);
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
        open_status = kernel_open_file_create_at(files->heap, start,
                    kernel_fs_context_root(fs), path, KERNEL_OPEN_PATH_CREATE,
                    (uint32_t)mode, &description, &result);
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

    /* Linux treats the internal __O_SYNC bit as implying O_DSYNC. */
    if ((flags & (LINUX_O_SYNC & ~LINUX_O_DSYNC)) != 0U)
        flags |= LINUX_O_DSYNC;
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
    struct kernel_vfs_path *start = 0;
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
    fs_status = copy_path_start(files, fs, mm, dirfd,
                                                    user_linkpath, linkpath,
                                                    KERNEL_FS_PATH_MAX,
                                                    &start, &mount, &result);
    if (fs_status != KERNEL_FS_CONTEXT_STATUS_OK) {
        (void)finish_path(files, storage);
        return KERNEL_FILES_STATUS_STATE;
    }
    if (result == 0) {
        result = target_length == 0U ? -KERNEL_ENOENT :
            kernel_vfs_symlink_at(start, kernel_fs_context_root(fs), storage, linkpath);
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
    struct kernel_vfs_path *start = 0;
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
    fs_status = copy_path_start(files, fs, mm, dirfd,
                                                    user_path, storage,
                                                    KERNEL_FS_PATH_MAX,
                                                    &start, &mount, &result);
    if (fs_status != KERNEL_FS_CONTEXT_STATUS_OK) {
        (void)finish_path(files, storage);
        return KERNEL_FILES_STATUS_STATE;
    }
    if (result == 0) {
        size_t capacity = size < KERNEL_FS_PATH_MAX ? (size_t)size :
                          KERNEL_FS_PATH_MAX;
        result = kernel_vfs_readlink_at(start, kernel_fs_context_root(fs), storage, buffer, capacity,
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
    struct kernel_linux_stat stat = {0};
    struct kernel_vfs_stat vfs_stat;
    struct kernel_vfs_path *start = 0;
    char *path;
    size_t length;
    int result;
    if (!kernel_files_is_live(files) || !kernel_fs_context_is_live(fs) ||
        !mm || !linux_result) return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    if (flags & ~(KERNEL_FILES_AT_SYMLINK_NOFOLLOW | KERNEL_FILES_AT_EMPTY_PATH)) {
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    enum kernel_heap_status allocation = kernel_heap_allocate(files->heap,
                                         KERNEL_FS_PATH_MAX, (void **)&path);
    if (allocation != KERNEL_HEAP_STATUS_OK) {
        if (allocation != KERNEL_HEAP_STATUS_EMPTY) return KERNEL_FILES_STATUS_STATE;
        *linux_result = -KERNEL_ENOMEM;
        return KERNEL_FILES_STATUS_OK;
    }
    enum kernel_uaccess_status access = kernel_copy_string_from_user(mm, path,
                                 user_path, KERNEL_FS_PATH_MAX, &length);
    if (access != KERNEL_UACCESS_STATUS_OK) {
        (void)finish_path(files, path);
        if (access != KERNEL_UACCESS_STATUS_FAULT && access != KERNEL_UACCESS_STATUS_TOO_LONG)
            return KERNEL_FILES_STATUS_STATE;
        *linux_result = access == KERNEL_UACCESS_STATUS_FAULT ? -KERNEL_EFAULT
                                                              : -KERNEL_ENAMETOOLONG;
        return KERNEL_FILES_STATUS_OK;
    }
    if (!length && (flags & KERNEL_FILES_AT_EMPTY_PATH)) {
        if (dirfd == KERNEL_FS_AT_FDCWD) {
            result = kernel_vfs_path_stat(kernel_fs_context_cwd(fs), &vfs_stat);
            if (!result) fill_linux_vfs_stat(&stat, &vfs_stat);
        } else {
            struct kernel_open_file_description *file =
                kernel_files_lookup_description(files, dirfd);
            result = file ? fill_linux_stat(&stat, file) : -KERNEL_EBADF;
        }
    } else {
        result = select_path_start(files, fs, dirfd, path, &start);
        if (!result) result = kernel_vfs_stat_at(start, kernel_fs_context_root(fs),
                    path, !(flags & KERNEL_FILES_AT_SYMLINK_NOFOLLOW), &vfs_stat);
        if (!result) fill_linux_vfs_stat(&stat, &vfs_stat);
    }
    if (finish_path(files, path) != KERNEL_FILES_STATUS_OK) return KERNEL_FILES_STATUS_STATE;
    if (result) { *linux_result = result; return KERNEL_FILES_STATUS_OK; }
    int copied = copy_stat_to_user(mm, user_buffer, &stat, linux_result);
    if (copied < 0) return KERNEL_FILES_STATUS_STATE;
    if (copied > 0) *linux_result = 0;
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
    struct kernel_vfs_path *start = 0;
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
    fs_status = copy_path_start(files, fs,
                                                    mm,
                                                    dirfd,
                                                    user_path,
                                                    path,
                                                    KERNEL_FS_PATH_MAX,
                                                    &start, &mount,
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
    result = kernel_vfs_mkdir_at(start, kernel_fs_context_root(fs), path, mode);
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
    struct kernel_vfs_path *start = 0;
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
    fs_status = copy_path_start(files, fs,
                                                    mm,
                                                    dirfd,
                                                    user_path,
                                                    path,
                                                    KERNEL_FS_PATH_MAX,
                                                    &start, &mount,
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
        result = kernel_vfs_unlink_at(start, kernel_fs_context_root(fs), path, 1);
    } else {
        result = kernel_vfs_unlink_at(start, kernel_fs_context_root(fs), path, 0);
    }
    if (finish_path(files, path) != KERNEL_FILES_STATUS_OK) {
        return KERNEL_FILES_STATUS_STATE;
    }
    *linux_result = result;
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_chdir(
    struct kernel_files *files, const struct kernel_fs_context *fs,
    struct kernel_mm *mm, uint64_t user_path, int64_t *linux_result)
{
    char *path;
    struct kernel_vfs_path *start = 0, *resolved = 0;
    struct kernel_vfs_mount *mount = 0;
    int result;
    if (!kernel_files_is_live(files) || !kernel_fs_context_is_live(fs) ||
        !mm || !linux_result) return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    enum kernel_heap_status allocation = kernel_heap_allocate(files->heap,
                                         KERNEL_FS_PATH_MAX, (void **)&path);
    if (allocation != KERNEL_HEAP_STATUS_OK) {
        if (allocation != KERNEL_HEAP_STATUS_EMPTY) return KERNEL_FILES_STATUS_STATE;
        *linux_result = -KERNEL_ENOMEM;
        return KERNEL_FILES_STATUS_OK;
    }
    enum kernel_fs_context_status status = copy_path_start(files, fs, mm,
        KERNEL_FS_AT_FDCWD, user_path, path, KERNEL_FS_PATH_MAX, &start, &mount, &result);
    if (status != KERNEL_FS_CONTEXT_STATUS_OK) {
        (void)finish_path(files, path);
        return KERNEL_FILES_STATUS_STATE;
    }
    if (!result) result = kernel_vfs_path_resolve(start,
                            kernel_fs_context_root(fs), path, 1, &resolved);
    if (!result) result = kernel_fs_context_set_cwd(fs, resolved);
    if (resolved) (void)kernel_vfs_path_release(&resolved);
    if (finish_path(files, path) != KERNEL_FILES_STATUS_OK) return KERNEL_FILES_STATUS_STATE;
    *linux_result = result;
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_fchdir(
    struct kernel_files *files, const struct kernel_fs_context *fs,
    int64_t fd, int64_t *linux_result)
{
    if (!kernel_files_is_live(files) || !kernel_fs_context_is_live(fs) || !linux_result)
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    struct kernel_open_file_description *file = kernel_files_lookup_description(files, fd);
    if (!file) *linux_result = -KERNEL_EBADF;
    else if ((kernel_open_file_mode(file) & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR)
        *linux_result = -KERNEL_ENOTDIR;
    else *linux_result = kernel_fs_context_set_cwd(fs, file->file.path);
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_getcwd(
    struct kernel_files *files, const struct kernel_fs_context *fs,
    struct kernel_mm *mm, uint64_t user_buffer, uint64_t size,
    int64_t *linux_result)
{
    char *path;
    if (!kernel_files_is_live(files) || !kernel_fs_context_is_live(fs) ||
        !mm || !linux_result) return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    enum kernel_heap_status allocation = kernel_heap_allocate(files->heap,
                                         KERNEL_FS_PATH_MAX, (void **)&path);
    if (allocation != KERNEL_HEAP_STATUS_OK) {
        if (allocation != KERNEL_HEAP_STATUS_EMPTY) return KERNEL_FILES_STATUS_STATE;
        *linux_result = -KERNEL_ENOMEM;
        return KERNEL_FILES_STATUS_OK;
    }
    int result = kernel_vfs_path_string(kernel_fs_context_cwd(fs),
                         kernel_fs_context_root(fs), path, KERNEL_FS_PATH_MAX);
    if (result == -KERNEL_ERANGE) result = -KERNEL_ENAMETOOLONG;
    size_t length = result ? 0U : strlen(path) + 1U;
    if (!result && length > size) result = -KERNEL_ERANGE;
    if (!result) {
        size_t copied = 0;
        enum kernel_uaccess_status access = kernel_copy_to_user(mm, user_buffer,
                                                        path, length, &copied);
        if (access == KERNEL_UACCESS_STATUS_FAULT) result = -KERNEL_EFAULT;
        else if (access != KERNEL_UACCESS_STATUS_OK || copied != length) {
            (void)finish_path(files, path);
            return KERNEL_FILES_STATUS_STATE;
        }
    }
    if (finish_path(files, path) != KERNEL_FILES_STATUS_OK) return KERNEL_FILES_STATUS_STATE;
    *linux_result = result ? result : (int64_t)length;
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_renameat(
    struct kernel_files *files, const struct kernel_fs_context *fs,
    struct kernel_mm *mm, int64_t old_dirfd, uint64_t old_user_path,
    int64_t new_dirfd, uint64_t new_user_path, uint32_t flags,
    int64_t *linux_result)
{
    char *paths;
    struct kernel_vfs_path *old_start = 0, *new_start = 0;
    struct kernel_vfs_mount *mount = 0;
    int result;
    if (!kernel_files_is_live(files) || !kernel_fs_context_is_live(fs) ||
        !mm || !linux_result) return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    if ((flags & ~7U) || ((flags & 2U) && (flags & 5U))) { *linux_result = -KERNEL_EINVAL; return KERNEL_FILES_STATUS_OK; }
    if (flags & 6U) { *linux_result = -KERNEL_ENOTSUP; return KERNEL_FILES_STATUS_OK; }
    enum kernel_heap_status allocation = kernel_heap_allocate(files->heap,
                                     2U * KERNEL_FS_PATH_MAX, (void **)&paths);
    if (allocation != KERNEL_HEAP_STATUS_OK) {
        if (allocation != KERNEL_HEAP_STATUS_EMPTY) return KERNEL_FILES_STATUS_STATE;
        *linux_result = -KERNEL_ENOMEM;
        return KERNEL_FILES_STATUS_OK;
    }
    enum kernel_fs_context_status status = copy_path_start(files, fs, mm,
        old_dirfd, old_user_path, paths, KERNEL_FS_PATH_MAX, &old_start, &mount, &result);
    if (status == KERNEL_FS_CONTEXT_STATUS_OK && !result)
        status = copy_path_start(files, fs, mm, new_dirfd, new_user_path,
              paths + KERNEL_FS_PATH_MAX, KERNEL_FS_PATH_MAX, &new_start, &mount, &result);
    if (status != KERNEL_FS_CONTEXT_STATUS_OK) {
        (void)finish_path(files, paths);
        return KERNEL_FILES_STATUS_STATE;
    }
    if (!result) result = kernel_vfs_rename_at(old_start, new_start,
                   kernel_fs_context_root(fs), paths, paths + KERNEL_FS_PATH_MAX, flags);
    if (finish_path(files, paths) != KERNEL_FILES_STATUS_OK) return KERNEL_FILES_STATUS_STATE;
    *linux_result = result;
    return KERNEL_FILES_STATUS_OK;
}
