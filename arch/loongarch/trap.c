#include <arch/task.h>
#include <arch/timer.h>
#include <arch/loongarch/signal.h>
#include <arch/loongarch/fpu.h>
#include <platform/loongarch_virt.h>
#include <kernel/task.h>
#include <kernel/tick.h>
#include <kernel/time.h>
#include <kernel/syscall.h>
#include <kernel/errno.h>
#include <kernel/signal.h>
#include <kernel/console.h>
#include <kernel/uaccess.h>
#include <platform/loongarch_pci.h>
#include <string.h>
void la_trap_entry(void);
void la_trap_initialize(void)
{
    uint64_t entry=(uintptr_t)la_trap_entry;
    __asm__ volatile("csrwr %0, 0xc\ncsrwr $zero, 0x30" : "+r"(entry) :: "memory");
}
static void fault(uint32_t signal, int32_t code, uint64_t address)
{ kernel_signal_force_fault(kernel_task_current(),signal,code,address); }
static int read_instruction(struct kernel_mm *mm,uint64_t pc,uint32_t *instruction)
{
    struct kernel_mm_mapping mapping;void *page;
    if((pc&3) || kernel_user_range_check(pc,sizeof(*instruction))!=KERNEL_UACCESS_STATUS_OK)return 0;
    enum kernel_mm_status status=kernel_mm_lookup(mm,pc,&mapping);
    if(status==KERNEL_MM_STATUS_NOT_MAPPED || status==KERNEL_MM_STATUS_ADDRESS_SPACE)return 0;
    if(status!=KERNEL_MM_STATUS_OK)la_virt_fatal("instruction MM owner");
    if((mapping.permissions&(KERNEL_MM_USER|KERNEL_MM_EXECUTE))!=(KERNEL_MM_USER|KERNEL_MM_EXECUTE))return 0;
    /* 已取指的页驻留且 trap 中不切换；以 EXEC 资格借用 owner，不放宽数据 READ。 */
    if(physical_page_resolve(mm->allocator,mapping.physical_address&~BOAROS_PAGE_MASK,&page)!=PHYSICAL_PAGE_STATUS_OK)
        la_virt_fatal("instruction page owner");
    memcpy(instruction,(unsigned char *)page+(pc&BOAROS_PAGE_MASK),sizeof(*instruction));
    return 1;
}
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
    if(user && code==15) {la_fpu_first_use();return;}
    if(user && code==18) {fault(8,(int32_t)la_fpu_take_exception(),frame->era);return;}
    if (user && (code==1 || code==2 || code==3 || code==4 || code==5 || code==6 || code==7)) {
        uint32_t access=(code==3 || code==6) ? KERNEL_MM_EXECUTE : ((code==2 || code==4) ? KERNEL_MM_WRITE : KERNEL_MM_READ);
        enum kernel_mm_status status=kernel_scheduler_resolve_current_user_fault(frame->badv,access);
        if (status==KERNEL_MM_STATUS_OK) return;
        if (status==KERNEL_MM_STATUS_NO_MEMORY)
            kernel_user_thread_exit(KERNEL_THREAD_EXIT_RESOURCE,
                                    KERNEL_THREAD_RESOURCE_NO_MEMORY,frame->badv);
        if (status==KERNEL_MM_STATUS_NOT_MAPPED || status==KERNEL_MM_STATUS_ACCESS) {
            la_signal_note_address_error(access);
            fault(11,status==KERNEL_MM_STATUS_NOT_MAPPED ? 1 : 2,frame->badv);return;
        }
        if (status==KERNEL_MM_STATUS_BUS_FAULT) { fault(7,2,frame->badv);return; }
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
        case KERNEL_SYSCALL_ACTION_SIGNAL_RETURN:
            la_signal_restore_current(frame);return;
        case KERNEL_SYSCALL_ACTION_RETURN: break;
        default: la_virt_fatal("unsupported internal syscall action");
        }
        if (result.value==-KERNEL_ERESTARTSYS) {
            kernel_signal_note_syscall_restart(kernel_task_current()); return;
        }
        frame->regs[4]=(uint64_t)result.value; frame->era+=4; return;
    }
    if (user) {
        if(code==12) {
            struct kernel_mm *mm;uint32_t instruction;
            if(kernel_task_mm_borrow_mutable(kernel_task_current(),&mm)!=KERNEL_TASK_STATUS_OK)
                la_virt_fatal("break MM owner");
            if(!read_instruction(mm,frame->era,&instruction))
                fault(11,128,0);
            else {
                /* Linux do_bp 用 break immediate 区分软件整数溢出与除零。 */
                uint32_t immediate=instruction&0x7fff;
                if(immediate==6 || immediate==7) fault(8,immediate==6 ? 2 : 1,frame->era);
                else fault(5,1,frame->era);
            }
        }
        else if(code==8 || code==9) fault(7,code==8 ? 2 : 1,frame->badv);
        else fault(4,128,0); /* Linux LA do_ri/disabled ISA 的 SI_KERNEL 来源。 */
        return;
    }
    la_virt_puts("trap code=");la_virt_hex(code);la_virt_puts(" pc=");la_virt_hex(frame->era);la_virt_puts(" badv=");la_virt_hex(frame->badv);la_virt_puts("\n");
    la_virt_fatal("kernel trap");
}
void la_trap_return_prepare(struct arch_trap_frame *frame)
{
    if ((frame->prmd&3)==3) {
        kernel_task_prepare_user_return();
        la_signal_prepare_user_return(frame);
    }
    else kernel_scheduler_prepare_idle_return();
}
