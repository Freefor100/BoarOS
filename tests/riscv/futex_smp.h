/* The shared observer already holds task identity and the control-page owner. */
#include <kernel/futex.h>
static uint64_t futex_reads[4],futex_updates[4],futex_keys[4];
enum kernel_mm_status __real_kernel_mm_prepare_futex(struct kernel_mm *,const uint64_t *,unsigned,int,unsigned,uint32_t,struct kernel_futex_key *,struct kernel_mm_user_pin *);
enum kernel_mm_status __wrap_kernel_mm_prepare_futex(struct kernel_mm *mm,const uint64_t *addresses,unsigned count,int shared,unsigned index,uint32_t access,struct kernel_futex_key *keys,struct kernel_mm_user_pin *pin)
{
    /* Exit/exec cleanup keeps its existing CPU0 service execution. */
    int cpu=pipe_observe_cpu(1);
    enum kernel_mm_status result=__real_kernel_mm_prepare_futex(mm,addresses,count,shared,index,access,keys,pin);
    if(cpu>=0){check(kernel_cpu_current()->logical_id==(unsigned)cpu);__atomic_fetch_add(access==KERNEL_MM_WRITE?&futex_updates[cpu]:access?&futex_reads[cpu]:&futex_keys[cpu],1,__ATOMIC_RELAXED);}
    return result;
}
static void futex_observe_report(void)
{
    for(unsigned cpu=0;cpu<BOAROS_MAX_CPUS;cpu++) {
        virt_uart_puts("BoarOS: FUTEX caller cpu=");virt_uart_put_hex(cpu);
        virt_uart_puts(" reads=");virt_uart_put_hex(futex_reads[cpu]);
        virt_uart_puts(" updates=");virt_uart_put_hex(futex_updates[cpu]);
        virt_uart_puts(" keys=");virt_uart_put_hex(futex_keys[cpu]);virt_uart_puts("\n");
    }
}
