#include <kernel/mm_backend.h>
#include <kernel/rlimit.h>
#include <kernel/heap.h>
#include <kernel/cpu.h>
#include <kernel/sync.h>
#include <kernel/vma.h>
#include <kernel/errno.h>
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
extern _Thread_local uint64_t mm_test_context;
void mm_test_attach(unsigned);
unsigned mm_test_parked(unsigned);
static unsigned char pool[64*1024*1024] __attribute__((aligned(16384)));
static struct physical_page_allocator pages;
static struct kernel_heap heap;
static struct kernel_mm mm;
static uint64_t stack_limit=UINT64_MAX, memlock_limit=UINT64_MAX;
uint64_t kernel_task_current_memlock_limit(void) { return memlock_limit; }
uint64_t kernel_task_current_stack_limit(void) { return stack_limit; }
static struct kernel_memory_limits limits={UINT64_MAX,UINT64_MAX,UINT64_MAX};
static unsigned limit_reads;
void kernel_task_current_memory_limits(struct kernel_memory_limits *out) { limit_reads++; *out=limits; }
void kernel_console_putc(char c) { fputc(c,stderr); }
void mm_test_unavailable(void) { abort(); }
static _Thread_local int pause_reclaim_fence;
static atomic_int fence_entered,fence_continue,release_done;
void mm_test_fence(uint64_t v,int instructions)
{
    (void)v;(void)instructions;
    if (pause_reclaim_fence) {
        atomic_store(&fence_entered,1);
        while (!atomic_load(&fence_continue)) sched_yield();
    }
}
static void *access(uint64_t p) { return (void *)(uintptr_t)p; }
static int address_of(const void *p,uint64_t *out) { *out=(uintptr_t)p; return 1; }
#define P BOAROS_PAGE_SIZE
#define RW (KERNEL_MM_READ|KERNEL_MM_WRITE)
#define BASE UINT64_C(0x400000)
static unsigned fail_resize, fail_page, fail_page_after;
static unsigned fail_heap_zero,fail_heap_zero_after;
enum kernel_heap_status __real_kernel_heap_allocate_zeroed(struct kernel_heap *,size_t,size_t,void **);
enum kernel_heap_status __wrap_kernel_heap_allocate_zeroed(struct kernel_heap *h,size_t n,size_t s,void **out)
{ return fail_heap_zero || (fail_heap_zero_after && !--fail_heap_zero_after) ?
    KERNEL_HEAP_STATUS_EMPTY : __real_kernel_heap_allocate_zeroed(h,n,s,out); }
enum physical_page_status __real_physical_page_allocate(struct physical_page_allocator *,uint64_t *);
enum physical_page_status __wrap_physical_page_allocate(struct physical_page_allocator *a,uint64_t *out)
{ return fail_page || (fail_page_after && !--fail_page_after) ? PHYSICAL_PAGE_STATUS_EMPTY : __real_physical_page_allocate(a,out); }
enum kernel_heap_status __real_kernel_heap_resize(struct kernel_heap *,void *,size_t,void **);
enum kernel_heap_status __wrap_kernel_heap_resize(struct kernel_heap *h,void *old,size_t size,void **out)
{ return fail_resize ? KERNEL_HEAP_STATUS_EMPTY : __real_kernel_heap_resize(h,old,size,out); }
static unsigned checks,failures;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"FAIL %s:%u: %s\n",__FILE__,__LINE__,#x); failures++; } } while(0)
static enum kernel_mm_status map(uint64_t at,uint64_t len,uint32_t prot,uint32_t flags)
{ uint64_t out=0; return kernel_mm_mmap_anonymous(&mm,at,len,prot,KERNEL_MM_MAP_FIXED|flags,&out); }
static uint64_t test_detached;
static uint64_t test_reclaim(uint64_t target)
{ return kernel_mm_reclaim_lazy(&pages,target,&test_detached); }
static struct kernel_mm retiring_mm;
static uint64_t reclaimed_count;
static enum kernel_mm_status release_result;
static void *reclaim_actor(void *unused)
{
    (void)unused;mm_test_attach(1);pause_reclaim_fence=1;
    reclaimed_count=test_reclaim(1);
    return NULL;
}
static void *release_actor(void *unused)
{
    (void)unused;mm_test_attach(2);
    release_result=kernel_mm_release(&retiring_mm);
    atomic_store(&release_done,1);return NULL;
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
    assert(!arch_mmu_user_space_init(&space,&pages,&kernel) && !kernel_mm_create(&mm,&space) && !kernel_mm_vma_enable(&mm,&heap));
    assert(!kernel_mm_brk_initialize(&mm,0x10000,0x20000) && !kernel_mm_context(&mm,&mm_test_context));
    struct kernel_mm held = {0};
    struct kernel_raw_lock identity;
    kernel_raw_lock_init(&identity, 40);
    puts("MM_ACQUIRE stable owner under task identity protection");
    {
        KERNEL_RAW_SCOPE(guard, &identity);
        CHECK(!kernel_mm_acquire(&held, &mm));
    }
    CHECK(!kernel_mm_release(&held));
    CHECK(!kernel_mm_madvise(&mm,BASE,0,9));
    assert(!map(BASE,3*P,RW,KERNEL_MM_MAP_SHARED));
    struct kernel_mm_mapping removed;
    CHECK(!kernel_mm_resolve_user_fault(&mm,BASE+P,KERNEL_MM_WRITE));
    CHECK(!kernel_mm_lookup(&mm,BASE+P,&removed));
    *(unsigned char *)(uintptr_t)removed.physical_address=0x62;
    struct kernel_mm removal_child={0};CHECK(!kernel_mm_fork(&removal_child,&mm));
    CHECK(!kernel_mm_madvise(&mm,BASE+P,1,9));
    CHECK(kernel_mm_lookup(&removal_child,BASE+P,&removed)==KERNEL_MM_STATUS_NOT_MAPPED);
    CHECK(!kernel_mm_resolve_user_fault(&removal_child,BASE+P,KERNEL_MM_READ));
    CHECK(!kernel_mm_lookup(&removal_child,BASE+P,&removed)&&!*(unsigned char *)(uintptr_t)removed.physical_address);
    CHECK(!kernel_mm_release(&removal_child)&&!kernel_mm_munmap(&mm,BASE,3*P));
    /* Lazy pages remain readable, dirty writes cancel eligibility, and the
     * reclaimer observes pins and mlock before detaching real PTE owners. */
    assert(!map(BASE,3*P,RW,0));
    struct kernel_mm_mapping lazy_mapping;
    for (unsigned i=0;i<3;i++) {
        assert(!kernel_mm_resolve_user_fault(&mm,BASE+i*P,KERNEL_MM_WRITE));
        assert(!kernel_mm_lookup(&mm,BASE+i*P,&lazy_mapping));
        *(unsigned char *)(uintptr_t)lazy_mapping.physical_address=(unsigned char)(30+i);
    }
    CHECK(!kernel_mm_madvise(&mm,BASE,3*P,8));
    struct kernel_mm_concurrency_state lazy_state;
    kernel_mm_get_concurrency_state(&mm,&lazy_state);
    CHECK(lazy_state.lazy_pages==3&&lazy_state.lazy_index_bytes&&lazy_state.lazy_registered);
    CHECK(!kernel_mm_lookup(&mm,BASE,&lazy_mapping)&&*(unsigned char *)(uintptr_t)lazy_mapping.physical_address==30);
    struct kernel_mm_user_pin lazy_pin={0};
    CHECK(!kernel_mm_pin_user_page(&mm,BASE+P,KERNEL_MM_READ,&lazy_pin));
    CHECK(!test_reclaim(3));
    kernel_mm_unpin_user_page(&lazy_pin);
    CHECK(!kernel_mm_resolve_user_fault(&mm,BASE,KERNEL_MM_WRITE));
    CHECK(!kernel_mm_lookup(&mm,BASE,&lazy_mapping));
    *(unsigned char *)(uintptr_t)lazy_mapping.physical_address=99;
    CHECK(!kernel_mm_mlock(&mm,BASE+P,P,1));
    CHECK(test_reclaim(3)==1);
    CHECK(kernel_mm_lookup(&mm,BASE+2*P,&lazy_mapping)==KERNEL_MM_STATUS_NOT_MAPPED);
    CHECK(!kernel_mm_lookup(&mm,BASE,&lazy_mapping)&&*(unsigned char *)(uintptr_t)lazy_mapping.physical_address==99);
    CHECK(!kernel_mm_munlock(&mm,BASE+P,P));
    CHECK(test_reclaim(3)==1);
    CHECK(kernel_mm_lookup(&mm,BASE+P,&lazy_mapping)==KERNEL_MM_STATUS_NOT_MAPPED);
    kernel_mm_get_concurrency_state(&mm,&lazy_state);
    CHECK(!lazy_state.lazy_pages&&!lazy_state.lazy_index_bytes&&!lazy_state.lazy_registered);
    CHECK(!kernel_mm_resolve_user_fault(&mm,BASE+P,KERNEL_MM_READ));
    CHECK(!kernel_mm_lookup(&mm,BASE+P,&lazy_mapping)&&!*(unsigned char *)(uintptr_t)lazy_mapping.physical_address);
    CHECK(!kernel_mm_munmap(&mm,BASE,3*P));
    assert(!map(BASE,P,RW,0)&&!kernel_mm_resolve_user_fault(&mm,BASE,KERNEL_MM_WRITE));
    fail_heap_zero=1;CHECK(kernel_mm_madvise(&mm,BASE,P,8)==-KERNEL_ENOMEM);fail_heap_zero=0;
    CHECK(!kernel_mm_lookup(&mm,BASE,&lazy_mapping)&&(lazy_mapping.permissions & KERNEL_MM_WRITE));
    CHECK(!test_reclaim(1)&&!kernel_mm_munmap(&mm,BASE,P));
    /* The first index allocation may succeed while the candidate fails. */
    assert(!map(BASE,P,RW,0)&&!kernel_mm_resolve_user_fault(&mm,BASE,KERNEL_MM_WRITE));
    fail_heap_zero_after=2;
    CHECK(kernel_mm_madvise(&mm,BASE,P,8)==-KERNEL_ENOMEM);
    kernel_mm_get_concurrency_state(&mm,&lazy_state);
    CHECK(!lazy_state.lazy_pages&&!lazy_state.lazy_index_bytes&&!lazy_state.lazy_registered);
    CHECK(!kernel_mm_lookup(&mm,BASE,&lazy_mapping)&&(lazy_mapping.permissions&KERNEL_MM_WRITE));
    CHECK(!kernel_mm_munmap(&mm,BASE,P));
    /* Failure while growing an existing index retains the accepted prefix. */
    assert(!map(BASE,17*P,RW,0));
    for (unsigned i=0;i<17;i++) assert(!kernel_mm_resolve_user_fault(&mm,BASE+i*P,KERNEL_MM_WRITE));
    fail_heap_zero_after=18;
    CHECK(kernel_mm_madvise(&mm,BASE,17*P,8)==-KERNEL_ENOMEM);
    kernel_mm_get_concurrency_state(&mm,&lazy_state);
    CHECK(lazy_state.lazy_pages==16&&lazy_state.lazy_registered);
    CHECK(!kernel_mm_lookup(&mm,BASE+16*P,&lazy_mapping)&&(lazy_mapping.permissions&KERNEL_MM_WRITE));
    CHECK(!kernel_mm_madvise(&mm,BASE,17*P,8));
    CHECK(test_reclaim(17)==16&&test_detached==16);
    CHECK(test_reclaim(1)==1&&test_detached==1);
    CHECK(!kernel_mm_munmap(&mm,BASE,17*P));
    /* Kernel writes use user-page pins; restoring protection and writes after
     * fork must not leave the new bytes eligible for later reclaim. */
    assert(!map(BASE,P,RW,0)&&!kernel_mm_resolve_user_fault(&mm,BASE,KERNEL_MM_WRITE));
    CHECK(!kernel_mm_madvise(&mm,BASE,P,8));
    CHECK(!kernel_mm_mprotect(&mm,BASE,P,0)&&!kernel_mm_mprotect(&mm,BASE,P,RW));
    CHECK(!kernel_mm_resolve_user_fault(&mm,BASE,KERNEL_MM_WRITE));
    CHECK(!kernel_mm_pin_user_page(&mm,BASE,KERNEL_MM_WRITE,&lazy_pin));
    *(unsigned char *)lazy_pin.page=0x79;kernel_mm_unpin_user_page(&lazy_pin);
    CHECK(!test_reclaim(1));
    CHECK(!kernel_mm_lookup(&mm,BASE,&lazy_mapping)&&*(unsigned char *)(uintptr_t)lazy_mapping.physical_address==0x79);
    CHECK(!kernel_mm_madvise(&mm,BASE,P,8));
    struct kernel_mm lazy_child={0};CHECK(!kernel_mm_fork(&lazy_child,&mm));
    CHECK(!kernel_mm_resolve_user_fault(&lazy_child,BASE,KERNEL_MM_WRITE));
    CHECK(!kernel_mm_lookup(&lazy_child,BASE,&lazy_mapping));
    *(unsigned char *)(uintptr_t)lazy_mapping.physical_address=0x41;
    CHECK(!kernel_mm_madvise(&lazy_child,BASE,P,8));
    CHECK(test_reclaim(2)==1&&test_detached==1);
    CHECK(test_reclaim(2)==1&&test_detached==1);
    CHECK(kernel_mm_lookup(&mm,BASE,&lazy_mapping)==KERNEL_MM_STATUS_NOT_MAPPED);
    CHECK(kernel_mm_lookup(&lazy_child,BASE,&lazy_mapping)==KERNEL_MM_STATUS_NOT_MAPPED);
    CHECK(!kernel_mm_release(&lazy_child)&&!kernel_mm_munmap(&mm,BASE,P));
    /* FREE before fork must remain reclaimable with both mappings alive. */
    assert(!map(BASE,P,RW,0)&&!kernel_mm_resolve_user_fault(&mm,BASE,KERNEL_MM_WRITE));
    CHECK(!kernel_mm_madvise(&mm,BASE,P,8));
    struct kernel_mm sharing_child={0};CHECK(!kernel_mm_fork(&sharing_child,&mm));
    uint64_t before_shared_reclaim=physical_page_available(&pages);
    CHECK(!test_reclaim(2)&&test_detached==1);
    CHECK(test_reclaim(2)==1&&test_detached==1);
    CHECK(kernel_mm_lookup(&mm,BASE,&lazy_mapping)==KERNEL_MM_STATUS_NOT_MAPPED);
    CHECK(kernel_mm_lookup(&sharing_child,BASE,&lazy_mapping)==KERNEL_MM_STATUS_NOT_MAPPED);
    CHECK(physical_page_available(&pages)>before_shared_reclaim);
    CHECK(!kernel_mm_release(&sharing_child)&&!kernel_mm_munmap(&mm,BASE,P));
    /* One-page pressure must not detach a whole shared range while holding
     * a single MM's write qualification; successive calls rotate owners. */
    enum { SHARED_BATCH_PAGES=64 };
    assert(!map(BASE,SHARED_BATCH_PAGES*P,RW,0));
    for (unsigned i=0;i<SHARED_BATCH_PAGES;i++)
        assert(!kernel_mm_resolve_user_fault(&mm,BASE+i*P,KERNEL_MM_WRITE));
    CHECK(!kernel_mm_madvise(&mm,BASE,SHARED_BATCH_PAGES*P,8));
    sharing_child=(struct kernel_mm){0};CHECK(!kernel_mm_fork(&sharing_child,&mm));
    before_shared_reclaim=physical_page_available(&pages);
    (void)test_reclaim(1);
    CHECK(test_detached>0&&test_detached<=16);
    struct kernel_mm_concurrency_state child_state;
    kernel_mm_get_concurrency_state(&mm,&lazy_state);
    kernel_mm_get_concurrency_state(&sharing_child,&child_state);
    CHECK(lazy_state.lazy_pages>=SHARED_BATCH_PAGES-16&&child_state.lazy_pages>=SHARED_BATCH_PAGES-16);
    CHECK(lazy_state.lazy_pages+child_state.lazy_pages==2*SHARED_BATCH_PAGES-test_detached);
    for (unsigned round=0;round<16 && (lazy_state.lazy_pages||child_state.lazy_pages);round++) {
        (void)test_reclaim(SHARED_BATCH_PAGES);
        CHECK(test_detached>0&&test_detached<=16);
        kernel_mm_get_concurrency_state(&mm,&lazy_state);
        kernel_mm_get_concurrency_state(&sharing_child,&child_state);
    }
    CHECK(!lazy_state.lazy_pages&&!child_state.lazy_pages);
    CHECK(physical_page_available(&pages)>before_shared_reclaim);
    CHECK(!kernel_mm_release(&sharing_child)&&!kernel_mm_munmap(&mm,BASE,SHARED_BATCH_PAGES*P));
    /* PROT_NONE retains an owned physical page; FREE may reclaim it without
     * making the mapping accessible or losing its VMA protection. */
    assert(!map(BASE,P,RW,0)&&!kernel_mm_resolve_user_fault(&mm,BASE,KERNEL_MM_WRITE));
    CHECK(!kernel_mm_mprotect(&mm,BASE,P,0)&&!kernel_mm_madvise(&mm,BASE,P,8));
    CHECK(test_reclaim(1)==1&&test_detached==1);
    CHECK(!kernel_mm_mprotect(&mm,BASE,P,RW)&&!kernel_mm_resolve_user_fault(&mm,BASE,KERNEL_MM_READ));
    CHECK(!kernel_mm_lookup(&mm,BASE,&lazy_mapping)&&!*(unsigned char *)(uintptr_t)lazy_mapping.physical_address);
    CHECK(!kernel_mm_munmap(&mm,BASE,P));
    /* FREE after fork affects just the caller; a sibling mapping stays intact. */
    assert(!map(BASE,P,RW,0)&&!kernel_mm_resolve_user_fault(&mm,BASE,KERNEL_MM_WRITE));
    assert(!kernel_mm_lookup(&mm,BASE,&lazy_mapping));
    *(unsigned char *)(uintptr_t)lazy_mapping.physical_address=0x37;
    sharing_child=(struct kernel_mm){0};CHECK(!kernel_mm_fork(&sharing_child,&mm));
    CHECK(!kernel_mm_madvise(&mm,BASE,P,8));
    CHECK(!test_reclaim(1)&&test_detached==1);
    CHECK(kernel_mm_lookup(&mm,BASE,&lazy_mapping)==KERNEL_MM_STATUS_NOT_MAPPED);
    CHECK(!kernel_mm_lookup(&sharing_child,BASE,&lazy_mapping)&&*(unsigned char *)(uintptr_t)lazy_mapping.physical_address==0x37);
    CHECK(!kernel_mm_release(&sharing_child)&&!kernel_mm_munmap(&mm,BASE,P));
    /* A moved PTE carries the reclaim candidate to its new virtual address. */
    assert(!map(BASE,P,RW,0)&&!kernel_mm_resolve_user_fault(&mm,BASE,KERNEL_MM_WRITE));
    CHECK(!kernel_mm_madvise(&mm,BASE,P,8));
    uint64_t lazy_moved=0;
    CHECK(!kernel_mm_mremap(&mm,BASE,P,P,3,BASE+4*P,&lazy_moved)&&lazy_moved==BASE+4*P);
    CHECK(test_reclaim(1)==1);
    CHECK(kernel_mm_lookup(&mm,lazy_moved,&lazy_mapping)==KERNEL_MM_STATUS_NOT_MAPPED);
    CHECK(!kernel_mm_munmap(&mm,lazy_moved,P));
    /* The last MM owner waits until a scanner has completed its TLB
     * confirmation and stopped touching the registered record. */
    assert(!map(BASE,P,RW,0)&&!kernel_mm_resolve_user_fault(&mm,BASE,KERNEL_MM_WRITE));
    assert(!kernel_mm_fork(&retiring_mm,&mm));
    assert(!kernel_mm_resolve_user_fault(&retiring_mm,BASE,KERNEL_MM_WRITE));
    assert(!kernel_mm_madvise(&retiring_mm,BASE,P,8));
    assert(!kernel_mm_lookup(&retiring_mm,BASE,&lazy_mapping));
    uint64_t retiring_physical=lazy_mapping.physical_address;
    pthread_t reclaim_thread,release_thread;
    assert(!pthread_create(&reclaim_thread,NULL,reclaim_actor,NULL));
    while(!atomic_load(&fence_entered)) sched_yield();
    kernel_mm_get_concurrency_state(&retiring_mm,&lazy_state);
    CHECK(!lazy_state.lazy_pages&&!lazy_state.lazy_registered&&lazy_state.lazy_borrows==1);
    unsigned parked_before=mm_test_parked(2);
    assert(!pthread_create(&release_thread,NULL,release_actor,NULL));
    while(mm_test_parked(2)==parked_before&&!atomic_load(&release_done)) sched_yield();
    CHECK(mm_test_parked(2)>parked_before&&!atomic_load(&release_done));
    uint32_t retiring_refs=0;
    CHECK(!physical_page_reference_count(&pages,retiring_physical,&retiring_refs)&&retiring_refs);
    atomic_store(&fence_continue,1);
    assert(!pthread_join(reclaim_thread,NULL)&&!pthread_join(release_thread,NULL));
    CHECK(reclaimed_count==1&&release_result==KERNEL_MM_STATUS_OK&&atomic_load(&release_done));
    CHECK(!kernel_mm_munmap(&mm,BASE,P));
    /* A large advised range exercises index growth, lookup, cancellation and
     * retirement using actual resident pages, rather than a fixed test key. */
    enum { LAZY_SCALE_PAGES=8192 };
    assert(!map(BASE,(uint64_t)LAZY_SCALE_PAGES*P,RW,0));
    for (unsigned i=0;i<LAZY_SCALE_PAGES;i++) {
        assert(!kernel_mm_resolve_user_fault(&mm,BASE+(uint64_t)i*P,KERNEL_MM_WRITE));
        assert(!kernel_mm_lookup(&mm,BASE+(uint64_t)i*P,&lazy_mapping));
        *(unsigned char *)(uintptr_t)lazy_mapping.physical_address=0x31;
    }
    CHECK(!kernel_mm_madvise(&mm,BASE,(uint64_t)LAZY_SCALE_PAGES*P,8));
    kernel_mm_get_concurrency_state(&mm,&lazy_state);
    CHECK(lazy_state.lazy_pages==LAZY_SCALE_PAGES&&lazy_state.lazy_registered);
    for (unsigned i=0;i<LAZY_SCALE_PAGES;i+=2)
        assert(!kernel_mm_resolve_user_fault(&mm,BASE+(uint64_t)i*P,KERNEL_MM_WRITE));
    uint64_t scale_released=0;
    while (scale_released<LAZY_SCALE_PAGES/2) {
        uint64_t released=test_reclaim(LAZY_SCALE_PAGES/2-scale_released);
        assert(released&&test_detached<=16);
        scale_released+=released;
    }
    CHECK(scale_released==LAZY_SCALE_PAGES/2);
    CHECK(!kernel_mm_lookup(&mm,BASE,&lazy_mapping)&&*(unsigned char *)(uintptr_t)lazy_mapping.physical_address==0x31);
    CHECK(kernel_mm_lookup(&mm,BASE+P,&lazy_mapping)==KERNEL_MM_STATUS_NOT_MAPPED);
    kernel_mm_get_concurrency_state(&mm,&lazy_state);
    CHECK(!lazy_state.lazy_pages&&!lazy_state.lazy_index_bytes&&!lazy_state.lazy_registered);
    CHECK(!kernel_mm_munmap(&mm,BASE,(uint64_t)LAZY_SCALE_PAGES*P));
    /* Allocate all remaining pages: allocator pressure invokes the same
     * registered reclaimer, not an isolated test-only reclamation path. */
    assert(!map(BASE,P,RW,0)&&!kernel_mm_resolve_user_fault(&mm,BASE,KERNEL_MM_WRITE));
    CHECK(!kernel_mm_madvise(&mm,BASE,P,8));
    uint64_t available=physical_page_available(&pages);
    uint64_t *held_pages=calloc(available+1,sizeof(*held_pages));assert(held_pages);
    uint64_t held_count=0;
    for (;held_count<=available;held_count++)
        if (physical_page_allocate(&pages,&held_pages[held_count])) break;
    CHECK(held_count==available+1);
    CHECK(kernel_mm_lookup(&mm,BASE,&lazy_mapping)==KERNEL_MM_STATUS_NOT_MAPPED);
    for (uint64_t i=0;i<held_count;i++) CHECK(!physical_page_release(&pages,held_pages[i]));
    free(held_pages);
    CHECK(!kernel_mm_resolve_user_fault(&mm,BASE,KERNEL_MM_READ));
    CHECK(!kernel_mm_lookup(&mm,BASE,&lazy_mapping)&&!*(unsigned char *)(uintptr_t)lazy_mapping.physical_address);
    CHECK(!kernel_mm_munmap(&mm,BASE,P));
    /* Fork policy controls actual child leaves and refs, not just a VMA flag. */
    assert(!map(BASE,4*P,RW,0));
    struct kernel_mm_mapping fork_page, original_page;
    for(unsigned i=0;i<4;i++) {
        assert(!kernel_mm_resolve_user_fault(&mm,BASE+i*P,KERNEL_MM_WRITE));
        assert(!kernel_mm_lookup(&mm,BASE+i*P,&fork_page));
        *(unsigned char *)(uintptr_t)fork_page.physical_address=(unsigned char)(60+i);
    }
    CHECK(!kernel_mm_madvise(&mm,BASE+P,P,10));
    CHECK(!kernel_mm_madvise(&mm,BASE+2*P,P,18));
    /* Other flag operations preserve fork policy and its data. */
    CHECK(!kernel_mm_mlock(&mm,BASE,4*P,0));
    CHECK(!kernel_mm_munlockall(&mm));
    CHECK(!kernel_mm_lookup(&mm,BASE,&original_page));
    struct kernel_mm child={0};
    for(unsigned failure=1;failure<=3;failure++) {
        fail_page_after=failure;CHECK(kernel_mm_fork(&child,&mm)==KERNEL_MM_STATUS_NO_MEMORY);fail_page_after=0;
        CHECK(child.state==KERNEL_MM_EMPTY);
        CHECK(!kernel_mm_lookup(&mm,BASE,&fork_page)&&fork_page.permissions==original_page.permissions&&
              fork_page.physical_address==original_page.physical_address);
    }
    CHECK(!kernel_mm_fork(&child,&mm));
    struct kernel_vma inherited;
    CHECK(kernel_mm_vma_lookup(&child,BASE+P,&inherited)==KERNEL_MM_STATUS_NOT_MAPPED);
    CHECK(kernel_mm_lookup(&child,BASE+P,&fork_page)==KERNEL_MM_STATUS_NOT_MAPPED);
    CHECK(!kernel_mm_vma_lookup(&child,BASE+2*P,&inherited)&&
        (inherited.flags&KERNEL_VMA_WIPEONFORK)&&!(inherited.flags&KERNEL_VMA_LOCK_MASK));
    CHECK(kernel_mm_lookup(&child,BASE+2*P,&fork_page)==KERNEL_MM_STATUS_NOT_MAPPED);
    CHECK(!kernel_mm_resolve_user_fault(&child,BASE+2*P,KERNEL_MM_WRITE));
    CHECK(!kernel_mm_lookup(&child,BASE+2*P,&fork_page)&&!*(unsigned char *)(uintptr_t)fork_page.physical_address);
    CHECK(!kernel_mm_lookup(&mm,BASE+2*P,&fork_page)&&*(unsigned char *)(uintptr_t)fork_page.physical_address==62);
    CHECK(!kernel_mm_lookup(&child,BASE,&fork_page)&&fork_page.physical_address==original_page.physical_address);
    CHECK(!kernel_mm_release(&child));
    CHECK(!kernel_mm_munmap(&mm,BASE,4*P));
    /* Discard PTEs without changing VMA identity, protection or AS/DATA usage. */
    assert(!map(BASE,3*P,RW,0));
    CHECK(!kernel_mm_resolve_user_fault(&mm,BASE,KERNEL_MM_WRITE));
    CHECK(!kernel_mm_resolve_user_fault(&mm,BASE+2*P,KERNEL_MM_WRITE));
    struct kernel_mm_mapping discarded;
    assert(!kernel_mm_lookup(&mm,BASE,&discarded));
    *(unsigned char *)(uintptr_t)discarded.physical_address=0x63;
    struct kernel_mm_proc_memory before_advice,after_advice;
    CHECK(!kernel_mm_proc_memory_snapshot(&mm,&before_advice));
    CHECK(!kernel_mm_madvise(&mm,BASE,P,4));
    CHECK(kernel_mm_lookup(&mm,BASE,&discarded)==KERNEL_MM_STATUS_NOT_MAPPED);
    CHECK(!kernel_mm_proc_memory_snapshot(&mm,&after_advice)&&
        after_advice.virtual_bytes==before_advice.virtual_bytes&&
        after_advice.resident_pages+1==before_advice.resident_pages);
    CHECK(!kernel_mm_resolve_user_fault(&mm,BASE,KERNEL_MM_WRITE));
    assert(!kernel_mm_lookup(&mm,BASE,&discarded));
    CHECK(!*(unsigned char *)(uintptr_t)discarded.physical_address);
    CHECK(!kernel_mm_munmap(&mm,BASE+P,P));
    CHECK(kernel_mm_madvise(&mm,BASE,3*P,4)==-KERNEL_ENOMEM);
    CHECK(kernel_mm_lookup(&mm,BASE,&discarded)==KERNEL_MM_STATUS_NOT_MAPPED);
    CHECK(kernel_mm_lookup(&mm,BASE+2*P,&discarded)==KERNEL_MM_STATUS_NOT_MAPPED);
    CHECK(!kernel_mm_madvise(&mm,UINT64_MAX&~BOAROS_PAGE_MASK,0,4));
    CHECK(kernel_mm_madvise(&mm,BASE,0,99)==-KERNEL_EINVAL);
    CHECK(kernel_mm_madvise(&mm,BASE,1,3)==-KERNEL_EBADF);
    CHECK(!kernel_mm_munmap(&mm,BASE,3*P));
    assert(!map(BASE,P,RW,0));
    unsigned reads_before=limit_reads;
    CHECK(!kernel_mm_mprotect(&mm,BASE,P,KERNEL_MM_READ));
    CHECK(limit_reads==reads_before);
    CHECK(!kernel_mm_mprotect(&mm,BASE,P,KERNEL_MM_READ));
    CHECK(limit_reads==reads_before);
    CHECK(!kernel_mm_mprotect(&mm,BASE,P,RW));
    CHECK(limit_reads==reads_before+1);
    assert(!kernel_mm_munmap(&mm,BASE,P));
    assert(!map(BASE,P,KERNEL_MM_READ,KERNEL_MM_MAP_SHARED));
    reads_before=limit_reads;
    CHECK(!kernel_mm_mprotect(&mm,BASE,P,RW));
    CHECK(limit_reads==reads_before);
    assert(!kernel_mm_munmap(&mm,BASE,P));
    /* Linux compares admission with both old and new flags, including AS. */
    assert(!map(BASE,2*P,KERNEL_MM_READ,0));
    limits.address_space=4*P; limits.data_soft=0; limits.data_hard=P;
    CHECK(kernel_mm_mprotect(&mm,BASE,2*P,RW)==KERNEL_MM_STATUS_NO_MEMORY);
    limits.address_space=3*P;
    CHECK(!kernel_mm_mprotect(&mm,BASE,2*P,RW));
    limits.address_space=limits.data_soft=limits.data_hard=UINT64_MAX;
    assert(!kernel_mm_mprotect(&mm,BASE,2*P,KERNEL_MM_READ));
    limits.address_space=P; limits.data_soft=1;
    CHECK(!kernel_mm_mprotect(&mm,BASE,2*P,RW));
    limits.address_space=limits.data_soft=UINT64_MAX;
    assert(!kernel_mm_munmap(&mm,BASE,2*P));
    limits.address_space=P;
    CHECK(map(BASE,2*P,KERNEL_MM_READ,0)==KERNEL_MM_STATUS_NO_MEMORY);
    assert(!kernel_mm_munmap(&mm,BASE,2*P));
    limits.address_space=UINT64_MAX; limits.data_soft=P;
    CHECK(map(BASE,2*P,RW,0)==KERNEL_MM_STATUS_NO_MEMORY);
    assert(!kernel_mm_munmap(&mm,BASE,2*P));
    CHECK(map(BASE,2*P,RW,KERNEL_MM_MAP_SHARED)==KERNEL_MM_STATUS_OK);
    assert(!kernel_mm_munmap(&mm,BASE,2*P));
    CHECK(map(BASE,2*P,KERNEL_MM_READ,0)==KERNEL_MM_STATUS_OK);
    CHECK(kernel_mm_mprotect(&mm,BASE,2*P,RW)==KERNEL_MM_STATUS_NO_MEMORY);
    assert(!kernel_mm_munmap(&mm,BASE,2*P));
    limits.data_soft=0; limits.data_hard=2*P;
    CHECK(map(BASE,2*P,RW,0)==KERNEL_MM_STATUS_OK);
    CHECK(map(BASE+2*P,P,RW,0)==KERNEL_MM_STATUS_NO_MEMORY);
    assert(!kernel_mm_munmap(&mm,BASE,3*P));
    limits.data_soft=UINT64_MAX; limits.data_hard=UINT64_MAX;
    uint64_t result;
    assert(!kernel_mm_brk(&mm,0x10004,&result));
    CHECK(result==0x10004);
    limits.data_soft=5;
    assert(!kernel_mm_brk(&mm,0x10006,&result));
    CHECK(result==0x10004);
    limits.data_soft=UINT64_MAX;
    assert(!kernel_mm_brk(&mm,0x10000,&result));
    /* Fixed replacement checks net virtual growth, including read-only coverage. */
    assert(!map(BASE,2*P,KERNEL_MM_READ,0));
    limits.address_space=2*P; limits.data_soft=1;
    CHECK(map(BASE,2*P,RW,0)==KERNEL_MM_STATUS_OK);
    limits.address_space=P;
    CHECK(map(BASE,2*P,KERNEL_MM_READ,0)==KERNEL_MM_STATUS_NO_MEMORY);
    struct kernel_vma vma;
    assert(!kernel_mm_vma_lookup(&mm,BASE,&vma)); CHECK(vma.permissions&KERNEL_MM_WRITE);
    limits.address_space=limits.data_soft=UINT64_MAX;
    assert(!kernel_mm_munmap(&mm,BASE,2*P));

    assert(!map(BASE,3*P,0,0));
    assert(!kernel_mm_mprotect(&mm,BASE,P,KERNEL_MM_READ));
    limits.data_soft=P;
    CHECK(kernel_mm_mprotect(&mm,BASE,3*P,RW)==KERNEL_MM_STATUS_NO_MEMORY);
    assert(!kernel_mm_vma_lookup(&mm,BASE,&vma)); CHECK(vma.permissions&KERNEL_MM_WRITE);
    assert(!kernel_mm_vma_lookup(&mm,BASE+P,&vma)); CHECK(!vma.permissions);
    limits.data_soft=UINT64_MAX;
    assert(!kernel_mm_munmap(&mm,BASE,3*P));
    assert(!map(BASE,3*P,KERNEL_MM_READ,0));
    assert(!kernel_mm_munmap(&mm,BASE+P,P));
    CHECK(kernel_mm_mprotect(&mm,BASE,3*P,RW)==KERNEL_MM_STATUS_NO_MEMORY);
    assert(!kernel_mm_vma_lookup(&mm,BASE,&vma)); CHECK(vma.permissions&KERNEL_MM_WRITE);
    assert(!kernel_mm_vma_lookup(&mm,BASE+2*P,&vma)); CHECK(!(vma.permissions&KERNEL_MM_WRITE));
    assert(!kernel_mm_munmap(&mm,BASE,3*P));

    assert(!map(BASE,P,RW,0)); assert(!map(BASE+4*P,2*P,KERNEL_MM_READ,0));
    limits.data_soft=P;
    CHECK(kernel_mm_mremap(&mm,BASE,P,2*P,3,BASE+4*P,&result)==-KERNEL_ENOMEM);
    CHECK(!kernel_mm_vma_lookup(&mm,BASE+4*P,&vma));
    limits.data_soft=UINT64_MAX;
    assert(!kernel_mm_munmap(&mm,BASE,6*P));

    /* exec main data byte interval remains independent from current VMAs. */
    assert(!kernel_mm_set_data_layout(&mm,0x30000,0x30020));
    assert(!kernel_mm_brk(&mm,0x10001,&result)&&result==0x10001);
    limits.data_soft=0x21;
    assert(!kernel_mm_brk(&mm,0x10002,&result)); CHECK(result==0x10001);
    limits.data_soft=UINT64_MAX;
    assert(!kernel_mm_brk(&mm,0x10000,&result));

    const uint64_t top=UINT64_C(0x1000000);
    assert(!kernel_mm_vma_insert_anon(&mm,top-2*P,top,RW,KERNEL_VMA_ROLE_STACK,KERNEL_VMA_FAULT_DEMAND_ZERO));
    stack_limit=4*P; limits.address_space=3*P; limits.data_soft=1;
    CHECK(!kernel_mm_resolve_user_fault(&mm,top-3*P,KERNEL_MM_WRITE));
    struct kernel_mm_proc_memory snapshot;
    assert(!kernel_mm_proc_memory_snapshot(&mm,&snapshot));CHECK(snapshot.virtual_bytes==3*P);
    CHECK(kernel_mm_resolve_user_fault(&mm,top-4*P,KERNEL_MM_WRITE)==KERNEL_MM_STATUS_NOT_MAPPED);
    limits.address_space=UINT64_MAX; stack_limit=3*P;
    CHECK(kernel_mm_resolve_user_fault(&mm,top-4*P,KERNEL_MM_WRITE)==KERNEL_MM_STATUS_NOT_MAPPED);
    stack_limit=0;
    CHECK(!kernel_mm_resolve_user_fault(&mm,top-P,KERNEL_MM_WRITE));
    stack_limit=UINT64_MAX; limits.data_soft=UINT64_MAX;
    assert(!map(top-5*P,P,KERNEL_MM_READ,0));
    CHECK(kernel_mm_resolve_user_fault(&mm,top-4*P,KERNEL_MM_WRITE)==KERNEL_MM_STATUS_NOT_MAPPED);
    assert(!kernel_mm_munmap(&mm,top-5*P,P));
    CHECK(!kernel_mm_resolve_user_fault(&mm,top-4*P,KERNEL_MM_WRITE));
    CHECK(kernel_mm_resolve_user_fault(&mm,top-KERNEL_RLIMIT_STACK_CAP-P,KERNEL_MM_WRITE)==KERNEL_MM_STATUS_NOT_MAPPED);
    /* 独立共享对象阻止VMA合并，验证一次保护不按碎片数重复完成TLB。 */
    for (unsigned i=0;i<32;i++) {
        assert(!map(BASE+i*P,P,RW,KERNEL_MM_MAP_SHARED));
        assert(!kernel_mm_resolve_user_fault(&mm,BASE+i*P,KERNEL_MM_WRITE));
    }
    struct kernel_mm_concurrency_state before,after;
    kernel_mm_get_concurrency_state(&mm,&before);
    CHECK(!kernel_mm_mprotect(&mm,BASE,32*P,KERNEL_MM_READ));
    kernel_mm_get_concurrency_state(&mm,&after);
    CHECK(after.generation-before.generation<=2);
    for (unsigned i=0;i<32;i++) {
        assert(!kernel_mm_vma_lookup(&mm,BASE+i*P,&vma));CHECK(vma.permissions==KERNEL_MM_READ);
        struct kernel_mm_mapping mapping;
        assert(!kernel_mm_lookup(&mm,BASE+i*P,&mapping));CHECK(!(mapping.permissions&KERNEL_MM_WRITE));
    }
    before=after;
    CHECK(!kernel_mm_mprotect(&mm,BASE,32*P,KERNEL_MM_READ));
    kernel_mm_get_concurrency_state(&mm,&after);CHECK(after.generation==before.generation);
    assert(!kernel_mm_munmap(&mm,BASE,32*P));
    for (unsigned i=0;i<32;i++) assert(!map(BASE+i*P,P,i%2?0:KERNEL_MM_READ,0));
    limits.data_soft=16*P;
    kernel_mm_get_concurrency_state(&mm,&before);
    CHECK(kernel_mm_mprotect(&mm,BASE,32*P,RW)==KERNEL_MM_STATUS_NO_MEMORY);
    kernel_mm_get_concurrency_state(&mm,&after);CHECK(after.generation-before.generation<=2);
    for (unsigned i=0;i<32;i++) {
        assert(!kernel_mm_vma_lookup(&mm,BASE+i*P,&vma));CHECK((vma.permissions&KERNEL_MM_WRITE)==(i<16?KERNEL_MM_WRITE:0));
    }
    limits.data_soft=UINT64_MAX;
    assert(!kernel_mm_munmap(&mm,BASE,32*P));
    /* 用真实分配拒绝找到描述符扩容边界，不依赖私有capacity数值。 */
    fail_resize=1;
    unsigned fragments=0;
    for (;fragments<256;fragments++) {
        enum kernel_mm_status mapped=map(BASE+fragments*2*P,2*P,RW,KERNEL_MM_MAP_SHARED);
        if (mapped==KERNEL_MM_STATUS_NO_MEMORY) break;
        assert(mapped==KERNEL_MM_STATUS_OK);
    }
    CHECK(fragments>1 && fragments<256);
    CHECK(kernel_mm_mprotect(&mm,BASE+P,P,KERNEL_MM_READ)==KERNEL_MM_STATUS_NO_MEMORY);
    assert(!kernel_mm_vma_lookup(&mm,BASE+P,&vma));CHECK(vma.permissions==RW);
    CHECK(kernel_mm_mprotect(&mm,BASE,(fragments*2-1)*P,KERNEL_MM_READ)==KERNEL_MM_STATUS_NO_MEMORY);
    assert(!kernel_mm_vma_lookup(&mm,BASE,&vma));CHECK(vma.permissions==KERNEL_MM_READ);
    assert(!kernel_mm_vma_lookup(&mm,BASE+(fragments-1)*2*P,&vma));CHECK(vma.permissions==RW);
    fail_resize=0;
    assert(!kernel_mm_munmap(&mm,BASE,fragments*2*P));
    /* Locked usage belongs to this MM; the initiating actor supplies admission. */
    assert(!map(BASE,4*P,RW,0));
    unsigned char residency[4]={0xaa,0xaa,0xaa,0xaa};unsigned completed=0;
    CHECK(!kernel_mm_mincore(&mm,BASE,4,residency,&completed)&&completed==4&&!residency[0]&&!residency[3]);
    CHECK(!kernel_mm_mlock(&mm,BASE,P,0));
    fail_page=1;CHECK(kernel_mm_mlock(&mm,BASE+P,P,0)==-KERNEL_EAGAIN);fail_page=0;
    CHECK(!kernel_mm_mincore(&mm,BASE,4,residency,&completed)&&residency[0]==1&&!residency[1]);
    CHECK(!kernel_mm_mprotect(&mm,BASE,P,0));
    CHECK(!kernel_mm_mincore(&mm,BASE,1,residency,&completed)&&residency[0]==1);
    CHECK(!kernel_mm_munlockall(&mm));
    CHECK(!kernel_mm_mprotect(&mm,BASE,P,RW));
    memlock_limit=2*P;
    CHECK(!kernel_mm_mlock(&mm,BASE+1,P,1));
    struct kernel_mm_proc_memory memory;
    CHECK(!kernel_mm_proc_memory_snapshot(&mm,&memory)&&memory.locked_bytes==2*P);
    CHECK(kernel_mm_mlock(&mm,BASE+2*P,P,1)==-KERNEL_ENOMEM);
    uint64_t moved=0;
    CHECK(kernel_mm_mremap(&mm,BASE,2*P,3*P,1,0,&moved)==-KERNEL_EAGAIN);
    CHECK(!kernel_mm_mprotect(&mm,BASE,P,0));
    CHECK(!kernel_mm_vma_lookup(&mm,BASE,&vma)&&vma.flags==3);
    CHECK(!kernel_mm_munlock(&mm,BASE,4*P));
    CHECK(!kernel_mm_mlockall(&mm,6));
    CHECK(map(BASE,4*P,RW,KERNEL_MM_MAP_LOCKED)==KERNEL_MM_STATUS_LOCK_LIMIT);
    CHECK(!kernel_mm_vma_lookup(&mm,BASE,&vma)&&!vma.flags);
    CHECK(!kernel_mm_munlockall(&mm));
    assert(!kernel_mm_munmap(&mm,BASE+P,P));
    CHECK(kernel_mm_mlock(&mm,BASE,4*P,1)==-KERNEL_ENOMEM);
    CHECK(!kernel_mm_proc_memory_snapshot(&mm,&memory)&&memory.locked_bytes==0);
    memlock_limit=UINT64_MAX;
    CHECK(kernel_mm_mlock(&mm,BASE,4*P,1)==-KERNEL_ENOMEM);
    CHECK(!kernel_mm_proc_memory_snapshot(&mm,&memory)&&memory.locked_bytes==P);
    CHECK(!kernel_mm_munlockall(&mm)&&!kernel_mm_munmap(&mm,BASE,4*P));
    memlock_limit=0;CHECK(kernel_mm_mlock(&mm,0,0,0)==-KERNEL_EPERM);
    CHECK(map(BASE,P,RW,KERNEL_MM_MAP_LOCKED)==KERNEL_MM_STATUS_LOCK_PERMISSION);
    memlock_limit=UINT64_MAX;
    CHECK(!map(BASE,2*P,RW,0)&&!kernel_mm_mlock(&mm,BASE,2*P,0));
    uint64_t resident_available=physical_page_available(&pages);
    CHECK(!kernel_mm_mlock(&mm,BASE,2*P,0));
    CHECK(physical_page_available(&pages)==resident_available);
    CHECK(!kernel_mm_munmap(&mm,BASE,2*P));
    memlock_limit=P;CHECK(!kernel_mm_mlockall(&mm,6));
    CHECK(!kernel_mm_brk(&mm,0x10000+P,&result)&&result==0x10000+P);
    CHECK(!kernel_mm_proc_memory_snapshot(&mm,&memory)&&memory.locked_bytes==P);
    CHECK(!kernel_mm_brk(&mm,0x10000+2*P,&result)&&result==0x10000+P);
    CHECK(!kernel_mm_brk(&mm,0x10000,&result)&&!kernel_mm_munlockall(&mm));
    memlock_limit=4*P;CHECK(!kernel_mm_mlock(&mm,top-4*P,4*P,1));
    CHECK(kernel_mm_resolve_user_fault(&mm,top-5*P,KERNEL_MM_WRITE)==KERNEL_MM_STATUS_NOT_MAPPED);
    memlock_limit=5*P;CHECK(!kernel_mm_resolve_user_fault(&mm,top-5*P,KERNEL_MM_WRITE));
    CHECK(!kernel_mm_proc_memory_snapshot(&mm,&memory)&&memory.locked_bytes==5*P);
    CHECK(!kernel_mm_munlockall(&mm));memlock_limit=UINT64_MAX;
    mm_test_context=0; assert(!kernel_mm_release(&mm));
    struct kernel_heap_statistics stats; kernel_heap_get_statistics(&heap,&stats);
    CHECK(!stats.live_allocations && !stats.current_pages && physical_page_available(&pages)==baseline);
    CHECK(!physical_page_allocator_audit(&pages));
    printf("MM_LIMITS checks=%u failures=%u\n",checks,failures);
    return failures?1:0;
}
