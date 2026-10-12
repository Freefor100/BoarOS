#ifndef BOAROS_ARCH_RISCV_SBI_H
#define BOAROS_ARCH_RISCV_SBI_H

#include <stdint.h>

long sbi_probe_extension(unsigned long extension_id);
long sbi_set_timer(uint64_t absolute_time);
#define SBI_EXT_HSM 0x48534dUL
#define SBI_EXT_IPI 0x735049UL
#define SBI_EXT_RFENCE 0x52464e43UL
#define SBI_HSM_STARTED 0L
#define SBI_HSM_STOPPED 1L
long sbi_hart_start(uint64_t hart, uint64_t entry, uint64_t opaque);
long sbi_hart_status(uint64_t hart);
long sbi_hart_stop(void);
long sbi_send_ipi(uint64_t hart);
/* 一个真实hart作为mask base，不要求hart ID能装进单个bitmask。 */
long sbi_remote_sfence(uint64_t hart, uint64_t address, uint64_t size);
long sbi_remote_fence_i(uint64_t hart);
void sbi_shutdown(void) __attribute__((noreturn));

#endif
