#include <arch/riscv/virt_uart.h>
#include <arch/riscv/root_boot.h>
#include <arch/riscv/sbi.h>
#include <kernel/block.h>
#include <kernel/fs_context.h>

#include <stddef.h>
#include <stdint.h>

static int fail_fs_context_create = 1;
static int fail_block_write;
static unsigned block_writes;

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
    block_writes++;
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

enum riscv_root_boot_status __real_riscv_root_boot_cleanup(
    struct riscv_root_boot *root);

enum riscv_root_boot_status __wrap_riscv_root_boot_cleanup(
    struct riscv_root_boot *root)
{
    void *owner = root->mount.private_data;
    unsigned writes = block_writes;
    uint64_t pages = physical_page_available(root->heap.page_allocator);
    enum riscv_root_boot_status result = __real_riscv_root_boot_cleanup(root);

    /* Journal I/O failed after a successful mount. Retrying teardown must
     * retain the same mount and its device/cache, without more writes. */
    if (result != RISCV_ROOT_BOOT_STATUS_CLEANUP ||
        root->state != RISCV_ROOT_BOOT_CLEANUP ||
        root->failure_status != RISCV_ROOT_BOOT_STATUS_RESOURCES ||
        owner == 0 || root->mount.private_data != owner ||
        root->cleanup_device_owned == 0U ||
        root->device.page_allocator == 0 ||
        root->page_cache.state != KERNEL_PAGE_CACHE_LIVE ||
        physical_page_available(root->heap.page_allocator) != pages ||
        block_writes != writes || fail_block_write != 0) {
        virt_uart_puts("BoarOS: root cleanup owner verification failed\n");
        sbi_shutdown();
    }
    virt_uart_puts("BoarOS: root cleanup retained failed journal owner\n");
    return result;
}
