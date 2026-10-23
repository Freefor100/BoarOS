#include <kernel/posix_timer.h>
static uint64_t posix_settings[4],posix_consumers[4];
enum kernel_syscall_status __real_syscall_handle_posix_timer(struct kernel_task *,const struct kernel_syscall_request *,struct kernel_syscall_result *);
enum kernel_syscall_status __wrap_syscall_handle_posix_timer(struct kernel_task *task,const struct kernel_syscall_request *request,struct kernel_syscall_result *result)
{
    int cpu=pipe_observe_cpu(0);
    enum kernel_syscall_status status=__real_syscall_handle_posix_timer(task,request,result);
    if(cpu>=0){check(kernel_cpu_current()->logical_id==(unsigned)cpu);__atomic_fetch_add(&posix_settings[cpu],1,__ATOMIC_RELAXED);}
    return status;
}
void __real_posix_timer_record_complete(struct signal_queue_record *);
void __wrap_posix_timer_record_complete(struct signal_queue_record *record)
{
    int cpu=pipe_observe_cpu(0);__real_posix_timer_record_complete(record);
    if(cpu>=0){check(kernel_cpu_current()->logical_id==(unsigned)cpu);__atomic_fetch_add(&posix_consumers[cpu],1,__ATOMIC_RELAXED);}
}
void __real_kernel_posix_timer_expire(uint64_t);
void __wrap_kernel_posix_timer_expire(uint64_t now)
{check(!kernel_cpu_current()->logical_id);__real_kernel_posix_timer_expire(now);}
void __real_scheduler_cpu_timer_expire(void);
void __wrap_scheduler_cpu_timer_expire(void)
{check(!kernel_cpu_current()->logical_id);__real_scheduler_cpu_timer_expire();}
static void posix_observe_report(void)
{
    for(unsigned cpu=0;cpu<BOAROS_MAX_CPUS;cpu++) {
        virt_uart_puts("BoarOS: POSIX caller cpu=");virt_uart_put_hex(cpu);
        virt_uart_puts(" settings=");virt_uart_put_hex(posix_settings[cpu]);
        virt_uart_puts(" consumers=");virt_uart_put_hex(posix_consumers[cpu]);virt_uart_puts("\n");
    }
    virt_uart_puts("BoarOS: POSIX expiry CPU0 OK\n");
}
