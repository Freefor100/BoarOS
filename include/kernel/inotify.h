#ifndef BOAROS_KERNEL_INOTIFY_H
#define BOAROS_KERNEL_INOTIFY_H
#include <kernel/files.h>

struct kernel_fs_context;
enum kernel_files_status kernel_files_inotify(struct kernel_files *,
    const struct kernel_fs_context *, struct kernel_mm *, unsigned syscall,
    int fd_or_flags, uint64_t path_or_watch, uint32_t mask, int64_t *);
#endif
