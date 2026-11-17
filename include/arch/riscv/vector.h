#ifndef BOAROS_RISCV_VECTOR_H
#define BOAROS_RISCV_VECTOR_H
#include <stdint.h>
#include <stddef.h>
#define RISCV_SSTATUS_VS_MASK 0x600U
#define RISCV_SSTATUS_VS_CLEAN 0x400U
#define RISCV_VECTOR_MAGIC 0x53465457U
#define RISCV_VECTOR_NOTE 0x900U
struct kernel_heap;
struct kernel_task;
struct riscv_trap_frame;
/* First five words are the Linux vector regset header. Register storage is
 * aligned, runtime-sized, and owned until exec or final task collection. */
struct riscv_vector_state {
    uint64_t vstart, vl, vtype, vcsr, vlenb;
    struct kernel_heap *heap;
    unsigned char regs[] __attribute__((aligned(16)));
};
_Static_assert(offsetof(struct riscv_vector_state,regs)==48,"vector asm offset");
extern uint64_t riscv_vector_vlenb;
void riscv_vector_boot(uint32_t features);
int riscv_vector_check_hart(void);
int riscv_vector_first_use(struct riscv_trap_frame *);
void riscv_vector_switch(struct kernel_task *,struct kernel_task *);
void riscv_vector_save(struct kernel_task *);
void riscv_vector_restore(struct kernel_task *);
int riscv_vector_clone(struct kernel_task *,struct kernel_task *);
void riscv_vector_release(struct kernel_task *);
void riscv_vector_exec(struct kernel_task *);
int64_t riscv_vector_control(struct kernel_task *,unsigned,uint64_t);
int64_t riscv_vector_ptrace(struct kernel_task *,struct kernel_task *,uint64_t,int);
int riscv_vector_csr_valid(const uint64_t *);
void riscv_vector_save_hw(struct riscv_vector_state *);
void riscv_vector_restore_hw(const struct riscv_vector_state *);
#endif
