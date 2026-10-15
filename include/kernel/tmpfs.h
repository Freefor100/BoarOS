#ifndef BOAROS_KERNEL_TMPFS_H
#define BOAROS_KERNEL_TMPFS_H
#include <stdint.h>
struct kernel_heap;
struct kernel_vfs_mount;
struct kernel_vfs_file;
struct kernel_vfs_node;
struct physical_page_allocator;
/* inode/OFD owner covers the object and its immutable instance budget. */
int kernel_tmpfs_check_domain(const struct kernel_vfs_node *, const struct kernel_heap *,
    const struct physical_page_allocator *);
/* Pinned inode/OFD; short metadata protects timestamps, no allocation, I/O,
 * sleeping or upgrade of the fault's inode read guard. */
void kernel_tmpfs_memory_modified(struct kernel_vfs_file *file);
int kernel_tmpfs_is_mount(const struct kernel_vfs_mount *mount);
int kernel_tmpfs_create(struct kernel_heap *, uint64_t flags, const char *options,
                         struct kernel_vfs_mount **owner);
#endif
