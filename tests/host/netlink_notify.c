/* Real FD/OFD/MM/heap/socket owners; only scheduler execution uses pthreads. */
#define main file_io_full_main
#define __wrap_kernel_wait_park file_io_wrap_kernel_wait_park
#include "file_io_concurrency.c"
#undef __wrap_kernel_wait_park
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

static pthread_mutex_t cookie_gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cookie_changed=PTHREAD_COND_INITIALIZER;
static unsigned cookie_waiting,cookie_resume,cookie_done;
static _Thread_local unsigned cookie_actor;
enum kernel_scheduler_status __wrap_kernel_wait_park(const struct kernel_wait_token *token,
    enum kernel_wait_wake_reason *reason)
{
    if(cookie_actor) {
        assert(!kernel_cpu_current()->raw_locks);
        assert(!pthread_mutex_lock(&cookie_gate));
        cookie_waiting=1;assert(!pthread_cond_broadcast(&cookie_changed));
        while(!cookie_resume)assert(!pthread_cond_wait(&cookie_changed,&cookie_gate));
        assert(!pthread_mutex_unlock(&cookie_gate));
    }
    return file_io_wrap_kernel_wait_park(token,reason);
}

static unsigned char expected_cookie[32];
static struct kernel_netlink_cookie *pending_cookies[128];
static unsigned pending_count;
static int pending_result;
static void *fill_receiver(void *unused)
{
    (void)unused;
    mm_test_attach(1);mm_test_bind(&mm);mm_test_bind_files(&files);cookie_actor=1;
    for(;pending_count<128;pending_count++) {
        int result=kernel_socket_mqueue_cookie_prepare(kernel_task_current(),0,
            expected_cookie,&pending_cookies[pending_count]);
        if(result) {pending_result=result;break;}
        if(cookie_waiting) {pending_count++;break;}
    }
    __atomic_store_n(&cookie_done,1,__ATOMIC_RELEASE);
    cookie_actor=0;mm_test_finish();return NULL;
}

static struct kernel_socket *create_receiver(int type,
    struct kernel_open_file_description **pin)
{
    int64_t result;
    assert(!kernel_files_socket_create(&files,KERNEL_SOCKET_AF_NETLINK,type,
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
    assert(kernel_socket_bind(socket,&address)==-KERNEL_EOPNOTSUPP &&
        kernel_socket_connect(socket,&address,0)==-KERNEL_EOPNOTSUPP &&
        kernel_socket_shutdown(socket,2)==-KERNEL_EOPNOTSUPP);
    assert(kernel_socket_write_packet_buffer(pin,"route",5,0)==-KERNEL_EOPNOTSUPP);
    assert(!(kernel_socket_poll(socket,NULL)&KERNEL_POLLIN));
    return socket;
}
static void expect_cookie(unsigned reason)
{
    unsigned char actual[32];size_t copied;
    assert(kernel_copy_from_user(&mm,actual,0x200100,sizeof(actual),&copied)==KERNEL_UACCESS_STATUS_OK &&
        copied==sizeof(actual));
    assert(!memcmp(actual,expected_cookie,sizeof(actual)-1) && actual[31]==reason);
}
static void receiver_records(int type)
{
    struct kernel_open_file_description *pin=NULL;
    struct kernel_socket *socket=create_receiver(type,&pin);
    struct kernel_netlink_cookie *first=NULL,*second=NULL,*cancelled=NULL;
    assert(!kernel_socket_mqueue_cookie_prepare(kernel_task_current(),0,expected_cookie,&first) &&
        !kernel_socket_mqueue_cookie_prepare(kernel_task_current(),0,expected_cookie,&second) &&
        !kernel_socket_mqueue_cookie_prepare(kernel_task_current(),0,expected_cookie,&cancelled));
    kernel_socket_mqueue_cookie_cancel(&cancelled);assert(!cancelled);
    kernel_socket_mqueue_cookie_send(&first,1);kernel_socket_mqueue_cookie_send(&second,2);
    assert(!first && !second && (kernel_socket_poll(socket,NULL)&KERNEL_POLLIN));
    struct kernel_socket_message message={.flags=KERNEL_SOCKET_MSG_PEEK};
    struct kernel_socket_read_request request={0};
    assert(kernel_socket_reserve_read(socket,kernel_task_current(),&request,&pin,32,1,&message)==32);
    struct kernel_socket_address source={0};uint32_t length;
    kernel_socket_read_info(&request,&source,&length);
    assert(source.family==KERNEL_SOCKET_AF_NETLINK && !source.scope && !source.port &&
        !source.bytes[0] && length==32);
    unsigned char actual[32];kernel_socket_copy_read(&request,0,actual,sizeof(actual));
    assert(!memcmp(actual,expected_cookie,31) && actual[31]==1);
    assert(kernel_socket_finish_read(&request,0)==32 && pin);
    assert(!kernel_open_file_release(&pin));
    int64_t result;
    assert(!kernel_files_read(&files,&mm,0,0x200100,64,&result) && result==32);expect_cookie(1);
    struct kernel_uaccess_iovec vectors[2]={{0x200100,8},{0x200108,24}};size_t copied;
    assert(!kernel_copy_to_user(&mm,0x200000,vectors,sizeof(vectors),&copied) && copied==sizeof(vectors));
    assert(!kernel_files_readv(&files,&mm,0,0x200000,2,&result) && result==32);expect_cookie(2);
    assert(!kernel_files_read(&files,&mm,0,0x200100,32,&result) && result==-KERNEL_EAGAIN);
    assert(!kernel_socket_mqueue_cookie_prepare(kernel_task_current(),0,expected_cookie,&first));
    kernel_socket_mqueue_cookie_send(&first,1);
    assert(!kernel_files_read(&files,&mm,0,0x300000,32,&result) && result==-KERNEL_EFAULT);
    assert(!kernel_files_read(&files,&mm,0,0x200100,32,&result) && result==-KERNEL_EAGAIN);
    assert(!kernel_socket_mqueue_cookie_prepare(kernel_task_current(),0,expected_cookie,&first) &&
        !kernel_socket_mqueue_cookie_prepare(kernel_task_current(),0,expected_cookie,&second));
    assert(!kernel_files_close(&files,0,&result) && !result);
    kernel_socket_mqueue_cookie_send(&first,1);kernel_socket_mqueue_cookie_cancel(&second);
    assert(!first && !second);
    printf("PASS NETLINK %s: exact 32-byte WOKENUP/REMOVED, source kernel0, PEEK/read/readv, receive fault discard, cancellation and final-close storage\n",type==3?"RAW":"DGRAM");
}

int main(int argc,char **argv)
{
    (void)argv;assert(argc==2);setvbuf(stdout,NULL,_IONBF,0);
    mm_test_attach(0);mm_test_activate_task();
    struct physical_page_allocator pages={0};struct kernel_heap heap={0};
    initialize(&pages,&heap,arena,sizeof(arena));uint64_t baseline=physical_page_available(&pages);
    struct arch_mmu_page_table kernel={0};struct arch_mmu_user_space user={0};
    assert(!riscv_sv39_page_table_init(&kernel,&pages));kernel.state=ARCH_MMU_STATE_ACTIVE;
    assert(!arch_mmu_user_space_init(&user,&pages,&kernel) && !kernel_mm_create(&mm,&user) &&
        !kernel_mm_vma_enable(&mm,&heap) && !kernel_mm_brk_initialize(&mm,0x10000,0x20000));
    assert(!kernel_mm_context(&mm,&context));mm_test_context=context;
    uint64_t address;assert(!kernel_mm_mmap_anonymous(&mm,0x200000,BOAROS_PAGE_SIZE,
        KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED_NOREPLACE,&address));
    assert(!kernel_files_create(&files,&heap));mm_test_bind(&mm);mm_test_bind_files(&files);
    for(unsigned i=0;i<32;i++)expected_cookie[i]=(unsigned char)(i+20);
    struct kernel_netlink_cookie *cookie=NULL;
    assert(kernel_socket_mqueue_cookie_prepare(kernel_task_current(),-1,expected_cookie,&cookie)==-KERNEL_EBADF);
    receiver_records(3);receiver_records(2);

    for(unsigned close_fd=0;close_fd<2;close_fd++) {
        struct kernel_open_file_description *pin=NULL;struct kernel_socket *socket=create_receiver(3,&pin);
        assert(!kernel_socket_set_option(socket,KERNEL_SOCKET_RCVBUF,1) && !kernel_open_file_release(&pin));
        pending_count=cookie_waiting=cookie_resume=cookie_done=0;pending_result=0;
        unsigned parked_before=mm_test_parked(1);
        pthread_t thread;assert(!pthread_create(&thread,NULL,fill_receiver,NULL));
        assert(!pthread_mutex_lock(&cookie_gate));
        while(!cookie_waiting)assert(!pthread_cond_wait(&cookie_changed,&cookie_gate));
        assert(pending_count>1 && pending_count<128);
        int64_t result;
        if(close_fd) {
            cookie_resume=1;assert(!pthread_cond_broadcast(&cookie_changed));
            assert(!pthread_mutex_unlock(&cookie_gate));
            while(mm_test_parked(1)==parked_before)sched_yield();
            assert(!kernel_files_close(&files,0,&result) && !result);
            uint64_t deadline=kernel_time_monotonic_ns()+UINT64_C(2000000000);
            while(!__atomic_load_n(&cookie_done,__ATOMIC_ACQUIRE) && kernel_time_monotonic_ns()<deadline)sched_yield();
            if(!__atomic_load_n(&cookie_done,__ATOMIC_ACQUIRE)) {
                fputs("FAIL NETLINK close alone did not wake the committed quota waiter; no cookie cancellation was issued\n",stderr);
                abort();
            }
        } else {
            kernel_socket_mqueue_cookie_send(&pending_cookies[0],1);
            assert(!kernel_files_read(&files,&mm,0,0x200100,32,&result) && result==32);expect_cookie(1);
            cookie_resume=1;assert(!pthread_cond_broadcast(&cookie_changed));
            assert(!pthread_mutex_unlock(&cookie_gate));
        }
        assert(!pthread_join(thread,NULL));
        assert(pending_result==(close_fd?-KERNEL_EBADF:0));
        if(!close_fd)assert(!kernel_files_close(&files,0,&result) && !result);
        for(unsigned i=0;i<pending_count;i++)kernel_socket_mqueue_cookie_cancel(&pending_cookies[i]);
    }
    puts("PASS NETLINK quota: unpublished cookies reserve receiver memory; receive returns quota, and close alone wakes the committed waiter to retry fd lookup with EBADF");

    assert(!kernel_files_release(&files));mm_test_context=0;
    assert(!kernel_mm_release(&mm) && !physical_page_release(&pages,kernel.root_address));mm_test_finish();
    struct kernel_heap_statistics stats;kernel_heap_get_statistics(&heap,&stats);
    assert(!stats.live_allocations && !stats.current_pages && !physical_page_allocator_audit(&pages) &&
        physical_page_available(&pages)==baseline);
    puts("PASS NETLINK owners: every FD/OFD, pending cookie, queued/faulted packet, wait token, endpoint and heap page returned");
}
