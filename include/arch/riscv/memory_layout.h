#ifndef BOAROS_ARCH_RISCV_MEMORY_LAYOUT_H
#define BOAROS_ARCH_RISCV_MEMORY_LAYOUT_H

#include <stdint.h>

/* Keep this synchronized with KERNEL_VIRTUAL_BASE in linker.ld. */
#define RISCV_KERNEL_VIRTUAL_BASE UINT64_C(0xffffffff80000000)

#define RISCV_DIRECT_MAP_BASE UINT64_C(0xffffffc000000000)
#define RISCV_DIRECT_MAP_SIZE UINT64_C(0x2000000000)
#define RISCV_KERNEL_MMIO_BASE \
    (RISCV_DIRECT_MAP_BASE + RISCV_DIRECT_MAP_SIZE)
#define RISCV_KERNEL_MMIO_SIZE \
    (RISCV_KERNEL_VIRTUAL_BASE - RISCV_KERNEL_MMIO_BASE)

/* Kernel stack window inside the otherwise unused top root slot.  Each
 * slot is one unmapped 4 KiB guard page followed by the stack; the empty
 * subtree is reserved while the page table is BUILDING so every user
 * root copy shares it, and leaves are inserted at runtime. */
#define RISCV_KERNEL_STACK_WINDOW_BASE UINT64_C(0xffffffffc0000000)
#define RISCV_KERNEL_STACK_WINDOW_SIZE UINT64_C(0x08000000)
#define RISCV_KERNEL_STACK_SLOT_SIZE UINT64_C(0x3000)

#endif
