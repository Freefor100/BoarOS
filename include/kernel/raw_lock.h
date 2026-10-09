#ifndef BOAROS_KERNEL_RAW_LOCK_H
#define BOAROS_KERNEL_RAW_LOCK_H
#include <kernel/cpu.h>
#include <stdint.h>

enum kernel_raw_rank { KERNEL_RAW_RANK_HEAP = 10, KERNEL_RAW_RANK_PAGE = 20 };
/* 初始化/移动仅允许未发布实例；原子字是权威互斥，CPU链负责owner与锁序。 */
struct kernel_raw_lock { uint32_t word, rank, initialized; };
struct kernel_raw_guard {
    struct kernel_raw_lock *lock;
    struct kernel_cpu *cpu;
    struct kernel_raw_guard *previous;
    uintptr_t interrupts;
    uint32_t previous_depth;
};
void kernel_raw_lock_init(struct kernel_raw_lock *, uint32_t rank);
void kernel_raw_lock_acquire(struct kernel_raw_lock *, struct kernel_raw_guard *);
void kernel_raw_lock_release(struct kernel_raw_guard *);
static inline void kernel_raw_scope_release(struct kernel_raw_guard *guard)
{ if (guard->lock) kernel_raw_lock_release(guard); }
#define KERNEL_RAW_SCOPE(name, object) \
    struct kernel_raw_guard name __attribute__((cleanup(kernel_raw_scope_release))) = {0}; \
    kernel_raw_lock_acquire((object), &name)
#endif
