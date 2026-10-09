#include <arch/riscv/context.h>
#include <arch/riscv/fpu.h>
#include <arch/riscv/sbi.h>
#include <arch/riscv/timer.h>
#include <arch/riscv/thread.h>
#include <arch/riscv/trap.h>
#include <arch/riscv/virt_uart.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/pid.h>
#include <kernel/scheduler.h>
#include <kernel/task.h>
#include <kernel/sync.h>
#include "../../kernel/sched/private.h"

#include <stdint.h>

extern unsigned char __boot_stack_bottom[];
extern unsigned char __boot_stack_top[];

#define TEST_PHYSICAL_BASE UINT64_C(0x41000000)
#define TEST_THREAD_COUNT 2U
#define TEST_TASK_PAGES (1U + KERNEL_STACK_PAGES)
/* One buddy metadata page plus enough alignment slack for two tasks. */
#define TEST_PAGE_COUNT (2U + TEST_THREAD_COUNT * TEST_TASK_PAGES)

/* Later concurrency cases use a fresh allocator with room for 32 waiters
 * plus the four concurrent deadline waiters and their retained pages. */
static unsigned char page_pool[BOAROS_PAGE_SIZE * (TEST_PAGE_COUNT + 44U * TEST_TASK_PAGES)]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));
static unsigned long access_calls_before_failure;
static unsigned long fail_access_count;
static int fail_stack_access;
static struct physical_page_allocator *test_page_allocator;
static uintptr_t entry_sp[TEST_THREAD_COUNT];
static void *entry_tp[TEST_THREAD_COUNT];
static uintptr_t entry_order[TEST_THREAD_COUNT];
static unsigned long entry_count;

_Static_assert(RISCV_THREAD_STATE_KERNEL_SP ==
                   offsetof(struct riscv_thread_state, kernel_sp),
               "RISC-V thread kernel-sp offset mismatch");
_Static_assert(RISCV_THREAD_STATE_USER_SP ==
                   offsetof(struct riscv_thread_state, user_sp),
               "RISC-V thread user-sp offset mismatch");
_Static_assert(RISCV_THREAD_STATE_USER_MODE ==
                   offsetof(struct riscv_thread_state, user_mode),
               "RISC-V thread user-mode offset mismatch");
_Static_assert(RISCV_THREAD_STATE_SATP ==
                   offsetof(struct riscv_thread_state, satp),
               "RISC-V thread satp offset mismatch");
_Static_assert(RISCV_THREAD_STATE_SIZE ==
                   sizeof(struct riscv_thread_state),
               "RISC-V thread state size mismatch");

static void *scheduler_page_access(uint64_t physical_address)
{
    if (fail_stack_access) {
        uint32_t order;
        if (physical_page_allocation_order(test_page_allocator, physical_address,
                                            &order) == PHYSICAL_PAGE_STATUS_OK &&
            order == KERNEL_STACK_ORDER) {
            fail_stack_access = 0;
            return 0;
        }
    }
    if (fail_access_count != 0U) {
        if (access_calls_before_failure == 0U) {
            fail_access_count--;
            return 0;
        }
        access_calls_before_failure--;
    }
    if (physical_address < TEST_PHYSICAL_BASE ||
        physical_address - TEST_PHYSICAL_BASE >= sizeof(page_pool)) {
        return 0;
    }

    return &page_pool[physical_address - TEST_PHYSICAL_BASE];
}

static void thread_entry(void *argument)
{
    uintptr_t stack_pointer;
    unsigned long index = entry_count;

    __asm__ volatile("mv %0, sp" : "=r"(stack_pointer));
    if (index < TEST_THREAD_COUNT) {
        entry_sp[index] = stack_pointer;
        entry_tp[index] = riscv_current_thread_get();
        entry_order[index] = (uintptr_t)argument;
    }
    entry_count++;
}

static unsigned long expect_status(
    enum kernel_scheduler_status expected,
    enum kernel_scheduler_status actual)
{
    return expected != actual;
}

static unsigned long run_pid_cases(void)
{
    uint64_t bitmap[KERNEL_PID_BITMAP_WORDS(3U)] = {UINT64_MAX};
    struct kernel_pid_allocator allocator = {0};
    kernel_pid_t first = -1;
    kernel_pid_t second = -1;
    kernel_pid_t third = -1;
    kernel_pid_t unchanged = 77;

    if (kernel_pid_allocator_init(&allocator, bitmap, 3U) !=
            KERNEL_PID_STATUS_OK ||
        kernel_pid_allocate(&allocator, &first) != KERNEL_PID_STATUS_OK ||
        kernel_pid_allocate(&allocator, &second) != KERNEL_PID_STATUS_OK ||
        kernel_pid_allocate(&allocator, &third) != KERNEL_PID_STATUS_OK ||
        first != 1 || second != 2 || third != 3) {
        return 1U;
    }
    if (kernel_pid_allocate(&allocator, &unchanged) !=
            KERNEL_PID_STATUS_EXHAUSTED ||
        unchanged != 77) {
        return 2U;
    }
    if (kernel_pid_release(&allocator, second) != KERNEL_PID_STATUS_OK ||
        kernel_pid_allocate(&allocator, &second) != KERNEL_PID_STATUS_OK ||
        second != 2) {
        return 3U;
    }
    if (kernel_pid_release(&allocator, first) != KERNEL_PID_STATUS_OK ||
        kernel_pid_release(&allocator, second) != KERNEL_PID_STATUS_OK ||
        kernel_pid_release(&allocator, third) != KERNEL_PID_STATUS_OK ||
        kernel_pid_release(&allocator, second) !=
            KERNEL_PID_STATUS_NOT_ALLOCATED ||
        kernel_pid_release(&allocator, 0) != KERNEL_PID_STATUS_INVALID ||
        kernel_pid_release(&allocator, 4) != KERNEL_PID_STATUS_INVALID) {
        return 4U;
    }
    return 0U;
}

static unsigned long run_preinit_cases(void)
{
    struct kernel_thread_completion completion = {
        .kind = (enum kernel_thread_kind)0x11,
        .reason = (enum kernel_thread_exit_reason)0x22,
        .status = UINT64_C(0x33445566778899aa),
        .detail = UINT64_C(0xbbccddeeff001122),
    };
    struct kernel_wait_queue queue = {0};
    enum kernel_wait_wake_reason reason = KERNEL_WAIT_WOKEN;
    unsigned long failures = 0U;

    kernel_wait_queue_init(&queue);
    failures += expect_status(KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED,
                              kernel_thread_create(thread_entry, 0));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED,
                              kernel_scheduler_on_tick(1U));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED,
                              kernel_scheduler_reap_one(&completion));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED,
                              kernel_wait_queue_wake_one(&queue));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED,
                              kernel_scheduler_block_current(&queue,
                                                             0U,
                                                             0,
                                                             &reason));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED,
                              kernel_scheduler_expire_deadlines(1U));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED,
                              kernel_scheduler_yield_current());
    if (reason != KERNEL_WAIT_WOKEN) {
        failures++;
    }
    if (completion.kind != (enum kernel_thread_kind)0x11 ||
        completion.reason != (enum kernel_thread_exit_reason)0x22 ||
        completion.status != UINT64_C(0x33445566778899aa) ||
        completion.detail != UINT64_C(0xbbccddeeff001122)) {
        failures++;
    }

    return failures;
}

static unsigned long run_init_cases(
    struct physical_page_allocator *allocator)
{
    uintptr_t stack_low = (uintptr_t)__boot_stack_bottom;
    uintptr_t stack_high = (uintptr_t)__boot_stack_top;
    void *idle_thread;
    unsigned long failures = 0U;

    failures += expect_status(KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT,
                              kernel_scheduler_init(0,
                                                    stack_low,
                                                    stack_high));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT,
                              kernel_scheduler_init(allocator,
                                                    stack_high,
                                                    stack_low));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT,
                              kernel_scheduler_init(allocator,
                                                    stack_low,
                                                    stack_low));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT,
                              kernel_scheduler_init(allocator,
                                                    stack_low + 1U,
                                                    stack_high));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT,
                              kernel_scheduler_init(allocator,
                                                    stack_high + 16U,
                                                    stack_high + 32U));

    __asm__ volatile("csrsi sstatus, 2" ::: "memory");
    failures += expect_status(KERNEL_SCHEDULER_STATUS_INVALID_STATE,
                              kernel_scheduler_init(allocator,
                                                    stack_low,
                                                    stack_high));
    __asm__ volatile("csrci sstatus, 2" ::: "memory");

    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_init(allocator,
                                                    stack_low,
                                                    stack_high));
    idle_thread = riscv_current_thread_get();
    if (idle_thread == 0) {
        failures++;
    }
    failures += expect_status(KERNEL_SCHEDULER_STATUS_ALREADY_INITIALIZED,
                              kernel_scheduler_init(allocator,
                                                    stack_low,
                                                    stack_high));
    if (riscv_current_thread_get() != idle_thread) {
        failures++;
    }

    return failures;
}

static unsigned long run_idle_cases(void)
{
    struct kernel_thread_completion completion = {
        .kind = (enum kernel_thread_kind)0x33,
        .reason = (enum kernel_thread_exit_reason)0x44,
        .status = UINT64_C(0x5566778899aabbcc),
        .detail = UINT64_C(0xddeeff0011223344),
    };
    unsigned long failures = 0U;

    failures += expect_status(KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT,
                              kernel_thread_create(0, 0));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_on_tick(0U));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_on_tick(1U));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_EMPTY,
                              kernel_scheduler_reap_one(&completion));
    if (completion.kind != (enum kernel_thread_kind)0x33 ||
        completion.reason != (enum kernel_thread_exit_reason)0x44 ||
        completion.status != UINT64_C(0x5566778899aabbcc) ||
        completion.detail != UINT64_C(0xddeeff0011223344)) {
        failures++;
    }

    __asm__ volatile("csrsi sstatus, 2" ::: "memory");
    failures += expect_status(KERNEL_SCHEDULER_STATUS_INVALID_STATE,
                              kernel_scheduler_on_tick(1U));
    completion.status = UINT64_C(0x8877665544332211);
    failures += expect_status(KERNEL_SCHEDULER_STATUS_INVALID_STATE,
                              kernel_scheduler_reap_one(&completion));
    __asm__ volatile("csrci sstatus, 2" ::: "memory");
    if (completion.status != UINT64_C(0x8877665544332211)) {
        failures++;
    }

    return failures;
}

static unsigned long run_create_cases(
    struct physical_page_allocator *allocator)
{
    uint64_t initial_available = physical_page_available(allocator);
    struct kernel_thread_completion completion = {
        .kind = (enum kernel_thread_kind)0x21,
        .reason = (enum kernel_thread_exit_reason)0x43,
        .status = UINT64_C(0x65768798a9bacbdc),
        .detail = UINT64_C(0xedfe0f1021324354),
    };
    unsigned long failures = 0U;

    access_calls_before_failure = 0U;
    fail_access_count = 1U;
    failures += expect_status(KERNEL_SCHEDULER_STATUS_PAGE_ACCESS,
                              kernel_thread_create(thread_entry, 0));
    if (physical_page_available(allocator) != initial_available) {
        failures++;
    }

    /* A stack page that cannot be mapped must release both allocations. */
    fail_stack_access = 1;
    failures += expect_status(KERNEL_SCHEDULER_STATUS_PAGE_ACCESS,
                              kernel_thread_create(thread_entry, 0));
    if (fail_stack_access != 0 ||
        physical_page_available(allocator) != initial_available)
        failures++;

    /* One free page cannot satisfy metadata plus its independent stack.
     * A failed creation must return that first allocation immediately. */
    uint64_t held[TEST_PAGE_COUNT - 1U];
    for (unsigned i = 0U; i < initial_available - 1U; i++) {
        if (physical_page_allocate(allocator, &held[i]) !=
            PHYSICAL_PAGE_STATUS_OK) return failures + 1U;
    }
    failures += expect_status(KERNEL_SCHEDULER_STATUS_NO_MEMORY,
                              kernel_thread_create(thread_entry, 0));
    if (physical_page_available(allocator) != 1U) failures++;
    for (unsigned i = 0U; i < initial_available - 1U; i++)
        (void)physical_page_release(allocator, held[i]);

    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_thread_create(thread_entry,
                                                   (void *)(uintptr_t)1U));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_thread_create(thread_entry,
                                                   (void *)(uintptr_t)2U));
    if (physical_page_available(allocator) !=
        initial_available - TEST_THREAD_COUNT * TEST_TASK_PAGES) {
        failures++;
    }
    failures += expect_status(KERNEL_SCHEDULER_STATUS_NO_MEMORY,
                              kernel_thread_create(thread_entry, 0));
    if (physical_page_available(allocator) !=
        initial_available - TEST_THREAD_COUNT * TEST_TASK_PAGES) {
        failures++;
    }

    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_on_tick(1U));
    /* Each exit returns to the cleanup context before the next dispatch. */
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_on_tick(1U));
    if (entry_count != TEST_THREAD_COUNT ||
        entry_order[0] != 1U || entry_order[1] != 2U ||
        entry_tp[0] == 0 || entry_tp[1] == 0 ||
        entry_tp[0] == entry_tp[1] ||
        ((entry_sp[0] - 1U) & ~(uintptr_t)BOAROS_PAGE_MASK) ==
            ((uintptr_t)entry_tp[0] & ~(uintptr_t)BOAROS_PAGE_MASK) ||
        (entry_sp[0] & ~(uintptr_t)BOAROS_PAGE_MASK) ==
            (entry_sp[1] & ~(uintptr_t)BOAROS_PAGE_MASK)) {
        failures++;
    }
    completion.kind = (enum kernel_thread_kind)0x55;
    completion.reason = (enum kernel_thread_exit_reason)0x66;
    completion.status = UINT64_MAX;
    completion.detail = UINT64_MAX;
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_reap_one(&completion));
    if (completion.kind != KERNEL_THREAD_KIND_KERNEL ||
        completion.reason != KERNEL_THREAD_EXIT_RETURNED ||
        completion.status != 0U || completion.detail != 0U ||
        physical_page_available(allocator) != initial_available - TEST_TASK_PAGES) {
        failures++;
    }

    completion.kind = (enum kernel_thread_kind)0x77;
    completion.reason = (enum kernel_thread_exit_reason)0x88;
    completion.status = UINT64_MAX;
    completion.detail = UINT64_MAX;
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_reap_one(&completion));
    if (completion.kind != KERNEL_THREAD_KIND_KERNEL ||
        completion.reason != KERNEL_THREAD_EXIT_RETURNED ||
        completion.status != 0U || completion.detail != 0U ||
        physical_page_available(allocator) != initial_available) {
        failures++;
    }

    completion.kind = (enum kernel_thread_kind)0x99;
    completion.reason = (enum kernel_thread_exit_reason)0xaa;
    completion.status = UINT64_C(0xbbccddeeff001122);
    completion.detail = UINT64_C(0x33445566778899aa);
    failures += expect_status(KERNEL_SCHEDULER_STATUS_EMPTY,
                              kernel_scheduler_reap_one(&completion));
    if (completion.kind != (enum kernel_thread_kind)0x99 ||
        completion.reason != (enum kernel_thread_exit_reason)0xaa ||
        completion.status != UINT64_C(0xbbccddeeff001122) ||
        completion.detail != UINT64_C(0x33445566778899aa) ||
        physical_page_available(allocator) != initial_available) {
        failures++;
    }

    return failures;
}

static struct kernel_wait_queue test_queue;
static uint64_t wait_case_deadline;
static volatile unsigned long timeout_wake_count;
static volatile unsigned long timeout_reason_value;
static volatile unsigned long event_wake_count;
static volatile unsigned long event_reason_value;
static volatile unsigned long signal_wake_count;
static volatile unsigned long signal_reason_value;
static volatile struct kernel_task *signal_waiter;
static volatile unsigned long yield_runs;

static void blocked_worker(void *argument)
{
    unsigned long slot = (uintptr_t)argument;
    enum kernel_wait_wake_reason reason = (enum kernel_wait_wake_reason)0xF0;
    uintptr_t saved = riscv_interrupt_save();

    if (slot == 2U) {
        signal_waiter = kernel_task_current();
    }
    if (kernel_scheduler_block_current(&test_queue,
                                       slot == 0U ? wait_case_deadline : 0U,
                                       slot == 2U ? 1 : 0,
                                       &reason) !=
        KERNEL_SCHEDULER_STATUS_OK) {
        riscv_interrupt_restore(saved);
        return;
    }
    riscv_interrupt_restore(saved);
    if (slot == 0U) {
        timeout_reason_value = (unsigned long)reason;
        timeout_wake_count++;
    } else if (slot == 1U) {
        event_reason_value = (unsigned long)reason;
        event_wake_count++;
    } else {
        signal_reason_value = (unsigned long)reason;
        signal_wake_count++;
    }
}

static void yielding_worker(void *argument)
{
    unsigned long slot = (uintptr_t)argument;
    uintptr_t saved = riscv_interrupt_save();

    if (slot == 0U) {
        kernel_scheduler_yield_current();
    }
    riscv_interrupt_restore(saved);
    yield_runs++;
}

static volatile unsigned long charged_user;
static volatile unsigned long charged_kernel;

static void charging_worker(void *argument)
{
    uint64_t user_ticks = 0;
    uint64_t kernel_ticks = 0;
    uint64_t child_user = 0;
    uint64_t child_kernel = 0;

    (void)argument;
    kernel_scheduler_charge_ticks(5U, 1);
    kernel_scheduler_charge_ticks(3U, 0);
    kernel_task_cpu_ticks(kernel_task_current(),
                          &user_ticks,
                          &kernel_ticks,
                          &child_user,
                          &child_kernel);
    charged_user = (unsigned long)user_ticks;
    charged_kernel = (unsigned long)kernel_ticks;
}

static unsigned long run_accounting_cases(
    struct physical_page_allocator *allocator)
{
    uint64_t initial_available = physical_page_available(allocator);
    struct kernel_thread_completion completion = {
        .kind = (enum kernel_thread_kind)0,
        .reason = (enum kernel_thread_exit_reason)0,
        .status = 0U,
        .detail = 0U,
    };
    unsigned long failures = 0U;

    /* Charging with no published current is a no-op. */
    kernel_scheduler_charge_ticks(7U, 1);

    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_thread_create(charging_worker, 0));
    uint64_t loads[3];
    uint16_t tasks;
    kernel_scheduler_system_statistics(loads, &tasks);
    if (tasks != 1) failures++;
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_on_tick(501U));
    kernel_scheduler_system_statistics(loads, &tasks);
    if (tasks != 1 || !loads[0] || !loads[1] || !loads[2] || loads[0] <= loads[1]) failures++;
    if (charged_user != 5U || charged_kernel != 3U) {
        failures++;
    }
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_reap_one(&completion));
    if (completion.kind != KERNEL_THREAD_KIND_KERNEL ||
        completion.reason != KERNEL_THREAD_EXIT_RETURNED ||
        completion.status != 0U || completion.detail != 0U ||
        physical_page_available(allocator) != initial_available) {
        failures++;
    }

    kernel_scheduler_system_statistics(loads, &tasks);
    if (tasks) failures++;
    return failures;
}

static struct kernel_wait_queue load_queue;
static void load_waiter(void *argument)
{
    enum kernel_wait_wake_reason reason;
    (void)riscv_interrupt_save();
    if (kernel_scheduler_block_current(&load_queue, 0, (int)(uintptr_t)argument, &reason)
            != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
}
static unsigned long run_load_wait_cases(struct physical_page_allocator *allocator)
{
    uint64_t available = physical_page_available(allocator), before[3], after[3];
    uint16_t tasks;
    unsigned long failures = 0;
    struct kernel_thread_completion completion;
    kernel_wait_queue_init(&load_queue);
    for (unsigned interruptible = 1;; interruptible--) {
        failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
            kernel_thread_create(load_waiter, (void *)(uintptr_t)interruptible));
        failures += expect_status(KERNEL_SCHEDULER_STATUS_OK, kernel_scheduler_on_tick(1));
        kernel_scheduler_system_statistics(before, &tasks);
        if (tasks != 1) failures++;
        failures += expect_status(KERNEL_SCHEDULER_STATUS_OK, kernel_scheduler_on_tick(501));
        kernel_scheduler_system_statistics(after, &tasks);
        if (interruptible ? after[0] >= before[0] : after[0] <= before[0]) failures++;
        failures += expect_status(KERNEL_SCHEDULER_STATUS_OK, kernel_wait_queue_wake_one(&load_queue));
        failures += expect_status(KERNEL_SCHEDULER_STATUS_OK, kernel_scheduler_on_tick(1));
        failures += expect_status(KERNEL_SCHEDULER_STATUS_OK, kernel_scheduler_reap_one(&completion));
        if (!interruptible) break;
    }
    return failures + (available != physical_page_available(allocator));
}

static unsigned long run_wait_cases(
    struct physical_page_allocator *allocator)
{
    uint64_t initial_available = physical_page_available(allocator);
    struct kernel_thread_completion completion = {
        .kind = (enum kernel_thread_kind)0,
        .reason = (enum kernel_thread_exit_reason)0,
        .status = 0U,
        .detail = 0U,
    };
    struct kernel_wait_queue invalid_queue = {0};
    enum kernel_wait_wake_reason reason = KERNEL_WAIT_WOKEN;
    uintptr_t saved_interrupts;
    unsigned long failures = 0U;

    kernel_wait_queue_init(&test_queue);
    wait_case_deadline = riscv_time_read() + UINT64_C(1000000000);

    failures += expect_status(KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT,
                              kernel_scheduler_block_current(0, 0U, 0, 0));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT,
                              kernel_scheduler_block_current(&invalid_queue,
                                                             0U,
                                                             0,
                                                             &reason));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT,
                              kernel_wait_queue_wake_one(0));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT,
                              kernel_wait_queue_wake_one(&invalid_queue));
    if (reason != KERNEL_WAIT_WOKEN) {
        failures++;
    }

    __asm__ volatile("csrsi sstatus, 2" ::: "memory");
    failures += expect_status(KERNEL_SCHEDULER_STATUS_INVALID_STATE,
                              kernel_scheduler_block_current(&test_queue,
                                                             0U,
                                                             0,
                                                             &reason));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_INVALID_STATE,
                              kernel_wait_queue_wake_one(&test_queue));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_INVALID_STATE,
                              kernel_scheduler_expire_deadlines(1U));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_INVALID_STATE,
                              kernel_scheduler_yield_current());
    __asm__ volatile("csrci sstatus, 2" ::: "memory");

    failures += expect_status(KERNEL_SCHEDULER_STATUS_INVALID_STATE,
                              kernel_scheduler_block_current(&test_queue,
                                                             0U,
                                                             0,
                                                             &reason));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_wait_queue_wake_one(&test_queue));

    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_thread_create(blocked_worker, 0));
    if (physical_page_available(allocator) + TEST_TASK_PAGES != initial_available) {
        failures++;
    }
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_on_tick(1U));
    if (timeout_wake_count != 0U) {
        failures++;
    }
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_expire_deadlines(wait_case_deadline - 1));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_on_tick(1U));
    if (timeout_wake_count != 0U) {
        failures++;
    }
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_expire_deadlines(wait_case_deadline));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_on_tick(1U));
    if (timeout_wake_count != 1U ||
        timeout_reason_value != (unsigned long)KERNEL_WAIT_TIMEOUT) {
        failures++;
    }
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_reap_one(&completion));
    if (completion.kind != KERNEL_THREAD_KIND_KERNEL ||
        completion.reason != KERNEL_THREAD_EXIT_RETURNED ||
        completion.status != 0U || completion.detail != 0U ||
        physical_page_available(allocator) != initial_available) {
        failures++;
    }

    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_thread_create(blocked_worker,
                                                   (void *)(uintptr_t)1U));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_on_tick(1U));
    if (event_wake_count != 0U) {
        failures++;
    }
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_wait_queue_wake_one(&test_queue));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_on_tick(1U));
    if (event_wake_count != 1U ||
        event_reason_value != (unsigned long)KERNEL_WAIT_WOKEN) {
        failures++;
    }
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_reap_one(&completion));
    if (completion.kind != KERNEL_THREAD_KIND_KERNEL ||
        completion.reason != KERNEL_THREAD_EXIT_RETURNED ||
        completion.status != 0U || completion.detail != 0U ||
        physical_page_available(allocator) != initial_available) {
        failures++;
    }

    /* An interruptible waiter must leave its queue with SIGNALLED rather
     * than the ordinary event wake reason. */
    signal_waiter = 0;
    signal_wake_count = 0U;
    signal_reason_value = UINT32_MAX;
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_thread_create(
                                  blocked_worker,
                                  (void *)(uintptr_t)2U));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_on_tick(1U));
    if (signal_waiter == 0 || signal_wake_count != 0U) {
        failures++;
    }
    saved_interrupts = riscv_interrupt_save();
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_wake_signal(
                                  (struct kernel_task *)signal_waiter));
    riscv_interrupt_restore(saved_interrupts);
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_on_tick(1U));
    if (signal_wake_count != 1U ||
        signal_reason_value != (unsigned long)KERNEL_WAIT_SIGNALLED) {
        failures++;
    }
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_reap_one(&completion));
    if (completion.kind != KERNEL_THREAD_KIND_KERNEL ||
        completion.reason != KERNEL_THREAD_EXIT_RETURNED ||
        completion.status != 0U || completion.detail != 0U ||
        physical_page_available(allocator) != initial_available) {
        failures++;
    }

    /* Yield switch path: the first worker yields to the second one. */
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_thread_create(yielding_worker, 0));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_thread_create(yielding_worker,
                                                   (void *)(uintptr_t)1U));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_on_tick(1U));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_on_tick(1U));
    if (yield_runs != 2U) {
        failures++;
    }
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_reap_one(&completion));
    if (completion.kind != KERNEL_THREAD_KIND_KERNEL ||
        completion.reason != KERNEL_THREAD_EXIT_RETURNED ||
        completion.status != 0U || completion.detail != 0U) {
        failures++;
    }
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_reap_one(&completion));
    if (completion.kind != KERNEL_THREAD_KIND_KERNEL ||
        completion.reason != KERNEL_THREAD_EXIT_RETURNED ||
        completion.status != 0U || completion.detail != 0U ||
        physical_page_available(allocator) != initial_available) {
        failures++;
    }
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_yield_current());
    if (yield_runs != 2U) {
        failures++;
    }

    {
        struct kernel_wait_queue q1, q2;
        struct kernel_wait_node n1, n2;

        kernel_wait_queue_init(&q1);
        kernel_wait_queue_init(&q2);
        kernel_wait_node_init(&n1, 0);
        kernel_wait_node_init(&n2, 0);

        kernel_wait_queue_add(&q1, &n1);
        kernel_wait_queue_add(&q2, &n2);

        if (q1.head != &n1 || q1.tail != &n1 || n1.queue != &q1) {
            failures++;
        }
        if (q2.head != &n2 || q2.tail != &n2 || n2.queue != &q2) {
            failures++;
        }

        kernel_wait_queue_remove(&n1);
        if (q1.head != 0 || q1.tail != 0 || n1.queue != 0) {
            failures++;
        }

        kernel_wait_queue_remove(&n2);
        if (q2.head != 0 || q2.tail != 0 || n2.queue != 0) {
            failures++;
        }
    }

    return failures;
}

static unsigned long run_fpu_cases(void)
{
    struct riscv_fpu_state previous;
    struct riscv_fpu_state next;
    uintptr_t saved_interrupts;
    uintptr_t sstatus;
    unsigned long failures = 0U;
    unsigned index;

    saved_interrupts = riscv_interrupt_save();

    /* Restore: the saved image must travel through the real registers
     * and the unit must end Clean, not Dirty. */
    for (index = 0U; index < 32U; index++) {
        next.regs[index] = UINT64_C(0x1111000000000000) + index;
        previous.regs[index] = 0U;
    }
    next.fcsr = UINT64_C(0xaa);
    previous.fcsr = 0U;
    next.saved = 1U;
    previous.saved = 0U;

    riscv_fpu_switch(&previous, &next);
    __asm__ volatile("csrr %0, sstatus" : "=r"(sstatus));
    if (((sstatus >> 13) & 3U) != 2U) {
        failures++;
    }

    /* Save: mark the live state Dirty and round-trip it back through
     * the same registers into the same memory image. */
    {
        uintptr_t dirty = RISCV_SSTATUS_FS_DIRTY;

        __asm__ volatile("csrs sstatus, %0" ::"r"(dirty) : "memory");
    }
    riscv_fpu_switch(&next, &next);
    __asm__ volatile("csrr %0, sstatus" : "=r"(sstatus));
    if (((sstatus >> 13) & 3U) != 2U || next.saved != 1U) {
        failures++;
    }
    for (index = 0U; index < 32U; index++) {
        if (next.regs[index] != UINT64_C(0x1111000000000000) + index) {
            failures++;
        }
    }
    if (next.fcsr != UINT64_C(0xaa)) {
        failures++;
    }

    /* Clean state must not be saved: `previous` keeps its zero image. */
    riscv_fpu_switch(&previous, &next);
    if (previous.saved != 0U) {
        failures++;
    }
    for (index = 0U; index < 32U; index++) {
        if (previous.regs[index] != 0U) {
            failures++;
        }
    }

    /* Reset: the exec path must leave initial register contents in both
     * the live unit and the memory image. */
    for (index = 0U; index < 32U; index++) {
        next.regs[index] = UINT64_C(0x2222000000000000) + index;
    }
    next.fcsr = UINT64_C(0xbb);
    next.saved = 1U;
    riscv_fpu_switch(&previous, &next);
    riscv_fpu_reset_current(&next);
    __asm__ volatile("csrr %0, sstatus" : "=r"(sstatus));
    if (((sstatus >> 13) & 3U) != 1U || next.saved != 1U) {
        failures++;
    }
    for (index = 0U; index < 32U; index++) {
        if (next.regs[index] != 0U) {
            failures++;
        }
    }
    if (next.fcsr != 0U) {
        failures++;
    }

    /* The reset set Initial; restore Clean so later cases are unaffected. */
    {
        uintptr_t clean = RISCV_SSTATUS_FS_CLEAN;

        __asm__ volatile("csrc sstatus, %0" ::"r"((uintptr_t)0x6000)
                         : "memory");
        __asm__ volatile("csrs sstatus, %0" ::"r"(clean) : "memory");
    }

    riscv_interrupt_restore(saved_interrupts);
    return failures;
}

static unsigned long stack_contract_failures;

static void stack_contract_worker(void *argument)
{
    /* Exercise a 3 KiB live C frame plus ordinary scheduler calls.
     * The stack contract still requires a full KiB of untouched reserve. */
    volatile unsigned char workload[3072];
    for (size_t i = 0U; i < sizeof(workload); i++) workload[i] = (unsigned char)i;
    struct kernel_task *task = kernel_task_current();
    uint64_t *canary = (uint64_t *)(task->stack_low - sizeof(uint64_t));
    uintptr_t old_status = riscv_interrupt_save();
    uint64_t saved = *canary;
    uint32_t order;
    if (physical_page_allocation_order(test_page_allocator,
            task->stack_physical_address, &order) != PHYSICAL_PAGE_STATUS_OK ||
        order != KERNEL_STACK_ORDER ||
        (task->stack_physical_address & (KERNEL_STACK_BYTES - 1U)) != 0U)
        stack_contract_failures++;
    (void)argument;
    *canary ^= 1U;
    stack_contract_failures += expect_status(
        KERNEL_SCHEDULER_STATUS_STACK_CORRUPT,
        kernel_scheduler_yield_current());
    *canary = saved;
    if (workload[sizeof(workload) - 1U] != 255U) stack_contract_failures++;
    riscv_interrupt_restore(old_status);
}

static unsigned long run_stack_contract_cases(
    struct physical_page_allocator *allocator)
{
    struct kernel_thread_completion completion;
    struct kernel_stack_statistics before, after;
    uint64_t available = physical_page_available(allocator);
    unsigned long failures = 0U;
    struct kernel_task *retained;
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              allocate_task_storage(&retained));
    retained->state = KERNEL_THREAD_STATE_EXITED;
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              release_task_stack(retained));
    if (physical_page_available(allocator) != available - 1U ||
        retained->stack_physical_address != KERNEL_THREAD_NO_PAGE ||
        retained->context.sp != 0U || retained->arch.kernel_sp != 0U)
        failures++;
    /* A retained zombie/group leader can re-enter cleanup; only metadata
     * remains owned and a repeated visit must never free another stack. */
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              release_task_stack(retained));
    if (physical_page_available(allocator) != available - 1U) failures++;
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
        release_task_storage(retained, KERNEL_SCHEDULER_STATUS_OK));
    kernel_scheduler_stack_statistics(&before);
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_thread_create(stack_contract_worker, 0));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_on_tick(1U));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_reap_one(&completion));
    kernel_scheduler_stack_statistics(&after);
    if (after.stacks_released != before.stacks_released + 1U ||
        after.maximum_used_bytes == 0U ||
        after.minimum_free_bytes < KERNEL_STACK_MINIMUM_RESERVE ||
        after.minimum_free_bytes + after.maximum_used_bytes !=
            KERNEL_STACK_BYTES - KERNEL_STACK_GUARD_BYTES ||
        physical_page_available(allocator) != available)
        failures++;
    return failures + stack_contract_failures;
}

static unsigned long io_buffer_failures;
static uint64_t io_buffer_pages[2];

static void io_buffer_worker(void *argument)
{
    unsigned slot = (unsigned)(uintptr_t)argument;
    uint64_t available = physical_page_available(test_page_allocator);
    struct kernel_task_io_buffer first = {0};
    struct kernel_task_io_buffer second = {0};
    uint64_t first_page = 0U;
    void *first_data = 0;
    if (kernel_task_io_buffer_acquire(&first, test_page_allocator) !=
        KERNEL_TASK_STATUS_OK) {
        io_buffer_failures++;
    } else {
        first_page = first.physical_address;
        first_data = first.data;
        kernel_task_io_buffer_release(&first);
    }
    if (kernel_task_io_buffer_acquire(&second, test_page_allocator) !=
        KERNEL_TASK_STATUS_OK) {
        io_buffer_failures++;
        return;
    }
    /* 顺序调用复用任务的常驻 scratch 页：第二次不再分配，释放后仍归任务。 */
    if (physical_page_available(test_page_allocator) != available - 1U)
        io_buffer_failures++;
    if (second.physical_address != first_page || second.data != first_data)
        io_buffer_failures++;
    if (second.allocator != test_page_allocator ||
        second.task != kernel_task_current())
        io_buffer_failures++;
    if (slot < 2U)
        io_buffer_pages[slot] = second.physical_address;
    kernel_task_io_buffer_release(&second);
    if (physical_page_available(test_page_allocator) != available - 1U)
        io_buffer_failures++;
}

static unsigned long run_io_buffer_cases(
    struct physical_page_allocator *allocator)
{
    uint64_t available = physical_page_available(allocator);
    struct kernel_thread_completion completion;
    unsigned long failures = 0U;
    /* 两个任务各自保留自己的 scratch 页，页身份不得相同：任务间共享或
     * 继承同一页会在这里暴露，并在销毁时表现为重复释放。 */
    for (unsigned slot = 0U; slot < 2U; slot++) {
        failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
            kernel_thread_create(io_buffer_worker, (void *)(uintptr_t)slot));
        failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                                  kernel_scheduler_on_tick(1U));
    }
    if (io_buffer_pages[0] == 0U || io_buffer_pages[1] == 0U ||
        io_buffer_pages[0] == io_buffer_pages[1])
        io_buffer_failures++;
    for (unsigned slot = 0U; slot < 2U; slot++)
        failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                                  kernel_scheduler_reap_one(&completion));
    /* 任务销毁后常驻页必须归还分配器。 */
    if (physical_page_available(allocator) != available) failures++;
    return failures + io_buffer_failures;
}

static struct kernel_wait_queue deadline_queue;
static uint64_t deadline_case_ticks[4];
static unsigned deadline_wake_order[4];
static unsigned deadline_wake_count;

static void deadline_waiter(void *argument)
{
    unsigned slot = (unsigned)(uintptr_t)argument;
    enum kernel_wait_wake_reason reason;
    (void)riscv_interrupt_save();
    if (kernel_scheduler_block_current(&deadline_queue,
            deadline_case_ticks[slot], 0, &reason) !=
        KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
    if (deadline_wake_count < 4U)
        deadline_wake_order[deadline_wake_count++] = slot;
}

static unsigned long run_deadline_cases(
    struct physical_page_allocator *allocator)
{
    uint64_t available = physical_page_available(allocator);
    struct kernel_thread_completion completion;
    unsigned long failures = 0U;
    /* Manual expiry uses a future origin, independent of construction time. */
    uint64_t now = riscv_time_read() + UINT64_C(1000000000);

    kernel_wait_queue_init(&deadline_queue);
    deadline_wake_count = 0U;
    for (unsigned slot = 0U; slot < 4U; slot++) {
        deadline_case_ticks[slot] = now + 1000U * (slot + 1U);
        failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
            kernel_thread_create(deadline_waiter, (void *)(uintptr_t)slot));
    }
    /* 阻塞会链式切换到下一个 ready 成员，一次 tick 让四个等待者全部就位。 */
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_on_tick(1U));

    /* timer 重装必须包含最早的阻塞期限。 */
    if (scheduler.armed_deadline != deadline_case_ticks[0]) failures++;

    /* 到期扫描只处理已到期者；未到期成员保持阻塞。 */
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
        kernel_scheduler_expire_deadlines(now + 1500U));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_on_tick(1U));
    if (deadline_wake_count != 1U || deadline_wake_order[0] != 0U) failures++;

    /* 事件唤醒必须摘除期限索引项并且不再被到期重复唤醒。fixture 的退出
     * 路径回到 idle，因此每个被唤醒成员需要各自一次调度。 */
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_wait_queue_wake_all(&deadline_queue));
    for (unsigned slot = 0U; slot < 3U; slot++)
        failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                                  kernel_scheduler_on_tick(1U));
    if (deadline_wake_count != 4U || deadline_wake_order[1] != 1U ||
        deadline_wake_order[2] != 2U || deadline_wake_order[3] != 3U)
        failures++;
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
        kernel_scheduler_expire_deadlines(now + 100000U));
    if (deadline_wake_count != 4U) failures++;

    for (unsigned slot = 0U; slot < 4U; slot++)
        failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                                  kernel_scheduler_reap_one(&completion));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_on_tick(1U));
    /* 成员全部销毁后，硬件事件不再携带阻塞期限。 */
    if (scheduler.armed_deadline != 0U) failures++;
    return failures + (physical_page_available(allocator) != available);
}

static struct kernel_rwlock io_lock;
static unsigned sync_order, sync_failures;
static void sync_reader(void *arg)
{
    struct kernel_lock_guard guard = {0};
    unsigned id = (uintptr_t)arg;
    kernel_rwlock_read(&io_lock, &guard);
    sync_order = sync_order * 10 + id;
    if (id == 1) {
        uintptr_t irq = riscv_interrupt_save();
        if (kernel_scheduler_yield_current() != KERNEL_SCHEDULER_STATUS_OK)
            sync_failures++;
        if (sync_order != 1) sync_failures++;
        riscv_interrupt_restore(irq);
    }
    kernel_lock_release(&guard);
}
static void sync_writer(void *arg)
{
    (void)arg;
    struct kernel_lock_guard guard = {0};
    kernel_rwlock_write(&io_lock, &guard);
    sync_order = sync_order * 10 + 2;
    kernel_lock_release(&guard);
}
static unsigned run_sync_cases(struct physical_page_allocator *allocator)
{
    struct boot_memory_layout layout = {0};
    layout.usable_count = 1;
    layout.usable[0].base = TEST_PHYSICAL_BASE;
    layout.usable[0].size = sizeof(page_pool);
    if (physical_page_allocator_init(allocator, &layout) != PHYSICAL_PAGE_STATUS_OK ||
        physical_page_allocator_bind_access(allocator, scheduler_page_access) != PHYSICAL_PAGE_STATUS_OK ||
        physical_page_allocator_finalize(allocator) != PHYSICAL_PAGE_STATUS_OK) return 1;
    uint64_t available = physical_page_available(allocator);
    struct kernel_thread_completion completion;
    kernel_rwlock_init(&io_lock, 1, 0);
    if (kernel_thread_create(sync_reader, (void *)1) != KERNEL_SCHEDULER_STATUS_OK ||
        kernel_thread_create(sync_writer, 0) != KERNEL_SCHEDULER_STATUS_OK ||
        kernel_thread_create(sync_reader, (void *)3) != KERNEL_SCHEDULER_STATUS_OK)
        return 1;
    for (unsigned i = 0; i < 3; i++) {
        if (kernel_scheduler_on_tick(1) != KERNEL_SCHEDULER_STATUS_OK) sync_failures++;
        if (kernel_scheduler_reap_one(&completion) != KERNEL_SCHEDULER_STATUS_OK)
            sync_failures++;
    }
    return sync_failures + (sync_order != 123) +
           (physical_page_available(allocator) != available);
}

static unsigned irq_return_runs;
static void irq_return_worker(void *argument)
{
    (void)argument;
    irq_return_runs++;
}
static unsigned run_idle_irq_return_case(struct physical_page_allocator *allocator)
{
    uint64_t available = physical_page_available(allocator);
    struct kernel_thread_completion completion;
    struct riscv_trap_frame frame = {.sstatus = RISCV_SSTATUS_SPP};
    if (kernel_thread_create(irq_return_worker, 0) != KERNEL_SCHEDULER_STATUS_OK)
        return 1;
    /* 模拟 IRQ 已将任务置 ready、尚未执行 idle 的 WFI；不借助 tick。 */
    riscv_trap_return_prepare(&frame);
    unsigned failure = irq_return_runs != 1;
    if (failure) (void)kernel_scheduler_yield_current();
    if (kernel_scheduler_reap_one(&completion) != KERNEL_SCHEDULER_STATUS_OK)
        failure++;
    virt_uart_puts("BoarOS: idle IRQ return failures=");
    virt_uart_put_hex(failure);
    virt_uart_putc('\n');
    return failure + (physical_page_available(allocator) != available);
}

static unsigned exit_join_done;
static struct kernel_thread_join exit_cleanup_owner;

static void exit_cleanup_entry(void *argument)
{
    (void)argument;
    (void)riscv_interrupt_save();
    kernel_scheduler_register_cleanup();
    for (;;) {
        struct kernel_thread_completion completion;
        while (kernel_scheduler_reap_one(&completion) == KERNEL_SCHEDULER_STATUS_OK) { }
        kernel_scheduler_wait_cleanup(0);
    }
}

static void exit_empty_entry(void *argument) { (void)argument; }

static void exit_join_entry(void *argument)
{
    (void)argument;
    struct kernel_thread_join child = {0};
    if (kernel_thread_create_joinable(exit_empty_entry, 0, &child) != KERNEL_SCHEDULER_STATUS_OK)
        return;
    kernel_thread_join(&child);
    exit_join_done = 1;
}

static unsigned run_exit_dispatch_case(struct physical_page_allocator *allocator)
{
    /* 最后运行：此 worker 与生产 cleanup 一样由启动 owner 保有至 shutdown。 */
    if (kernel_thread_create_joinable(exit_cleanup_entry, 0, &exit_cleanup_owner)
            != KERNEL_SCHEDULER_STATUS_OK ||
        kernel_scheduler_yield_current() != KERNEL_SCHEDULER_STATUS_OK)
        return 1;
    uint64_t available = physical_page_available(allocator);
    if (kernel_thread_create(exit_join_entry, 0) != KERNEL_SCHEDULER_STATUS_OK ||
        kernel_scheduler_yield_current() != KERNEL_SCHEDULER_STATUS_OK)
        return 1;
    /* fixture 未启动 timer；退出、join 唤醒和清理必须自行连续进展。 */
    unsigned failure = !exit_join_done;
    for (unsigned retry = 0; retry < 4; retry++)
        if (kernel_scheduler_yield_current() != KERNEL_SCHEDULER_STATUS_OK) failure++;
    if (!exit_join_done || physical_page_available(allocator) != available)
        failure++;
    virt_uart_puts("BoarOS: exit dispatch failures=");
    virt_uart_put_hex(failure);
    virt_uart_putc('\n');
    return failure;
}

static unsigned handoff_order;
static unsigned handoff_count, handoff_events[33];
static void handoff_waiter(void *argument)
{
    uintptr_t value = (uintptr_t)argument;
    struct kernel_lock_guard guard = {0};
    if (value & 0x100) kernel_rwlock_write(&io_lock, &guard);
    else kernel_rwlock_read(&io_lock, &guard);
    handoff_order = handoff_order * 10 + (value & 0xff);
    if (handoff_count < 33) handoff_events[handoff_count++] = value & 0xff;
    kernel_lock_release(&guard);
}
static unsigned run_handoff_cases(struct physical_page_allocator *allocator)
{
    uint64_t available = physical_page_available(allocator);
    struct kernel_thread_completion completion;
    unsigned failures = 0;
    for (unsigned mixed = 0; mixed < 2; mixed++) {
        struct kernel_lock_guard held = {0};
        kernel_rwlock_init(&io_lock, 1, 0);
        handoff_order = 0;
        handoff_count = 0;
        kernel_rwlock_write(&io_lock, &held);
        if (kernel_thread_create(handoff_waiter, (void *)(uintptr_t)(mixed ? 1 : 0x101)) != KERNEL_SCHEDULER_STATUS_OK ||
            kernel_thread_create(handoff_waiter, (void *)0x102) != KERNEL_SCHEDULER_STATUS_OK ||
            kernel_scheduler_yield_current() != KERNEL_SCHEDULER_STATUS_OK)
            return 1;
        /* 后到任务已 ready；释放锁必须预留资格，不能让它抢走旧等待者的锁。 */
        if (kernel_thread_create(handoff_waiter, (void *)(uintptr_t)(mixed ? 3 : 0x103)) != KERNEL_SCHEDULER_STATUS_OK)
            return 1;
        kernel_lock_release(&held);
        for (unsigned i = 0; i < 3; i++) {
            if (kernel_scheduler_yield_current() != KERNEL_SCHEDULER_STATUS_OK)
                failures++;
            if (kernel_scheduler_reap_one(&completion) != KERNEL_SCHEDULER_STATUS_OK)
                failures++;
        }
        if (handoff_order != 123) failures++;
    }
    const unsigned populations[] = {1, 8, 32};
    for (unsigned p = 0; p < 3; p++) {
        unsigned count = populations[p];
        struct kernel_lock_guard held = {0};
        kernel_rwlock_init(&io_lock, 1, 0);
        handoff_count = 0;
        kernel_rwlock_write(&io_lock, &held);
        for (unsigned i = 1; i <= count; i++)
            if (kernel_thread_create(handoff_waiter, (void *)(uintptr_t)(0x100 | i)) != KERNEL_SCHEDULER_STATUS_OK)
                return 1;
        if (kernel_scheduler_yield_current() != KERNEL_SCHEDULER_STATUS_OK ||
            kernel_thread_create(handoff_waiter, (void *)(uintptr_t)(0x100 | (count + 1))) != KERNEL_SCHEDULER_STATUS_OK)
            return 1;
        kernel_lock_release(&held);
        for (unsigned i = 0; i <= count; i++) {
            if (kernel_scheduler_yield_current() != KERNEL_SCHEDULER_STATUS_OK ||
                kernel_scheduler_reap_one(&completion) != KERNEL_SCHEDULER_STATUS_OK)
                failures++;
            if (handoff_events[i] != i + 1) failures++;
        }
        if (handoff_count != count + 1) failures++;
    }
    virt_uart_puts("BoarOS: lock handoff failures=");
    virt_uart_put_hex(failures);
    virt_uart_putc('\n');
    return failures + (physical_page_available(allocator) != available);
}

static unsigned deferred_ran;
static void deferred_entry(void *argument)
{ (void)argument; deferred_ran++; }
static unsigned run_deferred_preemption_case(struct physical_page_allocator *allocator)
{
    uint64_t before = physical_page_available(allocator);
    struct kernel_cpu *cpu = kernel_cpu_current();
    struct kernel_task *current = kernel_task_current();
    struct kernel_thread_completion completion;
    if (kernel_thread_create(deferred_entry, 0) != KERNEL_SCHEDULER_STATUS_OK) return 1;
    kernel_preempt_disable();
    unsigned failure = kernel_scheduler_on_tick(1) != KERNEL_SCHEDULER_STATUS_OK ||
        deferred_ran || kernel_task_current() != current || !cpu->need_resched;
    kernel_preempt_enable();
    kernel_scheduler_prepare_idle_return();
    failure += deferred_ran != 1 || kernel_scheduler_reap_one(&completion) != KERNEL_SCHEDULER_STATUS_OK ||
        physical_page_available(allocator) != before || cpu->preempt_depth || cpu->raw_locks;
    return failure;
}
static unsigned other_deferred_ran, other_deferred_failure;
static void other_deferred_peer(void *argument)
{ (void)argument; other_deferred_ran = 1; }
static void other_deferred_current(void *argument)
{
    (void)argument;
    uintptr_t irq = arch_interrupt_save();
    struct kernel_task *before = kernel_task_current();
    kernel_preempt_disable();
    other_deferred_failure = kernel_scheduler_on_tick(1) != KERNEL_SCHEDULER_STATUS_OK ||
        other_deferred_ran || kernel_task_current() != before;
    kernel_preempt_enable();
    kernel_task_prepare_user_return();
    other_deferred_failure += !other_deferred_ran || kernel_cpu_current()->need_resched ||
        kernel_task_current() != before;
    arch_interrupt_restore(irq);
}
static unsigned run_other_deferred_case(struct physical_page_allocator *allocator)
{
    uint64_t baseline = physical_page_available(allocator);
    struct kernel_thread_completion completion;
    if (kernel_thread_create(other_deferred_current, 0) != KERNEL_SCHEDULER_STATUS_OK ||
        kernel_thread_create(other_deferred_peer, 0) != KERNEL_SCHEDULER_STATUS_OK) return 1;
    /* 无cleanup worker的fixture每次退出回idle；分别驱动并回收两个owner。 */
    for (unsigned i = 0; i < 2; i++) {
        if (kernel_scheduler_yield_current() != KERNEL_SCHEDULER_STATUS_OK ||
            kernel_scheduler_reap_one(&completion) != KERNEL_SCHEDULER_STATUS_OK) return 1;
    }
    return other_deferred_failure + (physical_page_available(allocator) != baseline);
}

void kernel_main(unsigned long hart_id, const void *dtb)
{
    struct boot_memory_layout layout;
    struct physical_page_allocator allocator;
    enum physical_page_status page_status;
    unsigned long failures = 0U;

    (void)hart_id;
    (void)dtb;

    layout.usable_count = 1U;
    layout.usable[0].base = TEST_PHYSICAL_BASE;
    layout.usable[0].size = BOAROS_PAGE_SIZE * TEST_PAGE_COUNT;
    page_status = physical_page_allocator_init(&allocator, &layout);
    if (page_status == PHYSICAL_PAGE_STATUS_OK) {
        page_status = physical_page_allocator_bind_access(
            &allocator,
            scheduler_page_access);
    }

    if (page_status == PHYSICAL_PAGE_STATUS_OK) {
        failures += expect_status(KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT,
            kernel_scheduler_init(&allocator, (uintptr_t)__boot_stack_bottom,
                                   (uintptr_t)__boot_stack_top));
        page_status = physical_page_allocator_finalize(&allocator);
    }
    test_page_allocator = &allocator;
    failures += page_status != PHYSICAL_PAGE_STATUS_OK;
    failures += run_pid_cases();
    failures += run_preinit_cases();
    failures += run_init_cases(&allocator);
    failures += run_idle_cases();
    failures += run_create_cases(&allocator);
    failures += run_wait_cases(&allocator);
    failures += run_accounting_cases(&allocator);
    failures += run_load_wait_cases(&allocator);
    failures += run_fpu_cases();
    failures += run_stack_contract_cases(&allocator);
    failures += run_sync_cases(&allocator);
    failures += run_io_buffer_cases(&allocator);
    failures += run_deadline_cases(&allocator);
    failures += run_idle_irq_return_case(&allocator);
    failures += run_handoff_cases(&allocator);
    failures += run_deferred_preemption_case(&allocator);
    failures += run_other_deferred_case(&allocator);
    failures += run_exit_dispatch_case(&allocator);
    virt_uart_puts("BoarOS: scheduler cases failures=");
    virt_uart_put_hex(failures);
    virt_uart_putc('\n');
    sbi_shutdown();
}
