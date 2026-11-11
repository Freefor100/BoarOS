#ifndef BOAROS_KERNEL_TRACE_EVENTS_H
#define BOAROS_KERNEL_TRACE_EVENTS_H
#include <stdint.h>
#define KERNEL_TRACE_SYS_ENTER 1U
#define KERNEL_TRACE_SYS_EXIT 2U
struct kernel_trace_entry {
    uint16_t type;
    uint8_t flags,preempt_count;
    int32_t pid;
};
struct kernel_trace_sys_enter {
    struct kernel_trace_entry common;
    long id;
    unsigned long args[6];
};
struct kernel_trace_sys_exit {
    struct kernel_trace_entry common;
    long id;
    long ret;
};
#endif
