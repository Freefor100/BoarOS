#include <arch/riscv/smp.h>
#include <arch/riscv/sbi.h>
#include <arch/riscv/sv39.h>
#include <arch/riscv/memory_layout.h>
#include <arch/riscv/direct_map.h>
#include <arch/riscv/virt_uart.h>
#include <kernel/physical_page.h>
#include <kernel/scheduler.h>
#include <kernel/task.h>
#include <kernel/tick.h>
#include <arch/riscv/fpu.h>
#include <arch/riscv/timer.h>
#include <arch/riscv/trap.h>
#include <kernel/heap.h>
#include <kernel/irq.h>
#include <kernel/wait_internal.h>
#include "../../kernel/sched/private.h"

static struct physical_page_allocator *allocator;
static uint64_t baseline;
static unsigned done;
static uint32_t entered, counters[4], executions[4];
static struct kernel_thread_join joins[4];
static struct kernel_wait_queue migrate_queue;
static struct kernel_thread_join migration;
static unsigned migrated;
static unsigned shared_entered, shared_cpus, shared_counts[2];
extern int riscv_smp_gpr_wait(void);
static struct physical_page_allocator arena;
static struct kernel_heap heap;
static struct kernel_mutex model;
static uint64_t arena_address, shared_page;
static uint8_t owners[UINT32_C(1) << 18];
static unsigned arena_entered;
static unsigned drain_entered, drain_finished, drain_counts[2];
static uint64_t drained_pages[2][1024];
static struct kernel_wait_queue edge_queue;
static struct kernel_thread_join edge_join;
static unsigned boundary_armed, boundary_entered, boundary_release;
static unsigned edge_finished, edge_can_park, handoff_entered, handoff_release;
static unsigned fence_hold, fence_completed, fence_release, fence_observed;
static uint64_t fence_table, fence_page;
static struct riscv_switch_context *held_context;
static struct kernel_wait_token edge_token;
static uint64_t edge_deadline;
static int root_mode;
static unsigned root_background_started, root_background_stop;
static uint64_t root_background_count, root_background_begin;
static struct kernel_thread_join root_background_join, root_controller_join;
static uint64_t last_fence_address, last_fence_size;
static unsigned allocation_fail_after, allocation_attempt;
static unsigned alias_read, alias_continue, alias_checked;
static uint64_t alias_address;
static unsigned borrow_may_exit, borrow_put_done, borrow_migrate, borrow_put_allowed, borrow_owner_checked;
static struct kernel_wait_queue borrow_gate;
static struct kernel_thread_join creation_join, creation_target_join;
static struct kernel_wait_queue creation_gate;
static unsigned creation_race_armed, creation_waiting;
static enum kernel_scheduler_status creation_result;
static struct kernel_rwlock shared_lock;
static struct kernel_thread_join lock_joins[8];
static unsigned readers_entered, readers_left, writer_entered, later_entered;
static uint64_t lock_value;
static struct kernel_wait_queue policy_gate;
static struct kernel_thread_join policy_joins[2];
static unsigned policy_peer, policy_kind;
enum physical_page_status __real_physical_page_allocate(struct physical_page_allocator *, uint64_t *);
enum physical_page_status __wrap_physical_page_allocate(struct physical_page_allocator *pages,
                                                        uint64_t *address)
{
    if (pages == allocator && creation_race_armed && kernel_task_current() == creation_join.task) {
        creation_race_armed = 0;
        struct kernel_wait_token token;
        enum kernel_wait_wake_reason reason;
        if (kernel_wait_prepare(&creation_gate, 0, 0, &token) != KERNEL_SCHEDULER_STATUS_OK)
            __builtin_trap();
        __atomic_store_n(&creation_waiting, 1, __ATOMIC_RELEASE);
        if (kernel_wait_park(&token, &reason) != KERNEL_SCHEDULER_STATUS_OK ||
            kernel_wait_finish(&token) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
    }
    if (pages == allocator && allocation_fail_after && ++allocation_attempt == allocation_fail_after)
        return PHYSICAL_PAGE_STATUS_EMPTY;
    return __real_physical_page_allocate(pages, address);
}
long __real_sbi_probe_extension(unsigned long);
long __wrap_sbi_probe_extension(unsigned long extension)
{
    if (BOAROS_SMP_NEGATIVE == 1 && extension == SBI_EXT_RFENCE) return 0;
    return __real_sbi_probe_extension(extension);
}
long __real_sbi_hart_start(uint64_t, uint64_t, uint64_t);
long __wrap_sbi_hart_start(uint64_t hart, uint64_t entry, uint64_t opaque)
{
    if (BOAROS_SMP_NEGATIVE == 2) return -4;
    return __real_sbi_hart_start(hart, entry, opaque);
}
long __real_sbi_hart_status(uint64_t);
long __wrap_sbi_hart_status(uint64_t hart)
{
    long state = __real_sbi_hart_status(hart);
    struct kernel_cpu *cpu = riscv_smp_cpu(1);
    if (BOAROS_SMP_NEGATIVE == 4 && cpu && state == SBI_HSM_STOPPED &&
        __atomic_load_n(&cpu->state, __ATOMIC_ACQUIRE) == KERNEL_CPU_QUIESCING)
        return SBI_HSM_STARTED;
    return state;
}

static void *arena_access(uint64_t address)
{
    uint64_t va;
    if (riscv_direct_map_pa_to_va(address, 4096, &va)) return 0;
    return (void *)(uintptr_t)va;
}
static int arena_physical(const void *p, uint64_t *address)
{ return riscv_direct_map_va_to_pa((uintptr_t)p, 1, address) == RISCV_DIRECT_MAP_STATUS_OK; }

static void require(int ok, const char *what)
{ if (!ok) riscv_smp_fail(what, -1); }

static void root_background(void *unused)
{
    (void)unused;
    __atomic_store_n(&root_background_started, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&root_background_stop, __ATOMIC_ACQUIRE))
        __atomic_fetch_add(&root_background_count, 1, __ATOMIC_RELAXED);
}

void riscv_smp_test_root_finish(void)
{
    if (!root_mode) return;
    uint64_t progress = __atomic_load_n(&root_background_count, __ATOMIC_ACQUIRE) - root_background_begin;
    require(progress != 0, "root-background-overlap");
    __atomic_store_n(&root_background_stop, 1, __ATOMIC_RELEASE);
    /* 根管理者取得实际join结果，不能只凭body的done标志释放controller栈。 */
    kernel_thread_join(&root_controller_join);
    require(done, "root-controller-completion");
    virt_uart_puts("BoarOS: SMP root overlap counter=");
    virt_uart_put_hex(progress);
    virt_uart_puts(" controller/arena/wait owners returned\n");
}

void __real_riscv_context_switch(struct riscv_switch_context *, struct riscv_switch_context *);
void __wrap_riscv_context_switch(struct riscv_switch_context *previous,
                                 struct riscv_switch_context *next)
{
    if (previous == held_context && !__atomic_load_n(&handoff_release, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&handoff_entered, 1, __ATOMIC_RELEASE);
        uint64_t end = riscv_time_read() + 50000000;
        while (!__atomic_load_n(&handoff_release, __ATOMIC_ACQUIRE))
            require((int64_t)(riscv_time_read() - end) < 0, "handoff-test-timeout");
    }
    __real_riscv_context_switch(previous, next);
}

long __real_sbi_remote_sfence(uint64_t, uint64_t, uint64_t);
long __wrap_sbi_remote_sfence(uint64_t hart, uint64_t address, uint64_t size)
{
    last_fence_address = address;
    last_fence_size = size;
    if (BOAROS_SMP_NEGATIVE == 5 && fence_hold) {
        uint32_t references;
        if (physical_page_reference_count(allocator, fence_table, &references) != PHYSICAL_PAGE_STATUS_OK ||
            references != 1 || physical_page_reference_count(allocator, fence_page, &references) !=
            PHYSICAL_PAGE_STATUS_OK || references != 1) __builtin_trap();
        virt_uart_puts("BoarOS: SMP RFENCE error retains table and page owners\n");
        return -4;
    }
    long error = __real_sbi_remote_sfence(hart, address, size);
    if (!error && fence_hold) {
        __atomic_store_n(&fence_completed, 1, __ATOMIC_RELEASE);
        uint64_t end = riscv_time_read() + 50000000;
        while (!__atomic_load_n(&fence_release, __ATOMIC_ACQUIRE))
            require((int64_t)(riscv_time_read() - end) < 0, "RFENCE-test-timeout");
    }
    return error;
}

void riscv_smp_test_idle_boundary(struct kernel_cpu *cpu)
{
    if (!cpu->logical_id || !__atomic_load_n(&boundary_armed, __ATOMIC_ACQUIRE)) return;
    __atomic_store_n(&boundary_entered, 1, __ATOMIC_RELEASE);
    uint64_t end = riscv_time_read() + 50000000;
    while (!__atomic_load_n(&boundary_release, __ATOMIC_ACQUIRE))
        require((int64_t)(riscv_time_read() - end) < 0, "WFI-boundary-timeout");
}

static void wait_flag(const unsigned *flag, const char *what)
{
    uint64_t end = riscv_time_read() + 50000000;
    while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE))
        require((int64_t)(riscv_time_read() - end) < 0, what);
}

static void edge_wait(void *mode_argument)
{
    unsigned mode = (uintptr_t)mode_argument;
    while (!__atomic_load_n(&edge_can_park, __ATOMIC_ACQUIRE)) {}
    if (mode == 1) {
        uintptr_t irq = arch_interrupt_save();
        require(!sbi_set_timer(UINT64_MAX), "disable-CPU1-timer");
        asm volatile("csrc sie, %0" :: "r"((uintptr_t)32) : "memory");
        kernel_cpu_current()->timer.armed = 0;
        arch_interrupt_restore(irq);
    }
    enum kernel_wait_wake_reason reason;
    require(kernel_wait_prepare(&edge_queue, edge_deadline, 0, &edge_token) ==
            KERNEL_SCHEDULER_STATUS_OK, "edge-prepare");
    require(kernel_wait_park(&edge_token, &reason) == KERNEL_SCHEDULER_STATUS_OK,
            "edge-park");
    require(reason == (mode == 2 ? KERNEL_WAIT_TIMEOUT : KERNEL_WAIT_WOKEN), "edge-reason");
    require(kernel_wait_finish(&edge_token) == KERNEL_SCHEDULER_STATUS_OK, "edge-finish");
    if (mode == 1)
        require(riscv_timer_start(10000000, KERNEL_TICKS_PER_SECOND) == RISCV_TIMER_STATUS_OK,
                "restart-CPU1-timer");
    __atomic_store_n(&edge_finished, 1, __ATOMIC_RELEASE);
}

static void edge_cases(void)
{
    if (!riscv_smp_cpu(1)) return;
    kernel_wait_queue_init(&edge_queue);
    require(kernel_thread_create_joinable_mask(edge_wait, 0, &edge_join, 2) ==
            KERNEL_SCHEDULER_STATUS_OK, "edge-create");
    held_context = &edge_join.task->context;
    __atomic_store_n(&edge_can_park, 1, __ATOMIC_RELEASE);
    wait_flag(&handoff_entered, "early-wake-handshake");
    { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
      require(edge_join.task->wait.on_cpu && !edge_join.task->ready_node.queued &&
              riscv_smp_cpu(1)->switch_previous == edge_join.task, "old-stack-owner"); }
    require(kernel_wait_notify(&edge_token, KERNEL_WAIT_WOKEN) == KERNEL_SCHEDULER_STATUS_OK,
            "early-notify");
    require(kernel_wait_notify(&edge_token, KERNEL_WAIT_TIMEOUT) == KERNEL_SCHEDULER_STATUS_OK,
            "duplicate-notify");
    { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
      require(edge_join.task->wait.on_cpu && edge_join.task->wait.ready_pending &&
              !edge_join.task->ready_node.queued && !edge_join.task->eligible_nodes[1].queued,
              "early-ready-excluded"); }
    require(kernel_thread_set_cpu_mask(&edge_join, 1) == KERNEL_SCHEDULER_STATUS_BUSY,
            "old-stack-mask-busy");
    __atomic_store_n(&handoff_release, 1, __ATOMIC_RELEASE);
    kernel_thread_join(&edge_join);
    held_context = 0;
    require(edge_finished, "early-progress");
    edge_finished = edge_can_park = 0;
    require(kernel_thread_create_joinable_mask(edge_wait, (void *)1, &edge_join, 2) ==
            KERNEL_SCHEDULER_STATUS_OK, "WFI-create");
    __atomic_store_n(&edge_can_park, 1, __ATOMIC_RELEASE);
    uint64_t end = riscv_time_read() + 50000000;
    while (kernel_thread_set_cpu_mask(&edge_join, 2) == KERNEL_SCHEDULER_STATUS_BUSY)
        require((int64_t)(riscv_time_read() - end) < 0, "WFI-off-CPU-handshake");
    __atomic_store_n(&boundary_armed, 1, __ATOMIC_RELEASE);
    kernel_cpu_notify(riscv_smp_cpu(1), RISCV_IPI_HANDSHAKE);
    wait_flag(&boundary_entered, "WFI-checked-handshake");
    uintptr_t irq = arch_interrupt_save();
    require(kernel_wait_queue_wake_one(&edge_queue) == KERNEL_SCHEDULER_STATUS_OK, "WFI-edge-notify");
    __atomic_store_n(&boundary_release, 1, __ATOMIC_RELEASE);
    arch_interrupt_restore(irq);
    kernel_thread_join(&edge_join);
    __atomic_store_n(&boundary_armed, 0, __ATOMIC_RELEASE);
    require(edge_finished, "IPI-without-timer-progress");
    edge_finished = edge_can_park = 0;
    edge_deadline = riscv_time_read() + 500000;
    irq = arch_interrupt_save();
    require(!sbi_set_timer(UINT64_MAX), "deadline-disable-CPU0-periodic");
    require(kernel_thread_create_joinable_mask(edge_wait, (void *)2, &edge_join, 2) ==
            KERNEL_SCHEDULER_STATUS_OK, "remote-deadline-create");
    __atomic_store_n(&edge_can_park, 1, __ATOMIC_RELEASE);
    arch_interrupt_restore(irq);
    /* 不park/yield：仅CPU1的期限IPI能重装CPU0已取消的硬件timer。 */
    wait_flag(&edge_finished, "remote-deadline-rearm");
    kernel_thread_join(&edge_join);
    require(kernel_wait_queue_close(&edge_queue) == KERNEL_SCHEDULER_STATUS_OK &&
            kernel_wait_queue_destroy(&edge_queue) == KERNEL_SCHEDULER_STATUS_OK, "edge-queue-owner");
    virt_uart_puts("BoarOS: SMP early wake, WFI boundary and remote deadline passed\n");
}

static void fence_observer(void *unused)
{
    (void)unused;
    wait_flag(&fence_completed, "RFENCE-confirmation-handshake");
    uint32_t references;
    require(physical_page_reference_count(allocator, fence_table, &references) ==
            PHYSICAL_PAGE_STATUS_OK && references == 1, "RFENCE-retired-table-owner");
    require(physical_page_reference_count(allocator, fence_page, &references) ==
            PHYSICAL_PAGE_STATUS_OK && references == 1, "RFENCE-leaf-owner");
    __atomic_store_n(&fence_observed, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&fence_release, 1, __ATOMIC_RELEASE);
}

static void fence_case(void)
{
    if (!riscv_smp_cpu(1)) return;
    uint64_t va = RISCV_KERNEL_STACK_WINDOW_BASE + (UINT64_C(1) << 22);
    require(physical_page_allocate(allocator, &fence_page) == PHYSICAL_PAGE_STATUS_OK,
            "RFENCE-page-create");
    require(riscv_sv39_kernel_window_map(allocator, va, fence_page) == RISCV_SV39_STATUS_OK,
            "RFENCE-map");
    uint64_t *root, *level1;
    require(physical_page_resolve(allocator,
        (riscv_sv39_current_satp() & ((UINT64_C(1) << 44) - 1)) << 12,
        (void **)&root) == PHYSICAL_PAGE_STATUS_OK, "RFENCE-root");
    require(physical_page_resolve(allocator, (root[(va >> 30) & 511] >> 10) << 12,
        (void **)&level1) == PHYSICAL_PAGE_STATUS_OK, "RFENCE-level1");
    fence_table = (level1[(va >> 21) & 511] >> 10) << 12;
    require(kernel_thread_create_joinable_mask(fence_observer, 0, &edge_join, 2) ==
            KERNEL_SCHEDULER_STATUS_OK, "RFENCE-observer-create");
    uint64_t available = physical_page_available(allocator);
    fence_hold = 1;
    require(riscv_sv39_kernel_window_unmap(allocator, va) == RISCV_SV39_STATUS_OK,
            "RFENCE-unmap");
    fence_hold = 0;
    require(fence_observed && physical_page_available(allocator) == available + 1,
            "RFENCE-table-release-after-return");
    physical_page_release(allocator, fence_page);
    kernel_thread_join(&edge_join);
    virt_uart_puts("BoarOS: SMP delayed RFENCE return retains table and page owners\n");
}

static void alias_observer(void *unused)
{
    (void)unused;
    require(*(volatile uint64_t *)(uintptr_t)alias_address == UINT64_C(0x1111222233334444),
            "alias-original");
    __atomic_store_n(&alias_read, 1, __ATOMIC_RELEASE);
    wait_flag(&alias_continue, "alias-replacement-handshake");
    require(*(volatile uint64_t *)(uintptr_t)alias_address == UINT64_C(0x9999aaaabbbbcccc),
            "old-translation-invalidated");
    __atomic_store_n(&alias_checked, 1, __ATOMIC_RELEASE);
}

static void map_failure_and_reuse(void)
{
    uint64_t pages;
    require(physical_page_allocate_order(allocator, 1, &pages) == PHYSICAL_PAGE_STATUS_OK,
            "map-failure-pages");
    uint64_t va = RISCV_KERNEL_STACK_WINDOW_BASE + (UINT64_C(5) << 21) - 4096;
    uint64_t available = physical_page_available(allocator);
    allocation_attempt = 0;
    allocation_fail_after = 2;
    enum riscv_sv39_status status = riscv_sv39_kernel_window_map_pages(allocator, va, pages, 2);
    allocation_fail_after = 0;
    require(status == RISCV_SV39_STATUS_NO_MEMORY && allocation_attempt == 2 &&
            physical_page_available(allocator) == available, "second-table-OOM-rollback");
    require(riscv_sv39_kernel_window_map_pages(allocator, va, pages, 2) == RISCV_SV39_STATUS_OK &&
            riscv_sv39_kernel_window_unmap_pages(allocator, va, 2) == RISCV_SV39_STATUS_OK &&
            physical_page_available(allocator) == available, "OOM-retry-no-partial-leaves");
    if (riscv_smp_cpu(1)) {
        void *data;
        require(physical_page_resolve(allocator, pages, &data) == PHYSICAL_PAGE_STATUS_OK, "alias-page0");
        *(uint64_t *)data = UINT64_C(0x1111222233334444);
        require(physical_page_resolve(allocator, pages + 4096, &data) == PHYSICAL_PAGE_STATUS_OK, "alias-page1");
        *(uint64_t *)data = UINT64_C(0x9999aaaabbbbcccc);
        alias_address = RISCV_KERNEL_STACK_WINDOW_BASE + (UINT64_C(1) << 23);
        require(riscv_sv39_kernel_window_map(allocator, alias_address, pages) == RISCV_SV39_STATUS_OK,
                "alias-map-old");
        require(kernel_thread_create_joinable_mask(alias_observer, 0, &edge_join, 2) ==
                KERNEL_SCHEDULER_STATUS_OK, "alias-observer-create");
        wait_flag(&alias_read, "alias-cached-handshake");
        require(riscv_sv39_kernel_window_unmap(allocator, alias_address) == RISCV_SV39_STATUS_OK &&
                riscv_sv39_kernel_window_map(allocator, alias_address, pages + 4096) == RISCV_SV39_STATUS_OK,
                "alias-replace");
        __atomic_store_n(&alias_continue, 1, __ATOMIC_RELEASE);
        kernel_thread_join(&edge_join);
        require(alias_checked && riscv_sv39_kernel_window_unmap(allocator, alias_address) ==
                RISCV_SV39_STATUS_OK, "alias-observer-owner-return");
    }
    physical_page_release_order(allocator, pages, 1);
    virt_uart_puts("BoarOS: SMP map OOM rollback and stale translation replacement passed\n");
}

static void borrowed_exit(void *unused)
{
    (void)unused;
    wait_flag(&borrow_may_exit, "borrow-publication-handshake");
}

static void return_borrow(void *argument)
{
    if (borrow_migrate) {
        struct kernel_wait_token token;
        enum kernel_wait_wake_reason reason;
        require(kernel_wait_prepare(&borrow_gate, 0, 0, &token) == KERNEL_SCHEDULER_STATUS_OK &&
                kernel_wait_park(&token, &reason) == KERNEL_SCHEDULER_STATUS_OK &&
                kernel_wait_finish(&token) == KERNEL_SCHEDULER_STATUS_OK &&
                kernel_cpu_current()->logical_id == 0, "borrow-return-migration");
    }
    wait_flag(&borrow_put_allowed, "borrow-put-qualification");
    uintptr_t irq = arch_interrupt_save();
    scheduler_task_put(argument);
    if (!kernel_cpu_current()->logical_id) {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        require(scheduler.retired_pending == 1 && scheduler.retired_tasks &&
                scheduler.retired_tasks->retirement == 3, "controlled-put-queue-owner");
        __atomic_store_n(&borrow_owner_checked, 1, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&borrow_put_done, 1, __ATOMIC_RELEASE);
    arch_interrupt_restore(irq);
}

static void retirement_case(void)
{
    if (!riscv_smp_cpu(1)) return;
    for (unsigned route = 0; route < 3; route++) {
        borrow_may_exit = borrow_put_done = borrow_put_allowed = borrow_owner_checked = 0;
        borrow_migrate = route == 2;
        kernel_wait_queue_init(&borrow_gate);
        uint64_t available = physical_page_available(allocator);
        require(kernel_thread_create_joinable_mask(borrowed_exit, 0, &edge_join, 2) ==
                KERNEL_SCHEDULER_STATUS_OK, "borrow-target-create");
        struct kernel_task *borrow;
        { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
          borrow = edge_join.task;
          scheduler_task_get_locked(borrow); }
        __atomic_store_n(&borrow_may_exit, 1, __ATOMIC_RELEASE);
        kernel_thread_join(&edge_join);
        { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
          require(borrow->retirement == 2 && borrow->storage_borrows == 1 &&
                  borrow->stack_physical_address == KERNEL_THREAD_NO_PAGE && !borrow->wait.on_cpu,
                  "borrow-retains-only-storage"); }
        require(physical_page_available(allocator) == available - 1, "borrow-metadata-retained");
        require(kernel_thread_create_joinable_mask(return_borrow, borrow, &edge_join, route == 1 ? 1 : 2) ==
                KERNEL_SCHEDULER_STATUS_OK, "borrow-returner-create");
        if (borrow_migrate) {
            uint64_t end = riscv_time_read() + 50000000;
            for (;;) {
                int blocked;
                { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
                  blocked = edge_join.task->state == KERNEL_THREAD_STATE_BLOCKED &&
                            !edge_join.task->wait.on_cpu; }
                if (blocked) break;
                require((int64_t)(riscv_time_read() - end) < 0, "borrow-park-handshake");
            }
            enum kernel_scheduler_status mask_status = kernel_thread_set_cpu_mask(&edge_join, 1);
            require(mask_status == KERNEL_SCHEDULER_STATUS_OK, "borrow-migrate-mask");
            KERNEL_IRQ_SCOPE(borrow_wake_irq);
            require(kernel_wait_queue_wake_one(&borrow_gate) == KERNEL_SCHEDULER_STATUS_OK,
                    "borrow-migrate-wake");
        }
        /* CPU1归还期间CPU0禁止抢占，检查队列owner；CPU0受控归还则由actor
         * 在IRQ关闭区检查，避免把后续合法idle收集误判为提前释放。 */
        if (!route) kernel_preempt_disable();
        __atomic_store_n(&borrow_put_allowed, 1, __ATOMIC_RELEASE);
        wait_flag(&borrow_put_done, "remote-borrow-return");
        if (!route) {
            { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
              require(scheduler.retired_pending == 1 && scheduler.retired_tasks &&
                      scheduler.retired_tasks->retirement == 3, "remote-put-retirement-owner"); }
            require(physical_page_available(allocator) == available - 4, "remote-put-no-early-free");
            kernel_preempt_enable();
        } else require(borrow_owner_checked, "controlled-put-owner-check");
        kernel_thread_join(&edge_join);
        require(physical_page_available(allocator) == available, "borrow-final-owner-return");
        require(!borrow_gate.registrations && !borrow_gate.borrows, "borrow-wait-owner-return");
    }
    virt_uart_puts("BoarOS: SMP join/retirement borrow and CPU0 collection passed\n");
}

static void global_tick_case(void)
{
    uintptr_t irq = arch_interrupt_save();
    uint64_t period = kernel_cpu_current()->timer.period;
    uint64_t before = riscv_time_read(), ticks = kernel_tick_count();
    struct kernel_wait_token token;
    enum kernel_wait_wake_reason reason;
    require(kernel_wait_prepare(0, before + period * 20, 0, &token) == KERNEL_SCHEDULER_STATUS_OK &&
            kernel_wait_park(&token, &reason) == KERNEL_SCHEDULER_STATUS_OK &&
            reason == KERNEL_WAIT_TIMEOUT && kernel_wait_finish(&token) == KERNEL_SCHEDULER_STATUS_OK,
            "global-tick-timeout");
    uint64_t elapsed = (riscv_time_read() - before) / period;
    uint64_t advanced = kernel_tick_count() - ticks;
    require(advanced + 2 >= elapsed && advanced <= elapsed + 2, "global-tick-not-doubled");
    arch_interrupt_restore(irq);
    virt_uart_puts("BoarOS: SMP global tick/timekeeper single advancement passed\n");
}

static void lock_worker(void *argument)
{
    unsigned id = (uintptr_t)argument;
    KERNEL_LOCK_SCOPE(guard);
    if (id == 3) {
        kernel_rwlock_write(&shared_lock, &guard);
        require(readers_left == 3 && !later_entered, "RW-writer-FIFO-boundary");
        lock_value = UINT64_C(0x123456789abc);
        __atomic_store_n(&writer_entered, 1, __ATOMIC_RELEASE);
    } else {
        kernel_rwlock_read(&shared_lock, &guard);
        if (id < 3) {
            require(!__atomic_load_n(&writer_entered, __ATOMIC_ACQUIRE), "RW-first-reader-batch");
            __atomic_fetch_add(&readers_entered, 1, __ATOMIC_RELEASE);
            while (__atomic_load_n(&readers_entered, __ATOMIC_ACQUIRE) != 3) {}
            __atomic_fetch_add(&readers_left, 1, __ATOMIC_RELEASE);
        } else {
            require(__atomic_load_n(&writer_entered, __ATOMIC_ACQUIRE) &&
                    lock_value == UINT64_C(0x123456789abc), "RW-later-reader-no-bypass");
            __atomic_fetch_add(&later_entered, 1, __ATOMIC_RELEASE);
        }
    }
}

static void lock_case(void)
{
    if (!riscv_smp_cpu(1)) return;
    kernel_rwlock_init(&shared_lock, 50, (uintptr_t)&shared_lock);
    struct kernel_lock_guard initial = {0};
    kernel_rwlock_write(&shared_lock, &initial);
    for (unsigned i = 0; i < 8; i++) {
        require(kernel_thread_create_joinable_mask(lock_worker, (void *)(uintptr_t)i,
                &lock_joins[i], 2) == KERNEL_SCHEDULER_STATUS_OK, "RW-waiter-create");
        uint64_t end = riscv_time_read() + 50000000;
        for (;;) {
            unsigned registered;
            { KERNEL_RAW_SCOPE(guard, &shared_lock.metadata);
              registered = shared_lock.waiters.registrations; }
            if (registered == i + 1 && kernel_thread_set_cpu_mask(&lock_joins[i], 1U << (i % 2)) ==
                KERNEL_SCHEDULER_STATUS_OK) break;
            require((int64_t)(riscv_time_read() - end) < 0, "RW-registered-off-CPU-handshake");
        }
    }
    kernel_lock_release(&initial);
    for (unsigned i = 0; i < 8; i++) kernel_thread_join(&lock_joins[i]);
    require(readers_left == 3 && writer_entered && later_entered == 4 &&
            !shared_lock.readers && !shared_lock.writer && !shared_lock.pending_head &&
            !shared_lock.handoff_owner && !shared_lock.waiters.registrations &&
            !shared_lock.waiters.borrows, "RW-final-owner-return");
    require(kernel_wait_queue_close(&shared_lock.waiters) == KERNEL_SCHEDULER_STATUS_OK &&
            kernel_wait_queue_destroy(&shared_lock.waiters) == KERNEL_SCHEDULER_STATUS_OK,
            "RW-queue-destroy");
    virt_uart_puts("BoarOS: SMP RW reader batch, writer FIFO and qualification owners passed\n");
}

static void policy_worker(void *argument)
{
    unsigned id = (uintptr_t)argument;
    struct kernel_wait_token token;
    enum kernel_wait_wake_reason reason;
    require(kernel_wait_prepare(&policy_gate, 0, 0, &token) == KERNEL_SCHEDULER_STATUS_OK &&
            kernel_wait_park(&token, &reason) == KERNEL_SCHEDULER_STATUS_OK &&
            kernel_wait_finish(&token) == KERNEL_SCHEDULER_STATUS_OK, "policy-gate");
    if (id) { __atomic_store_n(&policy_peer, 1, __ATOMIC_RELEASE); return; }
    if (policy_kind == KERNEL_SCHED_FIFO) {
        uint64_t until = riscv_time_read() + kernel_cpu_current()->timer.period * 3;
        while ((int64_t)(riscv_time_read() - until) < 0)
            require(!__atomic_load_n(&policy_peer, __ATOMIC_ACQUIRE), "FIFO-no-tick-rotation");
        uintptr_t irq = arch_interrupt_save();
        require(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, "FIFO-same-priority-yield");
        arch_interrupt_restore(irq);
    }
    /* RR没有主动yield，只有本地时间片结算能让同级peer进入。 */
    wait_flag(&policy_peer, "policy-peer-progress");
}

static void policy_cases(void)
{
    if (!riscv_smp_cpu(1)) return;
    int64_t old_period, old_runtime;
    kernel_scheduler_rt_bandwidth_get(&old_period, &old_runtime);
    require(!kernel_scheduler_rt_bandwidth_set(1, -1), "policy-budget-disable");
    for (unsigned mode = 0; mode < 2; mode++) {
        policy_kind = mode ? KERNEL_SCHED_RR : KERNEL_SCHED_FIFO;
        policy_peer = 0;
        kernel_wait_queue_init(&policy_gate);
        for (unsigned id = 0; id < 2; id++) {
            require(kernel_thread_create_joinable_mask(policy_worker, (void *)(uintptr_t)id,
                    &policy_joins[id], 2) == KERNEL_SCHEDULER_STATUS_OK, "policy-create");
            uint64_t end = riscv_time_read() + 50000000;
            while (kernel_thread_set_cpu_mask(&policy_joins[id], 2) == KERNEL_SCHEDULER_STATUS_BUSY)
                require((int64_t)(riscv_time_read() - end) < 0, "policy-blocked-handshake");
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            require(!kernel_sched_policy_set(&policy_joins[id].task->scheduling,
                    policy_kind, 60, 0), "policy-set-off-CPU");
        }
        uintptr_t irq = arch_interrupt_save();
        require(kernel_wait_queue_wake_all(&policy_gate) == KERNEL_SCHEDULER_STATUS_OK, "policy-release");
        arch_interrupt_restore(irq);
        for (unsigned id = 0; id < 2; id++) kernel_thread_join(&policy_joins[id]);
        require(policy_peer && kernel_wait_queue_close(&policy_gate) == KERNEL_SCHEDULER_STATUS_OK &&
                kernel_wait_queue_destroy(&policy_gate) == KERNEL_SCHEDULER_STATUS_OK, "policy-owner-return");
    }
    require(!kernel_scheduler_rt_bandwidth_set(1, old_runtime), "policy-budget-restore");
    virt_uart_puts("BoarOS: SMP per-CPU FIFO/yield and RR/timer qualification passed\n");
}

static void track(void *pointer, size_t bytes, unsigned id, int acquire)
{
    uint64_t address;
    require(arena_physical(pointer, &address) && address >= arena_address &&
            address - arena_address + bytes <= (UINT64_C(1) << 22), "arena-track-range");
    unsigned first = (address - arena_address) / 16, count = (bytes + 15) / 16;
    for (unsigned i = first; i < first + count; i++) {
        require(owners[i] == (acquire ? 0 : id + 1), "arena-unique-owner");
        owners[i] = acquire ? id + 1 : 0;
    }
}

static void arena_worker(void *argument)
{
    unsigned id = (uintptr_t)argument;
    __atomic_fetch_add(&arena_entered, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&arena_entered, __ATOMIC_ACQUIRE) != 2) {}
    for (unsigned iteration = 0; iteration < 1000; iteration++) {
        uint64_t page;
        unsigned order = iteration % 4;
        require(physical_page_allocate_order(&arena, order, &page) == PHYSICAL_PAGE_STATUS_OK,
                "arena-page-allocate");
        size_t bytes = 4096U << order;
        uint8_t *data = arena_access(page);
        { KERNEL_LOCK_SCOPE(guard); kernel_mutex_lock(&model, &guard); track(data, bytes, id, 1); }
        for (size_t i = 0; i < bytes; i++) data[i] = (uint8_t)(id + iteration);
        for (size_t i = 0; i < bytes; i++) require(data[i] == (uint8_t)(id + iteration), "arena-page-content");
        { KERNEL_LOCK_SCOPE(guard); kernel_mutex_lock(&model, &guard); track(data, bytes, id, 0); }
        require(physical_page_release_order(&arena, page, order) == PHYSICAL_PAGE_STATUS_OK,
                "arena-page-release");
        require(physical_page_acquire(&arena, shared_page) == PHYSICAL_PAGE_STATUS_OK,
                "arena-shared-acquire");
        require(physical_page_release(&arena, shared_page) == PHYSICAL_PAGE_STATUS_OK,
                "arena-shared-release");
        bytes = iteration % 10 < 8 ? (size_t)16 << (iteration % 8) : 4096 + id * 4096;
        void *object;
        require(kernel_heap_allocate(&heap, bytes, &object) == KERNEL_HEAP_STATUS_OK, "arena-slab");
        { KERNEL_LOCK_SCOPE(guard); kernel_mutex_lock(&model, &guard); track(object, bytes, id, 1); }
        for (size_t i = 0; i < bytes; i++) ((uint8_t *)object)[i] = id + 0x35;
        if (!(iteration % 8)) {
            /* 仅ledger交接覆盖resize；普通buddy/slab元数据操作仍在两核并行。 */
            KERNEL_LOCK_SCOPE(guard);
            kernel_mutex_lock(&model, &guard);
            void *resized;
            require(kernel_heap_resize(&heap, object, bytes * 2, &resized) == KERNEL_HEAP_STATUS_OK,
                    "arena-resize");
            for (size_t i = 0; i < bytes; i++) require(((uint8_t *)resized)[i] == id + 0x35, "arena-resize-content");
            track(object, bytes, id, 0);
            object = resized;
            bytes *= 2;
            track(object, bytes, id, 1);
            for (size_t i = 0; i < bytes; i++) ((uint8_t *)object)[i] = id + 0x35;
        }
        for (size_t i = 0; i < bytes; i++) require(((uint8_t *)object)[i] == id + 0x35, "arena-slab-content");
        { KERNEL_LOCK_SCOPE(guard); kernel_mutex_lock(&model, &guard); track(object, bytes, id, 0); }
        require(kernel_heap_release(&heap, object) == KERNEL_HEAP_STATUS_OK, "arena-slab-release");
    }
}

static void arena_drain(void *argument)
{
    unsigned id = (uintptr_t)argument;
    __atomic_fetch_add(&drain_entered, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&drain_entered, __ATOMIC_ACQUIRE) != 2) {}
    for (;;) {
        uint64_t page;
        enum physical_page_status status = physical_page_allocate(&arena, &page);
        if (status == PHYSICAL_PAGE_STATUS_EMPTY) break;
        require(status == PHYSICAL_PAGE_STATUS_OK && drain_counts[id] < 1024, "arena-drain-allocate");
        unsigned index = drain_counts[id]++;
        drained_pages[id][index] = page;
        void *data = arena_access(page);
        { KERNEL_LOCK_SCOPE(guard); kernel_mutex_lock(&model, &guard); track(data, 4096, id, 1); }
        *(uint64_t *)data = UINT64_C(0x44520000) + (id << 12) + index;
    }
    __atomic_fetch_add(&drain_finished, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&drain_finished, __ATOMIC_ACQUIRE) != 2) {}
    for (unsigned i = 0; i < drain_counts[id]; i++) {
        void *data = arena_access(drained_pages[id][i]);
        require(*(uint64_t *)data == UINT64_C(0x44520000) + (id << 12) + i, "arena-drain-content");
        { KERNEL_LOCK_SCOPE(guard); kernel_mutex_lock(&model, &guard); track(data, 4096, id, 0); }
        require(physical_page_release(&arena, drained_pages[id][i]) == PHYSICAL_PAGE_STATUS_OK,
                "arena-drain-release");
    }
}

static void parallel_allocator(void)
{
    if (!riscv_smp_cpu(1)) return;
    require(physical_page_allocate_order(allocator, 10, &arena_address) == PHYSICAL_PAGE_STATUS_OK,
            "arena-reserve");
    struct boot_memory_layout layout = {.usable_count = 1,
        .usable = {{arena_address, UINT64_C(1) << 22}}};
    require(physical_page_allocator_init(&arena, &layout) == PHYSICAL_PAGE_STATUS_OK &&
            physical_page_allocator_bind_access(&arena, arena_access) == PHYSICAL_PAGE_STATUS_OK &&
            physical_page_allocator_finalize(&arena) == PHYSICAL_PAGE_STATUS_OK &&
            kernel_heap_init(&heap, &arena, arena_physical) == KERNEL_HEAP_STATUS_OK, "arena-initialize");
    require(!arena.reclaim_hooks.reclaim && !arena.reclaim_hooks.notify && !arena.reclaim_hooks.wait, "arena-no-pressure");
    uint64_t available = physical_page_available(&arena);
    require(physical_page_allocate(&arena, &shared_page) == PHYSICAL_PAGE_STATUS_OK, "arena-shared-owner");
    kernel_mutex_init(&model, 1, (uintptr_t)&model);
    for (unsigned i = 0; i < 2; i++)
        require(kernel_thread_create_joinable_mask(arena_worker, (void *)(uintptr_t)i, &joins[i],
                1U << i) == KERNEL_SCHEDULER_STATUS_OK, "arena-worker-create");
    for (unsigned i = 0; i < 2; i++) kernel_thread_join(&joins[i]);
    require(physical_page_release(&arena, shared_page) == PHYSICAL_PAGE_STATUS_OK, "arena-shared-owner-release");
    struct kernel_heap_statistics stats;
    kernel_heap_get_statistics(&heap, &stats);
    require(!stats.live_allocations && !stats.current_pages && physical_page_available(&arena) == available,
            "arena-baseline");
    for (unsigned i = 0; i < 2; i++)
        require(kernel_thread_create_joinable_mask(arena_drain, (void *)(uintptr_t)i, &joins[i],
                1U << i) == KERNEL_SCHEDULER_STATUS_OK, "arena-drainer-create");
    for (unsigned i = 0; i < 2; i++) kernel_thread_join(&joins[i]);
    require(drain_counts[0] + drain_counts[1] == available &&
            physical_page_available(&arena) == available, "arena-exhaustion-owner-return");
    for (unsigned i = 0; i < sizeof(owners); i++) require(!owners[i], "arena-ledger-empty");
    require(!model.lock.writer && !model.lock.pending_head && !model.lock.readers &&
            !model.lock.waiters.registrations && !model.lock.waiters.borrows, "arena-lock-owner");
    require(physical_page_allocator_audit(&arena) == PHYSICAL_PAGE_STATUS_OK, "arena-audit");
    physical_page_release_order(allocator, arena_address, 10);
    virt_uart_puts("BoarOS: SMP private arena buddy/ref/slab/resize and audit passed\n");
}

#ifdef BOAROS_SMP_FPU_SETUP_TEST
static unsigned fpu_setup_attempts, fpu_setup_unmasked;
#endif
/* 受控kernel测试把局部图像装入硬件；Dirty发布后才属于当前任务。 */
static void install_test_fpu(struct riscv_fpu_state *image)
{
    /* restore以Clean结束，但图像此时还不是task->fpu；禁止在Dirty
     * 发布前抢占。正常用户FP指令由硬件原子地标Dirty，不存在此测试间隙。 */
    KERNEL_IRQ_SCOPE(fpu_setup_irq);
    riscv_fpu_state_restore(image);
#ifdef BOAROS_SMP_FPU_SETUP_TEST
    __atomic_fetch_add(&fpu_setup_attempts,1,__ATOMIC_RELAXED);
    if (arch_interrupt_is_enabled()) {
        __atomic_fetch_add(&fpu_setup_unmasked,1,__ATOMIC_RELAXED);
        uintptr_t irq=arch_interrupt_save();
        require(kernel_scheduler_on_tick(1)==KERNEL_SCHEDULER_STATUS_OK,"fpu-setup-forced-tick");
        arch_interrupt_restore(irq);
    }
#endif
    asm volatile("csrs sstatus, %0" :: "r"((uintptr_t)0x6000) : "memory");
}

static void check_live_fs_return(void)
{
    KERNEL_IRQ_SCOPE(fs_irq);
    uintptr_t original;
    __asm__ volatile("csrr %0, sstatus" : "=r"(original));
    const uintptr_t states[]={0,RISCV_SSTATUS_FS_INITIAL,RISCV_SSTATUS_FS_CLEAN,RISCV_SSTATUS_FS_DIRTY};
    for(unsigned i=0;i<4;i++) {
        uintptr_t status=(original & ~RISCV_SSTATUS_FS_MASK) | states[i];
        __asm__ volatile("csrw sstatus, %0" :: "r"(status) : "memory");
        struct riscv_trap_frame frame={.sstatus=RISCV_SSTATUS_SPP|RISCV_SSTATUS_SPIE|states[3-i]};
        riscv_trap_return_prepare(&frame);
        require((frame.sstatus & RISCV_SSTATUS_FS_MASK)==states[i],"trap-return-live-FS");
        require((frame.sstatus & ~RISCV_SSTATUS_FS_MASK)==(RISCV_SSTATUS_SPP|RISCV_SSTATUS_SPIE),"trap-return-FS-only");
    }
    __asm__ volatile("csrw sstatus, %0" :: "r"(original) : "memory");
}

static void compute(void *argument)
{
    unsigned id = (uintptr_t)argument;
    require(kernel_cpu_current()->logical_id == id / 2, "compute-affinity");
    require(__atomic_exchange_n(&executions[id], 1, __ATOMIC_ACQ_REL) == 0, "execution-unique");
    struct riscv_fpu_state expected = {.fcsr = 0x61, .saved = 1}, actual;
    for (unsigned i = 0; i < 32; i++) expected.regs[i] = UINT64_C(0x3ff0010000000000) + id * 256 + i;
    install_test_fpu(&expected);
    check_live_fs_return();
    __atomic_fetch_add(&entered, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&entered, __ATOMIC_ACQUIRE) != 4) {}
    /* 没有主动yield：同CPU伙伴必须经过timer抢占才能进入并前进。 */
    for (;;) {
        __atomic_fetch_add(&counters[id], 1, __ATOMIC_RELAXED);
        unsigned all = 1;
        for (unsigned i = 0; i < 4; i++) all &= __atomic_load_n(&counters[i], __ATOMIC_ACQUIRE) >= 20000;
        if (all) break;
    }
    riscv_fpu_state_save(&actual);
    require(actual.fcsr == expected.fcsr, "preempt-FCSR");
    for (unsigned i = 0; i < 32; i++) {
        if (actual.regs[i]!=expected.regs[i]) {
            virt_uart_puts("BoarOS: preempt FD task="); virt_uart_put_hex(id);
            virt_uart_puts(" reg="); virt_uart_put_hex(i);
            virt_uart_puts(" expected="); virt_uart_put_hex(expected.regs[i]);
            virt_uart_puts(" actual="); virt_uart_put_hex(actual.regs[i]); virt_uart_putc('\n');
        }
        require(actual.regs[i] == expected.regs[i], "preempt-FD");
    }
    require(__atomic_exchange_n(&executions[id], 0, __ATOMIC_ACQ_REL) == 1, "execution-release");
}

void riscv_smp_park_migration(void)
{
    struct kernel_wait_token token;
    enum kernel_wait_wake_reason reason;
    require(kernel_wait_prepare(&migrate_queue, 0, 0, &token) == KERNEL_SCHEDULER_STATUS_OK,
            "migration-prepare");
    require(kernel_wait_park(&token, &reason) == KERNEL_SCHEDULER_STATUS_OK, "migration-park");
    require(kernel_wait_finish(&token) == KERNEL_SCHEDULER_STATUS_OK, "migration-finish");
}

static void migrate(void *unused)
{
    unsigned source = (uintptr_t)unused;
    uintptr_t sp = arch_current_stack();
    require(kernel_cpu_current()->logical_id == source, "migration-source");
    struct riscv_fpu_state expected = {.fcsr = 0x83, .saved = 1}, actual;
    for (unsigned i = 0; i < 32; i++) expected.regs[i] = UINT64_C(0x4000010000000000) + i;
    install_test_fpu(&expected);
    require(riscv_smp_gpr_wait(), "migration-integer-context-PC");
    require(kernel_cpu_current()->logical_id == (source ^ 1U) && arch_current_stack() == sp,
            "migration-destination-SP");
    riscv_fpu_state_save(&actual);
    require(actual.fcsr == expected.fcsr, "migration-FCSR");
    for (unsigned i = 0; i < 32; i++) require(actual.regs[i] == expected.regs[i], "migration-FD");
    __atomic_store_n(&migrated, 1, __ATOMIC_RELEASE);
}

static void shared_compute(void *argument)
{
    unsigned id = (uintptr_t)argument;
    struct kernel_task *task = kernel_task_current();
    uintptr_t sp = arch_current_stack();
    struct riscv_fpu_state expected = {.fcsr = 0x41, .saved = 1}, actual;
    for (unsigned i = 0; i < 32; i++) expected.regs[i] = UINT64_C(0x3ff0020000000000) + id * 256 + i;
    install_test_fpu(&expected);
    __atomic_fetch_add(&shared_entered, 1, __ATOMIC_RELEASE);
    uint64_t end = riscv_time_read() + 50000000;
    while (__atomic_load_n(&shared_entered, __ATOMIC_ACQUIRE) < 2) {
        require((int64_t)(riscv_time_read() - end) < 0, "shared-eligibility-entry");
    }
    for (;;) {
        unsigned logical;
        { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
          struct kernel_cpu *cpu = kernel_cpu_current();
          require(cpu->current == task && task->cpu == cpu && task->wait.on_cpu &&
                  !task->ready_node.queued, "shared-eligibility-execution-owner");
          logical = cpu->logical_id; }
        __atomic_fetch_or(&shared_cpus, 1U << logical, __ATOMIC_RELEASE);
        __atomic_fetch_add(&shared_counts[id], 1, __ATOMIC_RELAXED);
        if (__atomic_load_n(&shared_counts[0], __ATOMIC_ACQUIRE) >= 20000 &&
            __atomic_load_n(&shared_counts[1], __ATOMIC_ACQUIRE) >= 20000 &&
            __atomic_load_n(&shared_cpus, __ATOMIC_ACQUIRE) == 3) break;
        require((int64_t)(riscv_time_read() - end) < 0, "shared-eligibility-progress");
    }
    riscv_fpu_state_save(&actual);
    require(arch_current_stack() == sp && actual.fcsr == expected.fcsr, "shared-eligibility-SP-FCSR");
    for (unsigned i = 0; i < 32; i++) require(actual.regs[i] == expected.regs[i], "shared-eligibility-FD");
}

static void parallel_tasks(void)
{
    if (!riscv_smp_cpu(1)) return;
    for (unsigned i = 0; i < 4; i++)
        require(kernel_thread_create_joinable_mask(compute, (void *)(uintptr_t)i, &joins[i],
                1U << (i / 2)) == KERNEL_SCHEDULER_STATUS_OK, "compute-create");
    for (unsigned i = 0; i < 4; i++) kernel_thread_join(&joins[i]);
    for (unsigned i = 0; i < 4; i++) require(counters[i] >= 20000 && !executions[i], "compute-progress");
    virt_uart_puts("BoarOS: FPU trap return preserved live Off/Initial/Clean/Dirty on four owners\n");
    for (unsigned i = 0; i < 2; i++)
        require(kernel_thread_create_joinable_mask(shared_compute, (void *)(uintptr_t)i, &joins[i], 3) ==
                KERNEL_SCHEDULER_STATUS_OK, "shared-eligibility-create");
    for (unsigned i = 0; i < 2; i++) kernel_thread_join(&joins[i]);
    require(shared_cpus == 3 && shared_counts[0] >= 20000 && shared_counts[1] >= 20000,
            "shared-eligibility-owner-return");
    virt_uart_puts("BoarOS: SMP dual CPU eligibility, SP and FD ownership passed\n");
    for (unsigned source = 0; source < 2; source++) {
        migrated = 0;
        kernel_wait_queue_init(&migrate_queue);
        require(kernel_thread_create_joinable_mask(migrate, (void *)(uintptr_t)source, &migration, 1U << source) ==
                KERNEL_SCHEDULER_STATUS_OK, "migration-create");
        uint64_t end = riscv_time_read() + UINT64_C(50000000);
        enum kernel_scheduler_status status;
        do {
            status = kernel_thread_set_cpu_mask(&migration, 1U << (source ^ 1U));
            require(status == KERNEL_SCHEDULER_STATUS_BUSY || status == KERNEL_SCHEDULER_STATUS_OK,
                    "migration-mask-status");
            require((int64_t)(riscv_time_read() - end) < 0, "migration-handshake-timeout");
        } while (status == KERNEL_SCHEDULER_STATUS_BUSY);
        uintptr_t irq = arch_interrupt_save();
        require(kernel_wait_queue_wake_one(&migrate_queue) == KERNEL_SCHEDULER_STATUS_OK,
                "migration-wake");
        arch_interrupt_restore(irq);
        kernel_thread_join(&migration);
        require(migrated && !migrate_queue.registrations && !migrate_queue.borrows &&
                kernel_wait_queue_close(&migrate_queue) == KERNEL_SCHEDULER_STATUS_OK &&
                kernel_wait_queue_destroy(&migrate_queue) == KERNEL_SCHEDULER_STATUS_OK,
                "migration-owner-return");
    }
    virt_uart_puts("BoarOS: SMP timer preemption, unique execution and FD migration passed\n");
#ifdef BOAROS_SMP_FPU_SETUP_TEST
    require(fpu_setup_attempts==8 && !fpu_setup_unmasked,"fpu-setup-owner-publication");
    virt_uart_puts("BoarOS: FPU setup interrupt boundary protected; 8 owners returned\n");
#endif
}

static void creation_noop(void *unused) { (void)unused; }
static void creation_actor(void *unused)
{
    (void)unused;
    creation_result = kernel_thread_create_joinable_mask(creation_noop, 0, &creation_target_join, 2);
}
static void creation_stop_race(void)
{
    if (!riscv_smp_cpu(1)) return;
    uint64_t available = physical_page_available(allocator);
    kernel_wait_queue_init(&creation_gate);
    uintptr_t irq = arch_interrupt_save();
    require(kernel_thread_create_joinable(creation_actor, 0, &creation_join) ==
            KERNEL_SCHEDULER_STATUS_OK, "creation-race-actor");
    creation_race_armed = 1;
    arch_interrupt_restore(irq);
    wait_flag(&creation_waiting, "creation-private-allocation-handshake");
    { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
      require(creation_join.task->state == KERNEL_THREAD_STATE_BLOCKED &&
              !creation_join.task->wait.on_cpu && !scheduler.controlled_owners &&
              riscv_smp_cpu(1)->current == riscv_smp_cpu(1)->idle,
              "creation-private-owner-state"); }
    riscv_smp_stop();
    require(__atomic_load_n(&riscv_smp_cpu(1)->state, __ATOMIC_ACQUIRE) == KERNEL_CPU_STOPPED,
            "creation-HSM-stopped");
    { KERNEL_IRQ_SCOPE(creation_wake_irq);
      require(kernel_wait_queue_wake_one(&creation_gate) == KERNEL_SCHEDULER_STATUS_OK,
              "creation-release"); }
    kernel_thread_join(&creation_join);
    require(creation_result == KERNEL_SCHEDULER_STATUS_INVALID_STATE && !creation_target_join.task &&
            !creation_gate.registrations && !creation_gate.borrows &&
            physical_page_available(allocator) == available, "creation-stopped-rollback-owner");
    { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
      require(!scheduler.controlled_owners && !riscv_smp_cpu(1)->eligible.first,
              "creation-stopped-no-publication"); }
    virt_uart_puts("BoarOS: SMP sleeping create rechecks STOPPED and returns private owners\n");
}

static void orchestrate(void *unused)
{
    (void)unused;
    uint64_t page;
    require(physical_page_allocate_order(allocator, 1, &page) == PHYSICAL_PAGE_STATUS_OK,
            "test-pages");
    /* 两页跨2MiB表边界；第二个表准备失败不得提交第一个叶子。 */
    uint64_t va = RISCV_KERNEL_STACK_WINDOW_BASE + (UINT64_C(1) << 21) - 4096;
    require(riscv_sv39_kernel_window_map_pages(allocator, va, page, 2) == RISCV_SV39_STATUS_OK,
            "batch-map");
    if (riscv_smp_cpu(1)) require(!last_fence_address && (!last_fence_size || last_fence_size == UINT64_MAX),
                                  "nonleaf-map-full-RFENCE");
    *(volatile uint64_t *)(uintptr_t)va = 0x12345678;
    *(volatile uint64_t *)(uintptr_t)(va + 4096) = 0x87654321;
    void *physical;
    require(physical_page_resolve(allocator, page, &physical) == PHYSICAL_PAGE_STATUS_OK &&
            *(uint64_t *)physical == 0x12345678, "batch-alias");
    require(riscv_sv39_kernel_window_unmap_pages(allocator, va, 2) == RISCV_SV39_STATUS_OK,
            "batch-unmap");
    physical_page_release_order(allocator, page, 1);
    parallel_tasks();
    global_tick_case();
    if (!root_mode) {
        edge_cases();
        fence_case();
        map_failure_and_reuse();
        retirement_case();
        lock_case();
        policy_cases();
    }
    parallel_allocator();
    if (!root_mode) creation_stop_race();
    if (root_mode) kernel_thread_join(&root_background_join);
    riscv_smp_stop();
    __atomic_store_n(&done, 1, __ATOMIC_RELEASE);
}

static void guard_fault(void *unused)
{
    (void)unused;
    struct kernel_task *task = kernel_task_current();
    volatile uint64_t *guard = (void *)(task->stack_low - KERNEL_STACK_GUARD_BYTES - 8);
    (void)*guard;
    riscv_smp_fail("guard-was-readable", -1);
}

void riscv_smp_test_start(struct physical_page_allocator *pages, int has_root)
{
    allocator = pages;
    root_mode = has_root;
    baseline = physical_page_available(pages);
    if (BOAROS_SMP_NEGATIVE == 3) {
        require(kernel_thread_create_joinable_mask(guard_fault, 0, &edge_join, 2) ==
                KERNEL_SCHEDULER_STATUS_OK, "guard-create");
        return;
    }
    if (root_mode) {
        require(riscv_smp_cpu(1) && kernel_thread_create_joinable_mask(root_background, 0,
                &root_background_join, 2) == KERNEL_SCHEDULER_STATUS_OK, "root-background-create");
        wait_flag(&root_background_started, "root-background-start");
        root_background_begin = __atomic_load_n(&root_background_count, __ATOMIC_ACQUIRE);
        require(kernel_thread_create_joinable(orchestrate, 0, &root_controller_join) ==
                KERNEL_SCHEDULER_STATUS_OK, "root-controller-create");
    } else require(kernel_thread_create(orchestrate, 0) == KERNEL_SCHEDULER_STATUS_OK, "test-create");
}

void riscv_smp_test_poll(void)
{
    if (!__atomic_load_n(&done, __ATOMIC_ACQUIRE)) return;
    { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
      require(!scheduler.all_tasks && !scheduler.task_count && !scheduler.runqueue.first &&
              !scheduler.blocked_head && !scheduler.deadline_root && !scheduler.exited_head &&
              !scheduler.retired_pending && !scheduler.retired_tasks && !scheduler.controlled_owners &&
              !kernel_cpu_current()->switch_previous, "test-scheduler-owner-baseline"); }
    require(physical_page_available(allocator) == baseline, "test-page-baseline");
    require(physical_page_allocator_audit(allocator) == PHYSICAL_PAGE_STATUS_OK, "test-audit");
    virt_uart_emergency_begin();
    virt_uart_puts("BoarOS: SMP task-bytes="); virt_uart_put_hex(sizeof(struct kernel_task));
    virt_uart_puts(" cpu-bytes="); virt_uart_put_hex(sizeof(struct kernel_cpu));
    for (unsigned i = 0; i < 2; i++) {
        struct kernel_cpu *cpu = riscv_smp_cpu(i);
        if (!cpu) continue;
        virt_uart_puts(" cpu="); virt_uart_put_hex(i);
        virt_uart_puts(" ipi-tx="); virt_uart_put_hex(cpu->ipi_sent);
        virt_uart_puts(" ipi-rx="); virt_uart_put_hex(cpu->ipi_received);
        virt_uart_puts(" rfence="); virt_uart_put_hex(cpu->rfence_requests);
    }
    virt_uart_putc('\n');
    virt_uart_puts("BoarOS: SMP natural exit pages/stack/table owners returned\n");
    sbi_shutdown();
}
