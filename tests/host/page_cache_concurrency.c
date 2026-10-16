#include <kernel/page_cache.h>
#include <ext4.h>
#include <kernel/heap.h>
#include <kernel/cpu.h>
#include <kernel/sync.h>
#include <kernel/block.h>
#include <kernel/vfs.h>
#include <kernel/page.h>
#include <kernel/file_mapping.h>
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
#include <time.h>
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
static unsigned source_short;
static unsigned capture_reads, source_batches, source_batch_completed;
static unsigned source_pages[64], source_batch_sizes[8];
static unsigned source_fault_index;
static int source_fault_error;
static unsigned write_error;
static enum kernel_page_cache_status expected_status;
static unsigned watch_stop, stop_entered, stop_done;
static struct kernel_page_cache *current_cache;
static struct kernel_vfs_file *current_file;
struct read_operation_counts {
    uint64_t metadata_locks, inode_locks, physical_acquires, physical_releases;
    uint64_t descriptor_allocations;
#ifndef CACHE_MM_CASES
    uint64_t cache_allocations;
#endif
};
static struct kernel_rwlock *cache_metadata_lock;
static _Thread_local unsigned observe_cache_metadata;
static _Thread_local struct read_operation_counts *read_counts;
static _Thread_local struct kernel_rwlock *read_inode_lock;
static _Thread_local struct physical_page_allocator *read_allocator;
static _Thread_local struct kernel_rwlock *attempt_inode_lock;
static _Thread_local unsigned *inode_read_attempts;
static _Thread_local unsigned *inode_read_resume;
void __real_kernel_rwlock_read(struct kernel_rwlock *, struct kernel_lock_guard *);
void __wrap_kernel_rwlock_read(struct kernel_rwlock *lock, struct kernel_lock_guard *guard)
{
    if (read_counts && lock == read_inode_lock) read_counts->inode_locks++;
    if (inode_read_attempts && lock==attempt_inode_lock) {
        __atomic_fetch_add(inode_read_attempts,1,__ATOMIC_RELEASE);
        while(inode_read_resume && !__atomic_load_n(inode_read_resume,__ATOMIC_ACQUIRE))sched_yield();
    }
    __real_kernel_rwlock_read(lock, guard);
}
void __real_kernel_rwlock_write(struct kernel_rwlock *, struct kernel_lock_guard *);
void __wrap_kernel_rwlock_write(struct kernel_rwlock *lock, struct kernel_lock_guard *guard)
{
    /* mutex_lock是inline；记录它实际调用的write，指针来自真实cache状态读取。 */
    if (observe_cache_metadata) {
        assert(!cache_metadata_lock || cache_metadata_lock == lock);
        cache_metadata_lock = lock;
    }
    if (read_counts && lock == cache_metadata_lock) read_counts->metadata_locks++;
    __real_kernel_rwlock_write(lock, guard);
}
int __real_kernel_rwlock_try_write(struct kernel_rwlock *,struct kernel_lock_guard *);
int __wrap_kernel_rwlock_try_write(struct kernel_rwlock *lock,struct kernel_lock_guard *guard)
{
    if (read_counts && lock==cache_metadata_lock) read_counts->metadata_locks++;
    return __real_kernel_rwlock_try_write(lock,guard);
}
enum physical_page_status __real_physical_page_acquire(struct physical_page_allocator *, uint64_t);
enum physical_page_status __wrap_physical_page_acquire(struct physical_page_allocator *pages, uint64_t address)
{
    if (read_counts && pages == read_allocator) read_counts->physical_acquires++;
    return __real_physical_page_acquire(pages, address);
}
enum physical_page_status __real_physical_page_release(struct physical_page_allocator *, uint64_t);
static const uint64_t *tracked_version_pages;
static size_t tracked_version_count;
static _Thread_local size_t *version_releases;
static struct physical_page_allocator *destroy_allocator;
static uint64_t destroy_idle_page;
static unsigned pause_version_free,version_free_entered,resume_version_free;
enum physical_page_status __wrap_physical_page_release(struct physical_page_allocator *pages, uint64_t address)
{
    if (read_counts && pages == read_allocator) read_counts->physical_releases++;
    if(pages==destroy_allocator && address==destroy_idle_page &&
        __atomic_exchange_n(&pause_version_free,0,__ATOMIC_ACQ_REL)) {
        assert(!kernel_cpu_current()->raw_locks);
        __atomic_store_n(&version_free_entered,1,__ATOMIC_RELEASE);
        while(!__atomic_load_n(&resume_version_free,__ATOMIC_ACQUIRE))sched_yield();
    }
    if (version_releases && current_cache && pages==current_cache->allocator)
        for(size_t i=0;i<tracked_version_count;i++)if(address==tracked_version_pages[i])(*version_releases)++;
    return __real_physical_page_release(pages, address);
}
struct writeback_operation_counts {
    size_t allocations, large_allocations, largest, batches, maximum_pages;
    uint64_t maximum_borrows;
    const struct file_write_span *spans;
};
static _Thread_local struct writeback_operation_counts *write_counts;
static _Thread_local unsigned fail_workspace_allocation;
static _Thread_local size_t *demand_heap_allocations;
struct retained_version {
    struct file_data_source source;
    const unsigned char *data;
};
static _Thread_local struct retained_version *retain_version;
static struct retained_version late_version;
static struct retained_version after_destroy_version;
static struct retained_version *retain_background_version;
static unsigned reject_background_allocations,rejected_background_allocations;
static unsigned version_cycle_active,version_cycle_submissions;
static unsigned cycle_fg_entered,cycle_fg_resume,cycle_alloc_entered,cycle_alloc_resume,cycle_pause_allocation;
static unsigned cycle_background_waiting;
enum kernel_scheduler_status __real_kernel_wait_prepare(struct kernel_wait_queue *,uint64_t,int,struct kernel_wait_token *);
enum kernel_scheduler_status __wrap_kernel_wait_prepare(struct kernel_wait_queue *queue,
    uint64_t deadline,int interruptible,struct kernel_wait_token *token)
{
    enum kernel_scheduler_status status=__real_kernel_wait_prepare(queue,deadline,interruptible,token);
    if (!status && kernel_io_context_current()->background_reclaim &&
        __atomic_load_n(&cycle_alloc_entered,__ATOMIC_ACQUIRE) &&
        __atomic_load_n(&cycle_alloc_resume,__ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&cycle_fg_resume,__ATOMIC_ACQUIRE))
        __atomic_store_n(&cycle_background_waiting,1,__ATOMIC_RELEASE);
    return status;
}
enum kernel_heap_status __real_kernel_heap_allocate(struct kernel_heap *,size_t,void **);
enum kernel_heap_status __wrap_kernel_heap_allocate(struct kernel_heap *heap,size_t bytes,void **out)
{
    if (current_cache && heap==current_cache->heap) {
        unsigned reject=__atomic_load_n(&reject_background_allocations,__ATOMIC_ACQUIRE);
        if (reject && (reject>1 || kernel_io_context_current()->background_reclaim)) {
            __atomic_fetch_add(&rejected_background_allocations,1,__ATOMIC_RELEASE);
            if(kernel_io_context_current()->background_reclaim &&
                __atomic_exchange_n(&cycle_pause_allocation,0,__ATOMIC_ACQ_REL)) {
                assert(!kernel_cpu_current()->raw_locks);
                __atomic_store_n(&cycle_alloc_entered,1,__ATOMIC_RELEASE);
                while(!__atomic_load_n(&cycle_alloc_resume,__ATOMIC_ACQUIRE))sched_yield();
            }
            return KERNEL_HEAP_STATUS_EMPTY;
        }
        if (read_counts && bytes>256) read_counts->descriptor_allocations++;
        if (demand_heap_allocations) (*demand_heap_allocations)++;
        if (fail_workspace_allocation && !--fail_workspace_allocation) return KERNEL_HEAP_STATUS_EMPTY;
    }
    if(write_counts && current_cache && heap==current_cache->heap) {
        write_counts->allocations++;
        if(bytes>256)write_counts->large_allocations++;
        if(bytes>write_counts->largest)write_counts->largest=bytes;
    }
    return __real_kernel_heap_allocate(heap,bytes,out);
}
int __real_ext4_fpwrite_batch(ext4_file *,struct file_write_span *,unsigned);
int __wrap_ext4_fpwrite_batch(ext4_file *file,struct file_write_span *spans,unsigned count)
{
    if(write_counts) {
        if((const void *)spans!=(const void *)write_counts->spans)
            fprintf(stderr,"writeback descriptors copied: VFS=%p ext4=%p\n",(const void *)write_counts->spans,(void *)spans);
        assert((const void *)spans==(const void *)write_counts->spans);
    }
    return __real_ext4_fpwrite_batch(file,spans,count);
}
static unsigned execute_calls;
static void execute_on_cpu0(void *argument)
{
    assert(kernel_cpu_current()->logical_id==0 && !kernel_cpu_current()->raw_locks &&
        !kernel_io_context_current()->locks);
    assert(argument==&execute_calls);
    execute_calls++;
}
static void *execute_client(void *unused)
{
    (void)unused; mm_test_attach(1);
    assert(!kernel_page_cache_run_cpu0(current_cache,execute_on_cpu0,&execute_calls));
    return 0;
}
static void execution_case(struct kernel_page_cache *cache)
{
    current_cache=cache; execute_calls=0;
    assert(!kernel_page_cache_start_worker(cache));
    pthread_t client;
    assert(!pthread_create(&client,0,execute_client,0) && !pthread_join(client,0));
    assert(execute_calls==1);
    kernel_page_cache_stop_worker(cache);
    struct kernel_page_cache_concurrency_state state;
    kernel_page_cache_get_concurrency_state(cache,&state);
    assert(!state.pending && !state.running && state.requests_queued==state.requests_completed);
    puts("PASS CPU0 execution completes outside cache protection before caller unwind");
}
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
static void source_read_begin(uint64_t off)
{
    assert(kernel_cpu_current()->hardware_id == 0 && "cold miss escaped CPU0 executor");
    __atomic_fetch_add(&source_reads, 1, __ATOMIC_RELAXED);
    if (capture_reads) {
        assert(!(off & (BOAROS_PAGE_SIZE - 1U)) && off / BOAROS_PAGE_SIZE < 64);
        __atomic_fetch_add(&source_pages[off / BOAROS_PAGE_SIZE], 1, __ATOMIC_RELAXED);
    }
}
static void source_read_pause(void)
{
    if (__atomic_exchange_n(&pause_read, 0, __ATOMIC_ACQ_REL)) {
        assert(!kernel_cpu_current()->raw_locks);
        __atomic_store_n(&read_entered, 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&resume_read, __ATOMIC_ACQUIRE)) sched_yield();
    }
}
int __wrap_kernel_vfs_node_pread(struct kernel_vfs_node *node, uint64_t off, void *out, size_t size, size_t *done)
{
    source_read_begin(off);
    source_read_pause();
    if (source_error) { *done = 0; return source_error; }
    int result = __real_kernel_vfs_node_pread(node, off, out, size, done);
    if (!result && source_short && *done) --*done;
    return result;
}
int __real_kernel_vfs_node_pread_batch(struct kernel_vfs_node *, struct file_read_span *, size_t, struct file_read_workspace *);
int __wrap_kernel_vfs_node_pread_batch(struct kernel_vfs_node *node,
    struct file_read_span *spans, size_t count, struct file_read_workspace *workspace)
{
    assert(count && count <= KERNEL_VFS_READ_BATCH_MAX);
    for (size_t i = 0; i < count; i++) {
        source_read_begin(spans[i].offset);
#ifndef CACHE_MM_CASES
        if (read_counts) {
            const unsigned char *bytes=spans[i].buffer;
            assert(bytes[0]==0xd7 && bytes[BOAROS_PAGE_SIZE-1]==0xd7);
        }
#endif
    }
    unsigned batch = __atomic_fetch_add(&source_batches, 1, __ATOMIC_RELAXED);
    if (capture_reads) {
        assert(batch < sizeof(source_batch_sizes) / sizeof(source_batch_sizes[0]));
        source_batch_sizes[batch] = (unsigned)count;
    }
    source_read_pause();
    struct file_read_span kept[KERNEL_VFS_READ_BATCH_MAX];
    size_t destinations[KERNEL_VFS_READ_BATCH_MAX], used = 0;
    int injected = source_error;
    for (size_t i = 0; i < count; i++) {
        int error = source_error;
        if (!error && source_fault_error && spans[i].offset / BOAROS_PAGE_SIZE == source_fault_index)
            error = source_fault_error;
        if (error) {
            spans[i].completed = 0;
            spans[i].error = error;
            injected = error;
        } else {
            destinations[used] = i;
            kept[used++] = spans[i];
        }
    }
    int result = used ? __real_kernel_vfs_node_pread_batch(node, kept, used, workspace) : 0;
    for (size_t i = 0; i < used; i++) {
        spans[destinations[i]] = kept[i];
        if (source_short && !spans[destinations[i]].error && spans[destinations[i]].completed)
            --spans[destinations[i]].completed;
    }
    __atomic_fetch_add(&source_batch_completed, 1, __ATOMIC_RELEASE);
    return result ? result : injected;
}
#ifndef CACHE_MM_CASES
static unsigned fail_cache_page, fail_cache_entry;
enum physical_page_status __real_physical_page_allocate(struct physical_page_allocator *, uint64_t *);
enum physical_page_status __wrap_physical_page_allocate(struct physical_page_allocator *pages, uint64_t *out)
{
    if (fail_cache_page && current_cache && pages == current_cache->allocator && !--fail_cache_page)
        return PHYSICAL_PAGE_STATUS_EMPTY;
    enum physical_page_status status=__real_physical_page_allocate(pages,out);
    /* 读取生产者应直接填充未初始化页，不能先清整页再用I/O覆盖。 */
    if (!status && read_counts && current_cache && pages==current_cache->allocator)
        memset(access_page(*out),0xd7,BOAROS_PAGE_SIZE);
    return status;
}
enum kernel_heap_status __real_kernel_heap_allocate_zeroed(struct kernel_heap *, size_t, size_t, void **);
enum kernel_heap_status __wrap_kernel_heap_allocate_zeroed(struct kernel_heap *heap, size_t count,
    size_t size, void **out)
{
    if (read_counts && current_cache && heap == current_cache->heap) {
        /* inode资格已经阻止分配等待回收I/O，不再套一次相同用途的计数。 */
        assert(kernel_lock_held(&kernel_vfs_file_node(current_file)->io_lock, 0));
        assert(!kernel_io_context_current()->allocation_depth);
        read_counts->cache_allocations++;
    }
    if (fail_cache_entry && current_cache && heap == current_cache->heap && !--fail_cache_entry)
        return KERNEL_HEAP_STATUS_EMPTY;
    return __real_kernel_heap_allocate_zeroed(heap, count, size, out);
}
#endif
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
static int cycle_rearm_foreground(void *owner,uint64_t address)
{
    (void)owner;(void)address;
    __atomic_store_n(&cycle_fg_entered,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&cycle_fg_resume,__ATOMIC_ACQUIRE))sched_yield();
    return 0;
}
int __real_kernel_vfs_node_writeback(struct kernel_vfs_node *,struct file_write_span *,size_t);
int __wrap_kernel_vfs_node_writeback(struct kernel_vfs_node *node,struct file_write_span *spans,size_t count)
{
    if(__atomic_load_n(&version_cycle_active,__ATOMIC_ACQUIRE))
        __atomic_fetch_add(&version_cycle_submissions,1,__ATOMIC_RELEASE);
    struct retained_version *retained=retain_version;
    if (!retained && kernel_io_context_current()->background_reclaim)
        retained=__atomic_load_n(&retain_background_version,__ATOMIC_ACQUIRE);
    if (retained) {
        assert(count==1 && !retained->source.context);
        spans[0].source.retain(spans[0].source.context);
        retained->source=spans[0].source;
        retained->data=spans[0].buffer;
    }
    if(write_counts) {
        write_counts->batches++;
        if(count>write_counts->maximum_pages)write_counts->maximum_pages=count;
        if(!write_counts->spans)write_counts->spans=spans;
        else assert(write_counts->spans==spans);
        struct kernel_page_cache_concurrency_state state;
        kernel_page_cache_get_concurrency_state(current_cache,&state);
        if(state.entry_borrows>write_counts->maximum_borrows)write_counts->maximum_borrows=state.entry_borrows;
    }
    for (size_t i=0;i<count;i++) {
        size_t block=node->instance->writeback_block_size;
        uintptr_t base=(uintptr_t)spans[i].source_buffer, input=(uintptr_t)spans[i].buffer;
        assert(base && input>=base && spans[i].size<=spans[i].source_size-(input-base));
        if (block<=BOAROS_PAGE_SIZE) {
            assert(spans[i].source_size%block==0 && input-base==spans[i].offset%block);
            assert(spans[i].source_size==((spans[i].size+input-base+block-1)/block)*block);
        }
    }
    if (__atomic_exchange_n(&pause_write,0,__ATOMIC_ACQ_REL)) {
        __atomic_store_n(&write_entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&resume_write,__ATOMIC_ACQUIRE)) sched_yield();
    }
    unsigned fault=__atomic_exchange_n(&write_error,0,__ATOMIC_ACQ_REL);
    if (fault) {
        for (size_t i=0;i<count;i++) spans[i].written=0;
        if (fault==2 && count && spans[0].size>1) {
            struct file_write_span prefix=spans[0]; prefix.size/=2;
            int result=__real_kernel_vfs_node_writeback(node,&prefix,1);
            assert(!result && prefix.written==prefix.size);
            spans[0].written=prefix.written;
        }
        kernel_vfs_record_writeback_error(node,-KERNEL_EIO);
        return fault==2?0:-KERNEL_EIO;
    }
    return __real_kernel_vfs_node_writeback(node,spans,count);
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
static void sparse_cache_growth(struct kernel_page_cache *cache,struct kernel_vfs_mount *mount,
    unsigned actors)
{
    char name[48]; snprintf(name,sizeof(name),"/sparse-growth-%u",actors);
    struct kernel_vfs_file file={0}; assert(!kernel_vfs_create(mount,name,0600,&file));
    uint64_t end=2*BOAROS_PAGE_SIZE+3; size_t count;
    assert(!kernel_vfs_pwrite(&file,end,"end",3,&count) && count==3);
    unsigned char bytes[80]; memset(bytes,0xcc,sizeof(bytes));
    assert(!kernel_vfs_pread(&file,end-45,bytes,sizeof(bytes),&count) && count==48);
    for (unsigned i=0;i<45;i++) assert(!bytes[i]);
    assert(!memcmp(bytes+45,"end",3));
    uint64_t address; size_t valid;
    assert(!kernel_page_cache_get(cache,&file,0,&address,&valid) && valid==BOAROS_PAGE_SIZE);
    for (size_t i=0;i<valid;i++) assert(!((unsigned char *)access_page(address))[i]);
    assert(!physical_page_release(cache->allocator,address));
    uint64_t observed=kernel_vfs_error_sequence(&file);
    assert(!kernel_vfs_sync(&file,0,&observed));
    assert(!kernel_vfs_close(&file));
    puts("PASS sparse cache growth: uncached zero gap below logical EOF before backend size acceptance");
}

static void partial_snapshot(struct kernel_page_cache *cache,struct kernel_vfs_file *file,
    struct physical_page_allocator *pages)
{
    uint64_t observed=kernel_vfs_error_sequence(file);
    assert(!kernel_vfs_sync(file,0,&observed));
    unsigned char before=0xa3; size_t count;
    assert(!kernel_vfs_pwrite(file,17,&before,1,&count) && count==1);
    uint64_t address; size_t valid;
    assert(!kernel_page_cache_get(cache,file,0,&address,&valid));
    struct kernel_page_cache_alias alias={0};
    assert(!kernel_page_cache_alias_attach(cache,file,0,address,&alias,
        kernel_vfs_file_node(file),0x80000,rearm_alias));
    write_entered=resume_write=sync_done=0; pause_write=1;
    pthread_t sync; assert(!pthread_create(&sync,0,sync_file,file));
    while (!__atomic_load_n(&write_entered,__ATOMIC_ACQUIRE)) sched_yield();
    struct kernel_page_cache_write_borrow writer={0};
    assert(!kernel_page_cache_alias_write_begin(&alias,&writer));
    ((unsigned char *)access_page(address))[17]=0xb4;
    kernel_page_cache_alias_mark_dirty(&alias); kernel_page_cache_write_end(&writer);
    __atomic_store_n(&resume_write,1,__ATOMIC_RELEASE);
    assert(!pthread_join(sync,0));
    unsigned char actual;
    assert(!__real_kernel_vfs_node_pread(kernel_vfs_file_node(file),17,&actual,1,&count) && count==1 && actual==before);
    assert(!kernel_vfs_sync(file,0,&observed));
    assert(!__real_kernel_vfs_node_pread(kernel_vfs_file_node(file),17,&actual,1,&count) && count==1 && actual==0xb4);
    kernel_page_cache_alias_detach(&alias); kernel_page_cache_alias_drain(&alias);
    assert(!physical_page_release(pages,address));
    puts("PASS one-byte stable block: concurrent alias mutation preserves old borrowed bytes and next dirty generation");
}

static void alias_snapshot(struct kernel_page_cache *cache, struct kernel_vfs_file *file,
    struct physical_page_allocator *pages,unsigned count,int force_retry)
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
    struct kernel_page_cache_statistics before_retry;
    if (force_retry) {
        assert(!kernel_page_cache_start_worker(cache));
        struct kernel_page_cache_concurrency_state before_round,after_round;
        kernel_page_cache_get_concurrency_state(cache,&before_round);
        kernel_page_cache_get_statistics(cache,&before_retry);
        assert(!kernel_page_cache_request_writeback(cache));
        /* Only a completed background pass advances this value; mutex
         * contention or unrelated CPU0 parks cannot satisfy the handshake. */
        do {
            kernel_page_cache_get_concurrency_state(cache,&after_round);
            sched_yield();
        } while (after_round.writeback_rounds==before_round.writeback_rounds);
    }
    __atomic_store_n(&resume_write,1,__ATOMIC_RELEASE);
    assert(!pthread_join(sync,0));
    if (force_retry) {
        struct timespec start,now; assert(!clock_gettime(CLOCK_MONOTONIC,&start));
        for (;;) {
            struct kernel_page_cache_statistics after;
            kernel_page_cache_get_statistics(cache,&after);
            if (after.worker_written>before_retry.worker_written) break;
            assert(!clock_gettime(CLOCK_MONOTONIC,&now));
            assert(now.tv_sec-start.tv_sec<5 && "accepted force writeback lost its completion notification");
            sched_yield();
        }
        kernel_page_cache_stop_worker(cache);
    }
    unsigned char disk_byte; size_t done;
    assert(!__real_kernel_vfs_node_pread(kernel_vfs_file_node(file),0,&disk_byte,1,&done) && done==1 && disk_byte==(force_retry?0x66:0x41));
    uint64_t error=kernel_vfs_error_sequence(file); assert(!kernel_vfs_sync(file,0,&error));
    assert(!__real_kernel_vfs_node_pread(kernel_vfs_file_node(file),0,&disk_byte,1,&done) && done==1 && disk_byte==0x66);
    if (force_retry) printf("PASS forced writeback retry: %u aliases, completed skip round, newer generation persisted\n",count);
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

static void wait_writeback_round(struct kernel_page_cache *cache, uint64_t before)
{
    struct timespec start, now; assert(!clock_gettime(CLOCK_MONOTONIC,&start));
    for (;;) {
        struct kernel_page_cache_concurrency_state state;
        kernel_page_cache_get_concurrency_state(cache,&state);
        if (state.writeback_rounds>before) return;
        assert(!clock_gettime(CLOCK_MONOTONIC,&now) && now.tv_sec-start.tv_sec<5);
        sched_yield();
    }
}
static void foreground_windows(struct kernel_page_cache *cache,struct kernel_vfs_file *file)
{
    current_cache=cache;
    enum { COUNT=128 };
    unsigned char data[BOAROS_PAGE_SIZE];memset(data,0x58,sizeof(data));size_t done;
    uint64_t observed=kernel_vfs_error_sequence(file);
    assert(!kernel_vfs_sync(file,0,&observed));
    for(unsigned i=0;i<COUNT;i++)
        assert(!kernel_vfs_pwrite(file,(uint64_t)(COUNT-1-i)*BOAROS_PAGE_SIZE,data,sizeof(data),&done) && done==sizeof(data));
    struct writeback_operation_counts counts={0};write_counts=&counts;
    assert(!kernel_vfs_sync(file,0,&observed));write_counts=NULL;
    printf("foreground windows: pages=%u allocations=%zu descriptors=%zu largest=%zu batches=%zu maxpages=%zu\n",
        COUNT,counts.allocations,counts.large_allocations,counts.largest,counts.batches,counts.maximum_pages);
    assert(!counts.large_allocations && !counts.allocations);
    assert(counts.maximum_pages<=131072/BOAROS_PAGE_SIZE && counts.largest<=BOAROS_PAGE_SIZE);
    assert(counts.maximum_borrows<=2*(131072/BOAROS_PAGE_SIZE));
    for(unsigned i=0;i<COUNT;i++) {
        unsigned char actual;
        assert(!__real_kernel_vfs_node_pread(kernel_vfs_file_node(file),(uint64_t)i*BOAROS_PAGE_SIZE,&actual,1,&done) && done==1 && actual==0x58);
    }
    unsigned char one=0x76;
    assert(!kernel_vfs_pwrite(file,17,&one,1,&done) && done==1);
    counts=(struct writeback_operation_counts){0};write_counts=&counts;
    assert(!kernel_vfs_sync(file,0,&observed));write_counts=NULL;
    assert(!counts.allocations && !counts.large_allocations && counts.batches==1 && counts.maximum_pages==1);
    puts("PASS foreground writeback: reused bounded descriptors across sync calls/windows, scalar page uses no descriptors");
}
static void version_reuse_case(struct kernel_page_cache *cache,struct kernel_vfs_file *file)
{
    current_cache=cache;
    struct retained_version first={0},second={0},third={0};
    struct retained_version *versions[3]={&first,&second,&third};
    size_t done;
    uint64_t observed=kernel_vfs_error_sequence(file);
    for(unsigned i=0;i<3;i++) {
        unsigned char byte=0x91+i;
        assert(!kernel_vfs_pwrite(file,17,&byte,1,&done) && done==1);
        if(i==2) {
            second.source.release(second.source.context);
            assert(*first.data==0x91);
        }
        struct writeback_operation_counts counts={0};write_counts=&counts;
        retain_version=versions[i];
        assert(!kernel_vfs_sync(file,0,&observed));
        retain_version=NULL;write_counts=NULL;
        assert(*versions[i]->data==byte && *first.data==0x91);
        if(i==1)assert(first.source.context!=second.source.context && first.data!=second.data);
        if(i==2) {
            printf("stable version reuse: allocations=%zu old=%p reused=%p\n",counts.allocations,
                first.source.context,third.source.context);
            assert(!counts.allocations && third.source.context==second.source.context && third.data==second.data);
        }
    }
    first.source.release(first.source.context);third.source.release(third.source.context);
    puts("PASS stable versions: retained old bytes, distinct active owner, idle object and page reused without allocation");
}
static void retain_generation(struct kernel_vfs_file *file,unsigned char byte,struct retained_version *version)
{
    size_t done;uint64_t observed=kernel_vfs_error_sequence(file);
    retain_version=version;
    __atomic_store_n(&retain_background_version,version,__ATOMIC_RELEASE);
    assert(!kernel_vfs_pwrite(file,17,&byte,1,&done) && done==1);
    assert(!kernel_vfs_sync(file,0,&observed));retain_version=NULL;
    __atomic_store_n(&retain_background_version,NULL,__ATOMIC_RELEASE);
    assert(version->source.context && *version->data==byte);
}
static void version_capacity_case(struct kernel_page_cache *cache,struct kernel_vfs_file *file)
{
    enum { CAPACITY=262144U/BOAROS_PAGE_SIZE, COUNT=CAPACITY+3 };
    struct retained_version versions[COUNT];memset(versions,0,sizeof(versions));
    uint64_t addresses[COUNT];
    for(unsigned i=0;i<COUNT;i++) {
        retain_generation(file,0x20+i,&versions[i]);
        addresses[i]=(uintptr_t)versions[i].data&~BOAROS_PAGE_MASK;
        for(unsigned j=0;j<i;j++)assert(addresses[i]!=addresses[j] && *versions[j].data==0x20+j);
    }
    uint64_t pinned;size_t valid;
    assert(!kernel_page_cache_get(cache,file,0,&pinned,&valid));
    size_t released=0;version_releases=&released;
    tracked_version_pages=addresses;tracked_version_count=COUNT;
    for(unsigned i=0;i<COUNT;i++)versions[i].source.release(versions[i].source.context);
    assert(released==COUNT-CAPACITY);
    assert(!kernel_page_cache_start_worker(cache));
    unsigned excess=CAPACITY-BOAROS_PAGE_CACHE_WRITEBACK_PAGES;
    assert(kernel_page_cache_reclaim(cache,excess)==excess && released==COUNT-BOAROS_PAGE_CACHE_WRITEBACK_PAGES);
    rejected_background_allocations=0;__atomic_store_n(&reject_background_allocations,1,__ATOMIC_RELEASE);
    unsigned char byte=0xd1;size_t done;
    assert(!kernel_vfs_pwrite(file,17,&byte,1,&done) && done==1);
    struct kernel_page_cache_concurrency_state before;
    kernel_page_cache_get_concurrency_state(cache,&before);
    assert(!kernel_page_cache_request_writeback(cache));wait_writeback_round(cache,before.writeback_rounds);
    assert(!__atomic_load_n(&rejected_background_allocations,__ATOMIC_ACQUIRE));
    __atomic_store_n(&reject_background_allocations,0,__ATOMIC_RELEASE);
    kernel_page_cache_stop_worker(cache);
    assert(kernel_page_cache_reclaim(cache,BOAROS_PAGE_CACHE_WRITEBACK_PAGES)==BOAROS_PAGE_CACHE_WRITEBACK_PAGES && released==COUNT);
    version_releases=NULL;tracked_version_pages=NULL;tracked_version_count=0;
    assert(!physical_page_release(cache->allocator,pinned));
    printf("PASS version pool capacity: idle=%u pages <=256KiB, pressure frees surplus before cache pages, %u-page reserve writes with allocations rejected\n",
        CAPACITY,(unsigned)BOAROS_PAGE_CACHE_WRITEBACK_PAGES);
}
static void version_oom_case(struct kernel_page_cache *cache,struct kernel_vfs_file *file)
{
    enum { COUNT=BOAROS_PAGE_CACHE_WRITEBACK_PAGES };
    struct retained_version versions[COUNT],replacement={0};memset(versions,0,sizeof(versions));
    unsigned parked=mm_test_parked(0);
    assert(!kernel_page_cache_start_worker(cache));
    while(mm_test_parked(0)==parked)sched_yield();
    for(unsigned i=0;i<COUNT;i++)retain_generation(file,0x40+i,&versions[i]);
    rejected_background_allocations=0;__atomic_store_n(&reject_background_allocations,1,__ATOMIC_RELEASE);
    __atomic_store_n(&retain_background_version,&replacement,__ATOMIC_RELEASE);
    unsigned char byte=0xe2;size_t done;
    assert(!kernel_vfs_pwrite(file,17,&byte,1,&done) && done==1);
    struct kernel_page_cache_concurrency_state before;
    kernel_page_cache_get_concurrency_state(cache,&before);parked=mm_test_parked(0);
    assert(!kernel_page_cache_request_writeback(cache));
    while(!__atomic_load_n(&rejected_background_allocations,__ATOMIC_ACQUIRE) || mm_test_parked(0)==parked)sched_yield();
    for(unsigned i=0;i<COUNT;i++)assert(*versions[i].data==0x40+i);
    void *returned=versions[0].source.context;
    versions[0].source.release(returned);versions[0].source.context=NULL;
    wait_writeback_round(cache,before.writeback_rounds);
    assert(replacement.source.context==returned && *replacement.data==byte);
    __atomic_store_n(&retain_background_version,NULL,__ATOMIC_RELEASE);
    byte=0xe3;assert(!kernel_vfs_pwrite(file,17,&byte,1,&done) && done==1);
    rejected_background_allocations=0;parked=mm_test_parked(0);
    assert(!kernel_page_cache_request_writeback(cache));
    while(!__atomic_load_n(&rejected_background_allocations,__ATOMIC_ACQUIRE) || mm_test_parked(0)==parked)sched_yield();
    kernel_page_cache_stop_worker(cache);
    __atomic_store_n(&reject_background_allocations,0,__ATOMIC_RELEASE);
    replacement.source.release(replacement.source.context);
    for(unsigned i=1;i<COUNT;i++)versions[i].source.release(versions[i].source.context);
    uint64_t observed=kernel_vfs_error_sequence(file);assert(!kernel_vfs_sync(file,0,&observed));
    puts("PASS version OOM: retained owners block reuse, actual last release resumes background, stop cancels an allocation-starved wait");
}
static void version_prefix_cycle(struct kernel_page_cache *cache,struct kernel_vfs_file *file)
{
    enum { COUNT=BOAROS_PAGE_CACHE_WRITEBACK_PAGES };
    if(COUNT<2)return;
    (void)kernel_page_cache_reclaim(cache,UINT64_MAX);
    unsigned char byte=0x6a;size_t done,valid;
    for(unsigned i=1;i<=COUNT;i++)assert(!kernel_vfs_pwrite(file,(uint64_t)i*BOAROS_PAGE_SIZE+17,&byte,1,&done) && done==1);
    uint64_t address,foreground_address;
    assert(!kernel_page_cache_get(cache,file,COUNT-1,&address,&valid));
    assert(!kernel_page_cache_get(cache,file,0,&foreground_address,&valid));
    struct kernel_page_cache_alias alias={0};
    struct kernel_page_cache_alias foreground_alias={0};
    assert(!kernel_page_cache_alias_attach(cache,file,COUNT-1,address,&alias,kernel_vfs_file_node(file),0x99000,rearm_alias));
    assert(!kernel_page_cache_alias_attach(cache,file,0,foreground_address,&foreground_alias,kernel_vfs_file_node(file),0x9a000,cycle_rearm_foreground));
    alias_entered=resume_alias=0;pause_alias=1;
    cycle_fg_entered=cycle_fg_resume=cycle_alloc_entered=cycle_alloc_resume=0;cycle_pause_allocation=1;
    rejected_background_allocations=version_cycle_submissions=0;
    __atomic_store_n(&version_cycle_active,1,__ATOMIC_RELEASE);
    __atomic_store_n(&reject_background_allocations,1,__ATOMIC_RELEASE);
    assert(!kernel_page_cache_start_worker(cache));
    struct kernel_page_cache_concurrency_state before;
    kernel_page_cache_get_concurrency_state(cache,&before);
    assert(!kernel_page_cache_request_writeback(cache));
    while(!__atomic_load_n(&alias_entered,__ATOMIC_ACQUIRE))sched_yield();
    /* Background owns COUNT-1 entries and versions. A new lower page lets
     * foreground take the last idle version before waiting on that prefix. */
    byte=0x5e;assert(!kernel_vfs_pwrite(file,17,&byte,1,&done) && done==1);
    sync_done=0;
    pthread_t foreground;assert(!pthread_create(&foreground,NULL,sync_file,file));
    while(!__atomic_load_n(&cycle_fg_entered,__ATOMIC_ACQUIRE))sched_yield();
    __atomic_store_n(&resume_alias,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&cycle_alloc_entered,__ATOMIC_ACQUIRE))sched_yield();
    unsigned parked=mm_test_parked(0);
    __atomic_store_n(&cycle_fg_resume,1,__ATOMIC_RELEASE);
    while(mm_test_parked(0)==parked)sched_yield();
    __atomic_store_n(&cycle_alloc_resume,1,__ATOMIC_RELEASE);
    puts("version prefix handshake: foreground owns last version and waits on background; background allocation returned OOM");
    while(!__atomic_load_n(&version_cycle_submissions,__ATOMIC_ACQUIRE))sched_yield();
    assert(!pthread_join(foreground,NULL));wait_writeback_round(cache,before.writeback_rounds);
    __atomic_store_n(&reject_background_allocations,0,__ATOMIC_RELEASE);
    __atomic_store_n(&version_cycle_active,0,__ATOMIC_RELEASE);
    kernel_page_cache_stop_worker(cache);
    kernel_page_cache_alias_detach(&alias);kernel_page_cache_alias_drain(&alias);
    kernel_page_cache_alias_detach(&foreground_alias);kernel_page_cache_alias_drain(&foreground_alias);
    assert(!physical_page_release(cache->allocator,address));
    assert(!physical_page_release(cache->allocator,foreground_address));
    for(unsigned i=0;i<=COUNT;i++) {
        unsigned char actual;
        assert(!__real_kernel_vfs_node_pread(kernel_vfs_file_node(file),(uint64_t)i*BOAROS_PAGE_SIZE+17,&actual,1,&done) && done==1 && actual==(i?0x6a:0x5e));
    }
    puts("PASS version OOM interleave: submitted prepared background prefix before waiting on foreground's last version");
}
static int cycle_sync_result;
static void *cycle_sync_file(void *argument)
{
    mm_test_attach(0);uint64_t observed=kernel_vfs_error_sequence(argument);
    cycle_sync_result=kernel_vfs_sync(argument,0,&observed);return NULL;
}
static void version_empty_cycle(struct kernel_page_cache *cache,struct kernel_vfs_file *file)
{
    enum { COUNT=BOAROS_PAGE_CACHE_WRITEBACK_PAGES };
    if(COUNT<2)return;
    (void)kernel_page_cache_reclaim(cache,UINT64_MAX);
    unsigned parked=mm_test_parked(0);assert(!kernel_page_cache_start_worker(cache));
    while(mm_test_parked(0)==parked)sched_yield();
    struct retained_version held[COUNT-1];memset(held,0,sizeof(held));
    for(unsigned i=0;i<COUNT-1;i++)retain_generation(file,0x20+i,&held[i]);
    unsigned char byte=0xa1;size_t done,valid;uint64_t address;
    assert(!kernel_vfs_pwrite(file,17,&byte,1,&done) && done==1);
    byte=0xa2;assert(!kernel_vfs_pwrite(file,BOAROS_PAGE_SIZE+17,&byte,1,&done) && done==1);
    assert(!kernel_page_cache_get(cache,file,0,&address,&valid));
    struct kernel_page_cache_alias alias={0};
    assert(!kernel_page_cache_alias_attach(cache,file,0,address,&alias,kernel_vfs_file_node(file),0x9b000,cycle_rearm_foreground));
    cycle_fg_entered=cycle_fg_resume=cycle_alloc_entered=cycle_alloc_resume=0;cycle_pause_allocation=1;
    pthread_t foreground;assert(!pthread_create(&foreground,NULL,cycle_sync_file,file));
    while(!__atomic_load_n(&cycle_fg_entered,__ATOMIC_ACQUIRE))sched_yield();
    version_cycle_submissions=rejected_background_allocations=0;
    __atomic_store_n(&version_cycle_active,1,__ATOMIC_RELEASE);
    __atomic_store_n(&reject_background_allocations,2,__ATOMIC_RELEASE);
    struct kernel_page_cache_concurrency_state before;kernel_page_cache_get_concurrency_state(cache,&before);
    assert(!kernel_page_cache_request_writeback(cache));
    while(!__atomic_load_n(&cycle_alloc_entered,__ATOMIC_ACQUIRE))sched_yield();
    /* 等本次OOM后的实际等待登记，不能被worker上一次park计数提前放行。 */
    cycle_background_waiting=0;
    __atomic_store_n(&cycle_alloc_resume,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&cycle_background_waiting,__ATOMIC_ACQUIRE))sched_yield();
    __atomic_store_n(&cycle_fg_resume,1,__ATOMIC_RELEASE);
    puts("version empty-prefix handshake: foreground owns last version; background reached OOM before foreground continues");
    while(!__atomic_load_n(&version_cycle_submissions,__ATOMIC_ACQUIRE))sched_yield();
    assert(!pthread_join(foreground,NULL) && (!cycle_sync_result || cycle_sync_result==-KERNEL_ENOMEM));
    wait_writeback_round(cache,before.writeback_rounds);
    __atomic_store_n(&reject_background_allocations,0,__ATOMIC_RELEASE);
    __atomic_store_n(&version_cycle_active,0,__ATOMIC_RELEASE);
    kernel_page_cache_stop_worker(cache);
    for(unsigned i=0;i<COUNT-1;i++)held[i].source.release(held[i].source.context);
    uint64_t observed=kernel_vfs_error_sequence(file);assert(!kernel_vfs_sync(file,0,&observed));
    kernel_page_cache_alias_detach(&alias);kernel_page_cache_alias_drain(&alias);
    assert(!physical_page_release(cache->allocator,address));
    for(unsigned i=0;i<2;i++) {
        unsigned char actual;
        assert(!__real_kernel_vfs_node_pread(kernel_vfs_file_node(file),(uint64_t)i*BOAROS_PAGE_SIZE+17,&actual,1,&done) && done==1 && actual==0xa1+i);
    }
    puts("PASS version OOM empty-prefix interleave: background releases entry qualification before waiting; foreground prefix submits and both finish");
}
static void retain_after_cache_close(struct kernel_page_cache *cache,struct kernel_vfs_file *file)
{
    struct retained_version idle={0};
    retain_generation(file,0xf1,&late_version);
    retain_generation(file,0xf3,&after_destroy_version);
    retain_generation(file,0xf2,&idle);
    destroy_allocator=cache->allocator;
    destroy_idle_page=(uintptr_t)idle.data&~BOAROS_PAGE_MASK;
    idle.source.release(idle.source.context);
}
static void *destroy_with_borrowed_version(void *argument)
{
    mm_test_attach(0);assert(!kernel_page_cache_destroy(argument));return NULL;
}
static void destroy_version_case(struct kernel_page_cache *cache)
{
    if(!late_version.source.context) {assert(!kernel_page_cache_destroy(cache));return;}
    pause_version_free=1;version_free_entered=resume_version_free=0;
    pthread_t destroyer;assert(!pthread_create(&destroyer,NULL,destroy_with_borrowed_version,cache));
    while(!__atomic_load_n(&version_free_entered,__ATOMIC_ACQUIRE))sched_yield();
    assert(*late_version.data==0xf1);
    /* Closing detached idle versions and dropped the pool lock before their
     * physical release. A real last borrower can return concurrently here. */
    late_version.source.release(late_version.source.context);late_version=(struct retained_version){0};
    __atomic_store_n(&resume_version_free,1,__ATOMIC_RELEASE);
    assert(!pthread_join(destroyer,NULL));destroy_allocator=NULL;
    assert(!cache->record && !cache->heap && *after_destroy_version.data==0xf3);
    after_destroy_version.source.release(after_destroy_version.source.context);
    after_destroy_version=(struct retained_version){0};
    puts("PASS version pool destroy: closed idle drain overlaps last retained release, no cache pointer borrowed by late owner");
}
static unsigned window_truncate_started,window_truncate_done;
static void *truncate_writeback_window(void *file)
{
    mm_test_attach(0);
    __atomic_store_n(&window_truncate_started,1,__ATOMIC_RELEASE);
    assert(!kernel_vfs_ftruncate(file,BOAROS_PAGE_SIZE+37));
    __atomic_store_n(&window_truncate_done,1,__ATOMIC_RELEASE);
    return NULL;
}
static void foreground_window_truncate(struct kernel_page_cache *cache,struct kernel_vfs_file *file)
{
    enum { COUNT=64 };
    unsigned char data[BOAROS_PAGE_SIZE];memset(data,0x82,sizeof(data));size_t done;
    for(unsigned i=0;i<COUNT;i++)assert(!kernel_vfs_pwrite(file,(uint64_t)i*BOAROS_PAGE_SIZE,data,sizeof(data),&done) && done==sizeof(data));
    uint64_t addresses[2];size_t valid;struct kernel_page_cache_alias aliases[2]={0};
    for(unsigned i=0;i<2;i++) {
        assert(!kernel_page_cache_get(cache,file,i?COUNT-1:0,&addresses[i],&valid));
        assert(!kernel_page_cache_alias_attach(cache,file,i?COUNT-1:0,addresses[i],&aliases[i],
            kernel_vfs_file_node(file),0x500000+i*BOAROS_PAGE_SIZE,rearm_alias));
    }
    write_entered=resume_write=sync_done=0;pause_write=1;
    pthread_t sync,truncator;assert(!pthread_create(&sync,NULL,sync_file,file));
    while(!__atomic_load_n(&write_entered,__ATOMIC_ACQUIRE))sched_yield();
    struct kernel_page_cache_concurrency_state state;kernel_page_cache_get_concurrency_state(cache,&state);
    assert(state.entry_borrows<=2*(131072/BOAROS_PAGE_SIZE));
    for(unsigned i=0;i<2;i++) {
        struct kernel_page_cache_write_borrow writer={0};
        assert(!kernel_page_cache_alias_write_begin(&aliases[i],&writer));
        ((unsigned char *)access_page(addresses[i]))[0]=0x94;
        kernel_page_cache_alias_mark_dirty(&aliases[i]);kernel_page_cache_write_end(&writer);
    }
    /* This synthetic alias is not an MM mapping that truncate can detach.
     * End that alias borrow, retaining only its explicit physical-page owner. */
    kernel_page_cache_alias_detach(&aliases[1]);kernel_page_cache_alias_drain(&aliases[1]);
    window_truncate_started=window_truncate_done=0;unsigned parked=mm_test_parked(0);
    assert(!pthread_create(&truncator,NULL,truncate_writeback_window,file));
    while(!__atomic_load_n(&window_truncate_started,__ATOMIC_ACQUIRE) || mm_test_parked(0)==parked)sched_yield();
    assert(!__atomic_load_n(&window_truncate_done,__ATOMIC_ACQUIRE));
    __atomic_store_n(&resume_write,1,__ATOMIC_RELEASE);
    assert(!pthread_join(sync,NULL) && !pthread_join(truncator,NULL));
    assert(kernel_vfs_node_size(kernel_vfs_file_node(file))==BOAROS_PAGE_SIZE+37);
    for(unsigned i=0;i<2;i++) {
        if(kernel_page_cache_alias_active(&aliases[i])) {
            kernel_page_cache_alias_detach(&aliases[i]);kernel_page_cache_alias_drain(&aliases[i]);
        }
        assert(!physical_page_release(cache->allocator,addresses[i]));
    }
    uint64_t observed=kernel_vfs_error_sequence(file);assert(!kernel_vfs_sync(file,0,&observed));
    unsigned char actual;assert(!__real_kernel_vfs_node_pread(kernel_vfs_file_node(file),0,&actual,1,&done) && done==1 && actual==0x94);
    assert(!kernel_vfs_ftruncate(file,(uint64_t)COUNT*BOAROS_PAGE_SIZE));
    assert(!kernel_vfs_pread(file,(uint64_t)(COUNT-1)*BOAROS_PAGE_SIZE,&actual,1,&done) && done==1 && !actual);
    kernel_page_cache_get_concurrency_state(cache,&state);assert(!state.entry_borrows);
    puts("PASS foreground window truncate: held old completion, new dirty generation, unselected tail retirement and zero regrowth");
}
static void dirty_index_case(struct kernel_page_cache *cache, struct kernel_vfs_mount *mount,
    struct kernel_vfs_file *file, unsigned actors)
{
    unsigned char data[BOAROS_PAGE_SIZE]; memset(data,0x4b,sizeof(data)); size_t done;
    for (unsigned i=0;i<256;i++)
        assert(!kernel_vfs_pwrite(file,(uint64_t)i*BOAROS_PAGE_SIZE,data,sizeof(data),&done) && done==sizeof(data));
    uint64_t observed=kernel_vfs_error_sequence(file);
    assert(!kernel_vfs_sync(file,0,&observed));
    const unsigned sparse[]={211,1,99}; unsigned char value=0x69;
    for (unsigned i=0;i<3;i++)
        assert(!kernel_vfs_pwrite(file,(uint64_t)sparse[i]*BOAROS_PAGE_SIZE+17,&value,1,&done) && done==1);
    assert(!kernel_page_cache_start_worker(cache));
    struct kernel_page_cache_concurrency_state state;
    struct kernel_page_cache_statistics before, after;
    kernel_page_cache_get_concurrency_state(cache,&state);
    assert(state.index_live>=256 && state.index_capacity>=512);
    kernel_page_cache_get_statistics(cache,&before);
    assert(!kernel_page_cache_request_writeback(cache)); wait_writeback_round(cache,state.writeback_rounds);
    kernel_page_cache_get_statistics(cache,&after);
    printf("sparse dirty worker: clean-index=%zu dirty-visits=%llu written=%llu\n",state.index_capacity,
        (unsigned long long)(after.worker_scanned-before.worker_scanned),
        (unsigned long long)(after.worker_written-before.worker_written));
    assert(after.worker_scanned-before.worker_scanned==3 && after.worker_written-before.worker_written==3);
    assert(kernel_vfs_error_sequence(file)==observed);
    for (unsigned i=0;i<3;i++) {
        unsigned char actual;
        assert(!__real_kernel_vfs_node_pread(kernel_vfs_file_node(file),(uint64_t)sparse[i]*BOAROS_PAGE_SIZE+17,&actual,1,&done) && done==1 && actual==value);
    }
    const unsigned reverse[]={1,0};
    for (unsigned i=0;i<2;i++)
        assert(!kernel_vfs_pwrite(file,(uint64_t)reverse[i]*BOAROS_PAGE_SIZE+31,&value,1,&done) && done==1);
    kernel_page_cache_get_concurrency_state(cache,&state); kernel_page_cache_get_statistics(cache,&before);
    assert(!kernel_page_cache_request_writeback(cache)); wait_writeback_round(cache,state.writeback_rounds);
    kernel_page_cache_get_statistics(cache,&after);
    assert(after.worker_written-before.worker_written==2 && after.worker_scanned-before.worker_scanned==2 &&
        kernel_vfs_error_sequence(file)==observed && !kernel_vfs_sync(file,0,&observed));
    for (unsigned i=0;i<2;i++) {
        unsigned char actual;
        assert(!__real_kernel_vfs_node_pread(kernel_vfs_file_node(file),(uint64_t)reverse[i]*BOAROS_PAGE_SIZE+31,&actual,1,&done) && done==1 && actual==value);
    }
    puts("PASS dirty order: randomized sparse offsets and descending adjacent pages, real readback, no writeback error");
    for (unsigned i=0;i<3;i++)
        assert(!kernel_vfs_pwrite(file,(uint64_t)sparse[2-i]*BOAROS_PAGE_SIZE+19,&value,1,&done) && done==1);
    write_error=1;
    kernel_page_cache_get_concurrency_state(cache,&state); kernel_page_cache_get_statistics(cache,&before);
    assert(!kernel_page_cache_request_writeback(cache)); wait_writeback_round(cache,state.writeback_rounds);
    kernel_page_cache_get_statistics(cache,&after);
    unsigned failed_pages=BOAROS_PAGE_CACHE_WRITEBACK_PAGES<3?BOAROS_PAGE_CACHE_WRITEBACK_PAGES:3;
    assert(after.worker_scanned-before.worker_scanned==3 && after.worker_failed-before.worker_failed==failed_pages &&
        after.worker_written-before.worker_written==3-failed_pages);
    kernel_page_cache_get_concurrency_state(cache,&state);
    assert(!kernel_page_cache_request_writeback(cache)); wait_writeback_round(cache,state.writeback_rounds);
    kernel_page_cache_get_statistics(cache,&after);
    assert(after.worker_scanned-before.worker_scanned==3+failed_pages && after.worker_written-before.worker_written==3);
    /* The round cursor points to the other inode while this first inode's
     * device write is held. Removing it must update the persistent cursor. */
    struct kernel_vfs_file other={0}; char name[40];
    snprintf(name,sizeof(name),"/dirty-cursor-%u",actors);
    assert(!kernel_vfs_create(mount,name,0600,&other));
    assert(!kernel_vfs_pwrite(file,99*BOAROS_PAGE_SIZE+23,&value,1,&done) && done==1);
    assert(!kernel_vfs_pwrite(&other,0,&value,1,&done) && done==1);
    pause_write=1; write_entered=resume_write=0;
    kernel_page_cache_get_statistics(cache,&before);
    assert(!kernel_page_cache_request_writeback(cache));
    while (!__atomic_load_n(&write_entered,__ATOMIC_ACQUIRE)) sched_yield();
    assert(!kernel_vfs_ftruncate(&other,0) && !kernel_vfs_close(&other));
    struct kernel_vfs_file added={0};
    snprintf(name,sizeof(name),"/dirty-new-%u",actors);
    assert(!kernel_vfs_create(mount,name,0600,&added));
    assert(!kernel_vfs_pwrite(&added,0,&value,1,&done) && done==1);
    __atomic_store_n(&resume_write,1,__ATOMIC_RELEASE);
    struct timespec start,now; assert(!clock_gettime(CLOCK_MONOTONIC,&start));
    for (;;) {
        kernel_page_cache_get_statistics(cache,&after);
        if (after.worker_written>=before.worker_written+2) break;
        assert(!clock_gettime(CLOCK_MONOTONIC,&now) && now.tv_sec-start.tv_sec<5); sched_yield();
    }
    kernel_page_cache_stop_worker(cache);
    assert(after.worker_scanned-before.worker_scanned==2 && !kernel_vfs_node_dirty_pages(kernel_vfs_file_node(file))->count);
    unsigned char actual;
    assert(!kernel_vfs_node_dirty_pages(kernel_vfs_file_node(&added))->count);
    assert(!__real_kernel_vfs_node_pread(kernel_vfs_file_node(&added),0,&actual,1,&done) && done==1 && actual==value);
    assert(!kernel_page_cache_invalidate_node(cache,kernel_vfs_file_node(&added)) && !kernel_vfs_close(&added));
    puts("PASS dirty worker round: error retry, new dirty publication during held I/O, cursor inode truncate/close");
    assert(!kernel_page_cache_invalidate_node(cache,kernel_vfs_file_node(file)));
    kernel_page_cache_get_concurrency_state(cache,&state); size_t capacity=state.index_capacity;
    size_t baseline_live=state.index_live;
    assert(baseline_live<64);
    assert(!kernel_vfs_ftruncate(file,(uint64_t)2304*BOAROS_PAGE_SIZE));
    for (unsigned wave=0;wave<32;wave++) {
        for (unsigned i=0;i<64;i++) {
            uint64_t address; size_t valid;
            assert(!kernel_page_cache_get(cache,file,256+wave*64+i,&address,&valid) && valid==BOAROS_PAGE_SIZE);
            assert(!physical_page_release(cache->allocator,address));
        }
        assert(!kernel_page_cache_invalidate_node(cache,kernel_vfs_file_node(file)));
        kernel_page_cache_get_concurrency_state(cache,&state);
        if (state.index_capacity!=capacity) fprintf(stderr,"tombstone churn grew wave=%u old=%zu new=%zu live=%zu tombstones=%zu\n",
            wave,capacity,state.index_capacity,state.index_live,state.index_tombstones);
        assert(state.index_live==baseline_live && state.index_capacity==capacity);
    }
    printf("PASS tombstone churn: 2048 distinct pages, live<=64, stable index capacity=%zu\n",capacity);
    assert(!kernel_vfs_ftruncate(file,64*BOAROS_PAGE_SIZE));
    for (unsigned i=0;i<64;i++) {
        memset(data,0x30+i,sizeof(data));
        assert(!kernel_vfs_pwrite(file,(uint64_t)i*BOAROS_PAGE_SIZE,data,sizeof(data),&done) && done==sizeof(data));
    }
    observed=kernel_vfs_error_sequence(file);
    assert(!kernel_vfs_sync(file,0,&observed) && !kernel_page_cache_invalidate_node(cache,kernel_vfs_file_node(file)));
}

struct demand_read {
    unsigned cpu, done;
    unsigned watch_inode, inode_attempts, pause_inode, resume_inode, scalar;
    unsigned fail_workspace;
    size_t heap_allocations;
    uint64_t first;
    size_t count;
    enum kernel_page_cache_status status;
    struct kernel_page_cache_read_page pages[KERNEL_PAGE_CACHE_READ_BATCH_MAX];
};
static void *read_demand_batch(void *arg)
{
    struct demand_read *read = arg;
    mm_test_attach(read->cpu);
    if (read->watch_inode) {
        attempt_inode_lock=&kernel_vfs_file_node(current_file)->io_lock;
        inode_read_attempts=&read->inode_attempts;
        inode_read_resume=read->pause_inode?&read->resume_inode:NULL;
    }
    fail_workspace_allocation=read->fail_workspace;
    demand_heap_allocations=&read->heap_allocations;
    /* 结果必须由本次调用完整填写，错误页不能沿用旧地址或字节数。 */
    memset(read->pages, 0xa5, sizeof(read->pages));
    if(read->scalar) {
        assert(read->count==1);
        read->pages[0]=(struct kernel_page_cache_read_page){0};
        read->status=read->pages[0].status=kernel_page_cache_lookup(current_cache,current_file,
            read->first,&read->pages[0].physical_address,&read->pages[0].valid_bytes);
    } else read->status = kernel_page_cache_get_batch(current_cache, current_file,
        read->first, read->pages, read->count);
    assert(!fail_workspace_allocation);
    demand_heap_allocations=NULL;
    attempt_inode_lock=NULL;inode_read_attempts=NULL;inode_read_resume=NULL;
    __atomic_store_n(&read->done, 1, __ATOMIC_RELEASE);
    return 0;
}
static void wait_progress(const unsigned *value, unsigned minimum)
{
    struct timespec start, now;
    assert(!clock_gettime(CLOCK_MONOTONIC, &start));
    while (__atomic_load_n(value, __ATOMIC_ACQUIRE) < minimum) {
        assert(!clock_gettime(CLOCK_MONOTONIC, &now));
        assert((now.tv_sec - start.tv_sec) * INT64_C(1000000000) +
            now.tv_nsec - start.tv_nsec < INT64_C(5000000000));
        sched_yield();
    }
}
static void reset_demand_source(void)
{
    source_reads = source_batches = source_batch_completed = 0;
    read_entered = resume_read = pause_read = 0;
    source_error = source_fault_error = 0;
    source_short = 0;
    memset(source_pages, 0, sizeof(source_pages));
    memset(source_batch_sizes, 0, sizeof(source_batch_sizes));
    capture_reads = 1;
}
static void demand_idle(struct kernel_page_cache *cache)
{
    struct kernel_page_cache_concurrency_state state;
    kernel_page_cache_get_concurrency_state(cache, &state);
    assert(!state.pending && !state.running && !state.entry_borrows &&
        state.requests_queued == state.requests_completed);
}
static void demand_contents(const struct kernel_page_cache_read_page *page,
    unsigned char byte, size_t valid)
{
    assert(!page->status && page->physical_address && page->valid_bytes == valid);
    const unsigned char *data = access_page(page->physical_address);
    for (size_t i = 0; i < valid; i++) {
        if (data[i]!=byte) fprintf(stderr,"demand content offset=%zu actual=%u expected=%u valid=%zu\n",i,data[i],byte,valid);
        assert(data[i] == byte);
    }
}
static void demand_release(struct demand_read *read)
{
    for (size_t i = 0; i < read->count; i++) {
        if (!read->pages[i].status) {
            assert(!physical_page_release(current_cache->allocator, read->pages[i].physical_address));
            read->pages[i].physical_address = 0;
        } else assert(!read->pages[i].physical_address && !read->pages[i].valid_bytes);
    }
}
struct counted_demand {
    size_t count;
    struct read_operation_counts counts;
};
static void count_demand_read(struct counted_demand *read)
{
    struct demand_read result = {.count = read->count};
    memset(result.pages, 0xa5, sizeof(result.pages));
    read_inode_lock = &kernel_vfs_file_node(current_file)->io_lock;
    read_allocator = current_cache->allocator;
    read_counts = &read->counts;
    result.status = kernel_page_cache_get_batch(current_cache, current_file, 16,
        result.pages, result.count);
    assert(!result.status);
    for (size_t i = 0; i < result.count; i++)
        demand_contents(&result.pages[i], 0x40 + i, BOAROS_PAGE_SIZE);
    demand_release(&result);
    read_counts = 0;
}
static void *count_remote_demand(void *argument)
{
    mm_test_attach(1);
    count_demand_read(argument);
    return 0;
}
struct resident_demand {
    struct kernel_vfs_file *file;
    struct demand_read read;
    struct read_operation_counts counts;
};
static void *try_resident_demand(void *argument)
{
    struct resident_demand *call=argument;
    mm_test_attach(1);
    read_counts=&call->counts; read_allocator=current_cache->allocator;
    call->read.status=kernel_page_cache_try_resident_batch(current_cache,call->file,
        call->read.first,call->read.pages,call->read.count);
    read_counts=NULL; read_allocator=NULL;
    return NULL;
}
static void resident_batch_case(struct kernel_page_cache *cache,struct kernel_vfs_file *original)
{
    struct kernel_vfs_file file={0};
    assert(!kernel_vfs_create(original->mount,"/resident-batch",0600,&file));
    unsigned char bytes[BOAROS_PAGE_SIZE]; size_t done;
    for (unsigned i=0;i<8;i++) {
        memset(bytes,0xb0+i,sizeof(bytes));
        assert(!kernel_vfs_pwrite(&file,(uint64_t)i*BOAROS_PAGE_SIZE,bytes,sizeof(bytes),&done) && done==sizeof(bytes));
    }
    uint64_t observed=kernel_vfs_error_sequence(&file);
    assert(!kernel_vfs_sync(&file,0,&observed));
    observe_cache_metadata=1;
    struct kernel_page_cache_concurrency_state state;
    kernel_page_cache_get_concurrency_state(cache,&state);
    observe_cache_metadata=0;
    unsigned reads=source_reads,batches=source_batches;
    pthread_t thread;
    struct resident_demand hot={.file=&file,.read={.count=8}};
    assert(!pthread_create(&thread,NULL,try_resident_demand,&hot) && !pthread_join(thread,NULL));
    assert(!hot.read.status && hot.counts.metadata_locks==1 && hot.counts.physical_acquires==8 &&
        !hot.counts.physical_releases && !hot.counts.descriptor_allocations && !hot.counts.inode_locks);
    for(unsigned i=0;i<8;i++) demand_contents(&hot.read.pages[i],0xb0+i,BOAROS_PAGE_SIZE);
    demand_release(&hot.read);
    assert(source_reads==reads && source_batches==batches);

    KERNEL_LOCK_SCOPE(held);
    kernel_rwlock_write(cache_metadata_lock,&held);
    struct resident_demand busy={.file=&file,.read={.count=8}};
    assert(!pthread_create(&thread,NULL,try_resident_demand,&busy) && !pthread_join(thread,NULL));
    assert(busy.read.status==KERNEL_PAGE_CACHE_STATUS_BUSY && busy.counts.metadata_locks==1 &&
        !busy.counts.physical_acquires);
    kernel_lock_release(&held);
    struct kernel_vfs_node *node=kernel_vfs_file_node(&file);
    assert(!kernel_file_mapping_prepare(node));
    KERNEL_LOCK_SCOPE(inode);
    kernel_vfs_node_lock(node,&inode,1); kernel_file_mapping_close(node);
    struct resident_demand closed={.file=&file,.read={.count=8}};
    assert(!pthread_create(&thread,NULL,try_resident_demand,&closed) && !pthread_join(thread,NULL));
    assert(closed.read.status==KERNEL_PAGE_CACHE_STATUS_BUSY && !closed.counts.physical_acquires);
    kernel_file_mapping_open(node); kernel_lock_release(&inode);

    unsigned char byte=0xe7;
    assert(!kernel_vfs_pwrite(&file,0,&byte,1,&done) && done==1);
    write_entered=resume_write=sync_done=0; pause_write=1;
    pthread_t sync; assert(!pthread_create(&sync,NULL,sync_file,&file));
    wait_progress(&write_entered,1);
    struct resident_demand writing={.file=&file,.read={.count=8}};
    assert(!pthread_create(&thread,NULL,try_resident_demand,&writing) && !pthread_join(thread,NULL));
    assert(!writing.read.status && writing.counts.metadata_locks==1 &&
        *(unsigned char *)access_page(writing.read.pages[0].physical_address)==byte);
    demand_release(&writing.read);
    __atomic_store_n(&resume_write,1,__ATOMIC_RELEASE);
    assert(!pthread_join(sync,NULL));

    assert(!kernel_page_cache_invalidate_node(cache,node));
    struct demand_read prefix={.count=7};
    prefix.status=kernel_page_cache_get_batch(cache,&file,0,prefix.pages,prefix.count);
    assert(!prefix.status);
    reads=source_reads; batches=source_batches;
    struct resident_demand miss={.file=&file,.read={.count=8}};
    assert(!pthread_create(&thread,NULL,try_resident_demand,&miss) && !pthread_join(thread,NULL));
    assert(miss.read.status==KERNEL_PAGE_CACHE_STATUS_NOT_FOUND && miss.counts.metadata_locks==1 &&
        miss.counts.physical_acquires==7 && miss.counts.physical_releases==7 &&
        source_reads==reads && source_batches==batches && !miss.counts.descriptor_allocations);
    for(unsigned i=0;i<7;i++) {
        uint32_t references;
        assert(!physical_page_reference_count(cache->allocator,prefix.pages[i].physical_address,&references) && references==2);
    }
    demand_release(&prefix);
    uint64_t address;size_t valid;
    assert(!kernel_page_cache_get(cache,&file,7,&address,&valid) && !physical_page_release(cache->allocator,address));
    struct resident_demand eof={.file=&file,.read={.first=7,.count=8}};
    assert(!pthread_create(&thread,NULL,try_resident_demand,&eof) && !pthread_join(thread,NULL));
    assert(!eof.read.status && eof.counts.metadata_locks==1 && eof.counts.physical_acquires==1);
    demand_contents(&eof.read.pages[0],0xb7,BOAROS_PAGE_SIZE);
    for(unsigned i=1;i<8;i++) assert(eof.read.pages[i].status==KERNEL_PAGE_CACHE_STATUS_OUT_OF_RANGE);
    demand_release(&eof.read);
    assert(!kernel_page_cache_invalidate_node(cache,node) && !kernel_vfs_close(&file));
    demand_idle(cache);
    puts("PASS resident batch: one metadata try for eight hot pages, no allocation/I/O, busy/epoch/EOF and miss rollback, reads during held writeback");
}
static void demand_operation_cases(struct kernel_page_cache *cache, struct kernel_vfs_file *file)
{
    struct kernel_page_cache_concurrency_state state;
    observe_cache_metadata = 1;
    kernel_page_cache_get_concurrency_state(cache, &state);
    observe_cache_metadata = 0;
    assert(cache_metadata_lock);
    for (unsigned remote = 0; remote < 2; remote++) for (unsigned warm = 0; warm < 2; warm++)
        for (unsigned count = 1; count <= 8; count *= 8) {
            assert(!kernel_page_cache_invalidate_node(cache, kernel_vfs_file_node(file)));
            kernel_page_cache_get_concurrency_state(cache,&state);
            if(state.index_capacity-state.index_capacity/4-state.index_live-state.index_tombstones<count) {
                /* Churn can leave a large table needing a tombstone rebuild.
                 * Prepare that maintenance before counting the read itself. */
                struct demand_read prepared={.count=count};
                assert(!kernel_page_cache_get_batch(cache,file,16,prepared.pages,prepared.count));
                demand_release(&prepared);
                assert(!kernel_page_cache_invalidate_node(cache,kernel_vfs_file_node(file)));
            }
            if (warm) {
                struct demand_read prepared = {.count = count};
                assert(!kernel_page_cache_get_batch(cache, file, 16, prepared.pages, prepared.count));
                demand_release(&prepared);
            }
            reset_demand_source();
            struct counted_demand read = {.count = count};
            if (remote) {
                unsigned parked = mm_test_parked(1);
                pause_read = !warm;
                pthread_t client;
                assert(!pthread_create(&client, 0, count_remote_demand, &read));
                if (!warm) {
                    wait_progress(&read_entered, 1);
                    while (mm_test_parked(1) == parked) sched_yield();
                    __atomic_store_n(&resume_read, 1, __ATOMIC_RELEASE);
                }
                assert(!pthread_join(client, 0));
            } else count_demand_read(&read);
            assert(source_batches == !warm && source_reads == (warm ? 0 : count));
            assert(read.counts.physical_acquires == (warm || !remote ? count : 0) &&
                read.counts.physical_releases == count);
            assert(!read.counts.descriptor_allocations);
#ifndef CACHE_MM_CASES
            if(read.counts.cache_allocations!=(remote || warm ? 0 : count))fprintf(stderr,
                "cache allocation mismatch: remote=%u warm=%u count=%u actual=%llu\n",remote,warm,count,
                (unsigned long long)read.counts.cache_allocations);
            assert(read.counts.cache_allocations == (remote || warm ? 0 : count));
#endif
            /* expanded_index已建立足够桶容量；这里检查实际保护次数，
             * 避免热页重新退回逐页借用/完成，或冷页重复进入完成处理。 */
            assert(read.counts.inode_locks == (!remote && !warm));
            if (!remote) assert(read.counts.metadata_locks == (warm ? 1 : 4 + (count>1?2:0)));
            else if (warm) assert(read.counts.metadata_locks == 1);
            printf("read operation counts: page=%u cpu=%s cache=%s pages=%u metadata=%llu inode=%llu acquire=%llu release=%llu cache_probes=unavailable(COST=0) scope=caller\n",
                (unsigned)BOAROS_PAGE_SIZE, remote ? "remote" : "native", warm ? "hot" : "cold", count,
                (unsigned long long)read.counts.metadata_locks, (unsigned long long)read.counts.inode_locks,
                (unsigned long long)read.counts.physical_acquires, (unsigned long long)read.counts.physical_releases);
            demand_idle(cache);
        }
}
static void demand_cold_warm(struct kernel_page_cache *cache, struct kernel_vfs_file *file)
{
    assert(!kernel_page_cache_invalidate_node(cache, kernel_vfs_file_node(file)));
    reset_demand_source();
    struct demand_read cold = {.cpu = 1, .count = 8};
    pthread_t client;
    assert(!pthread_create(&client, 0, read_demand_batch, &cold) && !pthread_join(client, 0));
    assert(!cold.status && source_reads == 8 && source_batches == 1 && source_batch_sizes[0] == 8);
    for (unsigned i = 0; i < 8; i++) demand_contents(&cold.pages[i], 0x30 + i, BOAROS_PAGE_SIZE);
    struct kernel_page_cache_concurrency_state before, after;
    kernel_page_cache_get_concurrency_state(cache, &before);
    uint64_t available = physical_page_available(cache->allocator);
    struct demand_read warm = {.cpu = 1, .count = 8};
    assert(!pthread_create(&client, 0, read_demand_batch, &warm) && !pthread_join(client, 0));
    kernel_page_cache_get_concurrency_state(cache, &after);
    assert(!warm.status && source_reads == 8 && source_batches == 1 &&
        before.requests_queued == after.requests_queued &&
        available == physical_page_available(cache->allocator));
    for (unsigned i = 0; i < 8; i++) {
        uint32_t references;
        assert(warm.pages[i].physical_address == cold.pages[i].physical_address);
        assert(!physical_page_reference_count(cache->allocator, warm.pages[i].physical_address, &references) &&
            references == 3);
    }
    demand_release(&warm);
    demand_release(&cold);
    demand_idle(cache);
    puts("PASS demand batch: eight cold pages in one backend call; owned refs and request/allocation-free warm hits");
}
static void demand_hot_resize(struct kernel_page_cache *cache,struct kernel_vfs_file *original,unsigned actors)
{
    struct kernel_vfs_file file={0};
    char name[32];snprintf(name,sizeof(name),"/hot-resize-%u",actors);
    assert(!kernel_vfs_create(original->mount,name,0600,&file));
    current_cache=cache;current_file=&file;
    capture_reads=0;
    unsigned char data[BOAROS_PAGE_SIZE];size_t done;
    for(unsigned i=0;i<4;i++) {
        memset(data,0x70+i,sizeof(data));
        assert(!kernel_vfs_pwrite(&file,(uint64_t)i*BOAROS_PAGE_SIZE,data,sizeof(data),&done) && done==sizeof(data));
    }
    uint64_t observed=kernel_vfs_error_sequence(&file);
    assert(!kernel_vfs_sync(&file,0,&observed));
    struct demand_read held={.count=4,.watch_inode=1};read_demand_batch(&held);
    assert(!held.status && !held.inode_attempts);
    struct demand_read scalar_hot={.cpu=1,.count=1,.watch_inode=1,.scalar=1};pthread_t scalar_reader;
    assert(!pthread_create(&scalar_reader,NULL,read_demand_batch,&scalar_hot) && !pthread_join(scalar_reader,NULL));
    assert(!scalar_hot.status && !scalar_hot.inode_attempts);
    demand_contents(&scalar_hot.pages[0],0x70,BOAROS_PAGE_SIZE);demand_release(&scalar_hot);
    struct kernel_vfs_node *node=kernel_vfs_file_node(&file);
    assert(!kernel_vfs_ftruncate(&file,3*BOAROS_PAGE_SIZE+37));
    for(unsigned closed=0;closed<2;closed++) for(unsigned scalar=0;scalar<2;scalar++) {
        struct kernel_lock_guard operation={0},inode={0};
        struct kernel_page_cache_resize resize={0};
        if(closed) {
            kernel_vfs_file_write_lock(&file,&operation);
            assert(!kernel_page_cache_resize_prepare(cache,node,node->size,node->size+1,&resize) && resize.entry);
        }
        kernel_vfs_node_lock(node,&inode,1);
        if(!closed) kernel_file_mapping_close(node);
        unsigned parked=mm_test_parked(0);
        struct demand_read waiting={.first=scalar?3:0,.count=scalar?1:4,.watch_inode=1,.scalar=scalar};pthread_t reader;
        assert(!pthread_create(&reader,NULL,read_demand_batch,&waiting));
        wait_progress(&waiting.inode_attempts,1);
        while(mm_test_parked(0)==parked)sched_yield();
        assert(!__atomic_load_n(&waiting.done,__ATOMIC_ACQUIRE));
        uint32_t references;
        assert(!physical_page_reference_count(cache->allocator,held.pages[0].physical_address,&references) && references==2);
        if(closed) kernel_page_cache_resize_finish(&resize,node->size);
        else kernel_file_mapping_open(node);
        kernel_lock_release(&inode);
        if(operation.lock) kernel_lock_release(&operation);
        assert(!pthread_join(reader,NULL) && !waiting.status);
        for(unsigned i=0;i<waiting.count;i++) {
            unsigned index=(unsigned)waiting.first+i;
            demand_contents(&waiting.pages[i],0x70+index,index==3?37:BOAROS_PAGE_SIZE);
        }
        demand_release(&waiting);
    }
    assert(!kernel_page_cache_invalidate_node(cache,node));
    struct demand_read prefix={.count=1};read_demand_batch(&prefix);assert(!prefix.status);
    struct demand_read mixed={.count=2,.watch_inode=1,.pause_inode=1};pthread_t mixed_reader;
    assert(!pthread_create(&mixed_reader,NULL,read_demand_batch,&mixed));
    wait_progress(&mixed.inode_attempts,1);
    uint32_t references;
    assert(!physical_page_reference_count(cache->allocator,prefix.pages[0].physical_address,&references) && references==3);
    /* miss尚未取得inode资格；旧hot前缀已拥有页，截断/再增长只影响慢路重查。 */
    assert(!kernel_vfs_ftruncate(&file,0) && !kernel_vfs_ftruncate(&file,4*BOAROS_PAGE_SIZE));
    __atomic_store_n(&mixed.resume_inode,1,__ATOMIC_RELEASE);
    assert(!pthread_join(mixed_reader,NULL) && !mixed.status);
    demand_contents(&mixed.pages[0],0x70,BOAROS_PAGE_SIZE);
    demand_contents(&mixed.pages[1],0,BOAROS_PAGE_SIZE);
    assert(mixed.pages[0].physical_address==prefix.pages[0].physical_address &&
        mixed.pages[1].physical_address!=held.pages[1].physical_address);
    demand_release(&mixed);demand_release(&prefix);
    for(unsigned i=0;i<4;i++) {
        memset(data,0x70+i,sizeof(data));
        assert(!kernel_vfs_pwrite(&file,(uint64_t)i*BOAROS_PAGE_SIZE,data,sizeof(data),&done) && done==sizeof(data));
    }
    observed=kernel_vfs_error_sequence(&file);assert(!kernel_vfs_sync(&file,0,&observed));
    /* 已返回页只持数据页寿命；截断可以退休整页，重新增长不能复活其身份。 */
    assert(!kernel_vfs_ftruncate(&file,BOAROS_PAGE_SIZE+37));
    assert(!physical_page_reference_count(cache->allocator,held.pages[2].physical_address,&references) && references==1);
    demand_contents(&held.pages[2],0x72,BOAROS_PAGE_SIZE);
    struct demand_read eof={.count=4};read_demand_batch(&eof);
    assert(eof.status==KERNEL_PAGE_CACHE_STATUS_OUT_OF_RANGE);
    demand_contents(&eof.pages[0],0x70,BOAROS_PAGE_SIZE);
    demand_contents(&eof.pages[1],0x71,37);
    const unsigned char *eof_tail=access_page(eof.pages[1].physical_address);
    for(size_t i=37;i<BOAROS_PAGE_SIZE;i++) assert(!eof_tail[i]);
    assert(eof.pages[2].status==KERNEL_PAGE_CACHE_STATUS_OUT_OF_RANGE && eof.pages[3].status==KERNEL_PAGE_CACHE_STATUS_OUT_OF_RANGE);
    assert(!kernel_vfs_ftruncate(&file,4*BOAROS_PAGE_SIZE));
    struct demand_read grown={.count=4};read_demand_batch(&grown);assert(!grown.status);
    demand_contents(&grown.pages[0],0x70,BOAROS_PAGE_SIZE);
    const unsigned char *tail=access_page(grown.pages[1].physical_address);
    for(size_t i=0;i<BOAROS_PAGE_SIZE;i++)assert(tail[i]==(i<37?0x71:0));
    for(unsigned i=2;i<4;i++) {
        demand_contents(&grown.pages[i],0,BOAROS_PAGE_SIZE);
        assert(grown.pages[i].physical_address!=held.pages[i].physical_address);
    }
    demand_contents(&held.pages[2],0x72,BOAROS_PAGE_SIZE);
    demand_release(&grown);demand_release(&eof);demand_release(&held);
    demand_idle(cache);
    assert(!kernel_page_cache_invalidate_node(cache,node));
    assert(!kernel_vfs_unlink(file.mount,name) && !kernel_vfs_close(&file));
    current_file=original;
    puts("PASS hot read resize: batch/scalar closing/CLOSED waits, mixed hit/miss truncate-regrowth, old pins and new EOF identities");
}
static void demand_short_read(struct kernel_page_cache *cache, struct kernel_vfs_file *file)
{
    assert(!kernel_page_cache_invalidate_node(cache, kernel_vfs_file_node(file)));
    uint64_t available = physical_page_available(cache->allocator);
    reset_demand_source();
    source_short = 1;
    uint64_t address; size_t valid;
    assert(kernel_page_cache_get(cache, file, 0, &address, &valid) == KERNEL_PAGE_CACHE_STATUS_IO);
    demand_idle(cache);
    assert(physical_page_available(cache->allocator) == available);
    struct demand_read failed = {.cpu = 0, .count = 1};
    read_demand_batch(&failed);
    assert(failed.status == KERNEL_PAGE_CACHE_STATUS_IO &&
        !failed.pages[0].physical_address && !failed.pages[0].valid_bytes);
    demand_idle(cache);
    assert(physical_page_available(cache->allocator) == available);
    reset_demand_source();
    struct demand_read retry = {.cpu = 0, .count = 1};
    read_demand_batch(&retry);
    assert(!retry.status && source_reads == 1);
    demand_contents(&retry.pages[0], 0x30, BOAROS_PAGE_SIZE);
    demand_release(&retry);
    demand_idle(cache);
    puts("PASS short backend read: scalar and one-page batch reject incomplete success; failed entries release and retry");
}
static void demand_overlap(struct kernel_page_cache *cache, struct kernel_vfs_file *file)
{
    assert(!kernel_page_cache_invalidate_node(cache, kernel_vfs_file_node(file)));
    uint64_t hot[2]; size_t valid;
    assert(!kernel_page_cache_get(cache, file, 1, &hot[0], &valid) && valid == BOAROS_PAGE_SIZE);
    assert(!kernel_page_cache_get(cache, file, 6, &hot[1], &valid) && valid == BOAROS_PAGE_SIZE);
    reset_demand_source(); pause_read = 1;
    struct demand_read first = {.cpu = 1, .count = 4};
    struct demand_read overlap = {.cpu = 0, .first = 2, .count = 6};
    pthread_t producer, other;
    assert(!pthread_create(&producer, 0, read_demand_batch, &first));
    wait_progress(&read_entered, 1);
    assert(source_reads == 3 && source_batches == 1);
    unsigned parked = mm_test_parked(0);
    /* 第二个CPU0现场模拟首个生产者I/O睡眠后的任务调度；它必须先发布
     * 自有缺页，不能先等待重叠loading而堵住自己的订阅者。 */
    assert(!pthread_create(&other, 0, read_demand_batch, &overlap));
    wait_progress(&source_batch_completed, 1);
    while (mm_test_parked(0) == parked) sched_yield();
    assert(!__atomic_load_n(&overlap.done, __ATOMIC_ACQUIRE));
    uint64_t own; size_t own_valid;
    assert(!kernel_page_cache_lookup(cache, file, 4, &own, &own_valid) && own_valid == BOAROS_PAGE_SIZE &&
        *(unsigned char *)access_page(own) == 0x34);
    assert(!physical_page_release(cache->allocator, own));
    __atomic_store_n(&resume_read, 1, __ATOMIC_RELEASE);
    assert(!pthread_join(producer, 0) && !pthread_join(other, 0));
    assert(!first.status && !overlap.status && source_reads == 6 && source_batches == 2 &&
        source_batch_sizes[0] == 3 && source_batch_sizes[1] == 3);
    assert(overlap.heap_allocations==1);
    for (unsigned i = 0; i < 4; i++) demand_contents(&first.pages[i], 0x30 + i, BOAROS_PAGE_SIZE);
    for (unsigned i = 0; i < 6; i++) demand_contents(&overlap.pages[i], 0x32 + i, BOAROS_PAGE_SIZE);
    assert(first.pages[1].physical_address == hot[0] && overlap.pages[4].physical_address == hot[1] &&
        first.pages[2].physical_address == overlap.pages[0].physical_address &&
        first.pages[3].physical_address == overlap.pages[1].physical_address);
    for (unsigned i = 0; i < 8; i++) assert(source_pages[i] == (i != 1 && i != 6));
    for (unsigned i = 0; i < 2; i++) assert(!physical_page_release(cache->allocator, hot[i]));
    demand_release(&first);
    demand_release(&overlap);
    demand_idle(cache);
    puts("PASS overlapping demand batches: hot/loading/own misses, publish before wait, one load per cold page");
}
static void demand_workspace_oom(struct kernel_page_cache *cache,struct kernel_vfs_file *file)
{
    assert(!kernel_page_cache_invalidate_node(cache,kernel_vfs_file_node(file)));
    struct kernel_heap_statistics before,after;
    kernel_heap_get_statistics(cache->heap,&before);
    uint64_t hot;size_t valid;
    assert(!kernel_page_cache_get(cache,file,4,&hot,&valid) && valid==BOAROS_PAGE_SIZE);
    reset_demand_source();pause_read=1;
    struct demand_read first={.cpu=1,.count=2};
    pthread_t producer;
    assert(!pthread_create(&producer,NULL,read_demand_batch,&first));
    wait_progress(&read_entered,1);
    /* 首批仍在后端持有控制区；并发批次分配失败只影响自己的冷页，
     * 已取得的热页引用保留，不能等首批归还而阻塞独立I/O。 */
    struct demand_read failed={.cpu=0,.first=4,.count=3,.fail_workspace=1};
    read_demand_batch(&failed);
    assert(failed.status==KERNEL_PAGE_CACHE_STATUS_NO_MEMORY && failed.heap_allocations==1 &&
        source_reads==2 && source_batches==1 && !__atomic_load_n(&first.done,__ATOMIC_ACQUIRE));
    demand_contents(&failed.pages[0],0x34,BOAROS_PAGE_SIZE);
    for(unsigned i=1;i<3;i++) assert(failed.pages[i].status==KERNEL_PAGE_CACHE_STATUS_NO_MEMORY &&
        !failed.pages[i].physical_address && !failed.pages[i].valid_bytes);
    struct demand_read retry={.cpu=0,.first=4,.count=3};
    read_demand_batch(&retry);
    assert(!retry.status && retry.heap_allocations==1 && source_reads==4 && source_batches==2 &&
        !__atomic_load_n(&first.done,__ATOMIC_ACQUIRE) &&
        retry.pages[0].physical_address==failed.pages[0].physical_address);
    for(unsigned i=0;i<3;i++) demand_contents(&retry.pages[i],0x34+i,BOAROS_PAGE_SIZE);
    __atomic_store_n(&resume_read,1,__ATOMIC_RELEASE);
    assert(!pthread_join(producer,NULL) && !first.status);
    for(unsigned i=0;i<2;i++) demand_contents(&first.pages[i],0x30+i,BOAROS_PAGE_SIZE);
    demand_release(&retry);demand_release(&failed);demand_release(&first);
    assert(!physical_page_release(cache->allocator,hot));
    demand_idle(cache);
    assert(!kernel_page_cache_invalidate_node(cache,kernel_vfs_file_node(file)));
    kernel_heap_get_statistics(cache->heap,&after);
    assert(after.live_allocations==before.live_allocations);
    puts("PASS workspace contention: allocation failure preserves hot refs, independent retry finishes before paused owner, surplus descriptor freed");
}
static void demand_errors(struct kernel_page_cache *cache, struct kernel_vfs_file *file)
{
    for (unsigned fault = 0; fault < 2; fault++) {
        assert(!kernel_page_cache_invalidate_node(cache, kernel_vfs_file_node(file)));
        reset_demand_source(); source_fault_index = 2;
        source_fault_error = fault ? -KERNEL_ENOMEM : -KERNEL_EIO;
        enum kernel_page_cache_status expected = fault ? KERNEL_PAGE_CACHE_STATUS_NO_MEMORY : KERNEL_PAGE_CACHE_STATUS_IO;
        struct demand_read failed = {.cpu = 1, .count = 8};
        pthread_t client;
        assert(!pthread_create(&client, 0, read_demand_batch, &failed) && !pthread_join(client, 0));
        assert(failed.status == expected && source_reads == 8 && source_batches == 1);
        for (unsigned i = 0; i < 8; i++) {
            if (i == 2) assert(failed.pages[i].status == expected &&
                !failed.pages[i].physical_address && !failed.pages[i].valid_bytes);
            else demand_contents(&failed.pages[i], 0x30 + i, BOAROS_PAGE_SIZE);
        }
        demand_idle(cache);
        reset_demand_source();
        struct demand_read retry = {.cpu = 1, .count = 8};
        assert(!pthread_create(&client, 0, read_demand_batch, &retry) && !pthread_join(client, 0));
        assert(!retry.status && source_reads == 1 && source_batches == 1 &&
            source_batch_sizes[0] == 1 && source_pages[2] == 1);
        for (unsigned i = 0; i < 8; i++) {
            demand_contents(&retry.pages[i], 0x30 + i, BOAROS_PAGE_SIZE);
            if (i != 2) assert(retry.pages[i].physical_address == failed.pages[i].physical_address);
        }
        demand_release(&retry);
        demand_release(&failed);
        demand_idle(cache);
    }
    puts("PASS demand batch errors: per-page EIO/ENOMEM, preserved successful refs, failed-page-only retry");
}
#ifndef CACHE_MM_CASES
static void demand_oom(struct kernel_page_cache *cache, struct kernel_vfs_file *file)
{
    for (unsigned entry = 0; entry < 2; entry++) for (unsigned fail = 1; fail <= 8; fail++) {
        assert(!kernel_page_cache_invalidate_node(cache, kernel_vfs_file_node(file)));
        uint64_t available = physical_page_available(cache->allocator);
        reset_demand_source();
        if (entry) fail_cache_entry = fail;
        else fail_cache_page = fail;
        struct demand_read failed = {.cpu = 1, .count = 8};
        pthread_t client;
        assert(!pthread_create(&client, 0, read_demand_batch, &failed) && !pthread_join(client, 0));
        assert(!fail_cache_page && !fail_cache_entry && failed.status == KERNEL_PAGE_CACHE_STATUS_NO_MEMORY);
        unsigned failures = 0;
        for (unsigned i = 0; i < 8; i++) {
            if (failed.pages[i].status) {
                assert(failed.pages[i].status == KERNEL_PAGE_CACHE_STATUS_NO_MEMORY &&
                    !failed.pages[i].physical_address && !failed.pages[i].valid_bytes);
                failures++;
            } else demand_contents(&failed.pages[i], 0x30 + i, BOAROS_PAGE_SIZE);
        }
        assert(failures);
        demand_idle(cache);
        reset_demand_source();
        struct demand_read retry = {.cpu = 1, .count = 8};
        assert(!pthread_create(&client, 0, read_demand_batch, &retry) && !pthread_join(client, 0));
        assert(!retry.status && source_reads == failures && source_batches == 1);
        for (unsigned i = 0; i < 8; i++) {
            demand_contents(&retry.pages[i], 0x30 + i, BOAROS_PAGE_SIZE);
            if (!failed.pages[i].status) assert(retry.pages[i].physical_address == failed.pages[i].physical_address);
        }
        demand_release(&retry);
        demand_release(&failed);
        demand_idle(cache);
        assert(!kernel_page_cache_invalidate_node(cache, kernel_vfs_file_node(file)) &&
            physical_page_available(cache->allocator) == available);
    }
    puts("PASS demand batch OOM: each page/entry preparation failure, retry, no stranded loading or physical owner");
}
#endif
static void demand_cancel(struct kernel_page_cache *cache, struct kernel_vfs_file *file)
{
    assert(!kernel_page_cache_invalidate_node(cache, kernel_vfs_file_node(file)));
    uint64_t hot[2]; size_t valid;
    assert(!kernel_page_cache_get(cache, file, 1, &hot[0], &valid));
    assert(!kernel_page_cache_get(cache, file, 6, &hot[1], &valid));
    reset_demand_source(); pause_read = 1;
    struct demand_read active = {.cpu = 1, .count = 1};
    struct demand_read queued = {.cpu = 2, .first = 1, .count = 7};
    pthread_t producer, other, stopper;
    assert(!pthread_create(&producer, 0, read_demand_batch, &active));
    wait_progress(&read_entered, 1);
    assert(!pthread_create(&other, 0, read_demand_batch, &queued));
    struct kernel_page_cache_concurrency_state state;
    do { kernel_page_cache_get_concurrency_state(cache, &state); sched_yield(); } while (state.pending != 1);
    watch_stop = 1; stop_entered = stop_done = 0;
    assert(!pthread_create(&stopper, 0, stop_cache, 0));
    wait_progress(&stop_entered, 1);
    assert(!__atomic_load_n(&stop_done, __ATOMIC_ACQUIRE));
    __atomic_store_n(&resume_read, 1, __ATOMIC_RELEASE);
    assert(!pthread_join(producer, 0) && !pthread_join(other, 0) && !pthread_join(stopper, 0));
    watch_stop = 0;
    assert(!active.status && queued.status == KERNEL_PAGE_CACHE_STATUS_STATE &&
        source_reads == 1 && source_batches == 1);
    demand_contents(&active.pages[0], 0x30, BOAROS_PAGE_SIZE);
    for (unsigned i = 0; i < 7; i++) {
        if (i == 0 || i == 5) {
            demand_contents(&queued.pages[i], i == 0 ? 0x31 : 0x36, BOAROS_PAGE_SIZE);
            assert(queued.pages[i].physical_address == hot[i == 5]);
        } else assert(queued.pages[i].status == KERNEL_PAGE_CACHE_STATUS_STATE &&
            !queued.pages[i].physical_address && !queued.pages[i].valid_bytes);
    }
    demand_release(&active);
    demand_release(&queued);
    for (unsigned i = 0; i < 2; i++) assert(!physical_page_release(cache->allocator, hot[i]));
    demand_idle(cache);
    assert(!kernel_page_cache_start_worker(cache));
    puts("PASS cancelled demand batch: warm refs survive queued-miss cancellation; active owner drains before stop");
}
static unsigned demand_truncate_started, demand_truncate_done;
static void *truncate_demand_file(void *unused)
{
    (void)unused; mm_test_attach(0);
    __atomic_store_n(&demand_truncate_started, 1, __ATOMIC_RELEASE);
    assert(!kernel_vfs_ftruncate(current_file, 2 * BOAROS_PAGE_SIZE + 37));
    __atomic_store_n(&demand_truncate_done, 1, __ATOMIC_RELEASE);
    return 0;
}
static void demand_truncate(struct kernel_page_cache *cache, struct kernel_vfs_file *file)
{
    assert(!kernel_page_cache_invalidate_node(cache, kernel_vfs_file_node(file)));
    reset_demand_source(); pause_read = 1;
    demand_truncate_started = demand_truncate_done = 0;
    struct demand_read loading = {.cpu = 1, .count = 8};
    pthread_t client, truncator;
    assert(!pthread_create(&client, 0, read_demand_batch, &loading));
    wait_progress(&read_entered, 1);
    unsigned parked = mm_test_parked(0);
    assert(!pthread_create(&truncator, 0, truncate_demand_file, 0));
    wait_progress(&demand_truncate_started, 1);
    while (mm_test_parked(0) == parked) sched_yield();
    assert(!__atomic_load_n(&demand_truncate_done, __ATOMIC_ACQUIRE));
    __atomic_store_n(&resume_read, 1, __ATOMIC_RELEASE);
    assert(!pthread_join(client, 0) && !pthread_join(truncator, 0) && !loading.status);
    struct demand_read shrunk = {.count = 8};
    shrunk.status = kernel_page_cache_get_batch(cache, file, 0, shrunk.pages, shrunk.count);
    assert(shrunk.status == KERNEL_PAGE_CACHE_STATUS_OUT_OF_RANGE && source_reads == 8 && source_batches == 1);
    for (unsigned i = 0; i < 3; i++) {
        demand_contents(&shrunk.pages[i], 0x30 + i, i == 2 ? 37 : BOAROS_PAGE_SIZE);
        assert(shrunk.pages[i].physical_address == loading.pages[i].physical_address);
    }
    const unsigned char *tail = access_page(shrunk.pages[2].physical_address);
    for (size_t i = 37; i < BOAROS_PAGE_SIZE; i++) assert(!tail[i]);
    for (unsigned i = 3; i < 8; i++) {
        uint32_t references;
        assert(shrunk.pages[i].status == KERNEL_PAGE_CACHE_STATUS_OUT_OF_RANGE &&
            !shrunk.pages[i].physical_address && !shrunk.pages[i].valid_bytes);
        assert(!physical_page_reference_count(cache->allocator, loading.pages[i].physical_address, &references) &&
            references == 1);
    }
    demand_release(&shrunk);
    demand_release(&loading);
    assert(!kernel_page_cache_invalidate_node(cache, kernel_vfs_file_node(file)));
    reset_demand_source();
    struct demand_read eof = {.count = 8};
    eof.status = kernel_page_cache_get_batch(cache, file, 0, eof.pages, eof.count);
    assert(eof.status == KERNEL_PAGE_CACHE_STATUS_OUT_OF_RANGE &&
        source_reads == 3 && source_batches == 1 && source_batch_sizes[0] == 3);
    for (unsigned i = 0; i < 3; i++)
        demand_contents(&eof.pages[i], 0x30 + i, i == 2 ? 37 : BOAROS_PAGE_SIZE);
    tail = access_page(eof.pages[2].physical_address);
    for (size_t i = 37; i < BOAROS_PAGE_SIZE; i++) assert(!tail[i]);
    for (unsigned i = 3; i < 8; i++) assert(eof.pages[i].status == KERNEL_PAGE_CACHE_STATUS_OUT_OF_RANGE);
    demand_release(&eof);
    assert(!kernel_vfs_ftruncate(file, 8 * BOAROS_PAGE_SIZE));
    reset_demand_source();
    struct demand_read grown = {.count = 8};
    grown.status = kernel_page_cache_get_batch(cache, file, 0, grown.pages, grown.count);
    assert(!grown.status && source_reads == 5 && source_batches == 1 && source_batch_sizes[0] == 5);
    demand_contents(&grown.pages[0], 0x30, BOAROS_PAGE_SIZE);
    demand_contents(&grown.pages[1], 0x31, BOAROS_PAGE_SIZE);
    tail = access_page(grown.pages[2].physical_address);
    for (size_t i = 0; i < BOAROS_PAGE_SIZE; i++) assert(tail[i] == (i < 37 ? 0x32 : 0));
    for (unsigned i = 3; i < 8; i++) demand_contents(&grown.pages[i], 0, BOAROS_PAGE_SIZE);
    demand_release(&grown);
    demand_idle(cache);
    puts("PASS truncate waits for demand loading: short EOF, retired owners, zero tail and no stale pages after growth");
}
static void demand_cases(struct kernel_page_cache *cache, struct kernel_vfs_file *file, unsigned actors)
{
    assert(!kernel_page_cache_start_worker(cache));
    demand_hot_resize(cache,file,actors);
    if (actors == 2) demand_operation_cases(cache, file);
    demand_cold_warm(cache, file);
    if (actors==2) resident_batch_case(cache,file);
    demand_short_read(cache, file);
    demand_overlap(cache, file);
    demand_workspace_oom(cache,file);
    demand_errors(cache, file);
#ifndef CACHE_MM_CASES
    demand_oom(cache, file);
#endif
    demand_cancel(cache, file);
    demand_truncate(cache, file);
    capture_reads = 0;
    kernel_page_cache_stop_worker(cache);
    assert(!kernel_page_cache_invalidate_node(cache, kernel_vfs_file_node(file)));
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
    assert(!pthread_create(&threads[0], 0, get_cold_page, (void *)(uintptr_t)1));
    while (!__atomic_load_n(&read_entered, __ATOMIC_ACQUIRE)) sched_yield();
    /* 先发布被暂停的loading，再让其余actor借用同一次失败I/O；
     * 提前入队的请求尚无entry，不能用entry_borrows等待它们。 */
    for (unsigned i = 2; i <= actors; i++) assert(!pthread_create(&threads[i - 1], 0, get_cold_page, (void *)(uintptr_t)i));
    do { kernel_page_cache_get_concurrency_state(cache, &state); sched_yield(); }
    while (state.entry_borrows != actors);
    assert(state.running == 1 && !state.pending);
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
    sparse_cache_growth(cache,&mount,actors);
    partial_snapshot(cache,&file,pages);
    for (unsigned force_retry=0;force_retry<2;force_retry++) {
        alias_snapshot(cache, &file, pages,1,force_retry);
        alias_snapshot(cache, &file, pages,8,force_retry);
        alias_snapshot(cache, &file, pages,32,force_retry);
    }
    expanded_index_case(cache,&file,pages);
    version_reuse_case(cache,&file);
    foreground_windows(cache,&file);
    if(actors==2) {version_capacity_case(cache,&file);version_oom_case(cache,&file);version_prefix_cycle(cache,&file);version_empty_cycle(cache,&file);}
    foreground_window_truncate(cache,&file);
    dirty_index_case(cache,&mount,&file,actors);
    demand_cases(cache, &file, actors);
    if(actors==4)retain_after_cache_close(cache,&file);
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
    execution_case(&cache);
    destroy_version_case(&cache);
    assert(!physical_page_allocator_audit(&pages));
    assert(physical_page_available(&pages) == baseline);
    struct kernel_heap_statistics backend_stats;
    kernel_heap_get_statistics(&backend_heap,&backend_stats);
    assert(!backend_stats.live_allocations && !backend_stats.current_pages &&
        physical_page_available(&backend_pages)==backend_baseline && !physical_page_allocator_audit(&backend_pages));
    puts("PASS controlled cache construction: no pressure hook, private publication, owner return");
    return 0;
}
