#ifndef BOAROS_KERNEL_PROC_TASK_H
#define BOAROS_KERNEL_PROC_TASK_H

#include <kernel/pid.h>
#include <stdint.h>

struct kernel_vfs_path;
struct kernel_task;
struct kernel_open_file_description;
struct kernel_heap;

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
    kernel_pid_t session_id;
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
    uint32_t threads;
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

/* Fault I/O attribution belongs to the running task, including after sleep. */
void kernel_proc_task_note_block_read(void);
uint64_t kernel_proc_task_block_reads(const struct kernel_task *task);
void kernel_proc_task_note_fault(struct kernel_task *task, int major);

#endif
