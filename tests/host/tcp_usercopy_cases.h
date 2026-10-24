/* TCP/lwIP、MM pin、syscall I/O均为生产实现；宿主只负责执行握手与等待。 */
#include <stdlib.h>
#include <kernel/time.h>
#include <kernel/random.h>
#include <kernel/socket.h>
#include <kernel/signal.h>
#include "lwip/init.h"
#include "lwip/mem.h"
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"
int kernel_random_available(void) { return 0; }
enum kernel_random_status kernel_random_fill(void *data,size_t size)
{ (void)data;(void)size;abort(); }
enum kernel_time_status kernel_time_deadline_from_monotonic(uint64_t time,uint64_t *out)
{ *out=time;return KERNEL_TIME_STATUS_OK; }
extern unsigned mm_test_parked(unsigned);
static struct kernel_files *tcp_wait_files;
static int socket_buffer_write(struct kernel_files *files,int fd,const void *bytes,uint32_t size,uint32_t flags)
{
    struct kernel_open_file_description *pin=NULL;int64_t error;
    assert(!kernel_files_pin(files,fd,&pin,&error) && !error);
    int result;
    if(kernel_socket_is_tcp(kernel_open_file_socket(pin))) {
        struct kernel_socket_write_request request={0};kernel_socket_stream_begin(&request,&pin);
        result=kernel_socket_stream_reserve(&request,size,flags);
        if(result>0)result=kernel_socket_stream_commit(&request,bytes,(uint32_t)result,flags);
        kernel_socket_stream_finish(&request);
    } else result=kernel_socket_write_packet_buffer(&pin,bytes,size,flags);
    assert(pin && !kernel_open_file_release(&pin));return result;
}
static _Thread_local unsigned tcp_wait_role,tcp_finishing_socket;
static struct kernel_wait_queue *tcp_wait_queue;
static uint64_t tcp_wait_generation;
static unsigned tcp_wait_active,tcp_wait_registered,tcp_wait_contender_acquired;
static unsigned tcp_wait_park_call,tcp_wait_finish_count,tcp_wait_relocked;
static unsigned tcp_wait_token_finished,tcp_wait_caller_done,tcp_wait_reserved_under_core,tcp_wait_publication_done;
static unsigned tcp_wait_contender_parked,tcp_wait_caller_parked;
static struct kernel_socket *tcp_wait_client;
static void **tcp_wait_allocations;
static size_t tcp_wait_allocation_count;
static int tcp_wait_writing;
static _Thread_local unsigned tcp_write_observing,tcp_write_editor;
static unsigned tcp_write_mode,tcp_write_edit_started,tcp_write_edit_closed,tcp_write_edit_done;
static unsigned tcp_write_worker_ready,tcp_write_queued_ready,tcp_write_worker_published,tcp_write_pins_returned;
static unsigned tcp_write_notifications,tcp_write_signal_count;
static unsigned tcp_write_park_baseline;
static uint64_t tcp_write_source_page;
static struct kernel_task *tcp_write_caller_task;
static struct kernel_socket *tcp_write_client_socket;
static struct kernel_socket_write_request *tcp_write_queued_request;
static _Thread_local struct kernel_mm_user_pin *tcp_write_pins[4],*tcp_write_returning;
static _Thread_local unsigned tcp_write_held,tcp_write_pin_count,tcp_write_unpin_count;
static _Thread_local unsigned tcp_write_misses,tcp_write_denied,tcp_write_conflicts,tcp_write_commits;
static _Thread_local uint32_t tcp_write_commit_bytes[4];
static _Thread_local unsigned tcp_write_commit_segments[4];
static void tcp_write_pin_observed(uint32_t,enum kernel_mm_status,struct kernel_mm_user_pin *);
static int tcp_protocol_held(void);
static _Thread_local unsigned unix_deadline_active;
static _Thread_local uint64_t unix_deadline_now;
static unsigned unix_deadline_prepared,unix_deadline_parked,unix_deadline_steal,unix_deadline_stolen;
static unsigned unix_deadline_reservations;
static unsigned unix_deadline_park_baseline;
static uint64_t unix_deadlines[2];
static struct kernel_wait_queue *unix_deadline_queue;
static struct kernel_socket *unix_deadline_sender;
static struct kernel_files *unix_deadline_files;
uint64_t __real_kernel_time_monotonic_ns(void);
/* 只推进本次读者的时钟；park、唤醒和抢读均执行生产代码。 */
uint64_t __wrap_kernel_time_monotonic_ns(void)
{ return unix_deadline_active?unix_deadline_now:__real_kernel_time_monotonic_ns(); }
static enum kernel_files_status tcp_wait_status;
static int64_t tcp_wait_bytes;
static _Thread_local unsigned tcp_timeout_observe,tcp_timeout_nonblock,tcp_timeout_receives,tcp_timeout_sends;
static unsigned tcp_nonblock_change,tcp_nonblock_attempted,tcp_nonblock_changed;
static struct kernel_files *tcp_nonblock_files;
static int tcp_nonblock_fd;
uint64_t __real_kernel_socket_receive_timeout(const struct kernel_socket *);
uint64_t __wrap_kernel_socket_receive_timeout(const struct kernel_socket *socket)
{ if(tcp_timeout_observe) tcp_timeout_receives++;return __real_kernel_socket_receive_timeout(socket); }
uint64_t __real_kernel_socket_send_timeout(const struct kernel_socket *);
uint64_t __wrap_kernel_socket_send_timeout(const struct kernel_socket *socket)
{ if(tcp_timeout_observe) tcp_timeout_sends++;return __real_kernel_socket_send_timeout(socket); }
static void tcp_nonblock_attempt(void)
{
    if(!tcp_timeout_observe || !tcp_nonblock_change) return;
    tcp_nonblock_change=0;
    __atomic_store_n(&tcp_nonblock_attempted,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&tcp_nonblock_changed,__ATOMIC_ACQUIRE)) sched_yield();
}
int __real_kernel_socket_reserve_read(struct kernel_socket *,struct kernel_task *,struct kernel_socket_read_request *,struct kernel_open_file_description **,uint32_t,int,struct kernel_socket_message *);
int __wrap_kernel_socket_reserve_read(struct kernel_socket *socket,struct kernel_task *task,
    struct kernel_socket_read_request *request,struct kernel_open_file_description **owner,uint32_t capacity,int nonblock,struct kernel_socket_message *message)
{
    tcp_nonblock_attempt();
    if(unix_deadline_active && ++unix_deadline_reservations>1) {
        unsigned round=unix_deadline_reservations-1;
        assert(round<=2);
        __atomic_store_n(&unix_deadline_steal,round,__ATOMIC_RELEASE);
        while(__atomic_load_n(&unix_deadline_stolen,__ATOMIC_ACQUIRE)<round) sched_yield();
        unix_deadline_now=round==1?1010:1021;
    }
    return __real_kernel_socket_reserve_read(socket,task,request,owner,capacity,nonblock,message);
}
int __real_kernel_socket_stream_reserve(struct kernel_socket_write_request *,uint32_t,uint32_t);
int __wrap_kernel_socket_stream_reserve(struct kernel_socket_write_request *request,uint32_t size,uint32_t flags)
{
    tcp_nonblock_attempt();
    int result=__real_kernel_socket_stream_reserve(request,size,flags);
    if(tcp_wait_role==1 && tcp_wait_active && tcp_wait_finish_count && result>0) {
        assert(!tcp_protocol_held() && __atomic_load_n(&tcp_wait_contender_acquired,__ATOMIC_ACQUIRE));
        __atomic_store_n(&tcp_wait_reserved_under_core,1,__ATOMIC_RELEASE);
    }
    return result;
}
int __real_kernel_socket_write_unix(struct kernel_open_file_description **,struct kernel_mm *,struct kernel_uaccess_iov_cursor *,uint64_t,uint32_t,int *,struct kernel_socket_message *);
int __wrap_kernel_socket_write_unix(struct kernel_open_file_description **owner,struct kernel_mm *space,
    struct kernel_uaccess_iov_cursor *cursor,uint64_t count,uint32_t flags,int *fault,struct kernel_socket_message *message)
{ tcp_nonblock_attempt();return __real_kernel_socket_write_unix(owner,space,cursor,count,flags,fault,message); }
static int tcp_protocol_held(void)
{
    for(struct kernel_lock_guard *g=kernel_io_context_current()->locks;g;g=g->previous)
        if(g->lock->rank==25) return 1;
    return 0;
}
static void tcp_wait_preparing(struct kernel_wait_queue *queue)
{
    assert(!tcp_timeout_observe || !tcp_timeout_nonblock);
    if(tcp_wait_role==1 && tcp_wait_active && queue!=tcp_wait_queue &&
        kernel_task_current()->wait.phase!=KERNEL_WAIT_FINISHED)
        fputs("FAIL socket recheck tries a second wait token while its socket token is live\n",stderr);
}
static void tcp_wait_prepared(struct kernel_wait_queue *queue,uint64_t deadline)
{
    if(tcp_write_editor) {
        assert(!kernel_io_context_current()->user_pins && !deadline);
        __atomic_store_n(&tcp_write_edit_closed,1,__ATOMIC_RELEASE);
    }
    if(tcp_write_observing && tcp_write_mode>=4 && tcp_write_queued_request &&
        tcp_write_queued_request->segments && !queue) {
        assert(!deadline && !kernel_task_current()->wait.interruptible && tcp_write_held==2 &&
            !tcp_protocol_held() && tcp_write_queued_request->task==kernel_task_current() &&
            tcp_write_queued_request->completion.task==kernel_task_current() &&
            tcp_write_queued_request->completion.generation==kernel_task_current()->wait.generation);
        __atomic_fetch_add(&tcp_write_queued_ready,1,__ATOMIC_RELEASE);
    }
    if(unix_deadline_active && queue==unix_deadline_queue) {
        if(unix_deadline_prepared==2) {
            fprintf(stderr,"FAIL read SO_RCVTIMEO renewed after two real wake-and-steal interleavings: deadlines=%llu,%llu,%llu now=%llu\n",
                (unsigned long long)unix_deadlines[0],(unsigned long long)unix_deadlines[1],
                (unsigned long long)deadline,(unsigned long long)unix_deadline_now);
            abort();
        }
        unix_deadlines[unix_deadline_prepared++]=deadline;
    }
    if(tcp_wait_role!=1 || !tcp_wait_active || queue!=tcp_wait_queue) return;
    assert(!tcp_wait_registered && kernel_task_current()->wait.phase==KERNEL_WAIT_PREPARED);
    tcp_wait_generation=kernel_task_current()->wait.generation;
    assert(!tcp_protocol_held() && kernel_cpu_current()->raw_locks &&
        kernel_cpu_current()->raw_locks->lock->rank==KERNEL_RAW_RANK_OBJECT);
    __atomic_store_n(&tcp_wait_registered,1,__ATOMIC_RELEASE);
    /* socket登记不占core；对手在token存活时取得core并持有到端点工作完成。 */
    while(!__atomic_load_n(&tcp_wait_contender_acquired,__ATOMIC_ACQUIRE)) sched_yield();
}
static void tcp_wait_parking(const struct kernel_wait_token *token)
{
    if(unix_deadline_active && kernel_task_current()->wait.deadline) {
        assert(unix_deadline_prepared && !tcp_protocol_held());
        __atomic_store_n(&unix_deadline_parked,unix_deadline_prepared,__ATOMIC_RELEASE);
    }
    if(tcp_wait_role!=1 || !tcp_wait_active || token->generation!=tcp_wait_generation) return;
    assert(!tcp_protocol_held() && !kernel_cpu_current()->raw_locks);
    __atomic_store_n(&tcp_wait_park_call,1,__ATOMIC_RELEASE);
}
static void tcp_wait_finishing(const struct kernel_wait_token *token)
{
    if(tcp_write_observing && tcp_write_mode>=4 && tcp_write_queued_ready>1) {
        assert(tcp_write_held==2 && !tcp_protocol_held());
        while(!__atomic_load_n(&tcp_write_worker_published,__ATOMIC_ACQUIRE)) sched_yield();
    }
    tcp_finishing_socket=tcp_wait_role==1 && tcp_wait_active &&
        token->generation==tcp_wait_generation;
    if(tcp_finishing_socket) {
        assert(!tcp_protocol_held());
        /* 模拟同CPU任务只在本轮协议发布结束后恢复；不让宿主pthread同时执行IRQ短区。 */
        while(!__atomic_load_n(&tcp_wait_publication_done,__ATOMIC_ACQUIRE)) sched_yield();
    }
}
static void tcp_wait_finished(void)
{
    if(!tcp_finishing_socket) return;
    assert(kernel_task_current()->wait.phase==KERNEL_WAIT_FINISHED && !tcp_protocol_held());
    tcp_wait_finish_count++;tcp_finishing_socket=0;
    __atomic_store_n(&tcp_wait_token_finished,1,__ATOMIC_RELEASE);
}
static void tcp_wait_locking(struct kernel_rwlock *lock)
{
    if(tcp_wait_role!=1 || !tcp_wait_active || lock->rank!=25) return;
    if(tcp_wait_registered) assert(kernel_task_current()->wait.phase==KERNEL_WAIT_FINISHED);
    if(tcp_wait_finish_count) tcp_wait_relocked++;
}
static void *tcp_wait_caller(void *argument)
{
    struct kernel_files *handle=argument;
    mm_test_attach(1);mm_test_activate_task();mm_test_context=context;tcp_wait_role=1;
    tcp_timeout_observe=1;tcp_timeout_nonblock=0;tcp_timeout_receives=tcp_timeout_sends=0;
    tcp_wait_status=tcp_wait_writing?
        kernel_files_write(handle,&mm,0,0x200000,8,&tcp_wait_bytes):
        kernel_files_read(handle,&mm,1,0x201000,8,&tcp_wait_bytes);
    assert(tcp_wait_writing?tcp_timeout_sends>0:tcp_timeout_receives>0);tcp_timeout_observe=0;
    assert(kernel_task_current()->wait.phase==KERNEL_WAIT_FINISHED);
    __atomic_store_n(&tcp_wait_caller_done,1,__ATOMIC_RELEASE);
    tcp_wait_role=0;mm_test_finish();return NULL;
}
static void *tcp_wait_contender(void *unused)
{
    (void)unused;mm_test_attach(2);mm_test_activate_task();mm_test_context=context;tcp_wait_role=2;
    while(!__atomic_load_n(&tcp_wait_registered,__ATOMIC_ACQUIRE)) sched_yield();
    struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
    assert(mm_test_parked(2)==tcp_wait_contender_parked);
    __atomic_store_n(&tcp_wait_contender_acquired,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&tcp_wait_park_call,__ATOMIC_ACQUIRE) ||
        mm_test_parked(1)==tcp_wait_caller_parked) sched_yield();
    if(tcp_wait_writing) {
        for(size_t i=0;i<tcp_wait_allocation_count;i++) mem_free(tcp_wait_allocations[i]);
    } else assert(socket_buffer_write(tcp_wait_files,0,"wakeonce",8,0)==8);
    /* 在同一个真实core owner内推进loopback或池容量，发出实际socket通知。 */
    for(unsigned i=0;i<8;i++)
        (void)kernel_socket_service_owned(&scope,(struct kernel_socket_service_budget){8,8,1});
    __atomic_store_n(&tcp_wait_publication_done,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&tcp_wait_token_finished,__ATOMIC_ACQUIRE) ||
        !__atomic_load_n(tcp_wait_writing?&tcp_wait_reserved_under_core:&tcp_wait_caller_done,__ATOMIC_ACQUIRE)) sched_yield();
    assert(tcp_protocol_held() && arch_interrupt_is_enabled());
    kernel_socket_protocol_leave(&scope);
    (void)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
    tcp_wait_role=0;mm_test_finish();return NULL;
}
static void tcp_wait_owner_case(struct kernel_heap *heap,int writing)
{
    struct kernel_socket *listener=0,*client=0,*server=0;
    struct kernel_socket_address address={.family=KERNEL_SOCKET_AF_INET,.port=(uint16_t)(24020+writing),.bytes={127,0,0,1}};
    assert(!kernel_socket_create(heap,KERNEL_SOCKET_AF_INET,1,&listener));
    assert(!kernel_socket_bind(listener,&address) && !kernel_socket_listen(listener,4));
    assert(!kernel_socket_create(heap,KERNEL_SOCKET_AF_INET,1,&client));
    int connected=kernel_socket_connect(client,&address,0);
    assert(!connected || connected==-KERNEL_EINPROGRESS);
    for(unsigned i=0;i<8;i++) (void)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
    assert(!kernel_socket_accept(listener,&server, NULL) && !kernel_socket_set_option(client,KERNEL_SOCKET_NODELAY,1));
    struct kernel_files handle={0};assert(!kernel_files_create(&handle,heap));
    struct kernel_open_file_description *out=0,*in=0;
    assert(!kernel_open_file_create_socket(heap,client,2,&out));
    assert(!kernel_open_file_create_socket(heap,server,2,&in));
    assert(!kernel_files_install_new_owned_at(&handle,0,0,&out) && !kernel_files_install_new_owned_at(&handle,1,0,&in));
    size_t copied;assert(!kernel_copy_to_user(&mm,0x200000,"wakeonce",8,&copied) && copied==8);
    assert(!kernel_copy_to_user(&mm,0x201000,"........",8,&copied) && copied==8);
    uint64_t before=physical_page_available(mm.allocator);
    static void *held[32768];size_t count=0;void *allocation;
    if(writing) {
        struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
        while((allocation=mem_malloc(1024))) {assert(count<32768);held[count++]=allocation;}
        while((allocation=mem_malloc(1))) {assert(count<32768);held[count++]=allocation;}
        kernel_socket_protocol_leave(&scope);
    }
    tcp_wait_queue=kernel_socket_wait_queue(writing?client:server);tcp_wait_client=client;tcp_wait_files=&handle;
    tcp_wait_allocations=held;tcp_wait_allocation_count=count;tcp_wait_writing=writing;
    tcp_wait_generation=0;tcp_wait_registered=tcp_wait_contender_acquired=tcp_wait_park_call=0;
    tcp_wait_finish_count=tcp_wait_relocked=0;
    tcp_wait_token_finished=tcp_wait_caller_done=tcp_wait_reserved_under_core=tcp_wait_publication_done=0;tcp_wait_active=1;
    tcp_wait_contender_parked=mm_test_parked(2);tcp_wait_caller_parked=mm_test_parked(1);
    pthread_t caller,contender;
    assert(!pthread_create(&contender,0,tcp_wait_contender,NULL));
    assert(!pthread_create(&caller,0,tcp_wait_caller,&handle));
    assert(!pthread_join(caller,0) && !pthread_join(contender,0));
    tcp_wait_active=0;
    assert(!tcp_wait_status && tcp_wait_bytes==8 && tcp_wait_park_call && tcp_wait_caller_done);
    if(tcp_wait_finish_count!=1 || !tcp_wait_token_finished || mm_test_parked(1)<=tcp_wait_caller_parked)
        fprintf(stderr,"TCP wait counters: writing=%d finishes=%u token=%u caller parks=%u/%u contender=%u/%u\n",writing,
            tcp_wait_finish_count,tcp_wait_token_finished,mm_test_parked(1),tcp_wait_caller_parked,
            mm_test_parked(2),tcp_wait_contender_parked);
    assert(tcp_wait_finish_count==1 && tcp_wait_token_finished && mm_test_parked(1)>tcp_wait_caller_parked);
    assert(writing?(tcp_wait_reserved_under_core && tcp_wait_relocked):!tcp_wait_relocked);
    int64_t result;
    if(writing) assert(!kernel_files_read(&handle,&mm,1,0x201000,8,&result) && result==8);
    char actual[8];assert(!kernel_copy_from_user(&mm,actual,0x201000,8,&copied) && copied==8);
    assert(!memcmp(actual,"wakeonce",8) && physical_page_available(mm.allocator)==before);
    struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
    for(struct tcp_pcb *pcb=tcp_active_pcbs,*next;pcb;pcb=next) {
        next=pcb->next;if(pcb->callback_arg==client || pcb->callback_arg==server) tcp_abort(pcb);
    }
    kernel_socket_protocol_leave(&scope);
    assert(!kernel_files_release(&handle));kernel_socket_destroy(listener);
    assert(!tcp_active_pcbs && !tcp_tw_pcbs);
    mm_test_finish();mm_test_attach(0);mm_test_activate_task();
    printf("PASS TCP %s wait: socket token without core lock, endpoint progresses under contending core owner, notification and final owners\n",writing?"write":"read");
}

static _Thread_local unsigned tcp_batch_active,tcp_batch_copying,tcp_batch_pins,tcp_batch_unpins,tcp_batch_finishes;
static _Thread_local struct kernel_mm_user_pin *tcp_batch_pin;
static unsigned tcp_batch_late,tcp_batch_copy_entered,tcp_batch_append_done;
static uint32_t tcp_batch_captured,tcp_batch_completed;
static struct kernel_socket *tcp_batch_client;
static struct kernel_files *tcp_batch_files;
static int64_t tcp_batch_other_result;
static int tcp_batch_unpinning(struct kernel_mm_user_pin *pin)
{
    if(tcp_write_observing && tcp_write_held && pin==tcp_write_pins[tcp_write_held-1]) {
        assert(!tcp_write_returning && pin->registration==pin);
        tcp_write_returning=pin;return 1;
    }
    return tcp_batch_copying && pin==tcp_batch_pin;
}
static void tcp_batch_unpinned(struct kernel_mm_user_pin *pin)
{
    if(tcp_write_returning) {
        assert(pin==tcp_write_returning && !pin->registration && tcp_write_held);
        tcp_write_pins[--tcp_write_held]=NULL;tcp_write_unpin_count++;tcp_write_returning=NULL;
        if(!tcp_write_held) __atomic_store_n(&tcp_write_pins_returned,1,__ATOMIC_RELEASE);
        return;
    }
    assert(!pin->registration && !kernel_io_context_current()->user_pins);
    tcp_batch_unpins++;tcp_batch_pin=NULL;
}
enum kernel_mm_status __real_kernel_mm_pin_user_page(const struct kernel_mm *,uint64_t,uint32_t,struct kernel_mm_user_pin *);
enum kernel_mm_status __wrap_kernel_mm_pin_user_page(const struct kernel_mm *space,uint64_t address,
    uint32_t access,struct kernel_mm_user_pin *pin)
{
    enum kernel_mm_status status=__real_kernel_mm_pin_user_page(space,address,access,pin);
    if(tcp_write_observing) tcp_write_pin_observed(access,status,pin);
    if(tcp_batch_copying && status==KERNEL_MM_STATUS_OK) {
        assert(access==KERNEL_MM_WRITE && !tcp_protocol_held() && !tcp_batch_pin);
        assert(pin->registration==pin && pin->owner==kernel_io_context_current() && pin->owner->user_pins==pin);
        tcp_batch_pin=pin;tcp_batch_pins++;
        if(tcp_batch_late && !tcp_batch_copy_entered) {
            __atomic_store_n(&tcp_batch_copy_entered,1,__ATOMIC_RELEASE);
            while(!__atomic_load_n(&tcp_batch_append_done,__ATOMIC_ACQUIRE)) sched_yield();
        }
    }
    return status;
}
int __real_kernel_socket_read_user(struct kernel_socket_read_request *,struct kernel_mm *,struct kernel_uaccess_iov_cursor *);
int __wrap_kernel_socket_read_user(struct kernel_socket_read_request *request,struct kernel_mm *space,
    struct kernel_uaccess_iov_cursor *cursor)
{
    if(!tcp_batch_active) return __real_kernel_socket_read_user(request,space,cursor);
    assert(!tcp_batch_copying && !tcp_protocol_held());tcp_batch_captured=request->bytes;
    tcp_batch_copying=1;
    int result=__real_kernel_socket_read_user(request,space,cursor);
    tcp_batch_copying=0;
    assert(!tcp_batch_pin && !kernel_io_context_current()->user_pins);
    tcp_batch_completed=request->completed_bytes;
    return result;
}
uint32_t __real_kernel_socket_finish_read(struct kernel_socket_read_request *,int);
uint32_t __wrap_kernel_socket_finish_read(struct kernel_socket_read_request *request,int fault)
{
    if(tcp_batch_active) {
        /* 真正MM pin已归还，端点finish才能交还借用并退休packet。 */
        assert(!tcp_batch_copying && !tcp_batch_pin && !kernel_io_context_current()->user_pins);
        assert(tcp_batch_pins==tcp_batch_unpins && !tcp_protocol_held());tcp_batch_finishes++;
    }
    return __real_kernel_socket_finish_read(request,fault);
}
static void tcp_batch_observe(void)
{
    tcp_batch_pins=tcp_batch_unpins=tcp_batch_finishes=0;
    tcp_batch_captured=tcp_batch_completed=0;tcp_batch_active=1;
}
static void tcp_batch_sent(struct kernel_socket *client,const char *bytes,unsigned count)
{
    (void)client;assert(socket_buffer_write(tcp_batch_files,0,bytes,count,0)==(int)count);
    (void)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
}
static void *tcp_batch_append(void *unused)
{
    (void)unused;mm_test_attach(2);mm_test_activate_task();mm_test_context=context;
    while(!__atomic_load_n(&tcp_batch_copy_entered,__ATOMIC_ACQUIRE)) sched_yield();
    /* 另一真实reader不能借用正在复制的前缀；发送者仍可追加新packet。 */
    assert(!kernel_files_read(tcp_batch_files,&mm,1,0x200080,1,&tcp_batch_other_result));
    assert(tcp_batch_other_result==-KERNEL_EAGAIN);
    tcp_batch_sent(tcp_batch_client,"LATE",4);
    mm_test_finish();__atomic_store_n(&tcp_batch_append_done,1,__ATOMIC_RELEASE);return NULL;
}
static void tcp_read_batch_case(struct kernel_heap *heap)
{
    struct kernel_socket *listener=0,*client=0,*server=0;
    struct kernel_socket_address address={.family=KERNEL_SOCKET_AF_INET,.port=24030,.bytes={127,0,0,1}};
    assert(!kernel_socket_create(heap,KERNEL_SOCKET_AF_INET,1,&listener));
    assert(!kernel_socket_bind(listener,&address) && !kernel_socket_listen(listener,4));
    assert(!kernel_socket_create(heap,KERNEL_SOCKET_AF_INET,1,&client));
    int connected=kernel_socket_connect(client,&address,0);assert(!connected || connected==-KERNEL_EINPROGRESS);
    for(unsigned i=0;i<8;i++) (void)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
    assert(!kernel_socket_accept(listener,&server, NULL) && !kernel_socket_set_option(client,KERNEL_SOCKET_NODELAY,1));
    struct kernel_files handle={0};assert(!kernel_files_create(&handle,heap));
    struct kernel_open_file_description *out=0,*in=0;
    assert(!kernel_open_file_create_socket(heap,client,2,&out));
    assert(!kernel_open_file_create_socket(heap,server,2|KERNEL_FILES_O_NONBLOCK,&in));
    assert(!kernel_files_install_new_owned_at(&handle,0,0,&out) && !kernel_files_install_new_owned_at(&handle,1,0,&in));
    tcp_batch_files=&handle;
    const uint64_t destination=0x200080,iov_address=0x100200,readonly=0x200000+2U*BOAROS_PAGE_SIZE;
    size_t copied;int64_t result;char actual[16];
    assert(!kernel_copy_to_user(&mm,destination,"................",16,&copied) && copied==16);
    assert(!kernel_copy_to_user(&mm,readonly,"........",8,&copied) && copied==8);
    struct kernel_uaccess_iovec iov[]={{destination,3},{destination+3,5},{destination+8,4}};
    assert(!kernel_copy_to_user(&mm,iov_address,iov,sizeof(iov),&copied) && copied==sizeof(iov));
    tcp_batch_sent(client,"AAAA",4);tcp_batch_sent(client,"BBBB",4);tcp_batch_sent(client,"CCCC",4);
    tcp_batch_observe();
    assert(!kernel_files_readv(&handle,&mm,1,iov_address,3,&result) && result==12);
    tcp_batch_active=0;
    assert(tcp_batch_captured==12 && tcp_batch_completed==12 && tcp_batch_finishes==1);
    assert(tcp_batch_pins==1 && tcp_batch_unpins==1);
    assert(!kernel_copy_from_user(&mm,actual,destination,12,&copied) && copied==12 && !memcmp(actual,"AAAABBBBCCCC",12));
    puts("PASS TCP actual readv: three packets across three same-page iovecs, one real MM pin, unpin before finish owner");

    iov[0]=(struct kernel_uaccess_iovec){destination,6};iov[1]=(struct kernel_uaccess_iovec){readonly,4};
    iov[2]=(struct kernel_uaccess_iovec){destination+16,4};
    assert(!kernel_copy_to_user(&mm,iov_address,iov,sizeof(iov),&copied) && copied==sizeof(iov));
    assert(!kernel_mm_mprotect(&mm,readonly,BOAROS_PAGE_SIZE,KERNEL_MM_READ));
    tcp_batch_sent(client,"AAAA",4);tcp_batch_sent(client,"BBBBBB",6);tcp_batch_sent(client,"CCCC",4);
    tcp_batch_observe();
    assert(!kernel_files_readv(&handle,&mm,1,iov_address,3,&result) && result==4);
    tcp_batch_active=0;
    assert(tcp_batch_captured==14 && tcp_batch_completed==4 && tcp_batch_finishes==1);
    assert(tcp_batch_pins==1 && tcp_batch_unpins==1);
    assert(!kernel_copy_from_user(&mm,actual,destination,6,&copied) && copied==6 && !memcmp(actual,"AAAABB",6));
    assert(!kernel_mm_mprotect(&mm,readonly,BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE));
    assert(!kernel_files_read(&handle,&mm,1,destination,10,&result) && result==10);
    assert(!kernel_copy_from_user(&mm,actual,destination,10,&copied) && copied==10 && !memcmp(actual,"BBBBBBCCCC",10));
    puts("PASS TCP actual MM write fault: completed packet returns four bytes, partially copied fault packet replays in full with later packet");

    iov[0]=(struct kernel_uaccess_iovec){destination,3};iov[1]=(struct kernel_uaccess_iovec){destination+3,5};
    assert(!kernel_copy_to_user(&mm,iov_address,iov,2*sizeof(*iov),&copied) && copied==2*sizeof(*iov));
    tcp_batch_sent(client,"AAAA",4);tcp_batch_sent(client,"BBBB",4);
    tcp_batch_late=1;tcp_batch_copy_entered=tcp_batch_append_done=0;tcp_batch_client=client;tcp_batch_files=&handle;
    pthread_t appender;assert(!pthread_create(&appender,0,tcp_batch_append,NULL));
    tcp_batch_observe();
    assert(!kernel_files_readv(&handle,&mm,1,iov_address,2,&result) && result==8);
    tcp_batch_active=0;assert(!pthread_join(appender,0));tcp_batch_late=0;
    assert(tcp_batch_captured==8 && tcp_batch_completed==8 && tcp_batch_finishes==1 && tcp_batch_other_result==-KERNEL_EAGAIN);
    assert(tcp_batch_pins==1 && tcp_batch_unpins==1);
    assert(!kernel_copy_from_user(&mm,actual,destination,8,&copied) && copied==8 && !memcmp(actual,"AAAABBBB",8));
    assert(!kernel_files_read(&handle,&mm,1,destination,8,&result) && result==4);
    assert(!kernel_copy_from_user(&mm,actual,destination,4,&copied) && copied==4 && !memcmp(actual,"LATE",4));
    assert(!kernel_files_read(&handle,&mm,1,destination,1,&result) && result==-KERNEL_EAGAIN);
    struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
    for(struct tcp_pcb *pcb=tcp_active_pcbs,*next;pcb;pcb=next) {next=pcb->next;if(pcb->callback_arg==client || pcb->callback_arg==server) tcp_abort(pcb);}
    kernel_socket_protocol_leave(&scope);
    assert(!kernel_files_release(&handle));kernel_socket_destroy(listener);assert(!tcp_active_pcbs && !tcp_tw_pcbs);
    mm_test_finish();mm_test_attach(0);mm_test_activate_task();
    puts("PASS TCP actual concurrent reader and late append: active prefix remains exclusive, later packet is returned by the next read");
}
static void *unix_deadline_competitor(void *unused)
{
    (void)unused;mm_test_attach(2);mm_test_activate_task();mm_test_context=context;
    for(unsigned round=1;round<=2;round++) {
        while(__atomic_load_n(&unix_deadline_parked,__ATOMIC_ACQUIRE)<round) sched_yield();
        while(mm_test_parked(0)<unix_deadline_park_baseline+round) sched_yield();
        char value=(char)('A'+round-1);
        assert(socket_buffer_write(unix_deadline_files,0,&value,1,0)==1);
        while(__atomic_load_n(&unix_deadline_steal,__ATOMIC_ACQUIRE)<round) sched_yield();
        /* 被唤醒的读者先停在真正reserve之前；另一读者实际取走这一个字节。 */
        int64_t result;size_t copied;char actual;
        assert(!kernel_files_read(unix_deadline_files,&mm,1,0x200180,1,&result) && result==1);
        assert(!kernel_copy_from_user(&mm,&actual,0x200180,1,&copied) && copied==1 && actual==value);
        __atomic_store_n(&unix_deadline_stolen,round,__ATOMIC_RELEASE);
    }
    mm_test_finish();return NULL;
}
static void unix_receive_deadline_case(struct kernel_heap *heap)
{
    struct kernel_socket *sender=0,*receiver=0;
    assert(!kernel_socket_pair(heap,1,&sender,&receiver));
    kernel_socket_set_receive_timeout(receiver,20);
    struct kernel_files handle={0};struct kernel_open_file_description *in=0;
    assert(!kernel_files_create(&handle,heap));
    struct kernel_open_file_description *out=NULL;
    assert(!kernel_open_file_create_socket(heap,sender,2,&out) && !kernel_files_install_new_owned_at(&handle,0,0,&out));
    assert(!kernel_open_file_create_socket(heap,receiver,2,&in));
    assert(!kernel_files_install_new_owned_at(&handle,1,0,&in));
    unix_deadline_sender=sender;unix_deadline_files=&handle;unix_deadline_queue=kernel_socket_wait_queue(receiver);
    unix_deadline_prepared=unix_deadline_parked=unix_deadline_steal=unix_deadline_stolen=unix_deadline_reservations=0;
    unix_deadlines[0]=unix_deadlines[1]=0;unix_deadline_now=1000;
    unix_deadline_park_baseline=mm_test_parked(0);
    size_t copied;assert(!kernel_copy_to_user(&mm,0x200080,"........",8,&copied) && copied==8);
    assert(!kernel_copy_to_user(&mm,0x200180,".",1,&copied) && copied==1);
    uint64_t before=physical_page_available(mm.allocator);
    pthread_t competitor;assert(!pthread_create(&competitor,0,unix_deadline_competitor,NULL));
    unix_deadline_active=1;
    int64_t result;assert(!kernel_files_read(&handle,&mm,1,0x200080,8,&result));
    unix_deadline_active=0;assert(!pthread_join(competitor,0));
    assert(result==-KERNEL_EAGAIN && unix_deadline_reservations==3 && unix_deadline_prepared==2);
    assert(unix_deadlines[0]==1020 && unix_deadlines[1]==1020 && unix_deadline_stolen==2);
    assert(kernel_task_current()->wait.phase==KERNEL_WAIT_FINISHED && !kernel_io_context_current()->user_pins);
    assert(physical_page_available(mm.allocator)==before);
    assert(!kernel_files_release(&handle));
    puts("PASS UNIX read SO_RCVTIMEO: two actual park/wake cycles lose data to competing reader, one absolute deadline expires without a third wait");
}
static void unix_receive_error_prefix_case(struct kernel_heap *heap)
{
    struct kernel_socket *sender=0,*receiver=0;
    assert(!kernel_socket_pair(heap,1,&sender,&receiver));
    struct kernel_files handle={0};struct kernel_open_file_description *in=0;
    assert(!kernel_files_create(&handle,heap));
    struct kernel_open_file_description *out=NULL;
    assert(!kernel_open_file_create_socket(heap,sender,2,&out) && !kernel_files_install_new_owned_at(&handle,0,0,&out));
    assert(!kernel_open_file_create_socket(heap,receiver,2,&in));
    assert(!kernel_files_install_new_owned_at(&handle,1,0,&in));
    assert(socket_buffer_write(&handle,0,"DATA",4,0)==4);
    /* 对端尚有未读数据时关闭，生产UNIX实现向receiver发布ECONNRESET。 */
    assert(socket_buffer_write(&handle,1,"unread",6,0)==6);
    int64_t closed;assert(!kernel_files_close(&handle,0,&closed) && !closed);
    int64_t result;size_t copied;char actual[4];
    assert(!kernel_files_read(&handle,&mm,1,0x200080,8,&result) && result==4);
    assert(!kernel_copy_from_user(&mm,actual,0x200080,4,&copied) && copied==4 && !memcmp(actual,"DATA",4));
    assert(!kernel_files_read(&handle,&mm,1,0x200080,8,&result));
    if(result!=-KERNEL_ECONNRESET) {
        fprintf(stderr,"FAIL UNIX read consumed asynchronous ECONNRESET after returning the four-byte prefix: next read=%lld\n",(long long)result);
        abort();
    }
    assert(!kernel_files_read(&handle,&mm,1,0x200080,8,&result) && !result);
    assert(!kernel_files_release(&handle));
    assert(!kernel_io_context_current()->user_pins && kernel_task_current()->wait.phase==KERNEL_WAIT_FINISHED);
    puts("PASS UNIX read completed prefix preserves pending ECONNRESET for the next call, followed by EOF and final owner release");
}
static void *tcp_nonblock_clear(void *unused)
{
    (void)unused;mm_test_attach(2);mm_test_activate_task();mm_test_context=context;
    while(!__atomic_load_n(&tcp_nonblock_attempted,__ATOMIC_ACQUIRE)) sched_yield();
    int64_t result;assert(!kernel_files_fcntl(tcp_nonblock_files,tcp_nonblock_fd,KERNEL_FILES_F_SETFL,0,&result) && !result);
    mm_test_finish();__atomic_store_n(&tcp_nonblock_changed,1,__ATOMIC_RELEASE);return NULL;
}
static void tcp_nonblock_io(struct kernel_files *handle,int writing,int vector,int message,int change)
{
    const uint64_t buffer=0x200080,iov_address=0x100200;
    int fd=writing?0:1;int64_t result;
    assert(!kernel_files_fcntl(handle,fd,KERNEL_FILES_F_SETFL,message?0:KERNEL_FILES_O_NONBLOCK,&result) && !result);
    pthread_t changer;
    if(change) {
        tcp_nonblock_files=handle;tcp_nonblock_fd=fd;tcp_nonblock_attempted=tcp_nonblock_changed=0;tcp_nonblock_change=1;
        assert(!pthread_create(&changer,0,tcp_nonblock_clear,NULL));
    }
    tcp_timeout_receives=tcp_timeout_sends=0;tcp_timeout_nonblock=tcp_timeout_observe=1;
    if(message) {
        struct kernel_open_file_description *owner=0;
        assert(!kernel_files_pin(handle,fd,&owner,&result) && !result && owner);
        const struct kernel_uaccess_iovec iov[]={{buffer,3},{buffer+3,5}};
        assert(!kernel_files_socket_iov_io(handle,&mm,&owner,iov,2,8,KERNEL_SOCKET_MSG_DONTWAIT,
            writing,NULL,NULL,&result,NULL));
        assert(!kernel_open_file_release(&owner));
    } else if(vector) {
        assert(!(writing?kernel_files_writev(handle,&mm,fd,iov_address,2,&result):
            kernel_files_readv(handle,&mm,fd,iov_address,2,&result)));
    } else assert(!(writing?kernel_files_write(handle,&mm,fd,buffer,8,&result):
        kernel_files_read(handle,&mm,fd,buffer,8,&result)));
    tcp_timeout_observe=0;
    if(result!=-KERNEL_EAGAIN || tcp_timeout_receives || tcp_timeout_sends)
        fprintf(stderr,"FAIL nonblock %s vector=%d MSG_DONTWAIT=%d F_SETFL=%d: result=%lld receive_timeout=%u send_timeout=%u\n",
            writing?"write":"read",vector,message,change,(long long)result,tcp_timeout_receives,tcp_timeout_sends);
    assert(result==-KERNEL_EAGAIN && !tcp_timeout_receives && !tcp_timeout_sends);
    if(change) {assert(!pthread_join(changer,0));assert(tcp_nonblock_attempted && tcp_nonblock_changed && !tcp_nonblock_change);}
    assert(kernel_task_current()->wait.phase==KERNEL_WAIT_FINISHED && !kernel_io_context_current()->user_pins);
}
static void tcp_nonblock_timeout_case(struct kernel_heap *heap,int unix_socket)
{
    struct kernel_socket *listener=0,*client=0,*server=0;
    if(unix_socket) assert(!kernel_socket_pair(heap,1,&client,&server));
    else {
        struct kernel_socket_address address={.family=KERNEL_SOCKET_AF_INET,.port=24040,.bytes={127,0,0,1}};
        assert(!kernel_socket_create(heap,KERNEL_SOCKET_AF_INET,1,&listener));
        assert(!kernel_socket_bind(listener,&address) && !kernel_socket_listen(listener,4));
        assert(!kernel_socket_create(heap,KERNEL_SOCKET_AF_INET,1,&client));
        int connected=kernel_socket_connect(client,&address,0);assert(!connected || connected==-KERNEL_EINPROGRESS);
        for(unsigned i=0;i<8;i++) (void)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
        assert(!kernel_socket_accept(listener,&server, NULL) && !kernel_socket_set_option(client,KERNEL_SOCKET_NODELAY,1));
    }
    kernel_socket_set_receive_timeout(server,2000000000ULL);kernel_socket_set_send_timeout(client,2000000000ULL);
    struct kernel_files handle={0};assert(!kernel_files_create(&handle,heap));
    struct kernel_open_file_description *out=0,*in=0;
    assert(!kernel_open_file_create_socket(heap,client,2,&out) && !kernel_open_file_create_socket(heap,server,2,&in));
    assert(!kernel_files_install_new_owned_at(&handle,0,0,&out) && !kernel_files_install_new_owned_at(&handle,1,0,&in));
    size_t copied;const struct kernel_uaccess_iovec iov[]={{0x200080,3},{0x200083,5}};
    assert(!kernel_copy_to_user(&mm,0x200080,"nonblock",8,&copied) && copied==8);
    assert(!kernel_copy_to_user(&mm,0x100200,iov,sizeof(iov),&copied) && copied==sizeof(iov));
    for(int vector=0;vector<2;vector++) tcp_nonblock_io(&handle,0,vector,0,0);
    tcp_nonblock_io(&handle,0,0,1,0);
    for(int vector=0;vector<2;vector++) tcp_nonblock_io(&handle,0,vector,0,1);
    static const char payload[1024];int full=0;
    for(unsigned i=0;i<512;i++) {
        int sent=socket_buffer_write(&handle,0,payload,sizeof(payload),KERNEL_SOCKET_MSG_DONTWAIT);
        if(sent==-KERNEL_EAGAIN) {full=1;break;}
        assert(sent>0 && sent<=(int)sizeof(payload));
        (void)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
    }
    assert(full);
    for(int vector=0;vector<2;vector++) tcp_nonblock_io(&handle,1,vector,0,0);
    tcp_nonblock_io(&handle,1,0,1,0);
    for(int vector=0;vector<2;vector++) tcp_nonblock_io(&handle,1,vector,0,1);
    if(!unix_socket) {
        struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
        for(struct tcp_pcb *pcb=tcp_active_pcbs,*next;pcb;pcb=next) {next=pcb->next;if(pcb->callback_arg==client || pcb->callback_arg==server) tcp_abort(pcb);}
        kernel_socket_protocol_leave(&scope);
    }
    assert(!kernel_files_release(&handle));if(listener)kernel_socket_destroy(listener);
    assert(!tcp_active_pcbs && !tcp_tw_pcbs);
    mm_test_finish();mm_test_attach(0);mm_test_activate_task();
    printf("PASS %s actual nonblocking read/readv/write/writev and MSG_DONTWAIT: zero timeout getters, full/empty EAGAIN, entry flags survive concurrent F_SETFL clear\n",unix_socket?"UNIX":"TCP");
}
static enum kernel_files_status tcp_writer_status;
static int64_t tcp_writer_bytes;
static uint64_t tcp_writer_available;
static void *tcp_user_writer(void *argument)
{
    struct kernel_files *handle=argument;
    mm_test_attach(1);mm_test_activate_task();kernel_cpu_current()->logical_id=0;mm_test_context=context;
    tcp_writer_status=kernel_files_write(handle,&mm,0,0x200000,8,&tcp_writer_bytes);
    assert(!kernel_task_current()->io_buffer && kernel_task_current()->io_scratch_data &&
        !kernel_io_context_current()->user_pins);
    tcp_writer_available=physical_page_available(mm.allocator);
    mm_test_finish();return 0;
}
static void tcp_write_batch_case(struct kernel_heap *heap);
static void tcp_usercopy_case(struct kernel_heap *heap)
{
    kernel_socket_network_initialize();
    struct kernel_socket *listener=0,*client=0,*server=0;
    struct kernel_socket_address address={.family=KERNEL_SOCKET_AF_INET,.port=24011,.bytes={127,0,0,1}};
    assert(!kernel_socket_create(heap,KERNEL_SOCKET_AF_INET,1,&listener));
    assert(!kernel_socket_bind(listener,&address) && !kernel_socket_listen(listener,4));
    assert(!kernel_socket_create(heap,KERNEL_SOCKET_AF_INET,1,&client));
    int connected=kernel_socket_connect(client,&address,0);
    assert(!connected || connected==-KERNEL_EINPROGRESS);
    for(unsigned i=0;i<8;i++) (void)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
    assert(!kernel_socket_accept(listener,&server, NULL));
    assert(!kernel_socket_set_option(client,KERNEL_SOCKET_NODELAY,1));
    struct kernel_files handle={0};assert(!kernel_files_create(&handle,heap));
    struct kernel_open_file_description *out=0,*in=0;
    assert(!kernel_open_file_create_socket(heap,client,2,&out));
    assert(!kernel_open_file_create_socket(heap,server,2|KERNEL_FILES_O_NONBLOCK,&in));
    assert(!kernel_files_install_new_owned_at(&handle,0,0,&out) && !kernel_files_install_new_owned_at(&handle,1,0,&in));
    size_t copied;assert(!kernel_copy_to_user(&mm,0x200000,"oldvalue",8,&copied) && copied==8);
    assert(!kernel_copy_to_user(&mm,0x201000,"........",8,&copied) && copied==8);
    uint64_t before=physical_page_available(mm.allocator);
    static void *held[32768];size_t count=0;void *allocation;
    struct kernel_socket_protocol_scope irq; kernel_socket_protocol_enter(&irq);
    while ((allocation=mem_malloc(1024))) { assert(count<32768);held[count++]=allocation; }
    while ((allocation=mem_malloc(1))) { assert(count<32768);held[count++]=allocation; }
    kernel_socket_protocol_leave(&irq);
    struct kernel_socket_statistics start,blocked;kernel_socket_get_statistics(&start);
    unsigned parked=mm_test_parked(1);pthread_t writer;
    assert(!pthread_create(&writer,0,tcp_user_writer,&handle));
    while(mm_test_parked(1)==parked) sched_yield();
    kernel_socket_get_statistics(&blocked);
    assert(blocked.tcp_write_calls>start.tcp_write_calls && blocked.tcp_written_bytes==start.tcp_written_bytes);
    /* ERR_MEM未接纳字节；等待期间必须归还pin，允许撤销旧映射再发布新来源。 */
    assert(!kernel_mm_mprotect(&mm,0x200000,BOAROS_PAGE_SIZE,KERNEL_MM_READ));
    assert(!kernel_mm_munmap(&mm,0x200000,BOAROS_PAGE_SIZE));uint64_t mapped;
    assert(!kernel_mm_mmap_anonymous(&mm,0x200000,BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE,
        KERNEL_MM_MAP_FIXED_NOREPLACE,&mapped));
    assert(!kernel_copy_to_user(&mm,0x200000,"newvalue",8,&copied) && copied==8);
    kernel_socket_protocol_enter(&irq);
    for(size_t i=0;i<count;i++) mem_free(held[i]);
    kernel_socket_protocol_leave(&irq);
    (void)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
    assert(!pthread_join(writer,0) && !tcp_writer_status && tcp_writer_bytes==8);
    int64_t result;
    assert(!kernel_files_read(&handle,&mm,1,0x201000,8,&result) && result==8);
    char actual[8];assert(!kernel_copy_from_user(&mm,actual,0x201000,8,&copied) && copied==8);
    assert(!memcmp(actual,"newvalue",8));
    assert(!kernel_files_read(&handle,&mm,1,0x201000,1,&result) && result==-KERNEL_EAGAIN);
    assert(!kernel_mm_mprotect(&mm,0x200000,BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE));
    assert(tcp_writer_available+1==before && physical_page_available(mm.allocator)==before);
    uint64_t tail=0x200000+4U*BOAROS_PAGE_SIZE-4U;
    assert(!kernel_copy_to_user(&mm,tail,"tail",4,&copied) && copied==4);
    assert(!kernel_files_write(&handle,&mm,0,tail,8,&result) && result==4);
    assert(!kernel_files_read(&handle,&mm,1,0x201000,8,&result) && result==4);
    assert(!kernel_copy_from_user(&mm,actual,0x201000,4,&copied) && copied==4 && !memcmp(actual,"tail",4));
    assert(!kernel_files_write(&handle,&mm,0,tail+4,1,&result) && result==-KERNEL_EFAULT);
    assert(!kernel_files_read(&handle,&mm,1,0x201000,1,&result) && result==-KERNEL_EAGAIN);
    kernel_socket_protocol_enter(&irq);
    for(struct tcp_pcb *pcb=tcp_active_pcbs,*next;pcb;pcb=next) {
        next=pcb->next;if(pcb->callback_arg==client || pcb->callback_arg==server) tcp_abort(pcb);
    }
    kernel_socket_protocol_leave(&irq);
    assert(!kernel_files_release(&handle));kernel_socket_destroy(listener);
    assert(!tcp_active_pcbs && !tcp_tw_pcbs);
    mm_test_finish();mm_test_attach(0);mm_test_activate_task();
    puts("PASS TCP direct user pin: ERR_MEM wait releases pin, remapped bytes accepted once, fault prefix, one retained descriptor page and final owners returned");
    tcp_write_batch_case(heap);
}

/* Observe real pins and the real protocol COPY; no payload or result is supplied by the fixture. */
static void tcp_write_pin_observed(uint32_t access,enum kernel_mm_status status,struct kernel_mm_user_pin *pin)
{
    assert(access==KERNEL_MM_READ);
    if(status!=KERNEL_MM_STATUS_OK) {
        if(tcp_write_held) {
            if(status==KERNEL_MM_STATUS_NOT_MAPPED) tcp_write_misses++;
            else if(status==KERNEL_MM_STATUS_ACCESS) tcp_write_denied++;
            else if(status==KERNEL_MM_STATUS_CONFLICT) tcp_write_conflicts++;
            else assert(!"unexpected resident pin failure");
        }
        return;
    }
    assert(!tcp_protocol_held() && tcp_write_held<4 && pin->registration==pin &&
        pin->owner==kernel_io_context_current());
    tcp_write_pins[tcp_write_held++]=pin;tcp_write_pin_count++;
    if(tcp_write_mode==3 && tcp_write_pin_count==1) {
        __atomic_store_n(&tcp_write_edit_started,1,__ATOMIC_RELEASE);
        while(!__atomic_load_n(&tcp_write_edit_closed,__ATOMIC_ACQUIRE)) sched_yield();
    }
}
int __real_kernel_socket_stream_commitv(struct kernel_socket_write_request *,
    const struct kernel_socket_write_segment *,unsigned,uint32_t);
int __wrap_kernel_socket_stream_commitv(struct kernel_socket_write_request *request,
    const struct kernel_socket_write_segment *segments,unsigned count,uint32_t flags)
{
    if(!tcp_write_observing) return __real_kernel_socket_stream_commitv(request,segments,count,flags);
    assert(!tcp_protocol_held() && count==tcp_write_held && count && tcp_write_commits<4);
    uint32_t bytes=0;
    for(unsigned i=0;i<count;i++) {
        const unsigned char *page=tcp_write_pins[i]->page,*data=segments[i].data;
        assert(data>=page && data<page+BOAROS_PAGE_SIZE && segments[i].size &&
            segments[i].size<=(uint32_t)(page+BOAROS_PAGE_SIZE-data));
        bytes+=segments[i].size;
    }
    tcp_write_commit_bytes[tcp_write_commits]=bytes;
    tcp_write_commit_segments[tcp_write_commits++]=count;
    tcp_write_queued_request=request;
    int result=__real_kernel_socket_stream_commitv(request,segments,count,flags);
    assert(!request->segments && !request->next && request->pin && request->task==kernel_task_current());
    /* COPY and result publication finish before the caller returns any physical pin. */
    for(unsigned i=0;i<count;i++) assert(tcp_write_pins[i]->registration==tcp_write_pins[i]);
    return result;
}
static void *tcp_write_revoke(void *unused)
{
    (void)unused;mm_test_attach(2);mm_test_activate_task();mm_test_bind(&mm);mm_test_context=context;
    kernel_cpu_current()->logical_id=1;tcp_write_editor=1;
    while(!__atomic_load_n(&tcp_write_edit_started,__ATOMIC_ACQUIRE)) sched_yield();
    assert(!kernel_mm_mprotect(&mm,tcp_write_source_page+BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE,0));
    tcp_write_editor=0;__atomic_store_n(&tcp_write_edit_done,1,__ATOMIC_RELEASE);
    mm_test_finish();return NULL;
}
static void tcp_write_work_notification(struct kernel_wait_node *node,uint32_t reason)
{ (void)node;(void)reason;__atomic_fetch_add(&tcp_write_notifications,1,__ATOMIC_RELEASE); }
/* SIGPIPE is observed at the signal boundary so this fixture can inspect its target.
 * Every unrelated signal remains an unexpected consumer, rather than a success stub. */
enum kernel_signal_status __wrap_kernel_signal_send_task(struct kernel_task *task,uint32_t signal,kernel_pid_t sender_tid)
{
    assert(tcp_write_mode==5 && signal==13 && !sender_tid && task==tcp_write_caller_task &&
        task!=kernel_task_current() && tcp_write_queued_request &&
        tcp_write_queued_request->task==task && tcp_write_queued_request->segments);
    tcp_write_signal_count++;
    return KERNEL_SIGNAL_STATUS_OK;
}
static void *tcp_write_protocol_worker(void *unused)
{
    (void)unused;mm_test_attach(2);mm_test_activate_task();mm_test_context=context;
    struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
    __atomic_store_n(&tcp_write_worker_ready,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&tcp_write_queued_ready,__ATOMIC_ACQUIRE) ||
        mm_test_parked(0)==tcp_write_park_baseline) sched_yield();
    struct kernel_socket_write_request *request=tcp_write_queued_request;
    assert(request && request->segments && request->segment_count==2 && request->reserved==96 &&
        request->task==tcp_write_caller_task && request->pin && request->pin->references==3);
    assert(tcp_write_notifications && !kernel_io_context_current()->user_pins);
    /* socket 就绪通知不属于这个 COPY 请求；它继续持 pin 睡眠。 */
    {
        KERNEL_IRQ_SCOPE(irq);
        assert(!kernel_wait_queue_wake_all(kernel_socket_wait_queue(tcp_write_client_socket)));
    }
    struct kernel_wait_token token;
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
      assert(tcp_write_caller_task->wait.phase==KERNEL_WAIT_COMMITTED);
      token=(struct kernel_wait_token){tcp_write_caller_task,tcp_write_caller_task->wait.generation}; }
    assert(mm_test_parked(0)==tcp_write_park_baseline+1 &&
        __atomic_load_n(&tcp_write_queued_ready,__ATOMIC_ACQUIRE)==1);
    /* 直接注入提前通知仍须重新检查真实完成状态，不提前归还来源页。 */
    assert(!kernel_wait_notify(&token,KERNEL_WAIT_WOKEN));
    while(__atomic_load_n(&tcp_write_queued_ready,__ATOMIC_ACQUIRE)<2 ||
        mm_test_parked(0)<tcp_write_park_baseline+2) sched_yield();
    assert(request->segments && request->pin->references==3 &&
        !__atomic_load_n(&tcp_write_pins_returned,__ATOMIC_ACQUIRE));
    {
        if(tcp_write_mode==5) assert(!kernel_socket_shutdown(tcp_write_client_socket,1));
        for(unsigned i=0;i<8;i++)
            (void)kernel_socket_service_owned(&scope,(struct kernel_socket_service_budget){8,8,1});
    }
    assert(!request->segments && !request->next && !request->reserved &&
        request->result==(tcp_write_mode==5?-KERNEL_EPIPE:96));
    __atomic_store_n(&tcp_write_worker_published,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&tcp_write_pins_returned,__ATOMIC_ACQUIRE)) sched_yield();
    kernel_socket_protocol_leave(&scope);
    mm_test_finish();return NULL;
}
static void tcp_write_batch_case(struct kernel_heap *heap)
{
    struct kernel_wait_queue progress;struct kernel_wait_node work;
    kernel_wait_queue_init(&progress);kernel_wait_node_init_callback(&work,tcp_write_work_notification,NULL);
    kernel_wait_queue_add(&progress,&work);kernel_socket_network_hooks(NULL,&progress,NULL,NULL);
    /* Stop/queued COPY uses a separate stop caller and live worker in network_owner.c. */
    for(unsigned mode=0;mode<6;mode++) {
        struct kernel_socket *listener=NULL,*client=NULL,*server=NULL;
        struct kernel_socket_address address={.family=KERNEL_SOCKET_AF_INET,.port=(uint16_t)(24050+mode),.bytes={127,0,0,1}};
        assert(!kernel_socket_create(heap,KERNEL_SOCKET_AF_INET,1,&listener) &&
            !kernel_socket_bind(listener,&address) && !kernel_socket_listen(listener,4) &&
            !kernel_socket_create(heap,KERNEL_SOCKET_AF_INET,1,&client));
        int connected=kernel_socket_connect(client,&address,0);assert(!connected || connected==-KERNEL_EINPROGRESS);
        for(unsigned i=0;i<8;i++) (void)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
        assert(!kernel_socket_accept(listener,&server, NULL) && !kernel_socket_set_option(client,KERNEL_SOCKET_NODELAY,1));
        struct kernel_files handle={0};struct kernel_open_file_description *out=NULL,*in=NULL,*owner=NULL;
        assert(!kernel_files_create(&handle,heap) && !kernel_open_file_create_socket(heap,client,2,&out) &&
            !kernel_open_file_create_socket(heap,server,2|KERNEL_FILES_O_NONBLOCK,&in) &&
            !kernel_files_install_new_owned_at(&handle,0,0,&out) && !kernel_files_install_new_owned_at(&handle,1,0,&in));
        int64_t result;assert(!kernel_files_pin(&handle,0,&owner,&result) && !result && owner->references==2);
        tcp_write_source_page=0x800000;uint64_t mapped;
        assert(!kernel_mm_mmap_anonymous(&mm,tcp_write_source_page,2U*BOAROS_PAGE_SIZE,
            KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED_NOREPLACE,&mapped));
        uint64_t source=tcp_write_source_page+BOAROS_PAGE_SIZE-31;
        unsigned char expected[96],actual[96]={0};
        for(unsigned i=0;i<sizeof(expected);i++) expected[i]=(unsigned char)(17U*i+mode+1U);
        size_t copied;
        assert(!kernel_copy_to_user(&mm,source,expected,31,&copied) && copied==31);
        if(mode==1) memset(expected+31,0,65);
        else assert(!kernel_copy_to_user(&mm,source+31,expected+31,65,&copied) && copied==65);
        if(mode==2) assert(!kernel_mm_mprotect(&mm,tcp_write_source_page+BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE,0));
        assert(!kernel_copy_to_user(&mm,0x200080,actual,sizeof(actual),&copied) && copied==sizeof(actual));
        tcp_write_mode=mode;tcp_write_held=tcp_write_pin_count=tcp_write_unpin_count=0;
        tcp_write_misses=tcp_write_denied=tcp_write_conflicts=tcp_write_commits=0;
        tcp_write_edit_started=tcp_write_edit_closed=tcp_write_edit_done=0;
        tcp_write_worker_ready=tcp_write_queued_ready=tcp_write_worker_published=tcp_write_pins_returned=0;
        tcp_write_notifications=tcp_write_signal_count=0;tcp_write_queued_request=NULL;
        tcp_write_caller_task=kernel_task_current();tcp_write_client_socket=client;
        struct kernel_socket_statistics before_write,after_write;
        kernel_socket_get_statistics(&before_write);
        pthread_t other;
        if(mode==3) assert(!pthread_create(&other,NULL,tcp_write_revoke,NULL));
        if(mode>=4) {
            tcp_write_park_baseline=mm_test_parked(0);
            assert(!pthread_create(&other,NULL,tcp_write_protocol_worker,NULL));
            while(!__atomic_load_n(&tcp_write_worker_ready,__ATOMIC_ACQUIRE)) sched_yield();
        }
        tcp_write_observing=1;
        assert(!kernel_files_write(&handle,&mm,0,source,sizeof(expected),&result));
        tcp_write_observing=0;
        if(mode>=3) assert(!pthread_join(other,NULL));
        kernel_socket_get_statistics(&after_write);
        uint32_t accepted=mode==2 || mode==3?31:96;
        assert(result==(mode==5?-KERNEL_EPIPE:mode==6?-KERNEL_EIO:(int64_t)accepted) && !tcp_write_held &&
            tcp_write_pin_count==tcp_write_unpin_count && !kernel_io_context_current()->user_pins &&
            !kernel_task_current()->socket_write_request && !kernel_task_current()->io_buffer && owner->references==2);
        if(mode==0 || mode>=4) assert(tcp_write_commits==1 && tcp_write_commit_segments[0]==2 && tcp_write_commit_bytes[0]==96);
        if(mode==1) assert(tcp_write_misses==1 && tcp_write_commits==2 && tcp_write_commit_segments[0]==1 &&
            tcp_write_commit_bytes[0]==31 && tcp_write_commit_bytes[1]==65);
        if(mode==2) assert(tcp_write_denied+tcp_write_misses==1 && tcp_write_commits==1 && tcp_write_commit_bytes[0]==31);
        if(mode==3) assert(tcp_write_conflicts==1 && tcp_write_edit_done && tcp_write_commits==1 && tcp_write_commit_bytes[0]==31);
        if(mode==5) assert(tcp_write_signal_count==1);
        if(mode==6) assert(!tcp_write_signal_count && after_write.tcp_written_bytes==before_write.tcp_written_bytes);
        if(mode<5) {
            assert(!kernel_files_read(&handle,&mm,1,0x200080,accepted,&result) && result==accepted);
            assert(!kernel_copy_from_user(&mm,actual,0x200080,accepted,&copied) && copied==accepted && !memcmp(actual,expected,accepted));
            assert(!kernel_files_read(&handle,&mm,1,0x200080,1,&result) && result==-KERNEL_EAGAIN);
        }
        uint64_t scratch=kernel_task_current()->io_scratch_physical_address;
        assert(scratch && kernel_task_current()->io_scratch_data);
        if(mode==0) {
            assert(!kernel_files_write(&handle,&mm,0,source,sizeof(expected),&result) && result==sizeof(expected) &&
                kernel_task_current()->io_scratch_physical_address==scratch && !kernel_task_current()->io_buffer);
            assert(!kernel_files_read(&handle,&mm,1,0x200080,sizeof(expected),&result) && result==sizeof(expected));
            assert(!kernel_copy_from_user(&mm,actual,0x200080,sizeof(actual),&copied) &&
                copied==sizeof(actual) && !memcmp(actual,expected,sizeof(actual)));
        }
        struct kernel_mm_concurrency_state state;kernel_mm_get_concurrency_state(&mm,&state);
        assert(!state.pins && !state.write_pins && !state.pending_retirement);
        struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
        for(struct tcp_pcb *pcb=tcp_active_pcbs,*next;pcb;pcb=next) {
            next=pcb->next;if(pcb->callback_arg==client || pcb->callback_arg==server) tcp_abort(pcb);
        }
        kernel_socket_protocol_leave(&scope);
        assert(!kernel_open_file_release(&owner) && !kernel_files_release(&handle));kernel_socket_destroy(listener);
        assert(!tcp_active_pcbs && !tcp_tw_pcbs && !kernel_mm_munmap(&mm,tcp_write_source_page,2U*BOAROS_PAGE_SIZE));
        static const char *const names[]={"resident cross-page commit", "nonresident second page", "unreadable second page", "mprotect closes pin admission", "busy core worker COPY", "busy core EPIPE and caller SIGPIPE"};
        printf("PASS TCP batched write %s: real content/prefix, bounded pins, request result before unpin and final owners\n",names[mode]);
        mm_test_finish();mm_test_attach(0);mm_test_activate_task();mm_test_context=context;
    }
    kernel_socket_network_hooks(NULL,NULL,NULL,NULL);
    assert(!kernel_wait_node_remove_sync(&work) && !kernel_wait_queue_close(&progress) && !kernel_wait_queue_destroy(&progress));
}
