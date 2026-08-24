#include <arch/riscv/context.h>
#include <arch/riscv/thread.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/scheduler.h>

#include <stddef.h>
#include <stdint.h>

#define KERNEL_SCHEDULER_INITIALIZED UINT32_C(0x53434844)
#define KERNEL_THREAD_MAGIC UINT64_C(0x424f415254485244)
#define KERNEL_STACK_CANARY UINT64_C(0x535441434b4f4b21)
#define KERNEL_THREAD_NO_PAGE UINT64_MAX
#define KERNEL_THREAD_MINIMUM_STACK 512U

enum kernel_thread_state {
    KERNEL_THREAD_STATE_IDLE = 0,
    KERNEL_THREAD_STATE_READY,
    KERNEL_THREAD_STATE_RUNNING,
    KERNEL_THREAD_STATE_EXITED,
};

struct kernel_thread {
    struct riscv_thread_state arch;
    uint64_t magic;
    uint64_t physical_address;
    uintptr_t stack_low;
    uintptr_t stack_high;
    struct kernel_thread *next;
    uint32_t state;
    uint32_t idle;
    struct kernel_thread_completion completion;
    struct riscv_switch_context context;
} __attribute__((aligned(16)));

struct kernel_scheduler {
    uint32_t initialized;
    uint32_t idle_context_saved;
    struct physical_page_allocator *allocator;
    struct kernel_thread idle;
    struct kernel_thread *current;
    struct kernel_thread *ready_head;
    struct kernel_thread *ready_tail;
    struct kernel_thread *exited_head;
    struct kernel_thread *exited_tail;
    enum kernel_scheduler_status fatal_status;
    struct riscv_switch_context discard_context;
};

static struct kernel_scheduler scheduler;

_Static_assert(sizeof(struct kernel_thread) + sizeof(uint64_t) +
                       KERNEL_THREAD_MINIMUM_STACK <=
                   BOAROS_PAGE_SIZE,
               "kernel thread metadata leaves too little stack");
_Static_assert(offsetof(struct kernel_thread, arch) == 0U,
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
    const struct kernel_thread *head,
    const struct kernel_thread *tail)
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
    const struct kernel_thread *thread,
    enum kernel_thread_state expected_state)
{
    uintptr_t base;
    uintptr_t expected_low;
    const uint64_t *canary;

    if (thread == 0 || thread->magic != KERNEL_THREAD_MAGIC ||
        thread->state != (uint32_t)expected_state) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    if (thread->idle != 0U) {
        if (thread != &scheduler.idle ||
            expected_state != KERNEL_THREAD_STATE_IDLE ||
            thread->physical_address != KERNEL_THREAD_NO_PAGE ||
            thread->stack_low >= thread->stack_high ||
            thread->arch.kernel_sp != thread->stack_high) {
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
    return KERNEL_SCHEDULER_STATUS_OK;
}

static enum kernel_scheduler_status validate_queues(void)
{
    enum kernel_scheduler_status status;

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

static void ready_append(struct kernel_thread *thread)
{
    thread->next = 0;
    if (scheduler.ready_tail == 0) {
        scheduler.ready_head = thread;
    } else {
        scheduler.ready_tail->next = thread;
    }
    scheduler.ready_tail = thread;
}

static struct kernel_thread *ready_pop(void)
{
    struct kernel_thread *thread = scheduler.ready_head;

    scheduler.ready_head = thread->next;
    if (scheduler.ready_head == 0) {
        scheduler.ready_tail = 0;
    }
    thread->next = 0;
    return thread;
}

static void exited_append(struct kernel_thread *thread)
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
    if (physical_page_release(scheduler.allocator, physical_address) !=
        PHYSICAL_PAGE_STATUS_OK) {
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

    scheduler.allocator = allocator;
    scheduler.idle.arch.kernel_sp = idle_stack_high;
    scheduler.idle.magic = KERNEL_THREAD_MAGIC;
    scheduler.idle.physical_address = KERNEL_THREAD_NO_PAGE;
    scheduler.idle.stack_low = idle_stack_low;
    scheduler.idle.stack_high = idle_stack_high;
    scheduler.idle.next = 0;
    scheduler.idle.state = KERNEL_THREAD_STATE_IDLE;
    scheduler.idle.idle = 1U;
    scheduler.idle.completion.kind = KERNEL_THREAD_KIND_KERNEL;
    scheduler.idle.completion.reason = KERNEL_THREAD_EXIT_RETURNED;
    scheduler.idle.completion.status = 0U;
    scheduler.idle.completion.detail = 0U;
    scheduler.current = &scheduler.idle;
    scheduler.ready_head = 0;
    scheduler.ready_tail = 0;
    scheduler.exited_head = 0;
    scheduler.exited_tail = 0;
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
    struct kernel_thread *thread;
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
    thread->magic = KERNEL_THREAD_MAGIC;
    thread->physical_address = physical_address;
    thread->stack_low = stack_low;
    thread->stack_high = (uintptr_t)thread + BOAROS_PAGE_SIZE;
    thread->next = 0;
    thread->state = KERNEL_THREAD_STATE_READY;
    thread->idle = 0U;
    thread->completion.kind = KERNEL_THREAD_KIND_KERNEL;
    thread->completion.reason = KERNEL_THREAD_EXIT_RETURNED;
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

enum kernel_scheduler_status kernel_scheduler_on_tick(
    uint64_t elapsed_ticks)
{
    struct kernel_thread *previous;
    struct kernel_thread *next;
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
    struct kernel_thread *thread;
    struct kernel_thread *next;
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
    if (result.kind != KERNEL_THREAD_KIND_KERNEL ||
        result.reason != KERNEL_THREAD_EXIT_RETURNED ||
        result.status != 0U || result.detail != 0U) {
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
        scheduler.current = &scheduler.idle;
        riscv_context_switch(&scheduler.discard_context,
                             &scheduler.idle.context);
    }

    for (;;) {
        __asm__ volatile("wfi");
    }
}

void kernel_thread_exit(void)
{
    struct kernel_thread *current;
    struct kernel_thread *next;
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

    current->state = KERNEL_THREAD_STATE_EXITED;
    exited_append(current);
    if (scheduler.ready_head == 0) {
        next = &scheduler.idle;
    } else {
        next = ready_pop();
        next->state = KERNEL_THREAD_STATE_RUNNING;
    }
    scheduler.current = next;
    riscv_context_switch(&scheduler.discard_context, &next->context);

    switch_to_fatal_idle(KERNEL_SCHEDULER_STATUS_INVALID_STATE);
}
