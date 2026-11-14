#include <kernel/scheduler.h>
#include <assert.h>
#if BOAROS_MAX_CPUS > 1
void kernel_cpu_flush_notifications(void) { assert(!kernel_cpu_current()->outbound); }
#endif
void kernel_wait_queue_init(struct kernel_wait_queue *q) { q->initialized=KERNEL_WAIT_QUEUE_INITIALIZED; }
enum kernel_scheduler_status kernel_wait_queue_wake_all(struct kernel_wait_queue *q) { (void)q; return KERNEL_SCHEDULER_STATUS_OK; }
enum kernel_scheduler_status kernel_scheduler_block_current(struct kernel_wait_queue *q, uint64_t d, int i, enum kernel_wait_wake_reason *r) { (void)q;(void)d;(void)i; *r=KERNEL_WAIT_SIGNALLED; return KERNEL_SCHEDULER_STATUS_OK; }

#include "wait_boundary.h"
HOST_WAIT_BOUNDARY(, kernel_scheduler_block_current)
