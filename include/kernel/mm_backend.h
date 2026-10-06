#ifndef BOAROS_KERNEL_MM_BACKEND_H
#define BOAROS_KERNEL_MM_BACKEND_H
#include <arch/mmu.h>
#include <kernel/mm.h>
/* Successful creation consumes one backend space; failure preserves it. */
enum kernel_mm_status kernel_mm_create(struct kernel_mm *, struct arch_mmu_user_space *);
enum kernel_mm_status kernel_mm_context(const struct kernel_mm *, uint64_t *);
struct kernel_mm_statistics {
    uint64_t resident_probes, protect_visits;
    uint64_t protect_address_flushes, protect_global_flushes;
};
void kernel_mm_get_statistics(const struct kernel_mm *, struct kernel_mm_statistics *);
#endif
