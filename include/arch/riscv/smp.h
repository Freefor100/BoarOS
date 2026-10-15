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
/* CPU停止及根业务/UART drain完成之后，由可信CPU0输出静态统计。 */
void riscv_smp_report_user_execution(void);
void riscv_smp_sync_kernel(uint64_t address, uint64_t size);
void riscv_smp_sync_user(uint32_t targets, uint64_t address, uint64_t size,
    int instructions);
struct kernel_cpu *riscv_smp_cpu(unsigned logical);
void riscv_smp_fail(const char *stage, long error) __attribute__((noreturn));
struct riscv_trap_frame;
void riscv_smp_trap_fatal(const struct riscv_trap_frame *) __attribute__((noreturn));
int riscv_smp_output_allowed(void);
#endif
#endif
