/* 数字物理地址0的owner与空候选必须可区分；映射回独立宿主pool。 */
#include <kernel/mm_backend.h>
#include <kernel/heap.h>
#include <kernel/errno.h>
#include <kernel/vma.h>
#include <kernel/hugetlb.h>
#include <kernel/rlimit.h>
#include <kernel/uaccess.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void mm_test_attach(unsigned);
static unsigned char pool[16 * 1024 * 1024] __attribute__((aligned(4096)));
static struct physical_page_allocator pages;
static struct arch_mmu_page_table kernel;
static unsigned access_calls, fences;
void mm_test_fence(uint64_t,int);
static void *access(uint64_t physical)
{ access_calls++; return physical < sizeof(pool) ? pool + physical : 0; }
void riscv_smp_sync_user(uint32_t targets,uint64_t address,uint64_t size,int instructions)
{ (void)targets;(void)size;mm_test_fence(address,instructions); }
void riscv_smp_sync_instructions(uint32_t targets)
{ (void)targets;mm_test_fence(0,1); }
void riscv_smp_sync_kernel(uint64_t address,uint64_t size)
{ (void)size;mm_test_fence(address,0); }
void kernel_console_putc(char c) { fputc(c, stderr); }
void mm_test_unavailable(void) { abort(); }
void mm_test_fence(uint64_t address, int instructions)
{ (void)address; (void)instructions; fences++; __atomic_thread_fence(__ATOMIC_SEQ_CST); }
static uint64_t allocate(void)
{
    uint64_t physical;
    assert(!physical_page_allocate(&pages, &physical));
    memset(access(physical), 0, BOAROS_PAGE_SIZE);
    return physical;
}
#define HUGE (UINT64_C(1)<<21)
static int address_of(const void *p,uint64_t *out) { *out=(const unsigned char *)p-pool;return 1; }
static void put(struct kernel_mm *m,uint64_t a,unsigned char b)
{ size_t n=0;assert(!kernel_copy_to_user(m,a,&b,1,&n)&&n==1); }
static unsigned char get(struct kernel_mm *m,uint64_t a)
{ unsigned char b=0;size_t n=0;assert(!kernel_copy_from_user(m,&b,a,1,&n)&&n==1);return b; }
static unsigned stale_registration;
static unsigned reject_base, reject_thp, reject_split_table;
static uint64_t data_limit=UINT64_MAX;
void kernel_task_current_memory_limits(struct kernel_memory_limits *out)
{ *out=(struct kernel_memory_limits){.address_space=UINT64_MAX,.data_soft=data_limit,.data_hard=UINT64_MAX}; }
enum physical_page_status __real_physical_page_allocate(struct physical_page_allocator *,uint64_t *);
enum physical_page_status __real_physical_page_allocate_order_nosleep(
    struct physical_page_allocator *,uint32_t,uint64_t *);
enum physical_page_status __wrap_physical_page_allocate(struct physical_page_allocator *allocator,uint64_t *address)
{
    if (reject_base) { reject_base--;return PHYSICAL_PAGE_STATUS_EMPTY; }
    return __real_physical_page_allocate(allocator,address);
}
enum physical_page_status __wrap_physical_page_allocate_order_nosleep(
    struct physical_page_allocator *allocator,uint32_t order,uint64_t *address)
{
    if (order==9 && reject_thp) { reject_thp--;return PHYSICAL_PAGE_STATUS_EMPTY; }
    if (!order && reject_split_table) { reject_split_table--;return PHYSICAL_PAGE_STATUS_EMPTY; }
    return __real_physical_page_allocate_order_nosleep(allocator,order,address);
}
enum kernel_hugetlb_status __real_kernel_hugetlb_mapping_register(struct kernel_hugetlb_mapping *,uint64_t);
enum kernel_hugetlb_status __wrap_kernel_hugetlb_mapping_register(struct kernel_hugetlb_mapping *m,uint64_t epoch)
{
 if (stale_registration && !--stale_registration) return __real_kernel_hugetlb_mapping_register(m,epoch+1);
 return __real_kernel_hugetlb_mapping_register(m,epoch);
}
int main(void)
{
 mm_test_attach(0);
 struct boot_memory_layout layout={0};layout.usable_count=1;layout.usable[0].size=sizeof(pool);
 assert(!physical_page_allocator_init(&pages,&layout));assert(!physical_page_allocator_bind_access(&pages,access));assert(!physical_page_allocator_finalize(&pages));
 kernel=(struct arch_mmu_page_table){.allocator=&pages,.root_address=allocate(),.table_pages=1,.state=ARCH_MMU_STATE_ACTIVE};
 struct kernel_heap heap;assert(!kernel_heap_init(&heap,&pages,address_of));
 struct arch_mmu_user_space space={0};struct kernel_mm parent={0},child={0};
 assert(!arch_mmu_user_space_init(&space,&pages,&kernel));assert(!kernel_mm_create(&parent,&space));assert(!kernel_mm_vma_enable(&parent,&heap));
 assert(!kernel_mm_brk_initialize(&parent,0x10000,0x20000));
 assert(!kernel_mm_mmap_base_initialize(&parent,64*HUGE));
 struct kernel_hstate *h;assert(!kernel_hstate_get_or_create(&heap,&pages,9,&h));assert(!kernel_hstate_resize(h,3));
 uint64_t a,stack;
 assert(!kernel_mm_mmap_anonymous(&parent,16*HUGE+4096,4096,3,KERNEL_MM_MAP_FIXED|KERNEL_MM_MAP_GROWSDOWN,&stack));
 assert(!kernel_mm_mmap_hugetlb(&parent,15*HUGE,HUGE,3,0,0,0,&a));assert(a!=15*HUGE);
 assert(!kernel_mm_munmap(&parent,a,HUGE));assert(!kernel_mm_munmap(&parent,stack,4096));
 assert(!kernel_mm_mmap_hugetlb(&parent,0,HUGE,3,0,0,0,&a));uint64_t ceiling=a+HUGE;
 assert(!kernel_mm_munmap(&parent,a,HUGE));
 assert(!kernel_mm_mmap_anonymous(&parent,ceiling+4096,4096,3,KERNEL_MM_MAP_FIXED|KERNEL_MM_MAP_GROWSDOWN,&stack));
 assert(!kernel_mm_mmap_hugetlb(&parent,0,HUGE,3,0,0,0,&a));assert(a+HUGE<=stack-KERNEL_VMA_STACK_GUARD_PAGES*BOAROS_PAGE_SIZE);
 assert(!kernel_mm_munmap(&parent,a,HUGE));assert(!kernel_mm_munmap(&parent,stack,4096));
 assert(!kernel_mm_mmap_hugetlb(&parent,HUGE,HUGE,3,KERNEL_MM_MAP_FIXED,0,0,&a)&&a==HUGE);
 struct kernel_hstate_snapshot snap;kernel_hstate_snapshot(h,&snap);assert(snap.reserved==1&&snap.free==3);
 put(&parent,HUGE+12345,42);assert(get(&parent,HUGE+12345)==42);
 unsigned char resident[16];unsigned scanned=0;
 for(unsigned j=0;j<512;j+=16) { assert(!kernel_mm_mincore(&parent,HUGE+j*4096,16,resident,&scanned)&&scanned==16);
 for(unsigned i=0;i<16;i++) assert(resident[i]==1); }
 assert(kernel_mm_mprotect(&parent,HUGE+4096,HUGE-4096,0)==KERNEL_MM_STATUS_INVALID_ARGUMENT);
 assert(!kernel_mm_mprotect(&parent,HUGE,HUGE,0));size_t copied=0;unsigned char byte;assert(kernel_copy_from_user(&parent,&byte,HUGE+12345,1,&copied));
 assert(!kernel_mm_mprotect(&parent,HUGE,HUGE,3));assert(get(&parent,HUGE+12345)==42);
 kernel_hstate_snapshot(h,&snap);assert(snap.reserved==0&&snap.free==2);
 assert(!kernel_mm_mmap_hugetlb(&parent,3*HUGE,HUGE,3,KERNEL_MM_MAP_FIXED,0,0,&a));
 stale_registration=2;assert(kernel_mm_fork(&child,&parent));assert(!stale_registration);
 child=(struct kernel_mm){0};kernel_hstate_snapshot(h,&snap);assert(snap.maps==2&&snap.reserved==1&&snap.free==2);
 assert(get(&parent,HUGE+12345)==42);assert(!kernel_mm_munmap(&parent,3*HUGE,HUGE));
 assert(!kernel_mm_fork(&child,&parent));put(&child,HUGE+12345,17);assert(get(&parent,HUGE+12345)==42&&get(&child,HUGE+12345)==17);
 assert(kernel_mm_munmap(&parent,HUGE+4096,HUGE-4096)==KERNEL_MM_STATUS_INVALID_ARGUMENT);
 assert(!kernel_mm_release(&child));child=(struct kernel_mm){0};assert(!kernel_mm_munmap(&parent,HUGE,HUGE));
 kernel_hstate_snapshot(h,&snap);assert(snap.free==3&&!snap.reserved);
 assert(kernel_mm_mmap_hugetlb(&parent,HUGE,4*HUGE,3,KERNEL_MM_MAP_FIXED,0,0,&a)==KERNEL_MM_STATUS_NO_MEMORY);
 assert(!kernel_mm_mmap_hugetlb(&parent,HUGE,2*HUGE,3,KERNEL_MM_MAP_FIXED,0,0,&a));put(&parent,HUGE,1);put(&parent,2*HUGE,2);
 for(unsigned i=0;i<3;i++){assert(!kernel_mm_fork(&child,&parent));put(&child,HUGE,(unsigned char)(10+i));assert(get(&parent,HUGE)==1);assert(!kernel_mm_release(&child));child=(struct kernel_mm){0};}
 assert(!kernel_mm_munmap(&parent,2*HUGE,HUGE));kernel_hstate_snapshot(h,&snap);assert(snap.free==2&&!snap.reserved&&snap.maps==1);
 assert(!kernel_mm_munmap(&parent,HUGE,HUGE));kernel_hstate_snapshot(h,&snap);assert(snap.free==3&&!snap.reserved&&!snap.maps);
 assert(!kernel_hstate_resize(h,1));
 assert(!kernel_mm_mmap_hugetlb(&parent,HUGE,HUGE,3,KERNEL_MM_MAP_FIXED,0,0,&a));put(&parent,HUGE+99,72);
 assert(!kernel_mm_fork(&child,&parent));
 struct kernel_mm_user_pin child_pin={0};assert(!kernel_mm_pin_user_page(&child,HUGE+99,KERNEL_MM_READ,&child_pin));
 assert(kernel_mm_resolve_user_fault(&parent,HUGE+99,KERNEL_MM_WRITE)==KERNEL_MM_STATUS_CONFLICT);
 kernel_mm_unpin_user_page(&child_pin);
 put(&parent,HUGE+99,73);assert(get(&parent,HUGE+99)==73);
 unsigned char b;size_t n=0;assert(kernel_copy_from_user(&child,&b,HUGE+99,1,&n));
 assert(!kernel_mm_release(&child));child=(struct kernel_mm){0};
 assert(!kernel_mm_madvise(&parent,HUGE,4096,KERNEL_MM_ADVICE_DONTNEED));assert(get(&parent,HUGE+99)==73);
 assert(!kernel_mm_madvise(&parent,HUGE,HUGE,KERNEL_MM_ADVICE_DONTNEED));assert(get(&parent,HUGE+99)==0);
 assert(!kernel_mm_munmap(&parent,HUGE,HUGE));assert(!kernel_hstate_resize(h,3));
 assert(!kernel_mm_mmap_hugetlb(&parent,HUGE,HUGE,3,KERNEL_MM_MAP_FIXED,1,0,&a));put(&parent,HUGE+777,31);
 assert(!kernel_mm_fork(&child,&parent));put(&child,HUGE+777,61);assert(get(&parent,HUGE+777)==61);
 assert(!kernel_mm_release(&child));child=(struct kernel_mm){0};
 assert(!kernel_mm_madvise(&parent,HUGE,HUGE,KERNEL_MM_ADVICE_DONTNEED));
 assert(!kernel_mm_mincore(&parent,HUGE,16,resident,&scanned));for(unsigned i=0;i<16;i++)assert(resident[i]==1);
 assert(get(&parent,HUGE+777)==61);
 assert(!kernel_mm_munmap(&parent,HUGE,HUGE));assert(!kernel_hstate_resize(h,0));
 assert(kernel_mm_mmap_hugetlb(&parent,HUGE,HUGE,3,KERNEL_MM_MAP_FIXED,0,0,&a)==KERNEL_MM_STATUS_NO_MEMORY);
 assert(!kernel_mm_mmap_hugetlb(&parent,HUGE,HUGE,3,KERNEL_MM_MAP_FIXED,0,1,&a));
 n=0;assert(kernel_copy_from_user(&parent,&b,HUGE,1,&n));
 assert(!kernel_mm_munmap(&parent,HUGE,HUGE));
 assert(!kernel_hstate_resize(h,1));
 assert(!kernel_mm_mmap_hugetlb(&parent,HUGE,HUGE,3,KERNEL_MM_MAP_FIXED,0,0,&a));
 put(&parent,HUGE+33,47);
 uint64_t other=UINT64_C(2)<<30,destination;
 assert(!kernel_mm_mmap_anonymous(&parent,other,HUGE,3,KERNEL_MM_MAP_FIXED,&destination));
 put(&parent,other,22);
 reject_base=1;
 assert(kernel_mm_mremap(&parent,HUGE,HUGE,HUGE,3,other,&destination)==-KERNEL_ENOMEM);
 assert(!reject_base && get(&parent,HUGE+33)==47);
 struct kernel_vma vma;
 assert(kernel_mm_vma_lookup(&parent,other,&vma)==KERNEL_MM_STATUS_NOT_MAPPED);
 assert(!kernel_mm_mremap(&parent,HUGE,HUGE,HUGE,3,other,&destination));
 assert(destination==other && get(&parent,other+33)==47);
 assert(!kernel_mm_munmap(&parent,other,HUGE) && !kernel_hstate_resize(h,0));

 assert(!kernel_mm_mmap_anonymous(&parent,HUGE,HUGE,3,KERNEL_MM_MAP_FIXED,&a));
 assert(!kernel_mm_madvise(&parent,HUGE,HUGE,KERNEL_MM_ADVICE_HUGEPAGE));
 reject_thp=1;put(&parent,HUGE,57);
 assert(!reject_thp && !physical_page_thp_pages(&pages));
 reject_thp=1;
 assert(kernel_mm_madvise(&parent,HUGE,HUGE,KERNEL_MM_ADVICE_COLLAPSE)==-KERNEL_ENOMEM);
 assert(!reject_thp && get(&parent,HUGE)==57 && !physical_page_thp_pages(&pages));
 assert(!kernel_mm_madvise(&parent,HUGE,HUGE,KERNEL_MM_ADVICE_COLLAPSE));
 assert(get(&parent,HUGE)==57 && physical_page_thp_pages(&pages)==512);
 reject_split_table=1;
 assert(kernel_mm_mmap_anonymous(&parent,HUGE+4096,4096,3,KERNEL_MM_MAP_FIXED,&a)==KERNEL_MM_STATUS_NO_MEMORY);
 assert(!reject_split_table && get(&parent,HUGE)==57 && physical_page_thp_pages(&pages)==512);
 assert(!kernel_mm_mprotect(&parent,HUGE,HUGE,KERNEL_MM_READ));
 assert(!kernel_mm_madvise(&parent,HUGE+HUGE/2,HUGE/2,KERNEL_MM_ADVICE_DONTDUMP));
 data_limit=HUGE/2;
 assert(kernel_mm_mprotect(&parent,HUGE,HUGE,3)==KERNEL_MM_STATUS_NO_MEMORY);
 struct kernel_mm_mapping mapping;
 assert(!kernel_mm_lookup(&parent,HUGE,&mapping) && (mapping.permissions&KERNEL_MM_WRITE));
 assert(!kernel_mm_lookup(&parent,HUGE+HUGE/2,&mapping) && !(mapping.permissions&KERNEL_MM_WRITE));
 data_limit=UINT64_MAX;
 assert(!kernel_mm_munmap(&parent,HUGE+4096,4096));
 assert(get(&parent,HUGE)==57 && !physical_page_thp_pages(&pages));
 assert(!kernel_mm_release(&parent));
 kernel_hstate_snapshot(h,&snap);assert(snap.free==0&&!snap.reserved&&!snap.maps);
 kernel_hstate_put(h);assert(!kernel_hstate_shutdown(&pages));assert(!kernel_hstate_shutdown(&pages));assert(!physical_page_allocator_audit(&pages));
 puts("hugetlb MM fork, move rollback and transparent huge page fallback, collapse and split passed");
}
