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
#include <kernel/time.h>
#include <kernel/page_cache.h>
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
    uint8_t loading;
    uint32_t users;
    enum kernel_page_cache_status load_error;
    struct kernel_wait_queue ready;
    uint8_t page_references_owned;
    struct kernel_page_cache_alias *aliases;
};

struct page_cache_group {
    struct physical_page_allocator *allocator;
    struct kernel_heap *heap;
    struct kernel_page_cache_record *head, *cursor;
    struct kernel_wait_queue progress;
    uint64_t refs, round, pending, progressed;
};

#if BOAROS_PAGE_CACHE_READAHEAD_PAGES
struct readahead_job {
    struct kernel_vfs_node *node;
    uint64_t cookie, first;
    int active, cancelled;
};
#endif

struct kernel_page_cache_record {
    struct kernel_mutex metadata;
    uint8_t controlled;
    struct cache_request *reads_head, **reads_tail;
    struct kernel_io_context *executor_io;
    uint32_t read_requests, reads_running;
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
    uint64_t requested;
    int started, stopping;
    struct cache_version *snapshots[BOAROS_PAGE_CACHE_WRITEBACK_PAGES];
    size_t snapshot_count;
    struct writeback_workspace *writeback_workspace;
#if BOAROS_PAGE_CACHE_READAHEAD_PAGES
    struct readahead_job readahead_jobs[8];
    struct kernel_thread_join readahead_worker;
    struct kernel_wait_queue readahead_work;
    uint64_t readahead_cookie;
    int readahead_started;
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
    uint64_t queued_at;
    unsigned observed;
    enum kernel_page_cache_status status;
    unsigned done;
    struct kernel_wait_queue changed;
};
struct cache_read_request {
    struct kernel_page_cache *cache;
    const struct kernel_vfs_file *file;
    uint64_t index, address;
    size_t valid;
    enum kernel_page_cache_status status;
};
struct cache_guard {
    struct kernel_lock_guard lock;
    uintptr_t irq;
    int controlled, active;
};
static void cache_unlock(struct cache_guard *guard)
{
    if (!guard->active) return;
    if (guard->controlled) kernel_lock_release(&guard->lock);
    else arch_interrupt_restore(guard->irq);
    guard->active = 0;
}
/* guard必须在最终存储地址初始化：锁owner链不能指向返回值临时对象。 */
static void cache_guard_begin(struct kernel_page_cache *cache, struct cache_guard *guard)
{
    guard->controlled = cache->record->controlled;
    guard->active = 1;
    if (guard->controlled) kernel_mutex_lock(&cache->record->metadata, &guard->lock);
    else { kernel_assert_service_context(); guard->irq = arch_interrupt_save(); }
}
#define CACHE_SCOPE(name, cache) \
    struct cache_guard name __attribute__((cleanup(cache_unlock))) = {0}; \
    cache_guard_begin((cache), &name)
static int remote_cache_client(void)
{
#if BOAROS_MAX_CPUS > 1
    if (kernel_cpu_current()->logical_id != 0) return 1;
    const struct kernel_mm *bound=0;
    struct kernel_task *task=kernel_task_current();
    return task && kernel_task_mm_borrow(task,&bound)==KERNEL_TASK_STATUS_OK;
#else
    return 0;
#endif
}
static void cache_wake(struct kernel_wait_queue *queue)
{
    KERNEL_IRQ_SCOPE(irq);
    enum kernel_scheduler_status status = kernel_wait_queue_wake_all(queue);
    if (status && !(status == KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED &&
        !queue->registrations && !queue->borrows)) __builtin_trap();
}

/* One stable file-data version. The cache or writeback caller owns the first
 * reference; ordered transactions retain the same page instead of copying it. */
struct cache_version {
    struct kernel_heap *heap;
    struct physical_page_allocator *allocator;
    uint64_t physical;
    unsigned references;
    struct kernel_wait_queue ready;
};
static struct cache_version *version_new(struct kernel_page_cache *cache)
{
    struct cache_version *version;
    enum kernel_heap_status heap_status=kernel_heap_allocate(cache->heap,sizeof(*version),(void **)&version);
    if (heap_status) {
        if (heap_status!=KERNEL_HEAP_STATUS_EMPTY) __builtin_trap();
        return NULL;
    }
    uint64_t address;
    enum physical_page_status status=physical_page_allocate(cache->allocator,&address);
    if (status) {
        if (status!=PHYSICAL_PAGE_STATUS_EMPTY) __builtin_trap();
        if (kernel_heap_release(cache->heap,version)) __builtin_trap();
        return NULL;
    }
    *version=(struct cache_version){.heap=cache->heap,.allocator=cache->allocator,
        .physical=address,.references=1};
    kernel_wait_queue_init(&version->ready);
    return version;
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
    if (old==2) cache_wake(&version->ready);
    if (old!=1) return;
    if (physical_page_release(version->allocator,version->physical) ||
        kernel_heap_release(version->heap,version)) __builtin_trap();
}
static struct cache_version *version_take(struct kernel_page_cache *cache,size_t slot)
{
    struct kernel_page_cache_record *record=cache->record;
    if (!kernel_io_context_current()->background_reclaim || !record->started)
        return version_new(cache);
    if (slot>=record->snapshot_count) __builtin_trap();
    struct cache_version *version=record->snapshots[slot];
    unsigned available=1;
    if (__atomic_compare_exchange_n(&version->references,&available,2,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
        return version;
    /* A busy reserve must not cap a running group at one writeback batch.
     * Ordinary free RAM supplies the next version; wait only at real pressure. */
    struct cache_version *fresh=version_new(cache);
    if (fresh) return fresh;
    KERNEL_IRQ_SCOPE(irq);
    for (;;) {
        unsigned expected=1;
        if (__atomic_compare_exchange_n(&version->references,&expected,2,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
            return version;
        /* The reserved page is still an immutable ordered-data source. Wait
         * for its actual last borrower; starting the next write cannot reuse it. */
        enum kernel_wait_wake_reason reason;
        if (KERNEL_WAIT_RECHECK(&version->ready,0,0,&reason,
            (__atomic_load_n(&version->references,__ATOMIC_ACQUIRE)!=1))) __builtin_trap();
    }
}
static void *version_data(struct cache_version *version)
{
    void *data;
    if (physical_page_resolve(version->allocator,version->physical,&data)) __builtin_trap();
    return data;
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
    if (!entry->users && entry->retired && entry->cache->record->controlled && entry->cache->record->started) {
        entry->cache->record->requested=1; cache_wake(&entry->cache->record->work);
    }
}

static void readahead_cancel_node(struct kernel_page_cache *cache, struct kernel_vfs_node *node);
static void pressure_notify(void *context);
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
int kernel_page_cache_is_controlled(const struct kernel_page_cache *cache)
{ return cache_live(cache) && cache->record->controlled; }

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
static int reserve_bucket(struct kernel_page_cache *cache)
{
    for (;;) {
        size_t old_capacity, capacity;
        {
            CACHE_SCOPE(guard, cache);
            struct kernel_page_cache_record *r = cache->record;
            size_t limit = r->capacity - r->capacity / 4;
            if (r->occupied + r->tombstones + 1U <= limit) return 1;
            old_capacity = r->capacity;
            capacity = old_capacity;
            if (r->occupied + 1U > limit) {
                if (old_capacity > SIZE_MAX / 2U) return 0;
                capacity *= 2;
            }
        }
        struct kernel_page_cache_entry **buckets;
        if (kernel_heap_allocate_zeroed(cache->heap, capacity, sizeof(*buckets),
                (void **)&buckets) != KERNEL_HEAP_STATUS_OK) return 0;
        struct kernel_page_cache_entry **retired = 0;
        int published = 0, satisfied = 0;
        {
            CACHE_SCOPE(guard, cache);
            struct kernel_page_cache_record *r = cache->record;
            size_t limit = r->capacity - r->capacity / 4;
            satisfied = r->occupied + r->tombstones + 1U <= limit;
            /* 分配可能睡眠并发生插入/回收；当前 live 决定重排还是扩容。 */
            int grow = r->occupied + 1U > limit;
            if (!satisfied && r->capacity == old_capacity &&
                capacity == (grow ? old_capacity * 2 : old_capacity)) {
                retired = r->buckets;
                r->buckets = buckets; r->capacity = capacity; r->tombstones = 0;
                for (size_t i = 0; i < old_capacity; i++) {
                    struct kernel_page_cache_entry *entry = retired[i];
                    if (!entry || entry == PAGE_CACHE_TOMBSTONE) continue;
                    int found;
                    size_t destination = find_bucket(r, entry->node, entry->page_index, &found);
                    if (found) __builtin_trap();
                    buckets[destination] = entry;
                }
                published = 1;
            }
        }
        if (kernel_heap_release(cache->heap, published ? retired : buckets)) __builtin_trap();
        if (published || satisfied) return 1;
    }
}

static void lru_remove(struct kernel_page_cache_record *record,
                       struct kernel_page_cache_entry *entry)
{
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
    if (cache->record->controlled) {
        CACHE_SCOPE(guard,cache);
        /* 退休项已禁止新借用；在索引保护内确认最后借用结束后，
         * cleanup持有唯一存储owner，实际页/node/堆释放留在锁外。 */
        if (entry->users || (__atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)&UINT32_MAX)) return 0;
    } else if (entry->users || (__atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)&UINT32_MAX)) return 0;
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
    (void)kernel_heap_release(cache->heap, entry);
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
    entry->cleanup_next = cache->record->cleanup_entries;
    cache->record->cleanup_entries = entry;
    return KERNEL_PAGE_CACHE_STATUS_CLEANUP_REQUIRED;
}

static int drain_entries(struct kernel_page_cache *cache,
                         uint64_t *pages_released)
{
    if (cache->record->controlled) {
        struct kernel_page_cache_entry *entries;
        { CACHE_SCOPE(guard,cache); entries=cache->record->cleanup_entries; cache->record->cleanup_entries=0; }
        int failed=0;
        while (entries) {
            struct kernel_page_cache_entry *entry=entries; entries=entry->cleanup_next;
            if (!cleanup_entry(cache,entry,pages_released)) {
                CACHE_SCOPE(guard,cache);
                entry->cleanup_next=cache->record->cleanup_entries; cache->record->cleanup_entries=entry; failed=1;
            }
        }
        return !failed;
    }
    struct kernel_page_cache_entry **link =
        &cache->record->cleanup_entries;
    int failed = 0;

    while (*link != 0) {
        struct kernel_page_cache_entry *entry = *link;
        struct kernel_page_cache_entry *next = entry->cleanup_next;

        if (!cleanup_entry(cache, entry, pages_released)) {
            link = &entry->cleanup_next;
            failed = 1;
        } else {
            *link = next;
        }
    }
    return !failed;
}

/* The inode queue borrows its dirty entries. One offset tree replaces the
 * dirty chain, so foreground and background select the same ordered windows. */
static void dirty_mark(struct kernel_page_cache_entry *entry, size_t begin, size_t end)
{
    if (begin>=end || end>BOAROS_PAGE_SIZE) __builtin_trap();
    struct kernel_page_cache_record *r=entry->cache->record;
    if (!entry->dirty_end) {
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
    entry->generation++;
    if (r->controlled && r->started && !r->stopping && r->force_writeback) {
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
}
static void dirty_round_begin(struct kernel_page_cache_record *r)
{
    if (r->dirty_cursor && r->dirty_remaining) return;
    r->dirty_cursor=r->dirty_head;r->dirty_remaining=r->dirty_pages;r->dirty_scan_index=0;
}
static size_t dirty_batch(struct kernel_page_cache_record *r,struct kernel_page_cache_entry **batch)
{
    size_t count=0;unsigned scanned=0;
    size_t capacity=r->snapshot_count<BOAROS_PAGE_CACHE_WRITEBACK_PAGES?r->snapshot_count:BOAROS_PAGE_CACHE_WRITEBACK_PAGES;
    while (r->dirty_cursor && r->dirty_remaining && scanned<64 && count<capacity) {
        struct kernel_page_cache_dirty *dirty=r->dirty_cursor;
        if (!dirty->count || RB_EMPTY(&dirty->tree)) __builtin_trap();
        struct kernel_page_cache_entry key={.page_index=r->dirty_scan_index};
        struct kernel_page_cache_entry *entry=RB_NFIND(kernel_page_cache_dirty_tree,&dirty->tree,&key);
        if (!entry) {r->dirty_cursor=dirty->queue_next;r->dirty_scan_index=0;continue;}
        if (count && entry->node!=batch[0]->node) break;
        while (entry && r->dirty_remaining && scanned<64 && count<capacity) {
            r->dirty_scan_index=entry->page_index+1;
            r->dirty_remaining--;scanned++;r->statistics.worker_scanned++;
            if (!entry->loading && !entry->writeback && (r->controlled || !entry->users)) {
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
    if (entry->aliases != 0 || entry->loading || (entry->users && !cache->record->controlled)) __builtin_trap();
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
    uint64_t reclaimable = 0;
    for (struct kernel_page_cache_entry *e = cache->record->lru_head; e; e = e->lru_next) {
        out->cached += BOAROS_PAGE_SIZE;
        if (e->dirty_end) out->dirty += BOAROS_PAGE_SIZE;
        if (e->writeback) out->writeback += BOAROS_PAGE_SIZE;
        uint32_t refs;
        if (physical_page_reference_count(cache->allocator, e->physical_address, &refs)
                != PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
        if (refs == 1 && !e->aliases && !e->users && !e->loading && !e->writeback &&
            !e->writeback_error && (!e->dirty_end || kernel_vfs_node_writeback_allowed(e->node)))
            reclaimable += BOAROS_PAGE_SIZE;
    }
    out->reclaimable += reclaimable;
}

static void group_snapshot(void *context, struct kernel_memory_statistics *out)
{
    struct page_cache_group *group = context;
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

static uint64_t reclaim_callback(void *context, uint64_t target_pages)
{
    struct kernel_page_cache *cache = context;
    struct kernel_page_cache_entry *entry = cache->record->lru_tail;
    uint64_t released = 0;
    while (entry && released < target_pages) {
        struct kernel_page_cache_entry *previous = entry->lru_previous;
        uint32_t references;
        if (!entry->dirty_end && !entry->users && !entry->loading && !entry->writeback &&
            !entry->aliases && physical_page_reference_count(cache->allocator,
                entry->physical_address, &references) == PHYSICAL_PAGE_STATUS_OK && references == 1) {
            remove_entry(cache, entry);
            (void)drain_entries(cache, &released);
        }
        entry = previous;
    }
    cache->record->statistics.pages_reclaimed += released;
    return released;
}

static uint64_t group_reclaim(void *context, uint64_t target)
{
    struct page_cache_group *g = context;
    struct kernel_page_cache_record *first = g->cursor ? g->cursor : g->head;
    struct kernel_page_cache_record *r = first;
    uint64_t released = 0;
    if (!r) return 0;
    do {
        released += reclaim_callback(r->owner, target - released);
        r = r->next ? r->next : g->head;
    } while (released < target && r != first);
    g->cursor = r;
    return released;
}

static void group_put(struct page_cache_group *g)
{
    if (!g->refs) __builtin_trap();
    if (!--g->refs) (void)kernel_heap_release(g->heap, g);
}

static uint64_t group_dirty(struct page_cache_group *g)
{
    uint64_t dirty = 0;
    for (struct kernel_page_cache_record *r = g->head; r; r = r->next)
        dirty += r->dirty_pages;
    return dirty;
}

static void group_progress(struct page_cache_group *g)
{
    g->progressed++;
    (void)kernel_wait_queue_wake_all(&g->progress);
}

static void group_complete(struct kernel_page_cache_record *r, int progress)
{
    struct page_cache_group *g = r->group;
    if (progress) g->progressed++;
    if (r->wait_round && r->wait_round == g->round) {
        if (!g->pending) __builtin_trap();
        g->pending--;
    }
    r->wait_round = 0;
    (void)kernel_wait_queue_wake_all(&g->progress);
}

static void group_unregister(struct kernel_page_cache *cache)
{
    struct kernel_page_cache_record *r = cache->record;
    struct page_cache_group *g = r->group;
    struct kernel_page_cache_record **link = &g->head;
    while (*link != r) link = &(*link)->next;
    *link = r->next;
    if (g->cursor == r) g->cursor = r->next ? r->next : g->head;
    if (!g->head) {
        if (physical_page_allocator_clear_reclaimer(cache->allocator) != PHYSICAL_PAGE_STATUS_OK)
            __builtin_trap();
        cache->allocator->cache_snapshot = 0;
        cache->allocator->cache_context = 0;
        cache->allocator->pressure_notify = 0;
        cache->allocator->pressure_wait = 0;
        cache->allocator->pressure_context = 0;
    }
    r->group = 0;
    group_put(g);
}

static enum kernel_page_cache_status cache_init(
    struct kernel_page_cache *cache,
    struct kernel_heap *heap,
    struct physical_page_allocator *allocator, int controlled)
{
    struct kernel_page_cache_record *record;
    enum kernel_heap_status heap_status;

    if (cache == 0 || heap == 0 || allocator == 0 ||
        cache->state != KERNEL_PAGE_CACHE_EMPTY || cache->heap != 0 ||
        cache->allocator != 0 || cache->record != 0 ||
        !physical_page_allocator_is_finalized(allocator)) {
        return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    }
    if (controlled && (!heap->initialized || heap->page_allocator != allocator))
        return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    if (controlled && (allocator->reclaimer || allocator->pressure_notify ||
        allocator->pressure_wait || allocator->cache_snapshot))
        return KERNEL_PAGE_CACHE_STATUS_STATE;
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
        (void)kernel_heap_release(heap, record);
        return heap_status == KERNEL_HEAP_STATUS_EMPTY
                   ? KERNEL_PAGE_CACHE_STATUS_NO_MEMORY
                   : KERNEL_PAGE_CACHE_STATUS_STATE;
    }
    record->capacity = PAGE_CACHE_INITIAL_CAPACITY;
    record->controlled = controlled;
    kernel_mutex_init(&record->metadata, 45, (uintptr_t)record);
    cache->heap = heap;
    cache->allocator = allocator;
    cache->record = record;
    cache->state = KERNEL_PAGE_CACHE_LIVE;
    if (controlled) return KERNEL_PAGE_CACHE_STATUS_OK;
    struct page_cache_group *group = allocator->reclaimer == group_reclaim
        ? allocator->reclaimer_context : 0;
    if (!group) {
        if (allocator->reclaimer) goto failed;
        heap_status = kernel_heap_allocate_zeroed(heap, 1, sizeof(*group), (void **)&group);
        if (heap_status != KERNEL_HEAP_STATUS_OK) goto failed;
        group->allocator = allocator;
        group->heap = heap;
        kernel_wait_queue_init(&group->progress);
        if (physical_page_allocator_set_reclaimer(allocator, group_reclaim, group) != PHYSICAL_PAGE_STATUS_OK) {
            (void)kernel_heap_release(heap, group);
            goto failed;
        }
        allocator->reclaim_depth = reclaim_depth;
        allocator->cache_snapshot = group_snapshot;
        allocator->cache_context = group;
        allocator->pressure_notify = group_notify;
        allocator->pressure_wait = pressure_wait;
        allocator->pressure_context = group;
    }
    group->refs++;
    record->group = group;
    record->owner = cache;
    record->next = group->head;
    group->head = record;
    return KERNEL_PAGE_CACHE_STATUS_OK;
failed:
    (void)kernel_heap_release(heap, record->buckets);
    (void)kernel_heap_release(heap, record);
    *cache = (struct kernel_page_cache){0};
    return heap_status == KERNEL_HEAP_STATUS_EMPTY ? KERNEL_PAGE_CACHE_STATUS_NO_MEMORY
                                                  : KERNEL_PAGE_CACHE_STATUS_STATE;
}

enum kernel_page_cache_status kernel_page_cache_init(
    struct kernel_page_cache *cache, struct kernel_heap *heap,
    struct physical_page_allocator *allocator)
{ return cache_init(cache, heap, allocator, 0); }

enum kernel_page_cache_status kernel_page_cache_init_controlled(
    struct kernel_page_cache *cache, struct kernel_heap *heap,
    struct physical_page_allocator *allocator)
{
    kernel_assert_service_context();
    return cache_init(cache, heap, allocator, 1);
}

int kernel_page_cache_check_domain(const struct kernel_page_cache *cache,
    const struct kernel_heap *heap, const struct physical_page_allocator *allocator)
{
    return cache_live(cache) && cache->record->controlled &&
        cache->record->started && !cache->record->stopping &&
        cache->heap == heap && cache->allocator == allocator &&
        !allocator->reclaimer && !allocator->pressure_notify;
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
        .record_bytes = sizeof(*r), .request_bytes = sizeof(struct cache_request)+sizeof(struct cache_read_request),
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
    if (cache->record->controlled) {
        /* stop->drain的真实entry存储owner；回调末次put不能抢先释放它。 */
        entry_borrow(alias->entry);
        if (alias->borrows) { alias->retired_next = alias->next; alias_borrow(alias->retired_next); }
        return;
    }
    *alias = (struct kernel_page_cache_alias){0};
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
static int rearm_aliases(struct kernel_page_cache_entry *entry)
{
    struct kernel_page_cache *cache = entry->cache;
    struct kernel_page_cache_alias *alias;
    { CACHE_SCOPE(guard, cache); alias = entry->aliases; alias_borrow(alias); }
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
    pressure_notify(cache);
}
#define CACHE_WRITES_CLOSED (UINT64_C(1)<<63)
#define CACHE_WRITES_MASK (CACHE_WRITES_CLOSED | UINT32_MAX)
static int writer_begin(struct kernel_page_cache_entry *entry,
    struct kernel_page_cache_write_borrow *borrow)
{
    if (!borrow || borrow->registration || !entry->cache->record->controlled) __builtin_trap();
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
void kernel_page_cache_write_end(struct kernel_page_cache_write_borrow *borrow)
{
    if (!borrow || borrow->registration != borrow || borrow->owner != kernel_io_context_current()) __builtin_trap();
    struct kernel_page_cache_entry *entry = borrow->entry;
    struct kernel_io_context *owner = borrow->owner;
    struct kernel_page_cache_write_borrow **link = &owner->cache_writes;
    while (*link && *link != borrow) link = &(*link)->next;
    if (*link != borrow) __builtin_trap();
    CACHE_SCOPE(guard,entry->cache);
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
        {
            CACHE_SCOPE(guard, cache);
            if (!entry->loading) {
                status = entry->load_error;
                if (!status) take_read_page(cache, entry, address, valid, entry_valid_bytes(entry));
                entry_return(entry);
                if (status && !entry->users && !entry->retired) { remove_entry(cache, entry); clean = 1; }
                break;
            }
            if (kernel_wait_prepare(&entry->ready, 0, 0, &token)) __builtin_trap();
        }
        enum kernel_wait_wake_reason reason;
        if (kernel_wait_park(&token, &reason) || kernel_wait_finish(&token)) __builtin_trap();
    }
    /* 仅CPU0结束node/backend owner；远端最后失败者请求worker收尾。 */
    if (clean) {
        if (!remote_cache_client()) { uint64_t released = 0; (void)drain_entries(cache, &released); }
        else {
            CACHE_SCOPE(notify, cache);
            cache->record->requested = 1;
            cache_wake(&cache->record->work);
        }
    }
    return status;
}

enum kernel_page_cache_status kernel_page_cache_lookup(
    struct kernel_page_cache *cache, const struct kernel_vfs_file *file,
    uint64_t index, uint64_t *address, size_t *valid)
{
    if (!cache_live(cache) || !address || !valid) return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    struct kernel_vfs_node *node = kernel_vfs_file_node(file);
    if (!node) return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    KERNEL_LOCK_SCOPE(inode);
    if (cache->record->controlled) kernel_vfs_node_lock(node, &inode, 0);
    struct kernel_page_cache_entry *entry;
    {
        CACHE_SCOPE(guard, cache);
        enum kernel_page_cache_status status = lookup_entry(cache, file, index, &entry);
        if (status) return status;
        if (!entry->loading && !entry->load_error) {
            take_read_page(cache, entry, address, valid, entry_valid_bytes(entry));
            return KERNEL_PAGE_CACHE_STATUS_OK;
        }
        entry_borrow(entry);
    }
    return finish_read_page(cache, entry, address, valid);
}

/* New entries own node/page and remain loading until their producer publishes. */
static enum kernel_page_cache_status prepare_page(struct kernel_page_cache *cache,
    struct kernel_vfs_node *node, uint64_t page_index, int clear,
    struct kernel_page_cache_entry **output, void **bytes, int *created)
{
    struct kernel_page_cache_entry *entry;
    void *page;
    size_t bucket;
    int found;
    if (!reserve_bucket(cache) ||
        kernel_heap_allocate_zeroed(cache->heap,
                                    1U,
                                    sizeof(*entry),
                                    (void **)&entry) !=
            KERNEL_HEAP_STATUS_OK) {
        return KERNEL_PAGE_CACHE_STATUS_NO_MEMORY;
    }
    if (physical_page_allocate(cache->allocator,
                               &entry->physical_address) !=
        PHYSICAL_PAGE_STATUS_OK) {
        (void)kernel_heap_release(cache->heap, entry);
        return KERNEL_PAGE_CACHE_STATUS_NO_MEMORY;
    }
    entry->page_references_owned = 1U;
    if (physical_page_resolve(cache->allocator,
                              entry->physical_address,
                              &page) != PHYSICAL_PAGE_STATUS_OK) {
        return abandon_entry(cache,
                             entry,
                             KERNEL_PAGE_CACHE_STATUS_STATE);
    }
    if (clear) memset(page, 0, BOAROS_PAGE_SIZE);
    if (kernel_vfs_node_acquire(node) != 0) {
        return abandon_entry(cache,
                             entry,
                             KERNEL_PAGE_CACHE_STATUS_STATE);
    }
    entry->cache = cache;
    entry->node = node;
    entry->page_index = page_index;
    for (;;) {
        CACHE_SCOPE(publish, cache);
        bucket = find_bucket(cache->record, node, page_index, &found);
        if (!found && cache->record->occupied + cache->record->tombstones + 1U >
                cache->record->capacity - cache->record->capacity / 4U) {
            cache_unlock(&publish);
            if (!reserve_bucket(cache)) return abandon_entry(cache, entry, KERNEL_PAGE_CACHE_STATUS_NO_MEMORY);
            continue;
        }
        if (found) {
            *created = 0; *output = cache->record->buckets[bucket]; *bytes = 0;
            cache_unlock(&publish);
            (void)abandon_entry(cache, entry, KERNEL_PAGE_CACHE_STATUS_OK);
            return KERNEL_PAGE_CACHE_STATUS_OK;
        }
        kernel_wait_queue_init(&entry->ready);
        entry->loading = 1;
        entry_borrow(entry);
        entry->page_references_owned = 1U;
        if (cache->record->buckets[bucket] == PAGE_CACHE_TOMBSTONE) {
            cache->record->tombstones--;
        }
        cache->record->buckets[bucket] = entry;
        cache->record->occupied++;
        entry->node_next = *kernel_vfs_node_cache_pages(node);
        entry->node_previous = kernel_vfs_node_cache_pages(node);
        if (entry->node_next != 0)
            entry->node_next->node_previous = &entry->node_next;
        *entry->node_previous = entry;
        lru_insert_head(cache->record, entry);
        cache->record->statistics.insertions++;
        cache->record->statistics.current_pages++;
        if (cache->record->statistics.current_pages >
            cache->record->statistics.peak_pages) {
            cache->record->statistics.peak_pages =
                cache->record->statistics.current_pages;
        }
        *output = entry; *bytes = page; *created = 1;
        return KERNEL_PAGE_CACHE_STATUS_OK;
    }
}

/* 完成加载并归还生产者借用；需求读直接取得物理引用，预读只发布缓存内容。 */
static size_t load_read_pages(struct kernel_page_cache *cache, struct kernel_vfs_node *node,
    struct kernel_page_cache_entry **entries, struct kernel_vfs_read_span *spans, size_t count,
    struct kernel_page_cache_read_page **outputs)
{
    if (!count) return 0;
    int result = kernel_vfs_node_pread_batch(node, spans, count), identified = 0;
    for (size_t i = 0; i < count; i++) identified |= spans[i].error != 0;
    size_t failed = 0;
    int clean = 0;
    {
        CACHE_SCOPE(complete, cache);
        cache->record->statistics.misses += count;
        for (size_t i = 0; i < count; i++) {
            struct kernel_page_cache_entry *entry = entries[i];
            int error = spans[i].error;
            size_t valid = entry_valid_bytes(entry);
            if (spans[i].completed > spans[i].size) __builtin_trap();
            if (result && !identified) error = result;
            if (!error && spans[i].completed < valid) error = -KERNEL_EIO;
            entry->load_error = error == -KERNEL_ENOMEM ? KERNEL_PAGE_CACHE_STATUS_NO_MEMORY :
                error ? KERNEL_PAGE_CACHE_STATUS_IO : KERNEL_PAGE_CACHE_STATUS_OK;
            entry->loading = 0;
            if (outputs) {
                outputs[i]->status = entry->load_error;
                if (!entry->load_error) {
                    if (physical_page_acquire(cache->allocator, entry->physical_address)) __builtin_trap();
                    outputs[i]->physical_address = entry->physical_address;
                    outputs[i]->valid_bytes = valid;
                }
            }
            failed += entry->load_error != 0;
            entry_return(entry);
            cache_wake(&entry->ready);
            if (entry->load_error && !entry->users && !entry->retired) {
                remove_entry(cache, entry); clean = 1;
            }
        }
    }
    if (clean) { uint64_t released = 0; (void)drain_entries(cache, &released); }
    return failed;
}

#if BOAROS_PAGE_CACHE_READAHEAD_PAGES
static void readahead_drop(struct kernel_page_cache_record *r, struct readahead_job *job)
{
    if (job->active) __builtin_trap();
    if (job->cancelled) r->statistics.readahead_cancelled++;
    if (kernel_vfs_node_release(&job->node)) __builtin_trap();
    *job = (struct readahead_job){0};
}

/* Caller serializes the short queue mutation; active jobs keep their node. */
static void readahead_cancel_node(struct kernel_page_cache *cache, struct kernel_vfs_node *node)
{
    struct kernel_page_cache_record *r = cache->record;
    for (unsigned i = 0; i < 8; i++) {
        struct readahead_job *job = &r->readahead_jobs[i];
        if (!job->node || (node && job->node != node)) continue;
        job->cancelled = 1;
        if (!job->active) readahead_drop(r, job);
    }
}

void kernel_page_cache_cancel_readahead(struct kernel_page_cache *cache, uint64_t cookie)
{
    if (!cache_live(cache) || !cookie) return;
    uintptr_t irq = arch_interrupt_save();
    struct kernel_page_cache_record *r = cache->record;
    for (unsigned i = 0; i < 8; i++) {
        struct readahead_job *job = &r->readahead_jobs[i];
        if (!job->node || job->cookie != cookie) continue;
        job->cancelled = 1;
        if (!job->active) readahead_drop(r, job);
    }
    arch_interrupt_restore(irq);
}

uint64_t kernel_page_cache_readahead(struct kernel_page_cache *cache,
    struct kernel_vfs_node *node, uint64_t first, uint64_t cookie)
{
    if (!cache_live(cache) || !node || first > UINT64_MAX >> BOAROS_PAGE_SHIFT) return 0;
    uintptr_t irq = arch_interrupt_save();
    struct kernel_page_cache_record *r = cache->record;
    if (!r->readahead_started || r->stopping || physical_page_available(cache->allocator) <= r->low ||
        (first << BOAROS_PAGE_SHIFT) >= kernel_vfs_node_size(node)) {
        kernel_page_cache_cancel_readahead(cache, cookie);
        arch_interrupt_restore(irq); return 0;
    }
    struct readahead_job *available = 0;
    for (unsigned i = 0; i < 8; i++) {
        struct readahead_job *job = &r->readahead_jobs[i];
        if (!job->node) { if (!available) available = job; continue; }
        if (cookie && job->cookie == cookie && job->node == node && !job->cancelled) {
            if (!job->active) job->first = first;
            arch_interrupt_restore(irq); return cookie;
        }
    }
    if (!available) { arch_interrupt_restore(irq); return 0; }
    if (kernel_vfs_node_acquire(node)) __builtin_trap();
    if (!++r->readahead_cookie) __builtin_trap();
    *available = (struct readahead_job){.node = node, .first = first, .cookie = r->readahead_cookie};
    r->statistics.readahead_queued++;
    if (r->readahead_work.head) (void)kernel_wait_queue_wake_all(&r->readahead_work);
    cookie = available->cookie;
    arch_interrupt_restore(irq);
    return cookie;
}

static void readahead_load(struct kernel_page_cache *cache, struct readahead_job *job)
{
    KERNEL_LOCK_SCOPE(guard);
    if (!kernel_vfs_node_try_read(job->node, &guard)) { job->cancelled = 1; return; }
    struct kernel_page_cache_record *r = cache->record;
    struct kernel_page_cache_entry *entries[BOAROS_PAGE_CACHE_READAHEAD_PAGES] = {0};
    struct kernel_vfs_read_span spans[BOAROS_PAGE_CACHE_READAHEAD_PAGES];
    unsigned count = 0;
    /* inode读资格已经阻止pressure_wait发起回收I/O，包含尚未提交的loading。 */
    for (unsigned i = 0; i < BOAROS_PAGE_CACHE_READAHEAD_PAGES; i++) {
        if (r->stopping || job->cancelled || physical_page_available(cache->allocator) <= r->low) {
            job->cancelled = 1; break;
        }
        uint64_t index = job->first + i;
        if (index > UINT64_MAX >> BOAROS_PAGE_SHIFT || (index << BOAROS_PAGE_SHIFT) >= kernel_vfs_node_size(job->node)) break;
        int found, created;
        (void)find_bucket(r, job->node, index, &found);
        if (found) continue; /* 已有加载由原 owner 完成，预读不与它交叉等待。 */
        struct kernel_page_cache_entry *entry;
        void *page;
        enum kernel_page_cache_status status = prepare_page(cache, job->node, index, 1, &entry, &page, &created);
        if (status != KERNEL_PAGE_CACHE_STATUS_OK) break;
        if (!created) continue;
        entries[count] = entry;
        spans[count++] = (struct kernel_vfs_read_span){index << BOAROS_PAGE_SHIFT, page, BOAROS_PAGE_SIZE, 0, 0};
    }
    /* 取消仅丢弃尚无需求读者的页；已经订阅的需求保留完整加载 owner。 */
    if (r->stopping || job->cancelled || physical_page_available(cache->allocator) <= r->low) {
        unsigned kept = 0;
        for (unsigned i = 0; i < count; i++) {
            struct kernel_page_cache_entry *entry = entries[i];
            if (entry->users > 1) { entries[kept] = entry; spans[kept++] = spans[i]; continue; }
            entry->loading = 0; entry_return(entry);
            remove_entry(cache, entry);
        }
        count = kept;
        uint64_t released = 0; (void)drain_entries(cache, &released);
    }
    if (!count) return;
    r->statistics.readahead_batches++;
    if (count > r->statistics.readahead_peak_pages) r->statistics.readahead_peak_pages = count;
    size_t failed = load_read_pages(cache, job->node, entries, spans, count, NULL);
    CACHE_SCOPE(complete, cache);
    r->statistics.readahead_failed += failed;
    r->statistics.readahead_loaded += count - failed;
}

static void readahead_worker(void *argument)
{
    struct kernel_page_cache *cache = argument;
    struct kernel_page_cache_record *r = cache->record;
    (void)arch_interrupt_save();
    for (;;) {
        struct readahead_job *next = 0;
        for (unsigned i = 0; i < 8; i++) {
            struct readahead_job *job = &r->readahead_jobs[i];
            if (job->node && (!next || job->cookie < next->cookie)) next = job;
        }
        if (r->stopping) { readahead_cancel_node(cache, 0); break; }
        if (!next) {
            enum kernel_wait_wake_reason reason;
            if (KERNEL_WAIT_RECHECK(&r->readahead_work, 0, 1, &reason,
                (!r->stopping && !next)) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
            continue;
        }
        next->active = 1;
        readahead_load(cache, next);
        next->active = 0; readahead_drop(r, next);
        /* 一批最多八页；只在完整后端调用返回后让出，不截断内部 I/O。 */
        arch_interrupt_restore(ARCH_INTERRUPT_ENABLE_MASK); (void)arch_interrupt_save();
        if (kernel_scheduler_yield_current() != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
    }
}
#else
static void readahead_cancel_node(struct kernel_page_cache *cache, struct kernel_vfs_node *node)
{ (void)cache; (void)node; }
#endif

static enum kernel_page_cache_status get_page(
    struct kernel_page_cache *cache,
    const struct kernel_vfs_file *file,
    uint64_t page_index,
    uint64_t *physical_address,
    size_t *valid_bytes,
    int for_write, struct kernel_vfs_write_source *overwrite, int *overwritten)
{
    struct kernel_page_cache_entry *entry = 0;
    struct kernel_vfs_node *node;
    void *page = 0;
    uint64_t offset;
    size_t bytes_read = 0U;
    enum kernel_page_cache_status lookup_status;

    if (overwritten) *overwritten = 0;
    lookup_status = kernel_page_cache_lookup(cache,
                                             file,
                                             page_index,
                                             physical_address,
                                             valid_bytes);
    if (lookup_status == KERNEL_PAGE_CACHE_STATUS_OK) {
        return lookup_status;
    }
    if (lookup_status != KERNEL_PAGE_CACHE_STATUS_NOT_FOUND ||
        !cache_live(cache)) {
        return lookup_status;
    }
    node = kernel_vfs_file_node(file);
    if (node == 0 || page_index > UINT64_MAX >> BOAROS_PAGE_SHIFT) {
        return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    }
    offset = page_index << BOAROS_PAGE_SHIFT;
    if (!for_write && offset >= kernel_vfs_node_size(node)) {
        return KERNEL_PAGE_CACHE_STATUS_OUT_OF_RANGE;
    }
    { CACHE_SCOPE(miss, cache); cache->record->statistics.misses++; }
    int created = 0;
    lookup_status = prepare_page(cache, node, page_index, !overwrite, &entry, &page, &created);
    if (lookup_status != KERNEL_PAGE_CACHE_STATUS_OK) return lookup_status;
    if (!created) return kernel_page_cache_lookup(cache, file, page_index, physical_address, valid_bytes);
    int read_error = 0;
    if (!overwrite) read_error = kernel_vfs_node_pread(node, offset, page, BOAROS_PAGE_SIZE, &bytes_read);
    if (overwrite) {
        uint64_t old_size = kernel_vfs_node_size(node);
        kernel_vfs_write_source_copy(overwrite, page, BOAROS_PAGE_SIZE);
        if (offset > old_size)
            kernel_page_cache_extend(cache, node, old_size, offset);
        kernel_vfs_node_written(node, offset + BOAROS_PAGE_SIZE);
        *overwritten = 1;
    }
    size_t valid = entry_valid_bytes(entry);
    if (!overwrite && bytes_read > BOAROS_PAGE_SIZE) __builtin_trap();
    if (!overwrite && !read_error && bytes_read < valid) read_error = -KERNEL_EIO;
    enum kernel_page_cache_status result = read_error == -KERNEL_ENOMEM ? KERNEL_PAGE_CACHE_STATUS_NO_MEMORY :
        read_error ? KERNEL_PAGE_CACHE_STATUS_IO : KERNEL_PAGE_CACHE_STATUS_OK;
    uint64_t physical = entry->physical_address;
    if (!result && physical_page_acquire(cache->allocator, physical)) __builtin_trap();
    int clean = 0;
    {
        CACHE_SCOPE(complete, cache);
        if (overwrite) dirty_mark(entry, 0, BOAROS_PAGE_SIZE);
        entry->load_error = result; entry->loading = 0;
        entry_return(entry);
        cache_wake(&entry->ready);
        if (result && !entry->users) { remove_entry(cache, entry); clean = 1; }
    }
    if (clean) { uint64_t released = 0; (void)drain_entries(cache, &released); }
    if (result) return result;
    *physical_address = physical; *valid_bytes = valid;
    return KERNEL_PAGE_CACHE_STATUS_OK;
}

int kernel_page_cache_is_executor(const struct kernel_page_cache *cache)
{
    if (!cache || !cache->record || !cache->record->controlled) return 0;
    struct kernel_io_context *executor=__atomic_load_n(&cache->record->executor_io,__ATOMIC_ACQUIRE);
    return executor && executor==kernel_io_context_current();
}
static enum kernel_page_cache_status run_request(struct kernel_page_cache *cache,
    void (*execute)(void *),void *argument,unsigned observed)
{
    struct cache_request request = {.execute=execute,.argument=argument,.observed=observed,
        .queued_at=observed?kernel_time_monotonic_ns():0};
    kernel_wait_queue_init(&request.changed);
    struct kernel_page_cache_record *r = cache->record;
    {
        CACHE_SCOPE(publish, cache);
        if (!r->started || r->stopping) return KERNEL_PAGE_CACHE_STATUS_STATE;
        if (r->read_requests == UINT32_MAX) __builtin_trap();
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
    return request.status;
}
enum kernel_page_cache_status kernel_page_cache_run_cpu0(struct kernel_page_cache *cache,
    void (*execute)(void *),void *argument)
{
    if (!cache_live(cache) || !cache->record->controlled || !execute)
        return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    if (kernel_page_cache_is_executor(cache)) { execute(argument); return KERNEL_PAGE_CACHE_STATUS_OK; }
    struct kernel_io_context *io=kernel_io_context_current();
    if (io->locks || io->user_pins || io->cache_writes || kernel_cpu_current()->raw_locks ||
        !kernel_scheduler_can_sleep() || arch_external_interrupt_active()) return KERNEL_PAGE_CACHE_STATUS_STATE;
    return run_request(cache,execute,argument,1);
}
static void read_on_cpu0(void *argument)
{
    struct cache_read_request *request=argument;
    request->status=kernel_page_cache_get(request->cache,request->file,request->index,
        &request->address,&request->valid);
}
static enum kernel_page_cache_status remote_read(struct kernel_page_cache *cache,
    const struct kernel_vfs_file *file, uint64_t index, uint64_t *address, size_t *valid)
{
    struct cache_read_request request={.cache=cache,.file=file,.index=index};
    enum kernel_page_cache_status status=run_request(cache,read_on_cpu0,&request,0);
    if (status) return status;
    if (!request.status) { *address=request.address; *valid=request.valid; }
    return request.status;
}

enum kernel_page_cache_status kernel_page_cache_get(
    struct kernel_page_cache *cache, const struct kernel_vfs_file *file,
    uint64_t page_index, uint64_t *physical_address, size_t *valid_bytes)
{
    struct kernel_vfs_node *node = kernel_vfs_file_node(file);
    if (!node || !cache_live(cache)) return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    if (cache->record->controlled && remote_cache_client()) {
        enum kernel_page_cache_status found = kernel_page_cache_lookup(cache, file, page_index, physical_address, valid_bytes);
        if (found != KERNEL_PAGE_CACHE_STATUS_NOT_FOUND) return found;
        return remote_read(cache, file, page_index, physical_address, valid_bytes);
    }
    KERNEL_LOCK_SCOPE(node_guard);
    kernel_vfs_node_lock(node, &node_guard, 0);
    return get_page(cache, file, page_index, physical_address, valid_bytes, 0, 0, 0);
}

/* 不内联装入器，避免热命中也预留整批装入工作区。 */
static void __attribute__((noinline)) read_batch_local(struct kernel_page_cache *cache, const struct kernel_vfs_file *file,
    uint64_t first, struct kernel_page_cache_read_page *pages, size_t count,
    struct kernel_page_cache_entry **entries)
{
    struct kernel_vfs_node *node = kernel_vfs_file_node(file);
    struct read_workspace {
        struct kernel_page_cache_entry *loads[KERNEL_PAGE_CACHE_READ_BATCH_MAX];
        struct kernel_vfs_read_span spans[KERNEL_PAGE_CACHE_READ_BATCH_MAX];
        struct kernel_page_cache_read_page *outputs[KERNEL_PAGE_CACHE_READ_BATCH_MAX];
    } *workspace=NULL;
    struct kernel_page_cache_entry *single_load;
    struct kernel_vfs_read_span single_span;
    struct kernel_page_cache_read_page *single_output;
    if (count>1 && kernel_heap_allocate(cache->heap,sizeof(*workspace),(void **)&workspace)) {
        for (size_t i=0;i<count;i++) {
            if (entries[i]) pages[i].status=finish_read_page(cache,entries[i],
                &pages[i].physical_address,&pages[i].valid_bytes);
            else if (pages[i].status==KERNEL_PAGE_CACHE_STATUS_NOT_FOUND)
                pages[i].status=KERNEL_PAGE_CACHE_STATUS_NO_MEMORY;
        }
        return;
    }
    struct kernel_page_cache_entry **loads=workspace?workspace->loads:&single_load;
    struct kernel_vfs_read_span *spans=workspace?workspace->spans:&single_span;
    struct kernel_page_cache_read_page **outputs=workspace?workspace->outputs:&single_output;
    size_t loading = 0;
    /* 调用者持有inode读资格；pressure_wait不会在本批准备期间等待回收I/O。 */
    for (size_t i = 0; i < count; i++) {
        if (pages[i].status != KERNEL_PAGE_CACHE_STATUS_NOT_FOUND || entries[i]) continue;
        enum kernel_page_cache_status status;
        for (;;) {
            struct kernel_page_cache_entry *entry;
            void *data;
            int created;
            status = prepare_page(cache, node, first + i, 1, &entry, &data, &created);
            if (status) break;
            if (created) {
                loads[loading] = entry;
                outputs[loading] = &pages[i];
                spans[loading++] = (struct kernel_vfs_read_span){
                    (first + i) << BOAROS_PAGE_SHIFT, data, BOAROS_PAGE_SIZE, 0, 0};
                break;
            }
            /* 分配期间竞争者已发布；不使用未固定的返回指针，锁内重查。 */
            CACHE_SCOPE(lookup, cache);
            status = lookup_entry(cache, file, first + i, &entry);
            if (status == KERNEL_PAGE_CACHE_STATUS_NOT_FOUND) continue;
            if (!status) {
                if (!entry->loading && !entry->load_error) {
                    take_read_page(cache, entry, &pages[i].physical_address,
                        &pages[i].valid_bytes, entry_valid_bytes(entry));
                    pages[i].status = KERNEL_PAGE_CACHE_STATUS_OK;
                } else { entry_borrow(entry); entries[i] = entry; }
            }
            break;
        }
        if (status) {
            for (size_t j = i; j < count; j++)
                if (pages[j].status == KERNEL_PAGE_CACHE_STATUS_NOT_FOUND && !entries[j])
                    pages[j].status = status;
            break;
        }
    }
    /* 先完成本批自有页，再等其他loading；反向重叠的需求不会彼此等待未提交页。 */
    (void)load_read_pages(cache, node, loads, spans, loading, outputs);
    for (size_t i = 0; i < count; i++) if (entries[i])
        pages[i].status = finish_read_page(cache, entries[i],
            &pages[i].physical_address, &pages[i].valid_bytes);
    if (workspace && kernel_heap_release(cache->heap,workspace)) __builtin_trap();
}

struct cache_read_batch_request {
    struct kernel_page_cache *cache;
    const struct kernel_vfs_file *file;
    uint64_t first;
    struct kernel_page_cache_read_page *pages;
    size_t count;
};
static void read_batch_on_cpu0(void *argument)
{
    struct cache_read_batch_request *request = argument;
    struct kernel_page_cache *cache = request->cache;
    struct kernel_vfs_node *node = kernel_vfs_file_node(request->file);
    struct kernel_page_cache_entry *entries[KERNEL_PAGE_CACHE_READ_BATCH_MAX];
    KERNEL_LOCK_SCOPE(inode);
    kernel_vfs_node_lock(node, &inode, 0);
    uint64_t size = kernel_vfs_node_size(node);
    int pending = 0;
    {
        CACHE_SCOPE(lookup, cache);
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
                if (!entry->loading && !entry->load_error) {
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
        }
    }
    if (pending) read_batch_local(cache, request->file, request->first, request->pages, request->count, entries);
}

enum kernel_page_cache_status kernel_page_cache_get_batch(
    struct kernel_page_cache *cache, const struct kernel_vfs_file *file, uint64_t first,
    struct kernel_page_cache_read_page *pages, size_t count)
{
    if (!cache_live(cache) || !kernel_vfs_file_node(file) || count > KERNEL_PAGE_CACHE_READ_BATCH_MAX ||
        (count && (!pages || first > UINT64_MAX >> BOAROS_PAGE_SHIFT ||
            count - 1 > (UINT64_MAX >> BOAROS_PAGE_SHIFT) - first)))
        return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    for (size_t i = 0; i < count; i++) pages[i].status = KERNEL_PAGE_CACHE_STATUS_NOT_FOUND;
    struct cache_read_batch_request request = {cache, file, first, pages, count};
    if (cache->record->controlled && remote_cache_client()) {
        size_t missing = 0;
        for (size_t i = 0; i < count; i++) {
            pages[i].status = kernel_page_cache_lookup(cache, file, first + i,
                &pages[i].physical_address, &pages[i].valid_bytes);
            missing += pages[i].status == KERNEL_PAGE_CACHE_STATUS_NOT_FOUND;
        }
        if (missing) {
            enum kernel_page_cache_status status = run_request(cache, read_batch_on_cpu0, &request, 0);
            if (status) for (size_t i = 0; i < count; i++)
                if (pages[i].status == KERNEL_PAGE_CACHE_STATUS_NOT_FOUND) pages[i].status = status;
        }
    } else read_batch_on_cpu0(&request);
    enum kernel_page_cache_status result = KERNEL_PAGE_CACHE_STATUS_OK;
    for (size_t i = 0; i < count; i++) if (pages[i].status) {
        pages[i].physical_address = 0;
        pages[i].valid_bytes = 0;
        if (!result) result = pages[i].status;
    }
    return result;
}

static void resize_apply(struct kernel_page_cache_resize *,uint64_t);
static int controlled_write(struct kernel_page_cache *cache,struct kernel_vfs_file *file,
    uint64_t offset,struct kernel_vfs_write_source *source,size_t size,size_t *written)
{
    struct kernel_vfs_node *node=kernel_vfs_file_node(file);
    *written=0;
    while (*written<size) {
        size_t start=offset&BOAROS_PAGE_MASK;
        size_t count=BOAROS_PAGE_SIZE-start;
        if (count>size-*written) count=size-*written;
        count=kernel_vfs_write_source_span(source,count);
        if (!count) { source->access=KERNEL_UACCESS_STATUS_STATE; return 0; }
        KERNEL_LOCK_SCOPE(io);
        kernel_vfs_node_lock(node,&io,1);
        uint64_t old_size=node->size;
        struct kernel_page_cache_resize resize={0};
        if (offset+count>old_size) {
            kernel_lock_release(&io);
            int error=kernel_page_cache_resize_prepare(cache,node,old_size,offset+count,&resize);
            if (error) return error;
            kernel_vfs_node_lock(node,&io,1);
        }
        uint64_t address; size_t valid;
        enum kernel_page_cache_status status=get_page(cache,file,offset>>BOAROS_PAGE_SHIFT,
            &address,&valid,1,0,0);
        if (status) {
            if (io.lock) kernel_lock_release(&io);
            kernel_page_cache_resize_finish(&resize,old_size);
            return status==KERNEL_PAGE_CACHE_STATUS_NO_MEMORY ? -KERNEL_ENOMEM : -KERNEL_EIO;
        }
        struct kernel_page_cache_entry *entry;
        struct kernel_page_cache_write_borrow writer={0};
        struct kernel_mm_user_pin pin={0};
        int admitted;
        {
            CACHE_SCOPE(guard,cache);
            if (lookup_entry(cache,file,offset>>BOAROS_PAGE_SHIFT,&entry)) __builtin_trap();
            entry_borrow(entry);
        }
        /* source fault可重入同一文件：先结束inode资格，且不持目标写借用。 */
        kernel_lock_release(&io);
        if (!kernel_vfs_write_source_pin(source,&pin)) {
            kernel_page_cache_resize_finish(&resize,old_size);
            { CACHE_SCOPE(guard,cache); entry_return(entry); }
            physical_page_release(cache->allocator,address);
            return 0;
        }
        kernel_vfs_node_lock(node,&io,1);
        {
            CACHE_SCOPE(guard,cache);
            admitted=resize.entry==entry || writer_begin(entry,&writer);
        }
        if (!admitted) {
            kernel_lock_release(&io);
            if (pin.registration) kernel_mm_unpin_user_page(&pin);
            kernel_page_cache_resize_finish(&resize,old_size);
            (void)kernel_page_cache_wait_writable(cache,file,offset>>BOAROS_PAGE_SHIFT,address);
            { CACHE_SCOPE(guard,cache); entry_return(entry); }
            physical_page_release(cache->allocator,address);
            continue;
        }
        void *page;
        if (physical_page_resolve(cache->allocator,address,&page)) __builtin_trap();
        kernel_vfs_write_source_copy(source,(unsigned char *)page+start,count);
        /* 复制后只清洞；同页MAP_SHARED来源可位于旧EOF尾部，不能先清源字节。 */
        resize_apply(&resize,offset>old_size?offset:old_size);
        { CACHE_SCOPE(guard,cache); dirty_mark(entry,start,start+count); }
        kernel_vfs_node_written(node,offset+count);
        file->size=node->size;
        kernel_lock_release(&io);
        if (pin.registration) kernel_mm_unpin_user_page(&pin);
        if (writer.registration) kernel_page_cache_write_end(&writer);
        kernel_page_cache_resize_finish(&resize,old_size);
        { CACHE_SCOPE(guard,cache); entry_return(entry); }
        physical_page_release(cache->allocator,address);
        kernel_vfs_write_source_advance(source,count);
        offset+=count; *written+=count;
        pressure_notify(cache);
    }
    return 0;
}

int kernel_page_cache_write_source(struct kernel_page_cache *cache,
    struct kernel_vfs_file *file,uint64_t offset,
    struct kernel_vfs_write_source *source,size_t size,size_t *written)
{
    struct kernel_vfs_node *node=kernel_vfs_file_node(file);
    /* 来源缺页可以重入同一inode，调用者不得跨此入口持有数据锁。 */
    if (kernel_lock_held(&node->io_lock,0)) __builtin_trap();
    if (cache->record->controlled) return controlled_write(cache,file,offset,source,size,written);
    *written = 0;
    while (*written < size) {
        uint64_t address;
        size_t valid, start = (size_t)(offset & (BOAROS_PAGE_SIZE - 1U));
        size_t count = BOAROS_PAGE_SIZE - start;
        struct kernel_page_cache_entry *entry;
        void *page;
        enum kernel_page_cache_status status;
        if (count > size - *written) count = size - *written;
        count=kernel_vfs_write_source_span(source,count);
        if (!count) { source->access=KERNEL_UACCESS_STATUS_STATE; return 0; }
        struct kernel_mm_user_pin pin={0};
        if (!kernel_vfs_write_source_pin(source,&pin)) return 0;
        KERNEL_LOCK_SCOPE(io);
        kernel_vfs_node_lock(node,&io,1);
        int overwritten = 0;
        struct kernel_vfs_write_source *whole_page = !start && count == BOAROS_PAGE_SIZE ? source : 0;
        status = get_page(cache, file, offset >> BOAROS_PAGE_SHIFT,
                           &address, &valid, 1, whole_page, &overwritten);
        if (status != KERNEL_PAGE_CACHE_STATUS_OK) {
            kernel_lock_release(&io);
            if (pin.registration) kernel_mm_unpin_user_page(&pin);
            return status == KERNEL_PAGE_CACHE_STATUS_NO_MEMORY
                       ? -KERNEL_ENOMEM : -KERNEL_EIO;
        }
        if (lookup_entry(cache, file, offset >> BOAROS_PAGE_SHIFT, &entry) !=
                KERNEL_PAGE_CACHE_STATUS_OK ||
            physical_page_resolve(cache->allocator, address, &page) !=
                PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
        uintptr_t irq = arch_interrupt_save();
        if (!overwritten) {
            uint64_t old_size = kernel_vfs_node_size(entry->node);
            kernel_vfs_write_source_copy(source,(unsigned char *)page+start,count);
            if (offset > old_size)
                kernel_page_cache_extend(cache, entry->node, old_size, offset);
            dirty_mark(entry, start, start + count);
        }
        offset += count;
        *written += count;
        kernel_vfs_node_written(entry->node, offset);
        arch_interrupt_restore(irq);
        kernel_lock_release(&io);
        if (pin.registration) kernel_mm_unpin_user_page(&pin);
        kernel_vfs_write_source_advance(source,count);
        (void)physical_page_release(cache->allocator, address);
        pressure_notify(cache);
    }
    return 0;
}

static int complete_writeback(struct kernel_page_cache *cache,
    struct kernel_page_cache_entry *entry,size_t begin,size_t end,uint64_t generation,
    int result,size_t written)
{
    COST_ADD(WRITEBACK_ACCEPTED,written);
    if (written > end-begin) __builtin_trap();
    if (!result && written != end-begin) result=-KERNEL_EIO;
    CACHE_SCOPE(guard,cache);
    if (!result && !entry->retired && generation == entry->generation) {
        if (end == entry->dirty_end) dirty_clear(entry);
        else entry->dirty_begin=end;
    }
    if (!entry->writeback || !cache->record->writeback_active) __builtin_trap();
    entry->writeback_error=result; entry->writeback=0; cache->record->writeback_active--;
    cache_wake(&entry->ready);
    /* A force round can skip this foreground-owned entry and go to sleep.
     * Successful completion must republish work if a newer dirty generation
     * remains. Actual I/O errors retain their owner without a retry loop. */
    if (!result && cache->record->controlled && cache->record->started &&
        !cache->record->stopping && cache->record->force_writeback && !entry->retired && entry->dirty_end) {
        cache->record->requested=1;
        cache_wake(&cache->record->work);
    }
    return result;
}
struct prepared_writeback {
    struct kernel_page_cache_entry *entry;
    struct cache_version *version;
    int closed;
    size_t begin,end;
    uint64_t generation;
    struct kernel_vfs_writeback_span *span;
};

struct writeback_workspace {
    struct prepared_writeback *prepared;
    struct kernel_vfs_writeback_span *spans;
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
    if (kernel_heap_release(cache->heap,work->prepared)) __builtin_trap();
}

static void writeback_unprepare(struct kernel_page_cache *cache,
    struct prepared_writeback *prepared,int error)
{
    if (prepared->version) version_release(prepared->version);
    { CACHE_SCOPE(guard,cache);
      struct kernel_page_cache_entry *entry=prepared->entry;
      if (prepared->closed) {
          uint64_t previous=__atomic_fetch_and(&entry->writes,~CACHE_WRITES_CLOSED,__ATOMIC_RELEASE);
          if (!(previous&CACHE_WRITES_CLOSED)) __builtin_trap();
      }
      entry->writeback=0; cache->record->writeback_active--;
      entry_return(entry); cache_wake(&entry->ready); }
    (void)error;
}

static int prepare_writeback(struct kernel_page_cache *cache,
    struct kernel_page_cache_entry *entry,uint64_t limit,size_t slot,
    struct prepared_writeback *prepared)
{
    int controlled=cache->record->controlled;
    if (controlled) for (struct kernel_page_cache_write_borrow *b=kernel_io_context_current()->cache_writes;b;b=b->next)
        if (b->entry==entry) return -KERNEL_EBUSY;
    for (;;) {
        struct kernel_wait_token token={0};
        {
            CACHE_SCOPE(guard,cache);
            uint64_t start=entry->page_index<<BOAROS_PAGE_SHIFT;
            if (entry->retired || !entry->dirty_end || limit<=start || limit-start<=entry->dirty_begin) return 0;
            if (entry->loading || !kernel_vfs_node_writeback_allowed(entry->node)) return -KERNEL_EBUSY;
            if (!entry->writeback && !(__atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)&CACHE_WRITES_CLOSED)) {
                entry->writeback=1; cache->record->writeback_active++; entry_borrow(entry); break;
            }
            if (kernel_io_context_current()->background_reclaim) { entry->retry_blocked=1; return -KERNEL_EBUSY; }
            if (kernel_wait_prepare(&entry->ready,0,0,&token)) __builtin_trap();
        }
        enum kernel_wait_wake_reason reason;
        if (kernel_wait_park(&token,&reason) || kernel_wait_finish(&token)) __builtin_trap();
    }
    prepared->entry=entry;
    prepared->version=version_take(cache,slot);
    if (!prepared->version) { writeback_unprepare(cache,prepared,-KERNEL_ENOMEM); return -KERNEL_ENOMEM; }
    int result=0;
    if (controlled) {
        CACHE_SCOPE(guard,cache);
        if (kernel_io_context_current()->background_reclaim) {
            uint64_t expected=0;
            entry->retry_blocked=1;
            if (!__atomic_compare_exchange_n(&entry->writes,&expected,CACHE_WRITES_CLOSED,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
                result=-KERNEL_EBUSY;
            else prepared->closed=1;
        } else {
            uint64_t previous=__atomic_fetch_or(&entry->writes,CACHE_WRITES_CLOSED,__ATOMIC_ACQ_REL);
            if (previous&CACHE_WRITES_CLOSED || previous&~CACHE_WRITES_MASK) __builtin_trap();
            prepared->closed=1;
        }
    }
    if (!result && controlled) result=rearm_aliases(entry);
    while (!result && controlled) {
        struct kernel_wait_token token={0};
        {
            CACHE_SCOPE(guard,cache);
            if (!(__atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)&UINT32_MAX)) break;
            if (kernel_wait_prepare(&entry->ready,0,0,&token)) __builtin_trap();
        }
        enum kernel_wait_wake_reason reason;
        if (kernel_wait_park(&token,&reason) || kernel_wait_finish(&token)) __builtin_trap();
    }
    if (result) { writeback_unprepare(cache,prepared,result); return result; }
    return 0;
}

static void capture_writeback(struct kernel_page_cache *cache,
    struct prepared_writeback *prepared,uint64_t limit,struct kernel_vfs_writeback_span *span)
{
    struct kernel_page_cache_entry *entry=prepared->entry;
    int controlled=cache->record->controlled;
    void *page;
    if (physical_page_resolve(cache->allocator,entry->physical_address,&page)) __builtin_trap();
    {
        CACHE_SCOPE(guard,cache);
        if (!controlled) for (struct kernel_page_cache_alias *alias=entry->aliases;alias;alias=alias->next)
            if (alias->rearm(alias->owner,alias->virtual_address)) __builtin_trap();
        prepared->begin=entry->dirty_begin; prepared->end=entry->dirty_end;
        prepared->generation=entry->generation;
        uint64_t start=entry->page_index<<BOAROS_PAGE_SHIFT;
        size_t valid=entry_valid_bytes(entry);
        if (prepared->end>valid) prepared->end=valid;
        if (limit>start && prepared->end>limit-start) prepared->end=(size_t)(limit-start);
        if (entry->retired) prepared->begin=prepared->end=0;
        if (prepared->end<prepared->begin) prepared->end=prepared->begin;
        if (prepared->end>prepared->begin) {
            size_t size=prepared->end-prepared->begin;
            size_t owner_bytes=KERNEL_HEAP_ALIGNMENT;
            while (owner_bytes<sizeof(struct cache_version)) owner_bytes*=2;
            size_t block=entry->node->instance->writeback_block_size;
            if (!block) block=1;
            /* A filesystem block larger than a page is assembled by its backend. */
            if (block>BOAROS_PAGE_SIZE) block=BOAROS_PAGE_SIZE;
            if (block&(block-1)) __builtin_trap();
            size_t first=prepared->begin&~(block-1);
            size_t last=(prepared->end+block-1)&~(block-1);
            size_t copy_end=last<valid?last:valid;
            void *stable=(unsigned char *)version_data(prepared->version)+first;
            void *snapshot=(unsigned char *)stable+prepared->begin-first;
            /* Stable block coverage and logical dirty bytes have different bounds.
             * In particular, padding a final block must not extend inode size. */
            COST_ADD(SNAPSHOT_COPY,copy_end-first); COST_ADD(WRITEBACK_REQUESTED,size);
            memcpy(stable,(unsigned char *)page+first,copy_end-first);
            memset((unsigned char *)stable+copy_end-first,0,last-copy_end);
            prepared->span=span;
            *span=(struct kernel_vfs_writeback_span){
                .offset=start+prepared->begin,.buffer=snapshot,.size=size,
                .owner=prepared->version,.retain=version_retain,.release=version_release,
                .source_bytes=BOAROS_PAGE_SIZE+owner_bytes,
                .source_buffer=stable,.source_size=last-first};
        }
        if (prepared->closed) {
            uint64_t previous=__atomic_fetch_and(&entry->writes,~CACHE_WRITES_CLOSED,__ATOMIC_RELEASE);
            if (previous!=CACHE_WRITES_CLOSED) __builtin_trap();
            cache_wake(&entry->ready);
        }
        entry->retry_blocked=0;
        prepared->closed=0;
    }
}

/* Candidates are already pinned. Each selected page supplies a separate stable
 * version, so the batch never requires physically contiguous snapshot storage. */
static int writeback_run(struct kernel_page_cache *cache,
    struct kernel_page_cache_entry **entries,size_t count,uint64_t limit,size_t *used,
    struct writeback_workspace *workspace)
{
    if (!count || count>BOAROS_PAGE_CACHE_WRITEBACK_PAGES) __builtin_trap();
    if (kernel_io_context_current()->background_reclaim && cache->record->started && count>cache->record->snapshot_count)
        count=cache->record->snapshot_count;
    int background=kernel_io_context_current()->background_reclaim && cache->record->started;
    struct prepared_writeback single_prepared;
    struct kernel_vfs_writeback_span single_span;
    struct writeback_workspace local={.prepared=&single_prepared,.spans=&single_span};
    if (background) workspace=cache->record->writeback_workspace;
    if (!workspace) {
        if (count!=1) __builtin_trap();
        workspace=&local;
    }
    memset(workspace->prepared,0,count*sizeof(*workspace->prepared));
    memset(workspace->spans,0,count*sizeof(*workspace->spans));
    struct prepared_writeback *prepared=workspace->prepared;
    struct kernel_vfs_writeback_span *spans=workspace->spans;
    size_t ready=0,done=0;
    int result=0;
    for (;done<count;done++) {
        int r=prepare_writeback(cache,entries[done],limit,done,&prepared[done]);
        if (r) { result=r; break; }
    }
    KERNEL_LOCK_SCOPE(io);
    if (done) kernel_vfs_node_lock(entries[0]->node,&io,0);
    for (size_t i=0;i<done;i++) {
        if (!prepared[i].entry) continue;
        capture_writeback(cache,&prepared[i],limit,&spans[ready]);
        if (prepared[i].span) ready++;
    }
    /* A later allocation failure still permits the already captured pages.
     * No failed device write is retried as a smaller batch. */
    int submitted=ready ? kernel_vfs_node_writeback(entries[0]->node,spans,ready) : 0;
    for (size_t i=0;i<ready;i++) if (!submitted && spans[i].written!=spans[i].size) submitted=-KERNEL_EIO;
    if (submitted) result=submitted;
    for (size_t i=done;i>0;i--) {
        struct prepared_writeback *p=&prepared[i-1];
        if (!p->entry) continue;
        int r=complete_writeback(cache,p->entry,p->begin,p->end,p->generation,
            p->span ? submitted : 0,p->span ? p->span->written : 0);
        if (r && !result) result=r;
        version_release(p->version);
        { CACHE_SCOPE(guard,cache); entry_return(p->entry); }
    }
    *used=done?done:1;
    return result;
}

static int writeback_entry(struct kernel_page_cache *cache,
    struct kernel_page_cache_entry *entry,uint64_t limit)
{
    size_t used;
    return writeback_run(cache,&entry,1,limit,&used,NULL);
}

int kernel_page_cache_writeback_range(struct kernel_page_cache *cache,
    struct kernel_vfs_node *node,uint64_t start,uint64_t end)
{
    if (!cache_live(cache) || !node || start>=end) return -KERNEL_EINVAL;
    KERNEL_LOCK_SCOPE(node_guard);
    if (cache->record->controlled) {
        if (kernel_lock_held(&node->io_lock,0)) __builtin_trap();
    } else kernel_vfs_node_lock(node,&node_guard,0);
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
        if (!multiple) {single=entry;entry_borrow(single);}
    }
    struct writeback_workspace work={0};
    if (multiple && writeback_workspace_init(cache,&work)) return -KERNEL_ENOMEM;
    int result=0;
    for (;;) {
        struct kernel_page_cache_entry **selected=multiple?work.selected:&single;
        size_t count=0,used;
        if (multiple) {
            CACHE_SCOPE(guard,cache);
            struct kernel_page_cache_entry *entry=dirty_at_or_after(node,cursor);
            while (entry && entry->page_index<=last && count<BOAROS_PAGE_CACHE_WRITEBACK_PAGES) {
                selected[count++]=entry;entry_borrow(entry);
                COST_ADD(WRITEBACK_VISITS,1);
                entry=RB_NEXT(kernel_page_cache_dirty_tree,&kernel_vfs_node_dirty_pages(node)->tree,entry);
            }
        } else count=1;
        if (!count) break;
        /* Store only the key across I/O; a concurrent truncate may retire all
         * other pages. This window alone borrows entry storage until return. */
        cursor=selected[count-1]->page_index+1;
        result=writeback_run(cache,selected,count,end,&used,multiple?&work:NULL);
        {CACHE_SCOPE(guard,cache);for(size_t i=0;i<count;i++)entry_return(selected[i]);}
        if (result || !multiple || cursor>last) break;
    }
    if (multiple) writeback_workspace_release(cache,&work);
    return result;
}

int kernel_page_cache_writeback_before(struct kernel_page_cache *cache,
    struct kernel_vfs_node *node, uint64_t end)
{ return end ? kernel_page_cache_writeback_range(cache, node, 0, end) : 0; }

int kernel_page_cache_writeback(struct kernel_page_cache *cache, struct kernel_vfs_node *node)
{ return kernel_page_cache_writeback_range(cache, node, 0, UINT64_MAX); }

int kernel_page_cache_resize_prepare(struct kernel_page_cache *cache,struct kernel_vfs_node *node,
    uint64_t old_size,uint64_t target,struct kernel_page_cache_resize *token)
{
    if (!token || token->entry || !kernel_page_cache_is_controlled(cache)) return -KERNEL_EINVAL;
    kernel_assert_service_context();
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
    for (;;) {
        struct kernel_wait_token wait={0};
        {
            CACHE_SCOPE(guard,cache);
            if (!entry->writeback && !entry->loading && !(__atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)&CACHE_WRITES_CLOSED)) {
                __atomic_fetch_or(&entry->writes,CACHE_WRITES_CLOSED,__ATOMIC_ACQ_REL); break;
            }
            if (kernel_wait_prepare(&entry->ready,0,0,&wait)) __builtin_trap();
        }
        enum kernel_wait_wake_reason reason;
        if (kernel_wait_park(&wait,&reason) || kernel_wait_finish(&wait)) __builtin_trap();
    }
    int result=rearm_aliases(entry);
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
        if (physical_page_resolve(cache->allocator,entry->physical_address,&page)) __builtin_trap();
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

static void resize_rearm_aliases(struct kernel_page_cache_entry *entry)
{
    for (struct kernel_page_cache_alias *alias = entry->aliases; alias; alias = alias->next) {
        COST_ADD(RESIZE_ALIAS_REARMS, 1);
        alias->rearm(alias->owner, alias->virtual_address);
    }
}

void kernel_page_cache_extend(struct kernel_page_cache *cache,
    struct kernel_vfs_node *node, uint64_t old_size, uint64_t size)
{
    if (size < old_size) __builtin_trap();
    size_t tail = old_size & BOAROS_PAGE_MASK;
    if (size == old_size || !tail) return;
    int found;
    size_t bucket = find_bucket(cache->record, node, old_size >> BOAROS_PAGE_SHIFT, &found);
    if (!found) return;
    struct kernel_page_cache_entry *entry = cache->record->buckets[bucket];
    COST_ADD(RESIZE_VISITS, 1);
    COST_ADD(RESIZE_TAIL_PAGES, 1);
    resize_rearm_aliases(entry);
    void *page;
    if (physical_page_resolve(cache->allocator, entry->physical_address, &page) != PHYSICAL_PAGE_STATUS_OK)
        __builtin_trap();
    uint64_t exposed = size - old_size;
    if (exposed > BOAROS_PAGE_SIZE - tail) exposed = BOAROS_PAGE_SIZE - tail;
    /* 共享映射可能改过旧EOF以外的尾部；长度发布前清除新暴露的字节。 */
    memset((unsigned char *)page + tail, 0, (size_t)exposed);
    entry->generation++;
}

void kernel_page_cache_truncate(struct kernel_page_cache *cache,
    struct kernel_vfs_node *node, uint64_t size)
{
    if (cache->record->controlled) {
        kernel_assert_service_context();
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
    if (cache_live(cache)) readahead_cancel_node(cache, node);
    struct kernel_page_cache_entry *entry = *kernel_vfs_node_cache_pages(node);
    uint64_t released = 0;
    while (entry) {
        COST_ADD(RESIZE_VISITS, 1);
        struct kernel_page_cache_entry *next = entry->node_next;
        uint64_t start = entry->page_index << BOAROS_PAGE_SHIFT;
        if (entry->writeback) __builtin_trap();
        if (start >= size) {
            remove_entry(cache, entry);
        } else if (size - start < BOAROS_PAGE_SIZE) {
            size_t tail = (size_t)(size - start);
            void *page;
            resize_rearm_aliases(entry);
            if (physical_page_resolve(cache->allocator, entry->physical_address, &page) != PHYSICAL_PAGE_STATUS_OK)
                __builtin_trap();
            memset((unsigned char *)page + tail, 0, BOAROS_PAGE_SIZE - tail);
            if (entry->dirty_end > tail) entry->dirty_end = tail;
            if (entry->dirty_end && entry->dirty_begin >= entry->dirty_end) {
                dirty_clear(entry);
            }
            entry->generation++;
        }
        entry = next;
    }
    (void)drain_entries(cache, &released);
    cache->record->statistics.pages_reclaimed += released;
}

uint64_t kernel_page_cache_reclaim(struct kernel_page_cache *cache,
                                   uint64_t target_pages)
{
    struct kernel_page_cache_entry *entry;
    uint64_t reclaimed = 0U;
    uint64_t released = 0U;
    uint64_t scanned = 0U;
    uint64_t scan_limit;

    if (!cache_live(cache) || target_pages == 0U ||
        cache->record->writeback_active != 0U) {
        return 0U;
    }
    if (cache->record->controlled) {
        kernel_assert_service_context();
        {
            CACHE_SCOPE(guard,cache);
            cache->record->statistics.reclaim_calls++;
            scan_limit=cache->record->occupied;
            entry=cache->record->lru_tail;
            while (entry && reclaimed<target_pages && scanned++<scan_limit) {
                struct kernel_page_cache_entry *previous=entry->lru_previous;
                uint32_t references;
                if (!entry->dirty_end && !entry->loading && !entry->writeback && !entry->users &&
                    !entry->aliases && !__atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE) &&
                    !physical_page_reference_count(cache->allocator,entry->physical_address,&references) && references==1) {
                    remove_entry(cache,entry); reclaimed++;
                }
                entry=previous;
            }
        }
        (void)drain_entries(cache,&released);
        { CACHE_SCOPE(guard,cache); cache->record->statistics.pages_reclaimed+=released; }
        return released;
    }
    cache->record->statistics.reclaim_calls++;
    scan_limit = cache->record->occupied;
    entry = cache->record->lru_tail;
    while (entry != 0 && reclaimed < target_pages &&
           scanned < scan_limit) {
        struct kernel_page_cache_entry *previous = entry->lru_previous;
        uint32_t references;

        scanned++;
        if (physical_page_reference_count(cache->allocator,
                                           entry->physical_address,
                                           &references) ==
                PHYSICAL_PAGE_STATUS_OK &&
            references == 1U && !entry->loading && !entry->users && !entry->writeback &&
            (!entry->dirty_end || !kernel_io_context_current()->locks)) {
            KERNEL_LOCK_SCOPE(reclaim_guard);
            int dirty = entry->dirty_end != 0;
            if (!dirty || kernel_vfs_node_try_read(entry->node, &reclaim_guard)) {
                if (writeback_entry(cache, entry, UINT64_MAX) == 0 &&
                    physical_page_reference_count(cache->allocator, entry->physical_address, &references) == PHYSICAL_PAGE_STATUS_OK &&
                    references == 1 && !entry->aliases) {
                    remove_entry(cache, entry);
                    if (reclaim_guard.lock) kernel_lock_release(&reclaim_guard);
                    (void)drain_entries(cache, &released);
                    reclaimed = released;
                }
                /* An I/O sleep can invalidate unrelated LRU links. */
                if (dirty) previous = cache->record->lru_tail;
            }
        }
        entry = previous;
    }
    (void)drain_entries(cache, &released);
    cache->record->statistics.pages_reclaimed += released;
    return released;
}

static void group_notify(void *context)
{
    struct page_cache_group *g = context;
    if (kernel_io_context_current()->background_reclaim) return;
    uintptr_t irq = arch_interrupt_save();
    uint64_t dirty = group_dirty(g);
    for (struct kernel_page_cache_record *r = g->head; r; r = r->next) {
        if (!r->started || r->stopping) continue;
        if (physical_page_available(g->allocator) > r->low && dirty < r->dirty_high) continue;
        if (physical_page_available(g->allocator) <= r->low) readahead_cancel_node(r->owner, 0);
        r->requested = 1;
        if (r->work.head) (void)kernel_wait_queue_wake_all(&r->work);
    }
    arch_interrupt_restore(irq);
}

static void pressure_notify(void *context)
{
    struct kernel_page_cache *cache = context;
    if (!cache_live(cache)) return;
    if (cache->record->controlled) {
        CACHE_SCOPE(guard,cache);
        if (cache->record->started && !cache->record->stopping &&
            cache->record->dirty_pages>=cache->record->dirty_high) {
            cache->record->requested=1; cache_wake(&cache->record->work);
        }
    } else if (cache->record->group) group_notify(cache->record->group);
}
enum kernel_page_cache_status kernel_page_cache_request_writeback(struct kernel_page_cache *cache)
{
    kernel_assert_service_context();
    if (!kernel_page_cache_is_controlled(cache)) return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    CACHE_SCOPE(guard,cache);
    if (!cache->record->started || cache->record->stopping) return KERNEL_PAGE_CACHE_STATUS_STATE;
    cache->record->requested=1; cache->record->force_writeback=1;
    cache_wake(&cache->record->work);
    return KERNEL_PAGE_CACHE_STATUS_OK;
}

static void pressure_wait(void *context)
{
    struct page_cache_group *g = context;
    struct kernel_io_context *io = kernel_io_context_current();
    if (io->locks || io->allocation_depth || io->backend_depth || io->reclaim_depth ||
        io->background_reclaim || !kernel_scheduler_can_sleep() || arch_external_interrupt_active()) return;
    uintptr_t irq = arch_interrupt_save();
    /* 等待者持有组而非实例；最后一个实例卸载后仍可安全离开队列。 */
    g->refs++;
    uint64_t progressed = g->progressed;
    if (!g->pending) {
        if (!++g->round) __builtin_trap();
        for (struct kernel_page_cache_record *r = g->head; r; r = r->next) {
            if (!r->started || r->stopping) continue;
            r->wait_round = g->round;
            g->pending++;
            r->requested = 1;
            (void)kernel_wait_queue_wake_all(&r->work);
        }
    }
    uint64_t round = g->round;
    while (g->pending && round == g->round && progressed == g->progressed) {
        enum kernel_wait_wake_reason reason;
        if (KERNEL_WAIT_RECHECK(&g->progress, 0, 0, &reason,
                (g->pending && round == g->round && progressed == g->progressed)) != KERNEL_SCHEDULER_STATUS_OK)
            __builtin_trap();
    }
    group_put(g);
    arch_interrupt_restore(irq);
}

/* 单个worker不能等待自己尚未执行的冷读；忙写回由真实结束事件重试。 */
static void cache_yield(void)
{
    KERNEL_IRQ_SCOPE(irq);
    if (kernel_scheduler_yield_current()!=KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
}
static void controlled_background(struct kernel_page_cache *cache)
{
    struct kernel_page_cache_record *r=cache->record;
    unsigned force;
    { CACHE_SCOPE(guard,cache); force=r->force_writeback; }
    struct kernel_io_context *io=kernel_io_context_current();
    io->background_reclaim=1;
    { CACHE_SCOPE(guard,cache); dirty_round_begin(r); }
    int interrupted = 0;
    for (;;) {
        struct kernel_page_cache_entry *batch[BOAROS_PAGE_CACHE_WRITEBACK_PAGES];
        size_t count;
        {
            CACHE_SCOPE(guard,cache);
            if (r->stopping || (!force && r->dirty_pages<=r->dirty_low)) break;
            count = dirty_batch(r,batch);
            if (!count && (!r->dirty_cursor || !r->dirty_remaining)) break;
        }
        if (count) {
            size_t used;
            int result=writeback_run(cache,batch,count,UINT64_MAX,&used,r->writeback_workspace);
            if (result && result!=-KERNEL_EBUSY && result!=-KERNEL_ENOMEM)
                kernel_vfs_record_writeback_error(batch[0]->node,result);
            {
                CACHE_SCOPE(guard,cache);
                if (!result) r->statistics.worker_written+=used;
                else if (result!=-KERNEL_EBUSY && result!=-KERNEL_ENOMEM) r->statistics.worker_failed+=used;
                for (size_t i=0;i<count;i++) entry_return(batch[i]);
            }
        }
        int demands;
        { CACHE_SCOPE(guard,cache); demands=r->reads_head!=0; r->statistics.worker_batches++; }
        if (demands) {
            CACHE_SCOPE(guard,cache); r->requested=1; interrupted=1;
            break;
        }
        cache_yield();
    }
    io->background_reclaim=0;
    {
        CACHE_SCOPE(guard,cache);
        if (!interrupted) { r->dirty_cursor=NULL; r->dirty_remaining=0; }
        if (!r->dirty_pages) r->force_writeback=0;
        r->completed++; cache_wake(&r->progress);
    }
}
static void controlled_worker(struct kernel_page_cache *cache)
{
    struct kernel_page_cache_record *r = cache->record;
    __atomic_store_n(&r->executor_io,kernel_io_context_current(),__ATOMIC_RELEASE);
    unsigned reads=0;
    for (;;) {
        struct cache_request *request = 0;
        struct kernel_wait_token token = {0};
        int cleanup = 0, stopping=0;
        {
            CACHE_SCOPE(check, cache);
            stopping=r->stopping;
            if (r->requested && (reads>=16 || !r->reads_head)) {
                r->requested=0; cleanup=1; reads=0;
            } else if (r->reads_head) {
                request = r->reads_head; r->reads_head = request->next;
                if (!r->reads_head) r->reads_tail = &r->reads_head;
                r->reads_running++; reads++;
            } else if (r->stopping) break;
            else if (kernel_wait_prepare(&r->work, 0, 0, &token)) __builtin_trap();
        }
        if (!request) {
            if (cleanup) {
                uint64_t released=0; (void)drain_entries(cache,&released);
                if (!stopping) controlled_background(cache);
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
            request->execute(request->argument);
            result=KERNEL_PAGE_CACHE_STATUS_OK;
        }
        uint64_t finished=request->observed?kernel_time_monotonic_ns():0;
        {
            CACHE_SCOPE(complete, cache);
            if (!r->read_requests || !r->reads_running || request->done) __builtin_trap();
            r->read_requests--; r->reads_running--; r->requests_completed++;
            if (request->observed) {
                uint64_t queued=started-request->queued_at,elapsed=finished-started;
                r->executions_completed++; r->queue_ns+=queued; r->execute_ns+=elapsed;
                if (queued>r->max_queue_ns) r->max_queue_ns=queued;
                if (elapsed>r->max_execute_ns) r->max_execute_ns=elapsed;
            }
            request->status = result; request->done = 1;
            cache_wake(&request->changed);
        }
        uint64_t released = 0; (void)drain_entries(cache, &released);
        cache_yield();
    }
    __atomic_store_n(&r->executor_io,0,__ATOMIC_RELEASE);
}

static void page_cache_worker(void *argument)
{
    struct kernel_page_cache *cache = argument;
    struct kernel_page_cache_record *r = cache->record;
    if (r->controlled) { controlled_worker(cache); return; }
    (void)arch_interrupt_save();
    struct kernel_io_context *io = kernel_io_context_current();
    io->background_reclaim = 1;
    for (;;) {
        while (!r->requested && !r->stopping) {
            enum kernel_wait_wake_reason reason;
            /* 空工作队列不是不可中断 I/O，不计入负载。 */
            if (KERNEL_WAIT_RECHECK(&r->work, 0, 1, &reason,
                (!r->requested && !r->stopping)) != KERNEL_SCHEDULER_STATUS_OK)
                __builtin_trap();
        }
        if (r->stopping) break;
        r->requested = 0;
        int pressure = physical_page_available(cache->allocator) <= r->low;
        uint64_t released = 0;
        if (pressure) {
            size_t limit = r->capacity;
            for (size_t cursor = 0; cursor < limit && !r->stopping;) {
                for (unsigned scanned = 0; cursor < limit && scanned < 64 && !r->stopping; scanned++) {
                    if (cursor >= r->capacity) break;
                    struct kernel_page_cache_entry *entry = r->buckets[cursor++];
                    r->statistics.worker_scanned++;
                    if (!entry || entry == PAGE_CACHE_TOMBSTONE || entry->loading || entry->users ||
                        entry->writeback || entry->dirty_end || entry->aliases) continue;
                    uint32_t references;
                    if (physical_page_reference_count(cache->allocator,entry->physical_address,&references) || references != 1) continue;
                    remove_entry(cache,entry); (void)drain_entries(cache,&released);
                    group_progress(r->group);
                }
                r->statistics.worker_batches++;
                if (physical_page_available(cache->allocator) >= r->high) break;
                if (kernel_scheduler_yield_current()) __builtin_trap();
            }
        }
        dirty_round_begin(r);
        while (!r->stopping && r->dirty_cursor && r->dirty_remaining &&
            (pressure ? physical_page_available(cache->allocator) < r->high : group_dirty(r->group) > r->dirty_low)) {
            struct kernel_page_cache_entry *batch[BOAROS_PAGE_CACHE_WRITEBACK_PAGES];
            size_t count = dirty_batch(r,batch), used=0;
            if (count) {
                    KERNEL_LOCK_SCOPE(guard);
                if (kernel_vfs_node_try_read(batch[0]->node,&guard)) {
                    int result=writeback_run(cache,batch,count,UINT64_MAX,&used,r->writeback_workspace);
                    int removed=0;
                    for (size_t i=0;i<count;i++) {
                        struct kernel_page_cache_entry *entry=batch[i]; entry_return(entry);
                        if (i>=used) continue;
                        if (result) {
                            r->statistics.worker_failed++;
                            if (result!=-KERNEL_ENOMEM && result!=-KERNEL_EBUSY)
                                kernel_vfs_record_writeback_error(entry->node,result);
                        } else {
                            r->statistics.worker_written++;
                            uint32_t references;
                            if (pressure && !entry->dirty_end && !entry->aliases &&
                                !physical_page_reference_count(cache->allocator,entry->physical_address,&references) && references==1) {
                                remove_entry(cache,entry); removed=1;
                            }
                        }
                    }
                    if (removed) {
                        kernel_lock_release(&guard);
                        (void)drain_entries(cache,&released); group_progress(r->group);
                    }
                } else for (size_t i=0;i<count;i++) entry_return(batch[i]);
            }
            r->statistics.worker_batches++;
            if (kernel_scheduler_yield_current()) __builtin_trap();
        }
        r->dirty_cursor=NULL; r->dirty_remaining=0;
        r->statistics.pages_reclaimed += released;
        r->completed++;
        group_complete(r, released != 0);
        (void)kernel_wait_queue_wake_all(&r->progress);
        /* 一轮无进展也休眠，只有新的发布事件才能请求下一轮。 */
        if (kernel_scheduler_yield_current() != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
    }
    io->background_reclaim = 0;
    group_complete(r, 0);
    r->completed++;
    (void)kernel_wait_queue_wake_all(&r->progress);
}

int kernel_page_cache_start_worker(struct kernel_page_cache *cache)
{
    if (!cache_live(cache) || cache->record->started) return -KERNEL_EINVAL;
    struct kernel_page_cache_record *r = cache->record;
    if (kernel_heap_allocate(cache->heap,sizeof(*r->writeback_workspace),
        (void **)&r->writeback_workspace)) return -KERNEL_ENOMEM;
    if (writeback_workspace_init(cache,r->writeback_workspace)) {
        if (kernel_heap_release(cache->heap,r->writeback_workspace)) __builtin_trap();
        r->writeback_workspace=NULL; return -KERNEL_ENOMEM;
    }
    for (size_t i=0;i<BOAROS_PAGE_CACHE_WRITEBACK_PAGES;i++) {
        struct cache_version *version=version_new(cache);
        if (!version) break;
        r->snapshots[r->snapshot_count++]=version;
    }
    if (!r->snapshot_count) {
        writeback_workspace_release(cache,r->writeback_workspace);
        if (kernel_heap_release(cache->heap,r->writeback_workspace)) __builtin_trap();
        r->writeback_workspace=NULL; return -KERNEL_ENOMEM;
    }
    uint64_t total = physical_page_total(cache->allocator);
    r->low = total / 50 ? total / 50 : 1;
    r->high = total / 25 > r->low ? total / 25 : r->low + 1;
    r->dirty_low = total / 20 ? total / 20 : 1;
    r->dirty_high = total / 10 > r->dirty_low ? total / 10 : r->dirty_low + 1;
    r->stopping = 0;
    kernel_wait_queue_init(&r->work);
    kernel_wait_queue_init(&r->progress);
    enum kernel_scheduler_status result = kernel_thread_create_joinable(page_cache_worker, cache, &r->worker);
    if (result != KERNEL_SCHEDULER_STATUS_OK) {
        while (r->snapshot_count) version_release(r->snapshots[--r->snapshot_count]);
        writeback_workspace_release(cache,r->writeback_workspace);
        if (kernel_heap_release(cache->heap,r->writeback_workspace)) __builtin_trap();
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
    r->readahead_started = 1;
#endif
    pressure_notify(cache);
    return 0;
}

void kernel_page_cache_stop_worker(struct kernel_page_cache *cache)
{
    if (!cache_live(cache) || !cache->record->started) return;
    struct kernel_page_cache_record *r = cache->record;
    uintptr_t irq = arch_interrupt_save();
    { CACHE_SCOPE(stop, cache); r->stopping = 1; r->dirty_cursor=NULL; r->dirty_remaining=0; }
    if (r->controlled) {
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
        (void)kernel_wait_queue_wake_all(&r->readahead_work);
        kernel_thread_join(&r->readahead_worker);
        r->readahead_started = 0;
    }
#endif
    (void)kernel_wait_queue_wake_all(&r->work);
    (void)kernel_wait_queue_wake_all(&r->progress);
    kernel_thread_join(&r->worker);
    while (r->snapshot_count) version_release(r->snapshots[--r->snapshot_count]);
    writeback_workspace_release(cache,r->writeback_workspace);
    if (kernel_heap_release(cache->heap,r->writeback_workspace)) __builtin_trap();
    r->writeback_workspace=NULL;
    r->started = 0;
    arch_interrupt_restore(irq);
}

enum kernel_page_cache_status kernel_page_cache_purge_mount(
    struct kernel_page_cache *cache,
    const struct kernel_vfs_mount *mount)
{
    struct kernel_page_cache_entry *entry;
    uint64_t released = 0U;
    int pinned = 0;

    if (!cache_live(cache) || mount == 0) {
        return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    }
    entry = cache->record->lru_tail;
    while (entry != 0) {
        struct kernel_page_cache_entry *previous = entry->lru_previous;

        if (kernel_vfs_node_mount(entry->node) == mount) {
            uint32_t references;

            if (writeback_entry(cache, entry, UINT64_MAX) != 0)
                return KERNEL_PAGE_CACHE_STATUS_IO;
            if (physical_page_reference_count(cache->allocator,
                                               entry->physical_address,
                                               &references) !=
                    PHYSICAL_PAGE_STATUS_OK ||
                references != 1U) {
                pinned = 1;
            } else {
                remove_entry(cache, entry);
            }
        }
        entry = previous;
    }
    if (!drain_entries(cache, &released)) {
        cache->record->statistics.pages_reclaimed += released;
        return KERNEL_PAGE_CACHE_STATUS_CLEANUP_REQUIRED;
    }
    cache->record->statistics.pages_reclaimed += released;
    return pinned ? KERNEL_PAGE_CACHE_STATUS_STATE
                  : KERNEL_PAGE_CACHE_STATUS_OK;
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
    if (cache->record->controlled) {
        kernel_assert_service_context();
        {
            CACHE_SCOPE(guard,cache);
            for (entry=*kernel_vfs_node_cache_pages(node);entry;entry=entry->node_next)
                if (entry->loading || entry->writeback || entry->users || entry->aliases ||
                    __atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)) return KERNEL_PAGE_CACHE_STATUS_STATE;
            entry=*kernel_vfs_node_cache_pages(node);
            while (entry) {
                struct kernel_page_cache_entry *next=entry->node_next;
                remove_entry(cache,entry); entry=next;
            }
        }
        kernel_lock_release(&node_guard);
        int complete=drain_entries(cache,&released);
        { CACHE_SCOPE(guard,cache); cache->record->statistics.pages_reclaimed+=released; }
        return complete?KERNEL_PAGE_CACHE_STATUS_OK:KERNEL_PAGE_CACHE_STATUS_CLEANUP_REQUIRED;
    }
    readahead_cancel_node(cache, node);
    entry = *kernel_vfs_node_cache_pages(node);
    while (entry != 0) {
        struct kernel_page_cache_entry *next = entry->node_next;
        if (entry->writeback) __builtin_trap();
        remove_entry(cache, entry);
        entry = next;
    }
    if (!drain_entries(cache, &released)) {
        cache->record->statistics.pages_reclaimed += released;
        return KERNEL_PAGE_CACHE_STATUS_CLEANUP_REQUIRED;
    }
    cache->record->statistics.pages_reclaimed += released;
    return KERNEL_PAGE_CACHE_STATUS_OK;
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

enum kernel_page_cache_status kernel_page_cache_destroy(
    struct kernel_page_cache *cache)
{
    struct kernel_page_cache_entry *entry;
    uint64_t released = 0U;

    if (cache == 0 ||
        (cache->state != KERNEL_PAGE_CACHE_LIVE &&
         cache->state != KERNEL_PAGE_CACHE_CLEANUP) ||
        cache->record == 0 || cache->allocator == 0 ||
        cache->heap == 0) {
        return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    }
    kernel_page_cache_stop_worker(cache);
    if (cache->state == KERNEL_PAGE_CACHE_LIVE) {
        for (entry = cache->record->lru_tail;
             entry != 0;
             entry = entry->lru_previous) {
            uint32_t references;

            if (writeback_entry(cache, entry, UINT64_MAX) != 0)
                return KERNEL_PAGE_CACHE_STATUS_IO;

            if (physical_page_reference_count(cache->allocator,
                                               entry->physical_address,
                                               &references) !=
                    PHYSICAL_PAGE_STATUS_OK ||
                references != 1U) {
                return KERNEL_PAGE_CACHE_STATUS_STATE;
            }
        }
        if (cache->record->group) group_unregister(cache);
        entry = cache->record->lru_tail;
        while (entry != 0) {
            struct kernel_page_cache_entry *previous = entry->lru_previous;
            remove_entry(cache, entry);
            entry = previous;
        }
        cache->state = KERNEL_PAGE_CACHE_CLEANUP;
    }
    if (!drain_entries(cache, &released)) {
        return KERNEL_PAGE_CACHE_STATUS_CLEANUP_REQUIRED;
    }
    if (cache->record->buckets != 0) {
        (void)kernel_heap_release(cache->heap,
                                cache->record->buckets);
        cache->record->buckets = 0;
    }
    (void)kernel_heap_release(cache->heap, cache->record);
    cache->heap = 0;
    cache->allocator = 0;
    cache->record = 0;
    cache->state = KERNEL_PAGE_CACHE_RELEASED;
    return KERNEL_PAGE_CACHE_STATUS_OK;
}
