static uint64_t altstack_frames[4], altstack_returns[4];
uint64_t __real_kernel_signal_stack_prepare(struct kernel_task *,uint64_t,uint64_t,struct kernel_signal_stack *);
uint64_t __wrap_kernel_signal_stack_prepare(struct kernel_task *task,uint64_t sp,uint64_t flags,struct kernel_signal_stack *saved)
{
    int cpu=control ? pipe_observe_cpu(0) : -1;
    uint64_t top=__real_kernel_signal_stack_prepare(task,sp,flags,saved);
    if(cpu>=0 && saved->size) {
        check(task==kernel_task_current() && kernel_cpu_current()->logical_id==(unsigned)cpu);
        __atomic_fetch_add(&altstack_frames[cpu],1,__ATOMIC_RELAXED);
    }
    return top;
}
void __real_riscv_signal_restore_current(struct riscv_trap_frame *);
void __wrap_riscv_signal_restore_current(struct riscv_trap_frame *frame)
{
    int cpu=control ? pipe_observe_cpu(0) : -1;
    __real_riscv_signal_restore_current(frame);
    if(cpu>=0) {
        check(kernel_cpu_current()->logical_id==(unsigned)cpu);
        __atomic_fetch_add(&altstack_returns[cpu],1,__ATOMIC_RELAXED);
    }
}
static void altstack_observe_report(void)
{
    for(unsigned cpu=0;cpu<BOAROS_MAX_CPUS;cpu++) {
        virt_uart_puts("BoarOS: ALTSTACK caller cpu=");virt_uart_put_hex(cpu);
        virt_uart_puts(" frames=");virt_uart_put_hex(altstack_frames[cpu]);
        virt_uart_puts(" returns=");virt_uart_put_hex(altstack_returns[cpu]);virt_uart_puts("\n");
    }
}
