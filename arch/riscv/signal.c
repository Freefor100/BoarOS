#include <arch/riscv/signal.h>
#include <arch/riscv/fpu.h>
#include <arch/riscv/process.h>
#include <arch/riscv/trap.h>
#include <kernel/errno.h>
#include <kernel/mm.h>
#include <kernel/scheduler.h>
#include <kernel/signal.h>
#include <kernel/task.h>
#include <kernel/uaccess.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Linux reserves the Q-extension-sized, 16-byte-aligned FP union even
 * on a D-only machine. This padding is part of the libc ABI. */
struct riscv_linux_mcontext {
    uint64_t gregs[32];
    struct {
        uint8_t registers[512];
        uint32_t fcsr;
        uint32_t reserved[3];
    } fp __attribute__((aligned(16)));
};

struct riscv_linux_ucontext {
    uint64_t flags;
    uint64_t link;
    uint64_t stack_pointer;
    uint32_t stack_flags;
    uint32_t stack_padding;
    uint64_t stack_size;
    uint64_t sigmask[16];
    struct riscv_linux_mcontext mcontext;
};

#define RISCV_SIGNAL_INFO_SIZE 128U
#define RISCV_SIGNAL_CONTEXT_OFFSET 176U
#define RISCV_SIGNAL_MCONTEXT_OFFSET 304U
#define RISCV_SIGNAL_FRAME_SIZE 1088U

_Static_assert(offsetof(struct riscv_linux_ucontext, sigmask) == 40U,
               "Linux RISC-V signal mask offset");
_Static_assert(offsetof(struct riscv_linux_ucontext, mcontext) ==
                   RISCV_SIGNAL_CONTEXT_OFFSET, "Linux RISC-V mcontext offset");
_Static_assert(sizeof(struct riscv_linux_mcontext) == 784U,
               "Linux RISC-V mcontext size");
_Static_assert(sizeof(struct riscv_linux_ucontext) == 960U,
               "Linux RISC-V ucontext size");
_Static_assert(RISCV_SIGNAL_INFO_SIZE + sizeof(struct riscv_linux_ucontext) ==
                   RISCV_SIGNAL_FRAME_SIZE, "Linux RISC-V rt signal frame size");

static const uint32_t signal_gpr_offsets[32] = {
    RISCV_TRAP_FRAME_SEPC,
    RISCV_TRAP_FRAME_RA,
    RISCV_TRAP_FRAME_SP,
    RISCV_TRAP_FRAME_GP,
    RISCV_TRAP_FRAME_TP,
    RISCV_TRAP_FRAME_T0,
    RISCV_TRAP_FRAME_T1,
    RISCV_TRAP_FRAME_T2,
    RISCV_TRAP_FRAME_S0,
    RISCV_TRAP_FRAME_S1,
    RISCV_TRAP_FRAME_A0,
    RISCV_TRAP_FRAME_A1,
    RISCV_TRAP_FRAME_A2,
    RISCV_TRAP_FRAME_A3,
    RISCV_TRAP_FRAME_A4,
    RISCV_TRAP_FRAME_A5,
    RISCV_TRAP_FRAME_A6,
    RISCV_TRAP_FRAME_A7,
    RISCV_TRAP_FRAME_S2,
    RISCV_TRAP_FRAME_S3,
    RISCV_TRAP_FRAME_S4,
    RISCV_TRAP_FRAME_S5,
    RISCV_TRAP_FRAME_S6,
    RISCV_TRAP_FRAME_S7,
    RISCV_TRAP_FRAME_S8,
    RISCV_TRAP_FRAME_S9,
    RISCV_TRAP_FRAME_S10,
    RISCV_TRAP_FRAME_S11,
    RISCV_TRAP_FRAME_T3,
    RISCV_TRAP_FRAME_T4,
    RISCV_TRAP_FRAME_T5,
    RISCV_TRAP_FRAME_T6,
};

static void riscv_signal_bad_frame(void)
{
    kernel_user_thread_exit(KERNEL_THREAD_EXIT_SIGNAL, 11U, 1U);
}

static void riscv_signal_build_frame(
    struct kernel_mm *mm, struct riscv_trap_frame *frame,
    const struct kernel_signal_delivery *delivery)
{
    /* Reuse storage for the prefix and mcontext to bound the task stack.
     * No extra allocation or temporary full-frame copy is needed. */
    union {
        uint8_t prefix[RISCV_SIGNAL_MCONTEXT_OFFSET];
        struct riscv_linux_mcontext context;
    } data;
    struct riscv_fpu_state *fpu = riscv_process_fpu_borrow_current();
    uint64_t user_sp;
    uint32_t disabled = 2U;
    uint32_t fcsr;
    uint64_t vdso;
    size_t copied;

    if (frame->sp < RISCV_SIGNAL_FRAME_SIZE) {
        riscv_signal_bad_frame();
    }
    user_sp = (frame->sp - RISCV_SIGNAL_FRAME_SIZE) & ~UINT64_C(15);
    memset(&data, 0, sizeof(data));
    memcpy(data.prefix, &delivery->signal, sizeof(delivery->signal));
    memcpy(data.prefix + 16U, &delivery->sender, sizeof(delivery->sender));
    memcpy(data.prefix + RISCV_SIGNAL_INFO_SIZE + 24U,
           &disabled, sizeof(disabled));
    memcpy(data.prefix + RISCV_SIGNAL_INFO_SIZE + 40U,
           &delivery->restore_mask, sizeof(delivery->restore_mask));
    if (kernel_copy_to_user(mm, user_sp, data.prefix, sizeof(data.prefix),
                             &copied) != KERNEL_UACCESS_STATUS_OK ||
        copied != sizeof(data.prefix)) {
        riscv_signal_bad_frame();
    }
    memset(&data, 0, sizeof(data));
    for (uint32_t index = 0U; index < 32U; index++) {
        memcpy(&data.context.gregs[index],
               (const uint8_t *)frame + signal_gpr_offsets[index], 8U);
    }
    riscv_fpu_state_save(fpu);
    memcpy(data.context.fp.registers, fpu->regs, sizeof(fpu->regs));
    fcsr = (uint32_t)fpu->fcsr;
    memcpy(data.context.fp.registers + 256U, &fcsr, sizeof(fcsr));
    if (kernel_copy_to_user(mm, user_sp + RISCV_SIGNAL_MCONTEXT_OFFSET,
                             &data.context, sizeof(data.context), &copied) !=
            KERNEL_UACCESS_STATUS_OK || copied != sizeof(data.context)) {
        riscv_signal_bad_frame();
    }
    if (kernel_mm_vdso_address(mm, &vdso) != KERNEL_MM_STATUS_OK ||
        vdso > UINT64_MAX - 4U) {
        riscv_signal_bad_frame();
    }
    frame->sp = user_sp;
    frame->sepc = delivery->handler;
    frame->ra = vdso;
    frame->a0 = delivery->signal;
    frame->a1 = user_sp;
    frame->a2 = user_sp + RISCV_SIGNAL_INFO_SIZE;
}

void riscv_signal_prepare_user_return(struct riscv_trap_frame *frame)
{
    struct kernel_task *task = kernel_task_current();
    struct kernel_signal_delivery delivery;
    struct kernel_mm *mm;
    enum kernel_signal_restart restart;
    kernel_pid_t tid;
    int has_handler;
    enum kernel_signal_select_result selection;

    if (frame == 0 || (frame->sstatus & RISCV_SSTATUS_SPP) != 0U ||
        kernel_task_tid(task, &tid) != KERNEL_TASK_STATUS_OK) {
        return;
    }
    if (kernel_task_mm_borrow_mutable(task, &mm) != KERNEL_TASK_STATUS_OK) {
        riscv_signal_bad_frame();
    }
    selection = kernel_signal_select(task, &delivery);
    if (selection == KERNEL_SIGNAL_SELECT_EXIT)
        kernel_user_thread_exit(delivery.exit_reason, delivery.exit_status,
                                 delivery.exit_detail);
    has_handler = selection == KERNEL_SIGNAL_SELECT_HANDLER;
    restart = kernel_signal_restart_decide(task, has_handler,
                                            has_handler ? delivery.flags : 0U);
    if (restart == KERNEL_SIGNAL_RESTART_BLOCK) {
        frame->a7 = 128U;
    } else if (restart == KERNEL_SIGNAL_RESTART_INTERRUPTED) {
        if (frame->sepc > UINT64_MAX - 4U) {
            riscv_signal_bad_frame();
        }
        frame->a0 = (uint64_t)(int64_t)-KERNEL_EINTR;
        frame->sepc += 4U;
    }
    if (has_handler) {
        riscv_signal_build_frame(mm, frame, &delivery);
    }
}

void riscv_signal_restore_current(struct riscv_trap_frame *frame)
{
    struct kernel_task *task = kernel_task_current();
    struct riscv_fpu_state *fpu = riscv_process_fpu_borrow_current();
    struct kernel_mm *mm;
    struct riscv_linux_mcontext context;
    uint64_t sigmask;
    uint32_t fcsr;
    size_t copied;

    if (frame == 0 || (frame->sp & 15U) != 0U ||
        frame->sp > UINT64_MAX - RISCV_SIGNAL_FRAME_SIZE ||
        kernel_task_mm_borrow_mutable(task, &mm) != KERNEL_TASK_STATUS_OK) {
        riscv_signal_bad_frame();
    }
    /* Snapshot all restored input before committing registers or mask. */
    if (kernel_copy_from_user(mm, &sigmask,
                               frame->sp + RISCV_SIGNAL_INFO_SIZE + 40U,
                               sizeof(sigmask), &copied) !=
            KERNEL_UACCESS_STATUS_OK || copied != sizeof(sigmask) ||
        kernel_copy_from_user(mm, &context,
                               frame->sp + RISCV_SIGNAL_MCONTEXT_OFFSET,
                               sizeof(context), &copied) !=
            KERNEL_UACCESS_STATUS_OK || copied != sizeof(context)) {
        riscv_signal_bad_frame();
    }
    /* No vector/extended context is enabled. Linux requires reserved
     * words to remain zero when no extension record follows. */
    if (context.fp.reserved[0] != 0U || context.fp.reserved[1] != 0U ||
        context.fp.reserved[2] != 0U) {
        riscv_signal_bad_frame();
    }
    if (kernel_signal_update_blocked(task, LINUX_SIG_SETMASK, &sigmask, 0) !=
        KERNEL_SIGNAL_STATUS_OK) {
        riscv_signal_bad_frame();
    }
    kernel_signal_clear_syscall_restart(task);
    for (uint32_t index = 0U; index < 32U; index++) {
        memcpy((uint8_t *)frame + signal_gpr_offsets[index],
               &context.gregs[index], 8U);
    }
    memcpy(fpu->regs, context.fp.registers, sizeof(fpu->regs));
    memcpy(&fcsr, context.fp.registers + 256U, sizeof(fcsr));
    fpu->fcsr = fcsr;
    fpu->saved = 1U;
    riscv_fpu_state_restore(fpu);
    frame->sstatus = (frame->sstatus & ~RISCV_SSTATUS_FS_MASK) |
                     RISCV_SSTATUS_FS_CLEAN;
}
