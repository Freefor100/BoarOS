#include "private.h"

#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/task.h>
#include <kernel/uaccess.h>
#include <kernel/uts.h>
#include <kernel/personality.h>
#include <kernel/task_state.h>
#include <string.h>

static void project_legacy_release(char release[KERNEL_UTS_FIELD_SIZE])
{
    const char *cursor = release;
    unsigned major = 0, minor = 0, dots = 0;
    while (*cursor >= '0' && *cursor <= '9') major = major * 10U + (unsigned)(*cursor++ - '0');
    if (*cursor++ != '.') __builtin_trap();
    while (*cursor >= '0' && *cursor <= '9') minor = minor * 10U + (unsigned)(*cursor++ - '0');
    cursor = release;
    while (*cursor) {
        if (*cursor == '.' && ++dots >= 3) break;
        if ((*cursor < '0' || *cursor > '9') && *cursor != '.') break;
        cursor++;
    }
    unsigned version = minor + (major >= 4 ? 60U : 40U);
    char digits[10], projected[KERNEL_UTS_FIELD_SIZE] = "2.6.";
    size_t count = 0, used = 4;
    do { digits[count++] = (char)('0' + version % 10U); version /= 10U; } while (version);
    while (count) projected[used++] = digits[--count];
    while (*cursor && used < sizeof(projected)-1) projected[used++] = *cursor++;
    projected[used] = 0;
    memcpy(release, projected, sizeof(projected));
}

enum kernel_syscall_status syscall_handle_uts(
    struct kernel_task *caller, const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    struct kernel_mm *mm;
    size_t copied = 0;
    enum kernel_uaccess_status access;

    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    if (request->number == 160U) {
        struct kernel_utsname name;
        if (kernel_task_mm_borrow_mutable(caller, &mm) != KERNEL_TASK_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        kernel_uts_snapshot(kernel_task_uts_namespace(),&name);
        if (caller->personality & KERNEL_PERSONALITY_UNAME26)
            project_legacy_release(name.release);
        access = kernel_copy_to_user(mm, request->arguments[0], &name,
                                     sizeof(name), &copied);
        if (access == KERNEL_UACCESS_STATUS_FAULT) decoded->value = -KERNEL_EFAULT;
        else if (access == KERNEL_UACCESS_STATUS_OK && copied == sizeof(name))
            decoded->value = 0;
        else return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (request->number != 161U && request->number != 162U)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    if (!kernel_credentials_capable(kernel_credentials_current(), KERNEL_CAP_SYS_ADMIN)) {
        decoded->value = -KERNEL_EPERM;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    int32_t length = (int32_t)request->arguments[1];
    if (length < 0 || length > (int32_t)KERNEL_UTS_NAME_MAX) {
        decoded->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    char name[KERNEL_UTS_NAME_MAX];
    if (length) {
        if (kernel_task_mm_borrow_mutable(caller, &mm) != KERNEL_TASK_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        access = kernel_copy_from_user(mm, name, request->arguments[0],
                                       (size_t)length, &copied);
        if (access == KERNEL_UACCESS_STATUS_FAULT) {
            decoded->value = -KERNEL_EFAULT;
            return KERNEL_SYSCALL_STATUS_OK;
        }
        if (access != KERNEL_UACCESS_STATUS_OK || copied != (size_t)length)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    /* Publish only after the complete user range has been copied. */
    kernel_uts_name_set(kernel_task_uts_namespace(),request->number == 161U ? KERNEL_UTS_HOSTNAME :
                        KERNEL_UTS_DOMAINNAME, name, (size_t)length);
    decoded->value = 0;
    return KERNEL_SYSCALL_STATUS_OK;
}
