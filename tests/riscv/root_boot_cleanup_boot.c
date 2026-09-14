#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/vma.h>

static int fail_fs_context_create = 1;

enum kernel_fs_context_status __real_kernel_fs_context_create(
    struct kernel_fs_context *fs,
    struct kernel_vfs_mount *root_mount,
    struct kernel_heap *heap);

enum kernel_fs_context_status __wrap_kernel_fs_context_create(
    struct kernel_fs_context *fs,
    struct kernel_vfs_mount *root_mount,
    struct kernel_heap *heap)
{
    if (fail_fs_context_create != 0) {
        fail_fs_context_create = 0;
        return KERNEL_FS_CONTEXT_STATUS_NO_MEMORY;
    }
    return __real_kernel_fs_context_create(fs, root_mount, heap);
}
