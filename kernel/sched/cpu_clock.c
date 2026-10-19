#include "private.h"
#include <kernel/accounting.h>
#include <kernel/errno.h>
#include <kernel/tick.h>
#include <kernel/task.h>

static int thread_clock_visible(const struct kernel_task *task)
{
    return task->group_leader == task ||
        (task->state != KERNEL_THREAD_STATE_EXITED && task->state != KERNEL_THREAD_STATE_GROUP_DEAD);
}

/* PID引用固定编号身份，task借用仅固定存储；两者均不阻止退出和exec。 */
int kernel_task_cpu_clock(struct kernel_task *caller, int32_t clock, int resolution,
                          uint64_t *nanoseconds)
{
    if (clock == 2) clock = -6; /* process SCHED, PID 0 */
    else if (clock == 3) clock = -2; /* thread SCHED, PID 0 */
    else if (clock >= 0) return -KERNEL_EINVAL;
    unsigned kind = (uint32_t)clock & 3U;
    int thread = ((uint32_t)clock & 4U) != 0;
    kernel_pid_t number = ~(clock >> 3);
    if (kind == 3U) return -KERNEL_EINVAL;
    enum kernel_pid_role role = thread ? KERNEL_PID_TID : KERNEL_PID_TGID;
    struct kernel_pid *identity;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        if (!number || (!thread && !resolution && number == caller->tid))
            identity = process_identity(caller, role);
        else identity = kernel_pid_find(&scheduler.identities, number);
        if (!identity || !identity->members[role]) return -KERNEL_EINVAL;
        kernel_pid_get(identity);
    }
    int result = -KERNEL_EINVAL;
    for (;;) {
        SCHEDULER_TASK_BORROW(target, 0);
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            target = identity->members[role] ? identity->members[role]->task : 0;
            if (!target || (thread && (target->group_leader != caller->group_leader || !thread_clock_visible(target)))) {
                target = 0;
                break;
            }
            scheduler_task_get_locked(target);
            if (resolution) {
                *nanoseconds = kind == 2 ? 1U : UINT64_C(1000000000) / KERNEL_TICKS_PER_SECOND;
                result = 0;
                break;
            }
        }
        struct kernel_task_usage usage;
        if (thread) {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            scheduler_usage_thread_locked(target, &usage);
        } else scheduler_group_usage_snapshot(target, 0, &usage);
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            /* 非leader exec把同一个TGID交给新容器；丢弃旧快照并重新取样。
             * 回收后的空role返回EINVAL，绝不把复用编号的任务当成旧目标。 */
            if (!identity->members[role]) break;
            if (identity->members[role]->task != target) continue;
            if (thread && (target->group_leader != caller->group_leader || !thread_clock_visible(target))) break;
            *nanoseconds = usage.user_ns + (kind == 1 ? 0 : usage.kernel_ns);
            result = 0;
        }
        break;
    }
    kernel_task_identity_put(identity);
    return result;
}
