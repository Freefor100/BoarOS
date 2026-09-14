#include <arch/riscv/sbi.h>
#include <arch/riscv/virt_uart.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>

#include <stdint.h>

#if BOAROS_PAGE_SHIFT != 12
#error "the RISC-V target requires 4 KiB pages"
#endif

static unsigned char page_pool[BOAROS_PAGE_SIZE * 64U]
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


static void test_resolves_owned_pages_through_bound_access(void)
{
    struct boot_memory_layout layout;
    struct physical_page_allocator allocator;
    void *const sentinel = (void *)(uintptr_t)UINT64_C(0x1122334455667788);
    void *pointer = sentinel;
    uint64_t address;
    enum physical_page_status actual;

    layout.usable_count = 1U;
    layout.usable[0].base = TEST_PHYSICAL_BASE;
    layout.usable[0].size = BOAROS_PAGE_SIZE * 2U;
    actual = physical_page_allocator_init(&allocator, &layout);
    if (actual != PHYSICAL_PAGE_STATUS_OK ||
        physical_page_allocate(&allocator, &address) !=
            PHYSICAL_PAGE_STATUS_OK) {
        fail_page(44U, PHYSICAL_PAGE_STATUS_OK, actual);
    }

    actual = physical_page_resolve(&allocator, address, &pointer);
    if (actual != PHYSICAL_PAGE_STATUS_STATE || pointer != sentinel) {
        fail_page(45U, PHYSICAL_PAGE_STATUS_STATE, actual);
    }
    if (physical_page_allocator_bind_access(&allocator,
                                            mapped_page_access) !=
        PHYSICAL_PAGE_STATUS_OK) {
        fail_page(46U,
                  PHYSICAL_PAGE_STATUS_OK,
                  PHYSICAL_PAGE_STATUS_STATE);
    }

    actual = physical_page_resolve(0, address, &pointer);
    if (actual != PHYSICAL_PAGE_STATUS_INVALID || pointer != sentinel) {
        fail_page(47U, PHYSICAL_PAGE_STATUS_INVALID, actual);
    }
    actual = physical_page_resolve(&allocator, address, 0);
    if (actual != PHYSICAL_PAGE_STATUS_INVALID) {
        fail_page(48U, PHYSICAL_PAGE_STATUS_INVALID, actual);
    }
    actual = physical_page_resolve(&allocator, address + 1U, &pointer);
    if (actual != PHYSICAL_PAGE_STATUS_INVALID || pointer != sentinel) {
        fail_page(49U, PHYSICAL_PAGE_STATUS_INVALID, actual);
    }
    actual = physical_page_resolve(&allocator,
                                   address + BOAROS_PAGE_SIZE,
                                   &pointer);
    if (actual != PHYSICAL_PAGE_STATUS_INVALID || pointer != sentinel) {
        fail_page(50U, PHYSICAL_PAGE_STATUS_INVALID, actual);
    }

    actual = physical_page_resolve(&allocator, address, &pointer);
    if (actual != PHYSICAL_PAGE_STATUS_OK || pointer != &page_pool[0]) {
        fail_page(51U, PHYSICAL_PAGE_STATUS_OK, actual);
    }
}

static void test_resolve_rejects_inaccessible_page(void)
{
    struct boot_memory_layout layout;
    struct physical_page_allocator allocator;
    void *const sentinel = (void *)(uintptr_t)UINT64_C(0x8877665544332211);
    void *pointer = sentinel;
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
        fail_page(52U, PHYSICAL_PAGE_STATUS_OK, actual);
    }

    actual = physical_page_resolve(&allocator, address, &pointer);
    if (actual != PHYSICAL_PAGE_STATUS_INVALID || pointer != sentinel) {
        fail_page(53U, PHYSICAL_PAGE_STATUS_INVALID, actual);
    }
}

static void test_finalize_imports_bootstrap_state(void)
{
    struct boot_memory_layout layout;
    struct physical_page_allocator allocator;
    uint64_t first;
    uint64_t recycled;
    uint64_t third;
    uint64_t run = UINT64_C(0x1122334455667788);
    uint64_t available;
    enum physical_page_status actual;

    layout.usable_count = 1U;
    layout.usable[0].base = TEST_PHYSICAL_BASE;
    layout.usable[0].size = BOAROS_PAGE_SIZE * 64U;
    actual = init_bound_page_allocator(&allocator,
                                       &layout,
                                       mapped_page_access);
    if (actual != PHYSICAL_PAGE_STATUS_OK ||
        physical_page_allocate(&allocator, &first) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_allocate(&allocator, &recycled) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_allocate(&allocator, &third) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_release(&allocator, recycled) !=
            PHYSICAL_PAGE_STATUS_OK) {
        fail_page(54U, PHYSICAL_PAGE_STATUS_OK, actual);
    }
    available = physical_page_available(&allocator);

    actual = physical_page_allocator_finalize(&allocator);
    if (actual != PHYSICAL_PAGE_STATUS_OK ||
        !physical_page_allocator_is_finalized(&allocator) ||
        physical_page_metadata_pages(&allocator) != 1U ||
        physical_page_available(&allocator) + 1U != available) {
        fail_page(55U, PHYSICAL_PAGE_STATUS_OK, actual);
    }

    available = physical_page_available(&allocator);
    actual = physical_page_allocate_order(&allocator, 1U, &run);
    if (actual != PHYSICAL_PAGE_STATUS_OK ||
        (run & ((BOAROS_PAGE_SIZE << 1U) - 1U)) != 0U ||
        run < TEST_PHYSICAL_BASE ||
        run >= TEST_PHYSICAL_BASE + BOAROS_PAGE_SIZE * 64U) {
        fail_page(57U, PHYSICAL_PAGE_STATUS_OK, actual);
    }
    if (physical_page_release_order(&allocator, run, 1U) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_release(&allocator, first) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_release(&allocator, third) !=
            PHYSICAL_PAGE_STATUS_OK) {
        fail_page(58U, PHYSICAL_PAGE_STATUS_OK,
                  PHYSICAL_PAGE_STATUS_INVALID);
    }
}

static enum physical_page_status init_coalescing_buddy(
    struct physical_page_allocator *allocator,
    uint64_t *owned_page)
{
    struct boot_memory_layout layout;
    uint64_t pages[5];
    uint32_t index;
    enum physical_page_status status;

    layout.usable_count = 1U;
    layout.usable[0].base = TEST_PHYSICAL_BASE;
    layout.usable[0].size = BOAROS_PAGE_SIZE * 6U;
    status = init_bound_page_allocator(allocator,
                                       &layout,
                                       mapped_page_access);
    if (status != PHYSICAL_PAGE_STATUS_OK) {
        return status;
    }
    for (index = 0U; index < 5U; index++) {
        status = physical_page_allocate(allocator, &pages[index]);
        if (status != PHYSICAL_PAGE_STATUS_OK) {
            return status;
        }
    }
    for (index = 0U; index < 4U; index++) {
        status = physical_page_release(allocator, pages[index]);
        if (status != PHYSICAL_PAGE_STATUS_OK) {
            return status;
        }
    }
    status = physical_page_allocator_finalize(allocator);
    if (status == PHYSICAL_PAGE_STATUS_OK) {
        *owned_page = pages[4];
    }
    return status;
}

static void test_buddy_allocates_aligned_runs_and_coalesces(void)
{
    struct physical_page_allocator allocator;
    uint64_t owned_page;
    uint64_t pages[4];
    uint64_t run = UINT64_C(0x1122334455667788);
    uint32_t index;
    enum physical_page_status actual =
        init_coalescing_buddy(&allocator, &owned_page);

    if (actual != PHYSICAL_PAGE_STATUS_OK ||
        physical_page_available(&allocator) != 4U) {
        fail_page(59U, PHYSICAL_PAGE_STATUS_OK, actual);
    }
    for (index = 0U; index < 4U; index++) {
        actual = physical_page_allocate_order(&allocator, 0U, &pages[index]);
        if (actual != PHYSICAL_PAGE_STATUS_OK ||
            pages[index] < TEST_PHYSICAL_BASE ||
            pages[index] >= TEST_PHYSICAL_BASE + BOAROS_PAGE_SIZE * 4U) {
            fail_page(60U + index, PHYSICAL_PAGE_STATUS_OK, actual);
        }
    }
    if (physical_page_available(&allocator) != 0U) {
        fail_page(64U,
                  PHYSICAL_PAGE_STATUS_EMPTY,
                  PHYSICAL_PAGE_STATUS_OK);
    }

    actual = physical_page_release_order(&allocator, pages[1], 0U);
    if (actual == PHYSICAL_PAGE_STATUS_OK) {
        actual = physical_page_release_order(&allocator, pages[3], 0U);
    }
    if (actual == PHYSICAL_PAGE_STATUS_OK) {
        actual = physical_page_release_order(&allocator, pages[0], 0U);
    }
    if (actual == PHYSICAL_PAGE_STATUS_OK) {
        actual = physical_page_release_order(&allocator, pages[2], 0U);
    }
    if (actual != PHYSICAL_PAGE_STATUS_OK ||
        physical_page_available(&allocator) != 4U) {
        fail_page(65U, PHYSICAL_PAGE_STATUS_OK, actual);
    }

    actual = physical_page_allocate_order(&allocator, 2U, &run);
    if (actual != PHYSICAL_PAGE_STATUS_OK || run != TEST_PHYSICAL_BASE ||
        (run & ((BOAROS_PAGE_SIZE << 2U) - 1U)) != 0U) {
        fail_page(66U, PHYSICAL_PAGE_STATUS_OK, actual);
    }
    if (physical_page_release_order(&allocator, run, 2U) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_release(&allocator, owned_page) !=
            PHYSICAL_PAGE_STATUS_OK) {
        fail_page(67U, PHYSICAL_PAGE_STATUS_OK,
                  PHYSICAL_PAGE_STATUS_INVALID);
    }
}

static void test_buddy_rejects_invalid_ownership(void)
{
    struct boot_memory_layout layout;
    struct physical_page_allocator bootstrap;
    struct physical_page_allocator allocator;
    uint64_t owned_page;
    uint64_t run = UINT64_C(0x1122334455667788);
    uint64_t available;
    enum physical_page_status actual;

    layout.usable_count = 1U;
    layout.usable[0].base = TEST_PHYSICAL_BASE;
    layout.usable[0].size = BOAROS_PAGE_SIZE * 6U;
    actual = init_bound_page_allocator(&bootstrap,
                                       &layout,
                                       mapped_page_access);
    if (actual != PHYSICAL_PAGE_STATUS_OK ||
        physical_page_allocator_is_finalized(&bootstrap) ||
        physical_page_metadata_pages(&bootstrap) != 0U) {
        fail_page(68U, PHYSICAL_PAGE_STATUS_OK, actual);
    }
    actual = physical_page_allocate_order(&bootstrap, 0U, &run);
    if (actual != PHYSICAL_PAGE_STATUS_STATE ||
        run != UINT64_C(0x1122334455667788)) {
        fail_page(69U, PHYSICAL_PAGE_STATUS_STATE, actual);
    }

    actual = init_coalescing_buddy(&allocator, &owned_page);
    if (actual != PHYSICAL_PAGE_STATUS_OK) {
        fail_page(70U, PHYSICAL_PAGE_STATUS_OK, actual);
    }
    available = physical_page_available(&allocator);
    actual = physical_page_allocate_order(&allocator,
                                          PHYSICAL_PAGE_MAX_ORDER + 1U,
                                          &run);
    if (actual != PHYSICAL_PAGE_STATUS_INVALID ||
        run != UINT64_C(0x1122334455667788) ||
        physical_page_available(&allocator) != available) {
        fail_page(71U, PHYSICAL_PAGE_STATUS_INVALID, actual);
    }

    actual = physical_page_allocate_order(&allocator, 1U, &run);
    if (actual != PHYSICAL_PAGE_STATUS_OK) {
        fail_page(72U, PHYSICAL_PAGE_STATUS_OK, actual);
    }
    available = physical_page_available(&allocator);
    actual = physical_page_release_order(&allocator, run, 1U);
    if (actual != PHYSICAL_PAGE_STATUS_OK) {
        fail_page(77U, PHYSICAL_PAGE_STATUS_OK, actual);
    }
    available = physical_page_available(&allocator);
    if (physical_page_release(&allocator, owned_page) !=
        PHYSICAL_PAGE_STATUS_OK) {
        fail_page(79U, PHYSICAL_PAGE_STATUS_OK,
                  PHYSICAL_PAGE_STATUS_INVALID);
    }
}

static void write_test_link(uint64_t address, uint64_t value)
{
    unsigned char *bytes = mapped_page_access(address);
    uint32_t index;

    for (index = 0U; index < sizeof(value); index++) {
        bytes[index] = (unsigned char)(value >> (index * 8U));
    }
}

static void test_failed_finalize_preserves_bootstrap_allocator(void)
{
    struct boot_memory_layout layout;
    struct physical_page_allocator allocator;
    uint64_t pages[4];
    uint64_t available;
    uint64_t metadata_address;
    uint32_t index;
    enum physical_page_status actual;

    layout.usable_count = 1U;
    layout.usable[0].base = TEST_PHYSICAL_BASE;
    layout.usable[0].size = BOAROS_PAGE_SIZE * 4U;
    actual = physical_page_allocator_init(&allocator, &layout);
    if (actual != PHYSICAL_PAGE_STATUS_OK ||
        physical_page_allocator_finalize(&allocator) !=
            PHYSICAL_PAGE_STATUS_STATE ||
        physical_page_allocate(&allocator, &pages[0]) !=
            PHYSICAL_PAGE_STATUS_OK ||
        pages[0] != TEST_PHYSICAL_BASE) {
        fail_page(80U, PHYSICAL_PAGE_STATUS_OK, actual);
    }

    actual = init_bound_page_allocator(&allocator,
                                       &layout,
                                       mapped_page_access);
    if (actual != PHYSICAL_PAGE_STATUS_OK) {
        fail_page(81U, PHYSICAL_PAGE_STATUS_OK, actual);
    }
    for (index = 0U; index < 4U; index++) {
        if (physical_page_allocate(&allocator, &pages[index]) !=
            PHYSICAL_PAGE_STATUS_OK) {
            fail_page(82U, PHYSICAL_PAGE_STATUS_OK,
                      PHYSICAL_PAGE_STATUS_EMPTY);
        }
    }
    if (physical_page_release(&allocator, pages[1]) !=
        PHYSICAL_PAGE_STATUS_OK) {
        fail_page(83U, PHYSICAL_PAGE_STATUS_OK,
                  PHYSICAL_PAGE_STATUS_INVALID);
    }
    available = physical_page_available(&allocator);
    actual = physical_page_allocator_finalize(&allocator);
    if (actual != PHYSICAL_PAGE_STATUS_EMPTY ||
        physical_page_available(&allocator) != available ||
        physical_page_allocate(&allocator, &pages[0]) !=
            PHYSICAL_PAGE_STATUS_OK ||
        pages[0] != pages[1]) {
        fail_page(84U, PHYSICAL_PAGE_STATUS_EMPTY, actual);
    }

    actual = init_bound_page_allocator(&allocator,
                                       &layout,
                                       mapped_page_access);
    if (actual != PHYSICAL_PAGE_STATUS_OK ||
        physical_page_allocate(&allocator, &pages[0]) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_allocate(&allocator, &pages[1]) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_release(&allocator, pages[0]) !=
            PHYSICAL_PAGE_STATUS_OK) {
        fail_page(85U, PHYSICAL_PAGE_STATUS_OK, actual);
    }
    available = physical_page_available(&allocator);
    write_test_link(pages[0], TEST_PHYSICAL_BASE + BOAROS_PAGE_SIZE * 8U);
    actual = physical_page_allocator_finalize(&allocator);
    if (actual != PHYSICAL_PAGE_STATUS_INVALID ||
        physical_page_available(&allocator) != available ||
        physical_page_allocator_is_finalized(&allocator)) {
        fail_page(86U, PHYSICAL_PAGE_STATUS_INVALID, actual);
    }
    write_test_link(pages[0], UINT64_MAX);
    if (physical_page_allocate(&allocator, &pages[2]) !=
            PHYSICAL_PAGE_STATUS_OK ||
        pages[2] != pages[0]) {
        fail_page(87U, PHYSICAL_PAGE_STATUS_OK,
                  PHYSICAL_PAGE_STATUS_INVALID);
    }

    actual = init_bound_page_allocator(&allocator,
                                       &layout,
                                       mapped_page_access);
    if (actual != PHYSICAL_PAGE_STATUS_OK ||
        physical_page_allocator_finalize(&allocator) !=
            PHYSICAL_PAGE_STATUS_OK) {
        fail_page(88U, PHYSICAL_PAGE_STATUS_OK, actual);
    }
    available = physical_page_available(&allocator);
    metadata_address = allocator.metadata_address;
    actual = physical_page_allocator_finalize(&allocator);
    if (actual != PHYSICAL_PAGE_STATUS_STATE ||
        physical_page_available(&allocator) != available ||
        allocator.metadata_address != metadata_address) {
        fail_page(89U, PHYSICAL_PAGE_STATUS_STATE, actual);
    }
}

static void test_single_page_api_uses_order_zero_after_finalize(void)
{
    struct physical_page_allocator allocator;
    void *const sentinel = (void *)(uintptr_t)UINT64_C(0x1122334455667788);
    void *pointer = sentinel;
    uint64_t owned_page;
    uint64_t address;
    uint64_t available;
    enum physical_page_status actual =
        init_coalescing_buddy(&allocator, &owned_page);

    if (actual != PHYSICAL_PAGE_STATUS_OK) {
        fail_page(90U, PHYSICAL_PAGE_STATUS_OK, actual);
    }
    available = physical_page_available(&allocator);
    actual = physical_page_allocate(&allocator, &address);
    if (actual != PHYSICAL_PAGE_STATUS_OK ||
        physical_page_available(&allocator) + 1U != available ||
        physical_page_resolve(&allocator, address, &pointer) !=
            PHYSICAL_PAGE_STATUS_OK ||
        pointer != mapped_page_access(address)) {
        fail_page(91U, PHYSICAL_PAGE_STATUS_OK, actual);
    }
    if (physical_page_release(&allocator, address) !=
        PHYSICAL_PAGE_STATUS_OK) {
        fail_page(92U, PHYSICAL_PAGE_STATUS_OK,
                  PHYSICAL_PAGE_STATUS_INVALID);
    }
    pointer = sentinel;
    actual = physical_page_resolve(&allocator, address, &pointer);
    if (actual != PHYSICAL_PAGE_STATUS_INVALID || pointer != sentinel ||
        physical_page_available(&allocator) != available) {
        fail_page(93U, PHYSICAL_PAGE_STATUS_INVALID, actual);
    }
    if (physical_page_release(&allocator, owned_page) !=
        PHYSICAL_PAGE_STATUS_OK) {
        fail_page(94U, PHYSICAL_PAGE_STATUS_OK,
                  PHYSICAL_PAGE_STATUS_INVALID);
    }
}

struct reclaim_test_context {
    struct physical_page_allocator *allocator;
    uint64_t page;
    uint64_t target_pages;
    uint32_t calls;
    enum physical_page_status nested_status;
};

static uint64_t release_one_page_reclaimer(void *opaque,
                                           uint64_t target_pages)
{
    struct reclaim_test_context *context = opaque;

    context->calls++;
    context->target_pages = target_pages;
    if (physical_page_release(context->allocator, context->page) !=
        PHYSICAL_PAGE_STATUS_OK) {
        return 0U;
    }
    return 1U;
}

static uint64_t recursive_reclaimer(void *opaque, uint64_t target_pages)
{
    struct reclaim_test_context *context = opaque;
    uint64_t ignored;

    context->calls++;
    context->target_pages = target_pages;
    context->nested_status =
        physical_page_allocate(context->allocator, &ignored);
    return 0U;
}

static void test_finalized_order_zero_reference_counts(void)
{
    struct physical_page_allocator allocator;
    uint64_t owned_page;
    uint64_t page;
    uint64_t available;
    uint32_t references = UINT32_C(0xfeedface);
    enum physical_page_status actual =
        init_coalescing_buddy(&allocator, &owned_page);

    if (actual != PHYSICAL_PAGE_STATUS_OK ||
        physical_page_allocate(&allocator, &page) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_reference_count(&allocator, page, &references) !=
            PHYSICAL_PAGE_STATUS_OK ||
        references != 1U) {
        fail_page(95U, PHYSICAL_PAGE_STATUS_OK, actual);
    }
    available = physical_page_available(&allocator);
    if (physical_page_acquire(&allocator, page) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_reference_count(&allocator, page, &references) !=
            PHYSICAL_PAGE_STATUS_OK ||
        references != 2U ||
        physical_page_available(&allocator) != available) {
        fail_page(96U, PHYSICAL_PAGE_STATUS_OK,
                  PHYSICAL_PAGE_STATUS_INVALID);
    }
    if (physical_page_release(&allocator, page) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_reference_count(&allocator, page, &references) !=
            PHYSICAL_PAGE_STATUS_OK ||
        references != 1U ||
        physical_page_available(&allocator) != available) {
        fail_page(97U, PHYSICAL_PAGE_STATUS_OK,
                  PHYSICAL_PAGE_STATUS_INVALID);
    }
    if (physical_page_release(&allocator, page) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_available(&allocator) != available + 1U) {
        fail_page(98U, PHYSICAL_PAGE_STATUS_OK,
                  PHYSICAL_PAGE_STATUS_INVALID);
    }

    if (physical_page_allocate_order(&allocator, 1U, &page) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_release_order(&allocator, page, 1U) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_release(&allocator, owned_page) !=
            PHYSICAL_PAGE_STATUS_OK) {
        fail_page(99U, PHYSICAL_PAGE_STATUS_OK,
                  PHYSICAL_PAGE_STATUS_INVALID);
    }
}

static void test_finalized_allocator_reclaims_once_and_guards_recursion(void)
{
    struct physical_page_allocator allocator;
    struct reclaim_test_context context;
    uint64_t owned_page;
    uint64_t pages[4];
    uint64_t reclaimed;
    uint32_t index;
    enum physical_page_status actual =
        init_coalescing_buddy(&allocator, &owned_page);

    if (actual != PHYSICAL_PAGE_STATUS_OK) {
        fail_page(100U, PHYSICAL_PAGE_STATUS_OK, actual);
    }
    for (index = 0U; index < 4U; index++) {
        if (physical_page_allocate(&allocator, &pages[index]) !=
            PHYSICAL_PAGE_STATUS_OK) {
            fail_page(101U, PHYSICAL_PAGE_STATUS_OK,
                      PHYSICAL_PAGE_STATUS_EMPTY);
        }
    }

    context.allocator = &allocator;
    context.page = pages[3];
    context.target_pages = 0U;
    context.calls = 0U;
    context.nested_status = PHYSICAL_PAGE_STATUS_OK;
    if (physical_page_allocator_set_reclaimer(
            &allocator, release_one_page_reclaimer, &context) !=
        PHYSICAL_PAGE_STATUS_OK) {
        fail_page(102U, PHYSICAL_PAGE_STATUS_OK,
                  PHYSICAL_PAGE_STATUS_INVALID);
    }
    reclaimed = UINT64_C(0x1122334455667788);
    actual = physical_page_allocate(&allocator, &reclaimed);
    if (actual != PHYSICAL_PAGE_STATUS_OK || reclaimed != pages[3] ||
        context.calls != 1U || context.target_pages != 1U) {
        fail_page(103U, PHYSICAL_PAGE_STATUS_OK, actual);
    }
    if (physical_page_allocator_clear_reclaimer(&allocator) !=
        PHYSICAL_PAGE_STATUS_OK) {
        fail_page(104U, PHYSICAL_PAGE_STATUS_OK,
                  PHYSICAL_PAGE_STATUS_INVALID);
    }

    context.calls = 0U;
    context.target_pages = 0U;
    if (physical_page_allocator_set_reclaimer(
            &allocator, recursive_reclaimer, &context) !=
        PHYSICAL_PAGE_STATUS_OK) {
        fail_page(105U, PHYSICAL_PAGE_STATUS_OK,
                  PHYSICAL_PAGE_STATUS_INVALID);
    }
    actual = physical_page_allocate(&allocator, &reclaimed);
    if (actual != PHYSICAL_PAGE_STATUS_EMPTY || context.calls != 1U ||
        context.target_pages != 1U ||
        context.nested_status != PHYSICAL_PAGE_STATUS_EMPTY) {
        fail_page(106U, PHYSICAL_PAGE_STATUS_EMPTY, actual);
    }
    if (physical_page_allocator_clear_reclaimer(&allocator) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_release(&allocator, reclaimed) !=
            PHYSICAL_PAGE_STATUS_OK) {
        fail_page(107U, PHYSICAL_PAGE_STATUS_OK,
                  PHYSICAL_PAGE_STATUS_INVALID);
    }
    for (index = 0U; index < 3U; index++) {
        if (physical_page_release(&allocator, pages[index]) !=
            PHYSICAL_PAGE_STATUS_OK) {
            fail_page(108U, PHYSICAL_PAGE_STATUS_OK,
                      PHYSICAL_PAGE_STATUS_INVALID);
        }
    }
    if (physical_page_release(&allocator, owned_page) !=
        PHYSICAL_PAGE_STATUS_OK) {
        fail_page(109U, PHYSICAL_PAGE_STATUS_OK,
                  PHYSICAL_PAGE_STATUS_INVALID);
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
    test_resolves_owned_pages_through_bound_access();
    test_resolve_rejects_inaccessible_page();
    test_finalize_imports_bootstrap_state();
    test_buddy_allocates_aligned_runs_and_coalesces();
    test_buddy_rejects_invalid_ownership();
    test_failed_finalize_preserves_bootstrap_allocator();
    test_single_page_api_uses_order_zero_after_finalize();
    test_finalized_order_zero_reference_counts();
    test_finalized_allocator_reclaims_once_and_guards_recursion();
}
