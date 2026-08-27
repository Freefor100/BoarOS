#include <arch/riscv/sbi.h>
#include <arch/riscv/virt_uart.h>
#include <arch/riscv/virtio_mmio_block.h>
#include <kernel/dtb.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/vfs.h>

#include <stddef.h>
#include <stdint.h>

#define TEST_POOL_PAGES 512U

static unsigned char page_pool[BOAROS_PAGE_SIZE * TEST_POOL_PAGES]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));

static void *identity_access(uint64_t physical_address)
{
    return (void *)(uintptr_t)physical_address;
}

static int heap_physical_address(const void *pointer,
                                 uint64_t *physical_address)
{
    uintptr_t start = (uintptr_t)&page_pool[0];
    uintptr_t value = (uintptr_t)pointer;

    if (physical_address == 0 || value < start ||
        value - start >= sizeof(page_pool)) {
        return 0;
    }

    *physical_address = (uint64_t)value;
    return 1;
}

static int dma_physical_address(const void *pointer,
                                uint64_t size,
                                uint64_t *physical_address)
{
    uintptr_t start = (uintptr_t)&page_pool[0];
    uintptr_t value = (uintptr_t)pointer;

    if (physical_address == 0 || size == 0U || value < start ||
        size > sizeof(page_pool) - (value - start)) {
        return 0;
    }

    *physical_address = (uint64_t)value;
    return 1;
}

static void fail_vfs(unsigned long case_id,
                     long expected,
                     long actual)
    __attribute__((noreturn));

static void fail_vfs(unsigned long case_id,
                     long expected,
                     long actual)
{
    virt_uart_puts("BoarOS: VFS test failed case=");
    virt_uart_put_hex(case_id);
    virt_uart_puts(" expected=");
    virt_uart_put_hex((unsigned long)expected);
    virt_uart_puts(" actual=");
    virt_uart_put_hex((unsigned long)actual);
    virt_uart_putc('\n');
    sbi_shutdown();
}

#ifndef VFS_EXPECT_RECOVERY
static int bytes_equal(const unsigned char *bytes,
                       const char *expected,
                       size_t length)
{
    size_t index;

    for (index = 0U; index < length; index++) {
        if (bytes[index] != (unsigned char)expected[index]) {
            return 0;
        }
    }
    return 1;
}
#endif

static void run_vfs_test(const void *dtb)
{
    struct dtb_boot_info info;
    struct boot_memory_layout layout;
    struct physical_page_allocator allocator;
    struct kernel_heap heap;
    struct riscv_virtio_mmio_block device = {0};
    struct kernel_vfs_mount mount = {0};
#ifndef VFS_EXPECT_RECOVERY
    static const char expected[] =
        "BoarOS root init payload for VFS and ELF";
    struct riscv_virtio_mmio_block_statistics statistics;
    struct kernel_vfs_file file = {0};
    struct kernel_vfs_file missing = {0};
    struct kernel_read_source source;
    unsigned char buffer[64];
#endif
    uint64_t baseline;
    uint32_t index;
    int result;
#ifndef VFS_EXPECT_RECOVERY
    size_t read_count = 0U;
#endif
    int found = 0;

    if (dtb_read_boot_info(dtb, &info) != DTB_STATUS_OK ||
        info.timebase_frequency == 0U) {
        fail_vfs(1U, DTB_STATUS_OK, -1);
    }

    layout.usable_count = 1U;
    layout.usable[0].base = (uint64_t)(uintptr_t)&page_pool[0];
    layout.usable[0].size = sizeof(page_pool);
    if (physical_page_allocator_init(&allocator, &layout) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_allocator_bind_access(&allocator, identity_access) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_allocator_finalize(&allocator) !=
            PHYSICAL_PAGE_STATUS_OK ||
        kernel_heap_init(&heap, &allocator, heap_physical_address) !=
            KERNEL_HEAP_STATUS_OK) {
        fail_vfs(2U, 0, -1);
    }
    baseline = physical_page_available(&allocator);

    for (index = 0U; index < info.virtio_mmio_count; index++) {
        enum riscv_virtio_mmio_block_status status =
            riscv_virtio_mmio_block_init(
                &device,
                (volatile void *)(uintptr_t)info.virtio_mmio[index].base,
                info.virtio_mmio[index].size,
                &allocator,
                dma_physical_address,
                info.timebase_frequency);

        if (status == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK) {
            found = 1;
            break;
        }
        if (status != RISCV_VIRTIO_MMIO_BLOCK_STATUS_NOT_BLOCK) {
            fail_vfs(3U, RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK, status);
        }
    }
    if (!found) {
        fail_vfs(4U, 1, 0);
    }

    result = kernel_vfs_mount_root_readonly(&mount, &device.block, &heap);
#ifdef VFS_EXPECT_RECOVERY
    if (result != -KERNEL_EUCLEAN || mount.private_data != 0 ||
        mount.state != 0U) {
        fail_vfs(5U, -KERNEL_EUCLEAN, result);
    }
    if (riscv_virtio_mmio_block_destroy(&device) !=
            RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK ||
        physical_page_available(&allocator) != baseline) {
        fail_vfs(6U,
                 (long)baseline,
                 (long)physical_page_available(&allocator));
    }
#else
    if (result != 0) {
        fail_vfs(5U, 0, result);
    }
    result = kernel_vfs_open(&mount, "/missing", &missing);
    if (result != -KERNEL_ENOENT) {
        fail_vfs(6U, -KERNEL_ENOENT, result);
    }
    result = kernel_vfs_open(&mount, "/init", &file);
    if (result != 0 || file.size != sizeof(expected) - 1U ||
        (file.mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFREG ||
        (file.mode & (KERNEL_VFS_S_IXUSR |
                      KERNEL_VFS_S_IXGRP |
                      KERNEL_VFS_S_IXOTH)) == 0U) {
        fail_vfs(7U, 0, result);
    }

    result = kernel_vfs_file_read_source(&file, &source);
    if (result != 0 || source.size != file.size ||
        kernel_read_source_read_exact(
            &source,
            7U,
            buffer,
            sizeof(expected) - 1U - 7U) != 0 ||
        !bytes_equal(buffer,
                     expected + 7U,
                     sizeof(expected) - 1U - 7U)) {
        fail_vfs(8U, 0, result);
    }
    result = kernel_vfs_pread(&file,
                              file.size,
                              buffer,
                              sizeof(buffer),
                              &read_count);
    if (result != 0 || read_count != 0U) {
        fail_vfs(9U, 0, result);
    }

    result = kernel_vfs_unmount(&mount);
    if (result != -KERNEL_EBUSY) {
        fail_vfs(10U, -KERNEL_EBUSY, result);
    }

    result = kernel_vfs_close(&file);
    if (result != 0) {
        fail_vfs(11U, 0, result);
    }
    result = kernel_vfs_unmount(&mount);
    if (result != 0) {
        fail_vfs(12U, 0, result);
    }

    riscv_virtio_mmio_block_get_statistics(&device, &statistics);
    if (statistics.requests == 0U || statistics.timeouts != 0U ||
        statistics.io_errors != 0U) {
        fail_vfs(13U, 1, (long)statistics.requests);
    }
    if (riscv_virtio_mmio_block_destroy(&device) !=
            RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK ||
        physical_page_available(&allocator) != baseline) {
        fail_vfs(14U,
                 (long)baseline,
                 (long)physical_page_available(&allocator));
    }
#endif
}

void kernel_main(unsigned long hart_id, const void *dtb)
{
    (void)hart_id;

    run_vfs_test(dtb);

#ifdef VFS_EXPECT_RECOVERY
    virt_uart_puts("BoarOS: VFS ext4 recovery rejection passed\n");
#else
    virt_uart_puts("BoarOS: VFS ext4 tests passed\n");
#endif
    sbi_shutdown();
}
