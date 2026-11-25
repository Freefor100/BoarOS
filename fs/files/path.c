#include "../vfs_notify.h"
#include <kernel/scheduler.h>
#include "../open_file_internal.h"
#include "../lease.h"
#include "../pipe_internal.h"
#include "../vfs_internal.h"
#include "../char_device_internal.h"
#include "../block_file_internal.h"
#include "private.h"

#include <kernel/errno.h>
#include <kernel/credentials.h>
#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/open_file.h>
#include <kernel/mqueue.h>
#include <kernel/page.h>
#include <kernel/task.h>
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
#define LINUX_O_NOCTTY UINT64_C(00000400)
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
#define LINUX_O_EMPTYPATH (UINT64_C(1) << 26)
#define LINUX_OPENAT2_REGULAR (UINT64_C(1) << 32)
#define LINUX_OPEN_SUPPORTED_FLAGS (LINUX_O_ACCMODE | LINUX_O_CREAT | LINUX_O_TRUNC | \
    LINUX_O_APPEND | LINUX_O_EXCL | LINUX_O_NOCTTY | LINUX_O_DIRECT | LINUX_O_TMPFILE | \
    LINUX_O_NOATIME | LINUX_O_NONBLOCK | LINUX_O_DIRECTORY | LINUX_O_NOFOLLOW | \
    LINUX_O_LARGEFILE | LINUX_O_CLOEXEC | LINUX_O_SYNC | LINUX_O_PATH)

static void path_buffer_release(struct kernel_task_io_buffer *buffer)
{
    if (buffer->allocator) kernel_task_io_buffer_release(buffer);
}

static int validate_open_flags(uint64_t flags, uint64_t mode, uint64_t resolve,
                               int strict, uint32_t *fd_flags)
{
    uint64_t unsupported_flags = 0;
    uint64_t known_flags = LINUX_OPEN_SUPPORTED_FLAGS;
    uint64_t access_mode = flags & LINUX_O_ACCMODE;

    if ((flags & (LINUX_O_TMPFILE & ~LINUX_O_DIRECTORY)) &&
        (!(flags & LINUX_O_DIRECTORY) || !access_mode || (flags & LINUX_O_CREAT)))
        return -KERNEL_EINVAL;
    if (strict) {
        uint64_t tmpfile = flags & (LINUX_O_TMPFILE & ~LINUX_O_DIRECTORY);
        int create = !!(flags & LINUX_O_CREAT) || tmpfile;
        const uint64_t path_flags = LINUX_O_PATH | LINUX_O_DIRECTORY |
            LINUX_O_NOFOLLOW | LINUX_O_CLOEXEC | LINUX_O_EMPTYPATH;
        known_flags |= LINUX_OPENAT2_REGULAR | LINUX_O_EMPTYPATH;
        unsupported_flags |= LINUX_O_EMPTYPATH;
        if ((resolve & ~UINT64_C(0x3f)) ||
            (resolve & KERNEL_VFS_RESOLVE_SCOPED) == KERNEL_VFS_RESOLVE_SCOPED ||
            (mode & ~(create ? UINT64_C(07777) : UINT64_C(0))) ||
            (tmpfile && (!(flags & LINUX_O_DIRECTORY) || access_mode == 0)) ||
            ((flags & LINUX_OPENAT2_REGULAR) && (flags & LINUX_O_DIRECTORY)) ||
            ((flags & LINUX_O_PATH) && (flags & ~path_flags)))
            return -KERNEL_EINVAL;
    }
    if ((!strict && access_mode == 3U &&
         !(flags & (LINUX_O_TMPFILE & ~LINUX_O_DIRECTORY))) || (flags & ~known_flags) != 0U) {
        return -KERNEL_EINVAL;
    }
    if ((flags & LINUX_O_DIRECTORY) != 0U && (flags & LINUX_O_CREAT) != 0U) {
        return -KERNEL_EINVAL;
    }
    if (strict && (resolve & KERNEL_VFS_RESOLVE_CACHED) &&
        (flags & (LINUX_O_CREAT | LINUX_O_TRUNC | (LINUX_O_TMPFILE & ~LINUX_O_DIRECTORY))))
        return -KERNEL_EAGAIN;
    if ((flags & unsupported_flags) != 0U) {
        return -KERNEL_ENOTSUP;
    }
    *fd_flags = (flags & LINUX_O_CLOEXEC) != 0U
                    ? KERNEL_FILES_FD_CLOEXEC
                    : 0U;
    return 0;
}

/* peer ioctl已持有稳定路径；在可能睡眠的device open前预留fd。 */
int kernel_files_open_pty_peer(struct kernel_files *files, struct kernel_task *caller,
    struct kernel_vfs_path *path, uint32_t flags)
{
    KERNEL_SERVICE_SCOPE(service);
    (void)caller;
    uint32_t fd_flags = 0, fd;
    if (flags & LINUX_O_PATH) flags &= LINUX_O_PATH | LINUX_O_DIRECTORY | LINUX_O_NOFOLLOW | LINUX_O_CLOEXEC;
    int error = validate_open_flags(flags, 0, 0, 0, &fd_flags);
    if (error) return error;
    if (flags & LINUX_O_DIRECT) return -KERNEL_EINVAL;
    if (kernel_files_find_free_fd(files, &fd, &error) != KERNEL_FILES_STATUS_OK) __builtin_trap();
    if (error) return error;
    struct kernel_files_fd_reservation reservation __attribute__((cleanup(kernel_files_cancel_reservation))) = {files, fd};
    struct kernel_open_file_description *file = 0;
    enum kernel_heap_status allocation = kernel_heap_allocate_zeroed(files->heap, 1, sizeof(*file), (void **)&file);
    if (allocation != KERNEL_HEAP_STATUS_OK) return allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    file->heap = files->heap; file->references = 1;
    kernel_mutex_init(&file->offset_lock, 10, (uintptr_t)file);
    if (flags & LINUX_O_PATH) {
        error = kernel_vfs_path_acquire(path);
        if (!error) {
            file->kind = KERNEL_OPEN_FILE_KIND_PATH;
            file->file.path = path; file->file.mount = kernel_vfs_path_mount(path);
            file->file.mode = kernel_vfs_path_mode(path);
        }
    } else error = kernel_vfs_path_open(path, &file->file);
    if (error) { kernel_heap_release(files->heap, file); return error; }
    if (flags & LINUX_O_DIRECTORY) error = -KERNEL_ENOTDIR;
    if (!error && !(flags & LINUX_O_PATH)) {
        struct kernel_vfs_stat stat;
        error = kernel_vfs_fstat(&file->file, &stat);
        if (!error) {
            file->device = kernel_char_device_lookup(stat.rdev);
            if (!file->device) error = -KERNEL_ENXIO;
            else { file->kind = file->device->kind; error = kernel_char_device_open(file, flags); }
        }
    }
    if (error) {
        kernel_files_queue_description(files, file);
        (void)kernel_files_drain_file_cleanup(files);
        return error;
    }
    file->open_flags = flags & ~LINUX_O_CLOEXEC;
    file->observed_mount_error = kernel_vfs_mount_error_sequence(file->file.mount);
    file->observed_writeback_error = kernel_vfs_error_sequence(&file->file);
    file->credentials=kernel_credentials_get(kernel_credentials_current());
    if (kernel_files_install_new_owned_at(files, fd, fd_flags, &file) != KERNEL_FILES_STATUS_OK) __builtin_trap();
    reservation.files=0;
    return (int)fd;
}

/* Capture identity before namespace locking or a storage sleep. */
int kernel_files_path_start(struct kernel_files *files,
                              const struct kernel_fs_paths *snapshot,
                              int64_t dirfd, const char *path,
                              struct kernel_vfs_path **start)
{
    /* fd被关闭后，这个pin仍覆盖借用路径，直到取得独立path引用。 */
    KERNEL_FILES_PIN_SCOPE(selected_fd);
    if (!path[0]) return -KERNEL_ENOENT;
    if (path[0] == '/') *start = snapshot ? snapshot->root : NULL;
    else if (dirfd == KERNEL_FS_AT_FDCWD) *start = snapshot ? snapshot->cwd : NULL;
    else {
        struct kernel_open_file_description *file = kernel_files_hold_fd(files,dirfd,&selected_fd);
        if (!file) return -KERNEL_EBADF;
        if ((kernel_open_file_mode(file) & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR)
            return -KERNEL_ENOTDIR;
        *start = file->file.path;
    }
    return *start ? kernel_vfs_path_acquire(*start) : -KERNEL_EIO;
}

static int borrow_lookup_start(struct kernel_files *files,
    const struct kernel_fs_paths *snapshot,int64_t dirfd,const char *path,
    struct kernel_vfs_path **start,struct kernel_vfs_path **owner)
{
    if (!path[0]) return -KERNEL_ENOENT;
    if (path[0]=='/') *start=snapshot->root;
    else if (dirfd==KERNEL_FS_AT_FDCWD) *start=snapshot->cwd;
    else {
        int result=kernel_files_path_start(files,snapshot,dirfd,path,owner);
        if (result) return result;
        *start=*owner;
    }
    return *start ? 0 : -KERNEL_EIO;
}

static enum kernel_fs_context_status copy_path(
    struct kernel_mm *mm, uint64_t user_path, char *buffer, size_t capacity,
    int *result)
{
    size_t length;
    enum kernel_uaccess_status status = kernel_copy_string_from_user(mm,
                                  buffer, user_path, capacity, &length);
    if (status == KERNEL_UACCESS_STATUS_FAULT) *result = -KERNEL_EFAULT;
    else if (status == KERNEL_UACCESS_STATUS_TOO_LONG) *result = -KERNEL_ENAMETOOLONG;
    else if (status != KERNEL_UACCESS_STATUS_OK) return KERNEL_FS_CONTEXT_STATUS_STATE;
    else *result=0;
    return KERNEL_FS_CONTEXT_STATUS_OK;
}

static enum kernel_fs_context_status copy_path_start(
    struct kernel_files *files, const struct kernel_fs_paths *snapshot,
    struct kernel_mm *mm, int64_t dirfd, uint64_t user_path,
    char *buffer, size_t capacity, struct kernel_vfs_path **start,
    struct kernel_vfs_mount **mount, int *result)
{
    enum kernel_fs_context_status status=copy_path(mm,user_path,buffer,capacity,result);
    if (status==KERNEL_FS_CONTEXT_STATUS_OK && !*result) {
        *result = kernel_files_path_start(files, snapshot, dirfd, buffer, start);
        if (!*result) *mount = kernel_vfs_path_mount(*start);
    }
    return status;
}

static enum kernel_files_status finish_open(struct kernel_files *files,
    struct kernel_open_file_description *description, uint64_t flags,
    struct kernel_files_fd_reservation *reservation, uint32_t fd_flags,
    int created, int special_link, struct kernel_open_file_description **direct,
    int64_t *linux_result)
{
    KERNEL_SERVICE_SCOPE_IF(service, 0);
    uint32_t object_type = kernel_open_file_mode(description) & KERNEL_VFS_S_IFMT;
    if (!(flags & LINUX_O_PATH) && !special_link) {
        int error = kernel_vfs_fanotify_permission(&description->file, VFS_NOTIFY_OPEN_PERM,
            &description->file.notify_access_perm);
        if (error) {
            KERNEL_FILES_NOTE(files, open_failures, 1);
            kernel_files_queue_description(files, description);
            (void)kernel_files_drain_file_cleanup(files);
            *linux_result = error;
            return KERNEL_FILES_STATUS_OK;
        }
    }
    /* Permission notification precedes the backend's O_DIRECT check.
     * Validate direct I/O before truncation or device open. */
    if (!(flags & LINUX_O_PATH) && (flags & LINUX_O_DIRECT) && object_type != KERNEL_VFS_S_IFBLK &&
        !kernel_vfs_direct_alignment(&description->file)) {
        kernel_files_queue_description(files, description);
        (void)kernel_files_drain_file_cleanup(files);
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_vfs_namespace_requires_cpu0(description->file.mount) ||
        (!(flags & LINUX_O_PATH) && object_type != KERNEL_VFS_S_IFREG && object_type != KERNEL_VFS_S_IFDIR))
        kernel_service_scope_begin(&service);
    if (flags & LINUX_O_PATH) goto Finish_open;
    if (special_link) {
        if ((flags & LINUX_O_NOATIME) && !kernel_open_file_noatime_allowed(description)) {
            kernel_files_queue_description(files,description);
            (void)kernel_files_drain_file_cleanup(files);*linux_result=-KERNEL_EPERM;return 0;
        }
        if ((flags & LINUX_O_DIRECTORY) != 0U) {
            KERNEL_FILES_NOTE(files, open_failures, 1);
            kernel_files_queue_description(files, description);
            (void)kernel_files_drain_file_cleanup(files);
            *linux_result = -KERNEL_ENOTDIR;
            return KERNEL_FILES_STATUS_OK;
        }
        goto Finish_open;
    }
    /* VFS has checked access intent, O_DIRECTORY and directory write errors
     * before publishing this description or opening a device/FIFO. */
    if ((kernel_open_file_mode(description) & KERNEL_VFS_S_IFMT)==KERNEL_VFS_S_IFDIR) {
        description->kind = KERNEL_OPEN_FILE_KIND_DIRECTORY;
    } else if(kernel_mqueue_fs_is_mount(description->file.mount)) {
        struct kernel_open_file_description *mq=0;
        int error=kernel_mqueue_fs_open(files->heap,&description->file,(uint32_t)flags,&mq);
        if(!error)description->vfs_closed=1;
        if(kernel_open_file_release(&description))__builtin_trap();
        if(error){KERNEL_FILES_NOTE(files,open_failures,1);*linux_result=error;return KERNEL_FILES_STATUS_OK;}
        description=mq;
    } else if ((kernel_open_file_mode(description) & KERNEL_VFS_S_IFMT) ==
               KERNEL_VFS_S_IFREG) {
        uint64_t access_mode = flags & LINUX_O_ACCMODE;

        int generated = kernel_vfs_file_generated(&description->file);
        int write=access_mode==LINUX_O_WRONLY || access_mode==LINUX_O_RDWR;
        int error=0;
        if (!generated && write)
            error=kernel_vfs_file_acquire_write(&description->file);
        if (!error && !generated)
            error=kernel_vfs_lease_open(&description->file,(int)access_mode,flags&LINUX_O_NONBLOCK);
        if (!error && !generated && !created && (flags&LINUX_O_TRUNC))
            error=kernel_vfs_open_truncate(&description->file);
        if (error) {
            KERNEL_FILES_NOTE(files,open_failures,1);
            kernel_files_queue_description(files,description);
            (void)kernel_files_drain_file_cleanup(files);
            *linux_result=error;
            return KERNEL_FILES_STATUS_OK;
        }
        description->kind = generated
            ? KERNEL_OPEN_FILE_KIND_GENERATED : KERNEL_OPEN_FILE_KIND_REGULAR;
    } else if ((kernel_open_file_mode(description) & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFIFO) {
        int error = kernel_pipe_fifo_open(files->heap, description->file.private_data, (uint32_t)flags,
                &description->pipe, &description->pipe_endpoint, &description->pipe_observed_writers);
        if (error) {
            KERNEL_FILES_NOTE(files, open_failures, 1);
            kernel_files_queue_description(files, description);
            (void)kernel_files_drain_file_cleanup(files);
            *linux_result = error;
            return KERNEL_FILES_STATUS_OK;
        }
        description->kind = KERNEL_OPEN_FILE_KIND_PIPE;
    } else if ((kernel_open_file_mode(description) & KERNEL_VFS_S_IFMT) ==
               KERNEL_VFS_S_IFCHR) {
        struct kernel_vfs_stat stat;
        int stat_result = kernel_vfs_fstat(&description->file, &stat);

        if (stat_result == 0) {
            description->device = kernel_char_device_lookup(stat.rdev);
            if (!description->device) stat_result = -KERNEL_ENXIO;
            else {
                description->kind = description->device->kind;
                stat_result = kernel_char_device_open(description, (uint32_t)flags);
            }
        }
        if (stat_result != 0) {
            KERNEL_FILES_NOTE(files, open_failures, 1);
            kernel_files_queue_description(files, description);
            (void)kernel_files_drain_file_cleanup(files);
            *linux_result = stat_result;
            return KERNEL_FILES_STATUS_OK;
        }
    } else if ((kernel_open_file_mode(description) & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFBLK) {
        struct kernel_vfs_stat stat;
        int error = (flags & LINUX_O_DIRECTORY) ? -KERNEL_ENOTDIR :
            kernel_vfs_fstat(&description->file, &stat);
        if (!error) error = kernel_block_file_open(description, stat.rdev, (uint32_t)flags);
        if (error) {
            KERNEL_FILES_NOTE(files, open_failures, 1);
            kernel_files_queue_description(files, description);
            (void)kernel_files_drain_file_cleanup(files);
            *linux_result = error;
            return KERNEL_FILES_STATUS_OK;
        }
    } else {
        KERNEL_FILES_NOTE(files, open_failures, 1);
        kernel_files_queue_description(files, description);
        (void)kernel_files_drain_file_cleanup(files);
        *linux_result = -KERNEL_ENOTSUP;
        return KERNEL_FILES_STATUS_OK;
    }

Finish_open:
    /* Linux treats the internal __O_SYNC bit as implying O_DSYNC. */
    if ((flags & (LINUX_O_SYNC & ~LINUX_O_DSYNC)) != 0U)
        flags |= LINUX_O_DSYNC;
    description->open_flags = (uint32_t)((flags & LINUX_O_PATH) ? flags & ~LINUX_O_CLOEXEC : flags);
    if (!(flags&LINUX_O_PATH)) {
        description->notify_opened=1;
        kernel_vfs_notify_open(&description->file,&description->notify_backing,VFS_NOTIFY_OPEN);
    }
    if (direct) {
        *direct = description;
        *linux_result = 0;
        return KERNEL_FILES_STATUS_OK;
    }
    uint32_t fd = reservation->fd;
    enum kernel_files_status files_status = kernel_files_install_new_owned_at(files,
                                                     fd,
                                                     fd_flags,
                                                     &description);
    if (files_status != KERNEL_FILES_STATUS_OK) {
        kernel_files_queue_description(files, description);
        (void)kernel_files_drain_file_cleanup(files);
        return files_status;
    }
    reservation->files=NULL;
    *linux_result = fd;
    return KERNEL_FILES_STATUS_OK;
}

static void set_open_access(struct kernel_vfs_open_request *request, uint64_t flags)
{
    if (!(flags & LINUX_O_PATH)) {
        request->access=(flags & 3U)==0 ? 4U : (flags & 3U)==1 ? 2U : 6U;
        if (flags & LINUX_O_TRUNC) request->access |= 2U;
        else if (flags & LINUX_O_APPEND) request->flags |= KERNEL_VFS_OPEN_APPEND;
    }
    if ((flags & LINUX_O_NOATIME) && !(flags & LINUX_O_PATH)) request->flags|=KERNEL_VFS_OPEN_NOATIME;
    if (flags & LINUX_O_DIRECTORY) request->flags |= KERNEL_VFS_OPEN_DIRECTORY;
    if (flags & LINUX_O_NOFOLLOW) request->flags &= ~KERNEL_VFS_OPEN_FOLLOW;
    if (flags & LINUX_O_PATH) request->flags |= KERNEL_VFS_OPEN_PATH_ONLY;
}

static enum kernel_files_status files_open(
    struct kernel_files *files,
    const struct kernel_fs_context *fs,
    struct kernel_mm *mm,
    int64_t dirfd,
    uint64_t user_path,
    uint64_t flags,
    uint64_t mode, uint64_t resolve, int strict,
    struct kernel_open_file_description **direct,
    int64_t *linux_result)
{
    struct kernel_task_io_buffer path_buffer __attribute__((cleanup(path_buffer_release)))={0};
    struct kernel_fs_paths fs_paths __attribute__((cleanup(kernel_fs_paths_release)))={0};
    struct kernel_open_file_description *description = 0;
    KERNEL_FILES_PATH_SCOPE(start_owner);
    struct kernel_vfs_path *start=NULL;
    char *path;
    struct kernel_files_fd_reservation reservation
        __attribute__((cleanup(kernel_files_cancel_reservation))) = {0};
    uint32_t fd_flags = 0U;
    int result;
    enum kernel_open_file_status open_status;
    enum kernel_fs_context_status fs_status;
    enum kernel_files_status files_status;

    struct kernel_vfs_open_result opened={0};
    KERNEL_FILES_PATH_SCOPE(pathless_link);
    int special_link = 0;

    if (!kernel_files_is_live(files) || !kernel_fs_context_is_live(fs) ||
        mm == 0 || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    KERNEL_FILES_NOTE(files, open_calls, 1);
    if (!strict && (flags & LINUX_O_PATH))
        flags &= LINUX_O_PATH | LINUX_O_DIRECTORY | LINUX_O_NOFOLLOW | LINUX_O_CLOEXEC;
    result = validate_open_flags(flags, mode, resolve, strict, &fd_flags);
    if (result != 0) {
        KERNEL_FILES_NOTE(files, open_failures, 1);
        *linux_result = result;
        return KERNEL_FILES_STATUS_OK;
    }
    if (!direct) {
        uint32_t fd;
        files_status = kernel_files_find_free_fd(files, &fd, &result);
        if (files_status != KERNEL_FILES_STATUS_OK)
            return files_status;
        if (result != 0) {
            KERNEL_FILES_NOTE(files, open_failures, 1);
            *linux_result = result;
            return KERNEL_FILES_STATUS_OK;
        }
        reservation = (struct kernel_files_fd_reservation){files, fd};
    } else if (fd_flags) {
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    enum kernel_task_status storage=kernel_task_io_buffer_acquire(&path_buffer,mm->allocator);
    if (storage == KERNEL_TASK_STATUS_RESOURCE_UNAVAILABLE) {
        KERNEL_FILES_NOTE(files, open_failures, 1);
        *linux_result = -KERNEL_ENOMEM;
        return KERNEL_FILES_STATUS_OK;
    }
    if (storage != KERNEL_TASK_STATUS_OK) {
        return KERNEL_FILES_STATUS_STATE;
    }
    path=path_buffer.data;
    fs_status = copy_path(mm,user_path,path,KERNEL_FS_PATH_MAX,&result);
    if (fs_status==KERNEL_FS_CONTEXT_STATUS_OK && !result) {
        if (path[0]=='/' && (resolve & KERNEL_VFS_RESOLVE_BENEATH)) result=-KERNEL_EXDEV;
        else {
            const char *selection=path;
            if (path[0]=='/' && (resolve & KERNEL_VFS_RESOLVE_IN_ROOT)) selection=".";
            fs_paths=kernel_fs_context_path_snapshot(fs,
                selection[0]!='/' && dirfd==KERNEL_FS_AT_FDCWD);
            result=borrow_lookup_start(files,&fs_paths,dirfd,selection,&start,&start_owner);
        }
        /* Normal lookup revalidates names through the backend even on a dentry
         * hit. There is no cache-only backend walk to fall back to. */
        if (!result && (resolve & KERNEL_VFS_RESOLVE_CACHED)) result=-KERNEL_EAGAIN;
    }
    if (fs_status != KERNEL_FS_CONTEXT_STATUS_OK) {
        return KERNEL_FILES_STATUS_STATE;
    }
    if (result != 0) {
        KERNEL_FILES_NOTE(files, open_failures, 1);
        *linux_result = result;
        return KERNEL_FILES_STATUS_OK;
    }
    struct kernel_vfs_path *root=(resolve & KERNEL_VFS_RESOLVE_SCOPED) ? start : fs_paths.root;
    struct kernel_vfs_open_request request={.flags=KERNEL_VFS_OPEN_FOLLOW,
        .mode=(uint32_t)mode,.umask=fs_paths.umask,.resolve=(unsigned)resolve};
    if (flags & LINUX_OPENAT2_REGULAR) {
        request.flags |= KERNEL_VFS_OPEN_REGULAR;
        flags &= ~LINUX_OPENAT2_REGULAR;
    }
    set_open_access(&request, flags);
    if (!(flags & LINUX_O_PATH) && (flags & (LINUX_O_TMPFILE & ~LINUX_O_DIRECTORY))) {
        request.flags |= KERNEL_VFS_OPEN_TMPFILE;
        if (flags & LINUX_O_EXCL) request.flags |= KERNEL_VFS_OPEN_EXCLUSIVE;
    } else if (flags & LINUX_O_CREAT) {
        request.flags |= KERNEL_VFS_OPEN_CREATE;
        if (flags & LINUX_O_EXCL) {
            request.flags |= KERNEL_VFS_OPEN_EXCLUSIVE;
            request.flags &= ~KERNEL_VFS_OPEN_FOLLOW;
        }
    }
    open_status=kernel_open_file_create_at(files->heap,start,root,path,
                        &request,&description,&opened,&result);
    pathless_link=opened.pathless_link;
    if (open_status==KERNEL_OPEN_FILE_STATUS_OK && result==-KERNEL_ENOENT && !resolve &&
        (request.flags & KERNEL_VFS_OPEN_FOLLOW) &&
        !(request.flags & (KERNEL_VFS_OPEN_EXCLUSIVE | KERNEL_VFS_OPEN_REGULAR)) && pathless_link) {
        int link_result=kernel_vfs_reopen_link(pathless_link,files->heap,
                                                (uint32_t)flags,&description);
        if (link_result!=-KERNEL_ENOTSUP) {
            result=link_result;special_link=!result;
        }
    }
    kernel_files_path_guard_release(&pathless_link);
    kernel_task_io_buffer_release(&path_buffer);
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
        KERNEL_FILES_NOTE(files, open_failures, 1);
        *linux_result = result;
        return KERNEL_FILES_STATUS_OK;
    }
    return finish_open(files, description, flags, &reservation, fd_flags,
        opened.created, special_link, direct, linux_result);
}

/* The Linux header precedes the filesystem-specific opaque payload. */
struct linux_file_handle_header { uint32_t bytes; int32_t type; };
#define LINUX_FILE_HANDLE_MAX 128U
#define LINUX_HANDLE_MNT_ID_UNIQUE 0x001U
#define LINUX_HANDLE_CONNECTABLE 0x002U
#define LINUX_HANDLE_FID 0x200U
#define LINUX_SYMLINK_FOLLOW 0x400U
#define LINUX_FILEID_INVALID 255

static int handle_copy(struct kernel_mm *mm, void *buffer, uint64_t user,
    size_t size, int to_user)
{
    size_t copied = 0;
    enum kernel_uaccess_status status = to_user ?
        kernel_copy_to_user(mm, user, buffer, size, &copied) :
        kernel_copy_from_user(mm, buffer, user, size, &copied);
    return status == KERNEL_UACCESS_STATUS_OK && copied == size ? 0 : -KERNEL_EFAULT;
}

int kernel_files_name_to_handle(struct kernel_files *files, const struct kernel_fs_context *fs,
    struct kernel_mm *mm, int32_t dirfd, uint64_t user_path, uint64_t user_handle,
    uint64_t user_mount, uint32_t flags)
{
    const unsigned known = LINUX_HANDLE_MNT_ID_UNIQUE | LINUX_HANDLE_CONNECTABLE |
        LINUX_HANDLE_FID | LINUX_SYMLINK_FOLLOW | KERNEL_FILES_AT_EMPTY_PATH;
    if ((flags & ~known) || ((flags & LINUX_HANDLE_CONNECTABLE) &&
        (flags & (LINUX_HANDLE_FID | KERNEL_FILES_AT_EMPTY_PATH)))) return -KERNEL_EINVAL;
    struct kernel_task_io_buffer path_buffer __attribute__((cleanup(path_buffer_release))) = {0};
    if (kernel_task_io_buffer_acquire(&path_buffer, mm->allocator)) return -KERNEL_ENOMEM;
    char *path = path_buffer.data;
    int result = 0;
    if (!user_path && (flags & KERNEL_FILES_AT_EMPTY_PATH)) path[0] = 0;
    else if (copy_path(mm, user_path, path, KERNEL_FS_PATH_MAX, &result)) return -KERNEL_EFAULT;
    if (result) return result;

    struct kernel_fs_paths paths __attribute__((cleanup(kernel_fs_paths_release))) =
        kernel_fs_context_path_snapshot(fs, dirfd == KERNEL_FS_AT_FDCWD && path[0] != '/');
    KERNEL_FILES_PATH_SCOPE(resolved);
    if (!path[0]) {
        if (!(flags & KERNEL_FILES_AT_EMPTY_PATH)) return -KERNEL_ENOENT;
        KERNEL_FILES_PIN_SCOPE(pin);
        struct kernel_vfs_path *target = paths.cwd;
        if (dirfd != KERNEL_FS_AT_FDCWD) {
            struct kernel_open_file_description *file = kernel_files_hold_fd(files, dirfd, &pin);
            if (!file) return -KERNEL_EBADF;
            target = kernel_open_file_path(file);
        }
        if (!target) return -KERNEL_EOPNOTSUPP;
        result = kernel_vfs_path_acquire(target);
        if (result) return result;
        resolved = target;
    } else {
        KERNEL_FILES_PATH_SCOPE(start_owner);
        struct kernel_vfs_path *start = NULL;
        result = borrow_lookup_start(files, &paths, dirfd, path, &start, &start_owner);
        if (!result) result = kernel_vfs_path_resolve(start, paths.root, path,
            !!(flags & LINUX_SYMLINK_FOLLOW), &resolved);
        if (result) return result;
    }
    kernel_task_io_buffer_release(&path_buffer);
    int type = kernel_vfs_handle_type(resolved,flags & LINUX_HANDLE_FID);
    if (type < 0) return type;
    if (flags & LINUX_HANDLE_CONNECTABLE) return -KERNEL_EOPNOTSUPP;
    struct linux_file_handle_header input;
    if (handle_copy(mm, &input, user_handle, sizeof(input), 0)) return -KERNEL_EFAULT;
    if (input.bytes > LINUX_FILE_HANDLE_MAX) return -KERNEL_EINVAL;
    struct {
        struct linux_file_handle_header header;
        struct kernel_vfs_handle handle;
    } output;
    _Static_assert(offsetof(typeof(output), handle) == 8, "native file handle payload");
    uint64_t mount = kernel_vfs_path_mount(resolved)->id;
    int unique = !!(flags & LINUX_HANDLE_MNT_ID_UNIQUE);
    if (unique) mount=kernel_vfs_unique_mount_id(mount);
    size_t mount_size = unique ? sizeof(mount) : sizeof(uint32_t);
    if (handle_copy(mm, &mount, user_mount, mount_size, 1)) return -KERNEL_EFAULT;
    size_t bytes = sizeof(output.header);
    output.header.bytes = sizeof(output.handle);
    if (input.bytes < sizeof(output.handle)) {
        output.header.type = LINUX_FILEID_INVALID;
        result = -KERNEL_EOVERFLOW;
    } else {
        output.header.type = type;
        kernel_vfs_encode_handle(resolved, &output.handle);
        bytes = sizeof(output);
    }
    return handle_copy(mm, &output, user_handle, bytes, 1) ? -KERNEL_EFAULT : result;
}

int kernel_files_open_by_handle(struct kernel_files *files, const struct kernel_fs_context *fs,
    struct kernel_mm *mm, int32_t mountfd, uint64_t user_handle, uint32_t open_flags)
{
    struct linux_file_handle_header header;
    if (handle_copy(mm, &header, user_handle, sizeof(header), 0)) return -KERNEL_EFAULT;
    if (!header.bytes || header.bytes > LINUX_FILE_HANDLE_MAX || header.type < 0 ||
        ((uint32_t)header.type & ~UINT32_C(0x3ffff))) return -KERNEL_EINVAL;
    struct kernel_fs_paths paths __attribute__((cleanup(kernel_fs_paths_release))) = {0};
    KERNEL_FILES_PIN_SCOPE(pin);
    struct kernel_vfs_mount *mount;
    if (mountfd == KERNEL_FS_AT_FDCWD) {
        paths = kernel_fs_context_path_snapshot(fs, true);
        mount = kernel_vfs_path_mount(paths.cwd);
    } else {
        struct kernel_open_file_description *file = kernel_files_hold_fd(files, mountfd, &pin);
        if (!file || file->kind == KERNEL_OPEN_FILE_KIND_PATH) return -KERNEL_EBADF;
        struct kernel_vfs_path *anchor = kernel_open_file_path(file);
        mount = anchor ? kernel_vfs_path_mount(anchor) : NULL;
    }
    if (!kernel_credentials_capable(kernel_credentials_current(), KERNEL_CAP_DAC_READ_SEARCH))
        return -KERNEL_EPERM;
    union { struct kernel_vfs_handle handle; unsigned char bytes[LINUX_FILE_HANDLE_MAX]; } input;
    if (handle_copy(mm, &input, user_handle+sizeof(header), header.bytes, 0)) return -KERNEL_EFAULT;
    if (header.bytes != sizeof(input.handle)) return -KERNEL_ESTALE;
    KERNEL_FILES_PATH_SCOPE(resolved);
    int result = kernel_vfs_decode_handle(mount, header.type, &input.handle, &resolved);
    if (result) return result;

    uint64_t flags = (open_flags & LINUX_OPEN_SUPPORTED_FLAGS) | LINUX_O_LARGEFILE;
    if (flags & LINUX_O_PATH)
        flags &= LINUX_O_PATH | LINUX_O_DIRECTORY | LINUX_O_NOFOLLOW | LINUX_O_CLOEXEC;
    uint32_t fd, fd_flags = 0;
    result = validate_open_flags(flags, 0, 0, 0, &fd_flags);
    if (result) return result;
    if (kernel_files_find_free_fd(files, &fd, &result) != KERNEL_FILES_STATUS_OK) return -KERNEL_EIO;
    if (result) return result;
    struct kernel_files_fd_reservation reservation
        __attribute__((cleanup(kernel_files_cancel_reservation))) = {files, fd};
    struct kernel_vfs_open_request request = {0};
    set_open_access(&request, flags);
    const char *relative = NULL;
    int temporary = (flags & LINUX_O_TMPFILE) == LINUX_O_TMPFILE;
    if (temporary) {
        if ((kernel_vfs_path_mode(resolved) & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR)
            return -KERNEL_ENOTDIR;
        request.flags |= KERNEL_VFS_OPEN_TMPFILE;
        if (flags & LINUX_O_EXCL) request.flags |= KERNEL_VFS_OPEN_EXCLUSIVE;
        relative = ".";
    } else if (flags & LINUX_O_CREAT) {
        if (flags & LINUX_O_EXCL) return -KERNEL_EEXIST;
        request.flags |= KERNEL_VFS_OPEN_CREATE;
    }
    struct kernel_open_file_description *description = NULL;
    enum kernel_open_file_status opened = kernel_open_file_create_at(files->heap,
        resolved, resolved, relative, &request, &description, NULL, &result);
    if (opened != KERNEL_OPEN_FILE_STATUS_OK) return -KERNEL_EIO;
    if (result) return result;
    int64_t fd_result;
    return finish_open(files, description, flags, &reservation, fd_flags,
        temporary, 0, NULL, &fd_result) == KERNEL_FILES_STATUS_OK ? (int)fd_result : -KERNEL_EIO;
}

enum kernel_files_status kernel_files_openat(
    struct kernel_files *files, const struct kernel_fs_context *fs,
    struct kernel_mm *mm, int64_t dirfd, uint64_t user_path, uint64_t flags,
    uint64_t mode, int64_t *linux_result)
{
    return files_open(files,fs,mm,dirfd,user_path,flags,mode,0,0,NULL,linux_result);
}

enum kernel_files_status kernel_files_openat2(
    struct kernel_files *files, const struct kernel_fs_context *fs,
    struct kernel_mm *mm, int32_t dirfd, uint64_t user_path, uint64_t flags,
    uint64_t mode, uint64_t resolve, int64_t *linux_result)
{
    return files_open(files,fs,mm,dirfd,user_path,flags,mode,resolve,1,NULL,linux_result);
}

enum kernel_files_status kernel_files_uring_open(struct kernel_files *files,
    const struct kernel_fs_context *fs, struct kernel_mm *mm, int32_t dirfd,
    uint64_t user_path, const uint64_t how[3], int strict,
    struct kernel_open_file_description **direct, int64_t *result)
{
    return files_open(files, fs, mm, dirfd, user_path, how[0], how[1], how[2], strict,
        direct, result);
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
    KERNEL_FS_PATHS(fs_paths, fs);
    struct kernel_vfs_mount *mount;
    KERNEL_FILES_PATH_SCOPE(start);
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
        kernel_files_release_allocation(files, storage);
        if (result == 0) return KERNEL_FILES_STATUS_STATE;
        *linux_result = result;
        return KERNEL_FILES_STATUS_OK;
    }
    fs_status = copy_path_start(files, &fs_paths, mm, dirfd,
                                                    user_linkpath, linkpath,
                                                    KERNEL_FS_PATH_MAX,
                                                    &start, &mount, &result);
    if (fs_status != KERNEL_FS_CONTEXT_STATUS_OK) {
        kernel_files_release_allocation(files, storage);
        return KERNEL_FILES_STATUS_STATE;
    }
    if (result == 0) {
        result = target_length == 0U ? -KERNEL_ENOENT :
            kernel_vfs_symlink_at(start, fs_paths.root, storage, linkpath);
    }
    kernel_files_release_allocation(files, storage);
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
    struct kernel_fs_paths fs_paths __attribute__((cleanup(kernel_fs_paths_release))) = {0};
    KERNEL_FILES_PATH_SCOPE(start);
    struct kernel_vfs_path *lookup_start = NULL;
    struct kernel_task_io_buffer scratch __attribute__((cleanup(path_buffer_release))) = {0};
    size_t bytes_read = 0U;
    size_t copied = 0U;
    int result;
    enum kernel_fs_context_status fs_status;
    enum kernel_uaccess_status access_status;

    if (!kernel_files_is_live(files) || !kernel_fs_context_is_live(fs) ||
        mm == 0 || linux_result == 0) return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    if (size == 0U) {
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    enum kernel_task_status allocated = kernel_task_io_buffer_acquire(&scratch, mm->allocator);
    if (allocated == KERNEL_TASK_STATUS_RESOURCE_UNAVAILABLE) {
        *linux_result = -KERNEL_ENOMEM;
        return KERNEL_FILES_STATUS_OK;
    }
    if (allocated != KERNEL_TASK_STATUS_OK) return KERNEL_FILES_STATUS_STATE;
    /* The resolver consumes the entire pathname before the link target is
     * read. Both fit in the same task-owned page. */
    _Static_assert(KERNEL_FS_PATH_MAX <= BOAROS_PAGE_SIZE, "pathname fits the I/O page");
    char *buffer = scratch.data;
    size_t length;
    access_status = kernel_copy_string_from_user(mm, buffer, user_path,
                                  KERNEL_FS_PATH_MAX, &length);
    fs_status = KERNEL_FS_CONTEXT_STATUS_OK;
    if (access_status == KERNEL_UACCESS_STATUS_FAULT) result = -KERNEL_EFAULT;
    else if (access_status == KERNEL_UACCESS_STATUS_TOO_LONG) result = -KERNEL_ENAMETOOLONG;
    else if (access_status != KERNEL_UACCESS_STATUS_OK) fs_status = KERNEL_FS_CONTEXT_STATUS_STATE;
    else if (!length) {
        KERNEL_FILES_PIN_SCOPE(selected_fd);
        if (dirfd == KERNEL_FS_AT_FDCWD) {
            fs_paths = kernel_fs_context_snapshot(fs);
            lookup_start = fs_paths.cwd;
            result = lookup_start ? 0 : -KERNEL_EBADF;
        } else {
            struct kernel_open_file_description *file = kernel_files_hold_fd(files,dirfd,&selected_fd);
            struct kernel_vfs_path *path = file ? file->file.path : NULL;
            result = path ? kernel_vfs_path_acquire(path) : file ? -KERNEL_EINVAL : -KERNEL_EBADF;
            if (!result) lookup_start = start = path;
        }
    } else {
        fs_paths = kernel_fs_context_path_snapshot(fs, dirfd == KERNEL_FS_AT_FDCWD && buffer[0] != '/');
        result = borrow_lookup_start(files, &fs_paths, dirfd, buffer, &lookup_start, &start);
    }
    if (fs_status != KERNEL_FS_CONTEXT_STATUS_OK) {
        return KERNEL_FILES_STATUS_STATE;
    }
    if (result == 0) {
        size_t capacity = size < KERNEL_FS_PATH_MAX ? (size_t)size :
                          KERNEL_FS_PATH_MAX;
        result = buffer[0] ?
            kernel_vfs_readlink_at(lookup_start, fs_paths.root, buffer, buffer, capacity, &bytes_read) :
            kernel_vfs_path_readlink(lookup_start, buffer, capacity, &bytes_read);
    }
    if (result == 0) {
        access_status = kernel_copy_to_user(mm, user_buffer, buffer,
                                            bytes_read, &copied);
        if (access_status == KERNEL_UACCESS_STATUS_FAULT)
            result = -KERNEL_EFAULT;
        else if (access_status != KERNEL_UACCESS_STATUS_OK ||
                 copied != bytes_read) {
            return KERNEL_FILES_STATUS_STATE;
        }
    }
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
    stat->st_pad1 = 0;
    stat->st_pad2 = 0;
    stat->st_pad4 = 0;
    stat->st_pad5 = 0;
}

static int stat_description(
    struct kernel_vfs_stat *stat,
    const struct kernel_open_file_description *description)
{
    KERNEL_SERVICE_SCOPE_IF(service,kernel_vfs_namespace_requires_cpu0(description->file.mount) ||
        (!description->file.mount && kernel_open_file_kind(description)!=KERNEL_OPEN_FILE_KIND_PATH));

    int result = kernel_open_file_pseudo_stat(description, stat);
    if (result == -KERNEL_ENOTSUP)
        result = description->kind == KERNEL_OPEN_FILE_KIND_PATH ?
            kernel_vfs_path_stat(description->file.path, stat) :
            kernel_vfs_fstat(&description->file, stat);
    return result;
}

struct linux_statx_timestamp { int64_t seconds; uint32_t nanoseconds; int32_t reserved; };
struct linux_statx {
    uint32_t mask,blksize; uint64_t attributes;
    uint32_t nlink,uid,gid; uint16_t mode,pad;
    uint64_t ino,size,blocks,attributes_mask;
    struct linux_statx_timestamp atime,btime,ctime,mtime;
    uint32_t rdev_major,rdev_minor,dev_major,dev_minor;
    uint64_t mount_id;
    uint32_t dio_mem_align,dio_offset_align;
    uint64_t spare[12];
};
_Static_assert(sizeof(struct linux_statx)==256,"Linux statx ABI size");
_Static_assert(offsetof(struct linux_statx,dio_mem_align)==0x98,"Linux statx alignment offset");

static uint32_t device_major(uint64_t device)
{ return ((device>>8)&0xfff)|((device>>32)&0xfffff000); }
static uint32_t device_minor(uint64_t device)
{ return (device&0xff)|((device>>12)&0xffffff00); }

static uint32_t statx_dio_alignment(uint64_t number,const struct kernel_block_device *open_device)
{
    if (open_device) return kernel_block_user_dio_alignment(kernel_block_logical_size(open_device));
    struct kernel_block_device *device=kernel_block_get(number);
    if (!device) return 0;
    uint32_t alignment=kernel_block_user_dio_alignment(kernel_block_logical_size(device));
    kernel_block_put(device);
    return alignment;
}

static int copy_stat_to_user(struct kernel_mm *mm,
                             uint64_t user_buffer,
                             const struct kernel_vfs_stat *source,
                        int64_t *linux_result, const struct kernel_vfs_statx_extra *extra,
                             const struct kernel_block_device *open_device)
{
    union {struct linux_statx extended;struct kernel_linux_stat basic;} output;
    const void *bytes;size_t size;
    if (extra) {
        struct linux_statx *stat=&output.extended;
        stat->attributes=source->attributes;stat->attributes_mask=source->attributes_mask;
        stat->mask=0x7ff;stat->blksize=source->blksize;
        stat->nlink=source->nlink;stat->uid=source->uid;stat->gid=source->gid;
        stat->mode=source->mode;stat->ino=source->ino;stat->size=source->size;stat->blocks=source->blocks;
        stat->atime=(struct linux_statx_timestamp){source->atime.seconds,source->atime.nanoseconds,0};
        stat->btime=(struct linux_statx_timestamp){0};
        stat->mtime=(struct linux_statx_timestamp){source->mtime.seconds,source->mtime.nanoseconds,0};
        stat->ctime=(struct linux_statx_timestamp){source->ctime.seconds,source->ctime.nanoseconds,0};
        stat->dev_major=device_major(source->dev);stat->dev_minor=device_minor(source->dev);
        stat->rdev_major=device_major(source->rdev);stat->rdev_minor=device_minor(source->rdev);
        stat->pad=0;
        stat->mount_id=extra->mount_id;
        if (extra->mount_id) {
            stat->mask |= extra->request & KERNEL_VFS_STATX_MNT_ID_UNIQUE ?
                KERNEL_VFS_STATX_MNT_ID_UNIQUE : KERNEL_VFS_STATX_MNT_ID;
        }
        uint32_t alignment=extra->dio_alignment;
        if ((extra->request&KERNEL_VFS_STATX_DIOALIGN) &&
            (source->mode&KERNEL_VFS_S_IFMT)==KERNEL_VFS_S_IFBLK)
            alignment=statx_dio_alignment(source->rdev,open_device);
        if (alignment) stat->mask|=KERNEL_VFS_STATX_DIOALIGN;
        stat->dio_mem_align=stat->dio_offset_align=alignment;
        memset(stat->spare,0,sizeof(stat->spare));
        bytes=stat;size=sizeof(*stat);
    } else {
        fill_linux_vfs_stat(&output.basic,source);
        bytes=&output.basic;size=sizeof(output.basic);
    }
    size_t copied = 0U;
    enum kernel_uaccess_status access_status =
        kernel_copy_to_user(mm,
                            user_buffer,
                            bytes,
                            size,
                            &copied);

    if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
        *linux_result = -KERNEL_EFAULT;
        return 0;
    }
    return access_status == KERNEL_UACCESS_STATUS_OK &&
                   copied == size
               ? 1
               : -1;
}

static enum kernel_files_status stat_pinned(struct kernel_open_file_description *description,
    struct kernel_mm *mm,uint64_t user_buffer,int64_t *linux_result)
{
    struct kernel_vfs_stat stat={0};
    int copy_result;
    int result;

    result = stat_description(&stat, description);
    if (result != 0) {
        *linux_result = result;
        return KERNEL_FILES_STATUS_OK;
    }
    copy_result = copy_stat_to_user(mm, user_buffer, &stat, linux_result, NULL, NULL);
    if (copy_result < 0) {
        return KERNEL_FILES_STATUS_STATE;
    }
    if (copy_result > 0) {
        *linux_result = 0;
    }
    return KERNEL_FILES_STATUS_OK;
}
enum kernel_files_status kernel_files_fstat(struct kernel_files *files,struct kernel_mm *mm,
    int64_t fd,uint64_t user_buffer,int64_t *linux_result)
{
    if (!kernel_files_is_live(files) || !mm || !linux_result) return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    KERNEL_FILES_PIN_SCOPE(pin_guard);
    struct kernel_open_file_description *description=kernel_files_hold_fd(files,fd,&pin_guard);
    if (!description) { *linux_result=-KERNEL_EBADF; return KERNEL_FILES_STATUS_OK; }
    return stat_pinned(description,mm,user_buffer,linux_result);
}

static enum kernel_files_status stat_at_to_user(
    struct kernel_files *files,
    const struct kernel_fs_context *fs,
    struct kernel_mm *mm,
    int64_t dirfd,
    uint64_t user_path,
    uint64_t user_buffer,
    uint64_t flags,
    int64_t *linux_result, int extended, uint32_t mask)
{
    COST_SCOPE(query_cost, FILE_QUERY_TICKS);
    struct kernel_task_io_buffer path_buffer __attribute__((cleanup(path_buffer_release)))={0};
    KERNEL_FILES_PIN_SCOPE(pin_guard);
    struct kernel_vfs_stat stat={0};
    struct kernel_vfs_statx_extra extra={.request=mask};
    KERNEL_FILES_PATH_SCOPE(start_owner);
    struct kernel_vfs_path *start=NULL;
    char *path = 0;
    size_t length = 0;
    int result;
    if (!kernel_files_is_live(files) || !kernel_fs_context_is_live(fs) ||
        !mm || !linux_result) return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    /* 本地后端没有自动挂载或远端属性缓存；这些查询位不增加数据访问资格。 */
    const uint64_t valid_flags = KERNEL_FILES_AT_SYMLINK_NOFOLLOW |
        KERNEL_FILES_AT_EMPTY_PATH | UINT64_C(0x800) | UINT64_C(0x6000);
    int invalid_flags = (flags & ~valid_flags) != 0;
    int empty = 0;
    if (flags & KERNEL_FILES_AT_EMPTY_PATH) {
        if (!user_path) empty = 1;
        else {
            unsigned char first;
            size_t copied;
            enum kernel_uaccess_status access = kernel_copy_from_user(mm, &first,
                                                   user_path, 1, &copied);
            if (access == KERNEL_UACCESS_STATUS_FAULT) {
                *linux_result = invalid_flags ? -KERNEL_EINVAL : -KERNEL_EFAULT;
                return KERNEL_FILES_STATUS_OK;
            }
            if (access != KERNEL_UACCESS_STATUS_OK || copied != 1)
                return KERNEL_FILES_STATUS_STATE;
            empty = first == 0;
        }
        /* Linux 的空路径正 fd 直接进入 fstat，连其他查询位也不再参与校验。 */
        if (empty && dirfd >= 0) {
            if(!extended) return kernel_files_fstat(files, mm, dirfd, user_buffer, linux_result);
            struct kernel_open_file_description *file=kernel_files_hold_fd(files,dirfd,&pin_guard);
            result=file ? stat_description(&stat,file) : -KERNEL_EBADF;
            if(result) { *linux_result=result; return KERNEL_FILES_STATUS_OK; }
            kernel_vfs_statx_extra(&file->file, NULL, &stat, &extra);
            const struct kernel_block_device *device=file->kind==KERNEL_OPEN_FILE_KIND_BLOCK ? file->block : NULL;
            int copied=copy_stat_to_user(mm,user_buffer,&stat,linux_result,&extra,device);
            if(copied<0) return KERNEL_FILES_STATUS_STATE;
            if(copied>0) *linux_result=0;
            return KERNEL_FILES_STATUS_OK;
        }
    }
    if (invalid_flags) {
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    if (!empty) {
        enum kernel_task_status allocation=kernel_task_io_buffer_acquire(&path_buffer,mm->allocator);
        if (allocation != KERNEL_TASK_STATUS_OK) {
            if (allocation != KERNEL_TASK_STATUS_RESOURCE_UNAVAILABLE) return KERNEL_FILES_STATUS_STATE;
            *linux_result = -KERNEL_ENOMEM;
            return KERNEL_FILES_STATUS_OK;
        }
        path=path_buffer.data;
        enum kernel_uaccess_status access = kernel_copy_string_from_user(mm, path,
                                     user_path, KERNEL_FS_PATH_MAX, &length);
        if (access != KERNEL_UACCESS_STATUS_OK) {
            if (access != KERNEL_UACCESS_STATUS_FAULT && access != KERNEL_UACCESS_STATUS_TOO_LONG)
                return KERNEL_FILES_STATUS_STATE;
            *linux_result = access == KERNEL_UACCESS_STATUS_FAULT ? -KERNEL_EFAULT
                                                                  : -KERNEL_ENAMETOOLONG;
            return KERNEL_FILES_STATUS_OK;
        }
        empty = !length && (flags & KERNEL_FILES_AT_EMPTY_PATH);
    }
    /* Absolute paths and relative dirfd walks still need the root boundary
     * for symlinks and dot-dot, but never use the working directory. */
    struct kernel_fs_paths fs_paths __attribute__((cleanup(kernel_fs_paths_release))) =
        kernel_fs_context_path_snapshot(fs,dirfd==KERNEL_FS_AT_FDCWD &&
                                       (empty || path[0]!='/'));
    if (empty) {
        if (dirfd == KERNEL_FS_AT_FDCWD) {
            result = kernel_vfs_path_stat(fs_paths.cwd, &stat);
            if (!result && extended) kernel_vfs_statx_extra(NULL, fs_paths.cwd, &stat, &extra);
        } else {
            struct kernel_open_file_description *file =
                kernel_files_hold_fd(files, dirfd, &pin_guard);
            result = file ? stat_description(&stat, file) : -KERNEL_EBADF;
            if (!result && extended) kernel_vfs_statx_extra(&file->file, NULL, &stat, &extra);
        }
    } else {
        result=borrow_lookup_start(files,&fs_paths,dirfd,path,&start,&start_owner);
        int follow=!(flags & KERNEL_FILES_AT_SYMLINK_NOFOLLOW);
        if (!result) result = extended ?
            kernel_vfs_statx_at(start, fs_paths.root, path, follow, &stat, &extra) :
            kernel_vfs_stat_at(start, fs_paths.root, path, follow, &stat);
    }
    path_buffer_release(&path_buffer);
    if (result) { *linux_result = result; return KERNEL_FILES_STATUS_OK; }
    int copied = copy_stat_to_user(mm, user_buffer, &stat, linux_result, extended ? &extra : NULL, NULL);
    if (copied < 0) return KERNEL_FILES_STATUS_STATE;
    if (copied > 0) *linux_result = 0;
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_fstatat(struct kernel_files *files,
    const struct kernel_fs_context *fs,struct kernel_mm *mm,int64_t dirfd,
    uint64_t user_path,uint64_t user_buffer,uint64_t flags,int64_t *result)
{
    return stat_at_to_user(files,fs,mm,dirfd,user_path,user_buffer,flags,result,0,0);
}

enum kernel_files_status kernel_files_statx(struct kernel_files *files,
    const struct kernel_fs_context *fs,struct kernel_mm *mm,int64_t dirfd,
    uint64_t user_path,uint64_t flags,uint32_t mask,uint64_t user_buffer,int64_t *result)
{
    if(!kernel_files_is_live(files) || !kernel_fs_context_is_live(fs) || !mm || !result)
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    if((mask&0x80000000U) || (flags&0x6000)==0x6000) {
        *result=-KERNEL_EINVAL;return KERNEL_FILES_STATUS_OK;
    }
    return stat_at_to_user(files,fs,mm,dirfd,user_path,user_buffer,flags,result,1,mask);
}

static void restore_access_credentials(const struct kernel_credentials **previous)
{ (void)kernel_credentials_override(*previous); }

enum kernel_files_status kernel_files_faccessat(
    struct kernel_files *files, const struct kernel_fs_context *fs,
    struct kernel_mm *mm, int64_t dirfd, uint64_t user_path,
    uint64_t mode, uint64_t flags, int64_t *linux_result)
{
    KERNEL_FS_PATHS(fs_paths, fs);
    KERNEL_FILES_PATH_SCOPE(start);
    KERNEL_FILES_PIN_SCOPE(pin_guard);
    char *path;
    size_t length;
    int result;
    if (!kernel_files_is_live(files) || !kernel_fs_context_is_live(fs) ||
        mm == 0 || linux_result == 0)
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    if ((mode & ~UINT64_C(7)) != 0U ||
        (flags & ~(KERNEL_FILES_AT_EACCESS | KERNEL_FILES_AT_SYMLINK_NOFOLLOW |
                   KERNEL_FILES_AT_EMPTY_PATH))) {
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    const struct kernel_credentials *current=kernel_credentials_current();
    struct kernel_credentials actor=*current;
    if (!(flags & KERNEL_FILES_AT_EACCESS)) {
        actor.fsuid=actor.uid; actor.fsgid=actor.gid;
        if (!(actor.securebits & KERNEL_SECBIT_NO_SETUID_FIXUP))
            actor.effective=actor.uid==0 ? actor.permitted : 0;
    }
    /* proc permission checks and every searched directory see the same actor. */
    const struct kernel_credentials *previous
        __attribute__((cleanup(restore_access_credentials)))=
            kernel_credentials_override(&actor);
    enum kernel_heap_status allocation = kernel_heap_allocate(
        files->heap, KERNEL_FS_PATH_MAX, (void **)&path);
    if (allocation != KERNEL_HEAP_STATUS_OK) {
        if (allocation != KERNEL_HEAP_STATUS_EMPTY)
            return KERNEL_FILES_STATUS_STATE;
        *linux_result = -KERNEL_ENOMEM;
        return KERNEL_FILES_STATUS_OK;
    }
    enum kernel_uaccess_status access = kernel_copy_string_from_user(
        mm, path, user_path, KERNEL_FS_PATH_MAX, &length);
    if (access != KERNEL_UACCESS_STATUS_OK) {
        kernel_files_release_allocation(files, path);
        if (access != KERNEL_UACCESS_STATUS_FAULT &&
            access != KERNEL_UACCESS_STATUS_TOO_LONG)
            return KERNEL_FILES_STATUS_STATE;
        *linux_result = access == KERNEL_UACCESS_STATUS_FAULT
            ? -KERNEL_EFAULT : -KERNEL_ENAMETOOLONG;
        return KERNEL_FILES_STATUS_OK;
    }
    if (!length && (flags & KERNEL_FILES_AT_EMPTY_PATH)) {
        if (dirfd == KERNEL_FS_AT_FDCWD) {
            result=kernel_vfs_access_at(fs_paths.cwd,fs_paths.root,".",(unsigned)mode,1);
        } else {
            struct kernel_open_file_description *file=
                kernel_files_hold_fd(files,dirfd,&pin_guard);
            struct kernel_vfs_stat stat;
            result=file ? stat_description(&stat,file) : -KERNEL_EBADF;
            if (!result) result=kernel_vfs_file_access_check(&file->file,&stat,(unsigned)mode);
        }
    } else {
        result = kernel_files_path_start(files, &fs_paths, dirfd, path, &start);
        if (!result) result = kernel_vfs_access_at(start,
            fs_paths.root, path, (unsigned)mode,
            !(flags & KERNEL_FILES_AT_SYMLINK_NOFOLLOW));
    }
    kernel_files_release_allocation(files, path);
    *linux_result = result;
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
    KERNEL_FS_PATHS(fs_paths, fs);
    struct kernel_vfs_mount *mount;
    KERNEL_FILES_PATH_SCOPE(start);
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
    fs_status = copy_path_start(files, &fs_paths,
                                                    mm,
                                                    dirfd,
                                                    user_path,
                                                    path,
                                                    KERNEL_FS_PATH_MAX,
                                                    &start, &mount,
                                                    &result);
    if (fs_status != KERNEL_FS_CONTEXT_STATUS_OK) {
        kernel_files_release_allocation(files, path);
        return KERNEL_FILES_STATUS_STATE;
    }
    if (result != 0) {
        kernel_files_release_allocation(files, path);
        *linux_result = result;
        return KERNEL_FILES_STATUS_OK;
    }
    result = kernel_vfs_mkdir_at(start,fs_paths.root,path,mode,kernel_fs_context_umask(fs));
    kernel_files_release_allocation(files, path);
    *linux_result = result;
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_mknodat(
    struct kernel_files *files,
    const struct kernel_fs_context *fs,
    struct kernel_mm *mm,
    int64_t dirfd,
    uint64_t user_path,
    uint32_t mode,
    uint32_t device,
    int64_t *linux_result)
{
    KERNEL_FS_PATHS(fs_paths, fs);
    struct kernel_vfs_mount *mount;
    KERNEL_FILES_PATH_SCOPE(start);
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
    fs_status = copy_path_start(files, &fs_paths,
                                                    mm,
                                                    dirfd,
                                                    user_path,
                                                    path,
                                                    KERNEL_FS_PATH_MAX,
                                                    &start, &mount,
                                                    &result);
    if (fs_status != KERNEL_FS_CONTEXT_STATUS_OK) {
        kernel_files_release_allocation(files, path);
        return KERNEL_FILES_STATUS_STATE;
    }
    if (result != 0) {
        kernel_files_release_allocation(files, path);
        *linux_result = result;
        return KERNEL_FILES_STATUS_OK;
    }
    result = kernel_vfs_mknod_at(start,fs_paths.root,path,mode,device,kernel_fs_context_umask(fs));
    kernel_files_release_allocation(files, path);
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
    KERNEL_FS_PATHS(fs_paths, fs);
    struct kernel_vfs_mount *mount;
    KERNEL_FILES_PATH_SCOPE(start);
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
    fs_status = copy_path_start(files, &fs_paths,
                                                    mm,
                                                    dirfd,
                                                    user_path,
                                                    path,
                                                    KERNEL_FS_PATH_MAX,
                                                    &start, &mount,
                                                    &result);
    if (fs_status != KERNEL_FS_CONTEXT_STATUS_OK) {
        kernel_files_release_allocation(files, path);
        return KERNEL_FILES_STATUS_STATE;
    }
    if (result != 0) {
        kernel_files_release_allocation(files, path);
        *linux_result = result;
        return KERNEL_FILES_STATUS_OK;
    }
    if ((flags & KERNEL_FILES_AT_REMOVEDIR) != 0U) {
        result = kernel_vfs_unlink_at(start, fs_paths.root, path, 1);
    } else {
        result = kernel_vfs_unlink_at(start, fs_paths.root, path, 0);
    }
    kernel_files_release_allocation(files, path);
    *linux_result = result;
    return KERNEL_FILES_STATUS_OK;
}

static enum kernel_files_status change_directory_path(
    struct kernel_files *files, const struct kernel_fs_context *fs,
    struct kernel_mm *mm, uint64_t user_path, int64_t *linux_result, int root)
{
    KERNEL_FS_PATHS(fs_paths, fs);
    char *path;
    KERNEL_FILES_PATH_SCOPE(start);
    struct kernel_vfs_path *resolved = 0;
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
    enum kernel_fs_context_status status = copy_path_start(files, &fs_paths, mm,
        KERNEL_FS_AT_FDCWD, user_path, path, KERNEL_FS_PATH_MAX, &start, &mount, &result);
    if (status != KERNEL_FS_CONTEXT_STATUS_OK) {
        kernel_files_release_allocation(files, path);
        return KERNEL_FILES_STATUS_STATE;
    }
    if (!result) result = kernel_vfs_path_resolve(start,
                            fs_paths.root, path, 1, &resolved);
    if (!result) result = root ? kernel_fs_context_set_root(fs, resolved)
                              : kernel_fs_context_set_cwd(fs, resolved);
    if (resolved) (void)kernel_vfs_path_release(&resolved);
    kernel_files_release_allocation(files, path);
    *linux_result = result;
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_chdir(struct kernel_files *files,
    const struct kernel_fs_context *fs, struct kernel_mm *mm,
    uint64_t user_path, int64_t *linux_result)
{
    return change_directory_path(files, fs, mm, user_path, linux_result, 0);
}

enum kernel_files_status kernel_files_chroot(struct kernel_files *files,
    const struct kernel_fs_context *fs, struct kernel_mm *mm,
    uint64_t user_path, int64_t *linux_result)
{
    return change_directory_path(files, fs, mm, user_path, linux_result, 1);
}

enum kernel_files_status kernel_files_fchdir(
    struct kernel_files *files, const struct kernel_fs_context *fs,
    int64_t fd, int64_t *linux_result)
{
    KERNEL_FILES_PIN_SCOPE(pin_guard);
    if (!kernel_files_is_live(files) || !kernel_fs_context_is_live(fs) || !linux_result)
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    struct kernel_open_file_description *file = kernel_files_hold_fd(files, fd, &pin_guard);
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
    KERNEL_FS_PATHS(fs_paths, fs);
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
    int result = kernel_vfs_path_cwd_string(fs_paths.cwd,
                         fs_paths.root, path, KERNEL_FS_PATH_MAX);
    if (result == -KERNEL_ERANGE) result = -KERNEL_ENAMETOOLONG;
    size_t length = result ? 0U : strlen(path) + 1U;
    if (!result && length > size) result = -KERNEL_ERANGE;
    if (!result) {
        size_t copied = 0;
        enum kernel_uaccess_status access = kernel_copy_to_user(mm, user_buffer,
                                                        path, length, &copied);
        if (access == KERNEL_UACCESS_STATUS_FAULT) result = -KERNEL_EFAULT;
        else if (access != KERNEL_UACCESS_STATUS_OK || copied != length) {
            kernel_files_release_allocation(files, path);
            return KERNEL_FILES_STATUS_STATE;
        }
    }
    kernel_files_release_allocation(files, path);
    *linux_result = result ? result : (int64_t)length;
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_linkat(
    struct kernel_files *files, const struct kernel_fs_context *fs,
    struct kernel_mm *mm, int64_t old_dirfd, uint64_t old_user_path,
    int64_t new_dirfd, uint64_t new_user_path, uint32_t flags,
    int64_t *linux_result)
{
    KERNEL_FS_PATHS(fs_paths, fs);
    KERNEL_FILES_PIN_SCOPE(source_pin);
    KERNEL_FILES_PATH_SCOPE(old_start);
    KERNEL_FILES_PATH_SCOPE(new_start);
    KERNEL_FILES_PATH_SCOPE(source);
    char *paths;
    size_t length;
    int result = 0;
    if (!kernel_files_is_live(files) || !kernel_fs_context_is_live(fs) ||
        !mm || !linux_result) return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    if (flags & ~(KERNEL_FILES_AT_SYMLINK_FOLLOW | KERNEL_FILES_AT_EMPTY_PATH)) {
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    enum kernel_heap_status allocation = kernel_heap_allocate(files->heap,
                                      2U * KERNEL_FS_PATH_MAX, (void **)&paths);
    if (allocation != KERNEL_HEAP_STATUS_OK) {
        if (allocation != KERNEL_HEAP_STATUS_EMPTY) return KERNEL_FILES_STATUS_STATE;
        *linux_result = -KERNEL_ENOMEM;
        return KERNEL_FILES_STATUS_OK;
    }
    enum kernel_uaccess_status access = kernel_copy_string_from_user(mm, paths,
                                      old_user_path, KERNEL_FS_PATH_MAX, &length);
    if (access != KERNEL_UACCESS_STATUS_OK) {
        kernel_files_release_allocation(files, paths);
        if (access != KERNEL_UACCESS_STATUS_FAULT && access != KERNEL_UACCESS_STATUS_TOO_LONG)
            return KERNEL_FILES_STATUS_STATE;
        *linux_result = access == KERNEL_UACCESS_STATUS_FAULT ? -KERNEL_EFAULT : -KERNEL_ENAMETOOLONG;
        return KERNEL_FILES_STATUS_OK;
    }
    if (!length && (flags & KERNEL_FILES_AT_EMPTY_PATH)) {
        struct kernel_vfs_path *held = 0;
        if (old_dirfd == KERNEL_FS_AT_FDCWD) {
            if (!kernel_credentials_capable(kernel_credentials_current(),KERNEL_CAP_DAC_READ_SEARCH)) result=-KERNEL_ENOENT;
            else held = fs_paths.cwd;
        }
        else {
            struct kernel_open_file_description *file =
                kernel_files_hold_fd(files, old_dirfd, &source_pin);
            if (!file) result = -KERNEL_EBADF;
            else if (!kernel_credentials_capable(kernel_credentials_current(),KERNEL_CAP_DAC_READ_SEARCH) &&
                kernel_open_file_credentials(file)!=kernel_credentials_current()) result=-KERNEL_ENOENT;
            else held = file->file.path;
        }
        if (held) {
            result = kernel_vfs_path_acquire(held);
            if (!result) source = held;
        }
    } else {
        result = kernel_files_path_start(files, &fs_paths, old_dirfd, paths, &old_start);
        if (!result) result = kernel_vfs_path_resolve(old_start,
            fs_paths.root, paths,
            (flags & KERNEL_FILES_AT_SYMLINK_FOLLOW) != 0, &source);
    }
    if (!result) {
        struct kernel_vfs_mount *mount = 0;
        enum kernel_fs_context_status status = copy_path_start(files, &fs_paths, mm,
            new_dirfd, new_user_path, paths + KERNEL_FS_PATH_MAX,
            KERNEL_FS_PATH_MAX, &new_start, &mount, &result);
        if (status != KERNEL_FS_CONTEXT_STATUS_OK) {
            kernel_files_release_allocation(files, paths);
            return KERNEL_FILES_STATUS_STATE;
        }
    }
    if (!result) result = kernel_vfs_link_at(source, new_start,
                            fs_paths.root, paths + KERNEL_FS_PATH_MAX);
    kernel_files_release_allocation(files, paths);
    *linux_result = result;
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_renameat(
    struct kernel_files *files, const struct kernel_fs_context *fs,
    struct kernel_mm *mm, int64_t old_dirfd, uint64_t old_user_path,
    int64_t new_dirfd, uint64_t new_user_path, uint32_t flags,
    int64_t *linux_result)
{
    KERNEL_FS_PATHS(fs_paths, fs);
    char *paths;
    KERNEL_FILES_PATH_SCOPE(old_start);
    KERNEL_FILES_PATH_SCOPE(new_start);
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
    enum kernel_fs_context_status status = copy_path_start(files, &fs_paths, mm,
        old_dirfd, old_user_path, paths, KERNEL_FS_PATH_MAX, &old_start, &mount, &result);
    if (status == KERNEL_FS_CONTEXT_STATUS_OK && !result)
        status = copy_path_start(files, &fs_paths, mm, new_dirfd, new_user_path,
              paths + KERNEL_FS_PATH_MAX, KERNEL_FS_PATH_MAX, &new_start, &mount, &result);
    if (status != KERNEL_FS_CONTEXT_STATUS_OK) {
        kernel_files_release_allocation(files, paths);
        return KERNEL_FILES_STATUS_STATE;
    }
    if (!result) result = kernel_vfs_rename_at(old_start, new_start,
                   fs_paths.root, paths, paths + KERNEL_FS_PATH_MAX, flags);
    kernel_files_release_allocation(files, paths);
    *linux_result = result;
    return KERNEL_FILES_STATUS_OK;
}
