#include <kernel/event_fd.h>
static uint64_t event_io[4],event_completed[4],event_timer_calls[4],event_expiry,event_pins[4];
static struct kernel_task *event_active[32];
int64_t __real_kernel_event_fd_io(struct kernel_open_file_description *,struct kernel_mm *,const struct kernel_uaccess_iovec *,size_t,uint64_t,int);
int64_t __wrap_kernel_event_fd_io(struct kernel_open_file_description *file,struct kernel_mm *mm,const struct kernel_uaccess_iovec *iov,size_t n,uint64_t count,int writing)
{
    int cpu=pipe_observe_cpu(0);if(cpu>=0)__atomic_fetch_add(&event_io[cpu],1,__ATOMIC_RELAXED);
    unsigned slot=32;
    if(cpu>=0)for(unsigned i=0;i<32;i++){struct kernel_task *empty=0;if(__atomic_compare_exchange_n(&event_active[i],&empty,kernel_task_current(),0,__ATOMIC_RELEASE,__ATOMIC_RELAXED)){slot=i;break;}}
    if(cpu>=0)check(slot<32);
    int64_t result=__real_kernel_event_fd_io(file,mm,iov,n,count,writing);
    if(cpu>=0){__atomic_store_n(&event_active[slot],0,__ATOMIC_RELEASE);check(kernel_cpu_current()->logical_id==(unsigned)cpu);if(result>0)__atomic_fetch_add(&event_completed[cpu],(uint64_t)result,__ATOMIC_RELAXED);}return result;
}
static void event_observe_copy(int pin)
{
 struct kernel_task *task=kernel_task_current();
 for(unsigned i=0;i<32;i++)if(__atomic_load_n(&event_active[i],__ATOMIC_ACQUIRE)==task && task){int cpu=pipe_observe_cpu(0);check(cpu>=0);if(pin)__atomic_fetch_add(&event_pins[cpu],1,__ATOMIC_RELAXED);return;}
}
enum kernel_syscall_status __real_syscall_handle_timerfd(struct kernel_task *,const struct kernel_syscall_request *,struct kernel_syscall_result *);
enum kernel_syscall_status __wrap_syscall_handle_timerfd(struct kernel_task *task,const struct kernel_syscall_request *req,struct kernel_syscall_result *res)
{
    int cpu=pipe_observe_cpu(0);if(cpu>=0)__atomic_fetch_add(&event_timer_calls[cpu],1,__ATOMIC_RELAXED);
    enum kernel_syscall_status result=__real_syscall_handle_timerfd(task,req,res);
    if(cpu>=0)check(kernel_cpu_current()->logical_id==(unsigned)cpu);
    return result;
}
void __real_kernel_timer_fd_expire(uint64_t);
void __wrap_kernel_timer_fd_expire(uint64_t now)
{check(!kernel_cpu_current()->logical_id);event_expiry++;__real_kernel_timer_fd_expire(now);}
static void event_observe_report(void)
{
 for(unsigned cpu=0;cpu<BOAROS_MAX_CPUS;cpu++){
  virt_uart_puts("BoarOS: EVENT caller cpu=");virt_uart_put_hex(cpu);virt_uart_puts(" io=");virt_uart_put_hex(event_io[cpu]);virt_uart_puts(" completed-bytes=");virt_uart_put_hex(event_completed[cpu]);virt_uart_puts(" pins=");virt_uart_put_hex(event_pins[cpu]);virt_uart_puts(" timers=");virt_uart_put_hex(event_timer_calls[cpu]);virt_uart_puts("\n");
 }
 virt_uart_puts("BoarOS: EVENT CPU0 expiry=");virt_uart_put_hex(event_expiry);virt_uart_puts("\n");
}
