#include <arch/riscv/smp.h>
#include <arch/riscv/sbi.h>
#include <arch/riscv/sv39.h>
#include <arch/riscv/memory_layout.h>
#include <arch/riscv/direct_map.h>
#include <arch/riscv/virt_uart.h>
#include <kernel/physical_page.h>
#include <kernel/scheduler.h>
#include <kernel/task.h>
#include <arch/riscv/fpu.h>
#include <arch/riscv/timer.h>
#include <kernel/heap.h>
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
static struct physical_page_allocator arena;
static struct kernel_heap heap;
static struct kernel_mutex model;
static uint64_t arena_address, shared_page;
static uint8_t owners[UINT32_C(1) << 18];
static unsigned arena_entered;

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
    require(!arena.reclaimer && !arena.pressure_notify && !arena.pressure_wait, "arena-no-pressure");
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
    for (unsigned i = 0; i < sizeof(owners); i++) require(!owners[i], "arena-ledger-empty");
    require(!model.lock.writer && !model.lock.pending_head && !model.lock.readers &&
            !model.lock.waiters.registrations && !model.lock.waiters.borrows, "arena-lock-owner");
    require(physical_page_allocator_audit(&arena) == PHYSICAL_PAGE_STATUS_OK, "arena-audit");
    physical_page_release_order(allocator, arena_address, 10);
    virt_uart_puts("BoarOS: SMP private arena buddy/ref/slab/resize and audit passed\n");
}

static void compute(void *argument)
{
    unsigned id = (uintptr_t)argument;
    require(kernel_cpu_current()->logical_id == id / 2, "compute-affinity");
    require(__atomic_exchange_n(&executions[id], 1, __ATOMIC_ACQ_REL) == 0, "execution-unique");
    struct riscv_fpu_state expected = {.fcsr = 0x61, .saved = 1}, actual;
    for (unsigned i = 0; i < 32; i++) expected.regs[i] = UINT64_C(0x3ff0010000000000) + id * 256 + i;
    riscv_fpu_state_restore(&expected);
    asm volatile("csrs sstatus, %0" :: "r"((uintptr_t)0x6000) : "memory");
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
    for (unsigned i = 0; i < 32; i++) require(actual.regs[i] == expected.regs[i], "preempt-FD");
    require(__atomic_exchange_n(&executions[id], 0, __ATOMIC_ACQ_REL) == 1, "execution-release");
}

static void migrate(void *unused)
{
    (void)unused;
    uintptr_t sp = arch_current_stack();
    require(kernel_cpu_current()->logical_id == 1, "migration-source");
    struct riscv_fpu_state expected = {.fcsr = 0x83, .saved = 1}, actual;
    for (unsigned i = 0; i < 32; i++) expected.regs[i] = UINT64_C(0x4000010000000000) + i;
    riscv_fpu_state_restore(&expected);
    asm volatile("csrs sstatus, %0" :: "r"((uintptr_t)0x6000) : "memory");
    struct kernel_wait_token token;
    enum kernel_wait_wake_reason reason;
    require(kernel_wait_prepare(&migrate_queue, 0, 0, &token) == KERNEL_SCHEDULER_STATUS_OK,
            "migration-prepare");
    require(kernel_wait_park(&token, &reason) == KERNEL_SCHEDULER_STATUS_OK, "migration-park");
    require(kernel_wait_finish(&token) == KERNEL_SCHEDULER_STATUS_OK, "migration-finish");
    require(kernel_cpu_current()->logical_id == 0 && arch_current_stack() == sp,
            "migration-destination-SP");
    riscv_fpu_state_save(&actual);
    require(actual.fcsr == expected.fcsr, "migration-FCSR");
    for (unsigned i = 0; i < 32; i++) require(actual.regs[i] == expected.regs[i], "migration-FD");
    __atomic_store_n(&migrated, 1, __ATOMIC_RELEASE);
}

static void parallel_tasks(void)
{
    if (!riscv_smp_cpu(1)) return;
    for (unsigned i = 0; i < 4; i++)
        require(kernel_thread_create_joinable_mask(compute, (void *)(uintptr_t)i, &joins[i],
                1U << (i / 2)) == KERNEL_SCHEDULER_STATUS_OK, "compute-create");
    for (unsigned i = 0; i < 4; i++) kernel_thread_join(&joins[i]);
    for (unsigned i = 0; i < 4; i++) require(counters[i] >= 20000 && !executions[i], "compute-progress");
    kernel_wait_queue_init(&migrate_queue);
    require(kernel_thread_create_joinable_mask(migrate, 0, &migration, 2) ==
            KERNEL_SCHEDULER_STATUS_OK, "migration-create");
    uint64_t end = riscv_time_read() + UINT64_C(50000000);
    enum kernel_scheduler_status status;
    do {
        status = kernel_thread_set_cpu_mask(&migration, 1);
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
    virt_uart_puts("BoarOS: SMP timer preemption, unique execution and FD migration passed\n");
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
    *(volatile uint64_t *)(uintptr_t)va = 0x12345678;
    *(volatile uint64_t *)(uintptr_t)(va + 4096) = 0x87654321;
    void *physical;
    require(physical_page_resolve(allocator, page, &physical) == PHYSICAL_PAGE_STATUS_OK &&
            *(uint64_t *)physical == 0x12345678, "batch-alias");
    require(riscv_sv39_kernel_window_unmap_pages(allocator, va, 2) == RISCV_SV39_STATUS_OK,
            "batch-unmap");
    physical_page_release_order(allocator, page, 1);
    parallel_tasks();
    parallel_allocator();
    riscv_smp_stop();
    __atomic_store_n(&done, 1, __ATOMIC_RELEASE);
}

void riscv_smp_test_start(struct physical_page_allocator *pages)
{
    allocator = pages;
    baseline = physical_page_available(pages);
    require(kernel_thread_create(orchestrate, 0) == KERNEL_SCHEDULER_STATUS_OK, "test-create");
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
    virt_uart_puts("BoarOS: SMP natural exit pages/stack/table owners returned\n");
    sbi_shutdown();
}
