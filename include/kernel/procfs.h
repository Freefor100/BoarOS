#ifndef BOAROS_KERNEL_PROCFS_H
#define BOAROS_KERNEL_PROCFS_H

#include <stdint.h>

struct kernel_heap;
struct kernel_vfs_mount;
struct kernel_vfs_file;
struct kernel_pid;

/* The caller owns an unattached mount until attach succeeds. */
struct kernel_pid_registry;
int kernel_procfs_create_in(struct kernel_heap *, uint64_t,
    struct kernel_pid_registry *, struct kernel_vfs_mount **);
int kernel_procfs_create(struct kernel_heap *heap, uint64_t flags,
                         struct kernel_vfs_mount **owner);
int kernel_procfs_is_mount(const struct kernel_vfs_mount *mount);
/* Only these generated files retain the opener's IPC namespace in their OFD. */
int kernel_procfs_ipc_file(const struct kernel_vfs_file *file);
int kernel_procfs_kpageflags_file(const struct kernel_vfs_file *file);
struct kernel_mm_pagemap;
int kernel_procfs_pagemap_open(const struct kernel_vfs_file *, struct kernel_heap *,
    struct kernel_mm_pagemap **);
/* Borrowed while the actual opened process-directory file remains owned. */
struct kernel_pid *kernel_procfs_process_identity(const struct kernel_vfs_file *file);
/* Called after all users have exited, before the root ext4 unmount. */
int kernel_procfs_unmount_children(struct kernel_vfs_mount *root);

#endif
