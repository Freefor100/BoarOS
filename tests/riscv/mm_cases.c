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
static uint32_t fail_next_page_access;
static uint64_t failed_page;

static void *test_page_access(uint64_t address)
{
    if (fail_next_page_access != 0U) {
        fail_next_page_access = 0U;
        failed_page = address;
        return 0;
    }
    if (address == inaccessible_page) {
        return 0;
    }
    if (address == fail_once_page && fail_once_count == 0U) {
        fail_once_count++;
        return 0;
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
    fail_next_page_access = 0U;
    failed_page = UINT64_MAX;
    layout.usable_count = 1U;
    layout.usable[0].base = (uint64_t)(uintptr_t)page_pool;
    layout.usable[0].size = BOAROS_PAGE_SIZE * page_count;
    if (physical_page_allocator_init(allocator, &layout) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_allocator_bind_access(allocator,
                                            test_page_access) !=
            PHYSICAL_PAGE_STATUS_OK ||
        riscv_sv39_page_table_init(kernel_table, allocator) !=
            RISCV_SV39_STATUS_OK ||
        physical_page_allocator_finalize(allocator) !=
            PHYSICAL_PAGE_STATUS_OK) {
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
    uint64_t owner_id = 0U, shared_id = 0U;

    if (!setup(&allocator, &kernel_table, &baseline) ||
        !create_space(&allocator, &kernel_table, &space) ||
        riscv_kernel_mm_create(&owner, &space) != KERNEL_MM_STATUS_OK) {
        return 1U;
    }
    after_create = physical_page_available(&allocator);
    if (owner.state != KERNEL_MM_LIVE ||
        space.state != RISCV_SV39_USER_SPACE_MOVED ||
        kernel_mm_futex_id(&owner, &owner_id) != KERNEL_MM_STATUS_OK ||
        owner_id == 0U ||
        kernel_mm_acquire(&shared, &owner) != KERNEL_MM_STATUS_OK ||
        kernel_mm_futex_id(&shared, &shared_id) != KERNEL_MM_STATUS_OK ||
        shared_id != owner_id ||
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

static unsigned long run_forked_mm(void)
{
    static const unsigned char text_bytes[] = {
        0x13U, 0x05U, 0xa0U, 0x02U,
    };
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table kernel_table = {0};
    struct riscv_sv39_user_space space = {0};
    struct kernel_mm parent = {0};
    struct kernel_mm child = {0};
    struct kernel_mm_mapping parent_text;
    struct kernel_mm_mapping child_text;
    struct kernel_mm_mapping parent_stack;
    struct kernel_mm_mapping child_stack;
    unsigned char *parent_stack_page;
    unsigned char *child_stack_page;
    uint64_t baseline;
    uint64_t parent_id = 0U, child_id = 0U;

    if (!setup(&allocator, &kernel_table, &baseline) ||
        !create_space(&allocator, &kernel_table, &space) ||
        riscv_sv39_user_space_populate(&space,
                                       TEST_TEXT_ADDRESS,
                                       text_bytes,
                                       sizeof(text_bytes)) !=
            RISCV_SV39_STATUS_OK ||
        riscv_kernel_mm_create(&parent, &space) != KERNEL_MM_STATUS_OK) {
        return 1U;
    }
    {
        enum kernel_mm_status fork_status = kernel_mm_fork(&child, &parent);

        if (fork_status != KERNEL_MM_STATUS_OK) {
            return 20U + (unsigned long)fork_status;
        }
    }
    if (child.state != KERNEL_MM_LIVE) {
        return 7U;
    }
    if (kernel_mm_futex_id(&parent, &parent_id) != KERNEL_MM_STATUS_OK ||
        kernel_mm_futex_id(&child, &child_id) != KERNEL_MM_STATUS_OK ||
        parent_id == 0U || child_id == 0U || parent_id == child_id)
        return 12U;
    if (kernel_mm_lookup(&parent, TEST_TEXT_ADDRESS, &parent_text) !=
        KERNEL_MM_STATUS_OK) {
        return 8U;
    }
    if (kernel_mm_lookup(&child, TEST_TEXT_ADDRESS, &child_text) !=
        KERNEL_MM_STATUS_OK) {
        return 9U;
    }
    if (kernel_mm_lookup(&parent, TEST_STACK_ADDRESS, &parent_stack) !=
        KERNEL_MM_STATUS_OK) {
        return 10U;
    }
    if (kernel_mm_lookup(&child, TEST_STACK_ADDRESS, &child_stack) !=
        KERNEL_MM_STATUS_OK) {
        return 11U;
    }
    if (parent_text.permissions != child_text.permissions ||
        (parent_stack.permissions & KERNEL_MM_WRITE) != 0U ||
        parent_stack.permissions != child_stack.permissions ||
        (parent_text.physical_address & ~BOAROS_PAGE_MASK) !=
            (child_text.physical_address & ~BOAROS_PAGE_MASK) ||
        (parent_stack.physical_address & ~BOAROS_PAGE_MASK) !=
            (child_stack.physical_address & ~BOAROS_PAGE_MASK) ||
        *(unsigned char *)(uintptr_t)parent_text.physical_address !=
            text_bytes[0] ||
        *(unsigned char *)(uintptr_t)child_text.physical_address !=
            text_bytes[0]) {
        return 3U;
    }
    if (physical_page_resolve(&allocator,
                              parent_stack.physical_address &
                                  ~BOAROS_PAGE_MASK,
                              (void **)&parent_stack_page) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_resolve(&allocator,
                              child_stack.physical_address &
                                  ~BOAROS_PAGE_MASK,
                              (void **)&child_stack_page) !=
            PHYSICAL_PAGE_STATUS_OK) {
        return 4U;
    }
    if (parent_stack_page != child_stack_page) {
        return 5U;
    }
    if (kernel_mm_release(&parent) != KERNEL_MM_STATUS_OK ||
        kernel_mm_lookup(&child, TEST_TEXT_ADDRESS, &child_text) !=
            KERNEL_MM_STATUS_OK ||
        kernel_mm_release(&child) != KERNEL_MM_STATUS_OK ||
        physical_page_available(&allocator) != baseline) {
        return 6U;
    }
    return 0U;
}

static unsigned long run_create_access_failures(void)
{
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table kernel_table = {0};
    struct riscv_sv39_user_space space = {0};
    struct kernel_mm mm = {0};
    uint64_t baseline;
    uint64_t available;

    if (!setup(&allocator, &kernel_table, &baseline) ||
        !create_space(&allocator, &kernel_table, &space)) {
        return 1U;
    }
    available = physical_page_available(&allocator);
    fail_next_page_access = 1U;
    if (riscv_kernel_mm_create(&mm, &space) !=
            KERNEL_MM_STATUS_PAGE_ACCESS ||
        mm.state != KERNEL_MM_EMPTY ||
        space.state != RISCV_SV39_USER_SPACE_LIVE ||
        physical_page_available(&allocator) != available) {
        return 2U;
    }
    if (failed_page == UINT64_MAX) {
        return 2U;
    }
    if (riscv_sv39_user_space_destroy(&space) !=
            RISCV_SV39_STATUS_OK ||
        physical_page_available(&allocator) != baseline) {
        return 3U;
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

    if (!setup_pages(&allocator, &kernel_table, &baseline, 7U) ||
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

    if (!setup(&allocator, &kernel_table, &baseline) ||
        !create_space(&allocator, &kernel_table, &space)) {
        return 1U;
    }
    if (riscv_kernel_mm_create(&mm, &space) !=
        KERNEL_MM_STATUS_OK) {
        return 2U;
    }
    available = physical_page_available(&allocator);
    inaccessible_page = mm.record_page_address;
    if (kernel_mm_release(&mm) !=
            KERNEL_MM_STATUS_PAGE_ACCESS ||
        mm.state != KERNEL_MM_LIVE ||
        physical_page_available(&allocator) != available) {
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

void run_mm_resolution_invariant_fatal_case(void)
{
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table kernel_table = {0};
    struct riscv_sv39_user_space space = {0};
    struct kernel_mm mm = {0};
    uint64_t baseline;
    uint64_t blocked_table;

    if (!setup(&allocator, &kernel_table, &baseline) ||
        !create_space(&allocator, &kernel_table, &space) ||
        riscv_sv39_user_map_zeroed_page(
            &space,
            TEST_SECOND_REGION_ADDRESS,
            RISCV_SV39_READ | RISCV_SV39_WRITE) !=
            RISCV_SV39_STATUS_OK) {
        return;
    }
    blocked_table = level0_table_address(&space,
                                         TEST_SECOND_REGION_ADDRESS);
    if (blocked_table == UINT64_MAX ||
        riscv_kernel_mm_create(&mm, &space) != KERNEL_MM_STATUS_OK) {
        return;
    }

    inaccessible_page = blocked_table;
    (void)kernel_mm_release(&mm);
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
    result = run_forked_mm();
    if (result != 0U) {
        return UINT64_C(0x2c0) + result;
    }
    result = run_no_memory();
    if (result != 0U) {
        return UINT64_C(0x300) + result;
    }
    result = run_create_access_failures();
    if (result != 0U) {
        return UINT64_C(0x400) + result;
    }
    result = run_destroy_failures();
    return result == 0U ? 0U : UINT64_C(0x500) + result;
}
