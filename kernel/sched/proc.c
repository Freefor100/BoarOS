#include "private.h"

#include <arch/riscv/context.h>
#include <kernel/errno.h>
#include <kernel/proc_task.h>
#include <kernel/fs_context.h>
#include <kernel/task.h>
#include <kernel/vfs.h>
#include <string.h>

void kernel_proc_task_update_comm(struct kernel_task *task)
{
    struct kernel_vfs_path *path = 0;
    if (kernel_mm_executable_path_acquire(&task->mm, &path) !=
        KERNEL_MM_STATUS_OK) return;
    const char *name = kernel_vfs_path_name(path);
    if (name) {
        size_t length = strlen(name);
        if (length >= sizeof(task->comm)) length = sizeof(task->comm) - 1U;
        memcpy(task->comm, name, length);
        task->comm[length] = '\0';
    }
    if (kernel_vfs_path_release(&path)) __builtin_trap();
}

static int proc_visible(const struct kernel_task *task)
{
    return task && task->arch.user_mode && task->tid_owned &&
           task->group_leader && task->group_leader->tid_owned &&
           task->group_leader->tid > 0 &&
           task->state != KERNEL_THREAD_STATE_EXITED &&
           task->state != KERNEL_THREAD_STATE_ZOMBIE &&
           task->state != KERNEL_THREAD_STATE_GROUP_DEAD;
}

static struct kernel_task *find_member(kernel_pid_t pid, uint64_t identity)
{
    struct kernel_task *task = scheduler.current;
    if (proc_visible(task) && task->group_leader->tid == pid &&
        task->group_leader->proc_identity == identity) return task;
    for (task = scheduler.ready_head; task; task = task->next)
        if (proc_visible(task) && task->group_leader->tid == pid &&
            task->group_leader->proc_identity == identity) return task;
    for (task = scheduler.blocked_head; task; task = task->next)
        if (proc_visible(task) && task->group_leader->tid == pid &&
            task->group_leader->proc_identity == identity) return task;
    for (task = scheduler.stopped_head; task; task = task->next)
        if (proc_visible(task) && task->group_leader->tid == pid &&
            task->group_leader->proc_identity == identity) return task;
    return 0;
}

int kernel_proc_process_snapshot(kernel_pid_t pid, uint64_t identity,
                                 struct kernel_proc_process_snapshot *result)
{
    if (pid <= 0 || !identity || !result) return -KERNEL_EINVAL;
    uintptr_t irq = riscv_interrupt_save();
    struct kernel_task *task = find_member(pid, identity);
    if (!task) {
        riscv_interrupt_restore(irq);
        return -KERNEL_ENOENT;
    }
    struct kernel_task *leader = task->group_leader;
    *result = (struct kernel_proc_process_snapshot){
        .pid = pid,
        .ppid = leader->parent && leader->parent->group_leader
            ? leader->parent->group_leader->tid : 0,
        .process_group = leader->process_group,
        .identity = identity,
        .start_ticks = leader->proc_start_ticks,
        .state = task->state == KERNEL_THREAD_STATE_RUNNING ? 'R' :
                 task->state == KERNEL_THREAD_STATE_STOPPED ? 'T' : 'S',
    };
    memcpy(result->comm, leader->comm, sizeof(result->comm));
    kernel_task_cpu_ticks(task, &result->user_ticks,
        &result->kernel_ticks, &result->child_user_ticks,
        &result->child_kernel_ticks);
    riscv_interrupt_restore(irq);
    return 0;
}

int kernel_proc_process_path_acquire(kernel_pid_t pid, uint64_t identity,
                                     enum kernel_proc_path_kind kind, int fd,
                                     struct kernel_vfs_path **owner)
{
    (void)fd;
    if (pid <= 0 || !identity || !owner || *owner)
        return -KERNEL_EINVAL;
    uintptr_t irq = riscv_interrupt_save();
    struct kernel_task *task = find_member(pid, identity);
    if (!task) {
        riscv_interrupt_restore(irq);
        return -KERNEL_ENOENT;
    }
    int result = -KERNEL_ENOENT;
    if (kind == KERNEL_PROC_PATH_EXE) {
        if (task->mm.state == KERNEL_MM_LIVE &&
            kernel_mm_executable_path_acquire(&task->mm, owner) ==
                KERNEL_MM_STATUS_OK) result = 0;
    } else if (kind == KERNEL_PROC_PATH_CWD ||
               kind == KERNEL_PROC_PATH_ROOT) {
        if (kernel_fs_context_is_live(&task->fs)) {
            struct kernel_vfs_path *path = kind == KERNEL_PROC_PATH_CWD
                ? kernel_fs_context_cwd(&task->fs)
                : kernel_fs_context_root(&task->fs);
            if (path && !kernel_vfs_path_acquire(path)) {
                *owner = path;
                result = 0;
            }
        }
    }
    riscv_interrupt_restore(irq);
    return result;
}

static void consider(const struct kernel_task *task, kernel_pid_t after,
                     kernel_pid_t *best, uint64_t *identity)
{
    if (!proc_visible(task)) return;
    kernel_pid_t pid = task->group_leader->tid;
    if (pid > after && (*best == 0 || pid < *best)) {
        *best = pid;
        *identity = task->group_leader->proc_identity;
    }
}

int kernel_proc_next_process(kernel_pid_t after, kernel_pid_t *pid,
                             uint64_t *identity)
{
    if (!pid || !identity || after < 0) return -KERNEL_EINVAL;
    kernel_pid_t best = 0;
    uint64_t token = 0U;
    uintptr_t irq = riscv_interrupt_save();
    consider(scheduler.current, after, &best, &token);
    for (const struct kernel_task *task = scheduler.ready_head;
         task; task = task->next) consider(task, after, &best, &token);
    for (const struct kernel_task *task = scheduler.blocked_head;
         task; task = task->next) consider(task, after, &best, &token);
    for (const struct kernel_task *task = scheduler.stopped_head;
         task; task = task->next) consider(task, after, &best, &token);
    riscv_interrupt_restore(irq);
    if (!best) return -KERNEL_ENOENT;
    *pid = best;
    *identity = token;
    return 0;
}

int kernel_proc_process_identity(kernel_pid_t pid, uint64_t *identity)
{
    if (pid <= 0 || !identity) return -KERNEL_EINVAL;
    kernel_pid_t found;
    uint64_t token;
    kernel_pid_t after = pid - 1;
    int result = kernel_proc_next_process(after, &found, &token);
    if (result) return result;
    if (found != pid) return -KERNEL_ENOENT;
    *identity = token;
    return 0;
}
