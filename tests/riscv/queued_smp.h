#include "../../kernel/signal_pending.h"
#include <kernel/signal_fd.h>
static uint64_t queued_waits[4],queued_reads[4],queued_queries[4],queued_source_holds;
static struct kernel_task *queued_releasing;
enum kernel_signal_status __real_kernel_signal_wait_take(struct kernel_task *,struct kernel_signal_wait_info *);
enum kernel_signal_status __wrap_kernel_signal_wait_take(struct kernel_task *task,struct kernel_signal_wait_info *info)
{
    int cpu=pipe_observe_cpu(0);if(cpu>=0)__atomic_fetch_add(&queued_waits[cpu],1,__ATOMIC_RELAXED);
    return __real_kernel_signal_wait_take(task,info);
}
int __real_kernel_signal_fd_take(struct kernel_task *,const uint64_t *,struct kernel_signal_wait_info *,int);
int __wrap_kernel_signal_fd_take(struct kernel_task *task,const uint64_t *mask,struct kernel_signal_wait_info *info,int nonblock)
{
    int cpu=pipe_observe_cpu(0);if(cpu>=0)__atomic_fetch_add(&queued_reads[cpu],1,__ATOMIC_RELAXED);
    int result=__real_kernel_signal_fd_take(task,mask,info,nonblock);
    if(cpu>=0)check(kernel_cpu_current()->logical_id==(unsigned)cpu);
    return result;
}
uint32_t __real_kernel_signal_fd_poll(struct kernel_open_file_description *,struct kernel_wait_queue **);
uint32_t __wrap_kernel_signal_fd_poll(struct kernel_open_file_description *file,struct kernel_wait_queue **queue)
{
    int cpu=pipe_observe_cpu(1);if(cpu>=0)__atomic_fetch_add(&queued_queries[cpu],1,__ATOMIC_RELAXED);
    return __real_kernel_signal_fd_poll(file,queue);
}
void __real_signal_pending_prepare(struct kernel_task *,int,struct kernel_heap *,const struct kernel_signal_wait_info *,int,struct signal_pending_prepared *);
void __wrap_signal_pending_prepare(struct kernel_task *task,int shared,struct kernel_heap *heap,const struct kernel_signal_wait_info *info,int supplied,struct signal_pending_prepared *p)
{
    __real_signal_pending_prepare(task,shared,heap,info,supplied,p);
    if(p->record)socket_hold(4);
}
enum kernel_signal_status __real_kernel_signal_release_table(struct kernel_task *);
enum kernel_signal_status __wrap_kernel_signal_release_table(struct kernel_task *task)
{
    int selected=control&&composition_load(&control->socket_mode)==3&&(uint64_t)task->tid==composition_load(&control->socket_waiter_tid);
    if(selected)__atomic_store_n(&queued_releasing,task,__ATOMIC_RELEASE);
    enum kernel_signal_status result=__real_kernel_signal_release_table(task);
    if(selected)__atomic_store_n(&queued_releasing,0,__ATOMIC_RELEASE);
    return result;
}
static void queued_source_quiesce(void)
{
    struct kernel_task *task=__atomic_load_n(&queued_releasing,__ATOMIC_ACQUIRE);
    if(!task || !control || composition_load(&control->socket_mode)!=3)return;
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    struct kernel_signal_table *table=(void *)task->signal_table_address;
    struct kernel_wait_node *held=__atomic_load_n(&socket_held_node,__ATOMIC_ACQUIRE);
    if(table&&table->signalfd_waiters.closed&&table->signalfd_waiters.borrows) {
        check(held&&held->references&&!held->queue);
        if(!composition_load(&control->socket_waited)){queued_source_holds++;composition_store(&control->socket_waited,1);}
    }
}
static void queued_observe_report(void)
{
    for(unsigned cpu=0;cpu<BOAROS_MAX_CPUS;cpu++) {
        virt_uart_puts("BoarOS: QUEUED caller cpu=");virt_uart_put_hex(cpu);
        virt_uart_puts(" waits=");virt_uart_put_hex(queued_waits[cpu]);virt_uart_puts(" reads=");virt_uart_put_hex(queued_reads[cpu]);
        virt_uart_puts(" queries=");virt_uart_put_hex(queued_queries[cpu]);virt_uart_puts("\n");
    }
    virt_uart_puts("BoarOS: QUEUED source-holds=");virt_uart_put_hex(queued_source_holds);virt_uart_puts(" prepared-holds=");virt_uart_put_hex(socket_holds[3]);virt_uart_puts("\n");
}
