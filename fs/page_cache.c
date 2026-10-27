#include "vfs_objects.h"
#include "write_source.h"
#include <misc/tree.h>

#include <kernel/cost.h>
#include <kernel/heap.h>
#include <kernel/sync.h>
#include <kernel/irq.h>
#include <kernel/file_mapping.h>
#include <arch/context.h>
#include <arch/platform_io.h>
#include <kernel/errno.h>
#include <kernel/page.h>
#include <kernel/task.h>
#include <kernel/accounting.h>
#include <kernel/time.h>
#include <kernel/page_cache.h>
#include <kernel/block.h>
#include <kernel/physical_page.h>
#include <kernel/vfs.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define PAGE_CACHE_INITIAL_CAPACITY 16U
#define PAGE_CACHE_TOMBSTONE \
    ((struct kernel_page_cache_entry *)(uintptr_t)1U)

struct kernel_page_cache_entry {
    uint64_t writes;
    uint8_t retry_blocked;
    uint8_t retired;
    struct kernel_page_cache_entry *lru_previous;
    struct kernel_page_cache_entry *lru_next;
    struct kernel_page_cache_entry *cleanup_next;
    RB_ENTRY(kernel_page_cache_entry) dirty_link;
    uint8_t dirty_indexed;
    struct kernel_page_cache_entry *node_next;
    struct kernel_page_cache_entry **node_previous;
    struct kernel_page_cache *cache;
    struct kernel_vfs_node *node;
    uint64_t page_index;
    uint64_t physical_address;
    size_t dirty_begin;
    size_t dirty_end;
    uint64_t generation;
    int writeback_error;
    uint8_t writeback;
    uint32_t users;
    uint32_t load_status;
    struct kernel_wait_queue ready;
    uint8_t page_references_owned;
    struct kernel_page_cache_alias *aliases;
#if BOAROS_PAGE_CACHE_DIRTY_BLOCKS
    uint64_t dirty_blocks;
#endif
};


#define CACHE_LOAD_EARLY UINT32_C(0x80000000)
/* 索引保护内读最终结果无需额外fence；提前发布的页先取得DMA内容可见性。
 * producer借用覆盖entry寿命，整批排空后在cache保护内清除EARLY。 */
static enum kernel_page_cache_status entry_load_status(const struct kernel_page_cache_entry *entry)
{
    uint32_t status = __atomic_load_n(&entry->load_status, __ATOMIC_RELAXED);
    if (status & CACHE_LOAD_EARLY) __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return (enum kernel_page_cache_status)(status & ~CACHE_LOAD_EARLY);
}
static int entry_loading(const struct kernel_page_cache_entry *entry)
{ return entry_load_status(entry) == KERNEL_PAGE_CACHE_STATUS_BUSY; }

struct page_cache_group {
    struct physical_page_allocator *allocator;
    struct kernel_heap *heap;
    struct kernel_page_cache_record *head, *cursor;
    struct kernel_wait_queue progress;
    struct kernel_raw_lock metadata;
    struct kernel_mutex walk;
    uint64_t refs, round, pending, progressed, dirty;
    struct kernel_writeback_domain *domains;
    uint64_t domain_count,share_high,share_low;
    unsigned closing;
};

struct kernel_writeback_domain {
    struct page_cache_group *group;
    struct kernel_block_device *device;
    struct kernel_writeback_domain *next;
    struct kernel_writeback_owner *members;
    uint64_t references;
    uint64_t dirty_bytes,version_bytes,metadata_bytes;
};
static void domain_charge(uint64_t *counter,size_t bytes,int acquiring)
{
    uint64_t old=acquiring?__atomic_fetch_add(counter,bytes,__ATOMIC_RELAXED):
        __atomic_fetch_sub(counter,bytes,__ATOMIC_RELAXED);
    if (acquiring?old>UINT64_MAX-bytes:old<bytes) __builtin_trap();
}
static uint64_t domain_bytes(const struct kernel_writeback_domain *domain)
{
    return __atomic_load_n(&domain->dirty_bytes,__ATOMIC_RELAXED)+
        __atomic_load_n(&domain->version_bytes,__ATOMIC_RELAXED)+
        __atomic_load_n(&domain->metadata_bytes,__ATOMIC_RELAXED);
}
/* Fixed shares change with membership, not with every page admission. */
static void domain_shares_locked(struct page_cache_group *group)
{
    uint64_t high=0,low=0,count=group->domain_count;
    if (count) {
        uint64_t total=physical_page_total(group->allocator)*BOAROS_PAGE_SIZE;
        high=total/10/count;low=total/20/count;
        if (high<BOAROS_PAGE_SIZE) high=BOAROS_PAGE_SIZE;
        if (low<BOAROS_PAGE_SIZE) low=BOAROS_PAGE_SIZE;
    }
    __atomic_store_n(&group->share_high,high,__ATOMIC_RELAXED);
    __atomic_store_n(&group->share_low,low,__ATOMIC_RELAXED);
}
static uint64_t domain_limit(const struct kernel_writeback_domain *domain,int low)
{
    return __atomic_load_n(low?&domain->group->share_low:&domain->group->share_high,__ATOMIC_RELAXED);
}
static uint64_t background_limit(uint64_t high)
{
    return high/100*BOAROS_PAGE_CACHE_BACKGROUND_PERCENT+
        high%100*BOAROS_PAGE_CACHE_BACKGROUND_PERCENT/100;
}
void kernel_writeback_domain_memory(void *context,size_t bytes,bool acquiring)
{
    struct kernel_writeback_domain *domain=context;
    domain_charge(&domain->metadata_bytes,bytes,acquiring);
}

#if BOAROS_PAGE_CACHE_READAHEAD_PAGES
struct readahead_job {
    struct kernel_vfs_node *node;
    uint64_t cookie, first;
    unsigned pages;
    int active, cancelled;
};
#endif

struct kernel_page_cache_record {
    struct kernel_mutex metadata;
    struct cache_request *reads_head, **reads_tail;
    struct kernel_io_context *executor_io;
    uint32_t read_requests, reads_running;
    /* Atomic lifetime: final unwind returns without reacquiring metadata. */
    uint32_t request_borrows;
    uint64_t requests_queued, requests_completed, entry_borrows;
    uint64_t deferred_snapshots;
    uint64_t executions_queued, executions_completed;
    uint64_t queue_ns, execute_ns, max_queue_ns, max_execute_ns;
    uint8_t force_writeback;
    struct page_cache_group *group;
    struct kernel_page_cache_record *next;
    struct kernel_page_cache *owner;
    uint64_t wait_round;
    struct kernel_page_cache_entry **buckets;
    struct kernel_page_cache_entry *lru_head;
    struct kernel_page_cache_entry *lru_tail;
    struct kernel_page_cache_entry *cleanup_entries;
    struct kernel_page_cache_statistics statistics;
    size_t capacity;
    size_t occupied;
    size_t tombstones;
    unsigned int writeback_active;
    uint64_t dirty_pages;
    struct kernel_page_cache_dirty *dirty_head, *dirty_tail, *dirty_cursor;
    uint64_t dirty_remaining, dirty_scan_index;
    struct kernel_thread_join worker;
    struct kernel_wait_queue work, progress;
    uint64_t low, high, dirty_high, dirty_low;
    uint64_t completed;
    _Atomic uint64_t requested;
    _Atomic int started, stopping;
    struct version_pool *versions;
    size_t writeback_capacity;
    struct writeback_workspace *writeback_workspace;
    union foreground_workspace *foreground_workspace;
#if BOAROS_PAGE_CACHE_READAHEAD_PAGES
    struct readahead_job readahead_jobs[8];
    struct kernel_thread_join readahead_worker;
    struct kernel_wait_queue readahead_work;
    uint64_t readahead_cookie;
    int readahead_started;
#endif
    struct kernel_page_cache_entry *reclaim_cursor;
    uint32_t mounts;
#if BOAROS_PAGE_CACHE_DIRTY_AGE
    uint64_t dirty_deadline;
    unsigned expired_writeback;
#endif
};

static int dirty_compare(struct kernel_page_cache_entry *a,struct kernel_page_cache_entry *b)
{
    return a->page_index<b->page_index?-1:a->page_index>b->page_index;
}
RB_GENERATE_INTERNAL(kernel_page_cache_dirty_tree,kernel_page_cache_entry,dirty_link,dirty_compare,static inline)
static struct kernel_page_cache_entry *dirty_at_or_after(struct kernel_vfs_node *node,uint64_t index)
{
    struct kernel_page_cache_entry key={.page_index=index};
    return RB_NFIND(kernel_page_cache_dirty_tree,&kernel_vfs_node_dirty_pages(node)->tree,&key);
}

/* caller的等待现场持有节点及file借用，DONE之前不得退栈；worker只借用。
 * 请求发布不等于缓存页loading发布，后者须在CPU0取得inode保护后完成。 */
struct cache_request {
    struct cache_request *next;
    void (*execute)(void *);
    void *argument;
    const int *finished;
    struct kernel_task *account_task;
    uint64_t queued_at;
    unsigned observed;
    enum kernel_page_cache_status status;
    unsigned done;
    struct kernel_wait_queue changed;
};

#define CACHE_SCOPE(name, cache) \
    KERNEL_LOCK_SCOPE(name); \
    kernel_mutex_lock(&(cache)->record->metadata, &(name))

struct read_workspace {
    struct file_read_workspace backend;
    struct kernel_page_cache_entry *entries[KERNEL_PAGE_CACHE_READ_BATCH_MAX];
    struct kernel_page_cache_entry *loads[KERNEL_PAGE_CACHE_READ_BATCH_MAX];
    struct file_read_span spans[KERNEL_PAGE_CACHE_READ_BATCH_MAX];
    struct kernel_page_cache_read_page *outputs[KERNEL_PAGE_CACHE_READ_BATCH_MAX];
};
struct prepared_writeback {
    struct cache_version *version;
    uint64_t generation;
#if BOAROS_PAGE_CACHE_DIRTY_BLOCKS
    uint64_t blocks;
#endif
    uint32_t begin,end;
    uint32_t written;
    uint8_t closed;
};
struct writeback_storage {
    struct prepared_writeback prepared[BOAROS_PAGE_CACHE_WRITEBACK_PAGES];
    struct file_write_span spans[BOAROS_PAGE_CACHE_WRITEBACK_PAGES];
    struct kernel_page_cache_entry *selected[BOAROS_PAGE_CACHE_WRITEBACK_PAGES];
};
union foreground_workspace {
    struct read_workspace read;
    struct writeback_storage writeback;
};
_Static_assert(sizeof(union foreground_workspace) <= 2U*BOAROS_PAGE_SIZE,
    "foreground descriptors must fit two physical pages");

/* cache保留一个空闲控制区；借出后由调用现场独占。竞争者取得独立区，
 * 不在inode I/O资格或loading页借用下等待另一批归还。 */
static union foreground_workspace *foreground_workspace_take(struct kernel_page_cache *cache)
{
    union foreground_workspace *workspace;
    {
        CACHE_SCOPE(guard,cache);
        workspace=cache->record->foreground_workspace;
        cache->record->foreground_workspace=NULL;
    }
    if (!workspace && kernel_heap_allocate(cache->heap,sizeof(*workspace),(void **)&workspace)) return NULL;
    return workspace;
}
static void foreground_workspace_put(struct kernel_page_cache *cache,union foreground_workspace *workspace)
{
    {
        CACHE_SCOPE(guard,cache);
        if (!cache->record->foreground_workspace) {
            cache->record->foreground_workspace=workspace;
            return;
        }
    }
    kernel_heap_release(cache->heap,workspace);
}
struct read_batch_load {
    struct kernel_page_cache *cache;
    const struct kernel_vfs_file *file;
    uint64_t first;
    struct kernel_page_cache_read_page *pages;
    size_t count,candidates;
    struct kernel_page_cache_entry **entries,**loads;
    struct kernel_page_cache_read_page **outputs;
    struct file_read_span *spans;
    struct file_read_workspace *backend;
    enum kernel_page_cache_status allocation;
};

static int remote_cache_client(void)
{
#if BOAROS_MAX_CPUS > 1
    if (kernel_cpu_current()->logical_id != 0) return 1;
    const struct kernel_mm *bound=0;
    struct kernel_task *task=kernel_task_current();
    if (!task || kernel_task_mm_borrow(task,&bound)!=KERNEL_TASK_STATUS_OK) return 0;
#if BOAROS_USER_SMP
    if (kernel_task_has_user_context(task) && kernel_scheduler_user_service_active()) return 0;
#endif
    return 1;
#else
    return 0;
#endif
}
static void cache_wake(struct kernel_wait_queue *queue)
{
    KERNEL_IRQ_SCOPE(irq);
    enum kernel_scheduler_status status = kernel_wait_queue_wake_all(queue);
    if (status && !(status == KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED &&
        !__atomic_load_n(&queue->registrations,__ATOMIC_ACQUIRE) && !queue->borrows)) __builtin_trap();
}


/* A pool outlives the cache while a journal/DMA borrower still owns a
 * version. Idle entries have no borrowers; their page and object move together. */
#define CACHE_VERSION_IDLE_MAX (262144U / BOAROS_PAGE_SIZE)
struct version_pool {
    struct kernel_raw_lock lock;
    struct kernel_heap *heap;
    struct physical_page_allocator *allocator;
    struct cache_version *idle;
    size_t idle_count, reserve;
    unsigned references, closed, stopping;
    struct kernel_wait_queue ready;
};
struct cache_version {
    struct version_pool *pool;
    struct cache_version *next;
    uint64_t physical;
    unsigned references;
    struct kernel_writeback_domain *domain;
    void *data;
};
static size_t version_bytes(void)
{
    size_t owner=KERNEL_HEAP_ALIGNMENT;
    while (owner<sizeof(struct cache_version)) owner*=2;
    return BOAROS_PAGE_SIZE+owner;
}
static void version_pool_put(struct version_pool *pool)
{
    unsigned old=__atomic_fetch_sub(&pool->references,1,__ATOMIC_ACQ_REL);
    if (!old) __builtin_trap();
    if (old!=1) return;
    if (!pool->closed || pool->idle || kernel_wait_queue_destroy(&pool->ready))
        __builtin_trap();
    kernel_heap_release(pool->heap,pool);
}
static struct version_pool *version_pool_new(struct kernel_heap *heap,
    struct physical_page_allocator *allocator)
{
    struct version_pool *pool;
    if (kernel_heap_allocate(heap,sizeof(*pool),(void **)&pool)) return NULL;
    *pool=(struct version_pool){.heap=heap,.allocator=allocator,.references=1};
    kernel_raw_lock_init(&pool->lock,KERNEL_RAW_RANK_OBJECT);
    kernel_wait_queue_init(&pool->ready);
    return pool;
}
static struct cache_version *version_new(struct version_pool *pool)
{
    struct cache_version *version;
    enum kernel_heap_status heap_status=kernel_heap_allocate(pool->heap,sizeof(*version),(void **)&version);
    if (heap_status) {
        if (heap_status!=KERNEL_HEAP_STATUS_EMPTY) __builtin_trap();
        return NULL;
    }
    uint64_t address;
    enum physical_page_status status=physical_page_allocate(pool->allocator,&address);
    if (status) {
        if (status!=PHYSICAL_PAGE_STATUS_EMPTY) __builtin_trap();
        kernel_heap_release(pool->heap,version);
        return NULL;
    }
    unsigned old=__atomic_fetch_add(&pool->references,1,__ATOMIC_ACQ_REL);
    if (!old || old==UINT32_MAX) __builtin_trap();
    void *data;
    if (physical_page_map_owned(pool->allocator,address,&data)) __builtin_trap();
    *version=(struct cache_version){.pool=pool,.physical=address,.references=1,.data=data};
    return version;
}
static void version_dispose(struct cache_version *version)
{
    struct version_pool *pool=version->pool;
    if (physical_page_release(pool->allocator,version->physical)) __builtin_trap();
    kernel_heap_release(pool->heap,version);
    version_pool_put(pool);
}
static void version_retain(void *owner)
{
    struct cache_version *version=owner;
    unsigned old=__atomic_fetch_add(&version->references,1,__ATOMIC_ACQ_REL);
    if (!old || old==UINT32_MAX) __builtin_trap();
}
static void version_release(void *owner)
{
    struct cache_version *version=owner;
    unsigned old=__atomic_fetch_sub(&version->references,1,__ATOMIC_ACQ_REL);
    if (!old) __builtin_trap();
    if (old!=1) return;
    struct kernel_writeback_domain *domain=version->domain;
    if (domain) {
        version->domain=NULL;domain_charge(&domain->version_bytes,version_bytes(),0);
        kernel_writeback_domain_put(domain);
    }
    struct version_pool *pool=version->pool;
    {
        KERNEL_RAW_SCOPE(guard,&pool->lock);
        if (!pool->closed && pool->idle_count<CACHE_VERSION_IDLE_MAX) {
            version->next=pool->idle; pool->idle=version; pool->idle_count++;
            cache_wake(&pool->ready);
            return;
        }
    }
    version_dispose(version);
}
/* Detach only idle owners under the raw lock. Physical/heap releases can take
 * lower-ranked allocator locks and therefore always happen after unlocking. */
static uint64_t version_pool_reclaim(struct version_pool *pool,uint64_t target)
{
    struct cache_version *retired=NULL;
    uint64_t count=0;
    {
        KERNEL_RAW_SCOPE(guard,&pool->lock);
        while (pool->idle && pool->idle_count>pool->reserve && count<target) {
            struct cache_version *version=pool->idle;
            pool->idle=version->next; pool->idle_count--;
            version->next=retired; retired=version; count++;
        }
    }
    while (retired) {
        struct cache_version *version=retired;retired=version->next;
        version_dispose(version);
    }
    return count;
}
static void version_pool_close(struct version_pool *pool)
{
    {
        KERNEL_RAW_SCOPE(guard,&pool->lock);
        if (pool->closed) __builtin_trap();
        pool->closed=1;pool->reserve=0;
    }
    enum kernel_scheduler_status status=kernel_wait_queue_close(&pool->ready);
    if (status && status!=KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED) __builtin_trap();
    (void)version_pool_reclaim(pool,UINT64_MAX);
    version_pool_put(pool);
}
static struct cache_version *version_take_page(struct kernel_page_cache *cache)
{
    struct version_pool *pool=cache->record->versions;
    {
        KERNEL_RAW_SCOPE(guard,&pool->lock);
        if (pool->closed) __builtin_trap();
        struct cache_version *version=pool->idle;
        if (version) {
            pool->idle=version->next;pool->idle_count--;
            version->next=NULL;
            if (__atomic_exchange_n(&version->references,1,__ATOMIC_ACQ_REL)) __builtin_trap();
            return version;
        }
    }
    return version_new(pool);
}
static struct cache_version *version_take(struct kernel_page_cache *cache,struct kernel_vfs_node *node)
{
    struct cache_version *version=version_take_page(cache);
    if (version) {
        if (version->domain) __builtin_trap();
        struct kernel_writeback_domain *domain=node->instance->writeback.domain;
        if (domain) {
            kernel_writeback_domain_get(domain);version->domain=domain;
            domain_charge(&domain->version_bytes,version_bytes(),1);
        }
    }
    return version;
}
/* No entry writeback qualification or prepared versions may be held here:
 * a foreground borrower may need those entries before returning its version. */
static int version_wait(struct kernel_page_cache *cache)
{
    struct version_pool *pool=cache->record->versions;
    struct kernel_wait_token token={0};
    {
        KERNEL_RAW_SCOPE(guard,&pool->lock);
        if (pool->stopping) return 0;
        if (pool->idle) return 1;
        if (kernel_wait_prepare(&pool->ready,0,0,&token)) __builtin_trap();
    }
    enum kernel_wait_wake_reason reason;
    if (kernel_wait_park(&token,&reason) || kernel_wait_finish(&token)) __builtin_trap();
    return 1;
}
static size_t version_pool_reserve(struct version_pool *pool)
{
    size_t count;
    {
        KERNEL_RAW_SCOPE(guard,&pool->lock);
        pool->stopping=0;
        pool->reserve=BOAROS_PAGE_CACHE_WRITEBACK_PAGES;
        count=pool->idle_count;
    }
    while (count<BOAROS_PAGE_CACHE_WRITEBACK_PAGES) {
        struct cache_version *version=version_new(pool);
        if (!version) break;
        version_release(version);count++;
    }
    if (count>BOAROS_PAGE_CACHE_WRITEBACK_PAGES) count=BOAROS_PAGE_CACHE_WRITEBACK_PAGES;
    {KERNEL_RAW_SCOPE(guard,&pool->lock);pool->reserve=count;}
    return count;
}
static void version_pool_unreserve(struct version_pool *pool)
{
    KERNEL_RAW_SCOPE(guard,&pool->lock);pool->reserve=0;
}
static void version_pool_stop(struct version_pool *pool)
{
    KERNEL_RAW_SCOPE(guard,&pool->lock);
    pool->stopping=1;cache_wake(&pool->ready);
}
static void *version_data(struct cache_version *version)
{
    /* 页由version/pool持续持有；借用期间映射不变，不逐span重查分配器。 */
    return version->data;
}

/* index保护内取得/归还entry借用；实际页引用与此存储借用分开。 */
static void entry_borrow(struct kernel_page_cache_entry *entry)
{
    if (entry->users == UINT32_MAX || entry->cache->record->entry_borrows == UINT64_MAX) __builtin_trap();
    entry->users++; entry->cache->record->entry_borrows++;
}
static void entry_return(struct kernel_page_cache_entry *entry)
{
    if (!entry->users || !entry->cache->record->entry_borrows) __builtin_trap();
    entry->users--; entry->cache->record->entry_borrows--;
    if (!entry->users && entry->retired && entry->cache->record->started) {
        entry->cache->record->requested=1; cache_wake(&entry->cache->record->work);
    }
}

static void readahead_cancel_node(struct kernel_page_cache *cache, struct kernel_vfs_node *node);
static void group_notify(void *context);
static void pressure_wait(void *context);
static size_t entry_valid_bytes(const struct kernel_page_cache_entry *entry);

static int cache_live(const struct kernel_page_cache *cache)
{
    return cache != 0 && cache->state == KERNEL_PAGE_CACHE_LIVE &&
           cache->heap != 0 && cache->allocator != 0 &&
           cache->record != 0 && cache->record->buckets != 0 &&
           cache->record->capacity >= PAGE_CACHE_INITIAL_CAPACITY;
}


static uint64_t mix_key(const struct kernel_vfs_node *node,
                        uint64_t page_index)
{
    uint64_t value = (uint64_t)(uintptr_t)node;

    value ^= page_index + UINT64_C(0x9e3779b97f4a7c15) +
             (value << 6U) + (value >> 2U);
    value ^= value >> 30U;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27U;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31U);
}

static size_t find_bucket(struct kernel_page_cache_record *record,
                          const struct kernel_vfs_node *node,
                          uint64_t page_index,
                          int *found)
{
    size_t mask = record->capacity - 1U;
    size_t index = (size_t)mix_key(node, page_index) & mask;
    size_t first_tombstone = SIZE_MAX;

    for (;;) {
        COST_ADD(CACHE_PROBES, 1);
        struct kernel_page_cache_entry *entry = record->buckets[index];

        if (entry == 0) {
            *found = 0;
            return first_tombstone != SIZE_MAX ? first_tombstone : index;
        }
        if (entry == PAGE_CACHE_TOMBSTONE) {
            if (first_tombstone == SIZE_MAX) {
                first_tombstone = index;
            }
        } else if (entry->node == node &&
                   entry->page_index == page_index) {
            *found = 1;
            return index;
        }
        index = (index + 1U) & mask;
    }
}

/* 候选构造在保护外；分配睡眠后以当前表复查，发布后旧表不再被借用。 */
static size_t required_capacity(struct kernel_page_cache_record *r,size_t count)
{
    if (count>SIZE_MAX-r->occupied) return 0;
    size_t needed=r->occupied+count,capacity=r->capacity;
    while (needed>capacity-capacity/4) {
        if (capacity>SIZE_MAX/2) return 0;
        capacity*=2;
    }
    return capacity;
}
static int reserve_buckets(struct kernel_page_cache *cache,size_t count)
{
    for (;;) {
        size_t old_capacity,capacity;
        {
            CACHE_SCOPE(guard,cache);
            struct kernel_page_cache_record *r=cache->record;
            if (count<=r->capacity-r->capacity/4-r->occupied &&
                r->tombstones<=r->capacity-r->capacity/4-r->occupied-count) return 1;
            old_capacity=r->capacity;
            capacity=required_capacity(r,count);
            if (!capacity) return 0;
        }
        struct kernel_page_cache_entry **buckets;
        if (kernel_heap_allocate_zeroed(cache->heap,capacity,sizeof(*buckets),(void **)&buckets)) return 0;
        struct kernel_page_cache_entry **retired=NULL;
        int published=0,satisfied=0;
        {
            CACHE_SCOPE(guard,cache);
            struct kernel_page_cache_record *r=cache->record;
            satisfied=count<=r->capacity-r->capacity/4-r->occupied &&
                r->tombstones<=r->capacity-r->capacity/4-r->occupied-count;
            if (!satisfied && r->capacity==old_capacity && capacity==required_capacity(r,count)) {
                retired=r->buckets;
                r->buckets=buckets;r->capacity=capacity;r->tombstones=0;
                for (size_t i=0;i<old_capacity;i++) {
                    struct kernel_page_cache_entry *entry=retired[i];
                    if (!entry || entry==PAGE_CACHE_TOMBSTONE) continue;
                    int found;size_t destination=find_bucket(r,entry->node,entry->page_index,&found);
                    if (found) __builtin_trap();
                    buckets[destination]=entry;
                }
                published=1;
            }
        }
        kernel_heap_release(cache->heap,published?retired:buckets);
        if (published || satisfied) return 1;
    }
}

static void lru_remove(struct kernel_page_cache_record *record,
                       struct kernel_page_cache_entry *entry)
{
    if (record->reclaim_cursor==entry) record->reclaim_cursor=entry->lru_previous;
    if (entry->lru_previous != 0) {
        entry->lru_previous->lru_next = entry->lru_next;
    } else {
        record->lru_head = entry->lru_next;
    }
    if (entry->lru_next != 0) {
        entry->lru_next->lru_previous = entry->lru_previous;
    } else {
        record->lru_tail = entry->lru_previous;
    }
    entry->lru_previous = 0;
    entry->lru_next = 0;
}

static void lru_insert_head(struct kernel_page_cache_record *record,
                            struct kernel_page_cache_entry *entry)
{
    entry->lru_previous = 0;
    entry->lru_next = record->lru_head;
    if (record->lru_head != 0) {
        record->lru_head->lru_previous = entry;
    } else {
        record->lru_tail = entry;
    }
    record->lru_head = entry;
}

static void lru_touch(struct kernel_page_cache_record *record,
                      struct kernel_page_cache_entry *entry)
{
    if (record->lru_head == entry) {
        return;
    }
    lru_remove(record, entry);
    lru_insert_head(record, entry);
}

static int cleanup_entry(struct kernel_page_cache *cache,
                         struct kernel_page_cache_entry *entry,
                         uint64_t *pages_released)
{
    {
        CACHE_SCOPE(guard,cache);
        /* 退休项已禁止新借用；在索引保护内确认最后借用结束后，
         * cleanup持有唯一存储owner，实际页/node/堆释放留在锁外。 */
        if (entry->users || (__atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)&UINT32_MAX)) return 0;
    }
    while (entry->page_references_owned != 0U) {
        (void)physical_page_release(cache->allocator,
                                  entry->physical_address);
        entry->page_references_owned--;
        (*pages_released)++;
    }
    entry->physical_address = 0U;
    if (entry->node != 0 &&
        kernel_vfs_node_release(&entry->node) != 0) {
        return 0;
    }
    kernel_heap_release(cache->heap, entry);
    return 1;
}

static enum kernel_page_cache_status abandon_entry(
    struct kernel_page_cache *cache,
    struct kernel_page_cache_entry *entry,
    enum kernel_page_cache_status result)
{
    uint64_t pages_released = 0U;

    if (cleanup_entry(cache, entry, &pages_released)) {
        return result;
    }
    { CACHE_SCOPE(guard,cache);
      entry->cleanup_next = cache->record->cleanup_entries;
      cache->record->cleanup_entries = entry; }
    return KERNEL_PAGE_CACHE_STATUS_CLEANUP_REQUIRED;
}

static void take_cleanup_entries(struct kernel_page_cache *cache,
    struct kernel_page_cache_entry **entries)
{
    CACHE_SCOPE(guard,cache);
    *entries=cache->record->cleanup_entries;
    cache->record->cleanup_entries=NULL;
}

static int drain_cleanup_entries(struct kernel_page_cache *cache,
    struct kernel_page_cache_entry **entries, size_t limit, uint64_t *pages_released)
{
    int failed=0;
    while (*entries && limit--) {
        struct kernel_page_cache_entry *entry=*entries;
        *entries=entry->cleanup_next;
        if (!cleanup_entry(cache,entry,pages_released)) {
            CACHE_SCOPE(guard,cache);
            entry->cleanup_next=cache->record->cleanup_entries;
            cache->record->cleanup_entries=entry;
            failed=1;
        }
    }
    return !failed;
}

static int drain_entries(struct kernel_page_cache *cache,
                         uint64_t *pages_released)
{
    if (remote_cache_client()) {
        CACHE_SCOPE(guard,cache);
        if (!cache->record->cleanup_entries) return 1;
        /* The shared retired list may own the final reference to a different
         * inode. Its backend close and registry removal remain CPU0 work. */
        cache->record->requested=1;
        cache_wake(&cache->record->work);
        return 0;
    }
    struct kernel_page_cache_entry *entries;
    take_cleanup_entries(cache,&entries);
    return drain_cleanup_entries(cache,&entries,SIZE_MAX,pages_released);
}

#if BOAROS_PAGE_CACHE_DIRTY_AGE
static uint64_t dirty_deadline(void)
{
    const uint64_t age=UINT64_C(5000000000),now=kernel_time_monotonic_ns();
    return now>UINT64_MAX-age?UINT64_MAX:now+age;
}
#endif
/* The inode queue borrows its dirty entries. One offset tree replaces the
 * dirty chain, so foreground and background select the same ordered windows. */
#if BOAROS_PAGE_CACHE_DIRTY_BLOCKS
static size_t dirty_block_size(const struct kernel_page_cache_entry *entry)
{
    size_t block=entry->node->instance->writeback_block_size;
    return block && block<BOAROS_PAGE_SIZE && !(block&(block-1)) &&
        BOAROS_PAGE_SIZE/block<=64 ? block : 0;
}
static uint64_t dirty_block_range(size_t block,size_t begin,size_t end)
{
    if (begin>=end) return 0;
    unsigned first=begin/block,last=(end+block-1)/block;
    uint64_t mask=last==64?UINT64_MAX:(UINT64_C(1)<<last)-1;
    return mask&~((UINT64_C(1)<<first)-1);
}
#endif
static void dirty_mark(struct kernel_page_cache_entry *entry, size_t begin, size_t end)
{
    if (begin>=end || end>BOAROS_PAGE_SIZE) __builtin_trap();
    struct kernel_page_cache_record *r=entry->cache->record;
    if (!entry->dirty_end) {
#if BOAROS_PAGE_CACHE_DIRTY_AGE
        if (!r->dirty_pages) {
            r->dirty_deadline=dirty_deadline();
            if (r->started && !r->stopping) cache_wake(&r->work);
        }
#endif
        kernel_task_account_write(BOAROS_PAGE_SIZE);
        struct kernel_writeback_domain *domain=entry->node->instance->writeback.domain;
        if (domain) domain_charge(&domain->dirty_bytes,BOAROS_PAGE_SIZE,1);
        if (entry->cache->record->group) __atomic_fetch_add(&entry->cache->record->group->dirty,1,__ATOMIC_RELAXED);
        struct kernel_page_cache_dirty *dirty=kernel_vfs_node_dirty_pages(entry->node);
        if (entry->dirty_indexed) __builtin_trap();
        if (!dirty->count) {
            if (!RB_EMPTY(&dirty->tree) || dirty->queue_next || dirty->queue_previous) __builtin_trap();
            dirty->queue_previous=r->dirty_tail;
            if (r->dirty_tail) r->dirty_tail->queue_next=dirty; else r->dirty_head=dirty;
            r->dirty_tail=dirty;
        }
        if (RB_INSERT(kernel_page_cache_dirty_tree,&dirty->tree,entry)) __builtin_trap();
        entry->dirty_indexed=1;dirty->count++;r->dirty_pages++;
        entry->dirty_begin=begin;
    } else if (begin<entry->dirty_begin) entry->dirty_begin=begin;
    if (end>entry->dirty_end) entry->dirty_end=end;
#if BOAROS_PAGE_CACHE_DIRTY_BLOCKS
    size_t block=dirty_block_size(entry);
    if (block) entry->dirty_blocks|=dirty_block_range(block,begin,end);
#endif
    entry->generation++;
    if (r->started && !r->stopping && r->force_writeback) {
        r->requested=1;cache_wake(&r->work);
    }
}
static void dirty_clear(struct kernel_page_cache_entry *entry)
{
    if (!entry->dirty_indexed) {
        if (entry->dirty_end) __builtin_trap();
        return;
    }
    struct kernel_page_cache_record *r=entry->cache->record;
    struct kernel_page_cache_dirty *dirty=kernel_vfs_node_dirty_pages(entry->node);
    if (!dirty->count || !r->dirty_pages) __builtin_trap();
    if (RB_REMOVE(kernel_page_cache_dirty_tree,&dirty->tree,entry)!=entry) __builtin_trap();
    entry->dirty_indexed=0;dirty->count--;r->dirty_pages--;
#if BOAROS_PAGE_CACHE_DIRTY_AGE
    if (!r->dirty_pages) {
        r->dirty_deadline=0;
        if (r->started && !r->stopping) cache_wake(&r->work);
    }
#endif
    struct kernel_writeback_domain *domain=entry->node->instance->writeback.domain;
    if (domain) domain_charge(&domain->dirty_bytes,BOAROS_PAGE_SIZE,0);
    if (r->group) __atomic_fetch_sub(&r->group->dirty,1,__ATOMIC_RELAXED);
    if (!dirty->count) {
        if (!RB_EMPTY(&dirty->tree)) __builtin_trap();
        if (r->dirty_cursor==dirty) {r->dirty_cursor=dirty->queue_next;r->dirty_scan_index=0;}
        if (dirty->queue_previous) dirty->queue_previous->queue_next=dirty->queue_next;
        else r->dirty_head=dirty->queue_next;
        if (dirty->queue_next) dirty->queue_next->queue_previous=dirty->queue_previous;
        else r->dirty_tail=dirty->queue_previous;
        dirty->queue_next=dirty->queue_previous=NULL;
    }
    entry->dirty_begin=entry->dirty_end=0;
#if BOAROS_PAGE_CACHE_DIRTY_BLOCKS
    entry->dirty_blocks=0;
#endif
}
static void dirty_round_begin(struct kernel_page_cache_record *r)
{
    if (r->dirty_cursor && r->dirty_remaining) return;
    r->dirty_cursor=r->dirty_head;r->dirty_remaining=r->dirty_pages;r->dirty_scan_index=0;
}
static int node_pressure(const struct kernel_vfs_node *node,struct kernel_page_cache_record *record,int low)
{
    const struct kernel_writeback_domain *domain=node->instance->writeback.domain;
    if (domain) return domain_bytes(domain)>=(domain_limit(domain,low));
    return __atomic_load_n(&record->group->dirty,__ATOMIC_RELAXED)>=(low?record->dirty_low:record->dirty_high);
}
static size_t dirty_batch(struct kernel_page_cache_record *r,struct kernel_page_cache_entry **batch,int all)
{
    size_t count=0;unsigned scanned=0;
    size_t capacity=r->writeback_capacity<BOAROS_PAGE_CACHE_WRITEBACK_PAGES?r->writeback_capacity:BOAROS_PAGE_CACHE_WRITEBACK_PAGES;
    while (r->dirty_cursor && r->dirty_remaining && scanned<64 && count<capacity) {
        struct kernel_page_cache_dirty *dirty=r->dirty_cursor;
        if (!dirty->count || RB_EMPTY(&dirty->tree)) __builtin_trap();
        struct kernel_page_cache_entry key={.page_index=r->dirty_scan_index};
        struct kernel_page_cache_entry *entry=RB_NFIND(kernel_page_cache_dirty_tree,&dirty->tree,&key);
        if (!entry) {r->dirty_cursor=dirty->queue_next;r->dirty_scan_index=0;continue;}
        if (!all && !node_pressure(entry->node,r,1)) {
            uint64_t skip=dirty->count<r->dirty_remaining?dirty->count:r->dirty_remaining;
            r->dirty_remaining-=skip;r->dirty_cursor=dirty->queue_next;r->dirty_scan_index=0;scanned++;
            continue;
        }
        if (count && entry->node!=batch[0]->node) break;
        while (entry && r->dirty_remaining && scanned<64 && count<capacity) {
            r->dirty_scan_index=entry->page_index+1;
            r->dirty_remaining--;scanned++;r->statistics.worker_scanned++;
            if (!entry_loading(entry) && !entry->writeback) {
                batch[count++]=entry;entry_borrow(entry);
            }
            entry=RB_NEXT(kernel_page_cache_dirty_tree,&dirty->tree,entry);
        }
        if (!entry) {r->dirty_cursor=dirty->queue_next;r->dirty_scan_index=0;}
    }
    return count;
}

static void remove_entry(struct kernel_page_cache *cache,
                         struct kernel_page_cache_entry *entry)
{

    if (entry->aliases != 0 || entry_loading(entry)) __builtin_trap();
    if (entry->retired) __builtin_trap();
    entry->retired=1;
    struct kernel_page_cache_record *record = cache->record;
    int found;
    size_t bucket = find_bucket(record,
                                entry->node,
                                entry->page_index,
                                &found);

    if (found) {
        record->buckets[bucket] = PAGE_CACHE_TOMBSTONE;
        record->occupied--;
        record->tombstones++;
    }
    lru_remove(record, entry);
    *entry->node_previous = entry->node_next;
    if (entry->node_next != 0)
        entry->node_next->node_previous = entry->node_previous;
    entry->node_previous = 0;
    entry->node_next = 0;
    entry->cleanup_next = record->cleanup_entries;
    record->cleanup_entries = entry;
    dirty_clear(entry);
    record->statistics.current_pages--;
    record->statistics.evictions++;
}

static uint32_t *reclaim_depth(void)
{ return &kernel_io_context_current()->reclaim_depth; }

static void cache_memory_snapshot(void *context, struct kernel_memory_statistics *out)
{
    struct kernel_page_cache *cache = context;
    if (!cache_live(cache)) return;
    KERNEL_LOCK_SCOPE(guard);
    if (kernel_lock_held(&cache->record->metadata.lock,1) ||
        !kernel_mutex_try_lock(&cache->record->metadata,&guard)) return;
    uint64_t reclaimable = 0;
    {
        struct version_pool *pool=cache->record->versions;
        KERNEL_RAW_SCOPE(guard,&pool->lock);
        if (pool->idle_count>pool->reserve) reclaimable+=(pool->idle_count-pool->reserve)*BOAROS_PAGE_SIZE;
    }
    for (struct kernel_page_cache_entry *e = cache->record->lru_head; e; e = e->lru_next) {
        out->cached += BOAROS_PAGE_SIZE;
        if (e->dirty_end) out->dirty += BOAROS_PAGE_SIZE;
        if (e->writeback) out->writeback += BOAROS_PAGE_SIZE;
        uint32_t refs;
        if (physical_page_reference_count(cache->allocator, e->physical_address, &refs)
                != PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
        if (refs == 1 && !e->aliases && !e->users && !entry_loading(e) && !e->writeback &&
            !e->writeback_error && (!e->dirty_end || kernel_vfs_node_writeback_allowed(e->node)))
            reclaimable += BOAROS_PAGE_SIZE;
    }
    out->reclaimable += reclaimable;
}

static void group_snapshot(void *context, struct kernel_memory_statistics *out)
{
    struct page_cache_group *group = context;
    KERNEL_LOCK_SCOPE(walk);
    struct kernel_lock_guard *held=kernel_io_context_current()->locks;
    if ((held && held->lock->rank>=group->walk.lock.rank) ||
        !kernel_mutex_try_lock(&group->walk,&walk)) return;
    for (struct kernel_page_cache_record *r = group->head; r; r = r->next)
        cache_memory_snapshot(r->owner, out);
    uint64_t reclaimable = out->reclaimable;
    uint64_t low = physical_page_total(group->allocator) / 50;
    if (!low) low = 1;
    low *= BOAROS_PAGE_SIZE;
    uint64_t keep = reclaimable / 2 < low ? reclaimable / 2 : low;
    uint64_t available = out->free > low ? out->free - low : 0;
    available += reclaimable - keep;
    out->available = available < out->total ? available : out->total;
}



static uint64_t group_reclaim(void *context, uint64_t target)
{
    struct page_cache_group *g=context;
    struct kernel_lock_guard *held=kernel_io_context_current()->locks;
    KERNEL_LOCK_SCOPE(walk);
    if (held && held->lock->rank>=g->walk.lock.rank) return 0;
    kernel_mutex_lock(&g->walk,&walk);
    struct kernel_page_cache_record *first=g->cursor?g->cursor:g->head,*r=first;
    uint64_t released=0;
    if (!r) return 0;
    do {
        released+=kernel_page_cache_reclaim(r->owner,target-released);
        r=r->next?r->next:g->head;
    } while (released<target && r!=first);
    g->cursor=r;
    return released;
}

static void group_put(struct page_cache_group *g)
{
    uint64_t previous=__atomic_fetch_sub(&g->refs,1,__ATOMIC_ACQ_REL);
    if (!previous) __builtin_trap();
    if (previous==1) kernel_heap_release(g->heap,g);
}
static void group_retain(void *context)
{
    struct page_cache_group *g=context;
    uint64_t previous=__atomic_fetch_add(&g->refs,1,__ATOMIC_RELAXED);
    if (!previous || previous==UINT64_MAX) __builtin_trap();
}
static void group_release(void *context) { group_put(context); }

static int group_retire_locked(struct page_cache_group *group)
{
    if (group->head || group->domains || group->closing) return 0;
    group->closing=1;return 1;
}
void kernel_writeback_domain_get(void *context)
{
    struct kernel_writeback_domain *domain=context;
    uint64_t old=__atomic_fetch_add(&domain->references,1,__ATOMIC_RELAXED);
    if (!old || old==UINT64_MAX) __builtin_trap();
}
void kernel_writeback_domain_put(void *context)
{
    struct kernel_writeback_domain *domain=context;
    uint64_t references=__atomic_load_n(&domain->references,__ATOMIC_ACQUIRE);
    while (references>1) {
        if (__atomic_compare_exchange_n(&domain->references,&references,references-1,1,
            __ATOMIC_RELEASE,__ATOMIC_RELAXED)) return;
    }
    if (!references) __builtin_trap();
    struct page_cache_group *group=domain->group;int destroy=0,detach=0;
    {
        /* Registry lookup can add an owner. Only the final put needs to
         * serialize with lookup; ordinary version loans stay local. */
        KERNEL_RAW_SCOPE(guard,&group->metadata);
        uint64_t old=__atomic_fetch_sub(&domain->references,1,__ATOMIC_ACQ_REL);
        if (!old) __builtin_trap();
        if (old==1) {
            if (domain->members || domain_bytes(domain)) __builtin_trap();
            struct kernel_writeback_domain **link=&group->domains;
            while (*link!=domain) {if (!*link) __builtin_trap();link=&(*link)->next;}
            *link=domain->next;
            if (!__atomic_fetch_sub(&group->domain_count,1,__ATOMIC_RELAXED)) __builtin_trap();
            domain_shares_locked(group);detach=group_retire_locked(group);destroy=1;
        }
    }
    if (destroy) {
        if (detach && physical_page_allocator_clear_reclaimer(group->allocator)) __builtin_trap();
        if (domain->device) kernel_block_put(domain->device);
        kernel_heap_release(group->heap,domain);group_put(group);
    }
}
int kernel_page_cache_bind_writeback(struct kernel_page_cache *cache,struct kernel_block_device *block,
    int writable,struct kernel_writeback_owner *owner)
{
    if (!cache_live(cache) || !block || !owner || owner->cache) __builtin_trap();
    struct page_cache_group *group=cache->record->group;
    if (!writable) {
        KERNEL_RAW_SCOPE(guard,&group->metadata);
        if (cache->record->mounts==UINT32_MAX) __builtin_trap();
        cache->record->mounts++;*owner=(struct kernel_writeback_owner){.cache=cache};return 0;
    }
    struct kernel_block_device *device=kernel_block_pressure_device(block);
    struct kernel_writeback_domain *candidate=NULL,*domain;
    enum kernel_heap_status status=kernel_heap_allocate_zeroed(cache->heap,1,sizeof(*candidate),(void **)&candidate);
    if (status) {
        if (device) kernel_block_put(device);
        if (status!=KERNEL_HEAP_STATUS_EMPTY) __builtin_trap();
        return -KERNEL_ENOMEM;
    }
    {
        KERNEL_RAW_SCOPE(guard,&group->metadata);
        for (domain=group->domains;domain;domain=domain->next) if (domain->device==device) break;
        if (!domain) {
            domain=candidate;candidate=NULL;domain->group=group;domain->device=device;device=NULL;
            domain->next=group->domains;group->domains=domain;group_retain(group);
            __atomic_fetch_add(&group->domain_count,1,__ATOMIC_RELAXED);
            domain->references=1;domain_shares_locked(group);
        } else kernel_writeback_domain_get(domain);
        *owner=(struct kernel_writeback_owner){.domain=domain,.cache=cache,.next=domain->members};
        domain->members=owner;
        if (cache->record->mounts==UINT32_MAX) __builtin_trap();
        cache->record->mounts++;
    }
    if (candidate) kernel_heap_release(cache->heap,candidate);
    if (device) kernel_block_put(device);
    group_notify(group);return 0;
}
void kernel_page_cache_unbind_writeback(struct kernel_writeback_owner *owner)
{
    if (!owner->cache) return;
    struct kernel_writeback_domain *domain=owner->domain;
    struct kernel_page_cache_record *record=owner->cache->record;
    {
        KERNEL_RAW_SCOPE(guard,&record->group->metadata);
        if (!record->mounts) __builtin_trap();
        record->mounts--;
        if (domain) {
            struct kernel_writeback_owner **link=&domain->members;
            while (*link!=owner) {if (!*link) __builtin_trap();link=&(*link)->next;}
            *link=owner->next;
        }
        *owner=(struct kernel_writeback_owner){0};
    }
    if (domain) kernel_writeback_domain_put(domain);
}
void kernel_writeback_domain_snapshot(const struct kernel_writeback_owner *owner,struct kernel_writeback_statistics *out)
{
    const struct kernel_writeback_domain *domain=owner->domain;
    if (!domain || !out) __builtin_trap();
    *out=(struct kernel_writeback_statistics){.physical=domain->device!=NULL,
        .device=domain->device?domain->device->device_number:0,
        .dirty_bytes=__atomic_load_n(&domain->dirty_bytes,__ATOMIC_RELAXED),
        .version_bytes=__atomic_load_n(&domain->version_bytes,__ATOMIC_RELAXED),
        .metadata_bytes=__atomic_load_n(&domain->metadata_bytes,__ATOMIC_RELAXED),
        .high_bytes=domain_limit(domain,0),.low_bytes=domain_limit(domain,1)};
}

static uint64_t group_dirty(struct page_cache_group *g)
{ return __atomic_load_n(&g->dirty,__ATOMIC_RELAXED); }



static void group_complete(struct kernel_page_cache_record *r,int progress)
{
    struct page_cache_group *g=r->group;
    if (!g) return;
    {
        KERNEL_RAW_SCOPE(guard,&g->metadata);
        if (progress) g->progressed++;
        if (r->wait_round && r->wait_round==g->round) {
            if (!g->pending) __builtin_trap();
            g->pending--;
        }
        r->wait_round=0;
    }
    cache_wake(&g->progress);
}

static void group_unregister(struct kernel_page_cache *cache)
{
    struct kernel_page_cache_record *r=cache->record;
    struct page_cache_group *g=r->group;
    int detach;
    {
        KERNEL_LOCK_SCOPE(walk);kernel_mutex_lock(&g->walk,&walk);
        {
            KERNEL_RAW_SCOPE(guard,&g->metadata);
            struct kernel_page_cache_record **link=&g->head;
            while (*link!=r) link=&(*link)->next;
            *link=r->next;
            if (g->cursor==r) g->cursor=r->next?r->next:g->head;
            detach=group_retire_locked(g);
        }
        if (detach && physical_page_allocator_clear_reclaimer(cache->allocator)) __builtin_trap();
        r->group=NULL;
    }
    group_put(g);
}

static enum kernel_page_cache_status cache_init(
    struct kernel_page_cache *cache,
    struct kernel_heap *heap,
    struct physical_page_allocator *allocator)
{
    struct kernel_page_cache_record *record;
    enum kernel_heap_status heap_status;

    if (cache == 0 || heap == 0 || allocator == 0 ||
        cache->state != KERNEL_PAGE_CACHE_EMPTY || cache->heap != 0 ||
        cache->allocator != 0 || cache->record != 0 ||
        !physical_page_allocator_is_finalized(allocator)) {
        return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    }
    if ((!heap->initialized || heap->page_allocator != allocator))
        return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    heap_status = kernel_heap_allocate_zeroed(heap,
                                              1U,
                                              sizeof(*record),
                                              (void **)&record);
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        return heap_status == KERNEL_HEAP_STATUS_EMPTY
                   ? KERNEL_PAGE_CACHE_STATUS_NO_MEMORY
                   : KERNEL_PAGE_CACHE_STATUS_STATE;
    }
    heap_status = kernel_heap_allocate_zeroed(
        heap,
        PAGE_CACHE_INITIAL_CAPACITY,
        sizeof(*record->buckets),
        (void **)&record->buckets);
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        kernel_heap_release(heap, record);
        return heap_status == KERNEL_HEAP_STATUS_EMPTY
                   ? KERNEL_PAGE_CACHE_STATUS_NO_MEMORY
                   : KERNEL_PAGE_CACHE_STATUS_STATE;
    }
    record->versions=version_pool_new(heap,allocator);
    if (!record->versions) {
        kernel_heap_release(heap,record->buckets);
        kernel_heap_release(heap,record);
        return KERNEL_PAGE_CACHE_STATUS_NO_MEMORY;
    }
    record->capacity = PAGE_CACHE_INITIAL_CAPACITY;
    kernel_wait_queue_init(&record->work);
    kernel_wait_queue_init(&record->progress);
    record->reads_tail=&record->reads_head;
    kernel_mutex_init(&record->metadata, 45, (uintptr_t)record);
    cache->heap = heap;
    cache->allocator = allocator;
    cache->record = record;
    cache->state = KERNEL_PAGE_CACHE_LIVE;
    for (;;) {
        struct page_cache_group *group;
        int other;
        {
            KERNEL_RAW_SCOPE(guard,&allocator->lock);
            other=allocator->reclaim_hooks.reclaim && allocator->reclaim_hooks.reclaim!=group_reclaim;
            group=allocator->reclaim_hooks.reclaim==group_reclaim?allocator->reclaim_hooks.context:NULL;
            if (group) group_retain(group);
        }
        if (other) goto failed;
        if (!group) {
            heap_status=kernel_heap_allocate_zeroed(heap,1,sizeof(*group),(void **)&group);
            if (heap_status) goto failed;
            group->allocator=allocator;group->heap=heap;group->refs=1;
            kernel_raw_lock_init(&group->metadata,KERNEL_RAW_RANK_OBJECT);
            /* 聚合遍历只回收clean页，退休node由worker处理；允许后端
             * rank40内的分配先取walk，再取cache45，不递归存储I/O。 */
            kernel_mutex_init(&group->walk,41,(uintptr_t)group);
            kernel_wait_queue_init(&group->progress);
            struct physical_page_reclaim_hooks hooks={.reclaim=group_reclaim,.notify=group_notify,
                .wait=pressure_wait,.snapshot=group_snapshot,.depth=reclaim_depth,
                .retain=group_retain,.release=group_release,.context=group};
            if (physical_page_allocator_set_reclaimer(allocator,&hooks)) { group_put(group);continue; }
        }
        int installed;
        {
            KERNEL_LOCK_SCOPE(walk);kernel_mutex_lock(&group->walk,&walk);
            { KERNEL_RAW_SCOPE(binding,&allocator->lock);
              installed=allocator->reclaim_hooks.context==group; }
            if (installed) {
                KERNEL_RAW_SCOPE(guard,&group->metadata);
                if (group->closing) installed=0;
                else {
                    record->group=group;record->owner=cache;
                    record->next=group->head;group->head=record;
                }
            }
        }
        if (installed) break;
        group_put(group);
    }
    return KERNEL_PAGE_CACHE_STATUS_OK;
failed:
    version_pool_close(record->versions);
    kernel_heap_release(heap, record->buckets);
    kernel_heap_release(heap, record);
    *cache = (struct kernel_page_cache){0};
    return heap_status == KERNEL_HEAP_STATUS_EMPTY ? KERNEL_PAGE_CACHE_STATUS_NO_MEMORY
                                                  : KERNEL_PAGE_CACHE_STATUS_STATE;
}

enum kernel_page_cache_status kernel_page_cache_init(
    struct kernel_page_cache *cache, struct kernel_heap *heap,
    struct physical_page_allocator *allocator)
{ return cache_init(cache, heap, allocator); }



int kernel_page_cache_check_domain(const struct kernel_page_cache *cache,
    const struct kernel_heap *heap, const struct physical_page_allocator *allocator)
{
    return cache_live(cache) && cache->record->started && !cache->record->stopping &&
        cache->heap == heap && cache->allocator == allocator;
}

void kernel_page_cache_get_concurrency_state(struct kernel_page_cache *cache,
    struct kernel_page_cache_concurrency_state *out)
{
    if (!cache_live(cache) || !out) __builtin_trap();
    CACHE_SCOPE(guard, cache);
    struct kernel_page_cache_record *r = cache->record;
    *out = (struct kernel_page_cache_concurrency_state){
        .requests_queued = r->requests_queued, .requests_completed = r->requests_completed,
        .entry_borrows = r->entry_borrows, .pending = r->read_requests - r->reads_running,
        .deferred_snapshots = r->deferred_snapshots,
        .writeback_rounds = r->completed,
        .running = r->reads_running, .entry_bytes = sizeof(struct kernel_page_cache_entry),
        .record_bytes = sizeof(*r), .request_bytes = sizeof(struct cache_request)+sizeof(struct read_batch_load),
        .execution_request_bytes=sizeof(struct cache_request),
        .index_capacity=r->capacity,.index_live=r->occupied,.index_tombstones=r->tombstones,
        .executions_queued=r->executions_queued,.executions_completed=r->executions_completed,
        .queue_ns=r->queue_ns,.execute_ns=r->execute_ns,
        .max_queue_ns=r->max_queue_ns,.max_execute_ns=r->max_execute_ns};
}

static enum kernel_page_cache_status lookup_entry(
    struct kernel_page_cache *cache,
    const struct kernel_vfs_file *file,
    uint64_t page_index,
    struct kernel_page_cache_entry **entry_out)
{
    struct kernel_vfs_node *node;
    int found;
    size_t bucket;

    if (!cache_live(cache) || file == 0 || entry_out == 0) {
        return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    }
    node = kernel_vfs_file_node(file);
    if (node == 0) {
        return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    }
    bucket = find_bucket(cache->record, node, page_index, &found);
    if (!found) {
        return KERNEL_PAGE_CACHE_STATUS_NOT_FOUND;
    }
    *entry_out = cache->record->buckets[bucket];
    return KERNEL_PAGE_CACHE_STATUS_OK;
}

static void link_alias(struct kernel_page_cache_entry *entry,
                       struct kernel_page_cache_alias *alias)
{
    alias->entry = entry;
    alias->next = entry->aliases;
    alias->previous = &entry->aliases;
    if (alias->next != 0) alias->next->previous = &alias->next;
    entry->aliases = alias;
    __atomic_store_n(&alias->registered,1,__ATOMIC_RELEASE);
}

enum kernel_page_cache_status kernel_page_cache_alias_attach(
    struct kernel_page_cache *cache, const struct kernel_vfs_file *file,
    uint64_t page_index, uint64_t physical_address,
    struct kernel_page_cache_alias *alias, void *owner,
    uint64_t virtual_address,
    int (*rearm)(void *owner, uint64_t virtual_address))
{
    struct kernel_page_cache_entry *entry;
    enum kernel_page_cache_status status;
    if (alias == 0 || alias->entry != 0 || owner == 0 || rearm == 0)
        return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    CACHE_SCOPE(guard, cache);
    status = lookup_entry(cache, file, page_index, &entry);
    if (status != KERNEL_PAGE_CACHE_STATUS_OK) return status;
    if (entry->physical_address != physical_address) __builtin_trap();
    alias->owner = owner;
    alias->virtual_address = virtual_address;
    alias->rearm = rearm;
    link_alias(entry, alias);
    return KERNEL_PAGE_CACHE_STATUS_OK;
}

void kernel_page_cache_alias_clone(struct kernel_page_cache_alias *target,
    const struct kernel_page_cache_alias *source, void *owner)
{
    if (target == 0 || source == 0 || !kernel_page_cache_alias_active(source) ||
        kernel_page_cache_alias_active(target) || owner == 0) __builtin_trap();
    CACHE_SCOPE(guard, source->entry->cache);
    target->owner = owner;
    target->virtual_address = source->virtual_address;
    target->rearm = source->rearm;
    link_alias(source->entry, target);
}

static void alias_borrow(struct kernel_page_cache_alias *alias)
{
    if (!alias) return;
    if (!alias->entry || alias->borrows == UINT32_MAX) __builtin_trap();
    alias->borrows++; entry_borrow(alias->entry);
}
static void alias_return(struct kernel_page_cache_alias *alias)
{
    while (alias) {
        struct kernel_page_cache *cache = alias->entry->cache;
        struct kernel_page_cache_alias *next = 0;
        {
            CACHE_SCOPE(guard, cache);
            if (!alias->borrows) __builtin_trap();
            alias->borrows--; entry_return(alias->entry);
            if (!alias->borrows && !alias->previous) {
                next = alias->retired_next; alias->retired_next = 0;
            }
            cache_wake(&alias->entry->ready);
        }
        alias = next;
    }
}
void kernel_page_cache_alias_hold(struct kernel_page_cache_alias *alias)
{
    if (!alias || !alias->entry) __builtin_trap();
    CACHE_SCOPE(guard,alias->entry->cache); alias_borrow(alias);
}
void kernel_page_cache_alias_put(struct kernel_page_cache_alias *alias)
{ if (!alias || !alias->entry) __builtin_trap(); alias_return(alias); }
void kernel_page_cache_retry_alias(struct kernel_page_cache_alias *alias)
{
    struct kernel_page_cache_entry *entry=alias->entry;
    struct kernel_page_cache *cache=entry->cache;
    CACHE_SCOPE(guard,cache);
    entry->retry_blocked=0;
    if (cache->record->started && !cache->record->stopping) {
        cache->record->requested=1; cache_wake(&cache->record->work);
    }
}
void kernel_page_cache_alias_detach(struct kernel_page_cache_alias *alias)
{
    if (!alias || !alias->entry || !alias->previous) __builtin_trap();
    struct kernel_page_cache *cache = alias->entry->cache;
    CACHE_SCOPE(guard, cache);
    if (*alias->previous != alias) __builtin_trap();
    *alias->previous = alias->next;
    if (alias->next) alias->next->previous = alias->previous;
    alias->previous = 0;
    __atomic_store_n(&alias->registered,0,__ATOMIC_RELEASE);
    {
        /* stop->drain的真实entry存储owner；回调末次put不能抢先释放它。 */
        entry_borrow(alias->entry);
        if (alias->borrows) { alias->retired_next = alias->next; alias_borrow(alias->retired_next); }
        return;
    }
}
void kernel_page_cache_alias_drain(struct kernel_page_cache_alias *alias)
{
    if (!alias) __builtin_trap();
    if (!alias->entry) return;
    struct kernel_page_cache *cache = alias->entry->cache;
    if (kernel_cpu_current()->raw_locks || kernel_lock_held(&alias->entry->node->io_lock,0)) __builtin_trap();
    for (;;) {
        struct kernel_wait_token token = {0};
        {
            CACHE_SCOPE(guard, cache);
            if (alias->previous) __builtin_trap();
            if (!alias->borrows) {
                entry_return(alias->entry);
                *alias = (struct kernel_page_cache_alias){0};
                break;
            }
            if (kernel_wait_prepare(&alias->entry->ready,0,0,&token)) __builtin_trap();
        }
        enum kernel_wait_wake_reason reason;
        if (kernel_wait_park(&token,&reason) || kernel_wait_finish(&token)) __builtin_trap();
    }
}
/* 调用者在关闭写入的同一保护内取得首个alias借用。 */
static int rearm_aliases(struct kernel_page_cache_entry *entry,struct kernel_page_cache_alias *alias)
{
    struct kernel_page_cache *cache = entry->cache;
    while (alias) {
        int active;
        { CACHE_SCOPE(guard,cache); active = alias->previous != 0; }
        int result=active ? alias->rearm(alias->owner,alias->virtual_address) : 0;
        if (result) {
            if (result!=-KERNEL_EBUSY) __builtin_trap();
            alias_return(alias); return result;
        }
        struct kernel_page_cache_alias *next;
        {
            CACHE_SCOPE(guard,cache);
            next = alias->previous ? alias->next : alias->retired_next;
            alias_borrow(next);
        }
        alias_return(alias); alias = next;
    }
    return 0;
}
void kernel_page_cache_alias_mark_dirty(struct kernel_page_cache_alias *alias)
{
    if (!alias || !alias->entry || !alias->previous) __builtin_trap();
    struct kernel_page_cache_entry *entry = alias->entry;
    struct kernel_page_cache *cache = entry->cache;
    {
        CACHE_SCOPE(guard,cache);
        size_t end = entry_valid_bytes(entry);
        if (!end) __builtin_trap();
        dirty_mark(entry,0,end);
    }
    kernel_page_cache_notify_pressure(cache);
}
#define CACHE_WRITES_CLOSED (UINT64_C(1)<<63)
#define CACHE_WRITES_MASK (CACHE_WRITES_CLOSED | UINT32_MAX)
static int writer_begin(struct kernel_page_cache_entry *entry,
    struct kernel_page_cache_write_borrow *borrow)
{
    if (!borrow || borrow->registration) __builtin_trap();
    uint64_t state = __atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE);
    for (;;) {
        if (state & ~CACHE_WRITES_MASK || (uint32_t)state == UINT32_MAX) __builtin_trap();
        if (state & CACHE_WRITES_CLOSED) return 0;
        if (__atomic_compare_exchange_n(&entry->writes,&state,state+1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) break;
    }
    struct kernel_io_context *owner = kernel_io_context_current();
    *borrow = (struct kernel_page_cache_write_borrow){.entry=entry,.owner=owner,
        .registration=borrow,.next=owner->cache_writes};
    owner->cache_writes = borrow;
    return 1;
}
enum kernel_page_cache_status kernel_page_cache_alias_write_begin(
    struct kernel_page_cache_alias *alias, struct kernel_page_cache_write_borrow *borrow)
{
    if (!alias || !kernel_page_cache_alias_active(alias) || !alias->entry || !borrow) return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    return writer_begin(alias->entry,borrow) ? KERNEL_PAGE_CACHE_STATUS_OK : KERNEL_PAGE_CACHE_STATUS_BUSY;
}
static void writer_end_locked(struct kernel_page_cache_write_borrow *borrow)
{
    if (!borrow || borrow->registration != borrow || borrow->owner != kernel_io_context_current()) __builtin_trap();
    struct kernel_page_cache_entry *entry = borrow->entry;
    struct kernel_io_context *owner = borrow->owner;
    struct kernel_page_cache_write_borrow **link = &owner->cache_writes;
    while (*link && *link != borrow) link = &(*link)->next;
    if (*link != borrow) __builtin_trap();
    uint64_t state = __atomic_fetch_sub(&entry->writes,1,__ATOMIC_RELEASE);
    if (state & ~CACHE_WRITES_MASK || !(uint32_t)state) __builtin_trap();
    *link = borrow->next; *borrow = (struct kernel_page_cache_write_borrow){0};
    if ((state & CACHE_WRITES_CLOSED) && (uint32_t)state == 1) cache_wake(&entry->ready);
    if ((uint32_t)state==1 && entry->retry_blocked) {
        entry->retry_blocked=0;
        if (entry->cache->record->started && !entry->cache->record->stopping) {
            entry->cache->record->requested=1; cache_wake(&entry->cache->record->work);
        }
    }
    if ((uint32_t)state==1 && entry->retired && entry->cache->record->started) {
        entry->cache->record->requested=1; cache_wake(&entry->cache->record->work);
    }
}
void kernel_page_cache_write_end(struct kernel_page_cache_write_borrow *borrow)
{
    if (!borrow || !borrow->entry) __builtin_trap();
    CACHE_SCOPE(guard,borrow->entry->cache);
    writer_end_locked(borrow);
}
enum kernel_page_cache_status kernel_page_cache_wait_writable(
    struct kernel_page_cache *cache,const struct kernel_vfs_file *file,uint64_t index,uint64_t physical)
{
    struct kernel_page_cache_entry *entry;
    { CACHE_SCOPE(guard,cache);
      enum kernel_page_cache_status status=lookup_entry(cache,file,index,&entry);
      if (status) return status;
      if (entry->physical_address != physical) return KERNEL_PAGE_CACHE_STATUS_NOT_FOUND;
      entry_borrow(entry); }
    for (;;) {
        struct kernel_wait_token token={0};
        {
            CACHE_SCOPE(guard,cache);
            if (entry->retired || !(__atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)&CACHE_WRITES_CLOSED)) break;
            if (kernel_wait_prepare(&entry->ready,0,0,&token)) __builtin_trap();
            if (!(__atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)&CACHE_WRITES_CLOSED)) {
                if (kernel_wait_finish(&token)) __builtin_trap();
                break;
            }
        }
        enum kernel_wait_wake_reason reason;
        if (kernel_wait_park(&token,&reason) || kernel_wait_finish(&token)) __builtin_trap();
    }
    { CACHE_SCOPE(guard,cache); entry_return(entry); }
    return KERNEL_PAGE_CACHE_STATUS_OK;
}

static size_t entry_valid_bytes(const struct kernel_page_cache_entry *entry)
{
    uint64_t size = kernel_vfs_node_size(entry->node);
    uint64_t start = entry->page_index << BOAROS_PAGE_SHIFT;
    if (start >= size) return 0;
    return size - start < BOAROS_PAGE_SIZE ? (size_t)(size - start)
                                          : BOAROS_PAGE_SIZE;
}

/* 在索引保护内把已完成页交给读取者；物理引用接替entry保护，不必再借用一次entry。 */
static inline void take_read_page(struct kernel_page_cache *cache,
    struct kernel_page_cache_entry *entry, uint64_t *address, size_t *valid, size_t bytes)
{
    if (physical_page_acquire(cache->allocator, entry->physical_address)) __builtin_trap();
    *address = entry->physical_address;
    *valid = bytes;
    if (!entry->retired) lru_touch(cache->record, entry);
    cache->record->statistics.hits++;
}

static enum kernel_page_cache_status finish_read_page(
    struct kernel_page_cache *cache, struct kernel_page_cache_entry *entry,
    uint64_t *address, size_t *valid)
{
    enum kernel_page_cache_status status;
    int clean = 0;
    for (;;) {
        struct kernel_wait_token token = {0};
        int park;
        {
            CACHE_SCOPE(guard, cache);
            status = entry_load_status(entry);
            if (status != KERNEL_PAGE_CACHE_STATUS_BUSY && !(__atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)&CACHE_WRITES_CLOSED)) {
                if (!status) take_read_page(cache, entry, address, valid, entry_valid_bytes(entry));
                entry_return(entry);
                if (status && !entry->users && !entry->retired) { remove_entry(cache, entry); clean = 1; }
                break;
            }
            if (kernel_wait_prepare(&entry->ready, 0, 0, &token)) __builtin_trap();
            park = entry_loading(entry) || (__atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)&CACHE_WRITES_CLOSED);
        }
        enum kernel_wait_wake_reason reason;
        if (park && kernel_wait_park(&token, &reason)) __builtin_trap();
        if (kernel_wait_finish(&token)) __builtin_trap();
    }
    if (clean) {
        uint64_t released = 0;
        (void)drain_entries(cache, &released);
    }
    return status;
}

/* 取页资格只覆盖取页；physical ref不冻结内容，MM仍须核对自己的来源epoch。
 * resize关闭在cache保护内建立；整inode截断另外由既有mapping关闭代次表达。 */
static int read_cache_open(struct kernel_vfs_node *node)
{
    uint64_t epoch;
    return !kernel_file_mapping_epoch(node,&epoch);
}
static int read_entry_ready(struct kernel_page_cache_entry *entry)
{
    return entry_load_status(entry) == KERNEL_PAGE_CACHE_STATUS_OK && !entry->retired &&
        (!(__atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)&CACHE_WRITES_CLOSED));
}

int kernel_page_cache_resident(struct kernel_page_cache *cache,
    const struct kernel_vfs_file *file, uint64_t index)
{
    CACHE_SCOPE(guard, cache);
    struct kernel_page_cache_entry *entry;
    return !lookup_entry(cache, file, index, &entry) && !entry->retired &&
        entry_load_status(entry) == KERNEL_PAGE_CACHE_STATUS_OK;
}

enum kernel_page_cache_status kernel_page_cache_lookup(
    struct kernel_page_cache *cache, const struct kernel_vfs_file *file,
    uint64_t index, uint64_t *address, size_t *valid)
{
    if (!cache_live(cache) || !address || !valid || !kernel_vfs_file_node(file))
        return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    CACHE_SCOPE(guard,cache);
    if (!read_cache_open(kernel_vfs_file_node(file))) return KERNEL_PAGE_CACHE_STATUS_BUSY;
    struct kernel_page_cache_entry *entry;
    enum kernel_page_cache_status status=lookup_entry(cache,file,index,&entry);
    if (status) return status;
    if (!read_entry_ready(entry)) return entry_load_status(entry) ? entry_load_status(entry) : KERNEL_PAGE_CACHE_STATUS_BUSY;
    take_read_page(cache,entry,address,valid,entry_valid_bytes(entry));
    return KERNEL_PAGE_CACHE_STATUS_OK;
}

/* 未发布的候选持有自己的node/page；分配完成后才在cache保护内竞争发布。 */
static enum kernel_page_cache_status allocate_entry(struct kernel_page_cache *cache,
    struct kernel_vfs_node *node,uint64_t index,int clear,
    struct kernel_page_cache_entry **output,void **bytes)
{
    struct kernel_page_cache_entry *entry;
    if (kernel_heap_allocate_zeroed(cache->heap,1,sizeof(*entry),(void **)&entry))
        return KERNEL_PAGE_CACHE_STATUS_NO_MEMORY;
    if (physical_page_allocate(cache->allocator,&entry->physical_address)) {
        kernel_heap_release(cache->heap,entry);
        return KERNEL_PAGE_CACHE_STATUS_NO_MEMORY;
    }
    entry->page_references_owned=1;
    void *page;
    if (physical_page_map_owned(cache->allocator,entry->physical_address,&page))
        return abandon_entry(cache,entry,KERNEL_PAGE_CACHE_STATUS_STATE);
    if (clear) memset(page,0,BOAROS_PAGE_SIZE);
    if (kernel_vfs_node_acquire(node)) return abandon_entry(cache,entry,KERNEL_PAGE_CACHE_STATUS_STATE);
    entry->cache=cache;entry->node=node;entry->page_index=index;
    *output=entry;*bytes=page;
    return KERNEL_PAGE_CACHE_STATUS_OK;
}
static void publish_entry(struct kernel_page_cache *cache,struct kernel_page_cache_entry *entry,size_t bucket)
{
    struct kernel_page_cache_record *r=cache->record;
    kernel_wait_queue_init(&entry->ready);__atomic_store_n(&entry->load_status,KERNEL_PAGE_CACHE_STATUS_BUSY,__ATOMIC_RELAXED);entry_borrow(entry);
    if (r->buckets[bucket]==PAGE_CACHE_TOMBSTONE) r->tombstones--;
    r->buckets[bucket]=entry;r->occupied++;
    entry->node_next=*kernel_vfs_node_cache_pages(entry->node);
    entry->node_previous=kernel_vfs_node_cache_pages(entry->node);
    if (entry->node_next) entry->node_next->node_previous=&entry->node_next;
    *entry->node_previous=entry;
    lru_insert_head(r,entry);r->statistics.insertions++;r->statistics.current_pages++;
    if (r->statistics.current_pages>r->statistics.peak_pages) r->statistics.peak_pages=r->statistics.current_pages;
}
struct read_pages_progress {
    struct kernel_page_cache_entry **entries;
    uint32_t completed;
};
static inline enum kernel_page_cache_status read_page_result(const struct file_read_span *span,
    size_t valid)
{
    int error = span->error;
    if (span->completed > span->size) __builtin_trap();
    if (!error && span->completed < valid) error = -KERNEL_EIO;
    if (!error && valid < BOAROS_PAGE_SIZE)
        memset((unsigned char *)span->buffer + valid, 0, BOAROS_PAGE_SIZE - valid);
    return error == -KERNEL_ENOMEM ? KERNEL_PAGE_CACHE_STATUS_NO_MEMORY :
        error ? KERNEL_PAGE_CACHE_STATUS_IO : KERNEL_PAGE_CACHE_STATUS_OK;
}
static void complete_read_page(void *context, unsigned index, const struct file_read_span *span)
{
    struct read_pages_progress *progress = context;
    struct kernel_page_cache_entry *entry = progress->entries[index];
    enum kernel_page_cache_status status = read_page_result(span, entry_valid_bytes(entry));
    __atomic_store_n(&entry->load_status, (uint32_t)status | CACHE_LOAD_EARLY, __ATOMIC_RELEASE);
    progress->completed |= UINT32_C(1) << index;
    cache_wake(&entry->ready);
}
/* 单页保持一次同步完成；批量按页发布，生产者借用留到整批DMA排空后归还。 */
static size_t load_read_pages(struct kernel_page_cache *cache, struct kernel_vfs_node *node,
    struct kernel_page_cache_entry **entries, struct file_read_span *spans, size_t count,
    struct kernel_page_cache_read_page **outputs, struct file_read_workspace *workspace)
{
    if (!count) return 0;
    struct read_pages_progress progress = {entries, 0};
    const struct file_read_completion completion = {complete_read_page, &progress};
    int result = kernel_vfs_node_pread_batch(node, spans, count, workspace,
        count > 1 ? &completion : NULL), identified = 0;
    for (size_t i = 0; i < count; i++) identified |= spans[i].error != 0;
    int clean = 0;
    size_t failed = 0;
    {
        CACHE_SCOPE(complete, cache);
        cache->record->statistics.misses += count;
        for (size_t i = 0; i < count; i++) {
            struct kernel_page_cache_entry *entry = entries[i];
            size_t valid = entry_valid_bytes(entry);
            enum kernel_page_cache_status status;
            /* 同一调用栈已结束全部回调和DMA，不再取得提前发布者的可见性。 */
            if (progress.completed & (UINT32_C(1) << i))
                status = (enum kernel_page_cache_status)(
                    __atomic_load_n(&entry->load_status, __ATOMIC_RELAXED) & ~CACHE_LOAD_EARLY);
            else {
                if (result && !identified) spans[i].error = result;
                status = read_page_result(&spans[i], valid);
            }
            __atomic_store_n(&entry->load_status, status, __ATOMIC_RELAXED);
            if (outputs) {
                outputs[i]->status = status;
                if (!status) {
                    if (physical_page_acquire(cache->allocator, entry->physical_address)) __builtin_trap();
                    outputs[i]->physical_address = entry->physical_address;
                    outputs[i]->valid_bytes = valid;
                }
            }
            failed += status != 0;
            entry_return(entry);
            if (!(progress.completed & (UINT32_C(1) << i))) cache_wake(&entry->ready);
            if (status && !entry->users && !entry->retired) {
                remove_entry(cache, entry); clean = 1;
            }
        }
    }
    if (clean) { uint64_t released = 0; (void)drain_entries(cache, &released); }
    return failed;
}

#if BOAROS_PAGE_CACHE_READAHEAD_PAGES
/* Queue slots own one node pin. Callers only mark cancellation; the CPU0
 * worker clears the slot under cache protection and releases its pin outside. */
static void readahead_cancel_node(struct kernel_page_cache *cache, struct kernel_vfs_node *node)
{
    CACHE_SCOPE(cancel, cache);
    struct kernel_page_cache_record *r = cache->record;
    for (unsigned i = 0; i < 8; i++) {
        struct readahead_job *job = &r->readahead_jobs[i];
        if (job->node && (!node || job->node == node)) job->cancelled = 1;
    }
    if (r->readahead_started) cache_wake(&r->readahead_work);
}

void kernel_page_cache_cancel_readahead(struct kernel_page_cache *cache, uint64_t cookie)
{
    if (!cache_live(cache) || !cookie) return;
    CACHE_SCOPE(cancel, cache);
    struct kernel_page_cache_record *r = cache->record;
    for (unsigned i = 0; i < 8; i++) {
        struct readahead_job *job = &r->readahead_jobs[i];
        if (job->node && job->cookie == cookie) job->cancelled = 1;
    }
    if (r->readahead_started) cache_wake(&r->readahead_work);
}

struct kernel_page_cache_prediction kernel_page_cache_readahead(struct kernel_page_cache *cache,
    struct kernel_vfs_node *node,uint64_t first,unsigned pages,uint64_t cookie)
{
    const struct kernel_page_cache_prediction none={0};
    if (!pages || pages>BOAROS_PAGE_CACHE_READAHEAD_PAGES) __builtin_trap();
    if (!cache_live(cache) || !node || first>UINT64_MAX>>BOAROS_PAGE_SHIFT) return none;
    CACHE_SCOPE(publish,cache);
    struct kernel_page_cache_record *r=cache->record;
    uint64_t size=kernel_vfs_node_size(node);
    if (!r->readahead_started || r->stopping || physical_page_available(cache->allocator)<=r->low ||
        (first<<BOAROS_PAGE_SHIFT)>=size) {
        for (unsigned i=0;i<8;i++)
            if (cookie && r->readahead_jobs[i].node && r->readahead_jobs[i].cookie==cookie)
                r->readahead_jobs[i].cancelled=1;
        if (r->readahead_started) cache_wake(&r->readahead_work);
        return none;
    }
    uint64_t remaining=((size-1)>>BOAROS_PAGE_SHIFT)+1-first;
    if (pages>remaining) pages=(unsigned)remaining;
    struct readahead_job *available=NULL;
    for (unsigned i=0;i<8;i++) {
        struct readahead_job *job=&r->readahead_jobs[i];
        if (!job->node) {if (!available) available=job;continue;}
        if (cookie && job->cookie==cookie && job->node==node && !job->cancelled) {
            if (!job->active) {job->first=first;job->pages=pages;}
            return (struct kernel_page_cache_prediction){cookie,job->first+job->pages};
        }
    }
    /* Hot scans acknowledge resident coverage without waking a worker that
     * would repeat the same lookups. Publication still rechecks cold entries. */
    unsigned resident=0;
    for (;resident<pages;resident++) {
        int found;size_t bucket=find_bucket(r,node,first+resident,&found);
        if (!found || entry_loading(r->buckets[bucket]) || r->buckets[bucket]->retired) break;
    }
    if (resident==pages) return (struct kernel_page_cache_prediction){0,first+pages};
    if (!available) return none;
    if (kernel_vfs_node_acquire(node)) __builtin_trap();
    if (!++r->readahead_cookie) __builtin_trap();
    *available=(struct readahead_job){.node=node,.first=first,.pages=pages,.cookie=r->readahead_cookie};
    r->statistics.readahead_queued++;cache_wake(&r->readahead_work);
    return (struct kernel_page_cache_prediction){available->cookie,first+pages};
}

static int readahead_cancelled(struct kernel_page_cache *cache, struct readahead_job *job)
{
    CACHE_SCOPE(guard, cache);
    struct kernel_page_cache_record *r = cache->record;
    if (r->stopping || physical_page_available(cache->allocator) <= r->low) job->cancelled = 1;
    return job->cancelled;
}

static void readahead_load(struct kernel_page_cache *cache, struct readahead_job *job,
                            const struct readahead_job *plan)
{
    KERNEL_LOCK_SCOPE(guard);
    if (!kernel_vfs_node_try_read(plan->node, &guard) || !read_cache_open(plan->node)) {
        CACHE_SCOPE(cancel, cache);
        job->cancelled = 1;
        return;
    }
    struct kernel_page_cache_record *r = cache->record;
    union foreground_workspace *lease = foreground_workspace_take(cache);
    if (!lease) return;
    struct kernel_page_cache_entry **entries = lease->read.loads;
    struct file_read_span *spans = lease->read.spans;
    unsigned candidates = 0, count = 0;
    /* 候选尚未发布，分配和扩容不持cache保护，也不产生等待环。 */
    for (unsigned i = 0; i < plan->pages; i++) {
        if (readahead_cancelled(cache, job)) break;
        uint64_t index = plan->first + i;
        if (index > UINT64_MAX >> BOAROS_PAGE_SHIFT ||
            (index << BOAROS_PAGE_SHIFT) >= kernel_vfs_node_size(plan->node)) break;
        int found;
        { CACHE_SCOPE(lookup, cache); (void)find_bucket(r, plan->node, index, &found); }
        if (found) continue;
        struct kernel_page_cache_entry *entry;
        void *page;
        if (allocate_entry(cache, plan->node, index, 0, &entry, &page)) break;
        entries[candidates] = entry;
        spans[candidates++] = (struct file_read_span){index << BOAROS_PAGE_SHIFT, page, BOAROS_PAGE_SIZE, 0, 0};
    }
    struct kernel_page_cache_entry *discard = NULL;
    for (;;) {
        int room = reserve_buckets(cache, candidates);
        CACHE_SCOPE(publish, cache);
        if (room && candidates > r->capacity-r->capacity/4-r->occupied-r->tombstones)
            continue;
        for (unsigned i = 0; i < candidates; i++) {
            struct kernel_page_cache_entry *entry = entries[i];
            int found;
            size_t bucket = find_bucket(r, plan->node, entry->page_index, &found);
            if (!room || job->cancelled || r->stopping || found) {
                entry->cleanup_next = discard; discard = entry;
            } else {
                publish_entry(cache, entry, bucket);
                entries[count] = entry; spans[count++] = spans[i];
            }
        }
        break;
    }
    while (discard) {
        struct kernel_page_cache_entry *entry = discard; discard = entry->cleanup_next;
        (void)abandon_entry(cache, entry, KERNEL_PAGE_CACHE_STATUS_OK);
    }
    /* 取消仅丢弃尚无需求读者的页；已经订阅的需求保留完整加载 owner。 */
    if (readahead_cancelled(cache, job)) {
        unsigned kept = 0;
        {
            CACHE_SCOPE(cancel, cache);
            for (unsigned i = 0; i < count; i++) {
                struct kernel_page_cache_entry *entry = entries[i];
                if (entry->users > 1) { entries[kept] = entry; spans[kept++] = spans[i]; continue; }
                __atomic_store_n(&entry->load_status,KERNEL_PAGE_CACHE_STATUS_OK,__ATOMIC_RELAXED); entry_return(entry);
                remove_entry(cache, entry);
            }
        }
        count = kept;
        uint64_t released = 0; (void)drain_entries(cache, &released);
    }
    if (!count) { foreground_workspace_put(cache, lease); return; }
    {
        CACHE_SCOPE(submit, cache);
        r->statistics.readahead_batches++;
        if (count > r->statistics.readahead_peak_pages) r->statistics.readahead_peak_pages = count;
    }
    size_t failed = load_read_pages(cache, plan->node, entries, spans, count, NULL, &lease->read.backend);
    foreground_workspace_put(cache, lease);
    CACHE_SCOPE(complete, cache);
    r->statistics.readahead_failed += failed;
    r->statistics.readahead_loaded += count - failed;
}

static void readahead_worker(void *argument)
{
    struct kernel_page_cache *cache = argument;
    struct kernel_page_cache_record *r = cache->record;
    kernel_assert_service_context();
    (void)arch_interrupt_save();
    for (;;) {
        struct readahead_job *next = NULL, plan;
        struct kernel_wait_token token = {0};
        int stop, cancelled = 0;
        {
            CACHE_SCOPE(select, cache);
            stop = r->stopping;
            for (unsigned i = 0; i < 8; i++) {
                struct readahead_job *job = &r->readahead_jobs[i];
                if (stop && job->node) job->cancelled = 1;
                if (job->node && (!next || job->cookie < next->cookie)) next = job;
            }
            if (next) {
                if (next->active) __builtin_trap();
                next->active = 1;
                plan = *next;
                cancelled = next->cancelled;
            } else if (!stop && kernel_wait_prepare(&r->readahead_work, 0, 1, &token)) __builtin_trap();
        }
        if (!next) {
            if (stop) break;
            enum kernel_wait_wake_reason reason;
            if (kernel_wait_park(&token, &reason) || kernel_wait_finish(&token)) __builtin_trap();
            continue;
        }
        if (!cancelled) readahead_load(cache, next, &plan);
        {
            CACHE_SCOPE(complete, cache);
            if (!next->active || next->node != plan.node || next->cookie != plan.cookie) __builtin_trap();
            if (next->cancelled) r->statistics.readahead_cancelled++;
            *next = (struct readahead_job){0};
        }
        if (kernel_vfs_node_release(&plan.node)) __builtin_trap();
        { CACHE_SCOPE(finished,cache);r->statistics.readahead_completed++; }
        /* Only yield after the bounded request and its node borrow finish. */
        arch_interrupt_restore(ARCH_INTERRUPT_ENABLE_MASK); (void)arch_interrupt_save();
        if (kernel_scheduler_yield_current() != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
    }
}
#else
static void readahead_cancel_node(struct kernel_page_cache *cache, struct kernel_vfs_node *node)
{ (void)cache; (void)node; }
#endif

int kernel_page_cache_is_executor(const struct kernel_page_cache *cache)
{
    if (!cache_live(cache)) return 0;
    struct kernel_io_context *executor=__atomic_load_n(&cache->record->executor_io,__ATOMIC_ACQUIRE);
    return executor && executor==kernel_io_context_current();
}
int kernel_page_cache_needs_cpu0(const struct kernel_page_cache *cache)
{
    return cache_live(cache) && !kernel_page_cache_is_executor(cache) && remote_cache_client();
}

static inline void request_borrow_locked(struct kernel_page_cache_record *record)
{
    /* Admission is serialized by metadata; concurrent returns only lower it. */
    if (__atomic_load_n(&record->request_borrows,__ATOMIC_RELAXED)==UINT32_MAX) __builtin_trap();
    __atomic_fetch_add(&record->request_borrows,1,__ATOMIC_RELAXED);
}
static inline void request_return(struct kernel_page_cache_record *record)
{
    /* No record access follows the release: destroy may consume the zero. */
    if (!__atomic_fetch_sub(&record->request_borrows,1,__ATOMIC_RELEASE)) __builtin_trap();
}

static enum kernel_page_cache_status run_request(struct kernel_page_cache *cache,
    void (*execute)(void *),void *argument,unsigned observed,const int *finished)
{
    struct cache_request request = {.execute=execute,.argument=argument,.finished=finished,.observed=observed,
        .queued_at=observed?kernel_time_monotonic_ns():0};
    struct kernel_task *task = kernel_task_current();
    if (kernel_task_has_user_context(task)) request.account_task = task;
    else request.account_task = kernel_io_context_current()->account_task;
    kernel_wait_queue_init(&request.changed);
    struct kernel_page_cache_record *r = cache->record;
    {
        CACHE_SCOPE(publish, cache);
        if (!r->started || r->stopping) return KERNEL_PAGE_CACHE_STATUS_STATE;
        if (r->read_requests == UINT32_MAX) __builtin_trap();
        request_borrow_locked(r);
        if (!r->reads_head) r->reads_tail = &r->reads_head;
        *r->reads_tail = &request; r->reads_tail = &request.next; r->read_requests++;
        r->requests_queued++;
        if (observed) r->executions_queued++;
        cache_wake(&r->work);
    }
    for (;;) {
        struct kernel_wait_token token = {0};
        {
            CACHE_SCOPE(check, cache);
            if (request.done) break;
            if (kernel_wait_prepare(&request.changed, 0, 0, &token)) __builtin_trap();
        }
        enum kernel_wait_wake_reason reason;
        if (kernel_wait_park(&token, &reason) || kernel_wait_finish(&token)) __builtin_trap();
    }
    if (kernel_wait_queue_close(&request.changed) ||
        kernel_wait_queue_destroy(&request.changed)) __builtin_trap();
    request_return(r);
    return request.status;
}
static enum kernel_page_cache_status run_cpu0_until(struct kernel_page_cache *cache,
    void (*execute)(void *),void *argument,const struct kernel_lock_guard *borrowed,
    const int *finished)
{
    if (!cache_live(cache) || !execute)
        return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    struct kernel_io_context *io=kernel_io_context_current();
    if (borrowed && (io->locks!=borrowed || borrowed->owner!=io ||
        !borrowed->lock || !borrowed->write || !kernel_lock_held(borrowed->lock,1) ||
        io->user_pins || io->cache_writes || kernel_cpu_current()->raw_locks ||
        !kernel_scheduler_can_sleep() || arch_external_interrupt_active()))
        return KERNEL_PAGE_CACHE_STATUS_STATE;
    /* A blocked writer can retain both its offset and operation guards. The
     * worker starts at inode rank and never acquires either caller guard. */
    if (borrowed) for (const struct kernel_lock_guard *g=borrowed;g;g=g->previous)
        if (g->owner!=io || !g->write || !g->lock ||
            (g->lock->rank!=10 && g->lock->rank!=15)) return KERNEL_PAGE_CACHE_STATUS_STATE;
    if (kernel_page_cache_is_executor(cache)) {
        do { execute(argument); } while (finished && !*finished);
        return KERNEL_PAGE_CACHE_STATUS_OK;
    }
    if ((!borrowed && io->locks) || io->user_pins || io->cache_writes || kernel_cpu_current()->raw_locks ||
        !kernel_scheduler_can_sleep() || arch_external_interrupt_active()) return KERNEL_PAGE_CACHE_STATUS_STATE;
    return run_request(cache,execute,argument,1,finished);
}
enum kernel_page_cache_status kernel_page_cache_run_cpu0_borrowing(struct kernel_page_cache *cache,
    void (*execute)(void *),void *argument,const struct kernel_lock_guard *borrowed)
{
    return run_cpu0_until(cache,execute,argument,borrowed,NULL);
}
enum kernel_page_cache_status kernel_page_cache_run_cpu0(struct kernel_page_cache *cache,
    void (*execute)(void *),void *argument)
{
    return kernel_page_cache_run_cpu0_borrowing(cache,execute,argument,NULL);
}
enum kernel_page_cache_status kernel_page_cache_get(
    struct kernel_page_cache *cache, const struct kernel_vfs_file *file,
    uint64_t page_index, uint64_t *physical_address, size_t *valid_bytes)
{
    if (!physical_address || !valid_bytes) return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    struct kernel_page_cache_read_page page;
    enum kernel_page_cache_status status=kernel_page_cache_get_batch(cache,file,page_index,&page,1);
    if (!status) { *physical_address=page.physical_address; *valid_bytes=page.valid_bytes; }
    return status;
}



/* 候选尚未发布；先取得inode读资格，再重查并发布本次装入。
 * 等待队列中较早的截断不会被一个尚未执行的loading条目挡住。 */
static void commit_read_batch(void *argument)
{
    struct read_batch_load *request=argument;
    struct kernel_page_cache *cache=request->cache;
    const struct kernel_vfs_file *file=request->file;
    struct kernel_vfs_node *node=kernel_vfs_file_node(file);
    uint64_t first=request->first;
    struct kernel_page_cache_read_page *pages=request->pages;
    size_t count=request->count,candidates=request->candidates;
    struct kernel_page_cache_entry **entries=request->entries,**loads=request->loads;
    struct kernel_page_cache_read_page **outputs=request->outputs;
    struct file_read_span *spans=request->spans;
    enum kernel_page_cache_status allocation=request->allocation;
    KERNEL_LOCK_SCOPE(inode);
    for (;;) {
        kernel_vfs_node_lock(node,&inode,0);
        if (read_cache_open(node)) break;
        if (inode.lock) kernel_lock_release(&inode);
        kernel_file_mapping_wait(node);
    }
    struct kernel_page_cache_entry *discard=NULL;
    size_t ready=0;
    for (;;) {
        CACHE_SCOPE(publish,cache);
        struct kernel_page_cache_record *r=cache->record;
        size_t room=r->capacity-r->capacity/4-r->occupied-r->tombstones;
        if (candidates>room) {
            kernel_lock_scope_release(&publish);
            if (reserve_buckets(cache,candidates)) continue;
            allocation=KERNEL_PAGE_CACHE_STATUS_NO_MEMORY;
            for (size_t i=0;i<count;i++) if (loads[i]) {
                loads[i]->cleanup_next=discard;discard=loads[i];loads[i]=NULL;
            }
            candidates=0;
            continue;
        }
        for (size_t i=0;i<count;i++) {
            if (pages[i].status!=KERNEL_PAGE_CACHE_STATUS_NOT_FOUND || entries[i]) continue;
            struct kernel_page_cache_entry *candidate=loads[i];
            if (((first+i)<<BOAROS_PAGE_SHIFT)>=kernel_vfs_node_size(node)) {
                if (candidate) { candidate->cleanup_next=discard;discard=candidate; }
                pages[i].status=KERNEL_PAGE_CACHE_STATUS_OUT_OF_RANGE;
                continue;
            }
            int found;size_t bucket=find_bucket(r,node,first+i,&found);
            if (found) {
                struct kernel_page_cache_entry *entry=r->buckets[bucket];
                if (read_entry_ready(entry)) {
                    take_read_page(cache,entry,&pages[i].physical_address,&pages[i].valid_bytes,entry_valid_bytes(entry));
                    pages[i].status=KERNEL_PAGE_CACHE_STATUS_OK;
                } else { entry_borrow(entry);entries[i]=entry; }
                if (candidate) { candidate->cleanup_next=discard;discard=candidate; }
            } else if (candidate) {
                publish_entry(cache,candidate,bucket);
                loads[ready]=candidate;spans[ready]=spans[i];outputs[ready]=&pages[i];ready++;
            } else {
                if (!allocation) __builtin_trap();
                pages[i].status=allocation;
            }
        }
        break;
    }
    while (discard) {
        struct kernel_page_cache_entry *entry=discard;discard=entry->cleanup_next;entry->cleanup_next=NULL;
        (void)abandon_entry(cache,entry,KERNEL_PAGE_CACHE_STATUS_OK);
    }
    /* 自有加载全部完成后才等待其他producer，反向重叠批次不互等。 */
    (void)load_read_pages(cache,node,loads,spans,ready,outputs,request->backend);
}

/* 只有缺失批次进入此处；页、描述及容量由调用CPU准备，热命中不租工作区。 */
static void __attribute__((noinline)) read_batch_local(struct kernel_page_cache *cache,const struct kernel_vfs_file *file,
    uint64_t first,struct kernel_page_cache_read_page *pages,size_t count,
    struct kernel_page_cache_entry **entries,int need_capacity,struct read_workspace *workspace)
{
    struct kernel_vfs_node *node=kernel_vfs_file_node(file);
    struct kernel_page_cache_entry *single_load;
    struct file_read_span single_span;
    struct kernel_page_cache_read_page *single_output;
    struct kernel_page_cache_entry **loads=workspace?workspace->loads:&single_load;
    struct file_read_span *spans=workspace?workspace->spans:&single_span;
    struct kernel_page_cache_read_page **outputs=workspace?workspace->outputs:&single_output;
    size_t missing=0,candidates=0;
    for (size_t i=0;i<count;i++) {
        loads[i]=NULL;
        missing+=pages[i].status==KERNEL_PAGE_CACHE_STATUS_NOT_FOUND && !entries[i];
    }
    enum kernel_page_cache_status allocation=KERNEL_PAGE_CACHE_STATUS_OK;
    if (need_capacity && !reserve_buckets(cache,missing)) allocation=KERNEL_PAGE_CACHE_STATUS_NO_MEMORY;
    /* 容量按批次准备；候选未发布，整批分配不制造尚未提交的loading依赖。 */
    for (size_t i=0;!allocation && i<count;i++) {
        if (pages[i].status!=KERNEL_PAGE_CACHE_STATUS_NOT_FOUND || entries[i]) continue;
        void *data;
        allocation=allocate_entry(cache,node,first+i,0,&loads[i],&data);
        if (allocation) break;
        spans[i]=(struct file_read_span){(first+i)<<BOAROS_PAGE_SHIFT,data,BOAROS_PAGE_SIZE,0,0};
        candidates++;
    }
    if (candidates) {
        struct read_batch_load request={cache,file,first,pages,count,candidates,entries,loads,outputs,spans,
            workspace?&workspace->backend:NULL,allocation};
        enum kernel_page_cache_status submitted=KERNEL_PAGE_CACHE_STATUS_OK;
        if (node->instance->ops->read_execution==KERNEL_VFS_EXECUTION_CPU0 && remote_cache_client())
            submitted=run_request(cache,commit_read_batch,&request,0,NULL);
        else commit_read_batch(&request);
        if (submitted) {
            for (size_t i=0;i<count;i++) {
                if (loads[i]) (void)abandon_entry(cache,loads[i],submitted);
                if (pages[i].status==KERNEL_PAGE_CACHE_STATUS_NOT_FOUND) pages[i].status=submitted;
            }
            CACHE_SCOPE(guard,cache);
            for (size_t i=0;i<count;i++) if (entries[i]) { entry_return(entries[i]); entries[i]=NULL; }
        }
    } else {
        for (size_t i=0;i<count;i++)
            if (pages[i].status==KERNEL_PAGE_CACHE_STATUS_NOT_FOUND && !entries[i]) pages[i].status=allocation;
    }
    /* 自有加载已完成，等待其他producer只占用调用者，不堵塞CPU0执行者。 */
    for (size_t i=0;i<count;i++) if (entries[i])
        pages[i].status=finish_read_page(cache,entries[i],&pages[i].physical_address,&pages[i].valid_bytes);
}

struct cache_read_batch_request {
    struct kernel_page_cache *cache;
    const struct kernel_vfs_file *file;
    uint64_t first;
    struct kernel_page_cache_read_page *pages;
    size_t count;
};

static int read_cached_batch(struct cache_read_batch_request *request)
{
    struct kernel_page_cache *cache=request->cache;
    struct kernel_vfs_node *node=kernel_vfs_file_node(request->file);
    int pending=0,retry=0,remote=-1;
    {
        CACHE_SCOPE(guard,cache);
        retry=!read_cache_open(node);
        uint64_t size=retry?0:kernel_vfs_node_size(node);
        for (size_t i=0;i<request->count && !retry;i++) {
            struct kernel_page_cache_read_page *page=&request->pages[i];
            uint64_t offset=(request->first+i)<<BOAROS_PAGE_SHIFT;
            if (offset>=size) { page->status=KERNEL_PAGE_CACHE_STATUS_OUT_OF_RANGE; continue; }
            struct kernel_page_cache_entry *entry;
            enum kernel_page_cache_status status=lookup_entry(cache,request->file,request->first+i,&entry);
            if (status==KERNEL_PAGE_CACHE_STATUS_NOT_FOUND) {
#if BOAROS_PAGE_CACHE_READAHEAD_PAGES
                page->cache_miss=1;
#endif
                pending=1;
                if (remote<0) remote=remote_cache_client();
                if (!remote) break;
                continue;
            }
            if (status) __builtin_trap();
            if (!read_entry_ready(entry)) {
                if ((__atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)&CACHE_WRITES_CLOSED)) { retry=1; break; }
#if BOAROS_PAGE_CACHE_READAHEAD_PAGES
                page->cache_miss=1;
#endif
                pending=1;
                if (remote<0) remote=remote_cache_client();
                if (!remote) break;
                continue;
            }
            size_t valid=size-offset<BOAROS_PAGE_SIZE?size-offset:BOAROS_PAGE_SIZE;
            take_read_page(cache,entry,&page->physical_address,&page->valid_bytes,valid);
            page->status=KERNEL_PAGE_CACHE_STATUS_OK;
        }
        if (pending || retry) {
            /* Private preparation has no indexed entry yet. Retain the same
             * cache lifetime used by queued requests until caller unwind. */
            request_borrow_locked(cache->record);
        }
    }
    if (retry) {
        /* 跨resize资格不能保留本次暂取结果；先归还页，才允许慢路等待inode。
         * 普通miss保留成功项，未完成项在取得inode后重新检查，避免重复pin。 */
        for (size_t i=0;i<request->count;i++) {
            struct kernel_page_cache_read_page *page=&request->pages[i];
            if (!page->status && physical_page_release(cache->allocator,page->physical_address)) __builtin_trap();
            page->status=KERNEL_PAGE_CACHE_STATUS_NOT_FOUND;
        }
    }
    return pending || retry;
}
static void __attribute__((noinline)) prepare_read_batch(void *argument)
{
    struct cache_read_batch_request *request = argument;
    struct kernel_page_cache *cache = request->cache;
    struct kernel_vfs_node *node = kernel_vfs_file_node(request->file);
    union foreground_workspace *lease=request->count>1?foreground_workspace_take(cache):NULL;
    if (request->count>1 && !lease) {
        for (size_t i=0;i<request->count;i++)
            if (request->pages[i].status==KERNEL_PAGE_CACHE_STATUS_NOT_FOUND)
                request->pages[i].status=KERNEL_PAGE_CACHE_STATUS_NO_MEMORY;
        return;
    }
    /* 单页fault只保留一个entry；多页的借用表和装入器共用已有工作区。
     * 热命中在调用本函数前返回，不为慢路数组预留栈空间。 */
    struct kernel_page_cache_entry *single_entry;
    struct kernel_page_cache_entry **entries=lease?lease->read.entries:&single_entry;
    size_t missing=0;
    int pending = 0,need_capacity=0;
    for (;;) {
        CACHE_SCOPE(lookup, cache);
        if (!read_cache_open(node)) {
            kernel_lock_release(&lookup);
            kernel_file_mapping_wait(node);
            continue;
        }
        uint64_t size=kernel_vfs_node_size(node);
        for (size_t i = 0; i < request->count; i++) {
            struct kernel_page_cache_read_page *page = &request->pages[i];
            entries[i] = NULL;
            if (page->status != KERNEL_PAGE_CACHE_STATUS_NOT_FOUND) continue;
            uint64_t offset = (request->first + i) << BOAROS_PAGE_SHIFT;
            if (offset >= size) {
                page->status = KERNEL_PAGE_CACHE_STATUS_OUT_OF_RANGE;
                continue;
            }
            struct kernel_page_cache_entry *entry;
            enum kernel_page_cache_status status = lookup_entry(cache, request->file, request->first + i, &entry);
            if (!status) {
                if (read_entry_ready(entry)) {
                    size_t valid = size - offset < BOAROS_PAGE_SIZE ? size - offset : BOAROS_PAGE_SIZE;
                    take_read_page(cache, entry, &page->physical_address, &page->valid_bytes, valid);
                    page->status = KERNEL_PAGE_CACHE_STATUS_OK;
                    continue;
                }
                entry_borrow(entry); entries[i] = entry;
            } else if (status != KERNEL_PAGE_CACHE_STATUS_NOT_FOUND) {
                page->status = status;
                continue;
            }
            pending = 1;
            if (!entries[i]) missing++;
        }
        struct kernel_page_cache_record *r=cache->record;
        need_capacity=missing>r->capacity-r->capacity/4-r->occupied-r->tombstones;
        break;
    }
    if (pending) read_batch_local(cache,request->file,request->first,request->pages,request->count,entries,need_capacity,
        lease?&lease->read:NULL);
    if (lease) foreground_workspace_put(cache,lease);
}

enum kernel_page_cache_status kernel_page_cache_get_batch(
    struct kernel_page_cache *cache, const struct kernel_vfs_file *file, uint64_t first,
    struct kernel_page_cache_read_page *pages, size_t count)
{
    if (!cache_live(cache) || !kernel_vfs_file_node(file) || count > KERNEL_PAGE_CACHE_READ_BATCH_MAX ||
        (count && (!pages || first > UINT64_MAX >> BOAROS_PAGE_SHIFT ||
            count - 1 > (UINT64_MAX >> BOAROS_PAGE_SHIFT) - first)))
        return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    for (size_t i=0;i<count;i++) {
        pages[i].status=KERNEL_PAGE_CACHE_STATUS_NOT_FOUND;
#if BOAROS_PAGE_CACHE_READAHEAD_PAGES
        pages[i].cache_miss=0;
#endif
    }
    if (!count) return KERNEL_PAGE_CACHE_STATUS_OK;
    struct cache_read_batch_request request = {cache, file, first, pages, count};
    /* local首个缺页后交给inode资格下的重查，不重复预扫整批。remote须先
     * 取出全部热页：尚未执行的CPU0请求被取消时，后部成功项也继续有效。 */
    if (read_cached_batch(&request)) {
        prepare_read_batch(&request);
        request_return(cache->record);
    }
    enum kernel_page_cache_status result = KERNEL_PAGE_CACHE_STATUS_OK;
    for (size_t i = 0; i < count; i++) if (pages[i].status) {
        pages[i].physical_address = 0;
        pages[i].valid_bytes = 0;
        if (!result) result = pages[i].status;
    }
    return result;
}

static void resize_apply(struct kernel_page_cache_resize *,uint64_t);
static int write_admission(struct kernel_page_cache *cache,struct kernel_vfs_node *node);
/* 已发布entry的借用覆盖其数据页，不再另加物理引用；完整冷覆盖只准备私有候选，
 * 用户来源缺页结束后才发布，避免把未初始化数据交给读取者或等待自身加载。 */
static enum kernel_page_cache_status prepare_write_entry(struct kernel_page_cache *cache,
    struct kernel_vfs_file *file, uint64_t index, int whole,
    struct kernel_page_cache_entry **entry, struct kernel_page_cache_entry **candidate,
    uint64_t *address)
{
    struct kernel_vfs_node *node=kernel_vfs_file_node(file);
    int missing,need_capacity;
    {
        CACHE_SCOPE(guard,cache);
        enum kernel_page_cache_status status=lookup_entry(cache,file,index,entry);
        missing=status==KERNEL_PAGE_CACHE_STATUS_NOT_FOUND;
        struct kernel_page_cache_record *record=cache->record;
        need_capacity=record->occupied+record->tombstones>=record->capacity-record->capacity/4;
        if (!status && entry_load_status(*entry) == KERNEL_PAGE_CACHE_STATUS_OK) {
            entry_borrow(*entry);
            *address=(*entry)->physical_address;
            return KERNEL_PAGE_CACHE_STATUS_OK;
        }
        *entry=NULL;
        if (status && !missing) return status;
    }
    if (missing && (whole || (index<<BOAROS_PAGE_SHIFT)>=kernel_vfs_node_size(node))) {
        if (need_capacity && !reserve_buckets(cache,1)) return KERNEL_PAGE_CACHE_STATUS_NO_MEMORY;
        void *bytes;
        return allocate_entry(cache,node,index,!whole,candidate,&bytes);
    }
    size_t valid;
    enum kernel_page_cache_status status=kernel_page_cache_get(cache,file,index,address,&valid);
    if (status) return status;
    {
        CACHE_SCOPE(guard,cache);
        status=lookup_entry(cache,file,index,entry);
        if (!status && (*entry)->physical_address==*address) entry_borrow(*entry);
        else { *entry=NULL; status=KERNEL_PAGE_CACHE_STATUS_NOT_FOUND; }
    }
    if (physical_page_release(cache->allocator,*address)) __builtin_trap();
    return status;
}

struct prepare_space {
    struct kernel_vfs_node *node;
    uint64_t offset;
    size_t length,ready;
    int mapping,error,complete;
    void *reservation;
};
static void prepare_space(void *argument)
{
    struct prepare_space *request=argument;
    struct kernel_vfs_node *node=request->node;
    KERNEL_LOCK_SCOPE(io); kernel_vfs_node_lock(node,&io,1);
    size_t length=request->length;
    if (request->mapping) {
        if (request->offset>=node->size) { request->error=-KERNEL_EFAULT; return; }
        if (length>node->size-request->offset) length=node->size-request->offset;
    }
    request->error=node->instance->ops->error(node->instance);
    if (!request->error) {
        if (node->instance->ops->reserve_write)
            request->error=node->instance->ops->reserve_write(node,request->offset,length,&request->ready,&request->complete,&request->reservation);
        else if (node->instance->ops->prepare_write)
            request->error=node->instance->ops->prepare_write(node,request->offset,length,&request->ready,&request->complete);
        else request->ready=length;
    }
    if (request->ready>length || (!request->error && request->ready!=length)) __builtin_trap();
    if (request->mapping && request->ready!=length && !request->error) __builtin_trap();
}
static void finish_space(void *argument)
{
    struct prepare_space *request=argument;
    struct kernel_vfs_node *node=request->node;
    KERNEL_LOCK_SCOPE(io);kernel_vfs_node_lock(node,&io,1);
    node->instance->ops->finish_reservation(node,request->reservation,request->offset);
    request->reservation=NULL;
}
static void complete_space(void *argument)
{
    struct prepare_space *request=argument;
    struct kernel_vfs_node *node=request->node;
    request->error=kernel_page_cache_writeback_range_borrowing(node->instance->page_cache,
        node,0,UINT64_MAX,kernel_io_context_current()->locks);
    if (!request->error) request->error=node->instance->ops->complete_write(node);
}
static int prepare_space_request(struct kernel_page_cache *cache,struct prepare_space *request,
    void (*execute)(void *))
{
    /* 来源pin和页写资格在空间准备前归还；write/offset门闩保持同次写互斥。
     * CPU0后端借用这些门闩，CALLER后端直接使用当前任务的事务现场。 */
    struct kernel_io_context *context=kernel_io_context_current();
    if (context->user_pins || context->cache_writes || context->backend_depth || kernel_cpu_current()->raw_locks)
        __builtin_trap();
    for (struct kernel_lock_guard *g=context->locks;g;g=g->previous)
        if (g->lock->rank>15) __builtin_trap();
    if (request->node->instance->ops->file_metadata_execution == KERNEL_VFS_EXECUTION_CPU0 &&
        kernel_page_cache_needs_cpu0(cache)) {
        if (run_request(cache,execute,request,1,NULL)) return -KERNEL_EIO;
    } else execute(request);
    return request->error;
}
int kernel_page_cache_prepare_mapping_write(struct kernel_page_cache *cache,struct kernel_vfs_node *node,
    uint64_t index,struct kernel_page_cache_reservation *reservation)
{
    struct prepare_space request={.node=node,.offset=index<<BOAROS_PAGE_SHIFT,
        .length=BOAROS_PAGE_SIZE,.mapping=1};
    int result=prepare_space_request(cache,&request,prepare_space);
    if (result && request.reservation) prepare_space_request(cache,&request,finish_space);
    if (!result) *reservation=(struct kernel_page_cache_reservation){request.reservation,request.offset,request.offset+request.ready};
    return result;
}
void kernel_page_cache_accept_mapping_write(struct kernel_vfs_node *node,
    struct kernel_page_cache_reservation *reservation)
{
    if (reservation->ticket)
        node->instance->ops->accept_reservation(node,reservation->ticket,reservation->end);
}
void kernel_page_cache_finish_mapping_write(struct kernel_page_cache *cache,struct kernel_vfs_node *node,
    struct kernel_page_cache_reservation *reservation,int accepted)
{
    if (!reservation->ticket)return;
    struct prepare_space request={.node=node,.reservation=reservation->ticket,
        .offset=accepted?reservation->end:reservation->offset};
    prepare_space_request(cache,&request,finish_space);
    *reservation=(struct kernel_page_cache_reservation){0};
}

static int copy_cached_source(struct kernel_page_cache *cache,struct kernel_vfs_file *file,
    uint64_t offset,struct kernel_vfs_write_source *source,size_t size,size_t *written,int *complete,struct prepare_space *reservation)
{
    struct kernel_vfs_node *node=kernel_vfs_file_node(file);
    *written=0;
    size_t ready=0;
    int space_error=0;
    while (*written<size) {
        if (!ready) {
            if (reservation->reservation) {
                reservation->offset=offset;
                prepare_space_request(cache,reservation,finish_space);
            }
            if (space_error) return space_error;
            int error=write_admission(cache,node);
            if (error) return error;
            size_t length=BOAROS_PAGE_CACHE_WRITEBACK_PAGES*BOAROS_PAGE_SIZE-(offset&BOAROS_PAGE_MASK);
            if (length>size-*written) length=size-*written;
            int needed=0;
            *reservation=(struct prepare_space){.node=node,.offset=offset,.length=length};
            space_error=prepare_space_request(cache,reservation,prepare_space);
            ready=reservation->ready;needed=reservation->complete;
            *complete|=needed;
            if (!ready) return space_error;
        }
        size_t start=offset&BOAROS_PAGE_MASK;
        size_t count=BOAROS_PAGE_SIZE-start;
        if (count>size-*written) count=size-*written;
        if (count>ready) count=ready;
        size_t first_part;
        count=kernel_vfs_write_source_page_span(source,count,&first_part);
        if (!count) { source->access=KERNEL_UACCESS_STATUS_STATE; return 0; }
        KERNEL_LOCK_SCOPE(io);
        /* 外层文件写操作门固定逻辑尺寸，取得写入来源前不重复占用inode。 */
        uint64_t old_size=kernel_vfs_node_size(node);
        struct kernel_page_cache_resize resize={0};
        uint64_t address=0;
        struct kernel_page_cache_entry *entry=NULL,*candidate=NULL;
        enum kernel_page_cache_status status=prepare_write_entry(cache,file,offset>>BOAROS_PAGE_SHIFT,
            !start && count==BOAROS_PAGE_SIZE,&entry,&candidate,&address);
        if (status) {
            kernel_page_cache_resize_finish(&resize,old_size);
            if (status==KERNEL_PAGE_CACHE_STATUS_NOT_FOUND) continue;
            return status==KERNEL_PAGE_CACHE_STATUS_NO_MEMORY ? -KERNEL_ENOMEM : -KERNEL_EIO;
        }
        struct kernel_page_cache_write_borrow writer={0};
        struct kernel_mm_user_pin pin={0};
        if (!kernel_vfs_write_source_pin(source,&pin)) {
            if (candidate) (void)abandon_entry(cache,candidate,KERNEL_PAGE_CACHE_STATUS_OK);
            kernel_page_cache_resize_finish(&resize,old_size);
            if (entry) {
                { CACHE_SCOPE(guard,cache); entry_return(entry); }
            }
            return 0;
        }
        if (offset+count>old_size) {
            int error=kernel_page_cache_resize_prepare(cache,node,old_size,offset+count,&resize);
            if (error==-KERNEL_EBUSY && source->mm) {
                /* 有MM编辑正在排空本次读pin：先归还，再等待别名撤权。
                 * 之后只重取现有pin；若映射已变，开放目标后再做真正缺页。 */
                kernel_mm_unpin_user_page(&pin);
                error=kernel_page_cache_resize_prepare(cache,node,old_size,offset+count,&resize);
                if (!error) {
                    uint64_t user=source->user.iov[source->user.index].base+source->user.offset;
                    enum kernel_mm_status pinned=kernel_mm_pin_user_page(source->mm,user,KERNEL_MM_READ,&pin);
                    if (pinned==KERNEL_MM_STATUS_OK) kernel_vfs_write_source_use_pin(source,&pin);
                    else if (pinned!=KERNEL_MM_STATUS_NOT_MAPPED && pinned!=KERNEL_MM_STATUS_ACCESS)
                        __builtin_trap();
                }
            }
            if (error || (source->mm && !pin.registration)) {
                if (pin.registration) kernel_mm_unpin_user_page(&pin);
                kernel_page_cache_resize_finish(&resize,old_size);
                if (candidate) (void)abandon_entry(cache,candidate,KERNEL_PAGE_CACHE_STATUS_OK);
                if (entry) {
                    { CACHE_SCOPE(guard,cache); entry_return(entry); }
                }
                if (error) return error;
                continue;
            }
        }
        kernel_vfs_node_lock(node,&io,1);
        int admitted=0,loading=0;
        {
            CACHE_SCOPE(guard,cache);
            if (candidate) {
                int found; size_t bucket=find_bucket(cache->record,node,offset>>BOAROS_PAGE_SHIFT,&found);
                if (found) {
                    struct kernel_page_cache_entry *current=cache->record->buckets[bucket];
                    if (entry_load_status(current) == KERNEL_PAGE_CACHE_STATUS_OK) entry=current;
                } else if (cache->record->occupied+cache->record->tombstones+1 <=
                    cache->record->capacity-cache->record->capacity/4) {
                    entry=candidate; candidate=NULL;
                    publish_entry(cache,entry,bucket); loading=1;
                }
                if (entry) {
                    entry_borrow(entry); address=entry->physical_address;
                }
            }
            if (entry) admitted=resize.entry==entry || writer_begin(entry,&writer);
        }
        if (candidate) (void)abandon_entry(cache,candidate,KERNEL_PAGE_CACHE_STATUS_OK);
        if (!admitted) {
            if (loading) __builtin_trap(); /* 新候选尚无其他写入者。 */
            kernel_lock_release(&io);
            if (pin.registration) kernel_mm_unpin_user_page(&pin);
            kernel_page_cache_resize_finish(&resize,old_size);
            if (entry) {
                (void)kernel_page_cache_wait_writable(cache,file,offset>>BOAROS_PAGE_SHIFT,address);
                { CACHE_SCOPE(guard,cache); entry_return(entry); }
            }
            continue;
        }
        void *page;
        if (physical_page_map_owned(cache->allocator,address,&page)) __builtin_trap();
        /* 首片段的地址已由pin准备；标量写不再重复查span或重建来源指针。 */
        kernel_vfs_write_source_copy(source,(unsigned char *)page+start,first_part);
        kernel_vfs_write_source_advance(source,first_part);
        for (size_t copied=first_part;copied<count;) {
            size_t part=kernel_vfs_write_source_span(source,count-copied);
            if (!part) __builtin_trap();
            if (source->mm) kernel_vfs_write_source_use_pin(source,&pin);
            kernel_vfs_write_source_copy(source,(unsigned char *)page+start+copied,part);
            kernel_vfs_write_source_advance(source,part);
            copied+=part;
        }
        resize_apply(&resize,offset>old_size?offset:old_size);
        {
            CACHE_SCOPE(guard,cache);
            dirty_mark(entry,start,start+count);
            if (loading) { __atomic_store_n(&entry->load_status,KERNEL_PAGE_CACHE_STATUS_OK,__ATOMIC_RELAXED); entry_return(entry); cache_wake(&entry->ready); }
            if (writer.registration) writer_end_locked(&writer);
            entry_return(entry);
        }
        kernel_vfs_node_written(node,offset+count);
        kernel_lock_release(&io);
        if (pin.registration) kernel_mm_unpin_user_page(&pin);
        kernel_page_cache_resize_finish(&resize,old_size);
        offset+=count; *written+=count; ready-=count;
    }
    return 0;
}
static int write_cached_source(struct kernel_page_cache *cache,struct kernel_vfs_file *file,
    uint64_t offset,struct kernel_vfs_write_source *source,size_t size,size_t *written)
{
    int complete=0;
    struct prepare_space reservation={0};
    int error=copy_cached_source(cache,file,offset,source,size,written,&complete,&reservation);
    if (reservation.reservation) {
        reservation.offset=offset+*written;
        prepare_space_request(cache,&reservation,finish_space);
    }
    if (complete) {
        struct prepare_space request={.node=kernel_vfs_file_node(file)};
        int result=prepare_space_request(cache,&request,complete_space);
        if (!error) error=result;
    }
    return error;
}

int kernel_page_cache_write_source(struct kernel_page_cache *cache,
    struct kernel_vfs_file *file,uint64_t offset,
    struct kernel_vfs_write_source *source,size_t size,size_t *written)
{
    struct kernel_vfs_node *node=kernel_vfs_file_node(file);
    /* 来源缺页可以重入同一inode，调用者不得跨此入口持有数据锁。 */
    if (kernel_lock_held(&node->io_lock,0)) __builtin_trap();
    int result=write_cached_source(cache,file,offset,source,size,written);
    /* 按实际接纳的范围发布一次脏工作，部分成功后出错也必须通知。 */
    if (*written) kernel_page_cache_notify_pressure(cache);
    return result;
}

static int complete_writeback(struct kernel_page_cache *cache,
    struct kernel_page_cache_entry *entry,const struct prepared_writeback *prepared,int result)
{
    size_t begin=prepared->begin,end=prepared->end;
    COST_ADD(WRITEBACK_ACCEPTED,prepared->written);
    if (prepared->written>end-begin) __builtin_trap();
    if (!result && !entry->retired && prepared->generation==entry->generation) {
#if BOAROS_PAGE_CACHE_DIRTY_BLOCKS
        if (prepared->blocks) {
            size_t block=dirty_block_size(entry);
            uint64_t covered=prepared->blocks;
            /* 部分范围同步只清除已覆盖的脏块；边界外修改继续保留。 */
            if (end<entry->dirty_end && end%block)
                covered&=~(UINT64_C(1)<<(end/block));
            entry->dirty_blocks&=dirty_block_range(block,entry->dirty_begin,entry->dirty_end);
            entry->dirty_blocks&=~covered;
            if (!entry->dirty_blocks) dirty_clear(entry);
            else {
                uint64_t mask=entry->dirty_blocks;
                size_t first=0;
                while (!(mask&1)) { first+=block;mask>>=1; }
                size_t last=first;
                do { last+=block; } while ((mask>>=1));
                if (entry->dirty_begin<first) entry->dirty_begin=first;
                if (entry->dirty_end>last) entry->dirty_end=last;
            }
        } else
#endif
        if (end==entry->dirty_end) dirty_clear(entry);
        else entry->dirty_begin=end;
    }
    if (!entry->writeback || !cache->record->writeback_active) __builtin_trap();
    entry->writeback_error=result;entry->writeback=0;cache->record->writeback_active--;
    cache_wake(&entry->ready);
    /* 旧版本完成后只通知真实剩余工作；错误仍由原owner保留。 */
    if (!result && cache->record->started && !cache->record->stopping &&
        cache->record->force_writeback && !entry->retired && entry->dirty_end) {
        cache->record->requested=1;cache_wake(&cache->record->work);
    }
    return result;
}
_Static_assert(BOAROS_PAGE_SIZE <= UINT32_MAX && BOAROS_PAGE_CACHE_WRITEBACK_PAGES <= UINT8_MAX,
    "writeback page offsets and span indices must fit their stored representation");
_Static_assert(BOAROS_PAGE_CACHE_WRITEBACK_PAGES * (sizeof(struct prepared_writeback) +
    sizeof(struct file_write_span) + sizeof(struct kernel_page_cache_entry *)) <= BOAROS_PAGE_SIZE,
    "a complete writeback workspace must fit one physical page");

struct writeback_workspace {
    struct prepared_writeback *prepared;
    struct file_write_span *spans;
    struct kernel_page_cache_entry **selected;
};
static int writeback_workspace_init(struct kernel_page_cache *cache,struct writeback_workspace *work)
{
    size_t count=BOAROS_PAGE_CACHE_WRITEBACK_PAGES;
    size_t bytes=count*(sizeof(*work->prepared)+sizeof(*work->spans)+sizeof(*work->selected));
    if (kernel_heap_allocate(cache->heap,bytes,(void **)&work->prepared)) return -KERNEL_ENOMEM;
    work->spans=(void *)(work->prepared+count);
    work->selected=(void *)(work->spans+count);
    return 0;
}
static void writeback_workspace_release(struct kernel_page_cache *cache,struct writeback_workspace *work)
{
    kernel_heap_release(cache->heap,work->prepared);
}

static void writeback_unprepare(struct kernel_page_cache *cache,
    struct kernel_page_cache_entry *entry,struct prepared_writeback *prepared)
{
    if (prepared->version) version_release(prepared->version);
    prepared->version=NULL;
    { CACHE_SCOPE(guard,cache);
      if (prepared->closed) {
          uint64_t previous=__atomic_fetch_and(&entry->writes,~CACHE_WRITES_CLOSED,__ATOMIC_RELEASE);
          if (!(previous&CACHE_WRITES_CLOSED)) __builtin_trap();
      }
      entry->writeback=0; cache->record->writeback_active--;
      cache_wake(&entry->ready); }
}

static int prepare_writeback(struct kernel_page_cache *cache,
    struct kernel_page_cache_entry *entry,uint64_t limit,int wait_version,
    struct prepared_writeback *prepared)
{
    const int background=kernel_page_cache_is_executor(cache) && kernel_io_context_current()->background_reclaim;
    for (struct kernel_page_cache_write_borrow *b=kernel_io_context_current()->cache_writes;b;b=b->next)
        if (b->entry==entry) return -KERNEL_EBUSY;
Retry:
    for (;;) {
        struct kernel_wait_token token={0};
        {
            CACHE_SCOPE(guard,cache);
            uint64_t start=entry->page_index<<BOAROS_PAGE_SHIFT;
            if (entry->retired || !entry->dirty_end || limit<=start || limit-start<=entry->dirty_begin) return 0;
            if (entry_loading(entry) || !kernel_vfs_node_writeback_allowed(entry->node)) return -KERNEL_EBUSY;
            if (!entry->writeback && !(__atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)&CACHE_WRITES_CLOSED)) {
                /* 选择批次的借用覆盖准备、等待和完成；版本另按真实读者保活。 */
                entry->writeback=1; cache->record->writeback_active++; break;
            }
            if (background) { entry->retry_blocked=1; return -KERNEL_EBUSY; }
            if (kernel_wait_prepare(&entry->ready,0,0,&token)) __builtin_trap();
        }
        enum kernel_wait_wake_reason reason;
        if (kernel_wait_park(&token,&reason) || kernel_wait_finish(&token)) __builtin_trap();
    }
    prepared->version=version_take(cache,entry->node);
    if (!prepared->version) {
        writeback_unprepare(cache,entry,prepared);
        if (wait_version && background && cache->record->started &&
            version_wait(cache)) goto Retry;
        return -KERNEL_ENOMEM;
    }
    int result=0,writing=0;
    struct kernel_page_cache_alias *aliases=NULL;
    {
        CACHE_SCOPE(guard,cache);
        if (background) {
            uint64_t expected=0;
            entry->retry_blocked=1;
            if (!__atomic_compare_exchange_n(&entry->writes,&expected,CACHE_WRITES_CLOSED,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
                result=-KERNEL_EBUSY;
            else prepared->closed=1;
        } else {
            uint64_t previous=__atomic_fetch_or(&entry->writes,CACHE_WRITES_CLOSED,__ATOMIC_ACQ_REL);
            if (previous&CACHE_WRITES_CLOSED || previous&~CACHE_WRITES_MASK) __builtin_trap();
            prepared->closed=1;
            writing=(uint32_t)previous!=0;
        }
        if (!result) { aliases=entry->aliases; alias_borrow(aliases); }
    }
    if (!result) result=rearm_aliases(entry,aliases);
    while (!result && writing) {
        struct kernel_wait_token token={0};
        {
            CACHE_SCOPE(guard,cache);
            if (!(__atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)&UINT32_MAX)) break;
            if (kernel_wait_prepare(&entry->ready,0,0,&token)) __builtin_trap();
        }
        enum kernel_wait_wake_reason reason;
        if (kernel_wait_park(&token,&reason) || kernel_wait_finish(&token)) __builtin_trap();
    }
    if (result) {
        if (background) {
            CACHE_SCOPE(guard,cache);
            cache->record->deferred_snapshots++;
        }
        writeback_unprepare(cache,entry,prepared);
        return result;
    }
    return 0;
}

static int next_writeback_range(struct kernel_page_cache_entry *entry,
    struct prepared_writeback *prepared,size_t *cursor,size_t *begin,size_t *end)
{
    *begin=*cursor;*end=prepared->end;
    if (*begin>=*end) return 0;
#if BOAROS_PAGE_CACHE_DIRTY_BLOCKS
    if (prepared->blocks) {
        size_t block=dirty_block_size(entry);
        unsigned bit=*begin/block,last=(*end+block-1)/block;
        while (bit<last && !(prepared->blocks&(UINT64_C(1)<<bit))) bit++;
        if (bit==last) { *cursor=*end;return 0; }
        if (*begin<bit*block) *begin=bit*block;
        while (bit<last && (prepared->blocks&(UINT64_C(1)<<bit))) bit++;
        if (*end>bit*block) *end=bit*block;
    }
#else
    (void)entry;
#endif
    *cursor=*end;return 1;
}
static size_t writeback_block_size(const struct kernel_page_cache_entry *entry)
{
    size_t block=entry->node->instance->writeback_block_size;
    if (!block) block=1;
    if (block>BOAROS_PAGE_SIZE) block=BOAROS_PAGE_SIZE;
    if (block&(block-1)) __builtin_trap();
    return block;
}
static int next_writeback_span(struct kernel_page_cache_entry *entry,
    struct prepared_writeback *prepared,size_t *cursor,struct file_write_span *span)
{
    size_t begin,end;
    if (!next_writeback_range(entry,prepared,cursor,&begin,&end)) return 0;
    size_t block=writeback_block_size(entry);
    size_t first=begin&~(block-1),last=(end+block-1)&~(block-1);
    void *stable=(unsigned char *)version_data(prepared->version)+first;
    *span=(struct file_write_span){
        .offset=(entry->page_index<<BOAROS_PAGE_SHIFT)+begin,
        .buffer=(unsigned char *)stable+begin-first,.size=end-begin,
        .source={.context=prepared->version,.retain=version_retain,.release=version_release,.bytes=version_bytes()},
        .source_buffer=stable,.source_size=last-first};
    return 1;
}
static void capture_writeback(struct kernel_page_cache *cache,
    struct kernel_page_cache_entry *entry,struct prepared_writeback *prepared,uint64_t limit)
{
    void *page;
    if (physical_page_map_owned(cache->allocator,entry->physical_address,&page)) __builtin_trap();
    CACHE_SCOPE(guard,cache);
    prepared->begin=entry->dirty_begin;prepared->end=entry->dirty_end;
    prepared->generation=entry->generation;
    uint64_t start=entry->page_index<<BOAROS_PAGE_SHIFT;
    size_t valid=entry_valid_bytes(entry);
    if (prepared->end>valid) prepared->end=valid;
    if (limit>start && prepared->end>limit-start) prepared->end=(size_t)(limit-start);
    if (entry->retired) prepared->begin=prepared->end=0;
    if (prepared->end<prepared->begin) prepared->end=prepared->begin;
#if BOAROS_PAGE_CACHE_DIRTY_BLOCKS
    size_t block=dirty_block_size(entry);
    if (block) prepared->blocks=entry->dirty_blocks&dirty_block_range(block,prepared->begin,prepared->end);
#endif
    size_t cursor=prepared->begin,begin,end;
    size_t copy_block=writeback_block_size(entry);
    unsigned char *stable=version_data(prepared->version);
    while (next_writeback_range(entry,prepared,&cursor,&begin,&end)) {
        size_t first=begin&~(copy_block-1),last=(end+copy_block-1)&~(copy_block-1);
        size_t copy_end=last<valid?last:valid;
        COST_ADD(SNAPSHOT_COPY,copy_end-first);COST_ADD(WRITEBACK_REQUESTED,end-begin);
        memcpy(stable+first,(unsigned char *)page+first,copy_end-first);
        memset(stable+copy_end,0,last-copy_end);
    }
    if (prepared->closed) {
        uint64_t previous=__atomic_fetch_and(&entry->writes,~CACHE_WRITES_CLOSED,__ATOMIC_RELEASE);
        if (previous!=CACHE_WRITES_CLOSED) __builtin_trap();
        cache_wake(&entry->ready);
    }
    entry->retry_blocked=0;prepared->closed=0;
}
static int submit_writeback_spans(struct kernel_vfs_node *node,struct file_write_span *spans,
    const uint8_t *owners,size_t count,struct prepared_writeback *prepared)
{
    int result=kernel_vfs_node_writeback(node,spans,count);
    for (size_t i=0;i<count;i++) {
        if (spans[i].written>spans[i].size || (!result && spans[i].written!=spans[i].size)) __builtin_trap();
        prepared[owners[i]].written+=spans[i].written;
    }
    return result;
}

static void reclaim_window_memory(struct kernel_page_cache *cache)
{
    struct kernel_page_cache_record *r=cache->record;
    uint64_t available=physical_page_available(cache->allocator);
    /* 每批在取得inode/后端及稳定版本之前回收干净页，归还本批借用后
     * 也恢复余量，避免连续窗口只累积元数据与稳定版本的分配压力。 */
    if (available<r->high) (void)kernel_page_cache_reclaim(cache,r->high-available);
}

/* Candidates are already pinned. Each selected page supplies a separate stable
 * version, so the batch never requires physically contiguous snapshot storage. */
static int writeback_run(struct kernel_page_cache *cache,
    struct kernel_page_cache_entry **entries,size_t count,uint64_t limit,size_t *used,
    struct writeback_workspace *workspace)
{
    if (!count || count>BOAROS_PAGE_CACHE_WRITEBACK_PAGES) __builtin_trap();
    reclaim_window_memory(cache);
    /* Progress I/O (including loop) is exempt from admission throttling, but
     * that says nothing about ownership of this cache worker's workspace. */
    if (workspace && workspace==cache->record->writeback_workspace) {
        if (!kernel_page_cache_is_executor(cache)) __builtin_trap();
        if (count>cache->record->writeback_capacity) count=cache->record->writeback_capacity;
    }
    size_t span_capacity=workspace?BOAROS_PAGE_CACHE_WRITEBACK_PAGES:1;
    uint8_t span_owners[BOAROS_PAGE_CACHE_WRITEBACK_PAGES];
    struct prepared_writeback single_prepared;
    struct file_write_span single_span;
    struct writeback_workspace local={.prepared=&single_prepared,.spans=&single_span};
    if (!workspace) {
        if (count!=1) __builtin_trap();
        workspace=&local;
    }
    struct prepared_writeback *prepared=workspace->prepared;
    struct file_write_span *spans=workspace->spans;
    for (size_t first=0;first<count;) {
        memset(prepared,0,(count-first)*sizeof(*prepared));
        memset(spans,0,(count-first)*sizeof(*spans));
        size_t ready=0,done=0;
        int result=0;
        for (;done<count-first;done++) {
            int r=prepare_writeback(cache,entries[first+done],limit,!done,&prepared[done]);
            if (r) { result=r; break; }
        }
        int prepare_error=result;
        KERNEL_LOCK_SCOPE(io);
        if (done) kernel_vfs_node_lock(entries[first]->node,&io,0);
        for (size_t i=0;i<done;i++)
            if (prepared[i].version) capture_writeback(cache,entries[first+i],&prepared[i],limit);
        int submitted=0;
        for (size_t i=0;i<done && !submitted;i++) {
            if (!prepared[i].version) continue;
            size_t cursor=prepared[i].begin;
            while (next_writeback_span(entries[first+i],&prepared[i],&cursor,&spans[ready])) {
                span_owners[ready++]=(uint8_t)i;
                if (ready==span_capacity) {
                    submitted=submit_writeback_spans(entries[first]->node,spans,span_owners,ready,prepared);
                    ready=0;
                    if (submitted) break;
                }
            }
        }
        if (ready && !submitted)
            submitted=submit_writeback_spans(entries[first]->node,spans,span_owners,ready,prepared);
        if (submitted) result=submitted;
        /* 后端已结束实际访问；完成发布只依赖entry借用和缓存保护。 */
        if (io.lock) kernel_lock_release(&io);
        {
            CACHE_SCOPE(guard,cache);
            for (size_t i=done;i>0;i--) {
                struct prepared_writeback *p=&prepared[i-1];
                if (!p->version) continue;
                int r=complete_writeback(cache,entries[first+i-1],p,submitted);
                if (r && !result) result=r;
            }
        }
        /* 完成状态和entry借用按批发布；稳定版本的实际归还仍在保护外。 */
        for (size_t i=done;i>0;i--)
            if (prepared[i-1].version) version_release(prepared[i-1].version);
        /* Publish a prepared prefix before waiting for another stable owner.
         * This ends its entry qualifications, allowing a foreground writer
         * that borrowed the last version to finish and return that version. */
        if (prepare_error==-KERNEL_ENOMEM && done && !submitted && result==prepare_error) {
            first+=done;continue;
        }
        *used=first+(done?done:1);
        return result;
    }
    __builtin_unreachable();
}


struct write_admission_request {
    struct kernel_page_cache *cache;
    struct kernel_vfs_node *node;
#if BOAROS_PAGE_CACHE_BACKGROUND_PERCENT < 100
    struct kernel_vfs_node *progress;
#endif
    int result;
};
static void write_admission_window(void *argument)
{
    struct write_admission_request *request=argument;
    struct kernel_page_cache *cache=request->cache;
    struct kernel_page_cache_record *r=cache->record;
    /* 有界接纳工作统一由本cache执行者推进，独占其挂载期预留工作区；
     * 内存紧张时不再为了释放脏页而额外分配一份前景控制区。 */
    if (!kernel_page_cache_is_executor(cache)) __builtin_trap();
    struct writeback_workspace *workspace=r->writeback_workspace;
    struct kernel_page_cache_entry *selected[BOAROS_PAGE_CACHE_WRITEBACK_PAGES];
    size_t count=0;
    {
        CACHE_SCOPE(guard,cache);
        struct kernel_page_cache_dirty *dirty=r->dirty_head;
        struct kernel_page_cache_entry *entry=NULL;
        while (dirty) {
            entry=RB_MIN(kernel_page_cache_dirty_tree,&dirty->tree);
            if (entry && entry->node->instance->writeback.domain==request->node->instance->writeback.domain) break;
            dirty=dirty->queue_next;
        }
        if (dirty) {
            while (entry && count<r->writeback_capacity && count<BOAROS_PAGE_CACHE_WRITEBACK_PAGES) {
                selected[count++]=entry;entry_borrow(entry);
                entry=RB_NEXT(kernel_page_cache_dirty_tree,&dirty->tree,entry);
            }
        }
    }
    if (count) {
        size_t used;
        int error=writeback_run(cache,selected,count,UINT64_MAX,&used,workspace);
        /* 写回已把错误记在实际inode；不能把A的空间/I/O错误归给B的覆盖写。
         * 失败队首仍保留脏owner，但移到队尾，让后面的可回收数据取得进展。 */
        if (selected[0]->node==request->node && error!=-KERNEL_EBUSY && error!=-KERNEL_ENOMEM)
            request->result=error;
#if BOAROS_PAGE_CACHE_BACKGROUND_PERCENT < 100
        if (!error && selected[0]->node->instance->ops->writeback_reclaim) {
            request->progress=selected[0]->node;
            if (kernel_vfs_node_acquire(request->progress)) __builtin_trap();
        }
#endif
        CACHE_SCOPE(guard,cache);
        struct kernel_page_cache_dirty *dirty=kernel_vfs_node_dirty_pages(selected[0]->node);
        if (error && dirty->queue_next) {
            if (dirty->queue_previous) dirty->queue_previous->queue_next=dirty->queue_next;
            else r->dirty_head=dirty->queue_next;
            dirty->queue_next->queue_previous=dirty->queue_previous;
            dirty->queue_previous=r->dirty_tail;dirty->queue_next=NULL;
            r->dirty_tail->queue_next=dirty;r->dirty_tail=dirty;
        }
        for (size_t i=0;i<count;i++) entry_return(selected[i]);
    }
    reclaim_window_memory(cache);
}
static int write_admission(struct kernel_page_cache *cache,struct kernel_vfs_node *node)
{
    struct kernel_page_cache_record *r=cache->record;
    if (!r->started || r->stopping) return 0;
    uint64_t available=physical_page_available(cache->allocator);
    if (available>r->low && !node_pressure(node,r,0)) return 0;
    /* 写入接纳处尚无inode锁、来源pin或cache写借用。不能等到分配器
     * 持有重入保护才等待脏页；executor也不能排队等待自己的后台工作。 */
    struct kernel_io_context *io=kernel_io_context_current();
    if (io->allocation_depth || io->backend_depth || io->reclaim_depth ||
        io->background_reclaim || io->user_pins || io->cache_writes ||
        kernel_cpu_current()->raw_locks) return 0;
    for (struct kernel_lock_guard *guard=io->locks;guard;guard=guard->previous)
        if (guard->lock->rank>15) return 0;
    if (available<r->high) (void)kernel_page_cache_reclaim(cache,r->high-available);
    struct write_admission_request request={.cache=cache,.node=node};
    /* 这里只借用缓存owner与调用栈；worker不取得调用者的OFD/写操作锁。
     * 保留整次write互斥，仍允许独立worker取得inode后推进一批写回。 */
    if (!kernel_page_cache_is_executor(cache)) {
        if (run_request(cache,write_admission_window,&request,1,NULL)) request.result=-KERNEL_EIO;
    } else write_admission_window(&request);
#if BOAROS_PAGE_CACHE_BACKGROUND_PERCENT < 100
    /* 一批接纳可能只把dirty变成version；等固定日志目标归还真实占用，
     * 此时来源pin、entry和inode资格都已归还，进度执行者不进入此等待。 */
    struct kernel_vfs_node *progress=request.progress?request.progress:node;
    if (!request.result && node_pressure(node,r,0) && progress->instance->ops->writeback_reclaim) {
        int error=progress->instance->ops->writeback_reclaim(progress->instance);
        /* 共享物理额度不把另一挂载的日志错误转嫁给本次调用者。 */
        if (progress->instance==node->instance) request.result=error;
    }
    if (request.progress && kernel_vfs_node_release(&request.progress)) __builtin_trap();
#endif
    available=physical_page_available(cache->allocator);
    if (available<r->high) (void)kernel_page_cache_reclaim(cache,r->high-available);
    return request.result;
}



struct foreground_writeback {
    struct kernel_page_cache *cache;
    struct kernel_vfs_node *node;
    uint64_t cursor,last,end;
    struct kernel_page_cache_entry **selected;
    struct writeback_workspace *workspace;
    size_t capacity;
    int result,done;
};

static void writeback_window(void *argument)
{
    struct foreground_writeback *request=argument;
    struct kernel_page_cache *cache=request->cache;
    size_t count=0,used;
    {
        CACHE_SCOPE(guard,cache);
        struct kernel_page_cache_entry *entry=dirty_at_or_after(request->node,request->cursor);
        while (entry && entry->page_index<=request->last && count<request->capacity) {
            request->selected[count++]=entry;
            entry_borrow(entry); COST_ADD(WRITEBACK_VISITS,1);
            entry=RB_NEXT(kernel_page_cache_dirty_tree,&kernel_vfs_node_dirty_pages(request->node)->tree,entry);
        }
    }
    if (!count) { request->done=1; return; }
    /* 窗口只借用本批条目；下一次提交前重查索引，截断可退休其余页。 */
    request->cursor=request->selected[count-1]->page_index+1;
    request->result=writeback_run(cache,request->selected,count,request->end,&used,request->workspace);
    {
        CACHE_SCOPE(guard,cache);
        for (size_t i=0;i<count;i++) entry_return(request->selected[i]);
    }
    reclaim_window_memory(cache);
    request->done=request->result || request->cursor>request->last;
}

static void cache_yield(void);

int kernel_page_cache_writeback_range_borrowing(struct kernel_page_cache *cache,
    struct kernel_vfs_node *node,uint64_t start,uint64_t end,
    const struct kernel_lock_guard *borrowed)
{
    if (!cache_live(cache) || !node || start>=end) return -KERNEL_EINVAL;
    if (kernel_lock_held(&node->io_lock,0)) __builtin_trap();
    uint64_t cursor=start>>BOAROS_PAGE_SHIFT,last=(end-1)>>BOAROS_PAGE_SHIFT;
    struct kernel_page_cache_entry *single=NULL;
    int multiple=0;
    {
        CACHE_SCOPE(guard,cache);
        struct kernel_page_cache_entry *entry=dirty_at_or_after(node,cursor);
        if (!entry || entry->page_index>last) return 0;
        struct kernel_page_cache_entry *tail=RB_MAX(kernel_page_cache_dirty_tree,&kernel_vfs_node_dirty_pages(node)->tree);
        if (last>tail->page_index) last=tail->page_index;
        struct kernel_page_cache_entry *next=RB_NEXT(kernel_page_cache_dirty_tree,&kernel_vfs_node_dirty_pages(node)->tree,entry);
        multiple=next && next->page_index<=last;
    }
    reclaim_window_memory(cache);
    union foreground_workspace *lease=multiple?foreground_workspace_take(cache):NULL;
    if (multiple && !lease) return -KERNEL_ENOMEM;
    struct writeback_workspace work=multiple?(struct writeback_workspace){
        .prepared=lease->writeback.prepared,.spans=lease->writeback.spans,
        .selected=lease->writeback.selected}:(struct writeback_workspace){0};
    struct foreground_writeback request={.cache=cache,.node=node,.cursor=cursor,.last=last,
        .end=end,.selected=multiple?work.selected:&single,.workspace=multiple?&work:NULL,
        .capacity=multiple?BOAROS_PAGE_CACHE_WRITEBACK_PAGES:1};
    int dispatch=kernel_page_cache_needs_cpu0(cache);
    if (dispatch) {
        if (run_cpu0_until(cache,writeback_window,&request,borrowed,&request.done))
            request.result=-KERNEL_EIO;
    } else {
        while (!request.done) {
            writeback_window(&request);
            if (!request.done && kernel_scheduler_can_sleep()) {
                /* 此调用者不占执行队列；批末允许IRQ抢占，不逐窗强制轮转。
                 * restore只负责重新开启，须先save关回IRQ再恢复原状态。 */
                uintptr_t irq=arch_interrupt_save();
                arch_interrupt_restore(ARCH_INTERRUPT_ENABLE_MASK);
                (void)arch_interrupt_save();
                arch_interrupt_restore(irq);
            }
        }
    }
    if (lease) foreground_workspace_put(cache,lease);
    return request.result;
}

int kernel_page_cache_writeback_range(struct kernel_page_cache *cache,
    struct kernel_vfs_node *node,uint64_t start,uint64_t end)
{ return kernel_page_cache_writeback_range_borrowing(cache,node,start,end,NULL); }

int kernel_page_cache_writeback(struct kernel_page_cache *cache, struct kernel_vfs_node *node)
{ return kernel_page_cache_writeback_range(cache, node, 0, UINT64_MAX); }

int kernel_page_cache_resize_prepare(struct kernel_page_cache *cache,struct kernel_vfs_node *node,
    uint64_t old_size,uint64_t target,struct kernel_page_cache_resize *token)
{
    if (!token || token->entry || !cache_live(cache)) return -KERNEL_EINVAL;
    uint64_t boundary=old_size<target?old_size:target;
    if (old_size==target || !(boundary&BOAROS_PAGE_MASK)) return 0;
    if (kernel_lock_held(&node->io_lock,0) || kernel_cpu_current()->raw_locks) __builtin_trap();
    struct kernel_page_cache_entry *entry;
    {
        CACHE_SCOPE(guard,cache); int found;
        size_t index=find_bucket(cache->record,node,boundary>>BOAROS_PAGE_SHIFT,&found);
        if (!found) return 0;
        entry=cache->record->buckets[index]; entry_borrow(entry);
    }
    for (struct kernel_page_cache_write_borrow *b=kernel_io_context_current()->cache_writes;b;b=b->next)
        if (b->entry==entry) { CACHE_SCOPE(guard,cache); entry_return(entry); return -KERNEL_EBUSY; }
    struct kernel_page_cache_alias *aliases=NULL;
    for (;;) {
        struct kernel_wait_token wait={0};
        int park;
        {
            CACHE_SCOPE(guard,cache);
            if (!entry->writeback && !entry_loading(entry) && !(__atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)&CACHE_WRITES_CLOSED)) {
                __atomic_fetch_or(&entry->writes,CACHE_WRITES_CLOSED,__ATOMIC_ACQ_REL);
                aliases=entry->aliases;alias_borrow(aliases);break;
            }
            if (kernel_wait_prepare(&entry->ready,0,0,&wait)) __builtin_trap();
            park = entry->writeback || entry_loading(entry) ||
                (__atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)&CACHE_WRITES_CLOSED);
        }
        enum kernel_wait_wake_reason reason;
        if (park && kernel_wait_park(&wait,&reason)) __builtin_trap();
        if (kernel_wait_finish(&wait)) __builtin_trap();
    }
    int result=rearm_aliases(entry,aliases);
    if (result) {
        CACHE_SCOPE(guard,cache);
        __atomic_fetch_and(&entry->writes,~CACHE_WRITES_CLOSED,__ATOMIC_RELEASE);
        entry_return(entry); cache_wake(&entry->ready); return result;
    }
    for (;;) {
        struct kernel_wait_token wait={0};
        {
            CACHE_SCOPE(guard,cache);
            if (!(__atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)&UINT32_MAX)) break;
            if (kernel_wait_prepare(&entry->ready,0,0,&wait)) __builtin_trap();
        }
        enum kernel_wait_wake_reason reason;
        if (kernel_wait_park(&wait,&reason) || kernel_wait_finish(&wait)) __builtin_trap();
    }
    token->entry=entry; token->old_size=old_size;
    return 0;
}
static void resize_apply(struct kernel_page_cache_resize *token,uint64_t actual)
{
    if (!token || !token->entry) return;
    struct kernel_page_cache_entry *entry=token->entry;
    struct kernel_page_cache *cache=entry->cache;
    uint64_t boundary=token->old_size<actual?token->old_size:actual;
    if (actual!=token->old_size && entry->page_index==(boundary>>BOAROS_PAGE_SHIFT)) {
        void *page;
        if (physical_page_map_owned(cache->allocator,entry->physical_address,&page)) __builtin_trap();
        size_t begin=boundary&BOAROS_PAGE_MASK;
        size_t end=actual<token->old_size?BOAROS_PAGE_SIZE:
            actual-boundary<BOAROS_PAGE_SIZE-begin?begin+(size_t)(actual-boundary):BOAROS_PAGE_SIZE;
        memset((unsigned char *)page+begin,0,end-begin);
        CACHE_SCOPE(guard,cache);
        if (actual<token->old_size && entry->dirty_end>begin) entry->dirty_end=begin;
        if (entry->dirty_end && entry->dirty_begin>=entry->dirty_end) dirty_clear(entry);
        entry->generation++;
    }
}
void kernel_page_cache_resize_finish(struct kernel_page_cache_resize *token,uint64_t actual)
{
    if (!token || !token->entry) return;
    resize_apply(token,actual);
    struct kernel_page_cache_entry *entry=token->entry;
    struct kernel_page_cache *cache=entry->cache;
    {
        CACHE_SCOPE(guard,cache);
        if (entry->retry_blocked) { entry->retry_blocked=0; cache->record->requested=1; cache_wake(&cache->record->work); }
        if (__atomic_exchange_n(&entry->writes,0,__ATOMIC_RELEASE)!=CACHE_WRITES_CLOSED) __builtin_trap();
        entry_return(entry); cache_wake(&entry->ready);
    }
    *token=(struct kernel_page_cache_resize){0};
}





void kernel_page_cache_truncate(struct kernel_page_cache *cache,
    struct kernel_vfs_node *node, uint64_t size)
{
    if (cache_live(cache)) readahead_cancel_node(cache, node);
    {
        {
            CACHE_SCOPE(guard,cache);
            struct kernel_page_cache_entry *entry=*kernel_vfs_node_cache_pages(node);
            while (entry) {
                struct kernel_page_cache_entry *next=entry->node_next;
                if ((entry->page_index<<BOAROS_PAGE_SHIFT)>=size) {
                    /* 后端实际尺寸已确定；退休不回收尚在排空pin的
                     * writeback owner，完成者会看到retired并放弃旧提交。 */
                    remove_entry(cache,entry);
                    cache_wake(&entry->ready);
                }
                entry=next;
            }
        }
        uint64_t released=0; (void)drain_entries(cache,&released);
        return;
    }
}

void kernel_page_cache_invalidate_range(struct kernel_page_cache *cache,
    struct kernel_vfs_node *node, uint64_t start, uint64_t end)
{
    if (!cache_live(cache) || start >= end || !kernel_lock_held(&node->io_lock,1)) __builtin_trap();
    readahead_cancel_node(cache,node);
    uint64_t first = start >> BOAROS_PAGE_SHIFT, last = (end - 1) >> BOAROS_PAGE_SHIFT;
    {
        CACHE_SCOPE(guard,cache);
        if (last-first<32) {
            /* A small hole uses the existing page index, independent of how
             * many unrelated pages of this inode remain cached. */
            for (uint64_t index=first;index<=last;index++) {
                int found;size_t bucket=find_bucket(cache->record,node,index,&found);
                if (!found) continue;
                struct kernel_page_cache_entry *entry=cache->record->buckets[bucket];
                if (entry->dirty_end) __builtin_trap();
                remove_entry(cache,entry);cache_wake(&entry->ready);
            }
        } else {
            struct kernel_page_cache_entry *entry = *kernel_vfs_node_cache_pages(node);
            while (entry) {
                struct kernel_page_cache_entry *next = entry->node_next;
                if (entry->page_index >= first && entry->page_index <= last) {
                    /* A queued snapshot observes retirement before capture. */
                    if (entry->dirty_end) __builtin_trap();
                    remove_entry(cache,entry);cache_wake(&entry->ready);
                }
                entry = next;
            }
        }
    }
    uint64_t released = 0;
    (void)drain_entries(cache,&released);
}

uint64_t kernel_page_cache_reclaim(struct kernel_page_cache *cache,uint64_t target_pages)
{
    if (!cache_live(cache) || !target_pages) return 0;
    uint64_t released=version_pool_reclaim(cache->record->versions,target_pages);
    if (released>=target_pages) return released;
    struct kernel_lock_guard *held=kernel_io_context_current()->locks;
    if (held && held->lock->rank>=cache->record->metadata.lock.rank) return released;
    struct kernel_page_cache_record *r=cache->record;
    uint64_t scanned=0,limit;
    {
        CACHE_SCOPE(guard,cache);
        r->statistics.reclaim_calls++;
        limit=r->occupied;
    }
    while (scanned<limit && released<target_pages) {
        struct kernel_page_cache_entry *retired=NULL;
        uint64_t selected=0;
        {
            CACHE_SCOPE(guard,cache);
            if (!r->reclaim_cursor) r->reclaim_cursor=r->lru_tail;
            unsigned window=0;
            while (r->reclaim_cursor && released+selected<target_pages && scanned<limit && window<64) {
                struct kernel_page_cache_entry *entry=r->reclaim_cursor;
                r->reclaim_cursor=entry->lru_previous;scanned++;window++;
                uint32_t references;
                if (!entry->dirty_end && !entry_loading(entry) && !entry->writeback && !entry->users &&
                    !entry->aliases && !__atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE) &&
                    !physical_page_reference_count(cache->allocator,entry->physical_address,&references) && references==1) {
                    entry_borrow(entry);remove_entry(cache,entry);
                    entry->node_next=retired;retired=entry;selected++;
                }
            }
            if (!window) break;
        }
        /* Physical owners remain borrowed while the cache lock is absent.
         * A lock window is bounded; allocation still scans a complete round. */
        for (struct kernel_page_cache_entry *entry=retired;entry;entry=entry->node_next) {
            if (physical_page_release(cache->allocator,entry->physical_address)) __builtin_trap();
            released++;
        }
        if (retired) {
            CACHE_SCOPE(guard,cache);
            while (retired) {
                struct kernel_page_cache_entry *entry=retired;retired=entry->node_next;
                entry->physical_address=0;entry->page_references_owned=0;entry->node_next=NULL;
                entry_return(entry);
            }
        }
    }
    { CACHE_SCOPE(guard,cache);
      cache->record->statistics.pages_reclaimed+=released;
      if (cache->record->started && cache->record->cleanup_entries) {
          cache->record->requested=1;cache_wake(&cache->record->work);
      }
    }
    if (!remote_cache_client() && !kernel_lock_held(&cache->record->group->walk.lock,1)) { uint64_t extra=0;(void)drain_entries(cache,&extra); }
    return released;
}

static void group_notify(void *context)
{
    struct page_cache_group *g=context;
    if (kernel_io_context_current()->background_reclaim) return;
    uint64_t available=physical_page_available(g->allocator),dirty=group_dirty(g);
    KERNEL_RAW_SCOPE(guard,&g->metadata);
    for (struct kernel_page_cache_record *r=g->head;r;r=r->next) {
        if (!r->started || r->stopping || (available>r->low && (g->domains || dirty<background_limit(r->dirty_high)))) continue;
        r->requested=1;cache_wake(&r->work);
    }
    for (struct kernel_writeback_domain *domain=g->domains;domain;domain=domain->next) {
        if (!__atomic_load_n(&domain->dirty_bytes,__ATOMIC_RELAXED) ||
            domain_bytes(domain)<background_limit(domain_limit(domain,0))) continue;
        for (struct kernel_writeback_owner *owner=domain->members;owner;owner=owner->next) {
            struct kernel_page_cache_record *r=owner->cache->record;
            if (!r->started || r->stopping) continue;
            r->requested=1;cache_wake(&r->work);
        }
    }
}

void kernel_page_cache_notify_pressure(struct kernel_page_cache *cache)
{
    if (cache_live(cache) && cache->record->group) group_notify(cache->record->group);
}
enum kernel_page_cache_status kernel_page_cache_request_writeback(struct kernel_page_cache *cache)
{
    kernel_assert_service_context();
    if (!cache_live(cache)) return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    CACHE_SCOPE(guard,cache);
    if (!cache->record->started || cache->record->stopping) return KERNEL_PAGE_CACHE_STATUS_STATE;
    cache->record->requested=1; cache->record->force_writeback=1;
    cache_wake(&cache->record->work);
    return KERNEL_PAGE_CACHE_STATUS_OK;
}

static void pressure_wait(void *context)
{
    struct page_cache_group *g=context;
    struct kernel_io_context *io=kernel_io_context_current();
    if (io->locks || io->allocation_depth || io->backend_depth || io->reclaim_depth ||
        io->background_reclaim || !kernel_scheduler_can_sleep() || arch_external_interrupt_active()) return;
    uint64_t round,progressed;
    {
        KERNEL_RAW_SCOPE(guard,&g->metadata);
        progressed=g->progressed;
        if (!g->pending) {
            if (!++g->round) __builtin_trap();
            for (struct kernel_page_cache_record *r=g->head;r;r=r->next) {
                if (!r->started || r->stopping) continue;
                r->wait_round=g->round;g->pending++;r->requested=1;cache_wake(&r->work);
            }
        }
        round=g->round;
    }
    for (;;) {
        struct kernel_wait_token token={0};
        {
            KERNEL_RAW_SCOPE(guard,&g->metadata);
            if (!g->pending || round!=g->round || progressed!=g->progressed) break;
            if (kernel_wait_prepare(&g->progress,0,0,&token)) __builtin_trap();
        }
        enum kernel_wait_wake_reason reason;
        if (kernel_wait_park(&token,&reason) || kernel_wait_finish(&token)) __builtin_trap();
    }
}

/* 单个worker不能等待自己尚未执行的冷读；忙写回由真实结束事件重试。 */
static void cache_yield(void)
{
    KERNEL_IRQ_SCOPE(irq);
    if (kernel_scheduler_yield_current()!=KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
}
static void cache_background(struct kernel_page_cache *cache)
{
    struct kernel_page_cache_record *r=cache->record;
    unsigned force;
    { CACHE_SCOPE(guard,cache); force=r->force_writeback;
#if BOAROS_PAGE_CACHE_DIRTY_AGE
      force|=r->expired_writeback;
#endif
    }
    struct kernel_io_context *io=kernel_io_context_current();
    io->background_reclaim=1;
    uint64_t before=physical_page_available(cache->allocator);
    if (before<r->high) (void)kernel_page_cache_reclaim(cache,r->high-before);
    { CACHE_SCOPE(guard,cache); dirty_round_begin(r); }
    int interrupted = 0;
    for (;;) {
        struct kernel_page_cache_entry *batch[BOAROS_PAGE_CACHE_WRITEBACK_PAGES];
        size_t count;
        {
            CACHE_SCOPE(guard,cache);
            if (r->stopping) break;
            count = dirty_batch(r,batch,force || physical_page_available(cache->allocator)<=r->low);
            if (!count && (!r->dirty_cursor || !r->dirty_remaining)) break;
        }
        if (count) {
            size_t used;
            int result=writeback_run(cache,batch,count,UINT64_MAX,&used,r->writeback_workspace);
            {
                CACHE_SCOPE(guard,cache);
                if (!result) r->statistics.worker_written+=used;
                else if (result!=-KERNEL_EBUSY && result!=-KERNEL_ENOMEM) r->statistics.worker_failed+=used;
                for (size_t i=0;i<count;i++) entry_return(batch[i]);
            }
            reclaim_window_memory(cache);
        }
        int demands;
        { CACHE_SCOPE(guard,cache); demands=r->reads_head!=0; r->statistics.worker_batches++; }
        if (demands) {
            CACHE_SCOPE(guard,cache); r->requested=1; interrupted=1;
            break;
        }
        cache_yield();
    }
    uint64_t available=physical_page_available(cache->allocator);
    if (available<r->high) (void)kernel_page_cache_reclaim(cache,r->high-available);
    io->background_reclaim=0;
    {
        CACHE_SCOPE(guard,cache);
        if (!interrupted) {
            r->dirty_cursor=NULL;r->dirty_remaining=0;
#if BOAROS_PAGE_CACHE_DIRTY_AGE
            if (r->expired_writeback) {
                r->expired_writeback=0;r->dirty_deadline=r->dirty_pages?dirty_deadline():0;
            }
#endif
        }
        if (!r->dirty_pages) r->force_writeback=0;
        r->completed++; cache_wake(&r->progress);
    }
    group_complete(r,physical_page_available(cache->allocator)>before);
}
static void page_cache_worker(void *argument)
{
    struct kernel_page_cache *cache=argument;
    struct kernel_page_cache_record *r = cache->record;
    __atomic_store_n(&r->executor_io,kernel_io_context_current(),__ATOMIC_RELEASE);
    unsigned reads=0;
    struct kernel_page_cache_entry *cleanup_entries=NULL;
    for (;;) {
        struct cache_request *request = 0;
        struct kernel_wait_token token = {0};
        int cleanup = 0, stopping=0;
        {
            CACHE_SCOPE(check, cache);
            stopping=r->stopping;
#if BOAROS_PAGE_CACHE_DIRTY_AGE
            if (!stopping && r->dirty_deadline && kernel_time_monotonic_ns()>=r->dirty_deadline) {
                r->dirty_deadline=0;r->expired_writeback=1;r->requested=1;
            }
#endif
            if (r->requested && (reads>=16 || !r->reads_head)) {
                r->requested=0; cleanup=1; reads=0;
            } else if (r->reads_head) {
                request = r->reads_head; r->reads_head = request->next;
                if (!r->reads_head) r->reads_tail = &r->reads_head;
                r->reads_running++; reads++;
            } else if (cleanup_entries) cleanup=2;
            else if (r->stopping) break;
            else {
                uint64_t deadline=0;
#if BOAROS_PAGE_CACHE_DIRTY_AGE
                if (r->dirty_deadline) {
                    enum kernel_time_status status=kernel_time_deadline_from_monotonic(r->dirty_deadline,&deadline);
                    if (status==KERNEL_TIME_STATUS_DEADLINE_PASSED) {
                        r->dirty_deadline=0;r->expired_writeback=1;r->requested=1;continue;
                    }
                    if (status!=KERNEL_TIME_STATUS_OK) __builtin_trap();
                }
#endif
                if (kernel_wait_prepare(&r->work,deadline,0,&token)) __builtin_trap();
            }
        }
        if (!request) {
            if (cleanup) {
                if (!cleanup_entries) take_cleanup_entries(cache,&cleanup_entries);
                uint64_t released=0;
                (void)drain_cleanup_entries(cache,&cleanup_entries,16,&released);
                if (cleanup==1 && !stopping) cache_background(cache);
                cache_yield();
                continue;
            }
            enum kernel_wait_wake_reason reason;
            if (kernel_wait_park(&token, &reason) || kernel_wait_finish(&token)) __builtin_trap();
            continue;
        }
        enum kernel_page_cache_status result = KERNEL_PAGE_CACHE_STATUS_STATE;
        uint64_t started=request->observed?kernel_time_monotonic_ns():0;
        if (!stopping) {
            kernel_assert_service_context();
            struct kernel_io_context *io = kernel_io_context_current();
            if (io->account_task) __builtin_trap();
            io->account_task = request->account_task;
            request->execute(request->argument);
            io->account_task = NULL;
            result=KERNEL_PAGE_CACHE_STATUS_OK;
        }
        uint64_t finished=request->observed?kernel_time_monotonic_ns():0;
        {
            CACHE_SCOPE(complete, cache);
            if (!r->read_requests || !r->reads_running || request->done) __builtin_trap();
            r->reads_running--;
            if (request->observed) {
                uint64_t queued=started-request->queued_at,elapsed=finished-started;
                r->queue_ns+=queued; r->execute_ns+=elapsed;
                if (queued>r->max_queue_ns) r->max_queue_ns=queued;
                if (elapsed>r->max_execute_ns) r->max_execute_ns=elapsed;
            }
            if (!stopping && !r->stopping && request->finished && !*request->finished) {
                /* 同一请求只推进一个窗口；续批排到现有队尾，不唤醒caller重提。 */
                request->next=NULL;
                request->queued_at=finished;
                if (!r->reads_head) r->reads_tail=&r->reads_head;
                *r->reads_tail=request; r->reads_tail=&request->next;
            } else {
                if (r->stopping && request->finished && !*request->finished)
                    result=KERNEL_PAGE_CACHE_STATUS_STATE;
                r->read_requests--; r->requests_completed++;
                if (request->observed) r->executions_completed++;
                request->status = result; request->done = 1;
                cache_wake(&request->changed);
            }
        }
        /* 剩余退休owner随worker存活；每次最多释放16项，再接下一条前景请求。
         * 尚有借用的失败项归还缓存，等实际归还事件，不在本轮反复重试。 */
        if (!cleanup_entries) take_cleanup_entries(cache,&cleanup_entries);
        uint64_t released=0;
        (void)drain_cleanup_entries(cache,&cleanup_entries,16,&released);
        cache_yield();
    }
    group_complete(r,0);
    __atomic_store_n(&r->executor_io,0,__ATOMIC_RELEASE);
}



int kernel_page_cache_start_worker(struct kernel_page_cache *cache)
{
    if (!cache_live(cache) || cache->record->started) return -KERNEL_EINVAL;
    struct kernel_page_cache_record *r = cache->record;
    if (kernel_heap_allocate(cache->heap,sizeof(*r->writeback_workspace),
        (void **)&r->writeback_workspace)) return -KERNEL_ENOMEM;
    if (writeback_workspace_init(cache,r->writeback_workspace)) {
        kernel_heap_release(cache->heap,r->writeback_workspace);
        r->writeback_workspace=NULL; return -KERNEL_ENOMEM;
    }
    r->writeback_capacity=version_pool_reserve(r->versions);
    if (!r->writeback_capacity) {
        version_pool_unreserve(r->versions);
        writeback_workspace_release(cache,r->writeback_workspace);
        kernel_heap_release(cache->heap,r->writeback_workspace);
        r->writeback_workspace=NULL; return -KERNEL_ENOMEM;
    }
    uint64_t total = physical_page_total(cache->allocator);
    r->low = total / 50 ? total / 50 : 1;
    r->high = total / 25 > r->low ? total / 25 : r->low + 1;
    r->dirty_low = total / 20 ? total / 20 : 1;
    r->dirty_high = total / 10 > r->dirty_low ? total / 10 : r->dirty_low + 1;
    r->stopping = 0;
#if BOAROS_PAGE_CACHE_DIRTY_AGE
    if (r->expired_writeback) {r->expired_writeback=0;r->dirty_deadline=r->dirty_pages?dirty_deadline():0;}
#endif
    enum kernel_scheduler_status result = kernel_thread_create_joinable(page_cache_worker, cache, &r->worker);
    if (result != KERNEL_SCHEDULER_STATUS_OK) {
        version_pool_unreserve(r->versions);r->writeback_capacity=0;
        writeback_workspace_release(cache,r->writeback_workspace);
        kernel_heap_release(cache->heap,r->writeback_workspace);
        r->writeback_workspace=NULL;
        return result == KERNEL_SCHEDULER_STATUS_NO_MEMORY ? -KERNEL_ENOMEM : -KERNEL_EINVAL;
    }
    { CACHE_SCOPE(publish,cache); r->dirty_cursor=NULL; r->dirty_remaining=0; r->started = 1; }
#if BOAROS_PAGE_CACHE_READAHEAD_PAGES
    kernel_wait_queue_init(&r->readahead_work);
    result = kernel_thread_create_joinable(readahead_worker, cache, &r->readahead_worker);
    if (result != KERNEL_SCHEDULER_STATUS_OK) {
        kernel_page_cache_stop_worker(cache);
        return result == KERNEL_SCHEDULER_STATUS_NO_MEMORY ? -KERNEL_ENOMEM : -KERNEL_EINVAL;
    }
    { CACHE_SCOPE(publish, cache); r->readahead_started = 1; }
#endif
    kernel_page_cache_notify_pressure(cache);
    return 0;
}

void kernel_page_cache_stop_worker(struct kernel_page_cache *cache)
{
    if (!cache_live(cache) || !cache->record->started) return;
    struct kernel_page_cache_record *r = cache->record;
    { CACHE_SCOPE(stop, cache); r->stopping = 1; r->dirty_cursor=NULL; r->dirty_remaining=0; }
    version_pool_stop(r->versions);
    {
        for (;;) {
            CACHE_SCOPE(cancel,cache);
            for (unsigned count=0;r->reads_head && count<16;count++) {
                struct cache_request *request=r->reads_head;
                r->reads_head=request->next;
                if (!r->reads_head) r->reads_tail=&r->reads_head;
                if (!r->read_requests || request->done) __builtin_trap();
                r->read_requests--; r->requests_completed++;
                if (request->observed) r->executions_completed++;
                request->status=KERNEL_PAGE_CACHE_STATUS_STATE; request->done=1;
                cache_wake(&request->changed);
            }
            if (!r->reads_head) break;
        }
    }
#if BOAROS_PAGE_CACHE_READAHEAD_PAGES
    readahead_cancel_node(cache, 0);
    if (r->readahead_started) {
        cache_wake(&r->readahead_work);
        kernel_thread_join(&r->readahead_worker);
        { CACHE_SCOPE(stop, cache); r->readahead_started = 0; }
    }
#endif
    cache_wake(&r->work);
    cache_wake(&r->progress);
    kernel_thread_join(&r->worker);
    version_pool_unreserve(r->versions);r->writeback_capacity=0;
    writeback_workspace_release(cache,r->writeback_workspace);
    kernel_heap_release(cache->heap,r->writeback_workspace);
    r->writeback_workspace=NULL;
    r->started = 0;
}

static int flush_cache_mount(struct kernel_page_cache *cache,const struct kernel_vfs_mount *mount)
{
    for (;;) {
        struct kernel_vfs_node *node=NULL;
        {
            CACHE_SCOPE(guard,cache);
            for (struct kernel_page_cache_dirty *dirty=cache->record->dirty_head;dirty;dirty=dirty->queue_next) {
                struct kernel_page_cache_entry *entry=RB_MIN(kernel_page_cache_dirty_tree,&dirty->tree);
                if (!entry || (mount && kernel_vfs_node_mount(entry->node)!=mount)) continue;
                node=entry->node;
                if (kernel_vfs_node_acquire(node)) __builtin_trap();
                break;
            }
        }
        if (!node) return 0;
        int result=kernel_page_cache_writeback(cache,node);
        if (kernel_vfs_node_release(&node)) __builtin_trap();
        if (result) return result;
    }
}
enum kernel_page_cache_status kernel_page_cache_purge_mount(
    struct kernel_page_cache *cache,const struct kernel_vfs_mount *mount)
{
    if (!cache_live(cache) || !mount) return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    if (flush_cache_mount(cache,mount)) return KERNEL_PAGE_CACHE_STATUS_IO;
    int pinned=0;
    {
        CACHE_SCOPE(guard,cache);
        struct kernel_page_cache_entry *entry=cache->record->lru_tail;
        while (entry) {
            struct kernel_page_cache_entry *previous=entry->lru_previous;
            if (kernel_vfs_node_mount(entry->node)==mount) {
                uint32_t references;
                if (physical_page_reference_count(cache->allocator,entry->physical_address,&references)) __builtin_trap();
                if (references!=1 || entry->users || entry_loading(entry) || entry->writeback || entry->aliases ||
                    entry->dirty_end || __atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)) pinned=1;
                else remove_entry(cache,entry);
            }
            entry=previous;
        }
    }
    uint64_t released=0;
    int complete=drain_entries(cache,&released);
    { CACHE_SCOPE(guard,cache);cache->record->statistics.pages_reclaimed+=released; }
    if (!complete) return KERNEL_PAGE_CACHE_STATUS_CLEANUP_REQUIRED;
    return pinned?KERNEL_PAGE_CACHE_STATUS_STATE:KERNEL_PAGE_CACHE_STATUS_OK;
}

enum kernel_page_cache_status kernel_page_cache_invalidate_node(
    struct kernel_page_cache *cache,
    struct kernel_vfs_node *node)
{
    struct kernel_page_cache_entry *entry;
    uint64_t released = 0U;

    if (!cache_live(cache) || node == 0) {
        return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    }
    KERNEL_LOCK_SCOPE(node_guard);
    kernel_vfs_node_lock(node, &node_guard, 1);
    readahead_cancel_node(cache, node);
    {
        kernel_assert_service_context();
        {
            CACHE_SCOPE(guard,cache);
            for (entry=*kernel_vfs_node_cache_pages(node);entry;entry=entry->node_next)
                if (entry_loading(entry) || entry->writeback || entry->users || entry->aliases ||
                    __atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)) return KERNEL_PAGE_CACHE_STATUS_STATE;
            entry=*kernel_vfs_node_cache_pages(node);
            while (entry) {
                struct kernel_page_cache_entry *next=entry->node_next;
                remove_entry(cache,entry); entry=next;
            }
        }
        kernel_lock_scope_release(&node_guard);
        int complete=drain_entries(cache,&released);
        { CACHE_SCOPE(guard,cache); cache->record->statistics.pages_reclaimed+=released; }
        return complete?KERNEL_PAGE_CACHE_STATUS_OK:KERNEL_PAGE_CACHE_STATUS_CLEANUP_REQUIRED;
    }
}

void kernel_page_cache_get_statistics(
    const struct kernel_page_cache *cache,
    struct kernel_page_cache_statistics *statistics)
{
    if (cache_live(cache) && statistics != 0) {
        CACHE_SCOPE(guard, (struct kernel_page_cache *)cache);
        *statistics = cache->record->statistics;
    }
}

enum kernel_page_cache_status kernel_page_cache_destroy(struct kernel_page_cache *cache)
{
    if (!cache || (cache->state!=KERNEL_PAGE_CACHE_LIVE && cache->state!=KERNEL_PAGE_CACHE_CLEANUP) ||
        !cache->record || !cache->allocator || !cache->heap) return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    if (cache->state==KERNEL_PAGE_CACHE_LIVE) {
        KERNEL_RAW_SCOPE(guard,&cache->record->group->metadata);
        if (cache->record->mounts) return KERNEL_PAGE_CACHE_STATUS_STATE;
    }
    kernel_page_cache_stop_worker(cache);
    if (cache->state==KERNEL_PAGE_CACHE_LIVE) {
        if (flush_cache_mount(cache,NULL)) return KERNEL_PAGE_CACHE_STATUS_IO;
        {
            CACHE_SCOPE(guard,cache);
            if (__atomic_load_n(&cache->record->request_borrows,__ATOMIC_ACQUIRE)) return KERNEL_PAGE_CACHE_STATUS_STATE;
            for (struct kernel_page_cache_entry *entry=cache->record->lru_tail;entry;entry=entry->lru_previous) {
                uint32_t references;
                if (physical_page_reference_count(cache->allocator,entry->physical_address,&references)) __builtin_trap();
                if (references!=1 || entry->users || entry_loading(entry) || entry->writeback || entry->aliases ||
                    __atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)) return KERNEL_PAGE_CACHE_STATUS_STATE;
            }
            while (cache->record->lru_tail) remove_entry(cache,cache->record->lru_tail);
        }
        group_unregister(cache);
        cache->state=KERNEL_PAGE_CACHE_CLEANUP;
    }
    uint64_t released=0;
    if (!drain_entries(cache,&released)) return KERNEL_PAGE_CACHE_STATUS_CLEANUP_REQUIRED;
    if (cache->record->buckets) kernel_heap_release(cache->heap,cache->record->buckets);
    if (cache->record->foreground_workspace) kernel_heap_release(cache->heap,cache->record->foreground_workspace);
    version_pool_close(cache->record->versions);
    kernel_heap_release(cache->heap,cache->record);
    cache->heap=NULL;cache->allocator=NULL;cache->record=NULL;cache->state=KERNEL_PAGE_CACHE_RELEASED;
    return KERNEL_PAGE_CACHE_STATUS_OK;
}
