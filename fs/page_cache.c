#include "vfs_internal.h"

#include <kernel/heap.h>
#include <kernel/page.h>
#include <kernel/page_cache.h>
#include <kernel/physical_page.h>
#include <kernel/vfs.h>

#include <stddef.h>
#include <stdint.h>

#define PAGE_CACHE_INITIAL_CAPACITY 16U
#define PAGE_CACHE_TOMBSTONE \
    ((struct kernel_page_cache_entry *)(uintptr_t)1U)

struct kernel_page_cache_entry {
    struct kernel_page_cache_entry *lru_previous;
    struct kernel_page_cache_entry *lru_next;
    struct kernel_page_cache_entry *cleanup_next;
    struct kernel_vfs_node *node;
    uint64_t page_index;
    uint64_t physical_address;
    size_t valid_bytes;
    uint8_t page_references_owned;
};

struct kernel_page_cache_record {
    struct kernel_page_cache_entry **buckets;
    struct kernel_page_cache_entry *lru_head;
    struct kernel_page_cache_entry *lru_tail;
    struct kernel_page_cache_entry *cleanup_entries;
    struct kernel_page_cache_statistics statistics;
    size_t capacity;
    size_t occupied;
    size_t tombstones;
};

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

static void remove_entry(struct kernel_page_cache *cache,
                         struct kernel_page_cache_entry *entry)
{
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
    entry->cleanup_next = record->cleanup_entries;
    record->cleanup_entries = entry;
    record->statistics.current_pages--;
    record->statistics.evictions++;
}

static uint64_t reclaim_callback(void *context, uint64_t target_pages)
{
    return kernel_page_cache_reclaim(context, target_pages);
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
    if (physical_page_allocator_set_reclaimer(allocator,
                                               reclaim_callback,
                                               cache) !=
        PHYSICAL_PAGE_STATUS_OK) {
        (void)kernel_heap_release(heap, record->buckets);
        (void)kernel_heap_release(heap, record);
        *cache = (struct kernel_page_cache){0};
        return KERNEL_PAGE_CACHE_STATUS_STATE;
    }
    return KERNEL_PAGE_CACHE_STATUS_OK;
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
    if (physical_page_acquire(cache->allocator,
                              entry->physical_address) !=
        PHYSICAL_PAGE_STATUS_OK) {
        return KERNEL_PAGE_CACHE_STATUS_STATE;
    }
    lru_touch(cache->record, entry);
    cache->record->statistics.hits++;
    *physical_address = entry->physical_address;
    *valid_bytes = entry->valid_bytes;
    return KERNEL_PAGE_CACHE_STATUS_OK;
}

enum kernel_page_cache_status kernel_page_cache_get(
    struct kernel_page_cache *cache,
    const struct kernel_vfs_file *file,
    uint64_t page_index,
    uint64_t *physical_address,
    size_t *valid_bytes)
{
    struct kernel_page_cache_entry *entry;
    struct kernel_vfs_node *node;
    void *page;
    uint64_t offset;
    size_t bytes_read = 0U;
    int found;
    size_t bucket;
    enum kernel_page_cache_status lookup_status;

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
    if (offset >= kernel_vfs_node_size(node)) {
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
    zero_bytes(page, BOAROS_PAGE_SIZE);
    if (kernel_vfs_node_pread(node,
                              offset,
                              page,
                              BOAROS_PAGE_SIZE,
                              &bytes_read) != 0) {
        return abandon_entry(cache,
                             entry,
                             KERNEL_PAGE_CACHE_STATUS_IO);
    }
    if (kernel_vfs_node_acquire(node) != 0) {
        return abandon_entry(cache,
                             entry,
                             KERNEL_PAGE_CACHE_STATUS_STATE);
    }
    entry->node = node;
    if (physical_page_acquire(cache->allocator,
                              entry->physical_address) !=
        PHYSICAL_PAGE_STATUS_OK) {
        return abandon_entry(cache,
                             entry,
                             KERNEL_PAGE_CACHE_STATUS_STATE);
    }
    entry->page_references_owned = 2U;
    entry->page_index = page_index;
    entry->valid_bytes = bytes_read;
    bucket = find_bucket(cache->record, node, page_index, &found);
    if (found) {
        return abandon_entry(cache,
                             entry,
                             KERNEL_PAGE_CACHE_STATUS_STATE);
    }
    entry->page_references_owned = 1U;
    if (cache->record->buckets[bucket] == PAGE_CACHE_TOMBSTONE) {
        cache->record->tombstones--;
    }
    cache->record->buckets[bucket] = entry;
    cache->record->occupied++;
    lru_insert_head(cache->record, entry);
    cache->record->statistics.insertions++;
    cache->record->statistics.current_pages++;
    if (cache->record->statistics.current_pages >
        cache->record->statistics.peak_pages) {
        cache->record->statistics.peak_pages =
            cache->record->statistics.current_pages;
    }
    *physical_address = entry->physical_address;
    *valid_bytes = entry->valid_bytes;
    return KERNEL_PAGE_CACHE_STATUS_OK;
}

uint64_t kernel_page_cache_reclaim(struct kernel_page_cache *cache,
                                   uint64_t target_pages)
{
    struct kernel_page_cache_entry *entry;
    uint64_t reclaimed = 0U;
    uint64_t released = 0U;
    uint64_t scanned = 0U;
    uint64_t scan_limit;

    if (!cache_live(cache) || target_pages == 0U) {
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
            references == 1U) {
            remove_entry(cache, entry);
            (void)drain_entries(cache, &released);
            reclaimed = released;
        }
        entry = previous;
    }
    (void)drain_entries(cache, &released);
    cache->record->statistics.pages_reclaimed += released;
    return released;
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
    const struct kernel_vfs_node *node)
{
    struct kernel_page_cache_entry *entry;
    uint64_t released = 0U;

    if (!cache_live(cache) || node == 0) {
        return KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT;
    }
    entry = cache->record->lru_tail;
    while (entry != 0) {
        struct kernel_page_cache_entry *previous = entry->lru_previous;

        if (entry->node == node) {
            remove_entry(cache, entry);
        }
        entry = previous;
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
    if (cache->state == KERNEL_PAGE_CACHE_LIVE) {
        for (entry = cache->record->lru_tail;
             entry != 0;
             entry = entry->lru_previous) {
            uint32_t references;

            if (physical_page_reference_count(cache->allocator,
                                               entry->physical_address,
                                               &references) !=
                    PHYSICAL_PAGE_STATUS_OK ||
                references != 1U) {
                return KERNEL_PAGE_CACHE_STATUS_STATE;
            }
        }
        if (physical_page_allocator_clear_reclaimer(cache->allocator) !=
            PHYSICAL_PAGE_STATUS_OK) {
            return KERNEL_PAGE_CACHE_STATUS_STATE;
        }
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
