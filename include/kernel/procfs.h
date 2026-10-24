#ifndef BOAROS_KERNEL_PROCFS_H
#define BOAROS_KERNEL_PROCFS_H

#include <stdint.h>

struct kernel_heap;
struct kernel_vfs_mount;
struct kernel_vfs_file;
struct kernel_pid;

/* The caller owns an unattached mount until attach succeeds. */
int kernel_procfs_create(struct kernel_heap *heap, uint64_t flags,
                         struct kernel_vfs_mount **owner);
int kernel_procfs_is_mount(const struct kernel_vfs_mount *mount);
/* Borrowed while the actual opened process-directory file remains owned. */
struct kernel_pid *kernel_procfs_process_identity(const struct kernel_vfs_file *file);
/* Called after all users have exited, before the root ext4 unmount. */
int kernel_procfs_unmount_children(struct kernel_vfs_mount *root);

#endif
