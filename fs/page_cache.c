#include "vfs_objects.h"

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
    struct kernel_page_cache_entry *dirty_next;
    struct kernel_page_cache_entry **dirty_previous;
    struct kernel_page_cache_entry *node_next;
    struct kernel_page_cache_entry **node_previous;
    struct kernel_page_cache *cache;
    uint64_t worker_epoch;
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
    struct kernel_thread_join worker;
    struct kernel_wait_queue work, progress;
    uint64_t low, high, dirty_high, dirty_low;
    uint64_t snapshot_address, epoch, completed;
    uint64_t requested;
    int started, stopping;
    int snapshot_busy;
    uint32_t snapshot_order;
#if BOAROS_PAGE_CACHE_READAHEAD_PAGES
    struct readahead_job readahead_jobs[8];
    struct kernel_thread_join readahead_worker;
    struct kernel_wait_queue readahead_work;
    uint64_t readahead_cookie;
    int readahead_started;
#endif

};

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

static void zero_bytes(void *pointer, size_t size)
{
    unsigned char *bytes = pointer;
    size_t index;

    for (index = 0U; index < size; index++) {
        bytes[index] = 0U;
    }
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
static int reserve_bucket(struct kernel_page_cache *cache)
{
    for (;;) {
        size_t old_capacity;
        {
            CACHE_SCOPE(guard, cache);
            if ((cache->record->occupied + cache->record->tombstones + 1U) * 4U <
                cache->record->capacity * 3U) return 1;
            old_capacity = cache->record->capacity;
        }
        if (old_capacity > SIZE_MAX / 2U) return 0;
        size_t capacity = old_capacity * 2;
        struct kernel_page_cache_entry **buckets;
        if (kernel_heap_allocate_zeroed(cache->heap, capacity, sizeof(*buckets),
                (void **)&buckets) != KERNEL_HEAP_STATUS_OK) return 0;
        struct kernel_page_cache_entry **retired = 0;
        int published = 0;
        {
            CACHE_SCOPE(guard, cache);
            struct kernel_page_cache_record *r = cache->record;
            if (r->capacity == old_capacity) {
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
        if (published) return 1;
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

/* 调用者在不调度的区间内更新脏范围与组织；索引不另持页面引用。 */
static void dirty_mark(struct kernel_page_cache_entry *entry, size_t begin, size_t end)
{
    if (begin >= end || end > BOAROS_PAGE_SIZE) __builtin_trap();
    if (!entry->dirty_end) {
        struct kernel_page_cache_dirty *dirty = kernel_vfs_node_dirty_pages(entry->node);
        if (entry->dirty_previous) __builtin_trap();
        entry->dirty_next = dirty->head; entry->dirty_previous = &dirty->head;
        if (entry->dirty_next) entry->dirty_next->dirty_previous = &entry->dirty_next;
        dirty->head = entry; dirty->count++;
        entry->cache->record->dirty_pages++;
        entry->dirty_begin = begin;
    } else if (begin < entry->dirty_begin) entry->dirty_begin = begin;
    if (end > entry->dirty_end) entry->dirty_end = end;
    entry->generation++;
}
static void dirty_clear(struct kernel_page_cache_entry *entry)
{
    if (!entry->dirty_previous) {
        if (entry->dirty_end) __builtin_trap();
        return;
    }
    struct kernel_page_cache_dirty *dirty = kernel_vfs_node_dirty_pages(entry->node);
    if (!dirty->count || !entry->cache->record->dirty_pages) __builtin_trap();
    *entry->dirty_previous = entry->dirty_next;
    if (entry->dirty_next) entry->dirty_next->dirty_previous = entry->dirty_previous;
    entry->dirty_next = 0; entry->dirty_previous = 0;
    dirty->count--; entry->cache->record->dirty_pages--;
    entry->dirty_begin = entry->dirty_end = 0;
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
        entry_borrow(entry);
    }
    enum kernel_page_cache_status status;
    for (;;) {
        struct kernel_wait_token token = {0};
        {
            CACHE_SCOPE(guard, cache);
            if (!entry->loading) { status = entry->load_error; break; }
            if (kernel_wait_prepare(&entry->ready, 0, 0, &token)) __builtin_trap();
        }
        enum kernel_wait_wake_reason reason;
        if (kernel_wait_park(&token, &reason) || kernel_wait_finish(&token)) __builtin_trap();
    }
    uint64_t physical = entry->physical_address;
    if (!status && physical_page_acquire(cache->allocator, physical)) __builtin_trap();
    size_t bytes = !status ? entry_valid_bytes(entry) : 0;
    int clean = 0;
    {
        CACHE_SCOPE(guard, cache);
        entry_return(entry);
        if (!status) {
            if (!entry->retired) lru_touch(cache->record, entry);
            cache->record->statistics.hits++;
        } else if (!entry->users && !entry->retired) { remove_entry(cache, entry); clean = 1; }
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
    if (status) return status;
    *address = physical; *valid = bytes;
    return KERNEL_PAGE_CACHE_STATUS_OK;
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
    if (clear) zero_bytes(page, BOAROS_PAGE_SIZE);
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
        if (!found && (cache->record->occupied + cache->record->tombstones + 1U) * 4U >=
                cache->record->capacity * 3U) {
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
    {
        /* 准备未发布的 loading 页时禁止回收 I/O，避免和需求读互等。 */
        KERNEL_NO_RECLAIM_IO;
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
            r->statistics.misses++;
            entries[count] = entry;
            spans[count++] = (struct kernel_vfs_read_span){index << BOAROS_PAGE_SHIFT, page, BOAROS_PAGE_SIZE, 0, 0};
        }
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
    int result = kernel_vfs_node_pread_batch(job->node, spans, count);
    for (unsigned i = 0; i < count; i++) {
        struct kernel_page_cache_entry *entry = entries[i];
        int error = spans[i].error;
        /* A backend must identify its failed span; reject an unexplained global failure. */
        if (result && !error) {
            int identified = 0;
            for (unsigned j = 0; j < count; j++) identified |= spans[j].error != 0;
            if (!identified) error = result;
        }
        if (spans[i].completed > BOAROS_PAGE_SIZE) __builtin_trap();
        entry->load_error = error == -KERNEL_ENOMEM ? KERNEL_PAGE_CACHE_STATUS_NO_MEMORY :
            error ? KERNEL_PAGE_CACHE_STATUS_IO : KERNEL_PAGE_CACHE_STATUS_OK;
        entry->loading = 0; entry_return(entry);
        if (entry->ready.head) (void)kernel_wait_queue_wake_all(&entry->ready);
        if (error) {
            r->statistics.readahead_failed++;
            if (!entry->users) remove_entry(cache, entry);
        } else r->statistics.readahead_loaded++;
    }
    uint64_t released = 0; (void)drain_entries(cache, &released);
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
    int for_write, const void *overwrite, int *overwritten)
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
        if (offset + BOAROS_PAGE_SIZE > old_size)
            kernel_page_cache_extend(cache, node, old_size, offset + BOAROS_PAGE_SIZE);
        memcpy(page, overwrite, BOAROS_PAGE_SIZE);
        COST_ADD(CACHE_COPY, BOAROS_PAGE_SIZE);
        kernel_vfs_node_written(node, offset + BOAROS_PAGE_SIZE);
        *overwritten = 1;
    }
    enum kernel_page_cache_status result = read_error == -KERNEL_ENOMEM ? KERNEL_PAGE_CACHE_STATUS_NO_MEMORY :
        read_error ? KERNEL_PAGE_CACHE_STATUS_IO : KERNEL_PAGE_CACHE_STATUS_OK;
    uint64_t physical = entry->physical_address;
    if (!result && physical_page_acquire(cache->allocator, physical)) __builtin_trap();
    size_t valid = !result ? entry_valid_bytes(entry) : 0;
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

static void resize_apply(struct kernel_page_cache_resize *,uint64_t);
static int controlled_write(struct kernel_page_cache *cache,struct kernel_vfs_file *file,
    uint64_t offset,const unsigned char *source,size_t size,size_t *written)
{
    struct kernel_vfs_node *node=kernel_vfs_file_node(file);
    if (kernel_lock_held(&node->io_lock,0)) __builtin_trap();
    *written=0;
    while (*written<size) {
        size_t start=offset&BOAROS_PAGE_MASK;
        size_t count=BOAROS_PAGE_SIZE-start;
        if (count>size-*written) count=size-*written;
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
        int admitted;
        {
            CACHE_SCOPE(guard,cache);
            if (lookup_entry(cache,file,offset>>BOAROS_PAGE_SHIFT,&entry)) __builtin_trap();
            entry_borrow(entry);
            admitted=resize.entry==entry || writer_begin(entry,&writer);
        }
        if (!admitted) {
            kernel_lock_release(&io);
            kernel_page_cache_resize_finish(&resize,old_size);
            (void)kernel_page_cache_wait_writable(cache,file,offset>>BOAROS_PAGE_SHIFT,address);
            { CACHE_SCOPE(guard,cache); entry_return(entry); }
            physical_page_release(cache->allocator,address);
            continue;
        }
        /* 尾页关闭同时覆盖零化、复制和长度发布；开放后才允许下一代写者。 */
        resize_apply(&resize,offset+count>old_size?offset+count:old_size);
        void *page;
        if (physical_page_resolve(cache->allocator,address,&page)) __builtin_trap();
        COST_ADD(CACHE_COPY,count);
        memcpy((unsigned char *)page+start,source+*written,count);
        { CACHE_SCOPE(guard,cache); dirty_mark(entry,start,start+count); }
        kernel_vfs_node_written(node,offset+count);
        file->size=node->size;
        kernel_lock_release(&io);
        if (writer.registration) kernel_page_cache_write_end(&writer);
        kernel_page_cache_resize_finish(&resize,old_size);
        { CACHE_SCOPE(guard,cache); entry_return(entry); }
        physical_page_release(cache->allocator,address);
        offset+=count; *written+=count;
        pressure_notify(cache);
    }
    return 0;
}

int kernel_page_cache_write(struct kernel_page_cache *cache,
                             struct kernel_vfs_file *file, uint64_t offset,
                             const void *buffer, size_t size, size_t *written)
{
    const unsigned char *source = buffer;
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
        int overwritten = 0;
        const void *whole_page = !start && count == BOAROS_PAGE_SIZE ? source + *written : 0;
        status = get_page(cache, file, offset >> BOAROS_PAGE_SHIFT,
                           &address, &valid, 1, whole_page, &overwritten);
        if (status != KERNEL_PAGE_CACHE_STATUS_OK)
            return status == KERNEL_PAGE_CACHE_STATUS_NO_MEMORY
                       ? -KERNEL_ENOMEM : -KERNEL_EIO;
        if (lookup_entry(cache, file, offset >> BOAROS_PAGE_SHIFT, &entry) !=
                KERNEL_PAGE_CACHE_STATUS_OK ||
            physical_page_resolve(cache->allocator, address, &page) !=
                PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
        uintptr_t irq = arch_interrupt_save();
        if (!overwritten) {
            uint64_t old_size = kernel_vfs_node_size(entry->node);
            if (offset + count > old_size)
                kernel_page_cache_extend(cache, entry->node, old_size, offset + count);
            COST_ADD(CACHE_COPY, count);
            memcpy((unsigned char *)page + start, source + *written, count);
            dirty_mark(entry, start, start + count);
        }
        offset += count;
        *written += count;
        kernel_vfs_node_written(entry->node, offset);
        arch_interrupt_restore(irq);
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
/* entry借用覆盖完整提交；关闭只覆盖撤写/排空/复制，不覆盖磁盘等待。 */
static int controlled_writeback(struct kernel_page_cache *cache,
    struct kernel_page_cache_entry *entry,uint64_t limit)
{
    kernel_assert_service_context();
    for (struct kernel_page_cache_write_borrow *b=kernel_io_context_current()->cache_writes;b;b=b->next)
        if (b->entry==entry) return -KERNEL_EBUSY;
    for (;;) {
        struct kernel_wait_token token={0};
        {
            CACHE_SCOPE(guard,cache);
            uint64_t start=entry->page_index<<BOAROS_PAGE_SHIFT;
            if (entry->retired) return 0;
            if (!entry->dirty_end || limit<=start || limit-start<=entry->dirty_begin) return 0;
            if (entry->loading) return -KERNEL_EBUSY;
            if (!entry->writeback && !(__atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)&CACHE_WRITES_CLOSED)) {
                entry->writeback=1; cache->record->writeback_active++; entry_borrow(entry); break;
            }
            if (kernel_io_context_current()->background_reclaim) {
                entry->retry_blocked=1; return -KERNEL_EBUSY;
            }
            if (kernel_wait_prepare(&entry->ready,0,0,&token)) __builtin_trap();
        }
        enum kernel_wait_wake_reason reason;
        if (kernel_wait_park(&token,&reason) || kernel_wait_finish(&token)) __builtin_trap();
    }
    uint64_t snapshot_address;
    void *snapshot,*page;
    int reserved=0;
    {
        CACHE_SCOPE(guard,cache);
        if (kernel_io_context_current()->background_reclaim && cache->record->started && !cache->record->snapshot_busy) {
            reserved=1; cache->record->snapshot_busy=1; snapshot_address=cache->record->snapshot_address;
        }
    }
    enum physical_page_status allocated=reserved ? PHYSICAL_PAGE_STATUS_OK : physical_page_allocate(cache->allocator,&snapshot_address);
    if (allocated) {
        if (allocated!=PHYSICAL_PAGE_STATUS_EMPTY) __builtin_trap();
        CACHE_SCOPE(guard,cache);
        entry->writeback=0; cache->record->writeback_active--; entry_return(entry); cache_wake(&entry->ready);
        return -KERNEL_ENOMEM;
    }
    if (physical_page_resolve(cache->allocator,snapshot_address,&snapshot) ||
        physical_page_resolve(cache->allocator,entry->physical_address,&page)) __builtin_trap();
    int closed=0, preparation_error=0;
    {
        CACHE_SCOPE(guard,cache);
        if (kernel_io_context_current()->background_reclaim) {
            uint64_t expected=0;
            entry->retry_blocked=1;
            if (!__atomic_compare_exchange_n(&entry->writes,&expected,CACHE_WRITES_CLOSED,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
                preparation_error=-KERNEL_EBUSY;
            else closed=1;
        } else {
            uint64_t previous=__atomic_fetch_or(&entry->writes,CACHE_WRITES_CLOSED,__ATOMIC_ACQ_REL);
            if (previous&CACHE_WRITES_CLOSED || previous&~CACHE_WRITES_MASK) __builtin_trap();
            closed=1;
        }
    }
    if (!preparation_error) preparation_error=rearm_aliases(entry);
    if (preparation_error) {
        {
            CACHE_SCOPE(guard,cache);
            if (closed) {
                uint64_t previous=__atomic_fetch_and(&entry->writes,~CACHE_WRITES_CLOSED,__ATOMIC_RELEASE);
                if (!(previous&CACHE_WRITES_CLOSED)) __builtin_trap();
            }
            if (reserved) cache->record->snapshot_busy=0;
            if (kernel_io_context_current()->background_reclaim) cache->record->deferred_snapshots++;
            entry->writeback=0; cache->record->writeback_active--; entry_return(entry);
            cache_wake(&entry->ready);
        }
        if (!reserved) physical_page_release(cache->allocator,snapshot_address);
        return preparation_error;
    }
    for (;;) {
        struct kernel_wait_token token={0};
        {
            CACHE_SCOPE(guard,cache);
            if (!(__atomic_load_n(&entry->writes,__ATOMIC_ACQUIRE)&UINT32_MAX)) break;
            if (kernel_wait_prepare(&entry->ready,0,0,&token)) __builtin_trap();
        }
        enum kernel_wait_wake_reason reason;
        if (kernel_wait_park(&token,&reason) || kernel_wait_finish(&token)) __builtin_trap();
    }
    KERNEL_LOCK_SCOPE(io);
    kernel_vfs_node_lock(entry->node,&io,0);
    uint64_t epoch;
    int result=kernel_file_mapping_epoch(entry->node,&epoch);
    size_t begin,end;
    uint64_t generation;
    {
        CACHE_SCOPE(guard,cache);
        begin=entry->dirty_begin; end=entry->dirty_end; generation=entry->generation;
        /* 截断可在旧pin排空前退休整页，writeback借用继续固定entry。
         * 它已没有可提交内容；不能把旧快照写进重新增长的文件。 */
        if (entry->retired) { begin=end=0; result=0; }
        uint64_t start=entry->page_index<<BOAROS_PAGE_SHIFT;
        size_t valid=entry_valid_bytes(entry);
        if (end>valid) end=valid;
        if (limit>start && end>limit-start) end=(size_t)(limit-start);
        if (end<begin) end=begin;
    }
    if (!result && end>begin) {
        COST_ADD(SNAPSHOT_COPY,end-begin); COST_ADD(WRITEBACK_REQUESTED,end-begin);
        memcpy((unsigned char *)snapshot+begin,(unsigned char *)page+begin,end-begin);
    }
    {
        CACHE_SCOPE(guard,cache);
        entry->retry_blocked=0;
        uint64_t previous=__atomic_fetch_and(&entry->writes,~CACHE_WRITES_CLOSED,__ATOMIC_RELEASE);
        if (previous!=CACHE_WRITES_CLOSED) __builtin_trap();
        cache_wake(&entry->ready);
    }
    size_t written=0;
    if (!result && end>begin) result=kernel_vfs_node_writeback(entry->node,
        (entry->page_index<<BOAROS_PAGE_SHIFT)+begin,(unsigned char *)snapshot+begin,end-begin,&written);
    if (reserved) { CACHE_SCOPE(guard,cache); cache->record->snapshot_busy=0; }
    else if (physical_page_release(cache->allocator,snapshot_address)) __builtin_trap();
    result=complete_writeback(cache,entry,begin,end,generation,result,written);
    { CACHE_SCOPE(guard,cache); entry_return(entry); }
    return result;
}

static int writeback_entry(struct kernel_page_cache *cache,
                           struct kernel_page_cache_entry *entry, uint64_t limit)
{
    if (cache->record->controlled) return controlled_writeback(cache,entry,limit);
    void *page;
    size_t written = 0, begin, end;
    uint64_t generation;
    int result;
    uintptr_t wait_irq = arch_interrupt_save();
    while (entry->writeback) {
        enum kernel_wait_wake_reason reason;
        if (KERNEL_WAIT_RECHECK(&entry->ready, 0, 0, &reason,
                (entry->writeback)) != KERNEL_SCHEDULER_STATUS_OK)
            __builtin_trap();
    }
    arch_interrupt_restore(wait_irq);
    if (entry->dirty_end == 0) return 0;
    uint64_t start = entry->page_index << BOAROS_PAGE_SHIFT;
    if (limit <= start || limit - start <= entry->dirty_begin) return 0;
    /* A bcache miss may allocate while an outer ext4 read owns fpos and
     * buffer lookup state. Reclamation there can free clean pages only. */
    if (entry->loading || !kernel_vfs_node_writeback_allowed(entry->node))
        return -KERNEL_EBUSY;
    entry->writeback = 1;
    cache->record->writeback_active++;
    if (physical_page_acquire(cache->allocator, entry->physical_address) != PHYSICAL_PAGE_STATUS_OK ||
        physical_page_resolve(cache->allocator, entry->physical_address, &page) != PHYSICAL_PAGE_STATUS_OK)
        __builtin_trap();
    uint64_t snapshot_address;
    void *snapshot;
    int reserved = kernel_io_context_current()->background_reclaim &&
                   cache->record->started && !cache->record->snapshot_busy;
    enum physical_page_status allocated;
    if (reserved) {
        cache->record->snapshot_busy = 1;
        snapshot_address = cache->record->snapshot_address;
        allocated = PHYSICAL_PAGE_STATUS_OK;
    } else allocated = physical_page_allocate(cache->allocator, &snapshot_address);
    if (allocated != PHYSICAL_PAGE_STATUS_OK) {
        if (allocated != PHYSICAL_PAGE_STATUS_EMPTY) __builtin_trap();
        (void)physical_page_release(cache->allocator, entry->physical_address);
        entry->writeback = 0; cache->record->writeback_active--;
        uintptr_t irq = arch_interrupt_save();
        if (entry->ready.head) (void)kernel_wait_queue_wake_all(&entry->ready);
        arch_interrupt_restore(irq);
        return -KERNEL_ENOMEM;
    }
    if (physical_page_resolve(cache->allocator, snapshot_address, &snapshot) != PHYSICAL_PAGE_STATUS_OK)
        __builtin_trap();
    uintptr_t irq = arch_interrupt_save();
    for (struct kernel_page_cache_alias *alias = entry->aliases; alias; alias = alias->next)
        alias->rearm(alias->owner, alias->virtual_address);
    begin = entry->dirty_begin;
    end = entry->dirty_end;
    if (end > limit - start) end = (size_t)(limit - start);
    generation = entry->generation;
    COST_ADD(SNAPSHOT_COPY, end - begin);
    COST_ADD(WRITEBACK_REQUESTED, end - begin);
    memcpy((unsigned char *)snapshot + begin, (unsigned char *)page + begin, end - begin);
    arch_interrupt_restore(irq);
    result = kernel_vfs_node_writeback(entry->node,
                    (entry->page_index << BOAROS_PAGE_SHIFT) + begin,
                    (unsigned char *)snapshot + begin, end - begin, &written);
    if (reserved) cache->record->snapshot_busy = 0;
    else (void)physical_page_release(cache->allocator, snapshot_address);
    result=complete_writeback(cache,entry,begin,end,generation,result,written);
    (void)physical_page_release(cache->allocator,entry->physical_address);
    return result;
}

static uint32_t writeback_order(size_t count)
{
    uint32_t order = 0;
    while (((size_t)1 << order) < count) order++;
    return order;
}

/* 同一 inode 锁下只合并相接的脏字节；相邻页中的干净间隙不进入快照。 */
static int writeback_adjacent(const struct kernel_page_cache_entry *left,
    const struct kernel_page_cache_entry *right, uint64_t limit)
{
    uint64_t start = right->page_index << BOAROS_PAGE_SHIFT;
    return left->node == right->node && left->cache == right->cache &&
        right->page_index == left->page_index + 1 &&
        left->dirty_end == BOAROS_PAGE_SIZE && right->dirty_begin == 0 &&
        right->dirty_end && !right->loading && !right->writeback && limit > start;
}

/* Caller owns an inode read lock and pins every candidate across waits.
 * Only *used entries were submitted; a snapshot OOM falls back before I/O. */
static int writeback_run(struct kernel_page_cache *cache,
    struct kernel_page_cache_entry **entries, size_t count, uint64_t limit, size_t *used)
{
    *used = 1;
    if (!count || count > BOAROS_PAGE_CACHE_WRITEBACK_PAGES) __builtin_trap();
    if (count == 1 || entries[0]->writeback || entries[0]->loading || !entries[0]->dirty_end)
        return writeback_entry(cache, entries[0], limit);
    struct {
        size_t begin, end;
        uint64_t generation;
    } saved[BOAROS_PAGE_CACHE_WRITEBACK_PAGES];
    struct kernel_page_cache_record *record = cache->record;
    uint64_t snapshot_address;
    uint32_t order = writeback_order(count);
    int reserved = kernel_io_context_current()->background_reclaim &&
        record->started && !record->snapshot_busy && order <= record->snapshot_order;
    if (reserved) {
        record->snapshot_busy = 1;
        snapshot_address = record->snapshot_address;
    } else {
        enum physical_page_status status = physical_page_allocate_order(cache->allocator, order, &snapshot_address);
        if (status != PHYSICAL_PAGE_STATUS_OK) {
            if (status != PHYSICAL_PAGE_STATUS_EMPTY) __builtin_trap();
            return writeback_entry(cache, entries[0], limit);
        }
    }
    void *snapshot;
    if (physical_page_resolve(cache->allocator, snapshot_address, &snapshot) != PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
    uintptr_t irq = arch_interrupt_save();
    /* 分配可触发回收；重新检查成员，不能把已完成的页误当成本次 owner。 */
    size_t ready = 1;
    if (entries[0]->writeback || entries[0]->loading || !entries[0]->dirty_end ||
        !kernel_vfs_node_writeback_allowed(entries[0]->node)) ready = 0;
    while (ready && ready < count && writeback_adjacent(entries[ready - 1], entries[ready], limit)) ready++;
    if (ready < 2) {
        arch_interrupt_restore(irq);
        if (reserved) record->snapshot_busy = 0;
        else if (physical_page_release_order(cache->allocator, snapshot_address, order) != PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
        return writeback_entry(cache, entries[0], limit);
    }
    count = ready;
    uint64_t offset = (entries[0]->page_index << BOAROS_PAGE_SHIFT) + entries[0]->dirty_begin;
    size_t size = 0;
    for (size_t i = 0; i < count; i++) {
        struct kernel_page_cache_entry *entry = entries[i];
        uint64_t start = entry->page_index << BOAROS_PAGE_SHIFT;
        void *page;
        entry->writeback = 1; record->writeback_active++;
        if (physical_page_acquire(cache->allocator, entry->physical_address) != PHYSICAL_PAGE_STATUS_OK ||
            physical_page_resolve(cache->allocator, entry->physical_address, &page) != PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
        for (struct kernel_page_cache_alias *alias = entry->aliases; alias; alias = alias->next)
            alias->rearm(alias->owner, alias->virtual_address);
        saved[i].begin = entry->dirty_begin;
        saved[i].end = entry->dirty_end;
        if (saved[i].end > limit - start) saved[i].end = (size_t)(limit - start);
        saved[i].generation = entry->generation;
        size_t length = saved[i].end - saved[i].begin;
        COST_ADD(SNAPSHOT_COPY, length);
        memcpy((unsigned char *)snapshot + size, (unsigned char *)page + saved[i].begin, length);
        size += length;
    }
    COST_ADD(WRITEBACK_REQUESTED, size);
    arch_interrupt_restore(irq);
    size_t written = 0;
    int result = kernel_vfs_node_writeback(entries[0]->node, offset, snapshot, size, &written);
    COST_ADD(WRITEBACK_ACCEPTED, written);
    if (written > size) __builtin_trap();
    if (!result && written != size) result = -KERNEL_EIO;
    irq = arch_interrupt_save();
    for (size_t i = 0; i < count; i++) {
        struct kernel_page_cache_entry *entry = entries[i];
        if (!result && saved[i].generation == entry->generation) {
            if (saved[i].end == entry->dirty_end) dirty_clear(entry);
            else entry->dirty_begin = saved[i].end;
        }
        if (physical_page_release(cache->allocator, entry->physical_address) != PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
        entry->writeback_error = result;
        entry->writeback = 0; record->writeback_active--;
        if (entry->ready.head) (void)kernel_wait_queue_wake_all(&entry->ready);
    }
    if (reserved) record->snapshot_busy = 0;
    else if (physical_page_release_order(cache->allocator, snapshot_address, order) != PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
    arch_interrupt_restore(irq);
    *used = count;
    return result;
}

static int page_offset_compare(const void *left, const void *right)
{
    const struct kernel_page_cache_entry *a = *(struct kernel_page_cache_entry *const *)left;
    const struct kernel_page_cache_entry *b = *(struct kernel_page_cache_entry *const *)right;
    return a->page_index < b->page_index ? -1 : a->page_index > b->page_index;
}

static int controlled_writeback_range(struct kernel_page_cache *cache,
    struct kernel_vfs_node *node,uint64_t start,uint64_t end)
{
    struct kernel_page_cache_entry **selected=0;
    size_t capacity=0,count=0;
    uint64_t first=start>>BOAROS_PAGE_SHIFT,last=(end-1)>>BOAROS_PAGE_SHIFT,pages=last-first+1;
    for (;;) {
        size_t needed;
        { CACHE_SCOPE(guard,cache); needed=kernel_vfs_node_dirty_pages(node)->count;
          if (pages<needed) needed=(size_t)pages; }
        if (!needed) {
            if (selected) kernel_heap_release(cache->heap,selected);
            return 0;
        }
        if (needed>SIZE_MAX/sizeof(*selected)) return -KERNEL_ENOMEM;
        if (capacity<needed) {
            if (selected) kernel_heap_release(cache->heap,selected);
            if (kernel_heap_allocate(cache->heap,needed*sizeof(*selected),(void **)&selected)) return -KERNEL_ENOMEM;
            capacity=needed;
        }
        CACHE_SCOPE(guard,cache);
        struct kernel_page_cache_dirty *dirty=kernel_vfs_node_dirty_pages(node);
        needed=pages<dirty->count?(size_t)pages:dirty->count;
        if (needed>capacity) continue;
        if (pages<=dirty->count) {
            for (uint64_t index=first;index<=last;index++) {
                int found; size_t bucket=find_bucket(cache->record,node,index,&found);
                COST_ADD(WRITEBACK_VISITS,1);
                if (found && cache->record->buckets[bucket]->dirty_end) selected[count++]=cache->record->buckets[bucket];
            }
        } else {
            for (struct kernel_page_cache_entry *e=dirty->head;e;e=e->dirty_next) {
                COST_ADD(WRITEBACK_VISITS,1);
                if (e->page_index>=first && e->page_index<=last) selected[count++]=e;
            }
        }
        if (count>capacity) __builtin_trap();
        for (size_t i=0;i<count;i++) entry_borrow(selected[i]);
        break;
    }
    qsort(selected,count,sizeof(*selected),page_offset_compare);
    int result=0;
    for (size_t i=0;i<count && !result;i++) result=writeback_entry(cache,selected[i],end);
    { CACHE_SCOPE(guard,cache); for (size_t i=0;i<count;i++) entry_return(selected[i]); }
    kernel_heap_release(cache->heap,selected);
    return result;
}

int kernel_page_cache_writeback_range(struct kernel_page_cache *cache,
    struct kernel_vfs_node *node, uint64_t start, uint64_t end)
{
    if (!cache_live(cache) || !node || start >= end) return -KERNEL_EINVAL;
    if (cache->record->controlled) {
        if (kernel_lock_held(&node->io_lock,0)) __builtin_trap();
        return controlled_writeback_range(cache,node,start,end);
    }
    KERNEL_LOCK_SCOPE(node_guard);
    kernel_vfs_node_lock(node, &node_guard, 0);
    struct kernel_page_cache_entry **selected = 0;
    size_t count = 0;
    uint64_t first = start >> BOAROS_PAGE_SHIFT, last = (end - 1) >> BOAROS_PAGE_SHIFT;
    uint64_t pages = last - first + 1;
    /* 分配与捕获之间不能睡眠或被共享映射写者插入新脏页；失败返回真实ENOMEM。 */
    uintptr_t irq = arch_interrupt_save();
    {
        KERNEL_NO_RECLAIM_IO;
        struct kernel_page_cache_dirty *dirty = kernel_vfs_node_dirty_pages(node);
        size_t capacity = pages < dirty->count ? (size_t)pages : dirty->count;
        if (!capacity) { arch_interrupt_restore(irq); return 0; }
        if (capacity > SIZE_MAX / sizeof(*selected)) { arch_interrupt_restore(irq); return -KERNEL_ENOMEM; }
        enum kernel_heap_status allocated = kernel_heap_allocate(cache->heap,
            capacity * sizeof(*selected), (void **)&selected);
        if (allocated != KERNEL_HEAP_STATUS_OK) {
            if (allocated != KERNEL_HEAP_STATUS_EMPTY) __builtin_trap();
            arch_interrupt_restore(irq); return -KERNEL_ENOMEM;
        }
        if (pages <= dirty->count) {
            for (uint64_t index = first; index <= last; index++) {
                int found;
                size_t bucket = find_bucket(cache->record, node, index, &found);
                COST_ADD(WRITEBACK_VISITS, 1);
                if (found && cache->record->buckets[bucket]->dirty_end)
                    selected[count++] = cache->record->buckets[bucket];
            }
        } else {
            for (struct kernel_page_cache_entry *entry = dirty->head; entry; entry = entry->dirty_next) {
                COST_ADD(WRITEBACK_VISITS, 1);
                if (entry->page_index >= first && entry->page_index <= last) selected[count++] = entry;
            }
        }
        if (count > capacity) __builtin_trap();
        for (size_t i = 0; i < count; i++)
            if (physical_page_acquire(cache->allocator, selected[i]->physical_address) != PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
    }
    arch_interrupt_restore(irq);
    /* 在首次I/O等待前固定选中集合；排序不再沿可被别的读者扩展的inode链。 */
    qsort(selected, count, sizeof(*selected), page_offset_compare);
    int result = 0;
    for (size_t i = 0; i < count && !result; ) {
        size_t batch = 1, used;
        while (batch < BOAROS_PAGE_CACHE_WRITEBACK_PAGES && i + batch < count &&
               writeback_adjacent(selected[i + batch - 1], selected[i + batch], end)) batch++;
        result = writeback_run(cache, &selected[i], batch, end, &used);
        i += used;
    }
    for (size_t i = 0; i < count; i++)
        if (physical_page_release(cache->allocator, selected[i]->physical_address) != PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
    (void)kernel_heap_release(cache->heap, selected);
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
        zero_bytes((unsigned char *)page+begin,end-begin);
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
    zero_bytes((unsigned char *)page + tail, (size_t)exposed);
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
            zero_bytes((unsigned char *)page + tail, BOAROS_PAGE_SIZE - tail);
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
    for (size_t cursor=0;;) {
        struct kernel_page_cache_entry *entry=0;
        unsigned scanned=0;
        {
            CACHE_SCOPE(guard,cache);
            if (r->stopping || (!force && r->dirty_pages<=r->dirty_low)) break;
            while (cursor<r->capacity && scanned++<64) {
                struct kernel_page_cache_entry *candidate=r->buckets[cursor++];
                r->statistics.worker_scanned++;
                if (!candidate || candidate==PAGE_CACHE_TOMBSTONE || !candidate->dirty_end ||
                    candidate->loading || candidate->writeback) continue;
                entry=candidate; entry_borrow(entry); break;
            }
            if (!entry && cursor>=r->capacity) break;
        }
        if (entry) {
            int result=writeback_entry(cache,entry,UINT64_MAX);
            if (result && result!=-KERNEL_EBUSY && result!=-KERNEL_ENOMEM)
                kernel_vfs_record_writeback_error(entry->node,result);
            {
                CACHE_SCOPE(guard,cache);
                if (!result) r->statistics.worker_written++;
                else if (result!=-KERNEL_EBUSY && result!=-KERNEL_ENOMEM) r->statistics.worker_failed++;
                entry_return(entry);
            }
        }
        int demands;
        { CACHE_SCOPE(guard,cache); demands=r->reads_head!=0; r->statistics.worker_batches++; }
        if (demands) {
            CACHE_SCOPE(guard,cache); r->requested=1;
            break;
        }
        cache_yield();
    }
    io->background_reclaim=0;
    {
        CACHE_SCOPE(guard,cache);
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
        for (unsigned pass = pressure ? 0 : 1; pass < 2 && !r->stopping; pass++) {
            uint64_t epoch = ++r->epoch;
            if (!epoch) __builtin_trap();
            size_t limit = r->capacity;
            for (size_t cursor = 0; cursor < limit && !r->stopping; ) {
                unsigned scanned = 0;
                while (cursor < limit && !r->stopping && scanned++ < 64) {
                    if (cursor >= r->capacity) break;
                    struct kernel_page_cache_entry *e = r->buckets[cursor++];
                    r->statistics.worker_scanned++;
                    if (!e || e == PAGE_CACHE_TOMBSTONE || e->worker_epoch == epoch) continue;
                    e->worker_epoch = epoch;
                    if (e->loading || e->users || e->writeback) continue;
                    if (pass == 0) {
                        uint32_t refs;
                        if (!e->dirty_end && !e->aliases && physical_page_reference_count(cache->allocator,
                                e->physical_address, &refs) == PHYSICAL_PAGE_STATUS_OK && refs == 1) {
                            remove_entry(cache, e);
                            (void)drain_entries(cache, &released);
                            group_progress(r->group);
                        }
                        continue;
                    }
                    if (!e->dirty_end) continue;
                    if ((!pressure && group_dirty(r->group) <= r->dirty_low) ||
                        (pressure && physical_page_available(cache->allocator) >= r->high)) break;
                    KERNEL_LOCK_SCOPE(guard);
                    if (!kernel_vfs_node_try_read(e->node, &guard)) continue;
                    struct kernel_page_cache_entry *batch[BOAROS_PAGE_CACHE_WRITEBACK_PAGES];
                    size_t count = 1, used;
                    batch[0] = e;
                    while (count < ((size_t)1 << r->snapshot_order) && scanned < 64) {
                        scanned++; r->statistics.worker_scanned++;
                        int found;
                        size_t bucket = find_bucket(r, e->node, e->page_index + count, &found);
                        if (!found) break;
                        struct kernel_page_cache_entry *next = r->buckets[bucket];
                        if (next->worker_epoch == epoch || next->users ||
                            !writeback_adjacent(batch[count - 1], next, UINT64_MAX)) break;
                        batch[count++] = next;
                    }
                    /* 所有候选在首次等待前固定；回退未提交的成员留给后续轮次。 */
                    for (size_t i = 0; i < count; i++) entry_borrow(batch[i]);
                    int result = writeback_run(cache, batch, count, UINT64_MAX, &used);
                    int removed = 0;
                    for (size_t i = 0; i < count; i++) {
                        e = batch[i]; entry_return(e);
                        if (i >= used) continue;
                        e->worker_epoch = epoch;
                        if (result) {
                            r->statistics.worker_failed++;
                            if (result != -KERNEL_ENOMEM && result != -KERNEL_EBUSY)
                                kernel_vfs_record_writeback_error(e->node, result);
                        } else {
                            r->statistics.worker_written++;
                            uint32_t refs;
                            if (pressure && !e->dirty_end && !e->aliases &&
                                physical_page_reference_count(cache->allocator, e->physical_address, &refs)
                                    == PHYSICAL_PAGE_STATUS_OK && refs == 1) {
                                remove_entry(cache, e); removed = 1;
                            }
                        }
                    }
                    if (removed) {
                        kernel_lock_release(&guard);
                        (void)drain_entries(cache, &released);
                        group_progress(r->group);
                    }
                }
                r->statistics.worker_batches++;
                if ((!pressure && group_dirty(r->group) <= r->dirty_low) ||
                    (pressure && physical_page_available(cache->allocator) >= r->high)) break;
                if (kernel_scheduler_yield_current() != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
            }
        }
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
    r->snapshot_order = writeback_order(BOAROS_PAGE_CACHE_WRITEBACK_PAGES);
    for (;;) {
        enum physical_page_status status = r->snapshot_order
            ? physical_page_allocate_order(cache->allocator, r->snapshot_order, &r->snapshot_address)
            : physical_page_allocate(cache->allocator, &r->snapshot_address);
        if (status == PHYSICAL_PAGE_STATUS_OK) break;
        if (status != PHYSICAL_PAGE_STATUS_EMPTY) __builtin_trap();
        if (!r->snapshot_order) return -KERNEL_ENOMEM;
        r->snapshot_order--;
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
        if (physical_page_release_order(cache->allocator, r->snapshot_address, r->snapshot_order) != PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
        return result == KERNEL_SCHEDULER_STATUS_NO_MEMORY ? -KERNEL_ENOMEM : -KERNEL_EINVAL;
    }
    { CACHE_SCOPE(publish,cache); r->started = 1; }
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
    { CACHE_SCOPE(stop, cache); r->stopping = 1; }
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
    if (physical_page_release_order(cache->allocator, r->snapshot_address, r->snapshot_order) != PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
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
