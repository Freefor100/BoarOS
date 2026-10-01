#ifndef BOAROS_FS_LWEXT4_PORT_H
#define BOAROS_FS_LWEXT4_PORT_H

#include <kernel/heap.h>

struct ext4_lock;
struct kernel_rwlock;
void boaros_lwext4_lock_init(struct ext4_lock *callbacks, struct kernel_rwlock *lock);
void boaros_lwext4_wait(void *key);
void boaros_lwext4_wake(void *key);
int boaros_lwext4_read_context(void);
unsigned boaros_lwext4_pause(void);
void boaros_lwext4_resume(void *lock, unsigned depth);

int boaros_lwext4_heap_bind(struct kernel_heap *heap);
void boaros_lwext4_heap_unbind(struct kernel_heap *heap);
int boaros_lwext4_allocation_active(void);

#endif
