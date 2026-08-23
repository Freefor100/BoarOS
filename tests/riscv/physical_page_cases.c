#include <arch/riscv/sbi.h>
#include <arch/riscv/virt_uart.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>

#include <stdint.h>

#if BOAROS_PAGE_SHIFT != 12
#error "the RISC-V target requires 4 KiB pages"
#endif

static unsigned char page_pool[BOAROS_PAGE_SIZE * 8U]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));

static void fail_page(unsigned long case_id,
                      enum physical_page_status expected,
                      enum physical_page_status actual)
    __attribute__((noreturn));

static void fail_page(unsigned long case_id,
                      enum physical_page_status expected,
                      enum physical_page_status actual)
{
    virt_uart_puts("BoarOS: physical page test failed case=");
    virt_uart_put_hex(case_id);
    virt_uart_puts(" expected=");
    virt_uart_put_hex((unsigned long)expected);
    virt_uart_puts(" actual=");
    virt_uart_put_hex((unsigned long)actual);
    virt_uart_putc('\n');
    sbi_shutdown();
}

static void test_aligns_allocates_and_reports_exhaustion(void)
{
    struct boot_memory_layout layout;
    struct physical_page_allocator allocator;
    uint64_t address;
    uint64_t pool_base = (uint64_t)(uintptr_t)page_pool;
    enum physical_page_status actual;

    layout.usable_count = 2U;
    layout.usable[0].base = pool_base + 1U;
    layout.usable[0].size = BOAROS_PAGE_SIZE * 2U - 1U;
    layout.usable[1].base = pool_base + BOAROS_PAGE_SIZE * 4U;
    layout.usable[1].size = BOAROS_PAGE_SIZE * 2U;

    actual = physical_page_allocator_init(&allocator, &layout);
    if (actual != PHYSICAL_PAGE_STATUS_OK ||
        physical_page_total(&allocator) != 3U ||
        physical_page_available(&allocator) != 3U) {
        fail_page(1U, PHYSICAL_PAGE_STATUS_OK, actual);
    }

    actual = physical_page_allocate(&allocator, &address);
    if (actual != PHYSICAL_PAGE_STATUS_OK ||
        address != pool_base + BOAROS_PAGE_SIZE) {
        fail_page(2U, PHYSICAL_PAGE_STATUS_OK, actual);
    }
    actual = physical_page_allocate(&allocator, &address);
    if (actual != PHYSICAL_PAGE_STATUS_OK ||
        address != pool_base + BOAROS_PAGE_SIZE * 4U) {
        fail_page(3U, PHYSICAL_PAGE_STATUS_OK, actual);
    }
    actual = physical_page_allocate(&allocator, &address);
    if (actual != PHYSICAL_PAGE_STATUS_OK ||
        address != pool_base + BOAROS_PAGE_SIZE * 5U ||
        physical_page_available(&allocator) != 0U) {
        fail_page(4U, PHYSICAL_PAGE_STATUS_OK, actual);
    }

    address = 0x1122334455667788ULL;
    actual = physical_page_allocate(&allocator, &address);
    if (actual != PHYSICAL_PAGE_STATUS_EMPTY ||
        address != 0x1122334455667788ULL) {
        fail_page(5U, PHYSICAL_PAGE_STATUS_EMPTY, actual);
    }
}

static void test_releases_and_reuses_pages(void)
{
    struct boot_memory_layout layout;
    struct physical_page_allocator allocator;
    uint64_t first;
    uint64_t second;
    uint64_t recycled;
    uint64_t pool_base = (uint64_t)(uintptr_t)page_pool;
    enum physical_page_status actual;

    layout.usable_count = 1U;
    layout.usable[0].base = pool_base;
    layout.usable[0].size = BOAROS_PAGE_SIZE * 3U;
    actual = physical_page_allocator_init(&allocator, &layout);
    if (actual != PHYSICAL_PAGE_STATUS_OK) {
        fail_page(6U, PHYSICAL_PAGE_STATUS_OK, actual);
    }

    actual = physical_page_allocate(&allocator, &first);
    if (actual != PHYSICAL_PAGE_STATUS_OK) {
        fail_page(7U, PHYSICAL_PAGE_STATUS_OK, actual);
    }
    actual = physical_page_allocate(&allocator, &second);
    if (actual != PHYSICAL_PAGE_STATUS_OK) {
        fail_page(8U, PHYSICAL_PAGE_STATUS_OK, actual);
    }

    actual = physical_page_release(&allocator, first);
    if (actual != PHYSICAL_PAGE_STATUS_OK ||
        physical_page_available(&allocator) != 2U) {
        fail_page(9U, PHYSICAL_PAGE_STATUS_OK, actual);
    }
    actual = physical_page_allocate(&allocator, &recycled);
    if (actual != PHYSICAL_PAGE_STATUS_OK || recycled != first ||
        physical_page_available(&allocator) != 1U) {
        fail_page(10U, PHYSICAL_PAGE_STATUS_OK, actual);
    }

    actual = physical_page_release(&allocator, recycled);
    if (actual != PHYSICAL_PAGE_STATUS_OK ||
        physical_page_available(&allocator) != 2U) {
        fail_page(11U, PHYSICAL_PAGE_STATUS_OK, actual);
    }
    actual = physical_page_release(&allocator, recycled);
    if (actual != PHYSICAL_PAGE_STATUS_DOUBLE_FREE ||
        physical_page_available(&allocator) != 2U) {
        fail_page(12U, PHYSICAL_PAGE_STATUS_DOUBLE_FREE, actual);
    }

    actual = physical_page_release(&allocator,
                                   pool_base + BOAROS_PAGE_SIZE * 2U);
    if (actual != PHYSICAL_PAGE_STATUS_INVALID) {
        fail_page(13U, PHYSICAL_PAGE_STATUS_INVALID, actual);
    }
    actual = physical_page_release(&allocator, second + 1U);
    if (actual != PHYSICAL_PAGE_STATUS_INVALID) {
        fail_page(14U, PHYSICAL_PAGE_STATUS_INVALID, actual);
    }
    actual = physical_page_release(&allocator,
                                   pool_base + BOAROS_PAGE_SIZE * 7U);
    if (actual != PHYSICAL_PAGE_STATUS_INVALID) {
        fail_page(15U, PHYSICAL_PAGE_STATUS_INVALID, actual);
    }
}

static void expect_init_status(unsigned long case_id,
                               const struct boot_memory_layout *layout,
                               enum physical_page_status expected)
{
    struct physical_page_allocator allocator;
    enum physical_page_status actual =
        physical_page_allocator_init(&allocator, layout);

    if (actual != expected) {
        fail_page(case_id, expected, actual);
    }
}

static void test_rejects_invalid_layouts(void)
{
    struct boot_memory_layout layout;
    uint64_t pool_base = (uint64_t)(uintptr_t)page_pool;

    layout.usable_count = 0U;
    expect_init_status(16U, &layout, PHYSICAL_PAGE_STATUS_INVALID);

    layout.usable_count = BOOT_MEMORY_MAX_USABLE_RANGES + 1U;
    expect_init_status(17U, &layout, PHYSICAL_PAGE_STATUS_INVALID);

    layout.usable_count = 1U;
    layout.usable[0].base = pool_base;
    layout.usable[0].size = 0U;
    expect_init_status(18U, &layout, PHYSICAL_PAGE_STATUS_INVALID);

    layout.usable[0].base = UINT64_MAX - 0x100U;
    layout.usable[0].size = 0x200U;
    expect_init_status(19U, &layout, PHYSICAL_PAGE_STATUS_INVALID);

    layout.usable_count = 2U;
    layout.usable[0].base = pool_base;
    layout.usable[0].size = BOAROS_PAGE_SIZE * 2U;
    layout.usable[1].base = pool_base + BOAROS_PAGE_SIZE;
    layout.usable[1].size = BOAROS_PAGE_SIZE * 2U;
    expect_init_status(20U, &layout, PHYSICAL_PAGE_STATUS_INVALID);

    layout.usable[0].base = pool_base + BOAROS_PAGE_SIZE * 4U;
    layout.usable[0].size = BOAROS_PAGE_SIZE;
    layout.usable[1].base = pool_base;
    layout.usable[1].size = BOAROS_PAGE_SIZE;
    expect_init_status(21U, &layout, PHYSICAL_PAGE_STATUS_INVALID);

    layout.usable_count = 1U;
    layout.usable[0].base = pool_base + 1U;
    layout.usable[0].size = BOAROS_PAGE_SIZE - 2U;
    expect_init_status(22U, &layout, PHYSICAL_PAGE_STATUS_EMPTY);

    expect_init_status(23U, 0, PHYSICAL_PAGE_STATUS_INVALID);
    if (physical_page_allocator_init(0, &layout) !=
        PHYSICAL_PAGE_STATUS_INVALID) {
        fail_page(24U,
                  PHYSICAL_PAGE_STATUS_INVALID,
                  PHYSICAL_PAGE_STATUS_OK);
    }
}

static void test_failed_init_preserves_allocator(void)
{
    struct boot_memory_layout layout;
    struct physical_page_allocator allocator;
    uint64_t address;
    uint64_t pool_base = (uint64_t)(uintptr_t)page_pool;
    enum physical_page_status actual;

    layout.usable_count = 1U;
    layout.usable[0].base = pool_base;
    layout.usable[0].size = BOAROS_PAGE_SIZE * 2U;
    actual = physical_page_allocator_init(&allocator, &layout);
    if (actual != PHYSICAL_PAGE_STATUS_OK) {
        fail_page(25U, PHYSICAL_PAGE_STATUS_OK, actual);
    }
    actual = physical_page_allocate(&allocator, &address);
    if (actual != PHYSICAL_PAGE_STATUS_OK || address != pool_base) {
        fail_page(26U, PHYSICAL_PAGE_STATUS_OK, actual);
    }

    layout.usable[0].size = 0U;
    actual = physical_page_allocator_init(&allocator, &layout);
    if (actual != PHYSICAL_PAGE_STATUS_INVALID ||
        physical_page_total(&allocator) != 2U ||
        physical_page_available(&allocator) != 1U) {
        fail_page(27U, PHYSICAL_PAGE_STATUS_INVALID, actual);
    }
    actual = physical_page_allocate(&allocator, &address);
    if (actual != PHYSICAL_PAGE_STATUS_OK ||
        address != pool_base + BOAROS_PAGE_SIZE) {
        fail_page(28U, PHYSICAL_PAGE_STATUS_OK, actual);
    }
}

static void test_rejects_invalid_api_inputs(void)
{
    struct boot_memory_layout layout;
    struct physical_page_allocator allocator;
    uint64_t address = 0x1122334455667788ULL;
    uint64_t pool_base = (uint64_t)(uintptr_t)page_pool;
    enum physical_page_status actual;

    allocator.initialized = 0U;
    if (physical_page_total(0) != 0U || physical_page_available(0) != 0U ||
        physical_page_total(&allocator) != 0U ||
        physical_page_available(&allocator) != 0U) {
        fail_page(29U,
                  PHYSICAL_PAGE_STATUS_INVALID,
                  PHYSICAL_PAGE_STATUS_OK);
    }

    layout.usable_count = 1U;
    layout.usable[0].base = pool_base;
    layout.usable[0].size = BOAROS_PAGE_SIZE;
    actual = physical_page_allocator_init(&allocator, &layout);
    if (actual != PHYSICAL_PAGE_STATUS_OK) {
        fail_page(30U, PHYSICAL_PAGE_STATUS_OK, actual);
    }
    actual = physical_page_allocate(0, &address);
    if (actual != PHYSICAL_PAGE_STATUS_INVALID ||
        address != 0x1122334455667788ULL) {
        fail_page(31U, PHYSICAL_PAGE_STATUS_INVALID, actual);
    }
    actual = physical_page_allocate(&allocator, 0);
    if (actual != PHYSICAL_PAGE_STATUS_INVALID) {
        fail_page(32U, PHYSICAL_PAGE_STATUS_INVALID, actual);
    }
    actual = physical_page_release(0, pool_base);
    if (actual != PHYSICAL_PAGE_STATUS_INVALID) {
        fail_page(33U, PHYSICAL_PAGE_STATUS_INVALID, actual);
    }
}

void run_physical_page_tests(void)
{
    test_aligns_allocates_and_reports_exhaustion();
    test_releases_and_reuses_pages();
    test_rejects_invalid_layouts();
    test_failed_init_preserves_allocator();
    test_rejects_invalid_api_inputs();
}
