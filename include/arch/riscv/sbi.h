#ifndef BOAROS_ARCH_RISCV_SBI_H
#define BOAROS_ARCH_RISCV_SBI_H

#include <stdint.h>

long sbi_probe_extension(unsigned long extension_id);
long sbi_set_timer(uint64_t absolute_time);
void sbi_shutdown(void) __attribute__((noreturn));

#endif
