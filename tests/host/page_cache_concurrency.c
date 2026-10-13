#include <kernel/page_cache.h>
#include <kernel/heap.h>
#include <kernel/cpu.h>
#include <kernel/sync.h>
#include <kernel/block.h>
#include <kernel/vfs.h>
#include <kernel/page.h>
#include <kernel/errno.h>
#include "../../fs/vfs_objects.h"
#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <sched.h>
#ifdef CACHE_MM_CASES
void mm_test_attach(unsigned);
#include "mm_disk_cases.h"
#endif

void mm_test_attach(unsigned);
static unsigned char backend_pool[16 * 1024 * 1024] __attribute__((aligned(16384)));
static struct physical_page_allocator backend_pages;
static struct kernel_heap backend_heap;
static unsigned char pool[16 * 1024 * 1024] __attribute__((aligned(16384)));
static void *access_page(uint64_t p) { return (void *)(uintptr_t)p; }
static int page_address(const void *p, uint64_t *out)
{ *out = (uintptr_t)p; return 1; }
static int image_fd;
static unsigned source_reads, pause_read, read_entered, resume_read;
static int source_error;
static unsigned write_error;
static enum kernel_page_cache_status expected_status;
static unsigned watch_stop, stop_entered, stop_done;
static struct kernel_page_cache *current_cache;
static struct kernel_vfs_file *current_file;
static uint64_t received[5];
extern unsigned mm_test_parked(unsigned);
static enum kernel_block_status image_read(void *context, uint64_t off, void *out, size_t size)
{
    (void)context;
    assert(kernel_cpu_current()->hardware_id == 0 && "disk I/O must execute on CPU0");
    return pread(image_fd, out, size, off) == (ssize_t)size ? KERNEL_BLOCK_STATUS_OK : KERNEL_BLOCK_STATUS_IO;
}
static enum kernel_block_status image_write(void *context, uint64_t off, const void *in, size_t size)
{
    (void)context;
    assert(kernel_cpu_current()->hardware_id == 0);
    return pwrite(image_fd, in, size, off) == (ssize_t)size ? KERNEL_BLOCK_STATUS_OK : KERNEL_BLOCK_STATUS_IO;
}
static enum kernel_block_status image_flush(void *context)
{ (void)context; return fsync(image_fd) ? KERNEL_BLOCK_STATUS_IO : KERNEL_BLOCK_STATUS_OK; }
int __real_kernel_vfs_node_pread(struct kernel_vfs_node *, uint64_t, void *, size_t, size_t *);
int __wrap_kernel_vfs_node_pread(struct kernel_vfs_node *node, uint64_t off, void *out, size_t size, size_t *done)
{
    assert(kernel_cpu_current()->hardware_id == 0 && "cold miss escaped CPU0 executor");
    __atomic_fetch_add(&source_reads, 1, __ATOMIC_RELAXED);
    if (__atomic_exchange_n(&pause_read, 0, __ATOMIC_ACQ_REL)) {
        assert(!kernel_cpu_current()->raw_locks);
        __atomic_store_n(&read_entered, 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&resume_read, __ATOMIC_ACQUIRE)) sched_yield();
    }
    if (source_error) { *done = 0; return source_error; }
    return __real_kernel_vfs_node_pread(node, off, out, size, done);
}
static void *get_cold_page(void *arg)
{
    unsigned id = (unsigned)(uintptr_t)arg;
    mm_test_attach(id);
    size_t valid;
    assert(kernel_page_cache_get(current_cache, current_file, 0, &received[id], &valid) == expected_status);
    if (expected_status) return 0;
    assert(valid == BOAROS_PAGE_SIZE);
    for (size_t i = 0; i < valid; i++) assert(((unsigned char *)access_page(received[id]))[i] == 0x73);
    return 0;
}
void __real_kernel_thread_join(struct kernel_thread_join *);
void __wrap_kernel_thread_join(struct kernel_thread_join *join)
{
    if (__atomic_load_n(&watch_stop, __ATOMIC_ACQUIRE))
        __atomic_store_n(&stop_entered, 1, __ATOMIC_RELEASE);
    __real_kernel_thread_join(join);
}
static void *stop_cache(void *arg)
{
    (void)arg; mm_test_attach(0);
    kernel_page_cache_stop_worker(current_cache);
    __atomic_store_n(&stop_done, 1, __ATOMIC_RELEASE);
    return 0;
}
static void *get_cancelled_page(void *arg)
{
    (void)arg; mm_test_attach(2);
    uint64_t address; size_t valid;
    assert(kernel_page_cache_get(current_cache, current_file, 1, &address, &valid) == KERNEL_PAGE_CACHE_STATUS_STATE);
    return 0;
}
static unsigned rearms;
static unsigned pause_write, write_entered, resume_write, sync_done;
static unsigned pause_alias, alias_entered, resume_alias, drain_entered, drain_done;
static struct kernel_page_cache_alias *drain_target;
static int rearm_alias(void *owner, uint64_t address)
{
    struct kernel_vfs_node *node = owner;
    (void)address;
    assert(!kernel_cpu_current()->raw_locks && !kernel_lock_held(&node->io_lock, 0) &&
        "writeback callback must be outside inode I/O and raw protection");
    if (__atomic_exchange_n(&pause_alias,0,__ATOMIC_ACQ_REL)) {
        __atomic_store_n(&alias_entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&resume_alias,__ATOMIC_ACQUIRE)) sched_yield();
    }
    __atomic_fetch_add(&rearms,1,__ATOMIC_RELEASE);
    return 0;
}
int __real_kernel_vfs_node_writeback(struct kernel_vfs_node *,uint64_t,const void *,size_t,size_t *);
int __wrap_kernel_vfs_node_writeback(struct kernel_vfs_node *node,uint64_t offset,const void *data,size_t size,size_t *written)
{
    if (__atomic_exchange_n(&pause_write,0,__ATOMIC_ACQ_REL)) {
        __atomic_store_n(&write_entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&resume_write,__ATOMIC_ACQUIRE)) sched_yield();
    }
    unsigned fault=__atomic_exchange_n(&write_error,0,__ATOMIC_ACQ_REL);
    if (fault) {
        *written=0;
        if (fault==2 && size>1) {
            int result=__real_kernel_vfs_node_writeback(node,offset,data,size/2,written);
            assert(!result && *written==size/2);
        }
        kernel_vfs_record_writeback_error(node,-KERNEL_EIO);
        return fault==2?0:-KERNEL_EIO;
    }
    return __real_kernel_vfs_node_writeback(node,offset,data,size,written);
}
static void *sync_file(void *arg)
{
    mm_test_attach(0); uint64_t error=kernel_vfs_error_sequence(arg);
    assert(!kernel_vfs_sync(arg,0,&error));
    __atomic_store_n(&sync_done,1,__ATOMIC_RELEASE);
    return 0;
}
static void *drain_alias(void *arg)
{
    (void)arg; mm_test_attach(6);
    kernel_page_cache_alias_detach(drain_target);
    __atomic_store_n(&drain_entered,1,__ATOMIC_RELEASE);
    kernel_page_cache_alias_drain(drain_target);
    __atomic_store_n(&drain_done,1,__ATOMIC_RELEASE);
    return 0;
}
static void alias_snapshot(struct kernel_page_cache *cache, struct kernel_vfs_file *file,
    struct physical_page_allocator *pages,unsigned count)
{
    uint64_t address; size_t valid;
    assert(!kernel_page_cache_get(cache, file, 0, &address, &valid));
    struct kernel_page_cache_alias aliases[32] = {0};
    for (unsigned i = 0; i < count; i++) assert(!kernel_page_cache_alias_attach(cache, file, 0,
        address, &aliases[i], kernel_vfs_file_node(file), 0x40000 + i * BOAROS_PAGE_SIZE, rearm_alias));
    struct kernel_page_cache_write_borrow writer={0};
    assert(!kernel_page_cache_alias_write_begin(&aliases[0],&writer));
    ((unsigned char *)access_page(address))[0] = 0x41;
    kernel_page_cache_alias_mark_dirty(&aliases[0]);
    rearms = sync_done = write_entered = resume_write = 0; pause_write=1;
    pause_alias=count==32; alias_entered=resume_alias=drain_entered=drain_done=0;
    pthread_t sync; assert(!pthread_create(&sync,0,sync_file,file));
    if (count==32) {
        while (!__atomic_load_n(&alias_entered,__ATOMIC_ACQUIRE)) sched_yield();
        drain_target=&aliases[count-1]; unsigned parked=mm_test_parked(6);
        pthread_t drain; assert(!pthread_create(&drain,0,drain_alias,0));
        while (!__atomic_load_n(&drain_entered,__ATOMIC_ACQUIRE) || mm_test_parked(6)==parked) sched_yield();
        assert(!__atomic_load_n(&drain_done,__ATOMIC_ACQUIRE) && drain_target->entry);
        __atomic_store_n(&resume_alias,1,__ATOMIC_RELEASE);
        assert(!pthread_join(drain,0) && !drain_target->entry);
    }
    while (__atomic_load_n(&rearms,__ATOMIC_ACQUIRE)!=count) sched_yield();
    struct kernel_page_cache_write_borrow rejected={0};
    assert(kernel_page_cache_alias_write_begin(&aliases[0],&rejected)==KERNEL_PAGE_CACHE_STATUS_BUSY && !rejected.entry);
    assert(!__atomic_load_n(&sync_done,__ATOMIC_ACQUIRE));
    /* 无关页能取得写资格；目标页的已有借用仍是快照的必要前置。 */
    uint64_t other; size_t other_valid;
    assert(!kernel_page_cache_get(cache,file,1,&other,&other_valid));
    struct kernel_page_cache_alias unrelated={0};
    assert(!kernel_page_cache_alias_attach(cache,file,1,other,&unrelated,
        kernel_vfs_file_node(file),0x100000,rearm_alias));
    struct kernel_page_cache_write_borrow other_writer={0};
    assert(!kernel_page_cache_alias_write_begin(&unrelated,&other_writer));
    kernel_page_cache_write_end(&other_writer);
    kernel_page_cache_alias_detach(&unrelated); kernel_page_cache_alias_drain(&unrelated);
    assert(!physical_page_release(pages,other));
    kernel_page_cache_write_end(&writer);
    while (!__atomic_load_n(&write_entered,__ATOMIC_ACQUIRE)) sched_yield();
    /* I/O期间重新开放，后一代写不能被旧快照完成清除。 */
    assert(!kernel_page_cache_alias_write_begin(&aliases[0],&writer));
    ((unsigned char *)access_page(address))[0]=0x66;
    kernel_page_cache_alias_mark_dirty(&aliases[0]); kernel_page_cache_write_end(&writer);
    __atomic_store_n(&resume_write,1,__ATOMIC_RELEASE);
    assert(!pthread_join(sync,0));
    unsigned char disk_byte; size_t done;
    assert(!__real_kernel_vfs_node_pread(kernel_vfs_file_node(file),0,&disk_byte,1,&done) && done==1 && disk_byte==0x41);
    uint64_t error=kernel_vfs_error_sequence(file); assert(!kernel_vfs_sync(file,0,&error));
    assert(!__real_kernel_vfs_node_pread(kernel_vfs_file_node(file),0,&disk_byte,1,&done) && done==1 && disk_byte==0x66);
    for (unsigned fault=1;fault<=2;fault++) {
        assert(!kernel_page_cache_alias_write_begin(&aliases[0],&writer));
        ((unsigned char *)access_page(address))[0]=(unsigned char)(0x70+fault);
        kernel_page_cache_alias_mark_dirty(&aliases[0]); kernel_page_cache_write_end(&writer);
        uint64_t before=kernel_vfs_error_sequence(file);
        write_error=fault;
        assert(kernel_vfs_sync(file,0,&error)==-KERNEL_EIO && kernel_vfs_error_sequence(file)>before);
        struct kernel_page_cache_statistics failed;
        kernel_page_cache_get_statistics(cache,&failed);
        assert(failed.current_pages>=2);
        /* EIO及实际前缀短写都保留脏页，重试后从真实镜像读回本代内容。 */
        assert(!kernel_vfs_sync(file,0,&error));
        assert(!__real_kernel_vfs_node_pread(kernel_vfs_file_node(file),0,&disk_byte,1,&done) &&
            done==1 && disk_byte==0x70+fault);
    }
    for (unsigned i = 0; i < count; i++) {
        if (!aliases[i].entry) continue;
        kernel_page_cache_alias_detach(&aliases[i]);
        kernel_page_cache_alias_drain(&aliases[i]);
    }
    assert(!physical_page_release(pages, address));
    assert(!kernel_io_context_current()->cache_writes);
}
static void *read_expanded_index(void *arg)
{
    unsigned id=(unsigned)(uintptr_t)arg; mm_test_attach(id);
    for (unsigned index=id-1;index<64;index+=4) {
        uint64_t address; size_t valid;
        assert(!kernel_page_cache_get(current_cache,current_file,index,&address,&valid) && valid==BOAROS_PAGE_SIZE);
        for (size_t n=0;n<valid;n++) assert(((unsigned char *)access_page(address))[n]==0x30+index);
        assert(!physical_page_release(current_cache->allocator,address));
    }
    return 0;
}
static void expanded_index_case(struct kernel_page_cache *cache,struct kernel_vfs_file *file,
    struct physical_page_allocator *pages)
{
    unsigned char data[BOAROS_PAGE_SIZE]; size_t done;
    for (unsigned index=0;index<64;index++) {
        memset(data,0x30+index,sizeof(data));
        assert(!kernel_vfs_pwrite(file,(uint64_t)index*BOAROS_PAGE_SIZE,data,sizeof(data),&done) && done==sizeof(data));
    }
    uint64_t observed=kernel_vfs_error_sequence(file);
    assert(!kernel_vfs_sync(file,0,&observed));
    assert(!kernel_page_cache_invalidate_node(cache,kernel_vfs_file_node(file)));
    assert(!kernel_page_cache_start_worker(cache));
    source_reads=read_entered=resume_read=0; pause_read=1;
    pthread_t threads[4];
    for (unsigned id=1;id<=4;id++) assert(!pthread_create(&threads[id-1],0,read_expanded_index,(void *)(uintptr_t)id));
    while (!__atomic_load_n(&read_entered,__ATOMIC_ACQUIRE)) sched_yield();
    struct kernel_page_cache_concurrency_state before,after;
    do { kernel_page_cache_get_concurrency_state(cache,&before); sched_yield(); } while (before.pending!=3);
    __atomic_store_n(&resume_read,1,__ATOMIC_RELEASE);
    for (unsigned id=1;id<=4;id++) assert(!pthread_join(threads[id-1],0));
    assert(source_reads==64);
    kernel_page_cache_get_concurrency_state(cache,&before);
    uint64_t available=physical_page_available(pages);
    for (unsigned index=0;index<64;index++) {
        uint64_t address; size_t valid;
        assert(!kernel_page_cache_get(cache,file,index,&address,&valid) &&
            valid==BOAROS_PAGE_SIZE && *(unsigned char *)access_page(address)==0x30+index);
        assert(!physical_page_release(pages,address));
    }
    kernel_page_cache_get_concurrency_state(cache,&after);
    assert(source_reads==64 && before.requests_queued==after.requests_queued &&
        available==physical_page_available(pages) && !after.pending && !after.running && !after.entry_borrows);
    kernel_page_cache_stop_worker(cache);
    puts("PASS expanded cache index: 64 distinct cold pages, complete contents, allocation/I/O/request-free hits");
}
static void cold_case(struct kernel_page_cache *cache, struct kernel_heap *heap,
    struct physical_page_allocator *pages, const char *path, unsigned actors)
{
    (void)heap;
    image_fd = open(path, O_RDWR); assert(image_fd >= 0);
    struct stat st; assert(!fstat(image_fd, &st));
    struct kernel_block_device disk = {.read = image_read, .write = image_write,
        .flush = image_flush, .capacity_bytes = st.st_size, .logical_block_size = 512,
        .cache_mode = KERNEL_BLOCK_CACHE_WRITEBACK};
    struct kernel_vfs_mount mount = {0}; struct kernel_vfs_file file = {0};
    assert(!kernel_vfs_mount_ext4(&mount, &disk, &backend_heap, cache, 0));
    char name[32]; snprintf(name, sizeof(name), "/cold-%u", actors);
    assert(!kernel_vfs_create(&mount, name, 0600, &file));
    unsigned char bytes[BOAROS_PAGE_SIZE]; memset(bytes, 0x73, sizeof(bytes)); size_t done;
    assert(!kernel_vfs_pwrite(&file, 0, bytes, sizeof(bytes), &done) && done == sizeof(bytes));
    assert(!kernel_vfs_pwrite(&file, BOAROS_PAGE_SIZE, bytes, sizeof(bytes), &done) && done == sizeof(bytes));
    uint64_t error = 0; assert(!kernel_vfs_sync(&file, 0, &error));
    assert(!kernel_page_cache_invalidate_node(cache, kernel_vfs_file_node(&file)));
    assert(!kernel_page_cache_start_worker(cache));
    current_cache = cache; current_file = &file;
    source_reads = read_entered = resume_read = 0; pause_read = 1;
    pthread_t threads[4];
    assert(!pthread_create(&threads[0], 0, get_cold_page, (void *)(uintptr_t)1));
    while (!__atomic_load_n(&read_entered, __ATOMIC_ACQUIRE)) sched_yield();
    for (unsigned i = 2; i <= actors; i++) assert(!pthread_create(&threads[i - 1], 0, get_cold_page, (void *)(uintptr_t)i));
    struct kernel_page_cache_concurrency_state state;
    do { kernel_page_cache_get_concurrency_state(cache, &state); sched_yield(); }
    while (state.entry_borrows != actors);
    assert(state.running == 1 && !state.pending);
    __atomic_store_n(&resume_read, 1, __ATOMIC_RELEASE);
    for (unsigned i = 1; i <= actors; i++) assert(!pthread_join(threads[i - 1], 0) && received[i] == received[1]);
    assert(source_reads == 1);
    kernel_page_cache_get_concurrency_state(cache, &state);
    assert(!state.entry_borrows && !state.pending && !state.running &&
        state.requests_queued == state.requests_completed);
    uint64_t requests = state.requests_queued;
    uint64_t warm; size_t valid; uint64_t available = physical_page_available(pages);
    assert(!kernel_page_cache_get(cache, &file, 0, &warm, &valid) && warm == received[1]);
    assert(source_reads == 1 && physical_page_available(pages) == available);
    kernel_page_cache_get_concurrency_state(cache, &state);
    assert(state.requests_queued == requests);
    assert(!physical_page_release(pages, warm));
    for (unsigned i = 1; i <= actors; i++) assert(!physical_page_release(pages, received[i]));
    assert(!kernel_page_cache_invalidate_node(cache, kernel_vfs_file_node(&file)));
    source_reads = read_entered = resume_read = 0; pause_read = 1;
    source_error = -KERNEL_EIO; expected_status = KERNEL_PAGE_CACHE_STATUS_IO;
    for (unsigned i = 1; i <= actors; i++) assert(!pthread_create(&threads[i - 1], 0, get_cold_page, (void *)(uintptr_t)i));
    while (!__atomic_load_n(&read_entered, __ATOMIC_ACQUIRE)) sched_yield();
    do { kernel_page_cache_get_concurrency_state(cache, &state); sched_yield(); }
    while (state.entry_borrows != actors);
    __atomic_store_n(&resume_read, 1, __ATOMIC_RELEASE);
    for (unsigned i = 1; i <= actors; i++) assert(!pthread_join(threads[i - 1], 0));
    source_error = 0; expected_status = KERNEL_PAGE_CACHE_STATUS_OK;
    assert(source_reads == 1);
    /* queued请求未发布loading；stop不能让active请求的栈或缓冲提前归还。 */
    pause_read = 1; read_entered = resume_read = 0;
    assert(!pthread_create(&threads[0], 0, get_cold_page, (void *)(uintptr_t)1));
    while (!__atomic_load_n(&read_entered, __ATOMIC_ACQUIRE)) sched_yield();
    assert(!pthread_create(&threads[1], 0, get_cancelled_page, 0));
    do { kernel_page_cache_get_concurrency_state(cache, &state); sched_yield(); }
    while (state.pending != 1);
    watch_stop = 1; stop_entered = stop_done = 0;
    pthread_t stopper; assert(!pthread_create(&stopper, 0, stop_cache, 0));
    while (!__atomic_load_n(&stop_entered, __ATOMIC_ACQUIRE)) sched_yield();
    assert(!__atomic_load_n(&stop_done, __ATOMIC_ACQUIRE));
    __atomic_store_n(&resume_read, 1, __ATOMIC_RELEASE);
    assert(!pthread_join(threads[0], 0) && !pthread_join(threads[1], 0) && !pthread_join(stopper, 0));
    watch_stop = 0;
    assert(!physical_page_release(pages, received[1]));
    kernel_page_cache_get_concurrency_state(cache, &state);
    assert(!state.entry_borrows && !state.running && !state.pending && state.requests_completed == state.requests_queued);
#ifdef CACHE_MM_CASES
    assert(!kernel_page_cache_start_worker(cache));
    disk_mm_case(cache, heap, pages, &mount, name);
    kernel_page_cache_stop_worker(cache);
#endif
    alias_snapshot(cache, &file, pages,1);
    alias_snapshot(cache, &file, pages,8);
    alias_snapshot(cache, &file, pages,32);
    expanded_index_case(cache,&file,pages);
    assert(!kernel_vfs_close(&file));
    assert(!kernel_vfs_unmount(&mount));
    assert(!close(image_fd));
    printf("PASS %u-thread real ext4 cold read: CPU0 producer, one page/read, warm hit no allocation\n", actors);
}

int main(int argc, char **argv)
{
    setvbuf(stdout,0,_IONBF,0);
    mm_test_attach(0);
    struct physical_page_allocator pages = {0};
    struct kernel_heap heap = {0};
    struct kernel_page_cache cache = {0};
    struct boot_memory_layout layout = {.usable_count = 1,
        .usable = {{(uintptr_t)pool, sizeof(pool)}}};
    assert(!physical_page_allocator_init(&pages, &layout));
    assert(!physical_page_allocator_bind_access(&pages, access_page));
    assert(!physical_page_allocator_finalize(&pages));
    assert(!kernel_heap_init(&heap, &pages, page_address));
    uint64_t baseline = physical_page_available(&pages);
    struct boot_memory_layout backend_layout={.usable_count=1,
        .usable={{(uintptr_t)backend_pool,sizeof(backend_pool)}}};
    assert(!physical_page_allocator_init(&backend_pages,&backend_layout) &&
        !physical_page_allocator_bind_access(&backend_pages,access_page) &&
        !physical_page_allocator_finalize(&backend_pages) &&
        !kernel_heap_init(&backend_heap,&backend_pages,page_address));
    uint64_t backend_baseline=physical_page_available(&backend_pages);
    assert(!kernel_page_cache_init_controlled(&cache, &heap, &pages));
    struct kernel_page_cache_concurrency_state sizes;
    kernel_page_cache_get_concurrency_state(&cache, &sizes);
    printf("cache sizes: entry=%zu record=%zu request=%zu\n", sizes.entry_bytes, sizes.record_bytes, sizes.request_bytes);
    assert(!pages.reclaimer && !pages.pressure_notify && !pages.pressure_wait &&
        !pages.cache_snapshot && !pages.cache_context);
    assert(kernel_page_cache_init_controlled(&cache, &heap, &pages) ==
        KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT);
    if (argc == 2) {
        cold_case(&cache, &heap, &pages, argv[1], 2);
        cold_case(&cache, &heap, &pages, argv[1], 4);
    }
    assert(!kernel_page_cache_destroy(&cache));
    assert(!physical_page_allocator_audit(&pages));
    assert(physical_page_available(&pages) == baseline);
    struct kernel_heap_statistics backend_stats;
    kernel_heap_get_statistics(&backend_heap,&backend_stats);
    assert(!backend_stats.live_allocations && !backend_stats.current_pages &&
        physical_page_available(&backend_pages)==backend_baseline && !physical_page_allocator_audit(&backend_pages));
    puts("PASS controlled cache construction: no pressure hook, private publication, owner return");
    return 0;
}
