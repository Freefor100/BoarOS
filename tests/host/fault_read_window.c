/* Demand file faults borrow the existing cache batch loader. */
#define main cache_full_main
#define __wrap_kernel_heap_allocate_zeroed window_base_allocate_zeroed
#include "page_cache_concurrency.c"
#undef main
#undef __wrap_kernel_heap_allocate_zeroed
#include <kernel/elf64_source.h>

static _Thread_local unsigned window_allocate_actor,window_allocate_count;
static unsigned window_allocate_entered,window_allocate_resume;
static _Thread_local unsigned window_pause_after_get;
static unsigned window_get_entered,window_get_resume;
static uint64_t window_get_address;
static _Thread_local unsigned window_fail_nosleep,window_nosleep_failures;
static _Thread_local struct {
    struct physical_page_allocator *pages;
    uint64_t *borrowed;
    size_t count,capacity;
    unsigned armed,triggered;
} window_oom_order;
static void window_borrow_after_optional(void)
{
    if (!window_oom_order.armed) return;
    window_oom_order.armed=0;window_oom_order.triggered++;
    assert(!kernel_cpu_current()->raw_locks);
    for (;;) {
        uint64_t address;
        enum physical_page_status status=physical_page_allocate_order_nosleep(window_oom_order.pages,0,&address);
        if (status==PHYSICAL_PAGE_STATUS_EMPTY) break;
        assert(!status && window_oom_order.count<window_oom_order.capacity);
        window_oom_order.borrowed[window_oom_order.count++]=address;
    }
    assert(window_oom_order.count && !physical_page_available(window_oom_order.pages));
    printf("fault-around OOM order: first optional metadata succeeded; borrowed=%zu actual_free=0\n",
        window_oom_order.count);
}
enum kernel_heap_status __real_kernel_heap_allocate_nosleep(struct kernel_heap *,size_t,void **);
enum kernel_heap_status __real_kernel_heap_allocate_zeroed_nosleep(struct kernel_heap *,size_t,size_t,void **);
enum kernel_heap_status __wrap_kernel_heap_allocate_nosleep(struct kernel_heap *heap,size_t bytes,void **out)
{
    if (window_fail_nosleep) {
        window_nosleep_failures++;
        return KERNEL_HEAP_STATUS_EMPTY;
    }
    enum kernel_heap_status status=__real_kernel_heap_allocate_nosleep(heap,bytes,out);
    if (!status) window_borrow_after_optional();
    return status;
}
enum kernel_heap_status __wrap_kernel_heap_allocate_zeroed_nosleep(struct kernel_heap *heap,
    size_t count,size_t size,void **out)
{
    if (window_fail_nosleep) {
        window_nosleep_failures++;
        return KERNEL_HEAP_STATUS_EMPTY;
    }
    enum kernel_heap_status status=__real_kernel_heap_allocate_zeroed_nosleep(heap,count,size,out);
    if (!status) window_borrow_after_optional();
    return status;
}
enum kernel_page_cache_status __real_kernel_page_cache_get_for_fault(struct kernel_page_cache *,
    const struct kernel_vfs_file *,uint64_t,uint64_t *,size_t *,int,int);
enum kernel_page_cache_status __wrap_kernel_page_cache_get_for_fault(struct kernel_page_cache *cache,
    const struct kernel_vfs_file *file,uint64_t index,uint64_t *address,size_t *valid,int window,int no_reuse)
{
    enum kernel_page_cache_status status=__real_kernel_page_cache_get_for_fault(cache,file,index,address,valid,window,no_reuse);
    if (!status && window_pause_after_get) {
        window_pause_after_get=0;window_get_address=*address;
        assert(!kernel_io_context_current()->locks && !kernel_cpu_current()->raw_locks);
        __atomic_store_n(&window_get_entered,1,__ATOMIC_RELEASE);
        wait_progress(&window_get_resume,1);
    }
    return status;
}
enum kernel_heap_status __wrap_kernel_heap_allocate_zeroed(struct kernel_heap *heap,
    size_t count,size_t size,void **out)
{
    if (window_allocate_actor && current_cache && heap==current_cache->heap) {
        struct kernel_page_cache_concurrency_state state;
        kernel_page_cache_get_concurrency_state(current_cache,&state);
        if (count*size==state.entry_bytes && ++window_allocate_count==2) {
            __atomic_store_n(&window_allocate_entered,1,__ATOMIC_RELEASE);
            wait_progress(&window_allocate_resume,1);
        }
    }
    return window_base_allocate_zeroed(heap,count,size,out);
}

static void init_window_heap(struct physical_page_allocator *allocator,
    struct kernel_heap *heap,void *bytes,size_t length)
{
    struct boot_memory_layout layout={.usable_count=1,.usable={{(uintptr_t)bytes,length}}};
    assert(!physical_page_allocator_init(allocator,&layout) &&
        !physical_page_allocator_bind_access(allocator,access_page) &&
        !physical_page_allocator_finalize(allocator) && !kernel_heap_init(heap,allocator,page_address));
}

static void finish_window(struct kernel_page_cache *cache,struct kernel_open_file_description *file,
    struct kernel_open_file_page_preparation *prepared)
{
    uint64_t address=prepared->address;uint32_t references;
    assert(!physical_page_reference_count(cache->allocator,address,&references) && references==2);
    kernel_open_file_finish_mapping_page(file,prepared,0);
    assert(!physical_page_reference_count(cache->allocator,address,&references) && references==1);
    demand_idle(cache,0);
}

static void cold_window(struct kernel_page_cache *cache,struct kernel_open_file_description *file)
{
    assert(!kernel_page_cache_invalidate_node(cache,kernel_open_file_node(file)));
    reset_demand_source();
    cache_test_attach(1);
    struct kernel_open_file_page_preparation prepared={0};
    assert(!kernel_open_file_prepare_mapping_page(file,0,1,&prepared));
    unsigned expected=BOAROS_PAGE_CACHE_READAHEAD_PAGES;
    if (!expected) expected=1;
    printf("fault cold window: backend calls=%u pages=%u expected=%u\n",
        source_batches,source_reads,expected);
    assert(source_batches==1 && source_reads==expected && source_batch_sizes[0]==expected && source_first_cpu==1);
    assert(*(unsigned char *)access_page(prepared.address)==0x30);
    kernel_open_file_finish_mapping_page(file,&prepared,0);
    unsigned reads=source_reads;
    uint64_t available=physical_page_available(cache->allocator);
    size_t allocations=0;demand_heap_allocations=&allocations;
    assert(!kernel_open_file_prepare_mapping_page(file,0,1,&prepared));
    demand_heap_allocations=NULL;
    assert(!allocations && source_reads==reads && physical_page_available(cache->allocator)==available);
    kernel_open_file_finish_mapping_page(file,&prepared,0);
    demand_idle(cache,0);
    puts("PASS fault cold batch and allocation-free pure hit; target and neighbour owners returned");
    cache_test_attach(0);
}

static void reset_window(struct kernel_page_cache *cache,struct kernel_open_file_description *file)
{
    assert(!kernel_page_cache_invalidate_node(cache,kernel_open_file_node(file)));
    reset_demand_source();
}

static void window_errors(struct kernel_page_cache *cache,struct kernel_open_file_description *file)
{
    struct kernel_open_file_page_preparation prepared={0};
    reset_window(cache,file);
    assert(!kernel_open_file_prepare_mapping_page(file,0,0,&prepared));
    finish_window(cache,file,&prepared);
    reset_window(cache,file);source_fault_index=1;source_fault_error=-KERNEL_EIO;
    assert(!kernel_open_file_prepare_mapping_page(file,0,1,&prepared));
    assert(*(unsigned char *)access_page(prepared.address)==0x30);
    kernel_open_file_finish_mapping_page(file,&prepared,0);
    uint64_t address;size_t valid;
    assert(kernel_page_cache_lookup(cache,&file->file,1,&address,&valid)!=KERNEL_PAGE_CACHE_STATUS_OK);
    source_fault_error=0;
    assert(!kernel_open_file_prepare_mapping_page(file,1,0,&prepared));
    assert(*(unsigned char *)access_page(prepared.address)==0x31);
    kernel_open_file_finish_mapping_page(file,&prepared,0);
    reset_window(cache,file);source_fault_index=0;source_fault_error=-KERNEL_EIO;
    assert(kernel_open_file_prepare_mapping_page(file,0,1,&prepared)==-KERNEL_EIO && !prepared.pending);
    reset_window(cache,file);source_error=-KERNEL_EIO;
    assert(kernel_open_file_prepare_mapping_page(file,0,1,&prepared)==-KERNEL_EIO && !prepared.pending);
    reset_window(cache,file);source_error=-KERNEL_ENOMEM;
    assert(kernel_open_file_prepare_mapping_page(file,0,1,&prepared)==-KERNEL_ENOMEM && !prepared.pending);
    reset_window(cache,file);fail_workspace_allocation=1;
    assert(!kernel_open_file_prepare_mapping_page(file,0,1,&prepared) && !fail_workspace_allocation);
    assert(source_reads==1);
    kernel_open_file_finish_mapping_page(file,&prepared,0);
    for (unsigned point=1;point<=BOAROS_PAGE_CACHE_READAHEAD_PAGES+3U;point++) {
        reset_window(cache,file);disk_fail_page=point;
        assert(!kernel_open_file_prepare_mapping_page(file,0,1,&prepared));
        disk_fail_page=0;
        assert(*(unsigned char *)access_page(prepared.address)==0x30);
        finish_window(cache,file,&prepared);
    }
    for (unsigned point=1;point<=BOAROS_PAGE_CACHE_READAHEAD_PAGES+1U;point++) {
        reset_window(cache,file);disk_fail_heap=point;
        assert(!kernel_open_file_prepare_mapping_page(file,0,1,&prepared));
        disk_fail_heap=0;
        finish_window(cache,file,&prepared);
    }
    puts("PASS fault window EIO: bad neighbour stays local, target/global errors propagate; workspace/page/entry OOM preserves scalar demand");
}

static void window_policy(struct kernel_page_cache *cache,struct kernel_open_file_description *file)
{
    struct kernel_vfs_stat before,after;file->offset=777;
    assert(!kernel_vfs_fstat(&file->file,&before));
    const unsigned advice[]={1,5,0};
    for (unsigned i=0;i<3;i++) {
        assert(!kernel_open_file_advise(file,0,0,advice[i],0));
        reset_window(cache,file);
        struct kernel_open_file_page_preparation prepared={0};
        assert(!kernel_open_file_prepare_mapping_page(file,0,1,&prepared));
        unsigned expected=i<2?1:BOAROS_PAGE_CACHE_READAHEAD_PAGES;
        assert(source_reads==(expected?expected:1));
        kernel_open_file_finish_mapping_page(file,&prepared,0);
    }
    assert(file->offset==777 && !kernel_vfs_fstat(&file->file,&after));
    assert(!memcmp(&before.atime,&after.atime,sizeof(before.atime)));
    puts("PASS fault FADV_RANDOM/NOREUSE suppress neighbours; NORMAL restores; offset and atime retained");
}

struct window_call {
    struct kernel_open_file_description *file;
    uint64_t index;
    unsigned actor,window,done,race,post_get;
    int error;
    struct kernel_open_file_page_preparation prepared;
};
static void *window_reader(void *argument)
{
    struct window_call *call=argument;cache_test_attach(call->actor);
    window_allocate_actor=call->race;window_allocate_count=0;
    window_pause_after_get=call->post_get;
    call->error=kernel_open_file_prepare_mapping_page(call->file,call->index,call->window,&call->prepared);
    window_allocate_actor=0;
    __atomic_store_n(&call->done,1,__ATOMIC_RELEASE);return NULL;
}

static void window_neighbour_loading(struct kernel_page_cache *cache,struct kernel_open_file_description *file)
{
    if (BOAROS_PAGE_CACHE_READAHEAD_PAGES<2) return;
    for (unsigned race=0;race<2;race++) {
        reset_window(cache,file);window_allocate_entered=window_allocate_resume=0;
        struct window_call target={.file=file,.actor=1,.window=1,.race=race};
        struct window_call neighbour={.file=file,.index=1,.actor=2};
        pthread_t demand,other;
        if (race) {
            assert(!pthread_create(&demand,NULL,window_reader,&target));
            wait_progress(&window_allocate_entered,1);
        }
        pause_read=1;
        assert(!pthread_create(&other,NULL,window_reader,&neighbour));wait_progress(&read_entered,1);
        if (race) __atomic_store_n(&window_allocate_resume,1,__ATOMIC_RELEASE);
        else assert(!pthread_create(&demand,NULL,window_reader,&target));
        wait_progress(&target.done,1);
        assert(!target.error && !__atomic_load_n(&neighbour.done,__ATOMIC_ACQUIRE) && source_pages[1]==1);
        assert(!pthread_join(demand,NULL));
        uint64_t address=target.prepared.address;uint32_t references;
        assert(!physical_page_reference_count(cache->allocator,address,&references) && references==2);
        kernel_open_file_finish_mapping_page(file,&target.prepared,0);
        assert(!physical_page_reference_count(cache->allocator,address,&references) && references==1);
        __atomic_store_n(&resume_read,1,__ATOMIC_RELEASE);
        assert(!pthread_join(other,NULL) && !neighbour.error);
        finish_window(cache,file,&neighbour.prepared);
    }
    puts("PASS speculative neighbour never borrows/waits/promotes an existing producer, including publication race");
}
unsigned mm_test_parked(unsigned);
static void await_window_park(unsigned actor,unsigned before)
{
    struct timespec start,now;assert(!clock_gettime(CLOCK_MONOTONIC,&start));
    while (mm_test_parked(actor)==before) {
        assert(!clock_gettime(CLOCK_MONOTONIC,&now) && now.tv_sec-start.tv_sec<5);
        sched_yield();
    }
}
static void window_loading(struct kernel_page_cache *cache,struct kernel_open_file_description *file)
{
    reset_window(cache,file);pause_read=1;
    struct window_call first={.file=file,.actor=1},waiting={.file=file,.actor=2,.window=1};
    pthread_t producer,reader;
    assert(!pthread_create(&producer,NULL,window_reader,&first));wait_progress(&read_entered,1);
    unsigned parked=mm_test_parked(2);
    assert(!pthread_create(&reader,NULL,window_reader,&waiting));await_window_park(2,parked);
    assert(!__atomic_load_n(&waiting.done,__ATOMIC_ACQUIRE) && source_reads==1);
    __atomic_store_n(&resume_read,1,__ATOMIC_RELEASE);
    assert(!pthread_join(producer,NULL) && !pthread_join(reader,NULL) && !first.error && !waiting.error);
    assert(first.prepared.address==waiting.prepared.address && source_reads==1);
    kernel_open_file_finish_mapping_page(file,&first.prepared,0);
    kernel_open_file_finish_mapping_page(file,&waiting.prepared,0);
    demand_idle(cache,0);
    puts("PASS loading target waits on the existing producer without expanding a window; both page owners returned");
}

static void window_stop(struct kernel_page_cache *cache,struct kernel_open_file_description *file)
{
    reset_window(cache,file);pause_read=1;stop_done=0;
    struct window_call call={.file=file,.index=16,.actor=1,.window=1};pthread_t producer;
    assert(!pthread_create(&producer,NULL,window_reader,&call));wait_progress(&read_entered,1);
    kernel_page_cache_stop_worker(cache);
    assert(!__atomic_load_n(&call.done,__ATOMIC_ACQUIRE));
    struct kernel_open_file_page_preparation independent={0};
    unsigned reads=source_reads;
    assert(!kernel_open_file_prepare_mapping_page(file,0,1,&independent) && source_reads==reads+1);
    kernel_open_file_finish_mapping_page(file,&independent,0);
    __atomic_store_n(&resume_read,1,__ATOMIC_RELEASE);
    assert(!pthread_join(producer,NULL) && !call.error);
    kernel_open_file_finish_mapping_page(file,&call.prepared,0);demand_idle(cache,0);
    assert(!kernel_page_cache_start_worker(cache));
    puts("PASS caller-owned fault window survives worker stop; stopped cache keeps independent demand scalar; restart and owners return");
}

static void window_truncate(struct kernel_page_cache *cache,struct kernel_open_file_description *file)
{
    reset_window(cache,file);window_get_entered=window_get_resume=0;
    struct window_call call={.file=file,.index=3,.actor=1,.window=1,.post_get=1};pthread_t producer;
    struct kernel_vfs_node *node=kernel_open_file_node(file);
    assert(!kernel_file_mapping_prepare(node));
    uint64_t before_epoch,epoch;assert(!kernel_file_mapping_epoch(node,&before_epoch));
    assert(!pthread_create(&producer,NULL,window_reader,&call));wait_progress(&window_get_entered,1);
    uint32_t references;
    assert(!physical_page_reference_count(cache->allocator,window_get_address,&references) && references==2);
    assert(!kernel_vfs_ftruncate(&file->file,2*BOAROS_PAGE_SIZE+37));
    assert(!physical_page_reference_count(cache->allocator,window_get_address,&references) && references==1);
    uint64_t available=physical_page_available(cache->allocator);
    __atomic_store_n(&window_get_resume,1,__ATOMIC_RELEASE);
    assert(!pthread_join(producer,NULL));
    assert(!kernel_file_mapping_epoch(node,&epoch));
    printf("truncate post-get handshake: error=%d pending=%u original_epoch=%llu final_epoch=%llu final_size=%llu\n",
        call.error,call.prepared.pending,(unsigned long long)before_epoch,(unsigned long long)epoch,
        (unsigned long long)kernel_open_file_size(file));
    assert(!call.error && call.prepared.pending && epoch!=before_epoch &&
        physical_page_available(cache->allocator)==available);
    /* The borrowed old page is valid storage, but the MM must reject the old
     * inode epoch before publication. Rejection returns that exact last owner. */
    kernel_open_file_finish_mapping_page(file,&call.prepared,0);
    assert(!call.prepared.pending && physical_page_available(cache->allocator)==available+1);
    struct kernel_open_file_page_preparation prepared={0};
    assert(kernel_open_file_prepare_mapping_page(file,3,1,&prepared)==-KERNEL_EFAULT);
    reset_window(cache,file);
    assert(!kernel_open_file_prepare_mapping_page(file,2,1,&prepared) && source_reads==1);
    const unsigned char *bytes=access_page(prepared.address);
    for (size_t i=0;i<37;i++) assert(bytes[i]==0x32);
    for (size_t i=37;i<BOAROS_PAGE_SIZE;i++) assert(!bytes[i]);
    kernel_open_file_finish_mapping_page(file,&prepared,0);demand_idle(cache,0);
    puts("PASS truncate races a cold window: caller rejects the old epoch and returns its source; new EOF bounds the window and zeroes its tail");
}

struct window_mm {
    struct arch_mmu_page_table kernel;
    struct arch_mmu_user_space space;
    struct kernel_mm mm;
    uint64_t context;
};
static void window_mm_create(struct window_mm *view,struct kernel_heap *heap,
    struct physical_page_allocator *pages)
{
    assert(!riscv_sv39_page_table_init(&view->kernel,pages));view->kernel.state=ARCH_MMU_STATE_ACTIVE;
    assert(!arch_mmu_user_space_init(&view->space,pages,&view->kernel) && !kernel_mm_create(&view->mm,&view->space));
    assert(!kernel_mm_vma_enable(&view->mm,heap) && !kernel_mm_brk_initialize(&view->mm,0x10000,0x20000));
    assert(!kernel_mm_context(&view->mm,&view->context));
}
static void window_mm_close(struct window_mm *view,struct physical_page_allocator *pages)
{
    mm_test_bind(NULL);mm_test_context=0;
    assert(!kernel_mm_release(&view->mm) && !physical_page_release(pages,view->kernel.root_address));
}
static void window_map(struct window_mm *view,struct kernel_open_file_description *file,
    uint64_t address,uint64_t length)
{
    struct kernel_open_file_description *source=file;uint64_t mapped;
    assert(!kernel_open_file_acquire(source));
    assert(!kernel_mm_mmap_file_private(&view->mm,&source,address,length,0,KERNEL_MM_READ,
        KERNEL_MM_MAP_FIXED,&mapped) && !source && mapped==address);
}
static void expect_random(struct kernel_mm *mm,uint64_t address,int random)
{
    struct kernel_vma vma;assert(!kernel_mm_vma_lookup(mm,address,&vma));
    assert(!!(vma.flags&KERNEL_VMA_RANDOM_READ)==random);
}
static unsigned char window_mm_read(struct kernel_mm *mm,uint64_t context,uint64_t address)
{
    mm_test_bind(mm);mm_test_context=context;
    unsigned char value=0;size_t copied;
    assert(!kernel_copy_from_user(mm,&value,address,1,&copied) && copied==1);
    mm_test_bind(NULL);mm_test_context=0;return value;
}
static void window_mm_faultaround(struct kernel_page_cache *cache,struct kernel_heap *heap,
    struct physical_page_allocator *pages,struct kernel_open_file_description *file)
{
    if (BOAROS_PAGE_CACHE_READAHEAD_PAGES<3) return;
    const uint64_t base=0x400000,p=BOAROS_PAGE_SIZE;
    reset_window(cache,file);struct window_mm view={0};window_mm_create(&view,heap,pages);
    window_map(&view,file,base,3*p);
    assert(!kernel_mm_mprotect(&view.mm,base,3*p,KERNEL_MM_READ|KERNEL_MM_WRITE));
    assert(window_mm_read(&view.mm,view.context,base)==0x30);
    uint64_t cached[3];
    for (unsigned i=0;i<3;i++) {
        struct kernel_mm_mapping mapping;size_t valid;uint32_t references;
        enum kernel_mm_status status=kernel_mm_lookup(&view.mm,base+i*p,&mapping);
        printf("fault-around first read: page=%u lookup=%u\n",i,status);
        assert(!status && (mapping.permissions&(KERNEL_MM_READ|KERNEL_MM_WRITE|KERNEL_MM_EXECUTE))==KERNEL_MM_READ);
        assert(!kernel_page_cache_lookup(cache,&file->file,i,&cached[i],&valid) && valid==p &&
            mapping.physical_address==cached[i]);
        assert(!physical_page_release(pages,cached[i]));
        assert(!physical_page_reference_count(pages,cached[i],&references) && references==2);
        const unsigned char *bytes=access_page(cached[i]);
        for (size_t j=0;j<p;j++) assert(bytes[j]==0x30+i);
    }
    struct kernel_mm_mapping mapping;
    assert(kernel_mm_lookup(&view.mm,base+3*p,&mapping)==KERNEL_MM_STATUS_NOT_MAPPED);
    unsigned reads=source_reads;unsigned char byte=0xe7;size_t copied;
    mm_test_bind(&view.mm);mm_test_context=view.context;
    assert(!kernel_copy_to_user(&view.mm,base+p,&byte,1,&copied) && copied==1);
    mm_test_bind(NULL);mm_test_context=0;
    assert(!kernel_mm_lookup(&view.mm,base+p,&mapping) &&
        mapping.physical_address!=cached[1] && (mapping.permissions&KERNEL_MM_WRITE));
    assert(window_mm_read(&view.mm,view.context,base+p)==byte && source_reads==reads &&
        *(unsigned char *)access_page(cached[1])==0x31);
    assert(!kernel_mm_munmap(&view.mm,base,3*p));
    for (unsigned i=0;i<3;i++) {
        uint32_t references;
        assert(kernel_mm_lookup(&view.mm,base+i*p,&mapping)==KERNEL_MM_STATUS_NOT_MAPPED);
        assert(!physical_page_reference_count(pages,cached[i],&references) && references==1);
    }
    window_mm_close(&view,pages);demand_idle(cache,0);
    puts("PASS first private read maps READY neighbours read-only with real COW; VMA bound, cache bytes and final owners retained");
}
static void window_ready_pair(struct kernel_page_cache *cache,struct kernel_open_file_description *file)
{
    reset_window(cache,file);
    for (unsigned i=0;i<2;i++) {
        struct kernel_open_file_page_preparation prepared={0};
        assert(!kernel_open_file_prepare_mapping_page(file,i,0,&prepared));
        kernel_open_file_finish_mapping_page(file,&prepared,0);
    }
    assert(source_reads==2);
}
struct window_hot_lookup {
    struct kernel_page_cache *cache;
    struct kernel_open_file_description *file;
    uint64_t address;
    size_t valid;
    unsigned entered,start,done;
    int resident;
    enum kernel_page_cache_status status;
};
static void *window_hot_lookup_actor(void *argument)
{
    struct window_hot_lookup *call=argument;cache_test_attach(1);
    __atomic_store_n(&call->entered,1,__ATOMIC_RELEASE);wait_progress(&call->start,1);
    call->resident=kernel_page_cache_resident(call->cache,&call->file->file,0);
    call->status=kernel_page_cache_lookup(call->cache,&call->file->file,0,&call->address,&call->valid);
    __atomic_store_n(&call->done,1,__ATOMIC_RELEASE);return NULL;
}
static int window_hot_lookup(struct kernel_page_cache *cache,struct kernel_open_file_description *file)
{
    window_ready_pair(cache,file);
    struct kernel_page_cache_concurrency_state state;
    observe_cache_metadata=1;kernel_page_cache_get_concurrency_state(cache,&state);observe_cache_metadata=0;
    assert(cache_metadata_lock);
    struct window_hot_lookup call={.cache=cache,.file=file};pthread_t reader;
    assert(!pthread_create(&reader,NULL,window_hot_lookup_actor,&call));wait_progress(&call.entered,1);
    unsigned parked=mm_test_parked(1);
    KERNEL_LOCK_SCOPE(global);kernel_rwlock_write(cache_metadata_lock,&global);
    __atomic_store_n(&call.start,1,__ATOMIC_RELEASE);
    struct timespec start,now;assert(!clock_gettime(CLOCK_MONOTONIC,&start));
    while (!__atomic_load_n(&call.done,__ATOMIC_ACQUIRE) && mm_test_parked(1)==parked) {
        assert(!clock_gettime(CLOCK_MONOTONIC,&now) && now.tv_sec-start.tv_sec<5);sched_yield();
    }
    int progressed=__atomic_load_n(&call.done,__ATOMIC_ACQUIRE);
    kernel_lock_release(&global);
    assert(!pthread_join(reader,NULL));
    printf("hot lookup holding global mutex: completed_before_release=%u resident=%d status=%u\n",
        progressed,call.resident,call.status);
    assert(call.resident && !call.status && call.valid==BOAROS_PAGE_SIZE && source_reads==2);
    uint32_t references;
    assert(!physical_page_reference_count(cache->allocator,call.address,&references) && references==2);
    const unsigned char *bytes=access_page(call.address);
    for (size_t i=0;i<call.valid;i++) assert(bytes[i]==0x30);
    assert(!physical_page_release(cache->allocator,call.address) &&
        !physical_page_reference_count(cache->allocator,call.address,&references) && references==1);
    demand_idle(cache,0);
    if (progressed) puts("PASS READY resident and lookup complete while another actor holds cache-global metadata; real page owner and contents retained");
    return !progressed;
}
static int window_mm_faultaround_oom_order(struct kernel_page_cache *cache,struct kernel_heap *heap,
    struct physical_page_allocator *pages,struct kernel_open_file_description *file)
{
    const uint64_t base=0x400000,p=BOAROS_PAGE_SIZE;
    window_ready_pair(cache,file);
    /* Keep both cache entries borrowed: physical exhaustion must not turn
     * into eviction of the two READY inputs and a different I/O workload. */
    struct kernel_page_cache_read_page held[2]={0};
    assert(!kernel_page_cache_get_batch(cache,&file->file,0,held,2,0));
    for (unsigned i=0;i<2;i++) assert(!held[i].status && held[i].entry && held[i].valid_bytes==p);
    struct window_mm view={0};window_mm_create(&view,heap,pages);window_map(&view,file,base,2*p);
    struct kernel_mm_mapping mapping;
    assert(kernel_mm_lookup(&view.mm,base,&mapping)==KERNEL_MM_STATUS_NOT_MAPPED);
    window_oom_order.pages=pages;window_oom_order.count=window_oom_order.triggered=0;
    window_oom_order.capacity=physical_page_available(pages);
    window_oom_order.borrowed=malloc(window_oom_order.capacity*sizeof(*window_oom_order.borrowed));
    assert(window_oom_order.borrowed);window_oom_order.armed=1;
    mm_test_bind(&view.mm);mm_test_context=view.context;
    unsigned char value=0;size_t copied=0;
    enum kernel_uaccess_status status=kernel_copy_from_user(&view.mm,&value,base,1,&copied);
    mm_test_bind(NULL);mm_test_context=0;window_oom_order.armed=0;
    assert(window_oom_order.triggered==1 && window_oom_order.count);
    int failed=status!=KERNEL_UACCESS_STATUS_OK || copied!=1 || value!=0x30;
    printf("fault-around OOM order: target_status=%u copied=%zu value=%u borrowed=%zu\n",
        status,copied,value,window_oom_order.count);
    if (!failed) assert(!kernel_mm_lookup(&view.mm,base,&mapping) &&
        mapping.physical_address==held[0].physical_address && !(mapping.permissions&KERNEL_MM_WRITE));
    for (size_t i=0;i<window_oom_order.count;i++)
        assert(!physical_page_release(pages,window_oom_order.borrowed[i]));
    free(window_oom_order.borrowed);window_oom_order.borrowed=NULL;
    window_mm_close(&view,pages);
    for (unsigned i=0;i<2;i++) {
        uint32_t references;
        assert(!physical_page_reference_count(pages,held[i].physical_address,&references) && references==1);
        const unsigned char *bytes=kernel_page_cache_read_data(&held[i]);
        for (size_t j=0;j<p;j++) assert(bytes[j]==0x30+i);
    }
    kernel_page_cache_put_batch(cache,held,2);demand_idle(cache,0);
    if (!failed) puts("PASS optional metadata cannot consume the target's required page-table/hash resources; all borrowed pages and PTE owners returned");
    return failed;
}
static void window_mm_faultaround_policy(struct kernel_page_cache *cache,struct kernel_heap *heap,
    struct physical_page_allocator *pages,struct kernel_open_file_description *file)
{
    const uint64_t base=0x400000,p=BOAROS_PAGE_SIZE;
    for (unsigned policy=0;policy<3;policy++) {
        assert(!kernel_open_file_advise(file,0,0,0,0));window_ready_pair(cache,file);
        struct window_mm view={0};window_mm_create(&view,heap,pages);window_map(&view,file,base,2*p);
        if (!policy) assert(!kernel_mm_madvise(&view.mm,base,2*p,KERNEL_MM_ADVICE_RANDOM));
        else assert(!kernel_open_file_advise(file,0,0,policy==1?1:5,0));
        assert(window_mm_read(&view.mm,view.context,base)==0x30 && source_reads==2);
        struct kernel_mm_mapping mapping;
        assert(!kernel_mm_lookup(&view.mm,base,&mapping) &&
            !(mapping.permissions&KERNEL_MM_WRITE));
        assert(kernel_mm_lookup(&view.mm,base+p,&mapping)==KERNEL_MM_STATUS_NOT_MAPPED);
        window_mm_close(&view,pages);demand_idle(cache,0);
    }
    assert(!kernel_open_file_advise(file,0,0,0,0));
    puts("PASS MADV_RANDOM and FADV_RANDOM/NOREUSE suppress PTE neighbours even when their cache pages are READY");
}
static void window_mm_faultaround_existing(struct kernel_page_cache *cache,struct kernel_heap *heap,
    struct physical_page_allocator *pages,struct kernel_open_file_description *file)
{
    if (BOAROS_PAGE_CACHE_READAHEAD_PAGES<3) return;
    const uint64_t base=0x400000,p=BOAROS_PAGE_SIZE;
    reset_window(cache,file);struct window_mm view={0};window_mm_create(&view,heap,pages);
    window_map(&view,file,base,3*p);
    struct kernel_open_file_page_preparation prepared={0};
    assert(!kernel_open_file_prepare_mapping_page(file,0,0,&prepared));
    kernel_open_file_finish_mapping_page(file,&prepared,0);
    assert(!kernel_mm_mprotect(&view.mm,base,3*p,KERNEL_MM_READ|KERNEL_MM_WRITE));
    unsigned char byte=0xc9;size_t copied;
    mm_test_bind(&view.mm);mm_test_context=view.context;
    assert(!kernel_copy_to_user(&view.mm,base+p,&byte,1,&copied) && copied==1);
    mm_test_bind(NULL);mm_test_context=0;
    struct kernel_mm_mapping before,after;
    assert(!kernel_mm_lookup(&view.mm,base+p,&before) && (before.permissions&KERNEL_MM_WRITE));
    assert(kernel_mm_lookup(&view.mm,base,&after)==KERNEL_MM_STATUS_NOT_MAPPED &&
        kernel_mm_lookup(&view.mm,base+2*p,&after)==KERNEL_MM_STATUS_NOT_MAPPED);
    unsigned reads=source_reads;
    assert(window_mm_read(&view.mm,view.context,base)==0x30 && source_reads==reads);
    assert(!kernel_mm_lookup(&view.mm,base+p,&after) && after.physical_address==before.physical_address &&
        after.permissions==before.permissions && window_mm_read(&view.mm,view.context,base+p)==byte);
    assert(!kernel_mm_lookup(&view.mm,base+2*p,&after) && !(after.permissions&KERNEL_MM_WRITE));
    window_mm_close(&view,pages);demand_idle(cache,0);
    reset_window(cache,file);memset(&view,0,sizeof(view));window_mm_create(&view,heap,pages);
    window_map(&view,file,base,3*p);
    assert(!kernel_open_file_prepare_mapping_page(file,0,0,&prepared));
    kernel_open_file_finish_mapping_page(file,&prepared,0);
    assert(!kernel_mm_mprotect(&view.mm,base,3*p,KERNEL_MM_READ|KERNEL_MM_WRITE));
    mm_test_bind(&view.mm);mm_test_context=view.context;
    assert(!kernel_copy_to_user(&view.mm,base+p,&byte,1,&copied) && copied==1);
    mm_test_bind(NULL);mm_test_context=0;
    assert(!kernel_mm_lookup(&view.mm,base+p,&before));
    assert(!kernel_mm_mprotect(&view.mm,base+p,p,0));
    reads=source_reads;
    assert(window_mm_read(&view.mm,view.context,base)==0x30 && source_reads==reads);
    assert(kernel_mm_lookup(&view.mm,base+p,&after)==KERNEL_MM_STATUS_NOT_MAPPED &&
        kernel_mm_lookup(&view.mm,base+2*p,&after)==KERNEL_MM_STATUS_NOT_MAPPED);
    assert(!kernel_mm_mprotect(&view.mm,base+p,p,KERNEL_MM_READ|KERNEL_MM_WRITE));
    assert(!kernel_mm_lookup(&view.mm,base+p,&after) && after.physical_address==before.physical_address &&
        window_mm_read(&view.mm,view.context,base+p)==byte && source_reads==reads);
    window_mm_close(&view,pages);demand_idle(cache,0);
    puts("PASS private write faults stay scalar; read neighbours preserve private and PROT_NONE leaves and stop at VMA boundaries");
}
static void window_mm_faultaround_oom(struct kernel_page_cache *cache,struct kernel_heap *heap,
    struct physical_page_allocator *pages,struct kernel_open_file_description *file)
{
    const uint64_t base=0x400000,p=BOAROS_PAGE_SIZE;
    window_ready_pair(cache,file);struct window_mm view={0};window_mm_create(&view,heap,pages);
    window_map(&view,file,base,2*p);window_nosleep_failures=0;window_fail_nosleep=1;
    assert(window_mm_read(&view.mm,view.context,base)==0x30 && source_reads==2);
    window_fail_nosleep=0;
    struct kernel_mm_mapping mapping;
    assert(window_nosleep_failures && !kernel_mm_lookup(&view.mm,base,&mapping) &&
        kernel_mm_lookup(&view.mm,base+p,&mapping)==KERNEL_MM_STATUS_NOT_MAPPED);
    window_mm_close(&view,pages);demand_idle(cache,0);
    puts("PASS optional resident metadata OOM preserves the successful target and returns every neighbour owner");
}
static void window_mm_faultaround_bounds(struct kernel_page_cache *cache,struct kernel_heap *heap,
    struct physical_page_allocator *pages,struct kernel_open_file_description *file)
{
    const uint64_t base=0x400000,p=BOAROS_PAGE_SIZE;
    const unsigned last=65536U/BOAROS_PAGE_SIZE-1;
    if (BOAROS_PAGE_CACHE_READAHEAD_PAGES<2) return;
    reset_window(cache,file);struct window_mm view={0};window_mm_create(&view,heap,pages);
    window_map(&view,file,base,32*p);
    struct kernel_open_file_page_preparation prepared={0};
    assert(!kernel_open_file_prepare_mapping_page(file,last-1,0,&prepared));
    kernel_open_file_finish_mapping_page(file,&prepared,0);
    assert(window_mm_read(&view.mm,view.context,base+last*p)==0x30+last);
    struct kernel_mm_mapping mapping;uint64_t next;size_t valid;
    assert(!kernel_page_cache_lookup(cache,&file->file,last+1,&next,&valid) && valid==p);
    assert(!physical_page_release(pages,next));
    assert(!kernel_mm_lookup(&view.mm,base+(last-1)*p,&mapping) &&
        !(mapping.permissions&KERNEL_MM_WRITE) &&
        window_mm_read(&view.mm,view.context,base+(last-1)*p)==0x30+last-1);
    assert(kernel_mm_lookup(&view.mm,base+(last+1)*p,&mapping)==KERNEL_MM_STATUS_NOT_MAPPED);
    window_mm_close(&view,pages);demand_idle(cache,0);
    puts("PASS READY pages before the target in its natural 64 KiB window map readonly; the next window stays unmapped");
}
struct window_mm_read_call { struct window_mm *view; uint64_t address; unsigned done; unsigned char value; };
static void *window_mm_read_actor(void *argument)
{
    struct window_mm_read_call *call=argument;cache_test_attach(2);
    call->value=window_mm_read(&call->view->mm,call->view->context,call->address);
    __atomic_store_n(&call->done,1,__ATOMIC_RELEASE);return NULL;
}
static void window_mm_faultaround_loading(struct kernel_page_cache *cache,struct kernel_heap *heap,
    struct physical_page_allocator *pages,struct kernel_open_file_description *file)
{
    const uint64_t base=0x400000,p=BOAROS_PAGE_SIZE;
    reset_window(cache,file);struct kernel_open_file_page_preparation prepared={0};
    assert(!kernel_open_file_prepare_mapping_page(file,0,0,&prepared));
    kernel_open_file_finish_mapping_page(file,&prepared,0);
    struct window_mm view={0};window_mm_create(&view,heap,pages);window_map(&view,file,base,2*p);
    pause_read=1;struct window_call neighbour={.file=file,.index=1,.actor=1};pthread_t producer,reader;
    assert(!pthread_create(&producer,NULL,window_reader,&neighbour));wait_progress(&read_entered,1);
    struct window_mm_read_call target={.view=&view,.address=base};
    assert(!pthread_create(&reader,NULL,window_mm_read_actor,&target));wait_progress(&target.done,1);
    assert(!pthread_join(reader,NULL) && target.value==0x30 && source_reads==2 &&
        !__atomic_load_n(&neighbour.done,__ATOMIC_ACQUIRE));
    struct kernel_mm_mapping mapping;
    assert(kernel_mm_lookup(&view.mm,base+p,&mapping)==KERNEL_MM_STATUS_NOT_MAPPED);
    __atomic_store_n(&resume_read,1,__ATOMIC_RELEASE);
    assert(!pthread_join(producer,NULL) && !neighbour.error);
    finish_window(cache,file,&neighbour.prepared);
    window_mm_close(&view,pages);demand_idle(cache,0);
    puts("PASS real private target fault returns while the neighbour producer remains paused in I/O; no neighbour PTE or owner borrowed");
}
static void window_mm_faultaround_truncate(struct kernel_page_cache *cache,struct kernel_heap *heap,
    struct physical_page_allocator *pages,struct kernel_open_file_description *file)
{
    if (BOAROS_PAGE_CACHE_READAHEAD_PAGES<3) return;
    const uint64_t base=0x400000,p=BOAROS_PAGE_SIZE;
    reset_window(cache,file);struct window_mm view={0};window_mm_create(&view,heap,pages);
    window_map(&view,file,base,4*p);
    assert(window_mm_read(&view.mm,view.context,base)==0x30);
    struct kernel_mm_mapping mapping;uint64_t held;size_t valid;uint32_t references;
    assert(!kernel_mm_lookup(&view.mm,base+2*p,&mapping));
    assert(!kernel_page_cache_lookup(cache,&file->file,2,&held,&valid) && valid==37 &&
        mapping.physical_address==held);
    assert(!physical_page_reference_count(pages,held,&references) && references==3);
    const unsigned char *bytes=access_page(held);
    for (size_t i=0;i<37;i++) assert(bytes[i]==0x32);
    for (size_t i=37;i<p;i++) assert(!bytes[i]);
    assert(kernel_mm_lookup(&view.mm,base+3*p,&mapping)==KERNEL_MM_STATUS_NOT_MAPPED);
    assert(!kernel_vfs_ftruncate(&file->file,2*p));
    assert(kernel_mm_lookup(&view.mm,base+2*p,&mapping)==KERNEL_MM_STATUS_NOT_MAPPED &&
        !physical_page_reference_count(pages,held,&references) && references==1);
    assert(!physical_page_release(pages,held));
    assert(window_mm_read(&view.mm,view.context,base+p)==0x31);
    mm_test_bind(&view.mm);mm_test_context=view.context;
    unsigned char value;size_t copied;
    assert(kernel_copy_from_user(&view.mm,&value,base+2*p,1,&copied)==KERNEL_UACCESS_STATUS_FAULT && !copied);
    mm_test_bind(NULL);mm_test_context=0;
    window_mm_close(&view,pages);demand_idle(cache,0);
    puts("PASS EOF bounds READY neighbour PTEs and zero tail; truncate withdraws a mapped neighbour and its cache/PTE owners");
}
static void window_mm_hints(struct kernel_page_cache *cache,struct kernel_heap *heap,
    struct physical_page_allocator *pages,struct kernel_open_file_description *file)
{
    const uint64_t base=0x400000,moved=0x800000,p=BOAROS_PAGE_SIZE;
    reset_window(cache,file);struct window_mm parent={0};window_mm_create(&parent,heap,pages);
    window_map(&parent,file,base,6*p);
    assert(!kernel_mm_madvise(&parent.mm,base+p,2*p,KERNEL_MM_ADVICE_RANDOM));
    expect_random(&parent.mm,base,0);expect_random(&parent.mm,base+p,1);expect_random(&parent.mm,base+3*p,0);
    struct kernel_mm child={0};uint64_t child_context;
    assert(!kernel_mm_fork(&child,&parent.mm) && !kernel_mm_context(&child,&child_context));
    expect_random(&child,base+p,1);
    uint64_t result;
    assert(!kernel_mm_mremap(&parent.mm,base+p,2*p,2*p,3,moved,&result) && result==moved);
    expect_random(&parent.mm,moved,1);
    assert(!kernel_mm_madvise(&child,base+p,p,KERNEL_MM_ADVICE_NORMAL));
    expect_random(&child,base+p,0);expect_random(&child,base+2*p,1);expect_random(&parent.mm,moved,1);
    reset_demand_source();
    assert(window_mm_read(&child,child_context,base+2*p)==0x32 && source_reads==1);
    assert(!kernel_mm_release(&child));
    reset_window(cache,file);
    assert(!kernel_mm_madvise(&parent.mm,moved,2*p,KERNEL_MM_ADVICE_SEQUENTIAL));
    expect_random(&parent.mm,moved,0);
    unsigned expected=BOAROS_PAGE_CACHE_READAHEAD_PAGES?BOAROS_PAGE_CACHE_READAHEAD_PAGES:1;
    assert(window_mm_read(&parent.mm,parent.context,moved)==0x31 && source_reads==expected);
    assert(!kernel_mm_madvise(&parent.mm,moved,p,KERNEL_MM_ADVICE_RANDOM));
    assert(!kernel_mm_madvise(&parent.mm,moved,p,KERNEL_MM_ADVICE_NORMAL));expect_random(&parent.mm,moved,0);
    window_mm_close(&parent,pages);demand_idle(cache,0);
    puts("PASS MADV_RANDOM is range-local across split/fork/mremap; NORMAL/SEQUENTIAL clear it and real mmap faults obey the hint");
}

struct cancel_window {
    struct window_mm *view;
    uint64_t address,length;
    unsigned entered,done;
    enum kernel_uaccess_status status;
};
static void *cancel_window_read(void *argument)
{
    struct cancel_window *call=argument;cache_test_attach(1);
    mm_test_bind(&call->view->mm);mm_test_context=call->view->context;
    unsigned char value;size_t copied;
    call->status=kernel_copy_from_user(&call->view->mm,&value,call->address,1,&copied);
    assert(!copied);mm_test_bind(NULL);mm_test_context=0;
    return NULL;
}
static void *cancel_window_unmap(void *argument)
{
    struct cancel_window *call=argument;cache_test_attach(2);
    __atomic_store_n(&call->entered,1,__ATOMIC_RELEASE);
    assert(!kernel_mm_munmap(&call->view->mm,call->address,call->length));
    __atomic_store_n(&call->done,1,__ATOMIC_RELEASE);return NULL;
}
static void window_mm_cancel(struct kernel_page_cache *cache,struct kernel_heap *heap,
    struct physical_page_allocator *pages,struct kernel_open_file_description *file)
{
    reset_window(cache,file);struct window_mm view={0};window_mm_create(&view,heap,pages);
    struct cancel_window call={.view=&view,.address=0x400000,.length=4*BOAROS_PAGE_SIZE};
    window_map(&view,file,call.address,call.length);reset_demand_source();pause_read=1;
    pthread_t reader,unmapper;
    assert(!pthread_create(&reader,NULL,cancel_window_read,&call));wait_progress(&read_entered,1);
    assert(!pthread_create(&unmapper,NULL,cancel_window_unmap,&call));wait_progress(&call.entered,1);
    struct timespec start,now;assert(!clock_gettime(CLOCK_MONOTONIC,&start));
    for (;;) {
        struct kernel_vma vma;
        if (kernel_mm_vma_lookup(&view.mm,call.address,&vma)==KERNEL_MM_STATUS_NOT_MAPPED) break;
        assert(!clock_gettime(CLOCK_MONOTONIC,&now) && now.tv_sec-start.tv_sec<5);sched_yield();
    }
    __atomic_store_n(&resume_read,1,__ATOMIC_RELEASE);
    assert(!pthread_join(reader,NULL) && !pthread_join(unmapper,NULL) && call.done && call.status==KERNEL_UACCESS_STATUS_FAULT);
    window_mm_close(&view,pages);demand_idle(cache,0);
    puts("PASS unmap cancels a real cold mmap fault while its caller batch is in I/O; source/lease/page owners return");
}

static void window_u16(unsigned char *p,uint16_t value)
{ for(unsigned i=0;i<2;i++)p[i]=(unsigned char)(value>>(8*i)); }
static void window_u32(unsigned char *p,uint32_t value)
{ for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(value>>(8*i)); }
static void window_u64(unsigned char *p,uint64_t value)
{ for(unsigned i=0;i<8;i++)p[i]=(unsigned char)(value>>(8*i)); }
static void window_elf(struct kernel_page_cache *cache,struct kernel_heap *heap,
    struct physical_page_allocator *pages,struct kernel_vfs_mount *mount)
{
    struct kernel_open_file_description *file=NULL;int error;size_t written;
    assert(!kernel_open_file_create_mode(heap,mount,"/fault-elf",0700,&file,&error) && !error);file->open_flags=2;
    unsigned char bytes[BOAROS_PAGE_SIZE];
    for (unsigned i=0;i<32;i++) {
        memset(bytes,0x40+i,sizeof(bytes));
        if (!i) {
            memset(bytes,0,120);memcpy(bytes,"\177ELF\2\1\1",7);
            window_u16(bytes+16,KERNEL_ELF64_TYPE_EXECUTABLE);window_u16(bytes+18,KERNEL_ELF64_MACHINE_RISCV);
            window_u32(bytes+20,1);window_u64(bytes+24,0x100078);window_u64(bytes+32,64);
            window_u16(bytes+52,64);window_u16(bytes+54,56);window_u16(bytes+56,1);
            window_u32(bytes+64,KERNEL_ELF64_PROGRAM_LOAD);window_u32(bytes+68,KERNEL_ELF64_FLAG_READ|KERNEL_ELF64_FLAG_EXECUTE);
            window_u64(bytes+80,0x100000);window_u64(bytes+96,32*BOAROS_PAGE_SIZE);
            window_u64(bytes+104,32*BOAROS_PAGE_SIZE);window_u64(bytes+112,BOAROS_PAGE_SIZE);
        }
        assert(!kernel_vfs_pwrite(&file->file,(uint64_t)i*BOAROS_PAGE_SIZE,bytes,sizeof(bytes),&written) && written==sizeof(bytes));
    }
    uint64_t observed=kernel_vfs_error_sequence(&file->file);assert(!kernel_vfs_sync(&file->file,0,&observed));
    struct kernel_open_file_description *source_file=file;assert(!kernel_open_file_acquire(source_file));
    struct kernel_elf64_source *source=NULL;
    assert(!kernel_elf64_source_create(heap,&source_file,BOAROS_PAGE_SIZE,KERNEL_ELF64_MACHINE_RISCV,&source) && !source_file);
    const struct kernel_elf64_source_run *run=kernel_elf64_source_run_at(source,0);
    assert(run && run->kind==KERNEL_ELF64_SOURCE_RUN_FILE);
    struct window_mm view={0};window_mm_create(&view,heap,pages);
    assert(!kernel_mm_map_elf_source(&view.mm,source,0));
    uint64_t start=run->start;assert(!kernel_elf64_source_release(&source));
    reset_window(cache,file);
    assert(!kernel_mm_madvise(&view.mm,start,32*BOAROS_PAGE_SIZE,KERNEL_MM_ADVICE_RANDOM));
    assert(window_mm_read(&view.mm,view.context,start+BOAROS_PAGE_SIZE)==0x41 && source_reads==1);
    assert(!kernel_mm_madvise(&view.mm,start,32*BOAROS_PAGE_SIZE,KERNEL_MM_ADVICE_NORMAL));
    reset_demand_source();
    unsigned expected=BOAROS_PAGE_CACHE_READAHEAD_PAGES;
    if (!expected) expected=1;
    if (expected>12) expected=12;
    assert(window_mm_read(&view.mm,view.context,start+20*BOAROS_PAGE_SIZE)==0x54 && source_reads==expected);
    window_mm_close(&view,pages);
    assert(!kernel_open_file_release(&file));demand_idle(cache,0);
    puts("PASS ELF full FILE runs use the same fault window and per-VMA RANDOM policy; EOF bounds and executable source owners retained");
}

int main(int argc,char **argv)
{
    assert(argc==3);setvbuf(stdout,NULL,_IONBF,0);cache_test_attach(0);
    const char *test_case=getenv("FAULT_WINDOW_CASE");
    int oom_order_only=test_case && !strcmp(test_case,"oom-order"),oom_order_failed=0;
    int hot_lookup_only=test_case && !strcmp(test_case,"hot-lookup"),hot_lookup_failed=0;
    assert(!test_case || oom_order_only || hot_lookup_only);
    struct physical_page_allocator pages={0};struct kernel_heap heap={0};
    init_window_heap(&pages,&heap,pool,sizeof(pool));
    init_window_heap(&backend_pages,&backend_heap,backend_pool,sizeof(backend_pool));
    uint64_t baseline=physical_page_available(&pages),backend_baseline=physical_page_available(&backend_pages);
    struct kernel_page_cache cache={0};assert(!kernel_page_cache_init(&cache,&heap,&pages));current_cache=&cache;
    image_fd=open(argv[1],O_RDWR);assert(image_fd>=0);struct stat info;assert(!fstat(image_fd,&info));
    struct kernel_block_device disk={.read=image_read,.write=image_write,.flush=image_flush,
        .capacity_bytes=info.st_size,.logical_block_size=512,.cache_mode=KERNEL_BLOCK_CACHE_WRITEBACK};
    struct kernel_vfs_mount mount={0};assert(!kernel_vfs_mount_ext4(&mount,&disk,&backend_heap,&cache,0));
    assert(!kernel_page_cache_start_worker(&cache));
    struct kernel_open_file_description *file=NULL;int error;
    assert(!kernel_open_file_create_mode(&heap,&mount,"/fault-window",0600,&file,&error) && !error);
    file->open_flags=2;current_file=&file->file;
    unsigned char bytes[BOAROS_PAGE_SIZE];size_t written;
    for (unsigned i=0;i<32;i++) {
        memset(bytes,0x30+i,sizeof(bytes));
        assert(!kernel_vfs_pwrite(&file->file,(uint64_t)i*BOAROS_PAGE_SIZE,bytes,sizeof(bytes),&written) && written==sizeof(bytes));
    }
    uint64_t observed=kernel_vfs_error_sequence(&file->file);
    assert(!kernel_vfs_sync(&file->file,0,&observed));
    if (oom_order_only) oom_order_failed=window_mm_faultaround_oom_order(&cache,&heap,&pages,file);
    else if (hot_lookup_only) hot_lookup_failed=window_hot_lookup(&cache,file);
    else {
    hot_lookup_failed=window_hot_lookup(&cache,file);
    cold_window(&cache,file);
    window_errors(&cache,file);
    window_policy(&cache,file);
    window_loading(&cache,file);
    window_neighbour_loading(&cache,file);
    window_stop(&cache,file);
    window_mm_faultaround(&cache,&heap,&pages,file);
    window_mm_faultaround_policy(&cache,&heap,&pages,file);
    window_mm_faultaround_existing(&cache,&heap,&pages,file);
    window_mm_faultaround_oom(&cache,&heap,&pages,file);
    oom_order_failed=window_mm_faultaround_oom_order(&cache,&heap,&pages,file);
    window_mm_faultaround_bounds(&cache,&heap,&pages,file);
    window_mm_faultaround_loading(&cache,&heap,&pages,file);
    window_mm_hints(&cache,&heap,&pages,file);
    window_mm_cancel(&cache,&heap,&pages,file);
    capture_reads=0;window_elf(&cache,&heap,&pages,&mount);
    window_truncate(&cache,file);
    window_mm_faultaround_truncate(&cache,&heap,&pages,file);
    }
    capture_reads=0;current_file=NULL;
    assert(!kernel_open_file_release(&file) && !kernel_vfs_unmount(&mount));
    assert(!kernel_page_cache_destroy(&cache) && !close(image_fd));current_cache=NULL;
    struct kernel_heap_statistics state;
    kernel_heap_get_statistics(&heap,&state);assert(!state.live_allocations && !state.current_pages);
    kernel_heap_get_statistics(&backend_heap,&state);assert(!state.live_allocations && !state.current_pages);
    assert(!physical_page_allocator_audit(&pages) && !physical_page_allocator_audit(&backend_pages));
    assert(physical_page_available(&pages)==baseline && physical_page_available(&backend_pages)==backend_baseline);
    puts("PASS fault-window final page, heap, node and cache owners");
    assert(!oom_order_failed && "optional metadata must never starve the successful scalar target");
    assert(!hot_lookup_failed && "hot resident and lookup must complete before cache-global metadata is released");
    return 0;
}
