#include "private.h"
#include <kernel/errno.h>
#include <kernel/signal.h>
#include <kernel/tty_task.h>
struct kernel_tty *kernel_task_controlling_tty(const struct kernel_task *task)
{
    return task && task->group_leader ? task->group_leader->controlling_tty : 0;
}
int kernel_task_tty_set(struct kernel_task *task, struct kernel_tty *tty)
{
    if (!task || !task->group_leader || !tty)
        return -KERNEL_EINVAL;
    struct kernel_task *leader = task->group_leader;
    if (leader->controlling_tty == tty)
        return 0;
    if (leader->controlling_tty)
        return -KERNEL_EPERM;
    kernel_tty_get(tty);
    leader->controlling_tty = tty;
    return 0;
}
void kernel_task_tty_clear(struct kernel_task *task)
{
    if (!task || !task->group_leader)
        return;
    struct kernel_task *leader = task->group_leader;
    struct kernel_tty *tty = leader->controlling_tty;
    leader->controlling_tty = 0;
    if (tty)
        kernel_tty_put(tty);
}
void kernel_task_tty_clear_session(struct kernel_tty *tty, struct kernel_pid *session)
{
    if (!session)
        return;
    for (struct kernel_pid_member *m = session->members[KERNEL_PID_SID]; m; m = m->next) {
        struct kernel_task *task = m->task;
        if (kernel_task_controlling_tty(task) == tty)
            kernel_task_tty_clear(task);
    }
}
struct kernel_pid *kernel_task_tty_identity(const struct kernel_task *task,
                                            enum kernel_pid_role role)
{
    return process_identity(task, role);
}
int kernel_task_tty_find_group(struct kernel_task *task, kernel_pid_t number,
                               struct kernel_pid **out)
{
    struct kernel_pid *group = kernel_pid_find(&scheduler.identities, number);
    *out = 0;
    if (!group)
        return -KERNEL_ESRCH;
    /* 固定 Linux session_of_pgrp 在没有 PGID 成员时仍检查数字 PID 的 session。 */
    struct kernel_pid_member *member = group->members[KERNEL_PID_PGID];
    if (!member)
        member = group->members[KERNEL_PID_TID];
    if (!member || process_identity(member->task, KERNEL_PID_SID) !=
                       process_identity(task, KERNEL_PID_SID))
        return -KERNEL_EPERM;
    *out = group;
    return 0;
}
int kernel_task_tty_session_leader(const struct kernel_task *task)
{
    return task && task->group_leader && task->group_leader->session_leader;
}
int kernel_task_tty_signal_ignored(const struct kernel_task *task, unsigned signal)
{
    if (!task || !signal || signal > KERNEL_SIGNAL_COUNT)
        return 1;
    if (task->signal_blocked & (UINT64_C(1) << (signal - 1)))
        return 1;
    struct kernel_linux_sigaction action;
    return kernel_signal_get_action((struct kernel_task *)task, signal, &action) ==
               KERNEL_SIGNAL_STATUS_OK &&
           action.handler == KERNEL_SIGNAL_IGN;
}
int kernel_task_tty_group_orphaned(const struct kernel_task *task)
{
    return process_group_is_orphaned(task);
}
void kernel_task_tty_signal_group(struct kernel_pid *group, unsigned signal)
{
    if (!group)
        return;
    for (struct kernel_pid_member *m = group->members[KERNEL_PID_PGID]; m; m = m->next)
        (void)signal_send_kernel_group(m->task, signal);
}
void kernel_task_tty_signal_session(struct kernel_tty *tty, struct kernel_pid *session)
{
    if (!session)
        return;
    for (struct kernel_pid_member *m = session->members[KERNEL_PID_SID]; m; m = m->next) {
        struct kernel_task *task = m->task;
        if (kernel_task_tty_session_leader(task)) {
            (void)signal_send_kernel_group(task, 1);
            (void)signal_send_kernel_group(task, 18);
        }
        if (kernel_task_controlling_tty(task) == tty)
            kernel_task_tty_clear(task);
    }
}
void kernel_task_tty_request_set(struct kernel_task *task, struct kernel_tty_request *request)
{
    if (task) {
        if (task->tty_request)
            __builtin_trap();
        task->tty_request = request;
    }
}
void kernel_task_tty_request_clear(struct kernel_task *task,
                                   struct kernel_tty_request *request)
{
    if (task) {
        if (task->tty_request != request)
            __builtin_trap();
        task->tty_request = 0;
    }
}
