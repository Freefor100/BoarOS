#include "private.h"
#include <kernel/msg.h>
#include <kernel/task.h>
enum kernel_syscall_status syscall_handle_msg(struct kernel_task *caller,
                                              const struct kernel_syscall_request *request,
                                              struct kernel_syscall_result *decoded)
{
    struct kernel_mm *mm;
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    if (kernel_task_mm_borrow_mutable(caller, &mm) != KERNEL_TASK_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    const uint64_t *a = request->arguments;
    switch (request->number) {
    case 186:
        decoded->value = kernel_msg_get(caller, (int32_t)a[0], (int32_t)a[1]);
        break;
    case 187:
        decoded->value = kernel_msg_ctl(caller, mm, (int32_t)a[0], (int32_t)a[1], a[2]);
        break;
    case 188:
        decoded->value =
            kernel_msg_receive(caller, mm, (int32_t)a[0], a[1], a[2], (int64_t)a[3], (int32_t)a[4]);
        break;
    case 189:
        decoded->value = kernel_msg_send(caller, mm, (int32_t)a[0], a[1], a[2], (int32_t)a[3]);
        break;
    default:
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    return KERNEL_SYSCALL_STATUS_OK;
}
