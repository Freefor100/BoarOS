#include <arch/task.h>
#include <arch/context.h>
#include <kernel/files.h>
#include <kernel/errno.h>
#include <kernel/irq.h>
#include <kernel/exec.h>
#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/pid.h>
#include <kernel/scheduler.h>
#include <kernel/task.h>
#include <kernel/tick.h>
#include <kernel/time.h>
#include <kernel/uaccess.h>
#include <kernel/vma.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../exec_internal.h"
#include "private.h"

struct kernel_scheduler scheduler;
static struct kernel_cpu boot_cpu;
static struct kernel_task bootstrap_task;
static uintptr_t current_sp(void);
#if BOAROS_MAX_CPUS > 1
static struct kernel_task secondary_idle;
void kernel_assert_service_context(void)
{
    struct kernel_cpu *cpu = kernel_cpu_current();
    if (cpu->logical_id || (cpu->current && cpu->current->controlled)) __builtin_trap();
}

struct kernel_task *kernel_cpu_secondary_prepare(struct kernel_cpu *cpu,
    uint64_t hart, unsigned logical)
{
    if (kernel_cpu_current()->logical_id || logical != 1 || cpu->initialized)
        __builtin_trap();
    kernel_cpu_initialize(cpu, hart, &secondary_idle.io_context);
    cpu->logical_id = logical;
    secondary_idle.cpu = cpu;
    return &secondary_idle;
}

void kernel_cpu_secondary_attach(struct kernel_cpu *cpu, uintptr_t low, uintptr_t high)
{
    if (arch_interrupt_is_enabled() || !cpu || cpu->logical_id != 1 || cpu->current ||
        current_sp() < low || current_sp() >= high) __builtin_trap();
    struct kernel_task *idle = &secondary_idle;
    idle->arch.kernel_sp = high;
    arch_thread_set_mm(&idle->arch, scheduler.kernel_context);
    idle->magic = KERNEL_THREAD_MAGIC;
    idle->physical_address = idle->stack_physical_address = KERNEL_THREAD_NO_PAGE;
    idle->stack_low = low;
    idle->stack_high = high;
    idle->state = KERNEL_THREAD_STATE_IDLE;
    idle->idle = 1;
    idle->completion.kind = KERNEL_THREAD_KIND_KERNEL;
    kernel_wait_record_init(&idle->wait);
    idle->wait.on_cpu = 1;
    cpu->current = idle;
    arch_current_thread_set(idle);
    scheduler_cpu_initialize(cpu, idle, kernel_time_monotonic_ns());
}
#endif

_Static_assert(offsetof(struct kernel_task, cpu) == ARCH_TASK_CPU_OFFSET,
               "current CPU lookup must preserve the architecture prefix");
void kernel_cpu_boot_initialize(uint64_t hardware_id)
{
    kernel_cpu_initialize(&boot_cpu, hardware_id, &bootstrap_task.io_context);
    bootstrap_task.cpu = &boot_cpu;
    arch_current_thread_set(&bootstrap_task);
    kernel_wait_domain_init();
}
void kernel_cpu_boot_rebind(void)
{
    /* RV重定位后重新取高地址；任何短锁都不能跨地址切换持有。 */
    if (boot_cpu.raw_locks || boot_cpu.preempt_depth || boot_cpu.current) __builtin_trap();
    boot_cpu.bootstrap_io = &bootstrap_task.io_context;
    bootstrap_task.cpu = &boot_cpu;
    arch_current_thread_set(&bootstrap_task);
}
static int identity_heap_address(const void *pointer, uint64_t *address)
{
    return arch_direct_map_va_to_pa((uint64_t)(uintptr_t)pointer, 1U, address)
        == ARCH_DIRECT_MAP_STATUS_OK;
}



_Static_assert(sizeof(struct kernel_task) <= BOAROS_PAGE_SIZE,
               "kernel thread metadata exceeds its owned page");
_Static_assert(KERNEL_STACK_ORDER <= PHYSICAL_PAGE_MAX_ORDER &&
                   BOAROS_PAGE_SHIFT + KERNEL_STACK_ORDER < 64U,
               "kernel stack order must fit the physical allocator");
_Static_assert(KERNEL_STACK_BYTES == (BOAROS_PAGE_SIZE << KERNEL_STACK_ORDER),
               "kernel stack size must match its contiguous-page owner");
_Static_assert(KERNEL_STACK_BYTES >= KERNEL_STACK_GUARD_BYTES +
                   ARCH_TRAP_FRAME_SIZE + KERNEL_STACK_MINIMUM_RESERVE,
               "kernel stack needs room for a trap frame and reserve");
_Static_assert(offsetof(struct kernel_task, arch) == 0U,
               "architecture state must prefix the scheduler task");

static uintptr_t current_sp(void)
{
    return arch_current_stack();
}

static enum kernel_scheduler_status validate_queue_shape(
    const struct kernel_task *head,
    const struct kernel_task *tail)
{
    COST_ADD(QUEUE_SHAPE_CHECKS, 1);
    if ((head == 0) != (tail == 0)) {
        return KERNEL_SCHEDULER_STATUS_QUEUE_CORRUPT;
    }
    if (tail != 0 && tail->next != 0) {
        return KERNEL_SCHEDULER_STATUS_QUEUE_CORRUPT;
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}

static enum kernel_scheduler_status validate_thread(
    const struct kernel_task *thread,
    enum kernel_thread_state expected_state)
{
    COST_ADD(QUEUE_THREAD_CHECKS, 1);
    uintptr_t base;
    uintptr_t expected_low;
    const uint64_t *canary;
    int resources_absent;

    if (thread == 0 || thread->magic != KERNEL_THREAD_MAGIC ||
        thread->state != (uint32_t)expected_state) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    if (thread->idle != 0U) {
        if (thread != kernel_cpu_current()->idle ||
            expected_state != KERNEL_THREAD_STATE_IDLE ||
            thread->physical_address != KERNEL_THREAD_NO_PAGE ||
            thread->stack_low >= thread->stack_high ||
            thread->arch.kernel_sp != thread->stack_high ||
            thread->arch.user_sp != 0U ||
            thread->arch.user_mode != 0U ||
            arch_thread_mm(&thread->arch) != scheduler.kernel_context ||
            thread->completion.kind != KERNEL_THREAD_KIND_KERNEL ||
            thread->completion.tid != 0 ||
            thread->completion.tgid != 0 ||
            thread->tid != 0 || thread->tid_owned != 0U ||
            thread->group_leader != 0 || thread->group_members != 0U ||
            thread->files.state != KERNEL_FILES_EMPTY ||
            thread->fs.state != KERNEL_FS_CONTEXT_EMPTY ||
            thread->mm.state != KERNEL_MM_EMPTY ||
            thread->exec_transaction != 0) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
        return KERNEL_SCHEDULER_STATUS_OK;
    }

    base = (uintptr_t)thread;
    if ((base & BOAROS_PAGE_MASK) != 0U ||
        base > UINTPTR_MAX - BOAROS_PAGE_SIZE ||
        (thread->physical_address & BOAROS_PAGE_MASK) != 0U) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    expected_low = thread->stack_high - KERNEL_STACK_BYTES +
                   KERNEL_STACK_GUARD_BYTES;
    if (thread->stack_low != expected_low ||
        thread->stack_physical_address == KERNEL_THREAD_NO_PAGE ||
        (thread->stack_physical_address & (KERNEL_STACK_BYTES - 1U)) != 0U ||
        thread->stack_physical_address == thread->physical_address ||
        thread->stack_high < KERNEL_STACK_BYTES ||
        (thread->stack_high & BOAROS_PAGE_MASK) != 0U ||
        thread->context.tp != base ||
        thread->arch.kernel_sp != thread->stack_high ||
        thread->context.sp < thread->stack_low ||
        thread->context.sp > thread->stack_high ||
        (thread->context.sp & (uintptr_t)15U) != 0U) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }

    if (thread->arch.user_mode == 0U) {
        int bound_mm = 0;
#if BOAROS_MAX_CPUS > 1
        bound_mm = thread->bound_mm_context != 0;
        if (bound_mm && (!thread->controlled || thread->mm.state != KERNEL_MM_LIVE ||
            arch_thread_mm(&thread->arch) != thread->bound_mm_context))
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
#endif
        if (thread->arch.user_sp != 0U ||
            (!bound_mm && arch_thread_mm(&thread->arch) != scheduler.kernel_context) ||
            thread->completion.kind != KERNEL_THREAD_KIND_KERNEL ||
            thread->completion.tid != 0 ||
            thread->completion.tgid != 0 ||
            thread->tid != 0 || thread->tid_owned != 0U ||
            thread->group_leader != 0 || thread->group_members != 0U ||
            (thread->files.state != KERNEL_FILES_EMPTY &&
             !(bound_mm && thread->files.state == KERNEL_FILES_LIVE)) ||
            thread->fs.state != KERNEL_FS_CONTEXT_EMPTY ||
            (!bound_mm && thread->mm.state != KERNEL_MM_EMPTY) ||
            thread->exec_transaction != 0) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
    } else {
#if BOAROS_USER_SMP
        /* 远端只核对已发布的存储及稳定根；业务资源/组状态由CPU0校验。
         * execution owner阻止exec/退出在此任务仍运行时替换其MM/栈。 */
        if (kernel_cpu_current()->logical_id) {
            if (thread->arch.user_mode != 1 || thread->mm.state != KERNEL_MM_LIVE ||
                thread->completion.kind != KERNEL_THREAD_KIND_USER ||
                thread->mm.allocator != scheduler.allocator) return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
            canary = (const uint64_t *)(thread->stack_low - sizeof(*canary));
            return *canary == KERNEL_STACK_CANARY ? KERNEL_SCHEDULER_STATUS_OK :
                KERNEL_SCHEDULER_STATUS_STACK_CORRUPT;
        }
#endif
        resources_absent =
            thread->files.state == KERNEL_FILES_EMPTY &&
            thread->fs.state == KERNEL_FS_CONTEXT_EMPTY;
        if (thread->arch.user_mode != 1U ||
            thread->completion.kind != KERNEL_THREAD_KIND_USER ||
            thread->tid_owned > 1U ||
            (thread->mm.state != KERNEL_MM_LIVE &&
             (expected_state != KERNEL_THREAD_STATE_EXITED ||
              (thread->mm.state !=
                   KERNEL_MM_CLEANUP &&
               thread->mm.state !=
                   KERNEL_MM_RELEASED)))) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
        if ((thread->files.state == KERNEL_FILES_EMPTY) !=
            (thread->fs.state == KERNEL_FS_CONTEXT_EMPTY)) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
        if (!resources_absent) {
            if (expected_state != KERNEL_THREAD_STATE_EXITED) {
                if (thread->files.state != KERNEL_FILES_LIVE ||
                    thread->files.heap == 0 ||
                    thread->files.record == 0 ||
                    thread->fs.state != KERNEL_FS_CONTEXT_LIVE ||
                    thread->fs.heap == 0 || thread->fs.record == 0 ||
                    thread->files.heap != thread->fs.heap) {
                    return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
                }
            } else {
                if ((thread->files.state != KERNEL_FILES_LIVE &&
                     thread->files.state != KERNEL_FILES_CLEANUP &&
                     thread->files.state != KERNEL_FILES_RELEASED) ||
                    (thread->fs.state != KERNEL_FS_CONTEXT_LIVE &&
                     thread->fs.state != KERNEL_FS_CONTEXT_CLEANUP &&
                     thread->fs.state != KERNEL_FS_CONTEXT_RELEASED)) {
                    return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
                }
                if ((thread->files.state != KERNEL_FILES_RELEASED &&
                     (thread->fs.state != KERNEL_FS_CONTEXT_LIVE ||
                      thread->mm.state != KERNEL_MM_LIVE)) ||
                    (thread->fs.state != KERNEL_FS_CONTEXT_RELEASED &&
                     thread->mm.state != KERNEL_MM_LIVE)) {
                    return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
                }
            }
        }
        if (!kernel_exec_transaction_valid(thread->exec_transaction,
                                           resources_absent
                                               ? 0
                                               : thread->files.heap)) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
        if (resources_absent && thread->exec_transaction != 0) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
        if (thread->tid_owned != 0U) {
            if (thread->tid <= 0 ||
                thread->tid != process_identity_number(thread, KERNEL_PID_TID) ||
                thread->group_leader == 0 ||
                thread->group_leader->group_leader != thread->group_leader ||
                thread->group_leader->group_members == 0U) {
                return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
            }
        } else if (expected_state != KERNEL_THREAD_STATE_EXITED ||
                   thread->tid != 0 || thread->group_leader != 0 ||
                   thread->group_members != 0U) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
        if (thread->mm.state == KERNEL_MM_LIVE) {
            if (!thread->mm.record || thread->mm.allocator != scheduler.allocator) {
                return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
            }
        } else if (thread->mm.state ==
                   KERNEL_MM_CLEANUP) {
            if (!thread->mm.record || thread->mm.allocator != scheduler.allocator ||
                (thread->mm.record_page_address &
                 BOAROS_PAGE_MASK) != 0U) {
                return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
            }
        } else if (thread->mm.allocator != 0 ||
                   thread->mm.record_page_address != 0U || thread->mm.record != 0) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
    }

    canary = (const uint64_t *)(thread->stack_low - sizeof(*canary));
    if (*canary != KERNEL_STACK_CANARY) {
        return KERNEL_SCHEDULER_STATUS_STACK_CORRUPT;
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}

enum kernel_scheduler_status validate_current_on_cpu(struct kernel_cpu *cpu)
{
    /* IRQ关闭期间固定CPU/current借用；不跨可睡眠服务或架构切换。 */
    struct kernel_task *current = cpu->current;
    enum kernel_thread_state expected_state;
    enum kernel_scheduler_status status;
    uintptr_t stack_pointer;

    if (current == 0 ||
        arch_current_thread_get() != current) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    expected_state = current->idle != 0U
                         ? KERNEL_THREAD_STATE_IDLE
                         : KERNEL_THREAD_STATE_RUNNING;
    status = validate_thread(current, expected_state);
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }

    stack_pointer = current_sp();
    if (stack_pointer < current->stack_low ||
        stack_pointer >= current->stack_high) {
        return KERNEL_SCHEDULER_STATUS_STACK_CORRUPT;
    }
    if (arch_mmu_current_context() != arch_thread_mm(&current->arch)) {
        return KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE;
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}

enum kernel_scheduler_status validate_current(void)
{
    return validate_current_on_cpu(kernel_cpu_current());
}

enum kernel_scheduler_status activate_thread_address_space(
    const struct kernel_task *thread)
{
    if (thread == 0) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    if (kernel_mm_activate(thread->mm.state == KERNEL_MM_LIVE ? &thread->mm : 0,
        scheduler.kernel_context) != KERNEL_MM_STATUS_OK) {
        return KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE;
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}

enum kernel_scheduler_status validate_queues_locked(void)
    __attribute__((noinline, noclone));

enum kernel_scheduler_status validate_queues_locked(void)
{
    struct kernel_cpu *cpu = kernel_cpu_current();
    if (!cpu->raw_locks || cpu->raw_locks->lock != &kernel_wait_domain) __builtin_trap();
    COST_ADD(QUEUE_VALIDATIONS, 1);
    enum kernel_scheduler_status status;


    if (!!scheduler.runqueue.first != !!scheduler.runqueue.last ||
        (scheduler.runqueue.first && scheduler.runqueue.first->previous) ||
        (scheduler.runqueue.last && scheduler.runqueue.last->next))
        return KERNEL_SCHEDULER_STATUS_QUEUE_CORRUPT;
    status = validate_queue_shape(scheduler.exited_head,
                                  scheduler.exited_tail);
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }
    if (ready_first() != 0) {
        status = validate_thread(ready_first(),
                                 KERNEL_THREAD_STATE_READY);
        if (status != KERNEL_SCHEDULER_STATUS_OK) {
            return status;
        }
        status = validate_thread((struct kernel_task *)scheduler.runqueue.last->owner,
                                 KERNEL_THREAD_STATE_READY);
        if (status != KERNEL_SCHEDULER_STATUS_OK) {
            return status;
        }
    }
    if (scheduler.exited_head != 0) {
        if (scheduler.exited_head->magic != KERNEL_THREAD_MAGIC ||
            scheduler.exited_tail->magic != KERNEL_THREAD_MAGIC ||
            scheduler.exited_head->state != KERNEL_THREAD_STATE_EXITED ||
            scheduler.exited_tail->state != KERNEL_THREAD_STATE_EXITED ||
            scheduler.exited_head->idle != 0U ||
            scheduler.exited_tail->idle != 0U ||
            (scheduler.exited_head->physical_address &
             BOAROS_PAGE_MASK) != 0U ||
            (scheduler.exited_tail->physical_address &
             BOAROS_PAGE_MASK) != 0U ||
            scheduler.exited_head->publish_completion > 1U ||
            scheduler.exited_tail->publish_completion > 1U) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
    }
    status = validate_queue_shape(scheduler.blocked_head,
                                  scheduler.blocked_tail);
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }
    if (scheduler.blocked_head != 0) {
        if (scheduler.blocked_head->magic != KERNEL_THREAD_MAGIC ||
            scheduler.blocked_tail->magic != KERNEL_THREAD_MAGIC ||
            scheduler.blocked_head->state != KERNEL_THREAD_STATE_BLOCKED ||
            scheduler.blocked_tail->state != KERNEL_THREAD_STATE_BLOCKED ||
            scheduler.blocked_head->idle != 0U ||
            scheduler.blocked_tail->idle != 0U) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
    }
    status = validate_queue_shape(scheduler.stopped_head,
                                  scheduler.stopped_tail);
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }
    if (scheduler.stopped_head != 0) {
        if (scheduler.stopped_head->magic != KERNEL_THREAD_MAGIC ||
            scheduler.stopped_tail->magic != KERNEL_THREAD_MAGIC ||
            scheduler.stopped_head->state !=
                KERNEL_THREAD_STATE_STOPPED ||
            scheduler.stopped_tail->state !=
                KERNEL_THREAD_STATE_STOPPED ||
            scheduler.stopped_head->idle != 0U ||
            scheduler.stopped_tail->idle != 0U) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}

enum kernel_scheduler_status validate_queues(void)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    return validate_queues_locked();
}

void kernel_scheduler_switch_finish(void)
{
    struct kernel_cpu *cpu = kernel_cpu_current();
    if (!cpu->switch_previous) return;
    struct kernel_wait_cursor joined;
    int notify = 0;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        struct kernel_task *previous = cpu->switch_previous;
        cpu->switch_previous = 0;
        if (previous == cpu->current || !cpu->current->wait.on_cpu) __builtin_trap();
        kernel_wait_switch_finish_locked(previous);
        if (previous->state == KERNEL_THREAD_STATE_EXITED && previous->join)
            notify = kernel_wait_cursor_prepare_locked(&previous->join->waiters, &joined, 1);
    }
    if (notify) kernel_wait_cursor_wake(&joined, 1);
}

enum kernel_scheduler_status scheduler_switch_current_away(struct kernel_task *previous)
{
    kernel_assert_can_block();
    struct kernel_cpu *cpu = kernel_cpu_current();
    struct kernel_task *next;
    uint64_t deadline;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        scheduler_account_runtime_locked();
        next = scheduler_select_next_locked(cpu, previous, 1);
        deadline = scheduler_deadline_locked(cpu);
    }
    if (!next) return KERNEL_SCHEDULER_STATUS_OK;
    scheduler_arm_timer(deadline);
    return scheduler_switch_claimed(previous, next);
}

enum kernel_scheduler_status scheduler_switch_claimed(
    struct kernel_task *previous, struct kernel_task *next)
{
    kernel_assert_can_block();
    enum kernel_scheduler_status status = activate_thread_address_space(next);
    if (status != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
#if BOAROS_COST_DIAGNOSTICS
    kernel_cost_switch(&previous->cost, &next->cost);
#endif
    arch_fpu_switch(&previous->fpu, &next->fpu);
    /* 新栈完成handoff；raw已释放，IRQ仍关闭，旧任务尚不能再次运行/回收。 */
    scheduler_cpu_time_switch(previous, next);
    arch_context_switch(&previous->context, &next->context);
    kernel_scheduler_switch_finish();
    return KERNEL_SCHEDULER_STATUS_OK;
}

static void fill_owned_storage(void *pointer, size_t bytes, uint64_t word)
{
    /* 仅初始化已独占的对齐页/栈，不用于用户复制或MMIO。
     * may_alias覆盖对象表示，volatile保留完整写入；发布仍在初始化之后。 */
    typedef uint64_t storage_word __attribute__((may_alias));
    if ((uintptr_t)pointer % sizeof(storage_word) || bytes % sizeof(storage_word)) __builtin_trap();
    volatile storage_word *output = pointer;
    for (size_t i = 0; i < bytes / sizeof(storage_word); i++) output[i] = word;
}

void clear_page(void *pointer)
{
    fill_owned_storage(pointer, BOAROS_PAGE_SIZE, 0);
}

enum kernel_scheduler_status release_after_create_failure(
    uint64_t physical_address,
    enum kernel_scheduler_status original_status)
{
    (void)physical_page_release(scheduler.allocator, physical_address);
    return original_status;
}

/*
 * 内核栈窗口槽位：未映射 guard 页后接 KERNEL_STACK_BYTES。窗口骨架在
 * 页表构建期预留，只有生产内核激活它；无页表的 fixture 回退直接映射。
 */
_Static_assert(KERNEL_STACK_BYTES + BOAROS_PAGE_SIZE ==
                   ARCH_KERNEL_STACK_SLOT_SIZE,
               "kernel stack window slot is one guard page plus the stack");
#define KERNEL_STACK_SLOT_COUNT \
    ((uint32_t)(ARCH_KERNEL_STACK_WINDOW_SIZE / ARCH_KERNEL_STACK_SLOT_SIZE))
#define KERNEL_STACK_SLOT_WORDS ((KERNEL_STACK_SLOT_COUNT + 63U) / 64U)
#define KERNEL_STACK_ALIAS_PAGES (BOAROS_PAGE_SIZE / sizeof(uint64_t))
_Static_assert(KERNEL_STACK_ALIAS_PAGES > 1 &&
                   (KERNEL_STACK_ALIAS_PAGES + 1) % 3 == 0 &&
                   ARCH_KERNEL_STACK_SLOT_SIZE == 3 * BOAROS_PAGE_SIZE &&
                   (ARCH_KERNEL_STACK_WINDOW_BASE / BOAROS_PAGE_SIZE) % KERNEL_STACK_ALIAS_PAGES == 0,
               "stack aliases require an aligned three-page slot geometry");
static uint64_t kernel_stack_slot_bitmap[KERNEL_STACK_SLOT_WORDS];

static int kernel_stack_slot_acquire(uint64_t stack_address, uint64_t *window_address)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    /* 线性槽号曾让热栈与ext4对象反复争用QEMU soft-TLB同一槽；
     * 按物理栈页号分散别名。3的模逆元为(N+1)/3，无同色空槽仍用全容量。 */
    uint32_t selected = (uint32_t)(((stack_address / BOAROS_PAGE_SIZE - 1) *
        ((KERNEL_STACK_ALIAS_PAGES + 1) / 3)) & (KERNEL_STACK_ALIAS_PAGES - 1));
    for (; selected < KERNEL_STACK_SLOT_COUNT; selected += KERNEL_STACK_ALIAS_PAGES) {
        if (!(kernel_stack_slot_bitmap[selected / 64U] & (UINT64_C(1) << (selected % 64U))))
            goto found;
    }
    for (uint32_t word = 0U; word < KERNEL_STACK_SLOT_WORDS; word++) {
        uint64_t free_bits = ~kernel_stack_slot_bitmap[word];
        uint32_t bit = 0U;

        if (free_bits == 0U) continue;
        while ((free_bits & (UINT64_C(1) << bit)) == 0U) bit++;
        selected = word * 64U + bit;
        if (selected >= KERNEL_STACK_SLOT_COUNT) return -1;
        goto found;
    }
    return -1;
found:
    kernel_stack_slot_bitmap[selected / 64U] |= UINT64_C(1) << (selected % 64U);
    *window_address = ARCH_KERNEL_STACK_WINDOW_BASE + (uint64_t)selected * ARCH_KERNEL_STACK_SLOT_SIZE;
    return 0;
}

static void kernel_stack_slot_release(uint64_t window_address)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    uint32_t index = (uint32_t)((window_address -
                                 ARCH_KERNEL_STACK_WINDOW_BASE) /
                                ARCH_KERNEL_STACK_SLOT_SIZE);
    uint64_t mask = UINT64_C(1) << (index % 64U);
    if (!(kernel_stack_slot_bitmap[index / 64U] & mask)) __builtin_trap();
    kernel_stack_slot_bitmap[index / 64U] &= ~mask;
}

static void kernel_stack_slots_reset(void)
{
    for (uint32_t word = 0U; word < KERNEL_STACK_SLOT_WORDS; word++)
        kernel_stack_slot_bitmap[word] = 0U;
}

/* Allocate the metadata and execution stack before publishing any owner. */
enum kernel_scheduler_status allocate_task_storage(struct kernel_task **task)
{
    kernel_assert_service_context();
    struct kernel_task *thread;
    uint64_t metadata_address;
    uint64_t stack_address;
    void *metadata;
    void *stack;
    enum physical_page_status status;

    status = physical_page_allocate(scheduler.allocator, &metadata_address);
    if (status != PHYSICAL_PAGE_STATUS_OK)
        return status == PHYSICAL_PAGE_STATUS_EMPTY
                   ? KERNEL_SCHEDULER_STATUS_NO_MEMORY
                   : KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    status = physical_page_resolve(scheduler.allocator, metadata_address,
                                   &metadata);
    if (status != PHYSICAL_PAGE_STATUS_OK)
        return release_after_create_failure(metadata_address,
                                             KERNEL_SCHEDULER_STATUS_PAGE_ACCESS);
    clear_page(metadata);
    thread = metadata;
    thread->cpu = kernel_cpu_current();
    kernel_wait_record_init(&thread->wait);
    kernel_wait_node_init(&thread->default_wait_node, thread);
    thread->physical_address = metadata_address;
    thread->stack_physical_address = KERNEL_THREAD_NO_PAGE;
    status = physical_page_allocate_order(scheduler.allocator,
                                          KERNEL_STACK_ORDER, &stack_address);
    if (status != PHYSICAL_PAGE_STATUS_OK)
        return release_after_create_failure(metadata_address,
            status == PHYSICAL_PAGE_STATUS_EMPTY
                ? KERNEL_SCHEDULER_STATUS_NO_MEMORY
                : KERNEL_SCHEDULER_STATUS_INVALID_STATE);
    status = physical_page_resolve(scheduler.allocator, stack_address, &stack);
    if (status != PHYSICAL_PAGE_STATUS_OK) {
        (void)physical_page_release_order(scheduler.allocator, stack_address,
                                          KERNEL_STACK_ORDER);
        return release_after_create_failure(metadata_address,
                                             KERNEL_SCHEDULER_STATUS_PAGE_ACCESS);
    }
    fill_owned_storage(stack, KERNEL_STACK_BYTES,
                       UINT64_C(0x0101010101010101) * KERNEL_STACK_FILL);
    thread->stack_physical_address = stack_address;
    if (arch_mmu_kernel_window_active()) {
        uint64_t window;
        uint64_t stack_va;
        enum arch_mmu_status map_status;

        if (kernel_stack_slot_acquire(stack_address, &window) != 0) {
            (void)physical_page_release_order(scheduler.allocator,
                                              stack_address,
                                              KERNEL_STACK_ORDER);
            return release_after_create_failure(
                metadata_address, KERNEL_SCHEDULER_STATUS_NO_MEMORY);
        }
        stack_va = window + BOAROS_PAGE_SIZE;
#if !defined(BOAROS_ARCH_LOONGARCH)
        map_status = arch_mmu_kernel_window_map_pages(scheduler.allocator, stack_va,
                                                      stack_address, 2);
#else
        map_status = arch_mmu_kernel_window_map(scheduler.allocator,
                                                  stack_va, stack_address);
        if (map_status == ARCH_MMU_STATUS_OK) {
            map_status = arch_mmu_kernel_window_map(
                scheduler.allocator, stack_va + BOAROS_PAGE_SIZE,
                stack_address + BOAROS_PAGE_SIZE);
        }
#endif
        if (map_status != ARCH_MMU_STATUS_OK) {
#if defined(BOAROS_ARCH_LOONGARCH)
            (void)arch_mmu_kernel_window_unmap(scheduler.allocator,
                                                 stack_va);
            (void)arch_mmu_kernel_window_unmap(
                scheduler.allocator, stack_va + BOAROS_PAGE_SIZE);
#endif
            kernel_stack_slot_release(window);
            (void)physical_page_release_order(scheduler.allocator,
                                              stack_address,
                                              KERNEL_STACK_ORDER);
            return release_after_create_failure(
                metadata_address,
                map_status == ARCH_MMU_STATUS_NO_MEMORY
                    ? KERNEL_SCHEDULER_STATUS_NO_MEMORY
                    : KERNEL_SCHEDULER_STATUS_INVALID_STATE);
        }
        thread->stack_low = (uintptr_t)(stack_va + KERNEL_STACK_GUARD_BYTES);
        thread->stack_high = (uintptr_t)(stack_va + KERNEL_STACK_BYTES);
    } else {
        thread->stack_low = (uintptr_t)stack + KERNEL_STACK_GUARD_BYTES;
        thread->stack_high = (uintptr_t)stack + KERNEL_STACK_BYTES;
    }
    thread->arch.kernel_sp = thread->stack_high;
    *(uint64_t *)(thread->stack_low - sizeof(uint64_t)) = KERNEL_STACK_CANARY;
    *task = thread;
    return KERNEL_SCHEDULER_STATUS_OK;
}

/* This runs only on another trusted stack, including construction rollback.
 * EXITED can be visited repeatedly while a real I/O owner is still retained,
 * and a GROUP_DEAD leader can later re-enter the exited queue. */
enum kernel_scheduler_status release_task_stack(struct kernel_task *thread)
{
    kernel_assert_service_context();
    if (thread && (thread->wait.on_cpu || thread->wait.borrows || thread->wait.nodes ||
        thread->wait.phase != KERNEL_WAIT_FINISHED)) __builtin_trap();
    if (thread && thread->io_context.user_pins) __builtin_trap();
#if BOAROS_MAX_CPUS > 1
    /* join、自动reaper及构造回滚共用可信清理点；不能只在join归还MM。 */
    if (thread && thread->bound_mm_context && thread->mm.state == KERNEL_MM_LIVE) {
        if (thread->files.state==KERNEL_FILES_LIVE &&
            kernel_files_release(&thread->files)!=KERNEL_FILES_STATUS_OK) __builtin_trap();
        if (kernel_mm_release(&thread->mm) != KERNEL_MM_STATUS_OK) __builtin_trap();
        thread->bound_mm_context = 0;
    }
#endif
    const unsigned char *cursor;
    uint64_t free_bytes;
    uint64_t used_bytes;

    if (thread == kernel_cpu_current()->current || thread->idle != 0U)
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    if (thread->stack_physical_address == KERNEL_THREAD_NO_PAGE)
        return thread->stack_low == 0U && thread->stack_high == 0U &&
                       thread->arch.kernel_sp == 0U && thread->context.sp == 0U
                   ? KERNEL_SCHEDULER_STATUS_OK
                   : KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    if ((thread->stack_physical_address & (KERNEL_STACK_BYTES - 1U)) != 0U ||
        thread->stack_physical_address == thread->physical_address ||
        thread->stack_high < KERNEL_STACK_BYTES ||
        (thread->stack_high & BOAROS_PAGE_MASK) != 0U ||
        thread->stack_low != thread->stack_high - KERNEL_STACK_BYTES +
                                 KERNEL_STACK_GUARD_BYTES ||
        *(const uint64_t *)(thread->stack_low - sizeof(uint64_t)) !=
            KERNEL_STACK_CANARY)
        return KERNEL_SCHEDULER_STATUS_STACK_CORRUPT;
    cursor = (const unsigned char *)thread->stack_low;
    /* 栈owner已停止执行；只读可用区，不跨canary/高边界。整字相同后
     * 才跳过八字节，差异字仍逐字节定位，保留精确的最小剩余空间。 */
    _Static_assert(KERNEL_STACK_GUARD_BYTES % sizeof(uint64_t) == 0,
                   "stack fill scan must remain naturally aligned");
    while (thread->stack_high - (uintptr_t)cursor >= sizeof(uint64_t)) {
        uint64_t word;
        __builtin_memcpy(&word, __builtin_assume_aligned(cursor, sizeof(word)), sizeof(word));
        if (word != UINT64_C(0x0101010101010101) * KERNEL_STACK_FILL) break;
        cursor += sizeof(word);
    }
    while ((uintptr_t)cursor < thread->stack_high &&
           *cursor == KERNEL_STACK_FILL)
        cursor++;
    free_bytes = (uintptr_t)cursor - thread->stack_low;
    used_bytes = thread->stack_high - (uintptr_t)cursor;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        scheduler.stack_statistics.stacks_released++;
        if (free_bytes < scheduler.stack_statistics.minimum_free_bytes)
            scheduler.stack_statistics.minimum_free_bytes = free_bytes;
        if (used_bytes > scheduler.stack_statistics.maximum_used_bytes)
            scheduler.stack_statistics.maximum_used_bytes = used_bytes;
    }
    if (arch_mmu_kernel_window_active()) {
        uint64_t window = (uint64_t)thread->stack_low -
                          KERNEL_STACK_GUARD_BYTES - BOAROS_PAGE_SIZE;
#if !defined(BOAROS_ARCH_LOONGARCH)
        if (arch_mmu_kernel_window_unmap_pages(scheduler.allocator,
                  window + BOAROS_PAGE_SIZE, 2) != ARCH_MMU_STATUS_OK) __builtin_trap();
#else
        (void)arch_mmu_kernel_window_unmap(scheduler.allocator,
                                             window + BOAROS_PAGE_SIZE);
        (void)arch_mmu_kernel_window_unmap(
            scheduler.allocator, window + BOAROS_PAGE_SIZE * 2U);
#endif
        kernel_stack_slot_release(window);
    }
    (void)physical_page_release_order(scheduler.allocator,
                                      thread->stack_physical_address,
                                      KERNEL_STACK_ORDER);
    thread->stack_physical_address = KERNEL_THREAD_NO_PAGE;
    thread->stack_low = 0U;
    thread->stack_high = 0U;
    thread->arch.kernel_sp = 0U;
    thread->context.sp = 0U;
    return KERNEL_SCHEDULER_STATUS_OK;
}

enum kernel_scheduler_status release_task_storage(
    struct kernel_task *thread, enum kernel_scheduler_status original_status)
{
    if (thread->wait.on_cpu || thread->wait.borrows || thread->wait.nodes ||
        thread->wait.phase != KERNEL_WAIT_FINISHED) __builtin_trap();
    if (thread->io_context.user_pins) __builtin_trap();
    enum kernel_scheduler_status status = release_task_stack(thread);
    if (status != KERNEL_SCHEDULER_STATUS_OK) return status;
    scheduler_forget_task(thread);
    kernel_task_release_io_scratch(thread);
    scheduler_task_retire(thread);
    scheduler_task_collect();
    return original_status;
}

void kernel_scheduler_stack_statistics(struct kernel_stack_statistics *statistics)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    if (statistics) *statistics = scheduler.stack_statistics;
}

enum kernel_scheduler_status kernel_scheduler_init(
    struct physical_page_allocator *allocator,
    uintptr_t idle_stack_low,
    uintptr_t idle_stack_high)
{
    uintptr_t stack_pointer = current_sp();
    uint64_t kernel_context = arch_mmu_current_context();
    enum kernel_pid_status pid_status;

    if (scheduler.initialized == KERNEL_SCHEDULER_INITIALIZED) {
        return KERNEL_SCHEDULER_STATUS_ALREADY_INITIALIZED;
    }
    if (allocator == 0 || idle_stack_low == 0U ||
        idle_stack_low >= idle_stack_high ||
        (idle_stack_low & (uintptr_t)15U) != 0U ||
        (idle_stack_high & (uintptr_t)15U) != 0U ||
        stack_pointer < idle_stack_low || stack_pointer >= idle_stack_high ||
        !physical_page_allocator_is_finalized(allocator) ||
        physical_page_total(allocator) == 0U ||
        physical_page_available(allocator) > physical_page_total(allocator)) {
        return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    }
    if (arch_interrupt_is_enabled()) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    if (arch_mmu_switch_context(kernel_context) != ARCH_MMU_STATUS_OK) {
        return KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE;
    }
    pid_status = kernel_pid_allocator_init(&scheduler.pid_allocator,
                                           scheduler.pid_bitmap,
                                           KERNEL_PID_LIMIT);
    if (pid_status != KERNEL_PID_STATUS_OK) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }

    scheduler.all_tasks = 0;
    scheduler.load_ticks = 0;
    for (unsigned i = 0; i < 3; i++) scheduler.loads[i] = 0;
    scheduler.allocator = allocator;
    scheduler.identities.numbers = &scheduler.pid_allocator;
    scheduler.identities.next_generation = 1U;
    if (kernel_heap_init(&scheduler.identity_heap, allocator, identity_heap_address)
            != KERNEL_HEAP_STATUS_OK) return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    scheduler.kernel_context = kernel_context;
    scheduler.boot_idle.arch.kernel_sp = idle_stack_high;
    scheduler.boot_idle.cpu = kernel_cpu_current();
    scheduler.boot_idle.arch.user_sp = 0U;
    scheduler.boot_idle.arch.user_mode = 0U;
    arch_thread_set_mm(&scheduler.boot_idle.arch, scheduler.kernel_context);
    scheduler.boot_idle.magic = KERNEL_THREAD_MAGIC;
    scheduler.boot_idle.physical_address = KERNEL_THREAD_NO_PAGE;
    scheduler.boot_idle.stack_physical_address = KERNEL_THREAD_NO_PAGE;
    scheduler.boot_idle.stack_low = idle_stack_low;
    scheduler.boot_idle.stack_high = idle_stack_high;
    scheduler.boot_idle.next = 0;
    scheduler.boot_idle.parent = 0;
    scheduler.boot_idle.first_child = 0;
    scheduler.boot_idle.last_child = 0;
    scheduler.boot_idle.previous_sibling = 0;
    scheduler.boot_idle.next_sibling = 0;
    kernel_wait_record_init(&scheduler.boot_idle.wait);
    scheduler.boot_idle.wait.on_cpu = 1;
    scheduler.boot_idle.state = KERNEL_THREAD_STATE_IDLE;
    scheduler.boot_idle.idle = 1U;
    scheduler.boot_idle.tid = 0;
    scheduler.boot_idle.tid_owned = 0U;
    scheduler.boot_idle.publish_completion = 0U;
    scheduler.boot_idle.wait_status = 0U;
    scheduler.boot_idle.group_leader = 0;
    scheduler.boot_idle.group_members = 0U;
    scheduler.boot_idle.completion.kind = KERNEL_THREAD_KIND_KERNEL;
    scheduler.boot_idle.completion.reason = KERNEL_THREAD_EXIT_RETURNED;
    scheduler.boot_idle.completion.tid = 0;
    scheduler.boot_idle.completion.tgid = 0;
    scheduler.boot_idle.completion.status = 0U;
    scheduler.boot_idle.completion.detail = 0U;
#if BOAROS_COST_DIAGNOSTICS
    scheduler.boot_idle.cost.wait_flags = 128;
#endif
    kernel_cpu_current()->current = &scheduler.boot_idle;
    scheduler.cleanup_task = 0;
    kernel_wait_queue_init(&scheduler.cleanup_queue);
    memset(&scheduler.runqueue, 0, sizeof(scheduler.runqueue));
    uint64_t now = kernel_time_monotonic_ns();
    struct kernel_rt_bandwidth initial_budget;
    kernel_rt_bandwidth_init(&initial_budget, now);
    scheduler.rt_configuration = (struct kernel_rt_configuration){
        .period_ns = initial_budget.period_ns, .runtime_ns = initial_budget.runtime_ns };
    scheduler_cpu_initialize(kernel_cpu_current(), &scheduler.boot_idle, now);
    (void)kernel_cpu_consume_schedule(kernel_cpu_current());
    scheduler.exited_head = 0;
    scheduler.exited_tail = 0;
    scheduler.blocked_head = 0;
    scheduler.blocked_tail = 0;
    scheduler.deadline_root = 0;
    kernel_stack_slots_reset();
    scheduler.stopped_head = 0;
    scheduler.stopped_tail = 0;
    scheduler.init_task = 0;
    scheduler.fatal_status = KERNEL_SCHEDULER_STATUS_OK;
    scheduler.stack_statistics.stacks_released = 0U;
    scheduler.stack_statistics.minimum_free_bytes =
        KERNEL_STACK_BYTES - KERNEL_STACK_GUARD_BYTES;
    scheduler.stack_statistics.maximum_used_bytes = 0U;
    kernel_cpu_current()->idle_context_saved = 0U;
    scheduler.initialized = KERNEL_SCHEDULER_INITIALIZED;
    arch_current_thread_set(kernel_cpu_current()->idle);
    return KERNEL_SCHEDULER_STATUS_OK;
}

enum kernel_scheduler_status kernel_thread_create(void (*entry)(void *), void *argument)
{ return kernel_thread_create_joinable(entry, argument, 0); }

#if BOAROS_MAX_CPUS > 1
static unsigned available_cpu_mask_locked(void)
{
    unsigned mask = 0;
    for (struct kernel_cpu *cpu = scheduler.cpus; cpu; cpu = cpu->scheduler_next)
        if (!cpu->logical_id || kernel_cpu_online(cpu)) mask |= 1U << cpu->logical_id;
    return mask;
}
#endif

static enum kernel_scheduler_status create_kernel_thread(
    void (*entry)(void *), void *argument, struct kernel_thread_join *join,
    uint32_t cpu_mask, struct kernel_mm *bound_mm, struct kernel_files *bound_files)
{
    struct kernel_task *thread;
    uintptr_t old_status;
    enum arch_context_status context_status;
    enum kernel_scheduler_status status;
    uint64_t bound_context = 0;
    if (bound_mm && (kernel_mm_check_task_domain(bound_mm) != KERNEL_MM_STATUS_OK ||
        kernel_mm_context(bound_mm, &bound_context) != KERNEL_MM_STATUS_OK))
        return KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE;

    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED) {
        return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (entry == 0 || (join && join->task)) {
        return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    }

    old_status = arch_interrupt_save();
    if (scheduler_failure() != KERNEL_SCHEDULER_STATUS_OK) {
        status = scheduler_failure();
        goto restore_interrupts;
    }
    status = validate_current();
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        goto restore_interrupts;
    }
    status = validate_queues();
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        goto restore_interrupts;
    }

    status = allocate_task_storage(&thread);
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        goto restore_interrupts;
    }
    thread->arch.user_sp = 0U;
#if BOAROS_MAX_CPUS > 1
    thread->cpu_mask = cpu_mask ? cpu_mask : 1;
    thread->controlled = cpu_mask != 0;
#else
    (void)cpu_mask;
#endif
    thread->arch.user_mode = 0U;
    arch_thread_set_mm(&thread->arch, bound_mm ? bound_context : scheduler.kernel_context);
    thread->magic = KERNEL_THREAD_MAGIC;
    thread->next = 0;
    thread->state = KERNEL_THREAD_STATE_READY;
    thread->idle = 0U;
    thread->tid = 0;
    thread->tid_owned = 0U;
    thread->publish_completion = join ? 0U : 1U;
    thread->wait_status = 0U;
    thread->group_leader = 0;
    thread->group_members = 0U;
    thread->completion.kind = KERNEL_THREAD_KIND_KERNEL;
    thread->completion.reason = KERNEL_THREAD_EXIT_RETURNED;
    thread->completion.tid = 0;
    thread->completion.tgid = 0;
    thread->completion.status = 0U;
    thread->completion.detail = 0U;
    thread->exec_transaction = 0;
    kernel_wait_queue_init(&thread->child_exit_queue);
    kernel_wait_queue_init(&thread->vfork_done_queue);
    context_status = arch_context_init(&thread->context,
                                        thread->stack_high,
                                        entry,
                                        argument,
                                        thread);
    if (context_status != ARCH_CONTEXT_STATUS_OK) {
        status = release_task_storage(
            thread,
            KERNEL_SCHEDULER_STATUS_INVALID_STATE);
        goto restore_interrupts;
    }

    if (join) kernel_wait_queue_init(&join->waiters);
    status = KERNEL_SCHEDULER_STATUS_OK;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
#if BOAROS_MAX_CPUS > 1
        /* 私有准备可能睡眠；只有最终接纳点的在线资格有效。停止者在CPU0
         * 禁抢占，成功接纳先取得controlled owner再发布runnable。 */
        if (thread->controlled && (thread->cpu_mask & ~available_cpu_mask_locked()))
            status = KERNEL_SCHEDULER_STATUS_INVALID_STATE;
#endif
        if (status == KERNEL_SCHEDULER_STATUS_OK) {
            /* 最终ONLINE复查后才转移MM owner；失败保留caller原句柄。 */
            if (bound_mm) {
                if (kernel_mm_move(&thread->mm, bound_mm) != KERNEL_MM_STATUS_OK) __builtin_trap();
#if BOAROS_MAX_CPUS > 1
                thread->bound_mm_context = bound_context;
#else
                __builtin_trap();
#endif
            }
            if (bound_files && kernel_files_move(&thread->files,bound_files)!=KERNEL_FILES_STATUS_OK)
                __builtin_trap();
            if (join) {
                if (join->task) __builtin_trap();
                join->task = thread;
                thread->join = join;
            }
#if BOAROS_MAX_CPUS > 1
            if (thread->controlled) {
                if (scheduler.controlled_owners == UINT32_MAX) __builtin_trap();
                thread->controlled = 2;
                scheduler.controlled_owners++;
            }
#endif
            ready_enqueue_locked(thread, 0);
        }
    }
    if (status != KERNEL_SCHEDULER_STATUS_OK)
        status = release_task_storage(thread, status);

restore_interrupts:
    arch_interrupt_restore(old_status);
    return status;
}

enum kernel_scheduler_status kernel_thread_create_joinable(
    void (*entry)(void *), void *argument, struct kernel_thread_join *join)
{ return create_kernel_thread(entry, argument, join, 0, 0, 0); }

#if BOAROS_MAX_CPUS > 1
int kernel_scheduler_core_quiescent(struct kernel_cpu *cpu)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    return !scheduler.controlled_owners && cpu && cpu->current == cpu->idle &&
        !cpu->switch_previous && !cpu->eligible.first && !cpu->active_mm_record
#if BOAROS_USER_SMP
        && !cpu->user_reservations.first && !scheduler.rt_services.first
#endif
        ;
}
enum kernel_scheduler_status kernel_thread_create_joinable_mask(
    void (*entry)(void *), void *argument, struct kernel_thread_join *join, uint32_t mask)
{
    if (!join || !mask || (mask & ~3U)) return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    unsigned available;
    { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
      available = available_cpu_mask_locked(); }
    if (mask & ~available) return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    return create_kernel_thread(entry, argument, join, mask, 0, 0);
}
enum kernel_scheduler_status kernel_thread_create_joinable_mm(
    void (*entry)(void *), void *argument, struct kernel_thread_join *join,
    uint32_t mask, struct kernel_mm *mm)
{
    kernel_assert_service_context();
    if (!join || !mm || !mask || (mask & ~3U)) return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    unsigned available;
    { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain); available = available_cpu_mask_locked(); }
    if (mask & ~available) return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    return create_kernel_thread(entry, argument, join, mask, mm, 0);
}

enum kernel_scheduler_status kernel_thread_create_joinable_files(
    void (*entry)(void *), void *argument, struct kernel_thread_join *join,
    uint32_t mask, const struct kernel_mm *mm, const struct kernel_files *files)
{
    kernel_assert_service_context();
    if (!join || !mm || !files || !mask || (mask & ~3U))
        return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    if (kernel_files_check_task_domain(files,mm))
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    struct kernel_mm mm_owner={0}; struct kernel_files files_owner={0};
    enum kernel_mm_status acquired=kernel_mm_acquire(&mm_owner,mm);
    if (acquired) return acquired==KERNEL_MM_STATUS_NO_MEMORY
        ? KERNEL_SCHEDULER_STATUS_NO_MEMORY : KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE;
    enum kernel_scheduler_status status;
    if (kernel_files_acquire(&files_owner,files)) status=KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    else status=create_kernel_thread(entry,argument,join,mask,&mm_owner,&files_owner);
    if (files_owner.state==KERNEL_FILES_LIVE && kernel_files_release(&files_owner)) __builtin_trap();
    if (mm_owner.state==KERNEL_MM_LIVE && kernel_mm_release(&mm_owner)) __builtin_trap();
    return status;
}

enum kernel_scheduler_status kernel_thread_set_cpu_mask(struct kernel_thread_join *join,
                                                       uint32_t mask)
{
    kernel_assert_service_context();
    if (!join || !mask || (mask & ~3U)) return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    struct kernel_task *task = join->task;
    if (!task || !task->controlled) return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    unsigned available = available_cpu_mask_locked();
    if (mask & ~available) return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    if (task->state != KERNEL_THREAD_STATE_BLOCKED || task->wait.on_cpu ||
        task->ready_node.queued || task->wait.ready_pending)
        return KERNEL_SCHEDULER_STATUS_BUSY;
    task->cpu_mask = mask;
    return KERNEL_SCHEDULER_STATUS_OK;
}
#endif

enum kernel_scheduler_status kernel_user_thread_create(
    struct kernel_mm *mm,
    struct kernel_files *files,
    struct kernel_fs_context *fs,
    uintptr_t entry,
    uintptr_t stack_pointer,
    uintptr_t thread_pointer)
{
    struct kernel_mm_mapping entry_mapping;
    struct kernel_mm_mapping stack_mapping;
    struct kernel_vma entry_vma;
    struct arch_trap_frame *frame;
    struct kernel_task *thread;
    uint64_t user_context;
    uintptr_t old_status;
    enum arch_context_status context_status;
    enum kernel_pid_status pid_status;
    kernel_pid_t tid;
    enum kernel_mm_status mm_status;
    enum kernel_scheduler_status status;
    int entry_present = 0;

    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED) {
        return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (mm == 0 || (files == 0) != (fs == 0) || entry == 0U ||
        (entry & (uintptr_t)1U) != 0U || stack_pointer == 0U ||
        (stack_pointer & (uintptr_t)15U) != 0U) {
        return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    }

    old_status = arch_interrupt_save();
    if (scheduler_failure() != KERNEL_SCHEDULER_STATUS_OK) {
        status = scheduler_failure();
        goto restore_interrupts;
    }
    status = validate_current();
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        goto restore_interrupts;
    }
    status = validate_queues();
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        goto restore_interrupts;
    }
    if (mm->allocator != scheduler.allocator) {
        status = KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
        goto restore_interrupts;
    }
    if (files != 0 &&
        (!kernel_files_is_live(files) ||
         !kernel_fs_context_is_live(fs) || files->heap != fs->heap ||
         files->heap->page_allocator != scheduler.allocator)) {
        status = KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
        goto restore_interrupts;
    }
    mm_status = kernel_mm_context(mm, &user_context);
    if (mm_status != KERNEL_MM_STATUS_OK) {
        status = mm_status ==
                         KERNEL_MM_STATUS_INVALID_ARGUMENT
                     ? KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT
                     : KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE;
        goto restore_interrupts;
    }
    mm_status = kernel_mm_lookup(mm, entry, &entry_mapping);
    if (mm_status == KERNEL_MM_STATUS_OK) {
        entry_present = 1;
    }
    if (mm_status == KERNEL_MM_STATUS_NOT_MAPPED) {
        mm_status = kernel_mm_vma_lookup(mm, entry, &entry_vma);
        if (mm_status == KERNEL_MM_STATUS_OK &&
            (entry_vma.permissions & KERNEL_MM_EXECUTE) != 0U) {
            mm_status = KERNEL_MM_STATUS_OK;
        }
    }
    if (mm_status != KERNEL_MM_STATUS_OK) {
        status = mm_status == KERNEL_MM_STATUS_NOT_MAPPED ||
                         mm_status ==
                             KERNEL_MM_STATUS_INVALID_ARGUMENT
                     ? KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT
                     : KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE;
        goto restore_interrupts;
    }
    if (entry_present != 0 &&
        (entry_mapping.permissions &
         (KERNEL_MM_USER | KERNEL_MM_EXECUTE)) !=
            (KERNEL_MM_USER | KERNEL_MM_EXECUTE)) {
        status = KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
        goto restore_interrupts;
    }
    mm_status = kernel_mm_lookup(mm,
                                               stack_pointer - 1U,
                                               &stack_mapping);
    if (mm_status != KERNEL_MM_STATUS_OK) {
        status = mm_status == KERNEL_MM_STATUS_NOT_MAPPED ||
                         mm_status ==
                             KERNEL_MM_STATUS_INVALID_ARGUMENT
                     ? KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT
                     : KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE;
        goto restore_interrupts;
    }
    if ((stack_mapping.permissions &
         (KERNEL_MM_USER | KERNEL_MM_READ | KERNEL_MM_WRITE)) !=
        (KERNEL_MM_USER | KERNEL_MM_READ | KERNEL_MM_WRITE)) {
        status = KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
        goto restore_interrupts;
    }

    status = allocate_task_storage(&thread);
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        goto restore_interrupts;
    }
    thread->arch.user_sp = 0U;
    thread->arch.user_mode = 1U;
#if BOAROS_USER_SMP
    thread->user_service = 1;
    /* PID1在secondary ONLINE前构造；默认集合在首次服务入口固定。 */
    thread->cpu_mask = 0;
#endif
    arch_thread_set_mm(&thread->arch, user_context);
    thread->magic = KERNEL_THREAD_MAGIC;
    thread->next = 0;
    thread->state = KERNEL_THREAD_STATE_READY;
    thread->idle = 0U;
    thread->completion.kind = KERNEL_THREAD_KIND_USER;
    thread->completion.reason = KERNEL_THREAD_EXIT_SYSCALL;
    thread->completion.tid = 0;
    thread->completion.tgid = 0;
    thread->completion.status = 0U;
    thread->completion.detail = 0U;
    thread->exec_transaction = 0;
    kernel_wait_queue_init(&thread->child_exit_queue);
    kernel_wait_queue_init(&thread->vfork_done_queue);

    frame = (struct arch_trap_frame *)(thread->stack_high -
                                        sizeof(*frame));
    if ((uintptr_t)frame < thread->stack_low) {
        status = release_task_storage(
            thread,
            KERNEL_SCHEDULER_STATUS_INVALID_STATE);
        goto restore_interrupts;
    }
    arch_process_prepare_initial(thread, entry, stack_pointer, thread_pointer);
    context_status = arch_context_init_user(&thread->context,
                                             (uintptr_t)frame,
                                             thread);
    if (context_status != ARCH_CONTEXT_STATUS_OK) {
        status = release_task_storage(
            thread,
            KERNEL_SCHEDULER_STATUS_INVALID_STATE);
        goto restore_interrupts;
    }
    pid_status = process_identity_create(thread, 0, 0);
    if (pid_status != KERNEL_PID_STATUS_OK) {
        status = release_task_storage(
            thread,
            pid_status == KERNEL_PID_STATUS_EXHAUSTED
                ? KERNEL_SCHEDULER_STATUS_NO_MEMORY
                : KERNEL_SCHEDULER_STATUS_INVALID_STATE);
        goto restore_interrupts;
    }
    tid = thread->tid;
    thread->proc_start_ticks = kernel_tick_count();
    thread->publish_completion = 1U;
    thread->wait_status = 0U;
    thread->group_leader = thread;
    thread->group_members = 1U;
    thread->nofile_limit = (struct kernel_rlimit64){
        KERNEL_RLIMIT_NOFILE_CAP, KERNEL_RLIMIT_NOFILE_CAP};
    thread->stack_limit = (struct kernel_rlimit64){
        KERNEL_RLIMIT_STACK_CAP, KERNEL_RLIMIT_STACK_CAP};
    thread->core_limit = (struct kernel_rlimit64){0, KERNEL_RLIM_INFINITY};
    thread->fsize_limit = (struct kernel_rlimit64){KERNEL_RLIM_INFINITY, KERNEL_RLIM_INFINITY};
    process_group_initialize(thread);
    thread->completion.tid = tid;
    thread->completion.tgid = tid;
    mm_status = kernel_mm_move(&thread->mm, mm);
    if (mm_status != KERNEL_MM_STATUS_OK) {
        status = release_task_storage(
            thread,
            KERNEL_SCHEDULER_STATUS_INVALID_STATE);
        goto restore_interrupts;
    }
    kernel_proc_task_update_comm(thread);
    if (files != 0) {
        if (kernel_files_move(&thread->files, files) !=
                KERNEL_FILES_STATUS_OK ||
            kernel_fs_context_move(&thread->fs, fs) !=
                KERNEL_FS_CONTEXT_STATUS_OK) {
            status = KERNEL_SCHEDULER_STATUS_INVALID_STATE;
            goto restore_interrupts;
        }
    }

    kernel_mm_add_user(&thread->mm);
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        ready_enqueue_locked(thread, 0);
        if (tid == 1) scheduler.init_task = thread;
    }
    status = KERNEL_SCHEDULER_STATUS_OK;

restore_interrupts:
    arch_interrupt_restore(old_status);
    return status;
}

void scheduler_task_disposal_notify(void)
{
    KERNEL_IRQ_SCOPE(disposal_irq);
    (void)kernel_wait_queue_wake_all(&scheduler.cleanup_queue);
}

int scheduler_forget_task_locked(struct kernel_task *thread, struct kernel_wait_cursor *cursor)
{
    if (thread->deadline_indexed) __builtin_trap();
    process_identity_release_locked(thread);
    struct kernel_thread_join *join = thread->join;
    int notify = 0;
    if (join) {
        notify = kernel_wait_cursor_prepare_locked(&join->waiters, cursor, 1);
        join->task = 0;
        thread->join = 0;
    }
    if (scheduler.init_task == thread) scheduler.init_task = 0;
    scheduler_task_unlink_locked(thread);
    return notify;
}

void scheduler_forget_task(struct kernel_task *thread)
{
    struct kernel_wait_cursor cursor;
    int notify;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        notify = scheduler_forget_task_locked(thread, &cursor);
    }
    if (notify) kernel_wait_cursor_wake(&cursor, 1);
    process_identity_collect();
}

void kernel_scheduler_system_statistics(uint64_t loads[3], uint16_t *tasks)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    uint64_t count = scheduler.task_count;
    *tasks = count > UINT16_MAX ? UINT16_MAX : (uint16_t)count;
    for (unsigned i = 0; i < 3; i++) loads[i] = scheduler.loads[i] << 5;
}

static enum kernel_scheduler_status scheduler_sample_load(uint64_t elapsed)
{
    const uint64_t period = 5 * KERNEL_TICKS_PER_SECOND + 1;
    /* 只有CPU0的IRQ关闭tick推进这个采样余量，普通tick不另取调度域。 */
    scheduler.load_ticks += elapsed;
    if (scheduler.load_ticks < period) return KERNEL_SCHEDULER_STATUS_OK;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        enum kernel_scheduler_status status = validate_queues_locked();
        if (status != KERNEL_SCHEDULER_STATUS_OK) return status;
    }
    uint64_t active = 0;
    struct scheduler_task_cursor cursor;
    scheduler_task_cursor_begin(&cursor);
    while (cursor.task) {
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            struct kernel_task *task = cursor.task;
            if (task->accounted && (task->state == KERNEL_THREAD_STATE_READY ||
                task->state == KERNEL_THREAD_STATE_RUNNING ||
                (task->state == KERNEL_THREAD_STATE_BLOCKED && !task->wait.interruptible))) active++;
        }
        scheduler_task_cursor_next(&cursor);
    }
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    active <<= 11;
    static const unsigned decay[3] = {1884, 2014, 2037};
    while (scheduler.load_ticks >= period) {
        scheduler.load_ticks -= period;
        for (unsigned i = 0; i < 3; i++) {
            uint64_t load = scheduler.loads[i];
            scheduler.loads[i] = (load * decay[i] + active * (2048 - decay[i]) +
                                  (active >= load ? 2047 : 0)) >> 11;
        }
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}

enum kernel_scheduler_status kernel_scheduler_on_tick(uint64_t elapsed_ticks)
{
    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED)
        return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    if (arch_interrupt_is_enabled()) return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    if (scheduler_failure() != KERNEL_SCHEDULER_STATUS_OK) return scheduler_failure();
    enum kernel_scheduler_status status = validate_current();
    if (status != KERNEL_SCHEDULER_STATUS_OK) return status;
    if (elapsed_ticks
#if BOAROS_MAX_CPUS > 1
        && kernel_cpu_current()->logical_id == 0
#endif
    ) {
        status = scheduler_sample_load(elapsed_ticks);
        if (status != KERNEL_SCHEDULER_STATUS_OK) return status;
    }
    return scheduler_reschedule(elapsed_ticks != 0, 0, 1);
}

enum kernel_scheduler_status kernel_scheduler_yield_current(void)
{
    kernel_assert_can_block();
    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED)
        return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    if (arch_interrupt_is_enabled()) return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    if (scheduler_failure() != KERNEL_SCHEDULER_STATUS_OK) return scheduler_failure();
    enum kernel_scheduler_status status = validate_current();
    if (status != KERNEL_SCHEDULER_STATUS_OK) return status;
    return scheduler_reschedule(0, 1, 1);
}

void kernel_scheduler_charge_ticks(uint64_t elapsed_ticks, int from_user)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    struct kernel_task *current = kernel_cpu_current()->current;

    if (current && current->idle) {
        kernel_cpu_current()->idle_ticks += elapsed_ticks;
        return;
    }
    if (current == 0 ||
        current->magic != KERNEL_THREAD_MAGIC) {
        return;
    }
    (void)from_user; /* 用户/内核用量由执行边界结算，tick只保留idle与负载采样。 */
}

uint64_t kernel_scheduler_idle_ticks(void)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    uint64_t ticks = 0;
    for (struct kernel_cpu *cpu = scheduler.cpus; cpu; cpu = cpu->scheduler_next)
        ticks += cpu->idle_ticks;
    return ticks;
}
