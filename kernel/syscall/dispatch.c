#include "private.h"

#include <kernel/cost.h>
#include <kernel/errno.h>
#include <kernel/futex.h>
#include <kernel/task.h>
#include <kernel/mm.h>
#include <kernel/uaccess.h>
#include <kernel/scheduler.h>
#include <kernel/time.h>
#include <arch/riscv/context.h>

#include <stddef.h>
#include <stdint.h>

#define LINUX_SYSCALL_STATFS 43U
#define LINUX_SYSCALL_FSTATFS 44U
#define LINUX_SYSCALL_UTIMENSAT 88U
#define LINUX_SYSCALL_FCHMOD 52U
#define LINUX_SYSCALL_FCHMODAT 53U
#define LINUX_SYSCALL_FCHOWNAT 54U
#define LINUX_SYSCALL_FCHOWN 55U
#define LINUX_SYSCALL_GETCWD 17U
#define LINUX_SYSCALL_RENAMEAT 38U
#define LINUX_SYSCALL_UMOUNT2 39U
#define LINUX_SYSCALL_MOUNT 40U
#define LINUX_SYSCALL_CHDIR 49U
#define LINUX_SYSCALL_FCHDIR 50U
#define LINUX_SYSCALL_RENAMEAT2 276U
#define LINUX_SYSCALL_EPOLL_CREATE1 20U
#define LINUX_SYSCALL_EPOLL_CTL 21U
#define LINUX_SYSCALL_EPOLL_PWAIT 22U
#define LINUX_SYSCALL_DUP 23U
#define LINUX_SYSCALL_DUP3 24U
#define LINUX_SYSCALL_FCNTL 25U
#define LINUX_SYSCALL_IOCTL 29U
#define LINUX_SYSCALL_MKNODAT 33U
#define LINUX_SYSCALL_MKDIRAT 34U
#define LINUX_SYSCALL_UNLINKAT 35U
#define LINUX_SYSCALL_SYMLINKAT 36U
#define LINUX_SYSCALL_LINKAT 37U
#define LINUX_SYSCALL_TRUNCATE 45U
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
#define LINUX_SYSCALL_SENDFILE 71U
#define LINUX_SYSCALL_PSELECT6 72U
#define LINUX_SYSCALL_PPOLL 73U
#define LINUX_SYSCALL_READLINKAT 78U
#define LINUX_SYSCALL_FSYNC 82U
#define LINUX_SYSCALL_SYNC 81U
#define LINUX_SYSCALL_SYNCFS 267U
#define LINUX_SYSCALL_FDATASYNC 83U
#define LINUX_SYSCALL_EXIT 93U
#define LINUX_SYSCALL_EXIT_GROUP 94U
#define LINUX_SYSCALL_SET_TID_ADDRESS 96U
#define LINUX_SYSCALL_SET_ROBUST_LIST 99U
#define LINUX_SYSCALL_GET_ROBUST_LIST 100U
#define LINUX_SYSCALL_UNAME 160U
#define LINUX_SYSCALL_GETPID 172U
#define LINUX_SYSCALL_GETPPID 173U
#define LINUX_SYSCALL_GETUID 174U
#define LINUX_SYSCALL_GETEUID 175U
#define LINUX_SYSCALL_GETGID 176U
#define LINUX_SYSCALL_GETEGID 177U
#define LINUX_SYSCALL_GETTID 178U
#define LINUX_SYSCALL_UMASK 166U
#define LINUX_SYSCALL_SHMGET 194U
#define LINUX_SYSCALL_SHMCTL 195U
#define LINUX_SYSCALL_SHMAT 196U
#define LINUX_SYSCALL_SHMDT 197U
#define LINUX_SYSCALL_SOCKET 198U
#define LINUX_SYSCALL_SOCKETPAIR 199U
#define LINUX_SYSCALL_BIND 200U
#define LINUX_SYSCALL_LISTEN 201U
#define LINUX_SYSCALL_ACCEPT 202U
#define LINUX_SYSCALL_CONNECT 203U
#define LINUX_SYSCALL_GETSOCKNAME 204U
#define LINUX_SYSCALL_GETPEERNAME 205U
#define LINUX_SYSCALL_SENDTO 206U
#define LINUX_SYSCALL_RECVFROM 207U
#define LINUX_SYSCALL_SETSOCKOPT 208U
#define LINUX_SYSCALL_GETSOCKOPT 209U
#define LINUX_SYSCALL_SHUTDOWN 210U
#define LINUX_SYSCALL_SENDMSG 211U
#define LINUX_SYSCALL_RECVMSG 212U
#define LINUX_SYSCALL_BRK 214U
#define LINUX_SYSCALL_SYSLOG 116U
#define LINUX_SYSCALL_GETRANDOM 278U
#define LINUX_SYSCALL_SCHED_YIELD 124U
#define LINUX_SYSCALL_CLOCK_GETTIME 113U
#define LINUX_SYSCALL_CLOCK_GETRES 114U
#define LINUX_SYSCALL_NANOSLEEP 101U
#define LINUX_SYSCALL_GETITIMER 102U
#define LINUX_SYSCALL_SETITIMER 103U
#define LINUX_SYSCALL_CLOCK_NANOSLEEP 115U
#define LINUX_SYSCALL_GETTIMEOFDAY 169U
#define LINUX_SYSCALL_TIMES 153U
#define LINUX_CLOCK_REALTIME 0U
#define LINUX_SYSCALL_MUNMAP 215U
#define LINUX_SYSCALL_CLONE 220U
#define LINUX_SYSCALL_EXECVE 221U
#define LINUX_SYSCALL_MMAP 222U
#define LINUX_SYSCALL_MSYNC 227U
#define LINUX_SYSCALL_MPROTECT 226U
#define LINUX_SYSCALL_WAIT4 260U
#define LINUX_SYSCALL_PRLIMIT64 261U
#define LINUX_SYSCALL_NEWFSTATAT 79U
#define LINUX_SYSCALL_FACCESSAT 48U
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


struct linux_sysinfo {
    int64_t uptime;
    uint64_t loads[3], totalram, freeram, sharedram, bufferram, totalswap, freeswap;
    uint16_t procs, pad;
    uint32_t alignment;
    uint64_t totalhigh, freehigh;
    uint32_t mem_unit, tail;
};
_Static_assert(sizeof(struct linux_sysinfo) == 112, "RV64 sysinfo layout");

static enum kernel_syscall_status syscall_handle_sysinfo(struct kernel_task *caller,
    uint64_t address, struct kernel_syscall_result *decoded)
{
    struct kernel_mm *mm;
    if (kernel_task_mm_borrow_mutable(caller, &mm) != KERNEL_TASK_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    struct linux_sysinfo info = {0};
    struct kernel_memory_statistics memory;
    uintptr_t irq = riscv_interrupt_save();
    kernel_memory_snapshot(mm->allocator, &memory);
    kernel_scheduler_system_statistics(info.loads, &info.procs);
    uint64_t ns = kernel_time_monotonic_ns();
    info.uptime = ns / 1000000000 + (ns % 1000000000 != 0);
    riscv_interrupt_restore(irq);
    info.totalram = memory.total;
    info.freeram = memory.free;
    info.sharedram = memory.shared;
    info.bufferram = memory.buffers;
    info.mem_unit = 1;
    size_t copied;
    enum kernel_uaccess_status access = kernel_copy_to_user(mm, address, &info, sizeof(info), &copied);
    if (access != KERNEL_UACCESS_STATUS_OK && access != KERNEL_UACCESS_STATUS_FAULT)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = access == KERNEL_UACCESS_STATUS_FAULT ? -KERNEL_EFAULT : 0;
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

#if BOAROS_COST_DIAGNOSTICS
    kernel_cost_syscall(request->number, (int64_t)request->arguments[0]);
    COST_SCOPE(syscall_cost, OPERATION_TICKS);
    COST_ADD(OPERATIONS, 1);
#endif
    if (request->number == 179U) {
        if (syscall_handle_sysinfo(caller, request->arguments[0], &decoded) != KERNEL_SYSCALL_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    } else if (request->number == LINUX_SYSCALL_SHMGET) {
        if (syscall_handle_shmget(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_SHMCTL) {
        if (syscall_handle_shmctl(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_SHMAT) {
        if (syscall_handle_shmat(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_SHMDT) {
        if (syscall_handle_shmdt(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_SOCKET) {
        if (syscall_handle_socket(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_SOCKETPAIR) {
        if (syscall_handle_socketpair(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_BIND) {
        if (syscall_handle_bind(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_LISTEN ||
               request->number == LINUX_SYSCALL_ACCEPT ||
               request->number == LINUX_SYSCALL_CONNECT ||
               request->number == LINUX_SYSCALL_GETSOCKNAME ||
               request->number == LINUX_SYSCALL_GETPEERNAME ||
               request->number == LINUX_SYSCALL_SENDTO ||
               request->number == LINUX_SYSCALL_RECVFROM ||
               request->number == LINUX_SYSCALL_SETSOCKOPT ||
               request->number == LINUX_SYSCALL_GETSOCKOPT ||
               request->number == LINUX_SYSCALL_SHUTDOWN ||
               request->number == LINUX_SYSCALL_SENDMSG ||
               request->number == LINUX_SYSCALL_RECVMSG) {
        if (syscall_handle_socket_operation(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    } else if (request->number == LINUX_SYSCALL_EPOLL_CREATE1) {
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
    } else if (request->number == LINUX_SYSCALL_MOUNT ||
               request->number == LINUX_SYSCALL_UMOUNT2) {
        if (syscall_handle_mount(caller, request, &decoded,
                request->number == LINUX_SYSCALL_UMOUNT2) !=
            KERNEL_SYSCALL_STATUS_OK) return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    } else if (request->number == LINUX_SYSCALL_UTIMENSAT) {
        if (syscall_handle_utimensat(caller, request, &decoded) != KERNEL_SYSCALL_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    } else if (request->number == LINUX_SYSCALL_UMASK) {
        if (syscall_handle_umask(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    } else if (request->number == LINUX_SYSCALL_FCHOWNAT ||
               request->number == LINUX_SYSCALL_FCHOWN) {
        if (syscall_handle_chown(caller, request, &decoded,
                                request->number == LINUX_SYSCALL_FCHOWN) !=
            KERNEL_SYSCALL_STATUS_OK) return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    } else if (request->number == LINUX_SYSCALL_FCHMOD ||
               request->number == LINUX_SYSCALL_FCHMODAT) {
        if (syscall_handle_chmod(caller, request, &decoded,
                request->number == LINUX_SYSCALL_FCHMOD) !=
            KERNEL_SYSCALL_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    } else if (request->number == LINUX_SYSCALL_GETITIMER || request->number == LINUX_SYSCALL_SETITIMER) {
        if (syscall_handle_itimer(caller, request, &decoded) != KERNEL_SYSCALL_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    } else if (request->number == LINUX_SYSCALL_STATFS ||
               request->number == LINUX_SYSCALL_FSTATFS) {
        if (syscall_handle_statfs(caller, request, &decoded,
                request->number == LINUX_SYSCALL_FSTATFS) != KERNEL_SYSCALL_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    } else if (request->number == LINUX_SYSCALL_GETCWD) {
        if (syscall_handle_getcwd(caller, request, &decoded) != KERNEL_SYSCALL_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    } else if (request->number == LINUX_SYSCALL_CHDIR ||
               request->number == LINUX_SYSCALL_FCHDIR) {
        if (syscall_handle_chdir(caller, request, &decoded,
                request->number == LINUX_SYSCALL_FCHDIR) != KERNEL_SYSCALL_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    } else if (request->number == LINUX_SYSCALL_LINKAT) {
        if (syscall_handle_linkat(caller, request, &decoded) != KERNEL_SYSCALL_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    } else if (request->number == LINUX_SYSCALL_RENAMEAT ||
               request->number == LINUX_SYSCALL_RENAMEAT2) {
        if (syscall_handle_renameat(caller, request, &decoded,
                request->number == LINUX_SYSCALL_RENAMEAT2) != KERNEL_SYSCALL_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    } else if (request->number == LINUX_SYSCALL_MKNODAT) {
        if (syscall_handle_mknodat(caller, request, &decoded) != KERNEL_SYSCALL_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
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
    } else if (request->number == LINUX_SYSCALL_TRUNCATE) {
        if (syscall_handle_truncate(caller, request, &decoded) != KERNEL_SYSCALL_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    } else if (request->number == LINUX_SYSCALL_FTRUNCATE) {
        if (syscall_handle_ftruncate(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_SYNC || request->number == LINUX_SYSCALL_SYNCFS) {
        if (syscall_handle_sync(caller, request, &decoded,
                                request->number == LINUX_SYSCALL_SYNCFS) != KERNEL_SYSCALL_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    } else if (request->number == LINUX_SYSCALL_FSYNC ||
               request->number == LINUX_SYSCALL_FDATASYNC) {
        if (syscall_handle_fsync(caller, request, &decoded,
                request->number == LINUX_SYSCALL_FDATASYNC) !=
            KERNEL_SYSCALL_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
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
    } else if (request->number == LINUX_SYSCALL_SENDFILE) {
        if (syscall_handle_sendfile(caller, request, &decoded) != KERNEL_SYSCALL_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
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
    } else if (request->number == LINUX_SYSCALL_FACCESSAT) {
        if (syscall_handle_faccessat(caller, request, &decoded) !=
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
        /* exit_group requests termination of every live group member. */
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
                                      request->arguments[4],
                                      (uint32_t)request->arguments[5],
                                      &futex_status);
        if (futex_status != KERNEL_SCHEDULER_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    } else if (request->number == LINUX_SYSCALL_SET_TID_ADDRESS) {
        if (syscall_handle_set_tid_address(caller,
                                   request->arguments[0],
                                   &decoded) !=
            KERNEL_SYSCALL_STATUS_OK) {
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        }
    } else if (request->number == LINUX_SYSCALL_SET_ROBUST_LIST) {
        if (syscall_handle_set_robust_list(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    } else if ((request->number >= 118U && request->number <= 123U) ||
               (request->number >= 125U && request->number <= 127U)) {
        if (syscall_handle_sched(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    } else if (request->number == 154U) {
        if (syscall_handle_setpgid(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    } else if (request->number == 155U) {
        if (syscall_handle_getpgid(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    } else if (request->number == 156U) {
        if (syscall_handle_getsid(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    } else if (request->number == 157U) {
        if (syscall_handle_setsid(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    } else if (request->number == LINUX_SYSCALL_GET_ROBUST_LIST) {
        if (syscall_handle_get_robust_list(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
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
    } else if (request->number == LINUX_SYSCALL_MSYNC) {
        if (syscall_handle_msync(caller, request, &decoded) !=
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
    } else if (request->number == LINUX_SYSCALL_SYSLOG) {
        if (syscall_handle_syslog(caller, request, &decoded) != KERNEL_SYSCALL_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    } else if (request->number == LINUX_SYSCALL_GETRANDOM) {
        if (syscall_handle_getrandom(caller, request, &decoded) !=
            KERNEL_SYSCALL_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
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
