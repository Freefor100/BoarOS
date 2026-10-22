/* Uses the composition observer's existing task identity and lifecycle. */
#include "../../fs/pipe_internal.h"
#include <kernel/open_file.h>
static uint64_t pipe_reads[4],pipe_writes[4],pipe_queries[4],pipe_epoll_queries[4],pipe_events[4];
static int pipe_observe_cpu(int allow_service)
{
    struct kernel_task *task=kernel_task_current();uint64_t mask=0;
    if(!control||!task||!kernel_task_has_user_context(task)||kernel_task_affinity_get(task,0,&mask)||!mask||(mask&(mask-1)))return -1;
    if(allow_service&&kernel_scheduler_user_service_active())return -1;
    unsigned cpu=kernel_cpu_current()->logical_id;check(cpu<4&&mask==(UINT64_C(1)<<cpu));return (int)cpu;
}
enum kernel_pipe_status __real_kernel_pipe_readv(struct kernel_pipe *,struct kernel_mm *,const struct kernel_uaccess_iovec *,size_t,uint64_t,uint32_t,int64_t *);
enum kernel_pipe_status __wrap_kernel_pipe_readv(struct kernel_pipe *pipe,struct kernel_mm *mm,const struct kernel_uaccess_iovec *iov,size_t n,uint64_t count,uint32_t flags,int64_t *result)
{
    int cpu=pipe_observe_cpu(0);if(cpu>=0)__atomic_fetch_add(&pipe_reads[cpu],1,__ATOMIC_RELAXED);
    return __real_kernel_pipe_readv(pipe,mm,iov,n,count,flags,result);
}
enum kernel_pipe_status __real_kernel_pipe_writev(struct kernel_pipe *,struct kernel_mm *,const struct kernel_uaccess_iovec *,size_t,uint64_t,uint32_t,struct kernel_pipe_result *);
enum kernel_pipe_status __wrap_kernel_pipe_writev(struct kernel_pipe *pipe,struct kernel_mm *mm,const struct kernel_uaccess_iovec *iov,size_t n,uint64_t count,uint32_t flags,struct kernel_pipe_result *result)
{
    int cpu=pipe_observe_cpu(0);if(cpu>=0)__atomic_fetch_add(&pipe_writes[cpu],1,__ATOMIC_RELAXED);
    return __real_kernel_pipe_writev(pipe,mm,iov,n,count,flags,result);
}
uint32_t __real_kernel_open_file_poll(struct kernel_open_file_description *,uint32_t,struct kernel_wait_queue **);
uint32_t __wrap_kernel_open_file_poll(struct kernel_open_file_description *file,uint32_t events,struct kernel_wait_queue **queue)
{
    int cpu=pipe_observe_cpu(1);
    if(cpu>=0) __atomic_fetch_add(kernel_task_current()->epoll_wait_request?&pipe_epoll_queries[cpu]:&pipe_queries[cpu],1,__ATOMIC_RELAXED);
    return __real_kernel_open_file_poll(file,events,queue);
}
static void pipe_observe_events(size_t count)
{
    struct kernel_task *task=kernel_task_current();
    if(!task||!task->epoll_wait_request)return;
    int cpu=pipe_observe_cpu(0);if(cpu>=0)__atomic_fetch_add(&pipe_events[cpu],count,__ATOMIC_RELAXED);
}
static void pipe_observe_report(void)
{
    for(unsigned cpu=0;cpu<BOAROS_MAX_CPUS;cpu++) {
        virt_uart_puts("BoarOS: PIPE caller cpu=");virt_uart_put_hex(cpu);
        virt_uart_puts(" reads=");virt_uart_put_hex(pipe_reads[cpu]);virt_uart_puts(" writes=");virt_uart_put_hex(pipe_writes[cpu]);
        virt_uart_puts(" queries=");virt_uart_put_hex(pipe_queries[cpu]);virt_uart_puts(" epoll-queries=");virt_uart_put_hex(pipe_epoll_queries[cpu]);
        virt_uart_puts(" event-bytes=");virt_uart_put_hex(pipe_events[cpu]);virt_uart_puts("\n");
    }
}
