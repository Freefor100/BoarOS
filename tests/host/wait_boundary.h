#ifndef BOAROS_TEST_WAIT_BOUNDARY_H
#define BOAROS_TEST_WAIT_BOUNDARY_H
#include <kernel/scheduler.h>
/* Consumer tests model sleeping at the machine seam; wait-host separately links
 * the production arbiter. No queue/token lifetime algorithm is reproduced here. */
#define HOST_WAIT_JOIN_(a,b) a##b
#define HOST_WAIT_JOIN(a,b) HOST_WAIT_JOIN_(a,b)
#define HOST_WAIT_BOUNDARY(prefix, block) \
    static _Thread_local struct { struct kernel_wait_queue *queue; uint64_t deadline; int interruptible; } HOST_WAIT_JOIN(prefix, wait_parameters); \
    enum kernel_scheduler_status kernel_wait_prepare(struct kernel_wait_queue *q,uint64_t d,int i,struct kernel_wait_token *t) \
    { HOST_WAIT_JOIN(prefix,wait_parameters).queue=q; HOST_WAIT_JOIN(prefix,wait_parameters).deadline=d; \
      HOST_WAIT_JOIN(prefix,wait_parameters).interruptible=i; \
      *t=(struct kernel_wait_token){(void *)&HOST_WAIT_JOIN(prefix,wait_parameters),1}; return KERNEL_SCHEDULER_STATUS_OK; } \
    enum kernel_scheduler_status kernel_wait_park(const struct kernel_wait_token *t,enum kernel_wait_wake_reason *r) \
    { (void)t; return block(HOST_WAIT_JOIN(prefix,wait_parameters).queue,HOST_WAIT_JOIN(prefix,wait_parameters).deadline,HOST_WAIT_JOIN(prefix,wait_parameters).interruptible,r); } \
    enum kernel_scheduler_status kernel_wait_finish(struct kernel_wait_token *t) \
    { *t=(struct kernel_wait_token){0};return KERNEL_SCHEDULER_STATUS_OK; }
#endif
