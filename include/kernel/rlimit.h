#ifndef BOAROS_KERNEL_RLIMIT_H
#define BOAROS_KERNEL_RLIMIT_H

#include <stdint.h>

#define KERNEL_RLIMIT_STACK 3U
#define KERNEL_RLIMIT_NOFILE 7U
#define KERNEL_RLIMIT_STACK_CAP UINT64_C(0x800000)
#define KERNEL_RLIMIT_NOFILE_CAP UINT64_C(1024)

struct kernel_rlimit64 {
    uint64_t current;
    uint64_t maximum;
};

#endif
