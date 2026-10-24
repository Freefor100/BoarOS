/* Private composition observations; production and timed kernels omit this file. */
#include <kernel/socket.h>
#include <kernel/task_state.h>
static uint64_t socket_calls[4],socket_copies[4],socket_pins[4],socket_waits[4],socket_protocol_calls;
static uint64_t socket_holds[4];
static struct kernel_wait_node *socket_held_node;
static void relinquish(void);
static void socket_hold(unsigned mode)
{
    kernel_pid_t tid=0;
    if(!control || composition_load(&control->socket_mode)!=mode ||
       kernel_task_tid(kernel_task_current(),&tid) || (uint64_t)tid!=composition_load(&control->socket_tid))return;
    uint64_t expected=0;
    if(!__atomic_compare_exchange_n(&control->socket_entered,&expected,1,0,__ATOMIC_RELEASE,__ATOMIC_RELAXED))return;
    socket_holds[mode-1]++;
    while(!composition_load(&control->socket_release))relinquish();
}
static unsigned socket_issuing_cpu(void)
{
    struct kernel_task *task=kernel_task_current();uint64_t mask=0;
    check(task && kernel_task_has_user_context(task) && !kernel_task_affinity_get(task,0,&mask));
    unsigned cpu=kernel_cpu_current()->logical_id;
    check(cpu<4 && mask && !(mask&(mask-1)) && mask==(UINT64_C(1)<<cpu));
    return cpu;
}
static void socket_observe_copy(size_t bytes,int pin)
{
    struct kernel_task *task=kernel_task_current();
    if(!task || (!task->socket_read_request && !task->socket_write_request))return;
    unsigned cpu=socket_issuing_cpu();
    __atomic_fetch_add(pin?&socket_pins[cpu]:&socket_copies[cpu],pin?1:bytes,__ATOMIC_RELAXED);
}
int __real_kernel_socket_reserve_read(struct kernel_socket *,struct kernel_task *,struct kernel_socket_read_request *,struct kernel_open_file_description **,uint32_t,int,struct kernel_socket_message *);
int __wrap_kernel_socket_reserve_read(struct kernel_socket *socket,struct kernel_task *task,
    struct kernel_socket_read_request *request,struct kernel_open_file_description **pin,uint32_t size,int nonblocking,struct kernel_socket_message *message)
{
    __atomic_fetch_add(&socket_calls[socket_issuing_cpu()],1,__ATOMIC_RELAXED);
    int result=__real_kernel_socket_reserve_read(socket,task,request,pin,size,nonblocking,message);
    if(request->socket)socket_hold(1);
    return result;
}
int __real_kernel_socket_stream_reserve(struct kernel_socket_write_request *,uint32_t,uint32_t);
int __wrap_kernel_socket_stream_reserve(struct kernel_socket_write_request *request,uint32_t size,uint32_t flags)
{
    __atomic_fetch_add(&socket_calls[socket_issuing_cpu()],1,__ATOMIC_RELAXED);
    return __real_kernel_socket_stream_reserve(request,size,flags);
}
int __real_kernel_socket_write_datagram(struct kernel_open_file_description **,struct kernel_mm *,const struct kernel_uaccess_iovec *,size_t,uint64_t,uint32_t,const struct kernel_socket_address *,struct kernel_socket_message *);
int __wrap_kernel_socket_write_datagram(struct kernel_open_file_description **pin,struct kernel_mm *mm,const struct kernel_uaccess_iovec *iov,size_t count,uint64_t bytes,uint32_t flags,const struct kernel_socket_address *destination,struct kernel_socket_message *message)
{
    __atomic_fetch_add(&socket_calls[socket_issuing_cpu()],1,__ATOMIC_RELAXED);
    return __real_kernel_socket_write_datagram(pin,mm,iov,count,bytes,flags,destination,message);
}
int __real_kernel_socket_write_unix(struct kernel_open_file_description **,struct kernel_mm *,struct kernel_uaccess_iov_cursor *,uint64_t,uint32_t,int *,struct kernel_socket_message *);
int __wrap_kernel_socket_write_unix(struct kernel_open_file_description **pin,struct kernel_mm *mm,struct kernel_uaccess_iov_cursor *cursor,uint64_t bytes,uint32_t flags,int *fault,struct kernel_socket_message *message)
{
    __atomic_fetch_add(&socket_calls[socket_issuing_cpu()],1,__ATOMIC_RELAXED);
    return __real_kernel_socket_write_unix(pin,mm,cursor,bytes,flags,fault,message);
}
int __real_kernel_socket_wait_ready(struct kernel_socket *,uint32_t,uint64_t,uint32_t);
int __wrap_kernel_socket_wait_ready(struct kernel_socket *socket,uint32_t events,uint64_t target,uint32_t flags)
{
    if(kernel_scheduler_user_service_active())check(kernel_cpu_current()->logical_id==0);
    else __atomic_fetch_add(&socket_waits[socket_issuing_cpu()],1,__ATOMIC_RELAXED);
    return __real_kernel_socket_wait_ready(socket,events,target,flags);
}
struct kernel_socket_service_result __real_kernel_socket_service_owned(const struct kernel_socket_protocol_scope *,struct kernel_socket_service_budget);
struct kernel_socket_service_result __wrap_kernel_socket_service_owned(const struct kernel_socket_protocol_scope *scope,struct kernel_socket_service_budget budget)
{
    check(kernel_cpu_current()->logical_id==0);socket_protocol_calls++;
    return __real_kernel_socket_service_owned(scope,budget);
}
enum kernel_uaccess_status __real_kernel_copy_from_user(struct kernel_mm *,void *,uint64_t,size_t,size_t *);
enum kernel_uaccess_status __wrap_kernel_copy_from_user(struct kernel_mm *mm,void *to,uint64_t from,size_t size,size_t *copied)
{
    enum kernel_uaccess_status result=__real_kernel_copy_from_user(mm,to,from,size,copied);
    socket_observe_copy(*copied,0);return result;
}
void __real_kernel_wait_cursor_wake(struct kernel_wait_cursor *,int);
void __wrap_kernel_wait_cursor_wake(struct kernel_wait_cursor *cursor,int all)
{
    kernel_pid_t tid=0;
    if(cursor->node && cursor->node->callback && control && composition_load(&control->socket_mode)==3 &&
       !kernel_task_tid(kernel_task_current(),&tid) && (uint64_t)tid==composition_load(&control->socket_tid)) {
        __atomic_store_n(&socket_held_node,cursor->node,__ATOMIC_RELEASE);socket_hold(3);
        __atomic_store_n(&socket_held_node,0,__ATOMIC_RELEASE);
    }
    __real_kernel_wait_cursor_wake(cursor,all);
}
static void queued_source_quiesce(void);
void __real_kernel_wait_backend_quiesce(void);
void __wrap_kernel_wait_backend_quiesce(void)
{
    queued_source_quiesce();
    kernel_pid_t tid=0;
    if(control && composition_load(&control->socket_mode)==3 &&
       !kernel_task_tid(kernel_task_current(),&tid) && (uint64_t)tid==composition_load(&control->socket_waiter_tid)) {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        struct kernel_wait_node *held=__atomic_load_n(&socket_held_node,__ATOMIC_ACQUIRE);
        if(!composition_load(&control->socket_release)) {
            check(held && held->references!=0);
            composition_store(&control->socket_waited,1);
        }
    }
    __real_kernel_wait_backend_quiesce();
}
static void socket_observe_report(void)
{
    for(unsigned cpu=0;cpu<BOAROS_MAX_CPUS;cpu++) {
        virt_uart_puts("BoarOS: SOCKET caller cpu=");virt_uart_put_hex(cpu);
        virt_uart_puts(" operations=");virt_uart_put_hex(socket_calls[cpu]);
        virt_uart_puts(" copy-bytes=");virt_uart_put_hex(socket_copies[cpu]);
        virt_uart_puts(" pins=");virt_uart_put_hex(socket_pins[cpu]);
        virt_uart_puts(" waits=");virt_uart_put_hex(socket_waits[cpu]);virt_uart_puts("\n");
    }
    virt_uart_puts("BoarOS: SOCKET controlled rx=");virt_uart_put_hex(socket_holds[0]);
    virt_uart_puts(" tx-pin=");virt_uart_put_hex(socket_holds[1]);virt_uart_puts(" callback=");virt_uart_put_hex(socket_holds[2]);virt_uart_puts("\n");
    virt_uart_puts("BoarOS: SOCKET CPU0 protocol-calls=");virt_uart_put_hex(socket_protocol_calls);virt_uart_puts("\n");
}
