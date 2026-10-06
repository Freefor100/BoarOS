#include <arch/loongarch/signal.h>
#include <kernel/errno.h>
#include <kernel/signal.h>
#include <kernel/task.h>
#include <kernel/uaccess.h>
#include "../../kernel/sched/private.h"
#include <stddef.h>
#include <string.h>

struct la_linux_mcontext {
    uint64_t pc, regs[32];
    uint32_t flags;
} __attribute__((aligned(16)));
struct la_linux_ucontext {
    uint64_t flags, link, stack_pointer;
    uint32_t stack_flags, stack_padding;
    uint64_t stack_size, sigmask[16];
    struct la_linux_mcontext mcontext;
};
struct la_context_info { uint32_t magic, size; uint64_t padding; };
struct la_linux_frame {
    uint8_t info[128];
    struct la_linux_ucontext context;
    struct la_context_info end;
};
_Static_assert(sizeof(struct la_linux_mcontext)==272, "Linux LA sigcontext size");
_Static_assert(offsetof(struct la_linux_ucontext,sigmask)==40, "Linux LA mask offset");
_Static_assert(offsetof(struct la_linux_ucontext,mcontext)==176, "Linux LA mcontext offset");
_Static_assert(sizeof(struct la_linux_ucontext)==448, "Linux LA ucontext size");
/* sizeof rounds this aligned object up; only 592 ABI bytes are published. */
#define LA_SIGNAL_FRAME_BYTES (576U+sizeof(struct la_context_info))
_Static_assert(offsetof(struct la_linux_frame,end)==576, "Linux LA extension offset");

static void bad_frame(void)
{ kernel_user_group_exit(KERNEL_THREAD_EXIT_SIGNAL,11,1); }

void la_signal_note_address_error(uint32_t access)
{
    /* Linux retains this thread field after the fault, including later signals. */
    kernel_task_current()->arch.signal_error_code=access==KERNEL_MM_WRITE ? 2U : 1U;
}

static void build_frame(struct kernel_task *task, struct kernel_mm *mm,
    struct arch_trap_frame *frame, const struct kernel_signal_delivery *delivery)
{
    struct la_linux_frame data;
    uint64_t vdso;
    size_t copied;
    if(frame->regs[3]<LA_SIGNAL_FRAME_BYTES) bad_frame();
    uint64_t sp=(frame->regs[3]-LA_SIGNAL_FRAME_BYTES)&~UINT64_C(15);
    memset(&data,0,sizeof(data));
    memcpy(data.info,&delivery->signal,4);
    memcpy(data.info+8,&delivery->code,4);
    if(delivery->fault) memcpy(data.info+16,&delivery->fault_address,8);
    else memcpy(data.info+16,&delivery->sender,4);
    data.context.stack_flags=2; /* SS_DISABLE; altstack is not yet supported. */
    data.context.sigmask[0]=delivery->restore_mask;
    data.context.mcontext.pc=frame->era;
    memcpy(data.context.mcontext.regs,frame->regs,sizeof(frame->regs));
    if(task->arch.signal_error_code==1) data.context.mcontext.flags=UINT32_C(1)<<30;
    if(task->arch.signal_error_code==2) data.context.mcontext.flags=UINT32_C(1)<<31;
    if(kernel_copy_to_user(mm,sp,&data,LA_SIGNAL_FRAME_BYTES,&copied)!=KERNEL_UACCESS_STATUS_OK ||
       copied!=LA_SIGNAL_FRAME_BYTES || kernel_mm_vdso_address(mm,&vdso)!=KERNEL_MM_STATUS_OK)
        bad_frame();
    frame->regs[3]=sp;frame->regs[1]=vdso;frame->era=delivery->handler;
    frame->regs[4]=delivery->signal;frame->regs[5]=sp;frame->regs[6]=sp+128;
}

void la_signal_prepare_user_return(struct arch_trap_frame *frame)
{
    struct kernel_task *task=kernel_task_current();
    struct kernel_signal_delivery delivery;
    struct kernel_mm *mm;
    if(kernel_task_mm_borrow_mutable(task,&mm)!=KERNEL_TASK_STATUS_OK) bad_frame();
    enum kernel_signal_select_result selected=kernel_signal_select(task,&delivery);
    if(selected==KERNEL_SIGNAL_SELECT_EXIT)
        kernel_user_thread_exit(delivery.exit_reason,delivery.exit_status,delivery.exit_detail);
    int handler=selected==KERNEL_SIGNAL_SELECT_HANDLER;
    enum kernel_signal_restart restart=kernel_signal_restart_decide(task,handler,handler ? delivery.flags : 0);
    if(restart==KERNEL_SIGNAL_RESTART_BLOCK) frame->regs[11]=128;
    else if(restart==KERNEL_SIGNAL_RESTART_INTERRUPTED) {
        if(frame->era>UINT64_MAX-4) bad_frame();
        frame->regs[4]=(uint64_t)(int64_t)-KERNEL_EINTR;frame->era+=4;
    }
    if(handler) build_frame(task,mm,frame,&delivery);
}

void la_signal_restore_current(struct arch_trap_frame *frame)
{
    struct kernel_task *task=kernel_task_current();
    struct kernel_mm *mm;
    struct la_linux_mcontext context;
    struct la_context_info end;
    uint64_t mask,sp=frame->regs[3];
    size_t copied;
    if((sp&15) || sp>UINT64_MAX-LA_SIGNAL_FRAME_BYTES ||
       kernel_task_mm_borrow_mutable(task,&mm)!=KERNEL_TASK_STATUS_OK) bad_frame();
    /* 所有输入先快照；失败不能发布一半恢复的 mask 或寄存器。 */
    if(kernel_copy_from_user(mm,&mask,sp+128+40,sizeof(mask),&copied)!=KERNEL_UACCESS_STATUS_OK || copied!=sizeof(mask) ||
       kernel_copy_from_user(mm,&context,sp+128+176,sizeof(context),&copied)!=KERNEL_UACCESS_STATUS_OK || copied!=sizeof(context) ||
       kernel_copy_from_user(mm,&end,sp+576,sizeof(end),&copied)!=KERNEL_UACCESS_STATUS_OK || copied!=sizeof(end)) bad_frame();
    /* 整数阶段只接受 END；扩展记录必须由未来实际 FP/SIMD owner 解析。 */
    if(end.magic || (context.flags&1)) bad_frame();
    if(kernel_signal_update_blocked(task,LINUX_SIG_SETMASK,&mask,0)!=KERNEL_SIGNAL_STATUS_OK) bad_frame();
    kernel_signal_clear_syscall_restart(task);
    frame->era=context.pc;frame->regs[0]=0;
    memcpy(frame->regs+1,context.regs+1,31*sizeof(uint64_t));
    /* PRMD 与内核 TP 始终来自可信 trap，不能由用户帧提升权限。 */
}
