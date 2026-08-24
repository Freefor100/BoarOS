#include "user_test.h"

#include <arch/riscv/sbi.h>
#include <arch/riscv/sv39.h>
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
void __real_kernel_tick_advance(uint64_t elapsed_ticks);

static struct riscv_sv39_page_table *active_kernel_table;
static struct physical_page_allocator *test_allocator;
static volatile uint64_t *user_data;
static uint64_t initial_available;
static uint64_t completion_count;
static uint64_t tick_count;
static uint64_t test_failures;
static uint64_t worker_ran;
static uint64_t user_results_checked;

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
        kernel_user_thread_create(&space,
                                  USER_TEST_CODE_VA,
                                  USER_TEST_STACK_TOP,
                                  USER_TEST_TP_VALUE) !=
            KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT ||
        space.state != RISCV_SV39_USER_SPACE_LIVE ||
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
        RISCV_SV39_STATUS_OK) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    available_before_invalid = physical_page_available(allocator);
    if (kernel_user_thread_create(&space,
                                  USER_TEST_CODE_VA + 1U,
                                  USER_TEST_STACK_TOP,
                                  USER_TEST_TP_VALUE) !=
            KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT ||
        kernel_user_thread_create(&space,
                                  USER_TEST_CODE_VA,
                                  USER_TEST_STACK_TOP - 8U,
                                  USER_TEST_TP_VALUE) !=
            KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT ||
        space.state != RISCV_SV39_USER_SPACE_LIVE ||
        physical_page_available(allocator) != available_before_invalid) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    return kernel_user_thread_create(&space,
                                     USER_TEST_CODE_VA,
                                     USER_TEST_STACK_TOP,
                                     USER_TEST_TP_VALUE);
}

static enum kernel_scheduler_status create_fault_test(
    struct physical_page_allocator *allocator)
{
    struct riscv_sv39_user_space space = {0};
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
        RISCV_SV39_STATUS_OK) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    return kernel_user_thread_create(&space,
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
    if (completion_count != 3U ||
        completion->kind != KERNEL_THREAD_KIND_USER ||
        completion->reason != KERNEL_THREAD_EXIT_SYSCALL ||
        completion->status != USER_TEST_EXIT_STATUS ||
        completion->detail != 0U) {
        test_failures++;
    }
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
            user_data[USER_TEST_FAILURES_OFFSET / sizeof(uint64_t)] != 0U) {
            test_failures++;
        }
    }

    status = __real_kernel_scheduler_reap_one(completion);

    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }
    check_completion(completion);
    if (completion_count == 3U) {
        if (worker_ran == 0U || user_results_checked == 0U ||
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
