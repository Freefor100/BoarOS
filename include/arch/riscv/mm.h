#ifndef BOAROS_ARCH_RISCV_MM_H
#define BOAROS_ARCH_RISCV_MM_H

#include <arch/riscv/sv39.h>
#include <kernel/mm.h>

#include <stdint.h>

struct riscv_mm_statistics {
    uint64_t resident_probes;
    uint64_t protect_visits;
    uint64_t protect_address_flushes;
    uint64_t protect_global_flushes;
};
void riscv_kernel_mm_get_statistics(const struct kernel_mm *mm,
                                    struct riscv_mm_statistics *statistics);

/* Success consumes a LIVE Sv39 user space and publishes one LIVE MM ref. */
enum kernel_mm_status riscv_kernel_mm_create(
    struct kernel_mm *mm,
    struct riscv_sv39_user_space *space);

enum kernel_mm_status riscv_kernel_mm_satp(
    const struct kernel_mm *mm,
    uint64_t *satp);

#endif
