#ifndef BOAROS_KERNEL_FANOTIFY_H
#define BOAROS_KERNEL_FANOTIFY_H

#include <kernel/files.h>

struct kernel_fs_context;
struct kernel_fanotify_read;
void kernel_task_fanotify_read_set(struct kernel_task *, struct kernel_fanotify_read *);
void kernel_task_fanotify_read_clear(struct kernel_task *, struct kernel_fanotify_read *);
void kernel_fanotify_read_abort(struct kernel_fanotify_read *);
enum kernel_files_status kernel_files_fanotify(struct kernel_files *,
    const struct kernel_fs_context *, struct kernel_mm *, unsigned syscall,
    int32_t fd_or_flags, uint32_t flags, uint64_t mask, int32_t dirfd,
    uint64_t path, int64_t *result);

#endif
