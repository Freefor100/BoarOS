#ifndef BOAROS_FS_VFS_OBJECTS_H
#define BOAROS_FS_VFS_OBJECTS_H

#include "vfs_internal.h"
#include "record_lock.h"
#include <kernel/vfs.h>
#include <kernel/heap.h>

#define VFS_MOUNT_STATE_EMPTY 0U
#define VFS_MOUNT_STATE_LIVE UINT32_C(0x564d4e54)
#define VFS_MOUNT_STATE_CLEANUP UINT32_C(0x56434c4e)
#define VFS_FILE_STATE_EMPTY 0U
#define VFS_FILE_STATE_LIVE UINT32_C(0x5646494c)
#define VFS_FILE_STATE_CLEANUP UINT32_C(0x5646434c)

struct kernel_vfs_instance {
    const struct kernel_vfs_backend *ops;
    void *backend_data;
    struct kernel_mutex namespace_lock;
    struct kernel_heap *heap;
    struct kernel_page_cache *page_cache;
    struct kernel_vfs_node *nodes;
    struct kernel_vfs_node *cleanup_nodes;
    struct kernel_vfs_path *paths; /* Weak registry; live callers own references. */
    uint32_t external_files;
    uint8_t read_only;
};

struct kernel_vfs_node {
    struct kernel_vfs_node *next;
    struct kernel_page_cache_entry *cache_pages;
    struct kernel_file_mapping *mappings;
    struct kernel_record_lock_state record_locks;
    struct kernel_vfs_instance *instance;
    struct kernel_vfs_mount *mount;
    void *backend_data;
    uint64_t inode;
    uint64_t max_size;
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
    uint8_t retired;
};

struct kernel_vfs_path {
    struct kernel_vfs_file file;
    struct kernel_heap *heap;
    struct kernel_vfs_path *parent;
    struct kernel_vfs_path *next;
    struct kernel_vfs_path **previous;
    struct kernel_vfs_mount *mounted_here;
    uint32_t references;
    uint8_t detached;
    char *name;
    char initial_name[];
};

struct kernel_vfs_rename_result {
    uint64_t replaced_inode;
    int replaced_last_link, changed;
};

struct kernel_open_file_description;

/* 回调返回负 errno；close_node 只释放私有 handle，不等待设备。
 * 后端分配的 node 必须位于同一堆对象首部，发布后由通用层回收。 */
struct kernel_vfs_backend {
    int (*error)(struct kernel_vfs_instance *instance);
    int (*root)(struct kernel_vfs_instance *instance, uint64_t *inode, uint32_t *mode);
    int (*lookup)(struct kernel_vfs_instance *instance, uint64_t parent,
        const char *name, size_t length, uint64_t *inode, uint32_t *mode);
    int (*readlink)(struct kernel_vfs_node *node, char *buffer, size_t size, size_t *count);
    /* Object links resolve to a pinned path without a pathname relookup. */
    int (*follow_link)(struct kernel_vfs_node *node,
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
        char **buffer, size_t *length);
    int (*truncate)(struct kernel_vfs_node *node, uint64_t size, uint64_t *actual, int *changed);
    int (*close_node)(struct kernel_vfs_node *node);
    int (*writeback_allowed)(struct kernel_vfs_instance *instance);
    int (*sync_metadata)(struct kernel_vfs_node *node);
    int (*flush)(struct kernel_vfs_instance *instance);
    int (*symlink)(struct kernel_vfs_instance *instance, const char *target, const char *path);
    int (*mknod)(struct kernel_vfs_instance *instance, const char *path,
        uint32_t type, uint32_t mode, uint32_t device);
    int (*rename)(struct kernel_vfs_instance *instance,
        uint64_t old_parent, const char *old_name, uint64_t new_parent,
        const char *new_name, unsigned flags, struct kernel_vfs_rename_result *result);
    int (*unmount)(struct kernel_vfs_mount *mount);
    int (*statfs)(struct kernel_vfs_mount *mount,
        struct kernel_vfs_statfs *stat);
    int (*set_times)(struct kernel_vfs_file *file,
        const struct kernel_vfs_timespec times[2]);
    int (*set_mode)(struct kernel_vfs_file *file, uint32_t mode);
    int (*open)(struct kernel_vfs_mount *mount,
        const char *path, uint64_t inode_number,
        uint32_t inode_mode,
        struct kernel_vfs_file *file);
    int (*create)(struct kernel_vfs_mount *mount,
        const char *path,
        uint32_t mode,
        struct kernel_vfs_file *file);
    void (*accessed)(struct kernel_vfs_file *file);
    int (*modified)(struct kernel_vfs_file *file,
        uint64_t offset, int append);
    int (*stat)(const struct kernel_vfs_file *file,
        struct kernel_vfs_stat *stat);
    int (*mkdir)(struct kernel_vfs_mount *mount,
        const char *path,
        uint32_t mode);
    int (*unlink)(struct kernel_vfs_mount *mount,
        const char *path);
    int (*rmdir)(struct kernel_vfs_mount *mount,
        const char *path);
    void (*release_unlinked)(struct kernel_vfs_node *node);
    int (*dir_entry)(struct kernel_vfs_file *file,
        uint64_t position,
        uint64_t *next_position,
        uint64_t *inode,
        uint8_t *type,
        char *name,
        size_t name_size);
    int (*pread)(struct kernel_vfs_node *node,
        uint64_t offset,
        void *buffer,
        size_t size,
        size_t *bytes_read);
    int (*writeback)(struct kernel_vfs_node *node, uint64_t offset,
        const void *buffer, size_t size, size_t *written);
};
void kernel_vfs_record_writeback_error(struct kernel_vfs_node *node, int error);
int kernel_vfs_publish_node(struct kernel_vfs_mount *mount, struct kernel_vfs_node *node,
    struct kernel_vfs_file *file, int creating);
#endif
