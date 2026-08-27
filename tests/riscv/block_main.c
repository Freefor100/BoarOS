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

static void test_rejects_non_modern_or_non_block_mmio(void)
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
    registers[0x004U / 4U] = 1U;
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

static void test_reads_real_virtio_block(const void *dtb)
{
    static const char sector_marker[] = "BoarOS-direct-sector-one";
    static const char bounce_marker[] = "BoarOS-bounce-window";
    struct dtb_boot_info info;
    struct boot_memory_layout layout;
    struct physical_page_allocator allocator;
    struct riscv_virtio_mmio_block device = {0};
    struct riscv_virtio_mmio_block_statistics statistics;
    uint64_t baseline;
    uint64_t buffer_address;
    unsigned char *buffer;
    uint32_t index;
    int found = 0;
    enum riscv_virtio_mmio_block_status virtio_status =
        RISCV_VIRTIO_MMIO_BLOCK_STATUS_NOT_BLOCK;
    enum kernel_block_status block_status;

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
        virtio_status = riscv_virtio_mmio_block_init(
            &device,
            (volatile void *)(uintptr_t)info.virtio_mmio[index].base,
            info.virtio_mmio[index].size,
            &allocator,
            identity_dma_address,
            info.timebase_frequency);
        if (virtio_status == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK) {
            found = 1;
            break;
        }
        if (virtio_status != RISCV_VIRTIO_MMIO_BLOCK_STATUS_NOT_BLOCK) {
            fail_block(12U, RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK, virtio_status);
        }
    }
    if (!found || device.block.capacity_bytes < UINT64_C(1024) * 1024U ||
        device.block.logical_block_size != 512U) {
        fail_block(13U, 1U, (unsigned long)found);
    }

    if (physical_page_allocate(&allocator, &buffer_address) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_resolve(&allocator,
                              buffer_address,
                              (void **)&buffer) != PHYSICAL_PAGE_STATUS_OK) {
        fail_block(14U, PHYSICAL_PAGE_STATUS_OK, UINT64_MAX);
    }

    block_status = kernel_block_read_at(&device.block,
                                        512U,
                                        buffer,
                                        1024U);
    if (block_status != KERNEL_BLOCK_STATUS_OK ||
        !bytes_equal(buffer,
                     sector_marker,
                     sizeof(sector_marker) - 1U)) {
        fail_block(15U, KERNEL_BLOCK_STATUS_OK, block_status);
    }

    block_status = kernel_block_read_at(&device.block,
                                        1536U + 7U,
                                        buffer + 3U,
                                        sizeof(bounce_marker) - 1U);
    if (block_status != KERNEL_BLOCK_STATUS_OK ||
        !bytes_equal(buffer + 3U,
                     bounce_marker,
                     sizeof(bounce_marker) - 1U)) {
        fail_block(16U, KERNEL_BLOCK_STATUS_OK, block_status);
    }

    block_status = kernel_block_read_at(&device.block,
                                        device.block.capacity_bytes - 8U,
                                        buffer,
                                        16U);
    if (block_status != KERNEL_BLOCK_STATUS_OUT_OF_RANGE) {
        fail_block(17U, KERNEL_BLOCK_STATUS_OUT_OF_RANGE, block_status);
    }

    riscv_virtio_mmio_block_get_statistics(&device, &statistics);
    if (statistics.requests != 2U || statistics.direct_requests != 1U ||
        statistics.bounce_requests != 1U || statistics.sectors_read != 3U ||
        statistics.timeouts != 0U || statistics.io_errors != 0U) {
        fail_block(18U, 2U, (unsigned long)statistics.requests);
    }

    if (physical_page_release(&allocator, buffer_address) !=
        PHYSICAL_PAGE_STATUS_OK) {
        fail_block(19U, PHYSICAL_PAGE_STATUS_OK, UINT64_MAX);
    }
    virtio_status = riscv_virtio_mmio_block_destroy(&device);
    if (virtio_status != RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK ||
        physical_page_available(&allocator) != baseline) {
        fail_block(20U, RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK, virtio_status);
    }
}

void kernel_main(unsigned long hart_id, const void *dtb)
{
    (void)hart_id;

    test_rejects_non_modern_or_non_block_mmio();
    test_reads_real_virtio_block(dtb);

    virt_uart_puts("BoarOS: VirtIO block tests passed\n");
    sbi_shutdown();
}
