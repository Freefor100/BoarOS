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
#if BOAROS_MAX_CPUS > 1
    struct kernel_raw_lock timestamp_lock;
#endif
    struct ext4_lock locks;
    struct kernel_thread_join journal_worker, checkpoint_worker;
    struct kernel_wait_queue journal_work, checkpoint_work, journal_progress;
    uint8_t journal_started, checkpoint_started;
    uint8_t journal_requested, checkpoint_requested, journal_force, journal_stopping;
    char device_name[24];
    char mount_point[28];
    char root_source[48];
    struct lwext4_mount_adapter *next_adapter;
    struct ext4_blockdev_iface interface;
    struct ext4_blockdev device;
    unsigned char *physical_buffer;
    struct ext4_sblock *superblock;
    struct lwext4_orphan *orphans;
    struct lwext4_node *dirty_times, **dirty_times_tail, *staged_times;
    uint64_t times_deadline;
    size_t dirty_times_count;

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
    struct ext4_timestamp times[3], staged[3];
    uint64_t time_version[3], staged_version[3];
    struct lwext4_node *dirty_next, **dirty_previous, *staged_next;
    unsigned dirty_fields, staged_fields, extra_times;
    uint8_t noatime;

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
