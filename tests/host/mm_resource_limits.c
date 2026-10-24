#include <kernel/mm_backend.h>
#include <kernel/rlimit.h>
#include <kernel/heap.h>
#include <kernel/cpu.h>
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
static uint64_t stack_limit=UINT64_MAX;
uint64_t kernel_task_current_stack_limit(void) { return stack_limit; }
static struct kernel_memory_limits limits={UINT64_MAX,UINT64_MAX,UINT64_MAX};
void kernel_task_current_memory_limits(struct kernel_memory_limits *out) { *out=limits; }
void kernel_console_putc(char c) { fputc(c,stderr); }
void mm_test_unavailable(void) { abort(); }
void mm_test_fence(uint64_t v,int instructions) { (void)v; (void)instructions; }
static void *access(uint64_t p) { return (void *)(uintptr_t)p; }
static int address_of(const void *p,uint64_t *out) { *out=(uintptr_t)p; return 1; }
#define P BOAROS_PAGE_SIZE
#define RW (KERNEL_MM_READ|KERNEL_MM_WRITE)
#define BASE UINT64_C(0x400000)
static unsigned fail_resize;
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
    mm_test_context=0; assert(!kernel_mm_release(&mm));
    struct kernel_heap_statistics stats; kernel_heap_get_statistics(&heap,&stats);
    CHECK(!stats.live_allocations && !stats.current_pages && physical_page_available(&pages)==baseline);
    CHECK(!physical_page_allocator_audit(&pages));
    printf("MM_LIMITS checks=%u failures=%u\n",checks,failures);
    return failures?1:0;
}
