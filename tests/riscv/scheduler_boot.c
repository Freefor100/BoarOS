#include <arch/riscv/context.h>
#include <arch/riscv/sbi.h>
#include <arch/riscv/virt_uart.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/scheduler.h>
#include <kernel/tick.h>

#include <stdint.h>

extern unsigned char __boot_stack_bottom[];
extern unsigned char __boot_stack_top[];

void scheduler_test_worker_a(void *argument);
void scheduler_test_worker_b(void *argument);

enum kernel_scheduler_status __real_kernel_scheduler_init(
    struct physical_page_allocator *allocator,
    uintptr_t idle_stack_low,
    uintptr_t idle_stack_high);
enum kernel_scheduler_status __wrap_kernel_scheduler_init(
    struct physical_page_allocator *allocator,
    uintptr_t idle_stack_low,
    uintptr_t idle_stack_high);
void __real_kernel_tick_advance(uint64_t elapsed_ticks);
void __wrap_kernel_tick_advance(uint64_t elapsed_ticks);

volatile uint64_t scheduler_test_finish;
volatile uint64_t scheduler_test_worker_failures;

static struct physical_page_allocator *test_allocator;
static uint64_t initial_available;
static uintptr_t idle_thread;
static uintptr_t worker_sp[2];
static uintptr_t worker_tp[2];
static uint64_t started_mask;
static uint64_t done_mask;
static uint64_t order;
static uint64_t interrupt_entries;
static uint64_t test_failures;
static uint64_t init_seen;

static uintptr_t current_sp(void)
{
    uintptr_t value;

    __asm__ volatile("mv %0, sp" : "=r"(value));
    return value;
}

static void append_order(uint64_t value)
{
    if (order > (UINT64_MAX >> 4)) {
        test_failures++;
        return;
    }
    order = (order << 4) | value;
}

void scheduler_test_worker_started(uint64_t worker,
                                   uintptr_t stack_pointer,
                                   uintptr_t thread_pointer)
{
    uint64_t bit;

    if (worker >= 2U) {
        test_failures++;
        return;
    }
    bit = UINT64_C(1) << worker;
    if ((started_mask & bit) != 0U) {
        test_failures++;
        return;
    }
    worker_sp[worker] = stack_pointer;
    worker_tp[worker] = thread_pointer;
    started_mask |= bit;
    append_order(worker + 1U);
}

void scheduler_test_worker_resumed(uint64_t worker,
                                   uintptr_t stack_pointer,
                                   uintptr_t thread_pointer)
{
    uint64_t bit;

    if (worker >= 2U) {
        test_failures++;
        return;
    }
    bit = UINT64_C(1) << worker;
    if ((started_mask & bit) == 0U || (done_mask & bit) != 0U ||
        worker_sp[worker] != stack_pointer ||
        worker_tp[worker] != thread_pointer) {
        test_failures++;
    }
    done_mask |= bit;
    append_order(worker + 1U);
}

enum kernel_scheduler_status __wrap_kernel_scheduler_init(
    struct physical_page_allocator *allocator,
    uintptr_t idle_stack_low,
    uintptr_t idle_stack_high)
{
    enum kernel_scheduler_status status;

    status = __real_kernel_scheduler_init(allocator,
                                          idle_stack_low,
                                          idle_stack_high);
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }

    init_seen = 1U;
    test_allocator = allocator;
    initial_available = physical_page_available(allocator);
    idle_thread = (uintptr_t)riscv_current_thread_get();
    if (idle_thread == 0U ||
        kernel_thread_create(scheduler_test_worker_a, 0) !=
            KERNEL_SCHEDULER_STATUS_OK ||
        kernel_thread_create(scheduler_test_worker_b, 0) !=
            KERNEL_SCHEDULER_STATUS_OK) {
        test_failures++;
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}

static int scheduler_test_complete(void)
{
    uintptr_t stack_pointer = current_sp();
    uintptr_t current_thread = (uintptr_t)riscv_current_thread_get();

    if (init_seen == 0U || test_allocator == 0 || done_mask != 3U ||
        order != UINT64_C(0x1212) || scheduler_test_worker_failures != 0U) {
        return 0;
    }
    if (worker_tp[0] == 0U || worker_tp[1] == 0U ||
        worker_tp[0] == worker_tp[1] || worker_tp[0] == idle_thread ||
        worker_tp[1] == idle_thread ||
        (worker_sp[0] & ~(uintptr_t)BOAROS_PAGE_MASK) ==
            (worker_sp[1] & ~(uintptr_t)BOAROS_PAGE_MASK) ||
        (worker_sp[0] >= (uintptr_t)__boot_stack_bottom &&
         worker_sp[0] < (uintptr_t)__boot_stack_top) ||
        (worker_sp[1] >= (uintptr_t)__boot_stack_bottom &&
         worker_sp[1] < (uintptr_t)__boot_stack_top)) {
        test_failures++;
        return 0;
    }
    if (current_thread != idle_thread ||
        stack_pointer < (uintptr_t)__boot_stack_bottom ||
        stack_pointer >= (uintptr_t)__boot_stack_top ||
        physical_page_available(test_allocator) != initial_available) {
        return 0;
    }
    return 1;
}

void __wrap_kernel_tick_advance(uint64_t elapsed_ticks)
{
    interrupt_entries++;
    if (elapsed_ticks == 0U) {
        test_failures++;
    }
    __real_kernel_tick_advance(elapsed_ticks);

    if (started_mask == 3U && scheduler_test_finish == 0U) {
        scheduler_test_finish = 1U;
    }
    if (scheduler_test_complete()) {
        virt_uart_puts("BoarOS: scheduler preemption order=");
        virt_uart_put_hex((unsigned long)order);
        virt_uart_puts(" workers=");
        virt_uart_put_hex((unsigned long)done_mask);
        virt_uart_puts(" reaped=0x2 failures=");
        virt_uart_put_hex((unsigned long)test_failures);
        virt_uart_putc('\n');
        sbi_shutdown();
    }
    if (interrupt_entries > 12U) {
        test_failures++;
        virt_uart_puts("BoarOS: scheduler preemption timeout order=");
        virt_uart_put_hex((unsigned long)order);
        virt_uart_puts(" started=");
        virt_uart_put_hex((unsigned long)started_mask);
        virt_uart_puts(" done=");
        virt_uart_put_hex((unsigned long)done_mask);
        virt_uart_puts(" failures=");
        virt_uart_put_hex((unsigned long)(test_failures +
                                         scheduler_test_worker_failures));
        virt_uart_putc('\n');
        sbi_shutdown();
    }
}
