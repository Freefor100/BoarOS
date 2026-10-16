#ifndef BOAROS_KERNEL_PAGE_CACHE_H
#define BOAROS_KERNEL_PAGE_CACHE_H

#include <stddef.h>
#include <stdint.h>
#include <kernel/page.h>

#ifndef BOAROS_PAGE_CACHE_WRITEBACK_PAGES
#define BOAROS_PAGE_CACHE_WRITEBACK_PAGES (131072U / BOAROS_PAGE_SIZE)
#endif
#if BOAROS_PAGE_CACHE_WRITEBACK_PAGES < 1 || BOAROS_PAGE_CACHE_WRITEBACK_PAGES > (131072U / BOAROS_PAGE_SIZE)
#error "BOAROS_PAGE_CACHE_WRITEBACK_PAGES must fit the 128 KiB window"
#endif

#ifndef BOAROS_PAGE_CACHE_READAHEAD_PAGES
#define BOAROS_PAGE_CACHE_READAHEAD_PAGES 0
#endif
#if BOAROS_PAGE_CACHE_READAHEAD_PAGES < 0 || BOAROS_PAGE_CACHE_READAHEAD_PAGES > (131072U / BOAROS_PAGE_SIZE)
#error "BOAROS_PAGE_CACHE_READAHEAD_PAGES must fit the 128 KiB window"
#endif

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
    int (*rearm)(void *owner, uint64_t virtual_address);
    struct kernel_page_cache_alias *retired_next;
    uint32_t borrows, registered;
};
static inline int kernel_page_cache_alias_active(const struct kernel_page_cache_alias *alias)
{ return __atomic_load_n(&alias->registered,__ATOMIC_ACQUIRE) != 0; }
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
    KERNEL_PAGE_CACHE_STATUS_BUSY,
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
    uint64_t readahead_queued, readahead_cancelled, readahead_loaded, readahead_failed;
    uint64_t readahead_batches, readahead_peak_pages;
};

struct kernel_page_cache {
    struct kernel_heap *heap;
    struct physical_page_allocator *allocator;
    struct kernel_page_cache_record *record;
    enum kernel_page_cache_state state;
};

/* 一个 allocator 可注册多个独立缓存。统计/水位全局汇总，worker 与快照页各自拥有。
 * 元数据统一由对象mutex保护；实例、等待者和回调归还引用后释放聚合owner。 */
enum kernel_page_cache_status kernel_page_cache_init(
    struct kernel_page_cache *cache,
    struct kernel_heap *heap,
    struct physical_page_allocator *allocator);

/* 检查实际heap/allocator与worker绑定。调用者持有cache至
 * 全部MM/请求/别名结束并stop/join，末次后端清理仍由CPU0完成。 */
int kernel_page_cache_check_domain(const struct kernel_page_cache *cache,
    const struct kernel_heap *heap, const struct physical_page_allocator *allocator);
struct kernel_page_cache_concurrency_state {
    uint64_t requests_queued, requests_completed, entry_borrows;
    uint64_t deferred_snapshots, writeback_rounds;
    uint32_t pending, running;
    size_t entry_bytes, record_bytes, request_bytes;
    size_t execution_request_bytes;
    size_t index_capacity, index_live, index_tombstones;
    uint64_t executions_queued, executions_completed;
    uint64_t queue_ns, execute_ns, max_queue_ns, max_execute_ns;
};
/* 已有cache owner下的值快照；不固定业务状态，不扫描缓存或取得新引用。 */
void kernel_page_cache_get_concurrency_state(struct kernel_page_cache *,
    struct kernel_page_cache_concurrency_state *);
/* 内部同步CPU0执行。cache及argument的真实owner由可恢复的调用现场持有，
 * DONE及worker借用结束前不得退栈。无锁/用户pin/写入借用时提交；callback
 * 在普通CPU0任务、缓存保护外执行。关闭取消未执行者，执行者等实际结束。 */
enum kernel_page_cache_status kernel_page_cache_run_cpu0(struct kernel_page_cache *,
    void (*execute)(void *),void *argument);
struct kernel_lock_guard;
/* OFD位置操作的caller持唯一指定互斥资格至callback完成；executor只借用，
 * 不取得、转移或归还该锁。其他锁、user pin和cache写借用仍禁止排队。 */
enum kernel_page_cache_status kernel_page_cache_run_cpu0_borrowing(
    struct kernel_page_cache *,void (*execute)(void *),void *argument,
    const struct kernel_lock_guard *borrowed);
int kernel_page_cache_needs_cpu0(const struct kernel_page_cache *);
int kernel_page_cache_is_executor(const struct kernel_page_cache *);

/* Returns an owned physical-page reference that the caller must release. */
enum kernel_page_cache_status kernel_page_cache_get(
    struct kernel_page_cache *cache,
    const struct kernel_vfs_file *file,
    uint64_t page_index,
    uint64_t *physical_address,
    size_t *valid_bytes);

#define KERNEL_PAGE_CACHE_READ_BATCH_MAX (131072U / BOAROS_PAGE_SIZE)
struct kernel_page_cache_read_page {
    uint64_t physical_address;
    size_t valid_bytes;
    enum kernel_page_cache_status status;
};
/* 只装入本次请求覆盖的连续页。每项独立返回；只有OK项拥有物理页引用，
 * 调用者须归还全部成功项，包括首个错误之后的项。返回值是首个非OK状态。
 * 自有加载先一起完成、发布，再等待他人的loading，避免重叠批次互等。 */
enum kernel_page_cache_status kernel_page_cache_get_batch(
    struct kernel_page_cache *, const struct kernel_vfs_file *, uint64_t first,
    struct kernel_page_cache_read_page *pages, size_t count);

/* A successful lookup also returns an owned physical-page reference. */
/* 只借用当前已完成页；装入、resize或截断中的条目返回BUSY。 */
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
    int (*rearm)(void *owner, uint64_t virtual_address));
/* 已有alias owner/借用下使用；额外借用覆盖锁外通知，不能从裸指针复活。 */
void kernel_page_cache_alias_hold(struct kernel_page_cache_alias *);
void kernel_page_cache_alias_put(struct kernel_page_cache_alias *);
void kernel_page_cache_retry_alias(struct kernel_page_cache_alias *);
enum kernel_page_cache_status kernel_page_cache_request_writeback(struct kernel_page_cache *);
struct kernel_page_cache_resize { struct kernel_page_cache_entry *entry; uint64_t old_size; };
/* CPU0、文件操作门owner；prepare不持inode I/O，finish在关闭代次下收敛实际尺寸。
 * 未改变尺寸时只开放许可，不能清除旧内容。token不可复制，须覆盖后端结果。 */
int kernel_page_cache_resize_prepare(struct kernel_page_cache *,struct kernel_vfs_node *,
    uint64_t old_size,uint64_t target,struct kernel_page_cache_resize *);
void kernel_page_cache_resize_finish(struct kernel_page_cache_resize *,uint64_t actual);
void kernel_page_cache_alias_clone(struct kernel_page_cache_alias *target,
    const struct kernel_page_cache_alias *source, void *owner);
void kernel_page_cache_alias_detach(struct kernel_page_cache_alias *alias);
/* detach只停止发布，alias保持entry直到锁外drain；MM存储同样须存活。
 * drain不持MM gate、inode I/O或raw，不能由当前在途回调等待自己。 */
void kernel_page_cache_alias_drain(struct kernel_page_cache_alias *alias);
void kernel_page_cache_alias_mark_dirty(struct kernel_page_cache_alias *alias);
struct kernel_io_context;
/* 最终地址初始化，不能复制或跨任务归还；存活至end，覆盖entry及数据页。 */
struct kernel_page_cache_write_borrow {
    struct kernel_page_cache_entry *entry;
    struct kernel_io_context *owner;
    struct kernel_page_cache_write_borrow *registration, *next;
};
enum kernel_page_cache_status kernel_page_cache_alias_write_begin(
    struct kernel_page_cache_alias *, struct kernel_page_cache_write_borrow *);
void kernel_page_cache_write_end(struct kernel_page_cache_write_borrow *);
enum kernel_page_cache_status kernel_page_cache_wait_writable(
    struct kernel_page_cache *, const struct kernel_vfs_file *, uint64_t index, uint64_t physical);

int kernel_page_cache_writeback(struct kernel_page_cache *cache,
                                 struct kernel_vfs_node *node);
int kernel_page_cache_writeback_before(struct kernel_page_cache *cache,
    struct kernel_vfs_node *node, uint64_t end);
int kernel_page_cache_writeback_range(struct kernel_page_cache *cache,
    struct kernel_vfs_node *node, uint64_t start, uint64_t end);
/* Caller owns the inode mutation lock. Growth only touches the cached old
 * EOF tail; shrink follows mapping invalidation and removes out-of-range pages. */
void kernel_page_cache_truncate(struct kernel_page_cache *cache,
    struct kernel_vfs_node *node, uint64_t new_size);

#if BOAROS_PAGE_CACHE_READAHEAD_PAGES
/* A queue slot pins only the inode. The opaque cookie cancels unsubmitted
 * work; a published read retains its owners through completion/reset. */
uint64_t kernel_page_cache_readahead(struct kernel_page_cache *cache,
    struct kernel_vfs_node *node, uint64_t first, uint64_t cookie);
void kernel_page_cache_cancel_readahead(struct kernel_page_cache *cache, uint64_t cookie);
#endif

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
/* purge/destroy由CPU0独占生命周期owner调用：先结束外部来源/MM/别名/请求，
 * stop/join只结束worker，不替代这些调用者的存活保证。失败保留cache owner。 */

#endif
