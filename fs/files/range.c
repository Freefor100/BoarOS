#include "private.h"
#include "../open_file_internal.h"
#include "../vfs_internal.h"
#include <kernel/cpu.h>
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/page_cache.h>
#include <kernel/scheduler.h>
#include <kernel/task.h>
#include <arch/context.h>
#include <arch/platform_io.h>

struct fallocate_call {
    const struct kernel_credentials *credentials;
    struct kernel_vfs_file *file;
    const struct kernel_lock_guard *operation;
    struct kernel_vfs_size_limit *size_limit;
    uint64_t offset, length;
    unsigned mode;
    int result;
};
static void fallocate_cpu0(void *argument)
{
    struct fallocate_call *call = argument;
    const struct kernel_credentials *previous=kernel_credentials_override(call->credentials);
    call->result = kernel_vfs_fallocate_borrowing(call->file,call->mode,
        call->offset,call->length,call->operation,call->size_limit);
    (void)kernel_credentials_override(previous);
}

enum kernel_files_status kernel_files_fallocate(struct kernel_files *files,
    int64_t fd, unsigned mode, int64_t offset, int64_t length, int64_t *linux_result)
{
    if (!kernel_files_is_live(files) || !linux_result) return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    KERNEL_FILES_PIN_SCOPE(pin);
    struct kernel_open_file_description *file = kernel_files_hold_fd(files,fd,&pin);
    struct kernel_vfs_size_limit limit={.maximum=kernel_task_current_fsize_limit()};
    *linux_result = -KERNEL_EBADF;
    if (!file) return KERNEL_FILES_STATUS_OK;
    if (offset < 0 || length <= 0) {
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    if (mode != 0 && mode != KERNEL_VFS_FALLOC_KEEP_SIZE &&
        mode != (KERNEL_VFS_FALLOC_KEEP_SIZE | KERNEL_VFS_FALLOC_PUNCH_HOLE)) {
        *linux_result = -KERNEL_EOPNOTSUPP;
        return KERNEL_FILES_STATUS_OK;
    }
    if (file->kind == KERNEL_OPEN_FILE_KIND_PATH || (file->open_flags & 3U) == 0)
        return KERNEL_FILES_STATUS_OK;
    if (file->kind == KERNEL_OPEN_FILE_KIND_PIPE) *linux_result = -KERNEL_ESPIPE;
    else if (file->kind == KERNEL_OPEN_FILE_KIND_DIRECTORY) *linux_result = -KERNEL_EISDIR;
    else if (file->kind == KERNEL_OPEN_FILE_KIND_BLOCK) *linux_result = -KERNEL_EOPNOTSUPP;
    else if (file->kind != KERNEL_OPEN_FILE_KIND_REGULAR) *linux_result = -KERNEL_ENODEV;
    else if (length > INT64_MAX - offset) *linux_result = -KERNEL_EFBIG;
    else if (kernel_vfs_file_metadata_requires_cpu0(&file->file)) {
        struct kernel_io_context *io = kernel_io_context_current();
        if (io->locks || io->user_pins || io->cache_writes || kernel_cpu_current()->raw_locks ||
            !kernel_scheduler_can_sleep() || arch_external_interrupt_active()) return KERNEL_FILES_STATUS_STATE;
        KERNEL_LOCK_SCOPE(operation);
        kernel_vfs_file_write_lock(&file->file,&operation);
        struct fallocate_call call = {.credentials=kernel_credentials_current(),.file=&file->file,.operation=&operation,
            .offset=(uint64_t)offset,.length=(uint64_t)length,.mode=mode,.result=-KERNEL_EIO,.size_limit=&limit};
        if (kernel_page_cache_run_cpu0_borrowing(kernel_open_file_page_cache(file),
                fallocate_cpu0,&call,&operation)) return KERNEL_FILES_STATUS_STATE;
        *linux_result = call.result;
    } else *linux_result = kernel_vfs_fallocate_borrowing(&file->file,mode,(uint64_t)offset,(uint64_t)length,NULL,&limit);
    kernel_files_size_limit_notify(&limit);
    /* 范围事务接纳后先归还写操作资格，再等待本 OFD 的完整持久目标。
     * O_DSYNC 也可能修改分配和 EOF；与 O_SYNC 共用完整文件同步。 */
    if (!*linux_result && (file->open_flags & KERNEL_FILES_O_DSYNC))
        *linux_result = kernel_vfs_sync(&file->file,0,&file->observed_writeback_error);
    return KERNEL_FILES_STATUS_OK;
}
