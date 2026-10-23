#include <kernel/credentials.h>
#include <kernel/scheduler.h>
#include "private.h"

#include <kernel/errno.h>
#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/procfs.h>
#include <kernel/devpts.h>
#include <kernel/tmpfs.h>
#include <kernel/task.h>
#include <kernel/uaccess.h>
#include <kernel/vfs.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define LINUX_MS_RDONLY UINT64_C(1)
#define LINUX_MS_NOSUID UINT64_C(2)
#define LINUX_MS_SILENT UINT64_C(32768)

static int import_string(struct kernel_mm *mm, char *buffer, size_t capacity,
                         uint64_t address)
{
    size_t length = 0;
    enum kernel_uaccess_status status = kernel_copy_string_from_user(
        mm, buffer, address, capacity, &length);
    if (status == KERNEL_UACCESS_STATUS_FAULT) return -KERNEL_EFAULT;
    if (status == KERNEL_UACCESS_STATUS_TOO_LONG) return -KERNEL_ENAMETOOLONG;
    if (status != KERNEL_UACCESS_STATUS_OK) return -KERNEL_EIO;
    return 0;
}

static int mount_filesystem(struct kernel_mm *mm, const struct kernel_fs_context *fs,
                      const struct kernel_syscall_request *request)
{
    KERNEL_FS_PATHS(fs_paths, fs);
    uint64_t flags = request->arguments[3];
    char type[64]={0}, options[256] = {0};
    char *target = 0;
    struct kernel_vfs_path *covered = 0;
    struct kernel_vfs_mount *mounted = 0;
    int result;
    uint64_t source_device = 0;

    if (request->arguments[2]) {
        result = import_string(mm, type, sizeof(type), request->arguments[2]);
        if (result) return result;
    }
    int tmpfs = !strcmp(type, "tmpfs");
    int ext4 = !strcmp(type, "ext4");
    int disk = ext4 || !strcmp(type, "vfat");
    int devpts = !strcmp(type, "devpts");
    enum kernel_heap_status allocation = kernel_heap_allocate(fs->heap,
        2U * KERNEL_FS_PATH_MAX, (void **)&target);
    if (allocation != KERNEL_HEAP_STATUS_OK)
        return allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    char *source_name = target + KERNEL_FS_PATH_MAX;
    if (request->arguments[0]) {
        result = import_string(mm, source_name, KERNEL_FS_PATH_MAX,
                               request->arguments[0]);
        if (result) goto Finish;
    }
    if (request->arguments[4]) {
        result = import_string(mm, options, sizeof(options),
                               request->arguments[4]);
        if (result) goto Finish;
    }
    result = import_string(mm, target, KERNEL_FS_PATH_MAX,
                           request->arguments[1]);
    if (result) goto Finish;
    result = kernel_vfs_path_resolve(fs_paths.cwd,
              fs_paths.root, target, 1, &covered);
    if (result) goto Finish;
    if(!kernel_credentials_capable(kernel_credentials_current(),KERNEL_CAP_SYS_ADMIN)) {result=-KERNEL_EPERM;goto Finish;}
    if(flags & ~(LINUX_MS_RDONLY|LINUX_MS_NOSUID|LINUX_MS_SILENT)) {result=-KERNEL_ENOTSUP;goto Finish;}
    if(!tmpfs && !disk && !devpts && strcmp(type,"proc")) {result=-KERNEL_ENODEV;goto Finish;}
    if(!tmpfs && !devpts && options[0]) {result=-KERNEL_ENOTSUP;goto Finish;}
    if(disk) {
        if(!request->arguments[0]) {result=-KERNEL_EINVAL;goto Finish;}
        struct kernel_vfs_path *source=0;struct kernel_vfs_stat source_stat;
        result=kernel_vfs_path_resolve(fs_paths.cwd,fs_paths.root,source_name,1,&source);
        if(!result)result=kernel_vfs_path_stat(source,&source_stat);
        if(!result && (source_stat.mode&KERNEL_VFS_S_IFMT)!=KERNEL_VFS_S_IFBLK)result=-KERNEL_ENOTBLK;
        if(!result)source_device=source_stat.rdev;
        if(source && kernel_vfs_path_release(&source))__builtin_trap();
        if(result)goto Finish;
    }
    struct kernel_vfs_stat stat;
    result = kernel_vfs_path_stat(covered, &stat);
    if (result) goto Finish;
    if ((stat.mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR) {
        result = -KERNEL_ENOTDIR;
        goto Finish;
    }
    result = disk ? kernel_vfs_disk_create(fs->heap, source_device, flags & LINUX_MS_RDONLY, source_name, type, &mounted)
           : devpts ? kernel_devpts_create(fs->heap, flags & LINUX_MS_RDONLY, options, &mounted)
           : tmpfs ? kernel_tmpfs_create(fs->heap, flags & LINUX_MS_RDONLY, options, &mounted)
                   : kernel_procfs_create(fs->heap, flags & LINUX_MS_RDONLY, &mounted);
    if (result) goto Finish;
    mounted->nosuid=!!(flags&LINUX_MS_NOSUID);
    result = kernel_vfs_mount_attach(mounted, covered);
    if (result) {
        int cleanup = kernel_vfs_unmount(mounted);
        if (cleanup) { if (disk) kernel_vfs_disk_defer_cleanup(mounted); else __builtin_trap(); }
        mounted = 0;
    }
Finish:
    if (covered && kernel_vfs_path_release(&covered)) __builtin_trap();
    kernel_heap_release(fs->heap, target);
    return result;
}

static int unmount_filesystem(struct kernel_mm *mm,
                        const struct kernel_fs_context *fs,
                        const struct kernel_syscall_request *request)
{
    KERNEL_FS_PATHS(fs_paths, fs);
    if (request->arguments[1]) return -KERNEL_ENOTSUP;
    if (!request->arguments[0]) return -KERNEL_EFAULT;
    char *target = 0;
    struct kernel_vfs_path *root = 0;
    enum kernel_heap_status allocation = kernel_heap_allocate(fs->heap,
        KERNEL_FS_PATH_MAX, (void **)&target);
    if (allocation != KERNEL_HEAP_STATUS_OK)
        return allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    int result = import_string(mm, target, KERNEL_FS_PATH_MAX,
                               request->arguments[0]);
    if (result) goto Finish;
    result = kernel_vfs_path_resolve(fs_paths.cwd,
             fs_paths.root, target, 1, &root);
    if (result) goto Finish;
    if(!kernel_credentials_capable(kernel_credentials_current(),KERNEL_CAP_SYS_ADMIN)) {result=-KERNEL_EPERM;goto Finish;}
    struct kernel_vfs_mount *mount = kernel_vfs_path_mount(root);
    if (mount->root_path != root) {
        result = -KERNEL_EINVAL;
        goto Finish;
    }
    result = kernel_vfs_mount_prepare_detach(mount, root);
    if (result) goto Finish;
    result = kernel_vfs_mount_detach(mount, root);
    if (result) goto Finish;
    if (kernel_vfs_path_release(&root)) __builtin_trap();
    result = kernel_vfs_unmount(mount);
    if (result) kernel_vfs_disk_defer_cleanup(mount);
Finish:
    if (root && kernel_vfs_path_release(&root)) __builtin_trap();
    kernel_heap_release(fs->heap, target);
    return result;
}

enum kernel_syscall_status syscall_handle_mount(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded, int unmount)
{
    KERNEL_SERVICE_SCOPE(service);
    const struct kernel_fs_context *fs;
    struct kernel_mm *mm;
    if (kernel_task_fs_context_borrow(caller, &fs) != KERNEL_TASK_STATUS_OK ||
        kernel_task_mm_borrow_mutable(caller, &mm) != KERNEL_TASK_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = unmount ? unmount_filesystem(mm, fs, request)
                             : mount_filesystem(mm, fs, request);
    return KERNEL_SYSCALL_STATUS_OK;
}
