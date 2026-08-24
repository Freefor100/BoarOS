#include <arch/riscv/sv39.h>
#include <arch/riscv/user_process.h>
#include <kernel/boot_memory.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>

#include <stddef.h>
#include <stdint.h>

#define TEST_PAGE_COUNT 16U
#define TEST_TEXT_ADDRESS UINT64_C(0x10000)
#define TEST_STACK_ADDRESS UINT64_C(0x20000)

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

static unsigned long run_success_and_move(void)
{
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table kernel_table = {0};
    struct riscv_sv39_user_space space = {0};
    struct riscv_sv39_mapping mapping;
    struct riscv_user_process process = {0};
    struct riscv_user_process moved = {0};
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
    if (riscv_user_process_create(&process, &space) !=
            RISCV_USER_PROCESS_STATUS_OK ||
        process.state != RISCV_USER_PROCESS_LIVE ||
        process.allocator != &allocator ||
        space.state != RISCV_SV39_USER_SPACE_MOVED ||
        physical_page_available(&allocator) + 1U != available ||
        riscv_user_process_satp(&process, &actual_satp) !=
            RISCV_USER_PROCESS_STATUS_OK ||
        actual_satp != expected_satp ||
        riscv_user_process_lookup(&process,
                                  TEST_TEXT_ADDRESS,
                                  &mapping) !=
            RISCV_USER_PROCESS_STATUS_OK ||
        mapping.permissions !=
            (RISCV_SV39_USER | RISCV_SV39_READ |
             RISCV_SV39_EXECUTE)) {
        return 2U;
    }
    mapping.physical_address = UINT64_MAX;
    mapping.permissions = UINT32_MAX;
    if (riscv_user_process_lookup(&process,
                                  UINT64_C(0x30000),
                                  &mapping) !=
            RISCV_USER_PROCESS_STATUS_NOT_MAPPED ||
        mapping.physical_address != UINT64_MAX ||
        mapping.permissions != UINT32_MAX) {
        return 3U;
    }
    if (riscv_user_process_move(&moved, &process) !=
            RISCV_USER_PROCESS_STATUS_OK ||
        moved.state != RISCV_USER_PROCESS_LIVE ||
        process.state != RISCV_USER_PROCESS_MOVED ||
        riscv_user_process_satp(&process, &actual_satp) !=
            RISCV_USER_PROCESS_STATUS_STATE ||
        riscv_user_process_move(&process, &moved) !=
            RISCV_USER_PROCESS_STATUS_STATE) {
        return 4U;
    }
    if (riscv_user_process_destroy(&moved) !=
            RISCV_USER_PROCESS_STATUS_OK ||
        moved.state != RISCV_USER_PROCESS_DESTROYED ||
        physical_page_available(&allocator) != baseline) {
        return 5U;
    }
    return 0U;
}

static unsigned long run_create_access_failures(void)
{
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table kernel_table = {0};
    struct riscv_sv39_user_space space = {0};
    struct riscv_user_process process = {0};
    struct riscv_user_process moved = {0};
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
    if (riscv_user_process_create(&process, &space) !=
            RISCV_USER_PROCESS_STATUS_PAGE_ACCESS ||
        process.state != RISCV_USER_PROCESS_EMPTY ||
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
    process = (struct riscv_user_process){0};
    if (!setup(&allocator, &kernel_table, &baseline) ||
        !create_space(&allocator, &kernel_table, &space)) {
        return 4U;
    }
    available = physical_page_available(&allocator);
    record_address = allocator.ranges[0].next;
    inaccessible_page = record_address;
    if (riscv_user_process_create(&process, &space) !=
            RISCV_USER_PROCESS_STATUS_CLEANUP_REQUIRED ||
        process.state != RISCV_USER_PROCESS_CLEANUP ||
        process.record_page_address != record_address ||
        space.state != RISCV_SV39_USER_SPACE_LIVE ||
        physical_page_available(&allocator) + 1U != available) {
        return 5U;
    }
    if (riscv_user_process_destroy(&process) !=
            RISCV_USER_PROCESS_STATUS_PAGE_RELEASE ||
        process.state != RISCV_USER_PROCESS_CLEANUP) {
        return 6U;
    }
    if (riscv_user_process_move(&moved, &process) !=
            RISCV_USER_PROCESS_STATUS_OK ||
        moved.state != RISCV_USER_PROCESS_CLEANUP ||
        process.state != RISCV_USER_PROCESS_MOVED) {
        return 7U;
    }
    inaccessible_page = UINT64_MAX;
    if (riscv_user_process_destroy(&moved) !=
            RISCV_USER_PROCESS_STATUS_OK ||
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
    struct riscv_user_process process = {0};
    uint64_t baseline;

    if (!setup_pages(&allocator, &kernel_table, &baseline, 6U) ||
        !create_space(&allocator, &kernel_table, &space) ||
        physical_page_available(&allocator) != 0U) {
        return 1U;
    }
    if (riscv_user_process_create(&process, &space) !=
            RISCV_USER_PROCESS_STATUS_NO_MEMORY ||
        process.state != RISCV_USER_PROCESS_EMPTY ||
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
    struct riscv_user_process process = {0};
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
    if (riscv_user_process_create(&process, &space) !=
        RISCV_USER_PROCESS_STATUS_OK) {
        return 2U;
    }
    available = physical_page_available(&allocator);
    record_address = process.record_page_address;
    inaccessible_page = record_address;
    if (riscv_user_process_destroy(&process) !=
            RISCV_USER_PROCESS_STATUS_PAGE_ACCESS ||
        process.state != RISCV_USER_PROCESS_LIVE ||
        physical_page_available(&allocator) != available) {
        return 3U;
    }
    inaccessible_page = root_address;
    if (riscv_user_process_destroy(&process) !=
            RISCV_USER_PROCESS_STATUS_ADDRESS_SPACE ||
        process.state != RISCV_USER_PROCESS_LIVE) {
        return 4U;
    }
    inaccessible_page = UINT64_MAX;
    fail_after_first_page = record_address;
    if (riscv_user_process_destroy(&process) !=
            RISCV_USER_PROCESS_STATUS_CLEANUP_REQUIRED ||
        process.state != RISCV_USER_PROCESS_CLEANUP ||
        physical_page_available(&allocator) + 1U != baseline ||
        riscv_user_process_satp(&process, &satp) !=
            RISCV_USER_PROCESS_STATUS_STATE) {
        return 5U;
    }
    fail_after_first_page = UINT64_MAX;
    if (riscv_user_process_destroy(&process) !=
            RISCV_USER_PROCESS_STATUS_OK ||
        process.state != RISCV_USER_PROCESS_DESTROYED ||
        physical_page_available(&allocator) != baseline) {
        return 6U;
    }
    return 0U;
}

static unsigned long run_invalid_cases(void)
{
    struct riscv_sv39_user_space space = {0};
    struct riscv_user_process process = {0};
    uint64_t satp = UINT64_MAX;

    if (riscv_user_process_create(0, &space) !=
            RISCV_USER_PROCESS_STATUS_INVALID_ARGUMENT ||
        riscv_user_process_create(&process, 0) !=
            RISCV_USER_PROCESS_STATUS_INVALID_ARGUMENT ||
        riscv_user_process_create(&process, &space) !=
            RISCV_USER_PROCESS_STATUS_STATE ||
        riscv_user_process_move(&process, &process) !=
            RISCV_USER_PROCESS_STATUS_INVALID_ARGUMENT ||
        riscv_user_process_satp(&process, &satp) !=
            RISCV_USER_PROCESS_STATUS_STATE ||
        satp != UINT64_MAX ||
        riscv_user_process_destroy(&process) !=
            RISCV_USER_PROCESS_STATUS_STATE) {
        return 1U;
    }
    return 0U;
}

unsigned long run_all_user_process_cases(void)
{
    unsigned long result = run_invalid_cases();

    if (result != 0U) {
        return UINT64_C(0x100) + result;
    }
    result = run_success_and_move();
    if (result != 0U) {
        return UINT64_C(0x200) + result;
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
