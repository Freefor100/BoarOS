#ifndef BOAROS_ARCH_LOONGARCH_TASK_H
#define BOAROS_ARCH_LOONGARCH_TASK_H
#define ARCH_TRAP_FRAME_SIZE 304
#define ARCH_KERNEL_STACK_WINDOW_BASE 0xffff800000000000
#define ARCH_KERNEL_STACK_WINDOW_SIZE 0x06000000
#define ARCH_KERNEL_STACK_SLOT_SIZE 0xc000
#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>
struct arch_thread_state { uintptr_t kernel_sp, user_sp; uint64_t user_mode, page_root, signal_error_code; } __attribute__((aligned(16)));
struct arch_switch_context { uint64_t ra, sp, tp, fp, s[9]; } __attribute__((aligned(16)));
/* width 为当前硬件宽度；live_width 保留已初始化的上半部，即使 sigreturn 撤销 used。 */
struct arch_fpu_state { uint64_t regs[32][4], fcc; uint32_t fcsr, width; uint64_t saved; uint32_t live_width; } __attribute__((aligned(32)));
_Static_assert(sizeof(struct arch_fpu_state)==1056 && offsetof(struct arch_fpu_state,fcc)==1024 &&
               offsetof(struct arch_fpu_state,fcsr)==1032 && offsetof(struct arch_fpu_state,width)==1036 &&
               offsetof(struct arch_fpu_state,saved)==1040,"LA FP/SIMD offsets");
struct arch_trap_frame {
    uint64_t regs[32];
    uint64_t prmd, era, estat, badv, kernel_tp;
} __attribute__((aligned(16)));
_Static_assert(sizeof(struct arch_trap_frame) == ARCH_TRAP_FRAME_SIZE, "LA trap frame size");
_Static_assert(offsetof(struct arch_thread_state, kernel_sp) == 0, "LA kernel stack offset");
enum arch_context_status { ARCH_CONTEXT_STATUS_OK = 0, ARCH_CONTEXT_STATUS_INVALID_ARGUMENT };
enum arch_context_status arch_context_init(struct arch_switch_context *, uintptr_t, void (*)(void *), void *, void *);
enum arch_context_status arch_context_init_user(struct arch_switch_context *, uintptr_t, void *);
void arch_context_switch(struct arch_switch_context *, const struct arch_switch_context *);
void arch_fpu_switch(struct arch_fpu_state *, struct arch_fpu_state *);
void arch_process_prepare_initial(struct kernel_task *, uintptr_t, uintptr_t, uintptr_t);
void arch_process_prepare_exec(struct kernel_task *, uintptr_t, uintptr_t, uintptr_t);
enum kernel_scheduler_status arch_process_prepare_clone(struct kernel_task *, struct kernel_task *, const struct arch_trap_frame *, uint64_t, int, uint64_t);
static inline uint64_t arch_thread_mm(const struct arch_thread_state *s) { return s->page_root; }
static inline void arch_thread_set_mm(struct arch_thread_state *s, uint64_t root) { s->page_root=root; }
static inline uintptr_t arch_frame_pc(const struct arch_trap_frame *s) { return s->era; }
static inline uintptr_t arch_frame_task(const struct arch_trap_frame *s) { return s->kernel_tp; }
int arch_direct_map_va_to_pa(uint64_t, uint64_t, uint64_t *);
#define ARCH_DIRECT_MAP_STATUS_OK 0
#endif
#endif
