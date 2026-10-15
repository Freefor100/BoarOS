#define _POSIX_C_SOURCE 200809L
#include <kernel/mm_backend.h>
#include <kernel/memory_object.h>
#include <kernel/uaccess.h>
#include <kernel/heap.h>
#include <kernel/cpu.h>
#include <kernel/sync.h>
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern _Thread_local uint64_t mm_test_context;
void mm_test_attach(unsigned);
static unsigned char pool[16*1024*1024] __attribute__((aligned(16384)));
static struct physical_page_allocator pages;
static struct kernel_heap heap;
static struct kernel_mm parent, child, views[5];
static uint64_t roots[5];
static unsigned capture, entered[5], resume;
static unsigned delayed, fence_entered, fence_resume;
static uint64_t target;
static enum kernel_uaccess_status results[5];
#define SHARED UINT64_C(0x40000)
#define PRIVATE UINT64_C(0x60000)
#define RW (KERNEL_MM_READ|KERNEL_MM_WRITE)
void kernel_console_putc(char c) { fputc(c,stderr); }
void mm_test_unavailable(void) { fputs("excluded business reached\n",stderr); abort(); }
static void *access(uint64_t p) { return (void *)(uintptr_t)p; }
static int address_of(const void *p,uint64_t *out) { *out=(uintptr_t)p; return 1; }
void mm_test_fence(uint64_t v,int instructions)
{
    (void)instructions; __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if(v==UINT64_MAX && kernel_cpu_current()->hardware_id==1 && delayed) {
        __atomic_store_n(&fence_entered,1,__ATOMIC_RELEASE);
        while(!__atomic_load_n(&fence_resume,__ATOMIC_ACQUIRE)) sched_yield();
    }
}
enum kernel_memory_object_status __real_kernel_memory_object_prepare_page(struct kernel_memory_object *,uint64_t,uint64_t *,int *);
enum kernel_memory_object_status __wrap_kernel_memory_object_prepare_page(struct kernel_memory_object *o,uint64_t i,uint64_t *p,int *made)
{
    enum kernel_memory_object_status status=__real_kernel_memory_object_prepare_page(o,i,p,made);
    unsigned id=kernel_cpu_current()->hardware_id;
    if(status==KERNEL_MEMORY_OBJECT_OK && id<5 &&
        (__atomic_fetch_and(&capture,~(1U<<id),__ATOMIC_ACQ_REL)&(1U<<id))) {
        assert(!kernel_cpu_current()->raw_locks && !kernel_io_context_current()->locks);
        /* 尚未提交PTE，实际对象和返回页引用覆盖这次暂停。 */
        if (*made) ((unsigned char *)access(*p))[0]=0x7e;
        __atomic_store_n(&entered[id],1,__ATOMIC_RELEASE);
        while(!(__atomic_load_n(&resume,__ATOMIC_ACQUIRE)&(1U<<id))) sched_yield();
    }
    return status;
}
static void write_byte(struct kernel_mm *mm,uint64_t a,unsigned char v)
{ size_t done; assert(!kernel_copy_to_user(mm,a,&v,1,&done) && done==1); }
static unsigned char read_byte(struct kernel_mm *mm,uint64_t a)
{ unsigned char v; size_t done; assert(!kernel_copy_from_user(mm,&v,a,1,&done) && done==1); return v; }
static void map(struct kernel_mm *mm,uint64_t a,uint64_t length,uint32_t permissions,int shared)
{ uint64_t out; assert(!kernel_mm_mmap_anonymous(mm,a,length,permissions,KERNEL_MM_MAP_FIXED|(shared?KERNEL_MM_MAP_SHARED:0),&out) && out==a); }
static void *fault(void *arg)
{
    unsigned id=(unsigned)(uintptr_t)arg; mm_test_attach(id); mm_test_context=roots[id];
    unsigned char v=(unsigned char)(0x90+id); size_t done;
    results[id]=kernel_copy_to_user(&views[id],target+id,&v,1,&done);
    assert(done==(results[id]==KERNEL_UACCESS_STATUS_OK?1U:0U));
    return 0;
}
static void prepared(unsigned id)
{ while(!__atomic_load_n(&entered[id],__ATOMIC_ACQUIRE)) sched_yield(); }
static void first_fault(unsigned actors)
{
    target=SHARED+(actors==2?1:2)*BOAROS_PAGE_SIZE;
    memset(entered,0,sizeof(entered)); resume=0; capture=(1U<<(actors+1))-2;
    uint64_t charged=pages.shared_anon_pages;
    pthread_t workers[4];
    for(unsigned id=1;id<=actors;id++) assert(!pthread_create(&workers[id-1],0,fault,(void *)(uintptr_t)id));
    for(unsigned id=1;id<=actors;id++) prepared(id);
    assert(pages.shared_anon_pages==charged+1);
    __atomic_store_n(&resume,UINT32_MAX,__ATOMIC_RELEASE);
    for(unsigned id=1;id<=actors;id++) assert(!pthread_join(workers[id-1],0) && results[id]==KERNEL_UACCESS_STATUS_OK);
    struct kernel_mm_mapping a,b;
    assert(!kernel_mm_lookup(&parent,target,&a) && !kernel_mm_lookup(&child,target,&b));
    assert(a.physical_address==b.physical_address && (a.permissions&RW)==RW && (b.permissions&RW)==RW);
    for(unsigned id=1;id<=actors;id++) assert(read_byte(&parent,target+id)==0x90+id);
    assert(pages.shared_anon_pages==charged+1);
}
static void stale(int replace)
{
    target=UINT64_C(0x60000000);
    map(&parent,target,BOAROS_PAGE_SIZE,RW,1);
    uint64_t charged=pages.shared_anon_pages, before=physical_page_available(&pages);
    memset(entered,0,sizeof(entered)); resume=0; capture=2;
    pthread_t worker; assert(!pthread_create(&worker,0,fault,(void *)(uintptr_t)1)); prepared(1);
    assert(pages.shared_anon_pages==charged+1);
    assert(!kernel_mm_munmap(&parent,target,BOAROS_PAGE_SIZE));
    if(replace) map(&parent,target,BOAROS_PAGE_SIZE,RW,1);
    __atomic_store_n(&resume,UINT32_MAX,__ATOMIC_RELEASE);
    assert(!pthread_join(worker,0));
    if(replace) {
        assert(results[1]==KERNEL_UACCESS_STATUS_OK && read_byte(&parent,target)==0);
        assert(read_byte(&parent,target+1)==0x91);
        assert(!kernel_mm_munmap(&parent,target,BOAROS_PAGE_SIZE));
    } else assert(results[1]==KERNEL_UACCESS_STATUS_FAULT);
    assert(pages.shared_anon_pages==charged && physical_page_available(&pages)>=before);
}
static void oom(void)
{
    target=UINT64_C(0x80000000); map(&parent,target,BOAROS_PAGE_SIZE,RW,1);
    uint64_t charged=pages.shared_anon_pages;
    uint64_t pressure[4096]; unsigned n=0;
    while(n<4096 && physical_page_available(&pages)>2) assert(!physical_page_allocate(&pages,&pressure[n++]));
    unsigned char value=1; size_t done;
    assert(kernel_copy_to_user(&parent,target,&value,1,&done)==KERNEL_UACCESS_STATUS_FAULT && !done);
    assert(physical_page_available(&pages)==2 && pages.shared_anon_pages==charged);
    for(unsigned i=0;i<n;i++) physical_page_release(&pages,pressure[i]);
    write_byte(&parent,target,7); assert(read_byte(&parent,target)==7);
    assert(!kernel_mm_munmap(&parent,target,BOAROS_PAGE_SIZE));
    assert(pages.shared_anon_pages==charged);
}
static void adoption_rollback(int all_losers)
{
    target=UINT64_C(0x60000000);
    map(&parent,target,2*BOAROS_PAGE_SIZE,RW,1);
    struct kernel_mm other={0}; uint64_t other_root;
    assert(!kernel_mm_fork(&other,&parent) && !kernel_mm_context(&other,&other_root));
    uint64_t charged=pages.shared_anon_pages;
    memset(entered,0,sizeof(entered)); resume=0; capture=2;
    pthread_t a,b; assert(!pthread_create(&a,0,fault,(void *)(uintptr_t)1)); prepared(1);
    if(all_losers) {
        struct kernel_mm saved=views[2]; uint64_t saved_root=roots[2];
        views[2]=other; roots[2]=other_root; capture=4;
        assert(!pthread_create(&b,0,fault,(void *)(uintptr_t)2)); prepared(2);
        mm_test_context=other_root; assert(!kernel_mm_munmap(&other,target,BOAROS_PAGE_SIZE));
        mm_test_context=roots[1]; assert(!kernel_mm_munmap(&parent,target,BOAROS_PAGE_SIZE));
        /* creator先结束；第二个准备者仍持实际页引用，最后才回滚。 */
        __atomic_store_n(&resume,2,__ATOMIC_RELEASE); assert(!pthread_join(a,0));
        assert(results[1]==KERNEL_UACCESS_STATUS_FAULT && pages.shared_anon_pages==charged+1);
        __atomic_store_n(&resume,6,__ATOMIC_RELEASE); assert(!pthread_join(b,0));
        views[2]=saved; roots[2]=saved_root;
        assert(results[2]==KERNEL_UACCESS_STATUS_FAULT && pages.shared_anon_pages==charged);
    } else {
        mm_test_context=other_root; write_byte(&other,target,0x5b);
        assert(!kernel_mm_munmap(&other,target,BOAROS_PAGE_SIZE));
        mm_test_context=roots[1];
        /* 无关VMA修改迫使creator重查，但冷映射仍拥有已写内容。 */
        map(&parent,target+4*BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE,RW,0);
        __atomic_store_n(&resume,2,__ATOMIC_RELEASE); assert(!pthread_join(a,0));
        assert(results[1]==KERNEL_UACCESS_STATUS_OK && read_byte(&parent,target)==0x5b);
        assert(read_byte(&parent,target+1)==0x91);
        assert(!kernel_mm_munmap(&parent,target+4*BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE));
    }
    assert(!kernel_mm_munmap(&parent,target,2*BOAROS_PAGE_SIZE));
    mm_test_context=0; assert(!kernel_mm_release(&other)); mm_test_context=roots[1];
    assert(pages.shared_anon_pages==charged);
}
static void *unmap(void *unused)
{ (void)unused; mm_test_attach(1); mm_test_context=roots[1]; assert(!kernel_mm_munmap(&views[1],SHARED,3*BOAROS_PAGE_SIZE)); return 0; }
static void retire(void)
{
    mm_test_context=roots[2];
    assert(!kernel_mm_munmap(&child,SHARED,3*BOAROS_PAGE_SIZE));
    mm_test_context=roots[1];
    struct kernel_mm_user_pin pin={0}; assert(!kernel_mm_pin_user_page(&parent,SHARED,KERNEL_MM_READ,&pin));
    uint64_t before=physical_page_available(&pages);
    delayed=1; fence_entered=fence_resume=0;
    pthread_t worker; assert(!pthread_create(&worker,0,unmap,0));
    while(!__atomic_load_n(&fence_entered,__ATOMIC_ACQUIRE)) sched_yield();
    assert(physical_page_available(&pages)==before && ((unsigned char *)pin.page)[0]==11);
    struct kernel_mm_mapping mapping; assert(kernel_mm_lookup(&parent,SHARED,&mapping)==KERNEL_MM_STATUS_NOT_MAPPED);
    __atomic_store_n(&fence_resume,1,__ATOMIC_RELEASE); assert(!pthread_join(worker,0)); delayed=0;
    uint32_t refs; assert(!physical_page_reference_count(&pages,pin.physical_address,&refs) && refs==1);
    assert(!pages.shared_anon_pages && ((unsigned char *)pin.page)[0]==11);
    kernel_mm_unpin_user_page(&pin);
}
int main(void)
{
    mm_test_attach(0);
    struct boot_memory_layout layout={0}; layout.usable_count=1; layout.usable[0].base=(uintptr_t)pool; layout.usable[0].size=sizeof(pool);
    assert(!physical_page_allocator_init(&pages,&layout) && !physical_page_allocator_bind_access(&pages,access) && !physical_page_allocator_finalize(&pages));
    assert(!kernel_heap_init(&heap,&pages,address_of));
    struct arch_mmu_page_table kernel={0}; struct arch_mmu_user_space space={0};
    assert(!riscv_sv39_page_table_init(&kernel,&pages)); kernel.state=ARCH_MMU_STATE_ACTIVE;
    uint64_t baseline=physical_page_available(&pages);
    assert(!arch_mmu_user_space_init(&space,&pages,&kernel) && !kernel_mm_create(&parent,&space) && !kernel_mm_vma_enable(&parent,&heap));
    assert(!kernel_mm_brk_initialize(&parent,0x10000,0x20000) && !kernel_mm_context(&parent,&roots[1])); mm_test_context=roots[1];
    map(&parent,SHARED,3*BOAROS_PAGE_SIZE,RW,1); map(&parent,PRIVATE,BOAROS_PAGE_SIZE,RW,0);
    write_byte(&parent,SHARED,11); write_byte(&parent,PRIVATE,31);
    assert(!kernel_mm_enable_concurrency(&parent));
    assert(!kernel_mm_fork(&child,&parent));
    struct kernel_mm_mapping a,b;
    assert(!kernel_mm_lookup(&parent,SHARED,&a) && !kernel_mm_lookup(&child,SHARED,&b));
    assert(a.physical_address==b.physical_address && (a.permissions&RW)==RW && (b.permissions&RW)==RW);
    assert(!kernel_mm_lookup(&parent,PRIVATE,&a) && !kernel_mm_lookup(&child,PRIVATE,&b));
    assert(a.physical_address==b.physical_address && !(a.permissions&KERNEL_MM_WRITE));
    for(unsigned i=1;i<=4;i++) {
        assert(!kernel_mm_acquire(&views[i],i==2||i==4?&child:&parent));
        assert(!kernel_mm_context(&views[i],&roots[i]));
    }
    adoption_rollback(1); adoption_rollback(0); first_fault(2); first_fault(4);
    assert(!kernel_mm_mprotect(&parent,SHARED,BOAROS_PAGE_SIZE,KERNEL_MM_READ));
    mm_test_context=roots[2]; write_byte(&child,SHARED+1,44); mm_test_context=roots[1]; assert(read_byte(&parent,SHARED+1)==44);
    assert(!kernel_mm_mprotect(&parent,SHARED,BOAROS_PAGE_SIZE,RW));
    write_byte(&parent,PRIVATE,77); mm_test_context=roots[2]; assert(read_byte(&child,PRIVATE)==31); mm_test_context=roots[1];
    stale(0); stale(1); oom(); retire();
    mm_test_context=0; /* 末次销毁前切回可信内核地址空间。 */
    for(unsigned i=1;i<=4;i++) assert(!kernel_mm_release(&views[i]));
    assert(!kernel_mm_release(&child) && !kernel_mm_release(&parent));
    struct kernel_heap_statistics stats; kernel_heap_get_statistics(&heap,&stats);
    assert(!stats.live_allocations && !stats.current_pages && physical_page_available(&pages)==baseline && !pages.shared_anon_pages);
    assert(!physical_page_allocator_audit(&pages));
    puts("shared MM: cross-root publication, COW classification, stale/OOM, delayed retirement and final owners passed");
}
