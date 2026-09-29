#include "private.h"

#include <kernel/errno.h>
#include <kernel/files.h>
#include <kernel/open_file.h>
#include <kernel/scheduler.h>
#include <kernel/signal.h>
#include <kernel/socket.h>
#include <kernel/task.h>
#include <kernel/time.h>
#include <kernel/uaccess.h>

#include <arch/riscv/context.h>

#include <stdint.h>

/* RV64 Linux UAPI values; see fixed references/linux/net/socket.c. */
#define LINUX_AF_UNIX 1
#define LINUX_AF_INET 2
#define LINUX_AF_MAX 46
#define LINUX_SOCK_STREAM 1
#define LINUX_SOCK_DGRAM 2
#define LINUX_SOCK_MAX 11
#define LINUX_SOCK_TYPE_MASK 0xf
#define LINUX_SOCK_NONBLOCK 00004000
#define LINUX_SOCK_CLOEXEC 02000000
#define LINUX_IPPROTO_TCP 6
#define LINUX_IPPROTO_UDP 17
#define LINUX_SOL_SOCKET 1
#define LINUX_SO_RCVTIMEO 20

struct linux_sockaddr_in {
    uint16_t family;
    uint16_t port;
    uint32_t address;
    uint8_t zero[8];
};

struct linux_timeval {
    int64_t seconds;
    int64_t microseconds;
};

_Static_assert(sizeof(struct linux_sockaddr_in) == 16U,
               "RV64 sockaddr_in size");

static uint16_t network_port(uint16_t port)
{
    return (uint16_t)((port << 8) | (port >> 8));
}

static enum kernel_syscall_status borrow_socket(
    struct kernel_task *caller, int32_t fd, struct kernel_files **files_out,
    struct kernel_open_file_description **file_out, int64_t *result)
{
    enum kernel_task_status status = kernel_task_files_borrow(caller, files_out);
    if (status == KERNEL_TASK_STATUS_RESOURCE_UNAVAILABLE) {
        *result = -KERNEL_EBADF;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (status != KERNEL_TASK_STATUS_OK ||
        kernel_files_pin(*files_out, fd, file_out, result) !=
            KERNEL_FILES_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    if (*result == 0 && kernel_open_file_socket(*file_out) == 0) {
        if (kernel_open_file_release(file_out) != KERNEL_OPEN_FILE_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        *result = -KERNEL_ENOTSOCK;
    }
    return KERNEL_SYSCALL_STATUS_OK;
}

static enum kernel_syscall_status release_socket(
    struct kernel_open_file_description **file)
{
    return kernel_open_file_release(file) == KERNEL_OPEN_FILE_STATUS_OK
               ? KERNEL_SYSCALL_STATUS_OK
               : KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
}

static int copy_address_in(struct kernel_mm *mm, uint64_t user,
                           uint64_t length, struct linux_sockaddr_in *address)
{
    size_t copied = 0;
    if (length < sizeof(*address)) return -KERNEL_EINVAL;
    if (kernel_copy_from_user(mm, address, user, sizeof(*address), &copied) !=
            KERNEL_UACCESS_STATUS_OK || copied != sizeof(*address))
        return -KERNEL_EFAULT;
    if (address->family != LINUX_AF_INET) return -KERNEL_EAFNOSUPPORT;
    return 0;
}

static int copy_address_out(struct kernel_mm *mm, uint64_t user_address,
                            uint64_t user_length, uint32_t address,
                            uint16_t port)
{
    struct linux_sockaddr_in local = {
        .family = LINUX_AF_INET,
        .port = network_port(port),
        .address = address,
    };
    int32_t length;
    size_t copied = 0;
    if (kernel_copy_from_user(mm, &length, user_length, sizeof(length),
                              &copied) != KERNEL_UACCESS_STATUS_OK ||
        copied != sizeof(length)) return -KERNEL_EFAULT;
    if (length < 0) return -KERNEL_EINVAL;
    copied = 0;
    size_t written = (size_t)length < sizeof(local)
                         ? (size_t)length : sizeof(local);
    if (written != 0 &&
        (kernel_copy_to_user(mm, user_address, &local, written, &copied) !=
             KERNEL_UACCESS_STATUS_OK || copied != written))
        return -KERNEL_EFAULT;
    length = sizeof(local);
    copied = 0;
    if (kernel_copy_to_user(mm, user_length, &length, sizeof(length),
                            &copied) != KERNEL_UACCESS_STATUS_OK ||
        copied != sizeof(length)) return -KERNEL_EFAULT;
    return 0;
}

static int wait_ready(struct kernel_task *caller,
                      struct kernel_open_file_description *file,
                      uint32_t event, uint64_t timeout_ns)
{
    struct kernel_socket *socket = kernel_open_file_socket(file);
    uint64_t deadline = 0;
    uint64_t target_ns = 0;
    uintptr_t saved;
    /* Ready data wins over even a one-microsecond receive timeout. */
    if ((kernel_socket_poll(socket, 0) & event) != 0U) return 0;
    if (timeout_ns != 0) {
        uint64_t now = kernel_time_monotonic_ns();
        uint64_t target = UINT64_MAX - now < timeout_ns
                              ? UINT64_MAX : now + timeout_ns;
        target_ns = target;
        enum kernel_time_status status =
            kernel_time_deadline_from_monotonic(target, &deadline);
        if (status == KERNEL_TIME_STATUS_DEADLINE_PASSED)
            return -KERNEL_EAGAIN;
        if (status != KERNEL_TIME_STATUS_OK) return -KERNEL_EIO;
    }
    saved = riscv_interrupt_save();
    while ((kernel_socket_poll(socket, 0) & event) == 0U) {
        enum kernel_wait_wake_reason reason;
        uint64_t sleep_deadline = deadline;
        uint64_t protocol_deadline = kernel_socket_next_timer_deadline();
        if (protocol_deadline != 0U &&
            (sleep_deadline == 0U || protocol_deadline < sleep_deadline))
            sleep_deadline = protocol_deadline;
        if ((kernel_open_file_flags(file) & LINUX_SOCK_NONBLOCK) != 0U) {
            riscv_interrupt_restore(saved);
            return -KERNEL_EAGAIN;
        }
        if (kernel_scheduler_block_current(
                kernel_socket_wait_queue(socket), sleep_deadline, 1,
                &reason) !=
            KERNEL_SCHEDULER_STATUS_OK) {
            riscv_interrupt_restore(saved);
            return -KERNEL_EIO;
        }
        if (reason == KERNEL_WAIT_TIMEOUT) {
            if (target_ns != 0U &&
                kernel_time_monotonic_ns() >= target_ns) {
                riscv_interrupt_restore(saved);
                return -KERNEL_EAGAIN;
            }
            continue;
        }
        if (reason == KERNEL_WAIT_SIGNALLED) {
            riscv_interrupt_restore(saved);
            if (timeout_ns != 0U) return -KERNEL_EINTR;
            kernel_signal_note_syscall_restart(caller);
            return -KERNEL_ERESTARTSYS;
        }
    }
    riscv_interrupt_restore(saved);
    return 0;
}

enum kernel_syscall_status syscall_handle_socket(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    int family = (int32_t)request->arguments[0];
    int type = (int32_t)request->arguments[1];
    int protocol = (int32_t)request->arguments[2];
    int base_type;

    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    if (((type & ~LINUX_SOCK_TYPE_MASK) &
         ~(LINUX_SOCK_CLOEXEC | LINUX_SOCK_NONBLOCK)) != 0) {
        decoded->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    base_type = type & LINUX_SOCK_TYPE_MASK;
    if (family < 0 || family >= LINUX_AF_MAX) {
        decoded->value = -KERNEL_EAFNOSUPPORT;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (base_type >= LINUX_SOCK_MAX) {
        decoded->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (family != LINUX_AF_INET) {
        decoded->value = -KERNEL_EAFNOSUPPORT;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if ((base_type == LINUX_SOCK_STREAM &&
         protocol != 0 && protocol != LINUX_IPPROTO_TCP) ||
        (base_type == LINUX_SOCK_DGRAM &&
         protocol != 0 && protocol != LINUX_IPPROTO_UDP)) {
        decoded->value = -KERNEL_EPROTONOSUPPORT;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (base_type != LINUX_SOCK_STREAM && base_type != LINUX_SOCK_DGRAM) {
        decoded->value = -KERNEL_ESOCKTNOSUPPORT;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    struct kernel_files *files;
    enum kernel_task_status task_status =
        kernel_task_files_borrow(caller, &files);
    if (task_status == KERNEL_TASK_STATUS_RESOURCE_UNAVAILABLE) {
        decoded->value = -KERNEL_EMFILE;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (task_status != KERNEL_TASK_STATUS_OK ||
        kernel_files_socket_create(files, base_type,
                                   (uint32_t)type, &decoded->value) !=
            KERNEL_FILES_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_socketpair(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    int family = (int32_t)request->arguments[0];
    int type = (int32_t)request->arguments[1];
    int protocol = (int32_t)request->arguments[2];
    uint64_t user_sv = request->arguments[3];
    int base_type;

    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    if (((type & ~LINUX_SOCK_TYPE_MASK) &
         ~(LINUX_SOCK_CLOEXEC | LINUX_SOCK_NONBLOCK)) != 0) {
        decoded->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    base_type = type & LINUX_SOCK_TYPE_MASK;
    if (family < 0 || family >= LINUX_AF_MAX) {
        decoded->value = -KERNEL_EAFNOSUPPORT;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (base_type >= LINUX_SOCK_MAX) {
        decoded->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (family != LINUX_AF_UNIX) {
        decoded->value = -KERNEL_EOPNOTSUPP;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (base_type != LINUX_SOCK_STREAM && base_type != LINUX_SOCK_DGRAM) {
        decoded->value = -KERNEL_ESOCKTNOSUPPORT;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (protocol != 0 && protocol != LINUX_AF_UNIX) {
        decoded->value = -KERNEL_EPROTONOSUPPORT;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (kernel_user_range_check(user_sv, sizeof(int32_t) * 2) !=
        KERNEL_UACCESS_STATUS_OK) {
        decoded->value = -KERNEL_EFAULT;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    struct kernel_files *files;
    struct kernel_mm *mm;
    enum kernel_task_status task_status =
        kernel_task_files_borrow(caller, &files);
    if (task_status == KERNEL_TASK_STATUS_RESOURCE_UNAVAILABLE) {
        decoded->value = -KERNEL_EMFILE;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (task_status != KERNEL_TASK_STATUS_OK ||
        kernel_task_mm_borrow_mutable(caller, &mm) != KERNEL_TASK_STATUS_OK ||
        kernel_files_socketpair_create(files, mm, base_type,
                                       (uint32_t)type, user_sv,
                                       &decoded->value) !=
            KERNEL_FILES_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_bind(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    struct kernel_files *files;
    struct kernel_open_file_description *file = 0;
    struct kernel_mm *mm;
    struct linux_sockaddr_in address;
    int64_t linux_result = 0;
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    if (borrow_socket(caller, (int32_t)request->arguments[0],
                      &files, &file, &linux_result) != KERNEL_SYSCALL_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    (void)files;
    if (linux_result != 0) {
        decoded->value = linux_result;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (kernel_task_mm_borrow_mutable(caller, &mm) != KERNEL_TASK_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    linux_result = copy_address_in(mm, request->arguments[1],
                                   request->arguments[2], &address);
    if (linux_result == 0)
        linux_result = kernel_socket_bind(kernel_open_file_socket(file),
                                          address.address,
                                          network_port(address.port));
    if (release_socket(&file) != KERNEL_SYSCALL_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    decoded->value = linux_result;
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_socket_operation(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    struct kernel_files *files;
    struct kernel_open_file_description *file = 0;
    struct kernel_socket *socket;
    struct kernel_mm *mm;
    struct linux_sockaddr_in address;
    uint32_t peer_address = 0;
    uint16_t peer_port = 0;
    int64_t result = 0;
    int op = (int)request->number;

    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    /* import_ubuf precedes fd lookup and the UDP receive wait on Linux. */
    if (op == 207 && kernel_user_range_check(
            request->arguments[1], (size_t)request->arguments[2]) !=
            KERNEL_UACCESS_STATUS_OK) {
        decoded->value = -KERNEL_EFAULT;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (borrow_socket(caller, (int32_t)request->arguments[0],
                      &files, &file, &result) != KERNEL_SYSCALL_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    if (result != 0) {
        decoded->value = result;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    socket = kernel_open_file_socket(file);
    if (kernel_task_mm_borrow_mutable(caller, &mm) != KERNEL_TASK_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;

    switch (op) {
    case 201: /* listen */
        result = kernel_socket_listen(socket, (int32_t)request->arguments[1]);
        break;
    case 204: /* getsockname */
        result = kernel_socket_getname(socket, &peer_address, &peer_port);
        if (result == 0)
            result = copy_address_out(mm, request->arguments[1],
                                      request->arguments[2], peer_address,
                                      peer_port);
        break;
    case 203: /* connect */
        result = copy_address_in(mm, request->arguments[1],
                                 request->arguments[2], &address);
        if (result == 0)
            result = kernel_socket_connect(socket, address.address,
                     network_port(address.port),
                     (kernel_open_file_flags(file) & LINUX_SOCK_NONBLOCK) != 0U);
        if (result == -KERNEL_EINPROGRESS &&
            (kernel_open_file_flags(file) & LINUX_SOCK_NONBLOCK) == 0U) {
            result = wait_ready(caller, file, KERNEL_POLLOUT | KERNEL_POLLERR,
                                0U);
            if (result == 0) result = kernel_socket_connection_result(socket);
        }
        break;
    case 206: /* sendto */
        if (request->arguments[3] != 0U) {
            result = -KERNEL_EOPNOTSUPP;
            break;
        }
        result = copy_address_in(mm, request->arguments[4],
                                 request->arguments[5], &address);
        if (result == 0)
            result = kernel_socket_sendto(socket, mm,
                     request->arguments[1], request->arguments[2],
                     address.address, network_port(address.port));
        break;
    case 207: /* recvfrom */
        if (request->arguments[3] != 0U) {
            result = -KERNEL_EOPNOTSUPP;
            break;
        }
        result = wait_ready(caller, file, KERNEL_POLLIN,
                            kernel_socket_receive_timeout(socket));
        if (result == 0)
            result = kernel_socket_recvfrom(socket, mm, request->arguments[1],
                     request->arguments[2], &peer_address, &peer_port);
        if (result >= 0 && request->arguments[4] != 0U) {
            int address_result = copy_address_out(mm, request->arguments[4],
                request->arguments[5], peer_address, peer_port);
            if (address_result != 0) result = address_result;
        }
        break;
    case 208: { /* setsockopt */
        struct linux_timeval timeout;
        size_t copied = 0;
        if ((int32_t)request->arguments[1] != LINUX_SOL_SOCKET ||
            (int32_t)request->arguments[2] != LINUX_SO_RCVTIMEO) {
            result = -KERNEL_ENOTSUP;
            break;
        }
        if ((uint32_t)request->arguments[4] < sizeof(timeout)) {
            result = -KERNEL_EINVAL;
            break;
        }
        if (kernel_copy_from_user(mm, &timeout, request->arguments[3],
                                  sizeof(timeout), &copied) !=
                KERNEL_UACCESS_STATUS_OK || copied != sizeof(timeout)) {
            result = -KERNEL_EFAULT;
            break;
        }
        if (timeout.seconds < 0 || timeout.microseconds < 0 ||
            timeout.microseconds >= 1000000 ||
            (uint64_t)timeout.seconds >
                (UINT64_MAX - (uint64_t)timeout.microseconds * 1000U) /
                    UINT64_C(1000000000)) {
            result = -KERNEL_EINVAL;
            break;
        }
        kernel_socket_set_receive_timeout(socket,
            (uint64_t)timeout.seconds * UINT64_C(1000000000) +
            (uint64_t)timeout.microseconds * UINT64_C(1000));
        break;
    }
    case 202: /* accept */
        result = kernel_socket_accept_check(socket);
        if (result != 0) break;
        result = wait_ready(caller, file, KERNEL_POLLIN,
                            kernel_socket_receive_timeout(socket));
        if (result != 0) break;
        if (kernel_files_socket_accept(files, file, 0U, &peer_address,
                                       &peer_port, &result) !=
            KERNEL_FILES_STATUS_OK)
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        if (result >= 0 && request->arguments[1] != 0U) {
            int address_result = copy_address_out(mm, request->arguments[1],
                request->arguments[2], peer_address, peer_port);
            if (address_result != 0) {
                int64_t close_result;
                if (kernel_files_close(files, result, &close_result) !=
                    KERNEL_FILES_STATUS_OK || close_result != 0)
                    return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
                result = address_result;
            }
        }
        break;
    default:
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    if (release_socket(&file) != KERNEL_SYSCALL_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    decoded->value = result;
    return KERNEL_SYSCALL_STATUS_OK;
}
