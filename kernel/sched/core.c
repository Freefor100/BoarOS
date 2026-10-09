#include <arch/task.h>
#include <arch/context.h>
#include <kernel/files.h>
#include <kernel/errno.h>
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

_Static_assert(offsetof(struct kernel_task, cpu) == ARCH_TASK_CPU_OFFSET,
               "current CPU lookup must preserve the architecture prefix");
void kernel_cpu_boot_initialize(uint64_t hardware_id)
{
    kernel_cpu_initialize(&boot_cpu, hardware_id, &bootstrap_task.io_context);
    bootstrap_task.cpu = &boot_cpu;
    arch_current_thread_set(&bootstrap_task);
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
        if (thread != &scheduler.idle ||
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
        if (thread->arch.user_sp != 0U ||
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
    } else {
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
            if (thread->mm.allocator != scheduler.allocator) {
                return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
            }
        } else if (thread->mm.state ==
                   KERNEL_MM_CLEANUP) {
            if (thread->mm.allocator != scheduler.allocator ||
                (thread->mm.record_page_address &
                 BOAROS_PAGE_MASK) != 0U) {
                return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
            }
        } else if (thread->mm.allocator != 0 ||
                   thread->mm.record_page_address != 0U) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
    }

    canary = (const uint64_t *)(thread->stack_low - sizeof(*canary));
    if (*canary != KERNEL_STACK_CANARY) {
        return KERNEL_SCHEDULER_STATUS_STACK_CORRUPT;
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}

enum kernel_scheduler_status validate_current(void)
{
    enum kernel_thread_state expected_state;
    enum kernel_scheduler_status status;
    uintptr_t stack_pointer;

    if (kernel_cpu_current()->current == 0 ||
        arch_current_thread_get() != kernel_cpu_current()->current) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    expected_state = kernel_cpu_current()->current->idle != 0U
                         ? KERNEL_THREAD_STATE_IDLE
                         : KERNEL_THREAD_STATE_RUNNING;
    status = validate_thread(kernel_cpu_current()->current, expected_state);
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }

    stack_pointer = current_sp();
    if (stack_pointer < kernel_cpu_current()->current->stack_low ||
        stack_pointer >= kernel_cpu_current()->current->stack_high) {
        return KERNEL_SCHEDULER_STATUS_STACK_CORRUPT;
    }
    if (arch_mmu_current_context() != arch_thread_mm(&kernel_cpu_current()->current->arch)) {
        return KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE;
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}

enum kernel_scheduler_status activate_thread_address_space(
    const struct kernel_task *thread)
{
    if (thread == 0) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    if (arch_mmu_current_context() == arch_thread_mm(&thread->arch)) {
        return KERNEL_SCHEDULER_STATUS_OK;
    }
    if (arch_mmu_switch_context(arch_thread_mm(&thread->arch)) !=
        ARCH_MMU_STATUS_OK) {
        return KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE;
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}

enum kernel_scheduler_status validate_queues(void)
    __attribute__((noinline, noclone));

enum kernel_scheduler_status validate_queues(void)
{
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

enum kernel_scheduler_status scheduler_switch_current_away(
    struct kernel_task *previous)
{
    kernel_assert_can_block();
    scheduler_account_runtime();
    struct kernel_task *next = ready_best();
    if (!next) next = &scheduler.idle;
    enum kernel_scheduler_status status = activate_thread_address_space(next);
    if (status != KERNEL_SCHEDULER_STATUS_OK) return status;
    if (next != &scheduler.idle) {
        ready_remove(next);
        next->state = KERNEL_THREAD_STATE_RUNNING;
    }
#if BOAROS_COST_DIAGNOSTICS
    kernel_cost_switch(&previous->cost, &next->cost);
#endif
    next->cpu = kernel_cpu_current();
    kernel_cpu_current()->current = next;
    kernel_cpu_current()->need_resched = 0;
    scheduler_rearm_timer();
    if (next == previous) return KERNEL_SCHEDULER_STATUS_OK;
    arch_fpu_switch(&previous->fpu, &next->fpu);
    arch_context_switch(&previous->context, &next->context);
    return KERNEL_SCHEDULER_STATUS_OK;
}

void clear_page(void *pointer)
{
    volatile unsigned char *bytes = pointer;
    size_t index;

    for (index = 0U; index < BOAROS_PAGE_SIZE; index++) {
        bytes[index] = 0U;
    }
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
static uint64_t kernel_stack_slot_bitmap[KERNEL_STACK_SLOT_WORDS];

static int kernel_stack_slot_acquire(uint64_t *window_address)
{
    for (uint32_t word = 0U; word < KERNEL_STACK_SLOT_WORDS; word++) {
        uint64_t free_bits = ~kernel_stack_slot_bitmap[word];
        uint32_t bit = 0U;

        if (free_bits == 0U) continue;
        while ((free_bits & (UINT64_C(1) << bit)) == 0U) bit++;
        if (word * 64U + bit >= KERNEL_STACK_SLOT_COUNT) return -1;
        kernel_stack_slot_bitmap[word] |= UINT64_C(1) << bit;
        *window_address = ARCH_KERNEL_STACK_WINDOW_BASE +
                          (uint64_t)(word * 64U + bit) *
                              ARCH_KERNEL_STACK_SLOT_SIZE;
        return 0;
    }
    return -1;
}

static void kernel_stack_slot_release(uint64_t window_address)
{
    uint32_t index = (uint32_t)((window_address -
                                 ARCH_KERNEL_STACK_WINDOW_BASE) /
                                ARCH_KERNEL_STACK_SLOT_SIZE);
    kernel_stack_slot_bitmap[index / 64U] &= ~(UINT64_C(1) << (index % 64U));
}

static void kernel_stack_slots_reset(void)
{
    for (uint32_t word = 0U; word < KERNEL_STACK_SLOT_WORDS; word++)
        kernel_stack_slot_bitmap[word] = 0U;
}

/* Allocate the metadata and execution stack before publishing any owner. */
enum kernel_scheduler_status allocate_task_storage(struct kernel_task **task)
{
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
    memset(stack, KERNEL_STACK_FILL, KERNEL_STACK_BYTES);
    thread->stack_physical_address = stack_address;
    if (arch_mmu_kernel_window_active()) {
        uint64_t window;
        uint64_t stack_va;
        enum arch_mmu_status map_status;

        if (kernel_stack_slot_acquire(&window) != 0) {
            (void)physical_page_release_order(scheduler.allocator,
                                              stack_address,
                                              KERNEL_STACK_ORDER);
            return release_after_create_failure(
                metadata_address, KERNEL_SCHEDULER_STATUS_NO_MEMORY);
        }
        stack_va = window + BOAROS_PAGE_SIZE;
        map_status = arch_mmu_kernel_window_map(scheduler.allocator,
                                                  stack_va, stack_address);
        if (map_status == ARCH_MMU_STATUS_OK) {
            map_status = arch_mmu_kernel_window_map(
                scheduler.allocator, stack_va + BOAROS_PAGE_SIZE,
                stack_address + BOAROS_PAGE_SIZE);
        }
        if (map_status != ARCH_MMU_STATUS_OK) {
            (void)arch_mmu_kernel_window_unmap(scheduler.allocator,
                                                 stack_va);
            (void)arch_mmu_kernel_window_unmap(
                scheduler.allocator, stack_va + BOAROS_PAGE_SIZE);
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
    while ((uintptr_t)cursor < thread->stack_high &&
           *cursor == KERNEL_STACK_FILL)
        cursor++;
    free_bytes = (uintptr_t)cursor - thread->stack_low;
    used_bytes = thread->stack_high - (uintptr_t)cursor;
    scheduler.stack_statistics.stacks_released++;
    if (free_bytes < scheduler.stack_statistics.minimum_free_bytes)
        scheduler.stack_statistics.minimum_free_bytes = free_bytes;
    if (used_bytes > scheduler.stack_statistics.maximum_used_bytes)
        scheduler.stack_statistics.maximum_used_bytes = used_bytes;
    if (arch_mmu_kernel_window_active()) {
        uint64_t window = (uint64_t)thread->stack_low -
                          KERNEL_STACK_GUARD_BYTES - BOAROS_PAGE_SIZE;
        (void)arch_mmu_kernel_window_unmap(scheduler.allocator,
                                             window + BOAROS_PAGE_SIZE);
        (void)arch_mmu_kernel_window_unmap(
            scheduler.allocator, window + BOAROS_PAGE_SIZE * 2U);
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
    enum kernel_scheduler_status status = release_task_stack(thread);
    if (status != KERNEL_SCHEDULER_STATUS_OK) return status;
    scheduler_forget_task(thread);
    kernel_task_release_io_scratch(thread);
    (void)physical_page_release(scheduler.allocator, thread->physical_address);
    return original_status;
}

void kernel_scheduler_stack_statistics(struct kernel_stack_statistics *statistics)
{
    uintptr_t old_status = arch_interrupt_save();
    if (statistics != 0) *statistics = scheduler.stack_statistics;
    arch_interrupt_restore(old_status);
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
    scheduler.idle.arch.kernel_sp = idle_stack_high;
    scheduler.idle.cpu = kernel_cpu_current();
    scheduler.idle.arch.user_sp = 0U;
    scheduler.idle.arch.user_mode = 0U;
    arch_thread_set_mm(&scheduler.idle.arch, scheduler.kernel_context);
    scheduler.idle.magic = KERNEL_THREAD_MAGIC;
    scheduler.idle.physical_address = KERNEL_THREAD_NO_PAGE;
    scheduler.idle.stack_physical_address = KERNEL_THREAD_NO_PAGE;
    scheduler.idle.stack_low = idle_stack_low;
    scheduler.idle.stack_high = idle_stack_high;
    scheduler.idle.next = 0;
    scheduler.idle.parent = 0;
    scheduler.idle.first_child = 0;
    scheduler.idle.last_child = 0;
    scheduler.idle.previous_sibling = 0;
    scheduler.idle.next_sibling = 0;
    scheduler.idle.state = KERNEL_THREAD_STATE_IDLE;
    scheduler.idle.idle = 1U;
    scheduler.idle.tid = 0;
    scheduler.idle.tid_owned = 0U;
    scheduler.idle.publish_completion = 0U;
    scheduler.idle.wait_status = 0U;
    scheduler.idle.group_leader = 0;
    scheduler.idle.group_members = 0U;
    scheduler.idle.completion.kind = KERNEL_THREAD_KIND_KERNEL;
    scheduler.idle.completion.reason = KERNEL_THREAD_EXIT_RETURNED;
    scheduler.idle.completion.tid = 0;
    scheduler.idle.completion.tgid = 0;
    scheduler.idle.completion.status = 0U;
    scheduler.idle.completion.detail = 0U;
#if BOAROS_COST_DIAGNOSTICS
    scheduler.idle.cost.wait_flags = 128;
#endif
    kernel_cpu_current()->current = &scheduler.idle;
    scheduler.cleanup_task = 0;
    kernel_wait_queue_init(&scheduler.cleanup_queue);
    memset(&scheduler.runqueue, 0, sizeof(scheduler.runqueue));
    kernel_rt_bandwidth_init(&scheduler.rt_bandwidth, kernel_time_monotonic_ns());
    kernel_cpu_current()->need_resched = 0;
    scheduler.exited_head = 0;
    scheduler.exited_tail = 0;
    scheduler.blocked_head = 0;
    scheduler.blocked_tail = 0;
    scheduler.deadline_root = 0;
    scheduler.armed_deadline = 0;
    kernel_stack_slots_reset();
    scheduler.stopped_head = 0;
    scheduler.stopped_tail = 0;
    scheduler.init_task = 0;
    scheduler.fatal_status = KERNEL_SCHEDULER_STATUS_OK;
    scheduler.stack_statistics.stacks_released = 0U;
    scheduler.stack_statistics.minimum_free_bytes =
        KERNEL_STACK_BYTES - KERNEL_STACK_GUARD_BYTES;
    scheduler.stack_statistics.maximum_used_bytes = 0U;
    scheduler.idle_context_saved = 0U;
    scheduler.initialized = KERNEL_SCHEDULER_INITIALIZED;
    arch_current_thread_set(&scheduler.idle);
    return KERNEL_SCHEDULER_STATUS_OK;
}

enum kernel_scheduler_status kernel_thread_create(void (*entry)(void *), void *argument)
{ return kernel_thread_create_joinable(entry, argument, 0); }

enum kernel_scheduler_status kernel_thread_create_joinable(
    void (*entry)(void *), void *argument, struct kernel_thread_join *join)
{
    struct kernel_task *thread;
    uintptr_t old_status;
    enum arch_context_status context_status;
    enum kernel_scheduler_status status;

    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED) {
        return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (entry == 0 || (join && join->task)) {
        return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    }

    old_status = arch_interrupt_save();
    if (scheduler.fatal_status != KERNEL_SCHEDULER_STATUS_OK) {
        status = scheduler.fatal_status;
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
    thread->arch.user_mode = 0U;
    arch_thread_set_mm(&thread->arch, scheduler.kernel_context);
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

    if (join) {
        kernel_wait_queue_init(&join->waiters);
        join->task = thread;
        thread->join = join;
    }
    ready_append(thread);
    status = KERNEL_SCHEDULER_STATUS_OK;

restore_interrupts:
    arch_interrupt_restore(old_status);
    return status;
}

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
    if (scheduler.fatal_status != KERNEL_SCHEDULER_STATUS_OK) {
        status = scheduler.fatal_status;
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
    ready_append(thread);
    if (tid == 1) {
        scheduler.init_task = thread;
    }
    status = KERNEL_SCHEDULER_STATUS_OK;

restore_interrupts:
    arch_interrupt_restore(old_status);
    return status;
}

void scheduler_forget_task(struct kernel_task *thread)
{
    /* 退出任务必须先经唤醒路径摘除期限索引，不能带着索引项被销毁。 */
    if (thread->deadline_indexed) __builtin_trap();
    process_identity_release(thread);
    process_identity_collect();
    if (thread->join) {
        thread->join->task = 0;
        (void)kernel_wait_queue_wake_all(&thread->join->waiters);
        thread->join = 0;
    }
    if (!thread->accounted) return;
    struct kernel_task **link = &scheduler.all_tasks;
    while (*link && *link != thread) link = &(*link)->all_next;
    if (!*link) __builtin_trap();
    *link = thread->all_next;
    thread->accounted = 0;
}

void kernel_scheduler_system_statistics(uint64_t loads[3], uint16_t *tasks)
{
    uintptr_t irq = arch_interrupt_save();
    uint64_t count = 0;
    for (struct kernel_task *t = scheduler.all_tasks; t; t = t->all_next) count++;
    *tasks = count > UINT16_MAX ? UINT16_MAX : (uint16_t)count;
    for (unsigned i = 0; i < 3; i++) loads[i] = scheduler.loads[i] << 5;
    arch_interrupt_restore(irq);
}

static void scheduler_sample_load(uint64_t elapsed)
{
    scheduler.load_ticks += elapsed;
    const uint64_t period = 5 * KERNEL_TICKS_PER_SECOND + 1;
    if (scheduler.load_ticks < period) return;
    uint64_t active = 0;
    for (struct kernel_task *t = scheduler.all_tasks; t; t = t->all_next)
        if (t->state == KERNEL_THREAD_STATE_READY || t->state == KERNEL_THREAD_STATE_RUNNING ||
            (t->state == KERNEL_THREAD_STATE_BLOCKED && !t->wait_interruptible)) active++;
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
}

enum kernel_scheduler_status kernel_scheduler_on_tick(uint64_t elapsed_ticks)
{
    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED)
        return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    if (arch_interrupt_is_enabled()) return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    if (scheduler.fatal_status != KERNEL_SCHEDULER_STATUS_OK) return scheduler.fatal_status;
    enum kernel_scheduler_status status = validate_current();
    if (status != KERNEL_SCHEDULER_STATUS_OK) return status;
    status = validate_queues();
    if (status != KERNEL_SCHEDULER_STATUS_OK) return status;
    if (elapsed_ticks) scheduler_sample_load(elapsed_ticks);
    return scheduler_reschedule(elapsed_ticks != 0, 0);
}

enum kernel_scheduler_status kernel_scheduler_yield_current(void)
{
    kernel_assert_can_block();
    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED)
        return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    if (arch_interrupt_is_enabled()) return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    if (scheduler.fatal_status != KERNEL_SCHEDULER_STATUS_OK) return scheduler.fatal_status;
    enum kernel_scheduler_status status = validate_current();
    if (status != KERNEL_SCHEDULER_STATUS_OK) return status;
    status = validate_queues();
    if (status != KERNEL_SCHEDULER_STATUS_OK) return status;
    return scheduler_reschedule(0, 1);
}

void kernel_scheduler_charge_ticks(uint64_t elapsed_ticks, int from_user)
{
    struct kernel_task *current = kernel_cpu_current()->current;

    if (current == &scheduler.idle) {
        scheduler.idle_ticks += elapsed_ticks;
        return;
    }
    if (current == 0 ||
        current->magic != KERNEL_THREAD_MAGIC) {
        return;
    }
    if (from_user) {
        current->user_ticks += elapsed_ticks;
    } else {
        current->kernel_ticks += elapsed_ticks;
    }
}

uint64_t kernel_scheduler_idle_ticks(void)
{
    return scheduler.idle_ticks;
}
