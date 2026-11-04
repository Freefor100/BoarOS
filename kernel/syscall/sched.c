#include <kernel/scheduler.h>
#include <kernel/cpu.h>
#include "private.h"
#include <kernel/errno.h>
#include <kernel/page.h>
#include <kernel/sched_policy.h>
#include <kernel/task.h>
#include <kernel/uaccess.h>
#include <kernel/tick.h>
#include <stdint.h>

struct linux_sched_attr {
    uint32_t size, policy;
    uint64_t flags;
    int32_t nice;
    uint32_t priority;
    uint64_t runtime, deadline, period;
    uint32_t util_min, util_max;
};
_Static_assert(sizeof(struct linux_sched_attr)==56,"native sched_attr size");
_Static_assert(offsetof(struct linux_sched_attr,util_min)==48,"sched_attr version 0 size");

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
    if (nr==168) {
        unsigned cpu=0,node=0;
#if BOAROS_MAX_CPUS > 1
        cpu=kernel_cpu_current()->logical_id;
#endif
        /* Linux尝试两份输出；CPU输出失败仍保留有效node输出。 */
        int error=0;
        if(request->arguments[0])error|=sched_copy(caller,request->arguments[0],&cpu,sizeof(cpu),1);
        if(request->arguments[1])error|=sched_copy(caller,request->arguments[1],&node,sizeof(node),1);
        decoded->value=error ? -KERNEL_EFAULT : 0;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (nr==125 || nr==126) {
        int policy=(int)request->arguments[0];
        decoded->value = policy==0 || policy==3 || policy==5 || policy==6 || policy==7 ? 0 :
            policy==KERNEL_SCHED_FIFO || policy==KERNEL_SCHED_RR ?
                (nr==125 ? 99 : 1) : -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (nr==118 || nr==119) {
        KERNEL_SERVICE_SCOPE(service);
        uint64_t address=request->arguments[nr==118 ? 1 : 2];
        int priority, policy=nr==118 ? 0 : (int)request->arguments[1];
        if (!address || pid<0 || (nr==119 && policy<0)) result=-KERNEL_EINVAL;
        else if ((result=sched_copy(caller,address,&priority,sizeof(priority),0))==0)
            result=kernel_task_sched_set(caller,pid,policy,priority,nr==118);
    } else if (nr==122) {
        KERNEL_SERVICE_SCOPE(service);
        unsigned length=(unsigned)request->arguments[1];
        uint64_t mask=0;
        if (length>sizeof(mask)) length=sizeof(mask);
        result=length ? sched_copy(caller,request->arguments[2],&mask,length,0) : 0;
        if (!result) result=kernel_task_affinity_set(caller,pid,mask);
    } else if (nr==123) {
        unsigned length=(unsigned)request->arguments[1];
        uint64_t mask=1;
        if (length<sizeof(mask) || length%sizeof(mask)) result=-KERNEL_EINVAL;
        else {
            result=kernel_task_affinity_get(caller,pid,&mask);
            if (!result) result=sched_copy(caller,request->arguments[2],&mask,sizeof(mask),1);
            if (!result) result=sizeof(mask);
        }
    } else if (nr==275) {
        uint64_t address=request->arguments[1];
        unsigned size=(unsigned)request->arguments[2];
        unsigned flags=(unsigned)request->arguments[3];
        if (!address || pid<0 || size<48 || size>BOAROS_PAGE_SIZE)
            result=-KERNEL_EINVAL;
        else result=kernel_task_sched_get(caller,pid,&state);
        if (!result && flags) result=-KERNEL_EINVAL;
        if (!result) {
            struct linux_sched_attr attr={
                .size=size<sizeof(attr) ? size : sizeof(attr),
                .policy=(uint32_t)state.policy,
                .flags=state.reset_on_fork ? 1 : 0,
                .priority=(state.policy==KERNEL_SCHED_FIFO || state.policy==KERNEL_SCHED_RR)
                    ? (uint32_t)state.priority : 0,
                .runtime=state.policy==KERNEL_SCHED_OTHER
                    ? UINT64_C(1000000000)/KERNEL_TICKS_PER_SECOND : 0,
            };
            /* Linux先清扩展尾部；尾部fault时不写已知字段。任务快照已释放身份锁。 */
            const uint64_t zeros[8]={0};
            for (size_t offset=sizeof(attr); offset<size && !result;) {
                size_t count=size-offset;
                if (count>sizeof(zeros)) count=sizeof(zeros);
                result=sched_copy(caller,address+offset,(void *)zeros,count,1);
                offset+=count;
            }
            if (!result) result=sched_copy(caller,address,&attr,attr.size,1);
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
