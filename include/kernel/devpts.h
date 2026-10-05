#ifndef BOAROS_KERNEL_DEVPTS_H
#define BOAROS_KERNEL_DEVPTS_H

#include <kernel/vfs.h>

#define KERNEL_DEVPTS_DEFAULT_MAX 32U
#define KERNEL_DEVPTS_LIMIT 64U

struct kernel_devpts_entry;

int kernel_devpts_create(struct kernel_heap *heap, uint64_t flags,
    const char *options, struct kernel_vfs_mount **owner);
int kernel_devpts_is_mount(const struct kernel_vfs_mount *mount);

/* publish owns one entry reference and reserves its number until retire.
 * Unpublish clears the weak pair binding; retained VFS identities and mutable
 * inode metadata remain valid, including after that number is reused. */
int kernel_devpts_publish(struct kernel_vfs_mount *mount, void *pair,
    struct kernel_devpts_entry **owner);
int kernel_devpts_entry_acquire(struct kernel_devpts_entry *entry);
void kernel_devpts_entry_release(struct kernel_devpts_entry **owner);
void kernel_devpts_unpublish(struct kernel_devpts_entry *entry);
void kernel_devpts_retire(struct kernel_devpts_entry *entry);
uint32_t kernel_devpts_entry_number(const struct kernel_devpts_entry *entry);
/* Caller keeps interrupts disabled until it has acquired the pair owner. */
void *kernel_devpts_entry_binding(const struct kernel_devpts_entry *entry);

/* Borrowed identities: the caller retains the live VFS file throughout use. */
struct kernel_vfs_mount *kernel_devpts_file_mount(
    const struct kernel_vfs_file *file);
struct kernel_devpts_entry *kernel_devpts_file_entry(
    const struct kernel_vfs_file *file);
int kernel_devpts_file_is_ptmx(const struct kernel_vfs_file *file);
int kernel_devpts_mount_root(struct kernel_vfs_mount *mount,
    struct kernel_vfs_path **owner);
void *kernel_devpts_mount_private(const struct kernel_vfs_mount *mount);
void kernel_devpts_mount_set_private(struct kernel_vfs_mount *mount,
    void *private_data);

#endif
