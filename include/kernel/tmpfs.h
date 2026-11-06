#ifndef BOAROS_KERNEL_TMPFS_H
#define BOAROS_KERNEL_TMPFS_H
#include <stdint.h>
struct kernel_heap;
struct kernel_vfs_mount;
struct kernel_vfs_file;
struct kernel_vfs_node;
struct physical_page_allocator;

#define KERNEL_MFD_CLOEXEC 1U
#define KERNEL_MFD_ALLOW_SEALING 2U
#define KERNEL_MFD_HUGETLB 4U
#define KERNEL_MFD_NOEXEC_SEAL 8U
#define KERNEL_MFD_EXEC 16U
#define KERNEL_MEMFD_NAME_MAX 249U
#define KERNEL_SEAL_SEAL 1U
#define KERNEL_SEAL_SHRINK 2U
#define KERNEL_SEAL_GROW 4U
#define KERNEL_SEAL_WRITE 8U
#define KERNEL_SEAL_FUTURE_WRITE 16U
#define KERNEL_SEAL_EXEC 32U
#define KERNEL_SEAL_WRITES (KERNEL_SEAL_WRITE | KERNEL_SEAL_FUTURE_WRITE)
#define KERNEL_SEAL_ALL 63U

int kernel_tmpfs_memfd(struct kernel_heap *, const char *, uint32_t, struct kernel_vfs_file *);
int kernel_tmpfs_file_seals(struct kernel_vfs_file *, int add, uint32_t seals);

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
