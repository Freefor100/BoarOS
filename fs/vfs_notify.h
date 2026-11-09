#ifndef BOAROS_VFS_NOTIFY_H
#define BOAROS_VFS_NOTIFY_H

#include <stdint.h>

struct kernel_vfs_instance;
struct kernel_vfs_file;

enum {
    VFS_NOTIFY_ACCESS = 1, VFS_NOTIFY_MODIFY = 2, VFS_NOTIFY_ATTRIB = 4,
    VFS_NOTIFY_CLOSE_WRITE = 8, VFS_NOTIFY_CLOSE_NOWRITE = 16, VFS_NOTIFY_OPEN = 32,
    VFS_NOTIFY_MOVED_FROM = 64, VFS_NOTIFY_MOVED_TO = 128, VFS_NOTIFY_CREATE = 256,
    VFS_NOTIFY_DELETE = 512, VFS_NOTIFY_DELETE_SELF = 1024, VFS_NOTIFY_MOVE_SELF = 2048,
    VFS_NOTIFY_UNMOUNT = 0x2000, VFS_NOTIFY_ISDIR = 0x40000000
};

struct kernel_vfs_notify_ops {
    void (*emit)(struct kernel_vfs_instance *, uint64_t, uint32_t, uint32_t,
                 const char *, int detached);
    void (*unmount)(struct kernel_vfs_instance *);
};

void kernel_vfs_notify_file(const struct kernel_vfs_file *, uint32_t);

#endif
