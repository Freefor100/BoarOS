#ifndef BOAROS_KERNEL_TMPFS_H
#define BOAROS_KERNEL_TMPFS_H
#include <stdint.h>
struct kernel_heap;
struct kernel_vfs_mount;
struct kernel_vfs_file;
/* Pinned memory inode, local IRQs disabled: no allocation, I/O or sleeping. */
void kernel_tmpfs_memory_modified(struct kernel_vfs_file *file);
int kernel_tmpfs_is_mount(const struct kernel_vfs_mount *mount);
int kernel_tmpfs_create(struct kernel_heap *, uint64_t flags, const char *options,
                         struct kernel_vfs_mount **owner);
#endif
