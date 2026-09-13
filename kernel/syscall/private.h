#ifndef BOAROS_KERNEL_SYSCALL_PRIVATE_H
#define BOAROS_KERNEL_SYSCALL_PRIVATE_H

#include <kernel/syscall.h>

enum kernel_syscall_status syscall_handle_openat(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded);

enum kernel_syscall_status syscall_handle_pipe2(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded);

enum kernel_syscall_status syscall_handle_read(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded);

enum kernel_syscall_status syscall_handle_pread64(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded);

enum kernel_syscall_status syscall_handle_write(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded);

enum kernel_syscall_status syscall_handle_lseek(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded);

enum kernel_syscall_status syscall_handle_fstat(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded);

enum kernel_syscall_status syscall_handle_newfstatat(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded);

enum kernel_syscall_status syscall_handle_dup(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded);

enum kernel_syscall_status syscall_handle_dup3(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded);

enum kernel_syscall_status syscall_handle_fcntl(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded);

enum kernel_syscall_status syscall_handle_getdents64(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded);

enum kernel_syscall_status syscall_handle_ppoll(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded);

enum kernel_syscall_status syscall_handle_pselect6(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded);

enum kernel_syscall_status syscall_handle_writev(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded);

enum kernel_syscall_status syscall_handle_close(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded);

enum kernel_syscall_status syscall_handle_brk(
    struct kernel_task *caller,
    uint64_t requested,
    struct kernel_syscall_result *decoded);

enum kernel_syscall_status syscall_handle_mmap(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded);

enum kernel_syscall_status syscall_handle_munmap(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded);

enum kernel_syscall_status syscall_handle_mprotect(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded);

enum kernel_syscall_status syscall_handle_set_tid_address(
    struct kernel_task *caller,
    uint64_t address,
    struct kernel_syscall_result *decoded);

enum kernel_syscall_status syscall_handle_execve(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded);

void syscall_decode_clone(const struct kernel_syscall_request *request,
                         struct kernel_syscall_result *decoded);

enum kernel_syscall_status syscall_handle_rt_sigaction(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded);

enum kernel_syscall_status syscall_handle_rt_sigprocmask(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded);

enum kernel_syscall_status syscall_handle_rt_sigpending(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded);

enum kernel_syscall_status syscall_handle_rt_sigsuspend(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded);

enum kernel_syscall_status syscall_handle_signal_send(
    struct kernel_task *caller,
    uint32_t kind,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded);

enum kernel_syscall_status syscall_handle_clock_gettime(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded);

enum kernel_syscall_status syscall_handle_clock_getres(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded);

enum kernel_syscall_status syscall_handle_gettimeofday(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded);

enum kernel_syscall_status syscall_handle_times(
    struct kernel_task *caller,
    uint64_t tms_address,
    struct kernel_syscall_result *decoded);

enum kernel_syscall_status syscall_handle_restart_syscall(
    struct kernel_task *caller,
    struct kernel_syscall_result *decoded);

enum kernel_syscall_status syscall_handle_sleep_for(
    struct kernel_task *caller,
    uint32_t clock_id,
    uint32_t flags,
    uint64_t request_address,
    uint64_t remaining_address,
    struct kernel_syscall_result *decoded);

#endif
