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
#define KERNEL_VFS_S_IFLNK UINT32_C(0120000)
#define KERNEL_VFS_S_IFCHR UINT32_C(0020000)
#define KERNEL_VFS_S_IFIFO UINT32_C(0010000)
#define KERNEL_VFS_S_IXUSR UINT32_C(0000100)
#define KERNEL_VFS_S_IXGRP UINT32_C(0000010)
#define KERNEL_VFS_S_IXOTH UINT32_C(0000001)

struct kernel_vfs_mount {
    void *private_data;
    uint64_t id;
    uint32_t state;
};

struct kernel_vfs_timespec {
    int64_t seconds;
    int64_t nanoseconds;
};

struct kernel_vfs_stat {
    uint64_t dev;
    uint64_t ino;
    uint32_t mode;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint64_t rdev;
    uint64_t size;
    uint64_t blocks;
    uint64_t blksize;
    struct kernel_vfs_timespec atime;
    struct kernel_vfs_timespec mtime;
    struct kernel_vfs_timespec ctime;
};

struct kernel_page_cache;
struct kernel_vfs_path;

/* An owned path identity; root and cwd may name the same object while each
 * holding an independent reference. */
int kernel_vfs_path_root(struct kernel_vfs_mount *mount,
                         struct kernel_heap *heap,
                         struct kernel_vfs_path **owner);
int kernel_vfs_path_acquire(struct kernel_vfs_path *path);
int kernel_vfs_path_lookup(struct kernel_vfs_path *parent,
                           const char *name, size_t name_length,
                           struct kernel_vfs_path **owner);
int kernel_vfs_path_resolve(struct kernel_vfs_path *start,
                            struct kernel_vfs_path *root,
                            const char *path, int follow_final,
                            struct kernel_vfs_path **owner);
uint64_t kernel_vfs_path_inode(const struct kernel_vfs_path *path);
int kernel_vfs_path_stat(const struct kernel_vfs_path *path,
                         struct kernel_vfs_stat *stat);
int kernel_vfs_path_release(struct kernel_vfs_path **owner);
struct kernel_vfs_mount *kernel_vfs_path_mount(
    const struct kernel_vfs_path *path);

struct kernel_vfs_file {
    void *private_data;
    struct kernel_vfs_mount *mount;
    uint64_t size;
    uint32_t mode;
    uint32_t state;
    uint32_t write_lease;
    uint32_t exec_lease;
};

/* Returns zero or a negative Linux-compatible errno value. */
int kernel_vfs_mount_root(struct kernel_vfs_mount *mount,
                          struct kernel_block_device *block,
                          struct kernel_heap *heap,
                          struct kernel_page_cache *page_cache);

int kernel_vfs_unmount(struct kernel_vfs_mount *mount);

int kernel_vfs_mount_is_readonly(const struct kernel_vfs_mount *mount);

int kernel_vfs_open(struct kernel_vfs_mount *mount,
                    const char *path,
                    struct kernel_vfs_file *file);

int kernel_vfs_open_nofollow(struct kernel_vfs_mount *mount,
                             const char *path,
                             struct kernel_vfs_file *file);

int kernel_vfs_create(struct kernel_vfs_mount *mount,
                      const char *path,
                      uint32_t mode,
                      struct kernel_vfs_file *file);

/* Rejects non-regular or non-executable files with EACCES, or running executables with ETXTBSY. */
int kernel_vfs_open_executable(struct kernel_vfs_mount *mount,
                               const char *path,
                               struct kernel_vfs_file *file);

int kernel_vfs_file_acquire_write(struct kernel_vfs_file *file);

/* Request-boundary touches include cache hits, EOF and partial user faults.
 * Access-time I/O errors do not override the read result; modification errors
 * are returned before copying caller data. Neither API depends on a path. */
void kernel_vfs_file_accessed(struct kernel_vfs_file *file);
int kernel_vfs_file_modified(struct kernel_vfs_file *file,
                              uint64_t offset, int append);

int kernel_vfs_pread(struct kernel_vfs_file *file,
                     uint64_t offset,
                     void *buffer,
                     size_t size,
                     size_t *bytes_read);

int kernel_vfs_pwrite(struct kernel_vfs_file *file,
                      uint64_t offset,
                      const void *buffer,
                      size_t size,
                      size_t *bytes_written);

int kernel_vfs_append(struct kernel_vfs_file *file,
                      const void *buffer,
                      size_t size,
                      uint64_t *written_offset,
                      size_t *bytes_written);

int kernel_vfs_ftruncate(struct kernel_vfs_file *file,
                         uint64_t size);

/* Each OFD owns an error observation cursor; dup/fork share it with the OFD. */
uint64_t kernel_vfs_error_sequence(const struct kernel_vfs_file *file);
int kernel_vfs_sync(struct kernel_vfs_file *file, int datasync,
                    uint64_t *observed_error);

int kernel_vfs_mkdir(struct kernel_vfs_mount *mount,
                     const char *path,
                     uint32_t mode);

int kernel_vfs_unlink(struct kernel_vfs_mount *mount,
                      const char *path);

int kernel_vfs_rmdir(struct kernel_vfs_mount *mount,
                     const char *path);

int kernel_vfs_symlink(struct kernel_vfs_mount *mount,
                       const char *target,
                       const char *path);

int kernel_vfs_readlink(struct kernel_vfs_mount *mount,
                        const char *path,
                        char *buffer,
                        size_t size,
                        size_t *bytes_read);

/* The file must remain open while the source is in use. */
int kernel_vfs_file_read_source(struct kernel_vfs_file *file,
                                struct kernel_read_source *source);

int kernel_vfs_close(struct kernel_vfs_file *file);

/* Reads live inode metadata into a filesystem-independent representation. */
int kernel_vfs_fstat(const struct kernel_vfs_file *file,
                     struct kernel_vfs_stat *stat);

int kernel_vfs_stat_path(struct kernel_vfs_mount *mount,
                         const char *path,
                         int follow_final,
                         struct kernel_vfs_stat *stat);

/* Underlying ext4 inode number; zero when unavailable. */
uint32_t kernel_vfs_file_inode(const struct kernel_vfs_file *file);

/* Returns the current file size in bytes from the live VFS node. */
uint64_t kernel_vfs_file_size(const struct kernel_vfs_file *file);

/* Linux d_type values for directory entries. */
#define KERNEL_VFS_DT_UNKNOWN UINT8_C(0)
#define KERNEL_VFS_DT_FIFO UINT8_C(1)
#define KERNEL_VFS_DT_CHR UINT8_C(2)
#define KERNEL_VFS_DT_DIR UINT8_C(4)
#define KERNEL_VFS_DT_BLK UINT8_C(6)
#define KERNEL_VFS_DT_REG UINT8_C(8)
#define KERNEL_VFS_DT_LNK UINT8_C(7)
#define KERNEL_VFS_DT_SOCK UINT8_C(12)

/*
 * Report the first entry at or after the opaque byte `position` of an open
 * directory.  `next_position` receives the cookie to use for the following
 * entry.  Return 1 with fields filled, 0 at end of directory, or a negative
 * errno.  The position is a backend cookie, not an entry ordinal.
 */
int kernel_vfs_dir_entry(struct kernel_vfs_file *file,
                         uint64_t position,
                         uint64_t *next_position,
                         uint64_t *inode,
                         uint8_t *type,
                         char *name,
                         size_t name_size);

int kernel_vfs_files_share_node(const struct kernel_vfs_file *left,
                                const struct kernel_vfs_file *right);

#endif
