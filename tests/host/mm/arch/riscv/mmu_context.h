#ifndef BOAROS_HOST_MMU_CONTEXT_H
#define BOAROS_HOST_MMU_CONTEXT_H
#include <stdint.h>
extern _Thread_local uint64_t mm_test_context;
void mm_test_fence(uint64_t address, int instructions);
static inline uint64_t riscv_mmu_context_read(void) { return mm_test_context; }
static inline void riscv_mmu_context_write(uint64_t value) { mm_test_context = value; }
static inline void riscv_mmu_flush_all(void) { mm_test_fence(UINT64_MAX, 0); }
static inline void riscv_mmu_flush_address(uint64_t address) { mm_test_fence(address, 0); }
static inline void riscv_mmu_sync_instructions(void) { mm_test_fence(0, 1); }
#endif
