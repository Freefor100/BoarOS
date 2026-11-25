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

struct kernel_page_cache_entry {
    uint64_t writes;
    uint8_t retry_blocked;
    uint8_t dirty_indexed;
    uint8_t page_references_owned;
#if BOAROS_MAX_CPUS == 1
    uint8_t retired;
#else
    uint8_t accessed;
#endif
    struct kernel_page_cache_entry *lru_previous;
    struct kernel_page_cache_entry *lru_next;
    struct kernel_page_cache_entry *cleanup_next;
    RB_ENTRY(kernel_page_cache_entry) dirty_link;
    union {
        RB_ENTRY(kernel_page_cache_entry) page_link;
        /* Reclaim links only entries already removed from the inode tree. */
        struct kernel_page_cache_entry *reclaim_next;
    };
    struct kernel_page_cache *cache;
    struct kernel_vfs_node *node;
    uint64_t page_index;
    uint64_t physical_address;
    unsigned char *data; /* Fixed direct mapping for the owned physical page. */
    size_t dirty_begin;
    size_t dirty_end;
    uint64_t generation;
    int writeback_error;
    uint8_t writeback;
    uint32_t users;
    uint32_t load_status;
    struct kernel_wait_queue ready;
    struct kernel_page_cache_alias *aliases;
#if BOAROS_PAGE_CACHE_DIRTY_BLOCKS
    uint64_t dirty_blocks;
#endif
};
_Static_assert(sizeof(struct kernel_page_cache_entry)<=256,"page cache entry must fit its heap class");


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
        high=total/8;low=total/20;
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
static void cache_wake(struct kernel_wait_queue *queue);
static void group_returned(struct page_cache_group *group)
{
    /* JBD可在STATE及计量raw锁下归还；共同owner只通知调度器，
     * 不反向取得缓存mutex，也不在此等待。 */
    struct kernel_wait_queue *progress=&group->progress;
    /* 与登记后的复查配对，禁止额度归还及空队列查询互相越过。 */
#if BOAROS_MAX_CPUS > 1
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#else
    __asm__ volatile("" ::: "memory");
#endif
    if (__atomic_load_n(&progress->registrations,__ATOMIC_ACQUIRE)) cache_wake(progress);
}
void kernel_writeback_domain_memory(void *context,size_t bytes,bool acquiring)
{
    struct kernel_writeback_domain *domain=context;
    domain_charge(&domain->metadata_bytes,bytes,acquiring);
    if (bytes && !acquiring) group_returned(domain->group);
}

#if BOAROS_PAGE_CACHE_READAHEAD_PAGES
struct readahead_job {
    struct kernel_vfs_node *node;
    uint64_t cookie, first;
    unsigned pages;
    int active, cancelled;
};
#endif

enum { CACHE_SHADOW_LIMIT = 1024, CACHE_SHADOW_BUCKETS = 256 };
struct cache_shadow {
    struct kernel_vfs_node *node;
    uint64_t index, generation;
    uint16_t next, previous; /* Slot zero is the end of a bucket chain. */
};
struct cache_shadow_table {
    struct cache_shadow slots[CACHE_SHADOW_LIMIT];
    uint16_t buckets[CACHE_SHADOW_BUCKETS];
    uint16_t cursor;
};
struct kernel_page_cache_record {
    struct kernel_mutex metadata;
    struct cache_request *reads_head, **reads_tail;
    struct range_writeback *range_head, **range_tail;
    uint64_t range_collect_until;
    struct kernel_wait_queue range_progress;
    uint64_t range_sequence;
    unsigned range_count;
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
    struct kernel_page_cache_entry *lru_head;
    struct kernel_page_cache_entry *lru_tail;
    struct kernel_page_cache_entry *cleanup_entries;
    struct kernel_page_cache_statistics statistics;
#if BOAROS_MAX_CPUS > 1
    uint64_t read_hits;
#endif
    unsigned int writeback_active;
    uint64_t dirty_pages;
    struct kernel_page_cache_dirty *dirty_head, *dirty_tail, *dirty_cursor;
    uint64_t dirty_remaining, dirty_scan_index;
    struct kernel_thread_join worker;
    struct kernel_wait_queue work;
    uint64_t low, high, dirty_high, dirty_low;
    uint64_t completed;
    _Atomic uint64_t requested;
    _Atomic int started, stopping;
    struct version_pool *versions;
    size_t writeback_capacity;
    struct writeback_workspace *writeback_workspace;
#if BOAROS_USER_SMP
    struct writeback_lane *lane;
#endif
    union foreground_workspace *foreground_workspace;
#if BOAROS_PAGE_CACHE_READAHEAD_PAGES
    struct readahead_job readahead_jobs[8];
    struct kernel_thread_join readahead_worker;
    struct kernel_wait_queue readahead_work;
    uint64_t readahead_cookie;
    int readahead_started;
#endif
    struct kernel_page_cache_entry *reclaim_cursor;
    uint64_t reclaim_generation;
    struct cache_shadow_table *shadows;
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
RB_GENERATE_INTERNAL(kernel_page_cache_tree,kernel_page_cache_entry,page_link,dirty_compare,static inline)

/* Writers hold cache metadata and take the inode index only to publish or
 * retire. Readers take the inode index alone, then borrow the page before
 * releasing it. Never take cache metadata or do I/O under the index lock. */
#if BOAROS_MAX_CPUS > 1
#define CACHE_INDEX_SCOPE(name,node) KERNEL_RAW_SCOPE(name,&(node)->cache_index_lock)
#else
#define CACHE_INDEX_SCOPE(name,node) ((void)(node))
#endif

static struct kernel_page_cache_entry *index_at_or_after(struct kernel_vfs_node *node,uint64_t index)
{
    struct kernel_page_cache_entry *entry=RB_ROOT(&node->cache_pages), *next=NULL;
    while (entry) {
        COST_ADD(CACHE_PROBES,1);
        if (entry->page_index==index) return entry;
        if (entry->page_index>index) {
            next=entry;
            entry=RB_LEFT(entry,page_link);
        } else entry=RB_RIGHT(entry,page_link);
    }
    return next;
}
static struct kernel_page_cache_entry *index_find(const struct kernel_vfs_node *node,uint64_t index)
{
    struct kernel_page_cache_entry *entry=RB_ROOT(&node->cache_pages);
    while (entry) {
        COST_ADD(CACHE_PROBES,1);
        if (index==entry->page_index) break;
        entry=index<entry->page_index?RB_LEFT(entry,page_link):RB_RIGHT(entry,page_link);
    }
    return entry;
}

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

static unsigned shadow_bucket(const struct kernel_vfs_node *node, uint64_t index)
{
    uint64_t hash = ((uintptr_t)node >> 6) ^ index;
    return (hash * UINT64_C(0x9e3779b97f4a7c15)) >> 56;
}

/* Shadows borrow identity only. All links and keys are protected by the
 * cache metadata lock; final VFS release clears them before freeing a node. */
static void shadow_remove(struct cache_shadow_table *table, uint16_t slot)
{
    struct cache_shadow *shadow = &table->slots[slot - 1];
    struct kernel_vfs_node *node = shadow->node;
    if (shadow->previous)
        table->slots[shadow->previous - 1].next = shadow->next;
    else
        table->buckets[shadow_bucket(node, shadow->index)] = shadow->next;
    if (shadow->next)
        table->slots[shadow->next - 1].previous = shadow->previous;
    shadow->node = NULL;
    /* This is the last use of node by a cross-inode replacement. */
    __atomic_sub_fetch(&node->cache_shadow_count, 1, __ATOMIC_RELEASE);
}

static void shadow_refill(struct kernel_page_cache_record *record,
    struct kernel_vfs_node *node, uint64_t index)
{
    if (!__atomic_load_n(&node->cache_shadow_count, __ATOMIC_RELAXED)) return;
    struct cache_shadow_table *table = record->shadows;
    uint16_t slot = table->buckets[shadow_bucket(node, index)];
    for (; slot; slot = table->slots[slot - 1].next) {
        struct cache_shadow *shadow = &table->slots[slot - 1];
        if (shadow->node == node && shadow->index == index) {
            shadow_remove(table, slot);
            return;
        }
    }
}

static void shadow_record_reclaim(struct kernel_page_cache_record *record,
    struct kernel_vfs_node *node, uint64_t index)
{
    /* Publication removes a previous shadow for this page under this lock. */
    uint64_t generation = ++record->reclaim_generation;
    if (!kernel_vfs_node_cache_reporting(node)) return;
    struct cache_shadow_table *table = record->shadows;
    uint16_t slot = table->cursor + 1;
    table->cursor = slot % CACHE_SHADOW_LIMIT;
    struct cache_shadow *shadow = &table->slots[slot - 1];
    if (shadow->node) shadow_remove(table, slot);
    unsigned bucket = shadow_bucket(node, index);
    uint16_t next = table->buckets[bucket];
    *shadow = (struct cache_shadow){.node = node, .index = index,
        .generation = generation, .next = next};
    if (next) table->slots[next - 1].previous = slot;
    table->buckets[bucket] = slot;
    __atomic_add_fetch(&node->cache_shadow_count, 1, __ATOMIC_RELAXED);
}

static void shadow_forget_range(struct kernel_page_cache_record *record,
    struct kernel_vfs_node *node, uint64_t first, uint64_t last)
{
    if (!__atomic_load_n(&node->cache_shadow_count, __ATOMIC_RELAXED)) return;
    struct cache_shadow_table *table = record->shadows;
    for (uint16_t slot = 1; slot <= CACHE_SHADOW_LIMIT; slot++) {
        struct cache_shadow *shadow = &table->slots[slot - 1];
        if (shadow->node == node && shadow->index >= first && shadow->index <= last)
            shadow_remove(table, slot);
    }
}

void kernel_page_cache_forget_node(struct kernel_page_cache *cache,
    struct kernel_vfs_node *node)
{
    CACHE_SCOPE(guard, cache);
    shadow_forget_range(cache->record, node, 0, UINT64_MAX);
}

int kernel_page_cache_enable_shadows(struct kernel_page_cache *cache)
{
    struct kernel_page_cache_record *record = cache->record;
    {
        CACHE_SCOPE(guard, cache);
        if (record->shadows) return 0;
    }
    struct cache_shadow_table *table = NULL;
    if (kernel_heap_allocate_zeroed(cache->heap, 1, sizeof(*table), (void **)&table))
        return -KERNEL_ENOMEM;
    {
        CACHE_SCOPE(guard, cache);
        if (!record->shadows) {
            record->shadows = table;
            return 0;
        }
    }
    kernel_heap_release(cache->heap, table);
    return 0;
}

struct read_workspace {
    struct file_read_workspace backend;
#if BOAROS_PAGE_CACHE_READAHEAD_PAGES
    struct kernel_page_cache_read_page fault_pages[KERNEL_PAGE_CACHE_READ_BATCH_MAX];
#endif
    struct kernel_page_cache_entry *entries[KERNEL_PAGE_CACHE_READ_BATCH_MAX];
    struct kernel_page_cache_entry *loads[KERNEL_PAGE_CACHE_READ_BATCH_MAX];
    uint64_t allocation_pages[PHYSICAL_PAGE_ALLOCATION_BATCH_MAX];
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
    /* 捕获／复制阶段使用valid；批发布后转为实际已提交字节。 */
    union { uint32_t valid,written; };
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
 * 不在inode I/O资格或loading页借用下等待另一批归还。
 * 原子指针只转移闲置owner；使用期仍由挂载或请求借用固定cache寿命。 */
static union foreground_workspace *foreground_workspace_take(struct kernel_page_cache *cache)
{
    union foreground_workspace *workspace=__atomic_exchange_n(
        &cache->record->foreground_workspace,NULL,__ATOMIC_ACQUIRE);
    if (!workspace && kernel_heap_allocate(cache->heap,sizeof(*workspace),(void **)&workspace)) return NULL;
    return workspace;
}
static void foreground_workspace_put(struct kernel_page_cache *cache,union foreground_workspace *workspace)
{
    union foreground_workspace *empty=NULL;
    if (__atomic_compare_exchange_n(&cache->record->foreground_workspace,&empty,
        workspace,0,__ATOMIC_RELEASE,__ATOMIC_RELAXED)) return;
    kernel_heap_release(cache->heap,workspace);
}
struct read_batch_load {
    struct kernel_page_cache *cache;
    const struct kernel_vfs_file *file;
    uint64_t first;
    struct kernel_page_cache_read_page *pages;
    size_t count;
    struct kernel_page_cache_entry **entries,**loads;
    struct kernel_page_cache_read_page **outputs;
    struct file_read_span *spans;
    struct file_read_workspace *backend;
    enum kernel_page_cache_status allocation;
    int no_reuse,required;
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
#define CACHE_VERSION_IDLE_MIN (262144U / BOAROS_PAGE_SIZE)
#define CACHE_VERSION_IDLE_MAX (4194304U / BOAROS_PAGE_SIZE)
struct version_pool {
    struct kernel_raw_lock lock;
    struct kernel_heap *heap;
    struct physical_page_allocator *allocator;
    struct cache_version *idle;
    size_t idle_count, reserve, idle_limit;
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
    size_t limit=physical_page_total(allocator)/32;
    if (limit<CACHE_VERSION_IDLE_MIN) limit=CACHE_VERSION_IDLE_MIN;
    if (limit>CACHE_VERSION_IDLE_MAX) limit=CACHE_VERSION_IDLE_MAX;
    /* 不预分配；保留实际归还的一组存储，避免checkpoint后立即重新分配。
     * 空闲页仍由压力回收优先释放，小内存保持原有最小容量。 */
    *pool=(struct version_pool){.heap=heap,.allocator=allocator,.references=1,.idle_limit=limit};
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
/* A detached list still owns one pool reference per version. Keep a bounded
 * prefix in the pool; freeing the rest may enter the allocator. */
static void version_idle_return(struct version_pool *pool,struct cache_version *list)
{
    if (!list) return;
    {
        KERNEL_RAW_SCOPE(guard,&pool->lock);
        if (!pool->closed && pool->idle_count<pool->idle_limit) {
            struct cache_version *first=list,*tail=list;
            size_t count=1,available=pool->idle_limit-pool->idle_count;
            while (tail->next && count<available) {
                tail=tail->next;count++;
            }
            list=tail->next;
            tail->next=pool->idle;pool->idle=first;pool->idle_count+=count;
            cache_wake(&pool->ready);
        }
    }
    while (list) {
        struct cache_version *version=list;
        list=version->next;
        version_dispose(version);
    }
}

static int version_drop(struct cache_version *version,int notify,struct cache_version **idle)
{
    unsigned old=__atomic_fetch_sub(&version->references,1,__ATOMIC_ACQ_REL);
    if (!old) __builtin_trap();
    if (old!=1) return 0;
    struct kernel_writeback_domain *domain=version->domain;
    int returned=domain!=NULL;
    if (domain) {
        version->domain=NULL;domain_charge(&domain->version_bytes,version_bytes(),0);
        if (notify) group_returned(domain->group);
        kernel_writeback_domain_put(domain);
    }
    version->next=*idle;
    *idle=version;
    return returned;
}
static void version_release(void *owner)
{
    struct cache_version *version=owner,*idle=NULL;
    struct version_pool *pool=version->pool;
    (void)version_drop(version,1,&idle);
    version_idle_return(pool,idle);
}
/* Detach only idle owners under the raw lock. Physical/heap releases can take
 * lower-ranked allocator locks and therefore always happen after unlocking. */
static uint64_t version_pool_reclaim(struct version_pool *pool,uint64_t target)
{
    uint64_t released=0;
    while (released<target) {
        struct cache_version *retired=NULL;
        unsigned count=0;
        {
            KERNEL_RAW_SCOPE(guard,&pool->lock);
            /* 池可以容纳多批版本；扩大保留量不扩大一次关中断摘链。 */
            while (pool->idle && pool->idle_count>pool->reserve &&
                   count<64 && count<target-released) {
                struct cache_version *version=pool->idle;
                pool->idle=version->next; pool->idle_count--;
                version->next=retired; retired=version; count++;
            }
        }
        if (!count) break;
        while (retired) {
            struct cache_version *version=retired;retired=version->next;
            version_dispose(version);
        }
        released+=count;
    }
    return released;
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
/* One lock acquisition per available batch. Detached entries are exclusively
 * owned here, including through activation and later allocation failures. */
static struct cache_version *version_idle_take(struct version_pool *pool,size_t maximum)
{
    if (!maximum) return NULL;
    KERNEL_RAW_SCOPE(guard,&pool->lock);
    if (pool->closed) __builtin_trap();
    struct cache_version *head=pool->idle,*tail=head;
    if (!tail) return NULL;
    size_t count=1;
    while (tail->next && count<maximum) {
        tail=tail->next;count++;
    }
    pool->idle=tail->next;tail->next=NULL;pool->idle_count-=count;
    return head;
}
static struct cache_version *version_take_from(struct version_pool *pool,
    struct cache_version **idle,struct kernel_vfs_node *node)
{
    struct cache_version *version=*idle;
    if (version) {
        *idle=version->next;
        version->next=NULL;
        if (__atomic_exchange_n(&version->references,1,__ATOMIC_ACQ_REL)) __builtin_trap();
    } else version=version_new(pool);
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
static struct cache_version *version_take(struct kernel_page_cache *cache,struct kernel_vfs_node *node)
{
    struct version_pool *pool=cache->record->versions;
    struct cache_version *idle=version_idle_take(pool,1);
    return version_take_from(pool,&idle,node);
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

/* The SMP count shares one word with retirement: a reader can return a live
 * entry without racing the removal that hands the last return to cleanup. */
#define CACHE_USERS_RETIRED UINT32_C(0x80000000)
#define CACHE_USERS_COUNT (CACHE_USERS_RETIRED - 1U)

static inline uint32_t entry_users(const struct kernel_page_cache_entry *entry)
{
#if BOAROS_MAX_CPUS > 1
    return __atomic_load_n(&entry->users,__ATOMIC_ACQUIRE)&CACHE_USERS_COUNT;
#else
    return entry->users;
#endif
}

static inline int entry_retired(const struct kernel_page_cache_entry *entry)
{
#if BOAROS_MAX_CPUS > 1
    return (__atomic_load_n(&entry->users,__ATOMIC_RELAXED)&CACHE_USERS_RETIRED)!=0;
#else
    return entry->retired;
#endif
}

static inline void entry_retire(struct kernel_page_cache_entry *entry)
{
#if BOAROS_MAX_CPUS > 1
    if (__atomic_fetch_or(&entry->users,CACHE_USERS_RETIRED,__ATOMIC_ACQ_REL)&CACHE_USERS_RETIRED)
        __builtin_trap();
#else
    if (entry->retired) __builtin_trap();
    entry->retired=1;
#endif
}

static inline void entry_borrows_add(struct kernel_page_cache_record *record, size_t count)
{
#if BOAROS_MAX_CPUS > 1
    uint64_t previous = __atomic_fetch_add(&record->entry_borrows,count,__ATOMIC_RELAXED);
#else
    uint64_t previous = record->entry_borrows;
    record->entry_borrows += count;
#endif
    if (previous > UINT64_MAX-count) __builtin_trap();
}

/* Acquisition is serialized by the cache index; page references are separate. */
static inline void entry_borrow_indexed(struct kernel_page_cache_entry *entry)
{
#if BOAROS_MAX_CPUS > 1
    if ((__atomic_fetch_add(&entry->users,1,__ATOMIC_RELAXED)&CACHE_USERS_COUNT)==CACHE_USERS_COUNT)
        __builtin_trap();
#else
    if (entry->users == UINT32_MAX) __builtin_trap();
    entry->users++;
#endif
}

static void entry_borrow(struct kernel_page_cache_entry *entry)
{
    entry_borrow_indexed(entry);
    entry_borrows_add(entry->cache->record, 1);
}

static inline int entry_return_locked(struct kernel_page_cache_entry *entry)
{
#if BOAROS_MAX_CPUS > 1
    uint32_t users=__atomic_fetch_sub(&entry->users,1,__ATOMIC_ACQ_REL);
    if (!(users&CACHE_USERS_COUNT)) __builtin_trap();
    int last_retired=users==(CACHE_USERS_RETIRED|1U);
#else
    if (!entry->users) __builtin_trap();
    entry->users--;
    int last_retired=!entry->users && entry->retired;
#endif
    if (last_retired && entry->cache->record->started) {
        entry->cache->record->requested=1; cache_wake(&entry->cache->record->work);
    }
    return last_retired;
}

static inline void entry_borrows_sub(struct kernel_page_cache_record *record, size_t count)
{
#if BOAROS_MAX_CPUS > 1
    if (__atomic_fetch_sub(&record->entry_borrows,count,__ATOMIC_RELEASE)<count) __builtin_trap();
#else
    if (record->entry_borrows < count) __builtin_trap();
    record->entry_borrows -= count;
#endif
}

static int entry_return(struct kernel_page_cache_entry *entry)
{
    int retired = entry_return_locked(entry);
    entry_borrows_sub(entry->cache->record, 1);
    return retired;
}

#if BOAROS_MAX_CPUS > 1
static int entry_return_live(struct kernel_page_cache_entry *entry)
{
    uint32_t users=__atomic_load_n(&entry->users,__ATOMIC_RELAXED);
    while (!(users&CACHE_USERS_RETIRED)) {
        if (!users) __builtin_trap();
        if (__atomic_compare_exchange_n(&entry->users,&users,users-1,1,
            __ATOMIC_ACQ_REL,__ATOMIC_RELAXED))
            return 1;
    }
    return 0;
}
#endif

static void readahead_cancel_node(struct kernel_page_cache *cache, struct kernel_vfs_node *node);
static void group_notify(void *context);
static void group_allocation_notify(void *context, uint64_t available);
static int group_allocation_needs_notify(void *context, uint64_t available);
static void notify_dirty_node(struct kernel_page_cache *,struct kernel_vfs_node *);
static void pressure_wait(void *context);
static size_t entry_valid_bytes(const struct kernel_page_cache_entry *entry);

static int cache_live(const struct kernel_page_cache *cache)
{
    return cache && cache->state == KERNEL_PAGE_CACHE_LIVE &&
        cache->heap && cache->allocator && cache->record;
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

#if BOAROS_MAX_CPUS == 1
static void lru_touch(struct kernel_page_cache_record *record,
                      struct kernel_page_cache_entry *entry)
{
    if (record->lru_head == entry) {
        return;
    }
    lru_remove(record, entry);
    lru_insert_head(record, entry);
}

/* The caller visited this run from tail to head, the order that produces
 * the same recency list as touching each page separately. */
static void lru_touch_run(struct kernel_page_cache_record *record,
    struct kernel_page_cache_entry *head, struct kernel_page_cache_entry *tail,
    int cursor_in_run)
{
    struct kernel_page_cache_entry *previous = head->lru_previous;
    if (cursor_in_run && (previous || head != tail))
        record->reclaim_cursor = previous ? previous : tail;
    if (!previous) return;
    struct kernel_page_cache_entry *next = tail->lru_next;
    previous->lru_next = next;
    if (next) next->lru_previous = previous;
    else record->lru_tail = previous;
    head->lru_previous = NULL;
    tail->lru_next = record->lru_head;
    record->lru_head->lru_previous = tail;
    record->lru_head = head;
}

struct lru_read_run {
    struct kernel_page_cache_entry *head, *tail;
    int cursor_in_run;
};
static inline void lru_read_run_add(struct kernel_page_cache_record *record,
    struct lru_read_run *run, struct kernel_page_cache_entry *entry)
{
    if (!run->head || entry->lru_next != run->head) {
        if (run->head) lru_touch_run(record,run->head,run->tail,run->cursor_in_run);
        run->tail=entry;
        run->cursor_in_run=0;
    }
    run->head=entry;
    run->cursor_in_run|=record->reclaim_cursor==entry;
}
static inline void lru_read_run_finish(struct kernel_page_cache_record *record,
    const struct lru_read_run *run)
{
    if (run->head) lru_touch_run(record,run->head,run->tail,run->cursor_in_run);
}
#endif

static int cleanup_entry(struct kernel_page_cache *cache,
                         struct kernel_page_cache_entry *entry,
                         uint64_t *pages_released)
{
    {
        CACHE_SCOPE(guard,cache);
        /* 退休项已禁止新借用；在索引保护内确认最后借用结束后，
         * cleanup持有唯一存储owner，实际页/node/堆释放留在锁外。 */
        if (entry_users(entry) || (__atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)&UINT32_MAX)) return 0;
    }
    while (entry->page_references_owned != 0U) {
        (void)physical_page_release(cache->allocator,
                                  entry->physical_address);
        entry->page_references_owned--;
        (*pages_released)++;
    }
    entry->physical_address = 0U;
    entry->data = NULL;
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
static void publish_cache_page_state(struct kernel_page_cache_entry *entry)
{
    physical_page_publish_cache_state(entry->cache->allocator,entry->physical_address,
        entry->dirty_end!=0,entry->writeback);
}

static inline void dirty_charge(struct kernel_page_cache_record *record,
    const struct kernel_vfs_node *node, unsigned pages)
{
    size_t bytes = (size_t)pages * BOAROS_PAGE_SIZE;
    kernel_task_account_write(bytes);
    struct kernel_writeback_domain *domain = node->instance->writeback.domain;
    if (domain) domain_charge(&domain->dirty_bytes, bytes, 1);
    if (record->group) __atomic_fetch_add(&record->group->dirty, pages, __ATOMIC_RELAXED);
}

static inline void dirty_notify_forced(struct kernel_page_cache_record *record)
{
    if (record->started && !record->stopping && record->force_writeback) {
        record->requested=1;
        cache_wake(&record->work);
    }
}

static inline bool dirty_mark_entry(struct kernel_page_cache_entry *entry,
    size_t begin, size_t end, bool account)
{
    if (begin>=end || end>BOAROS_PAGE_SIZE) __builtin_trap();
    struct kernel_page_cache_record *r=entry->cache->record;
    int first_dirty=!entry->dirty_end;
    if (first_dirty) {
#if BOAROS_PAGE_CACHE_DIRTY_AGE
        if (!r->dirty_pages) {
            r->dirty_deadline=dirty_deadline();
            if (r->started && !r->stopping) cache_wake(&r->work);
        }
#endif
        if (account) dirty_charge(r, entry->node, 1);
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
    if (first_dirty) publish_cache_page_state(entry);
#if BOAROS_PAGE_CACHE_DIRTY_BLOCKS
    size_t block=dirty_block_size(entry);
    if (block) entry->dirty_blocks|=dirty_block_range(block,begin,end);
#endif
    entry->generation++;
    if (account) dirty_notify_forced(r);
    return first_dirty;
}
static void dirty_mark(struct kernel_page_cache_entry *entry, size_t begin, size_t end)
{
    dirty_mark_entry(entry, begin, end, true);
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
    publish_cache_page_state(entry);
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
    if (domain && domain_bytes(domain)>=(domain_limit(domain,low))) return 1;
    return __atomic_load_n(&record->group->dirty,__ATOMIC_RELAXED)>=(low?record->dirty_low:record->dirty_high);
}
#if BOAROS_MAX_CPUS > 1
enum { DIRTY_BATCH_OWNED=1, DIRTY_BATCH_REVISIT=2 };
static void dirty_batch_return(struct kernel_page_cache_record *r,struct kernel_vfs_node *node,int result)
{
    struct kernel_page_cache_dirty *dirty=kernel_vfs_node_dirty_pages(node);
    unsigned state=dirty->batch_state;
    if (!(state&DIRTY_BATCH_OWNED)) __builtin_trap();
    dirty->batch_state=0;
    /* An unchanged failed prefix cannot make progress by requeueing itself.
     * Busy aliases and writers publish their own retry when they release. */
    if (!result && (state&DIRTY_BATCH_REVISIT) && dirty->count && !r->stopping) {
        r->requested=1;
        cache_wake(&r->work);
    }
}
#endif
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
        if (count && entry->node!=batch[0]->node) break;
        int skip=!all && !node_pressure(entry->node,r,1);
#if BOAROS_MAX_CPUS > 1
        if (!skip && dirty->batch_state) {
            dirty->batch_state|=DIRTY_BATCH_REVISIT;
            skip=1;
        }
#endif
        if (skip) {
            /* A prefix of this inode may already have used the round budget.
             * Charging its full size again would hide the next dirty inode. */
            if (!r->dirty_scan_index) {
                uint64_t pages=dirty->count<r->dirty_remaining?dirty->count:r->dirty_remaining;
                r->dirty_remaining-=pages;
            }
            r->dirty_cursor=dirty->queue_next;r->dirty_scan_index=0;scanned++;
            continue;
        }
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

static void retire_index_entry(struct kernel_page_cache_entry *entry)
{
    entry_retire(entry);
    RB_REMOVE(kernel_page_cache_tree,&entry->node->cache_pages,entry);
}

static void remove_retired_entry(struct kernel_page_cache *cache,
    struct kernel_page_cache_entry *entry)
{
    struct kernel_page_cache_record *record=cache->record;
    lru_remove(record,entry);
    entry->cleanup_next=record->cleanup_entries;
    record->cleanup_entries=entry;
    dirty_clear(entry);
    record->statistics.current_pages--;
    record->statistics.evictions++;
}

static void remove_entry(struct kernel_page_cache *cache,
                         struct kernel_page_cache_entry *entry)
{
    if (entry->aliases || entry_loading(entry)) __builtin_trap();
    {
        CACHE_INDEX_SCOPE(index,entry->node);
        retire_index_entry(entry);
    }
    remove_retired_entry(cache,entry);
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
        if (refs == 1 && !e->aliases && !entry_users(e) && !entry_loading(e) && !e->writeback &&
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
    /* Directory retirement closes real inodes. Do it before the group walk
     * and only from an allocation caller without lower-level qualifications. */
    uint64_t released=held || kernel_io_context_current()->user_pins ||
        kernel_io_context_current()->cache_writes ? 0 :
        kernel_vfs_reclaim_clean(g->allocator,target);
    if (released>=target) return released;
    KERNEL_LOCK_SCOPE(walk);
    if (held && held->lock->rank>=g->walk.lock.rank) return released;
    kernel_mutex_lock(&g->walk,&walk);
    struct kernel_page_cache_record *first=g->cursor?g->cursor:g->head,*r=first;
    if (!r) return released;
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
    record->versions=version_pool_new(heap,allocator);
    if (!record->versions) {
        kernel_heap_release(heap,record);
        return KERNEL_PAGE_CACHE_STATUS_NO_MEMORY;
    }
    kernel_wait_queue_init(&record->work);
    kernel_wait_queue_init(&record->range_progress);
    record->reads_tail=&record->reads_head;
    record->range_tail=&record->range_head;
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
            struct physical_page_reclaim_hooks hooks={.reclaim=group_reclaim,.notify=group_allocation_notify,
                .wants_notification=group_allocation_needs_notify,
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
        .entry_borrows = __atomic_load_n(&r->entry_borrows,__ATOMIC_RELAXED), .pending = r->read_requests - r->reads_running,
        .deferred_snapshots = r->deferred_snapshots,
        .writeback_rounds = r->completed,
        .running = r->reads_running, .entry_bytes = sizeof(struct kernel_page_cache_entry),
        .record_bytes = sizeof(*r), .request_bytes = sizeof(struct cache_request)+sizeof(struct read_batch_load),
        .execution_request_bytes=sizeof(struct cache_request),
        .index_live=r->statistics.current_pages,
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

    if (!cache_live(cache) || file == 0 || entry_out == 0) {
        return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    }
    node = kernel_vfs_file_node(file);
    if (node == 0) {
        return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    }
    *entry_out=index_find(node,page_index);
    return *entry_out?KERNEL_PAGE_CACHE_STATUS_OK:KERNEL_PAGE_CACHE_STATUS_NOT_FOUND;
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

void kernel_page_cache_alias_attach_window(struct kernel_page_cache *cache,
    const struct kernel_vfs_file *file, uint64_t first, unsigned selected,
    const struct kernel_page_cache_alias_binding *bindings)
{
    if (!selected) return;
    struct kernel_vfs_node *node = kernel_vfs_file_node(file);
    CACHE_SCOPE(guard, cache);
    for (uint64_t index = first; selected; selected >>= 1, index++, bindings++) {
        if (!(selected & 1U)) continue;
        struct kernel_page_cache_entry *entry = bindings->entry;
        if (!entry) entry = index_find(node, index);
        if (!entry || entry->page_index != index || entry_retired(entry) ||
            entry->physical_address != bindings->physical_address) __builtin_trap();
        link_alias(entry, bindings->alias);
    }
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
static void alias_detach_locked(struct kernel_page_cache_alias *alias)
{
    if (!alias->previous || *alias->previous != alias) __builtin_trap();
    *alias->previous = alias->next;
    if (alias->next) alias->next->previous = alias->previous;
    alias->previous = 0;
    __atomic_store_n(&alias->registered,0,__ATOMIC_RELEASE);
    if (!alias->borrows) {
        alias->entry = 0;
        alias->next = 0;
        return;
    }
    /* Keep the entry and the retired successor live until drain and the
     * outstanding callbacks have finished, respectively. */
    entry_borrow(alias->entry);
    alias->retired_next = alias->next;
    alias_borrow(alias->retired_next);
}

void kernel_page_cache_alias_detach(struct kernel_page_cache_alias *alias)
{
    CACHE_SCOPE(guard, alias->entry->cache);
    alias_detach_locked(alias);
}

void kernel_page_cache_alias_detach_batch(struct kernel_page_cache_alias **aliases,
    unsigned count)
{
    for (unsigned i = 0; i < count;) {
        struct kernel_page_cache *cache = aliases[i]->entry->cache;
        CACHE_SCOPE(guard, cache);
        do {
            alias_detach_locked(aliases[i++]);
        } while (i < count && aliases[i]->entry->cache == cache);
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
    notify_dirty_node(cache,entry->node);
}
#define CACHE_WRITES_CLOSED (UINT64_C(1)<<63)
#define CACHE_ENTRY_CLOSED  (UINT64_C(1)<<62)
#define CACHE_WRITES_MASK (CACHE_WRITES_CLOSED | CACHE_ENTRY_CLOSED | UINT32_MAX)
static int writer_begin(struct kernel_page_cache_entry *entry,
    struct kernel_page_cache_write_borrow *borrow)
{
    if (!borrow || borrow->registration) __builtin_trap();
    uint64_t state = __atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE);
    for (;;) {
        if (state & ~CACHE_WRITES_MASK || (uint32_t)state == UINT32_MAX) __builtin_trap();
        if (state & (CACHE_WRITES_CLOSED | CACHE_ENTRY_CLOSED)) return 0;
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
    if ((state & (CACHE_WRITES_CLOSED | CACHE_ENTRY_CLOSED)) && (uint32_t)state == 1) cache_wake(&entry->ready);
    if ((uint32_t)state==1 && entry->retry_blocked) {
        entry->retry_blocked=0;
        if (entry->cache->record->started && !entry->cache->record->stopping) {
            entry->cache->record->requested=1; cache_wake(&entry->cache->record->work);
        }
    }
    if ((uint32_t)state==1 && entry_retired(entry) && entry->cache->record->started) {
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
            if (entry_retired(entry) || !(__atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)&(CACHE_WRITES_CLOSED | CACHE_ENTRY_CLOSED))) break;
            if (kernel_wait_prepare(&entry->ready,0,0,&token)) __builtin_trap();
            if (!(__atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)&(CACHE_WRITES_CLOSED | CACHE_ENTRY_CLOSED))) {
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

static inline void note_read_access(struct kernel_page_cache *cache,
    struct kernel_page_cache_entry *entry, int no_reuse)
{
#if BOAROS_MAX_CPUS > 1
    (void)cache;
    /* Reclaim clears the reference bit only under this inode's index lock.
     * Readers also protected by cache metadata cannot race that clear. */
    if (!no_reuse && !__atomic_load_n(&entry->accessed,__ATOMIC_RELAXED))
        __atomic_store_n(&entry->accessed,1,__ATOMIC_RELAXED);
#else
    if (!no_reuse && !entry_retired(entry)) lru_touch(cache->record,entry);
#endif
}

static inline void note_read_hits(struct kernel_page_cache *cache,size_t count)
{
#if BOAROS_MAX_CPUS > 1
    __atomic_fetch_add(&cache->record->read_hits,count,__ATOMIC_RELAXED);
#else
    cache->record->statistics.hits+=count;
#endif
}

static inline void note_read_hit(struct kernel_page_cache *cache,
    struct kernel_page_cache_entry *entry,int no_reuse)
{
    note_read_access(cache,entry,no_reuse);
    note_read_hits(cache,1);
}

/* 在索引保护内把已完成页交给读取者；物理引用接替entry保护，不必再借用一次entry。 */
static inline void take_read_page(struct kernel_page_cache *cache,
    struct kernel_page_cache_entry *entry, uint64_t *address, size_t *valid, size_t bytes,
    int no_reuse)
{
    if (physical_page_acquire(cache->allocator, entry->physical_address)) __builtin_trap();
    *address = entry->physical_address;
    *valid = bytes;
    note_read_access(cache,entry,no_reuse);
}

/* 批量读取中直接借用entry保护物理页，避免频繁进入分配器获取全局自旋锁。 */
static inline void take_read_page_borrow(struct kernel_page_cache *cache,
    struct kernel_page_cache_entry *entry, uint64_t *address, size_t *valid, void **entry_out, size_t bytes, int no_reuse)
{
    entry_borrow(entry);
    *entry_out = entry;
    *address = entry->physical_address;
    *valid = bytes;
    note_read_access(cache,entry,no_reuse);
}

static enum kernel_page_cache_status finish_read_page(
    struct kernel_page_cache *cache, struct kernel_page_cache_entry *entry,
    uint64_t *address, size_t *valid, void **entry_out, int no_reuse)
{
    enum kernel_page_cache_status status;
    int clean = 0;
    for (;;) {
        struct kernel_wait_token token = {0};
        int park;
        {
            CACHE_SCOPE(guard, cache);
            status = entry_load_status(entry);
            if (status != KERNEL_PAGE_CACHE_STATUS_BUSY && !(__atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)&CACHE_ENTRY_CLOSED)) {
                if (!status) {
                    *entry_out = entry;
                    *address = entry->physical_address;
                    *valid = entry_valid_bytes(entry);
                    note_read_hit(cache,entry,no_reuse);
                } else {
                    entry_return(entry);
                    if (!entry_users(entry) && !entry_retired(entry)) { remove_entry(cache, entry); clean = 1; }
                }
                break;
            }
            if (kernel_wait_prepare(&entry->ready, 0, 0, &token)) __builtin_trap();
            park = entry_loading(entry) || (__atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)&CACHE_ENTRY_CLOSED);
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
static inline int read_entry_ready(struct kernel_page_cache_entry *entry)
{
    return entry_load_status(entry) == KERNEL_PAGE_CACHE_STATUS_OK && !entry_retired(entry) &&
        (!(__atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)&CACHE_ENTRY_CLOSED));
}

int kernel_page_cache_resident(struct kernel_page_cache *cache,
    const struct kernel_vfs_file *file, uint64_t index)
{
    struct kernel_vfs_node *node=kernel_vfs_file_node(file);
    if (!cache_live(cache) || !node) return 0;
#if BOAROS_MAX_CPUS > 1
    CACHE_INDEX_SCOPE(guard,node);
    if (node->cache_index_closed) return 0;
#else
    CACHE_SCOPE(guard, cache);
#endif
    struct kernel_page_cache_entry *entry=index_find(node,index);
    return entry && !entry_retired(entry) &&
        entry_load_status(entry) == KERNEL_PAGE_CACHE_STATUS_OK;
}

static __attribute__((always_inline)) inline enum kernel_page_cache_status lookup_ready(
    struct kernel_page_cache *cache, const struct kernel_vfs_file *file,
    uint64_t index, uint64_t *address, size_t *valid, int no_reuse, int nowait)
{
    struct kernel_vfs_node *node=kernel_vfs_file_node(file);
    if (!cache_live(cache) || !address || !valid || !node)
        return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
#if BOAROS_MAX_CPUS > 1
    (void)nowait;
    CACHE_INDEX_SCOPE(guard,node);
    if (node->cache_index_closed) return KERNEL_PAGE_CACHE_STATUS_BUSY;
#else
    KERNEL_LOCK_SCOPE(guard);
    if (nowait) {
        if (!kernel_mutex_try_lock(&cache->record->metadata, &guard))
            return KERNEL_PAGE_CACHE_STATUS_BUSY;
    } else kernel_mutex_lock(&cache->record->metadata, &guard);
#endif
    if (!read_cache_open(node)) return KERNEL_PAGE_CACHE_STATUS_BUSY;
    struct kernel_page_cache_entry *entry=index_find(node,index);
    if (!entry) return KERNEL_PAGE_CACHE_STATUS_NOT_FOUND;
    if (!read_entry_ready(entry)) return entry_load_status(entry) ? entry_load_status(entry) : KERNEL_PAGE_CACHE_STATUS_BUSY;
    size_t bytes=entry_valid_bytes(entry);
    if (!bytes) return KERNEL_PAGE_CACHE_STATUS_OUT_OF_RANGE;
    take_read_page(cache,entry,address,valid,bytes,no_reuse);
    note_read_hits(cache,1);
    return KERNEL_PAGE_CACHE_STATUS_OK;
}

enum kernel_page_cache_status kernel_page_cache_lookup_ready(
    struct kernel_page_cache *cache, const struct kernel_vfs_file *file,
    uint64_t index, uint64_t *address, size_t *valid, int no_reuse)
{ return lookup_ready(cache, file, index, address, valid, no_reuse, 0); }

enum kernel_page_cache_status kernel_page_cache_lookup_nowait(
    struct kernel_page_cache *cache, const struct kernel_vfs_file *file,
    uint64_t index, uint64_t *address, size_t *valid, int no_reuse)
{ return lookup_ready(cache, file, index, address, valid, no_reuse, 1); }

enum kernel_page_cache_status kernel_page_cache_lookup(
    struct kernel_page_cache *cache, const struct kernel_vfs_file *file,
    uint64_t index, uint64_t *address, size_t *valid)
{ return kernel_page_cache_lookup_ready(cache,file,index,address,valid,0); }

unsigned kernel_page_cache_acquire_ready(struct kernel_page_cache *cache,
    const struct kernel_vfs_file *file, uint64_t first, unsigned selected,
    uint64_t *addresses, unsigned capacity, unsigned required,
    struct kernel_page_cache_alias_binding *bindings)
{
    _Static_assert(KERNEL_PAGE_CACHE_READY_WINDOW <= PHYSICAL_PAGE_REFERENCE_BATCH_MAX &&
        KERNEL_PAGE_CACHE_READY_WINDOW < 32, "bounded cache and physical reference batch");
    if (!selected || !capacity) return 0;
    struct kernel_vfs_node *node = kernel_vfs_file_node(file);
    if (!cache_live(cache) || !node || !addresses ||
        selected >> KERNEL_PAGE_CACHE_READY_WINDOW) __builtin_trap();
#if BOAROS_LOCK_DEBUG
    if (bindings && !kernel_lock_held(&node->io_lock,0)) __builtin_trap();
#endif
#if BOAROS_MAX_CPUS > 1
    CACHE_INDEX_SCOPE(guard, node);
    if (node->cache_index_closed) return 0;
#else
    CACHE_SCOPE(guard, cache);
#endif
    if (!read_cache_open(node)) return 0;
    uint64_t size = kernel_vfs_node_size(node);
    if (!size) return 0;
    uint64_t last = (size - 1) >> BOAROS_PAGE_SHIFT;
    unsigned acquired = 0, count = 0;
#if BOAROS_MAX_CPUS == 1
    struct lru_read_run run={0};
#endif
    struct kernel_page_cache_entry *entry = index_at_or_after(node, first);
    while (entry && count < capacity && entry->page_index - first < KERNEL_PAGE_CACHE_READY_WINDOW) {
        unsigned slot = entry->page_index - first;
        unsigned bit = 1U << slot;
        if (bit > selected || entry->page_index > last) break;
        if ((selected & bit) && read_entry_ready(entry)) {
            addresses[count++] = entry->physical_address;
            acquired |= bit;
            /* File I/O qualification excludes replacement; the physical
             * reference excludes clean reclaim until alias publication. */
            if (bindings) bindings[slot].entry = entry;
#if BOAROS_MAX_CPUS == 1
            if (KERNEL_PAGE_CACHE_READY_WINDOW > 4)
                lru_read_run_add(cache->record,&run,entry);
            else
#endif
            note_read_access(cache, entry, 0);
        }
        if (count == capacity) break;
        entry = RB_NEXT(kernel_page_cache_tree, &node->cache_pages, entry);
    }
#if BOAROS_MAX_CPUS == 1
    lru_read_run_finish(cache->record,&run);
#endif
    if ((acquired & required) != required) return 0;
    physical_page_acquire_batch(cache->allocator, addresses, count);
    if (count) note_read_hits(cache, count);
    return acquired;
}

/* Headers and data pages have separate preparation. Neither is visible to
 * readers until commit_read_batch publishes a complete loading owner. */
static enum kernel_page_cache_status allocate_entry_header(struct kernel_page_cache *cache,
    struct kernel_vfs_node *node,uint64_t index,struct kernel_page_cache_entry **output)
{
    struct kernel_page_cache_entry *entry;
    if (kernel_heap_allocate_zeroed(cache->heap,1,sizeof(*entry),(void **)&entry))
        return KERNEL_PAGE_CACHE_STATUS_NO_MEMORY;
    if (kernel_vfs_node_acquire(node)) {
        kernel_heap_release(cache->heap,entry);return KERNEL_PAGE_CACHE_STATUS_STATE;
    }
    entry->cache=cache;entry->node=node;entry->page_index=index;*output=entry;
    return KERNEL_PAGE_CACHE_STATUS_OK;
}
static enum kernel_page_cache_status allocate_entry(struct kernel_page_cache *cache,
    struct kernel_vfs_node *node,uint64_t index,int clear,
    struct kernel_page_cache_entry **output)
{
    struct kernel_page_cache_entry *entry;
    enum kernel_page_cache_status status=allocate_entry_header(cache,node,index,&entry);
    if (status) return status;
    if (physical_page_allocate(cache->allocator,&entry->physical_address))
        return abandon_entry(cache,entry,KERNEL_PAGE_CACHE_STATUS_NO_MEMORY);
    entry->page_references_owned=1;
    if (physical_page_map_owned(cache->allocator,entry->physical_address,(void **)&entry->data))
        return abandon_entry(cache,entry,KERNEL_PAGE_CACHE_STATUS_STATE);
    if (clear) memset(entry->data,0,BOAROS_PAGE_SIZE);
    *output=entry;
    return KERNEL_PAGE_CACHE_STATUS_OK;
}

static void prepare_entry_publication(struct kernel_page_cache_entry *entry,
    enum kernel_page_cache_status status)
{
    kernel_wait_queue_init(&entry->ready);
    __atomic_store_n(&entry->load_status,status,__ATOMIC_RELAXED);
    if (status==KERNEL_PAGE_CACHE_STATUS_BUSY) entry_borrow(entry);
}

static void account_entry_publication(struct kernel_page_cache *cache,struct kernel_page_cache_entry *entry)
{
    struct kernel_page_cache_record *r=cache->record;
    shadow_refill(r, entry->node, entry->page_index);
    lru_insert_head(r,entry);r->statistics.insertions++;r->statistics.current_pages++;
    if (r->statistics.current_pages>r->statistics.peak_pages) r->statistics.peak_pages=r->statistics.current_pages;
}

static void publish_entry(struct kernel_page_cache *cache,struct kernel_page_cache_entry *entry,
    enum kernel_page_cache_status status)
{
    prepare_entry_publication(entry,status);
    {
        CACHE_INDEX_SCOPE(index,entry->node);
        if (RB_INSERT(kernel_page_cache_tree,&entry->node->cache_pages,entry)) __builtin_trap();
    }
    account_entry_publication(cache,entry);
}
struct read_pages_progress {
    struct kernel_page_cache_entry **entries;
    uint32_t completed;
};
enum { READ_INDEX_BATCH = 8 };
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
        for (size_t first = 0; first < count; first += READ_INDEX_BATCH) {
            size_t end = count-first>READ_INDEX_BATCH ? first+READ_INDEX_BATCH : count;
            enum kernel_page_cache_status statuses[READ_INDEX_BATCH];
            for (size_t i = first; i < end; i++) {
                struct kernel_page_cache_entry *entry = entries[i];
                struct kernel_page_cache_read_page *output = outputs ? outputs[i] : NULL;
                bool early = (progress.completed & (UINT32_C(1) << i)) != 0;
                size_t valid = !early || output ? entry_valid_bytes(entry) : 0;
                enum kernel_page_cache_status status;
                /* This producer has finished all callbacks and drained DMA. */
                if (early)
                    status = (enum kernel_page_cache_status)(
                        __atomic_load_n(&entry->load_status, __ATOMIC_RELAXED) & ~CACHE_LOAD_EARLY);
                else {
                    if (result && !identified) spans[i].error = result;
                    status = read_page_result(&spans[i], valid);
                }
                statuses[i-first] = status;
                if (output) {
                    output->status = status;
                    if (!status) {
                        output->entry = entry;
                        output->physical_address = entry->physical_address;
                        output->valid_bytes = valid;
                    }
                }
            }
            {
                /* Final READY clears the early marker. The index lock
                 * publishes the completed bytes to readers as a batch. */
                CACHE_INDEX_SCOPE(index,node);
                for (size_t i = first; i < end; i++)
                    __atomic_store_n(&entries[i]->load_status,statuses[i-first],__ATOMIC_RELAXED);
            }
            for (size_t i = first; i < end; i++) {
                struct kernel_page_cache_entry *entry = entries[i];
                enum kernel_page_cache_status status = statuses[i-first];
                failed += status != 0;
                if (status || !outputs || !outputs[i]) entry_return(entry);
                if (!(progress.completed & (UINT32_C(1) << i))) cache_wake(&entry->ready);
                if (status && !entry_users(entry) && !entry_retired(entry)) {
                    remove_entry(cache, entry); clean = 1;
                }
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
    if (!pages || pages>2U*BOAROS_PAGE_CACHE_READAHEAD_PAGES) __builtin_trap();
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
        struct kernel_page_cache_entry *entry=index_find(node,first+resident);
        if (!entry || entry_loading(entry)) break;
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
    /* 候选尚未发布；分配不持索引保护，也不产生等待环。 */
    for (unsigned i = 0; i < plan->pages; i++) {
        if (readahead_cancelled(cache, job)) break;
        uint64_t index = plan->first + i;
        if (index > UINT64_MAX >> BOAROS_PAGE_SHIFT ||
            (index << BOAROS_PAGE_SHIFT) >= kernel_vfs_node_size(plan->node)) break;
        int found;
        { CACHE_SCOPE(lookup, cache); found=index_find(plan->node,index)!=NULL; }
        if (found) continue;
        struct kernel_page_cache_entry *entry=NULL;
        if (allocate_entry(cache, plan->node, index, 0, &entry)) break;
        entries[candidates] = entry;
        spans[candidates++] = (struct file_read_span){index << BOAROS_PAGE_SHIFT, entry->data, BOAROS_PAGE_SIZE, 0, 0};
    }
    struct kernel_page_cache_entry *discard = NULL;
    {
        CACHE_SCOPE(publish, cache);
        for (unsigned i = 0; i < candidates; i++) {
            struct kernel_page_cache_entry *entry = entries[i];
            if (job->cancelled || r->stopping || index_find(plan->node,entry->page_index)) {
                entry->cleanup_next = discard; discard = entry;
            } else {
                publish_entry(cache,entry,KERNEL_PAGE_CACHE_STATUS_BUSY);
                entries[count] = entry; spans[count++] = spans[i];
            }
        }
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
                if (entry_users(entry) > 1) { entries[kept] = entry; spans[kept++] = spans[i]; continue; }
                /* A new inode-local reader must not see cancelled, unfilled data. */
                __atomic_store_n(&entry->load_status,KERNEL_PAGE_CACHE_STATUS_IO,__ATOMIC_RELAXED);
                entry_return(entry);
                remove_entry(cache,entry);
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
        if (!cancelled) {
            struct readahead_job part=plan;
            while (part.pages && !readahead_cancelled(cache,next)) {
                unsigned batch=part.pages>KERNEL_PAGE_CACHE_READ_BATCH_MAX?KERNEL_PAGE_CACHE_READ_BATCH_MAX:part.pages;
                struct readahead_job window=part;window.pages=batch;
                readahead_load(cache,next,&window);
                part.first+=batch;part.pages-=batch;
            }
        }
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

static inline void request_borrow(struct kernel_page_cache_record *record)
{
    if (__atomic_fetch_add(&record->request_borrows,1,__ATOMIC_RELAXED)==UINT32_MAX) __builtin_trap();
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
        request_borrow(r);
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
void kernel_page_cache_put_batch(struct kernel_page_cache *cache,
    struct kernel_page_cache_read_page *pages, size_t count)
{
    if (!cache_live(cache) || !pages || !count) return;
#if BOAROS_MAX_CPUS > 1
    struct kernel_page_cache_record *record=cache->record;
    size_t returned=0;
    int locked=0;
    for (size_t i=0;i<count;i++) {
        if (pages[i].status!=KERNEL_PAGE_CACHE_STATUS_OK) continue;
        if (pages[i].entry && entry_return_live(pages[i].entry)) {
            pages[i].entry=NULL;
            pages[i].physical_address=0;
            returned++;
        } else if (pages[i].entry || pages[i].physical_address) locked=1;
    }
    /* Individual entries may retire after their decrement; the outstanding
     * aggregate keeps the cache alive through the whole release batch. */
    if (returned) entry_borrows_sub(record, returned);
    if (!locked) return;
#endif
    int clean = 0;
    {
        CACHE_SCOPE(guard, cache);
        size_t released_entries = 0;
        for (size_t i = 0; i < count; i++) {
            if (pages[i].status == KERNEL_PAGE_CACHE_STATUS_OK) {
                struct kernel_page_cache_entry *entry = pages[i].entry;
                if (entry) {
                    clean |= entry_return_locked(entry);
                    released_entries++;
                    pages[i].entry = NULL;
                } else if (pages[i].physical_address) {
                    physical_page_release(cache->allocator, pages[i].physical_address);
                    pages[i].physical_address = 0;
                }
            }
        }
        if (released_entries) entry_borrows_sub(cache->record, released_entries);
    }
    if (clean) {
        uint64_t released = 0;
        (void)drain_entries(cache, &released);
    }
}

const void *kernel_page_cache_read_data(const struct kernel_page_cache_read_page *page)
{
    const struct kernel_page_cache_entry *entry=page->entry;
    return entry->data;
}

static enum kernel_page_cache_status get_batch(struct kernel_page_cache *,
    const struct kernel_vfs_file *,uint64_t,struct kernel_page_cache_read_page *,size_t,int,int,bool);

static enum kernel_page_cache_status get_page(
    struct kernel_page_cache *cache, const struct kernel_vfs_file *file,
    uint64_t page_index, uint64_t *physical_address, size_t *valid_bytes,
    int read_window,int no_reuse)
{
    if (!physical_address || !valid_bytes) return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    struct kernel_page_cache_read_page page = {0};
    enum kernel_page_cache_status status = get_batch(cache,file,page_index,&page,1,no_reuse,read_window,true);
    if (!status) {
        if (page.entry && physical_page_acquire(cache->allocator, page.physical_address)) __builtin_trap();
        *physical_address = page.physical_address;
        *valid_bytes = page.valid_bytes;
        if (page.entry) kernel_page_cache_put_batch(cache, &page, 1);
    }
    return status;
}

enum kernel_page_cache_status kernel_page_cache_get(
    struct kernel_page_cache *cache,const struct kernel_vfs_file *file,
    uint64_t index,uint64_t *address,size_t *valid)
{ return get_page(cache,file,index,address,valid,0,0); }

enum kernel_page_cache_status kernel_page_cache_get_for_fault(
    struct kernel_page_cache *cache,const struct kernel_vfs_file *file,
    uint64_t index,uint64_t *address,size_t *valid,int read_window,int no_reuse)
{ return get_page(cache,file,index,address,valid,read_window,no_reuse); }



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
    size_t count=request->count;
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
    {
        CACHE_SCOPE(publish,cache);
        uint64_t size=kernel_vfs_node_size(node);
        for (size_t i=0;i<count;i++) {
            if (pages[i].status!=KERNEL_PAGE_CACHE_STATUS_NOT_FOUND || entries[i]) continue;
            struct kernel_page_cache_entry *candidate=loads[i];
            if (((first+i)<<BOAROS_PAGE_SHIFT)>=size) {
                if (candidate) { candidate->cleanup_next=discard;discard=candidate; }
                pages[i].status=KERNEL_PAGE_CACHE_STATUS_OUT_OF_RANGE;
                continue;
            }
            struct kernel_page_cache_entry *entry=index_find(node,first+i);
            if (entry) {
                if (request->required>=0 && i!=(size_t)request->required) {
                    pages[i].status=KERNEL_PAGE_CACHE_STATUS_BUSY;
                } else if (read_entry_ready(entry)) {
                    take_read_page_borrow(cache,entry,&pages[i].physical_address,&pages[i].valid_bytes,&pages[i].entry,entry_valid_bytes(entry),request->no_reuse);
                    note_read_hits(cache,1);
                    pages[i].status=KERNEL_PAGE_CACHE_STATUS_OK;
                } else { entry_borrow(entry);entries[i]=entry; }
                if (candidate) { candidate->cleanup_next=discard;discard=candidate; }
            } else if (candidate) {
                prepare_entry_publication(candidate,KERNEL_PAGE_CACHE_STATUS_BUSY);
                loads[ready]=candidate;
                spans[ready]=spans[i];
                /* Speculative neighbors keep the producer borrow through
                 * DMA completion. Only demand results transfer that borrow. */
                outputs[ready]=request->required<0 || i==(size_t)request->required ? &pages[i] : NULL;
                ready++;
            } else {
                if (!allocation) __builtin_trap();
                pages[i].status=allocation;
            }
        }
        /* The cache qualification spans candidate selection and insertion.
         * Readers can see only initialized loading owners. Keep each raw
         * section bounded even for the widest prefetch. */
        for (size_t i=0;i<ready;) {
            size_t end=ready-i>READ_INDEX_BATCH ? i+READ_INDEX_BATCH : ready;
            CACHE_INDEX_SCOPE(index,node);
            for (;i<end;i++)
                if (RB_INSERT(kernel_page_cache_tree,&node->cache_pages,loads[i])) __builtin_trap();
        }
        for (size_t i=0;i<ready;i++) account_entry_publication(cache,loads[i]);
    }
    while (discard) {
        struct kernel_page_cache_entry *entry=discard;discard=entry->cleanup_next;entry->cleanup_next=NULL;
        (void)abandon_entry(cache,entry,KERNEL_PAGE_CACHE_STATUS_OK);
    }
    /* 自有加载全部完成后才等待其他producer，反向重叠批次不互等。 */
    (void)load_read_pages(cache,node,loads,spans,ready,outputs,request->backend);
}

/* Reserve the demanded page first. Keep indexed spans in file order for
 * extent lookup and block merging even when the window starts before it. */
static size_t read_allocation_slot(size_t step, int required)
{
    if (required<=0) return step;
    if (!step) return (size_t)required;
    if (step<=(size_t)required) return step-1;
    return step;
}

/* 只有缺失批次进入此处；页、描述及容量由调用CPU准备，热命中不租工作区。 */
static void __attribute__((noinline)) read_batch_local(struct kernel_page_cache *cache,const struct kernel_vfs_file *file,
    uint64_t first,struct kernel_page_cache_read_page *pages,size_t count,
    struct kernel_page_cache_entry **entries,struct read_workspace *workspace,int no_reuse,int required)
{
    struct kernel_vfs_node *node=kernel_vfs_file_node(file);
    struct kernel_page_cache_entry *single_load;
    struct file_read_span single_span;
    struct kernel_page_cache_read_page *single_output;
    struct kernel_page_cache_entry **loads=workspace?workspace->loads:&single_load;
    struct file_read_span *spans=workspace?workspace->spans:&single_span;
    struct kernel_page_cache_read_page **outputs=workspace?workspace->outputs:&single_output;
    size_t candidates=0;
    memset(loads,0,count*sizeof(*loads));
    enum kernel_page_cache_status allocation=KERNEL_PAGE_CACHE_STATUS_OK;
    for (size_t step=0;!allocation && step<count;step++) {
        size_t i=read_allocation_slot(step,required);
        if (pages[i].status!=KERNEL_PAGE_CACHE_STATUS_NOT_FOUND || entries[i]) continue;
        allocation=allocate_entry_header(cache,node,first+i,&loads[i]);
        if (!allocation) candidates++;
    }
    /* Materialize only this batch's actual misses. Larger buddy blocks are
     * split into independent page owners under the same allocator protection;
     * shortage falls through to the existing one-page reclaim/wait path. */
    size_t prepared=0;
    unsigned owned=0,next=0;
    uint64_t *allocated=workspace?workspace->allocation_pages:NULL;
    for (size_t step=0;step<count;step++) {
        size_t i=read_allocation_slot(step,required);
        struct kernel_page_cache_entry *entry=loads[i];
        if (!entry) continue;
        enum kernel_page_cache_status status=KERNEL_PAGE_CACHE_STATUS_OK;
        uint64_t allowance=UINT64_MAX;
        if (required>=0) {
            /* A prepared page is already charged to the allocator. Read
             * global free space only when issuing another allocation. */
            if (next==owned) {
                uint64_t free=physical_page_available(cache->allocator);
                allowance=free>cache->record->high?free-cache->record->high:0;
            }
            if (i!=(size_t)required && (cache->record->stopping || !allowance))
                status=KERNEL_PAGE_CACHE_STATUS_NO_MEMORY;
        }
        if (!status && next==owned && allocated && candidates-prepared>1) {
            unsigned wanted=candidates-prepared>PHYSICAL_PAGE_ALLOCATION_BATCH_MAX?
                PHYSICAL_PAGE_ALLOCATION_BATCH_MAX:(unsigned)(candidates-prepared);
            if (allowance<wanted) wanted=(unsigned)allowance;
            owned=wanted?physical_page_allocate_batch(cache->allocator,allocated,wanted):0;
            next=0;
        }
        if (!status) {
            if (next<owned) entry->physical_address=allocated[next++];
            else if (physical_page_allocate(cache->allocator,&entry->physical_address))
                status=KERNEL_PAGE_CACHE_STATUS_NO_MEMORY;
            if (!status) {
                entry->page_references_owned=1;
                if (physical_page_map_owned(cache->allocator,entry->physical_address,(void **)&entry->data))
                    status=KERNEL_PAGE_CACHE_STATUS_STATE;
            }
        }
        if (status) {
            allocation=status;
            for (;step<count;step++) {
                size_t j=read_allocation_slot(step,required);
                if (loads[j]) {
                    (void)abandon_entry(cache,loads[j],status);loads[j]=NULL;
                }
            }
            break;
        }
        spans[i]=(struct file_read_span){(first+i)<<BOAROS_PAGE_SHIFT,entry->data,BOAROS_PAGE_SIZE,0,0};
        prepared++;
    }
    if (next<owned) physical_page_release_batch(cache->allocator,allocated+next,owned-next);
    candidates=prepared;
    if (candidates) {
        struct read_batch_load request={cache,file,first,pages,count,entries,loads,outputs,spans,
            workspace?&workspace->backend:NULL,allocation,no_reuse,required};
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
        pages[i].status=finish_read_page(cache,entries[i],&pages[i].physical_address,&pages[i].valid_bytes,&pages[i].entry,no_reuse);
}

struct cache_read_batch_request {
    struct kernel_page_cache *cache;
    const struct kernel_vfs_file *file;
    struct kernel_vfs_node *node;
    uint64_t first;
    struct kernel_page_cache_read_page *pages;
    size_t count;
    int no_reuse,read_window;
    bool demand_missing;
};

static int read_cached_batch(struct cache_read_batch_request *request, bool take_physical)
{
    struct kernel_page_cache *cache=request->cache;
    struct kernel_vfs_node *node=request->node;
    int pending=0,retry=0,remote=-1;
    size_t i=0,hits=0;
    {
#if BOAROS_MAX_CPUS > 1
        CACHE_INDEX_SCOPE(guard,node);
        retry=node->cache_index_closed;
#else
        CACHE_SCOPE(guard,cache);
        struct lru_read_run run={0};
        const bool promote_run=!request->no_reuse && request->count>4;
#endif
        if (!retry) retry=!read_cache_open(node);
        uint64_t size=retry?0:kernel_vfs_node_size(node);
        uint64_t first=request->first;
        struct kernel_page_cache_entry *next=NULL;
        if ((first<<BOAROS_PAGE_SHIFT)<size)
            next=request->count==1?index_find(node,first):index_at_or_after(node,first);
        for (;i<request->count && !retry;i++) {
            struct kernel_page_cache_read_page *page=&request->pages[i];
            page->status=KERNEL_PAGE_CACHE_STATUS_NOT_FOUND;
#if BOAROS_PAGE_CACHE_READAHEAD_PAGES
            page->cache_miss=0;
#endif
            uint64_t index=first+i;
            uint64_t offset=index<<BOAROS_PAGE_SHIFT;
            if (offset>=size) {
                page->status=KERNEL_PAGE_CACHE_STATUS_OUT_OF_RANGE;
                page->physical_address=0;
                page->valid_bytes=0;
                continue;
            }
            /* One root lookup per batch. A gap leaves the next cached page
             * in place until the loop reaches it. */
            struct kernel_page_cache_entry *entry=next && next->page_index==index?next:NULL;
            if (entry && i+1<request->count)
                next=RB_NEXT(kernel_page_cache_tree,&node->cache_pages,entry);
            if (!entry) {
#if BOAROS_PAGE_CACHE_READAHEAD_PAGES
                page->cache_miss=1;
                if (request->count == 1) request->demand_missing = true;
#endif
                pending=1;
                if (remote<0) remote=remote_cache_client();
                if (!remote) break;
                continue;
            }
            if (!read_entry_ready(entry)) {
                if ((__atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)&CACHE_ENTRY_CLOSED)) { retry=1; break; }
#if BOAROS_PAGE_CACHE_READAHEAD_PAGES
                page->cache_miss=1;
#endif
                pending=1;
                if (remote<0) remote=remote_cache_client();
                if (!remote) break;
                continue;
            }
            size_t valid=size-offset<BOAROS_PAGE_SIZE?size-offset:BOAROS_PAGE_SIZE;
            /* Scalar mapping readers need a physical reference. READY
             * entries are stable under the index guard until it is taken. */
            if (take_physical)
                take_read_page(cache,entry,&page->physical_address,&page->valid_bytes,valid,request->no_reuse);
            else {
                entry_borrow_indexed(entry);
                page->entry=entry;
                page->physical_address=entry->physical_address;
                page->valid_bytes=valid;
#if BOAROS_MAX_CPUS == 1
                if (promote_run)
                    lru_read_run_add(cache->record,&run,entry);
                else
#endif
                note_read_access(cache,entry,request->no_reuse);
            }
            page->status=KERNEL_PAGE_CACHE_STATUS_OK;
            hits++;
        }
        if (hits && !take_physical) entry_borrows_add(cache->record,hits);
#if BOAROS_MAX_CPUS == 1
        lru_read_run_finish(cache->record,&run);
        note_read_hits(cache,hits);
#endif
        if (retry) {
            for (size_t j=0;j<i;j++) {
                struct kernel_page_cache_read_page *page=&request->pages[j];
                if (!page->status && page->entry) {
                    entry_return(page->entry);
                    page->entry=NULL;
                }
                page->status=KERNEL_PAGE_CACHE_STATUS_NOT_FOUND;
            }
        }
        if (pending || retry) {
            /* Private preparation has no indexed entry yet. Retain the same
             * cache lifetime used by queued requests until caller unwind. */
            request_borrow(cache->record);
        }
    }
#if BOAROS_MAX_CPUS > 1
    if (hits) note_read_hits(cache,hits);
#endif
    if (pending || retry) {
        /* A local miss already initialized the slot where the scan stopped. */
        if (!retry && i<request->count) i++;
        for (;i<request->count;i++) {
            request->pages[i].status=KERNEL_PAGE_CACHE_STATUS_NOT_FOUND;
#if BOAROS_PAGE_CACHE_READAHEAD_PAGES
            request->pages[i].cache_miss=0;
#endif
        }
    }
    return pending || retry;
}
static void __attribute__((noinline)) prepare_read_batch(void *argument)
{
    struct cache_read_batch_request *request = argument;
    struct kernel_page_cache *cache = request->cache;
    struct kernel_vfs_node *node=request->node;
    size_t demand_count=request->count;
    size_t window=1;
    int required=-1;
#if BOAROS_PAGE_CACHE_READAHEAD_PAGES
    struct kernel_page_cache_read_page *demand=request->pages;
    uint64_t demand_first=request->first, window_first=demand_first;
    if (request->read_window && !request->no_reuse && request->demand_missing) {
        unsigned maximum=BOAROS_PAGE_CACHE_READAHEAD_PAGES;
        if (request->read_window==KERNEL_PAGE_CACHE_FAULT_EXEC_AHEAD) {
            maximum*=2U;
            if (maximum>KERNEL_PAGE_CACHE_READ_BATCH_MAX)
                maximum=KERNEL_PAGE_CACHE_READ_BATCH_MAX;
        }
        CACHE_SCOPE(check,cache);
        uint64_t size=kernel_vfs_node_size(node);
        uint64_t offset=request->first<<BOAROS_PAGE_SHIFT;
        if (!cache->record->stopping && read_cache_open(node) && offset<size &&
            physical_page_available(cache->allocator)>cache->record->high+maximum+2U) {
            if (request->read_window==KERNEL_PAGE_CACHE_FAULT_EXEC_AHEAD) {
                unsigned back=maximum/2U;
                window_first=demand_first>back?demand_first-back:0;
            }
            uint64_t remaining=((size-1)>>BOAROS_PAGE_SHIFT)+1-window_first;
            window=remaining<maximum?(size_t)remaining:maximum;
        }
    }
#endif
    union foreground_workspace *lease=(demand_count>1 || window>1)?foreground_workspace_take(cache):NULL;
#if BOAROS_PAGE_CACHE_READAHEAD_PAGES
    if (window>1 && lease) {
        required=(int)(demand_first-window_first);
        request->first=window_first;
        request->pages=lease->read.fault_pages;
        request->count=window;
        memset(request->pages,0,window*sizeof(*request->pages));
        for (size_t i=0;i<window;i++) request->pages[i].status=KERNEL_PAGE_CACHE_STATUS_NOT_FOUND;
        request->pages[required]=*demand;
    } else window=1;
#endif
    if (request->count>1 && !lease) {
        for (size_t i=0;i<request->count;i++)
            if (request->pages[i].status==KERNEL_PAGE_CACHE_STATUS_NOT_FOUND)
                request->pages[i].status=KERNEL_PAGE_CACHE_STATUS_NO_MEMORY;
        return;
    }
#if BOAROS_PAGE_CACHE_READAHEAD_PAGES
scalar:;
#endif
    /* The lease lives only in the existing prepared-read callback. A cold
     * fault may borrow a bounded result table here; callers still own one page. */
    struct kernel_page_cache_entry *single_entry;
    struct kernel_page_cache_entry **entries=lease?lease->read.entries:&single_entry;
    int pending=0;
    for (;;) {
        CACHE_SCOPE(lookup, cache);
        if (!read_cache_open(node)) {
            kernel_lock_release(&lookup);
            kernel_file_mapping_wait(node);
            continue;
        }
        uint64_t size=kernel_vfs_node_size(node);
        struct kernel_page_cache_entry *next=required>=0?index_at_or_after(node,request->first):NULL;
        for (size_t i = 0; i < request->count; i++) {
            struct kernel_page_cache_read_page *page = &request->pages[i];
            entries[i] = NULL;
            if (page->status != KERNEL_PAGE_CACHE_STATUS_NOT_FOUND) continue;
            uint64_t offset = (request->first + i) << BOAROS_PAGE_SHIFT;
            if (offset >= size) {
                page->status = KERNEL_PAGE_CACHE_STATUS_OUT_OF_RANGE;
                continue;
            }
            uint64_t index=request->first+i;
            struct kernel_page_cache_entry *entry;
            if (required<0) entry=index_find(node,index);
            else {
                while (next && next->page_index<index)
                    next=RB_NEXT(kernel_page_cache_tree,&node->cache_pages,next);
                entry=next && next->page_index==index?next:NULL;
            }
            if (entry) {
                if (required>=0 && i!=(size_t)required) {
                    page->status=KERNEL_PAGE_CACHE_STATUS_BUSY;
                    continue;
                }
                if (read_entry_ready(entry)) {
                    size_t valid = size - offset < BOAROS_PAGE_SIZE ? size - offset : BOAROS_PAGE_SIZE;
                    take_read_page_borrow(cache, entry, &page->physical_address, &page->valid_bytes, &page->entry, valid,request->no_reuse);
                    note_read_hits(cache,1);
                    page->status = KERNEL_PAGE_CACHE_STATUS_OK;
                    continue;
                }
                entry_borrow(entry); entries[i] = entry;
            }
            pending = 1;
        }
        /* A racing loader may have filled the demand slot after the fast
         * miss. Keep its real borrow and abandon optional misses before
         * preparing headers or physical pages. */
        if (required >= 0 && (entries[required] ||
            request->pages[required].status != KERNEL_PAGE_CACHE_STATUS_NOT_FOUND)) {
            pending = entries[required] != NULL;
            for (size_t i=0;i<request->count;i++)
                if (i!=(size_t)required)
                    request->pages[i].status=KERNEL_PAGE_CACHE_STATUS_BUSY;
        }
        break;
    }
    if (pending) read_batch_local(cache,request->file,request->first,request->pages,request->count,entries,
        lease?&lease->read:NULL,request->no_reuse,required);
#if BOAROS_PAGE_CACHE_READAHEAD_PAGES
    if (required>=0) {
        *demand=request->pages[required];
        request->first=demand_first;
        request->pages=demand;request->count=demand_count;
        if (demand->status==KERNEL_PAGE_CACHE_STATUS_NO_MEMORY) {
            /* Optional descriptors/pages must not turn a scalar demand into
             * ENOMEM. Drain first, release the lease, then try scalar once. */
            foreground_workspace_put(cache,lease);lease=NULL;
            required=-1;*demand=(struct kernel_page_cache_read_page){.status=KERNEL_PAGE_CACHE_STATUS_NOT_FOUND};
            goto scalar;
        }
    }
#endif
    if (lease) foreground_workspace_put(cache,lease);
}

static enum kernel_page_cache_status get_batch(
    struct kernel_page_cache *cache, const struct kernel_vfs_file *file, uint64_t first,
    struct kernel_page_cache_read_page *pages, size_t count, int no_reuse,int read_window,bool take_physical)
{
    /* A direct physical result is valid only for a scalar call. The fast
     * scan can roll back entry borrows when a later slot needs a retry. */
    if (take_physical && count != 1) __builtin_trap();
    struct kernel_vfs_node *node=kernel_vfs_file_node(file);
    if (!cache_live(cache) || !node || count > KERNEL_PAGE_CACHE_READ_BATCH_MAX ||
        (count && (!pages || first > UINT64_MAX >> BOAROS_PAGE_SHIFT ||
            count - 1 > (UINT64_MAX >> BOAROS_PAGE_SHIFT) - first)))
        return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    if (!count) return KERNEL_PAGE_CACHE_STATUS_OK;
    struct cache_read_batch_request request = {cache,file,node,first,pages,count,no_reuse,read_window,false};
    /* local首个缺页后交给inode资格下的重查，不重复预扫整批。remote须先
     * 取出全部热页：尚未执行的CPU0请求被取消时，后部成功项也继续有效。 */
    if (!read_cached_batch(&request,take_physical))
        /* The fast scan only returns hits and a possible EOF suffix. */
        return pages[count-1].status;
    prepare_read_batch(&request);
    request_return(cache->record);
    enum kernel_page_cache_status result = KERNEL_PAGE_CACHE_STATUS_OK;
    for (size_t i = 0; i < count; i++) if (pages[i].status) {
        pages[i].physical_address = 0;
        pages[i].valid_bytes = 0;
        if (!result) result = pages[i].status;
    }
    return result;
}

enum kernel_page_cache_status kernel_page_cache_get_batch(
    struct kernel_page_cache *cache,const struct kernel_vfs_file *file,uint64_t first,
    struct kernel_page_cache_read_page *pages,size_t count,int no_reuse)
{ return get_batch(cache,file,first,pages,count,no_reuse,0,false); }

#if BOAROS_PAGE_CACHE_READAHEAD_PAGES && BOAROS_PAGE_SIZE > 4096
enum kernel_page_cache_status kernel_page_cache_get_forward_page(
    struct kernel_page_cache *cache,const struct kernel_vfs_file *file,uint64_t first,
    struct kernel_page_cache_read_page *page)
{ return get_batch(cache,file,first,page,1,0,KERNEL_PAGE_CACHE_FAULT_READ_AHEAD,false); }
#endif

static void resize_apply(struct kernel_page_cache_resize *,uint64_t);
static int write_admission(struct kernel_page_cache *cache,struct kernel_vfs_node *node,int space_pressure);
/* 已发布entry的借用覆盖其数据页，不再另加物理引用；完整冷覆盖只准备私有候选，
 * 用户来源缺页结束后才发布，避免把未初始化数据交给读取者或等待自身加载。 */
static enum kernel_page_cache_status prepare_write_entry(struct kernel_page_cache *cache,
    struct kernel_vfs_file *file, uint64_t index, int whole,
    struct kernel_page_cache_entry **entry, struct kernel_page_cache_entry **candidate,
    uint64_t *address)
{
    struct kernel_vfs_node *node=kernel_vfs_file_node(file);
    int missing;
    {
        CACHE_SCOPE(guard,cache);
        enum kernel_page_cache_status status=lookup_entry(cache,file,index,entry);
        missing=status==KERNEL_PAGE_CACHE_STATUS_NOT_FOUND;
        if (!status && entry_load_status(*entry) == KERNEL_PAGE_CACHE_STATUS_OK) {
            entry_borrow(*entry);
            *address=(*entry)->physical_address;
            return KERNEL_PAGE_CACHE_STATUS_OK;
        }
        *entry=NULL;
        if (status && !missing) return status;
    }
    if (missing && (whole || (index<<BOAROS_PAGE_SHIFT)>=kernel_vfs_node_size(node))) {
        return allocate_entry(cache,node,index,!whole,candidate);
    }
    struct kernel_page_cache_read_page page={0};
    enum kernel_page_cache_status status=kernel_page_cache_get_batch(cache,file,index,&page,1, 0);
    if (status) return status;
    /* 装入器已经交出真实entry借用，直接转给写入现场；不先换成物理
     * 引用再查询索引取回同一借用。文件写操作门排除同inode的退休。 */
    if (!page.entry) __builtin_trap();
    *entry=page.entry;*address=page.physical_address;
    return KERNEL_PAGE_CACHE_STATUS_OK;
}

struct prepare_space {
    struct kernel_vfs_node *node;
    uint64_t offset,accepted_end;
    size_t length,ready;
    int mapping,error,complete;
    bool overwrite_mapped;
    struct file_write_reservation *reservation;
};
static void prepare_space(void *argument)
{
    struct prepare_space *request=argument;
    struct kernel_vfs_node *node=request->node;
    KERNEL_LOCK_SCOPE(io); kernel_vfs_node_lock(node,&io,1);
    size_t length=request->length;
    if (request->mapping) {
        uint64_t epoch;
        if (kernel_file_mapping_epoch(node,&epoch)) { request->error=-KERNEL_EBUSY; return; }
        if (kernel_vfs_node_attributes(node)&KERNEL_VFS_INODE_IMMUTABLE) { request->error=-KERNEL_EPERM; return; }
        if (request->offset>=node->size) { request->error=-KERNEL_EFAULT; return; }
        if (length>node->size-request->offset) length=node->size-request->offset;
    }
    request->error=node->instance->ops->error(node->instance);
    if (!request->error) {
        if (node->instance->ops->reserve_write)
            request->error=node->instance->ops->reserve_write(node,request->offset,length,&request->ready,&request->complete,request->reservation);
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
    node->instance->ops->finish_reservation(node,request->reservation,request->offset,request->accepted_end);
}
static void complete_space(void *argument)
{
    struct prepare_space *request=argument;
    struct kernel_vfs_node *node=request->node;
    request->error=kernel_page_cache_writeback_range_borrowing(node->instance->page_cache,
        node,0,UINT64_MAX,kernel_io_context_current()->locks,false);
    if (!request->error) request->error=node->instance->ops->complete_write(node);
}
static int prepare_space_request(struct kernel_page_cache *cache,struct prepare_space *request,
    void (*execute)(void *))
{
    if (request->overwrite_mapped) {
        struct kernel_vfs_instance *instance = request->node->instance;
        if (instance->read_only) return -KERNEL_EROFS;
        int error = instance->ops->error(instance);
        if (!error) request->ready = request->length;
        return error;
    }
    /* 来源pin和页写资格在空间准备前归还；write/offset门闩保持同次写互斥。
     * CPU0后端借用这些门闩，CALLER后端直接使用当前任务的事务现场。 */
    struct kernel_io_context *context=kernel_io_context_current();
    if (context->user_pins || context->cache_writes || context->backend_depth || kernel_cpu_current()->raw_locks)
        __builtin_trap();
    for (struct kernel_lock_guard *g=context->locks;g;g=g->previous)
        if (g->lock->rank>15) __builtin_trap();
    if (request->node->instance->ops->file_operation_execution == KERNEL_VFS_EXECUTION_CPU0 &&
        kernel_page_cache_needs_cpu0(cache)) {
        if (run_request(cache,execute,request,1,NULL)) return -KERNEL_EIO;
    } else execute(request);
    return request->error;
}
int kernel_page_cache_prepare_mapping_write(struct kernel_page_cache *cache,struct kernel_vfs_node *node,
    uint64_t index,struct kernel_page_cache_reservation *reservation)
{
    struct prepare_space request={.node=node,.offset=index<<BOAROS_PAGE_SHIFT,
        .length=BOAROS_PAGE_SIZE,.mapping=1,.reservation=&reservation->ticket};
    int result=prepare_space_request(cache,&request,prepare_space);
    if (result && reservation->ticket.owner) {
        request.accepted_end=request.offset;
        prepare_space_request(cache,&request,finish_space);
    }
    if (!result) {
        reservation->offset=request.offset;
        reservation->end=request.offset+request.ready;
    }
    return result;
}
void kernel_page_cache_accept_mapping_write(struct kernel_vfs_node *node,
    struct kernel_page_cache_reservation *reservation)
{
    if (reservation->ticket.owner)
        node->instance->ops->accept_reservation(node,&reservation->ticket,reservation->offset,reservation->end);
}
void kernel_page_cache_finish_mapping_write(struct kernel_page_cache *cache,struct kernel_vfs_node *node,
    struct kernel_page_cache_reservation *reservation,int accepted)
{
    if (!reservation->ticket.owner)return;
    struct prepare_space request={.node=node,.reservation=&reservation->ticket,.offset=reservation->offset,
        .accepted_end=accepted?reservation->end:reservation->offset};
    prepare_space_request(cache,&request,finish_space);
    *reservation=(struct kernel_page_cache_reservation){0};
}

#if BOAROS_USER_SMP || BOAROS_MAX_CPUS == 1
struct cached_write_workspace {
    struct kernel_mm_user_pin pins[KERNEL_MM_USER_READ_BATCH];
    union {
        struct kernel_page_cache_write_borrow writers[KERNEL_MM_USER_READ_BATCH];
        struct kernel_page_cache_entry *append[KERNEL_MM_USER_READ_BATCH];
    };
};
_Static_assert(sizeof(struct cached_write_workspace) <= BOAROS_PAGE_SIZE,
    "write descriptors fit in the task I/O page");

static unsigned cached_user_batch(const struct kernel_vfs_write_source *source,
    uint64_t offset, size_t limit, uint64_t *address)
{
    if (!source->mm || source->user.count != 1 || source->user.index ||
        (offset & BOAROS_PAGE_MASK))
        return 0;
    const struct kernel_uaccess_iovec *iov = source->user.iov;
    *address = iov->base + source->user.offset;
    if ((*address & BOAROS_PAGE_MASK) || source->user.offset >= iov->length)
        return 0;
    uint64_t available = iov->length - source->user.offset;
    if (limit > available) limit = available;
    size_t pages = limit / BOAROS_PAGE_SIZE;
    return pages > KERNEL_MM_USER_READ_BATCH ? KERNEL_MM_USER_READ_BATCH : (unsigned)pages;
}

/* The VFS write-operation owner fixes the append position. The candidates
 * remain private through allocation, user access and the final index check. */
static __attribute__((noinline)) size_t copy_cached_append_batch(
    struct kernel_page_cache *cache, struct kernel_vfs_node *node, uint64_t offset,
    struct kernel_vfs_write_source *source, size_t limit,
    struct file_write_reservation *ticket)
{
    uint64_t address;
    unsigned count = cached_user_batch(source, offset, limit, &address);
    if (count < 2) return 0;
    struct kernel_task_io_buffer scratch = {0};
    if (kernel_task_io_buffer_acquire(&scratch, cache->allocator)) return 0;
    struct cached_write_workspace *work = scratch.data;
    uint64_t pages[KERNEL_MM_USER_READ_BATCH];
    uint64_t first = offset >> BOAROS_PAGE_SHIFT;
    unsigned owned = physical_page_allocate_batch(cache->allocator, pages, count);
    unsigned built = 0;
    for (; built < owned; built++) {
        struct kernel_page_cache_entry *entry;
        if (allocate_entry_header(cache, node, first + built, &entry)) break;
        entry->physical_address = pages[built];
        entry->page_references_owned = 1;
        if (physical_page_map_owned(cache->allocator, pages[built], (void **)&entry->data))
            __builtin_trap();
        work->append[built] = entry;
    }
    physical_page_release_batch(cache->allocator, pages + built, owned - built);
    memset(work->pins, 0, built * sizeof(work->pins[0]));
    unsigned pinned = built ?
        kernel_mm_try_pin_user_read_batch(source->mm, address, built, work->pins) : 0;
    struct kernel_vfs_write_source copy = *source;
    for (unsigned i = 0; i < pinned; i++) {
        copy.data = work->pins[i].page;
        kernel_vfs_write_source_copy(&copy, work->append[i]->data, BOAROS_PAGE_SIZE);
        kernel_vfs_write_source_advance(&copy, BOAROS_PAGE_SIZE);
    }
    kernel_mm_unpin_user_read_batch(work->pins, pinned);
    unsigned published = 0;
    if (pinned) {
        KERNEL_LOCK_SCOPE(io);
        kernel_vfs_node_lock(node, &io, 1);
        if (kernel_vfs_node_size(node) == offset) {
            {
                CACHE_SCOPE(guard, cache);
                {
                    CACHE_INDEX_SCOPE(index, node);
#if BOAROS_MAX_CPUS > 1
                    if (!node->cache_index_closed)
#endif
                    {
                        struct kernel_page_cache_entry *next = index_at_or_after(node, first);
                        for (; published < pinned; published++) {
                            if (next && next->page_index == first + published) break;
                            struct kernel_page_cache_entry *entry = work->append[published];
                            prepare_entry_publication(entry, KERNEL_PAGE_CACHE_STATUS_OK);
                            if (RB_INSERT(kernel_page_cache_tree, &node->cache_pages, entry))
                                __builtin_trap();
                        }
                    }
                }
                for (unsigned i = 0; i < published; i++) {
                    account_entry_publication(cache, work->append[i]);
                    dirty_mark_entry(work->append[i], 0, BOAROS_PAGE_SIZE, false);
                }
                if (published) {
                    dirty_charge(cache->record, node, published);
                    dirty_notify_forced(cache->record);
                }
            }
            if (published) {
                size_t length = (size_t)published * BOAROS_PAGE_SIZE;
                kernel_vfs_node_written(node, offset + length);
                if (length == limit && ticket->owner && node->instance->ops->finish_full_reservation)
                    node->instance->ops->finish_full_reservation(ticket);
            }
        }
    }
    for (unsigned i = published; i < built; i++)
        (void)abandon_entry(cache, work->append[i], KERNEL_PAGE_CACHE_STATUS_OK);
    kernel_task_io_buffer_release(&scratch);
    size_t bytes = (size_t)published * BOAROS_PAGE_SIZE;
    if (published) source->data = NULL;
    source->user.offset += bytes;
    source->chunks += published;
    return bytes;
}

/* A resident, page-aligned overwrite can keep its inode writer across the
 * copy. Source faults and closed destination entries fall back at the first
 * unaccepted page. A batch never takes the cache mutex until publishing dirt. */
static __attribute__((noinline)) size_t copy_cached_overwrite_batch(
    struct kernel_page_cache *cache, struct kernel_vfs_node *node, uint64_t offset,
    struct kernel_vfs_write_source *source, size_t limit)
{
    uint64_t eof = kernel_vfs_node_size(node);
    if (offset >= eof) return 0;
    if (limit > eof - offset) limit = eof - offset;
    uint64_t address;
    unsigned count = cached_user_batch(source, offset, limit, &address);
    if (count < 2) return 0;

    struct kernel_task_io_buffer scratch = {0};
    if (kernel_task_io_buffer_acquire(&scratch, cache->allocator)) return 0;
    struct cached_write_workspace *work = scratch.data;
    memset(work->pins, 0, count * sizeof(work->pins[0]));
    struct kernel_mm_user_pin *pins = work->pins;
    count = kernel_mm_try_pin_user_read_batch(source->mm, address, count, pins);
    if (!count) {
        kernel_task_io_buffer_release(&scratch);
        return 0;
    }
    struct kernel_page_cache_write_borrow *writers = work->writers;
    memset(writers, 0, count * sizeof(writers[0]));
    unsigned used = 0;
    {
        KERNEL_LOCK_SCOPE(io);
        kernel_vfs_node_lock(node, &io, 1);
        {
#if BOAROS_MAX_CPUS > 1
            CACHE_INDEX_SCOPE(index, node);
            if (!node->cache_index_closed && read_cache_open(node)) {
#else
            CACHE_SCOPE(guard, cache);
            if (read_cache_open(node)) {
#endif
                uint64_t first = offset >> BOAROS_PAGE_SHIFT;
                struct kernel_page_cache_entry *entry = index_find(node, first);
                for (; used < count; used++) {
                    if (!entry || entry->page_index != first + used ||
                        !read_entry_ready(entry) || !writer_begin(entry, &writers[used]))
                        break;
                    entry_borrow_indexed(entry);
                    entry = RB_NEXT(kernel_page_cache_tree, &node->cache_pages, entry);
                }
                if (used) entry_borrows_add(cache->record, used);
            }
        }
        for (unsigned i = 0; i < used; i++) {
            source->data = pins[i].page;
            kernel_vfs_write_source_copy(source, writers[i].entry->data, BOAROS_PAGE_SIZE);
            kernel_vfs_write_source_advance(source, BOAROS_PAGE_SIZE);
        }
        kernel_mm_unpin_user_read_batch(pins, count);
        if (used) {
            CACHE_SCOPE(guard, cache);
            unsigned newly_dirty = 0;
            for (unsigned i = used; i > 0; i--) {
                struct kernel_page_cache_entry *entry = writers[i-1].entry;
                newly_dirty += dirty_mark_entry(entry, 0, BOAROS_PAGE_SIZE, false);
                writer_end_locked(&writers[i-1]);
                entry_return_locked(entry);
            }
            entry_borrows_sub(cache->record, used);
            if (newly_dirty) dirty_charge(cache->record, node, newly_dirty);
            dirty_notify_forced(cache->record);
        }
    }
    kernel_task_io_buffer_release(&scratch);
    return (size_t)used * BOAROS_PAGE_SIZE;
}
#endif

static int copy_cached_source(struct kernel_page_cache *cache,struct kernel_vfs_file *file,
    uint64_t offset,struct kernel_vfs_write_source *source,size_t size,size_t *written,int *complete,struct prepare_space *reservation)
{
    struct kernel_vfs_node *node=kernel_vfs_file_node(file);
    *written=0;
    size_t ready=0;
    int space_error=0;
    int retried_space=0;
    struct kernel_vfs_write_view view={0};
    struct file_write_reservation *ticket=reservation->reservation;
#if BOAROS_USER_SMP || BOAROS_MAX_CPUS == 1
    bool batch_write = true;
#if BOAROS_MAX_CPUS > 1
    bool batch_overwrite = kernel_cpu_get_boot_count() > 1;
#else
    const bool batch_overwrite = true;
#endif
#endif
    while (*written<size) {
        if (!ready) {
            int retry_space=space_error==-KERNEL_ENOSPC && ticket->space_pressure && !retried_space;
            if (ticket->owner) {
                reservation->accepted_end=offset;
                prepare_space_request(cache,reservation,finish_space);
            }
            if (space_error && !retry_space) return space_error;
            int error=write_admission(cache,node,retry_space);
            if (error) return error;
            retried_space=retry_space;
            size_t length=BOAROS_PAGE_CACHE_WRITEBACK_PAGES*BOAROS_PAGE_SIZE-(offset&BOAROS_PAGE_MASK);
            if (length>size-*written) length=size-*written;
            *reservation=(struct prepare_space){.node=node,.offset=offset,.length=length,
                .overwrite_mapped=source->overwrite_mapped,.reservation=ticket};
            ticket->space_pressure=0;
            space_error=prepare_space_request(cache,reservation,prepare_space);
            ready=reservation->ready;
            *complete|=reservation->complete;
            if (!ready) {
                if (space_error!=-KERNEL_ENOSPC || !ticket->space_pressure || retried_space)
                    return space_error;
                continue;
            }
        }
#if BOAROS_USER_SMP || BOAROS_MAX_CPUS == 1
        if (batch_write && ready >= 2 * BOAROS_PAGE_SIZE) {
            size_t copied = 0;
            if (offset == kernel_vfs_node_size(node))
                copied = copy_cached_append_batch(cache, node, offset, source, ready, ticket);
            else if (batch_overwrite && !ticket->owner)
                copied = copy_cached_overwrite_batch(cache, node, offset, source, ready);
            if (copied) {
                offset += copied;
                *written += copied;
                ready -= copied;
                retried_space = 0;
                continue;
            }
            batch_write = 0;
        }
#endif
        size_t start=offset&BOAROS_PAGE_MASK;
        size_t count=BOAROS_PAGE_SIZE-start;
        if (count>size-*written) count=size-*written;
        if (count>ready) count=ready;
        KERNEL_LOCK_SCOPE(io);
        /* 外层文件写操作门固定逻辑尺寸，取得写入来源前不重复占用inode。 */
        uint64_t old_size=kernel_vfs_node_size(node);
        struct kernel_page_cache_resize resize={0};
        uint64_t address=0;
        struct kernel_page_cache_entry *entry=NULL,*candidate=NULL;
        int whole=!start && count==BOAROS_PAGE_SIZE;
        enum kernel_page_cache_status status;
PrepareTarget:
        if (whole && offset==old_size) {
            /* 新的完整 EOF 页不需要旧内容。先填私有页，发布时再查
             * 索引和容量，避免每页为同一个未命中先取得一次缓存锁。 */
            status=allocate_entry(cache,node,offset>>BOAROS_PAGE_SHIFT,0,&candidate);
        } else
            status=prepare_write_entry(cache,file,offset>>BOAROS_PAGE_SHIFT,
                whole,&entry,&candidate,&address);
        if (status) {
            kernel_page_cache_resize_finish(&resize,old_size);
            if (status==KERNEL_PAGE_CACHE_STATUS_NOT_FOUND) continue;
            return status==KERNEL_PAGE_CACHE_STATUS_NO_MEMORY ? -KERNEL_ENOMEM : -KERNEL_EIO;
        }
        struct kernel_page_cache_write_borrow writer={0};
        if (!kernel_vfs_write_source_prepare(source,count,&view)) {
            if (candidate) (void)abandon_entry(cache,candidate,KERNEL_PAGE_CACHE_STATUS_OK);
            kernel_page_cache_resize_finish(&resize,old_size);
            if (entry) {
                { CACHE_SCOPE(guard,cache); entry_return(entry); }
            }
            return source->staging_error;
        }
        if (candidate && whole && view.size<count) {
            /* 第二来源页尚不可用时仍须接纳真实前缀；私有完整覆盖候选
             * 没有旧尾，先结束来源借用，再按部分覆盖装入旧内容或零页。 */
            count=view.size;
            kernel_vfs_write_source_finish(source,&view);
            (void)abandon_entry(cache,candidate,KERNEL_PAGE_CACHE_STATUS_OK);
            candidate=NULL;whole=0;
            goto PrepareTarget;
        }
        count=view.size;
        /* 连续写覆盖全部新字节，普通写借用已隔离稳定快照；只有空洞需要
         * 关闭旧EOF页并撤销别名写权限，清零旧EOF到本次起点之间的范围。 */
        if (offset>old_size) {
            int error=kernel_page_cache_resize_prepare(cache,node,old_size,offset,&resize);
            if (error==-KERNEL_EBUSY && source->mm) {
                /* MM编辑可正在排空来源借用。无pin推进撤权后先结束EOF
                 * 资格，再从共同入口重新取得来源，避免持关闭页去缺页。 */
                kernel_vfs_write_source_finish(source,&view);
                error=kernel_page_cache_resize_prepare(cache,node,old_size,offset,&resize);
                kernel_page_cache_resize_finish(&resize,old_size);
                if (candidate) (void)abandon_entry(cache,candidate,KERNEL_PAGE_CACHE_STATUS_OK);
                if (entry) {
                    CACHE_SCOPE(guard,cache);entry_return(entry);
                }
                if (error) return error;
                continue;
            }
            if (error) {
                kernel_vfs_write_source_finish(source,&view);
                kernel_page_cache_resize_finish(&resize,old_size);
                if (candidate) (void)abandon_entry(cache,candidate,KERNEL_PAGE_CACHE_STATUS_OK);
                if (entry) {
                    { CACHE_SCOPE(guard,cache); entry_return(entry); }
                }
                return error;
            }
        }
        struct kernel_vfs_write_source copied_source;
        if (candidate) {
            /* 私有页尚无读取者；先填入，再一次发布ready和dirty。
             * 竞胜页或写资格关闭时，原来源游标仍可重试。 */
            copied_source=*source;
            kernel_vfs_write_source_copy_view(&copied_source,candidate->data+start,&view);
        }
        kernel_vfs_node_lock(node,&io,1);
        int admitted=0,published=0;
        if (candidate) {
            CACHE_SCOPE(guard,cache);
            struct kernel_page_cache_entry *current=index_find(node,offset>>BOAROS_PAGE_SHIFT);
            if (current) {
                if (entry_load_status(current) == KERNEL_PAGE_CACHE_STATUS_OK) {
                    entry=current;entry_borrow(entry);address=entry->physical_address;
                    admitted=resize.entry==entry || writer_begin(entry,&writer);
                }
            } else {
                publish_entry(cache,candidate,KERNEL_PAGE_CACHE_STATUS_OK);
                dirty_mark(candidate,start,start+count);
                candidate=NULL;*source=copied_source;admitted=published=1;
            }
        } else if (entry) {
            admitted=resize.entry==entry || writer_begin(entry,&writer);
        }
        if (candidate) (void)abandon_entry(cache,candidate,KERNEL_PAGE_CACHE_STATUS_OK);
        if (!admitted) {
            kernel_lock_release(&io);
            kernel_vfs_write_source_finish(source,&view);
            kernel_page_cache_resize_finish(&resize,old_size);
            if (entry) {
                (void)kernel_page_cache_wait_writable(cache,file,offset>>BOAROS_PAGE_SHIFT,address);
                { CACHE_SCOPE(guard,cache); entry_return(entry); }
            }
            continue;
        }
        if (!published) {
            kernel_vfs_write_source_copy_view(source,entry->data+start,&view);
        }
        resize_apply(&resize,offset>old_size?offset:old_size);
        if (!published) {
            CACHE_SCOPE(guard,cache);
            dirty_mark(entry,start,start+count);
            if (writer.registration) writer_end_locked(&writer);
            entry_return(entry);
        }
        if (offset+count>old_size) kernel_vfs_node_written(node,offset+count);
        if (count==ready && ticket->owner && node->instance->ops->finish_full_reservation)
            node->instance->ops->finish_full_reservation(ticket);
        kernel_lock_release(&io);
        kernel_vfs_write_source_finish(source,&view);
        kernel_page_cache_resize_finish(&resize,old_size);
        offset+=count; *written+=count; ready-=count;
        retried_space=0;
    }
    return 0;
}
static int write_cached_source(struct kernel_page_cache *cache,struct kernel_vfs_file *file,
    uint64_t offset,struct kernel_vfs_write_source *source,size_t size,size_t *written)
{
    int complete=0;
    struct file_write_reservation ticket={0};
    struct prepare_space reservation={.reservation=&ticket};
    int error=copy_cached_source(cache,file,offset,source,size,written,&complete,&reservation);
    if (ticket.owner) {
        reservation.accepted_end=offset+*written;
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
    if (*written) notify_dirty_node(cache,node);
    return result;
}

static int complete_writeback(struct kernel_page_cache *cache,
    struct kernel_page_cache_entry *entry,const struct prepared_writeback *prepared,int result)
{
    size_t begin=prepared->begin,end=prepared->end;
    COST_ADD(WRITEBACK_ACCEPTED,prepared->written);
    if (prepared->written>end-begin) __builtin_trap();
    if (!result && !entry_retired(entry) && prepared->generation==entry->generation) {
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
    publish_cache_page_state(entry);
    /* All entry-ready queues register against the state under this mutex. */
    if (__atomic_load_n(&entry->ready.registrations, __ATOMIC_RELAXED))
        cache_wake(&entry->ready);
    /* 旧版本完成后只通知真实剩余工作；错误仍由原owner保留。 */
    if (!result && cache->record->started && !cache->record->stopping &&
        cache->record->force_writeback && !entry_retired(entry) && entry->dirty_end) {
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
#if BOAROS_USER_SMP
/* count publishes both the selected entry loans and exclusive use of the
 * workspace. Only the consuming thread returns the loans, including on stop. */
struct writeback_lane {
    struct kernel_page_cache *cache;
    struct kernel_thread_join worker;
    struct kernel_wait_queue work;
    struct writeback_workspace workspace;
    uint64_t round_start_pages;
    int round_waiting;
    struct kernel_page_cache_entry *batch[BOAROS_PAGE_CACHE_WRITEBACK_PAGES];
    size_t count;
};
#endif
static int writeback_workspace_init(struct kernel_page_cache *cache,struct writeback_workspace *work,
    size_t count)
{
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
      publish_cache_page_state(entry);
      cache_wake(&entry->ready); }
}

struct writeback_qualification {
    struct kernel_wait_token wait;
    struct kernel_page_cache_alias *aliases;
    int writing;
};
enum { WRITEBACK_NEEDS_SLOW = 1 };

static int owns_writeback_input(struct kernel_page_cache_entry *entry)
{
    for (struct kernel_page_cache_write_borrow *b=kernel_io_context_current()->cache_writes;b;b=b->next)
        if (b->entry==entry) return 1;
    return 0;
}

static int qualify_writeback_locked(struct kernel_page_cache *cache,
    struct kernel_page_cache_entry *entry,uint64_t limit,int background,
    struct prepared_writeback *prepared,struct writeback_qualification *slow)
{
    uint64_t start=entry->page_index<<BOAROS_PAGE_SHIFT;
    /* 选择后已变干净或退休，不再取得写回资格。 */
    if (entry_retired(entry) || !entry->dirty_end || limit<=start || limit-start<=entry->dirty_begin)
        return 0;
    if (entry_loading(entry) || !kernel_vfs_node_writeback_allowed(entry->node)) return -KERNEL_EBUSY;
    if (entry->writeback || (__atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)&CACHE_WRITES_CLOSED)) {
        if (!slow) return WRITEBACK_NEEDS_SLOW;
        if (background) { entry->retry_blocked=1; return -KERNEL_EBUSY; }
        if (kernel_wait_prepare(&entry->ready,0,0,&slow->wait)) __builtin_trap();
        return 0;
    }
    if (!prepared->version) return -KERNEL_ENOMEM;
    if (!slow || background) {
        if (!slow && entry->aliases) return WRITEBACK_NEEDS_SLOW;
        uint64_t expected=0;
        if (!__atomic_compare_exchange_n(&entry->writes,&expected,CACHE_WRITES_CLOSED,0,
                __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
            if (!slow) return WRITEBACK_NEEDS_SLOW;
            entry->retry_blocked=1;cache->record->deferred_snapshots++;
            return -KERNEL_EBUSY;
        }
    } else {
        uint64_t previous=__atomic_fetch_or(&entry->writes,CACHE_WRITES_CLOSED,__ATOMIC_ACQ_REL);
        if (previous&CACHE_WRITES_CLOSED || previous&~CACHE_WRITES_MASK) __builtin_trap();
        slow->writing=(uint32_t)previous!=0;
    }
    if (background) entry->retry_blocked=1;
    prepared->closed=1;entry->writeback=1;cache->record->writeback_active++;
    publish_cache_page_state(entry);
    if (slow) { slow->aliases=entry->aliases; alias_borrow(slow->aliases); }
    return 0;
}

static int prepare_writeback(struct kernel_page_cache *cache,
    struct kernel_page_cache_entry *entry,uint64_t limit,int wait_version,
    int background,struct prepared_writeback *prepared)
{
    if (owns_writeback_input(entry)) return -KERNEL_EBUSY;
Retry:
    /* 选页借用已固定entry；先取得版本，在同一次保护内取得写回资格并关闭
     * 写者。等待其他owner前归还未使用版本，不把池额度留在等待者手中。 */
    prepared->version=version_take(cache,entry->node);
    int result;
    struct writeback_qualification slow={0};
    {
        CACHE_SCOPE(guard,cache);
        result=qualify_writeback_locked(cache,entry,limit,background,prepared,&slow);
    }
    if (!prepared->closed) {
        if (prepared->version) version_release(prepared->version);
        prepared->version=NULL;
        if (slow.wait.task) {
            enum kernel_wait_wake_reason reason;
            if (kernel_wait_park(&slow.wait,&reason) || kernel_wait_finish(&slow.wait)) __builtin_trap();
            goto Retry;
        }
        if (result==-KERNEL_ENOMEM && wait_version && background && cache->record->started &&
            version_wait(cache)) goto Retry;
        return result;
    }
    result=rearm_aliases(entry,slow.aliases);
    while (!result && slow.writing) {
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

static void return_prepared_versions(struct kernel_page_cache *cache,
    struct prepared_writeback *prepared,size_t count,int unused_only)
{
    struct cache_version *idle=NULL;
    int returned=0;
    while (count) {
        struct prepared_writeback *p=&prepared[--count];
        if (!p->version || (unused_only && p->closed)) continue;
        returned|=version_drop(p->version,0,&idle);
        p->version=NULL;
    }
    version_idle_return(cache->record->versions,idle);
    if (returned) group_returned(cache->record->group);
}

/* Versions belong to this caller before qualification; never close a later
 * writer gate or keep its unused version while resolving this boundary. */
static int prepare_writeback_prefix(struct kernel_page_cache *cache,
    struct kernel_page_cache_entry **entries,size_t count,uint64_t limit,
    int background,struct prepared_writeback *prepared,size_t *done)
{
    size_t available=0,candidates=0;
    while (available<count && !owns_writeback_input(entries[available])) available++;
    struct version_pool *pool=cache->record->versions;
    struct cache_version *idle=version_idle_take(pool,available);
    while (candidates<available) {
        struct kernel_page_cache_entry *entry=entries[candidates];
        struct prepared_writeback *p=&prepared[candidates++];
        p->version=version_take_from(pool,&idle,entry->node);
        if (!p->version) break;
    }
    int result=0;
    *done=0;
    if (candidates) {
        CACHE_SCOPE(guard,cache);
        for (;*done<candidates;(*done)++) {
            result=qualify_writeback_locked(cache,entries[*done],limit,background,&prepared[*done],NULL);
            if (result) break;
        }
    }
    return_prepared_versions(cache,prepared,candidates,1);
    return result?result:available<count && candidates==available?-KERNEL_EBUSY:0;
}

static int prepare_writeback_batch(struct kernel_page_cache *cache,
    struct kernel_page_cache_entry **entries,size_t count,uint64_t limit,
    int background,struct prepared_writeback *prepared,size_t *done)
{
    *done=0;
    size_t possible=0;
    if (count>1) {
        CACHE_SCOPE(guard,cache);
        /* Don't speculatively fill the pool beyond an observed slow page.
         * Only qualification after allocation owns a page or its gate. */
        for (;possible<count;possible++) {
            struct kernel_page_cache_entry *entry=entries[possible];
            if (entry->aliases || entry->writeback ||
                __atomic_load_n(&entry->writes,__ATOMIC_RELAXED)) break;
        }
    }
    if (possible>1) {
        int result=prepare_writeback_prefix(cache,entries,possible,limit,background,prepared,done);
        if (result<0 && (result!=-KERNEL_ENOMEM || *done || !background)) return result;
    }
    /* The existing prefix may remain closed, as with sequential preparation.
     * In particular, aliases don't split an otherwise complete backend batch. */
    for (;*done<count;(*done)++) {
        int result=prepare_writeback(cache,entries[*done],limit,!*done,background,&prepared[*done]);
        if (result) return result;
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

static size_t seek_cached_page(const struct kernel_page_cache_entry *entry,
    size_t start, size_t end, bool seek_hole, bool unwritten)
{
    if (unwritten && entry_load_status(entry)==KERNEL_PAGE_CACHE_STATUS_OK)
        return seek_hole ? end : start;
    if (!entry->dirty_end) return seek_hole ? start : end;
#if BOAROS_PAGE_CACHE_DIRTY_BLOCKS
    size_t block = dirty_block_size(entry);
    if (block) {
        uint64_t mask = seek_hole ? ~entry->dirty_blocks : entry->dirty_blocks;
        mask &= dirty_block_range(block, start, end);
        if (!mask) return end;
        size_t match = (size_t)__builtin_ctzll(mask) * block;
        return match > start ? match : start;
    }
#endif
    size_t quantum = writeback_block_size(entry);
    size_t first = entry->dirty_begin & ~(quantum - 1);
    size_t last = (entry->dirty_end + quantum - 1) & ~(quantum - 1);
    size_t match;
    if (seek_hole) match = start < first || start >= last ? start : last;
    else match = start >= last ? end : start > first ? start : first;
    return match < end ? match : end;
}

uint64_t kernel_page_cache_seek_sparse(struct kernel_page_cache *cache, struct kernel_vfs_node *node,
    uint64_t start, uint64_t end, bool seek_hole, bool unwritten)
{
    while (start < end) {
        CACHE_SCOPE(guard, cache);
        struct kernel_page_cache_entry *entry = unwritten ?
            index_at_or_after(node, start >> BOAROS_PAGE_SHIFT) :
            dirty_at_or_after(node, start >> BOAROS_PAGE_SHIFT);
        /* Long runs use bounded batches and don't block other inodes. */
        for (unsigned checked = 0; checked < 16U && start < end; checked++) {
            if (!entry || entry->page_index > (end - 1) >> BOAROS_PAGE_SHIFT)
                return seek_hole ? start : end;
            uint64_t base = entry->page_index << BOAROS_PAGE_SHIFT;
            if (start < base) {
                if (seek_hole) return start;
                start = base;
            }
            size_t limit = end - base < BOAROS_PAGE_SIZE ? end - base : BOAROS_PAGE_SIZE;
            size_t match = seek_cached_page(entry, start - base, limit, seek_hole, unwritten);
            if (match < limit) return base + match;
            start = base + limit;
            entry = unwritten ? RB_NEXT(kernel_page_cache_tree, &node->cache_pages, entry) :
                RB_NEXT(kernel_page_cache_dirty_tree, &kernel_vfs_node_dirty_pages(node)->tree, entry);
        }
    }
    return end;
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
    struct kernel_page_cache_entry **entries,struct prepared_writeback *prepared,size_t count,uint64_t limit)
{
    size_t first=0;
    while (first<count && !prepared[first].version) first++;
    if (first==count) return;
    {
        CACHE_SCOPE(capture,cache);
        struct kernel_vfs_node *node=NULL;
        uint64_t size=0;
        for (size_t i=first;i<count;i++) {
            struct prepared_writeback *p=&prepared[i];
            if (!p->version) continue;
            struct kernel_page_cache_entry *entry=entries[i];
            if (node!=entry->node) {
                node=entry->node;
                size=kernel_vfs_node_size(node);
            }
            p->begin=entry->dirty_begin;p->end=entry->dirty_end;
            p->generation=entry->generation;
            uint64_t start=entry->page_index<<BOAROS_PAGE_SHIFT;
            uint64_t remaining=start<size?size-start:0;
            p->valid=remaining<BOAROS_PAGE_SIZE?(uint32_t)remaining:BOAROS_PAGE_SIZE;
            if (p->end>p->valid) p->end=p->valid;
            if (limit>start && p->end>limit-start) p->end=(uint32_t)(limit-start);
            if (entry_retired(entry)) p->begin=p->end=0;
            if (p->end<p->begin) p->end=p->begin;
#if BOAROS_PAGE_CACHE_DIRTY_BLOCKS
            size_t block=dirty_block_size(entry);
            if (block) p->blocks=entry->dirty_blocks&dirty_block_range(block,p->begin,p->end);
#endif
        }
    }
    /* 本批entry借用固定存储；关闭的写资格及已撤销映射排除写者，
     * 同一inode读资格固定EOF并排除范围变化。复制不占用共享索引mutex。 */
    for (size_t i=first;i<count;i++) {
        struct prepared_writeback *p=&prepared[i];
        if (!p->version) continue;
        struct kernel_page_cache_entry *entry=entries[i];
        size_t cursor=p->begin,begin,end;
        size_t copy_block=writeback_block_size(entry);
        unsigned char *stable=version_data(p->version);
        while (next_writeback_range(entry,p,&cursor,&begin,&end)) {
            size_t first=begin&~(copy_block-1),last=(end+copy_block-1)&~(copy_block-1);
            size_t copy_end=last<p->valid?last:p->valid;
            COST_ADD(SNAPSHOT_COPY,copy_end-first);COST_ADD(WRITEBACK_REQUESTED,end-begin);
            memcpy(stable+first,entry->data+first,copy_end-first);
            memset(stable+copy_end,0,last-copy_end);
        }
        p->written=0;
    }
    {
        CACHE_SCOPE(publish,cache);
        for (size_t i=first;i<count;i++) {
            struct prepared_writeback *p=&prepared[i];
            if (!p->version) continue;
            struct kernel_page_cache_entry *entry=entries[i];
            if (!p->closed) __builtin_trap();
            uint64_t previous=__atomic_fetch_and(&entry->writes,~CACHE_WRITES_CLOSED,__ATOMIC_RELEASE);
            if (previous!=CACHE_WRITES_CLOSED) __builtin_trap();
            entry->retry_blocked=0;p->closed=0;
        }
    }
    /* 整批资格已发布；选择时取得的借用覆盖锁外通知及后续提交。 */
    for (size_t i=first;i<count;i++)
        if (prepared[i].version &&
            __atomic_load_n(&entries[i]->ready.registrations,__ATOMIC_ACQUIRE))
            cache_wake(&entries[i]->ready);
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
    struct writeback_workspace *workspace,int background)
{
    enum { max_pages = KERNEL_VFS_GROWTH_WRITEBACK_MAX };
    _Static_assert(max_pages <= UINT8_MAX, "span owners must fit in one byte");
    if (!count || count>max_pages) __builtin_trap();
    reclaim_window_memory(cache);
    /* Progress I/O (including loop) is exempt from admission throttling, but
     * that says nothing about ownership of this cache worker's workspace. */
    if (workspace && workspace==cache->record->writeback_workspace) {
        if (!kernel_page_cache_is_executor(cache)) __builtin_trap();
        if (count>cache->record->writeback_capacity) count=cache->record->writeback_capacity;
    }
    size_t span_capacity=workspace?BOAROS_PAGE_CACHE_WRITEBACK_PAGES:1;
    if (span_capacity<count) span_capacity=count;
    uint8_t span_owners[max_pages];
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
        size_t ready=0,done=0;
        int result=prepare_writeback_batch(cache,entries+first,count-first,limit,background,
            prepared,&done);
        int prepare_error=result;
        KERNEL_LOCK_SCOPE(io);
        if (done) kernel_vfs_node_lock(entries[first]->node,&io,0);
        capture_writeback(cache,entries+first,prepared,done,limit);
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
        return_prepared_versions(cache,prepared,done,0);
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
    struct writeback_workspace *workspace;
    int caller_writeback, independent_batch, advanced, space_pressure;
    int result;
};
static void write_admission_window(void *argument)
{
    struct write_admission_request *request=argument;
    struct kernel_page_cache *cache=request->cache;
    struct kernel_page_cache_record *r=cache->record;
    if (!request->caller_writeback && !kernel_page_cache_is_executor(cache)) __builtin_trap();
    struct writeback_workspace *workspace=request->caller_writeback?
        request->workspace:r->writeback_workspace;
    struct kernel_page_cache_entry *single;
    struct kernel_page_cache_entry **selected=workspace?workspace->selected:&single;
    size_t capacity=request->caller_writeback?
        (workspace?BOAROS_PAGE_CACHE_WRITEBACK_PAGES:1):r->writeback_capacity;
    if (capacity>BOAROS_PAGE_CACHE_WRITEBACK_PAGES) __builtin_trap();
    size_t count=0;
    {
        CACHE_SCOPE(guard,cache);
        if (request->caller_writeback && r->stopping) {request->result=-KERNEL_EIO;return;}
        struct kernel_page_cache_dirty *dirty;
        struct kernel_page_cache_entry *entry;
#if BOAROS_MAX_CPUS > 1
        if (request->independent_batch) {
            /* First try the caller's inode. When it is clean or busy, claim a
             * bounded window of another idle inode on the same device. */
            struct kernel_page_cache_dirty *own=kernel_vfs_node_dirty_pages(request->node);
            dirty=own;
            for (unsigned checked=0;dirty && checked<16;checked++) {
                entry=RB_MIN(kernel_page_cache_dirty_tree,&dirty->tree);
                if (entry && !dirty->batch_state && !entry_loading(entry) && !entry->writeback &&
                    !__atomic_load_n(&entry->writes,__ATOMIC_RELAXED) &&
                    entry->node->instance->writeback.domain==request->node->instance->writeback.domain &&
                    entry->node->instance->ops->file_operation_execution==KERNEL_VFS_EXECUTION_CALLER) break;
                dirty=checked ? dirty->queue_next : r->dirty_head;
                entry=NULL;
            }
        } else
#endif
        for (dirty=r->dirty_head;dirty;dirty=dirty->queue_next) {
            entry=RB_MIN(kernel_page_cache_dirty_tree,&dirty->tree);
            if (entry && entry->node->instance->writeback.domain==request->node->instance->writeback.domain &&
                (!request->space_pressure || entry->node->instance==request->node->instance) &&
                (!request->caller_writeback ||
                 entry->node->instance->ops->file_operation_execution==KERNEL_VFS_EXECUTION_CALLER)) break;
        }
        if (dirty) {
            while (entry && count<capacity) {
                if (request->independent_batch && (entry_loading(entry) || entry->writeback ||
                    __atomic_load_n(&entry->writes,__ATOMIC_RELAXED))) break;
                selected[count++]=entry;entry_borrow(entry);
                entry=RB_NEXT(kernel_page_cache_dirty_tree,&dirty->tree,entry);
            }
#if BOAROS_MAX_CPUS > 1
            if (count && request->independent_batch) dirty->batch_state=DIRTY_BATCH_OWNED;
#endif
        }
    }
    if (count) {
        size_t used;
        int error=writeback_run(cache,selected,count,UINT64_MAX,&used,workspace,0);
        request->advanced=!error && used!=0;
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
#if BOAROS_MAX_CPUS > 1
        if (request->independent_batch) dirty_batch_return(r,selected[0]->node,error);
#endif
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

static void write_admission_local(struct write_admission_request *request)
{
    union foreground_workspace *lease=foreground_workspace_take(request->cache);
    struct writeback_workspace work;
    if (lease) {
        work=(struct writeback_workspace){.prepared=lease->writeback.prepared,
            .spans=lease->writeback.spans,.selected=lease->writeback.selected};
        request->workspace=&work;
    }
    write_admission_window(request);
    request->workspace=NULL;
    if (lease) foreground_workspace_put(request->cache,lease);
}
static int write_admission(struct kernel_page_cache *cache,struct kernel_vfs_node *node,int space_pressure)
{
    struct kernel_page_cache_record *r=cache->record;
    if (!r->started || r->stopping) return 0;
    uint64_t available=physical_page_available(cache->allocator);
    if (!space_pressure && available>r->low && !node_pressure(node,r,0)) return 0;
    /* 写入接纳处尚无inode锁、来源pin或cache写借用。不能等到分配器
     * 持有重入保护才等待脏页；executor也不能排队等待自己的后台工作。 */
    struct kernel_io_context *io=kernel_io_context_current();
    if (io->allocation_depth || io->backend_depth || io->reclaim_depth ||
        io->background_reclaim || io->user_pins || io->cache_writes ||
        kernel_cpu_current()->raw_locks) return 0;
    for (struct kernel_lock_guard *guard=io->locks;guard;guard=guard->previous)
        if (guard->lock->rank>15) return 0;
    if (available<r->high) (void)kernel_page_cache_reclaim(cache,r->high-available);
    int executor=kernel_page_cache_is_executor(cache);
    struct kernel_writeback_domain *domain=node->instance->writeback.domain;
    int can_help=!executor && domain && domain->device &&
        physical_page_available(cache->allocator)>=r->high &&
        node->instance->ops->file_operation_execution==KERNEL_VFS_EXECUTION_CALLER;
    int caller_writeback=can_help && domain_bytes(domain)>=domain_limit(domain,0) &&
        __atomic_load_n(&r->group->domain_count,__ATOMIC_RELAXED)>1;
    struct write_admission_request request={.cache=cache,.node=node,.caller_writeback=caller_writeback,
        .space_pressure=space_pressure};
#if BOAROS_MAX_CPUS > 1
    /* Only dirty-data pressure warrants helping here. If pinned versions or
     * journal metadata consume the budget, wait for their actual owner. */
    if (!space_pressure && can_help && !caller_writeback && kernel_cpu_get_boot_count()>1 &&
        __atomic_load_n(&domain->dirty_bytes,__ATOMIC_RELAXED)>=domain_limit(domain,0)) {
        request.caller_writeback=request.independent_batch=1;
        write_admission_local(&request);
        if (request.advanced || request.result) goto completed;
        request.caller_writeback=request.independent_batch=0;
    }
#endif
    if (!space_pressure && !executor && !caller_writeback &&
        physical_page_available(cache->allocator)>=r->high && node_pressure(node,r,0)) {
        /* 额度归还由跨cache的共同owner通知；dirty转为借出的版本不算归还。
         * 后台轮次结束仍不足时保留原有界推进及错误处理。 */
        uint64_t round=0;
        int joined=0;
        for (;;) {
            struct kernel_wait_token token={0};
            int park;
            {
                CACHE_SCOPE(guard,cache);
                if (!r->started || r->stopping || !node_pressure(node,r,0) ||
                    (joined && r->completed!=round)) break;
                if (kernel_wait_prepare(&r->group->progress,0,0,&token)) __builtin_trap();
                if (!joined) {
                    joined=1;round=r->completed;r->requested=1;cache_wake(&r->work);
                }
                /* release不取cache mutex；与归还侧的空队列查询排序后复查。 */
#if BOAROS_MAX_CPUS > 1
                __atomic_thread_fence(__ATOMIC_SEQ_CST);
#else
                __asm__ volatile("" ::: "memory");
#endif
                park=r->started && !r->stopping && node_pressure(node,r,0) && r->completed==round;
            }
            enum kernel_wait_wake_reason reason;
            if ((park && kernel_wait_park(&token,&reason)) || kernel_wait_finish(&token)) __builtin_trap();
        }
        if (physical_page_available(cache->allocator)>r->low && !node_pressure(node,r,0)) return 0;
    }
    /* 这里只借用缓存owner与调用栈；worker不取得调用者的OFD/写操作锁。
     * 保留整次write互斥，仍允许独立worker取得inode后推进一批写回。 */
    if (caller_writeback) write_admission_local(&request);
    else if (!executor) {
        if (run_request(cache,write_admission_window,&request,1,NULL)) request.result=-KERNEL_EIO;
    } else write_admission_window(&request);
#if BOAROS_MAX_CPUS > 1
completed:
#endif
#if BOAROS_PAGE_CACHE_BACKGROUND_PERCENT < 100
    /* quota先等已有提交归还版本；健康running组继续合并。物理余量低于
     * 回收目标线时允许结束running组，避免空等不可回收的版本。
     * 来源pin、entry和inode资格已归还，进度执行者不进入此等待。 */
    struct kernel_vfs_node *progress=request.progress?request.progress:node;
    available=physical_page_available(cache->allocator);
    int memory_pressure=available<r->high;
    if (!request.result && (memory_pressure || node_pressure(node,r,0)) && progress->instance->ops->writeback_reclaim) {
        int error=progress->instance->ops->writeback_reclaim(progress->instance,memory_pressure);
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
    bool mapped_candidate;
    int result,done;
};

enum { RANGE_WRITEBACK_SLOTS = 32, RANGE_COLLECT_REQUESTS = 4 };
#define RANGE_COLLECT_NS UINT64_C(200000)
struct range_writeback {
    struct range_writeback *next;
    struct foreground_writeback window;
    uint64_t first, sequence;
};

static bool can_writeback_mapped(struct kernel_page_cache_entry *entry, uint64_t limit)
{
    if (entry->aliases || entry->writeback || entry_loading(entry) ||
        __atomic_load_n(&entry->writes, __ATOMIC_RELAXED)) return false;
    size_t block = entry->node->instance->writeback_block_size;
    uint64_t start = entry->page_index << BOAROS_PAGE_SHIFT;
    if (!block || entry->dirty_begin % block || entry->dirty_end % block ||
        entry->dirty_end > limit - start) return false;
#if BOAROS_PAGE_CACHE_DIRTY_BLOCKS
    if (dirty_block_size(entry) &&
        entry->dirty_blocks != dirty_block_range(block, entry->dirty_begin, entry->dirty_end))
        return false;
#endif
    return true;
}

static void close_mapped_writers(struct kernel_page_cache_record *record,
    struct kernel_page_cache_entry **entries, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        struct kernel_page_cache_entry *entry = entries[i];
        __atomic_store_n(&entry->writes, CACHE_WRITES_CLOSED, __ATOMIC_RELEASE);
        entry->writeback = 1;
        publish_cache_page_state(entry);
    }
    record->writeback_active += count;
}

static int submit_mapped_writeback(struct kernel_vfs_node *node,
    struct kernel_page_cache_entry **entries, struct file_write_span *spans, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        struct kernel_page_cache_entry *entry = entries[i];
        size_t begin = entry->dirty_begin;
        spans[i] = (struct file_write_span){
            .offset = (entry->page_index << BOAROS_PAGE_SHIFT) + begin,
            .buffer = entry->data + begin, .size = entry->dirty_end - begin};
    }
    int result = node->instance->ops->writeback_mapped(node, spans, count);
    if (result && result != -KERNEL_EOPNOTSUPP)
        kernel_vfs_record_writeback_error(node, result);
    return result;
}

static void finish_mapped_writeback(struct kernel_page_cache *cache,
    struct kernel_page_cache_entry **entries, const struct file_write_span *spans,
    size_t count, int result, bool keep_for_fallback)
{
    for (size_t i = 0; i < count; i++) {
        struct kernel_page_cache_entry *entry = entries[i];
        __atomic_store_n(&entry->writes, 0, __ATOMIC_RELEASE);
        entry->retry_blocked = 0;
        const struct prepared_writeback done = {.begin=entry->dirty_begin,.end=entry->dirty_end,
            .generation=entry->generation,.written=spans[i].written};
        if (result == -KERNEL_EOPNOTSUPP) {
            entry->writeback = 0;
            cache->record->writeback_active--;
            publish_cache_page_state(entry);
            if (__atomic_load_n(&entry->ready.registrations, __ATOMIC_RELAXED))
                cache_wake(&entry->ready);
            if (keep_for_fallback) continue;
        } else (void)complete_writeback(cache, entry, &done, result);
        entry_return(entry);
    }
}

static size_t select_writeback_window(struct foreground_writeback *request, bool *mapped)
{
    struct kernel_page_cache_record *record = request->cache->record;
    struct kernel_page_cache_entry *entry = dirty_at_or_after(request->node, request->cursor);
    size_t count = 0;
    while (entry && entry->page_index <= request->last && count < request->capacity) {
        request->selected[count++] = entry;
        entry_borrow_indexed(entry);
        COST_ADD(WRITEBACK_VISITS,1);
        if (*mapped && !can_writeback_mapped(entry, request->end)) *mapped = false;
        entry = RB_NEXT(kernel_page_cache_dirty_tree,
            &kernel_vfs_node_dirty_pages(request->node)->tree, entry);
    }
    if (count) {
        entry_borrows_add(record, count);
        request->cursor = request->selected[count-1]->page_index + 1;
    }
    if (*mapped) close_mapped_writers(record, request->selected, count);
    return count;
}

static void writeback_window(void *argument)
{
    struct foreground_writeback *request = argument;
    struct kernel_page_cache *cache = request->cache;
    size_t count, used;
    KERNEL_LOCK_SCOPE(io);
    bool mapped = request->mapped_candidate && kernel_vfs_node_writeback_allowed(request->node);
    if (mapped) kernel_vfs_node_lock(request->node, &io, 0);
    {
        CACHE_SCOPE(guard,cache);
        count = select_writeback_window(request, &mapped);
    }
    if (!count) { request->done=1; return; }
    int result = -KERNEL_EOPNOTSUPP;
    unsigned submitted = 0;
    unsigned mapped_windows = request->capacity > BOAROS_PAGE_CACHE_WRITEBACK_PAGES ? 1U : 4U;
    struct file_write_span single_span;
    while (mapped && count) {
        struct file_write_span *spans = request->workspace ? request->workspace->spans : &single_span;
        result = submit_mapped_writeback(request->node, request->selected, spans, count);
        bool completed_batch = !result;
        {
            CACHE_SCOPE(guard,cache);
            finish_mapped_writeback(cache, request->selected, spans, count, result, true);
            /* Finish the current batch and qualify the next with one
             * acquisition. Keep the inode borrow bounded to four batches. */
            if (!result && ++submitted < mapped_windows && request->cursor <= request->last) {
                count = select_writeback_window(request, &mapped);
                if (count && !mapped) result = -KERNEL_EOPNOTSUPP;
                if (!count) request->done = 1;
            } else mapped = false;
        }
        if (completed_batch) group_returned(cache->record->group);
    }
    kernel_lock_scope_release(&io);
    if (result == -KERNEL_EOPNOTSUPP) {
        size_t limit = count;
        if (request->mapped_candidate) {
            limit = BOAROS_PAGE_CACHE_WRITEBACK_PAGES;
            request->capacity = limit;
        }
        request->mapped_candidate = false;
        result = 0;
        for (size_t first = 0; first < count && !result; first += limit) {
            size_t window = count - first;
            if (window > limit) window = limit;
            result=writeback_run(cache,request->selected+first,window,request->end,&used,request->workspace,0);
        }
        {
            CACHE_SCOPE(guard,cache);
            for (size_t i=0;i<count;i++) entry_return(request->selected[i]);
        }
        reclaim_window_memory(cache);
    }
    request->result=result;
    request->done=request->done || request->result || request->cursor>request->last;
}

static void cache_yield(void);

int kernel_page_cache_writeback_range_borrowing(struct kernel_page_cache *cache,
    struct kernel_vfs_node *node,uint64_t start,uint64_t end,
    const struct kernel_lock_guard *borrowed, bool data_only)
{
    if (!cache_live(cache) || !node || start>=end) return -KERNEL_EINVAL;
    if (kernel_lock_held(&node->io_lock,0)) __builtin_trap();
    struct kernel_page_cache_record *r=cache->record;
    uint64_t cursor=start>>BOAROS_PAGE_SHIFT,last=(end-1)>>BOAROS_PAGE_SHIFT;
    uint64_t first;
    size_t dirty;
    struct kernel_page_cache_entry *single=NULL;
    int multiple=0;
    bool prefer_mapped = node->instance->ops->writeback_mapped &&
        node->instance->writeback_block_size &&
        (data_only || node->instance->writeback_block_size < BOAROS_PAGE_SIZE);
    int dispatch=node->instance->ops->file_operation_execution==KERNEL_VFS_EXECUTION_CPU0 &&
        kernel_page_cache_needs_cpu0(cache);
#if BOAROS_MAX_CPUS == 1
    bool several_dirty_files;
#endif
    {
        CACHE_SCOPE(guard,cache);
        struct kernel_page_cache_entry *entry=dirty_at_or_after(node,cursor);
        if (!entry || entry->page_index>last) return 0;
        struct kernel_page_cache_entry *tail=RB_MAX(kernel_page_cache_dirty_tree,&kernel_vfs_node_dirty_pages(node)->tree);
        if (last>tail->page_index) last=tail->page_index;
        struct kernel_page_cache_entry *next=RB_NEXT(kernel_page_cache_dirty_tree,&kernel_vfs_node_dirty_pages(node)->tree,entry);
        multiple=next && next->page_index<=last;
        first=entry->page_index;
        dirty=kernel_vfs_node_dirty_pages(node)->count;
        if (!multiple && !dispatch && (!prefer_mapped || !can_writeback_mapped(entry, end))) {
            /* Transfer the selection's borrow to the scalar consumer. */
            entry_borrow(entry);
            single=entry;
            COST_ADD(WRITEBACK_VISITS,1);
        }
#if BOAROS_MAX_CPUS == 1
        several_dirty_files=r->dirty_head!=r->dirty_tail;
#endif
        request_borrow(r);
    }
    if (single) {
        size_t used;
        int result=writeback_run(cache,&single,1,end,&used,NULL,0);
        {
            CACHE_SCOPE(guard,cache);
            entry_return(single);
        }
        reclaim_window_memory(cache);
        request_return(r);
        return result;
    }
    reclaim_window_memory(cache);
    const size_t large_capacity=(512U * 1024U) / BOAROS_PAGE_SIZE;
    _Static_assert((512U * 1024U) / BOAROS_PAGE_SIZE <= KERNEL_VFS_GROWTH_WRITEBACK_MAX,
        "a growth window must fit the backend span array");
    struct writeback_workspace work={0};
    size_t capacity=multiple?BOAROS_PAGE_CACHE_WRITEBACK_PAGES:1;
    bool growth=false;
    bool large=false;
#if BOAROS_MAX_CPUS == 1
    bool mapped=several_dirty_files && multiple && end==UINT64_MAX &&
        node->instance->ops->writeback_mapped;
#else
    bool mapped=BOAROS_MAX_CPUS > 1 && multiple && end==UINT64_MAX &&
        node->instance->ops->writeback_mapped && kernel_cpu_get_boot_count()>1;
#endif
    if (mapped && large_capacity > capacity && dirty>=large_capacity &&
        last-first>=large_capacity-1 && node->instance->ops->writeback_growth) {
        {
            KERNEL_LOCK_SCOPE(io);
            kernel_vfs_node_lock(node,&io,0);
            growth=node->instance->ops->writeback_growth(node,first<<BOAROS_PAGE_SHIFT);
        }
        if (growth || node->instance->writeback_block_size==BOAROS_PAGE_SIZE)
            large=!writeback_workspace_init(cache,&work,large_capacity);
        if (large) capacity=large_capacity;
    }
    mapped |= prefer_mapped;
    union foreground_workspace *lease=NULL;
    if (multiple && !large) {
        lease=foreground_workspace_take(cache);
        if (!lease) {
            request_return(r);
            return -KERNEL_ENOMEM;
        }
        work=(struct writeback_workspace){.prepared=lease->writeback.prepared,
            .spans=lease->writeback.spans,.selected=lease->writeback.selected};
    }
    struct foreground_writeback request={.cache=cache,.node=node,.cursor=cursor,.last=last,
        .end=end,.selected=multiple?work.selected:&single,.workspace=multiple?&work:NULL,
        .capacity=capacity,
        .mapped_candidate=mapped && !growth};
    /* 前台批次使用调用者的描述符和稳定版本，可与同一 cache 上另一设备的
     * 后台写回并行。只在后端要求 CPU0 时借用执行队列。 */
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
    if (large) writeback_workspace_release(cache,&work);
    request_return(r);
    return request.result;
}

int kernel_page_cache_writeback_range(struct kernel_page_cache *cache,
    struct kernel_vfs_node *node,uint64_t start,uint64_t end)
{ return kernel_page_cache_writeback_range_borrowing(cache,node,start,end,NULL,false); }

int kernel_page_cache_writeback(struct kernel_page_cache *cache, struct kernel_vfs_node *node)
{ return kernel_page_cache_writeback_range(cache, node, 0, UINT64_MAX); }

enum { SMALL_FILE_WRITEBACK_MAX=8 };

static size_t writeback_small_files(struct kernel_page_cache *cache,
    struct kernel_vfs_node **nodes,size_t limit,struct writeback_storage *storage,
    size_t *attempted)
{
    if (limit>SMALL_FILE_WRITEBACK_MAX) limit=SMALL_FILE_WRITEBACK_MAX;
    if (limit>BOAROS_PAGE_CACHE_WRITEBACK_PAGES) limit=BOAROS_PAGE_CACHE_WRITEBACK_PAGES;
    size_t count=0,prepared_count=0,locked=0,accepted=0;
    struct kernel_page_cache_entry **entries=storage->selected;
    struct prepared_writeback *prepared=storage->prepared;
    struct kernel_lock_guard guards[SMALL_FILE_WRITEBACK_MAX]={0};
    unsigned order[SMALL_FILE_WRITEBACK_MAX];
    {
        CACHE_SCOPE(select,cache);
        for (;count<limit;count++) {
            struct kernel_vfs_node *node=nodes[count];
            struct kernel_page_cache_dirty *dirty=kernel_vfs_node_dirty_pages(node);
            if (dirty->count!=1 || kernel_vfs_node_size(node)>BOAROS_PAGE_SIZE ||
                !node->instance->ops->writeback_small_candidate(node)) break;
            struct kernel_page_cache_entry *entry=RB_MIN(kernel_page_cache_dirty_tree,&dirty->tree);
            if (entry->page_index || entry->dirty_begin || entry->aliases || entry->writeback ||
                __atomic_load_n(&entry->writes,__ATOMIC_RELAXED)) break;
            entries[count]=entry;
            entry_borrow_indexed(entry);
        }
        entry_borrows_add(cache->record,count);
    }
    if (count<2) goto Release;
    *attempted=count;
    COST_ADD(WRITEBACK_VISITS,count);
    reclaim_window_memory(cache);
    memset(prepared,0,count*sizeof(*prepared));
    if (prepare_writeback_prefix(cache,entries,count,UINT64_MAX,0,prepared,&prepared_count) ||
        prepared_count!=count) goto Release;
    for (unsigned i=0;i<count;i++) {
        if (!prepared[i].version) goto Release;
        unsigned j=i;
        while (j && nodes[order[j-1]]->io_lock.key>nodes[i]->io_lock.key) {
            order[j]=order[j-1];
            j--;
        }
        order[j]=i;
    }
    for (;locked<count;locked++)
        if (!kernel_vfs_node_try_read(nodes[order[locked]],&guards[locked])) goto Release;
    capture_writeback(cache,entries,prepared,count,UINT64_MAX);
    struct file_write_span *spans=storage->spans;
    for (size_t i=0;i<count;i++) {
        size_t cursor=prepared[i].begin,begin,end;
        if (!next_writeback_span(entries[i],&prepared[i],&cursor,&spans[i]) ||
            next_writeback_range(entries[i],&prepared[i],&cursor,&begin,&end)) goto Release;
    }
    if (nodes[0]->instance->ops->writeback_small_files(nodes,spans,count)) goto Release;
    accepted=count;
    for (size_t i=0;i<count;i++) {
        if (spans[i].written!=spans[i].size) __builtin_trap();
        prepared[i].written=spans[i].written;
    }
Release:
    while (locked) kernel_lock_release(&guards[--locked]);
    if (!accepted) {
        for (size_t i=0;i<prepared_count;i++)
            if (prepared[i].version) writeback_unprepare(cache,entries[i],&prepared[i]);
    }
    {
        CACHE_SCOPE(complete,cache);
        for (size_t i=0;i<count;i++) {
            if (accepted) (void)complete_writeback(cache,entries[i],&prepared[i],0);
            entry_return(entries[i]);
        }
    }
    if (accepted) return_prepared_versions(cache,prepared,accepted,0);
    return accepted;
}

static __attribute__((noinline)) void writeback_mount_pause(void)
{
    if (!kernel_scheduler_can_sleep()) return;
    KERNEL_IRQ_SCOPE(irq);
    /* The batch has returned its inode and cache guards. Deliver pending
     * completions before checking whether the local CPU should yield. */
    arch_interrupt_restore(ARCH_INTERRUPT_ENABLE_MASK);
    (void)arch_interrupt_save();
    if (kernel_cpu_schedule_requested(kernel_cpu_current()) &&
        kernel_scheduler_yield_current()) __builtin_trap();
}

int kernel_page_cache_writeback_mount(struct kernel_page_cache *cache,
    struct kernel_vfs_node **nodes,size_t count)
{
    int result=0;
    union foreground_workspace *lease=NULL;
    bool batch=count>1 && nodes[0]->instance->ops->writeback_small_files &&
        !nodes[0]->instance->namespace_parallel && !kernel_page_cache_needs_cpu0(cache);
    for (size_t i=0;i<count;) {
        size_t attempted=1;
        if (batch && !lease && count-i>1) {
            lease=foreground_workspace_take(cache);
            if (!lease) batch=false;
        }
        size_t accepted=lease?writeback_small_files(cache,nodes+i,count-i,&lease->writeback,&attempted):0;
        if (accepted) i+=accepted;
        else {
            if (lease) {
                foreground_workspace_put(cache,lease);
                lease=NULL;
            }
            for (size_t end=i+attempted;i<end;i++) {
                int error=kernel_page_cache_writeback(cache,nodes[i]);
                if (error && !result) result=error;
            }
        }
        if (i<count) writeback_mount_pause();
    }
    if (lease) foreground_workspace_put(cache,lease);
    return result;
}

static int range_has_dirty(struct kernel_vfs_node *node, uint64_t first, uint64_t last)
{
    struct kernel_page_cache_entry *entry = dirty_at_or_after(node, first);
    return entry && entry->page_index <= last;
}

static void wait_range_progress(struct kernel_wait_token *token)
{
    enum kernel_wait_wake_reason reason;
    if (kernel_wait_park(token, &reason) || kernel_wait_finish(token))
        __builtin_trap();
}

int kernel_page_cache_submit_range(struct kernel_page_cache *cache,
    struct kernel_vfs_node *node, uint64_t start, uint64_t end, bool collect)
{
    struct kernel_page_cache_record *record = cache->record;
    uint64_t first = start >> BOAROS_PAGE_SHIFT;
    uint64_t last = (end - 1) >> BOAROS_PAGE_SHIFT;
    {
        CACHE_SCOPE(check, cache);
        if (!range_has_dirty(node, first, last))
            return 0;
    }
    struct range_writeback *request = NULL;
    if (kernel_heap_allocate_zeroed(cache->heap, 1, sizeof(*request), (void **)&request))
        return -KERNEL_ENOMEM;
    int result = 0;
    for (;;) {
        struct kernel_wait_token token = {0};
        {
            CACHE_SCOPE(admit, cache);
            if (!record->started || record->stopping) {
                result = -KERNEL_EIO;
                break;
            }
            if (!range_has_dirty(node, first, last))
                break;
            if (record->range_count < RANGE_WRITEBACK_SLOTS) {
                if (kernel_vfs_node_acquire(node))
                    __builtin_trap();
                struct kernel_page_cache_entry *tail =
                    RB_MAX(kernel_page_cache_dirty_tree, &kernel_vfs_node_dirty_pages(node)->tree);
                if (last > tail->page_index)
                    last = tail->page_index;
                struct writeback_workspace *work = record->writeback_workspace;
                request->window = (struct foreground_writeback){
                    .cache = cache, .node = node, .cursor = first, .last = last,
                    .end = end, .selected = work->selected, .workspace = work,
                    .capacity = record->writeback_capacity,
                    .mapped_candidate = node->instance->ops->writeback_mapped != NULL,
                };
                request->first = first;
                request->sequence = ++record->range_sequence;
                if (!request->sequence) __builtin_trap();
                bool starts_queue = !record->range_head;
                if (starts_queue && collect && request->window.mapped_candidate &&
                    node->instance->writeback_block_size < BOAROS_PAGE_SIZE &&
                    kernel_vfs_node_dirty_pages(node)->count > last - first + 1) {
                    /* A series of asynchronous hints can publish before the
                     * worker takes the inode and starts a device batch. */
                    record->range_collect_until = kernel_time_monotonic_ns() + RANGE_COLLECT_NS;
                }
                *record->range_tail = request;
                record->range_tail = &request->next;
                record->range_count++;
                if (record->range_collect_until && (!collect || record->range_head->window.node != node ||
                    record->range_count >= RANGE_COLLECT_REQUESTS))
                    record->range_collect_until = 0;
                if (starts_queue || !record->range_collect_until) cache_wake(&record->work);
                return 0;
            }
            if (kernel_wait_prepare(&record->range_progress, 0, 0, &token))
                __builtin_trap();
        }
        wait_range_progress(&token);
    }
    kernel_heap_release(cache->heap, request);
    return result;
}

void kernel_page_cache_wait_range(struct kernel_page_cache *cache,
    struct kernel_vfs_node *node, uint64_t start, uint64_t end)
{
    struct kernel_page_cache_record *record = cache->record;
    uint64_t first = start >> BOAROS_PAGE_SHIFT;
    uint64_t last = (end - 1) >> BOAROS_PAGE_SHIFT;
    uint64_t sequence = 0;
    int sampled = 0;
    for (;;) {
        struct kernel_wait_token token = {0};
        struct kernel_page_cache_entry *inflight = NULL;
        {
            CACHE_SCOPE(check, cache);
            if (!sampled) {
                sequence = record->range_sequence;
                sampled = 1;
            }
            struct range_writeback *item = record->range_head;
            for (; item && item->sequence <= sequence; item = item->next)
                if (item->window.node == node && item->first <= last && item->window.last >= first)
                    break;
            if (item && item->sequence <= sequence) {
                if (record->range_collect_until) {
                    record->range_collect_until = 0;
                    cache_wake(&record->work);
                }
                if (kernel_wait_prepare(&record->range_progress, 0, 0, &token))
                    __builtin_trap();
            } else {
                if (!record->writeback_active)
                    return;
                struct kernel_page_cache_entry *entry = dirty_at_or_after(node, first);
                unsigned inspected = 0;
                while (entry && entry->page_index <= last && !entry->writeback &&
                       inspected++ < BOAROS_PAGE_CACHE_WRITEBACK_PAGES)
                    entry = RB_NEXT(kernel_page_cache_dirty_tree, &kernel_vfs_node_dirty_pages(node)->tree, entry);
                if (!entry || entry->page_index > last)
                    return;
                if (!entry->writeback) {
                    first = entry->page_index;
                    continue;
                }
                inflight = entry;
                entry_borrow(inflight);
                if (kernel_wait_prepare(&entry->ready, 0, 0, &token))
                    __builtin_trap();
            }
        }
        wait_range_progress(&token);
        if (inflight) {
            CACHE_SCOPE(release, cache);
            first = inflight->page_index;
            entry_return(inflight);
        }
    }
}

static void retire_range_requests(struct kernel_page_cache *cache,
    struct range_writeback *first, struct range_writeback *last, unsigned count)
{
    struct kernel_page_cache_record *record = cache->record;
    {
        CACHE_SCOPE(complete, cache);
        if (record->range_head != first || record->range_count < count)
            __builtin_trap();
        record->range_head = last->next;
        if (!record->range_head) record->range_tail = &record->range_head;
        last->next = NULL;
        record->range_count -= count;
        cache_wake(&record->range_progress);
    }
    while (first) {
        struct range_writeback *next = first->next;
        if (kernel_vfs_node_release(&first->window.node)) __builtin_trap();
        kernel_heap_release(cache->heap, first);
        first = next;
    }
}

static struct range_writeback *run_mapped_range_batch(struct kernel_page_cache *cache,
    struct range_writeback *first, unsigned *completed)
{
    struct kernel_vfs_node *node = first->window.node;
    if (!kernel_vfs_node_writeback_allowed(node)) return NULL;
    struct kernel_page_cache_record *record = cache->record;
    struct writeback_workspace *work = record->writeback_workspace;
    struct range_writeback *last = NULL;
    unsigned requests = 0;
    size_t pages = 0;
    KERNEL_LOCK_SCOPE(io);
    kernel_vfs_node_lock(node, &io, 0);
    {
        CACHE_SCOPE(select, cache);
        /* Admit only complete requests from the head, retaining the exact
         * range boundaries. A page already selected by another request or
         * a page needing the journal ends this device batch. */
        for (struct range_writeback *item = first; item && item->window.node == node &&
            item->window.mapped_candidate && requests < BOAROS_PAGE_CACHE_WRITEBACK_PAGES;
            item = item->next) {
            size_t selected = pages;
            struct kernel_page_cache_entry *entry = dirty_at_or_after(node, item->window.cursor);
            while (entry && entry->page_index <= item->window.last) {
                if (selected == record->writeback_capacity ||
                    (selected && work->selected[selected-1]->page_index >= entry->page_index) ||
                    !can_writeback_mapped(entry, item->window.end)) break;
                work->selected[selected++] = entry;
                entry = RB_NEXT(kernel_page_cache_dirty_tree,
                    &kernel_vfs_node_dirty_pages(node)->tree, entry);
            }
            if (entry && entry->page_index <= item->window.last) break;
            pages = selected;
            last = item;
            requests++;
        }
        if (requests < 2) return NULL;
        for (size_t i = 0; i < pages; i++) entry_borrow_indexed(work->selected[i]);
        if (pages) {
            COST_ADD(WRITEBACK_VISITS, pages);
            entry_borrows_add(record, pages);
            close_mapped_writers(record, work->selected, pages);
        }
    }
    int result = 0;
    if (pages) {
        result = submit_mapped_writeback(node, work->selected, work->spans, pages);
        {
            CACHE_SCOPE(complete, cache);
            finish_mapped_writeback(cache, work->selected, work->spans, pages, result, false);
        }
        if (!result) group_returned(record->group);
    }
    if (result == -KERNEL_EOPNOTSUPP) return NULL;
    *completed = requests;
    return last;
}

static void run_range_writeback(struct kernel_page_cache *cache,
    struct range_writeback *request, int stopping, bool batch)
{
    struct kernel_page_cache_record *record = cache->record;
    if (!stopping) {
        struct kernel_io_context *io = kernel_io_context_current();
        unsigned previous = io->background_reclaim;
        io->background_reclaim = 1;
        if (batch) {
            unsigned completed = 0;
            struct range_writeback *last = run_mapped_range_batch(cache, request, &completed);
            if (last) {
                io->background_reclaim = previous;
                retire_range_requests(cache, request, last, completed);
                return;
            }
        }
        uint64_t first = request->window.cursor;
        writeback_window(&request->window);
        if (!request->window.result && !request->window.mapped_candidate && request->window.cursor > first) {
            uint64_t end = request->window.cursor << BOAROS_PAGE_SHIFT;
            if (end > request->window.end) end = request->window.end;
            request->window.result = kernel_vfs_node_complete_data_range(request->window.node,
                first << BOAROS_PAGE_SHIFT, end);
            if (request->window.result) request->window.done = 1;
        }
        io->background_reclaim = previous;
    } else {
        request->window.result = -KERNEL_EIO;
        request->window.done = 1;
    }
    if (!request->window.done) {
        CACHE_SCOPE(progress, cache);
        request->first = request->window.cursor;
        cache_wake(&record->range_progress);
        return;
    }
    /* Backend failures already have a publication in kernel_vfs_node_writeback.
     * Failures before a submission still need a completion for a later wait. */
    if (stopping || request->window.result == -KERNEL_ENOMEM ||
        request->window.result == -KERNEL_EBUSY)
        kernel_vfs_record_writeback_error(request->window.node, request->window.result);
    retire_range_requests(cache, request, request, 1);
}

int kernel_page_cache_pin_dirty_nodes(struct kernel_page_cache *cache,
    const struct kernel_vfs_instance *instance, struct kernel_vfs_node **nodes,
    size_t capacity, size_t *count, size_t *required)
{
    if (!cache_live(cache)) return -KERNEL_EINVAL;
    CACHE_SCOPE(guard, cache);
    for (struct kernel_page_cache_dirty *dirty = cache->record->dirty_head;
         dirty; dirty = dirty->queue_next) {
        /* A queued inode owns at least one dirty entry and that entry pins
         * the inode. An in-flight write stays dirty through backend acceptance. */
        struct kernel_vfs_node *node = RB_MIN(kernel_page_cache_dirty_tree, &dirty->tree)->node;
        if (node->instance != instance) continue;
        (*required)++;
        if (*count == capacity) continue;
        int result = kernel_vfs_node_acquire(node);
        if (result) return result;
        nodes[(*count)++] = node;
    }
    return *required > capacity;
}

int kernel_page_cache_resize_prepare(struct kernel_page_cache *cache,struct kernel_vfs_node *node,
    uint64_t old_size,uint64_t target,struct kernel_page_cache_resize *token)
{
    if (!token || token->entry || !cache_live(cache)) return -KERNEL_EINVAL;
    uint64_t boundary=old_size<target?old_size:target;
    if (old_size==target || !(boundary&BOAROS_PAGE_MASK)) return 0;
    if (kernel_lock_held(&node->io_lock,0) || kernel_cpu_current()->raw_locks) __builtin_trap();
    struct kernel_page_cache_entry *entry;
    {
        CACHE_SCOPE(guard,cache);
        entry=index_find(node,boundary>>BOAROS_PAGE_SHIFT);
        if (!entry) return 0;
        entry_borrow(entry);
    }
    for (struct kernel_page_cache_write_borrow *b=kernel_io_context_current()->cache_writes;b;b=b->next)
        if (b->entry==entry) { CACHE_SCOPE(guard,cache); entry_return(entry); return -KERNEL_EBUSY; }
    struct kernel_page_cache_alias *aliases=NULL;
    for (;;) {
        struct kernel_wait_token wait={0};
        int park;
        {
            CACHE_SCOPE(guard,cache);
            if (!entry->writeback && !entry_loading(entry) && !(__atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)&(CACHE_WRITES_CLOSED | CACHE_ENTRY_CLOSED))) {
                __atomic_fetch_or(&entry->writes,CACHE_WRITES_CLOSED | CACHE_ENTRY_CLOSED,__ATOMIC_ACQ_REL);
                aliases=entry->aliases;alias_borrow(aliases);break;
            }
            if (kernel_io_context_current()->user_pins) {
                entry_return(entry);return -KERNEL_EBUSY;
            }
            if (kernel_wait_prepare(&entry->ready,0,0,&wait)) __builtin_trap();
            park = entry->writeback || entry_loading(entry) ||
                (__atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)&(CACHE_WRITES_CLOSED | CACHE_ENTRY_CLOSED));
        }
        enum kernel_wait_wake_reason reason;
        if (park && kernel_wait_park(&wait,&reason)) __builtin_trap();
        if (kernel_wait_finish(&wait)) __builtin_trap();
    }
    int result=rearm_aliases(entry,aliases);
    if (result) {
        CACHE_SCOPE(guard,cache);
        __atomic_fetch_and(&entry->writes,~(CACHE_WRITES_CLOSED | CACHE_ENTRY_CLOSED),__ATOMIC_RELEASE);
        entry_return(entry); cache_wake(&entry->ready); return result;
    }
    for (;;) {
        struct kernel_wait_token wait={0};
        {
            CACHE_SCOPE(guard,cache);
            if (!(__atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)&UINT32_MAX)) break;
            if (kernel_io_context_current()->user_pins) {
                __atomic_fetch_and(&entry->writes,~(CACHE_WRITES_CLOSED | CACHE_ENTRY_CLOSED),__ATOMIC_RELEASE);
                entry_return(entry);cache_wake(&entry->ready);return -KERNEL_EBUSY;
            }
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
        size_t begin=boundary&BOAROS_PAGE_MASK;
        size_t end=actual<token->old_size?BOAROS_PAGE_SIZE:
            actual-boundary<BOAROS_PAGE_SIZE-begin?begin+(size_t)(actual-boundary):BOAROS_PAGE_SIZE;
        memset(entry->data+begin,0,end-begin);
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
        if (__atomic_exchange_n(&entry->writes,0,__ATOMIC_RELEASE)!=(CACHE_WRITES_CLOSED | CACHE_ENTRY_CLOSED)) __builtin_trap();
        entry_return(entry); cache_wake(&entry->ready);
    }
    *token=(struct kernel_page_cache_resize){0};
}





void kernel_page_cache_truncate(struct kernel_page_cache *cache,
    struct kernel_vfs_node *node, uint64_t size)
{
    if (cache_live(cache)) readahead_cancel_node(cache, node);
    {
        uint64_t cancelled = 0;
        {
            CACHE_SCOPE(guard,cache);
            uint64_t first=(size>>BOAROS_PAGE_SHIFT)+!!(size&BOAROS_PAGE_MASK);
            shadow_forget_range(cache->record, node, size >> BOAROS_PAGE_SHIFT, UINT64_MAX);
            struct kernel_page_cache_entry *entry=index_at_or_after(node,first);
            while (entry) {
                struct kernel_page_cache_entry *next=RB_NEXT(kernel_page_cache_tree,&node->cache_pages,entry);
                /* The old writeback borrower observes retirement before capture. */
                if (entry->dirty_end) cancelled += BOAROS_PAGE_SIZE;
                remove_entry(cache,entry);
                cache_wake(&entry->ready);
                entry=next;
            }
        }
        if (cancelled) kernel_task_account_cancelled_write(cancelled);
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
        shadow_forget_range(cache->record, node, first, last);
        struct kernel_page_cache_entry *entry=index_at_or_after(node,first);
        while (entry && entry->page_index<=last) {
            struct kernel_page_cache_entry *next=RB_NEXT(kernel_page_cache_tree,&node->cache_pages,entry);
            if (entry->dirty_end) __builtin_trap();
            remove_entry(cache,entry);cache_wake(&entry->ready);
            entry=next;
        }
    }
    uint64_t released = 0;
    (void)drain_entries(cache,&released);
}

static int retire_clean_page(struct kernel_page_cache *cache,struct kernel_page_cache_entry *entry,int second_chance)
{
    {
        CACHE_INDEX_SCOPE(index,entry->node);
#if BOAROS_MAX_CPUS > 1
        if (second_chance && __atomic_exchange_n(&entry->accessed,0,__ATOMIC_RELAXED)) return 0;
#else
        (void)second_chance;
#endif
        uint32_t references;
        if (entry->dirty_end || entry_loading(entry) || entry->writeback || entry_users(entry) ||
            entry->aliases || __atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE) ||
            physical_page_reference_count(cache->allocator,entry->physical_address,&references) || references!=1)
            return 0;
        entry_borrow(entry);
        retire_index_entry(entry);
    }
    remove_retired_entry(cache,entry);
    return 1;
}
static uint64_t release_reclaimed_pages(struct kernel_page_cache *cache,struct kernel_page_cache_entry *retired)
{
    uint64_t released=0;
    for (struct kernel_page_cache_entry *entry=retired;entry;entry=entry->reclaim_next) {
        if (physical_page_release(cache->allocator,entry->physical_address)) __builtin_trap();
        released++;
    }
    if (retired) {
        CACHE_SCOPE(guard,cache);
        while (retired) {
            struct kernel_page_cache_entry *entry=retired;retired=entry->reclaim_next;
            entry->physical_address=0;entry->data=NULL;
            entry->page_references_owned=0;entry->reclaim_next=NULL;entry_return(entry);
        }
    }
    return released;
}
void kernel_page_cache_dontneed(struct kernel_page_cache *cache,struct kernel_vfs_node *node,
    uint64_t first,uint64_t last)
{
    if (!cache_live(cache) || !node || first>last) return;
    uint64_t cursor=first,released=0;
    for (;;) {
        struct kernel_page_cache_entry *retired=NULL;
        unsigned scanned=0;int done;
        {
            CACHE_SCOPE(guard,cache);
            struct kernel_page_cache_entry *entry=index_at_or_after(node,cursor);
            while (entry && entry->page_index<=last && scanned++<64) {
                struct kernel_page_cache_entry *next=RB_NEXT(kernel_page_cache_tree,&node->cache_pages,entry);
                cursor=entry->page_index+1;
                if (retire_clean_page(cache,entry,0)) {entry->reclaim_next=retired;retired=entry;}
                entry=next;
            }
            done=!entry || entry->page_index>last;
        }
        released+=release_reclaimed_pages(cache,retired);
        if (done) break;
    }
    { CACHE_SCOPE(guard,cache);
      cache->record->statistics.pages_reclaimed+=released;
      if (cache->record->started && cache->record->cleanup_entries) {
          cache->record->requested=1;cache_wake(&cache->record->work);
      }
    }
    if (!remote_cache_client()) { uint64_t extra=0;(void)drain_entries(cache,&extra); }
}

static uint64_t reclaim_clean_pages(struct kernel_page_cache *cache,uint64_t target_pages,
    int second_chance)
{
    struct kernel_page_cache_record *r=cache->record;
    uint64_t released=0,scanned=0,limit;
    {
        CACHE_SCOPE(guard,cache);
        r->statistics.reclaim_calls++;
        limit=r->statistics.current_pages;
#if BOAROS_MAX_CPUS > 1
        /* A referenced page gets one chance; if all pages were used, reclaim
         * may still complete a full round on the next pass. */
        if (second_chance) limit*=2;
#endif
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
                if (!retire_clean_page(cache,entry,second_chance)) continue;
                if (second_chance)
                    shadow_record_reclaim(r, entry->node, entry->page_index);
                entry->reclaim_next=retired;retired=entry;selected++;
            }
            if (!window) break;
        }
        /* The entry borrow spans physical release outside cache metadata. */
        released+=release_reclaimed_pages(cache,retired);
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

uint64_t kernel_page_cache_reclaim(struct kernel_page_cache *cache,uint64_t target_pages)
{
    if (!cache_live(cache) || !target_pages) return 0;
    uint64_t released=version_pool_reclaim(cache->record->versions,target_pages);
    if (released>=target_pages) return released;
    struct kernel_lock_guard *held=kernel_io_context_current()->locks;
    if (held && held->lock->rank>=cache->record->metadata.lock.rank) return released;
    released+=reclaim_clean_pages(cache,target_pages-released,1);
    if (released<target_pages)
        released+=kernel_vfs_reclaim_clean(cache->allocator,target_pages-released);
    return released;
}

void kernel_page_cache_drop_clean(struct kernel_page_cache *cache)
{
    if (cache_live(cache)) (void)reclaim_clean_pages(cache,UINT64_MAX,0);
}

static uint64_t cache_low_watermark(const struct physical_page_allocator *allocator)
{
    uint64_t low=physical_page_total(allocator)/50;
    return low?low:1;
}

static void group_notify_available(struct page_cache_group *g,uint64_t available)
{
    uint64_t dirty=group_dirty(g);
    /* All started members use this physical low watermark. A clean group
     * above it cannot request work in either loop. A concurrent new dirty
     * publisher sends its own notification after publishing the counters. */
    if (!dirty && available>cache_low_watermark(g->allocator)) return;
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

static void group_notify(void *context)
{
    struct page_cache_group *g=context;
    if (kernel_io_context_current()->background_reclaim) return;
    group_notify_available(g,physical_page_available(g->allocator));
}

static void group_allocation_notify(void *context, uint64_t available)
{
    if (!kernel_io_context_current()->background_reclaim)
        group_notify_available(context, available);
}

static int group_allocation_needs_notify(void *context, uint64_t available)
{
    struct page_cache_group *group = context;
    return available <= cache_low_watermark(group->allocator) || group_dirty(group);
}

static void notify_dirty_node(struct kernel_page_cache *cache,struct kernel_vfs_node *node)
{
    if (kernel_io_context_current()->background_reclaim) return;
    struct kernel_page_cache_record *r=cache->record;
    struct page_cache_group *g=r->group;
    struct kernel_writeback_domain *domain=node->instance->writeback.domain;
    uint64_t available=physical_page_available(g->allocator);
    if (!domain || !r->started || available<=r->low) {
        group_notify_available(g,available);
        return;
    }
    /* 文件引用保活所属domain。此次标脏只增加该后备域；正常水位无需
     * 取得全组锁或遍历无关挂载。低内存仍由全组共同推进。 */
    if (!__atomic_load_n(&domain->dirty_bytes,__ATOMIC_RELAXED) ||
        domain_bytes(domain)<background_limit(domain_limit(domain,0))) return;
    KERNEL_RAW_SCOPE(guard,&g->metadata);
    for (struct kernel_writeback_owner *owner=domain->members;owner;owner=owner->next) {
        struct kernel_page_cache_record *member=owner->cache->record;
        if (!member->started || member->stopping) continue;
        member->requested=1;cache_wake(&member->work);
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

static void write_dirty_batch(struct kernel_page_cache *cache,
    struct kernel_page_cache_entry **batch,size_t count,struct writeback_workspace *workspace)
{
    struct kernel_page_cache_record *r=cache->record;
    size_t used;
    int result=writeback_run(cache,batch,count,UINT64_MAX,&used,workspace,1);
    {
        CACHE_SCOPE(guard,cache);
        if (!result) r->statistics.worker_written+=used;
        else if (result!=-KERNEL_EBUSY && result!=-KERNEL_ENOMEM) r->statistics.worker_failed+=used;
#if BOAROS_MAX_CPUS > 1
        dirty_batch_return(r,batch[0]->node,result);
#endif
        for (size_t i=0;i<count;i++) entry_return(batch[i]);
    }
    reclaim_window_memory(cache);
}

/* The main worker accounts for both writebacks and wakes pressure waiters
 * only after every batch in this round has returned its entry loans. */
static void complete_writeback_round(struct kernel_page_cache *cache,uint64_t before)
{
    struct kernel_page_cache_record *r=cache->record;
    if (!r->dirty_pages) r->force_writeback=0;
    r->completed++;
    group_complete(r,physical_page_available(cache->allocator)>before);
}

#if BOAROS_USER_SMP
static void independent_writeback_worker(void *argument)
{
    struct writeback_lane *lane=argument;
    struct kernel_page_cache *cache=lane->cache;
    struct kernel_io_context *io=kernel_io_context_current();
    io->background_reclaim=1;
    for (;;) {
        struct kernel_wait_token token={0};
        size_t count;
        {
            CACHE_SCOPE(guard,cache);
            count=lane->count;
            if (!count) {
                if (cache->record->stopping) break;
                if (kernel_wait_prepare(&lane->work,0,0,&token)) __builtin_trap();
            }
        }
        if (!count) {
            enum kernel_wait_wake_reason reason;
            if (kernel_wait_park(&token,&reason) || kernel_wait_finish(&token)) __builtin_trap();
            continue;
        }
        write_dirty_batch(cache,lane->batch,count,&lane->workspace);
        {
            CACHE_SCOPE(guard,cache);
            lane->count=0;
            if (lane->round_waiting) cache_wake(&cache->record->work);
        }
    }
    io->background_reclaim=0;
}
#endif

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
#if BOAROS_MAX_CPUS > 1
            if (count) kernel_vfs_node_dirty_pages(batch[0]->node)->batch_state=DIRTY_BATCH_OWNED;
#endif
            if (!count && (!r->dirty_cursor || !r->dirty_remaining)) break;
#if BOAROS_USER_SMP
            if (count && r->lane && !r->lane->count && r->dirty_head!=r->dirty_tail &&
                batch[0]->node->instance->ops->file_operation_execution==KERNEL_VFS_EXECUTION_CALLER) {
                memcpy(r->lane->batch,batch,count*sizeof(*batch));
                r->lane->count=count;
                cache_wake(&r->lane->work);
                count=0;
            }
#endif
        }
        if (count) write_dirty_batch(cache,batch,count,r->writeback_workspace);
        {
            CACHE_SCOPE(guard,cache);
            r->statistics.worker_batches++;
            if (r->reads_head || r->range_head) {
                r->requested=1;interrupted=1;
                break;
            }
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
#if BOAROS_USER_SMP
        if (r->lane && r->lane->count) {
            r->lane->round_start_pages=before;
            r->lane->round_waiting=1;
            return;
        }
#endif
        complete_writeback_round(cache,before);
    }
}
static void page_cache_worker(void *argument)
{
    struct kernel_page_cache *cache=argument;
    struct kernel_page_cache_record *r = cache->record;
    __atomic_store_n(&r->executor_io,kernel_io_context_current(),__ATOMIC_RELEASE);
    unsigned reads=0;
    int prefer_range=1;
    struct kernel_page_cache_entry *cleanup_entries=NULL;
    for (;;) {
        struct cache_request *request = 0;
        struct range_writeback *range = NULL;
        bool batch_range = false;
        struct kernel_wait_token token = {0};
        int cleanup = 0, stopping=0, park=1, waiting_batch=0;
        uint64_t deadline = 0;
        {
            CACHE_SCOPE(check, cache);
            stopping=r->stopping;
#if BOAROS_USER_SMP
            /* Only this worker finishes rounds. While the dispatched batch
             * owns its loans, continue serving queued foreground requests. */
            if (r->lane && r->lane->round_waiting) {
                if (r->lane->count) waiting_batch=1;
                else {
                    r->lane->round_waiting=0;
                    complete_writeback_round(cache,r->lane->round_start_pages);
                }
            }
#endif
#if BOAROS_PAGE_CACHE_DIRTY_AGE
            if (!stopping && r->dirty_deadline && kernel_time_monotonic_ns()>=r->dirty_deadline) {
                r->dirty_deadline=0;r->expired_writeback=1;r->requested=1;
            }
#endif
            if (r->range_collect_until && !stopping && !r->requested &&
                kernel_time_deadline_from_monotonic(r->range_collect_until, &deadline) != KERNEL_TIME_STATUS_OK)
                r->range_collect_until = deadline = 0;
            if (r->range_head && !deadline && (prefer_range || !r->requested || waiting_batch) &&
                (!r->reads_head || reads >= 16)) {
                range = r->range_head;
                r->range_collect_until = 0;
                batch_range = range->window.mapped_candidate && range->next &&
                    range->next->window.node == range->window.node;
                reads = 0;
            } else if (r->requested && !waiting_batch && (reads>=16 || !r->reads_head)) {
                r->requested=0; cleanup=1; reads=0;
            } else if (r->reads_head) {
                request = r->reads_head; r->reads_head = request->next;
                if (!r->reads_head) r->reads_tail = &r->reads_head;
                r->reads_running++; reads++;
            } else if (cleanup_entries) cleanup=2;
            else if (r->stopping && !waiting_batch) break;
            else {
#if BOAROS_PAGE_CACHE_DIRTY_AGE
                if (r->dirty_deadline) {
                    uint64_t dirtied;
                    enum kernel_time_status status=kernel_time_deadline_from_monotonic(r->dirty_deadline,&dirtied);
                    if (status==KERNEL_TIME_STATUS_DEADLINE_PASSED) {
                        r->dirty_deadline=0;r->expired_writeback=1;r->requested=1;continue;
                    }
                    if (status!=KERNEL_TIME_STATUS_OK) __builtin_trap();
                    if (!deadline || dirtied < deadline) deadline = dirtied;
                }
#endif
                if (kernel_wait_prepare(&r->work,deadline,0,&token)) __builtin_trap();
                /* Pressure publishers do not take the cache mutex. With the
                 * group lock, either we see their request or they see the
                 * registration before testing for an empty wait queue. */
#if BOAROS_MAX_CPUS > 1
                KERNEL_RAW_SCOPE(group,&r->group->metadata);
#endif
                park=waiting_batch || !r->requested;
            }
        }
        if (range) {
            prefer_range=0;
            run_range_writeback(cache, range, stopping, batch_range);
            cache_yield();
            continue;
        }
        if (!request) {
            if (cleanup) {
                if (!cleanup_entries) take_cleanup_entries(cache,&cleanup_entries);
                uint64_t released=0;
                (void)drain_cleanup_entries(cache,&cleanup_entries,16,&released);
                if (cleanup==1 && !stopping) {
                    prefer_range=1;
                    cache_background(cache);
                }
                cache_yield();
                continue;
            }
            enum kernel_wait_wake_reason reason;
            if ((park && kernel_wait_park(&token, &reason)) || kernel_wait_finish(&token)) __builtin_trap();
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
                COST_SAMPLE(CACHE_QUEUE_NS,queued);
                COST_SAMPLE(CACHE_EXECUTE_NS,elapsed);
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



int kernel_page_cache_start_executor(struct kernel_page_cache *cache)
{
    if (!cache_live(cache) || cache->record->started) return -KERNEL_EINVAL;
    struct kernel_page_cache_record *r = cache->record;
    if (kernel_heap_allocate(cache->heap,sizeof(*r->writeback_workspace),
        (void **)&r->writeback_workspace)) return -KERNEL_ENOMEM;
    if (writeback_workspace_init(cache,r->writeback_workspace,BOAROS_PAGE_CACHE_WRITEBACK_PAGES)) {
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
    r->low=cache_low_watermark(cache->allocator);
    r->high = total / 25 > r->low ? total / 25 : r->low + 1;
    r->dirty_low = total / 20 ? total / 20 : 1;
    r->dirty_high = total / 8 > r->dirty_low ? total / 8 : r->dirty_low + 1;
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
    kernel_page_cache_notify_pressure(cache);
    return 0;
}

/* The caller owns mount startup. Its task can join the published threads on
 * failure, and a secondary target has already entered the scheduler. */
int kernel_page_cache_start_background(struct kernel_page_cache *cache)
{
    if (!cache_live(cache) || !cache->record->started || cache->record->stopping)
        return -KERNEL_EINVAL;
#if BOAROS_PAGE_CACHE_READAHEAD_PAGES || BOAROS_USER_SMP
    struct kernel_page_cache_record *r=cache->record;
    enum kernel_scheduler_status result;
#endif
#if BOAROS_PAGE_CACHE_READAHEAD_PAGES
    if (r->readahead_started) return -KERNEL_EINVAL;
    kernel_wait_queue_init(&r->readahead_work);
    result = kernel_thread_create_joinable(readahead_worker, cache, &r->readahead_worker);
    if (result != KERNEL_SCHEDULER_STATUS_OK) {
        kernel_page_cache_stop_worker(cache);
        return result == KERNEL_SCHEDULER_STATUS_NO_MEMORY ? -KERNEL_ENOMEM : -KERNEL_EINVAL;
    }
    { CACHE_SCOPE(publish, cache); r->readahead_started = 1; }
#endif
#if BOAROS_USER_SMP
    unsigned cpus=kernel_cpu_get_boot_count();
    if (cpus>1) {
        if (r->lane) return -KERNEL_EINVAL;
        struct writeback_lane *lane=NULL;
        if (kernel_heap_allocate_zeroed(cache->heap,1,sizeof(*lane),(void **)&lane) ||
            writeback_workspace_init(cache,&lane->workspace,BOAROS_PAGE_CACHE_WRITEBACK_PAGES)) {
            if (lane) kernel_heap_release(cache->heap,lane);
            kernel_page_cache_stop_worker(cache);
            return -KERNEL_ENOMEM;
        }
        lane->cache=cache;
        kernel_wait_queue_init(&lane->work);
        result=kernel_thread_create_joinable_worker(independent_writeback_worker,lane,&lane->worker,cpus-1);
        if (result) {
            writeback_workspace_release(cache,&lane->workspace);
            kernel_heap_release(cache->heap,lane);
            kernel_page_cache_stop_worker(cache);
            return result==KERNEL_SCHEDULER_STATUS_NO_MEMORY ? -KERNEL_ENOMEM : -KERNEL_EIO;
        }
        CACHE_SCOPE(publish,cache);
        r->lane=lane;
    }
#endif
    return 0;
}

int kernel_page_cache_start_worker(struct kernel_page_cache *cache)
{
    int error=kernel_page_cache_start_executor(cache);
    return error ? error : kernel_page_cache_start_background(cache);
}

void kernel_page_cache_stop_worker(struct kernel_page_cache *cache)
{
    if (!cache_live(cache) || !cache->record->started) return;
    struct kernel_page_cache_record *r = cache->record;
    { CACHE_SCOPE(stop, cache); r->stopping = 1; r->dirty_cursor=NULL; r->dirty_remaining=0; }
    cache_wake(&r->group->progress);
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
#if BOAROS_USER_SMP
    if (r->lane) cache_wake(&r->lane->work);
#endif
    kernel_thread_join(&r->worker);
#if BOAROS_USER_SMP
    if (r->lane) {
        kernel_thread_join(&r->lane->worker);
        if (r->lane->count) __builtin_trap();
        writeback_workspace_release(cache,&r->lane->workspace);
        kernel_heap_release(cache->heap,r->lane);
        r->lane=NULL;
    }
#endif
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
                if (!entry || (mount && kernel_vfs_node_mount(entry->node)->private_data!=mount->private_data)) continue;
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
            if (kernel_vfs_node_mount(entry->node)->private_data==mount->private_data) {
                int busy;
                {
                    CACHE_INDEX_SCOPE(index,entry->node);
                    uint32_t references;
                    if (physical_page_reference_count(cache->allocator,entry->physical_address,&references)) __builtin_trap();
                    busy=references!=1 || entry_users(entry) || entry_loading(entry) || entry->writeback ||
                        entry->aliases || entry->dirty_end || __atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE);
                    if (!busy) retire_index_entry(entry);
                }
                if (busy) pinned=1;
                else remove_retired_entry(cache,entry);
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
#if BOAROS_MAX_CPUS > 1
            {
                CACHE_INDEX_SCOPE(index,node);
                node->cache_index_closed=1;
            }
#endif
            int busy=0;
            RB_FOREACH(entry,kernel_page_cache_tree,&node->cache_pages) {
                if (entry_loading(entry) || entry->writeback || entry_users(entry) || entry->aliases ||
                    __atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)) { busy=1;break; }
            }
            if (!busy) {
                shadow_forget_range(cache->record, node, 0, UINT64_MAX);
                while ((entry=RB_MIN(kernel_page_cache_tree,&node->cache_pages)))
                    remove_entry(cache,entry);
            }
#if BOAROS_MAX_CPUS > 1
            {
                CACHE_INDEX_SCOPE(index,node);
                node->cache_index_closed=0;
            }
#endif
            if (busy) return KERNEL_PAGE_CACHE_STATUS_STATE;
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
#if BOAROS_MAX_CPUS > 1
        statistics->hits=__atomic_load_n(&cache->record->read_hits,__ATOMIC_RELAXED);
#endif
    }
}

void kernel_page_cache_range_statistics(struct kernel_page_cache *cache,
    struct kernel_vfs_node *node, uint64_t first, uint64_t last,
    struct kernel_page_cache_range_statistics *stats)
{
    *stats = (struct kernel_page_cache_range_statistics){0};
    if (first > last) return;
    struct kernel_page_cache_record *record = cache->record;
    uint64_t query_generation;
    {
        CACHE_SCOPE(guard, cache);
        query_generation = record->reclaim_generation;
    }
    uint64_t index = first;
    for (;;) {
        CACHE_SCOPE(guard, cache);
        struct kernel_page_cache_entry *entry = index_at_or_after(node, index);
        for (unsigned scanned = 0; entry && entry->page_index <= last; ) {
            stats->cached++;
            stats->dirty += entry->dirty_end != 0;
            stats->writeback += entry->writeback != 0;
            entry = RB_NEXT(kernel_page_cache_tree, &node->cache_pages, entry);
            if (++scanned == 64) break;
        }
        if (!entry || entry->page_index > last) break;
        index = entry->page_index;
    }
    {
        CACHE_SCOPE(guard, cache);
        if (!__atomic_load_n(&node->cache_shadow_count, __ATOMIC_RELAXED)) return;
        uint64_t capacity = record->statistics.current_pages;
        if (!capacity) capacity = 1;
        for (unsigned i = 0; i < CACHE_SHADOW_LIMIT; i++) {
            const struct cache_shadow *shadow = &record->shadows->slots[i];
            if (shadow->node != node || shadow->index < first || shadow->index > last ||
                shadow->generation > query_generation) continue;
            stats->evicted++;
            stats->recently_evicted += record->reclaim_generation - shadow->generation < capacity;
        }
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
            if (__atomic_load_n(&cache->record->request_borrows,__ATOMIC_ACQUIRE) ||
                (BOAROS_MAX_CPUS>1 &&
                 __atomic_load_n(&cache->record->entry_borrows,__ATOMIC_ACQUIRE)))
                return KERNEL_PAGE_CACHE_STATUS_STATE;
            for (struct kernel_page_cache_entry *entry=cache->record->lru_tail;entry;entry=entry->lru_previous) {
                uint32_t references;
                if (physical_page_reference_count(cache->allocator,entry->physical_address,&references)) __builtin_trap();
                if (references!=1 || entry_users(entry) || entry_loading(entry) || entry->writeback || entry->aliases ||
                    __atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)) return KERNEL_PAGE_CACHE_STATUS_STATE;
            }
            while (cache->record->lru_tail) remove_entry(cache,cache->record->lru_tail);
        }
        group_unregister(cache);
        cache->state=KERNEL_PAGE_CACHE_CLEANUP;
    }
    uint64_t released=0;
    if (!drain_entries(cache,&released)) return KERNEL_PAGE_CACHE_STATUS_CLEANUP_REQUIRED;
    union foreground_workspace *workspace=__atomic_exchange_n(
        &cache->record->foreground_workspace,NULL,__ATOMIC_ACQUIRE);
    if (workspace) kernel_heap_release(cache->heap,workspace);
    version_pool_close(cache->record->versions);
    if (cache->record->shadows) kernel_heap_release(cache->heap,cache->record->shadows);
    kernel_heap_release(cache->heap,cache->record);
    cache->heap=NULL;cache->allocator=NULL;cache->record=NULL;cache->state=KERNEL_PAGE_CACHE_RELEASED;
    return KERNEL_PAGE_CACHE_STATUS_OK;
}
