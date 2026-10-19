#ifndef BOAROS_FS_VFS_INTERNAL_H
#define BOAROS_FS_VFS_INTERNAL_H

#include <common/file_io.h>

#include <kernel/cost.h>
#include <stddef.h>
#include <kernel/sync.h>
#include <stdint.h>

struct kernel_vfs_file;
struct kernel_vfs_size_limit;
int kernel_vfs_check_write_limit(struct kernel_vfs_size_limit *, uint64_t, size_t *);
struct kernel_memory_object;
struct kernel_memory_object *kernel_vfs_file_memory(const struct kernel_vfs_file *file);
struct kernel_page_cache;
struct kernel_page_cache_entry;
struct kernel_page_cache_dirty_tree {
    struct kernel_page_cache_entry *rbh_root;
};
struct kernel_page_cache_dirty {
    struct kernel_page_cache_dirty_tree tree;
    struct kernel_page_cache_dirty *queue_next, *queue_previous;
    size_t count;
};
struct kernel_vfs_file;
struct kernel_vfs_mount;
struct kernel_vfs_instance;
struct kernel_vfs_node;
#if BOAROS_COST_DIAGNOSTICS
int kernel_vfs_file_is_cost(const struct kernel_vfs_file *file);
#endif
struct kernel_heap;
struct physical_page_allocator;
/* The owning backend checks its inode/storage domain without pathname lookup. */
int kernel_vfs_check_mapping_domain(const struct kernel_vfs_node *,
    const struct kernel_heap *, const struct physical_page_allocator *);
struct kernel_record_lock_state;

struct kernel_vfs_node *kernel_vfs_file_node(
    const struct kernel_vfs_file *file);
uint64_t kernel_vfs_file_max_size(const struct kernel_vfs_file *file);
struct kernel_record_lock_state *kernel_vfs_node_record_locks(
    struct kernel_vfs_node *node);

/* Borrow with IRQs disabled; pin before restoring IRQs unless a live owner
 * already covers the target. Namespace locking alone does not exclude reclaim. */
struct kernel_vfs_node *kernel_vfs_node_find(struct kernel_vfs_instance *, uint64_t inode);

#define KERNEL_VFS_READ_BATCH_MAX FILE_READ_BATCH_MAX
/* Caller owns a live node/read gate and at most KERNEL_VFS_READ_BATCH_MAX
 * nonoverlapping, valid kernel destinations until return. Successful spans
 * may stop at EOF; errors preserve each contiguous byte prefix. Multi-output
 * calls lend an independent request workspace through synchronous completion.
 * An optional completion publishes a final span before batch drain, at most once;
 * unreported results arrive on return. Its result pointer
 * is borrowed only during the callback. No later payload writes follow it. */
int kernel_vfs_node_pread_batch(struct kernel_vfs_node *node,
    struct file_read_span *spans, size_t count, struct file_read_workspace *workspace, const struct file_read_completion *completion);

int kernel_vfs_node_pread(struct kernel_vfs_node *node,
                          uint64_t offset,
                          void *buffer,
                          size_t size,
                          size_t *bytes_read);
int kernel_vfs_file_is_control(const struct kernel_vfs_file *file);
int kernel_vfs_file_control(const struct kernel_vfs_file *file, int write,
    uint64_t offset, char *buffer, size_t size, size_t *count);
int kernel_vfs_file_generated(const struct kernel_vfs_file *file);
int kernel_vfs_check_io_domain(const struct kernel_vfs_node *,const struct kernel_heap *,
    const struct physical_page_allocator *);
int kernel_vfs_file_snapshot(const struct kernel_vfs_file *file,
                             struct kernel_heap *heap,
                             char **buffer, size_t *length);

struct kernel_page_cache_entry **kernel_vfs_node_cache_pages(
    struct kernel_vfs_node *node);
struct kernel_page_cache_dirty *kernel_vfs_node_dirty_pages(struct kernel_vfs_node *node);
int kernel_vfs_node_writeback_allowed(const struct kernel_vfs_node *node);
void kernel_vfs_node_written(struct kernel_vfs_node *node, uint64_t end);
#define KERNEL_VFS_WRITEBACK_MAX 32U
/* Stable sources remain caller-owned. The backend may retain them through
 * ordered commit; source.bytes includes the entire retained allocation, not
 * just this dirty range. source_buffer/source_size describe initialized stable
 * bytes surrounding buffer; logical offset/size still define completion and EOF.
 * Shared source ownership is charged once. */
int kernel_vfs_node_writeback(struct kernel_vfs_node *node,
    struct file_write_span *spans, size_t count);

/* 调用者已有node owner；尺寸由inode修改者发布，不重复查询生命周期。 */
uint64_t kernel_vfs_node_size(const struct kernel_vfs_node *node);
const struct kernel_vfs_mount *kernel_vfs_node_mount(
    const struct kernel_vfs_node *node);

struct kernel_page_cache *kernel_vfs_file_page_cache(
    const struct kernel_vfs_file *file);

int kernel_vfs_node_try_read(struct kernel_vfs_node *node, struct kernel_lock_guard *guard);
void kernel_vfs_namespace_lock(struct kernel_vfs_mount *mount, struct kernel_lock_guard *guard);
/* 调用者已取得write_operation，CPU0执行者只借用，不能排队后再等同一门闩。 */
int kernel_vfs_ftruncate_borrowing(struct kernel_vfs_file *, uint64_t,
    const struct kernel_lock_guard *, struct kernel_vfs_size_limit *);
int kernel_vfs_fallocate_borrowing(struct kernel_vfs_file *, unsigned,
    uint64_t, uint64_t, const struct kernel_lock_guard *, struct kernel_vfs_size_limit *);
/* Whole syscall write/truncate gate; rank 15, before namespace/inode locks. */
void kernel_vfs_file_write_lock(struct kernel_vfs_file *file,
    struct kernel_lock_guard *guard);
void kernel_vfs_node_lock(struct kernel_vfs_node *node, struct kernel_lock_guard *guard, int write);
void kernel_vfs_record_writeback_error(struct kernel_vfs_node *node, int error);

/* The caller owns file/OFD and lends its observed_error cursor through
 * completion. CPU0 links only outstanding durable targets; each waiter
 * protects its result and registration handshake. */
struct kernel_vfs_sync_request {
    struct kernel_vfs_sync_request *index_left, *index_right;
    struct kernel_vfs_file *file;
    struct kernel_raw_lock metadata;
    struct kernel_wait_queue changed;
    uint64_t target;
    uint64_t *observed_error;
    int result;
    uint8_t done, index_height;
};
void kernel_vfs_sync_complete(struct kernel_vfs_sync_request *, int result);
int kernel_vfs_sync_borrowing(struct kernel_vfs_file *, int data_only,
    uint64_t *observed_error, const struct kernel_lock_guard *borrowed);

#endif
