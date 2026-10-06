#ifndef BOAROS_ARCH_TASK_H
#define BOAROS_ARCH_TASK_H
#include <arch/context.h>
#include <kernel/scheduler.h>
#include <kernel/mm_backend.h>
#if defined(BOAROS_ARCH_LOONGARCH)
#include <arch/loongarch/task.h>
#else
#include <arch/riscv/fpu.h>
#include <arch/riscv/thread.h>
#include <arch/riscv/trap.h>
#include <arch/riscv/process.h>
#include <arch/riscv/memory_layout.h>
#include <arch/riscv/direct_map.h>
#define arch_thread_state riscv_thread_state
#define arch_fpu_state riscv_fpu_state
#define arch_switch_context riscv_switch_context
#define arch_trap_frame riscv_trap_frame
#define arch_context_status riscv_context_status
#define ARCH_CONTEXT_STATUS_OK RISCV_CONTEXT_STATUS_OK
#define ARCH_TRAP_FRAME_SIZE RISCV_TRAP_FRAME_SIZE
#define ARCH_KERNEL_STACK_WINDOW_BASE RISCV_KERNEL_STACK_WINDOW_BASE
#define ARCH_KERNEL_STACK_WINDOW_SIZE RISCV_KERNEL_STACK_WINDOW_SIZE
#define ARCH_KERNEL_STACK_SLOT_SIZE RISCV_KERNEL_STACK_SLOT_SIZE
#define arch_context_init riscv_context_init
#define arch_context_init_user riscv_context_init_user
#define arch_context_switch riscv_context_switch
#define arch_fpu_switch riscv_fpu_switch
#define arch_process_prepare_initial riscv_process_prepare_initial
#define arch_process_prepare_exec riscv_process_prepare_exec
#define arch_process_prepare_clone riscv_process_prepare_clone
#define arch_direct_map_va_to_pa riscv_direct_map_va_to_pa
#define ARCH_DIRECT_MAP_STATUS_OK RISCV_DIRECT_MAP_STATUS_OK
static inline uint64_t arch_thread_mm(const struct arch_thread_state *s) { return s->satp; }
static inline void arch_thread_set_mm(struct arch_thread_state *s, uint64_t root) { s->satp=root; }
static inline uintptr_t arch_frame_pc(const struct arch_trap_frame *s) { return s->sepc; }
static inline uintptr_t arch_frame_task(const struct arch_trap_frame *s) { return s->kernel_tp; }
#endif
struct arch_fpu_state *arch_process_fpu_borrow_current(void);
enum kernel_scheduler_status arch_process_clone_current(
    const struct arch_trap_frame *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, int64_t *);
#endif
