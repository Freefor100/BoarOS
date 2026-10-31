#ifndef BOAROS_KERNEL_IOPRIO_H
#define BOAROS_KERNEL_IOPRIO_H
#include <stdint.h>
struct kernel_task;
/* Process, process-group and real-UID operations use Linux ioprio ABI. */
int64_t kernel_ioprio_call(struct kernel_task *,int,int32_t,int32_t,int32_t);
#endif
