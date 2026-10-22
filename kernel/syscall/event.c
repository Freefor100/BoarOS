#include "private.h"
#include <kernel/event_fd.h>
#include <kernel/errno.h>
#include <kernel/task.h>
enum kernel_syscall_status syscall_handle_eventfd(struct kernel_task *caller,
    const struct kernel_syscall_request *request,struct kernel_syscall_result *decoded)
{
    struct kernel_files *files;
    if (kernel_task_files_borrow(caller,&files)) return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    decoded->action=KERNEL_SYSCALL_ACTION_RETURN;
    return kernel_files_eventfd_create(files,(uint32_t)request->arguments[0],
        (uint32_t)request->arguments[1],&decoded->value)?KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT:KERNEL_SYSCALL_STATUS_OK;
}
