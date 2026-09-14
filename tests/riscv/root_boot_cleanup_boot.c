#include <arch/riscv/virt_uart.h>
#include <kernel/block.h>
#include <kernel/fs_context.h>

#include <stddef.h>
#include <stdint.h>

static int fail_fs_context_create = 1;
static int fail_block_write;

enum kernel_fs_context_status __real_kernel_fs_context_create(
    struct kernel_fs_context *fs,
    struct kernel_vfs_mount *root_mount,
    struct kernel_heap *heap);

enum kernel_block_status __real_kernel_block_write_at(
    struct kernel_block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t size);

enum kernel_block_status __wrap_kernel_block_write_at(
    struct kernel_block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t size)
{
    if (fail_block_write != 0) {
        fail_block_write = 0;
        virt_uart_puts(
            "BoarOS: root cleanup injected ext4 block write failure\n");
        return KERNEL_BLOCK_STATUS_IO;
    }
    return __real_kernel_block_write_at(device, offset, buffer, size);
}

enum kernel_fs_context_status __wrap_kernel_fs_context_create(
    struct kernel_fs_context *fs,
    struct kernel_vfs_mount *root_mount,
    struct kernel_heap *heap)
{
    if (fail_fs_context_create != 0) {
        fail_fs_context_create = 0;
        fail_block_write = 1;
        return KERNEL_FS_CONTEXT_STATUS_NO_MEMORY;
    }
    return __real_kernel_fs_context_create(fs, root_mount, heap);
}
