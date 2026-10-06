#include <kernel/ns16550.h>
#include <kernel/heap.h>
#include <kernel/scheduler.h>
#include <kernel/errno.h>
#include <kernel/log.h>
#include <platform/loongarch_virt.h>
void la_fault_injection_set(int);
int __real_ns16550_start(struct ns16550_port **,struct kernel_heap *,const struct dtb_uart_info *,
    volatile void *,uint64_t,const struct ns16550_irq_ops *);
enum kernel_heap_status __real_kernel_heap_allocate_zeroed(struct kernel_heap *,size_t,size_t,void **);
enum kernel_scheduler_status __real_kernel_thread_create_joinable(void (*)(void *),void *,struct kernel_thread_join *);
static int constructing;
static unsigned allocations;
static int reject_irq(void *context,uint32_t source,void (*handler)(void *),void *owner)
{ (void)context;(void)source;(void)handler;(void)owner;return 0; }
int __wrap_ns16550_start(struct ns16550_port **owner,struct kernel_heap *heap,const struct dtb_uart_info *info,
    volatile void *mapping,uint64_t frequency,const struct ns16550_irq_ops *irq)
{
    struct ns16550_irq_ops copy=*irq;
    if(UART_FAIL_CASE==4)copy.register_irq=reject_irq;
    constructing=1;
    int error=__real_ns16550_start(owner,heap,info,mapping,frequency,&copy);
    constructing=0;
    if(UART_FAIL_CASE==5) {
        if(error || !*owner)la_virt_fatal("UART fatal setup");
        (void)kernel_log_action(6,0,1,0,0,0);
        for(unsigned i=0;i<2048;i++)(void)ns16550_console('Q');
        la_virt_fatal("UART emergency bypass passed");
    }
    if(error!=(UART_FAIL_CASE==4 ? -KERNEL_EIO : -KERNEL_ENOMEM) || *owner || ns16550_console('X'))
        la_virt_fatal("UART construction owner");
    la_virt_puts("LA UART heap/task/stack/IRQ rollback passed\n");
    return error;
}
enum kernel_heap_status __wrap_kernel_heap_allocate_zeroed(struct kernel_heap *heap,size_t count,size_t size,void **out)
{
    if(constructing && UART_FAIL_CASE<2 && allocations++==UART_FAIL_CASE) {
        *out=0;return KERNEL_HEAP_STATUS_EMPTY;
    }
    return __real_kernel_heap_allocate_zeroed(heap,count,size,out);
}
enum kernel_scheduler_status __wrap_kernel_thread_create_joinable(void (*entry)(void *),void *argument,struct kernel_thread_join *join)
{
    if(constructing && (UART_FAIL_CASE==2 || UART_FAIL_CASE==3))la_fault_injection_set(UART_FAIL_CASE-2);
    enum kernel_scheduler_status result=__real_kernel_thread_create_joinable(entry,argument,join);
    la_fault_injection_set(-1);return result;
}
