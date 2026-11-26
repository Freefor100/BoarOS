#ifndef BOAROS_FS_VFS_INTERNAL_H
#define BOAROS_FS_VFS_INTERNAL_H

#include <common/file_io.h>

#include <kernel/cost.h>
#include <stddef.h>
#include <kernel/sync.h>
#include <kernel/raw_lock.h>

/* Weak identities, mount edges and FS context publication; no I/O or callbacks. */
extern struct kernel_raw_lock kernel_vfs_state_lock;
struct kernel_fs_context;
struct kernel_vfs_path;
struct kernel_vfs_file;
struct kernel_vfs_size_limit;
struct kernel_vfs_user_write_result;
struct kernel_mm_long_pin;
int kernel_vfs_write_long_pins(struct kernel_vfs_file *, const struct kernel_mm_long_pin *,
    size_t first, size_t count, uint64_t position, int append, int direct,
    struct kernel_vfs_size_limit *, struct kernel_vfs_user_write_result *);
struct kernel_credentials;
struct kernel_vfs_mount;
struct kernel_heap;
enum kernel_overlay_layer {
    KERNEL_OVERLAY_LOWER, KERNEL_OVERLAY_UPPER, KERNEL_OVERLAY_WORK
};
/* The context keeps its directory references through creation. The backend
 * acquires independent owners for an instance that is successfully created. */
int kernel_overlay_mount(struct kernel_heap *, uint64_t, const struct kernel_credentials *,
    struct kernel_vfs_path *const [3], struct kernel_vfs_mount **);
int kernel_vfs_reuse_cgroup_tree(struct kernel_heap *,uint64_t,struct kernel_vfs_path **);
int kernel_vfs_reuse_block_tree(uint64_t,const char *,int,int,struct kernel_heap *,uint64_t,struct kernel_vfs_path **);

size_t kernel_fs_context_pivot_publish(const struct kernel_fs_context *,struct kernel_vfs_path *,struct kernel_vfs_path *);
struct kernel_vfs_path;
int kernel_vfs_path_filesystem_string(const struct kernel_vfs_path *,char *,size_t);
/* Capture a mutable parent edge and its owner in the same state protection. */
int kernel_vfs_path_parent_pin(const struct kernel_vfs_path *path,struct kernel_vfs_path **owner);
/* Existing object capability (PTY master) selects kernel-owned children without new DAC search. */
int kernel_vfs_path_lookup_object(struct kernel_vfs_path *,const char *,size_t,struct kernel_vfs_path **);
/* Look up a directory entry of the selected layer without following another
 * filesystem mounted on that entry. Honor search permission on the layer. */
int kernel_vfs_path_lookup_layer(struct kernel_vfs_path *,const char *,size_t,struct kernel_vfs_path **);
/* Atomically exchange two existing ext4 backing names within the layer. */
int kernel_vfs_exchange_layer_entries(struct kernel_vfs_path *,struct kernel_vfs_path *,
    const char *,const char *);
#include <stdint.h>

#define VFS_PATH_CLOSING (UINT32_C(1)<<31U)
#define VFS_MS_NOSYMFOLLOW UINT64_C(0x100)
#define VFS_MOUNT_ATTR_NOSYMFOLLOW UINT64_C(0x200000)
#define VFS_ST_NOSYMFOLLOW UINT64_C(0x2000)
#define VFS_ATIME_NONE 1U
#define VFS_ATIME_STRICT 2U
#define VFS_ATIME_NODIR 4U
static inline uint8_t kernel_vfs_atime_policy(uint64_t flags,uint8_t previous,int remount)
{
    if (remount && !(flags & (UINT64_C(1024)|2048|2097152|16777216))) return previous;
    return (flags&16777216U ? VFS_ATIME_STRICT : flags&1024U ? VFS_ATIME_NONE : 0) |
        (flags&2048U ? VFS_ATIME_NODIR : 0);
}
struct kernel_vfs_file;
/* An overlay inode retains both layer paths. Borrow the selected backing
 * file for the operation while the caller holds the visible file. */
const struct kernel_vfs_file *kernel_vfs_data_file(const struct kernel_vfs_file *);
int kernel_vfs_copy_range_files(const struct kernel_vfs_file *input,
    const struct kernel_vfs_file *output, const struct kernel_vfs_file **source,
    const struct kernel_vfs_file **target);
struct kernel_vfs_path *kernel_vfs_backing_path(const struct kernel_vfs_file *);
int kernel_vfs_file_atime_enabled(const struct kernel_vfs_file *);
int kernel_vfs_file_noatime_allowed(const struct kernel_vfs_file *);
struct kernel_vfs_size_limit;
int kernel_vfs_check_write_limit(struct kernel_vfs_size_limit *, uint64_t, size_t *);
struct kernel_memory_object;
struct kernel_memory_object *kernel_vfs_file_memory(const struct kernel_vfs_file *file);
struct kernel_page_cache;
struct kernel_page_cache_entry;
struct kernel_page_cache_tree {
    struct kernel_page_cache_entry *rbh_root;
};
struct kernel_page_cache_dirty_tree {
    struct kernel_page_cache_entry *rbh_root;
};
struct kernel_page_cache_dirty {
    struct kernel_page_cache_dirty_tree tree;
    struct kernel_page_cache_dirty *queue_next, *queue_previous;
    size_t count;
    uint64_t expiry;
#if BOAROS_MAX_CPUS > 1
    unsigned batch_state;
#endif
};
struct kernel_vfs_file;
struct kernel_vfs_mount;
struct kernel_vfs_instance;
struct kernel_vfs_node;
/* Return 1 if the caller needs a larger array, with its exact current size.
 * The caller owns every returned pin, including the prefix on overflow. */
int kernel_page_cache_pin_dirty_nodes(struct kernel_page_cache *,
    const struct kernel_vfs_instance *, struct kernel_vfs_node **, size_t, size_t *, size_t *);
int kernel_page_cache_writeback_mount(struct kernel_page_cache *,
    struct kernel_vfs_node **, size_t);
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
/* Per-call NOAPPEND cannot bypass inode format restrictions. */
int kernel_vfs_file_noappend_allowed(const struct kernel_vfs_file *);
int kernel_vfs_file_mincore_allowed(const struct kernel_vfs_file *);
/* The caller pins the mount and holds its topology stable. */
int kernel_vfs_mount_root_string(const struct kernel_vfs_mount *,const struct kernel_vfs_path *,
    char *,size_t);
const char *kernel_vfs_mount_type(const struct kernel_vfs_mount *);
uint64_t kernel_vfs_mount_dominating_group(const struct kernel_vfs_mount *,const struct kernel_vfs_path *);
int kernel_vfs_mount_set_source(struct kernel_vfs_mount *,const char *);
int kernel_vfs_namespace_requires_cpu0(const struct kernel_vfs_mount *);
int kernel_vfs_file_metadata_requires_cpu0(const struct kernel_vfs_file *file);
uint64_t kernel_vfs_file_max_size(const struct kernel_vfs_file *file);
struct kernel_record_lock_state *kernel_vfs_node_record_locks(
    struct kernel_vfs_node *node);

/* Borrow under kernel_vfs_state_lock; pin before dropping the protection. */
struct kernel_vfs_node *kernel_vfs_node_find(struct kernel_vfs_instance *, uint64_t inode);
struct kernel_vfs_node *kernel_vfs_node_pin(struct kernel_vfs_instance *, uint64_t inode);
void kernel_vfs_cleanup_node(struct kernel_vfs_node *);
/* Terminal node snapshot; includes final releases already removed from the index. */
int kernel_vfs_nodes_internal_idle(struct kernel_vfs_instance *);
uint32_t kernel_vfs_internal_file_count(struct kernel_vfs_instance *);
/* The caller holds kernel_vfs_state_lock and has a live view. */
int kernel_vfs_mount_view_acquire_locked(struct kernel_vfs_mount *);
void kernel_vfs_mount_view_release(struct kernel_vfs_mount *);
uint64_t kernel_vfs_reclaim_clean(struct physical_page_allocator *, uint64_t target_pages);
unsigned kernel_vfs_cache_pressure(void);
void kernel_vfs_set_cache_pressure(unsigned);
void kernel_vfs_drop_caches(struct physical_page_allocator *, unsigned);
int kernel_vfs_nodes_idle(struct kernel_vfs_instance *, struct kernel_vfs_node *root);

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
int kernel_vfs_file_is_control(const struct kernel_vfs_file *file,int write);
/* Bounded legacy writes may opt into pwrite; low seven bits are import size. */
#define KERNEL_VFS_CONTROL_PREFIX_POSITIONED 0x80U
size_t kernel_vfs_file_control_prefix(const struct kernel_vfs_file *file);
int kernel_vfs_file_control_open_credentials(const struct kernel_vfs_file *);
int kernel_vfs_file_control_permission(const struct kernel_vfs_file *file, int write);
int kernel_vfs_file_control(const struct kernel_vfs_file *file, void *open_context, int write,
    uint64_t *offset, char *buffer, size_t size, size_t *count);
int kernel_vfs_file_generated(const struct kernel_vfs_file *file);
int kernel_vfs_check_io_domain(const struct kernel_vfs_node *,const struct kernel_heap *,
    const struct physical_page_allocator *);
struct kernel_namespace;
struct kernel_pid_registry;
int kernel_vfs_file_snapshot(const struct kernel_vfs_file *file,
                             struct kernel_heap *heap,
                             struct kernel_namespace *ipc_namespace,
                             struct kernel_pid_registry *pid_view,
                             char **buffer, size_t *length);

struct kernel_page_cache_dirty *kernel_vfs_node_dirty_pages(struct kernel_vfs_node *node);
int kernel_vfs_node_writeback_allowed(const struct kernel_vfs_node *node);
void kernel_vfs_node_written(struct kernel_vfs_node *node, uint64_t end);
#define KERNEL_VFS_WRITEBACK_MAX 32U
#define KERNEL_VFS_GROWTH_WRITEBACK_MAX 128U
/* Stable sources remain caller-owned. The backend may retain them through
 * ordered commit; source.bytes includes the entire retained allocation, not
 * just this dirty range. source_buffer/source_size describe initialized stable
 * bytes surrounding buffer; logical offset/size still define completion and EOF.
 * Shared source ownership is charged once. */
int kernel_vfs_node_writeback(struct kernel_vfs_node *node,
    struct file_write_span *spans, size_t count);

/* 调用者已有node owner；尺寸由inode修改者发布，不重复查询生命周期。 */
uint64_t kernel_vfs_node_size(const struct kernel_vfs_node *node);
int kernel_vfs_node_cache_reporting(const struct kernel_vfs_node *node);
void kernel_vfs_file_accessed_nowait(struct kernel_vfs_file *file, uint32_t open_flags);
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
int kernel_vfs_aio_direct_write(struct kernel_vfs_file *,uint64_t,const void *,size_t,size_t *,
    const struct kernel_lock_guard *);
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
    uint64_t writeback_epoch;
};
void kernel_vfs_sync_complete(struct kernel_vfs_sync_request *, int result);
int kernel_vfs_sync_borrowing(struct kernel_vfs_file *, int data_only,
    uint64_t *observed_error, const struct kernel_lock_guard *borrowed);
int kernel_vfs_sync_range_borrowing(struct kernel_vfs_file *, uint64_t start,
    uint64_t end, int data_only, uint64_t *observed_error,
    const struct kernel_lock_guard *borrowed);

#endif
