#include "private.h"
#include <kernel/errno.h>
#include <kernel/sched_policy.h>
#include <kernel/task.h>
#include <kernel/uaccess.h>
#include <kernel/tick.h>
#include <stdint.h>

static int sched_copy(struct kernel_task *caller, uint64_t address,
                      void *buffer, size_t size, int to_user)
{
    struct kernel_mm *mm;
    size_t copied=0;
    if (kernel_task_mm_borrow_mutable(caller,&mm)!=KERNEL_TASK_STATUS_OK)
        return -KERNEL_EFAULT;
    enum kernel_uaccess_status status = to_user
        ? kernel_copy_to_user(mm,address,buffer,size,&copied)
        : kernel_copy_from_user(mm,buffer,address,size,&copied);
    return status==KERNEL_UACCESS_STATUS_OK && copied==size ? 0 : -KERNEL_EFAULT;
}

enum kernel_syscall_status syscall_handle_sched(
    struct kernel_task *caller, const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    uint64_t nr=request->number;
    int32_t pid=(int32_t)request->arguments[0];
    struct kernel_sched_policy state;
    int result;
    decoded->action=KERNEL_SYSCALL_ACTION_RETURN;
    if (nr==125 || nr==126) {
        int policy=(int)request->arguments[0];
        decoded->value = policy==0 || policy==3 || policy==5 || policy==6 || policy==7 ? 0 :
            policy==KERNEL_SCHED_FIFO || policy==KERNEL_SCHED_RR ?
                (nr==125 ? 99 : 1) : -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (nr==118 || nr==119) {
        uint64_t address=request->arguments[nr==118 ? 1 : 2];
        int priority, policy=nr==118 ? 0 : (int)request->arguments[1];
        if (!address || pid<0 || (nr==119 && policy<0)) result=-KERNEL_EINVAL;
        else if ((result=sched_copy(caller,address,&priority,sizeof(priority),0))==0)
            result=kernel_task_sched_set(caller,pid,policy,priority,nr==118);
    } else if (nr==122) {
        unsigned length=(unsigned)request->arguments[1];
        uint64_t mask=0;
        if (length>sizeof(mask)) length=sizeof(mask);
        result=length ? sched_copy(caller,request->arguments[2],&mask,length,0) : 0;
        if (!result) result=pid<0 ? -KERNEL_ESRCH : kernel_task_sched_get(caller,pid,&state);
        if (!result && !(mask&1U)) result=-KERNEL_EINVAL;
        /* 唯一在线 CPU 为 0：接受掩码与在线集合相交后确实运行在 CPU0。 */
    } else if (nr==123) {
        unsigned length=(unsigned)request->arguments[1];
        uint64_t mask=1;
        if (length<sizeof(mask) || length%sizeof(mask)) result=-KERNEL_EINVAL;
        else {
            result=pid<0 ? -KERNEL_ESRCH : kernel_task_sched_get(caller,pid,&state);
            if (!result) result=sched_copy(caller,request->arguments[2],&mask,sizeof(mask),1);
            if (!result) result=sizeof(mask);
        }
    } else if (nr==120 || nr==121 || nr==127) {
        if (nr==121 && !request->arguments[1]) result=-KERNEL_EINVAL;
        else result=kernel_task_sched_get(caller,pid,&state);
        if (!result) {
            if (nr==120) result=state.policy |
                (state.reset_on_fork ? KERNEL_SCHED_RESET_ON_FORK : 0);
            else if (nr==121) result=sched_copy(caller,request->arguments[1],
                                               &state.priority,sizeof(state.priority),1);
            else {
                struct { int64_t sec,nsec; } interval={0,
                    state.policy==KERNEL_SCHED_FIFO ? 0 :
                    state.policy==KERNEL_SCHED_RR ? KERNEL_SCHED_RR_NS :
                        UINT64_C(1000000000)/KERNEL_TICKS_PER_SECOND};
                result=sched_copy(caller,request->arguments[1],&interval,sizeof(interval),1);
            }
        }
    } else result=-KERNEL_ENOSYS;
    decoded->value=result;
    return KERNEL_SYSCALL_STATUS_OK;
}
