#ifndef BOAROS_FS_DEVPTS_INTERNAL_H
#define BOAROS_FS_DEVPTS_INTERNAL_H

#include "vfs_objects.h"
#include <kernel/devpts.h>

struct devpts_mount;
struct kernel_devpts_entry {
    struct kernel_devpts_entry *next;
    struct devpts_mount *mount;
    void *binding;
    struct kernel_vfs_stat stat;
    uint32_t number, references;
    uint8_t published, reserved, embedded;
};

struct devpts_mount {
    struct kernel_vfs_mount mount;
    struct kernel_vfs_instance instance;
    struct kernel_devpts_entry root, ptmx;
    struct kernel_devpts_entry *entries;
    void *pty_private;
    uint64_t reserved_numbers, next_inode;
    uint32_t uid, gid, slave_mode, max;
    uint8_t worker_started;
};

/* PTY owns its worker/list; devpts owns the containing VFS mount lifetime. */
int kernel_pty_mount_start(struct kernel_vfs_mount *mount);
int kernel_pty_mount_stop(struct kernel_vfs_mount *mount);

#endif
