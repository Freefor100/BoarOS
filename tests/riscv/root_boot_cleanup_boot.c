#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/vma.h>

static int fail_fs_context_create = 1;
static int fail_vma_destroy = 1;

enum kernel_fs_context_status __real_kernel_fs_context_create(
    struct kernel_fs_context *fs,
    struct kernel_vfs_mount *root_mount,
    struct kernel_heap *heap);

enum kernel_vma_status __real_kernel_vma_set_destroy(
    struct kernel_vma_set **set);

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

enum kernel_vma_status __wrap_kernel_vma_set_destroy(
    struct kernel_vma_set **set)
{
    if (fail_vma_destroy != 0) {
        fail_vma_destroy = 0;
        return KERNEL_VMA_STATUS_CLEANUP_REQUIRED;
    }
    return __real_kernel_vma_set_destroy(set);
}
