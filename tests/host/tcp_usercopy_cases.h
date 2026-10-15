/* TCP/lwIP、MM pin、syscall I/O均为生产实现；宿主只负责执行握手与等待。 */
#include <stdlib.h>
#include <kernel/time.h>
#include <kernel/random.h>
#include <kernel/socket.h>
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
static enum kernel_files_status tcp_writer_status;
static int64_t tcp_writer_bytes;
static uint64_t tcp_writer_available;
static void *tcp_user_writer(void *argument)
{
    struct kernel_files *handle=argument;
    mm_test_attach(1);mm_test_activate_task();kernel_cpu_current()->logical_id=0;mm_test_context=context;
    tcp_writer_status=kernel_files_write(handle,&mm,0,0x200000,8,&tcp_writer_bytes);
    tcp_writer_available=physical_page_available(mm.allocator);
    mm_test_finish();return 0;
}
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
    assert(!kernel_socket_accept(listener,&server));
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
    uintptr_t irq=kernel_socket_protocol_enter();
    while ((allocation=mem_malloc(1024))) { assert(count<32768);held[count++]=allocation; }
    while ((allocation=mem_malloc(1))) { assert(count<32768);held[count++]=allocation; }
    kernel_socket_protocol_leave(irq);
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
    irq=kernel_socket_protocol_enter();
    for(size_t i=0;i<count;i++) mem_free(held[i]);
    kernel_socket_protocol_leave(irq);
    (void)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1});
    assert(!pthread_join(writer,0) && !tcp_writer_status && tcp_writer_bytes==8);
    int64_t result;
    assert(!kernel_files_read(&handle,&mm,1,0x201000,8,&result) && result==8);
    char actual[8];assert(!kernel_copy_from_user(&mm,actual,0x201000,8,&copied) && copied==8);
    assert(!memcmp(actual,"newvalue",8));
    assert(!kernel_files_read(&handle,&mm,1,0x201000,1,&result) && result==-KERNEL_EAGAIN);
    assert(!kernel_mm_mprotect(&mm,0x200000,BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE));
    assert(tcp_writer_available==before && physical_page_available(mm.allocator)==before); /* 返回前也没有scratch。 */
    uint64_t tail=0x200000+4U*BOAROS_PAGE_SIZE-4U;
    assert(!kernel_copy_to_user(&mm,tail,"tail",4,&copied) && copied==4);
    assert(!kernel_files_write(&handle,&mm,0,tail,8,&result) && result==4);
    assert(!kernel_files_read(&handle,&mm,1,0x201000,8,&result) && result==4);
    assert(!kernel_copy_from_user(&mm,actual,0x201000,4,&copied) && copied==4 && !memcmp(actual,"tail",4));
    assert(!kernel_files_write(&handle,&mm,0,tail+4,1,&result) && result==-KERNEL_EFAULT);
    assert(!kernel_files_read(&handle,&mm,1,0x201000,1,&result) && result==-KERNEL_EAGAIN);
    irq=kernel_socket_protocol_enter();
    for(struct tcp_pcb *pcb=tcp_active_pcbs,*next;pcb;pcb=next) {
        next=pcb->next;if(pcb->callback_arg==client || pcb->callback_arg==server) tcp_abort(pcb);
    }
    kernel_socket_protocol_leave(irq);
    assert(!kernel_files_release(&handle));kernel_socket_destroy(listener);
    assert(!tcp_active_pcbs && !tcp_tw_pcbs);
    mm_test_finish();mm_test_attach(0);mm_test_activate_task();
    puts("PASS TCP direct user pin: ERR_MEM wait releases pin, remapped bytes accepted once, fault prefix, no scratch and final owners returned");
}
