#ifndef BOAROS_KERNEL_VFS_H
#define BOAROS_KERNEL_VFS_H

#include <kernel/block.h>
#include <kernel/heap.h>
#include <kernel/read_source.h>

#include <stddef.h>
#include <stdint.h>

#define KERNEL_VFS_S_IFMT UINT32_C(0170000)
#define KERNEL_VFS_S_IFREG UINT32_C(0100000)
#define KERNEL_VFS_S_IFDIR UINT32_C(0040000)
#define KERNEL_VFS_S_IXUSR UINT32_C(0000100)
#define KERNEL_VFS_S_IXGRP UINT32_C(0000010)
#define KERNEL_VFS_S_IXOTH UINT32_C(0000001)

struct kernel_vfs_mount {
    void *private_data;
    uint32_t state;
};

struct kernel_page_cache;

struct kernel_vfs_file {
    void *private_data;
    struct kernel_vfs_mount *mount;
    uint64_t size;
    uint32_t mode;
    uint32_t state;
};

/* Returns zero or a negative Linux-compatible errno value. */
int kernel_vfs_mount_root_readonly(struct kernel_vfs_mount *mount,
                                   struct kernel_block_device *block,
                                   struct kernel_heap *heap,
                                   struct kernel_page_cache *page_cache);

int kernel_vfs_unmount(struct kernel_vfs_mount *mount);

int kernel_vfs_open(struct kernel_vfs_mount *mount,
                    const char *path,
                    struct kernel_vfs_file *file);

/* Rejects non-regular or non-executable files with EACCES. */
int kernel_vfs_open_executable(struct kernel_vfs_mount *mount,
                               const char *path,
                               struct kernel_vfs_file *file);

int kernel_vfs_pread(struct kernel_vfs_file *file,
                     uint64_t offset,
                     void *buffer,
                     size_t size,
                     size_t *bytes_read);

/* The file must remain open while the source is in use. */
int kernel_vfs_file_read_source(struct kernel_vfs_file *file,
                                struct kernel_read_source *source);

int kernel_vfs_close(struct kernel_vfs_file *file);

int kernel_vfs_files_share_node(const struct kernel_vfs_file *left,
                                const struct kernel_vfs_file *right);

#endif
