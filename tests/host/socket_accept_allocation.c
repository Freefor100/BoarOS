/* 复用现有宿主执行模型；本入口只运行accept及其相邻所有权路径。 */
#define main file_io_full_main
#include "file_io_concurrency.c"
#undef main
#include <kernel/credentials.h>

static unsigned accept_ofd_failure,accept_ofd_allocations;
enum kernel_heap_status __real_kernel_heap_allocate_zeroed(struct kernel_heap *,size_t,size_t,void **);
enum kernel_heap_status __wrap_kernel_heap_allocate_zeroed(struct kernel_heap *heap,
    size_t count,size_t size,void **owner)
{
    if(accept_ofd_failure && count==1 && size==sizeof(struct kernel_open_file_description)) {
        if(++accept_ofd_allocations==accept_ofd_failure) return KERNEL_HEAP_STATUS_EMPTY;
    }
    return __real_kernel_heap_allocate_zeroed(heap,count,size,owner);
}

static void expect_no_accept_fd(struct kernel_files *handle)
{
    struct kernel_open_file_description *pin=NULL;
    int64_t result;
    assert(!kernel_files_pin(handle,0,&pin,&result) && result==-KERNEL_EBADF && !pin);
    assert(!handle->record->statistics.current_open_fds &&
        !handle->record->slots[0].flags && !handle->record->slots[0].description);
}

static void exchange_packet(struct kernel_files *from,struct kernel_files *to,
    const char *bytes)
{
    uint32_t length=(uint32_t)strlen(bytes);
    assert(socket_buffer_write(from,0,bytes,length,KERNEL_SOCKET_MSG_NOSIGNAL)==(int)length);
    struct kernel_open_file_description *pin=NULL;
    int64_t result;
    assert(!kernel_files_pin(to,0,&pin,&result) && !result);
    struct kernel_socket_read_request request={0};
    assert(kernel_socket_reserve_read(kernel_open_file_socket(pin),kernel_task_current(),
        &request,&pin,length,1)==(int)length);
    char actual[32]={0};
    assert(length<sizeof(actual));
    kernel_socket_copy_read(&request,0,actual,length);
    assert(!memcmp(actual,bytes,length));
    assert(kernel_socket_finish_read(&request,0)==length && !kernel_open_file_release(&pin));
}

struct accept_copy_context { struct kernel_files *files; unsigned calls; int error; };
static int accept_copy_peer(void *argument,const struct kernel_socket_address *address)
{
    struct accept_copy_context *copy=argument;
    struct kernel_open_file_description *pin=NULL;
    int64_t result;
    assert(address->family==KERNEL_SOCKET_AF_UNIX);
    assert(!kernel_files_pin(copy->files,0,&pin,&result) && result==-KERNEL_EBADF && !pin);
    copy->calls++;
    return copy->error;
}

static void accept_allocation_case(struct kernel_heap *heap,
    struct kernel_credentials *credential,uint32_t flags)
{
    const unsigned char name[]={0,'a','c','c','e','p','t'};
    const struct kernel_socket_address address={.family=KERNEL_SOCKET_AF_UNIX,
        .unix_length=sizeof(name),.unix_path=name};
    struct kernel_socket *listener_socket=NULL,*client_socket=NULL;
    struct kernel_open_file_description *listener=NULL,*client=NULL;
    struct kernel_files server_files={0},client_files={0};
    assert(!kernel_files_create(&server_files,heap) && !kernel_files_create(&client_files,heap));
    assert(!kernel_socket_create(heap,KERNEL_SOCKET_AF_UNIX,1,&listener_socket) &&
        !kernel_socket_bind(listener_socket,&address) && !kernel_socket_listen(listener_socket,1));
    assert(!kernel_open_file_create_socket(heap,listener_socket,2|KERNEL_FILES_O_NONBLOCK,&listener));
    assert(!kernel_socket_create(heap,KERNEL_SOCKET_AF_UNIX,1,&client_socket) &&
        !kernel_socket_connect(client_socket,&address,1));
    assert(!kernel_open_file_create_socket(heap,client_socket,2,&client) &&
        !kernel_files_install_new_owned_at(&client_files,0,0,&client));
    assert(kernel_socket_poll(listener_socket,NULL)&KERNEL_POLLIN);

    struct kernel_heap_statistics before,after;
    kernel_heap_get_statistics(heap,&before);
    unsigned references=credential->references;
    struct accept_copy_context copy={.files=&server_files};
    int64_t result;
    /* 本调用除OFD外无同尺寸分配；底层生产分配器只在该处返回EMPTY。 */
    accept_ofd_failure=1;accept_ofd_allocations=0;
    assert(!kernel_files_socket_accept(&server_files,listener,flags,accept_copy_peer,&copy,&result));
    accept_ofd_failure=0;
    assert(result==-KERNEL_ENOMEM && accept_ofd_allocations==1 && !copy.calls);
    expect_no_accept_fd(&server_files);
    kernel_heap_get_statistics(heap,&after);
    unsigned pending=!!(kernel_socket_poll(listener_socket,NULL)&KERNEL_POLLIN);
    unsigned failure_references=credential->references;
    int64_t retried;
    assert(!kernel_files_socket_accept(&server_files,listener,flags,accept_copy_peer,&copy,&retried));
    if(!pending || after.live_allocations!=before.live_allocations || failure_references!=references ||
        credential->references!=references+1 || retried!=0 || copy.calls!=1) {
        fprintf(stderr,"FAIL accept OFD ENOMEM lost pending connection: flags=%#x pending=%u retry=%lld heap=%llu/%llu credentials=%u/%u copy=%u\n",
            flags,pending,(long long)retried,(unsigned long long)after.live_allocations,
            (unsigned long long)before.live_allocations,credential->references,references+1,copy.calls);
        abort();
    }
    struct kernel_open_file_description *accepted=NULL;
    assert(!kernel_files_pin(&server_files,0,&accepted,&result) && !result);
    assert(kernel_open_file_flags(accepted)==(2U|(flags&KERNEL_FILES_O_NONBLOCK)) &&
        kernel_open_file_credentials(accepted)==credential && !kernel_open_file_release(&accepted));
    assert(server_files.record->slots[0].flags==
        (flags&KERNEL_FILES_O_CLOEXEC?KERNEL_FILES_FD_CLOEXEC:0));
    assert(!(kernel_socket_poll(listener_socket,NULL)&KERNEL_POLLIN));
    exchange_packet(&client_files,&server_files,"retained");
    exchange_packet(&server_files,&client_files,"reply");
    assert(!kernel_files_release(&server_files) && !kernel_files_release(&client_files) &&
        !kernel_open_file_release(&listener));
    printf("PASS accept%s OFD EMPTY: pending retained, retry exchanges bytes, FD/OFD/credentials returned\n",
        flags?"4":"");
}

static void accept_failure_cleanup_case(struct kernel_heap *heap,struct kernel_credentials *credential)
{
    const unsigned char name[]={0,'e','r','r','o','r'};
    const struct kernel_socket_address address={.family=KERNEL_SOCKET_AF_UNIX,
        .unix_length=sizeof(name),.unix_path=name};
    struct kernel_socket *socket=NULL,*client=NULL;
    struct kernel_open_file_description *listener=NULL;
    struct kernel_files handle={0};
    assert(!kernel_files_create(&handle,heap));
    assert(!kernel_socket_create(heap,KERNEL_SOCKET_AF_UNIX,1,&socket) &&
        !kernel_socket_bind(socket,&address) && !kernel_socket_listen(socket,1));
    assert(!kernel_open_file_create_socket(heap,socket,2|KERNEL_FILES_O_NONBLOCK,&listener));
    struct kernel_heap_statistics before,after;
    kernel_heap_get_statistics(heap,&before);
    unsigned references=credential->references;
    int64_t result;
    assert(!kernel_files_socket_accept(&handle,listener,0,NULL,NULL,&result) && result==-KERNEL_EAGAIN);
    expect_no_accept_fd(&handle);
    kernel_heap_get_statistics(heap,&after);
    assert(after.live_allocations==before.live_allocations && credential->references==references);
    assert(!kernel_socket_create(heap,KERNEL_SOCKET_AF_UNIX,1,&client) && !kernel_socket_connect(client,&address,1));
    struct accept_copy_context copy={.files=&handle,.error=-KERNEL_EFAULT};
    assert(!kernel_files_socket_accept(&handle,listener,KERNEL_FILES_O_CLOEXEC,
        accept_copy_peer,&copy,&result) && result==-KERNEL_EFAULT && copy.calls==1);
    expect_no_accept_fd(&handle);
    assert(!(kernel_socket_poll(socket,NULL)&KERNEL_POLLIN) && credential->references==references);
    kernel_socket_destroy(client);
    assert(!kernel_open_file_release(&listener) && !kernel_files_release(&handle));
    puts("PASS accept EAGAIN/copyout EFAULT: unpublished FD, prepared/attached OFD and credentials returned");
}

static void socket_creation_case(struct kernel_heap *heap,struct kernel_credentials *credential,
    struct physical_page_allocator *pages)
{
    struct kernel_files handle={0};assert(!kernel_files_create(&handle,heap));
    struct kernel_heap_statistics before,after;kernel_heap_get_statistics(heap,&before);
    unsigned references=credential->references;
    int64_t result;
    accept_ofd_failure=1;accept_ofd_allocations=0;
    assert(!kernel_files_socket_create(&handle,KERNEL_SOCKET_AF_UNIX,1,0,&result) &&
        result==-KERNEL_ENOMEM && accept_ofd_allocations==1);
    accept_ofd_failure=0;expect_no_accept_fd(&handle);
    kernel_heap_get_statistics(heap,&after);
    assert(after.live_allocations==before.live_allocations && credential->references==references);
    assert(!kernel_files_socket_create(&handle,KERNEL_SOCKET_AF_UNIX,1,
        KERNEL_FILES_O_NONBLOCK|KERNEL_FILES_O_CLOEXEC,&result) && !result);
    assert(credential->references==references+1 &&
        handle.record->slots[0].flags==KERNEL_FILES_FD_CLOEXEC);
    assert(!kernel_files_close(&handle,0,&result) && !result && credential->references==references);

    struct arch_mmu_page_table kernel={0};struct arch_mmu_user_space user={0};
    assert(!riscv_sv39_page_table_init(&kernel,pages));kernel.state=ARCH_MMU_STATE_ACTIVE;
    assert(!arch_mmu_user_space_init(&user,pages,&kernel) && !kernel_mm_create(&mm,&user) &&
        !kernel_mm_vma_enable(&mm,heap) && !kernel_mm_brk_initialize(&mm,0x10000,0x20000));
    assert(!kernel_mm_context(&mm,&context));mm_test_context=context;
    uint64_t address;
    assert(!kernel_mm_mmap_anonymous(&mm,0x200000,BOAROS_PAGE_SIZE,
        KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED_NOREPLACE,&address));
    for(unsigned failure=1;failure<=2;failure++) {
        kernel_heap_get_statistics(heap,&before);
        accept_ofd_failure=failure;accept_ofd_allocations=0;
        assert(!kernel_files_socketpair_create(&handle,&mm,1,0,address,&result) &&
            result==-KERNEL_ENOMEM && accept_ofd_allocations==failure);
        accept_ofd_failure=0;expect_no_accept_fd(&handle);
        kernel_heap_get_statistics(heap,&after);
        assert(after.live_allocations==before.live_allocations && credential->references==references);
    }
    assert(!kernel_files_socketpair_create(&handle,&mm,1,
        KERNEL_FILES_O_NONBLOCK|KERNEL_FILES_O_CLOEXEC,address,&result) && !result);
    int32_t pair[2]={-1,-1};size_t copied;
    assert(!kernel_copy_from_user(&mm,pair,address,sizeof(pair),&copied) && copied==sizeof(pair) &&
        pair[0]==0 && pair[1]==1 && credential->references==references+2);
    assert(handle.record->slots[0].flags==KERNEL_FILES_FD_CLOEXEC &&
        handle.record->slots[1].flags==KERNEL_FILES_FD_CLOEXEC);
    assert(!kernel_files_release(&handle) && credential->references==references);
    mm_test_context=0;
    assert(!kernel_mm_release(&mm) && !physical_page_release(pages,kernel.root_address));
    puts("PASS socket/socketpair: OFD EMPTY before either endpoint publication, flags/credentials and final owners");
}

int main(int argc,char **argv)
{
    (void)argv;assert(argc==2);setvbuf(stdout,NULL,_IONBF,0);
    mm_test_attach(0);mm_test_activate_task();
    struct kernel_task *task=kernel_task_current();
    task->tid=1;task->tid_owned=1;task->group_leader=task;task->group_members=1;
    struct physical_page_allocator pages={0};struct kernel_heap heap={0};
    initialize(&pages,&heap,arena,sizeof(arena));
    uint64_t baseline=physical_page_available(&pages);
    struct kernel_credentials *credential=kernel_credentials_clone(&heap,kernel_credentials_current());
    assert(credential);
    const struct kernel_credentials *previous=kernel_credentials_override(credential);
    accept_allocation_case(&heap,credential,0);
    assert(credential->references==1);
    accept_allocation_case(&heap,credential,KERNEL_FILES_O_NONBLOCK|KERNEL_FILES_O_CLOEXEC);
    assert(credential->references==1);
    accept_failure_cleanup_case(&heap,credential);
    assert(credential->references==1);
    socket_creation_case(&heap,credential,&pages);
    assert(credential->references==1);
    kernel_credentials_override(previous);kernel_credentials_put(credential);
    mm_test_finish();
    struct kernel_heap_statistics statistics;kernel_heap_get_statistics(&heap,&statistics);
    assert(!statistics.live_allocations && !statistics.current_pages &&
        !physical_page_allocator_audit(&pages) && physical_page_available(&pages)==baseline);
    puts("PASS accept final owners: heap live=0 pages=0, physical pages restored");
    return 0;
}
