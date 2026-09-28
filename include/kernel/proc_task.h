#ifndef BOAROS_KERNEL_PROC_TASK_H
#define BOAROS_KERNEL_PROC_TASK_H

#include <kernel/pid.h>
#include <stdint.h>

struct kernel_vfs_path;

enum kernel_proc_path_kind {
    KERNEL_PROC_PATH_EXE = 1,
    KERNEL_PROC_PATH_CWD,
    KERNEL_PROC_PATH_ROOT,
    KERNEL_PROC_PATH_FD,
};

struct kernel_proc_process_snapshot {
    kernel_pid_t pid;
    kernel_pid_t ppid;
    kernel_pid_t process_group;
    uint64_t identity;
    uint64_t start_ticks;
    uint64_t user_ticks;
    uint64_t kernel_ticks;
    uint64_t child_user_ticks;
    uint64_t child_kernel_ticks;
    char state;
    char comm[16];
};

/* Identity changes on PID reuse; zero asks for the current allocation. */
int kernel_proc_process_identity(kernel_pid_t pid, uint64_t *identity);
int kernel_proc_next_process(kernel_pid_t after, kernel_pid_t *pid,
                             uint64_t *identity);
int kernel_proc_process_snapshot(kernel_pid_t pid, uint64_t identity,
                                 struct kernel_proc_process_snapshot *result);
int kernel_proc_process_path_acquire(kernel_pid_t pid, uint64_t identity,
                                     enum kernel_proc_path_kind kind, int fd,
                                     struct kernel_vfs_path **owner);

#endif
