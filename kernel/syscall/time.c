#include "private.h"

#include <arch/riscv/timer.h>
#include <kernel/errno.h>
#include <kernel/futex.h>
#include <kernel/mm.h>
#include <kernel/scheduler.h>
#include <kernel/signal.h>
#include <kernel/task.h>
#include <kernel/tick.h>
#include <kernel/time.h>
#include <kernel/uaccess.h>

#include <stddef.h>
#include <stdint.h>

#define LINUX_CLOCK_REALTIME 0U
#define LINUX_CLOCK_MONOTONIC 1U
#define LINUX_CLOCK_NSECS_PER_SEC UINT64_C(1000000000)
#define LINUX_TIMER_ABSTIME UINT64_C(1)

/*
 * clockid support covers the clocks the kernel can actually back:
 * CLOCK_REALTIME from the boot RTC reading plus the time counter, and
 * CLOCK_MONOTONIC from the time counter.  Everything else reports EINVAL.
 */
static int syscall_clock_id_valid(uint64_t clock_id)
{
    return clock_id == LINUX_CLOCK_REALTIME || clock_id == LINUX_CLOCK_MONOTONIC;
}

static void syscall_split_nanoseconds(uint64_t nanoseconds,
                                     int64_t *seconds,
                                     int64_t *sub_nanoseconds)
{
    *seconds = (int64_t)(nanoseconds / LINUX_CLOCK_NSECS_PER_SEC);
    *sub_nanoseconds = (int64_t)(nanoseconds % LINUX_CLOCK_NSECS_PER_SEC);
}

/* Final result formatting for the time handlers, not a copy-status API. */
static enum kernel_syscall_status syscall_return_time(
    struct kernel_task *caller, uint64_t user_address,
    int64_t seconds, int64_t sub_seconds,
    struct kernel_syscall_result *decoded)
{
    struct { int64_t seconds; int64_t sub_seconds; } value = {
        seconds, sub_seconds,
    };
    struct kernel_mm *mm;
    size_t copied = 0U;
    enum kernel_uaccess_status status;

    if (kernel_task_mm_borrow_mutable(caller, &mm) != KERNEL_TASK_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    status = kernel_copy_to_user(mm, user_address, &value, sizeof(value),
                                 &copied);
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    if (status == KERNEL_UACCESS_STATUS_FAULT) {
        decoded->value = -KERNEL_EFAULT;
    } else if (status == KERNEL_UACCESS_STATUS_OK && copied == sizeof(value)) {
        decoded->value = 0;
    } else {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_clock_gettime(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    uint64_t nanoseconds;
    int64_t seconds;
    int64_t sub_nanoseconds;

    if (!syscall_clock_id_valid(request->arguments[0])) {
        decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
        decoded->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    nanoseconds = request->arguments[0] == LINUX_CLOCK_REALTIME
                      ? kernel_time_realtime_ns()
                      : kernel_time_monotonic_ns();
    syscall_split_nanoseconds(nanoseconds, &seconds, &sub_nanoseconds);
    return syscall_return_time(caller,
                                request->arguments[1],
                                seconds,
                                sub_nanoseconds,
                                decoded);
}

enum kernel_syscall_status syscall_handle_clock_getres(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    if (!syscall_clock_id_valid(request->arguments[0])) {
        decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
        decoded->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (request->arguments[1] == 0U) {
        decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
        decoded->value = 0;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    /* Nanosecond time-counter resolution. */
    return syscall_return_time(caller,
                                request->arguments[1],
                                0,
                                1,
                                decoded);
}

enum kernel_syscall_status syscall_handle_gettimeofday(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    uint64_t nanoseconds;
    int64_t seconds;
    int64_t sub_nanoseconds;

    if (request->arguments[0] == 0U) {
        /* Linux returns success for a NULL timeval; tz is ignored. */
        decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
        decoded->value = 0;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    nanoseconds = kernel_time_realtime_ns();
    syscall_split_nanoseconds(nanoseconds, &seconds, &sub_nanoseconds);
    return syscall_return_time(caller,
                                request->arguments[0],
                                seconds,
                                sub_nanoseconds / 1000,
                                decoded);
}

struct linux_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct linux_tms {
    int64_t utime;
    int64_t stime;
    int64_t cutime;
    int64_t cstime;
};

/*
 * times(153): fills the optional tms buffer with scheduler-tick CPU
 * accounting (CLK_TCK = KERNEL_TICKS_PER_SECOND) and returns the tick
 * count since boot.  A NULL tms pointer only samples the uptime ticks.
 */
enum kernel_syscall_status syscall_handle_times(
    struct kernel_task *caller,
    uint64_t tms_address,
    struct kernel_syscall_result *decoded)
{
    uint64_t user_ticks;
    uint64_t kernel_ticks;
    uint64_t child_user_ticks;
    uint64_t child_kernel_ticks;

    kernel_task_cpu_ticks(caller,
                          &user_ticks,
                          &kernel_ticks,
                          &child_user_ticks,
                          &child_kernel_ticks);
    if (tms_address != 0U) {
        struct kernel_mm *mm;
        struct linux_tms value;
        size_t copied;
        enum kernel_task_status task_status;
        enum kernel_uaccess_status access_status;

        value.utime = (int64_t)user_ticks;
        value.stime = (int64_t)kernel_ticks;
        value.cutime = (int64_t)child_user_ticks;
        value.cstime = (int64_t)child_kernel_ticks;
        task_status = kernel_task_mm_borrow_mutable(caller, &mm);
        if (task_status != KERNEL_TASK_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
        access_status = kernel_copy_to_user(mm,
                                            tms_address,
                                            &value,
                                            sizeof(value),
                                            &copied);
        if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
            decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
            decoded->value = -KERNEL_EFAULT;
            return KERNEL_SYSCALL_STATUS_OK;
        }
        if (access_status != KERNEL_UACCESS_STATUS_OK ||
            copied != sizeof(value)) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    }
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = (int64_t)kernel_tick_count();
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_restart_syscall(
    struct kernel_task *caller,
    struct kernel_syscall_result *decoded)
{
    enum kernel_wait_wake_reason wake_reason = KERNEL_WAIT_WOKEN;
    enum kernel_scheduler_status sleep_status;
    uint64_t deadline;
    uint64_t remaining_address;
    uint64_t futex_address;
    uint64_t futex_deadline_ns;
    uint32_t futex_operation;
    uint32_t futex_expected;

    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    if (kernel_signal_futex_timed_restart(caller, &futex_address,
                                           &futex_operation,
                                           &futex_expected,
                                           &futex_deadline_ns)) {
        enum kernel_scheduler_status futex_status;

        decoded->value = kernel_futex_restart_timed(
            caller, futex_address, futex_operation, futex_expected,
            futex_deadline_ns, &futex_status);
        if (futex_status != KERNEL_SCHEDULER_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        if (decoded->value != -KERNEL_ERESTARTSYS)
            kernel_signal_clear_syscall_restart(caller);
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (!kernel_signal_nanosleep_restart(caller, &deadline,
                                         &remaining_address)) {
        decoded->value = -KERNEL_ENOSYS;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (deadline == 0U ||
        (int64_t)(riscv_time_read() - deadline) >= 0) {
        kernel_signal_clear_syscall_restart(caller);
        decoded->value = 0;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    sleep_status = kernel_scheduler_block_current(0, deadline, 1,
                                                  &wake_reason);
    if (sleep_status != KERNEL_SCHEDULER_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    if (wake_reason == KERNEL_WAIT_SIGNALLED) {
        struct kernel_mm *mm;
        uint64_t now_ns = kernel_time_monotonic_ns();
        uint64_t target_ns = kernel_time_ticks_to_ns(deadline);
        uint64_t remaining_ns = target_ns > now_ns
                                    ? target_ns - now_ns
                                    : 0U;

        if (remaining_address != 0U) {
            struct linux_timespec remaining = {
                .tv_sec = (int64_t)(remaining_ns /
                                    LINUX_CLOCK_NSECS_PER_SEC),
                .tv_nsec = (int64_t)(remaining_ns %
                                     LINUX_CLOCK_NSECS_PER_SEC),
            };
            enum kernel_task_status task_status =
                kernel_task_mm_borrow_mutable(caller, &mm);
            enum kernel_uaccess_status access_status;
            size_t copied = 0U;

            if (task_status != KERNEL_TASK_STATUS_OK) {
                return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
            }
            access_status = kernel_copy_to_user(mm,
                                                remaining_address,
                                                &remaining,
                                                sizeof(remaining),
                                                &copied);
            if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
                kernel_signal_clear_syscall_restart(caller);
                decoded->value = -KERNEL_EFAULT;
                return KERNEL_SYSCALL_STATUS_OK;
            }
            if (access_status != KERNEL_UACCESS_STATUS_OK ||
                copied != sizeof(remaining)) {
                return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
            }
        }
        decoded->value = -KERNEL_ERESTARTSYS;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    kernel_signal_clear_syscall_restart(caller);
    decoded->value = 0;
    return KERNEL_SYSCALL_STATUS_OK;
}

/*
 * Shared core of nanosleep(101) and clock_nanosleep(115).  The requested
 * point is normalized to the monotonic domain, the sleep blocks on the
 * timer deadline, and a signal wake records an internal restart until the
 * common user-return path selects handler EINTR or no-handler restart.
 */
enum kernel_syscall_status syscall_handle_sleep_for(
    struct kernel_task *caller,
    uint32_t clock_id,
    uint32_t flags,
    uint64_t request_address,
    uint64_t remaining_address,
    struct kernel_syscall_result *decoded)
{
    struct kernel_mm *mm;
    struct linux_timespec request_spec;
    enum kernel_task_status task_status;
    enum kernel_uaccess_status access_status;
    enum kernel_time_status time_status;
    enum kernel_scheduler_status sleep_status;
    enum kernel_wait_wake_reason wake_reason = KERNEL_WAIT_WOKEN;
    size_t copied;
    uint64_t duration_ns;
    uint64_t target_monotonic_ns;
    uint64_t deadline;
    uint64_t now_ns;

    if (clock_id != LINUX_CLOCK_REALTIME && clock_id != LINUX_CLOCK_MONOTONIC) {
        decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
        decoded->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if ((flags & ~LINUX_TIMER_ABSTIME) != 0U) {
        decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
        decoded->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }

    task_status = kernel_task_mm_borrow_mutable(caller, &mm);
    if (task_status != KERNEL_TASK_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    access_status = kernel_copy_from_user(mm,
                                          &request_spec,
                                          request_address,
                                          sizeof(request_spec),
                                          &copied);
    if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
        decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
        decoded->value = -KERNEL_EFAULT;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (access_status != KERNEL_UACCESS_STATUS_OK ||
        copied != sizeof(request_spec)) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    if (request_spec.tv_sec < 0 || request_spec.tv_nsec < 0 ||
        (uint64_t)request_spec.tv_nsec >= LINUX_CLOCK_NSECS_PER_SEC) {
        decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
        decoded->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }

    {
        unsigned __int128 requested =
            (unsigned __int128)(uint64_t)request_spec.tv_sec *
                LINUX_CLOCK_NSECS_PER_SEC + (uint64_t)request_spec.tv_nsec;

        duration_ns = requested > INT64_MAX ? INT64_MAX : (uint64_t)requested;
    }
    if ((flags & LINUX_TIMER_ABSTIME) != 0U) {
        target_monotonic_ns = clock_id == LINUX_CLOCK_REALTIME
                                  ? duration_ns -
                                        kernel_time_boot_realtime_offset()
                                  : duration_ns;
        if (clock_id == LINUX_CLOCK_REALTIME &&
            duration_ns < kernel_time_boot_realtime_offset()) {
            target_monotonic_ns = 0;
        }
    } else {
        now_ns = kernel_time_monotonic_ns();
        target_monotonic_ns = duration_ns > (uint64_t)INT64_MAX - now_ns
                                  ? INT64_MAX
                                  : now_ns + duration_ns;
    }

    time_status = kernel_time_deadline_from_monotonic(target_monotonic_ns,
                                                      &deadline);
    if (time_status == KERNEL_TIME_STATUS_DEADLINE_PASSED) {
        decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
        decoded->value = 0;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (time_status != KERNEL_TIME_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }

    sleep_status = kernel_scheduler_block_current(0, deadline, 1, &wake_reason);
    if (sleep_status != KERNEL_SCHEDULER_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }

    if ((wake_reason == KERNEL_WAIT_WOKEN ||
         wake_reason == KERNEL_WAIT_SIGNALLED) &&
        (flags & LINUX_TIMER_ABSTIME) == 0U && remaining_address != 0U) {
        uint64_t now = kernel_time_monotonic_ns();
        uint64_t remaining_ns = target_monotonic_ns > now
                                    ? target_monotonic_ns - now
                                    : 0U;

        {
            int64_t seconds;
            int64_t sub_nanoseconds;

            syscall_split_nanoseconds(remaining_ns,
                                     &seconds,
                                     &sub_nanoseconds);
            access_status = kernel_copy_to_user(mm,
                                                remaining_address,
                                                &(struct linux_timespec){
                                                    .tv_sec = seconds,
                                                    .tv_nsec = sub_nanoseconds,
                                                },
                                                sizeof(struct linux_timespec),
                                                &copied);
            if (access_status != KERNEL_UACCESS_STATUS_OK ||
                copied != sizeof(struct linux_timespec)) {
                if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
                    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
                    decoded->value = -KERNEL_EFAULT;
                    return KERNEL_SYSCALL_STATUS_OK;
                }
                return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
            }
        }
    }

    if (wake_reason == KERNEL_WAIT_SIGNALLED) {
        kernel_signal_note_nanosleep_restart(caller,
                                             deadline,
                                             (flags & LINUX_TIMER_ABSTIME) != 0U
                                                 ? 0U : remaining_address);
        decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
        decoded->value = -KERNEL_ERESTARTSYS;
        return KERNEL_SYSCALL_STATUS_OK;
    }

    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = 0;
    return KERNEL_SYSCALL_STATUS_OK;
}
