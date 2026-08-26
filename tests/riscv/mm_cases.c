#include <arch/riscv/sv39.h>
#include <arch/riscv/mm.h>
#include <kernel/boot_memory.h>
#include <kernel/mm.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>

#include <stddef.h>
#include <stdint.h>

#define TEST_PAGE_COUNT 16U
#define TEST_TEXT_ADDRESS UINT64_C(0x10000)
#define TEST_STACK_ADDRESS UINT64_C(0x20000)
#define TEST_SECOND_REGION_ADDRESS UINT64_C(0x400000)
#define TEST_SV39_INDEX_MASK UINT64_C(0x1ff)

static unsigned char page_pool[BOAROS_PAGE_SIZE * TEST_PAGE_COUNT]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));
static uint64_t inaccessible_page;
static uint64_t fail_once_page;
static uint32_t fail_once_count;
static uint64_t fail_after_first_page;
static uint32_t fail_after_first_count;

static void *test_page_access(uint64_t address)
{
    if (address == inaccessible_page) {
        return 0;
    }
    if (address == fail_once_page && fail_once_count == 0U) {
        fail_once_count++;
        return 0;
    }
    if (address == fail_after_first_page) {
        if (fail_after_first_count != 0U) {
            return 0;
        }
        fail_after_first_count++;
    }
    return (void *)(uintptr_t)address;
}

static int setup_pages(struct physical_page_allocator *allocator,
                       struct riscv_sv39_page_table *kernel_table,
                       uint64_t *baseline,
                       size_t page_count)
{
    struct boot_memory_layout layout;

    inaccessible_page = UINT64_MAX;
    fail_once_page = UINT64_MAX;
    fail_once_count = 0U;
    fail_after_first_page = UINT64_MAX;
    fail_after_first_count = 0U;
    layout.usable_count = 1U;
    layout.usable[0].base = (uint64_t)(uintptr_t)page_pool;
    layout.usable[0].size = BOAROS_PAGE_SIZE * page_count;
    if (physical_page_allocator_init(allocator, &layout) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_allocator_bind_access(allocator,
                                            test_page_access) !=
            PHYSICAL_PAGE_STATUS_OK ||
        riscv_sv39_page_table_init(kernel_table, allocator) !=
            RISCV_SV39_STATUS_OK) {
        return 0;
    }
    kernel_table->state = RISCV_SV39_STATE_ACTIVE;
    *baseline = physical_page_available(allocator);
    return 1;
}

static int setup(struct physical_page_allocator *allocator,
                 struct riscv_sv39_page_table *kernel_table,
                 uint64_t *baseline)
{
    return setup_pages(allocator,
                       kernel_table,
                       baseline,
                       TEST_PAGE_COUNT);
}

static int create_space(struct physical_page_allocator *allocator,
                        const struct riscv_sv39_page_table *kernel_table,
                        struct riscv_sv39_user_space *space)
{
    return riscv_sv39_user_space_init(space,
                                      allocator,
                                      kernel_table) ==
               RISCV_SV39_STATUS_OK &&
           riscv_sv39_user_map_zeroed_page(
               space,
               TEST_TEXT_ADDRESS,
               RISCV_SV39_READ | RISCV_SV39_EXECUTE) ==
               RISCV_SV39_STATUS_OK &&
           riscv_sv39_user_map_zeroed_page(
               space,
               TEST_STACK_ADDRESS,
               RISCV_SV39_READ | RISCV_SV39_WRITE) ==
           RISCV_SV39_STATUS_OK;
}

static uint64_t table_entry_address(uint64_t entry)
{
    return (entry >> 10U) << BOAROS_PAGE_SHIFT;
}

static uint64_t level0_table_address(
    const struct riscv_sv39_user_space *space,
    uint64_t virtual_address)
{
    uint64_t *root = test_page_access(space->root_address);
    uint64_t *level1;
    uint64_t level1_address;
    uint64_t root_index =
        (virtual_address >> 30U) & TEST_SV39_INDEX_MASK;
    uint64_t level1_index =
        (virtual_address >> 21U) & TEST_SV39_INDEX_MASK;

    if (root == 0 || root[root_index] == 0U) {
        return UINT64_MAX;
    }
    level1_address = table_entry_address(root[root_index]);
    level1 = test_page_access(level1_address);
    if (level1 == 0 || level1[level1_index] == 0U) {
        return UINT64_MAX;
    }
    return table_entry_address(level1[level1_index]);
}

static unsigned long run_success_and_move(void)
{
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table kernel_table = {0};
    struct riscv_sv39_user_space space = {0};
    struct kernel_mm_mapping mapping;
    struct kernel_mm mm = {0};
    struct kernel_mm moved = {0};
    uint64_t baseline;
    uint64_t available;
    uint64_t expected_satp;
    uint64_t actual_satp;

    if (!setup(&allocator, &kernel_table, &baseline) ||
        !create_space(&allocator, &kernel_table, &space) ||
        riscv_sv39_user_space_satp(&space, &expected_satp) !=
            RISCV_SV39_STATUS_OK) {
        return 1U;
    }
    available = physical_page_available(&allocator);
    if (riscv_kernel_mm_create(&mm, &space) !=
            KERNEL_MM_STATUS_OK ||
        mm.state != KERNEL_MM_LIVE ||
        mm.allocator != &allocator ||
        space.state != RISCV_SV39_USER_SPACE_MOVED ||
        physical_page_available(&allocator) + 1U != available ||
        riscv_kernel_mm_satp(&mm, &actual_satp) !=
            KERNEL_MM_STATUS_OK ||
        actual_satp != expected_satp ||
        kernel_mm_lookup(&mm,
                                  TEST_TEXT_ADDRESS,
                                  &mapping) !=
            KERNEL_MM_STATUS_OK ||
        mapping.permissions !=
            (KERNEL_MM_USER | KERNEL_MM_READ |
             KERNEL_MM_EXECUTE)) {
        return 2U;
    }
    mapping.physical_address = UINT64_MAX;
    mapping.permissions = UINT32_MAX;
    if (kernel_mm_lookup(&mm,
                                  UINT64_C(0x30000),
                                  &mapping) !=
            KERNEL_MM_STATUS_NOT_MAPPED ||
        mapping.physical_address != UINT64_MAX ||
        mapping.permissions != UINT32_MAX) {
        return 3U;
    }
    if (kernel_mm_move(&moved, &mm) !=
            KERNEL_MM_STATUS_OK ||
        moved.state != KERNEL_MM_LIVE ||
        mm.state != KERNEL_MM_MOVED ||
        riscv_kernel_mm_satp(&mm, &actual_satp) !=
            KERNEL_MM_STATUS_STATE ||
        kernel_mm_move(&mm, &moved) !=
            KERNEL_MM_STATUS_STATE) {
        return 4U;
    }
    if (kernel_mm_release(&moved) !=
            KERNEL_MM_STATUS_OK ||
        moved.state != KERNEL_MM_RELEASED ||
        physical_page_available(&allocator) != baseline) {
        return 5U;
    }
    return 0U;
}

static unsigned long run_shared_mm_references(void)
{
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table kernel_table = {0};
    struct riscv_sv39_user_space space = {0};
    struct kernel_mm owner = {0};
    struct kernel_mm shared = {0};
    struct kernel_mm_mapping mapping;
    uint64_t baseline;
    uint64_t after_create;

    if (!setup(&allocator, &kernel_table, &baseline) ||
        !create_space(&allocator, &kernel_table, &space) ||
        riscv_kernel_mm_create(&owner, &space) != KERNEL_MM_STATUS_OK) {
        return 1U;
    }
    after_create = physical_page_available(&allocator);
    if (owner.state != KERNEL_MM_LIVE ||
        space.state != RISCV_SV39_USER_SPACE_MOVED ||
        kernel_mm_acquire(&shared, &owner) != KERNEL_MM_STATUS_OK ||
        shared.state != KERNEL_MM_LIVE ||
        physical_page_available(&allocator) != after_create) {
        return 2U;
    }
    if (kernel_mm_release(&owner) != KERNEL_MM_STATUS_OK ||
        owner.state != KERNEL_MM_RELEASED ||
        physical_page_available(&allocator) != after_create ||
        kernel_mm_lookup(&shared, TEST_TEXT_ADDRESS, &mapping) !=
            KERNEL_MM_STATUS_OK ||
        mapping.permissions !=
            (KERNEL_MM_USER | KERNEL_MM_READ | KERNEL_MM_EXECUTE)) {
        return 3U;
    }
    if (kernel_mm_release(&shared) != KERNEL_MM_STATUS_OK ||
        shared.state != KERNEL_MM_RELEASED ||
        physical_page_available(&allocator) != baseline) {
        return 4U;
    }
    return 0U;
}

static unsigned long run_create_access_failures(void)
{
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table kernel_table = {0};
    struct riscv_sv39_user_space space = {0};
    struct kernel_mm mm = {0};
    struct kernel_mm moved = {0};
    uint64_t baseline;
    uint64_t available;
    uint64_t record_address;

    if (!setup(&allocator, &kernel_table, &baseline) ||
        !create_space(&allocator, &kernel_table, &space)) {
        return 1U;
    }
    available = physical_page_available(&allocator);
    record_address = allocator.ranges[0].next;
    fail_once_page = record_address;
    if (riscv_kernel_mm_create(&mm, &space) !=
            KERNEL_MM_STATUS_PAGE_ACCESS ||
        mm.state != KERNEL_MM_EMPTY ||
        space.state != RISCV_SV39_USER_SPACE_LIVE ||
        physical_page_available(&allocator) != available) {
        return 2U;
    }
    fail_once_page = UINT64_MAX;
    if (riscv_sv39_user_space_destroy(&space) !=
            RISCV_SV39_STATUS_OK ||
        physical_page_available(&allocator) != baseline) {
        return 3U;
    }

    kernel_table = (struct riscv_sv39_page_table){0};
    space = (struct riscv_sv39_user_space){0};
    mm = (struct kernel_mm){0};
    if (!setup(&allocator, &kernel_table, &baseline) ||
        !create_space(&allocator, &kernel_table, &space)) {
        return 4U;
    }
    available = physical_page_available(&allocator);
    record_address = allocator.ranges[0].next;
    inaccessible_page = record_address;
    if (riscv_kernel_mm_create(&mm, &space) !=
            KERNEL_MM_STATUS_CLEANUP_REQUIRED ||
        mm.state != KERNEL_MM_CLEANUP ||
        mm.record_page_address != record_address ||
        space.state != RISCV_SV39_USER_SPACE_LIVE ||
        physical_page_available(&allocator) + 1U != available) {
        return 5U;
    }
    if (kernel_mm_release(&mm) !=
            KERNEL_MM_STATUS_PAGE_RELEASE ||
        mm.state != KERNEL_MM_CLEANUP) {
        return 6U;
    }
    if (kernel_mm_move(&moved, &mm) !=
            KERNEL_MM_STATUS_OK ||
        moved.state != KERNEL_MM_CLEANUP ||
        mm.state != KERNEL_MM_MOVED) {
        return 7U;
    }
    inaccessible_page = UINT64_MAX;
    if (kernel_mm_release(&moved) !=
            KERNEL_MM_STATUS_OK ||
        riscv_sv39_user_space_destroy(&space) !=
            RISCV_SV39_STATUS_OK ||
        physical_page_available(&allocator) != baseline) {
        return 8U;
    }
    return 0U;
}

static unsigned long run_no_memory(void)
{
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table kernel_table = {0};
    struct riscv_sv39_user_space space = {0};
    struct kernel_mm mm = {0};
    uint64_t baseline;

    if (!setup_pages(&allocator, &kernel_table, &baseline, 6U) ||
        !create_space(&allocator, &kernel_table, &space) ||
        physical_page_available(&allocator) != 0U) {
        return 1U;
    }
    if (riscv_kernel_mm_create(&mm, &space) !=
            KERNEL_MM_STATUS_NO_MEMORY ||
        mm.state != KERNEL_MM_EMPTY ||
        space.state != RISCV_SV39_USER_SPACE_LIVE ||
        physical_page_available(&allocator) != 0U) {
        return 2U;
    }
    if (riscv_sv39_user_space_destroy(&space) !=
            RISCV_SV39_STATUS_OK ||
        physical_page_available(&allocator) != baseline) {
        return 3U;
    }
    return 0U;
}

static unsigned long run_destroy_failures(void)
{
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table kernel_table = {0};
    struct riscv_sv39_user_space space = {0};
    struct kernel_mm mm = {0};
    uint64_t baseline;
    uint64_t available;
    uint64_t record_address;
    uint64_t root_address;
    uint64_t satp;

    if (!setup(&allocator, &kernel_table, &baseline) ||
        !create_space(&allocator, &kernel_table, &space)) {
        return 1U;
    }
    root_address = space.root_address;
    if (riscv_kernel_mm_create(&mm, &space) !=
        KERNEL_MM_STATUS_OK) {
        return 2U;
    }
    available = physical_page_available(&allocator);
    record_address = mm.record_page_address;
    inaccessible_page = record_address;
    if (kernel_mm_release(&mm) !=
            KERNEL_MM_STATUS_PAGE_ACCESS ||
        mm.state != KERNEL_MM_LIVE ||
        physical_page_available(&allocator) != available) {
        return 3U;
    }
    inaccessible_page = root_address;
    if (kernel_mm_release(&mm) !=
            KERNEL_MM_STATUS_ADDRESS_SPACE ||
        mm.state != KERNEL_MM_LIVE) {
        return 4U;
    }
    inaccessible_page = UINT64_MAX;
    fail_after_first_page = record_address;
    if (kernel_mm_release(&mm) !=
            KERNEL_MM_STATUS_CLEANUP_REQUIRED ||
        mm.state != KERNEL_MM_CLEANUP ||
        physical_page_available(&allocator) + 1U != baseline ||
        riscv_kernel_mm_satp(&mm, &satp) !=
            KERNEL_MM_STATUS_STATE) {
        return 5U;
    }
    fail_after_first_page = UINT64_MAX;
    if (kernel_mm_release(&mm) !=
            KERNEL_MM_STATUS_OK ||
        mm.state != KERNEL_MM_RELEASED ||
        physical_page_available(&allocator) != baseline) {
        return 6U;
    }
    return 0U;
}

static unsigned long run_partial_destroy_requires_cleanup(void)
{
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table kernel_table = {0};
    struct riscv_sv39_user_space space = {0};
    struct kernel_mm mm = {0};
    struct kernel_mm_mapping mapping = {
        .physical_address = UINT64_MAX,
        .permissions = UINT32_MAX,
    };
    uint64_t baseline;
    uint64_t blocked_table;
    uint64_t satp = UINT64_MAX;

    if (!setup(&allocator, &kernel_table, &baseline) ||
        !create_space(&allocator, &kernel_table, &space) ||
        riscv_sv39_user_map_zeroed_page(
            &space,
            TEST_SECOND_REGION_ADDRESS,
            RISCV_SV39_READ | RISCV_SV39_WRITE) !=
            RISCV_SV39_STATUS_OK) {
        return 1U;
    }
    blocked_table = level0_table_address(&space,
                                         TEST_SECOND_REGION_ADDRESS);
    if (blocked_table == UINT64_MAX ||
        riscv_kernel_mm_create(&mm, &space) != KERNEL_MM_STATUS_OK) {
        return 2U;
    }

    inaccessible_page = blocked_table;
    if (kernel_mm_release(&mm) != KERNEL_MM_STATUS_CLEANUP_REQUIRED ||
        mm.state != KERNEL_MM_CLEANUP ||
        mm.cleanup_stage != KERNEL_MM_CLEANUP_SPACE ||
        kernel_mm_lookup(&mm, TEST_SECOND_REGION_ADDRESS, &mapping) !=
            KERNEL_MM_STATUS_STATE ||
        riscv_kernel_mm_satp(&mm, &satp) != KERNEL_MM_STATUS_STATE ||
        mapping.physical_address != UINT64_MAX ||
        mapping.permissions != UINT32_MAX || satp != UINT64_MAX) {
        return 3U;
    }
    inaccessible_page = UINT64_MAX;
    if (kernel_mm_release(&mm) != KERNEL_MM_STATUS_OK ||
        mm.state != KERNEL_MM_RELEASED ||
        physical_page_available(&allocator) != baseline) {
        return 4U;
    }
    return 0U;
}

static unsigned long run_invalid_cases(void)
{
    struct riscv_sv39_user_space space = {0};
    struct kernel_mm mm = {0};
    uint64_t satp = UINT64_MAX;

    if (riscv_kernel_mm_create(0, &space) !=
            KERNEL_MM_STATUS_INVALID_ARGUMENT ||
        riscv_kernel_mm_create(&mm, 0) !=
            KERNEL_MM_STATUS_INVALID_ARGUMENT ||
        riscv_kernel_mm_create(&mm, &space) !=
            KERNEL_MM_STATUS_STATE ||
        kernel_mm_move(&mm, &mm) !=
            KERNEL_MM_STATUS_INVALID_ARGUMENT ||
        riscv_kernel_mm_satp(&mm, &satp) !=
            KERNEL_MM_STATUS_STATE ||
        satp != UINT64_MAX ||
        kernel_mm_release(&mm) !=
            KERNEL_MM_STATUS_STATE) {
        return 1U;
    }
    return 0U;
}

unsigned long run_all_mm_cases(void)
{
    unsigned long result = run_invalid_cases();

    if (result != 0U) {
        return UINT64_C(0x100) + result;
    }
    result = run_success_and_move();
    if (result != 0U) {
        return UINT64_C(0x200) + result;
    }
    result = run_shared_mm_references();
    if (result != 0U) {
        return UINT64_C(0x280) + result;
    }
    result = run_no_memory();
    if (result != 0U) {
        return UINT64_C(0x300) + result;
    }
    result = run_create_access_failures();
    if (result != 0U) {
        return UINT64_C(0x400) + result;
    }
    result = run_partial_destroy_requires_cleanup();
    if (result != 0U) {
        return UINT64_C(0x480) + result;
    }
    result = run_destroy_failures();
    return result == 0U ? 0U : UINT64_C(0x500) + result;
}
