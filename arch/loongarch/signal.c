#include <arch/loongarch/signal.h>
#include <arch/loongarch/fpu.h>
#include <kernel/errno.h>
#include <kernel/signal.h>
#include <kernel/task.h>
#include <kernel/uaccess.h>
#include <platform/loongarch_virt.h>
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
struct la_linux_fp {uint64_t regs[32],fcc;uint32_t fcsr,padding;};
struct la_linux_frame {
    uint8_t info[128];
    struct la_linux_ucontext context;
    struct la_context_info extension;
    struct la_linux_fp fp;
    struct la_context_info end;
};
_Static_assert(sizeof(struct la_linux_mcontext)==272, "Linux LA sigcontext size");
_Static_assert(offsetof(struct la_linux_ucontext,sigmask)==40, "Linux LA mask offset");
_Static_assert(offsetof(struct la_linux_ucontext,mcontext)==176, "Linux LA mcontext offset");
_Static_assert(sizeof(struct la_linux_ucontext)==448, "Linux LA ucontext size");
/* sizeof rounds this aligned object up; only 592 ABI bytes are published. */
#define LA_SIGNAL_FRAME_BYTES (576U+sizeof(struct la_context_info))
_Static_assert(offsetof(struct la_linux_frame,extension)==576, "Linux LA extension offset");
_Static_assert(sizeof(struct la_linux_fp)==272 && sizeof(struct la_linux_frame)==880,"Linux LA FPU frame");

static void bad_frame(void)
{ kernel_user_group_exit(KERNEL_THREAD_EXIT_SIGNAL,11,1); }

void la_signal_note_address_error(uint32_t access)
{
    /* Linux retains this thread field after the fault, including later signals. */
    kernel_task_current()->arch.signal_error_code=access==KERNEL_MM_WRITE ? 2U : 1U;
}

static size_t semantic_bytes(unsigned width) { return 256U*width+12U; }
static size_t record_bytes(unsigned width) { return (semantic_bytes(width)+7U)&~(size_t)7U; }
static unsigned record_width(uint32_t magic)
{ return magic==0x46505501U ? 1 : magic==0x53580001U ? 2 : magic==0x41535801U ? 4 : 0; }
static uint32_t record_magic(unsigned width)
{ return width==1 ? 0x46505501U : width==2 ? 0x53580001U : 0x41535801U; }
static void build_frame(struct kernel_task *task, struct kernel_mm *mm,
    struct arch_trap_frame *frame, const struct kernel_signal_delivery *delivery)
{
    /* Linux 从原 SP 向下分配 END、对齐 payload、info 和固定576字节前缀。 */
    unsigned char data[1664] __attribute__((aligned(32)));
    struct la_linux_frame prefix;
    if(task->fpu.saved)la_fpu_save(&task->fpu);
    unsigned width=task->fpu.saved ? task->fpu.live_width : 0;
    uint64_t top=frame->regs[3]&~UINT64_C(15),sp,vdso;
    size_t copied;
    if(top<LA_SIGNAL_FRAME_BYTES)bad_frame();
    uint64_t end=top-16;
    struct la_context_info info={0};
    if(width) {
        size_t payload=record_bytes(width);
        if(end<payload)bad_frame();
        uint64_t address=(end-payload)&~(width==4 ? UINT64_C(31) : UINT64_C(15));
        if(address<16+576)bad_frame();
        sp=address-16-576;
        info.magic=record_magic(width);info.size=(uint32_t)(end-(sp+576));
    } else sp=end-576;
    size_t bytes=top-sp;
    if(bytes>sizeof(data))la_virt_fatal("signal frame capacity");
    memset(data,0,bytes);memset(&prefix,0,sizeof(prefix));
    memcpy(prefix.info,&delivery->signal,4);
    memcpy(prefix.info+8,&delivery->code,4);
    if(delivery->fault)memcpy(prefix.info+16,&delivery->fault_address,8);
    else memcpy(prefix.info+16,&delivery->sender,4);
    prefix.context.stack_flags=2;
    prefix.context.sigmask[0]=delivery->restore_mask;
    prefix.context.mcontext.pc=frame->era;
    memcpy(prefix.context.mcontext.regs,frame->regs,sizeof(frame->regs));
    if(task->arch.signal_error_code==1)prefix.context.mcontext.flags|=UINT32_C(1)<<30;
    if(task->arch.signal_error_code==2)prefix.context.mcontext.flags|=UINT32_C(1)<<31;
    if(width) {
        prefix.context.mcontext.flags|=1;
        unsigned char *payload=data+592;
        for(unsigned i=0;i<32;i++)memcpy(payload+i*width*8,task->fpu.regs[i],width*8);
        memcpy(payload+256*width,&task->fpu.fcc,8);
        memcpy(payload+256*width+8,&task->fpu.fcsr,4);
        memcpy(data+576,&info,sizeof(info));
    }
    memcpy(data,&prefix,576);
    if(kernel_copy_to_user(mm,sp,data,bytes,&copied)!=KERNEL_UACCESS_STATUS_OK || copied!=bytes ||
       kernel_mm_vdso_address(mm,&vdso)!=KERNEL_MM_STATUS_OK)bad_frame();
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
    struct la_context_info extension;
    struct arch_fpu_state fp;
    uint64_t mask,sp=frame->regs[3];
    size_t copied;
    if(sp>UINT64_MAX-LA_SIGNAL_FRAME_BYTES ||
       kernel_task_mm_borrow_mutable(task,&mm)!=KERNEL_TASK_STATUS_OK)bad_frame();
    /* 输入先快照；更宽记录优先，重复的同类记录取最后一条。 */
    if(kernel_copy_from_user(mm,&mask,sp+128+40,sizeof(mask),&copied)!=KERNEL_UACCESS_STATUS_OK || copied!=sizeof(mask) ||
       kernel_copy_from_user(mm,&context,sp+128+176,sizeof(context),&copied)!=KERNEL_UACCESS_STATUS_OK || copied!=sizeof(context))bad_frame();
    uint64_t address=sp+576,addresses[5]={0};
    for(;;) {
        if(kernel_copy_from_user(mm,&extension,address,8,&copied)!=KERNEL_UACCESS_STATUS_OK || copied!=8)bad_frame();
        if(!extension.magic)break;
        unsigned width=record_width(extension.magic);
        if(!width || extension.size<16+record_bytes(width) || address>UINT64_MAX-extension.size)bad_frame();
        addresses[width]=address+16;address+=extension.size;
    }
    /* 撤销 USED_FP 时 Linux 丢弃 handler 的硬件更新，保留原内存 image。 */
    if((context.flags&1) && task->fpu.width)la_fpu_save(&task->fpu);
    fp=task->fpu;
    unsigned width=addresses[4] ? 4 : addresses[2] ? 2 : addresses[1] ? 1 : 0;
    uint64_t fp_address=addresses[width];
    if(width) {
        unsigned char payload[1040];size_t bytes=semantic_bytes(width);
        /* 记录尺寸仍含 ABI padding，但恢复仅读取有效 FR/FCC/FCSR。 */
        if(kernel_copy_from_user(mm,payload,fp_address,bytes,&copied)!=KERNEL_UACCESS_STATUS_OK || copied!=bytes)bad_frame();
        for(unsigned i=0;i<32;i++)memcpy(fp.regs[i],payload+i*width*8,width*8);
        memcpy(&fp.fcc,payload+256*width,8);memcpy(&fp.fcsr,payload+256*width+8,4);
    }
    uint32_t pending=width ? fp.fcsr&((fp.fcsr&31U)<<24) : 0;
    if(pending) {
        fp.fcsr&=~((fp.fcsr&31U)<<24);
        if(kernel_copy_to_user(mm,fp_address+256*width+8,&fp.fcsr,4,&copied)!=KERNEL_UACCESS_STATUS_OK || copied!=4)bad_frame();
    }
    if(kernel_signal_update_blocked(task,LINUX_SIG_SETMASK,&mask,0)!=KERNEL_SIGNAL_STATUS_OK)bad_frame();
    kernel_signal_clear_syscall_restart(task);
    frame->era=context.pc;frame->regs[0]=0;
    memcpy(frame->regs+1,context.regs+1,31*sizeof(uint64_t));
    fp.saved=(context.flags&1)!=0;
    if(fp.saved) {
        if(!fp.width)fp.width=1;
        if(!fp.live_width)fp.live_width=1;
    } else fp.width=0;
    task->fpu=fp;
    if(fp.saved)la_fpu_restore(&task->fpu);
    else __asm__ volatile("csrwr $zero,2":::"memory");
    if(pending)kernel_signal_force_fault(task,8,128,0);
    /* PRMD 与内核 TP 始终来自可信 trap，不能由用户帧提升权限。 */
}
