#ifndef BOAROS_KERNEL_NETWORK_H
#define BOAROS_KERNEL_NETWORK_H
#include <kernel/dtb.h>
#include <kernel/heap.h>
struct kernel_network;
int kernel_network_start(struct kernel_network **owner, struct kernel_heap *heap,
    const struct dtb_boot_info *boot, const struct dtb_irq_info *irq);
int kernel_network_stop(struct kernel_network **owner);
#endif
