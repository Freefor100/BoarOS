/* Existing real socket/lwIP/allocator fixture; test only keepalive behavior. */
#define main unused_file_io_main
#define __wrap_kernel_time_monotonic_ns base_time_monotonic_ns
#include "file_io_concurrency.c"
#undef main
#undef __wrap_kernel_time_monotonic_ns
static uint64_t keepalive_now;
uint64_t __wrap_kernel_time_monotonic_ns(void) { return keepalive_now; }
static void slow_tick(void)
{
    for(unsigned i=0;i<2;i++) {
        keepalive_now+=UINT64_C(250000000);
        (void)kernel_socket_service_pending((struct kernel_socket_service_budget){0,0,8});
    }
}
static void service(void)
{ for(unsigned i=0;i<8;i++)(void)kernel_socket_service_pending((struct kernel_socket_service_budget){8,8,1}); }
static int option(struct kernel_socket *socket,enum kernel_socket_option name)
{ int value=-1;assert(!kernel_socket_get_option(socket,name,&value));return value; }
static void keepalive_case(struct kernel_heap *heap)
{
    struct kernel_socket *listener=NULL,*client=NULL,*server=NULL,*udp=NULL;
    const enum kernel_socket_option names[]={KERNEL_SOCKET_KEEPIDLE,KERNEL_SOCKET_KEEPINTVL,KERNEL_SOCKET_KEEPCNT};
    const int defaults[]={7200,75,9},limits[]={32767,32767,127};
    assert(!kernel_socket_create(heap,KERNEL_SOCKET_AF_INET,1,0,&listener));
    assert(!kernel_socket_create(heap,KERNEL_SOCKET_AF_INET,2,0,&udp));
    for(unsigned i=0;i<3;i++) {
        assert(option(listener,names[i])==defaults[i]);
        assert(kernel_socket_set_option(udp,names[i],1)==-KERNEL_ENOPROTOOPT);
        assert(kernel_socket_set_option(listener,names[i],0)==-KERNEL_EINVAL);
        assert(kernel_socket_set_option(listener,names[i],-1)==-KERNEL_EINVAL);
        assert(kernel_socket_set_option(listener,names[i],limits[i]+1)==-KERNEL_EINVAL);
        assert(!kernel_socket_set_option(listener,names[i],limits[i]) && option(listener,names[i])==limits[i]);
        assert(!kernel_socket_set_option(listener,names[i],i+2));
    }
    struct kernel_socket_address address={.family=KERNEL_SOCKET_AF_INET,.port=24141,.bytes={127,0,0,1}};
    assert(!kernel_socket_bind(listener,&address) && !kernel_socket_listen(listener,4));
    for(unsigned i=0;i<3;i++)assert(option(listener,names[i])==(int)i+2);
    assert(!kernel_socket_create(heap,KERNEL_SOCKET_AF_INET,1,0,&client));
    int connected=kernel_socket_connect(client,&address,0);assert(!connected || connected==-KERNEL_EINPROGRESS);
    service();assert(!kernel_socket_accept(listener,&server,NULL));
    for(unsigned i=0;i<3;i++) {
        assert(option(server,names[i])==(int)i+2);
        assert(!kernel_socket_set_option(listener,names[i],i+6));
        assert(option(server,names[i])==(int)i+2 && option(listener,names[i])==(int)i+6);
    }
    assert(!kernel_socket_set_option(client,KERNEL_SOCKET_KEEPIDLE,1));
    assert(!kernel_socket_set_option(client,KERNEL_SOCKET_KEEPINTVL,1));
    assert(!kernel_socket_set_option(client,KERNEL_SOCKET_KEEPCNT,2));
    struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
    struct tcp_pcb *pcb=NULL,*peer=NULL;
    for(struct tcp_pcb *p=tcp_active_pcbs;p;p=p->next) {
        if(p->callback_arg==client)pcb=p;
        if(p->callback_arg==server)peer=p;
    }
    assert(pcb && peer && pcb->keep_idle==1000 && pcb->keep_intvl==1000 && pcb->keep_cnt==2);
    assert(peer->keep_idle==2000 && peer->keep_intvl==3000 && peer->keep_cnt==4);
    kernel_socket_protocol_leave(&scope);
    for(unsigned i=0;i<3;i++)slow_tick();
    assert(!pcb->keep_cnt_sent); /* settings do not silently enable keepalive */
    assert(!kernel_socket_set_option(client,KERNEL_SOCKET_KEEPALIVE,1));
    slow_tick();assert(pcb->keep_cnt_sent==1);
    service();assert(!pcb->keep_cnt_sent); /* peer ACK resets probe/activity state */
    slow_tick();slow_tick();assert(!pcb->keep_cnt_sent);
    /* Do not deliver loopback probes to the peer: actual timer/output executes,
     * but no ACK can reset activity. Advance timer ticks, not wall-clock sleeps. */
    slow_tick();assert(pcb->keep_cnt_sent==1);
    slow_tick();assert(pcb->keep_cnt_sent==1);
    slow_tick();assert(pcb->keep_cnt_sent==2);
    slow_tick();assert(pcb->keep_cnt_sent==2);
    slow_tick(); /* elapsed > idle + count * interval: abort */
    for(struct tcp_pcb *p=tcp_active_pcbs;p;p=p->next)assert(p->callback_arg!=client);
    assert(option(client,KERNEL_SOCKET_ERROR)==KERNEL_ETIMEDOUT);
    assert(option(client,KERNEL_SOCKET_KEEPIDLE)==1); /* policy survives PCB retirement */
    kernel_socket_destroy(client);kernel_socket_destroy(server);kernel_socket_destroy(listener);kernel_socket_destroy(udp);service();
    assert(!tcp_active_pcbs && !tcp_tw_pcbs);
    puts("PASS TCP KEEPALIVE defaults limits listener inheritance timer probes expiry");
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
    keepalive_case(&heap);
    mm_test_finish();mm_test_context=0;
    assert(!kernel_mm_release(&mm) && !physical_page_release(&pages,kernel.root_address));
    struct kernel_heap_statistics stats;kernel_heap_get_statistics(&heap,&stats);
    assert(!stats.live_allocations && !stats.current_pages && !physical_page_allocator_audit(&pages) && physical_page_available(&pages)==baseline);
    puts("PASS TCP KEEPALIVE owners returned");return 0;
}
