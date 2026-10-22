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

enum kernel_syscall_status syscall_handle_timerfd(struct kernel_task *caller,
    const struct kernel_syscall_request *request,struct kernel_syscall_result *decoded)
{
    struct kernel_files *files;struct kernel_mm *mm;
    if (kernel_task_files_borrow(caller,&files)||kernel_task_mm_borrow_mutable(caller,&mm))
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    enum kernel_files_status status;
    if (request->number==85) status=kernel_files_timerfd_create(files,(int32_t)request->arguments[0],(uint32_t)request->arguments[1],&decoded->value);
    else if (request->number==86) status=kernel_files_timerfd_settime(files,mm,(int32_t)request->arguments[0],(uint32_t)request->arguments[1],request->arguments[2],request->arguments[3],&decoded->value);
    else status=kernel_files_timerfd_gettime(files,mm,(int32_t)request->arguments[0],request->arguments[1],&decoded->value);
    decoded->action=KERNEL_SYSCALL_ACTION_RETURN;
    return status?KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT:KERNEL_SYSCALL_STATUS_OK;
}
