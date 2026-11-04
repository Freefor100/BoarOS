#include "private.h"
#include "../open_file_internal.h"
#include "../flock.h"
#include <kernel/errno.h>
#include <kernel/signal.h>
#include <kernel/task_state.h>

void kernel_files_abort_flock(struct kernel_task *task)
{
    if (!task->flock_file) return;
    enum kernel_files_status status=kernel_files_release_uninstalled_description(
        &task->files,&task->flock_file);
    if (status!=KERNEL_FILES_STATUS_OK && status!=KERNEL_FILES_STATUS_CLEANUP_REQUIRED)
        __builtin_trap();
}

enum kernel_files_status kernel_files_flock(struct kernel_files *files,
    struct kernel_task *task,int64_t fd,unsigned operation,int64_t *linux_result)
{
    if (!kernel_files_is_live(files) || !task || !linux_result)
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    /* Linux still accepts the obsolete LOCK_MAND bit as a no-op. */
    if (operation&32U) { *linux_result=0;return KERNEL_FILES_STATUS_OK; }
    unsigned type=operation&~4U;
    if (type!=1 && type!=2 && type!=8) {
        *linux_result=-KERNEL_EINVAL;return KERNEL_FILES_STATUS_OK;
    }
    if (task->flock_file || files!=&task->files) __builtin_trap();
    enum kernel_files_status status=kernel_files_pin_data(files,fd,&task->flock_file,linux_result);
    if (status || !task->flock_file) return status;
    struct kernel_open_file_description *file=task->flock_file;
    *linux_result=kernel_flock_apply(kernel_open_file_flock_identity(file),
        &file->flock_owner,file->heap,operation);
    if (*linux_result==-KERNEL_ERESTARTSYS) kernel_signal_note_syscall_restart(task);
    kernel_files_abort_flock(task);
    return KERNEL_FILES_STATUS_OK;
}
