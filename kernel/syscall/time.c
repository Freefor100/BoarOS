#include <kernel/posix_timer.h>
#include <kernel/event_fd.h>
#include <kernel/credentials.h>
#include <kernel/sync.h>
#include <kernel/accounting.h>
#include "private.h"

#include <arch/timer.h>
#include <arch/context.h>
#include <kernel/errno.h>
#include <kernel/futex.h>
#include <kernel/mm.h>
#include <kernel/scheduler.h>
#include <kernel/signal.h>
#include <kernel/task.h>
#include <kernel/tick.h>
#include <kernel/time.h>
#include <kernel/timex.h>
#include <kernel/uaccess.h>
#include <kernel/wait_internal.h>

#include <stddef.h>
#include <stdint.h>

#define LINUX_CLOCK_REALTIME 0U
#define LINUX_CLOCK_MONOTONIC 1U
#define LINUX_CLOCK_MONOTONIC_RAW 4U
#define LINUX_CLOCK_REALTIME_COARSE 5U
#define LINUX_CLOCK_MONOTONIC_COARSE 6U
#define LINUX_CLOCK_BOOTTIME 7U
#define LINUX_CLOCK_NSECS_PER_SEC UINT64_C(1000000000)
#define LINUX_TIMER_ABSTIME UINT64_C(1)

struct linux_itimerval { int64_t interval_seconds, interval_usec, seconds, usec; };
static uint64_t itimer_ns(int64_t seconds, int64_t usec)
{
    uint64_t ns = (uint64_t)usec * 1000U;
    return (uint64_t)seconds > ((uint64_t)INT64_MAX - ns) / 1000000000U ?
        INT64_MAX : (uint64_t)seconds * 1000000000U + ns;
}

enum kernel_syscall_status syscall_handle_itimer(struct kernel_task *caller,
    const struct kernel_syscall_request *request, struct kernel_syscall_result *decoded)
{
    struct kernel_mm *mm;
    struct linux_itimerval input={0}, output={0};
    uint64_t remaining=0, interval=0;
    size_t copied=0;
    int setting = request->number == 103U;
    int which = (int32_t)request->arguments[0];
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = -KERNEL_EFAULT;
    if (kernel_task_mm_borrow_mutable(caller,&mm) != KERNEL_TASK_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    if (setting && request->arguments[1] != 0) {
        if (kernel_copy_from_user(mm,&input,request->arguments[1],sizeof(input),&copied) !=
            KERNEL_UACCESS_STATUS_OK || copied != sizeof(input)) return KERNEL_SYSCALL_STATUS_OK;
        if (input.seconds < 0 || input.interval_seconds < 0 ||
            (uint64_t)input.usec >= 1000000U || (uint64_t)input.interval_usec >= 1000000U) {
            decoded->value = -KERNEL_EINVAL; return KERNEL_SYSCALL_STATUS_OK;
        }
    }
    if (which < 0 || which > 2) {
        decoded->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    {
        KERNEL_SERVICE_SCOPE_IF(service, which != 0);
        uintptr_t irq = arch_interrupt_save();
        if (setting) kernel_signal_timer_set(caller,(uint32_t)which,itimer_ns(input.seconds,input.usec),
            itimer_ns(input.interval_seconds,input.interval_usec),&remaining,&interval);
        else kernel_signal_timer_get(caller,(uint32_t)which,&remaining,&interval);
        arch_interrupt_restore(irq);
    }
    output.seconds = (int64_t)(remaining/1000000000U);
    output.usec = (int64_t)((remaining%1000000000U)/1000U);
    output.interval_seconds = (int64_t)(interval/1000000000U);
    output.interval_usec = (int64_t)((interval%1000000000U)/1000U);
    uint64_t address = request->arguments[setting ? 2 : 1];
    if (!setting || address) {
        /* Linux 先安装新值，再复制旧值；输出 fault 不回滚已经生效的 alarm。 */
        if (kernel_copy_to_user(mm,address,&output,sizeof(output),&copied) != KERNEL_UACCESS_STATUS_OK ||
            copied != sizeof(output)) return KERNEL_SYSCALL_STATUS_OK;
    }
    decoded->value=0;
    return KERNEL_SYSCALL_STATUS_OK;
}

static int syscall_clock_id_coarse(uint64_t clock_id)
{
    return clock_id == LINUX_CLOCK_REALTIME_COARSE ||
           clock_id == LINUX_CLOCK_MONOTONIC_COARSE;
}

/* Readable clocks and sleep-capable clocks deliberately have separate checks. */
static int syscall_clock_id_valid(uint64_t clock_id)
{
    return clock_id == LINUX_CLOCK_REALTIME || clock_id == LINUX_CLOCK_MONOTONIC ||
           clock_id == LINUX_CLOCK_MONOTONIC_RAW ||
           clock_id == LINUX_CLOCK_BOOTTIME ||
           clock_id == KERNEL_CLOCK_TAI ||
           (kernel_clock_is_alarm((int32_t)clock_id) && kernel_time_has_rtc()) ||
           syscall_clock_id_coarse(clock_id);
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

    int32_t clock = (int32_t)request->arguments[0];
    if (!syscall_clock_id_valid((uint64_t)(int64_t)clock)) {
        int error = kernel_task_cpu_clock(caller, clock, 0, &nanoseconds);
        if (error) {
            decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
            decoded->value = error;
            return KERNEL_SYSCALL_STATUS_OK;
        }
    } else switch (clock) {
    case LINUX_CLOCK_REALTIME:
    case KERNEL_CLOCK_REALTIME_ALARM: nanoseconds = kernel_time_realtime_ns(); break;
    case KERNEL_CLOCK_TAI: nanoseconds = kernel_time_tai_ns(); break;
    /* No suspend support: boottime has no suspended interval to add. */
    case LINUX_CLOCK_BOOTTIME:
    case KERNEL_CLOCK_BOOTTIME_ALARM:
    case LINUX_CLOCK_MONOTONIC: nanoseconds = kernel_time_monotonic_ns(); break;
    case LINUX_CLOCK_MONOTONIC_RAW: nanoseconds = kernel_time_raw_ns(); break;
    case LINUX_CLOCK_REALTIME_COARSE: nanoseconds = kernel_time_coarse_realtime_ns(); break;
    default: nanoseconds = kernel_time_coarse_monotonic_ns(); break;
    }
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
    int32_t clock = (int32_t)request->arguments[0];
    uint64_t resolution;
    if (syscall_clock_id_valid((uint64_t)(int64_t)clock))
        resolution = syscall_clock_id_coarse(clock) ? kernel_time_coarse_resolution_ns() : 1;
    else {
        int error = kernel_task_cpu_clock(caller, clock, 1, &resolution);
        if (error) {
            decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
            decoded->value = error;
            return KERNEL_SYSCALL_STATUS_OK;
        }
    }
    if (request->arguments[1] == 0U) {
        decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
        decoded->value = 0;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    return syscall_return_time(caller, request->arguments[1], 0, (int64_t)resolution, decoded);
}

enum kernel_syscall_status syscall_handle_gettimeofday(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    uint64_t nanoseconds;
    int64_t seconds;
    int64_t sub_nanoseconds;

    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = 0;
    if (request->arguments[0] != 0U) {
        nanoseconds = kernel_time_realtime_ns();
        syscall_split_nanoseconds(nanoseconds, &seconds, &sub_nanoseconds);
        enum kernel_syscall_status status = syscall_return_time(caller,
            request->arguments[0], seconds, sub_nanoseconds / 1000, decoded);
        /* Linux writes timeval first; a failed write leaves timezone untouched. */
        if (status != KERNEL_SYSCALL_STATUS_OK || decoded->value != 0)
            return status;
    }
    if (request->arguments[1] != 0U) {
        struct kernel_mm *mm;
        int32_t timezone[2];
        kernel_time_get_timezone(timezone);
        size_t copied;
        if (kernel_task_mm_borrow_mutable(caller, &mm) != KERNEL_TASK_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        enum kernel_uaccess_status access = kernel_copy_to_user(mm,
            request->arguments[1], timezone, sizeof(timezone), &copied);
        if (access == KERNEL_UACCESS_STATUS_FAULT) decoded->value = -KERNEL_EFAULT;
        else if (access != KERNEL_UACCESS_STATUS_OK || copied != sizeof(timezone))
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    return KERNEL_SYSCALL_STATUS_OK;
}

struct linux_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

/* Serialize a complete wall step, including timer indexes and notifications.
 * No raw lock spans callbacks; ordinary time reads never take this mutex. */
static struct kernel_mutex clock_control={.lock={
    .metadata={.rank=KERNEL_RAW_RANK_OBJECT,.initialized=KERNEL_RAW_INITIALIZED},
    .waiters=KERNEL_WAIT_QUEUE_STATIC_INITIALIZER,.rank=1}};
static int timezone_initialized;
static int clock_step(uint64_t ns)
{
    int error=kernel_scheduler_set_realtime(ns);
    if (error) return error;
    kernel_timer_fd_clock_changed();
    kernel_posix_timer_clock_changed();
    return 0;
}
enum kernel_syscall_status syscall_handle_settime(struct kernel_task *caller,
    const struct kernel_syscall_request *request,struct kernel_syscall_result *decoded)
{
    struct kernel_mm *mm;size_t copied;
    struct linux_timespec value={0};int32_t timezone[2]={0};
    int clock=request->number==112U;
    uint64_t address=request->arguments[clock?1:0],tz=clock?0:request->arguments[1];
    decoded->action=KERNEL_SYSCALL_ACTION_RETURN;decoded->value=-KERNEL_EINVAL;
    if (clock && (int32_t)request->arguments[0]!=LINUX_CLOCK_REALTIME) return KERNEL_SYSCALL_STATUS_OK;
    if (kernel_task_mm_borrow_mutable(caller,&mm)) return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    if (clock || address) {
        if (kernel_copy_from_user(mm,&value,address,sizeof(value),&copied)) {
            decoded->value=-KERNEL_EFAULT;return KERNEL_SYSCALL_STATUS_OK;
        }
        if (!clock && (uint64_t)value.tv_nsec>=1000000) return KERNEL_SYSCALL_STATUS_OK;
        if (!clock) value.tv_nsec*=1000;
    }
    if (tz && kernel_copy_from_user(mm,timezone,tz,sizeof(timezone),&copied)) {
        decoded->value=-KERNEL_EFAULT;return KERNEL_SYSCALL_STATUS_OK;
    }
    /* Linux reserves thirty years of signed ktime for future uptime. */
    if ((clock||address) && (value.tv_sec<0 || (uint64_t)value.tv_sec>=8277292036ULL ||
            (uint64_t)value.tv_nsec>=1000000000)) return KERNEL_SYSCALL_STATUS_OK;
    decoded->value=-KERNEL_EPERM;
    if (!kernel_credentials_capable(kernel_credentials_current(),KERNEL_CAP_SYS_TIME)) return KERNEL_SYSCALL_STATUS_OK;
    decoded->value=-KERNEL_EINVAL;
    if (tz && (timezone[0]<-900 || timezone[0]>900)) return KERNEL_SYSCALL_STATUS_OK;
    KERNEL_LOCK_SCOPE(guard);kernel_mutex_lock(&clock_control,&guard);
    if (tz) {
        kernel_time_set_timezone(timezone);
        int warp=!timezone_initialized && !address;
        timezone_initialized=1;
        if (warp) {
            __int128 ns=(__int128)kernel_time_realtime_ns()+(int64_t)timezone[0]*INT64_C(60000000000);
            if (ns>=0 && ns<=INT64_MAX) (void)clock_step((uint64_t)ns);
        }
    }
    decoded->value=(clock||address)?clock_step((uint64_t)value.tv_sec*1000000000+(uint64_t)value.tv_nsec):0;
    return KERNEL_SYSCALL_STATUS_OK;
}
enum kernel_syscall_status syscall_handle_adjtimex(struct kernel_task *caller,
    const struct kernel_syscall_request *request,struct kernel_syscall_result *decoded)
{
    struct kernel_mm *mm;
    struct kernel_timex tx;
    size_t copied;
    int clock_call=request->number==266U;
    uint64_t address=request->arguments[clock_call?1:0];
    decoded->action=KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value=-KERNEL_EFAULT;
    if (kernel_task_mm_borrow_mutable(caller,&mm)) return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    if (kernel_copy_from_user(mm,&tx,address,sizeof(tx),&copied) || copied!=sizeof(tx))
        return KERNEL_SYSCALL_STATUS_OK;
    int32_t id=clock_call?(int32_t)request->arguments[0]:0;
    if (id) {
        /* There is no dynamic POSIX clock device. CPU clocks (including
         * negative encoded IDs) and other standard clocks have no adjust op. */
        int known=(id>=1 && id<=9) || id==11 || (id<0 && (id&7)!=3);
        decoded->value=known?-KERNEL_EOPNOTSUPP:-KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if ((tx.modes&KADJ_ADJTIME) && !(tx.modes&KADJ_OFFSET)) {
        decoded->value=-KERNEL_EINVAL;
        goto copy_out;
    }
    if (!kernel_timex_read_only(tx.modes) &&
        !kernel_credentials_capable(kernel_credentials_current(),KERNEL_CAP_SYS_TIME)) {
        decoded->value=-KERNEL_EPERM;
        goto copy_out;
    }
    {
        KERNEL_LOCK_SCOPE(guard);
        kernel_mutex_lock(&clock_control,&guard);
        enum kernel_clock_change change;
        if (kernel_timex_read_only(tx.modes))
            decoded->value=kernel_time_adjust(&tx,&change);
        else
            decoded->value=kernel_scheduler_adjust_time(&tx,&change);
        if (decoded->value >= 0) {
            if (change & (KERNEL_CLOCK_RATE | KERNEL_CLOCK_STEP)) {
                kernel_timer_fd_reindex(!!(change & KERNEL_CLOCK_STEP));
                kernel_posix_timer_clock_changed();
            } else if (change & KERNEL_CLOCK_TAI_OFFSET) {
                kernel_posix_timer_tai_changed();
            }
        }
    }
copy_out:
    if ((!clock_call || decoded->value>=0) && (kernel_copy_to_user(mm,address,&tx,sizeof(tx),&copied) ||
                             copied!=sizeof(tx))) decoded->value=-KERNEL_EFAULT;
    return KERNEL_SYSCALL_STATUS_OK;
}

static int alarm_permission(void)
{
    return kernel_credentials_capable(kernel_credentials_current(), KERNEL_CAP_WAKE_ALARM) ?
        0 : -KERNEL_EPERM;
}

static enum kernel_scheduler_status sleep_clock(enum kernel_time_domain domain, uint64_t ns,
                                                enum kernel_wait_wake_reason *reason)
{
    struct kernel_wait_token token;
    enum kernel_scheduler_status status;
    if (domain == KERNEL_TIME_TAI)
        status = kernel_wait_prepare_clock(0, ns, 1, &token, kernel_time_deadline_from_tai);
    else if (domain == KERNEL_TIME_REALTIME)
        status = kernel_wait_prepare_realtime(0, ns, 1, &token);
    else status = kernel_wait_prepare_monotonic(0, ns, 1, &token);
    if (status) return status;
    status=kernel_wait_park(&token,reason);
    if (kernel_wait_finish(&token)) __builtin_trap();
    return status;
}

struct linux_tms {
    int64_t utime;
    int64_t stime;
    int64_t cutime;
    int64_t cstime;
};

/*
 * times(153): fills the optional tms buffer by converting nanosecond CPU
 * execution accounting (CLK_TCK = KERNEL_TICKS_PER_SECOND) and returns the tick
 * count since boot.  A NULL tms pointer only samples the uptime ticks.
 */
enum kernel_syscall_status syscall_handle_times(
    struct kernel_task *caller,
    uint64_t tms_address,
    struct kernel_syscall_result *decoded)
{
    KERNEL_SERVICE_SCOPE(service);
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

static enum kernel_syscall_status syscall_cpu_sleep(struct kernel_task *caller,
    int32_t clock,int absolute,int original_absolute,uint64_t value,uint64_t remaining_address,
    struct kernel_syscall_result *decoded)
{
    uint64_t deadline=0,remaining=0;
    decoded->action=KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value=kernel_posix_cpu_sleep(caller,clock,absolute,value,&deadline,&remaining);
    if(decoded->value!=-KERNEL_ERESTARTSYS)return KERNEL_SYSCALL_STATUS_OK;
    if(remaining_address) {
        struct kernel_mm *mm;size_t copied=0;
        struct linux_timespec result={remaining/1000000000,remaining%1000000000};
        if(kernel_task_mm_borrow_mutable(caller,&mm))return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        enum kernel_uaccess_status status=kernel_copy_to_user(mm,remaining_address,&result,sizeof(result),&copied);
        if(status==KERNEL_UACCESS_STATUS_FAULT){decoded->value=-KERNEL_EFAULT;return KERNEL_SYSCALL_STATUS_OK;}
        if(status||copied!=sizeof(result))return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    kernel_signal_note_nanosleep_restart(caller,clock,original_absolute,deadline,remaining_address);
    return KERNEL_SYSCALL_STATUS_OK;
}

static enum kernel_syscall_status syscall_wait_clock(struct kernel_task *caller,
    int32_t clock, enum kernel_time_domain domain, int absolute, uint64_t deadline,
    uint64_t remaining_address,
    struct kernel_syscall_result *decoded)
{
    enum kernel_wait_wake_reason reason;
    if (sleep_clock(domain, deadline, &reason) != KERNEL_SCHEDULER_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;

    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = 0;
    if (reason != KERNEL_WAIT_SIGNALLED) return KERNEL_SYSCALL_STATUS_OK;

    if (remaining_address) {
        uint64_t now = kernel_time_domain_now(domain);
        int64_t seconds, sub_nanoseconds;
        syscall_split_nanoseconds(deadline > now ? deadline - now : 0, &seconds, &sub_nanoseconds);
        enum kernel_syscall_status status = syscall_return_time(
            caller, remaining_address, seconds, sub_nanoseconds, decoded);
        if (status != KERNEL_SYSCALL_STATUS_OK || decoded->value) return status;
    }
    kernel_signal_note_nanosleep_restart(caller, clock, absolute, deadline, remaining_address);
    decoded->value = -KERNEL_ERESTARTSYS;
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_restart_syscall(
    struct kernel_task *caller,
    struct kernel_syscall_result *decoded)
{
    int32_t clock_id;
    uint64_t deadline;
    uint64_t remaining_address;
    uint64_t futex_address;
    uint64_t futex_deadline_ns;
    uint32_t futex_operation;
    uint32_t futex_expected;
    uint32_t futex_bitset;

    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    if (kernel_signal_futex_timed_restart(caller, &futex_address,
                                           &futex_operation,
                                           &futex_expected,
                                           &futex_deadline_ns,
                                           &futex_bitset)) {
        enum kernel_scheduler_status futex_status;

        decoded->value = kernel_futex_restart_timed(
            caller, futex_address, futex_operation, futex_expected,
            futex_deadline_ns, futex_bitset, &futex_status);
        if (futex_status != KERNEL_SCHEDULER_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        if (decoded->value != -KERNEL_ERESTARTSYS)
            kernel_signal_clear_syscall_restart(caller);
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (!kernel_signal_nanosleep_restart(caller, &clock_id, &deadline,
                                         &remaining_address)) {
        decoded->value = -KERNEL_ENOSYS;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if(clock_id==2||clock_id<0) {
        enum kernel_syscall_status status=syscall_cpu_sleep(caller,clock_id,1,0,deadline,remaining_address,decoded);
        if(decoded->value!=-KERNEL_ERESTARTSYS)kernel_signal_clear_syscall_restart(caller);
        return status;
    }
    enum kernel_time_domain domain = kernel_time_timer_domain(clock_id, 1);
    if (kernel_time_domain_now(domain) >= deadline) {
        kernel_signal_clear_syscall_restart(caller);
        decoded->value = 0;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    enum kernel_syscall_status status = syscall_wait_clock(
        caller, clock_id, domain, 0, deadline, remaining_address, decoded);
    if (status != KERNEL_SYSCALL_STATUS_OK || decoded->value != -KERNEL_ERESTARTSYS)
        kernel_signal_clear_syscall_restart(caller);
    return status;
}

/*
 * Shared core of nanosleep(101) and clock_nanosleep(115).  The requested
 * point retains its absolute clock domain, the sleep blocks on the
 * timer deadline on the calling CPU, and a signal wake records an internal
 * restart until the common user-return path selects handler EINTR or
 * no-handler restart. Deadline publication notifies CPU0 to rearm its timer.
 */
enum kernel_syscall_status syscall_handle_sleep_for(
    struct kernel_task *caller,
    uint32_t clock_id,
    uint32_t flags,
    uint64_t request_address,
    uint64_t remaining_address,
    struct kernel_syscall_result *decoded)
{
    int cpu_clock=clock_id==2U||(int32_t)clock_id<0;
    struct kernel_mm *mm;
    struct linux_timespec request_spec;
    enum kernel_task_status task_status;
    enum kernel_uaccess_status access_status;
    size_t copied;
    int alarm = kernel_clock_is_alarm(clock_id);

    if (syscall_clock_id_coarse(clock_id) || clock_id == 3U ||
        clock_id == LINUX_CLOCK_MONOTONIC_RAW) {
        decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
        decoded->value = -KERNEL_EOPNOTSUPP;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (!cpu_clock && !alarm && clock_id != LINUX_CLOCK_REALTIME &&
        clock_id != LINUX_CLOCK_MONOTONIC && clock_id != LINUX_CLOCK_BOOTTIME &&
        clock_id != KERNEL_CLOCK_TAI) {
        decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
        decoded->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (!cpu_clock && !alarm && clock_id != LINUX_CLOCK_BOOTTIME &&
        clock_id != KERNEL_CLOCK_TAI && (flags & ~LINUX_TIMER_ABSTIME)) {
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

    if (alarm) {
        int error = !kernel_time_has_rtc() ? -KERNEL_EOPNOTSUPP :
            flags & ~LINUX_TIMER_ABSTIME ? -KERNEL_EINVAL : alarm_permission();
        if (error) {
            decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
            decoded->value = error;
            return KERNEL_SYSCALL_STATUS_OK;
        }
    }

    unsigned __int128 requested =
        (unsigned __int128)(uint64_t)request_spec.tv_sec *
        LINUX_CLOCK_NSECS_PER_SEC + (uint64_t)request_spec.tv_nsec;
    uint64_t deadline = requested > INT64_MAX ? INT64_MAX : (uint64_t)requested;
    int absolute = !!(flags & LINUX_TIMER_ABSTIME);
    if (absolute) remaining_address = 0;
    if (cpu_clock)
        return syscall_cpu_sleep(caller, (int32_t)clock_id, absolute, absolute,
                                 deadline, remaining_address, decoded);

    /* Relative REALTIME is monotonic. The other new clock IDs keep their
     * domain through timer arming and through a restart after a stop. */
    if (!absolute && clock_id == LINUX_CLOCK_REALTIME) clock_id = LINUX_CLOCK_MONOTONIC;
    enum kernel_time_domain domain = kernel_time_timer_domain(clock_id, 1);
    if (!absolute) {
        uint64_t now = kernel_time_domain_now(domain);
        deadline = now >= (uint64_t)INT64_MAX || deadline > (uint64_t)INT64_MAX - now ?
            INT64_MAX : deadline + now;
    }
    return syscall_wait_clock(caller, clock_id, domain, absolute, deadline, remaining_address, decoded);
}

enum kernel_syscall_status syscall_handle_getrusage(struct kernel_task *caller,
    const struct kernel_syscall_request *request, struct kernel_syscall_result *decoded)
{
    struct kernel_linux_rusage usage;
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = kernel_task_getrusage(caller, (int32_t)request->arguments[0], &usage);
    if (decoded->value) return KERNEL_SYSCALL_STATUS_OK;
    struct kernel_mm *mm;size_t copied=0;
    if (kernel_task_mm_borrow_mutable(caller,&mm) != KERNEL_TASK_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    enum kernel_uaccess_status status=kernel_copy_to_user(mm,request->arguments[1],&usage,sizeof(usage),&copied);
    if (status==KERNEL_UACCESS_STATUS_FAULT) decoded->value=-KERNEL_EFAULT;
    else if (status || copied!=sizeof(usage)) return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    return KERNEL_SYSCALL_STATUS_OK;
}

static enum kernel_syscall_status posix_copy(struct kernel_mm *mm,void *value,uint64_t address,size_t length,int output,int *error)
{
    size_t copied=0;
    enum kernel_uaccess_status status=output?kernel_copy_to_user(mm,address,value,length,&copied):
        kernel_copy_from_user(mm,value,address,length,&copied);
    if(status==KERNEL_UACCESS_STATUS_FAULT){*error=-KERNEL_EFAULT;return KERNEL_SYSCALL_STATUS_OK;}
    if(status!=KERNEL_UACCESS_STATUS_OK||copied!=length)return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    *error=0;return KERNEL_SYSCALL_STATUS_OK;
}
enum kernel_syscall_status syscall_handle_posix_timer(struct kernel_task *caller,
    const struct kernel_syscall_request *request,struct kernel_syscall_result *decoded)
{
    struct kernel_mm *mm;int error=0;int32_t id=(int32_t)request->arguments[0];
    decoded->action=KERNEL_SYSCALL_ACTION_RETURN;
    if(kernel_task_mm_borrow_mutable(caller,&mm)!=KERNEL_TASK_STATUS_OK)return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    if(request->number==107) {
        int32_t clock = id;
        struct kernel_posix_event event,*input=0;
        struct kernel_posix_timer *timer=0;
        if(request->arguments[1]) {
            if(posix_copy(mm,&event,request->arguments[1],sizeof(event),0,&error)!=KERNEL_SYSCALL_STATUS_OK)
                return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
            input=&event;
        }
        if(!error)error=kernel_posix_timer_prepare(caller,id,input,&id,&timer);
        if(!error) {
            enum kernel_syscall_status status=posix_copy(mm,&id,request->arguments[2],sizeof(id),1,&error);
            /* Linux validates alarm availability and permission at activation,
             * after copying out the reserved timer ID. */
            if (status == KERNEL_SYSCALL_STATUS_OK && !error && kernel_clock_is_alarm(clock))
                error = kernel_time_has_rtc() ? alarm_permission() : -KERNEL_EOPNOTSUPP;
            kernel_posix_timer_publish(timer,!error&&status==KERNEL_SYSCALL_STATUS_OK);
            if(status!=KERNEL_SYSCALL_STATUS_OK)return status;
        }
    } else if(request->number==108) {
        struct kernel_posix_itimerspec value;
        error=kernel_posix_timer_get(caller,id,&value);
        if(!error&&posix_copy(mm,&value,request->arguments[1],sizeof(value),1,&error)!=KERNEL_SYSCALL_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    } else if(request->number==109)error=kernel_posix_timer_overrun(caller,id);
    else if(request->number==111)error=kernel_posix_timer_delete(caller,id);
    else {
        if(!request->arguments[2]){decoded->value=-KERNEL_EINVAL;return KERNEL_SYSCALL_STATUS_OK;}
        struct kernel_posix_itimerspec input,output;
        if(posix_copy(mm,&input,request->arguments[2],sizeof(input),0,&error)!=KERNEL_SYSCALL_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        if(!error)error=kernel_posix_timer_set(caller,id,(int32_t)request->arguments[1],&input,&output);
        if(!error&&request->arguments[3]&&posix_copy(mm,&output,request->arguments[3],sizeof(output),1,&error)!=KERNEL_SYSCALL_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    decoded->value=error;return KERNEL_SYSCALL_STATUS_OK;
}
