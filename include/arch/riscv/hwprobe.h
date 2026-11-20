#ifndef BOAROS_RISCV_HWPROBE_H
#define BOAROS_RISCV_HWPROBE_H

#include <kernel/syscall.h>
#include <stdint.h>

void riscv_hwprobe_initialize(unsigned cpu, uint32_t features, uint32_t common, uint32_t frequency);
enum kernel_syscall_status riscv_syscall_hwprobe(struct kernel_task *,
    const struct kernel_syscall_request *, struct kernel_syscall_result *);

#endif
