#include "private.h"

#include <kernel/errno.h>
#include <kernel/shm.h>
#include <kernel/task.h>

#include <stdint.h>

enum kernel_syscall_status syscall_handle_shmget(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    if (caller == 0 || request == 0 || decoded == 0) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    int32_t shmid = 0;
    int ret = kernel_shm_get(caller,
                             (int32_t)request->arguments[0],
                             request->arguments[1],
                             (int32_t)request->arguments[2],
                             &shmid);
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = ret < 0 ? (int64_t)ret : (int64_t)shmid;
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_shmat(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    if (caller == 0 || request == 0 || decoded == 0) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    uint64_t attached_addr = 0;
    int ret = kernel_shm_at(caller,
                            (int32_t)request->arguments[0],
                            request->arguments[1],
                            (int32_t)request->arguments[2],
                            &attached_addr);
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = ret < 0 ? (int64_t)ret : (int64_t)attached_addr;
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_shmdt(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    if (caller == 0 || request == 0 || decoded == 0) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    int ret = kernel_shm_dt(caller, request->arguments[0]);
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = (int64_t)ret;
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_shmctl(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    if (caller == 0 || request == 0 || decoded == 0) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    int64_t result_val = 0;
    int ret = kernel_shm_ctl(caller,
                             (int32_t)request->arguments[0],
                             (int32_t)request->arguments[1],
                             request->arguments[2],
                             &result_val);
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = ret < 0 ? (int64_t)ret : result_val;
    return KERNEL_SYSCALL_STATUS_OK;
}
