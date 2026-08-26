#ifndef BOAROS_ARCH_RISCV_MM_H
#define BOAROS_ARCH_RISCV_MM_H

#include <arch/riscv/sv39.h>
#include <kernel/mm.h>

#include <stdint.h>

/* Success consumes a LIVE Sv39 user space and publishes one LIVE MM ref. */
enum kernel_mm_status riscv_kernel_mm_create(
    struct kernel_mm *mm,
    struct riscv_sv39_user_space *space);

enum kernel_mm_status riscv_kernel_mm_satp(
    const struct kernel_mm *mm,
    uint64_t *satp);

#endif
