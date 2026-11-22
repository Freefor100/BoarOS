#ifndef BOAROS_RISCV_MMU_CONTEXT_H
#define BOAROS_RISCV_MMU_CONTEXT_H
#include <stdint.h>
#include <kernel/page.h>
/* 硬件原语单独绑定；宿主模型替换CSR和fence，仍执行生产页表walker。 */
static inline uint64_t riscv_mmu_context_read(void)
{ uint64_t value; __asm__ volatile("csrr %0, satp" : "=r"(value)); return value; }
static inline void riscv_mmu_context_write(uint64_t value)
{ __asm__ volatile("csrw satp, %0" :: "r"(value) : "memory"); }
static inline void riscv_mmu_flush_all(void)
{ __asm__ volatile("sfence.vma zero, zero" ::: "memory"); }
static inline void riscv_mmu_flush_address(uint64_t address)
{ __asm__ volatile("sfence.vma %0, zero" :: "r"(address) : "memory"); }
static inline void riscv_mmu_flush_kernel_range(uint64_t address, uint64_t size)
{
    if (size == BOAROS_PAGE_SIZE || size == 2 * BOAROS_PAGE_SIZE) {
        riscv_mmu_flush_address(address);
        if (size == 2 * BOAROS_PAGE_SIZE)
            riscv_mmu_flush_address(address + BOAROS_PAGE_SIZE);
    } else {
        riscv_mmu_flush_all();
    }
}
static inline void riscv_mmu_instruction_publish_barrier(void)
{ __asm__ volatile("fence rw, rw" ::: "memory"); }
static inline void riscv_mmu_sync_instructions(void)
{ __asm__ volatile("fence.i" ::: "memory"); }
#endif
