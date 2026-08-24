#include <arch/riscv/sbi.h>
#include <arch/riscv/sv39.h>
#include <arch/riscv/user_elf.h>
#include <arch/riscv/user_process.h>
#include <arch/riscv/virt_uart.h>
#include <kernel/physical_page.h>
#include <kernel/scheduler.h>
#include <kernel/tick.h>

#include <stddef.h>
#include <stdint.h>

#define USER_ELF_EXIT_STATUS UINT64_C(0x5a)
#define RISCV_STORE_PAGE_FAULT UINT64_C(15)

extern unsigned char user_elf_program_start[];
extern unsigned char user_elf_program_end[];
extern unsigned char user_elf_fault_program_start[];
extern unsigned char user_elf_fault_program_end[];
extern unsigned char user_elf_guard_program_start[];
extern unsigned char user_elf_guard_program_end[];

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
static uint64_t initial_available;
static uint64_t fault_entry;
static uint64_t guard_fault_address;
static uint64_t completion_count;
static uint64_t tick_count;
static uint64_t test_failures;

static size_t image_size(const unsigned char *start,
                         const unsigned char *end)
{
    uintptr_t first = (uintptr_t)start;
    uintptr_t last = (uintptr_t)end;

    return last > first ? (size_t)(last - first) : 0U;
}

static enum kernel_scheduler_status create_elf_task(
    struct physical_page_allocator *allocator,
    const unsigned char *start,
    const unsigned char *end,
    int fault_kind)
{
    static const char argument_zero[] = "elf-probe";
    static const char argument_one[] = "hello";
    static const char environment_zero[] = "MODE=test";
    struct riscv_user_elf_string arguments[2];
    struct riscv_user_elf_string environment[1];
    struct riscv_user_elf_request request;
    struct riscv_sv39_user_space space = {0};
    struct riscv_user_process process = {0};
    struct riscv_user_elf_entry entry;
    enum riscv_user_elf_status elf_status;
    enum riscv_user_process_status process_status;
    enum kernel_scheduler_status scheduler_status;

    arguments[0].bytes = argument_zero;
    arguments[0].length = sizeof(argument_zero) - 1U;
    arguments[1].bytes = argument_one;
    arguments[1].length = sizeof(argument_one) - 1U;
    environment[0].bytes = environment_zero;
    environment[0].length = sizeof(environment_zero) - 1U;
    request.image = start;
    request.image_size = image_size(start, end);
    request.arguments = arguments;
    request.argument_count = sizeof(arguments) / sizeof(arguments[0]);
    request.environment = environment;
    request.environment_count = sizeof(environment) / sizeof(environment[0]);
    elf_status = riscv_user_elf_load(&request,
                                     allocator,
                                     active_kernel_table,
                                     &space,
                                     &entry);
    if (elf_status != RISCV_USER_ELF_STATUS_OK) {
        test_failures += (uint64_t)elf_status + 1U;
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    if (fault_kind == 1) {
        fault_entry = entry.entry;
    } else if (fault_kind == 2) {
        guard_fault_address = RISCV_USER_ELF_STACK_GUARD_BASE;
    }
    process_status = riscv_user_process_create(&process, &space);
    if (process_status != RISCV_USER_PROCESS_STATUS_OK) {
        if ((process.state == RISCV_USER_PROCESS_LIVE ||
             process.state == RISCV_USER_PROCESS_CLEANUP) &&
            riscv_user_process_destroy(&process) !=
                RISCV_USER_PROCESS_STATUS_OK) {
            return KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE;
        }
        if ((space.state == RISCV_SV39_USER_SPACE_LIVE ||
             space.state == RISCV_SV39_USER_SPACE_CLEANUP) &&
            riscv_sv39_user_space_destroy(&space) !=
                RISCV_SV39_STATUS_OK) {
            return KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE;
        }
        return process_status == RISCV_USER_PROCESS_STATUS_NO_MEMORY
                   ? KERNEL_SCHEDULER_STATUS_NO_MEMORY
                   : KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE;
    }
    scheduler_status = kernel_user_thread_create(&process,
                                                 entry.entry,
                                                 entry.stack_pointer,
                                                 0U);
    if (scheduler_status != KERNEL_SCHEDULER_STATUS_OK &&
        (process.state == RISCV_USER_PROCESS_LIVE ||
         process.state == RISCV_USER_PROCESS_CLEANUP) &&
        riscv_user_process_destroy(&process) !=
            RISCV_USER_PROCESS_STATUS_OK) {
        return KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE;
    }
    return scheduler_status;
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

    if (status != KERNEL_SCHEDULER_STATUS_OK ||
        active_kernel_table == 0) {
        return status != KERNEL_SCHEDULER_STATUS_OK
                   ? status
                   : KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    test_allocator = allocator;
    initial_available = physical_page_available(allocator);
    status = create_elf_task(allocator,
                             user_elf_program_start,
                             user_elf_program_end,
                             0);
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }
    status = create_elf_task(allocator,
                             user_elf_fault_program_start,
                             user_elf_fault_program_end,
                             1);
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }
    return create_elf_task(allocator,
                           user_elf_guard_program_start,
                           user_elf_guard_program_end,
                           2);
}

static void check_completion(
    const struct kernel_thread_completion *completion)
{
    completion_count++;
    if (completion_count == 1U) {
        if (completion->kind != KERNEL_THREAD_KIND_USER ||
            completion->reason != KERNEL_THREAD_EXIT_SYSCALL ||
            completion->status != USER_ELF_EXIT_STATUS ||
            completion->detail != 0U) {
            test_failures++;
        }
        return;
    }
    if (completion_count == 2U) {
        if (completion->kind != KERNEL_THREAD_KIND_USER ||
            completion->reason != KERNEL_THREAD_EXIT_USER_FAULT ||
            completion->status != RISCV_STORE_PAGE_FAULT ||
            completion->detail != fault_entry) {
            test_failures++;
        }
        return;
    }
    if (completion_count != 3U ||
        completion->kind != KERNEL_THREAD_KIND_USER ||
        completion->reason != KERNEL_THREAD_EXIT_USER_FAULT ||
        completion->status != RISCV_STORE_PAGE_FAULT ||
        completion->detail != guard_fault_address) {
        test_failures++;
    }
}

enum kernel_scheduler_status __wrap_kernel_scheduler_reap_one(
    struct kernel_thread_completion *completion)
{
    enum kernel_scheduler_status status =
        __real_kernel_scheduler_reap_one(completion);

    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }
    check_completion(completion);
    if (completion_count == 3U) {
        if (physical_page_available(test_allocator) != initial_available) {
            test_failures++;
        }
        virt_uart_puts("BoarOS: user ELF completions=");
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
    if (tick_count > 24U) {
        virt_uart_puts("BoarOS: user ELF timeout failures=");
        virt_uart_put_hex((unsigned long)(test_failures + 1U));
        virt_uart_putc('\n');
        sbi_shutdown();
    }
}
