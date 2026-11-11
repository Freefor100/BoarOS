#include <kernel/mm_backend.h>
#include <kernel/uaccess.h>
#include <kernel/open_file.h>
#include <kernel/errno.h>
#include <kernel/cpu.h>
#include <kernel/futex.h>
#include "../../kernel/bpf/private.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
extern _Thread_local uint64_t mm_test_context;
void mm_test_attach(unsigned);
static unsigned char pool[16*1024*1024] __attribute__((aligned(16384)));
static struct physical_page_allocator pages;
static struct kernel_heap heap;
static struct kernel_mm parent;
static unsigned entered,resume,delay_fence,fence_entered,fence_resume;
static uint64_t parent_context;
static unsigned fail_page;
enum physical_page_status __real_physical_page_allocate(struct physical_page_allocator *,uint64_t *);
enum physical_page_status __wrap_physical_page_allocate(struct physical_page_allocator *a,uint64_t *p)
{ if (fail_page && !--fail_page) return PHYSICAL_PAGE_STATUS_EMPTY;return __real_physical_page_allocate(a,p); }
static void *access_page(uint64_t p) { return (void *)(uintptr_t)p; }
static int address_of(const void *p,uint64_t *out) { *out=(uintptr_t)p;return 1; }
void mm_test_fence(uint64_t address,int instructions)
{
    (void)address;
    if (!instructions && __atomic_exchange_n(&delay_fence,0,__ATOMIC_ACQ_REL)) {
        __atomic_store_n(&fence_entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&fence_resume,__ATOMIC_ACQUIRE)) sched_yield();
    }
}
#define RW (KERNEL_MM_READ|KERNEL_MM_WRITE)
#define A UINT64_C(0x100000)
#define B UINT64_C(0x200000)
#define P BOAROS_PAGE_SIZE
static struct bpf_map_object *make_map(unsigned type,unsigned flags,unsigned entries,
    struct kernel_open_file_description **file)
{
    struct bpf_map_object *map;
    assert(!kernel_heap_allocate_zeroed(&heap,1,sizeof(*map),(void **)&map));
    bpf_object_initialize(&map->object,&heap,BPF_OBJECT_MAP);
    kernel_mutex_init(&map->mapping_lock,12,(uintptr_t)map);
    struct bpf_map_spec spec={type,type==27?0:4,type==27?0:8,entries,flags};
    assert(!bpf_map_create(&heap,&pages,&spec,&map->storage));
    assert(!bpf_object_file(&heap,&map->object,2,file));
    struct kernel_wait_queue *queue=NULL;
    uint32_t expected=type==27?0:8;
    assert(kernel_open_file_poll(*file,1,NULL)==expected);
    assert(kernel_open_file_poll(*file,1,&queue)==expected);
    assert(!!queue==(type==27));
    return map;
}
static int map_view(struct kernel_mm *mm,struct kernel_open_file_description *file,
    uint64_t address,uint64_t offset,uint64_t length,unsigned permissions)
{
    KERNEL_LOCK_SCOPE(lock);uint64_t *counter=NULL,out;
    int error=kernel_bpf_mmap_begin(file,offset,length,!!(permissions&KERNEL_MM_WRITE),&lock,&counter);
    if (error) return error;
    enum kernel_mm_status status=kernel_mm_mmap_object_shared(mm,file,address,length,offset,
        permissions,KERNEL_MM_MAP_SHARED|KERNEL_MM_MAP_FIXED,counter,&out);
    if (!status) assert(out==address);
    return status;
}
static unsigned char read_byte(struct kernel_mm *mm,uint64_t address)
{ unsigned char v;size_t n;assert(!kernel_copy_from_user(mm,&v,address,1,&n)&&n==1);return v; }
static void write_byte(struct kernel_mm *mm,uint64_t address,unsigned char v)
{ size_t n;assert(!kernel_copy_to_user(mm,address,&v,1,&n)&&n==1); }
static uint64_t physical(struct kernel_mm *mm,uint64_t a)
{ struct kernel_mm_mapping m;assert(!kernel_mm_lookup(mm,a,&m));return m.physical_address; }
static void reject_object_duplication(uint64_t address,uint64_t length)
{
    uint64_t out=0,target=UINT64_C(0x400000),page=physical(&parent,address);
    assert(kernel_mm_mremap(&parent,address,0,length,0,0,&out)==-KERNEL_EFAULT);
    for (unsigned fixed=0;fixed<=2;fixed+=2) {
        assert(kernel_mm_mremap(&parent,address,0,length,1|fixed,target,&out)==-KERNEL_EFAULT);
        assert(kernel_mm_mremap(&parent,address,length,length,5|fixed,target,&out)==-KERNEL_EINVAL);
        assert(physical(&parent,address)==page);
        struct kernel_vma vma;
        assert(kernel_mm_vma_lookup(&parent,target,&vma)==KERNEL_MM_STATUS_NOT_MAPPED);
    }
}
static void lifecycle(void)
{
    struct kernel_open_file_description *file;
    struct bpf_map_object *map=make_map(2,1024,3*P/8,&file);
    assert(map_view(&parent,file,A,0,3*P,RW)==0);
    assert(map->writable_bytes==3*P);
    reject_object_duplication(A,3*P);assert(map->writable_bytes==3*P);
    assert(map_view(&parent,file,B,0,P,KERNEL_MM_READ)==0);
    assert(kernel_mm_mprotect(&parent,B,P,RW)==KERNEL_MM_STATUS_ACCESS);
    assert(kernel_mm_mprotect(&parent,A,P,RW|KERNEL_MM_EXECUTE)==KERNEL_MM_STATUS_ACCESS);
    assert(!kernel_mm_mprotect(&parent,A+P,P,KERNEL_MM_READ));
    assert(map->writable_bytes==3*P);
    assert(!kernel_mm_mprotect(&parent,A+P,P,RW));
    uint32_t key=0;uint64_t value=77,actual=0;
    assert(!bpf_map_update(map->storage,&key,&value,0,1,0));
    assert(read_byte(&parent,A)==77 && read_byte(&parent,B)==77);
    write_byte(&parent,A,91);assert(read_byte(&parent,B)==91);
    assert(!bpf_map_lookup(map->storage,&key,&actual,1,0) && actual==91);
    assert(physical(&parent,A)==physical(&parent,B));
    struct kernel_mm_user_pin pin={0};
    assert(!kernel_mm_try_pin_user_page(&parent,A,KERNEL_MM_WRITE,&pin));
    kernel_mm_unpin_user_page(&pin);
    assert(!kernel_mm_mprotect(&parent,A,P,0));
    assert(!kernel_mm_mprotect(&parent,A,P,RW));
    assert(read_byte(&parent,A)==91);
    assert(!kernel_mm_msync(&parent,A,P,4));
    unsigned char resident;unsigned completed;
    assert(!kernel_mm_mincore(&parent,A,1,&resident,&completed) && completed==1 && resident);
    struct kernel_mm_proc_map *rows=NULL;uint32_t count;
    assert(!kernel_mm_proc_maps_snapshot(&parent,&heap,&rows,&count));
    unsigned objects=0;
    for (unsigned i=0;i<count;i++) if (rows[i].file) {
        assert(rows[i].shared && kernel_open_file_kind(rows[i].file)==KERNEL_OPEN_FILE_KIND_BPF);
        objects++;assert(!kernel_open_file_release(&rows[i].file));
    }
    assert(objects==2);kernel_heap_release(&heap,rows);
    struct kernel_mm child={0};uint64_t child_context;
    assert(!kernel_mm_fork(&child,&parent));assert(map->writable_bytes==6*P);
    assert(!kernel_mm_context(&child,&child_context));mm_test_context=child_context;
    write_byte(&child,A,92);assert(physical(&child,A)==physical(&parent,A));
    assert(read_byte(&child,A+2*P)==0); /* mmap installed even pages never accessed by the parent. */
    assert(!kernel_mm_munmap(&child,A+P,P));assert(map->writable_bytes==5*P);
    mm_test_context=parent_context;assert(!kernel_mm_release(&child));assert(map->writable_bytes==3*P);
    assert(!kernel_open_file_release(&file)); /* VMA owner keeps map storage alive after the final FD closes. */
    assert(read_byte(&parent,A+P)==0 && read_byte(&parent,A)==92);
    assert(!kernel_mm_madvise(&parent,A,P,KERNEL_MM_ADVICE_DONTNEED));
    assert(kernel_mm_resolve_user_fault(&parent,A,KERNEL_MM_READ)==KERNEL_MM_STATUS_BUS_FAULT);
    assert(read_byte(&parent,B)==92 && map->writable_bytes==3*P);
    assert(!kernel_mm_mincore(&parent,A,1,&resident,&completed) && !resident);
    assert(!kernel_mm_mprotect(&parent,A,P,0));
    assert(kernel_mm_resolve_user_fault(&parent,A,KERNEL_MM_READ)==KERNEL_MM_STATUS_ACCESS);
    assert(!kernel_mm_mprotect(&parent,A,P,RW));
    assert(kernel_mm_resolve_user_fault(&parent,A,KERNEL_MM_READ)==KERNEL_MM_STATUS_BUS_FAULT);
    child=(struct kernel_mm){0};
    assert(!kernel_mm_fork(&child,&parent));
    assert(!kernel_mm_context(&child,&child_context));mm_test_context=child_context;
    assert(kernel_mm_resolve_user_fault(&child,A,KERNEL_MM_READ)==KERNEL_MM_STATUS_BUS_FAULT);
    assert(read_byte(&child,A+P)==0 && read_byte(&child,B)==92);
    mm_test_context=parent_context;assert(!kernel_mm_release(&child));
    struct kernel_futex_key fk;uint64_t fa=A;
    assert(kernel_mm_prepare_futex(&parent,&fa,1,1,0,0,&fk,NULL)==KERNEL_MM_STATUS_ACCESS);
    assert(!kernel_mm_prepare_futex(&parent,&fa,1,0,0,0,&fk,NULL));
    uint64_t out;
    assert(kernel_mm_mremap(&parent,A,3*P,4*P,1,0,&out)==-KERNEL_EFAULT);
    assert(!kernel_mm_mremap(&parent,A,3*P,3*P,3,A+16*P,&out));
    assert(kernel_mm_resolve_user_fault(&parent,out,KERNEL_MM_READ)==KERNEL_MM_STATUS_BUS_FAULT);
    assert(read_byte(&parent,out+P)==0 && map->writable_bytes==3*P);
    assert(!kernel_mm_munmap(&parent,out,3*P));assert(!map->writable_bytes);
    assert(!bpf_map_freeze(map->storage));
    assert(map_view(&parent,(struct kernel_open_file_description *)0,A,0,P,RW)==-KERNEL_ENODEV);
    assert(!kernel_mm_munmap(&parent,B,P));
    bpf_object_put(&map->object);
}
static void permissions_and_ring(void)
{
    struct kernel_open_file_description *f;
    struct bpf_map_object *m=make_map(2,1024|128,1,&f);
    assert(map_view(&parent,f,A,0,P,RW)==-KERNEL_EACCES);
    assert(!map_view(&parent,f,A,0,P,KERNEL_MM_READ));
    assert(map_view(&parent,f,B,P,P,KERNEL_MM_READ)==-KERNEL_EINVAL);
    assert(map_view(&parent,f,B,1,P,KERNEL_MM_READ)==-KERNEL_EINVAL);
    assert(!kernel_mm_munmap(&parent,A,P));assert(!kernel_open_file_release(&f));bpf_object_put(&m->object);
    m=make_map(2,1024,1,&f);assert(!bpf_map_freeze(m->storage));
    assert(map_view(&parent,f,A,0,P,RW)==-KERNEL_EPERM);
    assert(!map_view(&parent,f,A,0,P,KERNEL_MM_READ));assert(!kernel_mm_munmap(&parent,A,P));
    assert(!kernel_open_file_release(&f));bpf_object_put(&m->object);
    m=make_map(27,0,2*P,&f);
    assert(map_view(&parent,f,A,P,P,RW)==-KERNEL_EPERM);
    assert(map_view(&parent,f,A,0,2*P,RW)==-KERNEL_EPERM);
    assert(!map_view(&parent,f,A,0,P,RW));
    assert(!map_view(&parent,f,B,P,5*P,KERNEL_MM_READ));
    reject_object_duplication(A,P);reject_object_duplication(B,5*P);
    uint64_t value=123;
    assert(!bpf_ring_output(bpf_map_ring(m->storage),&value,8,0));
    assert(kernel_open_file_poll(f,1,NULL)==1);
    assert(read_byte(&parent,B+P+8)==123 && read_byte(&parent,B+3*P+8)==123);
    assert(physical(&parent,B+P)==physical(&parent,B+3*P));
    assert(!kernel_mm_madvise(&parent,B+P,P,KERNEL_MM_ADVICE_DONTNEED));
    assert(kernel_mm_resolve_user_fault(&parent,B+P,KERNEL_MM_READ)==KERNEL_MM_STATUS_BUS_FAULT);
    assert(read_byte(&parent,B+3*P+8)==123);
    assert(!kernel_mm_madvise(&parent,B,5*P,KERNEL_MM_ADVICE_DONTNEED));
    assert(kernel_mm_resolve_user_fault(&parent,B+3*P,KERNEL_MM_READ)==KERNEL_MM_STATUS_BUS_FAULT);
    assert(!map_view(&parent,f,B,P,5*P,KERNEL_MM_READ));
    assert(read_byte(&parent,B+3*P+8)==123);
    assert(!kernel_mm_munmap(&parent,A,P));assert(!kernel_mm_munmap(&parent,B,5*P));
    assert(!kernel_open_file_release(&f));bpf_object_put(&m->object);
}
static void *unmap_worker(void *unused)
{ (void)unused;mm_test_attach(1);mm_test_context=parent_context;assert(!kernel_mm_munmap(&parent,A,P));return NULL; }
static void *admission_worker(void *argument)
{
    struct kernel_open_file_description *pin=argument;
    mm_test_attach(1);mm_test_context=parent_context;
    KERNEL_LOCK_SCOPE(admission);uint64_t *counter=NULL,out;
    assert(!kernel_bpf_mmap_begin(pin,0,P,1,&admission,&counter));
    assert(!kernel_mm_mmap_object_shared(&parent,pin,A,P,0,RW,
        KERNEL_MM_MAP_SHARED|KERNEL_MM_MAP_FIXED,counter,&out));
    __atomic_store_n(&entered,1,__ATOMIC_RELEASE);
    while (!__atomic_load_n(&resume,__ATOMIC_ACQUIRE)) sched_yield();
    /* The VMA and FD were removed by the other actor. The calling pin still
     * owns the map whose admission mutex this thread is about to release. */
    assert(bpf_object_from_file(pin)->references==1);
    kernel_lock_release(&admission);
    assert(!kernel_open_file_release(&pin));
    return NULL;
}
static void concurrency(void)
{
    struct kernel_open_file_description *f;
    struct bpf_map_object *m=make_map(2,1024,1,&f);
    assert(!kernel_open_file_acquire(f)); /* independent syscall pin */
    entered=resume=0;pthread_t admission;
    assert(!pthread_create(&admission,NULL,admission_worker,f));
    while (!__atomic_load_n(&entered,__ATOMIC_ACQUIRE)) sched_yield();
    assert(!kernel_mm_munmap(&parent,A,P));
    assert(!kernel_open_file_release(&f));bpf_object_put(&m->object);
    __atomic_store_n(&resume,1,__ATOMIC_RELEASE);assert(!pthread_join(admission,NULL));
    /* mmap allocation failure removes every partially installed page and VMA. */
    m=make_map(2,1024,2*P/8,&f);
    for (unsigned fail=1;fail<=3;fail++) {
        uint64_t before=physical_page_available(&pages);
        fail_page=fail;
        assert(map_view(&parent,f,B-P,0,2*P,RW)==KERNEL_MM_STATUS_NO_MEMORY);
        assert(!fail_page && !m->writable_bytes);
        assert(kernel_mm_resolve_user_fault(&parent,B-P,KERNEL_MM_READ)==KERNEL_MM_STATUS_NOT_MAPPED);
        assert(kernel_mm_resolve_user_fault(&parent,B,KERNEL_MM_READ)==KERNEL_MM_STATUS_NOT_MAPPED);
        assert(physical_page_available(&pages)==before);
    }
    assert(!map_view(&parent,f,A,0,P,0));
    assert(kernel_mm_resolve_user_fault(&parent,A,KERNEL_MM_READ)==KERNEL_MM_STATUS_ACCESS);
    assert(!kernel_mm_mprotect(&parent,A,P,KERNEL_MM_READ));
    assert(read_byte(&parent,A)==0);
    assert(!kernel_mm_munmap(&parent,A,P));
    assert(!kernel_open_file_release(&f));bpf_object_put(&m->object);
    pthread_t worker;
    m=make_map(2,1024,1,&f);assert(!map_view(&parent,f,A,0,P,RW));write_byte(&parent,A,5);
    uint64_t page=physical(&parent,A);uint32_t before,after;
    assert(!physical_page_reference_count(&pages,page,&before));
    delay_fence=1;fence_entered=fence_resume=0;assert(!pthread_create(&worker,NULL,unmap_worker,NULL));
    while (!__atomic_load_n(&fence_entered,__ATOMIC_ACQUIRE)) sched_yield();
    assert(!physical_page_reference_count(&pages,page,&after) && before==after);
    __atomic_store_n(&fence_resume,1,__ATOMIC_RELEASE);assert(!pthread_join(worker,NULL));
    assert(!physical_page_reference_count(&pages,page,&after) && after==before-1);
    assert(!kernel_open_file_release(&f));bpf_object_put(&m->object);
}
int main(void)
{
    mm_test_attach(0);
    struct boot_memory_layout layout={0};layout.usable_count=1;
    layout.usable[0].base=(uintptr_t)pool;layout.usable[0].size=sizeof(pool);
    assert(!physical_page_allocator_init(&pages,&layout));assert(!physical_page_allocator_bind_access(&pages,access_page));
    assert(!physical_page_allocator_finalize(&pages));assert(!kernel_heap_init(&heap,&pages,address_of));
    struct arch_mmu_page_table kernel={0};struct arch_mmu_user_space space={0};
    assert(!riscv_sv39_page_table_init(&kernel,&pages));kernel.state=ARCH_MMU_STATE_ACTIVE;
    uint64_t baseline=physical_page_available(&pages);
    assert(!arch_mmu_user_space_init(&space,&pages,&kernel) && !kernel_mm_create(&parent,&space) && !kernel_mm_vma_enable(&parent,&heap));
    assert(!kernel_mm_brk_initialize(&parent,0x10000,0x20000));
    assert(!kernel_mm_mmap_base_initialize(&parent,0x80000000));
    assert(!kernel_mm_context(&parent,&parent_context));mm_test_context=parent_context;
    lifecycle();permissions_and_ring();concurrency();
    mm_test_context=0;assert(!kernel_mm_release(&parent));
    struct kernel_heap_statistics stats;kernel_heap_get_statistics(&heap,&stats);
    assert(!stats.live_allocations && !stats.current_pages && physical_page_available(&pages)==baseline);
    assert(!physical_page_allocator_audit(&pages));
    puts("BPF mmap: actual ARRAY/RING aliases, permissions/freeze counters, fork/close, DONTNEED/remap, mmap rollback/PROT_NONE and TLB retirement passed");
}
