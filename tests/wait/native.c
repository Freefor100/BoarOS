#include <kernel/wait_internal.h>
#include <arch/timer.h>
#include "../../kernel/sched/private.h"
#if defined(BOAROS_ARCH_LOONGARCH)
#include <platform/loongarch_virt.h>
#define say la_virt_puts
#define hex la_virt_hex
#define shutdown la_virt_shutdown
#else
#include <arch/riscv/virt_uart.h>
#include <arch/riscv/sbi.h>
#define say virt_uart_puts
#define hex virt_uart_put_hex
#define shutdown sbi_shutdown
#endif
static struct kernel_wait_queue queue;
static unsigned started, ended, early_switch;
static uint64_t deadline;
#define check(valid) do { if (!(valid)) { say("WAIT failed line="); hex(__LINE__); say("\n"); __builtin_trap(); } } while (0)
static void empty(void *argument) { (void)argument; check(!kernel_cpu_current()->switch_previous); }
enum kernel_scheduler_status __real_kernel_wait_backend_switch(struct kernel_task *task);
enum kernel_scheduler_status __wrap_kernel_wait_backend_switch(struct kernel_task *task)
{
    if (early_switch) {
        early_switch = 0;
        check(task->wait.on_cpu && task->state == KERNEL_THREAD_STATE_BLOCKED);
        check(kernel_wait_queue_wake_one(&queue) == KERNEL_SCHEDULER_STATUS_OK);
        check(task->wait.ready_pending && !task->ready_node.queued);
    }
    return __real_kernel_wait_backend_switch(task);
}
static void worker(void *argument)
{
    uintptr_t irq = arch_interrupt_save();
    check(!kernel_cpu_current()->switch_previous);
    struct kernel_wait_token token;
    enum kernel_wait_wake_reason reason;
    started++;
    check(kernel_wait_prepare(&queue, 0, 0, &token) == KERNEL_SCHEDULER_STATUS_OK);
    check(kernel_wait_queue_wake_all(&queue) == KERNEL_SCHEDULER_STATUS_OK);
    check(kernel_wait_park(&token, &reason) == KERNEL_SCHEDULER_STATUS_OK && reason == KERNEL_WAIT_WOKEN);
    check(kernel_wait_finish(&token) == KERNEL_SCHEDULER_STATUS_OK);
    if (!(uintptr_t)argument) early_switch = 1;
    check(kernel_wait_prepare((uintptr_t)argument ? 0 : &queue,
        (uintptr_t)argument ? deadline : 0, 0, &token) == KERNEL_SCHEDULER_STATUS_OK);
    arch_interrupt_restore(irq);
    check(arch_interrupt_is_enabled());
    check(kernel_wait_park(&token, &reason) == KERNEL_SCHEDULER_STATUS_OK);
    check(arch_interrupt_is_enabled());
    (void)arch_interrupt_save();
    check(reason == ((uintptr_t)argument ? KERNEL_WAIT_TIMEOUT : KERNEL_WAIT_WOKEN));
    check(kernel_wait_finish(&token) == KERNEL_SCHEDULER_STATUS_OK);
    check(!kernel_cpu_current()->switch_previous && !kernel_wait_record_of(kernel_cpu_current()->current)->borrows);
    ended++;
    arch_interrupt_restore(irq);
}
#ifdef BOAROS_SCHED_LIFECYCLE
static struct kernel_task *held_task;
static unsigned cleanup_phase;
static void cleanup_probe(void *unused)
{
    (void)unused;
    (void)arch_interrupt_save();
    kernel_scheduler_register_cleanup();
    struct kernel_thread_completion completion;
    check(kernel_scheduler_reap_one(&completion) == KERNEL_SCHEDULER_STATUS_BUSY);
    check(!kernel_scheduler_reap_pending());
    cleanup_phase = 1;
    kernel_scheduler_wait_cleanup(0);
    check(cleanup_phase == 2);
    check(kernel_scheduler_reap_one(&completion) == KERNEL_SCHEDULER_STATUS_EMPTY);
    check(held_task->retirement == 2 && held_task->storage_borrows == 1);
    check(!kernel_scheduler_reap_pending());
    check(kernel_scheduler_stop_users()); /* root must await this storage owner */
    cleanup_phase = 3;
    kernel_scheduler_wait_cleanup(0);
    check(cleanup_phase == 4 && !scheduler.retired_pending);
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        scheduler.cleanup_task = 0;
    }
}
static void release_probe(void *unused)
{
    (void)unused;
    (void)arch_interrupt_save();
    check(cleanup_phase == 1 && kernel_wait_record_phase(&scheduler.cleanup_task->wait) == KERNEL_WAIT_COMMITTED &&
          !scheduler.cleanup_task->wait.on_cpu);
    cleanup_phase = 2;
    scheduler_task_wait_claim_release(held_task);
    check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK);
    check(cleanup_phase == 3 && kernel_wait_record_phase(&scheduler.cleanup_task->wait) == KERNEL_WAIT_COMMITTED &&
          !scheduler.cleanup_task->wait.on_cpu);
    cleanup_phase = 4;
    scheduler_task_put(held_task);
    held_task = 0;
    check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK);
}
static void cleanup_owner_progress(struct physical_page_allocator *allocator)
{
    uint64_t baseline = physical_page_available(allocator);
    struct kernel_thread_join join = {0};
    check(kernel_thread_create_joinable(empty, 0, &join) == KERNEL_SCHEDULER_STATUS_OK);
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        held_task = join.task;
        scheduler_task_get_locked(held_task);
        held_task->reaping = 1; /* wait-event copy still owns the task */
    }
    check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK);
    check(held_task->state == KERNEL_THREAD_STATE_EXITED && !held_task->wait.on_cpu);
    cleanup_phase = 0;
    check(kernel_thread_create(cleanup_probe, 0) == KERNEL_SCHEDULER_STATUS_OK);
    check(kernel_thread_create(release_probe, 0) == KERNEL_SCHEDULER_STATUS_OK);
    check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK);
    check(cleanup_phase == 4 && !held_task && !scheduler.cleanup_task && !join.task);
    /* The probe cleared cleanup_task before returning, so exit selected idle;
     * let the still-ready release owner resume its real return path. */
    check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK);
    struct kernel_thread_completion completion;
    for (unsigned i = 0; i < 2; i++)
        check(kernel_scheduler_reap_one(&completion) == KERNEL_SCHEDULER_STATUS_OK);
    check(physical_page_available(allocator) == baseline);
    check(!scheduler.cleanup_queue.registrations && !scheduler.cleanup_queue.borrows);
    check(physical_page_allocator_audit(allocator) == PHYSICAL_PAGE_STATUS_OK);
    say("SCHED cleanup passed: claimed exit, real park, last borrow and owner baseline\n");
}
static void lifecycle(struct physical_page_allocator *allocator)
{
    uint64_t baseline = physical_page_available(allocator);
    struct kernel_thread_join join = {0};
    check(kernel_thread_create_joinable(empty, 0, &join) == KERNEL_SCHEDULER_STATUS_OK);
    struct kernel_task *task;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        task = join.task;
        scheduler_task_get_locked(task);
    }
    check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK);
    check(!task->wait.on_cpu && task->state == KERNEL_THREAD_STATE_EXITED);
    struct kernel_thread_completion completion;
    check(kernel_scheduler_reap_one(&completion) == KERNEL_SCHEDULER_STATUS_EMPTY);
    check(!join.task && task->retirement == 2 && task->storage_borrows == 1);
    check(task->stack_physical_address == KERNEL_THREAD_NO_PAGE);
    check(physical_page_available(allocator) + 1 == baseline);
    /* A sleeping lookup owner is not immediately executable cleanup work. */
    check(!kernel_scheduler_reap_pending());
    scheduler_task_put(task);
    scheduler_task_collect();
    check(!scheduler.retired_pending && !scheduler.retired_tasks);
    check(physical_page_available(allocator) == baseline);
    check(physical_page_allocator_audit(allocator) == PHYSICAL_PAGE_STATUS_OK);
    say("SCHED lifecycle passed: real switch, trusted-stack release, borrowed metadata and owner baseline\n");
}
#endif
static void run(struct physical_page_allocator *allocator)
{
    (void)arch_interrupt_save();
    say("WAIT native entered\n");
    check(kernel_thread_create(empty, 0) == KERNEL_SCHEDULER_STATUS_OK);
    check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK);
    struct kernel_thread_completion completion;
    check(kernel_scheduler_reap_one(&completion) == KERNEL_SCHEDULER_STATUS_OK);
    uint64_t baseline = physical_page_available(allocator);
    kernel_wait_queue_init(&queue);
    deadline = arch_time_read() + UINT64_C(1000000000);
    check(kernel_thread_create(worker, 0) == KERNEL_SCHEDULER_STATUS_OK);
    check(kernel_thread_create(worker, (void *)1) == KERNEL_SCHEDULER_STATUS_OK);
    check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK);
    check(started == 2 && !early_switch);
    check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK);
    check(ended == 1);
    check(kernel_scheduler_expire_deadlines(deadline) == KERNEL_SCHEDULER_STATUS_OK);
    check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK);
    check(ended == 2 && !kernel_cpu_current()->switch_previous);
    for (unsigned i = 0; i < 2; i++) check(kernel_scheduler_reap_one(&completion) == KERNEL_SCHEDULER_STATUS_OK);
    check(kernel_scheduler_reap_one(&completion) == KERNEL_SCHEDULER_STATUS_EMPTY);
    check(kernel_wait_queue_close(&queue) == KERNEL_SCHEDULER_STATUS_OK);
    check(kernel_wait_queue_destroy(&queue) == KERNEL_SCHEDULER_STATUS_OK);
    check(physical_page_available(allocator) == baseline);
    check(physical_page_allocator_audit(allocator) == PHYSICAL_PAGE_STATUS_OK);
#ifdef BOAROS_SCHED_LIFECYCLE
    lifecycle(allocator);
    cleanup_owner_progress(allocator);
#endif
    say("WAIT native passed: first-entry, pre-switch wake, timeout, exit and owner baseline\n");
    shutdown();
}
#if defined(BOAROS_ARCH_LOONGARCH)
void __wrap_la_boot_tasks(struct physical_page_allocator *allocator) { run(allocator); }
#else
enum kernel_scheduler_status __real_kernel_scheduler_init(struct physical_page_allocator *, uintptr_t, uintptr_t);
enum kernel_scheduler_status __wrap_kernel_scheduler_init(struct physical_page_allocator *allocator, uintptr_t low, uintptr_t high)
{
    enum kernel_scheduler_status result = __real_kernel_scheduler_init(allocator, low, high);
    check(result == KERNEL_SCHEDULER_STATUS_OK);
    run(allocator);
    return result;
}
#endif
