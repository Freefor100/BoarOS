#ifndef BOAROS_FS_PTY_INTERNAL_H
#define BOAROS_FS_PTY_INTERNAL_H

#include <stdint.h>

struct kernel_vfs_mount;
struct kernel_vfs_file;
struct kernel_heap;
struct kernel_task;
struct kernel_char_device;

/* devpts挂载拥有一个可join worker；仍有配对时不能拆除owner。 */
int kernel_pty_mount_start(struct kernel_vfs_mount *mount);
int kernel_pty_mount_stop(struct kernel_vfs_mount *mount);

/* 只认devpts节点的绑定身份；调用者在整个open期间持有file。 */
int kernel_pty_open(const struct kernel_vfs_file *file, struct kernel_heap *heap,
    struct kernel_task *caller, uint32_t flags, void **instance,
    const struct kernel_char_device **device);
/* 普通5:2节点由文件层先解析同挂载命名空间中的关联devpts实例。 */
int kernel_pty_open_ptmx(struct kernel_vfs_mount *mount, struct kernel_heap *heap,
    struct kernel_task *caller, uint32_t flags, void **instance,
    const struct kernel_char_device **device);

#endif
