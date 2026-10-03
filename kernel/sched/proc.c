#include "private.h"

#include <arch/riscv/context.h>
#include <kernel/errno.h>
#include <kernel/files.h>
#include <kernel/open_file.h>
#include <kernel/proc_task.h>
#include <kernel/fs_context.h>
#include <kernel/task.h>
#include <kernel/tty_task.h>
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
    if (!task || !task->arch.user_mode || !task->tid_owned ||
        !task->group_leader || !task->group_leader->tid_owned ||
        task->group_leader->tid <= 0 ||
        task->state == KERNEL_THREAD_STATE_GROUP_DEAD) return 0;
    if (task->state != KERNEL_THREAD_STATE_EXITED) return 1;
    /* 子进程在清理期间仍占有 PID；wait 回收前不能暂时丢失数字目录。 */
    return task == task->group_leader && task->group_members == 1U &&
           task->parent != 0;
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
    struct kernel_task *leader = process_find_identity(pid, KERNEL_PID_TGID);
    return leader && process_identity_generation(leader) == identity &&
        representative(leader) ? leader : 0;
}

static struct kernel_task *find_resource_owner(kernel_pid_t pid,
                                                uint64_t identity)
{
    struct kernel_task *leader = find_member(pid, identity);
    return leader && !leader->proc_exiting ? leader : 0;
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
    uint32_t threads = leader->group_members;
    struct kernel_task *member = leader->group_next;
    while (member != leader) {
        if (member->proc_exiting && threads) threads--;
        member = member->group_next;
    }
    *result = (struct kernel_proc_process_snapshot){
        .pid = pid,
        .ppid = leader->parent && leader->parent->group_leader
            ? leader->parent->group_leader->tid : 0,
        .process_group = process_identity_number(leader, KERNEL_PID_PGID),
        .session_id = process_identity_number(leader, KERNEL_PID_SID),
        .tty_nr = kernel_tty_rdev(kernel_task_controlling_tty(leader)),
        .tty_pgrp = kernel_tty_foreground(kernel_task_controlling_tty(leader)),
        .identity = identity,
        .start_ticks = leader->proc_start_ticks,
        .state = task->proc_exiting ||
                 task->state == KERNEL_THREAD_STATE_EXITED ||
                 task->state == KERNEL_THREAD_STATE_GROUP_DEAD ||
                 task->state == KERNEL_THREAD_STATE_ZOMBIE ? 'Z' :
                 task->state == KERNEL_THREAD_STATE_RUNNING ||
                 task->state == KERNEL_THREAD_STATE_READY ? 'R' :
                 task->state == KERNEL_THREAD_STATE_STOPPED ? 'T' : 'S',
        .child_minor_faults = leader->child_minor_faults,
        .child_major_faults = leader->child_major_faults,
        .signal_pending = leader->signal_pending & UINT64_C(0x7fffffff),
        .signal_blocked = leader->signal_blocked & UINT64_C(0x7fffffff),
        .threads = threads,
        .scheduling_priority = leader->scheduling.priority
            ? -(leader->scheduling.priority + 1) : 20,
        .rt_priority = (uint32_t)leader->scheduling.priority,
        .scheduling_policy = (uint32_t)leader->scheduling.policy,
    };
    result->wait_channel_flag = threads < 2U &&
        task->state != KERNEL_THREAD_STATE_RUNNING &&
        task->state != KERNEL_THREAD_STATE_READY;
    const struct kernel_signal_table *actions =
        (const void *)(uintptr_t)leader->signal_table_address;
    if (actions) {
        if (actions->magic != KERNEL_SIGNAL_TABLE_MAGIC) __builtin_trap();
        for (unsigned sig = 0; sig < 31U; sig++) {
            if (actions->actions[sig].handler == KERNEL_SIGNAL_IGN)
                result->signal_ignored |= UINT64_C(1) << sig;
            else if (actions->actions[sig].handler != KERNEL_SIGNAL_DFL)
                result->signal_caught |= UINT64_C(1) << sig;
        }
    }
    memcpy(result->comm, leader->comm, sizeof(result->comm));
    kernel_task_cpu_ticks(task, &result->user_ticks,
        &result->kernel_ticks, &result->child_user_ticks,
        &result->child_kernel_ticks);
    member = leader;
    do {
        result->minor_faults += member->minor_faults;
        result->major_faults += member->major_faults;
        member = member->group_next;
    } while (member != leader);
    if (!task->proc_exiting && task->mm.state == KERNEL_MM_LIVE) {
        struct kernel_mm_proc_memory memory;
        if (kernel_mm_proc_memory_snapshot(&task->mm, &memory) !=
            KERNEL_MM_STATUS_OK) {
            riscv_interrupt_restore(irq);
            return -KERNEL_EIO;
        }
        result->virtual_bytes = memory.virtual_bytes;
        result->resident_pages = memory.resident_pages;
        result->start_code = memory.start_code;
        result->end_code = memory.end_code;
        result->start_stack = memory.start_stack;
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
    struct kernel_task *task = find_resource_owner(pid, identity);
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

int kernel_proc_next_fd(kernel_pid_t pid, uint64_t identity,
                         int after, int *fd)
{
    if (pid <= 0 || !identity || !fd || after < -1)
        return -KERNEL_EINVAL;
    uintptr_t irq = riscv_interrupt_save();
    struct kernel_task *task = find_resource_owner(pid, identity);
    int result = task && kernel_files_is_live(&task->files)
        ? kernel_files_next_open_fd(&task->files, after, fd)
        : -KERNEL_ENOENT;
    riscv_interrupt_restore(irq);
    return result;
}

int kernel_proc_fd_access_snapshot(kernel_pid_t pid, uint64_t identity,
                                   int fd, int *readable, int *writable)
{
    if (pid <= 0 || !identity || fd < 0 || !readable || !writable)
        return -KERNEL_EINVAL;
    uintptr_t irq = riscv_interrupt_save();
    struct kernel_task *task = find_resource_owner(pid, identity);
    struct kernel_open_file_description *file = task &&
        kernel_files_is_live(&task->files)
        ? kernel_files_fd_borrow(&task->files, fd) : 0;
    if (file) {
        if (kernel_open_file_kind(file) == KERNEL_OPEN_FILE_KIND_EPOLL) {
            *readable = 1;
            *writable = 1;
        } else {
            *readable = kernel_open_file_readable(file);
            *writable = kernel_open_file_writable(file);
        }
    }
    riscv_interrupt_restore(irq);
    return file ? 0 : -KERNEL_ENOENT;
}

int kernel_proc_fd_path_acquire(kernel_pid_t pid, uint64_t identity,
                                int fd, struct kernel_vfs_path **owner)
{
    if (pid <= 0 || !identity || fd < 0 || !owner || *owner)
        return -KERNEL_EINVAL;
    uintptr_t irq = riscv_interrupt_save();
    struct kernel_task *task = find_resource_owner(pid, identity);
    struct kernel_open_file_description *file = task &&
        kernel_files_is_live(&task->files)
        ? kernel_files_fd_borrow(&task->files, fd) : 0;
    struct kernel_vfs_path *path = file ? kernel_open_file_path(file) : 0;
    int result = -KERNEL_ENOENT;
    if (path && !kernel_vfs_path_acquire(path)) {
        *owner = path;
        result = 0;
    }
    riscv_interrupt_restore(irq);
    return result;
}

int kernel_proc_fd_pseudo_snapshot(kernel_pid_t pid, uint64_t identity,
                                   int fd,
                                   struct kernel_proc_fd_pseudo *snapshot)
{
    if (pid <= 0 || !identity || fd < 0 || !snapshot)
        return -KERNEL_EINVAL;
    uintptr_t irq = riscv_interrupt_save();
    struct kernel_task *task = find_resource_owner(pid, identity);
    struct kernel_open_file_description *file = task &&
        kernel_files_is_live(&task->files)
        ? kernel_files_fd_borrow(&task->files, fd) : 0;
    int result = file ? -KERNEL_ENOTSUP : -KERNEL_ENOENT;
    if (file && !kernel_open_file_path(file)) {
        snapshot->kind = (uint8_t)kernel_open_file_kind(file);
        snapshot->object_identity = kernel_open_file_pseudo_identity(file);
        result = 0;
    }
    riscv_interrupt_restore(irq);
    return result;
}

int kernel_proc_fd_pseudo_stat(kernel_pid_t pid, uint64_t identity,
                               int fd, struct kernel_vfs_stat *stat)
{
    if (pid <= 0 || !identity || fd < 0 || !stat)
        return -KERNEL_EINVAL;
    uintptr_t irq = riscv_interrupt_save();
    struct kernel_task *task = find_resource_owner(pid, identity);
    struct kernel_open_file_description *file = task &&
        kernel_files_is_live(&task->files)
        ? kernel_files_fd_borrow(&task->files, fd) : 0;
    /* 伪对象的元数据快照不分配也不等待；解锁后不再访问 OFD。 */
    int result = file ? kernel_open_file_pseudo_stat(file, stat)
                      : -KERNEL_ENOENT;
    riscv_interrupt_restore(irq);
    return result;
}

int kernel_proc_fd_reopen_link(kernel_pid_t pid, uint64_t identity,
                               int fd, struct kernel_heap *heap,
                               uint32_t flags,
                               struct kernel_open_file_description **owner)
{
    if (pid <= 0 || !identity || fd < 0 || !heap || !owner || *owner)
        return -KERNEL_EINVAL;
    struct kernel_open_file_pipe_pin pin = {0};
    uintptr_t irq = riscv_interrupt_save();
    struct kernel_task *task = find_resource_owner(pid, identity);
    struct kernel_open_file_description *file = task &&
        kernel_files_is_live(&task->files)
        ? kernel_files_fd_borrow(&task->files, fd) : 0;
    int result = file ? -KERNEL_ENXIO : -KERNEL_ENOENT;
    /* 解锁后 fd 可关闭；先钉住 pipe endpoint 再分配新 OFD。 */
    if (file && kernel_open_file_path(file)) result = -KERNEL_ENOTSUP;
    else if (file &&
             kernel_open_file_kind(file) == KERNEL_OPEN_FILE_KIND_PIPE)
        result = kernel_open_file_pipe_pin(file, flags, &pin);
    riscv_interrupt_restore(irq);
    if (result || !pin.pipe) return result;
    return kernel_open_file_pipe_finish(heap, &pin, flags, owner);
}

static void consider(const struct kernel_task *task, kernel_pid_t after,
                     kernel_pid_t *best, uint64_t *identity)
{
    if (!task) return;
    kernel_pid_t pid = task->group_leader->tid;
    if (pid > after && (*best == 0 || pid < *best)) {
        *best = pid;
        *identity = process_identity_generation(task);
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
