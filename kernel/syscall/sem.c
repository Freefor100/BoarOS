#include <kernel/scheduler.h>
#include <kernel/sem.h>
#include <kernel/task.h>
#include "private.h"

enum kernel_syscall_status syscall_handle_sem(struct kernel_task *caller,
    const struct kernel_syscall_request *request, struct kernel_syscall_result *decoded)
{
    struct kernel_mm *mm;
    decoded->action=KERNEL_SYSCALL_ACTION_RETURN;
    if (kernel_task_mm_borrow_mutable(caller,&mm)!=KERNEL_TASK_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    const uint64_t *arg=request->arguments;
    switch(request->number) {
    case 190:
        decoded->value=kernel_sem_get(caller,(int32_t)arg[0],(int32_t)arg[1],(int32_t)arg[2]);
        break;
    case 191:
        decoded->value=kernel_sem_ctl(caller,mm,(int32_t)arg[0],(int32_t)arg[1],(int32_t)arg[2],arg[3]);
        break;
    case 192:
    case 193:
        decoded->value=kernel_sem_op(caller,mm,(int32_t)arg[0],arg[1],(uint32_t)arg[2],request->number==192?arg[3]:0);
        break;
    default: return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    return KERNEL_SYSCALL_STATUS_OK;
}
