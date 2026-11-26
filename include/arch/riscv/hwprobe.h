#ifndef BOAROS_RISCV_HWPROBE_H
#define BOAROS_RISCV_HWPROBE_H

#include <kernel/syscall.h>
#include <stdint.h>

struct physical_page_allocator;
struct riscv_trap_frame;

enum riscv_hwprobe_extension {
    RISCV_HWPROBE_FD = 1U << 0,
    RISCV_HWPROBE_C = 1U << 1,
    RISCV_HWPROBE_V = 1U << 2,
    RISCV_HWPROBE_ZBA = 1U << 3,
    RISCV_HWPROBE_ZBB = 1U << 4,
    RISCV_HWPROBE_ZBS = 1U << 5,
    RISCV_HWPROBE_ZICBOZ = 1U << 6,
};

struct riscv_hwprobe_cpu_info {
    uint64_t ids[3], extensions;
    unsigned ids_valid;
    unsigned misaligned_scalar;
};

void riscv_hwprobe_initialize(unsigned cpu, uint32_t features, uint32_t common, uint32_t frequency);
void riscv_hwprobe_measure_scalar(unsigned cpu, struct physical_page_allocator *, uint32_t frequency);
int riscv_hwprobe_fault(struct riscv_trap_frame *frame);
/* Per-hart capabilities are immutable after that hart goes online. */
const struct riscv_hwprobe_cpu_info *riscv_hwprobe_get_cpu_info(unsigned cpu);
enum kernel_syscall_status riscv_syscall_hwprobe(struct kernel_task *,
    const struct kernel_syscall_request *, struct kernel_syscall_result *);

#endif
