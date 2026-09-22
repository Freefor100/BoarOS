#ifndef BOAROS_KERNEL_PAGE_CACHE_H
#define BOAROS_KERNEL_PAGE_CACHE_H

#include <stddef.h>
#include <stdint.h>

struct kernel_heap;
struct kernel_page_cache_record;
struct kernel_page_cache_entry;
struct kernel_vfs_file;
struct kernel_vfs_mount;
struct kernel_vfs_node;
struct physical_page_allocator;

enum kernel_page_cache_status {
    KERNEL_PAGE_CACHE_STATUS_OK = 0,
    KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT,
    KERNEL_PAGE_CACHE_STATUS_NOT_FOUND,
    KERNEL_PAGE_CACHE_STATUS_OUT_OF_RANGE,
    KERNEL_PAGE_CACHE_STATUS_NO_MEMORY,
    KERNEL_PAGE_CACHE_STATUS_IO,
    KERNEL_PAGE_CACHE_STATUS_CLEANUP_REQUIRED,
    KERNEL_PAGE_CACHE_STATUS_STATE,
};

enum kernel_page_cache_state {
    KERNEL_PAGE_CACHE_EMPTY = 0,
    KERNEL_PAGE_CACHE_LIVE,
    KERNEL_PAGE_CACHE_CLEANUP,
    KERNEL_PAGE_CACHE_RELEASED,
};

struct kernel_page_cache_statistics {
    uint64_t hits;
    uint64_t misses;
    uint64_t insertions;
    uint64_t evictions;
    uint64_t reclaim_calls;
    uint64_t pages_reclaimed;
    uint64_t current_pages;
    uint64_t peak_pages;
};

struct kernel_page_cache {
    struct kernel_heap *heap;
    struct physical_page_allocator *allocator;
    struct kernel_page_cache_record *record;
    enum kernel_page_cache_state state;
};

enum kernel_page_cache_status kernel_page_cache_init(
    struct kernel_page_cache *cache,
    struct kernel_heap *heap,
    struct physical_page_allocator *allocator);

/* Returns an owned physical-page reference that the caller must release. */
enum kernel_page_cache_status kernel_page_cache_get(
    struct kernel_page_cache *cache,
    const struct kernel_vfs_file *file,
    uint64_t page_index,
    uint64_t *physical_address,
    size_t *valid_bytes);

/* A successful lookup also returns an owned physical-page reference. */
enum kernel_page_cache_status kernel_page_cache_lookup(
    struct kernel_page_cache *cache,
    const struct kernel_vfs_file *file,
    uint64_t page_index,
    uint64_t *physical_address,
    size_t *valid_bytes);

/* Buffered writes own their dirty pages until writeback or explicit discard.
 * These operations return zero or a negative errno, retaining partial progress. */
int kernel_page_cache_write(struct kernel_page_cache *cache,
                             struct kernel_vfs_file *file, uint64_t offset,
                             const void *buffer, size_t size, size_t *written);
int kernel_page_cache_writeback(struct kernel_page_cache *cache,
                                 struct kernel_vfs_node *node);
int kernel_page_cache_writeback_before(struct kernel_page_cache *cache,
    struct kernel_vfs_node *node, uint64_t end);
void kernel_page_cache_truncate(struct kernel_page_cache *cache,
    struct kernel_vfs_node *node, uint64_t size);

uint64_t kernel_page_cache_reclaim(struct kernel_page_cache *cache,
                                   uint64_t target_pages);

enum kernel_page_cache_status kernel_page_cache_purge_mount(
    struct kernel_page_cache *cache,
    const struct kernel_vfs_mount *mount);

enum kernel_page_cache_status kernel_page_cache_invalidate_node(
    struct kernel_page_cache *cache,
    struct kernel_vfs_node *node);

void kernel_page_cache_get_statistics(
    const struct kernel_page_cache *cache,
    struct kernel_page_cache_statistics *statistics);

enum kernel_page_cache_status kernel_page_cache_destroy(
    struct kernel_page_cache *cache);

#endif
