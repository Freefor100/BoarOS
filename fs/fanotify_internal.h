#ifndef BOAROS_FS_FANOTIFY_INTERNAL_H
#define BOAROS_FS_FANOTIFY_INTERNAL_H

#include <stdint.h>

struct kernel_files;
struct kernel_open_file_description;
struct kernel_vfs_file;
struct kernel_vfs_handle;
struct kernel_vfs_instance;
struct kernel_vfs_mount;
struct kernel_vfs_path;

struct kernel_fanotify_ops {
    void (*notify)(const struct kernel_vfs_file *, const struct kernel_vfs_path *,
                   uint64_t parent_inode, uint32_t mask);
    void (*retire_inode)(struct kernel_vfs_instance *, uint64_t);
    void (*retire_mount)(struct kernel_vfs_mount *);
    void (*retire_instance)(struct kernel_vfs_instance *);
};

int kernel_vfs_fanotify_encode_handle(const struct kernel_vfs_file *,
                                    struct kernel_vfs_handle *);
int kernel_vfs_fanotify_acquire_path(const struct kernel_vfs_path *,
                                    struct kernel_vfs_path **);
int kernel_vfs_fanotify_open(struct kernel_vfs_path *, struct kernel_vfs_file *,
                             uint8_t *vfs_closed);
int kernel_files_open_fanotify_event(struct kernel_files *, struct kernel_vfs_path *,
                                    unsigned flags, struct kernel_open_file_description **);

#endif
