#include <arch/task.h>
#include <arch/timer.h>
#include <platform/loongarch_virt.h>
#include <kernel/task.h>
#include <kernel/tick.h>
#include <kernel/time.h>
#include <kernel/syscall.h>
#include <kernel/errno.h>
#include <kernel/signal.h>
#include <kernel/console.h>
#include <platform/loongarch_pci.h>
void la_trap_entry(void);
void la_trap_initialize(void)
{
    uint64_t entry=(uintptr_t)la_trap_entry;
    __asm__ volatile("csrwr %0, 0xc\ncsrwr $zero, 0x30" : "+r"(entry) :: "memory");
}
static void fault(struct arch_trap_frame *frame, uint32_t signal)
{ kernel_user_thread_exit(KERNEL_THREAD_EXIT_SIGNAL,signal,frame->badv); }
void la_trap_dispatch(struct arch_trap_frame *frame)
{
    uint64_t code=(frame->estat>>16)&63;
    int user=(frame->prmd&3)==3;
    uint64_t pending=0;
    if(!code) {
        uint64_t enabled;__asm__ volatile("csrrd %0, 4":"=r"(enabled));
        /* ESTAT 包含被 mask 的外设；timer trap 不能顺便分发未启用的来源。 */
        pending=frame->estat&enabled&0x1fff;
    }
    int external_handled=!code && (pending&4);
    if(external_handled) la_virt_irq_dispatch();
    if (!code && (pending&(UINT64_C(1)<<11))) {
        uint64_t elapsed;
        if (la_timer_interrupt(&elapsed)!=ARCH_TIMER_STATUS_OK) la_virt_fatal("timer interrupt");
        if (elapsed) { kernel_tick_advance(elapsed); kernel_time_update_coarse(); kernel_scheduler_charge_ticks(elapsed,user); }
        if (kernel_scheduler_expire_deadlines(arch_time_read())!=KERNEL_SCHEDULER_STATUS_OK)
            la_virt_fatal("timer deadlines");
        /* 首阶段 UART 未启用外部 IRQ，timer 必须唤醒已经睡眠的输入 owner。 */
        kernel_console_poll_input();
        if (kernel_scheduler_on_tick(elapsed)!=KERNEL_SCHEDULER_STATUS_OK)
            la_virt_fatal("timer scheduler");
        return;
    }
    if(external_handled) return;
    if (user && (code==1 || code==2 || code==3 || code==4 || code==5 || code==6 || code==7)) {
        uint32_t access=(code==3 || code==6) ? KERNEL_MM_EXECUTE : ((code==2 || code==4) ? KERNEL_MM_WRITE : KERNEL_MM_READ);
        enum kernel_mm_status status=kernel_scheduler_resolve_current_user_fault(frame->badv,access);
        if (status==KERNEL_MM_STATUS_OK) return;
        if (status==KERNEL_MM_STATUS_NO_MEMORY)
            kernel_user_thread_exit(KERNEL_THREAD_EXIT_RESOURCE,
                                    KERNEL_THREAD_RESOURCE_NO_MEMORY,frame->badv);
        if (status==KERNEL_MM_STATUS_NOT_MAPPED || status==KERNEL_MM_STATUS_ACCESS)
            fault(frame,11);
        if (status==KERNEL_MM_STATUS_BUS_FAULT) fault(frame,7);
        la_virt_fatal("fault owner");
    }
    if (user && code==11) {
        struct kernel_syscall_request request={.number=frame->regs[11]};
        for (unsigned i=0;i<6;i++) request.arguments[i]=frame->regs[4+i];
        struct kernel_syscall_result result;
        if (kernel_syscall_dispatch(kernel_task_current(),&request,&result)!=KERNEL_SYSCALL_STATUS_OK) la_virt_fatal("syscall dispatch");
        switch (result.action) {
        case KERNEL_SYSCALL_ACTION_EXIT_GROUP: kernel_user_group_exit(KERNEL_THREAD_EXIT_SYSCALL,result.value,0);
        case KERNEL_SYSCALL_ACTION_EXIT: kernel_user_thread_exit(KERNEL_THREAD_EXIT_SYSCALL,result.value,0);
        case KERNEL_SYSCALL_ACTION_YIELD:
            if (kernel_scheduler_yield_current()!=KERNEL_SCHEDULER_STATUS_OK) la_virt_fatal("yield");
            result.value=0; break;
        case KERNEL_SYSCALL_ACTION_CLONE:
            /* LA syscall ABI 的 child_tid/TLS 顺序与通用 clone 接口相反。 */
            if (arch_process_clone_current(frame,request.arguments[0],request.arguments[1],
                request.arguments[2],request.arguments[4],request.arguments[3],&result.value)!=KERNEL_SCHEDULER_STATUS_OK)
                la_virt_fatal("clone");
            break;
        case KERNEL_SYSCALL_ACTION_WAIT4:
            if (kernel_scheduler_wait4_current((int32_t)request.arguments[0],request.arguments[1],
                request.arguments[2],request.arguments[3],&result.value)!=KERNEL_SCHEDULER_STATUS_OK)
                la_virt_fatal("wait4");
            break;
        case KERNEL_SYSCALL_ACTION_EXEC:
            if (kernel_scheduler_exec_commit()!=KERNEL_SCHEDULER_STATUS_OK) la_virt_fatal("exec commit");
            return;
        case KERNEL_SYSCALL_ACTION_RETURN: break;
        default: la_virt_fatal("unsupported internal syscall action");
        }
        if (result.value==-KERNEL_ERESTARTSYS) {
            kernel_signal_note_syscall_restart(kernel_task_current()); return;
        }
        frame->regs[4]=(uint64_t)result.value; frame->era+=4; return;
    }
    if (user) fault(frame,code==12 ? 5 : ((code==8 || code==9) ? 7 : 4));
    la_virt_puts("trap code=");la_virt_hex(code);la_virt_puts(" pc=");la_virt_hex(frame->era);la_virt_puts(" badv=");la_virt_hex(frame->badv);la_virt_puts("\n");
    la_virt_fatal("kernel trap");
}
void la_trap_return_prepare(struct arch_trap_frame *frame)
{
    if ((frame->prmd&3)==3) {
        kernel_task_prepare_user_return();
        struct kernel_signal_delivery delivery;
        struct kernel_task *task=kernel_task_current();
        enum kernel_signal_select_result selected=kernel_signal_select(task,&delivery);
        if (selected==KERNEL_SIGNAL_SELECT_EXIT)
            kernel_user_thread_exit(delivery.exit_reason,delivery.exit_status,delivery.exit_detail);
        if (selected==KERNEL_SIGNAL_SELECT_HANDLER) la_virt_fatal("unregistered handler ABI");
        enum kernel_signal_restart restart=kernel_signal_restart_decide(task,0,0);
        if (restart==KERNEL_SIGNAL_RESTART_BLOCK) frame->regs[11]=128;
        else if (restart==KERNEL_SIGNAL_RESTART_INTERRUPTED) { frame->regs[4]=(uint64_t)(int64_t)-KERNEL_EINTR;frame->era+=4; }
    }
    else kernel_scheduler_prepare_idle_return();
}
