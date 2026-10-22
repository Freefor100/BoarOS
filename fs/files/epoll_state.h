#ifndef BOAROS_EPOLL_STATE_H
#define BOAROS_EPOLL_STATE_H
#include <kernel/raw_lock.h>
#include <kernel/wait_internal.h>
static struct kernel_raw_lock epoll_state_lock={.rank=KERNEL_RAW_RANK_OBJECT,.initialized=KERNEL_RAW_INITIALIZED};
struct epoll_state_guard {
    kernel_irq_lock_guard raw;
    struct epoll_state_guard *owner;
    struct kernel_wait_cursor notification;
};
#if BOAROS_MAX_CPUS == 1 && !BOAROS_LOCK_DEBUG && !BOAROS_COST_DIAGNOSTICS
/* UP 的嵌套 helper 共用 IRQ 保护内的栈 guard，沿用轻量 task/IRQ 原语。 */
static struct epoll_state_guard *epoll_state_active;
#endif
static void epoll_state_acquire(struct epoll_state_guard *guard)
{
    if(guard->owner)__builtin_trap();
#if BOAROS_MAX_CPUS == 1 && !BOAROS_LOCK_DEBUG && !BOAROS_COST_DIAGNOSTICS
    uintptr_t interrupts=arch_interrupt_save();
    if(epoll_state_active) {guard->owner=epoll_state_active;return;}
    kernel_task_lock_acquire(&epoll_state_lock,&guard->raw.task);
    guard->raw.interrupts=interrupts;epoll_state_active=guard;
#else
    struct kernel_raw_guard *held=kernel_cpu_current()->raw_locks;
    if(held && held->lock==&epoll_state_lock) {guard->owner=(void *)held;return;}
    kernel_raw_lock_acquire(&epoll_state_lock,&guard->raw);
#endif
    guard->owner=guard;
}
static void epoll_state_release(struct epoll_state_guard *guard)
{
    if(!guard->owner)return;
    int outer=guard->owner==guard;guard->owner=0;if(!outer)return;
#if BOAROS_MAX_CPUS == 1 && !BOAROS_LOCK_DEBUG && !BOAROS_COST_DIAGNOSTICS
    epoll_state_active=0;
#endif
    if(!guard->notification.queue){kernel_irq_lock_release(&guard->raw);return;}
    uintptr_t interrupts=guard->raw.interrupts;guard->raw.interrupts=0;
    kernel_irq_lock_release(&guard->raw);
    kernel_wait_cursor_wake(&guard->notification,1);
    arch_interrupt_restore(interrupts);
}
#define EPOLL_STATE(name) \
    struct epoll_state_guard name __attribute__((cleanup(epoll_state_release)))={0}; \
    epoll_state_acquire(&name)
static void epoll_state_notify(struct kernel_wait_queue *queue)
{
#if BOAROS_MAX_CPUS == 1 && !BOAROS_LOCK_DEBUG && !BOAROS_COST_DIAGNOSTICS
    struct epoll_state_guard *owner=epoll_state_active;
    if(!owner)__builtin_trap();
#else
    struct kernel_raw_guard *held=kernel_cpu_current()->raw_locks;
    if(!held || held->lock!=&epoll_state_lock)__builtin_trap();
    struct epoll_state_guard *owner=(void *)held;
#endif
    if(owner->notification.queue) {
        if(owner->notification.queue!=queue)__builtin_trap();
        return;
    }
    (void)kernel_wait_cursor_prepare(queue,&owner->notification);
}
static uintptr_t epoll_state_unlock_wait(struct epoll_state_guard *guard)
{
    if(guard->owner!=guard)__builtin_trap();
    uintptr_t interrupts=guard->raw.interrupts;guard->raw.interrupts=0;
    epoll_state_release(guard);return interrupts;
}
#endif
