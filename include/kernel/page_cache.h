#ifndef BOAROS_KERNEL_PAGE_CACHE_H
#define BOAROS_KERNEL_PAGE_CACHE_H

#include <stddef.h>
#include <stdint.h>

struct kernel_heap;
struct kernel_page_cache_record;
struct kernel_page_cache_entry;
/* Owned by one MM resident; the cache borrows it while attached. */
struct kernel_page_cache_alias {
    struct kernel_page_cache_entry *entry;
    struct kernel_page_cache_alias *next;
    struct kernel_page_cache_alias **previous;
    void *owner;
    uint64_t virtual_address;
    void (*rearm)(void *owner, uint64_t virtual_address);
};
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
    uint64_t worker_scanned, worker_written, worker_failed, worker_batches;
};

struct kernel_page_cache {
    struct kernel_heap *heap;
    struct physical_page_allocator *allocator;
    struct kernel_page_cache_record *record;
    enum kernel_page_cache_state state;
};

/* 一个 allocator 可注册多个独立缓存。统计/水位全局汇总，worker 与快照页各自拥有。
 * 单 hart 调用边界与原 VFS 一致；所有实例和等待者离开后才释放聚合 owner。 */
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

enum kernel_page_cache_status kernel_page_cache_alias_attach(
    struct kernel_page_cache *cache, const struct kernel_vfs_file *file,
    uint64_t page_index, uint64_t physical_address,
    struct kernel_page_cache_alias *alias, void *owner,
    uint64_t virtual_address,
    void (*rearm)(void *owner, uint64_t virtual_address));
void kernel_page_cache_alias_clone(struct kernel_page_cache_alias *target,
    const struct kernel_page_cache_alias *source, void *owner);
void kernel_page_cache_alias_detach(struct kernel_page_cache_alias *alias);
void kernel_page_cache_alias_mark_dirty(struct kernel_page_cache_alias *alias);

/* Caller supplies stable kernel bytes and owns the inode mutation lock.
 * A full-page cache miss stays loading until initialized and dirty, without
 * reading the overwritten data. Partial pages preserve their old contents.
 * Dirty pages remain owned until writeback or explicit discard; return zero
 * or a negative errno while retaining partial progress. */
int kernel_page_cache_write(struct kernel_page_cache *cache,
                             struct kernel_vfs_file *file, uint64_t offset,
                             const void *buffer, size_t size, size_t *written);
int kernel_page_cache_writeback(struct kernel_page_cache *cache,
                                 struct kernel_vfs_node *node);
int kernel_page_cache_writeback_before(struct kernel_page_cache *cache,
    struct kernel_vfs_node *node, uint64_t end);
int kernel_page_cache_writeback_range(struct kernel_page_cache *cache,
    struct kernel_vfs_node *node, uint64_t start, uint64_t end);
/* Caller owns the inode mutation lock. Growth only touches the cached old
 * EOF tail; shrink follows mapping invalidation and removes out-of-range pages. */
void kernel_page_cache_extend(struct kernel_page_cache *cache,
    struct kernel_vfs_node *node, uint64_t old_size, uint64_t new_size);
void kernel_page_cache_truncate(struct kernel_page_cache *cache,
    struct kernel_vfs_node *node, uint64_t new_size);

int kernel_page_cache_start_worker(struct kernel_page_cache *cache);
void kernel_page_cache_stop_worker(struct kernel_page_cache *cache);

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
