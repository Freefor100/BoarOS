/* 宿主只替代CSR/RFENCE与任务绑定；真实Sv39/MM/VFS/OFD/缓存/后端共用。 */
#include <kernel/mm_backend.h>
#include <kernel/uaccess.h>
#include <kernel/open_file.h>
#include "../../fs/open_file_internal.h"
extern _Thread_local uint64_t mm_test_context;
void mm_test_bind(struct kernel_mm *);
#include <ext4.h>
static _Thread_local unsigned disk_fail_page,disk_fail_heap;
enum physical_page_status __real_physical_page_allocate(struct physical_page_allocator *,uint64_t *);
enum physical_page_status __wrap_physical_page_allocate(struct physical_page_allocator *pages,uint64_t *out)
{
    if (disk_fail_page && !--disk_fail_page) return PHYSICAL_PAGE_STATUS_EMPTY;
    return __real_physical_page_allocate(pages,out);
}
enum kernel_heap_status __real_kernel_heap_allocate_zeroed(struct kernel_heap *,size_t,size_t,void **);
enum kernel_heap_status __wrap_kernel_heap_allocate_zeroed(struct kernel_heap *heap,size_t count,size_t size,void **out)
{
    if (disk_fail_heap && !--disk_fail_heap) return KERNEL_HEAP_STATUS_EMPTY;
    return __real_kernel_heap_allocate_zeroed(heap,count,size,out);
}
static unsigned disk_gap_capture,disk_gap_ready,disk_gap_resume;
enum kernel_mm_status __real_kernel_mm_resolve_user_fault(struct kernel_mm *,uint64_t,uint32_t);
enum kernel_mm_status __wrap_kernel_mm_resolve_user_fault(struct kernel_mm *mm,uint64_t address,uint32_t access)
{
    enum kernel_mm_status result=__real_kernel_mm_resolve_user_fault(mm,address,access);
    if (!result && access==KERNEL_MM_WRITE && kernel_cpu_current()->hardware_id==1 &&
        __atomic_exchange_n(&disk_gap_capture,0,__ATOMIC_ACQ_REL)) {
        assert(!kernel_io_context_current()->locks && !kernel_cpu_current()->raw_locks);
        __atomic_store_n(&disk_gap_ready,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&disk_gap_resume,__ATOMIC_ACQUIRE)) sched_yield();
    }
    return result;
}
static unsigned disk_truncate_error;
int __real_ext4_ftruncate(ext4_file *,uint64_t);
int __wrap_ext4_ftruncate(ext4_file *file,uint64_t size)
{
    unsigned error=disk_truncate_error; disk_truncate_error=0;
    if (error==1) return 5;
    if (error==2) {
        int result=__real_ext4_ftruncate(file,size+123);
        assert(!result); return 5;
    }
    return __real_ext4_ftruncate(file,size);
}
static struct kernel_mm *disk_views[5];
static uint64_t disk_roots[5],disk_target;
static unsigned disk_capture,disk_entered[5],disk_resume;
static enum kernel_uaccess_status disk_results[5];
int __real_kernel_open_file_prepare_mapping_page(struct kernel_open_file_description *,uint64_t,
    struct kernel_open_file_page_preparation *);
int __wrap_kernel_open_file_prepare_mapping_page(struct kernel_open_file_description *file,uint64_t index,
    struct kernel_open_file_page_preparation *prepared)
{
    int result=__real_kernel_open_file_prepare_mapping_page(file,index,prepared);
    unsigned id=kernel_cpu_current()->hardware_id;
    if (!result && id<5 && (__atomic_fetch_and(&disk_capture,~(1U<<id),__ATOMIC_ACQ_REL)&(1U<<id))) {
        assert(!kernel_io_context_current()->locks && !kernel_cpu_current()->raw_locks);
        __atomic_store_n(&disk_entered[id],1,__ATOMIC_RELEASE);
        while (!(__atomic_load_n(&disk_resume,__ATOMIC_ACQUIRE)&(1U<<id))) sched_yield();
    }
    return result;
}
static void *disk_fault_actor(void *arg)
{
    unsigned id=(unsigned)(uintptr_t)arg; cache_test_attach(id);
    mm_test_bind(disk_views[id]); mm_test_context=disk_roots[id];
    unsigned char byte=0x80+id; size_t done;
    disk_results[id]=kernel_copy_to_user(disk_views[id],disk_target+id,&byte,1,&done);
    assert(done==(disk_results[id]==KERNEL_UACCESS_STATUS_OK?1U:0U));
    mm_test_bind(0); return 0;
}
static unsigned char disk_read(struct kernel_mm *mm,uint64_t address)
{
    unsigned char byte=0; size_t done;
    assert(!kernel_copy_from_user(mm,&byte,address,1,&done) && done==1);
    return byte;
}
static unsigned disk_pin_ready,disk_pin_resume;
static int disk_sync_result;
static uint64_t disk_old_page;
static void *disk_retiring_writer(void *unused)
{
    (void)unused; cache_test_attach(1); mm_test_bind(disk_views[1]); mm_test_context=disk_roots[1];
    struct kernel_mm_user_pin pin={0};
    assert(!kernel_mm_resolve_user_fault(disk_views[1],disk_target,KERNEL_MM_WRITE));
    assert(!kernel_mm_pin_user_page(disk_views[1],disk_target,KERNEL_MM_WRITE,&pin));
    disk_old_page=pin.physical_address;
    __atomic_store_n(&disk_pin_ready,1,__ATOMIC_RELEASE);
    while (!__atomic_load_n(&disk_pin_resume,__ATOMIC_ACQUIRE)) sched_yield();
    /* 撤映射只结束PTE owner；旧pin仍可写旧页，但不能进入新文件代次。 */
    *(unsigned char *)pin.page=0xdd;
    kernel_mm_unpin_user_page(&pin); mm_test_bind(0); return 0;
}
static void *disk_retiring_sync(void *arg)
{
    cache_test_attach(0); uint64_t observed=0;
    disk_sync_result=kernel_vfs_sync(arg,0,&observed);
    return 0;
}
static void disk_cohort(struct kernel_mm *parent,struct kernel_mm *child,uint64_t address,unsigned actors)
{
    struct kernel_mm views[5]={0};
    memset(disk_entered,0,sizeof(disk_entered)); disk_resume=0;
    disk_capture=(1U<<(actors+1))-2; disk_target=address;
    pthread_t threads[4];
    for (unsigned id=1;id<=actors;id++) {
        assert(!kernel_mm_acquire(&views[id],id&1?parent:child));
        disk_views[id]=&views[id]; assert(!kernel_mm_context(&views[id],&disk_roots[id]));
        assert(!pthread_create(&threads[id-1],0,disk_fault_actor,(void *)(uintptr_t)id));
    }
    for (unsigned id=1;id<=actors;id++)
        while (!__atomic_load_n(&disk_entered[id],__ATOMIC_ACQUIRE)) sched_yield();
    __atomic_store_n(&disk_resume,UINT32_MAX,__ATOMIC_RELEASE);
    for (unsigned id=1;id<=actors;id++) {
        assert(!pthread_join(threads[id-1],0) && !disk_results[id]);
        assert(!kernel_mm_release(&views[id]));
        assert(disk_read(parent,address+id)==0x80+id);
    }
    struct kernel_mm_mapping a,b;
    assert(!kernel_mm_lookup(parent,address,&a) && !kernel_mm_lookup(child,address,&b) &&
        a.physical_address==b.physical_address);
}
static void disk_mm_case(struct kernel_page_cache *cache,struct kernel_heap *heap,
    struct physical_page_allocator *pages,struct kernel_vfs_mount *mount,const char *name)
{
    struct arch_mmu_page_table kernel={0};
    struct arch_mmu_user_space space={0}; struct kernel_mm parent={0},child={0};
    assert(!riscv_sv39_page_table_init(&kernel,pages)); kernel.state=ARCH_MMU_STATE_ACTIVE;
    assert(!arch_mmu_user_space_init(&space,pages,&kernel) && !kernel_mm_create(&parent,&space) && !kernel_mm_vma_enable(&parent,heap));
    assert(!kernel_mm_context(&parent,&mm_test_context));
    assert(!kernel_mm_brk_initialize(&parent,0x10000,0x20000));
    struct kernel_open_file_description *controller=0;
    int error; assert(!kernel_open_file_create(heap,mount,name,&controller,&error) && !error);
    controller->open_flags=2;
    struct kernel_open_file_description *source=controller;
    uint64_t shared;
    assert(!kernel_open_file_acquire(source));
    enum kernel_mm_status mapped=kernel_mm_mmap_file_private(&parent,&source,
        0x40000,2*BOAROS_PAGE_SIZE,0,KERNEL_MM_READ|KERNEL_MM_WRITE,
        KERNEL_MM_MAP_FIXED|KERNEL_MM_MAP_SHARED,&shared);
    if (mapped) fprintf(stderr,"disk map status=%d mode=%o flags=%u context=%llu\n",mapped,controller->file.mode,controller->open_flags,(unsigned long long)mm_test_context);
    assert(!mapped);
    assert(kernel_mm_enable_concurrency(&parent)==KERNEL_MM_STATUS_OK && "real ext4 cache domain must be admitted");
    assert(!kernel_mm_fork(&child,&parent));
    assert(!kernel_mm_context(&parent,&mm_test_context)); mm_test_bind(&parent);
    unsigned char byte=0x5a; size_t done;
    assert(!kernel_copy_to_user(&parent,shared,&byte,1,&done) && done==1);
    assert(!kernel_mm_context(&child,&mm_test_context)); mm_test_bind(&child);
    byte=0; assert(!kernel_copy_from_user(&child,&byte,shared,1,&done) && done==1 && byte==0x5a);
    struct kernel_mm_mapping p,c;
    assert(!kernel_mm_lookup(&parent,shared,&p) && !kernel_mm_lookup(&child,shared,&c) && p.physical_address==c.physical_address);
    mm_test_bind(0); assert(!kernel_mm_context(&parent,&mm_test_context));
    /* 新页在不同MM同时准备：worker只读一次，最终PTE共享同一页。 */
    disk_cohort(&parent,&child,shared+BOAROS_PAGE_SIZE,2);
    assert(!kernel_mm_munmap(&parent,shared+BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE));
    assert(!kernel_mm_context(&child,&mm_test_context));
    assert(!kernel_mm_munmap(&child,shared+BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE));
    for (unsigned n=0;n<2;n++) {
        assert(!kernel_mm_context(n?&child:&parent,&mm_test_context));
        struct kernel_open_file_description *owner=controller; uint64_t mapped;
        assert(!kernel_open_file_acquire(owner));
        assert(!kernel_mm_mmap_file_private(n?&child:&parent,&owner,shared+BOAROS_PAGE_SIZE,
            BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE,
            KERNEL_MM_MAP_FIXED|KERNEL_MM_MAP_SHARED,&mapped));
    }
    assert(!kernel_mm_context(&parent,&mm_test_context));
    disk_cohort(&parent,&child,shared+BOAROS_PAGE_SIZE,4);
    /* 旧来源结果准备完成后缩短：不能重新发布EOF以外的PTE。 */
    assert(!kernel_mm_context(&child,&mm_test_context));
    assert(!kernel_mm_munmap(&child,shared+BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE));
    source=controller; assert(!kernel_open_file_acquire(source));
    assert(!kernel_mm_mmap_file_private(&child,&source,shared+BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE,
        BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED|KERNEL_MM_MAP_SHARED,&shared));
    disk_target=0x40000+BOAROS_PAGE_SIZE; disk_views[1]=&child;
    assert(!kernel_mm_context(&child,&disk_roots[1]));
    disk_capture=2; disk_resume=0; disk_entered[1]=0;
    pthread_t stale; assert(!pthread_create(&stale,0,disk_fault_actor,(void *)(uintptr_t)1));
    while (!__atomic_load_n(&disk_entered[1],__ATOMIC_ACQUIRE)) sched_yield();
    assert(!kernel_vfs_ftruncate(&controller->file,BOAROS_PAGE_SIZE));
    __atomic_store_n(&disk_resume,2,__ATOMIC_RELEASE);
    assert(!pthread_join(stale,0) && disk_results[1]==KERNEL_UACCESS_STATUS_FAULT);
    assert(!kernel_vfs_ftruncate(&controller->file,2*BOAROS_PAGE_SIZE));
    assert(!kernel_mm_context(&parent,&mm_test_context));
    assert(disk_read(&parent,0x40000+BOAROS_PAGE_SIZE)==0);
    uint64_t private;
    source=controller; assert(!kernel_open_file_acquire(source));
    assert(!kernel_mm_mmap_file_private(&parent,&source,0x80000,BOAROS_PAGE_SIZE,0,
        KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED,&private));
    assert(disk_read(&parent,private)==0x5a);
    byte=0x36; assert(!kernel_copy_to_user(&parent,private,&byte,1,&done));
    assert(disk_read(&parent,0x40000)==0x5a);
    byte=0x5c; assert(!kernel_copy_to_user(&parent,0x40000+BOAROS_PAGE_SIZE+10,&byte,1,&done));
    disk_truncate_error=1;
    assert(kernel_vfs_ftruncate(&controller->file,BOAROS_PAGE_SIZE)==-KERNEL_EIO);
    assert(kernel_open_file_size(controller)==2*BOAROS_PAGE_SIZE && disk_read(&parent,0x40000+BOAROS_PAGE_SIZE+10)==0x5c);
    disk_truncate_error=2;
    assert(kernel_vfs_ftruncate(&controller->file,BOAROS_PAGE_SIZE)==-KERNEL_EIO);
    assert(kernel_open_file_size(controller)==BOAROS_PAGE_SIZE+123 && disk_read(&parent,0x40000+BOAROS_PAGE_SIZE+10)==0x5c);
    assert(!kernel_vfs_ftruncate(&controller->file,2*BOAROS_PAGE_SIZE));
    assert(disk_read(&parent,0x40000+BOAROS_PAGE_SIZE+123)==0);
    source=controller; assert(!kernel_open_file_acquire(source));
    uint64_t cold_private;
    assert(!kernel_mm_mmap_file_private(&parent,&source,0x40000000,BOAROS_PAGE_SIZE,0,
        KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED,&cold_private));
    for (unsigned fail=1;fail<=3;fail++) {
        disk_fail_page=fail;
        assert(kernel_copy_to_user(&parent,cold_private,&byte,1,&done)==KERNEL_UACCESS_STATUS_FAULT && !done && !disk_fail_page);
        struct kernel_mm_mapping absent;
        assert(kernel_mm_lookup(&parent,cold_private,&absent)==KERNEL_MM_STATUS_NOT_MAPPED);
    }
    disk_fail_heap=1;
    assert(kernel_copy_to_user(&parent,cold_private,&byte,1,&done)==KERNEL_UACCESS_STATUS_FAULT && !done && !disk_fail_heap);
    /* 独立来源owner跨准备存活；fixed替换只能提交到新匿名VMA。 */
    disk_views[1]=&parent; assert(!kernel_mm_context(&parent,&disk_roots[1]));
    disk_target=cold_private; disk_capture=2; disk_resume=0; disk_entered[1]=0;
    assert(!pthread_create(&stale,0,disk_fault_actor,(void *)(uintptr_t)1));
    while (!__atomic_load_n(&disk_entered[1],__ATOMIC_ACQUIRE)) sched_yield();
    uint64_t replacement;
    assert(!kernel_mm_mmap_anonymous(&parent,cold_private,BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE,
        KERNEL_MM_MAP_FIXED,&replacement));
    __atomic_store_n(&disk_resume,2,__ATOMIC_RELEASE);
    assert(!pthread_join(stale,0) && !disk_results[1] && disk_read(&parent,cold_private+1)==0x81);
    assert(!kernel_mm_msync(&parent,0x40000,2*BOAROS_PAGE_SIZE,4));
    for (unsigned revoke=0;revoke<2;revoke++) {
        assert(!kernel_mm_msync(&parent,0x40000,BOAROS_PAGE_SIZE,4));
        disk_gap_capture=1; disk_gap_ready=disk_gap_resume=0;
        disk_target=0x40000; disk_views[1]=&parent; assert(!kernel_mm_context(&parent,&disk_roots[1]));
        assert(!pthread_create(&stale,0,disk_fault_actor,(void *)(uintptr_t)1));
        while (!__atomic_load_n(&disk_gap_ready,__ATOMIC_ACQUIRE)) sched_yield();
        if (revoke) assert(!kernel_mm_mprotect(&parent,0x40000,BOAROS_PAGE_SIZE,KERNEL_MM_READ));
        else assert(!kernel_mm_msync(&parent,0x40000,BOAROS_PAGE_SIZE,4));
        struct kernel_mm_mapping leaf;
        assert(!kernel_mm_lookup(&parent,0x40000,&leaf) && !(leaf.permissions&KERNEL_MM_WRITE));
        __atomic_store_n(&disk_gap_resume,1,__ATOMIC_RELEASE);
        assert(!pthread_join(stale,0));
        assert(disk_results[1]==(revoke?KERNEL_UACCESS_STATUS_FAULT:KERNEL_UACCESS_STATUS_OK));
        if (revoke) assert(!kernel_mm_mprotect(&parent,0x40000,BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE));
    }
    puts("PASS fault-to-pin: snapshot revoke retries; actual VMA revoke faults");
    /* 前一组权限准备可能把页0标脏；本组只检查截掉页1时旧pin的退休，
     * 先同步保留页，避免完整fsync合法地将两页绑定成同一批次。 */
    assert(!kernel_mm_msync(&parent,0x40000,BOAROS_PAGE_SIZE,4));
    disk_views[1]=&parent; assert(!kernel_mm_context(&parent,&disk_roots[1]));
    disk_target=0x40000+BOAROS_PAGE_SIZE; disk_pin_ready=disk_pin_resume=0;
    pthread_t writer,sync;
    assert(!pthread_create(&writer,0,disk_retiring_writer,0));
    while (!__atomic_load_n(&disk_pin_ready,__ATOMIC_ACQUIRE)) sched_yield();
    assert(!pthread_create(&sync,0,disk_retiring_sync,&controller->file));
    for (;;) {
        struct kernel_mm_mapping leaf;
        assert(!kernel_mm_lookup(&parent,disk_target,&leaf));
        if (!(leaf.permissions&KERNEL_MM_WRITE)) break;
        sched_yield();
    }
    /* 快照已撤W并等待旧pin；截断不用等待这份已退休的写回业务。 */
    assert(!kernel_vfs_ftruncate(&controller->file,BOAROS_PAGE_SIZE));
    uint32_t references;
    assert(!physical_page_reference_count(pages,disk_old_page,&references) && references>=2);
    assert(!kernel_vfs_ftruncate(&controller->file,2*BOAROS_PAGE_SIZE));
    __atomic_store_n(&disk_pin_resume,1,__ATOMIC_RELEASE);
    assert(!pthread_join(writer,0) && !pthread_join(sync,0) && !disk_sync_result);
    assert(disk_read(&parent,disk_target)==0 && !kernel_mm_msync(&parent,0x40000,2*BOAROS_PAGE_SIZE,4));
    puts("PASS truncate during snapshot drain: retained pin/entry, no stale writeback after growth");
    struct kernel_page_cache_concurrency_state state; kernel_page_cache_get_concurrency_state(cache,&state);
    assert(!state.pending && !state.running);
    mm_test_bind(0); mm_test_context=0;
    assert(!kernel_mm_release(&child) && !kernel_mm_release(&parent));
    assert(!kernel_open_file_release(&controller));
    assert(!physical_page_release(pages,kernel.root_address));
    puts("PASS real ext4 MM: source domain, fork sharing, physical identity, owner return");
}
