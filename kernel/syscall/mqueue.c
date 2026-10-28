#include "private.h"
#include <kernel/mqueue.h>
#include <kernel/task.h>
#include <kernel/signal.h>
#include <kernel/errno.h>
enum kernel_syscall_status syscall_handle_mqueue(struct kernel_task *caller,
    const struct kernel_syscall_request *request,struct kernel_syscall_result *decoded)
{
    struct kernel_mm *mm;
    if(kernel_task_mm_borrow_mutable(caller,&mm))return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    const uint64_t *a=request->arguments;decoded->action=KERNEL_SYSCALL_ACTION_RETURN;
    switch(request->number){
    case 180:decoded->value=kernel_mqueue_open(caller,mm,a[0],(uint32_t)a[1],(uint32_t)a[2],a[3]);break;
    case 181:decoded->value=kernel_mqueue_unlink(caller,mm,a[0]);break;
    case 182:decoded->value=kernel_mqueue_send(caller,mm,(int64_t)a[0],a[1],a[2],(uint32_t)a[3],a[4]);break;
    case 183:decoded->value=kernel_mqueue_receive(caller,mm,(int64_t)a[0],a[1],a[2],a[3],a[4]);break;
    case 184:decoded->value=kernel_mqueue_notify(caller,mm,(int64_t)a[0],a[1]);break;
    case 185:decoded->value=kernel_mqueue_getsetattr(caller,mm,(int64_t)a[0],a[1],a[2]);break;
    default:return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    if(decoded->value==-KERNEL_ERESTARTSYS)kernel_signal_note_syscall_restart(caller);
    return KERNEL_SYSCALL_STATUS_OK;
}
