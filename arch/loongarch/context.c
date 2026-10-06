#include <arch/task.h>
#include <platform/loongarch_virt.h>
#include <string.h>
#include "../../kernel/sched/private.h"
void la_kernel_thread_trampoline(void);
void la_trap_return(void);
enum arch_context_status arch_context_init(struct arch_switch_context *context,
    uintptr_t stack, void (*entry)(void *), void *argument, void *task)
{
    if (!context || !stack || (stack & 15) || !entry || !task) return ARCH_CONTEXT_STATUS_INVALID_ARGUMENT;
    memset(context, 0, sizeof(*context)); context->ra=(uintptr_t)la_kernel_thread_trampoline;
    context->sp=stack; context->tp=(uintptr_t)task;
    context->s[0]=(uintptr_t)entry; context->s[1]=(uintptr_t)argument;
    return ARCH_CONTEXT_STATUS_OK;
}
enum arch_context_status arch_context_init_user(struct arch_switch_context *context, uintptr_t frame, void *task)
{
    if (!context || !frame || (frame&15) || !task) return ARCH_CONTEXT_STATUS_INVALID_ARGUMENT;
    memset(context, 0, sizeof(*context)); context->ra=(uintptr_t)la_trap_return;
    context->sp=frame; context->tp=(uintptr_t)task; return ARCH_CONTEXT_STATUS_OK;
}
void arch_fpu_switch(struct arch_fpu_state *previous, struct arch_fpu_state *next)
{
    /* 首阶段不允许任务借用浮点/向量寄存器，切换时保持 EUEN 禁用。 */
    if ((previous && previous->saved) || (next && next->saved)) la_virt_fatal("unsupported FPU owner");
    __asm__ volatile("csrwr $zero, 2" ::: "memory");
}
void arch_process_prepare_initial(struct kernel_task *task, uintptr_t entry, uintptr_t stack, uintptr_t tls)
{
    struct arch_trap_frame *frame=(void *)(task->stack_high-sizeof(*frame));
    memset(frame, 0, sizeof(*frame)); frame->regs[3]=stack; frame->regs[2]=tls;
    frame->era=entry; frame->prmd=7; frame->kernel_tp=(uintptr_t)task;
    task->fpu.saved=0;
}
void arch_process_prepare_exec(struct kernel_task *task, uintptr_t entry, uintptr_t stack, uintptr_t tls)
{ arch_process_prepare_initial(task, entry, stack, tls); __asm__ volatile("csrwr $zero, 2" ::: "memory"); }
enum kernel_scheduler_status arch_process_prepare_clone(struct kernel_task *child, struct kernel_task *parent,
    const struct arch_trap_frame *old, uint64_t stack, int set_tls, uint64_t tls)
{
    struct arch_trap_frame *frame=(void *)(child->stack_high-sizeof(*frame));
    if ((uintptr_t)frame<child->stack_low || parent->fpu.saved) return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    *frame=*old; frame->regs[4]=0; frame->era+=4; frame->kernel_tp=(uintptr_t)child;
    frame->estat=0; frame->badv=0;
    if (stack) frame->regs[3]=stack;
    if (set_tls) frame->regs[2]=tls;
    return arch_context_init_user(&child->context,(uintptr_t)frame,child)==ARCH_CONTEXT_STATUS_OK ?
        KERNEL_SCHEDULER_STATUS_OK : KERNEL_SCHEDULER_STATUS_INVALID_STATE;
}
int arch_direct_map_va_to_pa(uint64_t value, uint64_t size, uint64_t *address)
{
    if (!size || value < LA_DIRECT_BASE || value-LA_DIRECT_BASE >= (UINT64_C(1)<<48) ||
        size > (UINT64_C(1)<<48)-(value-LA_DIRECT_BASE) || !address) return 1;
    *address=value-LA_DIRECT_BASE; return 0;
}
