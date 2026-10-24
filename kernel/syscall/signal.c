#include "private.h"

#include <kernel/errno.h>
#include <kernel/mm.h>
#include <kernel/scheduler.h>
#include <kernel/signal.h>
#include <kernel/signal_fd.h>
#include <kernel/pidfd.h>
#include <kernel/tty_task.h>
#include <kernel/task.h>
#include <kernel/time.h>
#include <kernel/uaccess.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define LINUX_SIGSET_SIZE 8U
#define LINUX_SYSCALL_KILL 129U
#define LINUX_SYSCALL_TGKILL 131U

/* These helpers expose the actual copy result. Only handlers map errno. */
static enum kernel_uaccess_status signal_copy_from_user(
    struct kernel_mm *mm, uint64_t address, void *destination, size_t size)
{
    size_t copied = 0U;
    enum kernel_uaccess_status status =
        kernel_copy_from_user(mm, destination, address, size, &copied);

    return status == KERNEL_UACCESS_STATUS_OK && copied != size
               ? KERNEL_UACCESS_STATUS_STATE : status;
}

static enum kernel_uaccess_status signal_copy_to_user(
    struct kernel_mm *mm, uint64_t address, const void *source, size_t size)
{
    size_t copied = 0U;
    enum kernel_uaccess_status status =
        kernel_copy_to_user(mm, address, source, size, &copied);

    return status == KERNEL_UACCESS_STATUS_OK && copied != size
               ? KERNEL_UACCESS_STATUS_STATE : status;
}

static enum kernel_syscall_status signal_copy_error(
    enum kernel_uaccess_status status, struct kernel_syscall_result *decoded)
{
    if (status != KERNEL_UACCESS_STATUS_FAULT) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    decoded->value = -KERNEL_EFAULT;
    return KERNEL_SYSCALL_STATUS_OK;
}

static enum kernel_syscall_status signal_result(
    enum kernel_signal_status status, struct kernel_syscall_result *decoded)
{
    if (status == KERNEL_SIGNAL_STATUS_OK) {
        decoded->value = 0;
    } else if (status == KERNEL_SIGNAL_STATUS_INVALID_ARGUMENT) {
        decoded->value = -KERNEL_EINVAL;
    } else if (status == KERNEL_SIGNAL_STATUS_NO_MEMORY) {
        decoded->value = -KERNEL_ENOMEM;
    } else {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_sigaltstack(
    struct kernel_task *caller, const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    struct kernel_signal_stack input, old;
    struct kernel_mm *mm;
    enum kernel_uaccess_status copied;
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    if (kernel_task_mm_borrow_mutable(caller, &mm) != KERNEL_TASK_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    if (request->arguments[0]) {
        copied = signal_copy_from_user(mm, request->arguments[0], &input, sizeof(input));
        if (copied != KERNEL_UACCESS_STATUS_OK) return signal_copy_error(copied, decoded);
    }
    decoded->value = kernel_signal_stack_update(caller,
        request->arguments[0] ? &input : 0, &old, request->user_sp);
    if (!decoded->value && request->arguments[1]) {
        copied = signal_copy_to_user(mm, request->arguments[1], &old, sizeof(old));
        if (copied != KERNEL_UACCESS_STATUS_OK) return signal_copy_error(copied, decoded);
    }
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_rt_sigaction(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    KERNEL_SERVICE_SCOPE(service);
    uint32_t sig = (uint32_t)request->arguments[0];
    uint64_t input = request->arguments[1];
    uint64_t output = request->arguments[2];
    struct kernel_linux_sigaction action;
    struct kernel_linux_sigaction old_action;
    struct kernel_mm *mm;
    enum kernel_uaccess_status copy_status;
    enum kernel_signal_status status;

    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    if (request->arguments[3] != LINUX_SIGSET_SIZE || sig == 0U ||
        sig > KERNEL_SIGNAL_COUNT || sig == 9U || sig == 19U) {
        decoded->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (kernel_task_mm_borrow_mutable(caller, &mm) != KERNEL_TASK_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    /* Snapshot before writing oldact: act and oldact may alias. Linux
     * commits the action before copying oldact out, so output EFAULT is
     * not a rollback of an otherwise valid installation. */
    if (input != 0U) {
        copy_status = signal_copy_from_user(mm, input, &action, sizeof(action));
        if (copy_status != KERNEL_UACCESS_STATUS_OK) {
            return signal_copy_error(copy_status, decoded);
        }
    }
    if (input != 0U) {
        status = kernel_signal_set_action(caller, sig, &action, &old_action);
    } else status = kernel_signal_get_action(caller, sig, &old_action);
    if (status != KERNEL_SIGNAL_STATUS_OK) return signal_result(status, decoded);
    if (output != 0U) {
        copy_status = signal_copy_to_user(mm, output, &old_action,
                                          sizeof(old_action));
        if (copy_status != KERNEL_UACCESS_STATUS_OK) {
            return signal_copy_error(copy_status, decoded);
        }
    }
    return signal_result(KERNEL_SIGNAL_STATUS_OK, decoded);
}

enum kernel_syscall_status syscall_handle_rt_sigprocmask(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    KERNEL_SERVICE_SCOPE(service);
    uint32_t how = (uint32_t)request->arguments[0];
    uint64_t input = request->arguments[1];
    uint64_t output = request->arguments[2];
    uint64_t mask;
    uint64_t old_mask;
    struct kernel_mm *mm;
    enum kernel_uaccess_status copy_status;
    enum kernel_signal_status status;

    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    if (request->arguments[3] != LINUX_SIGSET_SIZE ||
        (input != 0U && how > LINUX_SIG_SETMASK)) {
        decoded->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (kernel_task_mm_borrow_mutable(caller, &mm) != KERNEL_TASK_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    if (input != 0U) {
        copy_status = signal_copy_from_user(mm, input, &mask, sizeof(mask));
        if (copy_status != KERNEL_UACCESS_STATUS_OK) {
            return signal_copy_error(copy_status, decoded);
        }
    }
    status = kernel_signal_update_blocked(caller, how,
                                          input != 0U ? &mask : 0, &old_mask);
    if (status != KERNEL_SIGNAL_STATUS_OK) {
        return signal_result(status, decoded);
    }
    if (output != 0U) {
        copy_status = signal_copy_to_user(mm, output, &old_mask, sizeof(old_mask));
        if (copy_status != KERNEL_UACCESS_STATUS_OK) {
            return signal_copy_error(copy_status, decoded);
        }
    }
    return signal_result(KERNEL_SIGNAL_STATUS_OK, decoded);
}

enum kernel_syscall_status syscall_handle_rt_sigpending(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    KERNEL_SERVICE_SCOPE(service);
    struct kernel_mm *mm;
    uint64_t pending;
    enum kernel_uaccess_status copy_status;
    enum kernel_signal_status status;

    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    if (request->arguments[1] != LINUX_SIGSET_SIZE) {
        decoded->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (kernel_task_mm_borrow_mutable(caller, &mm) != KERNEL_TASK_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    status = kernel_signal_get_pending(caller, &pending);
    if (status != KERNEL_SIGNAL_STATUS_OK) {
        return signal_result(status, decoded);
    }
    copy_status = signal_copy_to_user(mm, request->arguments[0],
                                      &pending, sizeof(pending));
    if (copy_status != KERNEL_UACCESS_STATUS_OK) {
        return signal_copy_error(copy_status, decoded);
    }
    return signal_result(KERNEL_SIGNAL_STATUS_OK, decoded);
}

enum kernel_syscall_status syscall_handle_rt_sigtimedwait(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    struct { int64_t seconds, nanoseconds; } timeout;
    uint8_t user_info[128];
    struct kernel_signal_wait_info taken;
    struct kernel_mm *mm;
    uint64_t mask;
    uint64_t deadline = 0U;
    uint64_t target_ns = 0U;
    int has_timeout = request->arguments[2] != 0U;
    enum kernel_uaccess_status copy_status;
    enum kernel_signal_status signal_status;
    enum kernel_time_status time_status;

    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    if (request->arguments[3] != LINUX_SIGSET_SIZE) {
        decoded->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (kernel_task_mm_borrow_mutable(caller, &mm) != KERNEL_TASK_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    copy_status = signal_copy_from_user(mm, request->arguments[0],
                                        &mask, sizeof(mask));
    if (copy_status != KERNEL_UACCESS_STATUS_OK)
        return signal_copy_error(copy_status, decoded);
    if (has_timeout) {
        copy_status = signal_copy_from_user(mm, request->arguments[2],
                                            &timeout, sizeof(timeout));
        if (copy_status != KERNEL_UACCESS_STATUS_OK)
            return signal_copy_error(copy_status, decoded);
        if (timeout.seconds < 0 || timeout.nanoseconds < 0 ||
            timeout.nanoseconds >= INT64_C(1000000000)) {
            decoded->value = -KERNEL_EINVAL;
            return KERNEL_SYSCALL_STATUS_OK;
        }
        unsigned __int128 duration =
            (unsigned __int128)(uint64_t)timeout.seconds *
                UINT64_C(1000000000) + (uint64_t)timeout.nanoseconds;
        uint64_t now_ns = kernel_time_monotonic_ns();

        target_ns = now_ns >= INT64_MAX ||
                    duration > (uint64_t)INT64_MAX - now_ns
                        ? INT64_MAX : now_ns + (uint64_t)duration;
    }
    signal_status = kernel_signal_wait_begin(caller, mask);
    if (signal_status != KERNEL_SIGNAL_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    for (;;) {
        enum kernel_wait_wake_reason wake_reason;

        signal_status = kernel_signal_wait_take(caller, &taken);
        if (signal_status != KERNEL_SIGNAL_STATUS_OK) {
            kernel_signal_wait_end(caller);
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
        if (taken.signal != 0U) break;
        if (kernel_signal_has_pending(caller) ||
            kernel_signal_termination_requested(caller) ||
            !has_timeout || target_ns == 0U) {
            if (kernel_signal_has_pending(caller) ||
                kernel_signal_termination_requested(caller))
                decoded->value = -KERNEL_EINTR;
            else if (has_timeout)
                decoded->value = -KERNEL_EAGAIN;
            else {
                if (KERNEL_WAIT_RECHECK(0, 0U, 1,
                                                   &wake_reason,
                (1)) !=
                    KERNEL_SCHEDULER_STATUS_OK) {
                    kernel_signal_wait_end(caller);
                    return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
                }
                continue;
            }
            break;
        }
        time_status = kernel_time_deadline_from_monotonic(target_ns,
                                                          &deadline);
        if (time_status == KERNEL_TIME_STATUS_DEADLINE_PASSED) {
            decoded->value = -KERNEL_EAGAIN;
            break;
        }
        if (time_status != KERNEL_TIME_STATUS_OK ||
            KERNEL_WAIT_RECHECK(0, deadline, 1,
                                           &wake_reason,
                (1)) !=
                KERNEL_SCHEDULER_STATUS_OK) {
            kernel_signal_wait_end(caller);
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    }
    kernel_signal_wait_end(caller);
    if (taken.signal == 0U) return KERNEL_SYSCALL_STATUS_OK;
    decoded->value = taken.signal;
    if (request->arguments[1] == 0U) return KERNEL_SYSCALL_STATUS_OK;
    kernel_signal_encode_info(user_info, &taken);
    copy_status = signal_copy_to_user(mm, request->arguments[1],
                                      &user_info, sizeof(user_info));
    if (copy_status != KERNEL_UACCESS_STATUS_OK)
        return signal_copy_error(copy_status, decoded);
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_rt_sigsuspend(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    KERNEL_SERVICE_SCOPE(service);
    struct kernel_mm *mm;
    uint64_t mask;
    enum kernel_uaccess_status copy_status;
    enum kernel_signal_status status;

    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    if (request->arguments[1] != LINUX_SIGSET_SIZE) {
        decoded->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (kernel_task_mm_borrow_mutable(caller, &mm) != KERNEL_TASK_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    copy_status = signal_copy_from_user(mm, request->arguments[0],
                                        &mask, sizeof(mask));
    if (copy_status != KERNEL_UACCESS_STATUS_OK) {
        return signal_copy_error(copy_status, decoded);
    }
    status = kernel_signal_suspend(caller, mask);
    if (status != KERNEL_SIGNAL_STATUS_OK) {
        return signal_result(status, decoded);
    }
    decoded->value = -KERNEL_EINTR;
    return KERNEL_SYSCALL_STATUS_OK;
}

static int64_t syscall_signal_pid(uint64_t value)
{
    return (int64_t)(int32_t)(uint32_t)value;
}

static int known_signal_info(unsigned sig,int code)
{
    if(code==128 || (code<=0 && code>=-7) || code==-60)return 1;
    if(code<=0)return 0;
    unsigned limit=sig==4?11:sig==8?15:sig==11?10:sig==7?5:sig==31?2:6;
    return (unsigned)code<=limit;
}

static int import_signal_info(struct kernel_mm *mm,uint64_t address,
    unsigned layout_signal,struct kernel_signal_wait_info *info)
{
    enum kernel_uaccess_status copied=signal_copy_from_user(mm,address,info,48);
    if(copied!=KERNEL_UACCESS_STATUS_OK)return -KERNEL_EFAULT;
    if(!known_signal_info(layout_signal,info->code)) {
        uint8_t expansion[80];
        if(address>UINT64_MAX-48)return -KERNEL_EFAULT;
        copied=signal_copy_from_user(mm,address+48,expansion,sizeof(expansion));
        if(copied!=KERNEL_UACCESS_STATUS_OK)return -KERNEL_EFAULT;
        for(unsigned i=0;i<sizeof(expansion);i++)if(expansion[i])return -KERNEL_E2BIG;
    }
    return 0;
}

enum kernel_syscall_status syscall_handle_pidfd_signal(struct kernel_task *caller,
    const struct kernel_syscall_request *request,struct kernel_syscall_result *decoded)
{
    KERNEL_SERVICE_SCOPE(service);
    uint32_t flags=(uint32_t)request->arguments[3],sig=(uint32_t)request->arguments[1];
    decoded->action=KERNEL_SYSCALL_ACTION_RETURN;
    struct kernel_pid *target __attribute__((cleanup(kernel_task_identity_end)))=0;
    unsigned scope;
    int error=kernel_pidfd_signal_target(caller,(int32_t)request->arguments[0],flags,&target,&scope);
    if(error){decoded->value=error;return 0;}
    struct kernel_signal_wait_info info={0};
    if(request->arguments[2]) {
        struct kernel_mm *mm;
        if(kernel_task_mm_borrow_mutable(caller,&mm)!=KERNEL_TASK_STATUS_OK)return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        error=import_signal_info(mm,request->arguments[2],sig,&info);
        if(error){decoded->value=error;return 0;}
    }
    decoded->value=kernel_pidfd_send(caller,target,scope,sig,request->arguments[2]?&info:0);
    return 0;
}

enum kernel_syscall_status syscall_handle_sigqueueinfo(struct kernel_task *caller,
    const struct kernel_syscall_request *request,struct kernel_syscall_result *decoded)
{
    int thread=request->number==240;
    int64_t group=syscall_signal_pid(request->arguments[0]);
    int64_t pid=thread?syscall_signal_pid(request->arguments[1]):group;
    unsigned sig=(uint32_t)request->arguments[thread?2:1];
    /* 实时排队与目标存储由共同状态保护；stop/continue 等控制信号
     * 仍在 CPU0 完成进程控制。 */
    KERNEL_SERVICE_SCOPE_IF(service,sig<32 || sig>64);
    COST_SCOPE(cost,SIGNAL_ENQUEUE);
    uint64_t address=request->arguments[thread?3:2];
    struct kernel_signal_wait_info info={0};struct kernel_mm *mm;
    decoded->action=KERNEL_SYSCALL_ACTION_RETURN;
    if(kernel_task_mm_borrow_mutable(caller,&mm)!=KERNEL_TASK_STATUS_OK)return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    /* Linux imports its 48-byte kernel siginfo, then checks the expansion
     * only for unknown layouts. Known input need not expose unused bytes. */
    int imported=import_signal_info(mm,address,sig,&info);
    if(imported){decoded->value=imported;return 0;}
    info.signal=sig;
    if(thread && (group<=0 || pid<=0)){decoded->value=-KERNEL_EINVAL;return KERNEL_SYSCALL_STATUS_OK;}
    kernel_pid_t self;
    if(kernel_task_tid(caller,&self)!=KERNEL_TASK_STATUS_OK)return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    if((info.code>=0 || info.code==-6) && pid!=self) {
        decoded->value=-KERNEL_EPERM;return KERNEL_SYSCALL_STATUS_OK;
    }
    decoded->value=pid<=0?-KERNEL_ESRCH:kernel_signal_queue_info((int32_t)group,(int32_t)pid,thread,&info);
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_signal_send(
    struct kernel_task *caller,
    uint32_t kind,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    KERNEL_SERVICE_SCOPE(service);
    int64_t pid = syscall_signal_pid(request->arguments[0]);
    uint32_t sig = (uint32_t)request->arguments[kind == LINUX_SYSCALL_TGKILL
                                                   ? 2U
                                                   : 1U];
    kernel_pid_t sender_pid;
    int64_t sent;
    enum kernel_task_status task_status;

    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    if (sig > KERNEL_SIGNAL_COUNT ||
        (kind != LINUX_SYSCALL_KILL && pid <= 0) ||
        (kind == LINUX_SYSCALL_TGKILL &&
         syscall_signal_pid(request->arguments[1]) <= 0)) {
        decoded->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (kind == LINUX_SYSCALL_KILL && sig == 0U) {
        sent = kernel_signal_resolve_targets(caller, pid);
        decoded->value = sent < 0 ? sent : sent == 0U ? -KERNEL_ESRCH : 0;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    task_status = kernel_task_tgid(caller, &sender_pid);
    if (task_status != KERNEL_TASK_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    if (kind == LINUX_SYSCALL_KILL) {
        sent = kernel_signal_send_targets(caller,
                                          pid,
                                          sig,
                                          sender_pid);
    } else {
        kernel_pid_t tgid = kind == LINUX_SYSCALL_TGKILL
                                ? (kernel_pid_t)syscall_signal_pid(
                                      request->arguments[0])
                                : 0;
        kernel_pid_t tid = (kernel_pid_t)syscall_signal_pid(
            request->arguments[kind == LINUX_SYSCALL_TGKILL ? 1U : 0U]);

        sent = kernel_signal_send_thread(caller,
                                          tgid,
                                          tid,
                                          sig,
                                          sender_pid);
    }
    decoded->value = sent < 0 ? sent : sent == 0 ? -KERNEL_ESRCH : 0;
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_signalfd(struct kernel_task *caller,
    const struct kernel_syscall_request *request,struct kernel_syscall_result *decoded)
{
    decoded->action=KERNEL_SYSCALL_ACTION_RETURN;
    if(request->arguments[2]!=8){decoded->value=-KERNEL_EINVAL;return KERNEL_SYSCALL_STATUS_OK;}
    struct kernel_mm *mm;struct kernel_files *files;
    if(kernel_task_mm_borrow_mutable(caller,&mm)||kernel_task_files_borrow(caller,&files))return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    uint64_t mask;
    enum kernel_uaccess_status copied=signal_copy_from_user(mm,request->arguments[1],&mask,8);
    if(copied!=KERNEL_UACCESS_STATUS_OK)return signal_copy_error(copied,decoded);
    return kernel_files_signalfd(files,caller,(int32_t)request->arguments[0],mask,(uint32_t)request->arguments[3],&decoded->value)
        ?KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT:KERNEL_SYSCALL_STATUS_OK;
}
