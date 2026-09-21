#include "private.h"

#include <kernel/errno.h>
#include <kernel/futex.h>
#include <kernel/task.h>
#include <kernel/mm.h>
#include <kernel/uaccess.h>

#include <stddef.h>
#include <stdint.h>

#define LINUX_SYSCALL_EPOLL_CREATE1 20U
#define LINUX_SYSCALL_EPOLL_CTL 21U
#define LINUX_SYSCALL_EPOLL_PWAIT 22U
#define LINUX_SYSCALL_DUP 23U
#define LINUX_SYSCALL_DUP3 24U
#define LINUX_SYSCALL_FCNTL 25U
#define LINUX_SYSCALL_IOCTL 29U
#define LINUX_SYSCALL_MKDIRAT 34U
#define LINUX_SYSCALL_UNLINKAT 35U
#define LINUX_SYSCALL_SYMLINKAT 36U
#define LINUX_SYSCALL_FTRUNCATE 46U
#define LINUX_SYSCALL_OPENAT 56U
#define LINUX_SYSCALL_CLOSE 57U
#define LINUX_SYSCALL_PIPE2 59U
#define LINUX_SYSCALL_GETDENTS64 61U
#define LINUX_SYSCALL_LSEEK 62U
#define LINUX_SYSCALL_READ 63U
#define LINUX_SYSCALL_WRITE 64U
#define LINUX_SYSCALL_READV 65U
#define LINUX_SYSCALL_WRITEV 66U
#define LINUX_SYSCALL_PREAD64 67U
#define LINUX_SYSCALL_PWRITE64 68U
#define LINUX_SYSCALL_PSELECT6 72U
#define LINUX_SYSCALL_PPOLL 73U
#define LINUX_SYSCALL_READLINKAT 78U
#define LINUX_SYSCALL_EXIT 93U
#define LINUX_SYSCALL_EXIT_GROUP 94U
#define LINUX_SYSCALL_SET_TID_ADDRESS 96U
#define LINUX_SYSCALL_UNAME 160U
#define LINUX_SYSCALL_GETPID 172U
#define LINUX_SYSCALL_GETPPID 173U
#define LINUX_SYSCALL_GETUID 174U
#define LINUX_SYSCALL_GETEUID 175U
#define LINUX_SYSCALL_GETGID 176U
#define LINUX_SYSCALL_GETEGID 177U
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
#define LINUX_SYSCALL_MUNMAP 215U
#define LINUX_SYSCALL_CLONE 220U
#define LINUX_SYSCALL_EXECVE 221U
#define LINUX_SYSCALL_MMAP 222U
#define LINUX_SYSCALL_MPROTECT 226U
#define LINUX_SYSCALL_WAIT4 260U
#define LINUX_SYSCALL_PRLIMIT64 261U
#define LINUX_SYSCALL_NEWFSTATAT 79U
#define LINUX_SYSCALL_FSTAT 80U
#define LINUX_SYSCALL_KILL 129U
#define LINUX_SYSCALL_TKILL 130U
#define LINUX_SYSCALL_TGKILL 131U
#define LINUX_SYSCALL_RT_SIGSUSPEND 133U
#define LINUX_SYSCALL_RT_SIGACTION 134U
#define LINUX_SYSCALL_RT_SIGPROCMASK 135U
#define LINUX_SYSCALL_RT_SIGPENDING 136U
#define LINUX_SYSCALL_RT_SIGTIMEDWAIT 137U
#define LINUX_SYSCALL_RT_SIGRETURN 139U
#define LINUX_SYSCALL_RESTART_SYSCALL 128U
#define LINUX_EXIT_STATUS_MASK UINT64_C(0xff)
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

static enum kernel_syscall_status syscall_handle_uname(
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

    if (request->number == LINUX_SYSCALL_EPOLL_CREATE1) {
        if (syscall_handle_epoll_create1(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_EPOLL_CTL) {
        if (syscall_handle_epoll_ctl(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_EPOLL_PWAIT) {
        if (syscall_handle_epoll_pwait(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_DUP) {
        if (syscall_handle_dup(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_DUP3) {
        if (syscall_handle_dup3(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_FCNTL) {
        if (syscall_handle_fcntl(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_IOCTL) {
        if (syscall_handle_ioctl(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_PIPE2) {
        if (syscall_handle_pipe2(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_MKDIRAT) {
        if (syscall_handle_mkdirat(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_UNLINKAT) {
        if (syscall_handle_unlinkat(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_SYMLINKAT) {
        if (syscall_handle_symlinkat(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_FTRUNCATE) {
        if (syscall_handle_ftruncate(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_OPENAT) {
        if (syscall_handle_openat(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_CLOSE) {
        if (syscall_handle_close(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_READ) {
        if (syscall_handle_read(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_READV) {
        if (syscall_handle_readv(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_PREAD64) {
        if (syscall_handle_pread64(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_PWRITE64) {
        if (syscall_handle_pwrite64(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_WRITE) {
        if (syscall_handle_write(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_WRITEV) {
        if (syscall_handle_writev(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_GETDENTS64) {
        if (syscall_handle_getdents64(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_LSEEK) {
        if (syscall_handle_lseek(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_NEWFSTATAT) {
        if (syscall_handle_newfstatat(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_READLINKAT) {
        if (syscall_handle_readlinkat(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_PRLIMIT64) {
        if (syscall_handle_prlimit64(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_FSTAT) {
        if (syscall_handle_fstat(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_PSELECT6) {
        if (syscall_handle_pselect6(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_PPOLL) {
        if (syscall_handle_ppoll(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_EXIT ||
               request->number == LINUX_SYSCALL_EXIT_GROUP) {
        /* Single-member thread groups: exit_group terminates this task. */
        decoded.action = request->number == LINUX_SYSCALL_EXIT_GROUP
                             ? KERNEL_SYSCALL_ACTION_EXIT_GROUP
                             : KERNEL_SYSCALL_ACTION_EXIT;
        decoded.value = (int64_t)(request->arguments[0] &
                                  LINUX_EXIT_STATUS_MASK);
    } else if (request->number == 98U) {
        enum kernel_scheduler_status futex_status;
        decoded.action = KERNEL_SYSCALL_ACTION_RETURN;
        decoded.value = kernel_futex(caller, request->arguments[0],
                                      (uint32_t)request->arguments[1],
                                      (uint32_t)request->arguments[2],
                                      request->arguments[3],
                                      request->arguments[4], &futex_status);
        if (futex_status != KERNEL_SCHEDULER_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    } else if (request->number == LINUX_SYSCALL_SET_TID_ADDRESS) {
        if (syscall_handle_set_tid_address(caller,
                                   request->arguments[0],
                                   &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_KILL ||
               request->number == LINUX_SYSCALL_TKILL ||
               request->number == LINUX_SYSCALL_TGKILL) {
        if (syscall_handle_signal_send(caller,
                               (uint32_t)request->number,
                               request,
                               &decoded) != KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_RT_SIGACTION) {
        if (syscall_handle_rt_sigaction(caller,
                                request,
                                &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_RT_SIGPROCMASK) {
        if (syscall_handle_rt_sigprocmask(caller,
                                  request,
                                  &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_RT_SIGPENDING) {
        if (syscall_handle_rt_sigpending(caller,
                                 request,
                                 &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_RT_SIGTIMEDWAIT) {
        if (syscall_handle_rt_sigtimedwait(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_RT_SIGSUSPEND) {
        if (syscall_handle_rt_sigsuspend(caller,
                                 request,
                                 &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_RT_SIGRETURN) {
        decoded.action = KERNEL_SYSCALL_ACTION_SIGNAL_RETURN;
        decoded.value = 0;
    } else if (request->number == LINUX_SYSCALL_RESTART_SYSCALL) {
        if (syscall_handle_restart_syscall(caller,
                                   &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_UNAME) {
        if (syscall_handle_uname(caller,
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
    } else if (request->number == LINUX_SYSCALL_GETUID ||
               request->number == LINUX_SYSCALL_GETEUID ||
               request->number == LINUX_SYSCALL_GETGID ||
               request->number == LINUX_SYSCALL_GETEGID) {
        /* Every task currently runs as immutable root, including after
         * fork/clone and exec. Credential-changing calls remain unsupported;
         * introducing mutable credentials must replace these queries too. */
        decoded.action = KERNEL_SYSCALL_ACTION_RETURN;
        decoded.value = 0;
    } else if (request->number == LINUX_SYSCALL_GETTID) {
        task_status = kernel_task_tid(caller, &id);
        if (task_status != KERNEL_TASK_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
        decoded.action = KERNEL_SYSCALL_ACTION_RETURN;
        decoded.value = id;
    } else if (request->number == LINUX_SYSCALL_BRK) {
        if (syscall_handle_brk(caller,
                       request->arguments[0],
                       &decoded) != KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_MUNMAP) {
        if (syscall_handle_munmap(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_MMAP) {
        if (syscall_handle_mmap(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_MPROTECT) {
        if (syscall_handle_mprotect(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_EXECVE) {
        if (syscall_handle_execve(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_CLONE) {
        syscall_decode_clone(request, &decoded);
    } else if (request->number == LINUX_SYSCALL_WAIT4) {
        decoded.action = KERNEL_SYSCALL_ACTION_WAIT4;
        decoded.value = 0;
    } else if (request->number == LINUX_SYSCALL_SCHED_YIELD) {
        decoded.action = KERNEL_SYSCALL_ACTION_YIELD;
        decoded.value = 0;
    } else if (request->number == LINUX_SYSCALL_CLOCK_GETTIME) {
        if (syscall_handle_clock_gettime(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_CLOCK_GETRES) {
        if (syscall_handle_clock_getres(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_GETTIMEOFDAY) {
        if (syscall_handle_gettimeofday(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_TIMES) {
        if (syscall_handle_times(caller, request->arguments[0], &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_NANOSLEEP) {
        if (syscall_handle_sleep_for(caller,
                             LINUX_CLOCK_REALTIME,
                             0U,
                             request->arguments[0],
                             request->arguments[1],
                             &decoded) != KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_CLOCK_NANOSLEEP) {
        if (syscall_handle_sleep_for(caller,
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
