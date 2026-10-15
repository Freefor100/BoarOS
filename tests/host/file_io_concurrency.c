#include <kernel/files.h>
#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/mm_backend.h>
#include <kernel/page_cache.h>
#include <kernel/tmpfs.h>
#include <kernel/task.h>
#include <kernel/uaccess.h>
#include <kernel/block.h>
#include <kernel/cpu.h>
#include <kernel/errno.h>
#include <kernel/file_mapping.h>
#include "../../fs/files/private.h"
#include "../../fs/open_file_internal.h"
#include "../../fs/vfs_internal.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

void mm_test_attach(unsigned);
void mm_test_bind(struct kernel_mm *);
void mm_test_bind_files(struct kernel_files *);
void mm_test_finish(void);
void mm_test_activate_task(void);
extern _Thread_local uint64_t mm_test_context;
static unsigned char arena[32*1024*1024] __attribute__((aligned(16384)));
static unsigned char backend[16*1024*1024] __attribute__((aligned(16384)));
static void *access_page(uint64_t address) { return (void *)(uintptr_t)address; }
static int page_address(const void *page,uint64_t *address) { *address=(uintptr_t)page; return 1; }
static struct kernel_mm mm;
static struct kernel_files files;
static struct kernel_fs_context fs;
static struct kernel_open_file_description *control;
static int image;
static uint64_t context;
static uint64_t shared_mapping;
static struct kernel_files views[4];
static struct kernel_open_file_description *controls[4];
static unsigned pause_access,access_entered,resume_access,stop_done;
#define PACKETS 32U
struct packet { uint32_t actor,sequence; unsigned char payload[24]; };
static pthread_barrier_t launch;
static void *parallel_writer(void *argument)
{
    unsigned id=(unsigned)(uintptr_t)argument;
    mm_test_attach(id+1); kernel_cpu_current()->logical_id=id&1;
    mm_test_bind(&mm); mm_test_bind_files(&views[id]); mm_test_context=context;
    int started=pthread_barrier_wait(&launch);
    assert(!started || started==PTHREAD_BARRIER_SERIAL_THREAD);
    for (unsigned sequence=0;sequence<PACKETS;sequence++) {
        struct packet packet={.actor=id,.sequence=sequence};
        memset(packet.payload,(int)(0x40+id),sizeof(packet.payload));
        size_t copied; int64_t result;
        assert(!kernel_copy_to_user(&mm,0x200000+id*BOAROS_PAGE_SIZE,&packet,sizeof(packet),&copied) && copied==sizeof(packet));
        assert(!kernel_files_write(&views[id],&mm,id,0x200000+id*BOAROS_PAGE_SIZE,sizeof(packet),&result) && result==sizeof(packet));
    }
    assert(!kernel_files_release(&views[id])); mm_test_finish(); return 0;
}
static void parallel_case(struct kernel_heap *heap,struct kernel_vfs_mount *mount,
    struct kernel_page_cache *cache,unsigned actors,int append)
{
    char name[32]; snprintf(name,sizeof(name),"/parallel-%u-%d",actors,append);
    files=(struct kernel_files){0}; assert(!kernel_files_create(&files,heap));
    int error; int64_t result;
    for (unsigned i=0;i<actors;i++) {
        if (!i) assert(!kernel_open_file_create_mode(heap,mount,name,0600,&controls[i],&error) && !error);
        else if (append) assert(!kernel_open_file_create(heap,mount,name,&controls[i],&error) && !error);
        else { controls[i]=controls[0]; assert(!kernel_open_file_acquire(controls[i])); }
        controls[i]->open_flags=2|(append?KERNEL_FILES_O_APPEND:0);
        if (append || !i) assert(!kernel_vfs_file_acquire_write(&controls[i]->file));
        struct kernel_open_file_description *slot=controls[i];
        assert(!kernel_open_file_acquire(slot) && !kernel_files_install_new_owned_at(&files,i,0,&slot));
    }
    if (cache) assert(!kernel_page_cache_start_worker(cache));
    assert(!kernel_files_enable_concurrency(&files,&mm));
    assert(!pthread_barrier_init(&launch,0,actors)); pthread_t threads[4];
    for (unsigned i=0;i<actors;i++) {
        views[i]=(struct kernel_files){0}; assert(!kernel_files_acquire(&views[i],&files));
        assert(!pthread_create(&threads[i],0,parallel_writer,(void *)(uintptr_t)i));
    }
    for (unsigned i=0;i<actors;i++) assert(!pthread_join(threads[i],0));
    assert(!pthread_barrier_destroy(&launch)); mm_test_attach(0); mm_test_activate_task();
    assert(!kernel_files_lseek(&files,0,0,1,&result));
    int64_t total=(int64_t)(actors*PACKETS*sizeof(struct packet));
    if (append) assert(result>=(int64_t)(PACKETS*sizeof(struct packet)) && result<=total &&
        result%(int64_t)sizeof(struct packet)==0);
    else assert(result==total);
    struct packet packets[4*PACKETS]; size_t done;
    assert(!kernel_open_file_pread(controls[0],0,packets,actors*PACKETS*sizeof(*packets),&done) &&
        done==actors*PACKETS*sizeof(*packets));
    if (append) assert(packets[result/sizeof(struct packet)-1].actor==0 &&
        packets[result/sizeof(struct packet)-1].sequence==PACKETS-1);
    unsigned seen[4][PACKETS]={0};
    for (unsigned i=0;i<actors*PACKETS;i++) {
        assert(packets[i].actor<actors && packets[i].sequence<PACKETS &&
            !seen[packets[i].actor][packets[i].sequence]++);
        for (unsigned j=0;j<sizeof(packets[i].payload);j++)
            assert(packets[i].payload[j]==0x40+packets[i].actor);
    }
    assert(!kernel_files_sync(&files,0,0,&result) && !result);
    assert(!kernel_files_release(&files));
    for (unsigned i=0;i<actors;i++) assert(!kernel_open_file_release(&controls[i]));
    if (cache) kernel_page_cache_stop_worker(cache);
    printf("PASS %u threads %s %s: unique whole packets, offsets, final owners\n",actors,
        cache?"ext4 CPU0":"tmpfs local",append?"independent append OFDs":"shared OFD");
}
static enum kernel_block_status read_image(void *unused,uint64_t offset,void *data,size_t size)
{
    (void)unused; assert(!kernel_cpu_current()->logical_id && !kernel_cpu_current()->raw_locks);
    return pread(image,data,size,offset)==(ssize_t)size?KERNEL_BLOCK_STATUS_OK:KERNEL_BLOCK_STATUS_IO;
}
static enum kernel_block_status write_image(void *unused,uint64_t offset,const void *data,size_t size)
{
    (void)unused; assert(!kernel_cpu_current()->logical_id && !kernel_cpu_current()->raw_locks);
    return pwrite(image,data,size,offset)==(ssize_t)size?KERNEL_BLOCK_STATUS_OK:KERNEL_BLOCK_STATUS_IO;
}
static enum kernel_block_status flush_image(void *unused)
{ (void)unused; return fsync(image)?KERNEL_BLOCK_STATUS_IO:KERNEL_BLOCK_STATUS_OK; }
void __real_kernel_vfs_file_accessed(struct kernel_vfs_file *);
void __wrap_kernel_vfs_file_accessed(struct kernel_vfs_file *file)
{
    if (!kernel_vfs_file_memory(file)) assert(!kernel_cpu_current()->logical_id &&
        "regular disk read touched backend metadata on CPU1");
    if (__atomic_exchange_n(&pause_access,0,__ATOMIC_ACQ_REL)) {
        __atomic_store_n(&access_entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&resume_access,__ATOMIC_ACQUIRE)) sched_yield();
    }
    __real_kernel_vfs_file_accessed(file);
}
struct crossing { unsigned id; enum kernel_files_status status; int64_t result; char bytes[8]; };
static void *crossing_read(void *argument)
{
    struct crossing *call=argument; mm_test_attach(call->id); mm_test_bind(&mm);
    mm_test_bind_files(&files); mm_test_context=context;
    uint64_t buffer=0x200000+call->id*BOAROS_PAGE_SIZE;
    call->status=kernel_files_pread(&files,&mm,0,buffer,8,0,&call->result);
    if (!call->status) {
        size_t copied;
        assert(call->result==8 && !kernel_copy_from_user(&mm,call->bytes,buffer,8,&copied) && copied==8);
    }
    mm_test_finish(); return 0;
}
static void *stop_requests(void *argument)
{
    mm_test_attach(0); mm_test_activate_task();
    kernel_page_cache_stop_worker(argument);
    __atomic_store_n(&stop_done,1,__ATOMIC_RELEASE); mm_test_finish(); return 0;
}
static void crossing_case(struct kernel_heap *heap,struct kernel_vfs_mount *mount,
    struct kernel_page_cache *cache)
{
    int error; size_t done; int64_t result;
    files=(struct kernel_files){0}; assert(!kernel_files_create(&files,heap));
    for (unsigned i=0;i<2;i++) {
        assert(!kernel_open_file_create_mode(heap,mount,i?"/new-identity":"/old-identity",0600,&controls[i],&error) && !error);
        controls[i]->open_flags=2; assert(!kernel_vfs_file_acquire_write(&controls[i]->file));
        assert(!kernel_vfs_pwrite(&controls[i]->file,0,i?"replaced":"original",8,&done) && done==8);
        struct kernel_open_file_description *slot=controls[i];
        assert(!kernel_open_file_acquire(slot) && !kernel_files_install_new_owned_at(&files,i,0,&slot));
    }
    assert(!kernel_page_cache_start_worker(cache) && !kernel_files_enable_concurrency(&files,&mm));
    access_entered=resume_access=stop_done=0; __atomic_store_n(&pause_access,1,__ATOMIC_RELEASE);
    struct crossing first={.id=1},queued={.id=2}; pthread_t active,pending,stopper;
    assert(!pthread_create(&active,0,crossing_read,&first));
    while (!__atomic_load_n(&access_entered,__ATOMIC_ACQUIRE)) sched_yield();
    /* worker已取得原OFD；fd0替换不会重定向正在使用它的请求。 */
    assert(!kernel_files_close(&files,0,&result) && !result && controls[0]->references==2);
    assert(!kernel_files_dup3(&files,1,0,0,&result) && result==0);
    assert(!pthread_create(&pending,0,crossing_read,&queued));
    struct kernel_page_cache_concurrency_state state;
    do { kernel_page_cache_get_concurrency_state(cache,&state); sched_yield(); } while (state.pending!=1);
    assert(state.running==1 && !pthread_create(&stopper,0,stop_requests,cache));
    assert(!pthread_join(pending,0) && queued.status==KERNEL_FILES_STATUS_STATE);
    assert(!__atomic_load_n(&stop_done,__ATOMIC_ACQUIRE));
    assert(controls[0]->references==2); /* cancel不是正在执行请求的完成。 */
    __atomic_store_n(&resume_access,1,__ATOMIC_RELEASE);
    assert(!pthread_join(active,0) && !first.status && !memcmp(first.bytes,"original",8));
    assert(!pthread_join(stopper,0));
    kernel_page_cache_get_concurrency_state(cache,&state);
    assert(!state.pending && !state.running && !state.entry_borrows && state.executions_queued==state.executions_completed);
    mm_test_attach(0); mm_test_activate_task(); assert(!kernel_files_release(&files));
    for (unsigned i=0;i<2;i++) assert(!kernel_open_file_release(&controls[i]));
    puts("PASS executing OFD survives close/reuse; queued cancellation drains before stop; actual prefix and final owners");
}
static void bind(unsigned id)
{ mm_test_attach(id); mm_test_bind(&mm); mm_test_bind_files(&files); mm_test_context=context; }
static void mapped_source_write(void)
{
    int64_t result; size_t copied;
    unsigned char bytes[68];
    assert(kernel_open_file_size(control)==68);
    /* 首次读取尚未驻留的同文件映射；fault期间目标没有inode锁或写借用。 */
    assert(!kernel_files_pwrite(&files,&mm,0,shared_mapping,8,4,&result) && result==8);
    assert(!kernel_copy_from_user(&mm,bytes,shared_mapping,12,&copied) && copied==12 &&
        !memcmp(bytes,"origoriginal",12));
    assert(!kernel_files_pwrite(&files,&mm,0,shared_mapping+4,8,0,&result) && result==8);
    assert(!kernel_copy_from_user(&mm,bytes,shared_mapping,8,&copied) && copied==8 &&
        !memcmp(bytes,"original",8));
    /* 来源位于旧EOF尾部：先复制，再清新洞，不能把来源先归零。 */
    assert(!kernel_copy_to_user(&mm,shared_mapping+96,"fromtail",8,&copied) && copied==8);
    assert(!kernel_files_pwrite(&files,&mm,0,shared_mapping+96,8,128,&result) && result==8 &&
        kernel_open_file_size(control)==136);
    assert(!kernel_copy_from_user(&mm,bytes,shared_mapping+68,sizeof(bytes),&copied) && copied==sizeof(bytes));
    if (control->io_domain==2) for (size_t i=0;i<60;i++) assert(!bytes[i]);
    assert(!memcmp(bytes+60,"fromtail",8));
}
static void *read_and_write(void *unused)
{
    (void)unused; bind(1);
    int64_t result; size_t copied;
    assert(!kernel_files_read(&files,&mm,0,0x100000,8,&result) && result==8);
    char bytes[8];
    assert(!kernel_copy_from_user(&mm,bytes,0x100000,8,&copied) && copied==8 && !memcmp(bytes,"original",8));
    assert(!kernel_files_pwrite(&files,&mm,0,0x100000,8,32,&result) && result==8);
    assert(!kernel_files_lseek(&files,0,0,1,&result) && result==8);
    assert(!kernel_files_pread(&files,&mm,0,0x100000+BOAROS_PAGE_SIZE,8,32,&result) && result==8);
    assert(!kernel_copy_from_user(&mm,bytes,0x100000+BOAROS_PAGE_SIZE,8,&copied) && copied==8 && !memcmp(bytes,"original",8));
    if (shared_mapping) assert(!kernel_mm_msync(&mm,shared_mapping,BOAROS_PAGE_SIZE,4));
    if (control->io_domain==2) {
        assert(!kernel_files_sync(&files,0,0,&result) && !result);
        assert(!kernel_files_ftruncate(&files,0,48,&result) && !result);
        assert(!kernel_files_fstat(&files,&mm,0,0x100080,&result) && !result);
        struct kernel_linux_stat stat;
        assert(!kernel_copy_from_user(&mm,&stat,0x100080,sizeof(stat),&copied) && copied==sizeof(stat) && stat.st_size==48);
    }
    if (control->io_domain==2) {
        struct kernel_mm_user_pin pin={0};
        assert(!kernel_mm_pin_user_page(&mm,0x100000,KERNEL_MM_READ,&pin));
        assert(kernel_files_pread(&files,&mm,0,0x100000,0,0,&result)==KERNEL_FILES_STATUS_STATE);
        kernel_mm_unpin_user_page(&pin);
        assert(!kernel_files_pread(&files,&mm,0,0x100000,0,0,&result) && !result);
    }
    unsigned char sentinel=0xa5;
    assert(!kernel_copy_to_user(&mm,0x100000+BOAROS_PAGE_SIZE,&sentinel,1,&copied) && copied==1);
    assert(!kernel_mm_mprotect(&mm,0x100000+BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE,0));
    assert(!kernel_files_pread(&files,&mm,0,0x100000+BOAROS_PAGE_SIZE-4,8,32,&result) && result==4);
    assert(!kernel_copy_from_user(&mm,bytes,0x100000+BOAROS_PAGE_SIZE-4,4,&copied) &&
        copied==4 && !memcmp(bytes,"orig",4));
    assert(!kernel_files_pwrite(&files,&mm,0,0x100000+BOAROS_PAGE_SIZE-4,8,64,&result) && result==4);
    assert(!kernel_mm_mprotect(&mm,0x100000+BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE));
    assert(!kernel_copy_from_user(&mm,&sentinel,0x100000+BOAROS_PAGE_SIZE,1,&copied) && copied==1 && sentinel==0xa5);
    mapped_source_write();
    assert(kernel_files_syncfs(&files,0,&result)==KERNEL_FILES_STATUS_STATE);
    assert(kernel_files_statx(&files,&fs,&mm,0,0,KERNEL_FILES_AT_EMPTY_PATH,0x7ff,0x100100,&result)==
        KERNEL_FILES_STATUS_STATE);
    assert(kernel_files_getcwd(&files,&fs,&mm,0x100100,64,&result)==KERNEL_FILES_STATUS_STATE);
    assert(kernel_files_socketpair_create(&files,&mm,1,0,0x100100,&result)==KERNEL_FILES_STATUS_STATE);
    assert(kernel_files_epoll_create1(&files,0,&result)==KERNEL_FILES_STATUS_STATE);
    assert(!kernel_files_dup(&files,0,&result) && result==1);
    assert(!kernel_files_close(&files,0,&result) && !result);
    assert(!kernel_files_close(&files,1,&result) && !result);
    mm_test_finish(); return 0;
}
static void file_case(struct kernel_heap *heap,struct kernel_vfs_mount *mount,
    struct kernel_page_cache *cache)
{
    struct kernel_mm child={0};
    int error; size_t done;
    files=(struct kernel_files){0};
    fs=(struct kernel_fs_context){0}; assert(!kernel_fs_context_create(&fs,mount,heap));
    assert(!kernel_open_file_create_mode(heap,mount,"/ordinary",0600,&control,&error) && !error);
    control->open_flags=2;
    assert(!kernel_vfs_file_acquire_write(&control->file));
    assert(!kernel_vfs_pwrite(&control->file,0,"original",8,&done) && done==8);
    uint64_t observed=0;
    assert(!kernel_vfs_sync(&control->file,0,&observed));
    shared_mapping=0;
    if (cache) {
        uint64_t address;
        assert(!kernel_mm_munmap(&mm,0x100000,2*BOAROS_PAGE_SIZE));
        assert(!kernel_mm_mmap_anonymous(&mm,0x100000,2*BOAROS_PAGE_SIZE,
            KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED_NOREPLACE,&address));
        unsigned char marker=0x55; size_t copied;
        assert(!kernel_copy_to_user(&mm,0x100000,&marker,1,&copied) && copied==1);
        assert(!kernel_mm_fork(&child,&mm));
        assert(!kernel_page_cache_invalidate_node(cache,kernel_open_file_node(control)));
        assert(!kernel_page_cache_start_worker(cache));
    }
    struct kernel_open_file_description *source=control;
    assert(!kernel_open_file_acquire(source));
    assert(!kernel_file_mapping_prepare(kernel_open_file_node(control)));
    assert(!kernel_mm_mmap_file_private(&mm,&source,0x400000,BOAROS_PAGE_SIZE,0,
        KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_SHARED|KERNEL_MM_MAP_FIXED_NOREPLACE,&shared_mapping) && !source);
    assert(!kernel_files_create(&files,heap));
    struct kernel_open_file_description *slot=control;
    assert(!kernel_open_file_acquire(slot) && !kernel_files_install_new_owned_at(&files,0,0,&slot));
    assert(!kernel_files_enable_concurrency(&files,&mm));
    pthread_t actor; assert(!pthread_create(&actor,0,read_and_write,0) && !pthread_join(actor,0));
    mm_test_attach(0); mm_test_activate_task();
    assert(!kernel_files_release(&files));
    assert(!kernel_fs_context_release(&fs));
    if (child.state==KERNEL_MM_LIVE) {
        unsigned char marker=0; size_t copied;
        assert(!kernel_copy_from_user(&child,&marker,0x100000,1,&copied) && copied==1 && marker==0x55);
        assert(!kernel_mm_release(&child));
    }
    if (shared_mapping) assert(!kernel_mm_munmap(&mm,shared_mapping,BOAROS_PAGE_SIZE));
    assert(control->references==1);
    if (cache) {
        struct kernel_page_cache_concurrency_state state;
        kernel_page_cache_get_concurrency_state(cache,&state);
        assert(state.executions_queued>=10 && state.executions_completed==state.executions_queued && !state.pending && !state.running);
    }
    char bytes[8];
    assert(!kernel_open_file_pread(control,32,bytes,8,&done) && done==8 && !memcmp(bytes,"original",8));
    assert(!kernel_open_file_pread(control,64,bytes,4,&done) && done==4 && !memcmp(bytes,"orig",4));
    assert(!kernel_open_file_release(&control));
    if (cache) kernel_page_cache_stop_worker(cache);
    puts(cache?"PASS cross-CPU ext4 file I/O, shared offset, positioned prefix and CPU0 final owner":
        "PASS cross-CPU tmpfs file I/O, shared offset, positioned prefix and CPU0 final owner");
}
static void initialize(struct physical_page_allocator *pages,struct kernel_heap *heap,void *pool,size_t size)
{
    struct boot_memory_layout layout={.usable_count=1,.usable={{(uintptr_t)pool,size}}};
    assert(!physical_page_allocator_init(pages,&layout) && !physical_page_allocator_bind_access(pages,access_page) &&
        !physical_page_allocator_finalize(pages) && !kernel_heap_init(heap,pages,page_address));
}
static void sendfile_pipe_case(struct kernel_heap *heap,struct kernel_vfs_mount *mount)
{
    struct kernel_files local={0};
    struct kernel_open_file_description *source=0;
    int error;int64_t result;size_t done;
    unsigned char data[BOAROS_PAGE_SIZE+43],actual[BOAROS_PAGE_SIZE+17];
    for (size_t i=0;i<sizeof(data);i++) data[i]=(unsigned char)(i*17U+3U);
    assert(!kernel_open_file_create_mode(heap,mount,"/sendfile-source",0600,&source,&error) && !error);
    source->open_flags=2;assert(!kernel_vfs_file_acquire_write(&source->file));
    assert(!kernel_vfs_pwrite(&source->file,0,data,sizeof(data),&done) && done==sizeof(data));
    struct kernel_open_file_description *source_file=source;
    assert(!kernel_files_create(&local,heap) && !kernel_files_install_new_owned_at(&local,0,0,&source));
    assert(!kernel_files_pipe2(&local,&mm,0x200000,KERNEL_FILES_O_NONBLOCK,&result) && !result);
    int pair[2];assert(!kernel_copy_from_user(&mm,pair,0x200000,sizeof(pair),&done) && done==sizeof(pair));
    int64_t offset=13;assert(!kernel_copy_to_user(&mm,0x200080,&offset,sizeof(offset),&done));
    assert(!kernel_files_sendfile(&local,&mm,pair[1],0,0x200080,sizeof(actual),&result) && result==sizeof(actual));
    assert(!kernel_copy_from_user(&mm,&offset,0x200080,sizeof(offset),&done) && offset==13+(int64_t)sizeof(actual));
    assert(!kernel_files_read(&local,&mm,pair[0],0x200000,sizeof(actual),&result) && result==sizeof(actual));
    assert(!kernel_copy_from_user(&mm,actual,0x200000,sizeof(actual),&done) && done==sizeof(actual) &&
        !memcmp(actual,data+13,sizeof(actual)));
    assert(!kernel_vfs_ftruncate(&source_file->file,4U*BOAROS_PAGE_SIZE));
    uint64_t physical;size_t valid;
    int memory=kernel_open_file_memory_backed(source_file);
    if (memory) assert(kernel_open_file_lookup_page(source_file,2,&physical,&valid)==KERNEL_PAGE_CACHE_STATUS_NOT_FOUND);
    offset=2U*BOAROS_PAGE_SIZE+7U;
    assert(!kernel_copy_to_user(&mm,0x200080,&offset,sizeof(offset),&done));
    assert(!kernel_files_sendfile(&local,&mm,pair[1],0,0x200080,17,&result) && result==17);
    assert(!kernel_files_read(&local,&mm,pair[0],0x200000,17,&result) && result==17);
    assert(!kernel_copy_from_user(&mm,actual,0x200000,17,&done) && done==17);
    for (size_t i=0;i<17;i++) assert(!actual[i]);
    if (memory) assert(kernel_open_file_lookup_page(source_file,2,&physical,&valid)==KERNEL_PAGE_CACHE_STATUS_NOT_FOUND);
    assert(!kernel_files_release(&local));
    mm_test_finish();mm_test_attach(0);mm_test_activate_task();
    puts("PASS sendfile source-page borrow: unaligned cross-page pipe content, sparse zero bytes and explicit offset");
}

static void borrowed_page_case(struct physical_page_allocator *pages)
{
    struct kernel_task_io_buffer scratch={0},borrowed={0};
    assert(!kernel_task_io_buffer_acquire(&scratch,pages));
    uint64_t retained=scratch.physical_address;
    kernel_task_io_buffer_release(&scratch);
    uint64_t before=physical_page_available(pages),physical;
    assert(!physical_page_allocate(pages,&physical));
    kernel_task_io_buffer_take_page(&borrowed,pages,physical);
    assert(borrowed.physical_address==physical && !borrowed.retained_scratch);
    kernel_task_io_buffer_release(&borrowed);
    assert(physical_page_available(pages)==before);
    assert(!kernel_task_io_buffer_acquire(&scratch,pages) && scratch.physical_address==retained);
    kernel_task_io_buffer_release(&scratch);
    mm_test_finish();mm_test_attach(0);mm_test_activate_task();
}

#ifdef TCP_IO_CASES
#include "tcp_usercopy_cases.h"
#endif
int main(int argc,char **argv)
{
    assert(argc==2); setvbuf(stdout,0,_IONBF,0); mm_test_attach(0); mm_test_activate_task();
    struct physical_page_allocator pages={0},backend_pages={0};
    struct kernel_heap heap={0},backend_heap={0};
    initialize(&pages,&heap,arena,sizeof(arena)); initialize(&backend_pages,&backend_heap,backend,sizeof(backend));
    uint64_t baseline=physical_page_available(&pages),backend_baseline=physical_page_available(&backend_pages);
    borrowed_page_case(&pages);
    struct arch_mmu_page_table kernel={0}; struct arch_mmu_user_space user={0};
    assert(!riscv_sv39_page_table_init(&kernel,&pages)); kernel.state=ARCH_MMU_STATE_ACTIVE;
    assert(!arch_mmu_user_space_init(&user,&pages,&kernel) && !kernel_mm_create(&mm,&user) &&
        !kernel_mm_vma_enable(&mm,&heap) && !kernel_mm_brk_initialize(&mm,0x10000,0x20000));
    assert(!kernel_mm_enable_concurrency(&mm) && !kernel_mm_context(&mm,&context));
    mm_test_context=context; /* 当前地址空间API的宿主硬件上下文，不是任务绑定。 */
    uint64_t address;
    assert(!kernel_mm_mmap_anonymous(&mm,0x100000,2*BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE,
        KERNEL_MM_MAP_FIXED_NOREPLACE,&address));
    assert(!kernel_mm_mmap_anonymous(&mm,0x200000,4*BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE,
        KERNEL_MM_MAP_FIXED_NOREPLACE,&address));
#ifdef TCP_IO_CASES
    tcp_usercopy_case(&heap);
#endif
    struct kernel_vfs_mount *memory=0;
    assert(!kernel_tmpfs_create(&heap,0,"",&memory)); sendfile_pipe_case(&heap,memory); file_case(&heap,memory,0);
    for (unsigned actors=2;actors<=4;actors+=2) for (int append=0;append<=1;append++)
        parallel_case(&heap,memory,0,actors,append);
    assert(!kernel_vfs_unmount(memory));
    image=open(argv[1],O_RDWR); assert(image>=0); struct stat info; assert(!fstat(image,&info));
    struct kernel_page_cache cache={0}; assert(!kernel_page_cache_init_controlled(&cache,&heap,&pages));
    struct kernel_block_device disk={.read=read_image,.write=write_image,.flush=flush_image,
        .capacity_bytes=info.st_size,.logical_block_size=512,.cache_mode=KERNEL_BLOCK_CACHE_WRITEBACK};
    struct kernel_vfs_mount mount={0}; assert(!kernel_vfs_mount_ext4(&mount,&disk,&backend_heap,&cache,0));
    sendfile_pipe_case(&heap,&mount);
    file_case(&heap,&mount,&cache);
    crossing_case(&heap,&mount,&cache);
    for (unsigned actors=2;actors<=4;actors+=2) for (int append=0;append<=1;append++)
        parallel_case(&heap,&mount,&cache,actors,append);
    assert(!kernel_vfs_unmount(&mount) && !kernel_page_cache_destroy(&cache)); assert(!close(image));
    mm_test_context=0; /* 页表最终释放在可信kernel上下文。 */
    assert(!kernel_mm_release(&mm) && !physical_page_release(&pages,kernel.root_address));
    struct kernel_heap_statistics statistics;
    kernel_heap_get_statistics(&heap,&statistics); assert(!statistics.live_allocations && !statistics.current_pages);
    kernel_heap_get_statistics(&backend_heap,&statistics); assert(!statistics.live_allocations && !statistics.current_pages);
    assert(!physical_page_allocator_audit(&pages) && !physical_page_allocator_audit(&backend_pages) &&
        physical_page_available(&pages)==baseline && physical_page_available(&backend_pages)==backend_baseline);
    printf("PASS file I/O owners returned: OFD=%zu FD=%zu\n",sizeof(*control),sizeof(struct kernel_files_record));
}
