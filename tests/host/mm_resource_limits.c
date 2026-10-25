#include <kernel/mm_backend.h>
#include <kernel/rlimit.h>
#include <kernel/heap.h>
#include <kernel/cpu.h>
#include <kernel/sync.h>
#include <kernel/vma.h>
#include <kernel/errno.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
extern _Thread_local uint64_t mm_test_context;
void mm_test_attach(unsigned);
static unsigned char pool[16*1024*1024] __attribute__((aligned(16384)));
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
void mm_test_fence(uint64_t v,int instructions) { (void)v; (void)instructions; }
static void *access(uint64_t p) { return (void *)(uintptr_t)p; }
static int address_of(const void *p,uint64_t *out) { *out=(uintptr_t)p; return 1; }
#define P BOAROS_PAGE_SIZE
#define RW (KERNEL_MM_READ|KERNEL_MM_WRITE)
#define BASE UINT64_C(0x400000)
static unsigned fail_resize, fail_page, mm_gate_acquisitions;
void __real_kernel_rwlock_write(struct kernel_rwlock *,struct kernel_lock_guard *);
void __wrap_kernel_rwlock_write(struct kernel_rwlock *lock,struct kernel_lock_guard *guard)
{ if(lock->rank==25)mm_gate_acquisitions++;__real_kernel_rwlock_write(lock,guard); }
enum physical_page_status __real_physical_page_allocate(struct physical_page_allocator *,uint64_t *);
enum physical_page_status __wrap_physical_page_allocate(struct physical_page_allocator *a,uint64_t *out)
{ return fail_page ? PHYSICAL_PAGE_STATUS_EMPTY : __real_physical_page_allocate(a,out); }
enum kernel_heap_status __real_kernel_heap_resize(struct kernel_heap *,void *,size_t,void **);
enum kernel_heap_status __wrap_kernel_heap_resize(struct kernel_heap *h,void *old,size_t size,void **out)
{ return fail_resize ? KERNEL_HEAP_STATUS_EMPTY : __real_kernel_heap_resize(h,old,size,out); }
static unsigned checks,failures;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"FAIL %s:%u: %s\n",__FILE__,__LINE__,#x); failures++; } } while(0)
static enum kernel_mm_status map(uint64_t at,uint64_t len,uint32_t prot,uint32_t flags)
{ uint64_t out=0; return kernel_mm_mmap_anonymous(&mm,at,len,prot,KERNEL_MM_MAP_FIXED|flags,&out); }
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
    CHECK(!kernel_mm_vma_lookup(&mm,BASE,&vma)&&vma.lock_flags==3);
    CHECK(!kernel_mm_munlock(&mm,BASE,4*P));
    CHECK(!kernel_mm_mlockall(&mm,6));
    CHECK(map(BASE,4*P,RW,KERNEL_MM_MAP_LOCKED)==KERNEL_MM_STATUS_LOCK_LIMIT);
    CHECK(!kernel_mm_vma_lookup(&mm,BASE,&vma)&&!vma.lock_flags);
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
    mm_gate_acquisitions=0;CHECK(!kernel_mm_mlock(&mm,BASE,2*P,0));
    CHECK(mm_gate_acquisitions==3); /* admission + one current-VMA/fault step per page */
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
