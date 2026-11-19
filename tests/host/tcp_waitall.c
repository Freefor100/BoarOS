/* Reuse the real MM/files/lwIP fixture, not its unrelated filesystem cases. */
#define main unused_file_io_main
#define __wrap_kernel_time_monotonic_ns base_time_monotonic_ns
#define __wrap_kernel_wait_prepare base_wait_prepare
#include "file_io_concurrency.c"
#undef main
#undef __wrap_kernel_time_monotonic_ns
#undef __wrap_kernel_wait_prepare
#include <kernel/socket_control.h>

static _Thread_local int waitall_reader;
static uint64_t waitall_now;
static uint64_t waitall_deadlines[8];
static unsigned waitall_waits, waitall_done;
static struct kernel_wait_token waitall_token;
static struct kernel_wait_queue *waitall_queue;
uint64_t __wrap_kernel_time_monotonic_ns(void)
{ return waitall_reader ? __atomic_load_n(&waitall_now,__ATOMIC_ACQUIRE) : base_time_monotonic_ns(); }
enum kernel_scheduler_status __wrap_kernel_wait_prepare(struct kernel_wait_queue *queue,
    uint64_t deadline,int interruptible,struct kernel_wait_token *token)
{
    enum kernel_scheduler_status status=base_wait_prepare(queue,deadline,interruptible,token);
    if(!status && waitall_reader && queue==waitall_queue) {
        assert(waitall_waits<8 && interruptible && !tcp_protocol_held());
        waitall_deadlines[waitall_waits++]=deadline;waitall_token=*token;
    }
    return status;
}
struct waitall_call { struct kernel_files *files; uint64_t address; unsigned flags; int64_t result; };
static void *waitall_receive(void *arg)
{
    struct waitall_call *call=arg;
    mm_test_attach(1);mm_test_activate_task();mm_test_context=context;waitall_reader=1;
    struct kernel_open_file_description *pin=NULL;int64_t error;
    assert(!kernel_files_pin(call->files,1,&pin,&error) && !error);
    struct kernel_socket_message message={.flags=call->flags};
    struct kernel_uaccess_iovec iov[2]={{call->address,1},{call->address+1,3}};
    assert(!kernel_files_socket_iov_io(call->files,&mm,&pin,iov,2,4,
        call->flags|KERNEL_SOCKET_IO_MESSAGE,0,NULL,NULL,&call->result,&message));
    assert(pin && !kernel_open_file_release(&pin) && !message.control);
    waitall_reader=0;mm_test_finish();__atomic_store_n(&waitall_done,1,__ATOMIC_RELEASE);return NULL;
}
static void waitall_parked(unsigned baseline)
{
    while(mm_test_parked(1)==baseline && !__atomic_load_n(&waitall_done,__ATOMIC_ACQUIRE))sched_yield();
    assert(!__atomic_load_n(&waitall_done,__ATOMIC_ACQUIRE) && waitall_waits);
}
static void waitall_service(void)
{ for(unsigned i=0;i<8;i++)(void)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1}); }
static void waitall_case(struct kernel_heap *heap,unsigned mode)
{
    struct kernel_socket *listener=NULL,*client=NULL,*server=NULL;
    struct kernel_socket_address address={.family=KERNEL_SOCKET_AF_INET,.port=(uint16_t)(24100+mode),.bytes={127,0,0,1}};
    assert(!kernel_socket_create(heap,KERNEL_SOCKET_AF_INET,1,0,&listener) &&
        !kernel_socket_bind(listener,&address) && !kernel_socket_listen(listener,4) &&
        !kernel_socket_create(heap,KERNEL_SOCKET_AF_INET,1,0,&client));
    int connected=kernel_socket_connect(client,&address,0);assert(!connected || connected==-KERNEL_EINPROGRESS);
    waitall_service();assert(!kernel_socket_accept(listener,&server,NULL));
    assert(!kernel_socket_set_option(client,KERNEL_SOCKET_NODELAY,1));
    struct kernel_files local={0};struct kernel_open_file_description *out=NULL,*in=NULL;
    assert(!kernel_files_create(&local,heap) && !kernel_open_file_create_socket(heap,client,2,&out) &&
        !kernel_open_file_create_socket(heap,server,2,&in));
    assert(!kernel_files_install_new_owned_at(&local,0,0,&out) && !kernel_files_install_new_owned_at(&local,1,0,&in));
    waitall_queue=kernel_socket_wait_queue(server);waitall_waits=waitall_done=0;waitall_now=1000;
    /* A completed prefix must not annotate the syscall for restart. */
    if(mode>=6 && mode!=7)kernel_socket_set_receive_timeout(server,20);
    if(mode<8)assert(socket_buffer_write(&local,0,"ab",2,0)==2);
    waitall_service();
    struct waitall_call call={&local,mode==4?0x200000+4*BOAROS_PAGE_SIZE-2:0x201000,
        KERNEL_SOCKET_MSG_WAITALL|((mode==1 || mode==2)?KERNEL_SOCKET_MSG_PEEK:0),0};
    unsigned parked=mm_test_parked(1);pthread_t reader;
    assert(!pthread_create(&reader,NULL,waitall_receive,&call));waitall_parked(parked);
    char bytes[4];size_t copied;
    if(mode<8)assert(!kernel_copy_from_user(&mm,bytes,call.address,2,&copied) && copied==2 && !memcmp(bytes,"ab",2));
    if(mode<=2 || mode==4) {
        if(mode==2) {int64_t result;assert(!kernel_files_read(&local,&mm,1,0x202000,2,&result) && result==2);}
        assert(socket_buffer_write(&local,0,"cd",2,0)==2);waitall_service();
    } else if(mode==3) { assert(!kernel_socket_shutdown(client,1));waitall_service(); }
    else if(mode==5 || mode==8) assert(!kernel_wait_notify(&waitall_token,KERNEL_WAIT_SIGNALLED));
    else if(mode==6 || mode==9) {
        if(mode==6) {
            parked=mm_test_parked(1);assert(socket_buffer_write(&local,0,"c",1,0)==1);waitall_service();
            waitall_parked(parked);assert(waitall_waits==2 && waitall_deadlines[0]==1020 && waitall_deadlines[1]==1020);
        }
        __atomic_store_n(&waitall_now,1021,__ATOMIC_RELEASE);
        assert(!kernel_wait_notify(&waitall_token,KERNEL_WAIT_TIMEOUT));
    } else {
        struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
        for(struct tcp_pcb *pcb=tcp_active_pcbs,*next;pcb;pcb=next) {
            next=pcb->next;if(pcb->callback_arg==client)tcp_abort(pcb);
        }
        kernel_socket_protocol_leave(&scope);waitall_service();
    }
    assert(!pthread_join(reader,NULL));
    int64_t expected=mode<=2?4:mode==6?3:mode==8?-KERNEL_EINTR:mode==9?-KERNEL_EAGAIN:2;
    assert(call.result==expected);
    if(mode<=2)assert(!kernel_copy_from_user(&mm,bytes,call.address,4,&copied) && copied==4 && !memcmp(bytes,"abcd",4));
    int64_t result;
    if(mode==1 || mode==2 || mode==4) {
        unsigned count=mode==1?4:2;
        assert(!kernel_files_read(&local,&mm,1,0x202000,count,&result) && result==count);
        assert(!kernel_copy_from_user(&mm,bytes,0x202000,count,&copied) && !memcmp(bytes,mode==1?"abcd":"cd",count));
    }
    if(mode==7)assert(!kernel_files_read(&local,&mm,1,0x202000,1,&result) && result==-KERNEL_ECONNRESET);
    struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
    for(struct tcp_pcb *pcb=tcp_active_pcbs,*next;pcb;pcb=next) {
        next=pcb->next;if(pcb->callback_arg==client || pcb->callback_arg==server)tcp_abort(pcb);
    }
    kernel_socket_protocol_leave(&scope);
    assert(!kernel_files_release(&local));kernel_socket_destroy(listener);waitall_service();
    assert(!tcp_active_pcbs && !tcp_tw_pcbs);printf("PASS TCP WAITALL host case %u\n",mode);
}
int main(int argc,char **argv)
{
    (void)argv;assert(argc==2);setvbuf(stdout,NULL,_IONBF,0);mm_test_attach(0);mm_test_activate_task();
    struct physical_page_allocator pages={0};struct kernel_heap heap={0};
    initialize(&pages,&heap,arena,sizeof(arena));uint64_t baseline=physical_page_available(&pages);
    struct arch_mmu_page_table kernel={0};struct arch_mmu_user_space user={0};
    assert(!riscv_sv39_page_table_init(&kernel,&pages));kernel.state=ARCH_MMU_STATE_ACTIVE;
    assert(!arch_mmu_user_space_init(&user,&pages,&kernel) && !kernel_mm_create(&mm,&user) &&
        !kernel_mm_vma_enable(&mm,&heap) && !kernel_mm_brk_initialize(&mm,0x10000,0x20000) && !kernel_mm_context(&mm,&context));mm_test_context=context;
    uint64_t address;assert(!kernel_mm_mmap_anonymous(&mm,0x200000,4*BOAROS_PAGE_SIZE,
        KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED_NOREPLACE,&address));
    kernel_socket_network_initialize();
    for(unsigned mode=0;mode<10;mode++)waitall_case(&heap,mode);
    mm_test_finish();mm_test_context=0;
    assert(!kernel_mm_release(&mm) && !physical_page_release(&pages,kernel.root_address));
    struct kernel_heap_statistics stats;kernel_heap_get_statistics(&heap,&stats);
    assert(!stats.live_allocations && !stats.current_pages && !physical_page_allocator_audit(&pages) && physical_page_available(&pages)==baseline);
    puts("PASS TCP WAITALL owners returned");return 0;
}
