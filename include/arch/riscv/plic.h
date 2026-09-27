#ifndef BOAROS_RISCV_PLIC_H
#define BOAROS_RISCV_PLIC_H
#include <stdint.h>
int riscv_plic_init(volatile void *base, uint64_t size, uint32_t context, uint32_t sources);
int riscv_plic_register(uint32_t source, void (*handle)(void *), void *owner);
void riscv_plic_unregister(uint32_t source, void *owner);
void riscv_plic_dispatch(void);
int riscv_plic_in_interrupt(void);
#endif
