#ifndef BOAROS_FS_LWEXT4_PORT_H
#define BOAROS_FS_LWEXT4_PORT_H

#include <kernel/heap.h>

int boaros_lwext4_heap_bind(struct kernel_heap *heap);
void boaros_lwext4_heap_unbind(struct kernel_heap *heap);
int boaros_lwext4_allocation_active(void);

#endif
