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
#include <kernel/time.h>
#include "../../fs/vfs_objects.h"
#include "../../fs/ext4_backend.h"
#include <ext4_fs.h>
#include <ext4_journal.h>
#include <ext4_journal_memory.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <sched.h>
#include <time.h>
void mm_test_attach(unsigned);
static void cache_test_attach(unsigned actor_id)
{
    mm_test_attach(actor_id);
    /* 多个调用者可来自同一远端 CPU；后台执行者固定在 CPU0。 */
    kernel_cpu_current()->logical_id = actor_id != 0;
}
enum kernel_time_status kernel_time_deadline_from_monotonic(uint64_t target,uint64_t *deadline)
{
    assert(deadline);
    *deadline=target;
    return target<=kernel_time_monotonic_ns()?KERNEL_TIME_STATUS_DEADLINE_PASSED:KERNEL_TIME_STATUS_OK;
}
#ifdef CACHE_MM_CASES
#include "mm_disk_cases.h"
#endif

static unsigned char backend_pool[16 * 1024 * 1024] __attribute__((aligned(16384)));
static struct physical_page_allocator backend_pages;
static struct kernel_heap backend_heap;
static unsigned char pool[16 * 1024 * 1024] __attribute__((aligned(16384)));
static void *access_page(uint64_t p) { return (void *)(uintptr_t)p; }
static int page_address(const void *p, uint64_t *out)
{ *out = (uintptr_t)p; return 1; }
static int image_fd;
static unsigned age_observing,age_waits,age_empty_wait;
static _Thread_local unsigned progress_writeback_caller;
static unsigned progress_writeback_waiting;
static uint64_t age_clock_offset,age_deadlines[2];
static struct kernel_wait_token age_tokens[2];
uint64_t __real_kernel_time_monotonic_ns(void);
uint64_t __wrap_kernel_time_monotonic_ns(void)
{ return __real_kernel_time_monotonic_ns()+__atomic_load_n(&age_clock_offset,__ATOMIC_RELAXED); }
static unsigned source_reads, pause_read, pause_after_read, read_entered, resume_read,source_first_cpu;
static unsigned expanded_done;
static void wait_progress(const unsigned *,unsigned);
static int source_error;
static unsigned source_short;
static unsigned capture_reads, source_batches, source_batch_completed;
static unsigned source_pages[64], source_batch_sizes[8];
static unsigned source_fault_index;
static int source_fault_error;
static unsigned write_error;
static ext4_file *metadata_fault_file;
static unsigned timestamp_fault, metadata_sync_fault;
static ext4_file *times_sync_file;
static unsigned times_sync_calls;
static _Thread_local unsigned fixed_target_client;
static unsigned fixed_target_waiting;
static struct kernel_page_cache *current_cache;
static struct kernel_vfs_instance *metadata_owner_instance;
struct kernel_vfs_node *__real_kernel_vfs_node_find(struct kernel_vfs_instance *, uint64_t);
struct kernel_vfs_node *__wrap_kernel_vfs_node_find(struct kernel_vfs_instance *instance, uint64_t inode)
{
    if (instance == metadata_owner_instance)
        assert(kernel_cpu_current()->logical_id == 0 &&
            "caller metadata must use the handle owner, not the CPU0 inode registry");
    return __real_kernel_vfs_node_find(instance, inode);
}
static int (*metadata_close_node)(struct kernel_vfs_node *);
static unsigned metadata_closed;
static struct kernel_vfs_node *metadata_nodes[3];
static unsigned metadata_closed_mask;
static struct {
    struct kernel_vfs_node *target;
    struct kernel_vfs_file *closing;
    unsigned entered, close_done;
} timestamp_owner;
static void (*metadata_store)(void *,void *,uint32_t,unsigned,struct ext4_timestamp);
static void observe_metadata_store(void *context, void *owner, uint32_t inode,
    unsigned field, struct ext4_timestamp value)
{
    if (owner && owner == timestamp_owner.target) {
        assert(kernel_cpu_current()->logical_id == 1 &&
            owner == lwext4_node_file(owner)->time_owner);
        __atomic_store_n(&timestamp_owner.entered,1,__ATOMIC_RELEASE);
        wait_progress(&timestamp_owner.close_done,1);
    }
    metadata_store(context,owner,inode,field,value);
}
int __real_ext4_mount_setup_time_observer(const char *,const struct ext4_time_observer *);
int __wrap_ext4_mount_setup_time_observer(const char *path,const struct ext4_time_observer *observer)
{
    if (!observer) return __real_ext4_mount_setup_time_observer(path,observer);
    struct ext4_time_observer observed=*observer;
    metadata_store=observer->store; observed.store=observe_metadata_store;
    return __real_ext4_mount_setup_time_observer(path,&observed);
}
static int observe_metadata_close(struct kernel_vfs_node *node)
{
    assert(kernel_cpu_current()->logical_id == 0 && "final node close belongs to CPU0");
    for (unsigned i=0;i<3;i++) if (node==metadata_nodes[i]) {
        assert(!(metadata_closed_mask & (1U<<i)));
        metadata_closed_mask |= 1U<<i;
    }
    metadata_closed++;
    return metadata_close_node(node);
}
static struct lwext4_mount_adapter *journal_wait_owner;
static struct { unsigned hold, entered, release; } durable_before_park;
static _Thread_local unsigned hold_durable_wait;
static struct {
    struct kernel_vfs_node *writer, *reader;
    unsigned batches, first_entered, resume_first, read_completed, close_completed, fault_batch;
} executor_fairness;
static struct {
    struct kernel_vfs_file *file;
    struct kernel_vfs_file *second_file;
    struct kernel_wait_queue *queue;
    struct kernel_wait_queue *second_queue;
    unsigned armed, flush_entered, resume_flush, wait_entered, second_wait_entered, fail_flush;
    uint64_t target;
} durable_wait;
static struct {
    struct kernel_vfs_file *file;
    struct kernel_wait_queue *queues[8];
    unsigned prepared,waiting,completed,seen[8];
} durable_cohort;
static void (*durable_prepare)(struct kernel_vfs_node *,int,struct kernel_vfs_sync_request *);
static void observe_sync_prepare(struct kernel_vfs_node *node,int data_only,
    struct kernel_vfs_sync_request *request)
{
    durable_prepare(node,data_only,request);
    if (request->file==durable_wait.file) {
        assert(request->target);
        durable_wait.target=request->target;
        __atomic_store_n(&durable_wait.queue,&request->changed,__ATOMIC_RELEASE);
    }
    if (request->file==durable_wait.second_file) {
        assert(request->target);
        __atomic_store_n(&durable_wait.second_queue,&request->changed,__ATOMIC_RELEASE);
    }
    if (request->file==durable_cohort.file) {
        unsigned index=durable_cohort.prepared;
        assert(index<8 && request->target==durable_wait.target);
        durable_cohort.queues[index]=&request->changed;
        __atomic_store_n(&durable_cohort.prepared,index+1,__ATOMIC_RELEASE);
    }
}
void __real_kernel_vfs_sync_complete(struct kernel_vfs_sync_request *,int);
void __wrap_kernel_vfs_sync_complete(struct kernel_vfs_sync_request *request,int result)
{
    if (request->target &&
        (request->file==durable_wait.file || request->file==durable_wait.second_file))
        assert(arch_interrupt_is_enabled() && "durable completion must run outside the pending-index IRQ section");
    if (request->file==durable_cohort.file)
        __atomic_fetch_add(&durable_cohort.completed,1,__ATOMIC_RELEASE);
    __real_kernel_vfs_sync_complete(request,result);
}
static enum kernel_page_cache_status expected_status;
static unsigned watch_stop, stop_entered, stop_done;
static struct kernel_vfs_file *current_file;
struct read_operation_counts {
    uint64_t metadata_locks, inode_locks, physical_acquires, physical_releases;
    uint64_t pressure_queries;
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
uint64_t __real_physical_page_available(const struct physical_page_allocator *);
uint64_t __wrap_physical_page_available(const struct physical_page_allocator *allocator)
{
    if (read_counts && allocator==read_allocator) read_counts->pressure_queries++;
    return __real_physical_page_available(allocator);
}
static _Thread_local struct kernel_rwlock *attempt_inode_lock;
static _Thread_local unsigned *inode_read_attempts;
static _Thread_local unsigned *inode_read_resume;
static struct kernel_rwlock *truncate_publish_lock;
static unsigned truncate_backend_done,truncate_writer_attempt,truncate_writer_done;
static _Thread_local unsigned truncate_after_backend,truncate_writer;
void __real_kernel_rwlock_read(struct kernel_rwlock *, struct kernel_lock_guard *);
void __wrap_kernel_rwlock_read(struct kernel_rwlock *lock, struct kernel_lock_guard *guard)
{
    if (read_counts && lock == read_inode_lock) read_counts->inode_locks++;
    if (inode_read_attempts && lock==attempt_inode_lock) {
        __atomic_fetch_add(inode_read_attempts,1,__ATOMIC_RELEASE);
        while(inode_read_resume && !__atomic_load_n(inode_read_resume,__ATOMIC_ACQUIRE))sched_yield();
    }
    if (truncate_writer && lock==truncate_publish_lock)
        __atomic_store_n(&truncate_writer_attempt,1,__ATOMIC_RELEASE);
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
    if (read_counts && lock == read_inode_lock) read_counts->inode_locks++;
    if (truncate_after_backend && lock==truncate_publish_lock)
        while (!__atomic_load_n(&truncate_writer_done,__ATOMIC_ACQUIRE)) sched_yield();
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
static _Thread_local int hold_ordinary_wait;
static unsigned ordinary_wait_entered,ordinary_wait_resume;
static _Thread_local unsigned delay_load_registration;
static _Thread_local struct kernel_wait_token delayed_load_token;
static unsigned load_registration_entered, load_registration_resume, observe_load_publication, load_published;
enum kernel_scheduler_status __real_kernel_wait_park(const struct kernel_wait_token *,enum kernel_wait_wake_reason *);
enum kernel_scheduler_status __wrap_kernel_wait_park(const struct kernel_wait_token *token,
    enum kernel_wait_wake_reason *reason)
{
    if (hold_durable_wait) {
        hold_durable_wait = 0;
        assert(!kernel_cpu_current()->raw_locks);
        __atomic_store_n(&durable_before_park.entered, 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&durable_before_park.release, __ATOMIC_ACQUIRE)) sched_yield();
    }
    assert(!delayed_load_token.generation || token->generation != delayed_load_token.generation ||
        token->task != delayed_load_token.task);
    if (hold_ordinary_wait) {
        __atomic_store_n(&ordinary_wait_entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&ordinary_wait_resume,__ATOMIC_ACQUIRE)) sched_yield();
    }
    return __real_kernel_wait_park(token,reason);
}
enum kernel_scheduler_status __real_kernel_wait_prepare(struct kernel_wait_queue *,uint64_t,int,struct kernel_wait_token *);
enum kernel_scheduler_status __wrap_kernel_wait_prepare(struct kernel_wait_queue *queue,
    uint64_t deadline,int interruptible,struct kernel_wait_token *token)
{
    if (journal_wait_owner && (queue == &journal_wait_owner->journal_work ||
        queue == &journal_wait_owner->checkpoint_work || queue == &journal_wait_owner->journal_progress))
        assert(kernel_cpu_current()->raw_locks && "journal condition and registration need shared state protection");
    for (unsigned i=0;i<__atomic_load_n(&durable_cohort.prepared,__ATOMIC_ACQUIRE);i++)
        if (queue==durable_cohort.queues[i] && !__atomic_exchange_n(&durable_cohort.seen[i],1,__ATOMIC_ACQ_REL))
            __atomic_fetch_add(&durable_cohort.waiting,1,__ATOMIC_RELEASE);
    if (queue==__atomic_load_n(&durable_wait.queue,__ATOMIC_ACQUIRE)) {
        assert(kernel_cpu_current()->logical_id==1 && !kernel_page_cache_is_executor(current_cache));
        __atomic_store_n(&durable_wait.wait_entered,1,__ATOMIC_RELEASE);
    }
    if (queue==__atomic_load_n(&durable_wait.second_queue,__ATOMIC_ACQUIRE)) {
        assert(kernel_cpu_current()->logical_id==1 && !kernel_page_cache_is_executor(current_cache));
        __atomic_store_n(&durable_wait.second_wait_entered,1,__ATOMIC_RELEASE);
    }
    int delaying = delay_load_registration == 1 && cache_metadata_lock && kernel_lock_held(cache_metadata_lock, 1);
    if (delaying) {
        delay_load_registration = 2;
        __atomic_store_n(&load_registration_entered, 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&load_registration_resume, __ATOMIC_ACQUIRE)) sched_yield();
    }
    enum kernel_scheduler_status status=__real_kernel_wait_prepare(queue,deadline,interruptible,token);
    if (!status && fixed_target_client && journal_wait_owner && queue==&journal_wait_owner->journal_progress)
        __atomic_store_n(&fixed_target_waiting,1,__ATOMIC_RELEASE);
    if (!status && progress_writeback_caller && queue) __atomic_store_n(&progress_writeback_waiting,1,__ATOMIC_RELEASE);
    if (!status && age_observing && kernel_page_cache_is_executor(current_cache)) {
        if (deadline) {
            unsigned index=age_waits;assert(index<2);
            age_tokens[index]=*token;age_deadlines[index]=deadline;
            __atomic_store_n(&age_waits,index+1,__ATOMIC_RELEASE);
        } else if (age_waits) __atomic_store_n(&age_empty_wait,1,__ATOMIC_RELEASE);
    }
    if (!status && queue == __atomic_load_n(&durable_wait.queue, __ATOMIC_ACQUIRE) &&
        __atomic_exchange_n(&durable_before_park.hold, 0, __ATOMIC_ACQ_REL)) hold_durable_wait = 1;
    if (delaying) { assert(!status); delayed_load_token = *token; }
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
int __real_ext4_file_writeback_times(ext4_file *,bool);
int __wrap_ext4_file_writeback_times(ext4_file *file,bool wait)
{
    if (file==times_sync_file) times_sync_calls++;
    if (file==metadata_fault_file && timestamp_fault) {
        timestamp_fault=0;
        return KERNEL_EIO;
    }
    return __real_ext4_file_writeback_times(file,wait);
}
int __real_ext4_filesystem_sync_metadata(const char *);
int __wrap_ext4_filesystem_sync_metadata(const char *mount)
{
    if (metadata_fault_file && metadata_sync_fault) {
        metadata_sync_fault=0;
        return KERNEL_EIO;
    }
    return __real_ext4_filesystem_sync_metadata(mount);
}
int __real_ext4_file_sync_metadata_mode(ext4_file *,bool);
int __wrap_ext4_file_sync_metadata_mode(ext4_file *file,bool data_only)
{
    if (file==metadata_fault_file && metadata_sync_fault) {
        metadata_sync_fault=0;
        return KERNEL_EIO;
    }
    return __real_ext4_file_sync_metadata_mode(file,data_only);
}
int __wrap_ext4_fpwrite_batch(ext4_file *file,struct file_write_span *spans,unsigned count)
{
    if(write_counts) {
        if((const void *)spans!=(const void *)write_counts->spans)
            fprintf(stderr,"writeback descriptors copied: VFS=%p ext4=%p\n",(const void *)write_counts->spans,(void *)spans);
        assert((const void *)spans==(const void *)write_counts->spans);
    }
    unsigned fault=__atomic_exchange_n(&write_error,0,__ATOMIC_ACQ_REL);
    if (fault) {
        for (unsigned i=0;i<count;i++) spans[i].written=0;
        if (fault==2 && count && spans[0].size>1) {
            struct file_write_span prefix=spans[0]; prefix.size/=2;
            int result=__real_ext4_fpwrite_batch(file,&prefix,1);
            assert(!result && prefix.written==prefix.size);
            spans[0].written=prefix.written;
        }
        return fault==2?0:fault==3?KERNEL_ENOSPC:KERNEL_EIO;
    }
    return __real_ext4_fpwrite_batch(file,spans,count);
}
static unsigned execute_calls;
static _Thread_local int pause_request_unwind;
static unsigned request_unwind_entered,request_unwind_resume;
enum kernel_scheduler_status __real_kernel_wait_queue_destroy(struct kernel_wait_queue *);
enum kernel_scheduler_status __wrap_kernel_wait_queue_destroy(struct kernel_wait_queue *queue)
{
    enum kernel_scheduler_status status=__real_kernel_wait_queue_destroy(queue);
    if (pause_request_unwind) {
        assert(!status && !kernel_cpu_current()->raw_locks && !kernel_io_context_current()->locks);
        __atomic_store_n(&request_unwind_entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&request_unwind_resume,__ATOMIC_ACQUIRE)) sched_yield();
    }
    return status;
}
static void execute_on_cpu0(void *argument)
{
    assert(kernel_cpu_current()->logical_id==0 && !kernel_cpu_current()->raw_locks &&
        !kernel_io_context_current()->locks);
    assert(argument==&execute_calls);
    execute_calls++;
}
static void *execute_client(void *unused)
{
    cache_test_attach(1);
    pause_request_unwind=unused!=NULL;
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

    struct kernel_page_cache empty={0};
    assert(!kernel_page_cache_init(&empty,cache->heap,cache->allocator));
    current_cache=&empty;
    request_unwind_entered=request_unwind_resume=0;
    assert(!kernel_page_cache_start_worker(&empty));
    assert(!pthread_create(&client,0,execute_client,&empty));
    while (!__atomic_load_n(&request_unwind_entered,__ATOMIC_ACQUIRE)) sched_yield();
    assert(kernel_page_cache_destroy(&empty)==KERNEL_PAGE_CACHE_STATUS_STATE);
    __atomic_store_n(&request_unwind_resume,1,__ATOMIC_RELEASE);
    assert(!pthread_join(client,0));
    assert(!kernel_page_cache_destroy(&empty));
    current_cache=cache;
    puts("PASS completed request retains cache until caller finishes unwinding; empty cache destroy waits for actual owner");
}
static uint64_t received[5];
extern unsigned mm_test_parked(unsigned);
static enum kernel_block_status image_read(void *context, uint64_t off, void *out, size_t size)
{
    (void)context;
    assert(!kernel_cpu_current()->raw_locks);
    return pread(image_fd, out, size, off) == (ssize_t)size ? KERNEL_BLOCK_STATUS_OK : KERNEL_BLOCK_STATUS_IO;
}
static enum kernel_block_status image_write(void *context, uint64_t off, const void *in, size_t size)
{
    (void)context;
    assert(!kernel_cpu_current()->raw_locks);
    return pwrite(image_fd, in, size, off) == (ssize_t)size ? KERNEL_BLOCK_STATUS_OK : KERNEL_BLOCK_STATUS_IO;
}
static enum kernel_block_status image_flush(void *context)
{
    (void)context;
    if (__atomic_exchange_n(&durable_wait.armed,0,__ATOMIC_ACQ_REL)) {
        assert(kernel_io_context_current()->background_reclaim &&
            !kernel_page_cache_is_executor(current_cache));
        __atomic_store_n(&durable_wait.flush_entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&durable_wait.resume_flush,__ATOMIC_ACQUIRE)) sched_yield();
        if (durable_wait.fail_flush)return KERNEL_BLOCK_STATUS_IO;
    }
    return fsync(image_fd) ? KERNEL_BLOCK_STATUS_IO : KERNEL_BLOCK_STATUS_OK;
}
int __real_kernel_vfs_node_pread(struct kernel_vfs_node *, uint64_t, void *, size_t, size_t *);
static void source_read_begin(uint64_t off)
{
    assert(!kernel_cpu_current()->raw_locks);
    if (!__atomic_fetch_add(&source_reads,1,__ATOMIC_RELAXED))
        source_first_cpu=kernel_cpu_current()->hardware_id;
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
    if (node == executor_fairness.reader)
        __atomic_store_n(&executor_fairness.read_completed, 1, __ATOMIC_RELEASE);
    return result;
}
struct source_read_progress {
    struct file_read_span *spans;
    const size_t *destinations;
    const struct file_read_completion *completion;
};
static void source_read_completed(void *context, unsigned index, const struct file_read_span *span)
{
    struct source_read_progress *progress = context;
    size_t destination = progress->destinations[index];
    progress->spans[destination] = *span;
    if (source_short && !span->error && span->completed) progress->spans[destination].completed--;
    progress->completion->complete(progress->completion->context, destination, &progress->spans[destination]);
    if (observe_load_publication && !destination) __atomic_store_n(&load_published, 1, __ATOMIC_RELEASE);
}
int __real_kernel_vfs_node_pread_batch(struct kernel_vfs_node *, struct file_read_span *, size_t, struct file_read_workspace *, const struct file_read_completion *);
int __wrap_kernel_vfs_node_pread_batch(struct kernel_vfs_node *node,
    struct file_read_span *spans, size_t count, struct file_read_workspace *workspace, const struct file_read_completion *completion)
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
    struct source_read_progress progress = {spans, destinations, completion};
    const struct file_read_completion forward = {source_read_completed, &progress};
    int result = used ? __real_kernel_vfs_node_pread_batch(node, kept, used, workspace, completion ? &forward : NULL) : 0;
    for (size_t i = 0; i < used; i++) {
        spans[destinations[i]] = kept[i];
        if (source_short && !spans[destinations[i]].error && spans[destinations[i]].completed)
            --spans[destinations[i]].completed;
    }
    __atomic_fetch_add(&source_batch_completed, 1, __ATOMIC_RELEASE);
    if (__atomic_exchange_n(&pause_after_read, 0, __ATOMIC_ACQ_REL)) {
        pause_read = 1;
        source_read_pause();
    }
    if (node == executor_fairness.reader)
        __atomic_store_n(&executor_fairness.read_completed, 1, __ATOMIC_RELEASE);
    return result ? result : injected;
}
#ifndef CACHE_MM_CASES
static unsigned fail_cache_page, fail_cache_entry;
static unsigned pause_candidate,candidate_entered,candidate_resume;
enum physical_page_status __real_physical_page_allocate(struct physical_page_allocator *, uint64_t *);
enum physical_page_status __wrap_physical_page_allocate(struct physical_page_allocator *pages, uint64_t *out)
{
    if (current_cache && pages==current_cache->allocator &&
        __atomic_exchange_n(&pause_candidate,0,__ATOMIC_ACQ_REL)) {
        assert(!kernel_cpu_current()->raw_locks && !kernel_io_context_current()->locks);
        __atomic_store_n(&candidate_entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&candidate_resume,__ATOMIC_ACQUIRE)) sched_yield();
    }
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
        /* 冷页资源在调用现场准备；不能把inode资格带入分配或CPU0排队。 */
        assert(!kernel_lock_held(&kernel_vfs_file_node(current_file)->io_lock, 0));
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
    cache_test_attach(id);
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
    (void)arg; cache_test_attach(0);
    kernel_page_cache_stop_worker(current_cache);
    __atomic_store_n(&stop_done, 1, __ATOMIC_RELEASE);
    return 0;
}
static void *get_independent_page(void *arg)
{
    (void)arg;cache_test_attach(2);
    uint64_t address;size_t valid;
    assert(!kernel_page_cache_get(current_cache,current_file,1,&address,&valid) && valid==BOAROS_PAGE_SIZE);
    for (size_t i=0;i<valid;i++) assert(((unsigned char *)access_page(address))[i]==0x73);
    assert(!physical_page_release(current_cache->allocator,address));
    return NULL;
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
    assert(!kernel_io_context_current()->background_reclaim);
    __atomic_store_n(&cycle_fg_entered,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&cycle_fg_resume,__ATOMIC_ACQUIRE))sched_yield();
    return 0;
}
int __real_kernel_vfs_node_writeback(struct kernel_vfs_node *,struct file_write_span *,size_t);
int __wrap_kernel_vfs_node_writeback(struct kernel_vfs_node *node,struct file_write_span *spans,size_t count)
{
    if (node == executor_fairness.writer) {
        unsigned batch = __atomic_add_fetch(&executor_fairness.batches, 1, __ATOMIC_ACQ_REL);
        if (batch == 1) {
            assert(count == BOAROS_PAGE_CACHE_WRITEBACK_PAGES);
            __atomic_store_n(&executor_fairness.first_entered, 1, __ATOMIC_RELEASE);
            while (!__atomic_load_n(&executor_fairness.resume_first, __ATOMIC_ACQUIRE)) sched_yield();
        } else {
            /* 两个请求已在首批I/O内排队；续批不得把它们继续压在整个fsync之后。 */
            assert(__atomic_load_n(&executor_fairness.read_completed, __ATOMIC_ACQUIRE));
            assert(__atomic_load_n(&executor_fairness.close_completed, __ATOMIC_ACQUIRE));
        }
        if (batch==executor_fairness.fault_batch)write_error=2;
    }
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
    return __real_kernel_vfs_node_writeback(node,spans,count);
}
static void *sync_file(void *arg)
{
    cache_test_attach(0); uint64_t error=kernel_vfs_error_sequence(arg);
    assert(!kernel_vfs_sync(arg,0,&error));
    __atomic_store_n(&sync_done,1,__ATOMIC_RELEASE);
    return 0;
}
static void *drain_alias(void *arg)
{
    (void)arg; cache_test_attach(6);
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
    assert(!kernel_vfs_pread(&file,end-45,bytes,sizeof(bytes),&count, NULL) && count==48);
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
    unsigned id=(unsigned)(uintptr_t)arg; cache_test_attach(id);
    for (unsigned index=id-1;index<64;index+=4) {
        uint64_t address; size_t valid;
        assert(!kernel_page_cache_get(current_cache,current_file,index,&address,&valid) && valid==BOAROS_PAGE_SIZE);
        for (size_t n=0;n<valid;n++) assert(((unsigned char *)access_page(address))[n]==0x30+index);
        assert(!physical_page_release(current_cache->allocator,address));
    }
    __atomic_fetch_add(&expanded_done,1,__ATOMIC_RELEASE);
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
    source_reads=read_entered=resume_read=expanded_done=0; pause_read=1;
    pthread_t threads[4];
    for (unsigned id=1;id<=4;id++) assert(!pthread_create(&threads[id-1],0,read_expanded_index,(void *)(uintptr_t)id));
    while (!__atomic_load_n(&read_entered,__ATOMIC_ACQUIRE)) sched_yield();
    struct kernel_page_cache_concurrency_state before,after;
    wait_progress(&expanded_done,3);
    kernel_page_cache_get_concurrency_state(cache,&before);
    assert(source_reads==49 && !before.pending && !before.running);
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

static void writeback_error_publication(struct kernel_page_cache *cache,
    struct kernel_vfs_mount *mount,unsigned actors)
{
    struct kernel_vfs_file file={0};
    char name[40]; snprintf(name,sizeof(name),"/writeback-errors-%u",actors);
    assert(!kernel_vfs_create(mount,name,0600,&file));
    size_t written;
    assert(!kernel_vfs_pwrite(&file,0,"initial",7,&written) && written==7);
    uint64_t observed=kernel_vfs_error_sequence(&file);
    assert(!kernel_vfs_sync(&file,0,&observed));
    for (unsigned mode=0;mode<3;mode++) for (unsigned fault=1;fault<=2;fault++) {
        assert(!kernel_vfs_pwrite(&file,0,"changed",7,&written) && written==7);
        uint64_t before=kernel_vfs_error_sequence(&file), independent=before;
        uint64_t mount_before=kernel_vfs_mount_error_sequence(mount), mount_observed=mount_before;
        write_error=fault;
        if (mode==0) assert(kernel_vfs_sync(&file,0,&observed)==-KERNEL_EIO);
        else if (mode==1) {
            assert(!kernel_page_cache_start_worker(cache));
            struct kernel_page_cache_concurrency_state state;
            kernel_page_cache_get_concurrency_state(cache,&state);
            assert(!kernel_page_cache_request_writeback(cache));
            wait_writeback_round(cache,state.writeback_rounds);
            kernel_page_cache_stop_worker(cache);
        } else assert(kernel_vfs_sync_mount(mount,&mount_observed)==-KERNEL_EIO);
        assert(!write_error && kernel_vfs_error_sequence(&file)==before+1);
        assert(kernel_vfs_mount_error_sequence(mount)==mount_before+1);
        if (mode) assert(kernel_vfs_sync(&file,0,&observed)==-KERNEL_EIO);
        assert(!kernel_vfs_sync(&file,0,&observed));
        assert(kernel_vfs_sync(&file,0,&independent)==-KERNEL_EIO);
        assert(!kernel_vfs_sync(&file,0,&independent));
        assert(kernel_vfs_sync_mount(mount,&mount_observed)==(mode==2?0:-KERNEL_EIO));
        assert(!kernel_vfs_sync_mount(mount,&mount_observed));
    }
    struct kernel_vfs_file root={0};
    assert(!kernel_vfs_open(mount,"/",&root));
    for (unsigned root_error=0;root_error<2;root_error++) {
        uint64_t before=kernel_vfs_error_sequence(&file);
        uint64_t root_before=kernel_vfs_error_sequence(&root);
        uint64_t mount_before=kernel_vfs_mount_error_sequence(mount), mount_observed=mount_before;
        if (root_error) {
            metadata_fault_file=lwext4_node_file(kernel_vfs_file_node(&root));
            metadata_sync_fault=1;
        } else {
            assert(!kernel_vfs_file_modified(&file,0,0));
            metadata_fault_file=lwext4_node_file(kernel_vfs_file_node(&file));
            timestamp_fault=1;
        }
        assert(kernel_vfs_sync_mount(mount,&mount_observed)==-KERNEL_EIO);
        assert(!timestamp_fault && !metadata_sync_fault);
        assert(kernel_vfs_error_sequence(&file)==before+!root_error);
        assert(kernel_vfs_error_sequence(&root)==root_before+root_error);
        assert(kernel_vfs_mount_error_sequence(mount)==mount_before+1);
        assert(!kernel_vfs_sync_mount(mount,&mount_observed));
        metadata_fault_file=NULL;
    }
    assert(!kernel_vfs_close(&root));
    assert(!kernel_vfs_close(&file));
    puts("PASS writeback error publication: data/short writes/timestamps/root metadata each publish once on the failed inode; independent observers consume once");
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
    cache_test_attach(0);uint64_t observed=kernel_vfs_error_sequence(argument);
    cycle_sync_result=kernel_vfs_sync(argument,0,&observed);return NULL;
}
static void version_empty_cycle(struct kernel_page_cache *cache,struct kernel_vfs_file *file)
{
    enum { COUNT=BOAROS_PAGE_CACHE_WRITEBACK_PAGES };
    if(COUNT<2)return;
    (void)kernel_page_cache_reclaim(cache,UINT64_MAX);
    unsigned parked=mm_test_parked(0);assert(!kernel_page_cache_start_worker(cache));
    while(mm_test_parked(0)==parked)sched_yield();
    struct retained_version held[COUNT];memset(held,0,sizeof(held));
    for(unsigned i=1;i<COUNT;i++)retain_generation(file,0x20+i-1,&held[i-1]);
    /* 先结束继承的强制回写轮次；否则它能在alias装好前写净下面的目标页，
     * 前台不再进入暂停回调，控制线程却会永久等那个从未发生的事件。 */
    struct kernel_page_cache_concurrency_state drained;
    kernel_page_cache_get_concurrency_state(cache,&drained);
    assert(!kernel_page_cache_request_writeback(cache));
    wait_writeback_round(cache,drained.writeback_rounds);
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
    for(unsigned i=1;i<COUNT;i++)held[i-1].source.release(held[i-1].source.context);
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
    cache_test_attach(0);assert(!kernel_page_cache_destroy(argument));return NULL;
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
    cache_test_attach(0);
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
    assert(!kernel_vfs_pread(file,(uint64_t)(COUNT-1)*BOAROS_PAGE_SIZE,&actual,1,&done, NULL) && done==1 && !actual);
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
    unsigned delay_registration;
    size_t heap_allocations;
    uint64_t first;
    size_t count;
    enum kernel_page_cache_status status;
    struct kernel_page_cache_read_page pages[KERNEL_PAGE_CACHE_READ_BATCH_MAX];
};
static void *read_demand_batch(void *arg)
{
    struct demand_read *read = arg;
    cache_test_attach(read->cpu);
    delay_load_registration = read->delay_registration;
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
        read->status=read->pages[0].status=kernel_page_cache_get(current_cache,current_file,
            read->first,&read->pages[0].physical_address,&read->pages[0].valid_bytes);
    } else read->status = kernel_page_cache_get_batch(current_cache, current_file,
        read->first, read->pages, read->count);
    assert(!fail_workspace_allocation);
    demand_heap_allocations=NULL;
    delayed_load_token = (struct kernel_wait_token){0};
    delay_load_registration = 0;
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
    cache_test_attach(1);
    count_demand_read(argument);
    return 0;
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
                pause_read = !warm;
                pthread_t client;
                assert(!pthread_create(&client, 0, count_remote_demand, &read));
                if (!warm) {
                    wait_progress(&read_entered, 1);
                    __atomic_store_n(&resume_read, 1, __ATOMIC_RELEASE);
                }
                assert(!pthread_join(client, 0));
            } else count_demand_read(&read);
            assert(source_batches == !warm && source_reads == (warm ? 0 : count));
            assert(read.counts.physical_acquires == count &&
                read.counts.physical_releases == count);
            assert(!read.counts.descriptor_allocations);
#ifndef CACHE_MM_CASES
            if(read.counts.cache_allocations!=(warm ? 0 : count))fprintf(stderr,
                "cache allocation mismatch: remote=%u warm=%u count=%u actual=%llu\n",remote,warm,count,
                (unsigned long long)read.counts.cache_allocations);
            assert(read.counts.cache_allocations == (warm ? 0 : count));
#endif
            /* expanded_index已建立足够桶容量；这里检查实际保护次数，
             * 避免热页重新退回逐页借用/完成，或冷页重复进入完成处理。 */
            assert(read.counts.inode_locks == !warm);
            if (warm) assert(read.counts.metadata_locks==1);
            else assert(read.counts.metadata_locks<=(count==1?4:6));
            printf("read operation counts: page=%u cpu=%s cache=%s pages=%u metadata=%llu inode=%llu acquire=%llu release=%llu cache_probes=unavailable(COST=0) scope=caller\n",
                (unsigned)BOAROS_PAGE_SIZE, remote ? "remote" : "native", warm ? "hot" : "cold", count,
                (unsigned long long)read.counts.metadata_locks, (unsigned long long)read.counts.inode_locks,
                (unsigned long long)read.counts.physical_acquires, (unsigned long long)read.counts.physical_releases);
            demand_idle(cache);
        }
}
static void write_owner_counts(struct kernel_page_cache *cache,struct kernel_vfs_file *source)
{
    struct kernel_vfs_file file={0};
    assert(!kernel_vfs_create(source->mount,"/write-owner-counts",0600,&file));
    unsigned char bytes[BOAROS_PAGE_SIZE];memset(bytes,0x6c,sizeof(bytes));
    size_t written;
    assert(!kernel_vfs_pwrite(&file,0,bytes,sizeof(bytes),&written) && written==sizeof(bytes));
    struct read_operation_counts counts={0};
    read_inode_lock=&kernel_vfs_file_node(&file)->io_lock;
    read_allocator=cache->allocator;read_counts=&counts;
    assert(!kernel_vfs_pwrite(&file,0,bytes,sizeof(bytes),&written) && written==sizeof(bytes));
    read_counts=NULL;read_allocator=NULL;read_inode_lock=NULL;
    assert(counts.metadata_locks==3 && counts.inode_locks==3 &&
        !counts.physical_acquires && !counts.physical_releases);
    counts=(struct read_operation_counts){0};
    read_inode_lock=&kernel_vfs_file_node(&file)->io_lock;
    read_allocator=cache->allocator;read_counts=&counts;
    assert(!kernel_vfs_pwrite(&file,BOAROS_PAGE_SIZE,bytes,sizeof(bytes),&written) && written==sizeof(bytes));
    read_counts=NULL;read_allocator=NULL;read_inode_lock=NULL;
    assert(counts.metadata_locks==3 && counts.inode_locks==3 &&
        !counts.physical_acquires && !counts.physical_releases);
    unsigned char *range=malloc(8U*BOAROS_PAGE_SIZE);assert(range);
    memset(range,0x6c,8U*BOAROS_PAGE_SIZE);
    assert(!kernel_vfs_pwrite(&file,0,range,8U*BOAROS_PAGE_SIZE,&written) && written==8U*BOAROS_PAGE_SIZE);
    counts=(struct read_operation_counts){0};
    read_allocator=cache->allocator;read_counts=&counts;
    assert(!kernel_vfs_pwrite(&file,0,range,8U*BOAROS_PAGE_SIZE,&written) && written==8U*BOAROS_PAGE_SIZE);
    read_counts=NULL;read_allocator=NULL;
    /* One admission query and one final dirty-work notification. */
    assert(counts.pressure_queries==2);
    free(range);
    uint64_t error=kernel_vfs_error_sequence(&file);
    assert(!kernel_vfs_sync(&file,0,&error));
    unsigned char actual[BOAROS_PAGE_SIZE];size_t read;
    assert(!kernel_vfs_pread(&file,0,actual,sizeof(actual),&read, NULL) && read==sizeof(actual) && !memcmp(actual,bytes,sizeof(bytes)));
    assert(!kernel_vfs_close(&file));
    puts("PASS overwrite/append: three metadata locks, three inode phases, no redundant physical refs; one window admission and one final pressure notification, sync/readback verified");
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
        while(mm_test_parked(0)==parked && !__atomic_load_n(&waiting.done,__ATOMIC_ACQUIRE))sched_yield();
        assert(mm_test_parked(0)>parked);
        assert(!__atomic_load_n(&waiting.done,__ATOMIC_ACQUIRE));
        uint32_t references;
        /* 尾页写资格关闭时，批次仍可持有此前取得的完整热页；
         * 全文件映射关闭则不能取得新页。两者最终都必须归还实际引用。 */
        assert(!physical_page_reference_count(cache->allocator,held.pages[0].physical_address,&references) &&
            references==(closed && !scalar ? 3U : 2U));
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
static void demand_published_before_drain(struct kernel_page_cache *cache, struct kernel_vfs_file *file)
{
    assert(!kernel_page_cache_invalidate_node(cache, kernel_vfs_file_node(file)));
    reset_demand_source(); pause_after_read = 1;
    struct demand_read batch = {.cpu = 1, .count = 4};
    pthread_t producer;
    assert(!pthread_create(&producer, NULL, read_demand_batch, &batch));
    wait_progress(&read_entered, 1);
    assert(!__atomic_load_n(&batch.done, __ATOMIC_ACQUIRE));
    uint64_t page; size_t valid;
    assert(!kernel_page_cache_lookup(cache, file, 0, &page, &valid));
    assert(valid == BOAROS_PAGE_SIZE && *(unsigned char *)access_page(page) == 0x30);
    assert(!physical_page_release(cache->allocator, page));
    __atomic_store_n(&resume_read, 1, __ATOMIC_RELEASE);
    assert(!pthread_join(producer, NULL) && !batch.status);
    demand_release(&batch); demand_idle(cache);
    puts("PASS completed page is visible while its batch owner is still draining");
}

static void demand_completion_before_registration(struct kernel_page_cache *cache, struct kernel_vfs_file *file)
{
    assert(!kernel_page_cache_invalidate_node(cache, kernel_vfs_file_node(file)));
    reset_demand_source(); pause_read = 1;
    load_registration_entered = load_registration_resume = load_published = 0;
    observe_load_publication = 1;
    struct demand_read first = {.cpu=1,.count=4};
    struct demand_read waiting = {.cpu=0,.count=1,.delay_registration=1};
    pthread_t producer, reader;
    assert(!pthread_create(&producer, NULL, read_demand_batch, &first));
    wait_progress(&read_entered, 1);
    assert(!pthread_create(&reader, NULL, read_demand_batch, &waiting));
    wait_progress(&load_registration_entered, 1);
    /* reader仍持cache保护，但尚未登记entry等待；producer在此窗口真正完成P0。 */
    __atomic_store_n(&resume_read, 1, __ATOMIC_RELEASE);
    wait_progress(&load_published, 1);
    __atomic_store_n(&load_registration_resume, 1, __ATOMIC_RELEASE);
    assert(!pthread_join(reader, NULL) && !pthread_join(producer, NULL));
    assert(!first.status && !waiting.status);
    assert(first.pages[0].physical_address == waiting.pages[0].physical_address);
    demand_contents(&waiting.pages[0], 0x30, BOAROS_PAGE_SIZE);
    observe_load_publication = 0;
    demand_release(&waiting); demand_release(&first); demand_idle(cache);
    puts("PASS load completion before registration: no cache-lock dependency, no lost wake or park, exact owner return");
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
static void demand_worker_stop(struct kernel_page_cache *cache, struct kernel_vfs_file *file)
{
    assert(!kernel_page_cache_invalidate_node(cache,kernel_vfs_file_node(file)));
    uint64_t hot[2];size_t valid;
    assert(!kernel_page_cache_get(cache,file,1,&hot[0],&valid));
    assert(!kernel_page_cache_get(cache,file,6,&hot[1],&valid));
    reset_demand_source();pause_read=1;
    struct demand_read active={.cpu=1,.count=1};
    struct demand_read independent={.cpu=2,.first=1,.count=7};
    pthread_t producer,other;
    assert(!pthread_create(&producer,NULL,read_demand_batch,&active));
    wait_progress(&read_entered,1);
    kernel_page_cache_stop_worker(cache);
    assert(!pthread_create(&other,NULL,read_demand_batch,&independent) && !pthread_join(other,NULL));
    assert(!independent.status && !__atomic_load_n(&active.done,__ATOMIC_ACQUIRE));
    for (unsigned i=0;i<7;i++) {
        demand_contents(&independent.pages[i],0x31+i,BOAROS_PAGE_SIZE);
        if (i==0 || i==5) assert(independent.pages[i].physical_address==hot[i==5]);
    }
    __atomic_store_n(&resume_read,1,__ATOMIC_RELEASE);
    assert(!pthread_join(producer,NULL) && !active.status && source_reads==6 && source_batches==2);
    demand_contents(&active.pages[0],0x30,BOAROS_PAGE_SIZE);
    demand_release(&active);demand_release(&independent);
    for (unsigned i=0;i<2;i++) assert(!physical_page_release(cache->allocator,hot[i]));
    demand_idle(cache);
    assert(!kernel_page_cache_start_worker(cache));
    puts("PASS caller cold batches survive worker stop: independent cold/hot content and both producer owners returned");
}

static unsigned demand_truncate_started,demand_truncate_done;
static void *truncate_demand_file(void *unused)
{
    (void)unused; cache_test_attach(0);
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
    if (actors == 2) { demand_operation_cases(cache, file); write_owner_counts(cache,file); }
    demand_cold_warm(cache, file);
    demand_short_read(cache, file);
    demand_published_before_drain(cache, file);
    demand_completion_before_registration(cache, file);
    demand_overlap(cache, file);
    demand_workspace_oom(cache,file);
    demand_errors(cache, file);
#ifndef CACHE_MM_CASES
    demand_oom(cache, file);
#endif
    demand_worker_stop(cache, file);
    demand_truncate(cache, file);
    capture_reads = 0;
    kernel_page_cache_stop_worker(cache);
    assert(!kernel_page_cache_invalidate_node(cache, kernel_vfs_file_node(file)));
}
struct executor_sync {
    struct kernel_vfs_file *file;
    uint64_t observed;
    int result;
    unsigned done;
};
static void *executor_sync_client(void *argument)
{
    struct executor_sync *sync=argument;
    cache_test_attach(7);
    sync->result=kernel_vfs_sync(sync->file,0,&sync->observed);
    __atomic_store_n(&sync->done,1,__ATOMIC_RELEASE);
    return NULL;
}

struct error_consumer {
    struct kernel_vfs_sync_request request;
    pthread_barrier_t *barrier;
    unsigned id;
};
static void *consume_shared_error(void *argument)
{
    struct error_consumer *client = argument;
    cache_test_attach(8 + client->id);
    kernel_cpu_current()->logical_id = client->id % 2;
    for (unsigned i = 0; i < 32; i++)
        kernel_vfs_record_writeback_error(kernel_vfs_file_node(client->request.file), -KERNEL_ENOMEM);
    int status = pthread_barrier_wait(client->barrier);
    assert(!status || status == PTHREAD_BARRIER_SERIAL_THREAD);
    kernel_vfs_sync_complete(&client->request, 0);
    return NULL;
}
static void shared_error_cursor(struct kernel_vfs_file *file)
{
    uint64_t observed = kernel_vfs_error_sequence(file), before = observed;
    struct error_consumer clients[8] = {0};
    pthread_t threads[8]; pthread_barrier_t barrier;
    assert(!pthread_barrier_init(&barrier, NULL, 8));
    for (unsigned i = 0; i < 8; i++) {
        clients[i].request.file = file;
        clients[i].request.observed_error = &observed;
        clients[i].barrier = &barrier; clients[i].id = i;
        kernel_raw_lock_init(&clients[i].request.metadata, KERNEL_RAW_RANK_OBJECT);
        assert(!pthread_create(&threads[i], NULL, consume_shared_error, &clients[i]));
    }
    unsigned consumed = 0;
    for (unsigned i = 0; i < 8; i++) {
        assert(!pthread_join(threads[i], NULL) && clients[i].request.done);
        int error = clients[i].request.result;
        assert(!error || error == -KERNEL_ENOMEM);
        consumed += error != 0;
    }
    assert(consumed == 1 && observed == before + 256 &&
        kernel_vfs_error_sequence(file) == observed && !pthread_barrier_destroy(&barrier));
    puts("PASS shared error cursor: 256 publications, one observation across CPU0/CPU1, eight completions");
}
static void *executor_cold_client(void *argument)
{
    struct kernel_vfs_file *file=argument;
    cache_test_attach(8);
    uint64_t page;size_t valid;
    assert(!kernel_page_cache_get(current_cache,file,0,&page,&valid));
    assert(valid==BOAROS_PAGE_SIZE);
    const unsigned char *bytes=access_page(page);
    for (size_t i=0;i<valid;i++)assert(bytes[i]==0x73);
    assert(!physical_page_release(current_cache->allocator,page));
    return NULL;
}
static void executor_close(void *argument)
{
    assert(!kernel_vfs_close(argument));
    __atomic_store_n(&executor_fairness.close_completed,1,__ATOMIC_RELEASE);
}
static void *executor_close_client(void *argument)
{
    cache_test_attach(9);
    assert(!kernel_page_cache_run_cpu0(current_cache,executor_close,argument));
    return NULL;
}
static void executor_wait_pending(struct kernel_page_cache *cache,unsigned count)
{
    struct timespec start,now;assert(!clock_gettime(CLOCK_MONOTONIC,&start));
    for (;;) {
        struct kernel_page_cache_concurrency_state state;
        kernel_page_cache_get_concurrency_state(cache,&state);
        if (state.pending>=count)return;
        assert(!clock_gettime(CLOCK_MONOTONIC,&now) && now.tv_sec-start.tv_sec<5);
        sched_yield();
    }
}
static void executor_range_fairness(struct kernel_page_cache *cache,
    struct kernel_vfs_mount *mount,unsigned actors,unsigned prefix_error)
{
    enum { COUNT=3*BOAROS_PAGE_CACHE_WRITEBACK_PAGES };
    char name[48];struct kernel_vfs_file writer={0},reader={0},closing={0};
    snprintf(name,sizeof(name),"/executor-write-%u-%u",actors,prefix_error);
    assert(!kernel_vfs_create(mount,name,0600,&writer));
    snprintf(name,sizeof(name),"/executor-read-%u-%u",actors,prefix_error);
    assert(!kernel_vfs_create(mount,name,0600,&reader));
    snprintf(name,sizeof(name),"/executor-close-%u-%u",actors,prefix_error);
    assert(!kernel_vfs_create(mount,name,0600,&closing));
    unsigned char bytes[BOAROS_PAGE_SIZE];memset(bytes,0x73,sizeof(bytes));size_t done;
    assert(!kernel_vfs_pwrite(&reader,0,bytes,sizeof(bytes),&done) && done==sizeof(bytes));
    uint64_t observed=kernel_vfs_error_sequence(&reader);
    assert(!kernel_vfs_sync(&reader,0,&observed));
    observed=kernel_vfs_error_sequence(&closing);
    assert(!kernel_vfs_sync(&closing,0,&observed));
    assert(!kernel_page_cache_invalidate_node(cache,kernel_vfs_file_node(&reader)));
    memset(bytes,0x49,sizeof(bytes));
    for (unsigned i=0;i<COUNT;i++)
        assert(!kernel_vfs_pwrite(&writer,(uint64_t)i*sizeof(bytes),bytes,sizeof(bytes),&done) && done==sizeof(bytes));
    executor_fairness=(__typeof__(executor_fairness)){
        .writer=kernel_vfs_file_node(&writer),.reader=kernel_vfs_file_node(&reader),
        .fault_batch=prefix_error?2:0};
    current_cache=cache;capture_reads=0;
    assert(!kernel_page_cache_start_worker(cache));
    struct executor_sync sync={.file=&writer,.observed=kernel_vfs_error_sequence(&writer)};
    pthread_t syncing,reading,close_thread;
    assert(!pthread_create(&syncing,NULL,executor_sync_client,&sync));
    wait_progress(&executor_fairness.first_entered,1);
    assert(!pthread_create(&reading,NULL,executor_cold_client,&reader));
    assert(!pthread_create(&close_thread,NULL,executor_close_client,&closing));
    executor_wait_pending(cache,1);
    wait_progress(&executor_fairness.read_completed,1);
    assert(!__atomic_load_n(&sync.done,__ATOMIC_ACQUIRE));
    __atomic_store_n(&executor_fairness.resume_first,1,__ATOMIC_RELEASE);
    assert(!pthread_join(syncing,NULL) && !pthread_join(reading,NULL) && !pthread_join(close_thread,NULL));
    assert(sync.result==(prefix_error?-KERNEL_EIO:0) && !closing.private_data && !closing.mount);
    assert(executor_fairness.batches==(prefix_error?2U:3U));
    if (prefix_error) {
        assert(sync.observed==kernel_vfs_error_sequence(&writer));
        assert(kernel_vfs_node_dirty_pages(kernel_vfs_file_node(&writer))->count==
            COUNT-BOAROS_PAGE_CACHE_WRITEBACK_PAGES);
        /* 第一批完整接纳，第二批只接纳半页；重试不得丢掉已经提交的前缀。 */
        for (unsigned i=0;i<BOAROS_PAGE_CACHE_WRITEBACK_PAGES;i++) {
            unsigned char actual;
            assert(!__real_kernel_vfs_node_pread(kernel_vfs_file_node(&writer),
                (uint64_t)i*BOAROS_PAGE_SIZE,&actual,1,&done) && done==1 && actual==0x49);
        }
        assert(!kernel_vfs_sync(&writer,0,&sync.observed));
        assert(executor_fairness.batches==4);
    }
    executor_fairness=(__typeof__(executor_fairness)){0};
    kernel_page_cache_stop_worker(cache);
    assert(!kernel_page_cache_invalidate_node(cache,kernel_vfs_file_node(&writer)));
    for (unsigned i=0;i<COUNT;i++) {
        unsigned char actual;
        assert(!kernel_vfs_pread(&writer,(uint64_t)i*BOAROS_PAGE_SIZE,&actual,1,&done, NULL) && done==1 && actual==0x49);
    }
    assert(!kernel_vfs_close(&writer) && !kernel_vfs_close(&reader));
    if (prefix_error)
        puts("PASS CPU0 range error: first window and second-window prefix survive, error consumed once, unrelated requests progress and retry persists the remainder");
    else
        printf("PASS CPU0 range fairness: caller cold read and queued last close finish between %u KiB fsync batches; persisted contents and owners returned\n",
            (unsigned)(BOAROS_PAGE_CACHE_WRITEBACK_PAGES*BOAROS_PAGE_SIZE/1024));
}
static void executor_range_stop(struct kernel_page_cache *cache,struct kernel_vfs_mount *mount)
{
    enum { COUNT=3*BOAROS_PAGE_CACHE_WRITEBACK_PAGES };
    struct kernel_vfs_file writer={0};
    assert(!kernel_vfs_create(mount,"/executor-stop",0600,&writer));
    unsigned char bytes[BOAROS_PAGE_SIZE];memset(bytes,0x6a,sizeof(bytes));size_t done;
    for (unsigned i=0;i<COUNT;i++)
        assert(!kernel_vfs_pwrite(&writer,(uint64_t)i*sizeof(bytes),bytes,sizeof(bytes),&done) && done==sizeof(bytes));
    executor_fairness=(__typeof__(executor_fairness)){.writer=kernel_vfs_file_node(&writer),
        .read_completed=1,.close_completed=1};
    current_cache=cache;assert(!kernel_page_cache_start_worker(cache));
    struct executor_sync sync={.file=&writer,.observed=kernel_vfs_error_sequence(&writer)};
    pthread_t syncing,stopping;
    assert(!pthread_create(&syncing,NULL,executor_sync_client,&sync));
    wait_progress(&executor_fairness.first_entered,1);
    watch_stop=1;stop_entered=stop_done=0;
    assert(!pthread_create(&stopping,NULL,stop_cache,NULL));
    wait_progress(&stop_entered,1);
    assert(!__atomic_load_n(&stop_done,__ATOMIC_ACQUIRE) && !__atomic_load_n(&sync.done,__ATOMIC_ACQUIRE));
    __atomic_store_n(&executor_fairness.resume_first,1,__ATOMIC_RELEASE);
    assert(!pthread_join(syncing,NULL) && !pthread_join(stopping,NULL));
    watch_stop=0;
    assert(sync.result==-KERNEL_EIO && executor_fairness.batches==1);
    assert(kernel_vfs_node_dirty_pages(kernel_vfs_file_node(&writer))->count==COUNT-BOAROS_PAGE_CACHE_WRITEBACK_PAGES);
    struct kernel_page_cache_concurrency_state state;
    kernel_page_cache_get_concurrency_state(cache,&state);
    assert(!state.pending && !state.running && !state.entry_borrows && state.requests_queued==state.requests_completed);
    for (unsigned i=0;i<BOAROS_PAGE_CACHE_WRITEBACK_PAGES;i++) {
        unsigned char actual;
        assert(!__real_kernel_vfs_node_pread(kernel_vfs_file_node(&writer),
            (uint64_t)i*BOAROS_PAGE_SIZE,&actual,1,&done) && done==1 && actual==0x6a);
    }
    assert(!kernel_page_cache_start_worker(cache));
    sync.result=0;sync.done=0;
    assert(!pthread_create(&syncing,NULL,executor_sync_client,&sync) && !pthread_join(syncing,NULL));
    assert(!sync.result && executor_fairness.batches==3 && !kernel_vfs_node_dirty_pages(kernel_vfs_file_node(&writer))->count);
    executor_fairness=(__typeof__(executor_fairness)){0};
    kernel_page_cache_stop_worker(cache);
    assert(!kernel_page_cache_invalidate_node(cache,kernel_vfs_file_node(&writer)));
    for (unsigned i=0;i<COUNT;i++) {
        unsigned char actual;
        assert(!kernel_vfs_pread(&writer,(uint64_t)i*BOAROS_PAGE_SIZE,&actual,1,&done, NULL) && done==1 && actual==0x6a);
    }
    assert(!kernel_vfs_close(&writer));
    puts("PASS CPU0 range stop: in-flight window ends before stop, continuation cancelled, dirty suffix retained and restart persists it without leaked request owners");
}
static void executor_range_cases(struct kernel_page_cache *cache,const char *path)
{
    current_cache=cache;image_fd=open(path,O_RDWR);assert(image_fd>=0);
    struct stat st;assert(!fstat(image_fd,&st));
    struct kernel_block_device disk={.read=image_read,.write=image_write,.flush=image_flush,
        .capacity_bytes=st.st_size,.logical_block_size=512,.cache_mode=KERNEL_BLOCK_CACHE_WRITEBACK};
    struct kernel_vfs_mount mount={0};assert(!kernel_vfs_mount_ext4(&mount,&disk,&backend_heap,cache,0));
    executor_range_fairness(cache,&mount,1,0);
    executor_range_fairness(cache,&mount,1,1);
    executor_range_stop(cache,&mount);
    assert(!kernel_vfs_unmount(&mount) && !close(image_fd) && !kernel_page_cache_destroy(cache));
    current_cache=NULL;
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
#ifndef CACHE_MM_CASES
    pause_candidate=1;candidate_entered=candidate_resume=0;
    pthread_t allocating;
    assert(!pthread_create(&allocating,NULL,get_cold_page,(void *)(uintptr_t)1));
    wait_progress(&candidate_entered,1);
    assert(kernel_page_cache_destroy(cache)==KERNEL_PAGE_CACHE_STATUS_STATE &&
        "unpublished cold candidate must retain its cache owner");
    __atomic_store_n(&candidate_resume,1,__ATOMIC_RELEASE);
    assert(!pthread_join(allocating,NULL) && !physical_page_release(pages,received[1]));
    assert(!kernel_page_cache_invalidate_node(cache,kernel_vfs_file_node(&file)) &&
        !kernel_page_cache_start_worker(cache));
    puts("PASS unpublished cold read: destroy retains cache through private preparation");
#endif
    source_reads = read_entered = resume_read = 0; pause_read = 1;
    pthread_t threads[4];
    assert(!pthread_create(&threads[0], 0, get_cold_page, (void *)(uintptr_t)1));
    while (!__atomic_load_n(&read_entered, __ATOMIC_ACQUIRE)) sched_yield();
    for (unsigned i = 2; i <= actors; i++) assert(!pthread_create(&threads[i - 1], 0, get_cold_page, (void *)(uintptr_t)i));
    struct kernel_page_cache_concurrency_state state;
    do { kernel_page_cache_get_concurrency_state(cache, &state); sched_yield(); }
    while (state.entry_borrows != actors);
    assert(!state.running && !state.pending && source_first_cpu==1);
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
    assert(!state.running && !state.pending && source_first_cpu==1);
    __atomic_store_n(&resume_read, 1, __ATOMIC_RELEASE);
    for (unsigned i = 1; i <= actors; i++) assert(!pthread_join(threads[i - 1], 0));
    source_error = 0; expected_status = KERNEL_PAGE_CACHE_STATUS_OK;
    assert(source_reads == 1);
    /* Stopping the CPU0 worker does not own a caller's cold read. */
    pause_read=1;read_entered=resume_read=0;
    assert(!pthread_create(&threads[0],NULL,get_cold_page,(void *)(uintptr_t)1));
    wait_progress(&read_entered,1);
    kernel_page_cache_stop_worker(cache);
    assert(kernel_page_cache_destroy(cache)==KERNEL_PAGE_CACHE_STATUS_STATE);
    assert(!pthread_create(&threads[1],NULL,get_independent_page,NULL) && !pthread_join(threads[1],NULL));
    __atomic_store_n(&resume_read,1,__ATOMIC_RELEASE);
    assert(!pthread_join(threads[0],NULL) && !physical_page_release(pages,received[1]));
    kernel_page_cache_get_concurrency_state(cache,&state);
    assert(!state.entry_borrows && !state.running && !state.pending && state.requests_completed==state.requests_queued);
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
    writeback_error_publication(cache,&mount,actors);
    if(actors==2) {version_capacity_case(cache,&file);version_oom_case(cache,&file);version_prefix_cycle(cache,&file);version_empty_cycle(cache,&file);}
    foreground_window_truncate(cache,&file);
    dirty_index_case(cache,&mount,&file,actors);
    demand_cases(cache, &file, actors);
    executor_range_fairness(cache,&mount,actors,0);
    executor_range_fairness(cache,&mount,actors,1);
    if (actors==2)executor_range_stop(cache,&mount);
    if(actors==4)retain_after_cache_close(cache,&file);
    assert(!kernel_vfs_close(&file));
    assert(!kernel_vfs_unmount(&mount));
    assert(!close(image_fd));
    printf("PASS %u-thread real ext4 cold read: caller producer, one page/read, warm hit no allocation\n", actors);
}

static void *ordinary_writeback_waiter(void *argument)
{
    cache_test_attach(0);hold_ordinary_wait=1;
    assert(!kernel_page_cache_writeback(current_cache,kernel_vfs_file_node(argument)));
    hold_ordinary_wait=0;return NULL;
}
static void ordinary_reclaim_borrow(struct kernel_heap *heap,struct physical_page_allocator *pages,const char *path)
{
    struct kernel_page_cache cache={0};
    assert(!kernel_page_cache_init(&cache,heap,pages));
    current_cache=&cache;
    image_fd=open(path,O_RDWR);assert(image_fd>=0);
    struct stat st;assert(!fstat(image_fd,&st));
    struct kernel_block_device disk={.read=image_read,.write=image_write,.flush=image_flush,
        .capacity_bytes=st.st_size,.logical_block_size=512,.cache_mode=KERNEL_BLOCK_CACHE_WRITEBACK};
    struct kernel_vfs_mount mount={0};struct kernel_vfs_file file={0};
    assert(!kernel_vfs_mount_ext4(&mount,&disk,&backend_heap,&cache,0));
    assert(!kernel_vfs_create(&mount,"/ordinary-borrow",0600,&file));
    unsigned char bytes[BOAROS_PAGE_SIZE];memset(bytes,0x69,sizeof(bytes));size_t done;
    assert(!kernel_vfs_pwrite(&file,0,bytes,sizeof(bytes),&done) && done==sizeof(bytes));
    pause_write=1;write_entered=resume_write=0;
    ordinary_wait_entered=ordinary_wait_resume=0;
    assert(!kernel_page_cache_start_worker(&cache));
    uint64_t *held=malloc(physical_page_total(pages)*sizeof(*held));assert(held);size_t count=0;
    while (physical_page_available(pages)>physical_page_total(pages)/100)
        assert(!physical_page_allocate(pages,&held[count++]));
    wait_progress(&write_entered,1);
    pthread_t waiter;assert(!pthread_create(&waiter,NULL,ordinary_writeback_waiter,&file));
    wait_progress(&ordinary_wait_entered,1);
    struct kernel_page_cache_concurrency_state before,after;
    struct kernel_page_cache_statistics stats_before,stats_after;
    kernel_page_cache_get_concurrency_state(&cache,&before);
    kernel_page_cache_get_statistics(&cache,&stats_before);
    __atomic_store_n(&resume_write,1,__ATOMIC_RELEASE);
    wait_writeback_round(&cache,before.writeback_rounds);
    kernel_page_cache_get_concurrency_state(&cache,&after);
    kernel_page_cache_get_statistics(&cache,&stats_after);
    assert(stats_after.worker_written>stats_before.worker_written && after.entry_borrows &&
        after.index_live==before.index_live && !kernel_vfs_node_dirty_pages(kernel_vfs_file_node(&file))->count);
    /* 等待者仍借用元数据；物理引用数恢复为1不代表可以退休缓存项。 */
    __atomic_store_n(&ordinary_wait_resume,1,__ATOMIC_RELEASE);
    assert(!pthread_join(waiter,NULL));
    kernel_page_cache_stop_worker(&cache);
    for(size_t i=0;i<count;i++)assert(!physical_page_release(pages,held[i]));
    free(held);
    unsigned char actual[BOAROS_PAGE_SIZE];
    assert(!kernel_vfs_pread(&file,0,actual,sizeof(actual),&done, NULL) && done==sizeof(actual) && !memcmp(actual,bytes,sizeof(bytes)));
    assert(!kernel_vfs_close(&file) && !kernel_vfs_unmount(&mount));
    assert(!kernel_page_cache_destroy(&cache) && !close(image_fd));current_cache=NULL;
    puts("PASS ordinary pressure writeback: parked foreground keeps entry alive after the last physical pin; contents and owners returned");
}

static void executor_durable_progress(struct kernel_heap *heap,
    struct physical_page_allocator *pages,const char *path)
{
    struct kernel_page_cache cache={0};assert(!kernel_page_cache_init(&cache,heap,pages));
    current_cache=&cache;
    image_fd=open(path,O_RDWR);assert(image_fd>=0);
    struct stat st;assert(!fstat(image_fd,&st));
    struct kernel_block_device disk={.read=image_read,.write=image_write,.flush=image_flush,
        .capacity_bytes=st.st_size,.logical_block_size=512,.cache_mode=KERNEL_BLOCK_CACHE_WRITEBACK};
    struct kernel_vfs_mount mount={0};struct kernel_vfs_file writer={0},reader={0},closing={0};
    assert(!kernel_vfs_mount_ext4(&mount,&disk,&backend_heap,&cache,0));
    struct kernel_vfs_instance *instance=mount.private_data;
    const struct kernel_vfs_backend *backend=instance->ops;
    struct kernel_vfs_backend observed_backend=*backend;
    durable_prepare=observed_backend.sync_prepare;
    observed_backend.sync_prepare=observe_sync_prepare;
    instance->ops=&observed_backend;
    assert(!kernel_vfs_create(&mount,"/durable-writer",0600,&writer));
    assert(!kernel_vfs_create(&mount,"/durable-reader",0600,&reader));
    assert(!kernel_vfs_create(&mount,"/durable-closing",0600,&closing));
    unsigned char bytes[BOAROS_PAGE_SIZE];memset(bytes,0x73,sizeof(bytes));size_t done;
    assert(!kernel_vfs_pwrite(&reader,0,bytes,sizeof(bytes),&done) && done==sizeof(bytes));
    uint64_t observed=kernel_vfs_error_sequence(&reader);
    assert(!kernel_vfs_sync(&reader,0,&observed));
    observed=kernel_vfs_error_sequence(&closing);
    assert(!kernel_vfs_sync(&closing,0,&observed));
    assert(!kernel_page_cache_invalidate_node(&cache,kernel_vfs_file_node(&reader)));
    journal_wait_owner = mount.private_data;
    assert(!kernel_vfs_start_journal_worker(&mount) && !kernel_page_cache_start_worker(&cache));
    memset(bytes,0x35,sizeof(bytes));
    assert(!kernel_vfs_pwrite(&writer,0,bytes,sizeof(bytes),&done) && done==sizeof(bytes));
    durable_wait=(__typeof__(durable_wait)){.file=&writer,.armed=1};
    durable_before_park=(__typeof__(durable_before_park)){.hold=1};
    durable_cohort=(__typeof__(durable_cohort)){.file=&writer};
    executor_fairness=(__typeof__(executor_fairness)){.reader=kernel_vfs_file_node(&reader)};
    struct executor_sync sync={.file=&writer,.observed=kernel_vfs_error_sequence(&writer)};
    struct executor_sync siblings[7];pthread_t sibling_threads[7];
    pthread_t syncing,reading,close_thread;
    assert(!pthread_create(&syncing,NULL,executor_sync_client,&sync));
    wait_progress(&durable_wait.wait_entered,1);
    wait_progress(&durable_before_park.entered,1);
    wait_progress(&durable_wait.flush_entered,1);
    assert(durable_wait.target && !__atomic_load_n(&sync.done,__ATOMIC_ACQUIRE));
    for (unsigned i=0;i<7;i++) {
        siblings[i]=(struct executor_sync){.file=&writer,.observed=kernel_vfs_error_sequence(&writer)};
        assert(!pthread_create(&sibling_threads[i],NULL,executor_sync_client,&siblings[i]));
    }
    wait_progress(&durable_cohort.waiting,8);
    for (unsigned i=0;i<7;i++) assert(!__atomic_load_n(&siblings[i].done,__ATOMIC_ACQUIRE));
    /* 封口版本尚未持久化时应用继续改页；旧fsync只等待已捕获版本。 */
    memset(bytes,0x36,sizeof(bytes));
    assert(!kernel_vfs_pwrite(&writer,0,bytes,sizeof(bytes),&done) && done==sizeof(bytes));
    assert(!pthread_create(&reading,NULL,executor_cold_client,&reader));
    assert(!pthread_create(&close_thread,NULL,executor_close_client,&closing));
    /* FLUSH retains exclusive transfer ownership through completion. A cold disk read
     * waits there, while cached data and CPU0 close work remain independent. */
    for (;;) {
        unsigned waiting;
        { KERNEL_RAW_SCOPE(guard,&disk.transfer_lock.metadata);
          waiting=disk.transfer_lock.pending_head!=NULL; }
        if (waiting) break;
        sched_yield();
    }
    wait_progress(&executor_fairness.close_completed,1);
    unsigned char hot;
    assert(!kernel_vfs_pread(&writer,0,&hot,1,&done, NULL) && done==1 && hot==0x36);
    assert(!__atomic_load_n(&executor_fairness.read_completed,__ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&sync.done,__ATOMIC_ACQUIRE) && !pthread_join(close_thread,NULL));
    __atomic_store_n(&durable_wait.resume_flush,1,__ATOMIC_RELEASE);
    wait_progress(&durable_cohort.completed,8);
    assert(!__atomic_load_n(&sync.done,__ATOMIC_ACQUIRE));
    __atomic_store_n(&durable_before_park.release,1,__ATOMIC_RELEASE);
    assert(!pthread_join(syncing,NULL) && !sync.result && !pthread_join(reading,NULL));
    assert(__atomic_load_n(&executor_fairness.read_completed,__ATOMIC_ACQUIRE));
    for (unsigned i=0;i<7;i++) assert(!pthread_join(sibling_threads[i],NULL) && !siblings[i].result);
    assert(durable_cohort.prepared==8 && durable_cohort.completed==8);
    assert(kernel_vfs_node_dirty_pages(kernel_vfs_file_node(&writer))->count==1);
    unsigned char old;
    assert(!__real_kernel_vfs_node_pread(kernel_vfs_file_node(&writer),0,&old,1,&done) && done==1 && old==0x35);
    durable_wait=(__typeof__(durable_wait)){0};durable_cohort=(__typeof__(durable_cohort)){0};
    executor_fairness=(__typeof__(executor_fairness)){0};
    assert(!kernel_vfs_sync(&writer,0,&sync.observed));
    kernel_page_cache_stop_worker(&cache);
    instance->ops=backend;
    assert(!kernel_page_cache_invalidate_node(&cache,kernel_vfs_file_node(&writer)));
    unsigned char actual[BOAROS_PAGE_SIZE];
    assert(!kernel_vfs_pread(&writer,0,actual,sizeof(actual),&done, NULL) && done==sizeof(actual) && !memcmp(actual,bytes,sizeof(actual)));
    shared_error_cursor(&writer);
    assert(!kernel_vfs_close(&writer) && !kernel_vfs_close(&reader) && !kernel_vfs_unmount(&mount));
    assert(!kernel_page_cache_destroy(&cache) && !close(image_fd));current_cache=NULL;
    journal_wait_owner = NULL;
    puts("PASS durable caller wait: paused precommit flush retains physical I/O exclusion; hot read/CPU0 close progress; cold reader finishes after flush; fixed old version completes without clearing new dirt, next sync persists it and owners return");
}

static void executor_durable_fault(struct kernel_heap *heap,
    struct physical_page_allocator *pages,const char *path,int recover)
{
    struct kernel_page_cache cache={0};assert(!kernel_page_cache_init(&cache,heap,pages));
    current_cache=&cache;
    image_fd=open(path,O_RDWR);assert(image_fd>=0);
    struct stat st;assert(!fstat(image_fd,&st));
    struct kernel_block_device disk={.read=image_read,.write=image_write,.flush=image_flush,
        .capacity_bytes=st.st_size,.logical_block_size=512,.cache_mode=KERNEL_BLOCK_CACHE_WRITEBACK};
    struct kernel_vfs_mount mount={0};struct kernel_vfs_file files[3]={0};
    const char *names[]={"/durable-committed","/durable-error-a","/durable-error-b"};
    assert(!kernel_vfs_mount_ext4(&mount,&disk,&backend_heap,&cache,0));
    unsigned char bytes[BOAROS_PAGE_SIZE],actual[BOAROS_PAGE_SIZE];size_t done;
    if (recover) {
        for (unsigned i=0;i<3;i++) {
            assert(!kernel_vfs_open(&mount,names[i],&files[i]));
            assert(!kernel_vfs_pread(&files[i],0,actual,sizeof(actual),&done, NULL) && done==sizeof(actual));
            /* 失败fsync不承诺新版本持久化；已成功fsync的文件必须完整保留。 */
            unsigned char allowed=actual[0];
            assert(i?allowed==0x21 || allowed==0x42:allowed==0x23);
            for (size_t j=0;j<sizeof(actual);j++)assert(actual[j]==allowed);
            assert(!kernel_vfs_close(&files[i]));
        }
        assert(!kernel_vfs_unmount(&mount) && !kernel_page_cache_destroy(&cache) && !close(image_fd));
        current_cache=NULL;
        puts("PASS durable failure recovery: new process replays the journal, preserves committed data, reads complete allowed versions and releases owners");
        return;
    }
    for (unsigned i=0;i<3;i++) {
        assert(!kernel_vfs_create(&mount,names[i],0600,&files[i]));
        memset(bytes,i?0x21:0x23,sizeof(bytes));
        assert(!kernel_vfs_pwrite(&files[i],0,bytes,sizeof(bytes),&done) && done==sizeof(bytes));
        uint64_t observed=kernel_vfs_error_sequence(&files[i]);
        assert(!kernel_vfs_sync(&files[i],0,&observed));
    }
    struct lwext4_mount_adapter *adapter=mount.private_data;
    const struct kernel_vfs_backend *backend=adapter->instance.ops;
    struct kernel_vfs_backend observed_backend=*backend;
    durable_prepare=observed_backend.sync_prepare;observed_backend.sync_prepare=observe_sync_prepare;
    adapter->instance.ops=&observed_backend;
    assert(!kernel_vfs_start_journal_worker(&mount) && !kernel_page_cache_start_worker(&cache));
    memset(bytes,0x42,sizeof(bytes));
    for (unsigned i=1;i<3;i++)
        assert(!kernel_vfs_pwrite(&files[i],0,bytes,sizeof(bytes),&done) && done==sizeof(bytes));
    durable_wait=(__typeof__(durable_wait)){
        .file=&files[1],.second_file=&files[2],.armed=1,.fail_flush=1};
    struct executor_sync sync[2]={
        {.file=&files[1],.observed=kernel_vfs_error_sequence(&files[1])},
        {.file=&files[2],.observed=kernel_vfs_error_sequence(&files[2])}};
    pthread_t callers[2];
    assert(!pthread_create(&callers[0],NULL,executor_sync_client,&sync[0]));
    wait_progress(&durable_wait.wait_entered,1);wait_progress(&durable_wait.flush_entered,1);
    assert(!pthread_create(&callers[1],NULL,executor_sync_client,&sync[1]));
    wait_progress(&durable_wait.second_wait_entered,1);
    assert(!sync[0].done && !sync[1].done && adapter->sync_waiters);
    __atomic_store_n(&durable_wait.resume_flush,1,__ATOMIC_RELEASE);
    for (unsigned i=0;i<2;i++) {
        assert(!pthread_join(callers[i],NULL) && sync[i].result==-KERNEL_EIO);
        assert(sync[i].observed==kernel_vfs_error_sequence(&files[i+1]));
    }
    kernel_page_cache_stop_worker(&cache);
    struct kernel_page_cache_concurrency_state state;kernel_page_cache_get_concurrency_state(&cache,&state);
    assert(!state.running && !state.pending && !state.entry_borrows && state.requests_queued==state.requests_completed);
    struct jbd_journal *journal=adapter->device.fs->jbd_journal;
    assert(!adapter->sync_waiters && journal->error && journal->failed_trans && jbd_memory_used(journal) && journal->sources.rbh_root);
    int error=journal->error;
    adapter->instance.ops=backend;
    for (unsigned i=0;i<3;i++)assert(!kernel_vfs_close(&files[i]));
    assert(kernel_vfs_unmount(&mount)==-KERNEL_EIO && mount.private_data==adapter && journal->error==error);
    assert(journal->failed_trans && journal->sources.rbh_root && adapter->mounted && adapter->block_claimed);
    puts("PASS durable journal error: real flush EIO wakes both callers, consumes their errors and unlinks waiters; failed journal/version/device owners remain reachable until process restart");
    /* 错误挂载有意保留journal、版本和worker；本进程到此结束，下一进程重挂恢复。 */
    _Exit(0);
}

static int (*truncate_backend)(struct kernel_vfs_node *,uint64_t,uint64_t *,int *);
static uint64_t truncate_length;
static int truncate_partial_error;
static int hold_truncate_result(struct kernel_vfs_node *node,uint64_t size,uint64_t *actual,int *changed)
{
    int result=truncate_backend(node,truncate_partial_error ? BOAROS_PAGE_SIZE+17 : size,actual,changed);
    if (!result && truncate_partial_error) result=-KERNEL_EIO;
    truncate_after_backend=1;
    __atomic_store_n(&truncate_backend_done,1,__ATOMIC_RELEASE);
    while (!__atomic_load_n(&truncate_writer_attempt,__ATOMIC_ACQUIRE)) sched_yield();
    return result;
}
static void *truncate_before_publish(void *file)
{
    cache_test_attach(0);
    assert(kernel_vfs_ftruncate(file,truncate_length)==(truncate_partial_error ? -KERNEL_EIO : 0));
    truncate_after_backend=0;
    return NULL;
}
static void *writeback_after_truncate(void *file)
{
    cache_test_attach(0); truncate_writer=1;
    assert(!kernel_page_cache_writeback_range(current_cache,kernel_vfs_file_node(file),
        2*BOAROS_PAGE_SIZE,3*BOAROS_PAGE_SIZE));
    truncate_writer=0;
    __atomic_store_n(&truncate_writer_done,1,__ATOMIC_RELEASE);
    return NULL;
}
static void truncate_writeback_publication(struct kernel_page_cache *cache, const char *path)
{
    image_fd=open(path,O_RDWR); assert(image_fd>=0);
    struct stat st; assert(!fstat(image_fd,&st));
    struct kernel_block_device disk={.read=image_read,.write=image_write,.flush=image_flush,
        .capacity_bytes=st.st_size,.logical_block_size=512,.cache_mode=KERNEL_BLOCK_CACHE_WRITEBACK};
    struct kernel_vfs_mount mount={0};
    assert(!kernel_vfs_mount_ext4(&mount,&disk,&backend_heap,cache,0));
    for (unsigned scenario=0;scenario<3;scenario++) {
        struct kernel_vfs_file file={0}; char name[40];
        snprintf(name,sizeof(name),"/truncate-writeback-%u",scenario);
        assert(!kernel_vfs_create(&mount,name,0600,&file));
        unsigned char bytes[3*BOAROS_PAGE_SIZE]; memset(bytes,0x53,sizeof(bytes)); size_t done;
        assert(!kernel_vfs_pwrite(&file,0,bytes,sizeof(bytes),&done) && done==sizeof(bytes));
        struct kernel_vfs_node *node=kernel_vfs_file_node(&file);
        const struct kernel_vfs_backend *backend=node->instance->ops;
        struct kernel_vfs_backend observed=*backend;
        truncate_backend=backend->truncate; observed.truncate=hold_truncate_result;
        node->instance->ops=&observed; current_cache=cache;
        truncate_publish_lock=&node->io_lock;
        truncate_backend_done=truncate_writer_attempt=truncate_writer_done=0;
        truncate_length=scenario==1 ? BOAROS_PAGE_SIZE+17 : 0;
        truncate_partial_error=scenario==2;
        pthread_t truncator,writer;
        assert(!pthread_create(&truncator,NULL,truncate_before_publish,&file));
        while (!__atomic_load_n(&truncate_backend_done,__ATOMIC_ACQUIRE)) sched_yield();
        /* 后端已缩小但仍持 inode 写保护；旧尾页写回排队取读保护。
         * 第三种情况保留真实后端已接受的部分变化，并向 VFS 报告 EIO。 */
        assert(!pthread_create(&writer,NULL,writeback_after_truncate,&file));
        assert(!pthread_join(truncator,NULL) && !pthread_join(writer,NULL));
        node->instance->ops=backend; truncate_publish_lock=NULL;
        uint64_t retained=scenario ? BOAROS_PAGE_SIZE+17 : 0;
        assert(kernel_vfs_node_size(node)==retained);
        assert(!kernel_vfs_ftruncate(&file,sizeof(bytes)));
        memset(bytes,0xcc,sizeof(bytes));
        assert(!kernel_vfs_pread(&file,0,bytes,sizeof(bytes),&done, NULL) && done==sizeof(bytes));
        for (size_t i=0;i<sizeof(bytes);i++) {
            unsigned char expected=i<retained ? 0x53 : 0;
            if (bytes[i]!=expected) fprintf(stderr,"truncate scenario %u resurrected byte at %zu: 0x%x != 0x%x\n",
                scenario,i,bytes[i],expected);
            assert(bytes[i]==expected);
        }
        assert(!kernel_vfs_close(&file));
    }
    assert(!kernel_vfs_unmount(&mount) && !close(image_fd) && !kernel_page_cache_destroy(cache));
    current_cache=NULL;
    puts("PASS truncate publication: zero/partial shrink and partial EIO exclude queued old writeback, preserve prefix/zero regrowth and return owners");
}

struct pressure_isolation {
    struct kernel_vfs_file *failed,*healthy;
    int injected,error;
    size_t written;
};
static void pressure_isolation_write(void *argument)
{
    struct pressure_isolation *test=argument;
    unsigned char *bytes=malloc(1024*1024);assert(bytes);memset(bytes,0x45,1024*1024);
    size_t done;
    assert(!kernel_vfs_pwrite(test->failed,0,bytes,1024*1024,&done) && done==1024*1024);
    free(bytes);
    struct physical_page_allocator *pages=current_cache->allocator;
    uint64_t *held=calloc(physical_page_total(pages),sizeof(*held));assert(held);
    size_t count=0;uint64_t reserve=physical_page_total(pages)/100;
    while (physical_page_available(pages)>reserve) {
        if (physical_page_allocate(pages,&held[count])) break;
        count++;
    }
    write_error=test->injected;
    test->error=kernel_vfs_pwrite(test->healthy,0,"new!",4,&test->written);
    for (size_t i=0;i<count;i++) assert(!physical_page_release(pages,held[i]));
    free(held);
}
static void pressure_error_isolation(struct kernel_page_cache *cache,const char *path,int injected)
{
    image_fd=open(path,O_RDWR);assert(image_fd>=0);
    struct stat st;assert(!fstat(image_fd,&st));
    struct kernel_block_device disk={.read=image_read,.write=image_write,.flush=image_flush,
        .capacity_bytes=st.st_size,.logical_block_size=512,.cache_mode=KERNEL_BLOCK_CACHE_WRITEBACK};
    struct kernel_vfs_mount mount={0};
    assert(!kernel_vfs_mount_ext4(&mount,&disk,&backend_heap,cache,0));
    assert(!kernel_vfs_start_journal_worker(&mount));
    struct kernel_vfs_file failed={0},healthy={0};size_t done;uint64_t observed=0;
    assert(!kernel_vfs_create(&mount,"/failed",0600,&failed));
    assert(!kernel_vfs_create(&mount,"/healthy",0600,&healthy));
    assert(!kernel_vfs_pwrite(&healthy,0,"old!",4,&done) && done==4 && !kernel_vfs_sync(&healthy,0,&observed));
    current_cache=cache;assert(!kernel_page_cache_start_worker(cache));
    struct pressure_isolation test={.failed=&failed,.healthy=&healthy,.injected=injected};
    assert(!kernel_page_cache_run_cpu0(cache,pressure_isolation_write,&test));
    fprintf(stderr,"pressure unrelated writeback error=%d healthy error=%d written=%zu\n",
        injected==3?-KERNEL_ENOSPC:-KERNEL_EIO,test.error,test.written);
    assert(!test.error && test.written==4 && !write_error);
    assert(!kernel_vfs_error_sequence(&healthy) && kernel_vfs_error_sequence(&failed));
    assert(!kernel_vfs_sync(&healthy,0,&observed));
    char actual[4];assert(!kernel_vfs_pread(&healthy,0,actual,4,&done, NULL) && done==4 && !memcmp(actual,"new!",4));
    observed=0;
    assert(kernel_vfs_sync(&failed,0,&observed)==(injected==3?-KERNEL_ENOSPC:-KERNEL_EIO));
    assert(!kernel_vfs_sync(&failed,0,&observed));
    kernel_page_cache_stop_worker(cache);
    assert(!kernel_vfs_close(&failed) && !kernel_vfs_close(&healthy) && !kernel_vfs_unmount(&mount));
    assert(!kernel_page_cache_destroy(cache) && !close(image_fd));current_cache=NULL;
    puts("PASS write pressure error ownership: failed inode retains ENOSPC/EIO, independent overwrite and sync succeed, exact observers and all owners return");
}

struct pressure_write {
    struct kernel_vfs_file *file;
    unsigned char *bytes;
    size_t size, written;
    int error;
};
static void executor_pressure_write(void *argument)
{
    struct pressure_write *write=argument;
    assert(kernel_page_cache_is_executor(current_cache));
    while (write->written<write->size) {
        size_t done=0,chunk=1024*1024;
        if (chunk>write->size-write->written) chunk=write->size-write->written;
        int error=kernel_vfs_pwrite(write->file,write->written,
            write->bytes+write->written,chunk,&done);
        write->written+=done;
        if (error || done!=chunk) {
            if (!error) error=kernel_vfs_pwrite(write->file,write->written,
                write->bytes+write->written,1,&done);
            write->error=error;
            return;
        }
    }
}
static void write_pressure_case(struct kernel_page_cache *cache,const char *path,int mode)
{
    image_fd=open(path,O_RDWR); assert(image_fd>=0);
    struct stat st; assert(!fstat(image_fd,&st));
    struct kernel_block_device disk={.read=image_read,.write=image_write,.flush=image_flush,
        .capacity_bytes=st.st_size,.logical_block_size=512,.cache_mode=KERNEL_BLOCK_CACHE_WRITEBACK};
    struct kernel_vfs_mount mount={0};
    assert(!kernel_vfs_mount_ext4(&mount,&disk,&backend_heap,cache,0));
    assert(!kernel_vfs_start_journal_worker(&mount));
    struct kernel_vfs_file file={0};
    assert(!kernel_vfs_create(&mount,"/pressure",0600,&file));
    current_cache=cache;
    assert(!kernel_page_cache_start_worker(cache));
    size_t length=mode==1?48*1024*1024:24*1024*1024;
    uint64_t *held=NULL; size_t held_count=0;
    if (mode==2) write_error=1;
    if (mode==3) {
        held=calloc(physical_page_total(cache->allocator),sizeof(*held)); assert(held);
        while (!physical_page_allocate(cache->allocator,&held[held_count])) held_count++;
    }
    unsigned char *bytes=malloc(length); assert(bytes);
    for (size_t i=0;i<length;i++) bytes[i]=(unsigned char)(i*37+(i>>12));
    struct pressure_write write={.file=&file,.bytes=bytes,.size=length};
    assert(!kernel_page_cache_run_cpu0(cache,executor_pressure_write,&write));
    fprintf(stderr,"pressure mode=%d progress=%zu/%zu error=%d free=%llu\n",mode,
        write.written,length,write.error,(unsigned long long)physical_page_available(cache->allocator));
    if (mode==3) {
        assert(write.error==-KERNEL_ENOMEM && !write.written);
        for (size_t i=0;i<held_count;i++) assert(!physical_page_release(cache->allocator,held[i]));
        free(held);
    } else if (mode==2) {
        assert(write.written && write.written<length && !write_error);
        assert(kernel_vfs_file_node(&file)->writeback_error==-KERNEL_EIO);
        uint64_t observed=0;
        assert(kernel_vfs_sync(&file,0,&observed)==-KERNEL_EIO);
        assert(!kernel_vfs_ftruncate(&file,0));
    } else if (mode==1) {
        assert(write.error==-KERNEL_ENOSPC && write.written>16*1024*1024 && write.written<length);
        assert(!kernel_vfs_ftruncate(&file,0));
    } else {
        assert(!write.error && write.written==length);
        uint64_t observed=kernel_vfs_error_sequence(&file);
        assert(!kernel_vfs_sync(&file,0,&observed));
        assert(!kernel_page_cache_invalidate_node(cache,kernel_vfs_file_node(&file)));
        unsigned char actual[BOAROS_PAGE_SIZE];
        for (size_t offset=0;offset<length;offset+=sizeof(actual)) {
            size_t done;
            assert(!kernel_vfs_pread(&file,offset,actual,sizeof(actual),&done, NULL) &&
                done==sizeof(actual) && !memcmp(actual,bytes+offset,done));
        }
    }
    free(bytes);
    kernel_page_cache_stop_worker(cache);
    assert(!kernel_vfs_close(&file) && !kernel_vfs_unmount(&mount));
    assert(!kernel_page_cache_destroy(cache) && !close(image_fd)); current_cache=NULL;
    puts("PASS write admission: executor makes bounded dirty-page progress; disk space and memory exhaustion remain distinct; contents and owners returned");
}

static void *close_during_timestamp(void *unused)
{
    (void)unused; cache_test_attach(0);
    wait_progress(&timestamp_owner.entered,1);
    assert(!kernel_vfs_close(timestamp_owner.closing));
    __atomic_store_n(&timestamp_owner.close_done,1,__ATOMIC_RELEASE);
    return NULL;
}
static void indexed_reclaim(struct kernel_page_cache *cache,const char *path)
{
    image_fd=open(path,O_RDWR);assert(image_fd>=0);
    struct stat st;assert(!fstat(image_fd,&st));
    struct kernel_block_device disk={.read=image_read,.write=image_write,.flush=image_flush,
        .capacity_bytes=st.st_size,.logical_block_size=512,.cache_mode=KERNEL_BLOCK_CACHE_WRITEBACK};
    struct kernel_vfs_mount mount={0};struct kernel_vfs_file file={0};
    assert(!kernel_vfs_mount_ext4(&mount,&disk,&backend_heap,cache,0));
    assert(!kernel_vfs_create(&mount,"/reclaim-window",0600,&file));
    assert(!kernel_vfs_ftruncate(&file,129U*BOAROS_PAGE_SIZE));
    (void)kernel_page_cache_reclaim(cache,UINT64_MAX);
    uint64_t held[129];size_t valid;
    for (unsigned i=0;i<129;i++) assert(!kernel_page_cache_get(cache,&file,i,&held[i],&valid));
    assert(!physical_page_release(cache->allocator,held[128]));
    assert(kernel_page_cache_reclaim(cache,1)==1);
    uint64_t address;
    assert(kernel_page_cache_lookup(cache,&file,128,&address,&valid)==KERNEL_PAGE_CACHE_STATUS_NOT_FOUND);
    assert(!physical_page_release(cache->allocator,held[100]));
    assert(kernel_page_cache_reclaim(cache,1)==1);
    assert(kernel_page_cache_lookup(cache,&file,100,&address,&valid)==KERNEL_PAGE_CACHE_STATUS_NOT_FOUND);
    struct kernel_vfs_node *node=kernel_vfs_file_node(&file);
    {
        KERNEL_LOCK_SCOPE(writer);kernel_vfs_node_lock(node,&writer,1);
        kernel_page_cache_invalidate_range(cache,node,99U*BOAROS_PAGE_SIZE,100U*BOAROS_PAGE_SIZE);
    }
    assert(kernel_page_cache_lookup(cache,&file,99,&address,&valid)==KERNEL_PAGE_CACHE_STATUS_NOT_FOUND);
    assert(!physical_page_release(cache->allocator,held[99]));
    /* Touch the successor of a removed scan position before the next round. */
    assert(!kernel_page_cache_get(cache,&file,98,&address,&valid));
    assert(!physical_page_release(cache->allocator,address));
    for (unsigned i=0;i<128;i++) if (i!=99 && i!=100)
        assert(!physical_page_release(cache->allocator,held[i]));
    assert(kernel_page_cache_reclaim(cache,UINT64_MAX)==126 && !node->cache_pages);
    assert(!kernel_vfs_unlink(&mount,"/reclaim-window") && !kernel_vfs_close(&file));
    assert(!kernel_vfs_unmount(&mount) && !kernel_page_cache_destroy(cache) && !close(image_fd));
    puts("PASS indexed range/reclaim:128 pinned predecessors, later clean victim, removed/touched scan position and all owners returned");
}
static void caller_metadata_owners(struct kernel_page_cache *cache, const char *path)
{
    image_fd=open(path,O_RDWR); assert(image_fd>=0);
    struct stat st; assert(!fstat(image_fd,&st));
    struct kernel_block_device disk={.read=image_read,.write=image_write,.flush=image_flush,
        .capacity_bytes=st.st_size,.logical_block_size=512,.cache_mode=KERNEL_BLOCK_CACHE_WRITEBACK};
    struct kernel_vfs_mount mount={0};
    assert(!kernel_vfs_mount_ext4(&mount,&disk,&backend_heap,cache,0));
    struct kernel_vfs_file caller={0}, unrelated={0}, closing={0};
    assert(!kernel_vfs_create(&mount,"/caller",0600,&caller));
    assert(!kernel_vfs_create(&mount,"/unrelated",0600,&unrelated));
    assert(!kernel_vfs_create(&mount,"/closing",0600,&closing));
    struct kernel_vfs_node *other=kernel_vfs_file_node(&unrelated);
    metadata_nodes[0]=kernel_vfs_file_node(&caller);
    metadata_nodes[1]=other; metadata_nodes[2]=kernel_vfs_file_node(&closing);
    metadata_closed_mask=0;
    unsigned char bytes[BOAROS_PAGE_SIZE]; memset(bytes,0x53,sizeof(bytes)); size_t done;
    assert(!kernel_vfs_pwrite(&unrelated,0,bytes,sizeof(bytes),&done) && done==sizeof(bytes));
    uint64_t error=0; assert(!kernel_vfs_sync(&unrelated,0,&error));
    assert(!kernel_vfs_close(&unrelated) && other->cache_pages);
    struct kernel_vfs_instance *instance=mount.private_data;
    const struct kernel_vfs_backend *backend=instance->ops;
    struct kernel_vfs_backend observed=*backend;
    metadata_close_node=backend->close_node; observed.close_node=observe_metadata_close;
    instance->ops=&observed; metadata_closed=0;
    cache_test_attach(1);
    assert(kernel_page_cache_reclaim(cache,UINT64_MAX) && !other->cache_pages && !metadata_closed);
    assert(other->references==1);
    /* 另一 inode 的退休页仍持最后引用。调用者截断只能退休自己的页；
     * 真正的节点关闭由 CPU0 排空，不能顺便消费全局 cleanup 链。 */
    {
        KERNEL_LOCK_SCOPE(io); kernel_vfs_node_lock(kernel_vfs_file_node(&caller),&io,1);
        kernel_page_cache_invalidate_range(cache,kernel_vfs_file_node(&caller),0,BOAROS_PAGE_SIZE);
    }
    assert(!metadata_closed);
    timestamp_owner=(__typeof__(timestamp_owner)){.target=kernel_vfs_file_node(&caller),.closing=&closing};
    pthread_t closer; assert(!pthread_create(&closer,NULL,close_during_timestamp,NULL));
    metadata_owner_instance=instance;
    assert(!kernel_vfs_ftruncate(&caller,2*BOAROS_PAGE_SIZE));
    assert(!pthread_join(closer,NULL));
    assert(timestamp_owner.entered && timestamp_owner.close_done);
    timestamp_owner=(__typeof__(timestamp_owner)){0};
    metadata_owner_instance=NULL;
    cache_test_attach(0);
    error=0; assert(!kernel_vfs_sync(&caller,0,&error));
    assert(!kernel_vfs_close(&caller));
    assert(!kernel_page_cache_purge_mount(cache,&mount));
    assert(metadata_closed_mask==7);
    memset(metadata_nodes,0,sizeof(metadata_nodes));
    instance->ops=backend; metadata_close_node=NULL;
    assert(!kernel_vfs_unmount(&mount) && !close(image_fd) && !kernel_page_cache_destroy(cache));
    puts("PASS caller metadata ownership: handle timestamps and deferred CPU0 final close");
}

struct domain_disk { int fd; unsigned armed,entered,resume,fail_flush; };
static enum kernel_block_status domain_read(void *context,uint64_t offset,void *bytes,size_t size)
{
    struct domain_disk *disk=context;
    return pread(disk->fd,bytes,size,offset)==(ssize_t)size?0:KERNEL_BLOCK_STATUS_IO;
}
static enum kernel_block_status domain_write(void *context,uint64_t offset,const void *bytes,size_t size)
{
    struct domain_disk *disk=context;
    if (__atomic_exchange_n(&disk->armed,0,__ATOMIC_ACQ_REL)) {
        __atomic_store_n(&disk->entered,1,__ATOMIC_RELEASE);
        wait_progress(&disk->resume,1);
    }
    return pwrite(disk->fd,bytes,size,offset)==(ssize_t)size?0:KERNEL_BLOCK_STATUS_IO;
}
static enum kernel_block_status domain_flush(void *context)
{ struct domain_disk *disk=context;
  if (__atomic_exchange_n(&disk->fail_flush,0,__ATOMIC_ACQ_REL)) return KERNEL_BLOCK_STATUS_IO;
  return fsync(disk->fd)?KERNEL_BLOCK_STATUS_IO:0; }
static void writeback_start_failure(struct kernel_page_cache *cache,const char *path)
{
    struct domain_disk storage={.fd=open(path,O_RDWR),.fail_flush=1};assert(storage.fd>=0);
    struct stat st;assert(!fstat(storage.fd,&st));
    struct kernel_block_device disk={.context=&storage,.read=domain_read,.write=domain_write,
        .flush=domain_flush,.logical_block_size=512,.capacity_bytes=st.st_size,.cache_mode=KERNEL_BLOCK_CACHE_WRITEBACK};
    struct kernel_vfs_mount mount={0};
    assert(kernel_vfs_mount_ext4(&mount,&disk,&backend_heap,cache,0)==-KERNEL_EIO && mount.private_data);
    struct lwext4_mount_adapter *adapter=mount.private_data;
    struct kernel_writeback_statistics pressure;kernel_writeback_domain_snapshot(&adapter->instance.writeback,&pressure);
    struct jbd_memory_stats journal;jbd_memory_snapshot(adapter->device.fs->jbd_journal->memory,&journal);
    assert(pressure.metadata_bytes && pressure.metadata_bytes==journal.used-journal.cached-journal.borrowed &&
        disk.references==2 && kernel_page_cache_destroy(cache)==KERNEL_PAGE_CACHE_STATUS_STATE &&
        kernel_vfs_unmount(&mount)==-KERNEL_EIO && mount.private_data);
    assert(!close(storage.fd));
    puts("PASS failed journal-start FLUSH retains mount, cache, physical pressure owner and actual allocated metadata");
}
static void *domain_fast_writer(void *argument)
{
    cache_test_attach(9);struct kernel_vfs_file *file=argument;size_t done;
    unsigned char value=0x39;
    assert(!kernel_vfs_pwrite(file,BOAROS_PAGE_SIZE,&value,1,&done) && done==1);
    value=0;assert(!kernel_vfs_pread(file,BOAROS_PAGE_SIZE,&value,1,&done, NULL) && done==1 && value==0x39);
    return NULL;
}
static void writeback_domains(struct kernel_page_cache *cache,const char *path)
{
    struct kernel_heap *heap_for_domains=cache->heap;
    struct physical_page_allocator *pages_for_domains=cache->allocator;
    char other[1024];assert(snprintf(other,sizeof(other),"%s-other",path)<(int)sizeof(other));
    struct domain_disk storage[2]={{.fd=open(path,O_RDWR)},{.fd=open(other,O_RDWR)}};
    struct kernel_block_device disks[2]={0};struct kernel_vfs_mount mounts[2]={0};
    struct kernel_vfs_file files[2]={0};struct kernel_writeback_statistics stats[2],before;
    for (unsigned i=0;i<2;i++) {
        assert(storage[i].fd>=0);struct stat st;assert(!fstat(storage[i].fd,&st));
        disks[i]=(struct kernel_block_device){.context=&storage[i],.read=domain_read,.write=domain_write,
            .flush=domain_flush,.logical_block_size=512,.capacity_bytes=st.st_size,.cache_mode=KERNEL_BLOCK_CACHE_WRITEBACK};
        assert(!kernel_block_register(&disks[i],KERNEL_BLOCK_DEVICE_NUMBER(i)));
        assert(!kernel_vfs_mount_ext4(&mounts[i],&disks[i],&backend_heap,cache,0));
        assert(!kernel_vfs_create(&mounts[i],"/pressure-domain",0600,&files[i]));
    }
    struct kernel_vfs_instance *fast=mounts[0].private_data,*slow=mounts[1].private_data;
    assert(fast->writeback.domain!=slow->writeback.domain);
    kernel_writeback_domain_snapshot(&fast->writeback,&stats[0]);
    kernel_writeback_domain_snapshot(&slow->writeback,&stats[1]);
    assert(stats[0].physical && stats[1].physical && stats[0].device!=stats[1].device &&
        stats[0].high_bytes==stats[1].high_bytes);
    struct kernel_writeback_owner alias={0};
    assert(!kernel_page_cache_bind_writeback(cache,&disks[0],1,&alias) && alias.domain==fast->writeback.domain);
    kernel_writeback_domain_snapshot(&alias,&before);assert(before.high_bytes==stats[0].high_bytes);
    kernel_page_cache_unbind_writeback(&alias);
    struct kernel_block_device read_only_device={.read=domain_read,.context=&storage[0],
        .logical_block_size=512,.capacity_bytes=disks[0].capacity_bytes};
    struct kernel_writeback_owner read_only_owner={0};
    assert(!kernel_block_register(&read_only_device,KERNEL_BLOCK_DEVICE_NUMBER(2)));
    assert(!kernel_page_cache_bind_writeback(cache,&read_only_device,0,&read_only_owner) && !read_only_owner.domain);
    kernel_writeback_domain_snapshot(&fast->writeback,&before);assert(before.high_bytes==stats[0].high_bytes);
    kernel_page_cache_unbind_writeback(&read_only_owner);assert(!kernel_block_unregister(&read_only_device));
    unsigned char bytes[BOAROS_PAGE_SIZE];memset(bytes,0x72,sizeof(bytes));size_t done;
    assert(!kernel_vfs_pwrite(&files[0],0,bytes,sizeof(bytes),&done) && done==sizeof(bytes));
    kernel_writeback_domain_snapshot(&fast->writeback,&before);
    assert(before.dirty_bytes==BOAROS_PAGE_SIZE && !before.version_bytes);
    struct retained_version retained={0};retain_version=&retained;
    uint64_t observed=kernel_vfs_error_sequence(&files[0]);assert(!kernel_vfs_sync(&files[0],0,&observed));retain_version=NULL;
    kernel_writeback_domain_snapshot(&fast->writeback,&before);
    assert(!before.dirty_bytes && before.version_bytes==retained.source.bytes);
    retained.source.retain(retained.source.context);
    kernel_writeback_domain_snapshot(&fast->writeback,&stats[0]);assert(stats[0].version_bytes==before.version_bytes);
    retained.source.release(retained.source.context);
    kernel_writeback_domain_snapshot(&fast->writeback,&stats[0]);assert(stats[0].version_bytes==retained.source.bytes);
    struct jbd_memory_stats journal;struct lwext4_mount_adapter *adapter=(void *)fast;
    jbd_memory_snapshot(adapter->device.fs->jbd_journal->memory,&journal);
    assert(stats[0].metadata_bytes==journal.used-journal.cached-journal.borrowed);
    size_t pages=(2*stats[1].high_bytes)/BOAROS_PAGE_SIZE+4;
    for (size_t i=0;i<pages;i++)
        assert(!kernel_vfs_pwrite(&files[1],i*BOAROS_PAGE_SIZE,bytes,sizeof(bytes),&done) && done==sizeof(bytes));
    storage[1].armed=1;current_cache=cache;assert(!kernel_page_cache_start_worker(cache));
    wait_progress(&storage[1].entered,1);
    /* One cache intentionally serves both disks. Its worker is in slow I/O;
     * a fast-domain write must not join that worker's admission queue. */
    pthread_t fast_writer;assert(!pthread_create(&fast_writer,NULL,domain_fast_writer,&files[0]));
    assert(!pthread_join(fast_writer,NULL) && !storage[1].resume);
    assert(kernel_page_cache_destroy(cache)==KERNEL_PAGE_CACHE_STATUS_STATE);
    __atomic_store_n(&storage[1].resume,1,__ATOMIC_RELEASE);
    for (unsigned i=0;i<2;i++) {
        observed=kernel_vfs_error_sequence(&files[i]);assert(!kernel_vfs_sync(&files[i],0,&observed));
        kernel_writeback_domain_snapshot(i?&slow->writeback:&fast->writeback,&stats[i]);
        assert(!stats[i].dirty_bytes && stats[i].version_bytes==(i?0:retained.source.bytes));
        assert(!kernel_vfs_close(&files[i]));
    }
    kernel_page_cache_stop_worker(cache);
    struct kernel_writeback_domain *held_domain=fast->writeback.domain;
    assert(!kernel_vfs_unmount(&mounts[0]) && disks[0].references==1 &&
        kernel_block_unregister(&disks[0])==-KERNEL_EBUSY);
    assert(!kernel_vfs_unmount(&mounts[1]) && !disks[1].references && !kernel_block_unregister(&disks[1]));
    assert(!kernel_page_cache_destroy(cache) && *retained.data==0x72);
    struct kernel_page_cache replacement={0};struct kernel_writeback_owner joined={0};
    assert(!kernel_page_cache_init(&replacement,heap_for_domains,pages_for_domains));
    assert(!kernel_page_cache_bind_writeback(&replacement,&disks[0],1,&joined) && joined.domain==held_domain);
    kernel_writeback_domain_snapshot(&joined,&before);assert(before.version_bytes==retained.source.bytes);
    retained.source.release(retained.source.context);kernel_writeback_domain_snapshot(&joined,&before);
    assert(!before.version_bytes);
    kernel_page_cache_unbind_writeback(&joined);
    assert(!kernel_block_claim(&disks[0],&read_only_owner) &&
        !kernel_page_cache_bind_writeback(&replacement,&disks[0],0,&read_only_owner));
    assert(kernel_page_cache_destroy(&replacement)==KERNEL_PAGE_CACHE_STATUS_STATE);
    kernel_page_cache_unbind_writeback(&read_only_owner);kernel_block_release_claim(&disks[0],&read_only_owner);
    assert(!kernel_page_cache_destroy(&replacement));
    assert(!disks[0].references && !kernel_block_unregister(&disks[0]));
    for(unsigned i=0;i<2;i++)assert(!close(storage[i].fd));
    current_cache=NULL;
    puts("PASS physical writeback domains: shared-cache slow/fast isolation, shared-device quota, stable source single charge, journal ownership and final references");
}

static void dirty_age_case(struct kernel_page_cache *cache,const char *path)
{
    assert(BOAROS_PAGE_CACHE_DIRTY_AGE);
    image_fd=open(path,O_RDWR);assert(image_fd>=0);struct stat st;assert(!fstat(image_fd,&st));
    struct kernel_block_device disk={.read=image_read,.write=image_write,.flush=image_flush,
        .logical_block_size=512,.capacity_bytes=st.st_size,.cache_mode=KERNEL_BLOCK_CACHE_WRITEBACK};
    struct kernel_vfs_mount mount={0};struct kernel_vfs_file file={0};
    assert(!kernel_vfs_mount_ext4(&mount,&disk,&backend_heap,cache,0) && !kernel_vfs_create(&mount,"/aged",0600,&file));
    for (unsigned failure=0;failure<2;failure++) {
        age_waits=age_empty_wait=0;age_clock_offset=0;age_observing=1;current_cache=cache;
        unsigned char bytes[BOAROS_PAGE_SIZE];memset(bytes,(int)(0x60+failure),sizeof(bytes));size_t done;
        uint64_t start=kernel_time_monotonic_ns();
        assert(!kernel_vfs_pwrite(&file,0,bytes,sizeof(bytes),&done) && done==sizeof(bytes));
        uint64_t observed=kernel_vfs_error_sequence(&file);
        assert(!kernel_page_cache_start_worker(cache));wait_progress(&age_waits,1);
        assert(age_deadlines[0]>=start+UINT64_C(5000000000));
        assert(kernel_vfs_node_dirty_pages(kernel_vfs_file_node(&file))->count==1);
        write_error=failure;
        __atomic_store_n(&age_clock_offset,UINT64_C(5000000001),__ATOMIC_RELAXED);
        assert(!kernel_wait_notify(&age_tokens[0],KERNEL_WAIT_TIMEOUT));
        if (failure) {
            wait_progress(&age_waits,2);
            assert(!write_error && age_deadlines[1]>age_deadlines[0]+UINT64_C(5000000000) &&
                kernel_vfs_node_dirty_pages(kernel_vfs_file_node(&file))->count==1 &&
                kernel_vfs_error_sequence(&file)==observed+1);
            __atomic_store_n(&age_clock_offset,UINT64_C(10000000002),__ATOMIC_RELAXED);
            assert(!kernel_wait_notify(&age_tokens[1],KERNEL_WAIT_TIMEOUT));
        }
        wait_progress(&age_empty_wait,1);kernel_page_cache_stop_worker(cache);
        age_observing=0;age_clock_offset=0;
        assert(!kernel_vfs_node_dirty_pages(kernel_vfs_file_node(&file))->count);
        unsigned char actual;assert(!__real_kernel_vfs_node_pread(kernel_vfs_file_node(&file),0,&actual,1,&done) && done==1 && actual==bytes[0]);
        assert(kernel_vfs_sync(&file,0,&observed)==(failure?-KERNEL_EIO:0));
        assert(!kernel_vfs_sync(&file,0,&observed));
    }
    assert(!kernel_vfs_close(&file) && !kernel_vfs_unmount(&mount) && !kernel_page_cache_destroy(cache) && !close(image_fd));
    current_cache=NULL;
    puts("PASS dirty-age deadline: five-second registration, controlled expiry, bounded error retry, idle wait and owners");
}

struct progress_sync_request { struct kernel_vfs_file *file; int result; unsigned done; };
static void *progress_sync_client(void *argument)
{
    cache_test_attach(9);kernel_cpu_current()->logical_id=0;
    /* A distinct loop/progress task on CPU0 bypasses the cache execution queue. */
    struct progress_sync_request *request=argument;
    struct kernel_io_context *io=kernel_io_context_current();assert(!io->background_reclaim);
    io->background_reclaim=1;progress_writeback_caller=1;
    uint64_t observed=kernel_vfs_error_sequence(request->file);
    request->result=kernel_vfs_sync(request->file,0,&observed);
    progress_writeback_caller=0;io->background_reclaim=0;
    __atomic_store_n(&request->done,1,__ATOMIC_RELEASE);return NULL;
}
static void progress_writeback_owners(struct kernel_page_cache *cache,const char *path)
{
    image_fd=open(path,O_RDWR);assert(image_fd>=0);struct stat st;assert(!fstat(image_fd,&st));
    struct kernel_block_device disk={.read=image_read,.write=image_write,.flush=image_flush,
        .logical_block_size=512,.capacity_bytes=st.st_size,.cache_mode=KERNEL_BLOCK_CACHE_WRITEBACK};
    struct kernel_vfs_mount mount={0};assert(!kernel_vfs_mount_ext4(&mount,&disk,&backend_heap,cache,0));current_cache=cache;
    for(unsigned distinct=0;distinct<2;distinct++) {
        struct kernel_vfs_file files[2]={{0}};unsigned char bytes[BOAROS_PAGE_SIZE];size_t done;
        for(unsigned i=0;i<2;i++) {
            char name[32];snprintf(name,sizeof(name),"/progress-%u-%u",distinct,i);
            assert(!kernel_vfs_create(&mount,name,0600,&files[i]));memset(bytes,(int)(0x61+i),sizeof(bytes));
            assert(!kernel_vfs_pwrite(&files[i],0,bytes,sizeof(bytes),&done) && done==sizeof(bytes));
        }
        write_entered=resume_write=progress_writeback_waiting=0;pause_write=1;
        assert(!kernel_page_cache_start_worker(cache) && !kernel_page_cache_request_writeback(cache));
        wait_progress(&write_entered,1);
        struct progress_sync_request request={.file=&files[distinct]};pthread_t caller;
        assert(!pthread_create(&caller,NULL,progress_sync_client,&request));
        if(!distinct) {
            while(!__atomic_load_n(&progress_writeback_waiting,__ATOMIC_ACQUIRE) &&
                !__atomic_load_n(&request.done,__ATOMIC_ACQUIRE))sched_yield();
            if (request.done)fprintf(stderr,"progress caller prematurely returned %d from active writeback\n",request.result);
            assert(progress_writeback_waiting && !request.done);
            __atomic_store_n(&resume_write,1,__ATOMIC_RELEASE);
        }
        assert(!pthread_join(caller,NULL) && !request.result);
        if(distinct)__atomic_store_n(&resume_write,1,__ATOMIC_RELEASE);
        kernel_page_cache_stop_worker(cache);
        for(unsigned i=0;i<2;i++) {
            uint64_t observed=kernel_vfs_error_sequence(&files[i]);assert(!kernel_vfs_sync(&files[i],0,&observed));
            assert(!kernel_page_cache_invalidate_node(cache,kernel_vfs_file_node(&files[i])));
            memset(bytes,0,sizeof(bytes));assert(!kernel_vfs_pread(&files[i],0,bytes,sizeof(bytes),&done, NULL) && done==sizeof(bytes));
            for(size_t j=0;j<sizeof(bytes);j++)assert(bytes[j]==0x61+i);
            assert(!kernel_vfs_close(&files[i]));
        }
    }
    assert(!kernel_vfs_unmount(&mount) && !kernel_page_cache_destroy(cache) && !close(image_fd));current_cache=NULL;
    puts("PASS progress I/O keeps its own writeback workspace and waits for busy entries; worker versions, exact disk contents and owners survive");
}

#include "sync_goal_cases.h"

int main(int argc, char **argv)
{
    setvbuf(stdout,0,_IONBF,0);
    cache_test_attach(0);
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
    assert(!kernel_page_cache_init(&cache, &heap, &pages));
    struct kernel_page_cache_concurrency_state sizes;
    kernel_page_cache_get_concurrency_state(&cache, &sizes);
    printf("cache sizes: entry=%zu record=%zu request=%zu\n", sizes.entry_bytes, sizes.record_bytes, sizes.request_bytes);
    assert(pages.reclaim_hooks.reclaim && pages.reclaim_hooks.notify && pages.reclaim_hooks.wait &&
        pages.reclaim_hooks.snapshot && pages.reclaim_hooks.context);
    assert(kernel_page_cache_init(&cache, &heap, &pages) ==
        KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT);
    if (argc==3) {
        if (!strcmp(argv[2],"domain-start-failure")) {writeback_start_failure(&cache,argv[1]);return 0;}
        if (!strcmp(argv[2],"progress-owners")) {progress_writeback_owners(&cache,argv[1]);goto owners;}
        if (!strcmp(argv[2],"dirty-age")) {dirty_age_case(&cache,argv[1]);goto owners;}
        if (!strcmp(argv[2],"pressure-domains")) {writeback_domains(&cache,argv[1]);goto owners;}
        if (!strcmp(argv[2],"indexed-reclaim")) {
            indexed_reclaim(&cache,argv[1]);
            goto owners;
        }
#ifdef CACHE_MM_CASES
        if (!strcmp(argv[2],"mremap")) {
            image_fd=open(argv[1],O_RDWR); assert(image_fd>=0);
            struct stat st; assert(!fstat(image_fd,&st));
            struct kernel_block_device disk={.read=image_read,.write=image_write,.flush=image_flush,
                .capacity_bytes=st.st_size,.logical_block_size=512,.cache_mode=KERNEL_BLOCK_CACHE_WRITEBACK};
            struct kernel_vfs_mount mount={0};
            assert(!kernel_vfs_mount_ext4(&mount,&disk,&backend_heap,&cache,0));
            assert(!kernel_page_cache_start_worker(&cache));
            disk_remap_case(&heap,&pages,&mount);
            kernel_page_cache_stop_worker(&cache);
            assert(!kernel_vfs_unmount(&mount) && !close(image_fd) && !kernel_page_cache_destroy(&cache));
            goto owners;
        }
#endif
        if (!strcmp(argv[2],"caller-metadata-owners")) {
            caller_metadata_owners(&cache,argv[1]);
            goto owners;
        }
        if (!strcmp(argv[2],"write-pressure-other-eio") || !strcmp(argv[2],"write-pressure-other-enospc")) {
            pressure_error_isolation(&cache,argv[1],!strcmp(argv[2],"write-pressure-other-enospc")?3:1);
            goto owners;
        }
        if (!strncmp(argv[2],"write-pressure",14)) {
            int mode=!strcmp(argv[2],"write-pressure-full")?1:
                !strcmp(argv[2],"write-pressure-eio")?2:!strcmp(argv[2],"write-pressure-oom")?3:0;
            write_pressure_case(&cache,argv[1],mode);
            goto owners;
        }
        if (!strcmp(argv[2],"truncate-writeback")) {
            truncate_writeback_publication(&cache,argv[1]);
            goto owners;
        }
        if (!strcmp(argv[2],"executor-range")) {
            executor_range_cases(&cache,argv[1]);
            goto owners;
        }
        assert(!kernel_page_cache_destroy(&cache));
        if (!strcmp(argv[2],"datasync-times")) {
            datasync_times(&heap,&pages,argv[1]);goto owners;
        }
        if (!strcmp(argv[2],"durable-fixed-target")) {
            durable_fixed_target(&heap,&pages,argv[1]);goto owners;
        }
        if (!strcmp(argv[2],"durable-progress")) {
            executor_durable_progress(&heap,&pages,argv[1]);
            goto owners;
        }
        if (!strcmp(argv[2],"durable-error")) {
            executor_durable_fault(&heap,&pages,argv[1],0);
            return 0;
        }
        assert(!strcmp(argv[2],"durable-recover"));
        executor_durable_fault(&heap,&pages,argv[1],1);
        goto owners;
    }
    if (argc == 2) {
        cold_case(&cache, &heap, &pages, argv[1], 2);
        cold_case(&cache, &heap, &pages, argv[1], 4);
    }
    execution_case(&cache);
    destroy_version_case(&cache);
    if(argc==2)ordinary_reclaim_borrow(&heap,&pages,argv[1]);
    if(argc==2)executor_durable_progress(&heap,&pages,argv[1]);
owners:
    assert(!physical_page_allocator_audit(&pages));
    assert(physical_page_available(&pages) == baseline);
    struct kernel_heap_statistics backend_stats;
    kernel_heap_get_statistics(&backend_heap,&backend_stats);
    assert(!backend_stats.live_allocations && !backend_stats.current_pages &&
        physical_page_available(&backend_pages)==backend_baseline && !physical_page_allocator_audit(&backend_pages));
    puts("PASS ordinary cache construction: real pressure hooks, shared protection, owner return");
    return 0;
}
