#ifndef BOAROS_ARCH_RISCV_SMP_H
#define BOAROS_ARCH_RISCV_SMP_H
#include <kernel/cpu.h>
#include <kernel/dtb.h>

#define RISCV_IPI_SCHEDULE 1U
#define RISCV_IPI_DEADLINE 2U
#define RISCV_IPI_HANDSHAKE 4U
#define RISCV_IPI_STOP 8U
#if BOAROS_MAX_CPUS > 1
void riscv_smp_start(const struct dtb_cpu_info *, uint64_t transition_satp,
                     uint64_t final_satp, uint32_t frequency);
void riscv_smp_handle_ipi(void);
void riscv_smp_timer_seen(void);
void riscv_smp_idle(void);
void riscv_smp_stop(void);
void riscv_smp_sync_kernel(uint64_t address, uint64_t size);
struct kernel_cpu *riscv_smp_cpu(unsigned logical);
void riscv_smp_fail(const char *stage, long error) __attribute__((noreturn));
#endif
#endif
