#include <arch/riscv/mm.h>
#include <kernel/mm_backend.h>

enum kernel_mm_status riscv_kernel_mm_create(struct kernel_mm *mm,
    struct riscv_sv39_user_space *space)
{ return kernel_mm_create(mm, space); }

enum kernel_mm_status riscv_kernel_mm_satp(const struct kernel_mm *mm,
    uint64_t *satp)
{ return kernel_mm_context(mm, satp); }

void riscv_kernel_mm_get_statistics(const struct kernel_mm *mm,
    struct riscv_mm_statistics *statistics)
{
    struct kernel_mm_statistics result;
    if (!statistics) __builtin_trap();
    kernel_mm_get_statistics(mm, &result);
    *statistics = (struct riscv_mm_statistics){result.resident_probes,
        result.protect_visits, result.protect_address_flushes,
        result.protect_global_flushes};
}
