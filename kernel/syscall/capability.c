#include "private.h"

#include <kernel/errno.h>
#include <kernel/task.h>
#include <kernel/task_state.h>
#include <kernel/credentials.h>
#include <kernel/uaccess.h>

#include <stddef.h>
#include <stdint.h>

#define LINUX_CAPABILITY_VERSION_1 UINT32_C(0x19980330)
#define LINUX_CAPABILITY_VERSION_2 UINT32_C(0x20071026)
#define LINUX_CAPABILITY_VERSION_3 UINT32_C(0x20080522)
/* references/linux/include/uapi/linux/capability.h: CAP_CHECKPOINT_RESTORE. */
#define LINUX_CAP_LAST_CAP 40U

struct linux_cap_data { uint32_t effective, permitted, inheritable; };
_Static_assert(sizeof(struct linux_cap_data) == 12U, "Linux capability layout");

static void capability_target_end(struct kernel_task **task)
{
    if (*task) kernel_task_lookup_put(*task);
}

enum kernel_syscall_status syscall_handle_capget(struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    struct kernel_mm *mm;
    uint64_t header = request->arguments[0], data = request->arguments[1];
    uint32_t version;
    int32_t pid;
    size_t copied = 0U;
    unsigned words;

    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = -KERNEL_EFAULT;
    if (kernel_task_mm_borrow_mutable(caller, &mm) != KERNEL_TASK_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    if (kernel_copy_from_user(mm, &version, header, sizeof(version), &copied) !=
        KERNEL_UACCESS_STATUS_OK || copied != sizeof(version))
        return KERNEL_SYSCALL_STATUS_OK;
    switch (version) {
    case LINUX_CAPABILITY_VERSION_1:
        words = 1U;
        break;
    case LINUX_CAPABILITY_VERSION_2:
    case LINUX_CAPABILITY_VERSION_3:
        words = 2U;
        break;
    default:
        version = LINUX_CAPABILITY_VERSION_3;
        if (kernel_copy_to_user(mm, header, &version, sizeof(version), &copied) !=
            KERNEL_UACCESS_STATUS_OK || copied != sizeof(version))
            return KERNEL_SYSCALL_STATUS_OK;
        /* Linux 允许只探测版本；这条路径只访问 version，不读取 pid。 */
        decoded->value = data ? -KERNEL_EINVAL : 0;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (!data) {
        decoded->value = 0;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (kernel_copy_from_user(mm, &pid, header + sizeof(version), sizeof(pid),
                              &copied) != KERNEL_UACCESS_STATUS_OK ||
        copied != sizeof(pid))
        return KERNEL_SYSCALL_STATUS_OK;
    if (pid < 0) {
        decoded->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    struct kernel_task *target __attribute__((cleanup(capability_target_end))) =
        pid ? kernel_task_find_by_tid(pid) : 0;
    if (pid && !target) {
        decoded->value = -KERNEL_ESRCH;
        return KERNEL_SYSCALL_STATUS_OK;
    }

    const struct kernel_credentials *cred=kernel_credentials_snapshot(target?target:caller);
    const struct linux_cap_data output[2] = {
        {(uint32_t)cred->effective, (uint32_t)cred->permitted, (uint32_t)cred->inheritable},
        {(uint32_t)(cred->effective>>32), (uint32_t)(cred->permitted>>32), (uint32_t)(cred->inheritable>>32)},
    };
    kernel_credentials_put(cred);
    size_t length = words * sizeof(output[0]);
    if (kernel_copy_to_user(mm, data, output, length, &copied) !=
        KERNEL_UACCESS_STATUS_OK || copied != length)
        return KERNEL_SYSCALL_STATUS_OK;
    decoded->value = 0;
    return KERNEL_SYSCALL_STATUS_OK;
}


enum kernel_syscall_status syscall_handle_capset(struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    struct kernel_mm *mm;
    uint64_t header=request->arguments[0],data=request->arguments[1];
    uint32_t version; int32_t pid; unsigned words; size_t copied=0;
    struct linux_cap_data input[2]={{0}};
    decoded->action=KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value=-KERNEL_EFAULT;
    if(kernel_task_mm_borrow_mutable(caller,&mm)!=KERNEL_TASK_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    if(kernel_copy_from_user(mm,&version,header,sizeof(version),&copied)!=KERNEL_UACCESS_STATUS_OK||copied!=sizeof(version))
        return KERNEL_SYSCALL_STATUS_OK;
    switch(version){
    case LINUX_CAPABILITY_VERSION_1:words=1;break;
    case LINUX_CAPABILITY_VERSION_2:
    case LINUX_CAPABILITY_VERSION_3:words=2;break;
    default:
        version=LINUX_CAPABILITY_VERSION_3;
        if(kernel_copy_to_user(mm,header,&version,sizeof(version),&copied)==KERNEL_UACCESS_STATUS_OK&&copied==sizeof(version))
            decoded->value=-KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if(kernel_copy_from_user(mm,&pid,header+4,sizeof(pid),&copied)!=KERNEL_UACCESS_STATUS_OK||copied!=sizeof(pid))
        return KERNEL_SYSCALL_STATUS_OK;
    if(pid&&pid!=caller->tid){decoded->value=-KERNEL_EPERM;return KERNEL_SYSCALL_STATUS_OK;}
    size_t bytes=words*sizeof(input[0]);
    if(kernel_copy_from_user(mm,input,data,bytes,&copied)!=KERNEL_UACCESS_STATUS_OK||copied!=bytes)
        return KERNEL_SYSCALL_STATUS_OK;
    uint64_t effective=((uint64_t)input[0].effective|((uint64_t)input[1].effective<<32))&KERNEL_CAP_VALID_MASK;
    uint64_t permitted=((uint64_t)input[0].permitted|((uint64_t)input[1].permitted<<32))&KERNEL_CAP_VALID_MASK;
    uint64_t inheritable=((uint64_t)input[0].inheritable|((uint64_t)input[1].inheritable<<32))&KERNEL_CAP_VALID_MASK;
    const struct kernel_credentials *old=kernel_credentials_task_locked(caller);
    struct kernel_credentials *candidate=kernel_credentials_clone(caller->files.heap,old);
    if(!candidate){decoded->value=-KERNEL_ENOMEM;return KERNEL_SYSCALL_STATUS_OK;}
    decoded->value=-KERNEL_EPERM;
    if((!kernel_credentials_capable(old,KERNEL_CAP_SETPCAP)&&(inheritable&~(old->inheritable|old->permitted)))||
       (inheritable&~(old->inheritable|old->bounding))||
       (permitted&~old->permitted)||(effective&~permitted)){
        kernel_credentials_put(candidate);return KERNEL_SYSCALL_STATUS_OK;
    }
    candidate->effective=effective;candidate->permitted=permitted;candidate->inheritable=inheritable;
    candidate->ambient&=permitted&inheritable;
    kernel_credentials_publish(caller,candidate);
    decoded->value=0;return KERNEL_SYSCALL_STATUS_OK;
}
