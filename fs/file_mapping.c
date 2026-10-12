#include "vfs_objects.h"
#include <kernel/file_mapping.h>
#include <kernel/errno.h>
#include <arch/context.h>

/* inode 独占状态，仅在首次映射/来源准备时创建；现有 inode owner
 * 覆盖其寿命，最终 inode 释放才销毁，不增加第二层引用。 */
struct kernel_file_mapping_state {
    struct kernel_raw_lock metadata;
    struct kernel_wait_queue changed;
    struct kernel_file_mapping *mappings;
    uint64_t epoch;
    uint32_t borrows;
    uint8_t closing;
};

static struct kernel_file_mapping_state *state(struct kernel_vfs_node *node)
{ return __atomic_load_n(&node->mapping_state, __ATOMIC_ACQUIRE); }

void kernel_file_mapping_init(struct kernel_vfs_node *node)
{ node->mapping_state = 0; }

int kernel_file_mapping_prepare(struct kernel_vfs_node *node)
{
    if (!node || !node->references || !node->instance) return -KERNEL_EINVAL;
    if (state(node)) return 0;
    KERNEL_LOCK_SCOPE(io);
    kernel_vfs_node_lock(node, &io, 0);
    if (state(node)) return 0;
    struct kernel_file_mapping_state *candidate;
    enum kernel_heap_status status = kernel_heap_allocate_zeroed(
        node->instance->heap, 1, sizeof(*candidate), (void **)&candidate);
    if (status != KERNEL_HEAP_STATUS_OK)
        return status == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    kernel_raw_lock_init(&candidate->metadata, KERNEL_RAW_RANK_OBJECT);
    kernel_wait_queue_init(&candidate->changed);
    candidate->epoch = 1;
    /* I/O 读保护排除无状态截断；已有 I/O metadata 仅用于一次发布，
     * 不能在 raw 内构造候选或释放并发输掉的候选。 */
    {
        KERNEL_RAW_SCOPE(publish, &node->io_lock.metadata);
        if (!state(node)) {
            __atomic_store_n(&node->mapping_state, candidate, __ATOMIC_RELEASE);
            candidate = 0;
        }
    }
    if (candidate) kernel_heap_release(node->instance->heap, candidate);
    return 0;
}

int kernel_file_mapping_empty(struct kernel_vfs_node *node)
{
    struct kernel_file_mapping_state *s = state(node);
    if (!s) return 1;
    KERNEL_RAW_SCOPE(guard, &s->metadata);
    return !s->mappings && !s->borrows;
}

void kernel_file_mapping_destroy(struct kernel_vfs_node *node)
{
    struct kernel_file_mapping_state *s = state(node);
    if (!s) return;
    if (!kernel_file_mapping_empty(node) || s->closing ||
        s->changed.registrations || s->changed.borrows) __builtin_trap();
    node->mapping_state = 0;
    kernel_heap_release(node->instance->heap, s);
}

static void borrow(struct kernel_file_mapping_state *s, struct kernel_file_mapping *mapping)
{
    if (!mapping) return;
    if (mapping->node->mapping_state != s || mapping->borrows == UINT32_MAX || s->borrows == UINT32_MAX)
        __builtin_trap();
    mapping->borrows++;
    s->borrows++;
}

static void changed(struct kernel_file_mapping_state *s)
{
    uintptr_t irq = arch_interrupt_save();
    enum kernel_scheduler_status status = kernel_wait_queue_wake_all(&s->changed);
    /* 非调度fixture/bootstrap还没有等待者；不能为其伪造scheduler就绪。
     * 已有登记或借用时必须真的完成通知。 */
    if (status != KERNEL_SCHEDULER_STATUS_OK &&
        !(status == KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED &&
          !s->changed.registrations && !s->changed.borrows))
        __builtin_trap();
    arch_interrupt_restore(irq);
}

/* Retired successor references can form a long chain. Drop at most 16 per
 * raw segment; the returned continuation itself still holds a real borrow. */
static struct kernel_file_mapping *put_segment(struct kernel_file_mapping_state *s, struct kernel_file_mapping *m)
{
    for (unsigned count = 0; m && count < 16; count++) {
        if (m->node->mapping_state != s || !m->borrows || !s->borrows) __builtin_trap();
        s->borrows--;
        if (--m->borrows || m->previous) return 0;
        struct kernel_file_mapping *next = m->retired_next;
        m->retired_next = 0;
        m->next = 0;
        m = next;
    }
    return m;
}

static void put(struct kernel_file_mapping_state *s, struct kernel_file_mapping *m)
{
    do {
        { KERNEL_RAW_SCOPE(guard, &s->metadata); m = put_segment(s, m); }
        changed(s);
    } while (m);
}

int kernel_file_mapping_register(struct kernel_file_mapping *mapping)
{
    if (!mapping || !mapping->node || !mapping->owner ||
        !mapping->truncate || mapping->previous || mapping->borrows || mapping->retired_next)
        __builtin_trap();
    struct kernel_file_mapping_state *s = state(mapping->node);
    if (!s) __builtin_trap();
    KERNEL_RAW_SCOPE(guard, &s->metadata);
    if (s->closing) return -KERNEL_EBUSY;
    mapping->next = s->mappings;
    mapping->previous = &s->mappings;
    if (mapping->next) mapping->next->previous = &mapping->next;
    s->mappings = mapping;
    return 0;
}

void kernel_file_mapping_stop(struct kernel_file_mapping *mapping)
{
    struct kernel_file_mapping_state *s = state(mapping->node);
    KERNEL_RAW_SCOPE(guard, &s->metadata);
    if (!mapping->previous) return;
    if (*mapping->previous != mapping || mapping->retired_next ||
        (mapping->next && (mapping->next->node != mapping->node ||
                          mapping->next->previous != &mapping->next))) __builtin_trap();
    *mapping->previous = mapping->next;
    if (mapping->next) mapping->next->previous = mapping->previous;
    mapping->previous = 0;
    if (mapping->borrows) {
        mapping->retired_next = mapping->next;
        borrow(s, mapping->retired_next);
    } else mapping->next = 0;
}

void kernel_file_mapping_drain(struct kernel_file_mapping *mapping)
{
    struct kernel_vfs_node *node = mapping->node;
    struct kernel_file_mapping_state *s = state(node);
    if (kernel_lock_held(&node->io_lock, 0) || kernel_cpu_current()->raw_locks)
        __builtin_trap();
    uintptr_t irq = arch_interrupt_save();
    for (;;) {
        { KERNEL_RAW_SCOPE(guard, &s->metadata);
          if (mapping->previous) __builtin_trap();
          if (!mapping->borrows) break; }
        struct kernel_wait_token token;
        enum kernel_wait_wake_reason reason;
        if (kernel_wait_prepare(&s->changed, 0, 0, &token)) __builtin_trap();
        int busy;
        {
            KERNEL_RAW_SCOPE(guard, &s->metadata);
            if (mapping->previous) __builtin_trap();
            busy = mapping->borrows != 0;
        }
        if (busy && kernel_wait_park(&token, &reason)) __builtin_trap();
        if (kernel_wait_finish(&token)) __builtin_trap();
        if (!busy) break;
    }
    arch_interrupt_restore(irq);
}

void kernel_file_mapping_unregister(struct kernel_file_mapping *mapping)
{
    kernel_file_mapping_stop(mapping);
    kernel_file_mapping_drain(mapping);
}

void kernel_file_mapping_notify(struct kernel_vfs_node *node, uint64_t size)
{
    struct kernel_file_mapping_state *s = state(node);
    if (!s) return;
    struct kernel_file_mapping *mapping;
    { KERNEL_RAW_SCOPE(guard, &s->metadata);
      mapping = s->mappings; borrow(s, mapping); }
    while (mapping) {
        int active;
        { KERNEL_RAW_SCOPE(guard, &s->metadata); active = mapping->previous != 0; }
        if (active) mapping->truncate(mapping->owner, node, size);
        struct kernel_file_mapping *next;
        { KERNEL_RAW_SCOPE(guard, &s->metadata);
          next = mapping->previous ? mapping->next : mapping->retired_next; borrow(s, next); }
        put(s, mapping);
        mapping = next;
    }
}

void kernel_file_mapping_close(struct kernel_vfs_node *node)
{
    struct kernel_file_mapping_state *s = state(node);
    if (!kernel_lock_held(&node->io_lock, 1)) __builtin_trap();
    KERNEL_RAW_SCOPE(guard, &s->metadata);
    if (s->closing || s->epoch == UINT64_MAX) __builtin_trap();
    s->closing = 1;
    s->epoch++;
}

void kernel_file_mapping_open(struct kernel_vfs_node *node)
{
    struct kernel_file_mapping_state *s = state(node);
    if (!kernel_lock_held(&node->io_lock, 1)) __builtin_trap();
    { KERNEL_RAW_SCOPE(guard, &s->metadata);
      if (!s->closing) __builtin_trap();
      s->closing = 0; }
    changed(s);
}

int kernel_file_mapping_epoch(struct kernel_vfs_node *node, uint64_t *epoch)
{
    struct kernel_file_mapping_state *s = state(node);
    if (!s) { *epoch = 0; return 0; }
    KERNEL_RAW_SCOPE(guard, &s->metadata);
    *epoch = s->epoch;
    return s->closing ? -KERNEL_EBUSY : 0;
}

void kernel_file_mapping_wait(struct kernel_vfs_node *node)
{
    struct kernel_file_mapping_state *s = state(node);
    if (!s) return;
    if (kernel_lock_held(&node->io_lock, 0) || kernel_cpu_current()->raw_locks)
        __builtin_trap();
    uintptr_t irq = arch_interrupt_save();
    uint64_t epoch;
    while (kernel_file_mapping_epoch(node, &epoch)) {
        enum kernel_wait_wake_reason reason;
        if (KERNEL_WAIT_RECHECK(&s->changed, 0, 0,
             &reason, kernel_file_mapping_epoch(node, &epoch))) __builtin_trap();
    }
    arch_interrupt_restore(irq);
}
