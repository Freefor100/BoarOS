#include "private.h"
#include <kernel/errno.h>
#include <kernel/open_file.h>
#include <kernel/task.h>
#include <kernel/vfs.h>

enum kernel_files_status kernel_files_fallocate(struct kernel_files *files,
    int64_t fd, unsigned mode, int64_t offset, int64_t length, int64_t *linux_result)
{
    if (!kernel_files_is_live(files) || !linux_result) return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    KERNEL_FILES_PIN_SCOPE(pin);
    struct kernel_open_file_description *file = kernel_files_hold_fd(files,fd,&pin);
    struct kernel_vfs_size_limit limit={.maximum=kernel_task_current_fsize_limit()};
    *linux_result = file ? kernel_open_file_fallocate(file,mode,offset,length,&limit) : -KERNEL_EBADF;
    kernel_files_size_limit_notify(&limit);
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_advise(struct kernel_files *files,int64_t fd,
    int64_t offset,int64_t length,int advice,int explicit_readahead,int64_t *result)
{
    if (!kernel_files_is_live(files) || !result) return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    KERNEL_FILES_PIN_SCOPE(pin);
    struct kernel_open_file_description *file=kernel_files_hold_fd(files,fd,&pin);
    *result=file?kernel_open_file_advise(file,offset,length,advice,explicit_readahead):-KERNEL_EBADF;
    return KERNEL_FILES_STATUS_OK;
}
