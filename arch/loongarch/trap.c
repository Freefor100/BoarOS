#include <kernel/accounting.h>
#include <arch/context.h>
#include <arch/task.h>
#include <arch/timer.h>
#include <arch/loongarch/signal.h>
#include <arch/loongarch/fpu.h>
#include <platform/loongarch_virt.h>
#include <kernel/task.h>
#include <kernel/cpu.h>
#include <kernel/tick.h>
#include <kernel/time.h>
#include <kernel/syscall.h>
#include <kernel/errno.h>
#include <kernel/signal.h>
#include <kernel/console.h>
#include <kernel/uaccess.h>
#include <kernel/cost.h>
#include <platform/loongarch_pci.h>
#include <arch/loongarch/smp.h>
#include <string.h>
void la_trap_entry(void);
void la_kernel_stack_overflow(uint64_t sp)
{
    kernel_console_emergency_begin();
    la_virt_puts("LA exhausted kernel SP=");la_virt_hex(sp);la_virt_puts("\n");
    la_virt_fatal("kernel stack guard");
}
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
#if BOAROS_COST_DIAGNOSTICS
    kernel_cost_irq_disabled(kernel_cost_clock());
#endif
    uint64_t code=(frame->estat>>16)&63;
    int user=(frame->prmd&3)==3;
#if BOAROS_USER_SMP
    if(user){kernel_cpu_current()->user_traps++;if(!code)kernel_cpu_current()->user_local_interrupts++;}
#endif
    struct kernel_cpu_account_scope accounting __attribute__((cleanup(kernel_cpu_account_trap_end)));
    kernel_cpu_account_trap_begin(&accounting,user,!code);
    if(!user && code>=1 && code<=7 && frame->badv>=ARCH_KERNEL_STACK_WINDOW_BASE &&
       frame->badv-ARCH_KERNEL_STACK_WINDOW_BASE<ARCH_KERNEL_STACK_WINDOW_SIZE &&
       (frame->badv-ARCH_KERNEL_STACK_WINDOW_BASE)%ARCH_KERNEL_STACK_SLOT_SIZE<BOAROS_PAGE_SIZE)
        la_virt_fatal("kernel stack guard");
    uint64_t pending=0;
    if(!code) {
        uint64_t enabled;__asm__ volatile("csrrd %0, 4":"=r"(enabled));
        /* ESTAT 包含被 mask 的外设；timer trap 不能顺便分发未启用的来源。 */
        pending=frame->estat&enabled&0x1fff;
    }
#if BOAROS_MAX_CPUS > 1
    if(!code && (pending&(1U<<12)))la_smp_handle_ipi();
#endif
    int external_handled=!code && (pending&4);
    if(external_handled) la_virt_irq_dispatch();
    if (!code && (pending&(UINT64_C(1)<<11))) {
        uint64_t elapsed;
        if (la_timer_interrupt(&elapsed)!=ARCH_TIMER_STATUS_OK) la_virt_fatal("timer interrupt");
#if BOAROS_MAX_CPUS > 1
        la_smp_timer_seen();
        if(kernel_cpu_current()->logical_id) {
            if(kernel_scheduler_on_tick(elapsed)!=KERNEL_SCHEDULER_STATUS_OK)la_virt_fatal("AP scheduler");
            return;
        }
#endif
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
#if BOAROS_MAX_CPUS > 1
    if(!code && (pending&(1U<<12)))return;
#endif
    if(user && (code==15 || code==16 || code==17)) {
        if(!la_fpu_first_use(code==15 ? 1 : code==16 ? 2 : 4))fault(4,1,frame->era);
        return;
    }
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
        /* 用户陷入已切到所属内核栈且关闭中断；在此核对一次真实执行者。 */
        struct kernel_task *caller=arch_current_thread_get();
        if(!caller || frame->kernel_tp!=(uintptr_t)caller ||
           kernel_cpu_current()->current!=caller)la_virt_fatal("syscall owner");
        struct kernel_syscall_request request={.number=frame->regs[11],.user_sp=frame->regs[3]};
        for (unsigned i=0;i<6;i++) request.arguments[i]=frame->regs[4+i];
        struct kernel_syscall_result result;
        if (kernel_syscall_dispatch(caller,&request,&result)!=KERNEL_SYSCALL_STATUS_OK) la_virt_fatal("syscall dispatch");
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
        case KERNEL_SYSCALL_ACTION_WAITID:
            if (kernel_scheduler_waitid_current((int32_t)request.arguments[0],
                (int64_t)(int32_t)(uint32_t)request.arguments[1],request.arguments[2],
                (uint32_t)request.arguments[3],request.arguments[4],&result.value)!=KERNEL_SCHEDULER_STATUS_OK)
                la_virt_fatal("waitid");
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
    /* 首错可能发生在日志/等待保护内；寄存器前缀也必须直接写UART。 */
    kernel_console_emergency_begin();
    la_virt_puts("trap code=");la_virt_hex(code);la_virt_puts(" pc=");la_virt_hex(frame->era);la_virt_puts(" badv=");la_virt_hex(frame->badv);
    /* 故障PC可能是合并的fatal分支；RA定位实际违反契约的调用点。 */
    la_virt_puts(" ra=");la_virt_hex(frame->regs[1]);
    la_virt_puts(" tp=");la_virt_hex(frame->regs[2]);la_virt_puts("\n");
    la_virt_fatal("kernel trap");
}
void la_trap_return_prepare(struct arch_trap_frame *frame)
{
    if ((frame->prmd&3)==3) {
#if BOAROS_USER_SMP
        do {
            if(kernel_task_prepare_user_return()) {
                la_signal_prepare_user_return(frame);
                if(kernel_scheduler_user_service_active())kernel_scheduler_leave_user_service();
            }
        } while(kernel_scheduler_user_return_pending());
        kernel_cpu_current()->user_returns++;
#else
        kernel_task_prepare_user_return();la_signal_prepare_user_return(frame);
#endif
        kernel_cpu_account_user_return();
#if BOAROS_COST_DIAGNOSTICS
        kernel_cost_user_return();
#endif
    }
    else kernel_scheduler_prepare_idle_return();
}
