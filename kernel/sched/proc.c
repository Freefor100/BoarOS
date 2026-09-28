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
           task->state != KERNEL_THREAD_STATE_GROUP_DEAD;
}

static struct kernel_task *next_process(struct kernel_task *task,
                                        struct kernel_task *root)
{
    if (task->first_child) return task->first_child;
    while (task != root) {
        if (task->next_sibling) return task->next_sibling;
        task = task->parent;
        if (!task) return 0;
    }
    return 0;
}

static struct kernel_task *representative(struct kernel_task *leader)
{
    if (!leader || !leader->tid_owned || leader->tid <= 0 ||
        !leader->group_next) return 0;
    struct kernel_task *member = leader;
    do {
        if (proc_visible(member)) return member;
        member = member->group_next;
    } while (member != leader);
    return 0;
}

static struct kernel_task *find_member(kernel_pid_t pid, uint64_t identity)
{
    struct kernel_task *root = scheduler.init_task;
    for (struct kernel_task *leader = root; leader;
         leader = next_process(leader, root))
        if (leader->tid == pid && leader->proc_identity == identity)
            return representative(leader);
    return 0;
}

void kernel_proc_task_note_block_read(void)
{
    struct kernel_task *task = kernel_task_current();
    if (task && task->arch.user_mode && task->block_reads != UINT64_MAX)
        task->block_reads++;
}

uint64_t kernel_proc_task_block_reads(const struct kernel_task *task)
{
    return task ? task->block_reads : 0U;
}

void kernel_proc_task_note_fault(struct kernel_task *task, int major)
{
    if (!task || !task->arch.user_mode) return;
    uintptr_t irq = riscv_interrupt_save();
    uint64_t *counter = major ? &task->major_faults : &task->minor_faults;
    if (*counter != UINT64_MAX) (*counter)++;
    riscv_interrupt_restore(irq);
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
        .session_id = leader->session_id,
        .identity = identity,
        .start_ticks = leader->proc_start_ticks,
        .state = task->state == KERNEL_THREAD_STATE_RUNNING ||
                 task->state == KERNEL_THREAD_STATE_READY ? 'R' :
                 task->state == KERNEL_THREAD_STATE_STOPPED ? 'T' :
                 task->state == KERNEL_THREAD_STATE_ZOMBIE ? 'Z' : 'S',
        .child_minor_faults = leader->child_minor_faults,
        .child_major_faults = leader->child_major_faults,
        .threads = leader->group_members,
    };
    memcpy(result->comm, leader->comm, sizeof(result->comm));
    kernel_task_cpu_ticks(task, &result->user_ticks,
        &result->kernel_ticks, &result->child_user_ticks,
        &result->child_kernel_ticks);
    struct kernel_task *member = leader;
    do {
        result->minor_faults += member->minor_faults;
        result->major_faults += member->major_faults;
        member = member->group_next;
    } while (member != leader);
    if (task->mm.state == KERNEL_MM_LIVE) {
        struct kernel_mm_proc_memory memory;
        if (kernel_mm_proc_memory_snapshot(&task->mm, &memory) !=
            KERNEL_MM_STATUS_OK) {
            riscv_interrupt_restore(irq);
            return -KERNEL_EIO;
        }
        result->virtual_bytes = memory.virtual_bytes;
        result->resident_pages = memory.resident_pages;
    }
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
    if (!task) return;
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
    struct kernel_task *root = scheduler.init_task;
    for (struct kernel_task *leader = root; leader;
         leader = next_process(leader, root))
        consider(representative(leader), after, &best, &token);
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
