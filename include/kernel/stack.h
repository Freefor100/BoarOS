#ifndef BOAROS_KERNEL_STACK_H
#define BOAROS_KERNEL_STACK_H

#include <kernel/page.h>

/* Chosen from compiler frame reports and measured production workloads.
 * This is a physical stack allocation; it has no unmapped guard page. */
#define KERNEL_STACK_BYTES BOAROS_PAGE_SIZE
#define KERNEL_STACK_GUARD_BYTES 16U
#define KERNEL_STACK_MINIMUM_RESERVE 1024U

#endif
