#include <arch/riscv/sbi.h>
#include <arch/riscv/virt_uart.h>
#include <arch/riscv/virtio_mmio_block.h>
#include <kernel/block.h>
#include <kernel/dtb.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>

#include <stddef.h>
#include <stdint.h>

#define TEST_POOL_PAGES 32U

static unsigned char page_pool[BOAROS_PAGE_SIZE * TEST_POOL_PAGES]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));

static void *identity_access(uint64_t physical_address)
{
    return (void *)(uintptr_t)physical_address;
}

static int identity_dma_address(const void *pointer,
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

static void fail_block(unsigned long case_id,
                       unsigned long expected,
                       unsigned long actual)
    __attribute__((noreturn));

static void fail_block(unsigned long case_id,
                       unsigned long expected,
                       unsigned long actual)
{
    virt_uart_puts("BoarOS: block test failed case=");
    virt_uart_put_hex(case_id);
    virt_uart_puts(" expected=");
    virt_uart_put_hex(expected);
    virt_uart_puts(" actual=");
    virt_uart_put_hex(actual);
    virt_uart_putc('\n');
    sbi_shutdown();
}

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

static void test_rejects_invalid_or_non_block_mmio(void)
{
    uint32_t registers[0x200U / sizeof(uint32_t)] = {0U};
    struct boot_memory_layout layout;
    struct physical_page_allocator allocator;
    struct riscv_virtio_mmio_block device = {0};
    enum riscv_virtio_mmio_block_status status;

    layout.usable_count = 1U;
    layout.usable[0].base = (uint64_t)(uintptr_t)&page_pool[0];
    layout.usable[0].size = sizeof(page_pool);
    if (physical_page_allocator_init(&allocator, &layout) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_allocator_bind_access(&allocator, identity_access) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_allocator_finalize(&allocator) !=
            PHYSICAL_PAGE_STATUS_OK) {
        fail_block(1U, PHYSICAL_PAGE_STATUS_OK, UINT64_MAX);
    }

    registers[0x000U / 4U] = UINT32_C(0x74726976);
    registers[0x004U / 4U] = 3U;
    registers[0x008U / 4U] = 2U;
    status = riscv_virtio_mmio_block_init(&device,
                                          registers,
                                          sizeof(registers),
                                          &allocator,
                                          identity_dma_address,
                                          10000000U);
    if (status != RISCV_VIRTIO_MMIO_BLOCK_STATUS_UNSUPPORTED) {
        fail_block(2U,
                   RISCV_VIRTIO_MMIO_BLOCK_STATUS_UNSUPPORTED,
                   status);
    }

    registers[0x004U / 4U] = 2U;
    registers[0x008U / 4U] = 1U;
    status = riscv_virtio_mmio_block_init(&device,
                                          registers,
                                          sizeof(registers),
                                          &allocator,
                                          identity_dma_address,
                                          10000000U);
    if (status != RISCV_VIRTIO_MMIO_BLOCK_STATUS_NOT_BLOCK) {
        fail_block(3U, RISCV_VIRTIO_MMIO_BLOCK_STATUS_NOT_BLOCK, status);
    }
}

static void test_real_virtio_block(const void *dtb)
{
    static const char sector_marker[] = "BoarOS-direct-sector-one";
    static const char bounce_marker[] = "BoarOS-bounce-window";
    struct dtb_boot_info info;
    struct boot_memory_layout layout;
    struct physical_page_allocator allocator;
    struct riscv_virtio_mmio_block devices[2] = {0};
    struct riscv_virtio_mmio_block_statistics statistics;
    struct riscv_virtio_mmio_block *rw_dev = 0;
    struct riscv_virtio_mmio_block *ro_dev = 0;
    uint64_t baseline;
    uint64_t buffer_address;
    unsigned char *buffer;
    uint32_t index;
    uint32_t device_count = 0U;
    enum riscv_virtio_mmio_block_status virtio_status =
        RISCV_VIRTIO_MMIO_BLOCK_STATUS_NOT_BLOCK;
    enum kernel_block_status block_status;
    unsigned char val_x;
    unsigned char val_y;
    unsigned char val_z[2];
    size_t i;

    if (dtb_read_boot_info(dtb, &info) != DTB_STATUS_OK ||
        info.timebase_frequency == 0U || info.virtio_mmio_count == 0U) {
        fail_block(10U, DTB_STATUS_OK, UINT64_MAX);
    }

    layout.usable_count = 1U;
    layout.usable[0].base = (uint64_t)(uintptr_t)&page_pool[0];
    layout.usable[0].size = sizeof(page_pool);
    if (physical_page_allocator_init(&allocator, &layout) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_allocator_bind_access(&allocator, identity_access) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_allocator_finalize(&allocator) !=
            PHYSICAL_PAGE_STATUS_OK) {
        fail_block(11U, PHYSICAL_PAGE_STATUS_OK, UINT64_MAX);
    }
    baseline = physical_page_available(&allocator);

    for (index = 0U; index < info.virtio_mmio_count; index++) {
        if (device_count >= 2U) {
            break;
        }
        virtio_status = riscv_virtio_mmio_block_init(
            &devices[device_count],
            (volatile void *)(uintptr_t)info.virtio_mmio[index].base,
            info.virtio_mmio[index].size,
            &allocator,
            identity_dma_address,
            info.timebase_frequency);
        if (virtio_status == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK) {
            device_count++;
            continue;
        }
        if (virtio_status != RISCV_VIRTIO_MMIO_BLOCK_STATUS_NOT_BLOCK) {
            fail_block(12U, RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK, virtio_status);
        }
    }
    if (device_count != 2U) {
        fail_block(13U, 2U, (unsigned long)device_count);
    }

    if (devices[0].read_only == 0U && devices[1].read_only != 0U) {
        rw_dev = &devices[0];
        ro_dev = &devices[1];
    } else if (devices[1].read_only == 0U && devices[0].read_only != 0U) {
        rw_dev = &devices[1];
        ro_dev = &devices[0];
    } else {
        fail_block(14U, 1U, 0U);
    }

    if (rw_dev->block.capacity_bytes < UINT64_C(1024) * 1024U ||
        rw_dev->block.logical_block_size != 512U ||
        ro_dev->block.capacity_bytes < UINT64_C(1024) * 1024U ||
        ro_dev->block.logical_block_size != 512U) {
        fail_block(15U, 512U, (unsigned long)rw_dev->block.logical_block_size);
    }

    if (physical_page_allocate(&allocator, &buffer_address) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_resolve(&allocator,
                              buffer_address,
                              (void **)&buffer) != PHYSICAL_PAGE_STATUS_OK) {
        fail_block(16U, PHYSICAL_PAGE_STATUS_OK, UINT64_MAX);
    }

    /* 1. Read-only device checks: write pointer must be NULL and write must fail */
    if (ro_dev->block.write != 0) {
        fail_block(17U, 0U, (unsigned long)(uintptr_t)ro_dev->block.write);
    }
    block_status = kernel_block_write_at(&ro_dev->block, 512U, buffer, 512U);
    if (block_status != KERNEL_BLOCK_STATUS_UNSUPPORTED) {
        fail_block(18U, KERNEL_BLOCK_STATUS_UNSUPPORTED, block_status);
    }
    block_status = kernel_block_read_at(&ro_dev->block, 512U, buffer, 512U);
    if (block_status != KERNEL_BLOCK_STATUS_OK ||
        !bytes_equal(buffer, sector_marker, sizeof(sector_marker) - 1U)) {
        fail_block(19U, KERNEL_BLOCK_STATUS_OK, block_status);
    }

    /* 2. Read-write device checks */
    if (rw_dev->block.write == 0) {
        fail_block(20U, 1U, 0U);
    }

    /* Direct read check */
    block_status = kernel_block_read_at(&rw_dev->block, 512U, buffer, 1024U);
    if (block_status != KERNEL_BLOCK_STATUS_OK ||
        !bytes_equal(buffer, sector_marker, sizeof(sector_marker) - 1U)) {
        fail_block(21U, KERNEL_BLOCK_STATUS_OK, block_status);
    }

    /* Bounce read check */
    block_status = kernel_block_read_at(&rw_dev->block,
                                        1536U + 7U,
                                        buffer + 3U,
                                        sizeof(bounce_marker) - 1U);
    if (block_status != KERNEL_BLOCK_STATUS_OK ||
        !bytes_equal(buffer + 3U, bounce_marker, sizeof(bounce_marker) - 1U)) {
        fail_block(22U, KERNEL_BLOCK_STATUS_OK, block_status);
    }

    /* Direct write check: sector 2 (offset 1024, length 512) */
    for (i = 0U; i < 512U; i++) {
        buffer[i] = (unsigned char)('A' + (i % 26U));
    }
    block_status = kernel_block_write_at(&rw_dev->block, 1024U, buffer, 512U);
    if (block_status != KERNEL_BLOCK_STATUS_OK) {
        fail_block(23U, KERNEL_BLOCK_STATUS_OK, block_status);
    }
    for (i = 0U; i < 512U; i++) {
        buffer[i] = 0U;
    }
    block_status = kernel_block_read_at(&rw_dev->block, 1024U, buffer, 512U);
    if (block_status != KERNEL_BLOCK_STATUS_OK) {
        fail_block(24U, KERNEL_BLOCK_STATUS_OK, block_status);
    }
    for (i = 0U; i < 512U; i++) {
        if (buffer[i] != (unsigned char)('A' + (i % 26U))) {
            fail_block(25U, (unsigned char)('A' + (i % 26U)), buffer[i]);
        }
    }

    /* Bounce RMW check 1: single byte write at offset 1537 (offset % 512 == 1) */
    val_x = 'X';
    block_status = kernel_block_write_at(&rw_dev->block, 1537U, &val_x, 1U);
    if (block_status != KERNEL_BLOCK_STATUS_OK) {
        fail_block(26U, KERNEL_BLOCK_STATUS_OK, block_status);
    }
    block_status = kernel_block_read_at(&rw_dev->block, 1536U, buffer, 3U);
    if (block_status != KERNEL_BLOCK_STATUS_OK ||
        buffer[0] != 0U || buffer[1] != 'X' || buffer[2] != 0U) {
        fail_block(27U, 'X', buffer[1]);
    }

    /* Bounce RMW check 2: single byte write at sector boundary 2047 (offset % 512 == 511) */
    val_y = 'Y';
    block_status = kernel_block_write_at(&rw_dev->block, 2047U, &val_y, 1U);
    if (block_status != KERNEL_BLOCK_STATUS_OK) {
        fail_block(28U, KERNEL_BLOCK_STATUS_OK, block_status);
    }
    block_status = kernel_block_read_at(&rw_dev->block, 2046U, buffer, 2U);
    if (block_status != KERNEL_BLOCK_STATUS_OK ||
        buffer[0] != 0U || buffer[1] != 'Y') {
        fail_block(29U, 'Y', buffer[1]);
    }

    /* Bounce RMW check 3: multi-byte write across sector boundary (2047, len 2: 2047 and 2048) */
    val_z[0] = 'Z';
    val_z[1] = 'W';
    block_status = kernel_block_write_at(&rw_dev->block, 2047U, val_z, 2U);
    if (block_status != KERNEL_BLOCK_STATUS_OK) {
        fail_block(30U, KERNEL_BLOCK_STATUS_OK, block_status);
    }
    block_status = kernel_block_read_at(&rw_dev->block, 2046U, buffer, 4U);
    if (block_status != KERNEL_BLOCK_STATUS_OK ||
        buffer[0] != 0U || buffer[1] != 'Z' || buffer[2] != 'W' || buffer[3] != 0U) {
        fail_block(31U, 'Z', buffer[1]);
    }

    /* Verify adjacent bounce marker at 1543..1562 was preserved */
    block_status = kernel_block_read_at(&rw_dev->block,
                                        1536U + 7U,
                                        buffer,
                                        sizeof(bounce_marker) - 1U);
    if (block_status != KERNEL_BLOCK_STATUS_OK ||
        !bytes_equal(buffer, bounce_marker, sizeof(bounce_marker) - 1U)) {
        fail_block(32U, KERNEL_BLOCK_STATUS_OK, block_status);
    }

    /* Boundary errors */
    block_status = kernel_block_write_at(&rw_dev->block,
                                         rw_dev->block.capacity_bytes - 8U,
                                         buffer,
                                         16U);
    if (block_status != KERNEL_BLOCK_STATUS_OUT_OF_RANGE) {
        fail_block(33U, KERNEL_BLOCK_STATUS_OUT_OF_RANGE, block_status);
    }
    block_status = kernel_block_write_at(&rw_dev->block, 0U, 0, 512U);
    if (block_status != KERNEL_BLOCK_STATUS_INVALID) {
        fail_block(34U, KERNEL_BLOCK_STATUS_INVALID, block_status);
    }
    block_status = kernel_block_read_at(&rw_dev->block,
                                        rw_dev->block.capacity_bytes - 8U,
                                        buffer,
                                        16U);
    if (block_status != KERNEL_BLOCK_STATUS_OUT_OF_RANGE) {
        fail_block(35U, KERNEL_BLOCK_STATUS_OUT_OF_RANGE, block_status);
    }
    block_status = kernel_block_read_at(&rw_dev->block, 0U, 0, 512U);
    if (block_status != KERNEL_BLOCK_STATUS_INVALID) {
        fail_block(36U, KERNEL_BLOCK_STATUS_INVALID, block_status);
    }

    block_status = kernel_block_flush(&rw_dev->block);
    if (block_status != KERNEL_BLOCK_STATUS_OK ||
        rw_dev->block.cache_mode == KERNEL_BLOCK_CACHE_UNKNOWN) {
        fail_block(42U, KERNEL_BLOCK_STATUS_OK, block_status);
    }
    block_status = kernel_block_flush(&ro_dev->block);
    if (block_status != KERNEL_BLOCK_STATUS_OK) {
        fail_block(43U, KERNEL_BLOCK_STATUS_OK, block_status);
    }

    /* Statistics check */
    riscv_virtio_mmio_block_get_statistics(rw_dev, &statistics);
    if (statistics.direct_requests == 0U ||
        statistics.bounce_requests == 0U ||
        statistics.requests != statistics.direct_requests + statistics.bounce_requests + statistics.flush_requests ||
        statistics.flush_requests !=
            (rw_dev->block.cache_mode == KERNEL_BLOCK_CACHE_WRITEBACK ? 1U : 0U) ||
        statistics.sectors_written == 0U ||
        statistics.sectors_read == 0U ||
        statistics.timeouts != 0U ||
        statistics.io_errors != 0U) {
        fail_block(37U, 1U, (unsigned long)statistics.requests);
    }
    virt_uart_puts(rw_dev->block.cache_mode == KERNEL_BLOCK_CACHE_WRITEBACK
        ? "BoarOS: block cache writeback\n" : "BoarOS: block cache writethrough\n");

    /* Cleanup */
    if (physical_page_release(&allocator, buffer_address) !=
        PHYSICAL_PAGE_STATUS_OK) {
        fail_block(38U, PHYSICAL_PAGE_STATUS_OK, UINT64_MAX);
    }
    virtio_status = riscv_virtio_mmio_block_destroy(&devices[0]);
    if (virtio_status != RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK) {
        fail_block(39U, RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK, virtio_status);
    }
    virtio_status = riscv_virtio_mmio_block_destroy(&devices[1]);
    if (virtio_status != RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK) {
        fail_block(40U, RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK, virtio_status);
    }
    if (physical_page_available(&allocator) != baseline) {
        fail_block(41U, (unsigned long)baseline, (unsigned long)physical_page_available(&allocator));
    }
}

void kernel_main(unsigned long hart_id, const void *dtb)
{
    (void)hart_id;

    test_rejects_invalid_or_non_block_mmio();
    test_real_virtio_block(dtb);

    virt_uart_puts("BoarOS: VirtIO block tests passed\n");
    sbi_shutdown();
}
