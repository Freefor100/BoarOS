#include "private.h"

#include <kernel/errno.h>
#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/procfs.h>
#include <kernel/task.h>
#include <kernel/uaccess.h>
#include <kernel/vfs.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define LINUX_MS_RDONLY UINT64_C(1)
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

static int mount_proc(struct kernel_mm *mm, const struct kernel_fs_context *fs,
                      const struct kernel_syscall_request *request)
{
    uint64_t flags = request->arguments[3];
    char type[64];
    char *target = 0;
    struct kernel_vfs_path *covered = 0;
    struct kernel_vfs_mount *mounted = 0;
    int result;

    if (flags & ~(LINUX_MS_RDONLY | LINUX_MS_SILENT)) return -KERNEL_ENOTSUP;
    if (request->arguments[2] == 0U || request->arguments[1] == 0U)
        return -KERNEL_EFAULT;
    result = import_string(mm, type, sizeof(type), request->arguments[2]);
    if (result) return result;
    if (strcmp(type, "proc")) return -KERNEL_ENODEV;
    enum kernel_heap_status allocation = kernel_heap_allocate(fs->heap,
        KERNEL_FS_PATH_MAX, (void **)&target);
    if (allocation != KERNEL_HEAP_STATUS_OK)
        return allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    if (request->arguments[0]) {
        result = import_string(mm, target, KERNEL_FS_PATH_MAX,
                               request->arguments[0]);
        if (result) goto Finish;
    }
    if (request->arguments[4]) {
        result = import_string(mm, target, KERNEL_FS_PATH_MAX,
                               request->arguments[4]);
        if (result) goto Finish;
        if (target[0]) { result = -KERNEL_ENOTSUP; goto Finish; }
    }
    result = import_string(mm, target, KERNEL_FS_PATH_MAX,
                           request->arguments[1]);
    if (result) goto Finish;
    result = kernel_vfs_path_resolve(kernel_fs_context_cwd(fs),
              kernel_fs_context_root(fs), target, 1, &covered);
    if (result) goto Finish;
    struct kernel_vfs_stat stat;
    result = kernel_vfs_path_stat(covered, &stat);
    if (result) goto Finish;
    if ((stat.mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR) {
        result = -KERNEL_ENOTDIR;
        goto Finish;
    }
    result = kernel_procfs_create(fs->heap, flags & LINUX_MS_RDONLY, &mounted);
    if (result) goto Finish;
    result = kernel_vfs_mount_attach(mounted, covered);
    if (result) {
        if (kernel_vfs_unmount(mounted)) __builtin_trap();
        mounted = 0;
    }
Finish:
    if (covered && kernel_vfs_path_release(&covered)) __builtin_trap();
    if (kernel_heap_release(fs->heap, target) != KERNEL_HEAP_STATUS_OK)
        __builtin_trap();
    return result;
}

static int unmount_proc(struct kernel_mm *mm,
                        const struct kernel_fs_context *fs,
                        const struct kernel_syscall_request *request)
{
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
    result = kernel_vfs_path_resolve(kernel_fs_context_cwd(fs),
             kernel_fs_context_root(fs), target, 1, &root);
    if (result) goto Finish;
    struct kernel_vfs_mount *mount = kernel_vfs_path_mount(root);
    if (!kernel_procfs_is_mount(mount) || mount->root_path != root) {
        result = -KERNEL_EINVAL;
        goto Finish;
    }
    result = kernel_vfs_mount_detach(mount, root);
    if (result) goto Finish;
    if (kernel_vfs_path_release(&root)) __builtin_trap();
    if (kernel_vfs_unmount(mount)) __builtin_trap();
Finish:
    if (root && kernel_vfs_path_release(&root)) __builtin_trap();
    if (kernel_heap_release(fs->heap, target) != KERNEL_HEAP_STATUS_OK)
        __builtin_trap();
    return result;
}

enum kernel_syscall_status syscall_handle_mount(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded, int unmount)
{
    const struct kernel_fs_context *fs;
    struct kernel_mm *mm;
    if (kernel_task_fs_context_borrow(caller, &fs) != KERNEL_TASK_STATUS_OK ||
        kernel_task_mm_borrow_mutable(caller, &mm) != KERNEL_TASK_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = unmount ? unmount_proc(mm, fs, request)
                             : mount_proc(mm, fs, request);
    return KERNEL_SYSCALL_STATUS_OK;
}
