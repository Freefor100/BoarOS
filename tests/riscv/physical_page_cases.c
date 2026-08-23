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

#define TEST_PHYSICAL_BASE UINT64_C(0x40000000)

static void *identity_page_access(uint64_t address)
{
    return (void *)(uintptr_t)address;
}

static void *mapped_page_access(uint64_t address)
{
    if (address < TEST_PHYSICAL_BASE ||
        address - TEST_PHYSICAL_BASE >= sizeof(page_pool)) {
        return 0;
    }

    return &page_pool[address - TEST_PHYSICAL_BASE];
}

static void *inaccessible_page_access(uint64_t address)
{
    (void)address;
    return 0;
}

static enum physical_page_status init_bound_page_allocator(
    struct physical_page_allocator *allocator,
    const struct boot_memory_layout *layout,
    physical_page_access_fn access)
{
    enum physical_page_status status =
        physical_page_allocator_init(allocator, layout);

    if (status != PHYSICAL_PAGE_STATUS_OK) {
        return status;
    }
    return physical_page_allocator_bind_access(allocator, access);
}

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

    actual = init_bound_page_allocator(&allocator,
                                       &layout,
                                       identity_page_access);
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
    actual = init_bound_page_allocator(&allocator,
                                       &layout,
                                       identity_page_access);
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
    actual = init_bound_page_allocator(&allocator,
                                       &layout,
                                       identity_page_access);
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
    actual = init_bound_page_allocator(&allocator,
                                       &layout,
                                       identity_page_access);
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

static void test_accesses_recycled_nodes_through_mapping(void)
{
    struct boot_memory_layout layout;
    struct physical_page_allocator allocator;
    uint64_t first;
    uint64_t second;
    uint64_t recycled;
    enum physical_page_status actual;

    layout.usable_count = 1U;
    layout.usable[0].base = TEST_PHYSICAL_BASE;
    layout.usable[0].size = BOAROS_PAGE_SIZE * 2U;
    actual = physical_page_allocator_init(&allocator, &layout);
    if (actual != PHYSICAL_PAGE_STATUS_OK) {
        fail_page(34U, PHYSICAL_PAGE_STATUS_OK, actual);
    }
    if (physical_page_allocate(&allocator, &first) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_allocate(&allocator, &second) !=
            PHYSICAL_PAGE_STATUS_OK ||
        first != TEST_PHYSICAL_BASE ||
        second != TEST_PHYSICAL_BASE + BOAROS_PAGE_SIZE) {
        fail_page(35U, PHYSICAL_PAGE_STATUS_OK,
                  PHYSICAL_PAGE_STATUS_INVALID);
    }

    actual = physical_page_release(&allocator, first);
    if (actual != PHYSICAL_PAGE_STATUS_STATE ||
        physical_page_available(&allocator) != 0U) {
        fail_page(36U, PHYSICAL_PAGE_STATUS_STATE, actual);
    }
    actual = physical_page_allocator_bind_access(&allocator, 0);
    if (actual != PHYSICAL_PAGE_STATUS_INVALID) {
        fail_page(37U, PHYSICAL_PAGE_STATUS_INVALID, actual);
    }
    actual = physical_page_allocator_bind_access(&allocator,
                                                 mapped_page_access);
    if (actual != PHYSICAL_PAGE_STATUS_OK) {
        fail_page(38U, PHYSICAL_PAGE_STATUS_OK, actual);
    }
    actual = physical_page_allocator_bind_access(&allocator,
                                                 identity_page_access);
    if (actual != PHYSICAL_PAGE_STATUS_STATE) {
        fail_page(39U, PHYSICAL_PAGE_STATUS_STATE, actual);
    }

    actual = physical_page_release(&allocator, first);
    if (actual != PHYSICAL_PAGE_STATUS_OK ||
        page_pool[0] != UINT8_MAX) {
        fail_page(40U, PHYSICAL_PAGE_STATUS_OK, actual);
    }
    actual = physical_page_allocate(&allocator, &recycled);
    if (actual != PHYSICAL_PAGE_STATUS_OK || recycled != first) {
        fail_page(41U, PHYSICAL_PAGE_STATUS_OK, actual);
    }
}

static void test_rejects_inaccessible_recycled_nodes(void)
{
    struct boot_memory_layout layout;
    struct physical_page_allocator allocator;
    uint64_t address;
    enum physical_page_status actual;

    layout.usable_count = 1U;
    layout.usable[0].base = TEST_PHYSICAL_BASE;
    layout.usable[0].size = BOAROS_PAGE_SIZE;
    actual = physical_page_allocator_init(&allocator, &layout);
    if (actual != PHYSICAL_PAGE_STATUS_OK ||
        physical_page_allocate(&allocator, &address) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_allocator_bind_access(
            &allocator,
            inaccessible_page_access) != PHYSICAL_PAGE_STATUS_OK) {
        fail_page(42U, PHYSICAL_PAGE_STATUS_OK, actual);
    }

    actual = physical_page_release(&allocator, address);
    if (actual != PHYSICAL_PAGE_STATUS_INVALID ||
        physical_page_available(&allocator) != 0U) {
        fail_page(43U, PHYSICAL_PAGE_STATUS_INVALID, actual);
    }
}

void run_physical_page_tests(void)
{
    test_aligns_allocates_and_reports_exhaustion();
    test_releases_and_reuses_pages();
    test_rejects_invalid_layouts();
    test_failed_init_preserves_allocator();
    test_rejects_invalid_api_inputs();
    test_accesses_recycled_nodes_through_mapping();
    test_rejects_inaccessible_recycled_nodes();
}
