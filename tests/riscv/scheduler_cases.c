#include <arch/riscv/context.h>
#include <arch/riscv/fpu.h>
#include <arch/riscv/sbi.h>
#include <arch/riscv/thread.h>
#include <arch/riscv/trap.h>
#include <arch/riscv/virt_uart.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/pid.h>
#include <kernel/scheduler.h>
#include <kernel/task.h>

#include <stdint.h>

extern unsigned char __boot_stack_bottom[];
extern unsigned char __boot_stack_top[];

#define TEST_PHYSICAL_BASE UINT64_C(0x41000000)
#define TEST_PAGE_COUNT 2U

static unsigned char page_pool[BOAROS_PAGE_SIZE * TEST_PAGE_COUNT]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));
static unsigned long access_calls_before_failure;
static unsigned long fail_access_count;
static uintptr_t entry_sp[TEST_PAGE_COUNT];
static void *entry_tp[TEST_PAGE_COUNT];
static uintptr_t entry_order[TEST_PAGE_COUNT];
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
    if (index < TEST_PAGE_COUNT) {
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
    failures += expect_status(KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT,
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
    fail_access_count = 2U;
    failures += expect_status(KERNEL_SCHEDULER_STATUS_PAGE_RELEASE,
                              kernel_thread_create(thread_entry, 0));
    if (physical_page_available(allocator) + 1U != initial_available) {
        failures++;
    }
    fail_access_count = 0U;
    failures += expect_status(KERNEL_SCHEDULER_STATUS_PAGE_RELEASE,
                              kernel_thread_create(thread_entry, 0));
    if (physical_page_available(allocator) + 1U != initial_available) {
        failures++;
    }
    access_calls_before_failure = 0U;
    fail_access_count = 1U;
    failures += expect_status(KERNEL_SCHEDULER_STATUS_PAGE_RELEASE,
                              kernel_scheduler_reap_one(&completion));
    if (completion.kind != (enum kernel_thread_kind)0x21 ||
        completion.reason != (enum kernel_thread_exit_reason)0x43 ||
        completion.status != UINT64_C(0x65768798a9bacbdc) ||
        completion.detail != UINT64_C(0xedfe0f1021324354) ||
        physical_page_available(allocator) + 1U != initial_available) {
        failures++;
    }
    failures += expect_status(KERNEL_SCHEDULER_STATUS_EMPTY,
                              kernel_scheduler_reap_one(&completion));
    if (completion.kind != (enum kernel_thread_kind)0x21 ||
        completion.reason != (enum kernel_thread_exit_reason)0x43 ||
        completion.status != UINT64_C(0x65768798a9bacbdc) ||
        completion.detail != UINT64_C(0xedfe0f1021324354) ||
        physical_page_available(allocator) != initial_available) {
        failures++;
    }

    access_calls_before_failure = 1U;
    fail_access_count = 1U;
    failures += expect_status(KERNEL_SCHEDULER_STATUS_PAGE_ACCESS,
                              kernel_thread_create(thread_entry, 0));
    if (physical_page_available(allocator) != initial_available) {
        failures++;
    }

    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_thread_create(thread_entry,
                                                   (void *)(uintptr_t)1U));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_thread_create(thread_entry,
                                                   (void *)(uintptr_t)2U));
    if (physical_page_available(allocator) != 0U) {
        failures++;
    }
    failures += expect_status(KERNEL_SCHEDULER_STATUS_NO_MEMORY,
                              kernel_thread_create(thread_entry, 0));
    if (physical_page_available(allocator) != 0U) {
        failures++;
    }

    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_on_tick(1U));
    if (entry_count != TEST_PAGE_COUNT ||
        entry_order[0] != 1U || entry_order[1] != 2U ||
        entry_tp[0] == 0 || entry_tp[1] == 0 ||
        entry_tp[0] == entry_tp[1] ||
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
        physical_page_available(allocator) != initial_available - 1U) {
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
                                       slot == 0U ? 1000U : 0U,
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
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_on_tick(1U));
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

    return failures;
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
    if (physical_page_available(allocator) + 1U != initial_available) {
        failures++;
    }
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_on_tick(1U));
    if (timeout_wake_count != 0U) {
        failures++;
    }
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_expire_deadlines(999U));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_on_tick(1U));
    if (timeout_wake_count != 0U) {
        failures++;
    }
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_expire_deadlines(1000U));
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

void kernel_main(unsigned long hart_id, const void *dtb)
{
    struct boot_memory_layout layout;
    struct physical_page_allocator allocator;
    enum physical_page_status page_status;
    unsigned long failures;

    (void)hart_id;
    (void)dtb;

    layout.usable_count = 1U;
    layout.usable[0].base = TEST_PHYSICAL_BASE;
    layout.usable[0].size = sizeof(page_pool);
    page_status = physical_page_allocator_init(&allocator, &layout);
    if (page_status == PHYSICAL_PAGE_STATUS_OK) {
        page_status = physical_page_allocator_bind_access(
            &allocator,
            scheduler_page_access);
    }

    failures = page_status != PHYSICAL_PAGE_STATUS_OK;
    failures += run_pid_cases();
    failures += run_preinit_cases();
    failures += run_init_cases(&allocator);
    failures += run_idle_cases();
    failures += run_create_cases(&allocator);
    failures += run_wait_cases(&allocator);
    failures += run_accounting_cases(&allocator);
    failures += run_fpu_cases();

    virt_uart_puts("BoarOS: scheduler cases failures=");
    virt_uart_put_hex(failures);
    virt_uart_putc('\n');
    sbi_shutdown();
}
