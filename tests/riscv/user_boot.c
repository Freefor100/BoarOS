#include "user_test.h"

#include <arch/riscv/sbi.h>
#include <arch/riscv/sv39.h>
#include <arch/riscv/mm.h>
#include <kernel/mm.h>
#include <arch/riscv/virt_uart.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/scheduler.h>
#include <kernel/tick.h>

#include <stddef.h>
#include <stdint.h>

extern unsigned char user_test_payload_start[];
extern unsigned char user_test_payload_end[];
extern unsigned char user_test_fault_payload_start[];
extern unsigned char user_test_fault_payload_end[];

enum riscv_sv39_status __real_riscv_sv39_activate(
    struct riscv_sv39_page_table *table);
enum kernel_scheduler_status __real_kernel_scheduler_init(
    struct physical_page_allocator *allocator,
    uintptr_t idle_stack_low,
    uintptr_t idle_stack_high);
enum kernel_scheduler_status __real_kernel_scheduler_reap_one(
    struct kernel_thread_completion *completion);
enum physical_page_status __real_physical_page_release(
    struct physical_page_allocator *allocator,
    uint64_t address);
void __real_kernel_tick_advance(uint64_t elapsed_ticks);

static struct riscv_sv39_page_table *active_kernel_table;
static struct physical_page_allocator *test_allocator;
static volatile uint64_t *user_data;
static unsigned char *user_uname_first;
static unsigned char *user_uname_second;
static uint64_t initial_available;
static uint64_t completion_count;
static uint64_t tick_count;
static uint64_t test_failures;
static uint64_t worker_ran;
static uint64_t user_results_checked;
static uint64_t normal_record_address;
static uint64_t normal_leaf_address;
static uint64_t fault_record_address;
static uint64_t fail_release_address = UINT64_MAX;
static uint64_t fail_release_once;
static uint64_t fail_thread_after_record;
static uint64_t normal_record_released;
static uint64_t fault_reap_failure_checked;
static uint64_t normal_reap_failures_checked;

struct test_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

static const struct test_utsname expected_utsname = {
    .sysname = "Linux",
    .nodename = "boaros",
    .release = "0.1.0-boaros-dev",
    .version = "#1 BoarOS",
    .machine = "riscv64",
    .domainname = "(none)",
};

_Static_assert(sizeof(struct test_utsname) == USER_TEST_UNAME_SIZE,
               "test utsname must match the Linux ABI");

static void clear_page(void *page)
{
    volatile unsigned char *bytes = page;
    size_t index;

    for (index = 0U; index < BOAROS_PAGE_SIZE; index++) {
        bytes[index] = 0U;
    }
}

static int copy_payload(void *page,
                        const unsigned char *start,
                        const unsigned char *end)
{
    unsigned char *destination = page;
    uintptr_t start_address = (uintptr_t)start;
    uintptr_t end_address = (uintptr_t)end;
    size_t size;
    size_t index;

    if (end_address <= start_address ||
        end_address - start_address > BOAROS_PAGE_SIZE) {
        return 0;
    }
    size = (size_t)(end_address - start_address);
    for (index = 0U; index < size; index++) {
        destination[index] = *(const unsigned char *)(start_address + index);
    }
    return 1;
}

static unsigned char user_uname_byte(size_t index)
{
    size_t first_bytes =
        USER_TEST_UNAME_SECOND_VA - USER_TEST_UNAME_ADDRESS;

    if (index < first_bytes) {
        return user_uname_first[
            (USER_TEST_UNAME_ADDRESS - USER_TEST_UNAME_FIRST_VA) +
            index];
    }
    return user_uname_second[index - first_bytes];
}

static int user_uname_matches(void)
{
    const unsigned char *expected =
        (const unsigned char *)&expected_utsname;
    size_t index;

    if (user_uname_first == 0 || user_uname_second == 0 ||
        user_uname_first[USER_TEST_UNAME_ADDRESS -
                         USER_TEST_UNAME_FIRST_VA - 1U] != 0U ||
        user_uname_second[USER_TEST_UNAME_SIZE -
                          (USER_TEST_UNAME_SECOND_VA -
                           USER_TEST_UNAME_ADDRESS)] != 0U) {
        return 0;
    }
    for (index = 0U; index < sizeof(expected_utsname); index++) {
        if (user_uname_byte(index) != expected[index]) {
            return 0;
        }
    }
    return 1;
}

static int remember_user_uname_pages(const struct kernel_mm *mm)
{
    struct kernel_mm_mapping mapping;
    void *pointer;

    if (kernel_mm_lookup(mm, USER_TEST_UNAME_FIRST_VA, &mapping) !=
            KERNEL_MM_STATUS_OK ||
        physical_page_resolve(mm->allocator,
                              mapping.physical_address &
                                  ~BOAROS_PAGE_MASK,
                              &pointer) != PHYSICAL_PAGE_STATUS_OK) {
        return 0;
    }
    user_uname_first = pointer;
    if (kernel_mm_lookup(mm, USER_TEST_UNAME_SECOND_VA, &mapping) !=
            KERNEL_MM_STATUS_OK ||
        physical_page_resolve(mm->allocator,
                              mapping.physical_address &
                                  ~BOAROS_PAGE_MASK,
                              &pointer) != PHYSICAL_PAGE_STATUS_OK) {
        return 0;
    }
    user_uname_second = pointer;
    return 1;
}

static void kernel_worker(void *argument)
{
    uintptr_t scratch;

    (void)argument;

    __asm__ volatile("csrr %0, sscratch" : "=r"(scratch));
    worker_ran = 1U;
    if (scratch != 0U || user_data == 0 ||
        user_data[USER_TEST_STARTED_OFFSET / sizeof(uint64_t)] !=
            UINT64_C(USER_TEST_STARTED_VALUE)) {
        test_failures++;
    }
    user_data[USER_TEST_FLAG_OFFSET / sizeof(uint64_t)] = 1U;
}

static enum kernel_scheduler_status create_user_test(
    struct physical_page_allocator *allocator)
{
    struct riscv_sv39_user_space space = {0};
    struct kernel_mm mm = {0};
    struct kernel_mm shared = {0};
    uint64_t code_address;
    uint64_t data_address;
    uint64_t available_before_invalid;
    void *code_page;
    void *data_page;

    if (active_kernel_table == 0 ||
        riscv_sv39_user_space_init(&space,
                                   allocator,
                                   active_kernel_table) !=
            RISCV_SV39_STATUS_OK ||
        physical_page_allocate(allocator, &code_address) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_resolve(allocator, code_address, &code_page) !=
            PHYSICAL_PAGE_STATUS_OK) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    clear_page(code_page);
    if (!copy_payload(code_page,
                      user_test_payload_start,
                      user_test_payload_end) ||
        riscv_sv39_user_map_owned_page(
            &space,
            USER_TEST_CODE_VA,
            code_address,
            RISCV_SV39_READ | RISCV_SV39_EXECUTE) !=
            RISCV_SV39_STATUS_OK ||
        physical_page_allocate(allocator, &data_address) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_resolve(allocator, data_address, &data_page) !=
            PHYSICAL_PAGE_STATUS_OK) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    clear_page(data_page);
    user_data = data_page;
    if (riscv_sv39_user_map_owned_page(
            &space,
            USER_TEST_DATA_VA,
            data_address,
            RISCV_SV39_READ | RISCV_SV39_WRITE) !=
            RISCV_SV39_STATUS_OK ||
        riscv_sv39_user_map_zeroed_page(
            &space,
            USER_TEST_SECOND_STACK_VA,
            RISCV_SV39_READ | RISCV_SV39_WRITE) !=
            RISCV_SV39_STATUS_OK ||
        riscv_sv39_user_map_zeroed_page(
            &space,
            USER_TEST_UNAME_FIRST_VA,
            RISCV_SV39_READ | RISCV_SV39_WRITE) !=
            RISCV_SV39_STATUS_OK ||
        riscv_sv39_user_map_zeroed_page(
            &space,
            USER_TEST_UNAME_SECOND_VA,
            RISCV_SV39_READ | RISCV_SV39_WRITE) !=
            RISCV_SV39_STATUS_OK ||
        riscv_kernel_mm_create(&mm, &space) !=
            KERNEL_MM_STATUS_OK ||
        !remember_user_uname_pages(&mm)) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    normal_record_address = mm.record_page_address;
    normal_leaf_address = code_address;
    available_before_invalid = physical_page_available(allocator);
    if (kernel_user_thread_create(&mm,
                                  0,
                                  0,
                                  USER_TEST_CODE_VA + 1U,
                                  USER_TEST_STACK_TOP,
                                  USER_TEST_TP_VALUE) !=
            KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT ||
        kernel_user_thread_create(&mm,
                                  0,
                                  0,
                                  USER_TEST_CODE_VA,
                                  USER_TEST_STACK_TOP - 8U,
                                  USER_TEST_TP_VALUE) !=
            KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT ||
        mm.state != KERNEL_MM_LIVE ||
        physical_page_available(allocator) != available_before_invalid) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    if (kernel_mm_acquire(&shared, &mm) != KERNEL_MM_STATUS_OK ||
        kernel_user_thread_create(&mm,
                                  0,
                                  0,
                                  USER_TEST_CODE_VA,
                                  USER_TEST_STACK_TOP,
                                  USER_TEST_TP_VALUE) !=
            KERNEL_SCHEDULER_STATUS_OK) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    return kernel_user_thread_create(&shared,
                                     0,
                                     0,
                                     USER_TEST_CODE_VA,
                                     USER_TEST_SECOND_STACK_TOP,
                                     USER_TEST_SECOND_TP_VALUE);
}

static enum kernel_scheduler_status create_fault_test(
    struct physical_page_allocator *allocator)
{
    struct riscv_sv39_user_space space = {0};
    struct kernel_mm mm = {0};
    uint64_t code_address;
    uint64_t stack_address;
    void *code_page;
    void *stack_page;

    if (riscv_sv39_user_space_init(&space,
                                   allocator,
                                   active_kernel_table) !=
            RISCV_SV39_STATUS_OK ||
        physical_page_allocate(allocator, &code_address) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_resolve(allocator, code_address, &code_page) !=
            PHYSICAL_PAGE_STATUS_OK) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    clear_page(code_page);
    if (!copy_payload(code_page,
                      user_test_fault_payload_start,
                      user_test_fault_payload_end) ||
        riscv_sv39_user_map_owned_page(
            &space,
            USER_TEST_CODE_VA,
            code_address,
            RISCV_SV39_READ | RISCV_SV39_EXECUTE) !=
            RISCV_SV39_STATUS_OK ||
        physical_page_allocate(allocator, &stack_address) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_resolve(allocator, stack_address, &stack_page) !=
            PHYSICAL_PAGE_STATUS_OK) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    clear_page(stack_page);
    if (riscv_sv39_user_map_owned_page(
            &space,
            USER_TEST_DATA_VA,
            stack_address,
            RISCV_SV39_READ | RISCV_SV39_WRITE) !=
            RISCV_SV39_STATUS_OK ||
        riscv_kernel_mm_create(&mm, &space) !=
            KERNEL_MM_STATUS_OK) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    fault_record_address = mm.record_page_address;
    return kernel_user_thread_create(&mm,
                                     0,
                                     0,
                                     USER_TEST_CODE_VA,
                                     USER_TEST_STACK_TOP,
                                     0U);
}

enum riscv_sv39_status __wrap_riscv_sv39_activate(
    struct riscv_sv39_page_table *table)
{
    enum riscv_sv39_status status = __real_riscv_sv39_activate(table);

    if (status == RISCV_SV39_STATUS_OK) {
        active_kernel_table = table;
    }
    return status;
}

enum kernel_scheduler_status __wrap_kernel_scheduler_init(
    struct physical_page_allocator *allocator,
    uintptr_t idle_stack_low,
    uintptr_t idle_stack_high)
{
    enum kernel_scheduler_status status = __real_kernel_scheduler_init(
        allocator,
        idle_stack_low,
        idle_stack_high);

    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }
    test_allocator = allocator;
    initial_available = physical_page_available(allocator);
    status = create_user_test(allocator);
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }
    status = kernel_thread_create(kernel_worker, 0);
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }
    return create_fault_test(allocator);
}

static void check_completion(
    const struct kernel_thread_completion *completion)
{
    completion_count++;
    if (completion_count == 1U) {
        if (completion->kind != KERNEL_THREAD_KIND_KERNEL ||
            completion->reason != KERNEL_THREAD_EXIT_RETURNED ||
            completion->status != 0U || completion->detail != 0U) {
            test_failures++;
        }
        return;
    }
    if (completion_count == 2U) {
        if (completion->kind != KERNEL_THREAD_KIND_USER ||
            completion->reason != KERNEL_THREAD_EXIT_USER_FAULT ||
            completion->status != 13U || completion->detail != 0U) {
            test_failures++;
        }
        return;
    }
    if ((completion_count != 3U && completion_count != 4U) ||
        completion->kind != KERNEL_THREAD_KIND_USER ||
        completion->reason != KERNEL_THREAD_EXIT_SYSCALL ||
        completion->status != USER_TEST_EXIT_STATUS ||
        completion->detail != 0U) {
        test_failures++;
    }
}

static void set_completion_sentinel(
    struct kernel_thread_completion *completion)
{
    completion->kind = (enum kernel_thread_kind)0x31;
    completion->reason = (enum kernel_thread_exit_reason)0x42;
    completion->status = UINT64_C(0x5364758697a8b9ca);
    completion->detail = UINT64_C(0xdbecfd0e1f203142);
}

static int completion_is_sentinel(
    const struct kernel_thread_completion *completion)
{
    return completion->kind == (enum kernel_thread_kind)0x31 &&
           completion->reason ==
               (enum kernel_thread_exit_reason)0x42 &&
           completion->status == UINT64_C(0x5364758697a8b9ca) &&
           completion->detail == UINT64_C(0xdbecfd0e1f203142);
}

enum physical_page_status __wrap_physical_page_release(
    struct physical_page_allocator *allocator,
    uint64_t address)
{
    enum physical_page_status status;

    if (fail_release_once != 0U && address == fail_release_address) {
        fail_release_once = 0U;
        return PHYSICAL_PAGE_STATUS_INVALID;
    }
    if (fail_thread_after_record != 0U &&
        normal_record_released != 0U &&
        address != normal_record_address) {
        fail_thread_after_record = 0U;
        normal_record_released = 0U;
        return PHYSICAL_PAGE_STATUS_INVALID;
    }
    status = __real_physical_page_release(allocator, address);
    if (status == PHYSICAL_PAGE_STATUS_OK &&
        address == normal_record_address) {
        normal_record_released = 1U;
    }
    return status;
}

enum kernel_scheduler_status __wrap_kernel_scheduler_reap_one(
    struct kernel_thread_completion *completion)
{
    enum kernel_scheduler_status status;

    if (completion_count == 2U) {
        user_results_checked = 1U;
        if (user_data == 0 ||
            user_data[USER_TEST_STARTED_OFFSET / sizeof(uint64_t)] !=
                UINT64_C(USER_TEST_STARTED_VALUE) ||
            user_data[USER_TEST_RESUMED_OFFSET / sizeof(uint64_t)] !=
                UINT64_C(USER_TEST_RESUMED_VALUE) ||
            user_data[USER_TEST_FAILURES_OFFSET / sizeof(uint64_t)] != 0U ||
            user_data[USER_TEST_ID_COUNT_OFFSET / sizeof(uint64_t)] != 2U ||
            user_data[USER_TEST_ID_RECORDS_OFFSET / sizeof(uint64_t)] == 0U ||
            user_data[(USER_TEST_ID_RECORDS_OFFSET + 8U) /
                      sizeof(uint64_t)] !=
                user_data[USER_TEST_ID_RECORDS_OFFSET /
                          sizeof(uint64_t)] ||
            user_data[(USER_TEST_ID_RECORDS_OFFSET +
                       USER_TEST_ID_RECORD_STRIDE) /
                      sizeof(uint64_t)] == 0U ||
            user_data[(USER_TEST_ID_RECORDS_OFFSET +
                       USER_TEST_ID_RECORD_STRIDE + 8U) /
                      sizeof(uint64_t)] !=
                user_data[(USER_TEST_ID_RECORDS_OFFSET +
                           USER_TEST_ID_RECORD_STRIDE) /
                          sizeof(uint64_t)] ||
            user_data[USER_TEST_ID_RECORDS_OFFSET /
                      sizeof(uint64_t)] ==
                user_data[(USER_TEST_ID_RECORDS_OFFSET +
                           USER_TEST_ID_RECORD_STRIDE) /
                          sizeof(uint64_t)] ||
            !user_uname_matches()) {
            test_failures++;
        }
    }

    if (completion_count == 1U && fault_reap_failure_checked == 0U) {
        set_completion_sentinel(completion);
        fail_release_address = fault_record_address;
        fail_release_once = 1U;
        status = __real_kernel_scheduler_reap_one(completion);
        if (status != KERNEL_SCHEDULER_STATUS_PAGE_RELEASE ||
            !completion_is_sentinel(completion)) {
            test_failures++;
        }
        fault_reap_failure_checked = 1U;
    }
    if (completion_count == 3U && normal_reap_failures_checked == 0U) {
        if (normal_record_released != 0U) {
            test_failures++;
        }
        set_completion_sentinel(completion);
        fail_release_address = normal_leaf_address;
        fail_release_once = 1U;
        status = __real_kernel_scheduler_reap_one(completion);
        if (status != KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE ||
            !completion_is_sentinel(completion)) {
            test_failures++;
        }

        set_completion_sentinel(completion);
        fail_thread_after_record = 1U;
        normal_record_released = 0U;
        status = __real_kernel_scheduler_reap_one(completion);
        if (status != KERNEL_SCHEDULER_STATUS_PAGE_RELEASE ||
            !completion_is_sentinel(completion)) {
            test_failures++;
        }
        normal_reap_failures_checked = 1U;
    }

    status = __real_kernel_scheduler_reap_one(completion);

    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }
    check_completion(completion);
    if (completion_count == 4U) {
        if (worker_ran == 0U || user_results_checked == 0U ||
            fault_reap_failure_checked == 0U ||
            normal_reap_failures_checked == 0U ||
            physical_page_available(test_allocator) != initial_available) {
            test_failures++;
        }
        virt_uart_puts("BoarOS: user mode completions=");
        virt_uart_put_hex((unsigned long)completion_count);
        virt_uart_puts(" ticks=");
        virt_uart_put_hex((unsigned long)tick_count);
        virt_uart_puts(" failures=");
        virt_uart_put_hex((unsigned long)test_failures);
        virt_uart_putc('\n');
        sbi_shutdown();
    }
    return status;
}

void __wrap_kernel_tick_advance(uint64_t elapsed_ticks)
{
    tick_count++;
    __real_kernel_tick_advance(elapsed_ticks);
    if (tick_count > 32U) {
        virt_uart_puts("BoarOS: user mode timeout failures=");
        virt_uart_put_hex((unsigned long)(test_failures + 1U));
        virt_uart_putc('\n');
        sbi_shutdown();
    }
}
