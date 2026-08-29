#include <arch/riscv/context.h>
#include <arch/riscv/mm.h>
#include <arch/riscv/sv39.h>
#include <arch/riscv/thread.h>
#include <arch/riscv/trap.h>
#include <kernel/files.h>
#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/pid.h>
#include <kernel/scheduler.h>
#include <kernel/task.h>

#include <stddef.h>
#include <stdint.h>

#define KERNEL_SCHEDULER_INITIALIZED UINT32_C(0x53434844)
#define KERNEL_THREAD_MAGIC UINT64_C(0x424f415254485244)
#define KERNEL_STACK_CANARY UINT64_C(0x535441434b4f4b21)
#define KERNEL_THREAD_NO_PAGE UINT64_MAX
#define KERNEL_THREAD_MINIMUM_STACK 512U
#define KERNEL_PID_LIMIT 32768U

enum kernel_thread_state {
    KERNEL_THREAD_STATE_IDLE = 0,
    KERNEL_THREAD_STATE_READY,
    KERNEL_THREAD_STATE_RUNNING,
    KERNEL_THREAD_STATE_EXITED,
};

struct kernel_task {
    struct riscv_thread_state arch;
    uint64_t magic;
    uint64_t physical_address;
    uintptr_t stack_low;
    uintptr_t stack_high;
    struct kernel_task *next;
    uint32_t state;
    uint32_t idle;
    kernel_pid_t tid;
    uint32_t tid_owned;
    struct kernel_task *group_leader;
    uint32_t group_members;
    struct kernel_thread_completion completion;
    struct kernel_files files;
    struct kernel_fs_context fs;
    struct kernel_mm mm;
    struct riscv_switch_context context;
} __attribute__((aligned(16)));

struct kernel_scheduler {
    uint32_t initialized;
    uint32_t idle_context_saved;
    uint64_t kernel_satp;
    struct physical_page_allocator *allocator;
    struct kernel_pid_allocator pid_allocator;
    uint64_t pid_bitmap[KERNEL_PID_BITMAP_WORDS(KERNEL_PID_LIMIT)];
    struct kernel_task idle;
    struct kernel_task *current;
    struct kernel_task *ready_head;
    struct kernel_task *ready_tail;
    struct kernel_task *exited_head;
    struct kernel_task *exited_tail;
    uint64_t cleanup_page_address;
    uint32_t cleanup_page_owned;
    enum kernel_scheduler_status fatal_status;
    struct riscv_switch_context discard_context;
};

static struct kernel_scheduler scheduler;

_Static_assert(sizeof(struct kernel_task) + sizeof(uint64_t) +
                       KERNEL_THREAD_MINIMUM_STACK <=
                   BOAROS_PAGE_SIZE,
               "kernel thread metadata leaves too little stack");
_Static_assert(offsetof(struct kernel_task, arch) == 0U,
               "RISC-V thread state must prefix the scheduler thread");

static uintptr_t current_sp(void)
{
    uintptr_t value;

    __asm__ volatile("mv %0, sp" : "=r"(value));
    return value;
}

static uintptr_t align_up_16(uintptr_t value)
{
    return (value + 15U) & ~(uintptr_t)15U;
}

static enum kernel_scheduler_status validate_queue_shape(
    const struct kernel_task *head,
    const struct kernel_task *tail)
{
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
            thread->arch.satp != scheduler.kernel_satp ||
            thread->completion.kind != KERNEL_THREAD_KIND_KERNEL ||
            thread->completion.tid != 0 ||
            thread->completion.tgid != 0 ||
            thread->tid != 0 || thread->tid_owned != 0U ||
            thread->group_leader != 0 || thread->group_members != 0U ||
            thread->files.state != KERNEL_FILES_EMPTY ||
            thread->fs.state != KERNEL_FS_CONTEXT_EMPTY ||
            thread->mm.state != KERNEL_MM_EMPTY) {
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
    expected_low = align_up_16(base + sizeof(*thread) + sizeof(*canary));
    if (thread->stack_low != expected_low ||
        thread->stack_high != base + BOAROS_PAGE_SIZE ||
        thread->context.tp != base ||
        thread->arch.kernel_sp != thread->stack_high ||
        thread->context.sp < thread->stack_low ||
        thread->context.sp > thread->stack_high ||
        (thread->context.sp & (uintptr_t)15U) != 0U) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }

    if (thread->arch.user_mode == 0U) {
        if (thread->arch.user_sp != 0U ||
            thread->arch.satp != scheduler.kernel_satp ||
            thread->completion.kind != KERNEL_THREAD_KIND_KERNEL ||
            thread->completion.tid != 0 ||
            thread->completion.tgid != 0 ||
            thread->tid != 0 || thread->tid_owned != 0U ||
            thread->group_leader != 0 || thread->group_members != 0U ||
            thread->files.state != KERNEL_FILES_EMPTY ||
            thread->fs.state != KERNEL_FS_CONTEXT_EMPTY ||
            thread->mm.state != KERNEL_MM_EMPTY) {
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
                    thread->files.heap == 0 || thread->files.slots == 0 ||
                    thread->fs.state != KERNEL_FS_CONTEXT_LIVE ||
                    thread->fs.heap == 0 || thread->fs.root_mount == 0 ||
                    thread->fs.cwd == 0 ||
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
        if (thread->tid_owned != 0U) {
            if (thread->tid <= 0 || thread->group_leader != thread ||
                thread->group_members != 1U) {
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

static enum kernel_scheduler_status validate_current(void)
{
    enum kernel_thread_state expected_state;
    enum kernel_scheduler_status status;
    uintptr_t stack_pointer;

    if (scheduler.current == 0 ||
        riscv_current_thread_get() != scheduler.current) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    expected_state = scheduler.current->idle != 0U
                         ? KERNEL_THREAD_STATE_IDLE
                         : KERNEL_THREAD_STATE_RUNNING;
    status = validate_thread(scheduler.current, expected_state);
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }

    stack_pointer = current_sp();
    if (stack_pointer < scheduler.current->stack_low ||
        stack_pointer >= scheduler.current->stack_high) {
        return KERNEL_SCHEDULER_STATUS_STACK_CORRUPT;
    }
    if (riscv_sv39_current_satp() != scheduler.current->arch.satp) {
        return KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE;
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}

static enum kernel_scheduler_status activate_thread_address_space(
    const struct kernel_task *thread)
{
    if (thread == 0) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    if (riscv_sv39_current_satp() == thread->arch.satp) {
        return KERNEL_SCHEDULER_STATUS_OK;
    }
    if (riscv_sv39_switch_satp(thread->arch.satp) !=
        RISCV_SV39_STATUS_OK) {
        return KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE;
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}

static enum kernel_scheduler_status validate_queues(void)
{
    enum kernel_scheduler_status status;

    if (scheduler.cleanup_page_owned > 1U ||
        (scheduler.cleanup_page_owned == 0U &&
         scheduler.cleanup_page_address != 0U) ||
        (scheduler.cleanup_page_owned != 0U &&
         (scheduler.cleanup_page_address & BOAROS_PAGE_MASK) != 0U)) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }

    status = validate_queue_shape(scheduler.ready_head,
                                  scheduler.ready_tail);
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }
    status = validate_queue_shape(scheduler.exited_head,
                                  scheduler.exited_tail);
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }
    if (scheduler.ready_head != 0) {
        status = validate_thread(scheduler.ready_head,
                                 KERNEL_THREAD_STATE_READY);
        if (status != KERNEL_SCHEDULER_STATUS_OK) {
            return status;
        }
        status = validate_thread(scheduler.ready_tail,
                                 KERNEL_THREAD_STATE_READY);
        if (status != KERNEL_SCHEDULER_STATUS_OK) {
            return status;
        }
    }
    if (scheduler.exited_head != 0) {
        status = validate_thread(scheduler.exited_head,
                                 KERNEL_THREAD_STATE_EXITED);
        if (status != KERNEL_SCHEDULER_STATUS_OK) {
            return status;
        }
        status = validate_thread(scheduler.exited_tail,
                                 KERNEL_THREAD_STATE_EXITED);
        if (status != KERNEL_SCHEDULER_STATUS_OK) {
            return status;
        }
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}

static void ready_append(struct kernel_task *thread)
{
    thread->next = 0;
    if (scheduler.ready_tail == 0) {
        scheduler.ready_head = thread;
    } else {
        scheduler.ready_tail->next = thread;
    }
    scheduler.ready_tail = thread;
}

static struct kernel_task *ready_pop(void)
{
    struct kernel_task *thread = scheduler.ready_head;

    scheduler.ready_head = thread->next;
    if (scheduler.ready_head == 0) {
        scheduler.ready_tail = 0;
    }
    thread->next = 0;
    return thread;
}

static void exited_append(struct kernel_task *thread)
{
    thread->next = 0;
    if (scheduler.exited_tail == 0) {
        scheduler.exited_head = thread;
    } else {
        scheduler.exited_tail->next = thread;
    }
    scheduler.exited_tail = thread;
}

static void clear_page(void *pointer)
{
    volatile unsigned char *bytes = pointer;
    size_t index;

    for (index = 0U; index < BOAROS_PAGE_SIZE; index++) {
        bytes[index] = 0U;
    }
}

static enum kernel_scheduler_status release_after_create_failure(
    uint64_t physical_address,
    enum kernel_scheduler_status original_status)
{
    if (scheduler.cleanup_page_owned != 0U) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    if (physical_page_release(scheduler.allocator, physical_address) !=
        PHYSICAL_PAGE_STATUS_OK) {
        scheduler.cleanup_page_address = physical_address;
        scheduler.cleanup_page_owned = 1U;
        return KERNEL_SCHEDULER_STATUS_PAGE_RELEASE;
    }
    return original_status;
}

enum kernel_scheduler_status kernel_scheduler_init(
    struct physical_page_allocator *allocator,
    uintptr_t idle_stack_low,
    uintptr_t idle_stack_high)
{
    uintptr_t stack_pointer = current_sp();
    uint64_t kernel_satp = riscv_sv39_current_satp();
    enum kernel_pid_status pid_status;

    if (scheduler.initialized == KERNEL_SCHEDULER_INITIALIZED) {
        return KERNEL_SCHEDULER_STATUS_ALREADY_INITIALIZED;
    }
    if (allocator == 0 || idle_stack_low == 0U ||
        idle_stack_low >= idle_stack_high ||
        (idle_stack_low & (uintptr_t)15U) != 0U ||
        (idle_stack_high & (uintptr_t)15U) != 0U ||
        stack_pointer < idle_stack_low || stack_pointer >= idle_stack_high ||
        physical_page_total(allocator) == 0U ||
        physical_page_available(allocator) > physical_page_total(allocator)) {
        return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    }
    if (riscv_interrupt_is_enabled()) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    if (riscv_sv39_switch_satp(kernel_satp) != RISCV_SV39_STATUS_OK) {
        return KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE;
    }
    pid_status = kernel_pid_allocator_init(&scheduler.pid_allocator,
                                           scheduler.pid_bitmap,
                                           KERNEL_PID_LIMIT);
    if (pid_status != KERNEL_PID_STATUS_OK) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }

    scheduler.allocator = allocator;
    scheduler.kernel_satp = kernel_satp;
    scheduler.idle.arch.kernel_sp = idle_stack_high;
    scheduler.idle.arch.user_sp = 0U;
    scheduler.idle.arch.user_mode = 0U;
    scheduler.idle.arch.satp = scheduler.kernel_satp;
    scheduler.idle.magic = KERNEL_THREAD_MAGIC;
    scheduler.idle.physical_address = KERNEL_THREAD_NO_PAGE;
    scheduler.idle.stack_low = idle_stack_low;
    scheduler.idle.stack_high = idle_stack_high;
    scheduler.idle.next = 0;
    scheduler.idle.state = KERNEL_THREAD_STATE_IDLE;
    scheduler.idle.idle = 1U;
    scheduler.idle.tid = 0;
    scheduler.idle.tid_owned = 0U;
    scheduler.idle.group_leader = 0;
    scheduler.idle.group_members = 0U;
    scheduler.idle.completion.kind = KERNEL_THREAD_KIND_KERNEL;
    scheduler.idle.completion.reason = KERNEL_THREAD_EXIT_RETURNED;
    scheduler.idle.completion.tid = 0;
    scheduler.idle.completion.tgid = 0;
    scheduler.idle.completion.status = 0U;
    scheduler.idle.completion.detail = 0U;
    scheduler.current = &scheduler.idle;
    scheduler.ready_head = 0;
    scheduler.ready_tail = 0;
    scheduler.exited_head = 0;
    scheduler.exited_tail = 0;
    scheduler.cleanup_page_address = 0U;
    scheduler.cleanup_page_owned = 0U;
    scheduler.fatal_status = KERNEL_SCHEDULER_STATUS_OK;
    scheduler.idle_context_saved = 0U;
    scheduler.initialized = KERNEL_SCHEDULER_INITIALIZED;
    riscv_current_thread_set(&scheduler.idle);
    return KERNEL_SCHEDULER_STATUS_OK;
}

enum kernel_scheduler_status kernel_thread_create(
    void (*entry)(void *),
    void *argument)
{
    struct kernel_task *thread;
    uint64_t physical_address;
    void *page;
    uintptr_t old_status;
    uintptr_t stack_low;
    enum physical_page_status page_status;
    enum riscv_context_status context_status;
    enum kernel_scheduler_status status;

    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED) {
        return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (entry == 0) {
        return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    }

    old_status = riscv_interrupt_save();
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
    if (scheduler.cleanup_page_owned != 0U) {
        status = KERNEL_SCHEDULER_STATUS_PAGE_RELEASE;
        goto restore_interrupts;
    }

    page_status = physical_page_allocate(scheduler.allocator,
                                         &physical_address);
    if (page_status == PHYSICAL_PAGE_STATUS_EMPTY) {
        status = KERNEL_SCHEDULER_STATUS_NO_MEMORY;
        goto restore_interrupts;
    }
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        status = KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        goto restore_interrupts;
    }
    page_status = physical_page_resolve(scheduler.allocator,
                                        physical_address,
                                        &page);
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        status = release_after_create_failure(
            physical_address,
            KERNEL_SCHEDULER_STATUS_PAGE_ACCESS);
        goto restore_interrupts;
    }

    clear_page(page);
    thread = page;
    stack_low = align_up_16((uintptr_t)thread + sizeof(*thread) +
                            sizeof(uint64_t));
    thread->arch.kernel_sp = (uintptr_t)thread + BOAROS_PAGE_SIZE;
    thread->arch.user_sp = 0U;
    thread->arch.user_mode = 0U;
    thread->arch.satp = scheduler.kernel_satp;
    thread->magic = KERNEL_THREAD_MAGIC;
    thread->physical_address = physical_address;
    thread->stack_low = stack_low;
    thread->stack_high = (uintptr_t)thread + BOAROS_PAGE_SIZE;
    thread->next = 0;
    thread->state = KERNEL_THREAD_STATE_READY;
    thread->idle = 0U;
    thread->tid = 0;
    thread->tid_owned = 0U;
    thread->group_leader = 0;
    thread->group_members = 0U;
    thread->completion.kind = KERNEL_THREAD_KIND_KERNEL;
    thread->completion.reason = KERNEL_THREAD_EXIT_RETURNED;
    thread->completion.tid = 0;
    thread->completion.tgid = 0;
    thread->completion.status = 0U;
    thread->completion.detail = 0U;
    *(uint64_t *)(stack_low - sizeof(uint64_t)) = KERNEL_STACK_CANARY;
    context_status = riscv_context_init(&thread->context,
                                        thread->stack_high,
                                        entry,
                                        argument,
                                        thread);
    if (context_status != RISCV_CONTEXT_STATUS_OK) {
        status = release_after_create_failure(
            physical_address,
            KERNEL_SCHEDULER_STATUS_INVALID_STATE);
        goto restore_interrupts;
    }

    ready_append(thread);
    status = KERNEL_SCHEDULER_STATUS_OK;

restore_interrupts:
    riscv_interrupt_restore(old_status);
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
    struct riscv_trap_frame *frame;
    struct kernel_task *thread;
    uint64_t physical_address;
    uint64_t user_satp;
    void *page;
    uintptr_t old_status;
    uintptr_t stack_low;
    enum physical_page_status page_status;
    enum riscv_context_status context_status;
    enum kernel_pid_status pid_status;
    kernel_pid_t tid;
    enum kernel_mm_status mm_status;
    enum kernel_scheduler_status status;

    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED) {
        return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (mm == 0 || (files == 0) != (fs == 0) || entry == 0U ||
        (entry & (uintptr_t)1U) != 0U || stack_pointer == 0U ||
        (stack_pointer & (uintptr_t)15U) != 0U) {
        return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    }

    old_status = riscv_interrupt_save();
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
    if (scheduler.cleanup_page_owned != 0U) {
        status = KERNEL_SCHEDULER_STATUS_PAGE_RELEASE;
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
    mm_status = riscv_kernel_mm_satp(mm, &user_satp);
    if (mm_status != KERNEL_MM_STATUS_OK) {
        status = mm_status ==
                         KERNEL_MM_STATUS_INVALID_ARGUMENT
                     ? KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT
                     : KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE;
        goto restore_interrupts;
    }
    mm_status = kernel_mm_lookup(mm,
                                               entry,
                                               &entry_mapping);
    if (mm_status != KERNEL_MM_STATUS_OK) {
        status = mm_status == KERNEL_MM_STATUS_NOT_MAPPED ||
                         mm_status ==
                             KERNEL_MM_STATUS_INVALID_ARGUMENT
                     ? KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT
                     : KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE;
        goto restore_interrupts;
    }
    if ((entry_mapping.permissions &
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

    page_status = physical_page_allocate(scheduler.allocator,
                                         &physical_address);
    if (page_status == PHYSICAL_PAGE_STATUS_EMPTY) {
        status = KERNEL_SCHEDULER_STATUS_NO_MEMORY;
        goto restore_interrupts;
    }
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        status = KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        goto restore_interrupts;
    }
    page_status = physical_page_resolve(scheduler.allocator,
                                        physical_address,
                                        &page);
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        status = release_after_create_failure(
            physical_address,
            KERNEL_SCHEDULER_STATUS_PAGE_ACCESS);
        goto restore_interrupts;
    }

    clear_page(page);
    thread = page;
    stack_low = align_up_16((uintptr_t)thread + sizeof(*thread) +
                            sizeof(uint64_t));
    thread->arch.kernel_sp = (uintptr_t)thread + BOAROS_PAGE_SIZE;
    thread->arch.user_sp = 0U;
    thread->arch.user_mode = 1U;
    thread->arch.satp = user_satp;
    thread->magic = KERNEL_THREAD_MAGIC;
    thread->physical_address = physical_address;
    thread->stack_low = stack_low;
    thread->stack_high = (uintptr_t)thread + BOAROS_PAGE_SIZE;
    thread->next = 0;
    thread->state = KERNEL_THREAD_STATE_READY;
    thread->idle = 0U;
    thread->completion.kind = KERNEL_THREAD_KIND_USER;
    thread->completion.reason = KERNEL_THREAD_EXIT_SYSCALL;
    thread->completion.tid = 0;
    thread->completion.tgid = 0;
    thread->completion.status = 0U;
    thread->completion.detail = 0U;
    *(uint64_t *)(stack_low - sizeof(uint64_t)) = KERNEL_STACK_CANARY;

    frame = (struct riscv_trap_frame *)(thread->stack_high -
                                        sizeof(*frame));
    if ((uintptr_t)frame < stack_low) {
        status = release_after_create_failure(
            physical_address,
            KERNEL_SCHEDULER_STATUS_INVALID_STATE);
        goto restore_interrupts;
    }
    frame->sp = stack_pointer;
    frame->tp = thread_pointer;
    frame->sstatus = RISCV_SSTATUS_SPIE | RISCV_SSTATUS_UXL_64;
    frame->sepc = entry;
    frame->kernel_tp = (uintptr_t)thread;
    context_status = riscv_context_init_user(&thread->context,
                                             (uintptr_t)frame,
                                             thread);
    if (context_status != RISCV_CONTEXT_STATUS_OK) {
        status = release_after_create_failure(
            physical_address,
            KERNEL_SCHEDULER_STATUS_INVALID_STATE);
        goto restore_interrupts;
    }
    pid_status = kernel_pid_allocate(&scheduler.pid_allocator, &tid);
    if (pid_status != KERNEL_PID_STATUS_OK) {
        status = release_after_create_failure(
            physical_address,
            pid_status == KERNEL_PID_STATUS_EXHAUSTED
                ? KERNEL_SCHEDULER_STATUS_NO_MEMORY
                : KERNEL_SCHEDULER_STATUS_INVALID_STATE);
        goto restore_interrupts;
    }
    thread->tid = tid;
    thread->tid_owned = 1U;
    thread->group_leader = thread;
    thread->group_members = 1U;
    thread->completion.tid = tid;
    thread->completion.tgid = tid;
    thread->mm = *mm;
    mm->allocator = 0;
    mm->record_page_address = 0U;
    mm->state = KERNEL_MM_MOVED;
    mm->cleanup_stage = KERNEL_MM_CLEANUP_NONE;
    if (files != 0) {
        thread->files = *files;
        files->heap = 0;
        files->slots = 0;
        files->cleanup_files = 0;
        files->cleanup_allocations = 0;
        files->next_fd = 0U;
        files->state = KERNEL_FILES_MOVED;

        thread->fs = *fs;
        fs->heap = 0;
        fs->root_mount = 0;
        fs->cwd = 0;
        fs->state = KERNEL_FS_CONTEXT_MOVED;
    }

    ready_append(thread);
    status = KERNEL_SCHEDULER_STATUS_OK;

restore_interrupts:
    riscv_interrupt_restore(old_status);
    return status;
}

enum kernel_scheduler_status kernel_scheduler_on_tick(
    uint64_t elapsed_ticks)
{
    struct kernel_task *previous;
    struct kernel_task *next;
    enum kernel_scheduler_status status;

    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED) {
        return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (elapsed_ticks == 0U) {
        return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    }
    if (riscv_interrupt_is_enabled()) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    if (scheduler.fatal_status != KERNEL_SCHEDULER_STATUS_OK) {
        return scheduler.fatal_status;
    }
    status = validate_current();
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }
    status = validate_queues();
    if (status != KERNEL_SCHEDULER_STATUS_OK || scheduler.ready_head == 0) {
        return status;
    }

    previous = scheduler.current;
    next = scheduler.ready_head;
    status = activate_thread_address_space(next);
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }
    next = ready_pop();
    if (previous->idle == 0U) {
        previous->state = KERNEL_THREAD_STATE_READY;
        ready_append(previous);
    } else {
        scheduler.idle_context_saved = 1U;
    }
    next->state = KERNEL_THREAD_STATE_RUNNING;
    scheduler.current = next;
    riscv_context_switch(&previous->context, &next->context);

    if (scheduler.fatal_status != KERNEL_SCHEDULER_STATUS_OK) {
        return scheduler.fatal_status;
    }
    return validate_current();
}

enum kernel_scheduler_status kernel_scheduler_reap_one(
    struct kernel_thread_completion *completion)
{
    struct kernel_thread_completion result;
    struct kernel_task *thread;
    struct kernel_task *next;
    enum kernel_mm_status mm_status;
    enum kernel_files_status files_status;
    enum kernel_fs_context_status fs_status;
    enum kernel_scheduler_status status;

    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED) {
        return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (completion == 0) {
        return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    }
    if (riscv_interrupt_is_enabled()) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    if (scheduler.fatal_status != KERNEL_SCHEDULER_STATUS_OK) {
        return scheduler.fatal_status;
    }
    status = validate_current();
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }
    if (scheduler.current != &scheduler.idle) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    status = validate_queues();
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }

    if (scheduler.cleanup_page_owned != 0U) {
        if (physical_page_release(scheduler.allocator,
                                  scheduler.cleanup_page_address) !=
            PHYSICAL_PAGE_STATUS_OK) {
            return KERNEL_SCHEDULER_STATUS_PAGE_RELEASE;
        }
        scheduler.cleanup_page_address = 0U;
        scheduler.cleanup_page_owned = 0U;
    }

    if (scheduler.exited_head == 0) {
        return KERNEL_SCHEDULER_STATUS_EMPTY;
    }

    thread = scheduler.exited_head;
    next = thread->next;
    status = validate_thread(thread, KERNEL_THREAD_STATE_EXITED);
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }
    result = thread->completion;
    if (result.kind == KERNEL_THREAD_KIND_KERNEL) {
        if (result.reason != KERNEL_THREAD_EXIT_RETURNED ||
            result.tid != 0 || result.tgid != 0 ||
            result.status != 0U || result.detail != 0U) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
    } else if (result.kind == KERNEL_THREAD_KIND_USER) {
        if (result.reason != KERNEL_THREAD_EXIT_SYSCALL &&
            result.reason != KERNEL_THREAD_EXIT_USER_FAULT) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
        if ((thread->tid_owned != 0U &&
             (thread->group_leader == 0 ||
              result.tid != thread->tid ||
              result.tgid != thread->group_leader->tid)) ||
            (thread->tid_owned == 0U &&
             (result.tid <= 0 || result.tgid <= 0))) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
        if (thread->files.state == KERNEL_FILES_LIVE ||
            thread->files.state == KERNEL_FILES_CLEANUP) {
            files_status = kernel_files_release(&thread->files);
            if (files_status != KERNEL_FILES_STATUS_OK) {
                return files_status ==
                               KERNEL_FILES_STATUS_CLEANUP_REQUIRED
                           ? KERNEL_SCHEDULER_STATUS_RESOURCE_CLEANUP
                           : KERNEL_SCHEDULER_STATUS_INVALID_STATE;
            }
        }
        if (thread->files.state != KERNEL_FILES_EMPTY &&
            thread->files.state != KERNEL_FILES_RELEASED) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
        if (thread->fs.state == KERNEL_FS_CONTEXT_LIVE ||
            thread->fs.state == KERNEL_FS_CONTEXT_CLEANUP) {
            fs_status = kernel_fs_context_release(&thread->fs);
            if (fs_status != KERNEL_FS_CONTEXT_STATUS_OK) {
                return fs_status ==
                               KERNEL_FS_CONTEXT_STATUS_CLEANUP_REQUIRED
                           ? KERNEL_SCHEDULER_STATUS_RESOURCE_CLEANUP
                           : KERNEL_SCHEDULER_STATUS_INVALID_STATE;
            }
        }
        if (thread->fs.state != KERNEL_FS_CONTEXT_EMPTY &&
            thread->fs.state != KERNEL_FS_CONTEXT_RELEASED) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
        if (thread->mm.state == KERNEL_MM_LIVE ||
            thread->mm.state == KERNEL_MM_CLEANUP) {
            mm_status = kernel_mm_release(
                &thread->mm);
            if (mm_status != KERNEL_MM_STATUS_OK) {
                if (mm_status ==
                    KERNEL_MM_STATUS_PAGE_ACCESS) {
                    return KERNEL_SCHEDULER_STATUS_PAGE_ACCESS;
                }
                if (mm_status ==
                        KERNEL_MM_STATUS_PAGE_RELEASE ||
                    mm_status ==
                        KERNEL_MM_STATUS_CLEANUP_REQUIRED) {
                    return KERNEL_SCHEDULER_STATUS_PAGE_RELEASE;
                }
                return mm_status ==
                               KERNEL_MM_STATUS_ADDRESS_SPACE
                           ? KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE
                           : KERNEL_SCHEDULER_STATUS_INVALID_STATE;
            }
        }
        if (thread->mm.state != KERNEL_MM_RELEASED) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
        if (thread->tid_owned != 0U) {
            if (kernel_pid_release(&scheduler.pid_allocator,
                                   thread->tid) !=
                KERNEL_PID_STATUS_OK) {
                return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
            }
            thread->tid = 0;
            thread->tid_owned = 0U;
            thread->group_leader = 0;
            thread->group_members = 0U;
        }
    } else {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    if (physical_page_release(scheduler.allocator,
                              thread->physical_address) !=
        PHYSICAL_PAGE_STATUS_OK) {
        return KERNEL_SCHEDULER_STATUS_PAGE_RELEASE;
    }
    scheduler.exited_head = next;
    if (next == 0) {
        scheduler.exited_tail = 0;
    }
    *completion = result;
    return KERNEL_SCHEDULER_STATUS_OK;
}

static void switch_to_fatal_idle(
    enum kernel_scheduler_status status) __attribute__((noreturn));

static void switch_to_fatal_idle(enum kernel_scheduler_status status)
{
    if (scheduler.fatal_status == KERNEL_SCHEDULER_STATUS_OK) {
        scheduler.fatal_status = status;
    }
    if (scheduler.idle_context_saved != 0U &&
        scheduler.current != &scheduler.idle) {
        if (activate_thread_address_space(&scheduler.idle) !=
            KERNEL_SCHEDULER_STATUS_OK) {
            for (;;) {
                __asm__ volatile("wfi");
            }
        }
        scheduler.current = &scheduler.idle;
        riscv_context_switch(&scheduler.discard_context,
                             &scheduler.idle.context);
    }

    for (;;) {
        __asm__ volatile("wfi");
    }
}

static void kernel_thread_finish(
    const struct kernel_thread_completion *completion)
    __attribute__((noreturn));

static void kernel_thread_finish(
    const struct kernel_thread_completion *completion)
{
    struct kernel_task *current;
    struct kernel_task *next;
    enum kernel_scheduler_status status;

    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED ||
        riscv_interrupt_is_enabled()) {
        switch_to_fatal_idle(KERNEL_SCHEDULER_STATUS_INVALID_STATE);
    }
    current = scheduler.current;
    status = validate_current();
    if (status != KERNEL_SCHEDULER_STATUS_OK || current == &scheduler.idle) {
        switch_to_fatal_idle(
            status == KERNEL_SCHEDULER_STATUS_OK
                ? KERNEL_SCHEDULER_STATUS_INVALID_STATE
                : status);
    }
    status = validate_queues();
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        switch_to_fatal_idle(status);
    }

    if (scheduler.ready_head == 0) {
        next = &scheduler.idle;
    } else {
        next = scheduler.ready_head;
    }
    status = activate_thread_address_space(next);
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        switch_to_fatal_idle(status);
    }

    current->completion = *completion;
    current->state = KERNEL_THREAD_STATE_EXITED;
    exited_append(current);
    if (next != &scheduler.idle) {
        next = ready_pop();
        next->state = KERNEL_THREAD_STATE_RUNNING;
    }
    scheduler.current = next;
    riscv_context_switch(&scheduler.discard_context, &next->context);

    switch_to_fatal_idle(KERNEL_SCHEDULER_STATUS_INVALID_STATE);
}

void kernel_thread_exit(void)
{
    const struct kernel_thread_completion completion = {
        .kind = KERNEL_THREAD_KIND_KERNEL,
        .reason = KERNEL_THREAD_EXIT_RETURNED,
        .tid = 0,
        .tgid = 0,
        .status = 0U,
        .detail = 0U,
    };

    kernel_thread_finish(&completion);
}

void kernel_user_thread_exit(
    enum kernel_thread_exit_reason reason,
    uint64_t status,
    uint64_t detail)
{
    struct kernel_thread_completion completion = {
        .kind = KERNEL_THREAD_KIND_USER,
        .reason = reason,
        .tid = 0,
        .tgid = 0,
        .status = status,
        .detail = detail,
    };

    if (reason != KERNEL_THREAD_EXIT_SYSCALL &&
        reason != KERNEL_THREAD_EXIT_USER_FAULT) {
        switch_to_fatal_idle(KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT);
    }
    if (scheduler.current == 0 ||
        scheduler.current->arch.user_mode != 1U) {
        switch_to_fatal_idle(KERNEL_SCHEDULER_STATUS_INVALID_STATE);
    }
    completion.tid = scheduler.current->tid;
    completion.tgid = scheduler.current->group_leader->tid;
    kernel_thread_finish(&completion);
}

struct kernel_task *kernel_task_current(void)
{
    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED ||
        scheduler.current == 0 ||
        riscv_current_thread_get() != scheduler.current) {
        return 0;
    }
    return scheduler.current;
}

enum kernel_task_status kernel_task_tid(
    const struct kernel_task *task,
    kernel_pid_t *tid)
{
    if (task == 0 || tid == 0) {
        return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    }
    if (task->magic != KERNEL_THREAD_MAGIC || task->tid_owned != 1U ||
        task->tid <= 0) {
        return KERNEL_TASK_STATUS_STATE;
    }
    *tid = task->tid;
    return KERNEL_TASK_STATUS_OK;
}

enum kernel_task_status kernel_task_tgid(
    const struct kernel_task *task,
    kernel_pid_t *tgid)
{
    const struct kernel_task *leader;

    if (task == 0 || tgid == 0) {
        return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    }
    leader = task->group_leader;
    if (task->magic != KERNEL_THREAD_MAGIC || task->tid_owned != 1U ||
        leader == 0 || leader->magic != KERNEL_THREAD_MAGIC ||
        leader->tid_owned != 1U || leader->tid <= 0 ||
        leader->group_leader != leader || leader->group_members == 0U) {
        return KERNEL_TASK_STATUS_STATE;
    }
    *tgid = leader->tid;
    return KERNEL_TASK_STATUS_OK;
}

enum kernel_task_status kernel_task_mm_borrow(
    const struct kernel_task *task,
    const struct kernel_mm **mm)
{
    if (task == 0 || mm == 0) {
        return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    }
    if (task != scheduler.current ||
        task->magic != KERNEL_THREAD_MAGIC ||
        task->state != KERNEL_THREAD_STATE_RUNNING ||
        task->arch.user_mode != 1U ||
        task->mm.state != KERNEL_MM_LIVE) {
        return KERNEL_TASK_STATUS_STATE;
    }
    *mm = &task->mm;
    return KERNEL_TASK_STATUS_OK;
}

static enum kernel_task_status validate_task_resource_borrow(
    const struct kernel_task *task)
{
    if (task != scheduler.current ||
        task->magic != KERNEL_THREAD_MAGIC ||
        task->state != KERNEL_THREAD_STATE_RUNNING ||
        task->arch.user_mode != 1U) {
        return KERNEL_TASK_STATUS_STATE;
    }
    if (task->files.state == KERNEL_FILES_EMPTY &&
        task->fs.state == KERNEL_FS_CONTEXT_EMPTY) {
        return KERNEL_TASK_STATUS_RESOURCE_UNAVAILABLE;
    }
    if (!kernel_files_is_live(&task->files) ||
        !kernel_fs_context_is_live(&task->fs)) {
        return KERNEL_TASK_STATUS_STATE;
    }
    return KERNEL_TASK_STATUS_OK;
}

enum kernel_task_status kernel_task_files_borrow(
    struct kernel_task *task,
    struct kernel_files **files)
{
    enum kernel_task_status status;

    if (task == 0 || files == 0) {
        return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    }
    status = validate_task_resource_borrow(task);
    if (status != KERNEL_TASK_STATUS_OK) {
        return status;
    }
    *files = &task->files;
    return KERNEL_TASK_STATUS_OK;
}

enum kernel_task_status kernel_task_fs_context_borrow(
    const struct kernel_task *task,
    const struct kernel_fs_context **fs)
{
    enum kernel_task_status status;

    if (task == 0 || fs == 0) {
        return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    }
    status = validate_task_resource_borrow(task);
    if (status != KERNEL_TASK_STATUS_OK) {
        return status;
    }
    *fs = &task->fs;
    return KERNEL_TASK_STATUS_OK;
}
