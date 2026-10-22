#ifndef BOAROS_SOCKET_STATE_H
#define BOAROS_SOCKET_STATE_H
#include <kernel/raw_lock.h>
#include <kernel/wait_internal.h>

static struct kernel_raw_lock socket_state_lock = {
    .rank=KERNEL_RAW_RANK_OBJECT, .initialized=KERNEL_RAW_INITIALIZED
};
struct socket_state_guard {
    kernel_irq_lock_guard raw;
    struct socket_state_guard *owner;
    struct kernel_wait_cursor notifications[2];
    unsigned count;
    void (*work)(void *);
    void *context;
};
#if BOAROS_MAX_CPUS == 1 && !BOAROS_LOCK_DEBUG && !BOAROS_COST_DIAGNOSTICS
/* UP 的嵌套 helper 共用 IRQ 保护内的栈 guard，沿用轻量 task/IRQ 原语。 */
static struct socket_state_guard *socket_state_active;
#endif
static void socket_state_acquire(struct socket_state_guard *guard)
{
    if(guard->owner)__builtin_trap();
#if BOAROS_MAX_CPUS == 1 && !BOAROS_LOCK_DEBUG && !BOAROS_COST_DIAGNOSTICS
    uintptr_t interrupts=arch_interrupt_save();
    if(socket_state_active) {guard->owner=socket_state_active;return;}
    kernel_task_lock_acquire(&socket_state_lock,&guard->raw.task);
    guard->raw.interrupts=interrupts;socket_state_active=guard;
#else
    struct kernel_raw_guard *held=kernel_cpu_current()->raw_locks;
    if(held && held->lock==&socket_state_lock) {guard->owner=(void *)held;return;}
    kernel_raw_lock_acquire(&socket_state_lock,&guard->raw);
#endif
    guard->owner=guard;
    guard->count=0;guard->work=0;
}
static void socket_state_release(struct socket_state_guard *guard)
{
    if(!guard->owner)return;
    int outer=guard->owner==guard;guard->owner=0;
    if(!outer)return;
#if BOAROS_MAX_CPUS == 1 && !BOAROS_LOCK_DEBUG && !BOAROS_COST_DIAGNOSTICS
    socket_state_active=0;
#endif
    if(!guard->count && !guard->work) {kernel_irq_lock_release(&guard->raw);return;}
    uintptr_t interrupts=guard->raw.interrupts;guard->raw.interrupts=0;
    kernel_irq_lock_release(&guard->raw);
    /* Queue cursors retain their actual storage; callbacks run without state locks. */
    for(unsigned i=0;i<guard->count;i++)kernel_wait_cursor_wake(&guard->notifications[i],1);
    if(guard->work)guard->work(guard->context);
    arch_interrupt_restore(interrupts);
}
static uintptr_t socket_state_unlock_wait(struct socket_state_guard *guard)
{
    if(guard->owner!=guard)__builtin_trap();
    uintptr_t interrupts=guard->raw.interrupts;guard->raw.interrupts=0;
    socket_state_release(guard);return interrupts;
}
#define SOCKET_STATE(name) \
    struct socket_state_guard name __attribute__((cleanup(socket_state_release)))={0}; \
    socket_state_acquire(&name)
static struct socket_state_guard *socket_state_owner(void)
{
#if BOAROS_MAX_CPUS == 1 && !BOAROS_LOCK_DEBUG && !BOAROS_COST_DIAGNOSTICS
    if(!socket_state_active)__builtin_trap();
    return socket_state_active;
#else
    struct kernel_raw_guard *held=kernel_cpu_current()->raw_locks;
    if(!held || held->lock!=&socket_state_lock)__builtin_trap();
    return (struct socket_state_guard *)held;
#endif
}
static void socket_state_notify(struct kernel_wait_queue *queue)
{
    struct socket_state_guard *guard=socket_state_owner();
    for(unsigned i=0;i<guard->count;i++)if(guard->notifications[i].queue==queue)return;
    if(guard->count==2)__builtin_trap();
    if(kernel_wait_cursor_prepare(queue,&guard->notifications[guard->count]))guard->count++;
}
#endif
