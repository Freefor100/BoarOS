#ifndef BOAROS_FS_VFS_OBJECTS_H
#define BOAROS_FS_VFS_OBJECTS_H

#include "vfs_internal.h"
#include "record_lock.h"
#include "flock.h"
#include <kernel/cost.h>
#include <kernel/vfs.h>
#include <kernel/heap.h>

#define VFS_MOUNT_STATE_EMPTY 0U
#define VFS_MOUNT_STATE_LIVE UINT32_C(0x564d4e54)
#define VFS_MOUNT_STATE_CLEANUP UINT32_C(0x56434c4e)
#define VFS_FILE_STATE_EMPTY 0U
#define VFS_FILE_STATE_LIVE UINT32_C(0x5646494c)
#define VFS_FILE_STATE_CLEANUP UINT32_C(0x5646434c)

struct kernel_vfs_notify_ops;
struct kernel_vfs_instance {
    const struct kernel_vfs_backend *ops;
    void *backend_data;
    struct kernel_mutex namespace_lock;
    struct kernel_rwlock reconfiguration; /* Namespace mutation/new writer versus live remount. */
    struct kernel_heap *heap;
    struct kernel_page_cache *page_cache;
    size_t writeback_block_size; /* Zero means no block alignment requirement. */
    struct kernel_vfs_node *nodes, *node_index;
    size_t node_count;
    struct kernel_vfs_node *cleanup_nodes;
    struct kernel_vfs_dentry *paths; /* Weak canonical directory-entry registry. */
    uint32_t external_files;
    uint64_t writeback_error_sequence;
    int writeback_error;
    uint8_t read_only;
    uint8_t quiescing;
    struct kernel_raw_lock error_lock;
    struct kernel_vfs_dentry *root_name;
    struct kernel_vfs_dentry *dentry_cache_first, *dentry_cache_last;
    size_t dentry_cache_count, dentry_cache_limit;
    size_t transient_owners; /* Node retirement and clean reclaim still borrow the backend. */
    uint8_t namespace_parallel; /* Enabled only after this mount's metadata owners are ready. */
    struct kernel_writeback_owner writeback;
    struct kernel_vfs_mount backend_mount; /* Instance lifetime, independent of caller view storage. */
    uint32_t mount_views;
    struct kernel_vfs_mount *views;
    struct kernel_vfs_instance *active_next, **active_previous;
    struct kernel_vfs_instance *cleanup_next;
    struct kernel_vfs_mount *owner_storage;
    void (*release_storage)(struct kernel_vfs_mount *);
    uint8_t cleanup_queued, cleanup_running;
    struct kernel_vfs_mount *retry_view;
    const struct kernel_vfs_notify_ops *notify_ops;
    unsigned notify_marks; /* Protected by the active notification backend. */
    uint64_t device_id; /* Filesystem st_dev, shared by every mount view. */
};


struct kernel_vfs_node {
    struct kernel_pipe *fifo_pipe;
    struct kernel_vfs_node *next, **previous;
    struct kernel_vfs_node *index_left, *index_right;
    uint8_t index_height;
    struct kernel_page_cache_tree cache_pages;
    struct kernel_page_cache_dirty dirty_cache_pages;
    struct kernel_memory_object *memory;
    struct kernel_file_mapping_state *mapping_state;
    struct kernel_record_lock_state record_locks;
    struct kernel_flock_identity flock_identity;
    struct kernel_vfs_instance *instance;
    struct kernel_vfs_mount *mount;
    void *backend_data;
    uint64_t inode;
    uint64_t max_size;
    struct kernel_mutex write_operation;
    struct kernel_rwlock io_lock;
    uint64_t size;
    uint64_t writeback_error_sequence;
    int writeback_error;
    uint32_t mode;
    uint32_t references;
    uint32_t open_files;
    uint32_t write_openers;
    uint32_t exec_users;
    uint8_t closed;
    uint8_t unlinked;
    uint8_t tmpfile_linkable; /* Only a never-published, non-exclusive tmpfile may gain its first name. */
    uint8_t retired;
    uint8_t generated_control, control_write_prefix;
#if BOAROS_COST_DIAGNOSTICS
    uint8_t generated_diagnostic;
#endif
    /* Directory entries use a separate qualification from file data I/O. */
    struct kernel_rwlock namespace_lock;
    uint32_t uid, gid;
    uint8_t attributes; /* Three Linux inode flags, published after backend acceptance. */
    uint8_t file_seals; /* tmpfs seals, protected by the inode I/O qualification. */
    uint8_t memfd;
    uint64_t shared_write_bytes; /* Committed MAYWRITE VMA bytes, including read-only mappings. */
#if BOAROS_MAX_CPUS > 1
    struct kernel_raw_lock cache_index_lock;
    uint8_t cache_index_closed;
#endif
};

/* A dentry owns its inode and canonical parent; mount aliases share mutations. */
struct kernel_vfs_dentry {
    struct kernel_vfs_file file;
    struct kernel_heap *heap;
    struct kernel_vfs_dentry *parent, *next, **previous;
    uint32_t references;
    struct kernel_vfs_dentry *cache_next, *cache_previous;
    uint8_t cached, detached, recently_used, mountpoint_seen;
    uint32_t mountpoints;
    char *name;
    struct kernel_vfs_dentry *index_left, *index_right;
    /* Weak child names. VFS state precedes this lock on publication and
     * final retirement. A cache-hit reader can obtain a reference here. */
    struct kernel_raw_lock names_lock;
    struct kernel_vfs_dentry *children, *lookup_hint;
    uint8_t index_height;
    char initial_name[];
};
/* A path owns one mount view and dentry. Its file borrows the dentry inode. */
struct kernel_vfs_path {
    struct kernel_vfs_file file;
    struct kernel_heap *heap;
    struct kernel_vfs_dentry *dentry;
    struct kernel_vfs_path *next, **previous;
    struct kernel_vfs_path *index_left, *index_right;
    uint8_t index_height;
    struct kernel_vfs_mount *mounted_here;
    uint32_t references;
    uint8_t transient; /* Resolver-owned component storage, never published. */
    bool borrow_mount; /* Lifetime covered by a resolver input path. */
};

struct kernel_vfs_rename_result {
    uint64_t replaced_inode;
    int replaced_last_link, changed;
};

struct kernel_open_file_description;

/* 前台后端操作选择执行位置；后台写回仍由缓存 worker 推进。 */
enum kernel_vfs_execution {
    KERNEL_VFS_EXECUTION_CPU0 = 0,
    KERNEL_VFS_EXECUTION_CALLER,
};
/* 回调返回负 errno；close_node 只释放私有 handle，不等待设备。
 * 后端分配的 node 必须位于同一堆对象首部，发布后由通用层回收。 */
struct kernel_vfs_backend {
    enum kernel_vfs_execution read_execution;
    enum kernel_vfs_execution file_operation_execution;
    enum kernel_vfs_execution namespace_execution;
    /* Existing positive dentries are authoritative: all name changes use VFS
     * parent qualification and update its index before releasing it. */
    bool owns_dentry_names;
    /* Control writes authorize with the OFD opener, not current credentials. */
    bool control_open_credentials;
    int (*check_domain)(const struct kernel_vfs_node *, const struct kernel_heap *,
        const struct physical_page_allocator *);
    int (*error)(struct kernel_vfs_instance *instance);
    int (*root)(struct kernel_vfs_instance *instance, uint64_t *inode, uint32_t *mode);
    /* Owned-name backends accept NULL mode: open imports real inode state. */
    int (*lookup)(struct kernel_vfs_instance *instance, uint64_t parent,
        const char *name, size_t length, uint64_t *inode, uint32_t *mode);
    /* Optional lookup through the live, namespace-qualified directory. */
    int (*lookup_node)(struct kernel_vfs_node *parent, const char *name,
        size_t length, uint64_t *inode, uint32_t *mode);
    /* A backend with one directory entry per inode can expose several lookup
     * spellings (VFAT long/short names). They share one canonical path owner. */
    int (*entry_name)(struct kernel_vfs_instance *, uint64_t inode, char *, size_t);
    int (*readlink)(struct kernel_vfs_node *node, char *buffer, size_t size, size_t *count);
    /* Object links resolve to a pinned path without a pathname relookup. */
    int (*follow_link)(struct kernel_vfs_node *node, unsigned resolve,
                       struct kernel_vfs_path **owner);
    /* A pseudo-object link may open a new description without a VFS path. */
    int (*reopen_link)(struct kernel_vfs_node *node, struct kernel_heap *heap,
        uint32_t flags, struct kernel_open_file_description **owner);
    /* Metadata of a pathless object named by a final symlink. */
    int (*stat_link)(struct kernel_vfs_node *node,
                     struct kernel_vfs_stat *stat);
    /* Generated files return an owned, single-read-epoch snapshot. The OFD
     * releases it and creates a new one after seek to offset zero. */
    int (*snapshot)(struct kernel_vfs_node *node, struct kernel_heap *heap,
        struct kernel_namespace *ipc_namespace, struct kernel_pid_registry *pid_view, char **buffer, size_t *length);
    /* Controls use a complete kernel buffer and return the new position,
     * which can differ from old position + accepted bytes. The files layer
     * commits that position only after successful user copying. */
    int (*control)(struct kernel_vfs_node *node, int write, uint64_t *offset,
        char *buffer, size_t size, size_t *count);
    int (*truncate)(struct kernel_vfs_node *node, uint64_t size, uint64_t *actual, int *changed);
    int (*fallocate)(struct kernel_vfs_node *node, unsigned mode, uint64_t offset,
        uint64_t length, uint64_t *actual, int *changed);
    /* prepare returns a physically backed prefix without publishing EOF.
     * If complete is set, the caller must finish accepted data and unused tail
     * cleanup under its write-operation qualification, including error exits. */
    int (*prepare_write)(struct kernel_vfs_node *node, uint64_t offset,
        size_t length, size_t *ready, int *complete);
    int (*complete_write)(struct kernel_vfs_node *node);
    /* Delayed admission borrows prepared blocks until the actual copied
     * prefix is known; overlapping sources share acceptance and lifetime. */
    int (*reserve_write)(struct kernel_vfs_node *, uint64_t, size_t, size_t *, int *, struct file_write_reservation *);
    void (*finish_reservation)(struct kernel_vfs_node *, struct file_write_reservation *, uint64_t start, uint64_t accepted_end);
    /* Accept while inode qualification still protects this mapping generation. */
    void (*accept_reservation)(struct kernel_vfs_node *, struct file_write_reservation *, uint64_t start, uint64_t accepted_end);

    struct kernel_block_device *(*block_device)(struct kernel_vfs_instance *instance);
    int (*close_node)(struct kernel_vfs_node *node);
    int (*writeback_allowed)(struct kernel_vfs_instance *instance);
    /* No allocation, I/O or executor migration; only release idle clean storage. */
    void (*reclaim_clean)(struct kernel_vfs_instance *instance);
    /* Caller has returned source/entry and inode qualifications before waiting. */
    int (*writeback_reclaim)(struct kernel_vfs_instance *instance, int memory_pressure);
    /* Returns after capturing/registering a fixed target. The backend completes
     * request once durable or failed, without borrowing a CPU0 executor stack.
     * NULL denotes a memory-only backend without persistent metadata. */
    void (*sync_prepare)(struct kernel_vfs_node *node, int data_only,
                         struct kernel_vfs_sync_request *request);
    /* Synchronizes the instance owned by the view. NULL denotes a backend
     * without persistent writeback. */
    int (*sync_filesystem)(struct kernel_vfs_mount *mount);
    int (*flush)(struct kernel_vfs_instance *instance);
    int (*symlink)(struct kernel_vfs_instance *instance, uint64_t parent_inode, const char *target, const char *path);
    int (*mknod)(struct kernel_vfs_instance *instance, uint64_t parent_inode, const char *path,
        uint32_t type, uint32_t mode, uint32_t device, uint32_t umask);
    int (*link)(struct kernel_vfs_instance *instance, uint64_t source_inode,
        uint64_t target_parent, const char *name);
    int (*rename)(struct kernel_vfs_instance *instance,
        uint64_t old_parent, const char *old_name, uint64_t new_parent,
        const char *new_name, unsigned flags, struct kernel_vfs_rename_result *result);
    /* Failure keeps the attached root and backend owner reachable for retry. */
    int (*remount)(struct kernel_vfs_mount *mount, int read_only);
    int (*prepare_unmount)(struct kernel_vfs_mount *mount);
    int (*unmount)(struct kernel_vfs_mount *mount);
    int (*statfs)(struct kernel_vfs_mount *mount,
        struct kernel_vfs_statfs *stat);
    int (*set_times)(struct kernel_vfs_file *file,
        const struct kernel_vfs_timespec times[2]);
    int (*get_flags)(struct kernel_vfs_node *,uint32_t *);
    int (*set_flags)(struct kernel_vfs_node *,uint32_t);
    int (*set_mode)(struct kernel_vfs_file *file, uint32_t mode);
    int (*set_owner)(struct kernel_vfs_file *file, uint32_t uid, uint32_t gid);
    int (*permission)(struct kernel_vfs_node *, const struct kernel_credentials *, unsigned);
    /* ACL-aware DAC on backends that store per-inode access ACLs. */
    int (*acl_permission)(struct kernel_vfs_node *, const struct kernel_credentials *, unsigned);
    int (*exec_metadata)(struct kernel_vfs_node *, struct kernel_vfs_exec_metadata *);
    int (*killpriv)(struct kernel_vfs_node *, uint32_t mode);
    int (*open)(struct kernel_vfs_mount *mount,
        const char *path, uint64_t inode_number,
        uint32_t inode_mode,
        struct kernel_vfs_file *file);
    int (*create)(struct kernel_vfs_mount *mount,
        uint64_t parent_inode, const char *path,
        uint32_t mode,
        struct kernel_vfs_file *file, uint32_t umask);
    int (*tmpfile)(struct kernel_vfs_mount *, uint64_t parent_inode, uint32_t mode,
        struct kernel_vfs_file *, uint32_t umask);
    int (*link_tmpfile)(struct kernel_vfs_instance *, uint64_t inode, uint64_t parent, const char *name);
    /* The backend takes mutation qualification only when it publishes atime. */
    void (*accessed)(struct kernel_vfs_file *file);
    /* A directory scan observed expired relatime. Recheck before publishing. */
    void (*accessed_after_relatime)(struct kernel_vfs_file *file);
    int (*modified)(struct kernel_vfs_file *file,
        uint64_t offset, int append);
    int (*stat)(const struct kernel_vfs_file *file,
        struct kernel_vfs_stat *stat);
    int (*mkdir)(struct kernel_vfs_mount *mount,
        uint64_t parent_inode, const char *path,
        uint32_t mode, uint32_t umask);
    int (*unlink)(struct kernel_vfs_mount *mount,
        uint64_t parent_inode, const char *path);
    int (*rmdir)(struct kernel_vfs_mount *mount,
        uint64_t parent_inode, const char *path);
    /* Namespace and target I/O qualifications protect the resolved identity.
     * Exclusive means the operation and the name cache own the only dentry
     * references and its embedded file owns the target's only node reference. */
    int (*remove_resolved)(struct kernel_vfs_mount *, uint64_t parent_inode,
        const char *name, struct kernel_vfs_node *target, bool directory, bool exclusive);
    void (*release_unlinked)(struct kernel_vfs_node *node);
    int (*read_directory)(struct kernel_vfs_file *, struct kernel_vfs_dir_batch *);
    int (*dir_entry)(struct kernel_vfs_file *file,
        uint64_t position,
        uint64_t *next_position,
        uint64_t *inode,
        uint8_t *type,
        char *name,
        size_t name_size);
    /* Nonzero alignment advertises real synchronous file-data direct I/O. */
    size_t (*direct_alignment)(struct kernel_vfs_node *);
    /* Closed mapping epoch; direction is read=0, write=1, drain-only=-1. */
    int (*direct_io)(struct kernel_vfs_node *,uint64_t,void *,size_t,int,size_t *);
    uint64_t (*direct_write_limit)(struct kernel_vfs_node *);
    int (*pread)(struct kernel_vfs_node *node,
        uint64_t offset,
        void *buffer,
        size_t size,
        size_t *bytes_read);
    int (*pread_batch)(struct kernel_vfs_node *node,
        struct file_read_span *spans, size_t count, struct file_read_workspace *workspace,
        const struct file_read_completion *completion);
    void (*memory_written)(struct kernel_vfs_node *node, uint64_t end);
    int (*writeback)(struct kernel_vfs_node *node,
        struct file_write_span *spans, size_t count);
    /* Caller owns inode io_lock; positive result is the returned byte count. */
    int (*xattr)(struct kernel_vfs_node *, enum kernel_xattr_operation,
        const char *, void *, size_t, uint32_t);
    const char *type; /* Immutable filesystem type, independent of its mount source. */
};
/* Called under the same inode writer as the corresponding metadata update. */


/* Writable OFDs already exclude RO transitions. Other mutation publishers
 * borrow this qualification after deciding that an update is needed. */
static inline void kernel_vfs_mutation_lock(struct kernel_vfs_mount *mount,
    struct kernel_lock_guard *guard)
{
    struct kernel_vfs_instance *instance=mount->private_data;
    if (!kernel_lock_held(&instance->reconfiguration,0))
        kernel_rwlock_read(&instance->reconfiguration,guard);
}
int kernel_vfs_inode_times_allowed(struct kernel_vfs_node *, const struct kernel_vfs_timespec [2]);
void kernel_vfs_creation_identity(uint32_t parent_mode, uint32_t parent_gid,
    uint32_t *mode, uint32_t *uid, uint32_t *gid);
void kernel_vfs_record_writeback_error(struct kernel_vfs_node *node, int error);
void kernel_vfs_record_mount_writeback_error(struct kernel_vfs_mount *mount, int error);
int kernel_vfs_attach_unnamed_path(struct kernel_vfs_file *, const char *name);
int kernel_vfs_publish_node(struct kernel_vfs_mount *mount, struct kernel_vfs_node *node,
    struct kernel_vfs_file *file, int creating);
#endif
