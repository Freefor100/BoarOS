#include <kernel/credentials.h>
#include <kernel/scheduler.h>
#include "private.h"

#include <kernel/errno.h>
#include <kernel/exec.h>
#include <kernel/futex.h>
#include <kernel/mm.h>
#include <kernel/page.h>
#include <kernel/signal.h>
#include <kernel/task.h>
#include <kernel/uaccess.h>
#include <kernel/wait_internal.h>
#include <kernel/task_state.h>

#include <stddef.h>
#include <stdint.h>

static void lookup_end(struct kernel_task **task)
{ if (*task) kernel_task_lookup_put(*task); }

#define LINUX_CLONE_SIGNAL_MASK UINT64_C(0xff)
#define LINUX_SIGCHLD UINT64_C(17)
#define LINUX_CLONE_VM UINT64_C(0x100)
#define LINUX_CLONE_FS UINT64_C(0x200)
#define LINUX_CLONE_FILES UINT64_C(0x400)
#define LINUX_CLONE_SIGHAND UINT64_C(0x800)
#define LINUX_CLONE_CLEAR_SIGHAND UINT64_C(0x100000000)
#define LINUX_CLONE_VFORK UINT64_C(0x4000)
#define LINUX_CLONE_NEWNS UINT64_C(0x20000)
#define LINUX_CLONE_CHILD_CLEARTID UINT64_C(0x200000)
#define LINUX_CLONE_CHILD_SETTID UINT64_C(0x1000000)
#define LINUX_CLONE_KNOWN_FLAGS UINT64_C(0x3ffffffff)

enum kernel_syscall_status syscall_handle_prlimit64(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    KERNEL_SERVICE_SCOPE(service);
    struct kernel_rlimit64 before;
    struct kernel_rlimit64 replacement;
    struct kernel_mm *mm;
    struct kernel_task *target;
    int64_t pid = (int64_t)(int32_t)(uint32_t)request->arguments[0];
    uint32_t resource = (uint32_t)request->arguments[1];
    uint64_t new_address = request->arguments[2];
    uint64_t old_address = request->arguments[3];
    size_t copied = 0U;
    enum kernel_uaccess_status access_status;

    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    if (kernel_task_mm_borrow_mutable(caller, &mm) != KERNEL_TASK_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    if (new_address != 0U) {
        access_status = kernel_copy_from_user(mm, &replacement, new_address,
                                              sizeof(replacement), &copied);
        if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
            decoded->value = -KERNEL_EFAULT;
            return KERNEL_SYSCALL_STATUS_OK;
        }
        if (access_status != KERNEL_UACCESS_STATUS_OK ||
            copied != sizeof(replacement))
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    target = pid == 0 ? caller :
             pid > 0 && pid <= INT32_MAX
                 ? kernel_task_find_by_tid((kernel_pid_t)pid) : 0;
    struct kernel_task *lookup __attribute__((cleanup(lookup_end))) = pid > 0 ? target : 0;
    if (target == 0) {
        decoded->value = -KERNEL_ESRCH;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        const struct kernel_credentials *from=kernel_credentials_task_locked(caller),
            *to=kernel_credentials_task_locked(target);
        if(caller!=target && !kernel_credentials_capable(from,KERNEL_CAP_SYS_RESOURCE) &&
            !(from->uid==to->uid && from->uid==to->euid && from->uid==to->suid &&
              from->gid==to->gid && from->gid==to->egid && from->gid==to->sgid)) {
            decoded->value=-KERNEL_EPERM;return KERNEL_SYSCALL_STATUS_OK;
        }
    }
    if (resource >= KERNEL_RLIMIT_COUNT) {
        decoded->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    enum kernel_task_status task_status = kernel_task_get_rlimit(target, resource, &before);
    if (task_status == KERNEL_TASK_STATUS_STATE) {
        decoded->value = -KERNEL_ESRCH;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (task_status != KERNEL_TASK_STATUS_OK) return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    if (new_address != 0U) {
        uint64_t capacity = resource == KERNEL_RLIMIT_NOFILE
                                ? KERNEL_RLIMIT_NOFILE_CAP : KERNEL_RLIM_INFINITY;
        if (replacement.current > replacement.maximum) {
            decoded->value = -KERNEL_EINVAL;
            return KERNEL_SYSCALL_STATUS_OK;
        }
        if (replacement.maximum > capacity ||
            (replacement.maximum>before.maximum &&
             !kernel_credentials_capable(kernel_credentials_current(),KERNEL_CAP_SYS_RESOURCE))) {
            decoded->value = -KERNEL_EPERM;
            return KERNEL_SYSCALL_STATUS_OK;
        }
        task_status = kernel_task_set_rlimit(target, resource, &replacement);
        if (task_status == KERNEL_TASK_STATUS_STATE) {
            decoded->value = -KERNEL_ESRCH;
            return KERNEL_SYSCALL_STATUS_OK;
        }
        if (task_status != KERNEL_TASK_STATUS_OK) return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    if (old_address != 0U) {
        copied = 0U;
        access_status = kernel_copy_to_user(mm, old_address, &before,
                                            sizeof(before), &copied);
        if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
            decoded->value = -KERNEL_EFAULT;
            return KERNEL_SYSCALL_STATUS_OK;
        }
        if (access_status != KERNEL_UACCESS_STATUS_OK ||
            copied != sizeof(before))
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    decoded->value = 0;
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_set_tid_address(
    struct kernel_task *caller,
    uint64_t address,
    struct kernel_syscall_result *decoded)
{
    kernel_pid_t tid;

    if (kernel_task_set_tid_address(caller, address, &tid) !=
        KERNEL_TASK_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = tid;
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_set_robust_list(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    if (request->arguments[1] != 3U * sizeof(uint64_t)) {
        decoded->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    kernel_futex_set_robust_list(caller, request->arguments[0]);
    decoded->value = 0;
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_get_robust_list(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    KERNEL_SERVICE_SCOPE(service);
    int32_t pid = (int32_t)(uint32_t)request->arguments[0];
    struct kernel_task *target = pid == 0 ? caller :
                                 pid > 0 ? kernel_signal_find_by_tid(pid) : 0;
    struct kernel_mm *mm;
    uint64_t length = 3U * sizeof(uint64_t);
    uint64_t head;
    size_t copied = 0U;
    enum kernel_uaccess_status access;

    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    struct kernel_task *lookup __attribute__((cleanup(lookup_end))) = pid > 0 ? target : 0;
    if (target == 0) {
        decoded->value = -KERNEL_ESRCH;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (kernel_task_mm_borrow_mutable(caller, &mm) != KERNEL_TASK_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        if(!kernel_credentials_may_access_locked(caller,target,0)) {
            decoded->value=-KERNEL_EPERM;return KERNEL_SYSCALL_STATUS_OK;
        }
        head = kernel_futex_get_robust_list(target);
    }
    access = kernel_copy_to_user(mm, request->arguments[2], &length,
                                 sizeof(length), &copied);
    if (access == KERNEL_UACCESS_STATUS_FAULT ||
        (access == KERNEL_UACCESS_STATUS_OK && copied != sizeof(length))) {
        decoded->value = -KERNEL_EFAULT;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (access != KERNEL_UACCESS_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    copied = 0U;
    access = kernel_copy_to_user(mm, request->arguments[1], &head,
                                 sizeof(head), &copied);
    if (access == KERNEL_UACCESS_STATUS_FAULT ||
        (access == KERNEL_UACCESS_STATUS_OK && copied != sizeof(head))) {
        decoded->value = -KERNEL_EFAULT;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (access != KERNEL_UACCESS_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    decoded->value = 0;
    return KERNEL_SYSCALL_STATUS_OK;
}


enum kernel_syscall_status syscall_handle_execve(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
#if BOAROS_USER_SMP
    if (kernel_task_has_user_context(kernel_task_current()) && !kernel_scheduler_user_service_active())
        kernel_scheduler_enter_user_service();
#endif
    int64_t linux_result;

    enum kernel_exec_status status = request->number == 281
        ? kernel_execveat_prepare(caller, (int32_t)request->arguments[0],
            request->arguments[1], request->arguments[2], request->arguments[3],
            (uint32_t)request->arguments[4], &linux_result)
        : kernel_execve_prepare(caller, request->arguments[0],
            request->arguments[1], request->arguments[2], &linux_result);
    if (status != KERNEL_EXEC_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    decoded->action = linux_result == 0
                          ? KERNEL_SYSCALL_ACTION_EXEC
                          : KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = linux_result;
    return KERNEL_SYSCALL_STATUS_OK;
}


/*
 * clone(220) accepts the process forms: fork with an optional custom
 * child stack, and vfork (CLONE_VM|CLONE_VFORK) which shares the parent
 * address space and suspends the parent until the child execs or exits.
 * Both process forms may share the fs context with CLONE_FS and the
 * independent semaphore undo group with CLONE_SYSVSEM.
 * Threads share MM and dispositions; their files and fs contexts follow
 * CLONE_FILES and CLONE_FS. Processes may also share files or MM.
 */
void syscall_decode_clone(const struct kernel_syscall_request *request,
                         struct kernel_syscall_result *decoded)
{
    uint64_t flags = request->arguments[0];
    const uint64_t thread_required = UINT64_C(0x100) | UINT64_C(0x800) | UINT64_C(0x10000);
    const uint64_t thread_optional = LINUX_CLONE_FS | LINUX_CLONE_FILES | UINT64_C(0x80001000) |
        UINT64_C(0x40000) | UINT64_C(0x80000) |
        UINT64_C(0x100000) | UINT64_C(0x200000) | UINT64_C(0x400000) |
        UINT64_C(0x1000000);

    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    if ((flags & ~LINUX_CLONE_KNOWN_FLAGS) != 0U ||
        ((flags & LINUX_CLONE_NEWNS) && (flags & (LINUX_CLONE_FS | UINT64_C(0x10000)))) ||
        ((flags & UINT64_C(0x401000)) == UINT64_C(0x401000)) ||
        ((flags & UINT64_C(0x10000)) && !(flags & UINT64_C(0x800))) ||
        ((flags & UINT64_C(0x800)) && !(flags & LINUX_CLONE_VM)) ||
        ((flags & UINT64_C(0x08000000)) && (flags & UINT64_C(0x40000)))) {
        decoded->value = -KERNEL_EINVAL;
        return;
    }
    if ((flags & thread_required) == thread_required &&
        (flags & ~(thread_required | thread_optional)) == 0U) {
        decoded->action = KERNEL_SYSCALL_ACTION_CLONE;
        decoded->value = 0;
        return;
    }
    if ((flags & LINUX_CLONE_SIGNAL_MASK) > 64U) {
        decoded->value = -KERNEL_EINVAL;
        return;
    }
    /* A fork child owns its private child-TID store and exit clear. */
    uint64_t process_flags = flags & ~(LINUX_CLONE_SIGNAL_MASK | LINUX_CLONE_FS | LINUX_CLONE_FILES | LINUX_CLONE_CLEAR_SIGHAND | LINUX_CLONE_NEWNS | UINT64_C(0x80001000) |
        UINT64_C(0x40000) | UINT64_C(0x80000) | UINT64_C(0x100000) |
        LINUX_CLONE_CHILD_SETTID | LINUX_CLONE_CHILD_CLEARTID);
    if (process_flags != 0 && process_flags != LINUX_CLONE_VM &&
        process_flags != (LINUX_CLONE_VM | LINUX_CLONE_SIGHAND) &&
        process_flags != (LINUX_CLONE_VM | LINUX_CLONE_VFORK | LINUX_CLONE_SIGHAND) &&
        process_flags != (LINUX_CLONE_VM | LINUX_CLONE_VFORK)) {
        decoded->value = -KERNEL_ENOTSUP;
        return;
    }
    /* Without the CLONE_*SETTID/SETTLS flag bits Linux
     * ignores the remaining arguments; musl's fork passes only two. */
    decoded->action = KERNEL_SYSCALL_ACTION_CLONE;
    decoded->value = 0;
}

enum kernel_syscall_status syscall_handle_unshare(struct kernel_task *caller,
    const struct kernel_syscall_request *request,struct kernel_syscall_result *decoded)
{
    KERNEL_SERVICE_SCOPE(service);
    uint64_t flags=request->arguments[0];
    decoded->action=KERNEL_SYSCALL_ACTION_RETURN;
    if (flags & ~(LINUX_CLONE_FS | LINUX_CLONE_NEWNS))
        decoded->value=-KERNEL_EINVAL;
    else if ((flags & LINUX_CLONE_NEWNS) &&
             !kernel_credentials_capable(kernel_credentials_current(),KERNEL_CAP_SYS_ADMIN))
        decoded->value=-KERNEL_EPERM;
    else decoded->value=flags?kernel_task_unshare_fs(caller,!!(flags&LINUX_CLONE_NEWNS)):0;
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_setns(struct kernel_task *caller,
    const struct kernel_syscall_request *request,struct kernel_syscall_result *decoded)
{
    KERNEL_SERVICE_SCOPE(service);
    decoded->action=KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value=kernel_task_setns_mount(caller,(int32_t)request->arguments[0],
        (uint32_t)request->arguments[1]);
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_clone3(struct kernel_task *caller,
    struct kernel_syscall_request *request, struct kernel_syscall_result *decoded)
{
    struct clone3_args {
        uint64_t flags, pidfd, child_tid, parent_tid, exit_signal;
        uint64_t stack, stack_size, tls, set_tid, set_tid_size, cgroup;
    } args = {0};
    const uint64_t address = request->arguments[0], size = request->arguments[1];
    size_t copied;
    struct kernel_mm *mm;
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    if (size > BOAROS_PAGE_SIZE) { decoded->value = -KERNEL_E2BIG; return KERNEL_SYSCALL_STATUS_OK; }
    if (size < 64U) { decoded->value = -KERNEL_EINVAL; return KERNEL_SYSCALL_STATUS_OK; }
    if (kernel_task_mm_borrow_mutable(caller, &mm) != KERNEL_TASK_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    size_t length = size < sizeof(args) ? (size_t)size : sizeof(args);
    for (uint64_t offset=sizeof(args); offset<size;) {
        unsigned char tail[64];
        size_t count = size-offset < sizeof(tail) ? (size_t)(size-offset) : sizeof(tail);
        if (address > UINT64_MAX-offset || kernel_copy_from_user(mm,tail,address+offset,count,&copied) != KERNEL_UACCESS_STATUS_OK || copied != count) {
            decoded->value=-KERNEL_EFAULT; return KERNEL_SYSCALL_STATUS_OK;
        }
        for(size_t i=0;i<count;i++) if(tail[i]) { decoded->value=-KERNEL_E2BIG; return KERNEL_SYSCALL_STATUS_OK; }
        offset+=count;
    }
    if (kernel_copy_from_user(mm, &args, address, length, &copied) != KERNEL_UACCESS_STATUS_OK || copied != length) {
        decoded->value = -KERNEL_EFAULT; return KERNEL_SYSCALL_STATUS_OK;
    }
    if (args.set_tid_size > 32U || (!!args.set_tid != !!args.set_tid_size) ||
        (args.exit_signal & ~UINT64_C(0xff)) ||
        ((args.flags & UINT64_C(0x200000000)) && (size < sizeof(args) || args.cgroup > INT32_MAX))) {
        decoded->value=-KERNEL_EINVAL; return KERNEL_SYSCALL_STATUS_OK;
    }
    if (args.set_tid) {
        uint32_t requested[32];
        size_t count=(size_t)args.set_tid_size*sizeof(requested[0]);
        if (kernel_copy_from_user(mm,requested,args.set_tid,count,&copied)!=KERNEL_UACCESS_STATUS_OK || copied!=count) {
            decoded->value=-KERNEL_EFAULT; return KERNEL_SYSCALL_STATUS_OK;
        }
    }
    if ((args.flags & UINT64_C(0xff)) || (args.flags & UINT64_C(0x400000)) ||
        ((args.flags & UINT64_C(0x18000)) && args.exit_signal) ||
        ((args.flags & UINT64_C(0x100000800)) == UINT64_C(0x100000800)) ||
        ((args.stack == 0) != (args.stack_size == 0)) ||
        (args.stack && kernel_user_range_check(args.stack,args.stack_size) != KERNEL_UACCESS_STATUS_OK)) {
        decoded->value=-KERNEL_EINVAL; return KERNEL_SYSCALL_STATUS_OK;
    }
    if ((args.flags & UINT64_C(0x1000)) && (args.flags & UINT64_C(0x100000)) && args.pidfd==args.parent_tid) {
        decoded->value=-KERNEL_EINVAL; return KERNEL_SYSCALL_STATUS_OK;
    }
    if (args.set_tid || (args.flags & UINT64_C(0x200000000))) {
        decoded->value=-KERNEL_EOPNOTSUPP; return KERNEL_SYSCALL_STATUS_OK;
    }
    struct kernel_syscall_request legacy={.arguments={args.flags|args.exit_signal}};
    syscall_decode_clone(&legacy,decoded);
    if (decoded->action == KERNEL_SYSCALL_ACTION_CLONE) {
        request->arguments[0]=legacy.arguments[0];
        request->arguments[1]=args.stack ? args.stack+args.stack_size : 0;
        request->arguments[2]=args.parent_tid;
        request->arguments[3]=args.tls;
        request->arguments[4]=args.child_tid;
        request->arguments[5]=args.pidfd;
    }
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_setpgid(struct kernel_task *caller,
    const struct kernel_syscall_request *request, struct kernel_syscall_result *decoded)
{
    KERNEL_SERVICE_SCOPE(service);
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = kernel_task_setpgid(caller, (int32_t)request->arguments[0],
                                        (int32_t)request->arguments[1]);
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_getpgid(struct kernel_task *caller,
    const struct kernel_syscall_request *request, struct kernel_syscall_result *decoded)
{
    KERNEL_SERVICE_SCOPE(service);
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = kernel_task_getpgid(caller, (int32_t)request->arguments[0]);
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_getsid(struct kernel_task *caller,
    const struct kernel_syscall_request *request, struct kernel_syscall_result *decoded)
{
    KERNEL_SERVICE_SCOPE(service);
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = kernel_task_getsid(caller, (int32_t)request->arguments[0]);
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_setsid(struct kernel_task *caller,
    const struct kernel_syscall_request *request, struct kernel_syscall_result *decoded)
{
    KERNEL_SERVICE_SCOPE(service);
    (void)request;
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = kernel_task_setsid(caller);
    return KERNEL_SYSCALL_STATUS_OK;
}
