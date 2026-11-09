#ifndef BOAROS_KERNEL_VDSO_H
#define BOAROS_KERNEL_VDSO_H

#define KERNEL_VDSO_RESTORER_OFFSET 0x100
#define KERNEL_VDSO_CLOCK_SHIFT 32U

#ifndef __ASSEMBLER__
#include <stdint.h>

/* Shared by the kernel writer and the read-only userspace mapping. The
 * multiplier is published last; boot parameters then remain immutable. */
struct kernel_vdso_time_data {
    uint64_t multiplier;
    uint64_t boot_realtime_ns;
    uint64_t coarse_monotonic_ns;
    uint64_t coarse_resolution_ns;
};

#if defined(__riscv) || defined(__loongarch__)
#define KERNEL_HAS_VDSO_ELF 1
struct physical_page_allocator;
/* One backing-owner reference per MM, independent of its mapped PTE. */
int kernel_time_vvar_acquire(struct physical_page_allocator *, uint64_t *);
void kernel_time_vvar_retain(uint64_t);
void kernel_time_vvar_release(struct physical_page_allocator *, uint64_t);
#else
#define KERNEL_HAS_VDSO_ELF 0
#endif
#endif

#endif
