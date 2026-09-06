#include <kernel/errno.h>
#include <kernel/exec.h>
#include <kernel/files.h>
#include <kernel/fs_context.h>
#include <kernel/mm.h>
#include <kernel/open_file.h>
#include <kernel/page.h>
#include <kernel/scheduler.h>
#include <kernel/syscall.h>
#include <kernel/task.h>
#include <kernel/tick.h>
#include <kernel/time.h>
#include <kernel/uaccess.h>

#include <stddef.h>
#include <stdint.h>

#define LINUX_SYSCALL_DUP 23U
#define LINUX_SYSCALL_DUP3 24U
#define LINUX_SYSCALL_FCNTL 25U
#define LINUX_SYSCALL_OPENAT 56U
#define LINUX_SYSCALL_CLOSE 57U
#define LINUX_SYSCALL_GETDENTS64 61U
#define LINUX_SYSCALL_LSEEK 62U
#define LINUX_SYSCALL_READ 63U
#define LINUX_SYSCALL_WRITE 64U
#define LINUX_SYSCALL_WRITEV 66U
#define LINUX_SYSCALL_EXIT 93U
#define LINUX_SYSCALL_EXIT_GROUP 94U
#define LINUX_SYSCALL_SET_TID_ADDRESS 96U
#define LINUX_SYSCALL_UNAME 160U
#define LINUX_SYSCALL_GETPID 172U
#define LINUX_SYSCALL_GETPPID 173U
#define LINUX_SYSCALL_GETTID 178U
#define LINUX_SYSCALL_BRK 214U
#define LINUX_SYSCALL_SCHED_YIELD 124U
#define LINUX_SYSCALL_CLOCK_GETTIME 113U
#define LINUX_SYSCALL_CLOCK_GETRES 114U
#define LINUX_SYSCALL_NANOSLEEP 101U
#define LINUX_SYSCALL_CLOCK_NANOSLEEP 115U
#define LINUX_SYSCALL_GETTIMEOFDAY 169U
#define LINUX_SYSCALL_TIMES 153U
#define LINUX_CLOCK_REALTIME 0U
#define LINUX_CLOCK_MONOTONIC 1U
#define LINUX_CLOCK_NSECS_PER_SEC UINT64_C(1000000000)
#define LINUX_TIMER_ABSTIME UINT64_C(1)
#define LINUX_SYSCALL_MUNMAP 215U
#define LINUX_SYSCALL_CLONE 220U
#define LINUX_SYSCALL_EXECVE 221U
#define LINUX_SYSCALL_MMAP 222U
#define LINUX_SYSCALL_MPROTECT 226U
#define LINUX_SYSCALL_WAIT4 260U
#define LINUX_SYSCALL_NEWFSTATAT 79U
#define LINUX_SYSCALL_FSTAT 80U
#define LINUX_SYSCALL_DUP2 33U
#define LINUX_EXIT_STATUS_MASK UINT64_C(0xff)
#define LINUX_CLONE_SIGNAL_MASK UINT64_C(0xff)
#define LINUX_SIGCHLD UINT64_C(17)
#define LINUX_CLONE_VM UINT64_C(0x100)
#define LINUX_CLONE_VFORK UINT64_C(0x4000)
#define LINUX_CLONE_KNOWN_FLAGS UINT64_C(0x3ffffffff)
#define LINUX_UTS_FIELD_SIZE 65U
#define LINUX_PROT_READ UINT64_C(0x1)
#define LINUX_PROT_WRITE UINT64_C(0x2)
#define LINUX_PROT_EXEC UINT64_C(0x4)
#define LINUX_MAP_SHARED UINT64_C(0x1)
#define LINUX_MAP_PRIVATE UINT64_C(0x2)
#define LINUX_MAP_TYPE_MASK UINT64_C(0x3)
#define LINUX_MAP_FIXED UINT64_C(0x10)
#define LINUX_MAP_ANONYMOUS UINT64_C(0x20)
#define LINUX_MAP_NORESERVE UINT64_C(0x4000)
#define LINUX_MAP_POPULATE UINT64_C(0x8000)
#define LINUX_MAP_STACK UINT64_C(0x20000)
#define LINUX_MAP_FIXED_NOREPLACE UINT64_C(0x100000)
#define LINUX_MAP_KNOWN_FLAGS                                             \
    (LINUX_MAP_TYPE_MASK | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS |       \
     LINUX_MAP_NORESERVE | LINUX_MAP_POPULATE | LINUX_MAP_STACK |        \
     LINUX_MAP_FIXED_NOREPLACE)

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
    struct kernel_mm *mm;
    size_t copied;
    enum kernel_task_status task_status;
    enum kernel_uaccess_status access_status;

    task_status = kernel_task_mm_borrow_mutable(caller, &mm);
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

/*
 * clockid support covers the clocks the kernel can actually back:
 * CLOCK_REALTIME from the boot RTC reading plus the time counter, and
 * CLOCK_MONOTONIC from the time counter.  Everything else reports EINVAL.
 */
static int decode_clock_id_valid(uint64_t clock_id)
{
    return clock_id == LINUX_CLOCK_REALTIME || clock_id == LINUX_CLOCK_MONOTONIC;
}

static void decode_split_nanoseconds(uint64_t nanoseconds,
                                     int64_t *seconds,
                                     int64_t *sub_nanoseconds)
{
    *seconds = (int64_t)(nanoseconds / LINUX_CLOCK_NSECS_PER_SEC);
    *sub_nanoseconds = (int64_t)(nanoseconds % LINUX_CLOCK_NSECS_PER_SEC);
}

/* timespec and timeval share the {i64, i64} riscv64 layout. */
static enum kernel_syscall_status decode_copy_out(
    struct kernel_task *caller,
    uint64_t user_address,
    const void *kernel_source,
    size_t size,
    struct kernel_syscall_result *decoded)
{
    struct kernel_mm *mm;
    size_t copied;
    enum kernel_task_status task_status;
    enum kernel_uaccess_status access_status;

    task_status = kernel_task_mm_borrow_mutable(caller, &mm);
    if (task_status != KERNEL_TASK_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    access_status = kernel_copy_to_user(mm,
                                        user_address,
                                        kernel_source,
                                        size,
                                        &copied);
    if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
        decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
        decoded->value = -KERNEL_EFAULT;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (access_status != KERNEL_UACCESS_STATUS_OK ||
        copied != size) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = 0;
    return KERNEL_SYSCALL_STATUS_OK;
}

static enum kernel_syscall_status decode_copy_time_out(
    struct kernel_task *caller,
    uint64_t user_address,
    int64_t seconds,
    int64_t sub_seconds,
    struct kernel_syscall_result *decoded)
{
    struct {
        int64_t seconds;
        int64_t sub_seconds;
    } value;

    value.seconds = seconds;
    value.sub_seconds = sub_seconds;
    return decode_copy_out(caller,
                           user_address,
                           &value,
                           sizeof(value),
                           decoded);
}

static enum kernel_syscall_status decode_clock_gettime(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    uint64_t nanoseconds;
    int64_t seconds;
    int64_t sub_nanoseconds;

    if (!decode_clock_id_valid(request->arguments[0])) {
        decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
        decoded->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    nanoseconds = request->arguments[0] == LINUX_CLOCK_REALTIME
                      ? kernel_time_realtime_ns()
                      : kernel_time_monotonic_ns();
    decode_split_nanoseconds(nanoseconds, &seconds, &sub_nanoseconds);
    return decode_copy_time_out(caller,
                                request->arguments[1],
                                seconds,
                                sub_nanoseconds,
                                decoded);
}

static enum kernel_syscall_status decode_clock_getres(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    if (!decode_clock_id_valid(request->arguments[0])) {
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
    return decode_copy_time_out(caller,
                                request->arguments[1],
                                0,
                                1,
                                decoded);
}

static enum kernel_syscall_status decode_gettimeofday(
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
    decode_split_nanoseconds(nanoseconds, &seconds, &sub_nanoseconds);
    return decode_copy_time_out(caller,
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
static enum kernel_syscall_status decode_times(
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

/*
 * Shared core of nanosleep(101) and clock_nanosleep(115).  The requested
 * point is normalized to the monotonic domain, the sleep blocks on the
 * timer deadline, and the remaining time is only reported on an early
 * wake (signals do not exist yet, so relative sleeps currently always
 * run to the deadline and never touch `remaining`).
 */
static enum kernel_syscall_status decode_sleep_for(
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

    duration_ns = (uint64_t)request_spec.tv_sec >=
                          UINT64_MAX / LINUX_CLOCK_NSECS_PER_SEC
                      ? UINT64_MAX
                      : (uint64_t)request_spec.tv_sec *
                                LINUX_CLOCK_NSECS_PER_SEC +
                            (uint64_t)request_spec.tv_nsec;
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
        target_monotonic_ns = duration_ns > UINT64_MAX - now_ns
                                  ? UINT64_MAX
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

    sleep_status = kernel_scheduler_block_current(0, deadline, &wake_reason);
    if (sleep_status != KERNEL_SCHEDULER_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }

    if (wake_reason == KERNEL_WAIT_WOKEN &&
        (flags & LINUX_TIMER_ABSTIME) == 0U && remaining_address != 0U) {
        uint64_t remaining_ns = target_monotonic_ns -
                                kernel_time_monotonic_ns();

        if ((int64_t)remaining_ns > 0) {
            int64_t seconds;
            int64_t sub_nanoseconds;

            decode_split_nanoseconds(remaining_ns,
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
    struct kernel_mm *mm;
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
        kernel_task_mm_borrow_mutable(caller, &mm) !=
            KERNEL_TASK_STATUS_OK ||
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
    struct kernel_mm *mm;
    int64_t linux_result;
    enum kernel_task_status task_status;

    task_status = kernel_task_files_borrow(caller, &files);
    if (task_status == KERNEL_TASK_STATUS_RESOURCE_UNAVAILABLE) {
        decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
        decoded->value = -KERNEL_EBADF;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (task_status != KERNEL_TASK_STATUS_OK ||
        kernel_task_mm_borrow_mutable(caller, &mm) !=
            KERNEL_TASK_STATUS_OK ||
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

static enum kernel_syscall_status decode_write(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    struct kernel_files *files;
    struct kernel_mm *mm;
    int64_t linux_result;
    enum kernel_task_status task_status;

    task_status = kernel_task_files_borrow(caller, &files);
    if (task_status == KERNEL_TASK_STATUS_RESOURCE_UNAVAILABLE) {
        decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
        decoded->value = -KERNEL_EBADF;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (task_status != KERNEL_TASK_STATUS_OK ||
        kernel_task_mm_borrow_mutable(caller, &mm) !=
            KERNEL_TASK_STATUS_OK ||
        kernel_files_write(files,
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

static enum kernel_syscall_status decode_lseek(
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
        kernel_files_lseek(files,
                           (int64_t)request->arguments[0],
                           (int64_t)request->arguments[1],
                           request->arguments[2],
                           &linux_result) != KERNEL_FILES_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = linux_result;
    return KERNEL_SYSCALL_STATUS_OK;
}

static enum kernel_syscall_status decode_fstat(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    struct kernel_files *files;
    struct kernel_mm *mm;
    int64_t linux_result;
    enum kernel_task_status task_status;

    task_status = kernel_task_files_borrow(caller, &files);
    if (task_status == KERNEL_TASK_STATUS_RESOURCE_UNAVAILABLE) {
        decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
        decoded->value = -KERNEL_EBADF;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (task_status != KERNEL_TASK_STATUS_OK ||
        kernel_task_mm_borrow_mutable(caller, &mm) !=
            KERNEL_TASK_STATUS_OK ||
        kernel_files_fstat(files,
                           mm,
                           (int64_t)request->arguments[0],
                           request->arguments[1],
                           &linux_result) != KERNEL_FILES_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = linux_result;
    return KERNEL_SYSCALL_STATUS_OK;
}

static enum kernel_syscall_status decode_newfstatat(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    struct kernel_files *files;
    const struct kernel_fs_context *fs;
    struct kernel_mm *mm;
    int64_t linux_result;
    enum kernel_task_status task_status;

    task_status = kernel_task_files_borrow(caller, &files);
    if (task_status == KERNEL_TASK_STATUS_RESOURCE_UNAVAILABLE) {
        decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
        decoded->value = -KERNEL_EBADF;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (task_status != KERNEL_TASK_STATUS_OK ||
        kernel_task_fs_context_borrow(caller, &fs) !=
            KERNEL_TASK_STATUS_OK ||
        kernel_task_mm_borrow_mutable(caller, &mm) !=
            KERNEL_TASK_STATUS_OK ||
        kernel_files_fstatat(files,
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

static enum kernel_syscall_status decode_set_tid_address(
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

static enum kernel_syscall_status decode_dup(
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
        kernel_files_dup(files,
                         (int64_t)request->arguments[0],
                         &linux_result) != KERNEL_FILES_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = linux_result;
    return KERNEL_SYSCALL_STATUS_OK;
}

static enum kernel_syscall_status decode_dup2(
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
        kernel_files_dup2(files,
                          (int64_t)request->arguments[0],
                          (int64_t)request->arguments[1],
                          &linux_result) != KERNEL_FILES_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = linux_result;
    return KERNEL_SYSCALL_STATUS_OK;
}

static enum kernel_syscall_status decode_dup3(
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
        kernel_files_dup3(files,
                          (int64_t)request->arguments[0],
                          (int64_t)request->arguments[1],
                          request->arguments[2],
                          &linux_result) != KERNEL_FILES_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = linux_result;
    return KERNEL_SYSCALL_STATUS_OK;
}

static enum kernel_syscall_status decode_fcntl(
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
        kernel_files_fcntl(files,
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

static enum kernel_syscall_status decode_getdents64(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    struct kernel_files *files;
    struct kernel_mm *mm;
    int64_t linux_result;
    enum kernel_task_status task_status;

    task_status = kernel_task_files_borrow(caller, &files);
    if (task_status == KERNEL_TASK_STATUS_RESOURCE_UNAVAILABLE) {
        decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
        decoded->value = -KERNEL_EBADF;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (task_status != KERNEL_TASK_STATUS_OK ||
        kernel_task_mm_borrow_mutable(caller, &mm) !=
            KERNEL_TASK_STATUS_OK ||
        kernel_files_getdents(files,
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

static enum kernel_syscall_status decode_writev(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    struct kernel_files *files;
    struct kernel_mm *mm;
    int64_t linux_result;
    enum kernel_task_status task_status;

    task_status = kernel_task_files_borrow(caller, &files);
    if (task_status == KERNEL_TASK_STATUS_RESOURCE_UNAVAILABLE) {
        decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
        decoded->value = -KERNEL_EBADF;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (task_status != KERNEL_TASK_STATUS_OK ||
        kernel_task_mm_borrow_mutable(caller, &mm) !=
            KERNEL_TASK_STATUS_OK ||
        kernel_files_writev(files,
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

static enum kernel_syscall_status decode_brk(
    struct kernel_task *caller,
    uint64_t requested,
    struct kernel_syscall_result *decoded)
{
    struct kernel_mm *mm;
    uint64_t program_break;

    if (kernel_task_mm_borrow_mutable(caller, &mm) !=
            KERNEL_TASK_STATUS_OK ||
        kernel_mm_brk(mm, requested, &program_break) !=
            KERNEL_MM_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = (int64_t)program_break;
    return KERNEL_SYSCALL_STATUS_OK;
}

static uint32_t mm_permissions_from_linux(uint64_t protections)
{
    uint32_t permissions = 0U;

    if ((protections & LINUX_PROT_READ) != 0U) {
        permissions |= KERNEL_MM_READ;
    }
    if ((protections & LINUX_PROT_WRITE) != 0U) {
        permissions |= KERNEL_MM_WRITE;
    }
    if ((protections & LINUX_PROT_EXEC) != 0U) {
        permissions |= KERNEL_MM_EXECUTE;
    }
    return permissions;
}

static int64_t mmap_error(enum kernel_mm_status status)
{
    if (status == KERNEL_MM_STATUS_NO_MEMORY) {
        return -KERNEL_ENOMEM;
    }
    if (status == KERNEL_MM_STATUS_CONFLICT) {
        return -KERNEL_EEXIST;
    }
    return -KERNEL_EINVAL;
}

static enum kernel_syscall_status decode_mmap(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    struct kernel_files *files;
    struct kernel_mm *mm;
    struct kernel_open_file_description *file = 0;
    uint64_t protections = request->arguments[2];
    uint64_t flags = request->arguments[3];
    uint64_t mapped_address;
    uint32_t mm_flags = 0U;
    int64_t linux_result;
    enum kernel_mm_status status;
    enum kernel_task_status task_status;

    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    if ((protections & ~(LINUX_PROT_READ | LINUX_PROT_WRITE |
                         LINUX_PROT_EXEC)) != 0U ||
        (flags & ~LINUX_MAP_KNOWN_FLAGS) != 0U ||
        ((flags & LINUX_MAP_FIXED) != 0U &&
         (flags & LINUX_MAP_FIXED_NOREPLACE) != 0U) ||
        (request->arguments[5] & BOAROS_PAGE_MASK) != 0U) {
        decoded->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if ((flags & LINUX_MAP_TYPE_MASK) != LINUX_MAP_PRIVATE ||
        (flags & LINUX_MAP_POPULATE) != 0U) {
        decoded->value = -KERNEL_ENOTSUP;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if ((flags & LINUX_MAP_FIXED) != 0U) {
        mm_flags = KERNEL_MM_MAP_FIXED;
    } else if ((flags & LINUX_MAP_FIXED_NOREPLACE) != 0U) {
        mm_flags = KERNEL_MM_MAP_FIXED_NOREPLACE;
    }
    if (kernel_task_mm_borrow_mutable(caller, &mm) !=
        KERNEL_TASK_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    if ((flags & LINUX_MAP_ANONYMOUS) != 0U) {
        status = kernel_mm_mmap_anonymous(
            mm,
            request->arguments[0],
            request->arguments[1],
            mm_permissions_from_linux(protections),
            mm_flags,
            &mapped_address);
    } else {
        task_status = kernel_task_files_borrow(caller, &files);
        if (task_status == KERNEL_TASK_STATUS_RESOURCE_UNAVAILABLE) {
            decoded->value = -KERNEL_EBADF;
            return KERNEL_SYSCALL_STATUS_OK;
        }
        if (task_status != KERNEL_TASK_STATUS_OK ||
            kernel_files_pin(files,
                             (int64_t)request->arguments[4],
                             &file,
                             &linux_result) != KERNEL_FILES_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
        if (linux_result != 0) {
            decoded->value = linux_result;
            return KERNEL_SYSCALL_STATUS_OK;
        }
        if (kernel_open_file_kind(file) !=
            KERNEL_OPEN_FILE_KIND_REGULAR) {
            /* Only regular files back the private-mapping page-cache path. */
            if (kernel_open_file_release(&file) !=
                KERNEL_OPEN_FILE_STATUS_OK) {
                return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
            }
            decoded->value = -KERNEL_ENODEV;
            return KERNEL_SYSCALL_STATUS_OK;
        }
        status = kernel_mm_mmap_file_private(
            mm,
            &file,
            request->arguments[0],
            request->arguments[1],
            request->arguments[5],
            mm_permissions_from_linux(protections),
            mm_flags,
            &mapped_address);
        if (status != KERNEL_MM_STATUS_OK &&
            kernel_open_file_release(&file) !=
                KERNEL_OPEN_FILE_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    }
    if (status != KERNEL_MM_STATUS_OK &&
        status != KERNEL_MM_STATUS_INVALID_ARGUMENT &&
        status != KERNEL_MM_STATUS_NO_MEMORY &&
        status != KERNEL_MM_STATUS_CONFLICT) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    decoded->value = status == KERNEL_MM_STATUS_OK
                         ? (int64_t)mapped_address
                         : mmap_error(status);
    return KERNEL_SYSCALL_STATUS_OK;
}

static enum kernel_syscall_status decode_munmap(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    struct kernel_mm *mm;
    enum kernel_mm_status status;

    if (kernel_task_mm_borrow_mutable(caller, &mm) !=
        KERNEL_TASK_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    status = kernel_mm_munmap(mm,
                              request->arguments[0],
                              request->arguments[1]);
    if (status != KERNEL_MM_STATUS_OK &&
        status != KERNEL_MM_STATUS_INVALID_ARGUMENT &&
        status != KERNEL_MM_STATUS_NO_MEMORY) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = status == KERNEL_MM_STATUS_OK
                         ? 0
                         : mmap_error(status);
    return KERNEL_SYSCALL_STATUS_OK;
}

static enum kernel_syscall_status decode_mprotect(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    struct kernel_mm *mm;
    uint64_t protections = request->arguments[2];
    enum kernel_mm_status status;

    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    if ((protections & ~(LINUX_PROT_READ | LINUX_PROT_WRITE |
                         LINUX_PROT_EXEC)) != 0U) {
        decoded->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (kernel_task_mm_borrow_mutable(caller, &mm) !=
        KERNEL_TASK_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    status = kernel_mm_mprotect(mm,
                                request->arguments[0],
                                request->arguments[1],
                                mm_permissions_from_linux(protections));
    if (status != KERNEL_MM_STATUS_OK &&
        status != KERNEL_MM_STATUS_INVALID_ARGUMENT &&
        status != KERNEL_MM_STATUS_NOT_MAPPED &&
        status != KERNEL_MM_STATUS_NO_MEMORY) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    if (status == KERNEL_MM_STATUS_NOT_MAPPED ||
        status == KERNEL_MM_STATUS_NO_MEMORY) {
        decoded->value = -KERNEL_ENOMEM;
    } else {
        decoded->value = status == KERNEL_MM_STATUS_OK
                             ? 0
                             : -KERNEL_EINVAL;
    }
    return KERNEL_SYSCALL_STATUS_OK;
}

/*
 * clone(220) accepts the process form: fork with an optional custom
 * child stack.  Tid-pointer and TLS arguments belong to the thread
 * model and stay ENOTSUP; the vfork flag set lands with shared-MM
 * process support.
 */
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
    if (flags != LINUX_SIGCHLD || request->arguments[2] != 0U ||
        request->arguments[3] != 0U || request->arguments[4] != 0U) {
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

    if (request->number == LINUX_SYSCALL_DUP) {
        if (decode_dup(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_DUP2) {
        if (decode_dup2(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_DUP3) {
        if (decode_dup3(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_FCNTL) {
        if (decode_fcntl(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_OPENAT) {
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
    } else if (request->number == LINUX_SYSCALL_WRITE) {
        if (decode_write(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_WRITEV) {
        if (decode_writev(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_GETDENTS64) {
        if (decode_getdents64(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_LSEEK) {
        if (decode_lseek(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_NEWFSTATAT) {
        if (decode_newfstatat(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_FSTAT) {
        if (decode_fstat(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_EXIT ||
               request->number == LINUX_SYSCALL_EXIT_GROUP) {
        /* Single-member thread groups: exit_group terminates this task. */
        decoded.action = KERNEL_SYSCALL_ACTION_EXIT;
        decoded.value = (int64_t)(request->arguments[0] &
                                  LINUX_EXIT_STATUS_MASK);
    } else if (request->number == LINUX_SYSCALL_SET_TID_ADDRESS) {
        if (decode_set_tid_address(caller,
                                   request->arguments[0],
                                   &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
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
    } else if (request->number == LINUX_SYSCALL_BRK) {
        if (decode_brk(caller,
                       request->arguments[0],
                       &decoded) != KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_MUNMAP) {
        if (decode_munmap(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_MMAP) {
        if (decode_mmap(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_MPROTECT) {
        if (decode_mprotect(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
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
    } else if (request->number == LINUX_SYSCALL_SCHED_YIELD) {
        decoded.action = KERNEL_SYSCALL_ACTION_YIELD;
        decoded.value = 0;
    } else if (request->number == LINUX_SYSCALL_CLOCK_GETTIME) {
        if (decode_clock_gettime(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_CLOCK_GETRES) {
        if (decode_clock_getres(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_GETTIMEOFDAY) {
        if (decode_gettimeofday(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_TIMES) {
        if (decode_times(caller, request->arguments[0], &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_NANOSLEEP) {
        if (decode_sleep_for(caller,
                             LINUX_CLOCK_REALTIME,
                             0U,
                             request->arguments[0],
                             request->arguments[1],
                             &decoded) != KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_CLOCK_NANOSLEEP) {
        if (decode_sleep_for(caller,
                             (uint32_t)request->arguments[0],
                             (uint32_t)request->arguments[1],
                             request->arguments[2],
                             request->arguments[3],
                             &decoded) != KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else {
        decoded.action = KERNEL_SYSCALL_ACTION_RETURN;
        decoded.value = -KERNEL_ENOSYS;
    }

    *result = decoded;
    return KERNEL_SYSCALL_STATUS_OK;
}
