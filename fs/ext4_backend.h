#ifndef BOAROS_FS_EXT4_BACKEND_H
#define BOAROS_FS_EXT4_BACKEND_H

#include "vfs_objects.h"
#include <ext4.h>
#include <ext4_blockdev.h>

struct lwext4_orphan {
    struct lwext4_orphan *next;
    uint32_t inode;
    uint8_t orphan_freed;
};
struct lwext4_mount_adapter {
    struct kernel_vfs_instance instance;
    struct kernel_block_device *block;
    struct kernel_rwlock backend_lock;
    struct ext4_lock locks;
    struct kernel_thread_join journal_worker;
    struct kernel_wait_queue journal_work, journal_progress;
    uint8_t journal_started, journal_requested, journal_force, journal_stopping;
    char device_name[24];
    char mount_point[28];
    struct lwext4_mount_adapter *next_adapter;
    struct ext4_blockdev_iface interface;
    struct ext4_blockdev device;
    unsigned char *physical_buffer;
    struct ext4_sblock *superblock;
    struct lwext4_orphan *orphans;
    uint8_t block_claimed;
    uint8_t registered;
    uint8_t mounted;
    uint8_t heap_bound;
    uint8_t unmount_sync_pending;
    uint8_t recovery_pending;
    uint8_t unmount_prepared;
    int mount_error;
};
struct lwext4_node {
    struct kernel_vfs_node node;
    ext4_file file;
};
_Static_assert(offsetof(struct lwext4_mount_adapter, instance) == 0,
               "mount private_data begins with its VFS instance");
_Static_assert(offsetof(struct lwext4_node, node) == 0,
               "VFS owns the backend node allocation");

static inline ext4_file *lwext4_node_file(const struct kernel_vfs_node *node)
{ return (ext4_file *)node->backend_data; }
static inline struct lwext4_mount_adapter *lwext4_instance(const struct kernel_vfs_instance *instance)
{ return instance->backend_data; }
#endif
