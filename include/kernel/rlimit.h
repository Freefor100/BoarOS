#ifndef BOAROS_KERNEL_RLIMIT_H
#define BOAROS_KERNEL_RLIMIT_H

#include <stdint.h>

#define KERNEL_RLIMIT_CPU 0U
#define KERNEL_RLIMIT_STACK 3U
#define KERNEL_RLIMIT_FSIZE 1U
#define KERNEL_RLIMIT_DATA 2U
#define KERNEL_RLIMIT_CORE 4U
#define KERNEL_RLIMIT_RSS 5U
#define KERNEL_RLIMIT_NPROC 6U
#define KERNEL_RLIMIT_NOFILE 7U
#define KERNEL_RLIMIT_MEMLOCK 8U
#define KERNEL_RLIMIT_AS 9U
#define KERNEL_RLIMIT_LOCKS 10U
#define KERNEL_RLIMIT_SIGPENDING 11U
#define KERNEL_RLIMIT_MSGQUEUE 12U
#define KERNEL_RLIMIT_NICE 13U
#define KERNEL_RLIMIT_RTPRIO 14U
#define KERNEL_RLIMIT_RTTIME 15U
#define KERNEL_RLIMIT_COUNT 16U
#define KERNEL_RESOURCE_NPROC_CHARGED UINT32_C(1)
#define KERNEL_RESOURCE_NPROC_EXCEEDED UINT32_C(2)
#define KERNEL_RLIMIT_SIGPENDING_DEFAULT UINT64_C(4096)
#define KERNEL_RLIMIT_STACK_CAP UINT64_C(0x800000)
#define KERNEL_RLIMIT_NOFILE_CAP UINT64_C(1024)
#define KERNEL_RLIM_INFINITY UINT64_MAX

struct kernel_rlimit64 {
    uint64_t current;
    uint64_t maximum;
};

struct kernel_heap;
struct kernel_resource_limits {
    struct kernel_heap *heap;
    uint32_t references;
    struct kernel_rlimit64 values[KERNEL_RLIMIT_COUNT];
};
/* A task owns one reference; clone threads share, fork takes a value copy. */
struct kernel_resource_limits *kernel_resource_limits_create(struct kernel_heap *,
    const struct kernel_resource_limits *source);
struct kernel_resource_limits *kernel_resource_limits_get(struct kernel_resource_limits *);
void kernel_resource_limits_put(struct kernel_resource_limits *);

/* Usage belongs to the MM; these are the initiating thread group's limits. */
struct kernel_memory_limits {
    uint64_t address_space, data_soft, data_hard;
};

#endif
