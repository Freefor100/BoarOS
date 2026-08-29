#include <kernel/errno.h>
#include <kernel/exec.h>
#include <kernel/files.h>
#include <kernel/fs_context.h>
#include <kernel/mm.h>
#include <kernel/syscall.h>
#include <kernel/task.h>
#include <kernel/uaccess.h>

#include <stddef.h>
#include <stdint.h>

#define LINUX_SYSCALL_OPENAT 56U
#define LINUX_SYSCALL_CLOSE 57U
#define LINUX_SYSCALL_READ 63U
#define LINUX_SYSCALL_EXIT 93U
#define LINUX_SYSCALL_UNAME 160U
#define LINUX_SYSCALL_GETPID 172U
#define LINUX_SYSCALL_GETPPID 173U
#define LINUX_SYSCALL_GETTID 178U
#define LINUX_SYSCALL_CLONE 220U
#define LINUX_SYSCALL_EXECVE 221U
#define LINUX_SYSCALL_WAIT4 260U
#define LINUX_EXIT_STATUS_MASK UINT64_C(0xff)
#define LINUX_CLONE_SIGNAL_MASK UINT64_C(0xff)
#define LINUX_SIGCHLD UINT64_C(17)
#define LINUX_CLONE_KNOWN_FLAGS UINT64_C(0x3ffffffff)
#define LINUX_UTS_FIELD_SIZE 65U

#ifndef BOAROS_UTS_MACHINE
#error "BOAROS_UTS_MACHINE must name the Linux architecture"
#endif

struct linux_new_utsname {
    char sysname[LINUX_UTS_FIELD_SIZE];
    char nodename[LINUX_UTS_FIELD_SIZE];
    char release[LINUX_UTS_FIELD_SIZE];
    char version[LINUX_UTS_FIELD_SIZE];
    char machine[LINUX_UTS_FIELD_SIZE];
    char domainname[LINUX_UTS_FIELD_SIZE];
};

static const struct linux_new_utsname kernel_utsname = {
    .sysname = "Linux",
    .nodename = "boaros",
    /* BoarOS release identity, not a claimed Linux feature level. */
    .release = "0.1.0-boaros-dev",
    .version = "#1 BoarOS",
    .machine = BOAROS_UTS_MACHINE,
    .domainname = "(none)",
};

_Static_assert(sizeof(struct linux_new_utsname) == 390U,
               "Linux new_utsname ABI size must remain 390 bytes");

static enum kernel_syscall_status decode_uname(
    struct kernel_task *caller,
    uint64_t user_address,
    struct kernel_syscall_result *decoded)
{
    const struct kernel_mm *mm;
    size_t copied;
    enum kernel_task_status task_status;
    enum kernel_uaccess_status access_status;

    task_status = kernel_task_mm_borrow(caller, &mm);
    if (task_status != KERNEL_TASK_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    access_status = kernel_copy_to_user(mm,
                                        user_address,
                                        &kernel_utsname,
                                        sizeof(kernel_utsname),
                                        &copied);
    if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
        decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
        decoded->value = -KERNEL_EFAULT;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (access_status != KERNEL_UACCESS_STATUS_OK ||
        copied != sizeof(kernel_utsname)) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = 0;
    return KERNEL_SYSCALL_STATUS_OK;
}

static enum kernel_syscall_status decode_openat(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    struct kernel_files *files;
    const struct kernel_fs_context *fs;
    const struct kernel_mm *mm;
    int64_t linux_result;
    enum kernel_task_status task_status;

    task_status = kernel_task_files_borrow(caller, &files);
    if (task_status == KERNEL_TASK_STATUS_RESOURCE_UNAVAILABLE) {
        decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
        decoded->value = -KERNEL_ENODEV;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (task_status != KERNEL_TASK_STATUS_OK ||
        kernel_task_fs_context_borrow(caller, &fs) !=
            KERNEL_TASK_STATUS_OK ||
        kernel_task_mm_borrow(caller, &mm) != KERNEL_TASK_STATUS_OK ||
        kernel_files_openat(files,
                            fs,
                            mm,
                            (int64_t)request->arguments[0],
                            request->arguments[1],
                            request->arguments[2],
                            request->arguments[3],
                            &linux_result) != KERNEL_FILES_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = linux_result;
    return KERNEL_SYSCALL_STATUS_OK;
}

static enum kernel_syscall_status decode_read(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    struct kernel_files *files;
    const struct kernel_mm *mm;
    int64_t linux_result;
    enum kernel_task_status task_status;

    task_status = kernel_task_files_borrow(caller, &files);
    if (task_status == KERNEL_TASK_STATUS_RESOURCE_UNAVAILABLE) {
        decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
        decoded->value = -KERNEL_EBADF;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (task_status != KERNEL_TASK_STATUS_OK ||
        kernel_task_mm_borrow(caller, &mm) != KERNEL_TASK_STATUS_OK ||
        kernel_files_read(files,
                          mm,
                          (int64_t)request->arguments[0],
                          request->arguments[1],
                          request->arguments[2],
                          &linux_result) != KERNEL_FILES_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = linux_result;
    return KERNEL_SYSCALL_STATUS_OK;
}

static enum kernel_syscall_status decode_close(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    struct kernel_files *files;
    int64_t linux_result;
    enum kernel_task_status task_status;

    task_status = kernel_task_files_borrow(caller, &files);
    if (task_status == KERNEL_TASK_STATUS_RESOURCE_UNAVAILABLE) {
        decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
        decoded->value = -KERNEL_EBADF;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (task_status != KERNEL_TASK_STATUS_OK ||
        kernel_files_close(files,
                           (int64_t)request->arguments[0],
                           &linux_result) != KERNEL_FILES_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = linux_result;
    return KERNEL_SYSCALL_STATUS_OK;
}

static enum kernel_syscall_status decode_execve(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    int64_t linux_result;

    if (kernel_execve_prepare(caller,
                              request->arguments[0],
                              request->arguments[1],
                              request->arguments[2],
                              &linux_result) != KERNEL_EXEC_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    decoded->action = linux_result == 0
                          ? KERNEL_SYSCALL_ACTION_EXEC
                          : KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = linux_result;
    return KERNEL_SYSCALL_STATUS_OK;
}

static void decode_clone(const struct kernel_syscall_request *request,
                         struct kernel_syscall_result *decoded)
{
    uint64_t flags = request->arguments[0];

    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    if ((flags & ~LINUX_CLONE_KNOWN_FLAGS) != 0U ||
        (flags & LINUX_CLONE_SIGNAL_MASK) != LINUX_SIGCHLD) {
        decoded->value = -KERNEL_EINVAL;
        return;
    }
    if (flags != LINUX_SIGCHLD || request->arguments[1] != 0U ||
        request->arguments[2] != 0U || request->arguments[3] != 0U ||
        request->arguments[4] != 0U) {
        decoded->value = -KERNEL_ENOTSUP;
        return;
    }
    decoded->action = KERNEL_SYSCALL_ACTION_CLONE;
    decoded->value = 0;
}

enum kernel_syscall_status kernel_syscall_dispatch(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *result)
{
    struct kernel_syscall_result decoded;
    kernel_pid_t id;
    enum kernel_task_status task_status;

    if (caller == 0 || request == 0 || result == 0) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }

    if (request->number == LINUX_SYSCALL_OPENAT) {
        if (decode_openat(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_CLOSE) {
        if (decode_close(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_READ) {
        if (decode_read(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_EXIT) {
        decoded.action = KERNEL_SYSCALL_ACTION_EXIT;
        decoded.value = (int64_t)(request->arguments[0] &
                                  LINUX_EXIT_STATUS_MASK);
    } else if (request->number == LINUX_SYSCALL_UNAME) {
        if (decode_uname(caller,
                         request->arguments[0],
                         &decoded) != KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_GETPID) {
        task_status = kernel_task_tgid(caller, &id);
        if (task_status != KERNEL_TASK_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
        decoded.action = KERNEL_SYSCALL_ACTION_RETURN;
        decoded.value = id;
    } else if (request->number == LINUX_SYSCALL_GETPPID) {
        task_status = kernel_task_ppid(caller, &id);
        if (task_status != KERNEL_TASK_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
        decoded.action = KERNEL_SYSCALL_ACTION_RETURN;
        decoded.value = id;
    } else if (request->number == LINUX_SYSCALL_GETTID) {
        task_status = kernel_task_tid(caller, &id);
        if (task_status != KERNEL_TASK_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
        decoded.action = KERNEL_SYSCALL_ACTION_RETURN;
        decoded.value = id;
    } else if (request->number == LINUX_SYSCALL_EXECVE) {
        if (decode_execve(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_CLONE) {
        decode_clone(request, &decoded);
    } else if (request->number == LINUX_SYSCALL_WAIT4) {
        decoded.action = KERNEL_SYSCALL_ACTION_WAIT4;
        decoded.value = 0;
    } else {
        decoded.action = KERNEL_SYSCALL_ACTION_RETURN;
        decoded.value = -KERNEL_ENOSYS;
    }

    *result = decoded;
    return KERNEL_SYSCALL_STATUS_OK;
}
