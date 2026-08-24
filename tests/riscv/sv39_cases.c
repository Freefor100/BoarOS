#include <arch/riscv/sv39.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>

#include <stdint.h>

#define TEST_POOL_WORDS(pages) \
    ((BOAROS_PAGE_SIZE * (pages)) / sizeof(uint64_t))

static uint64_t page_pool[TEST_POOL_WORDS(8U)]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));
static uint64_t scale_page_pool[TEST_POOL_WORDS(32U)]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));
static uint64_t mixed_page_pool[TEST_POOL_WORDS(8U)]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));
static uint64_t exhausted_page_pool[TEST_POOL_WORDS(2U)]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));
static uint64_t lifecycle_page_pool[TEST_POOL_WORDS(8U)]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));
static uint64_t model_page_pool[TEST_POOL_WORDS(16U)]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));
static uint64_t single_page_pool[TEST_POOL_WORDS(1U)]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));
static uint64_t user_space_page_pool[TEST_POOL_WORDS(16U)]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));
static uint64_t user_space_oom_pool[TEST_POOL_WORDS(4U)]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));

static void *identity_page_access(uint64_t address)
{
    return (void *)(uintptr_t)address;
}

static enum physical_page_status init_test_page_allocator(
    struct physical_page_allocator *allocator,
    const struct boot_memory_layout *layout)
{
    enum physical_page_status status =
        physical_page_allocator_init(allocator, layout);

    if (status != PHYSICAL_PAGE_STATUS_OK) {
        return status;
    }
    return physical_page_allocator_bind_access(allocator,
                                               identity_page_access);
}

#define TEST_PTE_VALID UINT64_C(0x001)
#define TEST_PTE_READ UINT64_C(0x002)
#define TEST_PTE_WRITE UINT64_C(0x004)
#define TEST_PTE_EXECUTE UINT64_C(0x008)
#define TEST_PTE_USER UINT64_C(0x010)
#define TEST_PTE_RWX \
    (TEST_PTE_READ | TEST_PTE_WRITE | TEST_PTE_EXECUTE)
#define TEST_PTE_FLAGS UINT64_C(0x3ff)
#define TEST_PTE_ALLOWED_MASK ((UINT64_C(1) << 54U) - UINT64_C(1))
#define TEST_INDEX_MASK UINT64_C(0x1ff)

enum test_walk_status {
    TEST_WALK_UNMAPPED = 0,
    TEST_WALK_MAPPED,
    TEST_WALK_MALFORMED,
};

struct test_walk_result {
    uint64_t physical_address;
    uint64_t leaf_size;
    uint32_t permissions;
};

static void reset_page_table(struct riscv_sv39_page_table *table)
{
    table->allocator = 0;
    table->root_address = 0U;
    table->table_pages = 0U;
    table->leaf_4k = 0U;
    table->leaf_2m = 0U;
    table->state = RISCV_SV39_STATE_UNINITIALIZED;
}

static uint32_t test_permissions(uint64_t entry)
{
    uint32_t permissions = 0U;

    if ((entry & TEST_PTE_READ) != 0U) {
        permissions |= RISCV_SV39_READ;
    }
    if ((entry & TEST_PTE_WRITE) != 0U) {
        permissions |= RISCV_SV39_WRITE;
    }
    if ((entry & TEST_PTE_EXECUTE) != 0U) {
        permissions |= RISCV_SV39_EXECUTE;
    }
    if ((entry & TEST_PTE_USER) != 0U) {
        permissions |= RISCV_SV39_USER;
    }

    return permissions;
}

static enum test_walk_status test_walk_sv39(
    const struct riscv_sv39_page_table *table,
    uint64_t virtual_address,
    struct test_walk_result *result)
{
    const uint64_t sizes[3] = {
        UINT64_C(0x40000000),
        RISCV_SV39_PAGE_SIZE_2M,
        RISCV_SV39_PAGE_SIZE_4K,
    };
    const uint32_t shifts[3] = {30U, 21U, 12U};
    uint64_t *entries = (uint64_t *)(uintptr_t)table->root_address;
    uint32_t level;

    for (level = 0U; level < 3U; level++) {
        uint64_t entry =
            entries[(virtual_address >> shifts[level]) & TEST_INDEX_MASK];
        uint64_t rwx = entry & TEST_PTE_RWX;
        uint64_t physical_base;

        if (entry == 0U) {
            return TEST_WALK_UNMAPPED;
        }
        if ((entry & ~TEST_PTE_ALLOWED_MASK) != 0U ||
            (entry & TEST_PTE_VALID) == 0U ||
            ((entry & TEST_PTE_WRITE) != 0U &&
             (entry & TEST_PTE_READ) == 0U)) {
            return TEST_WALK_MALFORMED;
        }
        if (rwx != 0U) {
            physical_base = (entry >> 10U) << BOAROS_PAGE_SHIFT;
            if ((physical_base & (sizes[level] - 1U)) != 0U) {
                return TEST_WALK_MALFORMED;
            }
            result->physical_address =
                physical_base | (virtual_address & (sizes[level] - 1U));
            result->leaf_size = sizes[level];
            result->permissions = test_permissions(entry);
            return TEST_WALK_MAPPED;
        }
        if ((entry & TEST_PTE_FLAGS) != TEST_PTE_VALID || level == 2U) {
            return TEST_WALK_MALFORMED;
        }

        entries = (uint64_t *)(uintptr_t)((entry >> 10U) <<
                                           BOAROS_PAGE_SHIFT);
    }

    return TEST_WALK_MALFORMED;
}

static int expect_walk(const struct riscv_sv39_page_table *table,
                       uint64_t virtual_address,
                       uint64_t physical_address,
                       uint64_t leaf_size,
                       uint32_t permissions)
{
    struct test_walk_result result;

    return test_walk_sv39(table, virtual_address, &result) ==
               TEST_WALK_MAPPED &&
           result.physical_address == physical_address &&
           result.leaf_size == leaf_size &&
           result.permissions == permissions;
}

static int init_allocator(struct physical_page_allocator *allocator)
{
    struct boot_memory_layout layout;

    layout.usable_count = 1U;
    layout.usable[0].base = (uint64_t)(uintptr_t)page_pool;
    layout.usable[0].size = sizeof(page_pool);
    return init_test_page_allocator(allocator, &layout) ==
           PHYSICAL_PAGE_STATUS_OK;
}

static int test_lifecycle_states(void)
{
    struct boot_memory_layout layout;
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table table;
    uint64_t address;
    uint64_t available;

    layout.usable_count = 1U;
    layout.usable[0].base = (uint64_t)(uintptr_t)lifecycle_page_pool;
    layout.usable[0].size = sizeof(lifecycle_page_pool);
    reset_page_table(&table);
    if (init_test_page_allocator(&allocator, &layout) !=
            PHYSICAL_PAGE_STATUS_OK) {
        return 23;
    }

    if (riscv_sv39_map_range(&table,
                             UINT64_C(0x0),
                             UINT64_C(0x0),
                             RISCV_SV39_PAGE_SIZE_4K,
                             RISCV_SV39_READ) != RISCV_SV39_STATUS_STATE ||
        riscv_sv39_activate(&table) != RISCV_SV39_STATUS_STATE ||
        riscv_sv39_page_table_init(&table, 0) !=
            RISCV_SV39_STATUS_INVALID ||
        table.state != RISCV_SV39_STATE_UNINITIALIZED) {
        return 24;
    }

    if (riscv_sv39_page_table_init(&table, &allocator) !=
            RISCV_SV39_STATUS_OK ||
        table.state != RISCV_SV39_STATE_BUILDING) {
        return 25;
    }
    available = physical_page_available(&allocator);
    if (riscv_sv39_page_table_init(&table, &allocator) !=
            RISCV_SV39_STATUS_STATE ||
        riscv_sv39_page_table_init(&table, 0) !=
            RISCV_SV39_STATUS_STATE ||
        table.state != RISCV_SV39_STATE_BUILDING ||
        physical_page_available(&allocator) != available) {
        return 26;
    }

    if (riscv_sv39_map_range(&table,
                             UINT64_C(0x1),
                             UINT64_C(0x0),
                             RISCV_SV39_PAGE_SIZE_4K,
                             RISCV_SV39_READ) != RISCV_SV39_STATUS_INVALID ||
        table.state != RISCV_SV39_STATE_BUILDING) {
        return 27;
    }
    if (riscv_sv39_map_range(&table,
                             UINT64_C(0x0),
                             UINT64_C(0x0),
                             RISCV_SV39_PAGE_SIZE_4K,
                             RISCV_SV39_READ) != RISCV_SV39_STATUS_OK ||
        table.state != RISCV_SV39_STATE_BUILDING) {
        return 28;
    }
    if (riscv_sv39_map_range(&table,
                             UINT64_C(0x0),
                             UINT64_C(0x0),
                             RISCV_SV39_PAGE_SIZE_4K,
                             RISCV_SV39_READ) != RISCV_SV39_STATUS_CONFLICT ||
        table.state != RISCV_SV39_STATE_FAILED) {
        return 29;
    }

    available = physical_page_available(&allocator);
    if (riscv_sv39_page_table_init(&table, &allocator) !=
            RISCV_SV39_STATUS_STATE ||
        riscv_sv39_page_table_init(&table, 0) !=
            RISCV_SV39_STATUS_STATE ||
        riscv_sv39_map_range(&table,
                             UINT64_C(0x1000),
                             UINT64_C(0x1000),
                             RISCV_SV39_PAGE_SIZE_4K,
                             RISCV_SV39_READ) != RISCV_SV39_STATUS_STATE ||
        riscv_sv39_activate(&table) != RISCV_SV39_STATUS_STATE ||
        table.state != RISCV_SV39_STATE_FAILED ||
        physical_page_available(&allocator) != available) {
        return 30;
    }

    reset_page_table(&table);
    if (riscv_sv39_page_table_init(&table, &allocator) !=
            RISCV_SV39_STATUS_OK ||
        table.state != RISCV_SV39_STATE_BUILDING) {
        return 31;
    }
    table.root_address++;
    if (riscv_sv39_activate(&table) != RISCV_SV39_STATUS_INVALID ||
        table.state != RISCV_SV39_STATE_FAILED) {
        return 32;
    }

    layout.usable[0].base = (uint64_t)(uintptr_t)single_page_pool;
    layout.usable[0].size = sizeof(single_page_pool);
    reset_page_table(&table);
    if (init_test_page_allocator(&allocator, &layout) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_allocate(&allocator, &address) !=
            PHYSICAL_PAGE_STATUS_OK ||
        riscv_sv39_page_table_init(&table, &allocator) !=
            RISCV_SV39_STATUS_NO_MEMORY ||
        table.state != RISCV_SV39_STATE_UNINITIALIZED) {
        return 41;
    }

    layout.usable[0].base = (uint64_t)(uintptr_t)lifecycle_page_pool;
    layout.usable[0].size = sizeof(lifecycle_page_pool);
    reset_page_table(&table);
    if (init_test_page_allocator(&allocator, &layout) !=
            PHYSICAL_PAGE_STATUS_OK ||
        riscv_sv39_page_table_init(&table, &allocator) !=
            RISCV_SV39_STATUS_OK) {
        return 42;
    }
    allocator.ranges[0].end =
        (UINT64_C(1) << 56U) + RISCV_SV39_PAGE_SIZE_4K;
    if (riscv_sv39_map_range(&table,
                             UINT64_C(0x0),
                             UINT64_C(0x0),
                             RISCV_SV39_PAGE_SIZE_4K,
                             RISCV_SV39_READ) != RISCV_SV39_STATUS_INVALID ||
        table.state != RISCV_SV39_STATE_FAILED) {
        return 43;
    }

    return 0;
}

static int test_model_and_boundaries(void)
{
    struct boot_memory_layout layout;
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table table;
    struct test_walk_result result;
    uint64_t *root;
    uint32_t index;

    for (index = 0U;
         index < sizeof(model_page_pool) / sizeof(model_page_pool[0]);
         index++) {
        model_page_pool[index] = UINT64_C(0xa5a5a5a5a5a5a5a5);
    }

    layout.usable_count = 1U;
    layout.usable[0].base = (uint64_t)(uintptr_t)model_page_pool;
    layout.usable[0].size = sizeof(model_page_pool);
    reset_page_table(&table);
    if (init_test_page_allocator(&allocator, &layout) !=
            PHYSICAL_PAGE_STATUS_OK ||
        riscv_sv39_page_table_init(&table, &allocator) !=
            RISCV_SV39_STATUS_OK) {
        return 33;
    }

    root = (uint64_t *)(uintptr_t)table.root_address;
    for (index = 0U; index < BOAROS_PAGE_SIZE / sizeof(*root); index++) {
        if (root[index] != 0U) {
            return 34;
        }
    }

    if (riscv_sv39_map_range(&table,
                             UINT64_C(0x0000003ffffff000),
                             UINT64_C(0x0),
                             UINT64_C(0x2000),
                             RISCV_SV39_READ) !=
            RISCV_SV39_STATUS_INVALID ||
        riscv_sv39_map_range(&table,
                             UINT64_C(0xffffffc000000000),
                             UINT64_C(0x00fffffffffff000),
                             UINT64_C(0x2000),
                             RISCV_SV39_READ) !=
            RISCV_SV39_STATUS_INVALID ||
        table.state != RISCV_SV39_STATE_BUILDING) {
        return 35;
    }

    if (riscv_sv39_map_range(&table,
                             UINT64_C(0x40001000),
                             UINT64_C(0x80001000),
                             UINT64_C(0x400000),
                             RISCV_SV39_READ) != RISCV_SV39_STATUS_OK ||
        riscv_sv39_map_range(&table,
                             UINT64_C(0xffffffc000000000),
                             UINT64_C(0x00fffffffffff000),
                             RISCV_SV39_PAGE_SIZE_4K,
                             RISCV_SV39_READ | RISCV_SV39_EXECUTE) !=
            RISCV_SV39_STATUS_OK) {
        return 36;
    }

    if (!expect_walk(&table,
                     UINT64_C(0x40001000),
                     UINT64_C(0x80001000),
                     RISCV_SV39_PAGE_SIZE_4K,
                     RISCV_SV39_READ) ||
        !expect_walk(&table,
                     UINT64_C(0x401fffff),
                     UINT64_C(0x801fffff),
                     RISCV_SV39_PAGE_SIZE_4K,
                     RISCV_SV39_READ) ||
        !expect_walk(&table,
                     UINT64_C(0x40200000),
                     UINT64_C(0x80200000),
                     RISCV_SV39_PAGE_SIZE_2M,
                     RISCV_SV39_READ) ||
        !expect_walk(&table,
                     UINT64_C(0x403fffff),
                     UINT64_C(0x803fffff),
                     RISCV_SV39_PAGE_SIZE_2M,
                     RISCV_SV39_READ) ||
        !expect_walk(&table,
                     UINT64_C(0x40400000),
                     UINT64_C(0x80400000),
                     RISCV_SV39_PAGE_SIZE_4K,
                     RISCV_SV39_READ) ||
        !expect_walk(&table,
                     UINT64_C(0x40400fff),
                     UINT64_C(0x80400fff),
                     RISCV_SV39_PAGE_SIZE_4K,
                     RISCV_SV39_READ)) {
        return 37;
    }

    if (!expect_walk(&table,
                     UINT64_C(0xffffffc000000000),
                     UINT64_C(0x00fffffffffff000),
                     RISCV_SV39_PAGE_SIZE_4K,
                     RISCV_SV39_READ | RISCV_SV39_EXECUTE) ||
        !expect_walk(&table,
                     UINT64_C(0xffffffc000000fff),
                     UINT64_C(0x00ffffffffffffff),
                     RISCV_SV39_PAGE_SIZE_4K,
                     RISCV_SV39_READ | RISCV_SV39_EXECUTE) ||
        test_walk_sv39(&table, UINT64_C(0x40000000), &result) !=
            TEST_WALK_UNMAPPED ||
        test_walk_sv39(&table, UINT64_C(0x40401000), &result) !=
            TEST_WALK_UNMAPPED ||
        test_walk_sv39(&table, UINT64_C(0xffffffc000001000), &result) !=
            TEST_WALK_UNMAPPED ||
        table.table_pages != 6U || table.leaf_2m != 1U ||
        table.leaf_4k != 513U) {
        return 38;
    }

    reset_page_table(&table);
    if (init_test_page_allocator(&allocator, &layout) !=
            PHYSICAL_PAGE_STATUS_OK ||
        riscv_sv39_page_table_init(&table, &allocator) !=
            RISCV_SV39_STATUS_OK ||
        riscv_sv39_map_range(&table,
                             UINT64_C(0x0),
                             UINT64_C(0x200000),
                             RISCV_SV39_PAGE_SIZE_2M,
                             RISCV_SV39_READ) != RISCV_SV39_STATUS_OK ||
        riscv_sv39_map_range(&table,
                             UINT64_C(0x1000),
                             UINT64_C(0x201000),
                             RISCV_SV39_PAGE_SIZE_4K,
                             RISCV_SV39_READ) !=
            RISCV_SV39_STATUS_CONFLICT ||
        table.state != RISCV_SV39_STATE_FAILED ||
        !expect_walk(&table,
                     UINT64_C(0x1000),
                     UINT64_C(0x201000),
                     RISCV_SV39_PAGE_SIZE_2M,
                     RISCV_SV39_READ)) {
        return 39;
    }

    reset_page_table(&table);
    if (init_test_page_allocator(&allocator, &layout) !=
            PHYSICAL_PAGE_STATUS_OK ||
        riscv_sv39_page_table_init(&table, &allocator) !=
            RISCV_SV39_STATUS_OK ||
        riscv_sv39_map_range(&table,
                             UINT64_C(0x0),
                             UINT64_C(0x400000),
                             RISCV_SV39_PAGE_SIZE_4K,
                             RISCV_SV39_READ) != RISCV_SV39_STATUS_OK ||
        riscv_sv39_map_range(&table,
                             UINT64_C(0x0),
                             UINT64_C(0x400000),
                             RISCV_SV39_PAGE_SIZE_2M,
                             RISCV_SV39_READ) !=
            RISCV_SV39_STATUS_CONFLICT ||
        table.state != RISCV_SV39_STATE_FAILED ||
        !expect_walk(&table,
                     UINT64_C(0x0),
                     UINT64_C(0x400000),
                     RISCV_SV39_PAGE_SIZE_4K,
                     RISCV_SV39_READ) ||
        test_walk_sv39(&table, UINT64_C(0x1000), &result) !=
            TEST_WALK_UNMAPPED) {
        return 40;
    }

    return 0;
}

static int test_user_space_lifecycle(void)
{
    const uint64_t code_va = UINT64_C(0x10000);
    const uint64_t stack_va = UINT64_C(0x3ffffff000);
    const uint64_t kernel_va = UINT64_C(0xffffffc000000000);
    struct boot_memory_layout layout;
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table kernel_table;
    struct riscv_sv39_user_space source = {0};
    struct riscv_sv39_user_space destination = {0};
    struct riscv_sv39_mapping mapping;
    uint64_t *kernel_root;
    uint64_t *user_root;
    uint64_t kernel_high_entry;
    uint64_t code_page;
    uint64_t stack_page;
    uint64_t rejected_page;
    uint64_t conflict_page;
    uint64_t available_before_invalid;
    uint64_t user_satp = UINT64_C(0x1122334455667788);

    layout.usable_count = 1U;
    layout.usable[0].base = (uint64_t)(uintptr_t)user_space_page_pool;
    layout.usable[0].size = sizeof(user_space_page_pool);
    reset_page_table(&kernel_table);
    if (init_test_page_allocator(&allocator, &layout) !=
            PHYSICAL_PAGE_STATUS_OK ||
        riscv_sv39_page_table_init(&kernel_table, &allocator) !=
            RISCV_SV39_STATUS_OK ||
        riscv_sv39_map_range(&kernel_table,
                             kernel_va,
                             UINT64_C(0x200000),
                             RISCV_SV39_PAGE_SIZE_4K,
                             RISCV_SV39_READ | RISCV_SV39_EXECUTE) !=
            RISCV_SV39_STATUS_OK) {
        return 44;
    }
    kernel_table.state = RISCV_SV39_STATE_ACTIVE;
    if (riscv_sv39_user_space_satp(&source, &user_satp) !=
            RISCV_SV39_STATUS_STATE ||
        user_satp != UINT64_C(0x1122334455667788)) {
        return 45;
    }
    kernel_root = (uint64_t *)(uintptr_t)kernel_table.root_address;
    kernel_high_entry = kernel_root[256];
    if (kernel_high_entry == 0U ||
        riscv_sv39_user_space_init(&source,
                                   &allocator,
                                   &kernel_table) !=
            RISCV_SV39_STATUS_OK) {
        return 45;
    }

    user_root = (uint64_t *)(uintptr_t)source.root_address;
    if (riscv_sv39_user_space_satp(&source, &user_satp) !=
            RISCV_SV39_STATUS_OK ||
        user_satp != ((UINT64_C(8) << 60U) |
                      (source.root_address >> BOAROS_PAGE_SHIFT)) ||
        riscv_sv39_user_space_satp(&source, 0) !=
            RISCV_SV39_STATUS_INVALID ||
        source.table_pages != 1U || source.leaf_pages != 0U ||
        user_root[0] != 0U || user_root[255] != 0U ||
        user_root[256] != kernel_high_entry ||
        user_root[511] != kernel_root[511]) {
        return 46;
    }

    if (physical_page_allocate(&allocator, &rejected_page) !=
            PHYSICAL_PAGE_STATUS_OK) {
        return 47;
    }
    available_before_invalid = physical_page_available(&allocator);
    if (riscv_sv39_user_map_owned_page(&source,
                                       0U,
                                       rejected_page,
                                       RISCV_SV39_READ) !=
            RISCV_SV39_STATUS_INVALID ||
        riscv_sv39_user_map_owned_page(&source,
                                       code_va,
                                       rejected_page,
                                       RISCV_SV39_WRITE) !=
            RISCV_SV39_STATUS_INVALID ||
        source.table_pages != 1U || source.leaf_pages != 0U ||
        physical_page_available(&allocator) != available_before_invalid ||
        physical_page_release(&allocator, rejected_page) !=
            PHYSICAL_PAGE_STATUS_OK) {
        return 48;
    }

    if (physical_page_allocate(&allocator, &code_page) !=
            PHYSICAL_PAGE_STATUS_OK ||
        riscv_sv39_user_map_owned_page(&source,
                                       code_va,
                                       code_page,
                                       RISCV_SV39_READ |
                                           RISCV_SV39_EXECUTE) !=
            RISCV_SV39_STATUS_OK ||
        riscv_sv39_user_lookup(&source, code_va + UINT64_C(0x321),
                               &mapping) != RISCV_SV39_STATUS_OK ||
        mapping.physical_address != code_page + UINT64_C(0x321) ||
        mapping.permissions != (RISCV_SV39_READ |
                                RISCV_SV39_EXECUTE |
                                RISCV_SV39_USER) ||
        source.table_pages != 3U || source.leaf_pages != 1U) {
        return 49;
    }

    if (physical_page_allocate(&allocator, &conflict_page) !=
            PHYSICAL_PAGE_STATUS_OK) {
        return 50;
    }
    available_before_invalid = physical_page_available(&allocator);
    if (riscv_sv39_user_map_owned_page(&source,
                                       code_va,
                                       conflict_page,
                                       RISCV_SV39_READ) !=
            RISCV_SV39_STATUS_CONFLICT ||
        source.table_pages != 3U || source.leaf_pages != 1U ||
        physical_page_available(&allocator) != available_before_invalid ||
        physical_page_release(&allocator, conflict_page) !=
            PHYSICAL_PAGE_STATUS_OK) {
        return 51;
    }

    if (physical_page_allocate(&allocator, &stack_page) !=
            PHYSICAL_PAGE_STATUS_OK ||
        riscv_sv39_user_map_owned_page(&source,
                                       stack_va,
                                       stack_page,
                                       RISCV_SV39_READ |
                                           RISCV_SV39_WRITE) !=
            RISCV_SV39_STATUS_OK ||
        riscv_sv39_user_lookup(&source,
                               stack_va + RISCV_SV39_PAGE_SIZE_4K - 1U,
                               &mapping) != RISCV_SV39_STATUS_OK ||
        mapping.physical_address !=
            stack_page + RISCV_SV39_PAGE_SIZE_4K - 1U ||
        mapping.permissions != (RISCV_SV39_READ |
                                RISCV_SV39_WRITE |
                                RISCV_SV39_USER) ||
        source.table_pages != 5U || source.leaf_pages != 2U ||
        riscv_sv39_user_lookup(&source,
                               stack_va - RISCV_SV39_PAGE_SIZE_4K,
                               &mapping) != RISCV_SV39_STATUS_NOT_MAPPED) {
        return 52;
    }

    if (riscv_sv39_user_space_move(&destination, &source) !=
            RISCV_SV39_STATUS_OK ||
        source.state != RISCV_SV39_USER_SPACE_MOVED ||
        destination.state != RISCV_SV39_USER_SPACE_LIVE ||
        riscv_sv39_user_lookup(&source, code_va, &mapping) !=
            RISCV_SV39_STATUS_STATE ||
        riscv_sv39_user_space_destroy(&source) !=
            RISCV_SV39_STATUS_STATE ||
        riscv_sv39_user_space_destroy(&destination) !=
            RISCV_SV39_STATUS_OK ||
        destination.state != RISCV_SV39_USER_SPACE_DESTROYED ||
        physical_page_available(&allocator) !=
            16U - kernel_table.table_pages ||
        kernel_root[256] != kernel_high_entry ||
        !expect_walk(&kernel_table,
                     kernel_va,
                     UINT64_C(0x200000),
                     RISCV_SV39_PAGE_SIZE_4K,
                     RISCV_SV39_READ | RISCV_SV39_EXECUTE)) {
        return 53;
    }

    return 0;
}

static int test_user_space_oom_rollback(void)
{
    struct boot_memory_layout layout;
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table kernel_table;
    struct riscv_sv39_user_space space = {0};
    uint64_t leaf_page;

    layout.usable_count = 1U;
    layout.usable[0].base = (uint64_t)(uintptr_t)user_space_oom_pool;
    layout.usable[0].size = sizeof(user_space_oom_pool);
    reset_page_table(&kernel_table);
    if (init_test_page_allocator(&allocator, &layout) !=
            PHYSICAL_PAGE_STATUS_OK ||
        riscv_sv39_page_table_init(&kernel_table, &allocator) !=
            RISCV_SV39_STATUS_OK) {
        return 54;
    }
    kernel_table.state = RISCV_SV39_STATE_ACTIVE;
    if (riscv_sv39_user_space_init(&space,
                                   &allocator,
                                   &kernel_table) !=
            RISCV_SV39_STATUS_OK ||
        physical_page_allocate(&allocator, &leaf_page) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_available(&allocator) != 1U) {
        return 55;
    }

    if (riscv_sv39_user_map_owned_page(&space,
                                       UINT64_C(0x10000),
                                       leaf_page,
                                       RISCV_SV39_READ) !=
            RISCV_SV39_STATUS_NO_MEMORY ||
        space.table_pages != 1U || space.leaf_pages != 0U ||
        physical_page_available(&allocator) != 1U ||
        physical_page_release(&allocator, leaf_page) !=
            PHYSICAL_PAGE_STATUS_OK ||
        riscv_sv39_user_space_destroy(&space) !=
            RISCV_SV39_STATUS_OK ||
        physical_page_available(&allocator) != 3U) {
        return 56;
    }

    return 0;
}

int run_sv39_tests(void)
{
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table table;
    struct boot_memory_layout scale_layout;
    int result;
    uint64_t table_pages;
    uint64_t leaf_4k;
    uint64_t leaf_2m;
    uint64_t *root;
    uint64_t *level1;
    uint64_t *level0;
    uint64_t root_entry;
    uint64_t level1_entry;
    uint64_t initial_satp = riscv_sv39_current_satp();

    if (riscv_sv39_switch_satp(initial_satp) != RISCV_SV39_STATUS_OK ||
        riscv_sv39_switch_satp(UINT64_C(1) << 60U) !=
            RISCV_SV39_STATUS_INVALID ||
        riscv_sv39_switch_satp((UINT64_C(8) << 60U) |
                               (UINT64_C(1) << 44U)) !=
            RISCV_SV39_STATUS_INVALID ||
        riscv_sv39_current_satp() != initial_satp) {
        return 57;
    }

    reset_page_table(&table);
    if (!init_allocator(&allocator) ||
        riscv_sv39_page_table_init(&table, &allocator) !=
            RISCV_SV39_STATUS_OK) {
        return 1;
    }

    if (riscv_sv39_map_range(&table,
                             UINT64_C(0x40000000),
                             UINT64_C(0x80000000),
                             RISCV_SV39_PAGE_SIZE_2M,
                             RISCV_SV39_READ | RISCV_SV39_WRITE) !=
        RISCV_SV39_STATUS_OK) {
        return 2;
    }

    root = (uint64_t *)(uintptr_t)table.root_address;
    root_entry = root[1];
    if ((root_entry & UINT64_C(0x3ff)) != UINT64_C(0x1)) {
        return 3;
    }

    level1 = (uint64_t *)(uintptr_t)((root_entry >> 10U) << 12U);
    if (level1[0] != UINT64_C(0x200000c7)) {
        return 4;
    }
    if (table.table_pages != 2U || table.leaf_2m != 1U ||
        table.leaf_4k != 0U || physical_page_available(&allocator) != 6U) {
        return 5;
    }

    if (riscv_sv39_map_range(&table,
                             UINT64_C(0x40200000),
                             UINT64_C(0x90001000),
                             RISCV_SV39_PAGE_SIZE_4K,
                             RISCV_SV39_READ | RISCV_SV39_EXECUTE) !=
        RISCV_SV39_STATUS_OK) {
        return 6;
    }

    level1_entry = level1[1];
    if ((level1_entry & UINT64_C(0x3ff)) != UINT64_C(0x1)) {
        return 7;
    }
    level0 = (uint64_t *)(uintptr_t)((level1_entry >> 10U) << 12U);
    if (level0[0] != UINT64_C(0x2400044b)) {
        return 8;
    }
    if (table.table_pages != 3U || table.leaf_2m != 1U ||
        table.leaf_4k != 1U || physical_page_available(&allocator) != 5U) {
        return 9;
    }

    reset_page_table(&table);
    scale_layout.usable_count = 1U;
    scale_layout.usable[0].base =
        (uint64_t)(uintptr_t)scale_page_pool;
    scale_layout.usable[0].size = sizeof(scale_page_pool);
    if (init_test_page_allocator(&allocator, &scale_layout) !=
            PHYSICAL_PAGE_STATUS_OK ||
        riscv_sv39_page_table_init(&table, &allocator) !=
            RISCV_SV39_STATUS_OK ||
        riscv_sv39_map_range(&table,
                             UINT64_C(0x80000000),
                             UINT64_C(0x80000000),
                             UINT64_C(0x400000000),
                             RISCV_SV39_READ | RISCV_SV39_WRITE) !=
            RISCV_SV39_STATUS_OK) {
        return 10;
    }
    if (table.table_pages != 17U || table.leaf_2m != 8192U ||
        table.leaf_4k != 0U || physical_page_available(&allocator) != 15U) {
        return 11;
    }

    if (riscv_sv39_map_range(&table,
                             UINT64_C(0x4000000000),
                             UINT64_C(0x10000000),
                             RISCV_SV39_PAGE_SIZE_4K,
                             RISCV_SV39_READ) !=
            RISCV_SV39_STATUS_INVALID ||
        riscv_sv39_map_range(&table,
                             UINT64_C(0x20000000),
                             UINT64_C(0x10000000),
                             RISCV_SV39_PAGE_SIZE_4K,
                             RISCV_SV39_WRITE) !=
            RISCV_SV39_STATUS_INVALID ||
        riscv_sv39_map_range(&table,
                             UINT64_C(0x20000001),
                             UINT64_C(0x10000000),
                             RISCV_SV39_PAGE_SIZE_4K,
                             RISCV_SV39_READ) != RISCV_SV39_STATUS_INVALID) {
        return 13;
    }
    table_pages = table.table_pages;
    leaf_4k = table.leaf_4k;
    leaf_2m = table.leaf_2m;
    if (riscv_sv39_map_range(&table,
                             UINT64_C(0x80000000),
                             UINT64_C(0x80000000),
                             RISCV_SV39_PAGE_SIZE_2M,
                             RISCV_SV39_READ | RISCV_SV39_WRITE) !=
            RISCV_SV39_STATUS_CONFLICT ||
        table.table_pages != table_pages || table.leaf_4k != leaf_4k ||
        table.leaf_2m != leaf_2m ||
        table.state != RISCV_SV39_STATE_FAILED) {
        return 12;
    }

    reset_page_table(&table);
    scale_layout.usable[0].base =
        (uint64_t)(uintptr_t)mixed_page_pool;
    scale_layout.usable[0].size = sizeof(mixed_page_pool);
    if (init_test_page_allocator(&allocator, &scale_layout) !=
            PHYSICAL_PAGE_STATUS_OK ||
        riscv_sv39_page_table_init(&table, &allocator) !=
            RISCV_SV39_STATUS_OK ||
        riscv_sv39_map_range(&table,
                             UINT64_C(0x1000),
                             UINT64_C(0x80001000),
                             UINT64_C(0x400000),
                             RISCV_SV39_READ) != RISCV_SV39_STATUS_OK) {
        return 14;
    }
    if (table.table_pages != 4U || table.leaf_2m != 1U ||
        table.leaf_4k != 512U || physical_page_available(&allocator) != 4U) {
        return 15;
    }

    reset_page_table(&table);
    scale_layout.usable[0].base =
        (uint64_t)(uintptr_t)exhausted_page_pool;
    scale_layout.usable[0].size = sizeof(exhausted_page_pool);
    if (init_test_page_allocator(&allocator, &scale_layout) !=
            PHYSICAL_PAGE_STATUS_OK ||
        riscv_sv39_page_table_init(&table, &allocator) !=
            RISCV_SV39_STATUS_OK) {
        return 16;
    }
    if (riscv_sv39_map_range(&table,
                             UINT64_C(0x0),
                             UINT64_C(0x1000),
                             RISCV_SV39_PAGE_SIZE_4K,
                             RISCV_SV39_READ) !=
            RISCV_SV39_STATUS_NO_MEMORY ||
        table.table_pages != 2U || table.leaf_2m != 0U ||
        table.leaf_4k != 0U || physical_page_available(&allocator) != 0U) {
        return 17;
    }

    reset_page_table(&table);
    scale_layout.usable_count = 2U;
    scale_layout.usable[0].base = (uint64_t)(uintptr_t)page_pool;
    scale_layout.usable[0].size = BOAROS_PAGE_SIZE;
    scale_layout.usable[1].base = UINT64_C(1) << 56U;
    scale_layout.usable[1].size = BOAROS_PAGE_SIZE;
    if (init_test_page_allocator(&allocator, &scale_layout) !=
            PHYSICAL_PAGE_STATUS_OK ||
        riscv_sv39_page_table_init(&table, &allocator) !=
            RISCV_SV39_STATUS_INVALID ||
        table.state != RISCV_SV39_STATE_UNINITIALIZED ||
        physical_page_available(&allocator) != 2U) {
        return 18;
    }

    reset_page_table(&table);
    scale_layout.usable_count = 1U;
    scale_layout.usable[0].base =
        (uint64_t)(uintptr_t)exhausted_page_pool;
    scale_layout.usable[0].size = sizeof(exhausted_page_pool);
    if (init_test_page_allocator(&allocator, &scale_layout) !=
            PHYSICAL_PAGE_STATUS_OK ||
        riscv_sv39_page_table_init(&table, &allocator) !=
            RISCV_SV39_STATUS_OK ||
        riscv_sv39_map_range(&table,
                             UINT64_C(0x0),
                             UINT64_C(0x0),
                             UINT64_C(0x40200000),
                             RISCV_SV39_READ) !=
            RISCV_SV39_STATUS_NO_MEMORY ||
        table.state != RISCV_SV39_STATE_FAILED ||
        table.table_pages != 2U || table.leaf_2m != 512U ||
        table.leaf_4k != 0U || physical_page_available(&allocator) != 0U) {
        return 19;
    }

    reset_page_table(&table);
    if (!init_allocator(&allocator) ||
        riscv_sv39_page_table_init(&table, &allocator) !=
            RISCV_SV39_STATUS_OK ||
        riscv_sv39_map_range(&table,
                             UINT64_C(0x200000),
                             UINT64_C(0x200000),
                             RISCV_SV39_PAGE_SIZE_2M,
                             RISCV_SV39_READ) != RISCV_SV39_STATUS_OK) {
        return 20;
    }
    if (riscv_sv39_map_range(&table,
                             UINT64_C(0x0),
                             UINT64_C(0x400000),
                             UINT64_C(0x400000),
                             RISCV_SV39_READ) !=
            RISCV_SV39_STATUS_CONFLICT ||
        table.table_pages != 2U || table.leaf_2m != 2U ||
        table.leaf_4k != 0U) {
        return 21;
    }
    root = (uint64_t *)(uintptr_t)table.root_address;
    level1 = (uint64_t *)(uintptr_t)((root[0] >> 10U) << 12U);
    if (level1[0] != UINT64_C(0x100043) ||
        level1[1] != UINT64_C(0x80043)) {
        return 22;
    }

    result = test_lifecycle_states();
    if (result != 0) {
        return result;
    }

    result = test_user_space_lifecycle();
    if (result != 0) {
        return result;
    }

    result = test_user_space_oom_rollback();
    if (result != 0) {
        return result;
    }

    return test_model_and_boundaries();
}
