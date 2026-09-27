#ifndef BOAROS_FS_RECORD_LOCK_H
#define BOAROS_FS_RECORD_LOCK_H

#include <kernel/scheduler.h>
#include <stdint.h>

struct kernel_heap;
struct kernel_record_lock;

/* The inode owns the tree; the fd table or OFD owns the intrusive index. */
struct kernel_record_lock_state {
    struct kernel_record_lock *root;
    struct kernel_wait_queue waiters;
};

struct kernel_record_lock_conflict {
    int64_t start, end;
    int32_t pid;
    int16_t type;
    uint8_t kind;
    const void *owner;
};

void kernel_record_lock_state_init(struct kernel_record_lock_state *state);
int kernel_record_lock_state_empty(const struct kernel_record_lock_state *state);

/* Owner identity is the address of the table or OFD, qualified by kind. */
int kernel_record_lock_get(struct kernel_record_lock_state *state,
                           const void *owner, uint8_t kind,
                           int64_t start, int64_t end, int16_t type,
                           struct kernel_record_lock_conflict *found);
int kernel_record_lock_set(struct kernel_record_lock_state *state,
                           struct kernel_record_lock **owner_head,
                           const void *owner, uint8_t kind, int32_t pid,
                           int64_t start, int64_t end, int16_t type,
                           struct kernel_heap *heap);

/* Removes only this owner's locks on state; never allocates or does I/O. */
void kernel_record_lock_release(struct kernel_record_lock_state *state,
                                struct kernel_record_lock **owner_head,
                                struct kernel_heap *heap);

#endif
