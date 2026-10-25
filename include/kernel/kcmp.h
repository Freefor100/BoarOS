#ifndef BOAROS_KERNEL_KCMP_H
#define BOAROS_KERNEL_KCMP_H
#include <stdint.h>
struct kernel_task;
int64_t kernel_kcmp(struct kernel_task *, int32_t, int32_t, int32_t, uint64_t, uint64_t);
#endif
