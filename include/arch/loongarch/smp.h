#ifndef BOAROS_ARCH_LOONGARCH_SMP_H
#define BOAROS_ARCH_LOONGARCH_SMP_H
#include <kernel/cpu.h>
#define LA_IPI_SCHEDULE 1U
#define LA_IPI_DEADLINE 2U
#define LA_IPI_HANDSHAKE 4U
#define LA_IPI_STOP 8U
#define LA_IPI_TLB 16U
#if BOAROS_MAX_CPUS > 1
void la_smp_discover(const void *dtb);
void la_smp_start(uint32_t frequency);
void la_smp_handle_ipi(void);
void la_smp_timer_seen(void);
void la_smp_idle(void);
void la_smp_stop(void);
void la_smp_report(void);
void la_smp_sync_user(uint32_t targets,uint64_t address,uint64_t size,int instructions);
void la_smp_sync_kernel(uint64_t address,uint64_t size);
struct kernel_cpu *la_smp_cpu(unsigned logical);
void la_mmu_secondary_initialize(void);
extern uint64_t la_secondary_thread;
static inline uint32_t la_iocsr_read(uint32_t address)
{uint32_t v;__asm__ volatile("iocsrrd.w %0,%1":"=r"(v):"r"(address):"memory");return v;}
static inline void la_iocsr_write(uint32_t address,uint32_t value)
{__asm__ volatile("iocsrwr.w %0,%1"::"r"(value),"r"(address):"memory");}
#endif
#endif
