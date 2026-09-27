#ifndef BOAROS_FS_LWEXT4_PORT_H
#define BOAROS_FS_LWEXT4_PORT_H

#include <kernel/heap.h>

struct ext4_lock;
extern struct ext4_lock boaros_lwext4_locks;
void boaros_lwext4_wait(void *key);
void boaros_lwext4_wake(void *key);
int boaros_lwext4_read_context(void);

int boaros_lwext4_heap_bind(struct kernel_heap *heap);
void boaros_lwext4_heap_unbind(struct kernel_heap *heap);
int boaros_lwext4_allocation_active(void);

#endif
