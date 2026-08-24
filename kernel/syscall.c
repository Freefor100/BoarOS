#include <kernel/syscall.h>

#include <stdint.h>

#define LINUX_SYSCALL_EXIT 93U
#define LINUX_ERROR_NOT_IMPLEMENTED 38
#define LINUX_EXIT_STATUS_MASK UINT64_C(0xff)

enum kernel_syscall_status kernel_syscall_dispatch(
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *result)
{
    struct kernel_syscall_result decoded;

    if (request == 0 || result == 0) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }

    if (request->number == LINUX_SYSCALL_EXIT) {
        decoded.action = KERNEL_SYSCALL_ACTION_EXIT;
        decoded.value = (int64_t)(request->arguments[0] &
                                  LINUX_EXIT_STATUS_MASK);
    } else {
        decoded.action = KERNEL_SYSCALL_ACTION_RETURN;
        decoded.value = -LINUX_ERROR_NOT_IMPLEMENTED;
    }

    *result = decoded;
    return KERNEL_SYSCALL_STATUS_OK;
}
