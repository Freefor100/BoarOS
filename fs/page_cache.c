#include "vfs_internal.h"

#include <kernel/cost.h>
#include <kernel/heap.h>
#include <kernel/sync.h>
#include <arch/riscv/context.h>
#include <arch/riscv/plic.h>
#include <kernel/errno.h>
#include <kernel/page.h>
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

struct kernel_page_cache_record {
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

};

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

static int resize_hash(struct kernel_page_cache *cache, size_t capacity)
{
    struct kernel_page_cache_record *record = cache->record;
    struct kernel_page_cache_entry **buckets;
    struct kernel_page_cache_entry **old_buckets = record->buckets;
    size_t old_capacity = record->capacity;
    size_t index;

    if (kernel_heap_allocate_zeroed(cache->heap,
                                    capacity,
                                    sizeof(*buckets),
                                    (void **)&buckets) !=
        KERNEL_HEAP_STATUS_OK) {
        return 0;
    }
    record->buckets = buckets;
    record->capacity = capacity;
    record->tombstones = 0U;
    for (index = 0U; index < old_capacity; index++) {
        struct kernel_page_cache_entry *entry = old_buckets[index];

        if (entry != 0 && entry != PAGE_CACHE_TOMBSTONE) {
            int found;
            size_t destination = find_bucket(record,
                                             entry->node,
                                             entry->page_index,
                                             &found);

            if (found) {
                return 0;
            }
            buckets[destination] = entry;
        }
    }
    (void)kernel_heap_release(cache->heap, old_buckets);
    return 1;
}

static int reserve_bucket(struct kernel_page_cache *cache)
{
    struct kernel_page_cache_record *record = cache->record;

    if ((record->occupied + record->tombstones + 1U) * 4U <
        record->capacity * 3U) {
        return 1;
    }
    if (record->capacity > SIZE_MAX / 2U) {
        return 0;
    }
    return resize_hash(cache, record->capacity * 2U);
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
    if (entry->aliases != 0 || entry->loading || entry->users) __builtin_trap();
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

enum kernel_page_cache_status kernel_page_cache_init(
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
    cache->heap = heap;
    cache->allocator = allocator;
    cache->record = record;
    cache->state = KERNEL_PAGE_CACHE_LIVE;
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
}

enum kernel_page_cache_status kernel_page_cache_alias_attach(
    struct kernel_page_cache *cache, const struct kernel_vfs_file *file,
    uint64_t page_index, uint64_t physical_address,
    struct kernel_page_cache_alias *alias, void *owner,
    uint64_t virtual_address,
    void (*rearm)(void *owner, uint64_t virtual_address))
{
    struct kernel_page_cache_entry *entry;
    enum kernel_page_cache_status status;
    if (alias == 0 || alias->previous != 0 || owner == 0 || rearm == 0)
        return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
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
    if (target == 0 || source == 0 || source->previous == 0 ||
        target->previous != 0 || owner == 0) __builtin_trap();
    target->owner = owner;
    target->virtual_address = source->virtual_address;
    target->rearm = source->rearm;
    link_alias(source->entry, target);
}

void kernel_page_cache_alias_detach(struct kernel_page_cache_alias *alias)
{
    if (alias == 0 || alias->previous == 0) __builtin_trap();
    *alias->previous = alias->next;
    if (alias->next != 0) alias->next->previous = alias->previous;
    alias->entry = 0;
    alias->next = 0;
    alias->previous = 0;
    alias->owner = 0;
    alias->rearm = 0;
}

void kernel_page_cache_alias_mark_dirty(struct kernel_page_cache_alias *alias)
{
    struct kernel_page_cache_entry *entry;
    size_t end;
    if (alias == 0 || alias->previous == 0 || alias->entry == 0)
        __builtin_trap();
    entry = alias->entry;
    end = entry_valid_bytes(entry);
    if (end == 0) __builtin_trap();
    uintptr_t irq = riscv_interrupt_save();
    dirty_mark(entry, 0, end);
    riscv_interrupt_restore(irq);
    pressure_notify(entry->cache);
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
    struct kernel_page_cache *cache,
    const struct kernel_vfs_file *file,
    uint64_t page_index,
    uint64_t *physical_address,
    size_t *valid_bytes)
{
    struct kernel_page_cache_entry *entry;
    enum kernel_page_cache_status status;

    if (physical_address == 0 || valid_bytes == 0) {
        return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    }
    status = lookup_entry(cache, file, page_index, &entry);
    if (status != KERNEL_PAGE_CACHE_STATUS_OK) {
        return status;
    }
    uintptr_t irq = riscv_interrupt_save();
    entry->users++;
    while (entry->loading) {
        enum kernel_wait_wake_reason reason;
        if (kernel_scheduler_block_current(&entry->ready, 0, 0, &reason) != KERNEL_SCHEDULER_STATUS_OK)
            __builtin_trap();
    }
    status = entry->load_error;
    entry->users--;
    if (status != KERNEL_PAGE_CACHE_STATUS_OK) {
        if (!entry->users) { remove_entry(cache, entry); uint64_t released = 0; (void)drain_entries(cache, &released); }
        riscv_interrupt_restore(irq);
        return status;
    }
    riscv_interrupt_restore(irq);
    if (physical_page_acquire(cache->allocator,
                              entry->physical_address) !=
        PHYSICAL_PAGE_STATUS_OK) {
        return KERNEL_PAGE_CACHE_STATUS_STATE;
    }
    lru_touch(cache->record, entry);
    cache->record->statistics.hits++;
    *physical_address = entry->physical_address;
    *valid_bytes = entry_valid_bytes(entry);
    return KERNEL_PAGE_CACHE_STATUS_OK;
}

static enum kernel_page_cache_status get_page(
    struct kernel_page_cache *cache,
    const struct kernel_vfs_file *file,
    uint64_t page_index,
    uint64_t *physical_address,
    size_t *valid_bytes,
    int for_write, const void *overwrite, int *overwritten)
{
    struct kernel_page_cache_entry *entry;
    struct kernel_vfs_node *node;
    void *page;
    uint64_t offset;
    size_t bytes_read = 0U;
    int found;
    size_t bucket;
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
    cache->record->statistics.misses++;
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
    if (!overwrite) zero_bytes(page, BOAROS_PAGE_SIZE);
    if (kernel_vfs_node_acquire(node) != 0) {
        return abandon_entry(cache,
                             entry,
                             KERNEL_PAGE_CACHE_STATUS_STATE);
    }
    entry->cache = cache;
    entry->node = node;
    entry->page_index = page_index;
    bucket = find_bucket(cache->record, node, page_index, &found);
    if (found) {
        (void)abandon_entry(cache, entry, KERNEL_PAGE_CACHE_STATUS_OK);
        return kernel_page_cache_lookup(cache, file, page_index, physical_address, valid_bytes);
    }
    kernel_wait_queue_init(&entry->ready);
    entry->loading = 1;
    entry->users = 1;
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
    int read_error = 0;
    if (!overwrite) read_error = kernel_vfs_node_pread(node, offset, page, BOAROS_PAGE_SIZE, &bytes_read);
    uintptr_t irq = riscv_interrupt_save();
    if (overwrite) {
        uint64_t old_size = kernel_vfs_node_size(node);
        if (offset + BOAROS_PAGE_SIZE > old_size)
            kernel_page_cache_extend(cache, node, old_size, offset + BOAROS_PAGE_SIZE);
        /* 稳定内核输入整页覆盖：loading一直保留到内容、脏组织及可见长度均已就绪。 */
        memcpy(page, overwrite, BOAROS_PAGE_SIZE);
        COST_ADD(CACHE_COPY, BOAROS_PAGE_SIZE);
        dirty_mark(entry, 0, BOAROS_PAGE_SIZE);
        kernel_vfs_node_written(node, offset + BOAROS_PAGE_SIZE);
        *overwritten = 1;
    }
    entry->load_error = read_error == -KERNEL_ENOMEM ? KERNEL_PAGE_CACHE_STATUS_NO_MEMORY :
                        read_error ? KERNEL_PAGE_CACHE_STATUS_IO : KERNEL_PAGE_CACHE_STATUS_OK;
    entry->loading = 0;
    entry->users--;
    if (entry->ready.head && kernel_wait_queue_wake_all(&entry->ready) != KERNEL_SCHEDULER_STATUS_OK)
        __builtin_trap();
    if (read_error) {
        enum kernel_page_cache_status failed = entry->load_error;
        if (!entry->users) { remove_entry(cache, entry); uint64_t released = 0; (void)drain_entries(cache, &released); }
        riscv_interrupt_restore(irq);
        return failed;
    }
    if (physical_page_acquire(cache->allocator, entry->physical_address) != PHYSICAL_PAGE_STATUS_OK)
        __builtin_trap();
    riscv_interrupt_restore(irq);
    *physical_address = entry->physical_address;
    *valid_bytes = entry_valid_bytes(entry);
    return KERNEL_PAGE_CACHE_STATUS_OK;
}

enum kernel_page_cache_status kernel_page_cache_get(
    struct kernel_page_cache *cache, const struct kernel_vfs_file *file,
    uint64_t page_index, uint64_t *physical_address, size_t *valid_bytes)
{
    struct kernel_vfs_node *node = kernel_vfs_file_node(file);
    if (!node) return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    KERNEL_LOCK_SCOPE(node_guard);
    kernel_vfs_node_lock(node, &node_guard, 0);
    return get_page(cache, file, page_index, physical_address, valid_bytes, 0, 0, 0);
}

int kernel_page_cache_write(struct kernel_page_cache *cache,
                             struct kernel_vfs_file *file, uint64_t offset,
                             const void *buffer, size_t size, size_t *written)
{
    const unsigned char *source = buffer;
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
        uintptr_t irq = riscv_interrupt_save();
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
        riscv_interrupt_restore(irq);
        (void)physical_page_release(cache->allocator, address);
        pressure_notify(cache);
    }
    return 0;
}

static int writeback_entry(struct kernel_page_cache *cache,
                           struct kernel_page_cache_entry *entry, uint64_t limit)
{
    void *page;
    size_t written = 0, begin, end;
    uint64_t generation;
    int result;
    uintptr_t wait_irq = riscv_interrupt_save();
    while (entry->writeback) {
        enum kernel_wait_wake_reason reason;
        if (kernel_scheduler_block_current(&entry->ready, 0, 0, &reason) != KERNEL_SCHEDULER_STATUS_OK)
            __builtin_trap();
    }
    riscv_interrupt_restore(wait_irq);
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
        uintptr_t irq = riscv_interrupt_save();
        if (entry->ready.head) (void)kernel_wait_queue_wake_all(&entry->ready);
        riscv_interrupt_restore(irq);
        return -KERNEL_ENOMEM;
    }
    if (physical_page_resolve(cache->allocator, snapshot_address, &snapshot) != PHYSICAL_PAGE_STATUS_OK)
        __builtin_trap();
    uintptr_t irq = riscv_interrupt_save();
    for (struct kernel_page_cache_alias *alias = entry->aliases; alias; alias = alias->next)
        alias->rearm(alias->owner, alias->virtual_address);
    begin = entry->dirty_begin;
    end = entry->dirty_end;
    if (end > limit - start) end = (size_t)(limit - start);
    generation = entry->generation;
    COST_ADD(SNAPSHOT_COPY, end - begin);
    COST_ADD(WRITEBACK_REQUESTED, end - begin);
    memcpy((unsigned char *)snapshot + begin, (unsigned char *)page + begin, end - begin);
    riscv_interrupt_restore(irq);
    result = kernel_vfs_node_writeback(entry->node,
                    (entry->page_index << BOAROS_PAGE_SHIFT) + begin,
                    (unsigned char *)snapshot + begin, end - begin, &written);
    if (reserved) cache->record->snapshot_busy = 0;
    else (void)physical_page_release(cache->allocator, snapshot_address);
    COST_ADD(WRITEBACK_ACCEPTED, written);
    if (written > end - begin) __builtin_trap();
    if (result == 0 && written != end - begin) result = -KERNEL_EIO;
    irq = riscv_interrupt_save();
    if (result == 0 && generation == entry->generation) {
        if (end == entry->dirty_end) dirty_clear(entry);
        else entry->dirty_begin = end;
    }
    riscv_interrupt_restore(irq);
    (void)physical_page_release(cache->allocator, entry->physical_address);
    entry->writeback_error = result;
    entry->writeback = 0;
    cache->record->writeback_active--;
    irq = riscv_interrupt_save();
    if (entry->ready.head) (void)kernel_wait_queue_wake_all(&entry->ready);
    riscv_interrupt_restore(irq);
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
    uintptr_t irq = riscv_interrupt_save();
    /* 分配可触发回收；重新检查成员，不能把已完成的页误当成本次 owner。 */
    size_t ready = 1;
    if (entries[0]->writeback || entries[0]->loading || !entries[0]->dirty_end ||
        !kernel_vfs_node_writeback_allowed(entries[0]->node)) ready = 0;
    while (ready && ready < count && writeback_adjacent(entries[ready - 1], entries[ready], limit)) ready++;
    if (ready < 2) {
        riscv_interrupt_restore(irq);
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
    riscv_interrupt_restore(irq);
    size_t written = 0;
    int result = kernel_vfs_node_writeback(entries[0]->node, offset, snapshot, size, &written);
    COST_ADD(WRITEBACK_ACCEPTED, written);
    if (written > size) __builtin_trap();
    if (!result && written != size) result = -KERNEL_EIO;
    irq = riscv_interrupt_save();
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
    riscv_interrupt_restore(irq);
    *used = count;
    return result;
}

static int page_offset_compare(const void *left, const void *right)
{
    const struct kernel_page_cache_entry *a = *(struct kernel_page_cache_entry *const *)left;
    const struct kernel_page_cache_entry *b = *(struct kernel_page_cache_entry *const *)right;
    return a->page_index < b->page_index ? -1 : a->page_index > b->page_index;
}

int kernel_page_cache_writeback_range(struct kernel_page_cache *cache,
    struct kernel_vfs_node *node, uint64_t start, uint64_t end)
{
    if (!cache_live(cache) || !node || start >= end) return -KERNEL_EINVAL;
    KERNEL_LOCK_SCOPE(node_guard);
    kernel_vfs_node_lock(node, &node_guard, 0);
    struct kernel_page_cache_entry **selected = 0;
    size_t count = 0;
    uint64_t first = start >> BOAROS_PAGE_SHIFT, last = (end - 1) >> BOAROS_PAGE_SHIFT;
    uint64_t pages = last - first + 1;
    /* 分配与捕获之间不能睡眠或被共享映射写者插入新脏页；失败返回真实ENOMEM。 */
    uintptr_t irq = riscv_interrupt_save();
    {
        KERNEL_NO_RECLAIM_IO;
        struct kernel_page_cache_dirty *dirty = kernel_vfs_node_dirty_pages(node);
        size_t capacity = pages < dirty->count ? (size_t)pages : dirty->count;
        if (!capacity) { riscv_interrupt_restore(irq); return 0; }
        if (capacity > SIZE_MAX / sizeof(*selected)) { riscv_interrupt_restore(irq); return -KERNEL_ENOMEM; }
        enum kernel_heap_status allocated = kernel_heap_allocate(cache->heap,
            capacity * sizeof(*selected), (void **)&selected);
        if (allocated != KERNEL_HEAP_STATUS_OK) {
            if (allocated != KERNEL_HEAP_STATUS_EMPTY) __builtin_trap();
            riscv_interrupt_restore(irq); return -KERNEL_ENOMEM;
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
    riscv_interrupt_restore(irq);
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
    uintptr_t irq = riscv_interrupt_save();
    uint64_t dirty = group_dirty(g);
    for (struct kernel_page_cache_record *r = g->head; r; r = r->next) {
        if (!r->started || r->stopping) continue;
        if (physical_page_available(g->allocator) > r->low && dirty < r->dirty_high) continue;
        r->requested = 1;
        if (r->work.head) (void)kernel_wait_queue_wake_all(&r->work);
    }
    riscv_interrupt_restore(irq);
}

static void pressure_notify(void *context)
{
    struct kernel_page_cache *cache = context;
    if (cache_live(cache)) group_notify(cache->record->group);
}

static void pressure_wait(void *context)
{
    struct page_cache_group *g = context;
    struct kernel_io_context *io = kernel_io_context_current();
    if (io->locks || io->allocation_depth || io->backend_depth || io->reclaim_depth ||
        io->background_reclaim || !kernel_scheduler_can_sleep() || riscv_plic_in_interrupt()) return;
    uintptr_t irq = riscv_interrupt_save();
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
        if (kernel_scheduler_block_current(&g->progress, 0, 0, &reason) != KERNEL_SCHEDULER_STATUS_OK)
            __builtin_trap();
    }
    group_put(g);
    riscv_interrupt_restore(irq);
}

static void page_cache_worker(void *argument)
{
    struct kernel_page_cache *cache = argument;
    struct kernel_page_cache_record *r = cache->record;
    (void)riscv_interrupt_save();
    struct kernel_io_context *io = kernel_io_context_current();
    io->background_reclaim = 1;
    for (;;) {
        while (!r->requested && !r->stopping) {
            enum kernel_wait_wake_reason reason;
            /* 空工作队列不是不可中断 I/O，不计入负载。 */
            if (kernel_scheduler_block_current(&r->work, 0, 1, &reason) != KERNEL_SCHEDULER_STATUS_OK)
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
                    for (size_t i = 0; i < count; i++) batch[i]->users++;
                    int result = writeback_run(cache, batch, count, UINT64_MAX, &used);
                    int removed = 0;
                    for (size_t i = 0; i < count; i++) {
                        e = batch[i]; e->users--;
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
    r->started = 1;
    pressure_notify(cache);
    return 0;
}

void kernel_page_cache_stop_worker(struct kernel_page_cache *cache)
{
    if (!cache_live(cache) || !cache->record->started) return;
    struct kernel_page_cache_record *r = cache->record;
    uintptr_t irq = riscv_interrupt_save();
    r->stopping = 1;
    (void)kernel_wait_queue_wake_all(&r->work);
    (void)kernel_wait_queue_wake_all(&r->progress);
    kernel_thread_join(&r->worker);
    if (physical_page_release_order(cache->allocator, r->snapshot_address, r->snapshot_order) != PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
    r->started = 0;
    riscv_interrupt_restore(irq);
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
        group_unregister(cache);
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
