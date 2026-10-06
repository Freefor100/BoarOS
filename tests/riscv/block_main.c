#include <arch/riscv/sbi.h>
#include <arch/riscv/context.h>
#include <arch/riscv/plic.h>
#include <kernel/scheduler.h>
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

static void test_write_batch(struct riscv_virtio_mmio_block *rw,
                              struct riscv_virtio_mmio_block *ro,
                              unsigned char *buffer)
{
    struct kernel_block_span spans[8];
    for (unsigned i = 0; i < 8; i++) {
        for (unsigned j = 0; j < 512; j++) buffer[i * 512 + j] = (unsigned char)(i + 1);
        spans[i] = (struct kernel_block_span){4096 + i * 1024, buffer + i * 512, 512};
    }
    enum kernel_block_status result = kernel_block_write_batch(&ro->block, spans, 8);
    if (result != KERNEL_BLOCK_STATUS_UNSUPPORTED || ro->block.write_batch)
        fail_block(60, KERNEL_BLOCK_STATUS_UNSUPPORTED, result);
    result = kernel_block_write_batch(&rw->block, spans, 8);
    if (result != KERNEL_BLOCK_STATUS_OK) fail_block(61, KERNEL_BLOCK_STATUS_OK, result);
    struct riscv_virtio_mmio_block_statistics stats;
    riscv_virtio_mmio_block_get_statistics(rw, &stats);
    if (stats.max_inflight != 8) fail_block(62, 8, stats.max_inflight);
    for (unsigned i = 0; i < 8; i++) {
        result = kernel_block_read_at(&rw->block, spans[i].offset, buffer, 512);
        if (result != KERNEL_BLOCK_STATUS_OK) fail_block(63, KERNEL_BLOCK_STATUS_OK, result);
        for (unsigned j = 0; j < 512; j++)
            if (buffer[j] != i + 1) fail_block(64, i + 1, buffer[j]);
    }
    const char first[] = "a", second[] = "bc", crossing[] = "de";
    const char sector[] = "fg";
    spans[0] = (struct kernel_block_span){200 * 512 + 1, first, 1};
    spans[1] = (struct kernel_block_span){200 * 512 + 2, second, 2};
    spans[2] = (struct kernel_block_span){201 * 512 - 1, crossing, 2};
    spans[3] = (struct kernel_block_span){202 * 512, sector, 2};
    spans[4] = (struct kernel_block_span){rw->block.capacity_bytes, 0, 0};
    result = kernel_block_write_batch(&rw->block, spans, 5);
    if (result != KERNEL_BLOCK_STATUS_OK) fail_block(65, KERNEL_BLOCK_STATUS_OK, result);
    result = kernel_block_read_at(&rw->block, 200 * 512, buffer, 3 * 512);
    if (result != KERNEL_BLOCK_STATUS_OK || buffer[0] || buffer[1] != 'a' ||
        buffer[2] != 'b' || buffer[3] != 'c' || buffer[4] ||
        buffer[511] != 'd' || buffer[512] != 'e' || buffer[513] ||
        buffer[1024] != 'f' || buffer[1025] != 'g' || buffer[1026])
        fail_block(66, KERNEL_BLOCK_STATUS_OK, result);
    virt_uart_puts("BoarOS: block batch direct and adjacent RMW passed\n");
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

extern unsigned char __boot_stack_bottom[], __boot_stack_top[];
static struct riscv_virtio_mmio_block *irq_device;
static unsigned char *irq_buffers;
static unsigned irq_done;
static void irq_read_worker(void *argument)
{
    uintptr_t index = (uintptr_t)argument;
    if (kernel_block_read_at(&irq_device->block, 512,
        irq_buffers + index * 512, 512) != KERNEL_BLOCK_STATUS_OK ||
        !bytes_equal(irq_buffers + index * 512, "BoarOS-direct-sector-one", 23))
        fail_block(42, 0, index);
    irq_done++;
}
static void test_irq_reads(const void *dtb, struct physical_page_allocator *allocator,
                            struct riscv_virtio_mmio_block *device, unsigned char *buffer)
{
    struct dtb_irq_info irq;
    if (dtb_read_irq_info(dtb, 0, &irq) != DTB_STATUS_OK ||
        !riscv_plic_init((void *)(uintptr_t)irq.plic.base, irq.plic.size, irq.context, irq.source_count))
        fail_block(43, 0, 1);
    uint32_t source = 0;
    for (unsigned i = 0; i < irq.route_count; i++)
        if (irq.routes[i].base == (uintptr_t)riscv_virtio_mmio_block_base(device)) source = irq.routes[i].source;
    if (kernel_scheduler_init(allocator, (uintptr_t)__boot_stack_bottom,
         (uintptr_t)__boot_stack_top) != KERNEL_SCHEDULER_STATUS_OK ||
        !riscv_virtio_mmio_block_enable_irq(device, source)) fail_block(44, 0, 1);
    irq_device = device; irq_buffers = buffer;
    for (uintptr_t i = 0; i < 2; i++)
        if (kernel_thread_create(irq_read_worker, (void *)i) != KERNEL_SCHEDULER_STATUS_OK)
            fail_block(45, 0, 1);
    unsigned reaped = 0;
    while (reaped < 2) {
        uintptr_t saved = riscv_interrupt_save();
        if (kernel_scheduler_yield_current() != KERNEL_SCHEDULER_STATUS_OK) fail_block(46, 0, 1);
        struct kernel_thread_completion completion;
        if (kernel_scheduler_reap_one(&completion) == KERNEL_SCHEDULER_STATUS_OK) reaped++;
        riscv_interrupt_restore(saved | RISCV_SSTATUS_SIE);
    }
    (void)riscv_interrupt_save();
    struct riscv_virtio_mmio_block_statistics stats;
    riscv_virtio_mmio_block_get_statistics(device, &stats);
    if (irq_done != 2 || !stats.interrupts || !stats.sleeps || stats.runtime_polls)
        fail_block(47, 2, irq_done);
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

    test_write_batch(rw_dev, ro_dev, buffer);

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

    test_irq_reads(dtb, &allocator, rw_dev, buffer);

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
