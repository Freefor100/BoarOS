/* Real socket/FD/wait/heap owners, shared with the MQ lifecycle regression. */
#define main file_io_unused_main
#include "file_io_concurrency.c"
#undef main
#include <kernel/socket_control.h>
enum kernel_open_file_status __real_kernel_open_file_release(struct kernel_open_file_description **);
enum kernel_open_file_status __wrap_kernel_open_file_release(struct kernel_open_file_description **owner)
{
    /* The host scheduler has no user-service migration. Execute final socket
     * cleanup as CPU0 service, retaining the actual OFD/endpoint/packet owners. */
    struct kernel_task *task=kernel_task_current();
    int service=owner && *owner && task && task->controlled &&
        kernel_open_file_kind(*owner)==KERNEL_OPEN_FILE_KIND_SOCKET &&
        kernel_open_file_references(*owner)<=1;
    if(service) {mm_test_bind(NULL);mm_test_activate_task();}
    enum kernel_open_file_status result=__real_kernel_open_file_release(owner);
    if(service) {mm_test_bind(&mm);mm_test_bind_files(&files);}
    return result;
}


static unsigned char expected_cookie[32];
static struct kernel_socket *create_receiver(int type,
    struct kernel_open_file_description **pin)
{
    int64_t result;
    assert(!kernel_files_socket_create(&files,KERNEL_SOCKET_AF_NETLINK,type, 0,
        KERNEL_FILES_O_NONBLOCK|KERNEL_FILES_O_CLOEXEC,&result) && result==0);
    assert(files.record->slots[0].flags==KERNEL_FILES_FD_CLOEXEC);
    assert(!kernel_files_pin(&files,0,pin,&result) && !result);
    struct kernel_socket *socket=kernel_open_file_socket(*pin);
    assert(socket && kernel_socket_preserves_messages(socket));
    int actual;assert(!kernel_socket_get_option(socket,KERNEL_SOCKET_TYPE,&actual) && actual==type);
    assert(kernel_open_file_flags(*pin)&KERNEL_FILES_O_NONBLOCK);
    struct kernel_socket_address address={0};
    assert(!kernel_socket_getname(socket,&address) && address.family==KERNEL_SOCKET_AF_NETLINK &&
        !address.scope && !address.port);
    assert(!kernel_socket_bind(socket,&address) &&
        kernel_socket_connect(socket,&address,0)==-KERNEL_EOPNOTSUPP &&
        kernel_socket_shutdown(socket,2)==-KERNEL_EOPNOTSUPP);
    assert(kernel_socket_write_packet_buffer(pin,"route",5,0)==5);
    assert(!(kernel_socket_poll(socket,NULL)&KERNEL_POLLIN));
    return socket;
}

struct route_header { uint32_t length; uint16_t type, flags; uint32_t sequence, port; };
static unsigned route_align(unsigned n) {return (n+3U)&~3U;}
static unsigned char response[16384];
static unsigned exchange(struct kernel_open_file_description **pin,const void *data,unsigned size,unsigned capacity,unsigned flags)
{
    struct kernel_socket *socket=kernel_open_file_socket(*pin);
    /* Host has no user-service migration: execute protocol snapshot as CPU0
     * service, retaining the actual socket, active-write and OFD ownership. */
    mm_test_bind(NULL);mm_test_activate_task();
    assert(kernel_socket_write_packet_buffer(pin,data,size,0)==(int)size);
    mm_test_bind(&mm);mm_test_bind_files(&files);
    struct kernel_socket_message message={.flags=flags};
    struct kernel_socket_read_request read={0};
    int n=kernel_socket_reserve_read(socket,kernel_task_current(),&read,pin,capacity,1,&message);
    assert(n>=0);
    struct kernel_socket_address source={0};uint32_t full;
    kernel_socket_read_info(&read,&source,&full);
    assert(source.family==KERNEL_SOCKET_AF_NETLINK && !source.scope);
    kernel_socket_copy_read(&read,0,response,(unsigned)n);
    assert(kernel_socket_finish_read(&read,0)==(unsigned)n && *pin);
    return full;
}
static void route_cases(struct kernel_heap *heap)
{
    struct kernel_open_file_description *pin=NULL;
    struct kernel_socket *socket=create_receiver(3,&pin);
    struct kernel_socket_address address;
    assert(!kernel_socket_getname(socket,&address) && address.scope);
    struct kernel_socket *other=NULL;
    assert(!kernel_socket_create(heap,KERNEL_SOCKET_AF_NETLINK,2,0,&other));
    assert(kernel_socket_bind(other,&address)==-KERNEL_EADDRINUSE);
    unsigned port=address.scope;address.scope=0;assert(!kernel_socket_bind(other,&address));
    struct kernel_socket_address second;assert(!kernel_socket_getname(other,&second) && second.scope && second.scope!=port);
    kernel_socket_destroy(other);
    struct {struct route_header header;unsigned char payload[16];} request={{32,18,0x301,123,0},{0}};
    unsigned n=exchange(&pin,&request,sizeof(request),sizeof(response),0),links=0,done=0;
    for(unsigned at=0;at<n;) {
        struct route_header *h=(void *)(response+at);
        assert(h->length>=16 && h->length<=n-at && h->sequence==123 && h->port==port);
        if(h->type==16)links++;else {assert(h->type==3 && h->flags==2);done++;}
        at+=route_align(h->length);
    }
    assert(links && done==1);
    request.header.type=22;request.header.length=24;request.header.sequence++;
    n=exchange(&pin,&request,24,sizeof(response),0);assert(n>20);
    request.header.type=26;request.header.length=28;request.header.sequence++;
    n=exchange(&pin,&request,28,sizeof(response),0);assert(n>=20);
    request.header.type=99;request.header.length=16;
    n=exchange(&pin,&request,16,sizeof(response),0);
    assert(n==36 && ((struct route_header *)response)->type==2 && *(int32_t *)(response+16)==-KERNEL_EOPNOTSUPP);
    request.header.type=18;request.header.length=32;request.header.flags=1;
    n=exchange(&pin,&request,32,sizeof(response),0);
    assert(n==52 && *(int32_t *)(response+16)==-KERNEL_EINVAL);
    request.header.flags=0x301;
    assert(exchange(&pin,&request,32,1,0)>1);
    /* A mixed queue is closed with both response and notification owners. */
    mm_test_bind(NULL);mm_test_activate_task();
    assert(kernel_socket_write_packet_buffer(&pin,&request,32,0)==32);
    mm_test_bind(&mm);mm_test_bind_files(&files);
    struct kernel_netlink_cookie *cookie=NULL;
    assert(!kernel_socket_mqueue_cookie_prepare(kernel_task_current(),0,expected_cookie,&cookie));
    kernel_socket_mqueue_cookie_send(&cookie,1);
    assert(!kernel_open_file_release(&pin));int64_t result;assert(!kernel_files_close(&files,0,&result) && !result);
    socket=create_receiver(2,&pin);
    assert(!kernel_socket_set_option(socket,KERNEL_SOCKET_RCVBUF,1));
    mm_test_bind(NULL);mm_test_activate_task();
    unsigned accepted=0;int sent;
    do {sent=kernel_socket_write_packet_buffer(&pin,&request,32,0);if(sent==32)accepted++;}while(sent==32 && accepted<100);
    assert(accepted && accepted<100 && sent==-KERNEL_ENOBUFS && pin);
    assert(kernel_socket_write_packet_buffer(&pin,&request,4097,0)==-KERNEL_EMSGSIZE && pin);
    mm_test_bind(&mm);mm_test_bind_files(&files);
    assert(!kernel_open_file_release(&pin));assert(!kernel_files_close(&files,0,&result) && !result);
    puts("PASS RTNETLINK bind dump seq multipart error truncation quota mixed-MQ close");
}
int main(int argc,char **argv)
{
    (void)argv;assert(argc==2);setvbuf(stdout,NULL,_IONBF,0);mm_test_attach(0);mm_test_activate_task();
    struct physical_page_allocator pages={0};struct kernel_heap heap={0};
    initialize(&pages,&heap,arena,sizeof(arena));uint64_t baseline=physical_page_available(&pages);
    struct arch_mmu_page_table kernel={0};struct arch_mmu_user_space user={0};
    assert(!riscv_sv39_page_table_init(&kernel,&pages));kernel.state=ARCH_MMU_STATE_ACTIVE;
    assert(!arch_mmu_user_space_init(&user,&pages,&kernel) && !kernel_mm_create(&mm,&user) &&
        !kernel_mm_vma_enable(&mm,&heap) && !kernel_mm_brk_initialize(&mm,0x10000,0x20000));
    assert(!kernel_mm_context(&mm,&context));mm_test_context=context;
    assert(!kernel_files_create(&files,&heap));kernel_socket_network_initialize();mm_test_bind(&mm);mm_test_bind_files(&files);
    route_cases(&heap);
    assert(!kernel_files_release(&files));mm_test_context=0;
    assert(!kernel_mm_release(&mm) && !physical_page_release(&pages,kernel.root_address));mm_test_finish();
    struct kernel_heap_statistics stats;kernel_heap_get_statistics(&heap,&stats);
    assert(!stats.live_allocations && !stats.current_pages && !physical_page_allocator_audit(&pages) && physical_page_available(&pages)==baseline);
    puts("PASS RTNETLINK all owners returned");return 0;
}
