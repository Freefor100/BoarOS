#define main original_file_io_main
#define __wrap_kernel_rwlock_write fixture_rwlock_write
#define __wrap_kernel_wait_prepare fixture_wait_prepare
#define __wrap_kernel_uaccess_pin_user_page fixture_uaccess_pin
#include "file_io_concurrency.c"
#include "../../mm/uaccess_internal.h"
#undef __wrap_kernel_wait_prepare
#undef __wrap_kernel_rwlock_write
#undef __wrap_kernel_uaccess_pin_user_page
#undef main

static _Thread_local struct kernel_rwlock *target_lock;
static _Thread_local unsigned target_locks, pinned_resize;
static struct kernel_vfs_node *target_read;
static unsigned target_reads;
static unsigned source_pin_entered,source_editor_done,source_pin_armed,source_editor_parked;

enum kernel_uaccess_status __wrap_kernel_uaccess_pin_user_page(struct kernel_mm *space,uint64_t address,
    uint32_t access,struct kernel_mm_user_pin *pin)
{
    enum kernel_uaccess_status status=fixture_uaccess_pin(space,address,access,pin);
    if(!status && source_pin_armed && address==UINT64_C(0x300000)+BOAROS_PAGE_SIZE/3) {
        source_pin_armed=0;__atomic_store_n(&source_pin_entered,1,__ATOMIC_RELEASE);
        while(mm_test_parked(1)==source_editor_parked)sched_yield();
    }
    return status;
}

void __wrap_kernel_rwlock_write(struct kernel_rwlock *lock,struct kernel_lock_guard *guard)
{
    if(lock==target_lock)target_locks++;
    fixture_rwlock_write(lock,guard);
}
enum kernel_scheduler_status __wrap_kernel_wait_prepare(struct kernel_wait_queue *queue,
    uint64_t deadline,int interruptible,struct kernel_wait_token *token)
{
    assert(!(pinned_resize && kernel_io_context_current()->user_pins) &&
        "resize must return busy before waiting with source pins");
    return fixture_wait_prepare(queue,deadline,interruptible,token);
}
int __real_kernel_vfs_node_pread_batch(struct kernel_vfs_node *,struct file_read_span *,size_t,
    struct file_read_workspace *,const struct file_read_completion *);
int __wrap_kernel_vfs_node_pread_batch(struct kernel_vfs_node *node,struct file_read_span *spans,size_t count,
    struct file_read_workspace *workspace,const struct file_read_completion *completion)
{
    if(node==target_read)target_reads++;
    return __real_kernel_vfs_node_pread_batch(node,spans,count,workspace,completion);
}

#define SOURCE_ADDRESS UINT64_C(0x300000)
static unsigned char expected[2*BOAROS_PAGE_SIZE],actual[2*BOAROS_PAGE_SIZE];

static struct kernel_open_file_description *target_file(struct kernel_heap *heap,
    struct kernel_vfs_mount *mount,struct kernel_files *local,const char *name)
{
    struct kernel_open_file_description *file=NULL;int error=0;
    assert(!kernel_files_create(local,heap));
    assert(!kernel_open_file_create_mode(heap,mount,name,0600,&file,&error) && !error);
    file->open_flags=2;assert(!kernel_vfs_file_acquire_write(&file->file));
    struct kernel_open_file_description *owned=file;
    assert(!kernel_files_install_new_owned_at(local,0,0,&owned));return file;
}
static void target_contents(struct kernel_open_file_description *file,size_t size)
{
    size_t copied=0;
    assert(kernel_open_file_size(file)==size &&
        !kernel_open_file_pread(file,0,actual,sizeof(actual),&copied) && copied==size);
    assert(!memcmp(actual,expected,size));
    assert(!kernel_io_context_current()->user_pins && !kernel_io_context_current()->cache_writes &&
        !kernel_io_context_current()->locks);
}
static void complete_target_pages(struct kernel_heap *heap,struct kernel_vfs_mount *mount,
    struct kernel_page_cache *cache,int overwrite)
{
    unsigned counts[2];size_t copied;
    for(unsigned unaligned=0;unaligned<2;unaligned++) {
        struct kernel_files local={0};char name[64];
        snprintf(name,sizeof(name),"/whole-%u-%u",overwrite,unaligned);
        struct kernel_open_file_description *file=target_file(heap,mount,&local,name);
        if(overwrite) {
            memset(actual,0x71,sizeof(actual));
            assert(!kernel_vfs_pwrite(&file->file,0,actual,sizeof(actual),&copied) && copied==sizeof(actual));
            uint64_t observed=0;assert(!kernel_vfs_sync(&file->file,0,&observed));
            if(cache)assert(!kernel_page_cache_invalidate_node(cache,kernel_open_file_node(file)));
        }
        uint64_t source=SOURCE_ADDRESS+(unaligned?BOAROS_PAGE_SIZE/3:0);
        assert(!kernel_copy_to_user(&mm,source,expected,sizeof(expected),&copied) && copied==sizeof(expected));
        target_locks=target_reads=0;target_lock=&kernel_open_file_node(file)->io_lock;
        target_read=kernel_open_file_node(file);int64_t result;
        assert(!kernel_files_write(&local,&mm,0,source,sizeof(expected),&result) && result==sizeof(expected));
        target_lock=NULL;target_read=NULL;counts[unaligned]=target_locks;
        assert(!target_reads && "complete file-page overwrite must not read the superseded old payload");
        target_contents(file,sizeof(expected));assert(!kernel_files_release(&local));
    }
    printf("target page qualifications overwrite=%d aligned=%u unaligned=%u\n",overwrite,counts[0],counts[1]);
    assert(counts[0]==counts[1] && "source-page boundaries must not repeat target-page qualification");
    puts("PASS aligned and unaligned complete target pages: one qualification and no old-payload read");
}
static void source_fault_prefix(struct kernel_heap *heap,struct kernel_vfs_mount *mount,
    struct kernel_page_cache *cache,int append)
{
    struct kernel_files local={0};char name[48];snprintf(name,sizeof(name),"/prefix-%u",append);
    struct kernel_open_file_description *file=target_file(heap,mount,&local,name);size_t copied;
    if(!append) {
        memset(actual,0x71,BOAROS_PAGE_SIZE);
        assert(!kernel_vfs_pwrite(&file->file,0,actual,BOAROS_PAGE_SIZE,&copied) && copied==BOAROS_PAGE_SIZE);
        uint64_t observed=0;assert(!kernel_vfs_sync(&file->file,0,&observed));
        if(cache)assert(!kernel_page_cache_invalidate_node(cache,kernel_open_file_node(file)));
    }
    size_t offset=BOAROS_PAGE_SIZE/3,prefix=BOAROS_PAGE_SIZE-offset;
    assert(!kernel_copy_to_user(&mm,SOURCE_ADDRESS+offset,expected,prefix,&copied) && copied==prefix);
    assert(!kernel_mm_mprotect(&mm,SOURCE_ADDRESS+BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE,0));
    int64_t result;
    assert(!kernel_files_write(&local,&mm,0,SOURCE_ADDRESS+offset,BOAROS_PAGE_SIZE,&result) && result==(int64_t)prefix);
    assert(!kernel_mm_mprotect(&mm,SOURCE_ADDRESS+BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE));
    memset(actual,0,sizeof(actual));
    assert(!kernel_open_file_pread(file,0,actual,sizeof(actual),&copied) && copied==(append?prefix:BOAROS_PAGE_SIZE));
    assert(!memcmp(actual,expected,prefix));
    if(!append)for(size_t i=prefix;i<BOAROS_PAGE_SIZE;i++)assert(actual[i]==0x71);
    else {
        uint64_t physical;size_t valid;void *page;
        assert(!(cache?kernel_page_cache_get(cache,&file->file,0,&physical,&valid):
            kernel_open_file_lookup_page(file,0,&physical,&valid)) && valid==prefix);
        struct physical_page_allocator *allocator=cache?cache->allocator:heap->page_allocator;
        assert(!physical_page_map_owned(allocator,physical,&page));
        for(size_t i=prefix;i<BOAROS_PAGE_SIZE;i++)assert(!((unsigned char *)page)[i]);
        assert(!physical_page_release(allocator,physical));
    }
    assert(!kernel_files_release(&local));
    assert(!kernel_io_context_current()->user_pins && !kernel_io_context_current()->cache_writes);
    puts("PASS second source-page fault: completed prefix, original tail or new EOF zeros, owners returned");
}
static void scattered_sources(struct kernel_heap *heap,struct kernel_vfs_mount *mount,
    struct kernel_page_cache *cache,int fault)
{
    struct kernel_files local={0};char name[48];snprintf(name,sizeof(name),"/scatter-%u",fault);
    struct kernel_open_file_description *file=target_file(heap,mount,&local,name);
    struct kernel_uaccess_iovec iov[6]={{0}};size_t copied,part=BOAROS_PAGE_SIZE/4;
    for(unsigned i=0;i<4;i++) {
        iov[i+1]=(struct kernel_uaccess_iovec){SOURCE_ADDRESS+i*BOAROS_PAGE_SIZE+13,part};
        assert(!kernel_copy_to_user(&mm,iov[i+1].base,expected+i*part,part,&copied) && copied==part);
    }
    assert(!kernel_copy_to_user(&mm,0x200000,iov,sizeof(iov),&copied) && copied==sizeof(iov));
    if(fault) {
        memset(actual,0x71,BOAROS_PAGE_SIZE);
        assert(!kernel_vfs_pwrite(&file->file,0,actual,BOAROS_PAGE_SIZE,&copied) && copied==BOAROS_PAGE_SIZE);
        uint64_t observed=0;assert(!kernel_vfs_sync(&file->file,0,&observed));
        if(cache)assert(!kernel_page_cache_invalidate_node(cache,kernel_open_file_node(file)));
        assert(!kernel_mm_mprotect(&mm,SOURCE_ADDRESS+2*BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE,0));
    }
    int64_t result;
    assert(!kernel_files_writev(&local,&mm,0,0x200000,6,&result) &&
        result==(int64_t)(fault?2*part:BOAROS_PAGE_SIZE));
    if(fault)assert(!kernel_mm_mprotect(&mm,SOURCE_ADDRESS+2*BOAROS_PAGE_SIZE,
        BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE));
    assert(!kernel_open_file_pread(file,0,actual,sizeof(actual),&copied) && copied==BOAROS_PAGE_SIZE);
    assert(!memcmp(actual,expected,fault?2*part:BOAROS_PAGE_SIZE));
    if(fault)for(size_t i=2*part;i<BOAROS_PAGE_SIZE;i++)assert(actual[i]==0x71);
    assert(!kernel_files_release(&local) && !kernel_io_context_current()->user_pins);
    puts("PASS scattered writev beyond source window: zero vectors, next-window fault, exact prefix and old tail");
}
static void mapped_source_overlap(struct kernel_heap *heap,struct kernel_vfs_mount *mount)
{
    struct kernel_files local={0};
    struct kernel_open_file_description *file=target_file(heap,mount,&local,"/mapped-source");
    size_t copied,offset=BOAROS_PAGE_SIZE/3;
    assert(!kernel_vfs_pwrite(&file->file,0,expected,sizeof(expected),&copied) && copied==sizeof(expected));
    assert(!kernel_open_file_acquire(file));struct kernel_open_file_description *source=file;
    uint64_t mapping;
    assert(!kernel_mm_mmap_file_private(&mm,&source,0x600000,sizeof(expected),0,
        KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_SHARED|KERNEL_MM_MAP_FIXED_NOREPLACE,&mapping) && !source);
    for(unsigned resident=0;resident<2;resident++) {
        if(resident)assert(!kernel_vfs_pwrite(&file->file,0,expected,sizeof(expected),&copied) && copied==sizeof(expected));
        int64_t result;
        assert(!kernel_files_pwrite(&local,&mm,0,mapping+offset,BOAROS_PAGE_SIZE,0,&result) && result==BOAROS_PAGE_SIZE);
        assert(!kernel_open_file_pread(file,0,actual,sizeof(actual),&copied) && copied==sizeof(actual));
        assert(!memcmp(actual,expected+offset,BOAROS_PAGE_SIZE) &&
            !memcmp(actual+BOAROS_PAGE_SIZE,expected+BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE));
    }
    assert(!kernel_mm_munmap(&mm,mapping,sizeof(expected)) && !kernel_files_release(&local));
    assert(!kernel_io_context_current()->user_pins && !kernel_io_context_current()->cache_writes);
    puts("PASS two-page shared-mmap source: overlapping copies preserve fragment order and mapping owners");
}
static void *source_editor(void *unused)
{
    (void)unused;mm_test_attach(1);mm_test_bind(&mm);mm_test_context=context;
    while(!__atomic_load_n(&source_pin_entered,__ATOMIC_ACQUIRE))sched_yield();
    assert(!kernel_mm_mprotect(&mm,SOURCE_ADDRESS+BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE,0));
    __atomic_store_n(&source_editor_done,1,__ATOMIC_RELEASE);mm_test_finish();return NULL;
}
static void source_editor_prefix(struct kernel_heap *heap,struct kernel_vfs_mount *mount,
    struct kernel_page_cache *cache)
{
    struct kernel_files local={0};
    struct kernel_open_file_description *file=target_file(heap,mount,&local,"/source-editor");
    size_t copied,prefix=BOAROS_PAGE_SIZE-BOAROS_PAGE_SIZE/3;
    memset(actual,0x71,BOAROS_PAGE_SIZE);
    assert(!kernel_vfs_pwrite(&file->file,0,actual,BOAROS_PAGE_SIZE,&copied) && copied==BOAROS_PAGE_SIZE);
    uint64_t observed=0;assert(!kernel_vfs_sync(&file->file,0,&observed));
    if(cache)assert(!kernel_page_cache_invalidate_node(cache,kernel_open_file_node(file)));
    assert(!kernel_copy_to_user(&mm,SOURCE_ADDRESS+BOAROS_PAGE_SIZE/3,expected,BOAROS_PAGE_SIZE,&copied) && copied==BOAROS_PAGE_SIZE);
    source_pin_entered=source_editor_done=0;source_pin_armed=1;source_editor_parked=mm_test_parked(1);
    pthread_t editor;assert(!pthread_create(&editor,NULL,source_editor,NULL));
    int64_t result;
    assert(!kernel_files_write(&local,&mm,0,SOURCE_ADDRESS+BOAROS_PAGE_SIZE/3,BOAROS_PAGE_SIZE,&result) && result==(int64_t)prefix);
    assert(!pthread_join(editor,NULL) && source_editor_done && mm_test_parked(1)>source_editor_parked);
    assert(!kernel_mm_mprotect(&mm,SOURCE_ADDRESS+BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE));
    assert(!kernel_open_file_pread(file,0,actual,sizeof(actual),&copied) && copied==BOAROS_PAGE_SIZE);
    assert(!memcmp(actual,expected,prefix));for(size_t i=prefix;i<BOAROS_PAGE_SIZE;i++)assert(actual[i]==0x71);
    assert(!kernel_files_release(&local) && !kernel_io_context_current()->user_pins);
    puts("PASS source MM edit after first pin: nonblocking second page, completed prefix and preserved old tail");
}
static void pinned_resize_busy(struct kernel_heap *heap,struct kernel_vfs_mount *mount,
    struct kernel_page_cache *cache)
{
    struct kernel_files local={0};struct kernel_open_file_description *file=target_file(heap,mount,&local,"/resize-pin");
    size_t copied;assert(!kernel_vfs_pwrite(&file->file,0,"old",3,&copied) && copied==3);
    struct kernel_page_cache_resize held={0},attempt={0};struct kernel_mm_user_pin pin={0};
    assert(!kernel_page_cache_resize_prepare(cache,kernel_open_file_node(file),3,BOAROS_PAGE_SIZE+3,&held));
    assert(!kernel_uaccess_pin_user_page(&mm,SOURCE_ADDRESS,KERNEL_MM_READ,&pin));
    pinned_resize=1;
    assert(kernel_page_cache_resize_prepare(cache,kernel_open_file_node(file),3,BOAROS_PAGE_SIZE+3,&attempt)==-KERNEL_EBUSY);
    pinned_resize=0;assert(!attempt.entry);kernel_mm_unpin_user_page(&pin);
    kernel_page_cache_resize_finish(&held,3);assert(!kernel_files_release(&local));
    puts("PASS resize busy while pinned: no wait registration and no retained source or target borrow");
}

int main(int argc,char **argv)
{
    assert(argc==2);setvbuf(stdout,NULL,_IONBF,0);mm_test_attach(0);mm_test_activate_task();
    struct physical_page_allocator pages={0},backend_pages={0};struct kernel_heap heap={0},backend_heap={0};
    initialize(&pages,&heap,arena,sizeof(arena));initialize(&backend_pages,&backend_heap,backend,sizeof(backend));
    uint64_t available=physical_page_available(&pages),backend_available=physical_page_available(&backend_pages);
    struct arch_mmu_page_table kernel={0};struct arch_mmu_user_space user={0};
    assert(!riscv_sv39_page_table_init(&kernel,&pages));kernel.state=ARCH_MMU_STATE_ACTIVE;
    assert(!arch_mmu_user_space_init(&user,&pages,&kernel) && !kernel_mm_create(&mm,&user) &&
        !kernel_mm_vma_enable(&mm,&heap) && !kernel_mm_brk_initialize(&mm,0x10000,0x20000));
    assert(!kernel_mm_check_task_domain(&mm) && !kernel_mm_context(&mm,&context));mm_test_context=context;
    uint64_t address;
    assert(!kernel_mm_mmap_anonymous(&mm,0x100000,2*BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED_NOREPLACE,&address));
    assert(!kernel_mm_mmap_anonymous(&mm,0x200000,4*BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED_NOREPLACE,&address));
    assert(!kernel_mm_mmap_anonymous(&mm,SOURCE_ADDRESS,4*BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED_NOREPLACE,&address));
    for(size_t i=0;i<sizeof(expected);i++)expected[i]=(unsigned char)(i*13+i/31);
    struct kernel_vfs_mount *memory=NULL;assert(!kernel_tmpfs_create(&heap,0,"",&memory));
    complete_target_pages(&heap,memory,NULL,0);complete_target_pages(&heap,memory,NULL,1);
    source_fault_prefix(&heap,memory,NULL,0);source_fault_prefix(&heap,memory,NULL,1);
    scattered_sources(&heap,memory,NULL,0);scattered_sources(&heap,memory,NULL,1);
    mapped_source_overlap(&heap,memory);source_editor_prefix(&heap,memory,NULL);write_page_fragments(&heap,memory);
    assert(!kernel_vfs_unmount(memory));
    image=open(argv[1],O_RDWR);assert(image>=0);struct stat info;assert(!fstat(image,&info));
    struct kernel_page_cache cache={0};assert(!kernel_page_cache_init(&cache,&heap,&pages));
    struct kernel_block_device disk={.read=read_image,.write=write_image,.flush=flush_image,.capacity_bytes=info.st_size,.logical_block_size=512,.cache_mode=KERNEL_BLOCK_CACHE_WRITEBACK};
    assert(!kernel_block_register(&disk,KERNEL_BLOCK_DEVICE_NUMBER(108)));
    struct kernel_vfs_mount mount={0};assert(!kernel_vfs_mount_ext4(&mount,&disk,&backend_heap,&cache,0));
    assert(!kernel_vfs_start_journal_worker(&mount));
    complete_target_pages(&heap,&mount,&cache,0);complete_target_pages(&heap,&mount,&cache,1);
    source_fault_prefix(&heap,&mount,&cache,0);source_fault_prefix(&heap,&mount,&cache,1);
    scattered_sources(&heap,&mount,&cache,0);scattered_sources(&heap,&mount,&cache,1);
    mapped_source_overlap(&heap,&mount);
    source_editor_prefix(&heap,&mount,&cache);
    write_page_fragments(&heap,&mount);pinned_resize_busy(&heap,&mount,&cache);
    assert(!kernel_vfs_unmount(&mount) && !kernel_page_cache_destroy(&cache) && !kernel_block_unregister(&disk));
    mm_test_context=0;assert(!kernel_mm_release(&mm) && !physical_page_release(&pages,kernel.root_address));mm_test_finish();
    assert(physical_page_available(&pages)==available && physical_page_available(&backend_pages)==backend_available);
    assert(!close(image));puts("PASS target-page source owners returned");return 0;
}
