#ifndef BOAROS_KERNEL_PROC_TASK_H
#define BOAROS_KERNEL_PROC_TASK_H

#include <kernel/pid.h>
#include <stdint.h>

struct kernel_vfs_path;
struct kernel_task;
struct kernel_open_file_description;
struct kernel_heap;
struct kernel_vfs_stat;

enum kernel_proc_path_kind {
    KERNEL_PROC_PATH_EXE = 1,
    KERNEL_PROC_PATH_CWD,
    KERNEL_PROC_PATH_ROOT,
    KERNEL_PROC_PATH_FD,
};

struct kernel_proc_process_snapshot {
    kernel_pid_t pid, tgid;
    kernel_pid_t ppid;
    kernel_pid_t process_group;
    kernel_pid_t session_id;
    uint64_t tty_nr;
    int32_t tty_pgrp;
    uint64_t identity;
    uint64_t start_ticks;
    uint64_t user_ticks;
    uint64_t kernel_ticks;
    uint64_t child_user_ticks;
    uint64_t child_kernel_ticks;
    uint64_t minor_faults;
    uint64_t major_faults;
    uint64_t child_minor_faults;
    uint64_t child_major_faults;
    uint64_t virtual_bytes;
    uint64_t resident_pages;
    uint64_t start_code, end_code, start_stack;
    uint64_t signal_pending, signal_shared_pending, signal_blocked, signal_ignored, signal_caught;
    uint64_t signal_queued, signal_queue_limit;
    uint32_t threads;
    uint32_t wait_channel_flag;
    int32_t scheduling_priority;
    uint32_t rt_priority;
    uint32_t scheduling_policy;
    char state;
    char comm[16];
};

/* Identity changes on PID reuse; zero asks for the current allocation. */
int kernel_proc_process_identity(kernel_pid_t pid, uint64_t *identity);
/* Exclusive upper bound of the live PID/TID allocator. */
uint32_t kernel_proc_pid_max(void);
int kernel_proc_next_process(kernel_pid_t after, kernel_pid_t *pid,
                             uint64_t *identity);
/* A zero tgid skips membership filtering; nonzero also checks its generation. */
int kernel_proc_thread_identity(kernel_pid_t tgid, uint64_t group_identity,
    kernel_pid_t tid, uint64_t *identity);
int kernel_proc_thread_group(kernel_pid_t tid, uint64_t identity,
    kernel_pid_t *tgid, uint64_t *group_identity);
int kernel_proc_next_thread(kernel_pid_t tgid, uint64_t group_identity,
    kernel_pid_t after, kernel_pid_t *tid, uint64_t *identity);
int kernel_proc_thread_snapshot(kernel_pid_t tid, uint64_t identity,
    struct kernel_proc_process_snapshot *result);
int kernel_proc_process_snapshot(kernel_pid_t pid, uint64_t identity,
                                 struct kernel_proc_process_snapshot *result);
int kernel_proc_process_path_acquire(kernel_pid_t pid, uint64_t identity,
                                     enum kernel_proc_path_kind kind, int fd,
                                     struct kernel_vfs_path **owner);
int kernel_proc_next_fd(kernel_pid_t pid, uint64_t identity,
                         int after, int *fd);
int kernel_proc_fd_access_snapshot(kernel_pid_t pid, uint64_t identity,
                                   int fd, int *readable, int *writable);
int kernel_proc_fd_path_acquire(kernel_pid_t pid, uint64_t identity,
                                int fd, struct kernel_vfs_path **owner);
struct kernel_proc_fd_pseudo {
    uint64_t object_identity;
    uint8_t kind;
};
int kernel_proc_fd_pseudo_snapshot(kernel_pid_t pid, uint64_t identity,
                                   int fd,
                                   struct kernel_proc_fd_pseudo *snapshot);
int kernel_proc_fd_reopen_link(kernel_pid_t pid, uint64_t identity,
                               int fd, struct kernel_heap *heap,
                               uint32_t flags,
                               struct kernel_open_file_description **owner);
int kernel_proc_fd_pseudo_stat(kernel_pid_t pid, uint64_t identity,
                               int fd, struct kernel_vfs_stat *stat);

#endif
