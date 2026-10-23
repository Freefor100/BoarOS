#include <kernel/mm_backend.h>
#include <kernel/uaccess.h>
#include <kernel/heap.h>
#include <kernel/errno.h>
#include <kernel/tmpfs.h>
#include <kernel/open_file.h>
#include <kernel/cpu.h>
#include <kernel/sync.h>
#include "../../fs/open_file_internal.h"
#include <pthread.h>
#include <sched.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern _Thread_local uint64_t mm_test_context;
void mm_test_attach(unsigned);
unsigned mm_test_parked(unsigned);
static unsigned char pool[16*1024*1024] __attribute__((aligned(16384)));
static struct physical_page_allocator pages;
static struct kernel_heap heap;
static unsigned delay_fence, fence_entered, fence_resume;
void mm_test_fence(uint64_t v,int instructions)
{
    (void)v;
    if (!instructions && __atomic_exchange_n(&delay_fence,0,__ATOMIC_ACQ_REL)) {
        __atomic_store_n(&fence_entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&fence_resume,__ATOMIC_ACQUIRE)) sched_yield();
    }
}
static void *access(uint64_t p) { return (void *)(uintptr_t)p; }
static int address_of(const void *p,uint64_t *out) { *out=(uintptr_t)p; return 1; }
static void write_byte(struct kernel_mm *mm,uint64_t a,unsigned char v)
{ size_t done; assert(!kernel_copy_to_user(mm,a,&v,1,&done) && done==1); }
static unsigned char read_byte(struct kernel_mm *mm,uint64_t a)
{ unsigned char v; size_t done; assert(!kernel_copy_from_user(mm,&v,a,1,&done) && done==1); return v; }
#define RW (KERNEL_MM_READ|KERNEL_MM_WRITE)
static unsigned fail_page;
enum physical_page_status __real_physical_page_allocate(struct physical_page_allocator *,uint64_t *);
enum physical_page_status __wrap_physical_page_allocate(struct physical_page_allocator *a,uint64_t *out)
{
    if (fail_page && !--fail_page) return PHYSICAL_PAGE_STATUS_EMPTY;
    return __real_physical_page_allocate(a,out);
}
static void map(struct kernel_mm *mm,uint64_t a,uint64_t size,int shared)
{
    uint64_t out;
    assert(!kernel_mm_mmap_anonymous(mm,a,size,RW,KERNEL_MM_MAP_FIXED|
        (shared?KERNEL_MM_MAP_SHARED:0),&out) && out==a);
}
static void anonymous_cases(struct kernel_mm *parent)
{
    const uint64_t p=BOAROS_PAGE_SIZE, a=0x100000, b=0x200000, d=0xc0000000;
    uint64_t out, root=mm_test_context, child_root;
    struct kernel_mm child={0}; struct kernel_mm_mapping before,after;
    map(parent,a,3*p,0); write_byte(parent,a,11); write_byte(parent,a+p,22); write_byte(parent,a+2*p,33);
    map(parent,b,p,1); write_byte(parent,b,44);
    assert(kernel_mm_mremap(parent,b,0,p,0,0,&out)==-KERNEL_ENOMEM);
    assert(!kernel_mm_fork(&child,parent) && !kernel_mm_context(&child,&child_root));
    assert(!kernel_mm_lookup(parent,a+p,&before));
    assert(!kernel_mm_mprotect(parent,a+p,p,0));
    assert(!kernel_mm_mremap(parent,a+p,p,p,3,d,&out) && out==d);
    assert(kernel_mm_lookup(parent,d,&after)==KERNEL_MM_STATUS_NOT_MAPPED);
    struct kernel_vma vma;
    assert(!kernel_mm_vma_lookup(parent,d,&vma) && !vma.permissions);
    assert(!kernel_mm_mprotect(parent,d,p,RW));
    assert(!kernel_mm_lookup(parent,d,&after) && after.physical_address==before.physical_address);
    write_byte(parent,d,55); assert(read_byte(parent,a)==11 && read_byte(parent,a+2*p)==33);
    mm_test_context=child_root; assert(read_byte(&child,a+p)==22); mm_test_context=root;
    /* Each fork grows its view of the same sparse anonymous object. */
    assert(!kernel_mm_mremap(parent,b,p,2*p,0,0,&out) && out==b);
    mm_test_context=child_root;
    assert(!kernel_mm_mremap(&child,b,p,2*p,0,0,&out) && out==b);
    mm_test_context=root; write_byte(parent,b+p,66);
    mm_test_context=child_root; assert(read_byte(&child,b+p)==66); mm_test_context=root;
    assert(!kernel_mm_mremap(parent,b+p,p,2*p,3,d+4*p,&out));
    assert(read_byte(parent,d+4*p)==66 && read_byte(parent,d+5*p)==0);
    write_byte(parent,d+4*p,77);
    mm_test_context=child_root; assert(read_byte(&child,b+p)==77); mm_test_context=root;
    assert(!kernel_mm_mremap(parent,d+4*p,2*p,p,0,0,&out) && out==d+4*p);
    assert(kernel_mm_vma_lookup(parent,d+5*p,&vma)==KERNEL_MM_STATUS_NOT_MAPPED);
    /* A blocked suffix requires MAYMOVE, and the source prefix survives. */
    map(parent,0x1000000,2*p,0); write_byte(parent,0x1000000,88);
    assert(kernel_mm_mremap(parent,0x1000000,p,3*p,0,0,&out)==-KERNEL_ENOMEM);
    assert(!kernel_mm_mremap(parent,0x1000000,p,3*p,1,0,&out) && out!=0x1000000);
    assert(read_byte(parent,out)==88 && read_byte(parent,out+2*p)==0);
    assert(!kernel_mm_vma_lookup(parent,0x1000000+p,&vma));
    mm_test_context=0; assert(!kernel_mm_release(&child)); mm_test_context=root;
}
static void preparation_failures(struct kernel_mm *parent)
{
    const uint64_t from=0x10000000, length=(UINT64_C(1)<<21)+BOAROS_PAGE_SIZE, to=0x100000000;
    uint64_t out; struct kernel_mm_mapping before,after; struct kernel_vma vma;
    map(parent,from,length,0); write_byte(parent,from,91); write_byte(parent,from+length-BOAROS_PAGE_SIZE,92);
    assert(!kernel_mm_lookup(parent,from,&before));
    for (unsigned failure=1;failure<=3;failure++) {
        uint64_t available=physical_page_available(&pages);
        fail_page=failure; out=123;
        assert(kernel_mm_mremap(parent,from,length,length,3,to,&out)==-KERNEL_ENOMEM);
        assert(!fail_page && out==123 && physical_page_available(&pages)==available);
        assert(!kernel_mm_lookup(parent,from,&after) && before.physical_address==after.physical_address);
        assert(read_byte(parent,from)==91 && read_byte(parent,from+length-BOAROS_PAGE_SIZE)==92);
        assert(kernel_mm_vma_lookup(parent,to,&vma)==KERNEL_MM_STATUS_NOT_MAPPED);
    }
    assert(!kernel_mm_mremap(parent,from,length,length,3,to,&out) && out==to);
    assert(read_byte(parent,to)==91 && read_byte(parent,to+length-BOAROS_PAGE_SIZE)==92);
}
static struct kernel_mm moving;
static uint64_t moving_root;
static unsigned move_done;
static void *move_worker(void *unused)
{
    (void)unused; mm_test_attach(1); mm_test_context=moving_root;
    uint64_t out;
    assert(!kernel_mm_mremap(&moving,0x180000000,BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE,3,0x1c0000000,&out));
    assert(out==0x1c0000000);
    __atomic_store_n(&move_done,1,__ATOMIC_RELEASE);
    return 0;
}
static void pin_and_retirement(struct kernel_mm *parent)
{
    map(parent,0x180000000,BOAROS_PAGE_SIZE,0); write_byte(parent,0x180000000,99);
    assert(!kernel_mm_acquire(&moving,parent) && !kernel_mm_context(&moving,&moving_root));
    struct kernel_mm_user_pin pin={0};
    assert(!kernel_mm_pin_user_page(parent,0x180000000,KERNEL_MM_READ,&pin));
    unsigned parked=mm_test_parked(1);
    uint64_t available=physical_page_available(&pages);
    delay_fence=1; fence_entered=fence_resume=move_done=0;
    pthread_t thread; assert(!pthread_create(&thread,0,move_worker,0));
    while (mm_test_parked(1)==parked) sched_yield();
    assert(!move_done && !fence_entered && *(unsigned char *)pin.page==99);
    struct kernel_mm_mapping mapping;
    assert(!kernel_mm_lookup(parent,0x180000000,&mapping));
    kernel_mm_unpin_user_page(&pin);
    while (!__atomic_load_n(&fence_entered,__ATOMIC_ACQUIRE)) sched_yield();
    assert(!move_done && physical_page_available(&pages)==available-2);
    assert(kernel_mm_lookup(parent,0x180000000,&mapping)==KERNEL_MM_STATUS_NOT_MAPPED);
    assert(!kernel_mm_lookup(parent,0x1c0000000,&mapping));
    __atomic_store_n(&fence_resume,1,__ATOMIC_RELEASE);
    assert(!pthread_join(thread,0) && move_done && physical_page_available(&pages)==available);
    assert(read_byte(parent,0x1c0000000)==99 && !kernel_mm_release(&moving));
}
static struct kernel_vfs_mount *mount;
static struct kernel_open_file_description *file;
static unsigned truncate_capture, truncate_entered, truncate_resume;
void __real_kernel_rwlock_write(struct kernel_rwlock *,struct kernel_lock_guard *);
void __wrap_kernel_rwlock_write(struct kernel_rwlock *lock,struct kernel_lock_guard *guard)
{
    if (kernel_cpu_current()->hardware_id==1 && lock->rank==25 &&
        __atomic_exchange_n(&truncate_capture,0,__ATOMIC_ACQ_REL)) {
        __atomic_store_n(&truncate_entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&truncate_resume,__ATOMIC_ACQUIRE)) sched_yield();
    }
    __real_kernel_rwlock_write(lock,guard);
}
static void *truncate_worker(void *unused)
{
    (void)unused; mm_test_attach(1);
    assert(!kernel_vfs_ftruncate(&file->file,2*BOAROS_PAGE_SIZE));
    return 0;
}
static void file_cases(struct kernel_mm *parent)
{
    const uint64_t p=BOAROS_PAGE_SIZE,a=0x20000000,d=0x140000000;
    uint64_t out; int error;
    assert(!kernel_tmpfs_create(&heap,0,"nr_blocks=16",&mount));
    assert(!kernel_open_file_create_mode(&heap,mount,"/mapping",0600,&file,&error) && !error);
    file->open_flags=2;
    assert(!kernel_vfs_ftruncate(&file->file,4*p));
    struct kernel_open_file_description *owner=file;
    assert(!kernel_open_file_acquire(owner) && !kernel_mm_mmap_file_private(parent,&owner,a,3*p,p,RW,
        KERNEL_MM_MAP_FIXED|KERNEL_MM_MAP_SHARED,&out));
    write_byte(parent,a,17); write_byte(parent,a+p,27);
    owner=file;
    assert(!kernel_open_file_acquire(owner) && !kernel_mm_mmap_file_private(parent,&owner,a+8*p,2*p,p,RW,
        KERNEL_MM_MAP_FIXED,&out));
    assert(read_byte(parent,a+8*p)==17); write_byte(parent,a+9*p,37);
    assert(!kernel_mm_mremap(parent,a+p,p,3*p,3,d,&out));
    struct kernel_vma vma;
    assert(!kernel_mm_vma_lookup(parent,d,&vma) && vma.backing==file && vma.backing_offset==2*p);
    assert(read_byte(parent,d)==27 && read_byte(parent,d+p)==0);
    assert(kernel_mm_resolve_user_fault(parent,d+2*p,KERNEL_MM_READ)==KERNEL_MM_STATUS_BUS_FAULT);
    write_byte(parent,d,47); assert(read_byte(parent,a+9*p)==37);
    assert(!kernel_mm_mremap(parent,a+8*p,2*p,2*p,3,d+8*p,&out));
    assert(read_byte(parent,d+8*p)==17 && read_byte(parent,d+9*p)==37);
    truncate_capture=1; truncate_entered=truncate_resume=0;
    pthread_t truncator; assert(!pthread_create(&truncator,0,truncate_worker,0));
    while (!__atomic_load_n(&truncate_entered,__ATOMIC_ACQUIRE)) sched_yield();
    assert(!kernel_mm_mremap(parent,d,3*p,3*p,3,d+16*p,&out));
    map(parent,d,p,0); write_byte(parent,d,101);
    __atomic_store_n(&truncate_resume,1,__ATOMIC_RELEASE);
    assert(!pthread_join(truncator,0));
    assert(read_byte(parent,d)==101);
    assert(kernel_mm_resolve_user_fault(parent,d+16*p,KERNEL_MM_READ)==KERNEL_MM_STATUS_BUS_FAULT);
    assert(kernel_mm_resolve_user_fault(parent,d+9*p,KERNEL_MM_READ)==KERNEL_MM_STATUS_BUS_FAULT);
    assert(read_byte(parent,d+8*p)==17);
}
int main(void)
{
    mm_test_attach(0);
    struct boot_memory_layout layout={0}; layout.usable_count=1;
    layout.usable[0].base=(uintptr_t)pool; layout.usable[0].size=sizeof(pool);
    assert(!physical_page_allocator_init(&pages,&layout) && !physical_page_allocator_bind_access(&pages,access) && !physical_page_allocator_finalize(&pages));
    assert(!kernel_heap_init(&heap,&pages,address_of));
    struct arch_mmu_page_table kernel={0}; struct arch_mmu_user_space space={0};
    assert(!riscv_sv39_page_table_init(&kernel,&pages)); kernel.state=ARCH_MMU_STATE_ACTIVE;
    uint64_t baseline=physical_page_available(&pages);
    struct kernel_mm parent={0};
    assert(!arch_mmu_user_space_init(&space,&pages,&kernel) && !kernel_mm_create(&parent,&space) && !kernel_mm_vma_enable(&parent,&heap));
    assert(!kernel_mm_brk_initialize(&parent,0x10000,0x20000));
    assert(!kernel_mm_context(&parent,&mm_test_context));
    uint64_t from=0x40000,to=0x80000000,out=0;
    assert(!kernel_mm_mmap_anonymous(&parent,from,BOAROS_PAGE_SIZE,RW,KERNEL_MM_MAP_FIXED,&out));
    write_byte(&parent,from,73);
    struct kernel_mm_mapping before,after;
    assert(!kernel_mm_lookup(&parent,from,&before));
    assert(!kernel_mm_mremap(&parent,from,BOAROS_PAGE_SIZE,2*BOAROS_PAGE_SIZE,3,to,&out) && out==to);
    assert(!kernel_mm_lookup(&parent,to,&after) && before.physical_address==after.physical_address);
    assert(read_byte(&parent,to)==73 && read_byte(&parent,to+BOAROS_PAGE_SIZE)==0);
    assert(kernel_mm_lookup(&parent,from,&after)==KERNEL_MM_STATUS_NOT_MAPPED);
    anonymous_cases(&parent); preparation_failures(&parent); pin_and_retirement(&parent); file_cases(&parent);
    mm_test_context=0; assert(!kernel_mm_release(&parent));
    assert(!kernel_open_file_release(&file) && !kernel_vfs_unmount(mount));
    struct kernel_heap_statistics stats; kernel_heap_get_statistics(&heap,&stats);
    assert(!stats.live_allocations && !stats.current_pages && physical_page_available(&pages)==baseline);
    assert(!physical_page_allocator_audit(&pages));
    puts("mremap: move/grow/shrink/subrange, COW/PROT_NONE, shared identity, file offset/EOF/truncate, three OOM boundaries, pin drain/TLB retirement and final owners passed");
}
