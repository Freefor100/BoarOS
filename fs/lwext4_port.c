#include "lwext4_port.h"

#include <kernel/heap.h>
#include <kernel/sync.h>
#include <arch/context.h>
#include <ext4.h>

#include <stddef.h>

static struct kernel_heap *lwext4_heap;
static unsigned heap_users;
static struct kernel_wait_queue channels[64];
static void backend_enter(void *lock, int read)
{
    struct kernel_io_context *context = kernel_io_context_current();
    if (context->backend_depth) {
        if (context->backend_guard.lock != lock || (!read && context->backend_read)) __builtin_trap();
    } else {
        if (read) kernel_rwlock_read(lock, &context->backend_guard);
        else kernel_rwlock_write(lock, &context->backend_guard);
        context->backend_read = read;
    }
    context->backend_depth++;
}
static void backend_read(void *lock) { backend_enter(lock, 1); }
static void backend_write(void *lock) { backend_enter(lock, 0); }
static void backend_leave(void *lock)
{
    struct kernel_io_context *context = kernel_io_context_current();
    if (!context->backend_depth || context->backend_guard.lock != lock) __builtin_trap();
    if (!--context->backend_depth) kernel_lock_release(&context->backend_guard);
}
static uintptr_t backend_owner(void *lock) { (void)lock; return (uintptr_t)kernel_io_context_current(); }
unsigned boaros_lwext4_pause(void)
{
    struct kernel_io_context *context = kernel_io_context_current();
    unsigned depth = context->backend_depth;
    if (!depth || context->backend_read) __builtin_trap();
    context->backend_depth = 0;
    kernel_lock_release(&context->backend_guard);
    return depth;
}
void boaros_lwext4_resume(void *lock, unsigned depth)
{
    struct kernel_io_context *context = kernel_io_context_current();
    if (!depth || context->backend_depth) __builtin_trap();
    backend_write(lock);
    context->backend_depth = depth;
}
void boaros_lwext4_lock_init(struct ext4_lock *callbacks, struct kernel_rwlock *lock)
{
    callbacks->lock = backend_write;
    callbacks->unlock = backend_leave;
    callbacks->read_lock = backend_read;
    callbacks->owner = backend_owner;
    callbacks->context = lock;
    kernel_rwlock_init(lock, 40, (uintptr_t)lock);
}
int boaros_lwext4_read_context(void)
{
    struct kernel_io_context *context = kernel_io_context_current();
    return context->backend_depth && context->backend_read;
}
static struct kernel_wait_queue *channel(void *key)
{ return &channels[((uintptr_t)key >> 4) % 64]; }
void boaros_lwext4_wait(void *key)
{
    uintptr_t irq = arch_interrupt_save();
    enum kernel_wait_wake_reason reason;
    if (KERNEL_WAIT_RECHECK(channel(key), 0, 0, &reason,
                (1)) != KERNEL_SCHEDULER_STATUS_OK)
        __builtin_trap();
    arch_interrupt_restore(irq);
}
void boaros_lwext4_wake(void *key)
{
    uintptr_t irq = arch_interrupt_save();
    struct kernel_wait_queue *queue = channel(key);
    if (queue->head && kernel_wait_queue_wake_all(queue) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
    arch_interrupt_restore(irq);
}

int boaros_lwext4_allocation_active(void)
{
    return kernel_io_context_current()->allocation_depth != 0U ||
           kernel_io_context_current()->backend_depth != 0U;
}

int boaros_lwext4_heap_bind(struct kernel_heap *heap)
{
    if (heap == 0 || (lwext4_heap != 0 && lwext4_heap != heap)) {
        return 0;
    }

    if (!lwext4_heap) {
        for (unsigned i = 0; i < 64; i++) kernel_wait_queue_init(&channels[i]);
    }
    if (heap_users == UINT32_MAX) __builtin_trap();
    heap_users++;
    lwext4_heap = heap;
    return 1;
}

void boaros_lwext4_heap_unbind(struct kernel_heap *heap)
{
    if (lwext4_heap != heap || !heap_users) __builtin_trap();
    if (!--heap_users) lwext4_heap = 0;
}

void *ext4_user_malloc(size_t size)
{
    void *pointer = 0;
    if (lwext4_heap == 0) return 0;
    kernel_io_context_current()->allocation_depth++;
    enum kernel_heap_status status = kernel_heap_allocate(lwext4_heap, size, &pointer);
    kernel_io_context_current()->allocation_depth--;
    return status == KERNEL_HEAP_STATUS_OK ? pointer : 0;
}

void *ext4_user_calloc(size_t count, size_t size)
{
    void *pointer = 0;

    if (lwext4_heap == 0) return 0;
    kernel_io_context_current()->allocation_depth++;
    enum kernel_heap_status status = kernel_heap_allocate_zeroed(lwext4_heap,
                                    count,
                                    size,
                                    &pointer);
    kernel_io_context_current()->allocation_depth--;
    return status == KERNEL_HEAP_STATUS_OK ? pointer : 0;
}

void *ext4_user_realloc(void *pointer, size_t size)
{
    void *result = 0;

    if (lwext4_heap == 0) return 0;
    kernel_io_context_current()->allocation_depth++;
    enum kernel_heap_status status = kernel_heap_resize(lwext4_heap, pointer, size, &result);
    kernel_io_context_current()->allocation_depth--;
    return status == KERNEL_HEAP_STATUS_OK ? result : 0;
}

void ext4_user_free(void *pointer)
{
    if (lwext4_heap != 0) {
        (void)kernel_heap_release(lwext4_heap, pointer);
    }
}
