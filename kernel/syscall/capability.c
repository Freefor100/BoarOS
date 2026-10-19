#include <kernel/scheduler.h>
#include "private.h"

#include <kernel/errno.h>
#include <kernel/task.h>
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
    KERNEL_SERVICE_SCOPE(service);
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

    /* 当前凭据固定为 root；目标借用保持查询期间的 TID 对象存活。
     * capability 表示权限，具体 syscall/设备能力由对应接口报告。 */
    const uint64_t valid = (UINT64_C(1) << (LINUX_CAP_LAST_CAP + 1U)) - 1U;
    const struct linux_cap_data output[2] = {
        {(uint32_t)valid, (uint32_t)valid, 0U},
        {(uint32_t)(valid >> 32), (uint32_t)(valid >> 32), 0U},
    };
    size_t length = words * sizeof(output[0]);
    if (kernel_copy_to_user(mm, data, output, length, &copied) !=
        KERNEL_UACCESS_STATUS_OK || copied != length)
        return KERNEL_SYSCALL_STATUS_OK;
    decoded->value = 0;
    return KERNEL_SYSCALL_STATUS_OK;
}
