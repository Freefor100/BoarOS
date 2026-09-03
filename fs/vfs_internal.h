#ifndef BOAROS_FS_VFS_INTERNAL_H
#define BOAROS_FS_VFS_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

struct kernel_page_cache;
struct kernel_vfs_file;
struct kernel_vfs_mount;
struct kernel_vfs_node;

struct kernel_vfs_node *kernel_vfs_file_node(
    const struct kernel_vfs_file *file);

int kernel_vfs_node_acquire(struct kernel_vfs_node *node);
int kernel_vfs_node_release(struct kernel_vfs_node **owner);

int kernel_vfs_node_pread(struct kernel_vfs_node *node,
                          uint64_t offset,
                          void *buffer,
                          size_t size,
                          size_t *bytes_read);

uint64_t kernel_vfs_node_size(const struct kernel_vfs_node *node);
const struct kernel_vfs_mount *kernel_vfs_node_mount(
    const struct kernel_vfs_node *node);

struct kernel_page_cache *kernel_vfs_file_page_cache(
    const struct kernel_vfs_file *file);

#endif
