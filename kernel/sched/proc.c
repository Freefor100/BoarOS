#include <kernel/credentials.h>
#include "private.h"
#include "../../fs/files/private.h"

#include <arch/context.h>
#include <kernel/errno.h>
#include <kernel/irq.h>
#include <kernel/files.h>
#include <kernel/open_file.h>
#include <kernel/proc_task.h>
#include <kernel/pidfd.h>
#include <kernel/bpf.h>
#include <kernel/mqueue.h>
#include "../../fs/open_file_internal.h"
#include <kernel/fs_context.h>
#include <kernel/task.h>
#include <kernel/tty_task.h>
#include <kernel/vfs.h>
#include <kernel/namespace.h>
#include "ptrace_internal.h"
#include <string.h>

int kernel_credentials_may_access_locked(const struct kernel_task *caller,const struct kernel_task *target,int fscreds)
{
    if(!target)return 0;
    if(!caller || caller->group_leader==target->group_leader)return 1;
    /* access() 的真实ID快照覆盖整个路径解析；跨proc链接也使用
     * 同一个调用者视图。目标仍读取其发布凭据，不继承访问者override。 */
    const struct kernel_credentials *from=caller==kernel_task_current()?
        kernel_credentials_current():kernel_credentials_task_locked(caller),
        *to=kernel_credentials_task_locked(target);
    if(kernel_credentials_capable(from,KERNEL_CAP_SYS_PTRACE))return 1;
    uint32_t uid=fscreds?from->fsuid:from->uid,gid=fscreds?from->fsgid:from->gid;
    uint64_t caps=fscreds?from->effective:from->permitted;
    return uid==to->uid && uid==to->euid && uid==to->suid &&
        gid==to->gid && gid==to->egid && gid==to->sgid &&
        kernel_exec_security_dumpable(target->exec_security)==1 && !(to->permitted&~caps);
}
struct proc_access {
    struct kernel_task *task;
    const struct kernel_credentials *credentials;
    struct kernel_mm_record *mm;
    struct kernel_exec_security *security;
    int fscreds;
};
static void proc_access_release(struct proc_access *access)
{ if(access->credentials)kernel_credentials_put(access->credentials); kernel_exec_security_put(access->security); }
static int proc_access_begin_mode(struct kernel_task *task,struct proc_access *access,int fscreds)
{
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    if(!task)return -KERNEL_ENOENT;
    if(!kernel_credentials_may_access_locked(kernel_task_current(),task,fscreds))return -KERNEL_EACCES;
    *access=(struct proc_access){task,kernel_credentials_get(kernel_credentials_task_locked(task)),task->mm.record,
        kernel_exec_security_get(task->exec_security),fscreds};
    return 0;
}
static int proc_access_begin(struct kernel_task *task,struct proc_access *access)
{return proc_access_begin_mode(task,access,1);}
static int proc_access_check(struct proc_access *access)
{
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    struct kernel_task *task=access->task;
    return task && task->mm.record==access->mm && task->exec_security==access->security &&
        kernel_credentials_task_locked(task)==access->credentials &&
        kernel_credentials_may_access_locked(kernel_task_current(),task,access->fscreds)?0:-KERNEL_EACCES;
}
#define PROC_ACCESS(task) \
    struct proc_access access __attribute__((cleanup(proc_access_release)))={0}; \
    int access_error=proc_access_begin(task,&access); \
    if(access_error)return access_error

static void comm_assign(struct kernel_task *task, const char name[16])
{
    size_t length = 0;
    while (length < sizeof(task->comm) - 1U && name[length]) length++;
    memcpy(task->comm, name, length);
    memset(task->comm + length, 0, sizeof(task->comm) - length);
}

void kernel_task_comm_get(struct kernel_task *task, char name[16])
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    memcpy(name, task->comm, sizeof(task->comm));
}

void kernel_task_comm_set(struct kernel_task *task, const char name[16])
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    comm_assign(task, name);
}

int kernel_proc_comm(kernel_pid_t pid, uint64_t identity, int thread,
                     char name[16], int writing)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    enum kernel_pid_role role = thread ? KERNEL_PID_TID : KERNEL_PID_TGID;
    struct kernel_task *task = process_find_identity(pid, role);
    struct kernel_pid *id = task ? process_identity(task, role) : 0;
    if (!id || id->generation != identity || !task->accounted || task->retirement)
        return -KERNEL_ESRCH;
    if (writing) {
        struct kernel_task *caller = kernel_task_current();
        if (!caller || caller->group_leader != task->group_leader) return -KERNEL_EINVAL;
        comm_assign(task, name);
    } else memcpy(name, task->comm, sizeof(task->comm));
    return 0;
}

int kernel_proc_timerslack(kernel_pid_t pid, uint64_t identity,
                           uint64_t *value, int writing)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    struct kernel_task *task = process_find_identity(pid, KERNEL_PID_TGID);
    struct kernel_pid *id = task ? process_identity(task, KERNEL_PID_TGID) : 0;
    if (!id || id->generation != identity || !task->accounted || task->retirement)
        return -KERNEL_ESRCH;
    if (task != kernel_task_current() &&
        !kernel_credentials_capable(kernel_credentials_current(), KERNEL_CAP_SYS_NICE))
        return -KERNEL_EPERM;
    if (writing) kernel_task_timerslack_set_locked(task, *value);
    else *value = kernel_task_timerslack_get(task);
    return 0;
}

/* Identity and MM publication/release share wait_domain. The mask accessors
 * are nonblocking atomics; no task or MM pointer survives this scope. */
int kernel_proc_coredump_filter(kernel_pid_t pid, uint64_t identity, int thread,
    unsigned *value, int writing)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    enum kernel_pid_role role = thread ? KERNEL_PID_TID : KERNEL_PID_TGID;
    struct kernel_task *task = process_find_identity(pid, role);
    struct kernel_pid *id = task ? process_identity(task, role) : 0;
    if (!id || id->generation != identity || !task->accounted || task->retirement)
        return -KERNEL_ESRCH;
    if (task->proc_exiting || task->mm.state != KERNEL_MM_LIVE)
        return writing ? -KERNEL_ESRCH : 0;
    if (writing) kernel_mm_coredump_filter_set(&task->mm, *value);
    else *value = kernel_mm_coredump_filter_get(&task->mm);
    return 1;
}

void kernel_proc_task_update_comm(struct kernel_task *task)
{
    struct kernel_vfs_path *path = 0;
    if (kernel_mm_executable_path_acquire(&task->mm, &path) != KERNEL_MM_STATUS_OK) return;
    char name[16] = {0};
    if (kernel_vfs_path_copy_name(path, name, sizeof(name))) __builtin_trap();
    if (kernel_vfs_path_release(&path)) __builtin_trap();
    kernel_task_comm_set(task, name);
}

static int proc_visible(const struct kernel_task *task)
{
    /* EXITED/GROUP_DEAD仍拥有已发布身份；真实reap才撤数字目录。 */
    return task && task->arch.user_mode && task->tid_owned &&
        task->group_leader && task->group_leader->tid_owned &&
        task->group_leader->tid > 0 && process_identity(task,KERNEL_PID_TGID);
}

struct kernel_pid_registry *kernel_proc_view_acquire(void)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    struct kernel_pid_registry *view = process_pid_active(kernel_task_current());
    if (!view) view = &scheduler.identities; /* early boot mount */
    if (view != &scheduler.identities) kernel_pid_registry_get(view);
    return view;
}

void kernel_proc_view_retain(struct kernel_pid_registry *view)
{
    if (view == &scheduler.identities) return;
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    kernel_pid_registry_get(view);
}

void kernel_proc_view_release(struct kernel_pid_registry *view)
{
    if (view == &scheduler.identities) return;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        kernel_pid_registry_put(view);
    }
    process_identity_collect();
}

int kernel_proc_view_self(struct kernel_pid_registry *view, kernel_pid_t *tgid,
    kernel_pid_t *tid)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    struct kernel_task *task = kernel_task_current();
    *tgid = kernel_pid_number_in(process_identity(task, KERNEL_PID_TGID), view);
    *tid = kernel_pid_number_in(process_identity(task, KERNEL_PID_TID), view);
    return *tgid && *tid ? 0 : -KERNEL_ENOENT;
}

/* Called under identity lock; canonical group and generation come from the
 * parent inode, never from a second interpretation of a user-visible number. */
static int proc_view_entry(struct kernel_pid *id, kernel_pid_t group,
    uint64_t group_generation, int thread, kernel_pid_t *canonical,
    uint64_t *generation)
{
    enum kernel_pid_role role = thread ? KERNEL_PID_TID : KERNEL_PID_TGID;
    struct kernel_task *task = id && id->members[role] ? id->members[role]->task : NULL;
    if (!task || !task->accounted || task->retirement || !proc_visible(task) ||
        (group && (process_identity_number(task, KERNEL_PID_TGID) != group ||
                   process_identity_generation(task) != group_generation)))
        return -KERNEL_ENOENT;
    *canonical = kernel_pid_number_in(id, &scheduler.identities);
    *generation = id->generation;
    return 0;
}

int kernel_proc_view_lookup(struct kernel_pid_registry *view, kernel_pid_t number,
    kernel_pid_t group, uint64_t group_generation, int thread,
    kernel_pid_t *canonical, uint64_t *generation)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    return proc_view_entry(kernel_pid_find(view, number), group, group_generation,
        thread, canonical, generation);
}

int kernel_proc_view_next(struct kernel_pid_registry *view, kernel_pid_t after,
    kernel_pid_t group, uint64_t group_generation, int thread,
    kernel_pid_t *number, kernel_pid_t *canonical, uint64_t *generation)
{
    if (after < 0) return -KERNEL_EINVAL;
    for (;;) {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        struct kernel_pid *id = kernel_pid_next_published(view, after);
        if (!id) return -KERNEL_ENOENT;
        after = kernel_pid_number_in(id, view);
        if (!proc_view_entry(id, group, group_generation, thread, canonical, generation)) {
            *number = after;
            return 0;
        }
    }
}

/* Namespace columns start at the mount view and end at the task's active
 * namespace, not the reader's current namespace. */
static unsigned proc_namespace_ids(const struct kernel_pid *id,
    struct kernel_pid_registry *view, kernel_pid_t *numbers)
{
    if (!kernel_pid_number_in(id, view)) return 0;
    unsigned count = id->registry->depth - view->depth + 1U;
    struct kernel_pid_registry *space = id->registry;
    for (unsigned n = count; n; n--, space = space->parent)
        numbers[n - 1U] = kernel_pid_number_in(id, space);
    return count;
}

uint32_t kernel_proc_pid_max(void)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    /* 分配器的 limit 是包含端点的编号；proc 的 pid_max 是排他上界。 */
    return scheduler.pid_allocator.limit + 1U;
}

static int proc_group_visible(struct kernel_task *leader)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    return leader->accounted && !leader->retirement && proc_visible(leader);
}

static struct kernel_task *find_member(kernel_pid_t pid, uint64_t identity)
{
    struct kernel_task *leader;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        leader = process_find_identity(pid, KERNEL_PID_TGID);
        if (!leader || process_identity_generation(leader) != identity ||
            !leader->accounted || leader->retirement) return 0;
        scheduler_task_get_locked(leader);
    }
    int visible = proc_group_visible(leader);
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        visible = visible && leader->accounted && !leader->retirement &&
                  process_find_identity(pid, KERNEL_PID_TGID) == leader &&
                  process_identity_generation(leader) == identity;
    }
    if (!visible) { scheduler_task_put(leader); return 0; }
    return leader;
}

static struct kernel_task *find_thread(kernel_pid_t tid, uint64_t identity);

static struct kernel_task *find_resource_owner(kernel_pid_t pid,
                                                uint64_t identity)
{
    int thread = !!(identity & KERNEL_PROC_THREAD_TARGET);
    identity &= ~KERNEL_PROC_THREAD_TARGET;
    struct kernel_task *leader = thread ? find_thread(pid, identity) :
        find_member(pid, identity);
    int departing;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        departing = leader && (leader->retirement || leader->proc_exiting);
    }
    if (departing) { scheduler_task_put(leader); return 0; }
    return leader;
}


static struct kernel_task *find_thread(kernel_pid_t tid, uint64_t identity)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    struct kernel_task *task = process_find_identity(tid, KERNEL_PID_TID);
    struct kernel_pid *id = process_identity(task, KERNEL_PID_TID);
    if (!task || !id || id->generation != identity || !task->accounted ||
        task->retirement || !proc_visible(task)) return 0;
    scheduler_task_get_locked(task);
    return task;
}

int kernel_proc_thread_resources_live(kernel_pid_t tid, uint64_t identity)
{
    KERNEL_IRQ_SCOPE(proc_irq);
    SCHEDULER_TASK_BORROW(task,
        find_resource_owner(tid, identity | KERNEL_PROC_THREAD_TARGET));
    return task ? 0 : -KERNEL_ENOENT;
}

int kernel_proc_thread_identity(kernel_pid_t tgid, uint64_t group_identity,
    kernel_pid_t tid, uint64_t *identity)
{
    if (tid <= 0 || !identity || tgid < 0 || (tgid && !group_identity))
        return -KERNEL_EINVAL;
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    struct kernel_task *task = process_find_identity(tid, KERNEL_PID_TID);
    if (!task || !task->accounted || task->retirement || !proc_visible(task) ||
        (tgid && (process_identity_number(task, KERNEL_PID_TGID) != tgid ||
                  process_identity_generation(task) != group_identity)))
        return -KERNEL_ENOENT;
    *identity = process_identity(task, KERNEL_PID_TID)->generation;
    return 0;
}

int kernel_proc_thread_group(kernel_pid_t tid, uint64_t identity,
    kernel_pid_t *tgid, uint64_t *group_identity)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    struct kernel_task *task = process_find_identity(tid, KERNEL_PID_TID);
    struct kernel_pid *id = process_identity(task, KERNEL_PID_TID);
    if (!task || !id || id->generation != identity || !task->accounted ||
        task->retirement || !proc_visible(task)) return -KERNEL_ENOENT;
    *tgid = process_identity_number(task, KERNEL_PID_TGID);
    *group_identity = process_identity_generation(task);
    return 0;
}

int kernel_proc_next_thread(kernel_pid_t tgid, uint64_t group_identity,
    kernel_pid_t after, kernel_pid_t *tid, uint64_t *identity)
{
    if (tgid <= 0 || !group_identity || after < 0 || !tid || !identity)
        return -KERNEL_EINVAL;
    KERNEL_IRQ_SCOPE(proc_irq);
    SCHEDULER_TASK_BORROW(leader, find_member(tgid, group_identity));
    if (!leader) return -KERNEL_ENOENT;
    for (;;) {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        if (!leader->accounted || leader->retirement ||
            process_find_identity(tgid, KERNEL_PID_TGID) != leader ||
            process_identity_generation(leader) != group_identity)
            return -KERNEL_ENOENT;
        struct kernel_pid *id = kernel_pid_next_published(&scheduler.identities, after);
        if (!id) return -KERNEL_ENOENT;
        after = id->number;
        struct kernel_task *task = id->members[KERNEL_PID_TID]
            ? id->members[KERNEL_PID_TID]->task : 0;
        if (task && task->accounted && !task->retirement &&
            task->group_leader == leader && proc_visible(task)) {
            *tid = id->number; *identity = id->generation;
            return 0;
        }
    }
}

static void proc_mm_release(struct kernel_mm *mm)
{
    if (mm->state == KERNEL_MM_LIVE && kernel_mm_release(mm)) __builtin_trap();
}

static int proc_task_snapshot(struct kernel_task *task, uint64_t identity,
    int thread, struct kernel_proc_process_snapshot *result,
    struct kernel_pid_registry *view)
{
    struct kernel_task *leader = NULL;
    SCHEDULER_TASK_BORROW(group_owner, NULL);
    struct kernel_mm mm __attribute__((cleanup(proc_mm_release))) = {0};
    uint32_t threads = 0;
    {
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    leader = task->group_leader;
    struct kernel_pid *selected = process_identity(task, thread ? KERNEL_PID_TID : KERNEL_PID_TGID);
    struct kernel_pid *tid_id = process_identity(task, KERNEL_PID_TID);
    if (!leader || leader->retirement || !leader->accounted || !selected || !tid_id ||
        selected->generation != identity || !kernel_pid_number_in(selected, view))
        return -KERNEL_ENOENT;
    /* A borrowed nonleader can exec while later MM/TTY reads sleep. Preserve
     * the captured group's storage without charging ordinary leader reads. */
    if (leader != task) { scheduler_task_get_locked(leader); group_owner = leader; }
    threads = leader->group_members;
    if (!task->proc_exiting && task->mm.state == KERNEL_MM_LIVE &&
        kernel_mm_acquire(&mm, &task->mm)) return -KERNEL_EIO;
    *result = (struct kernel_proc_process_snapshot){
        .credentials=kernel_credentials_get(kernel_credentials_task_locked(task)),
        .pid = kernel_pid_number_in(process_identity(task, KERNEL_PID_TID), view),
        .tgid = kernel_pid_number_in(process_identity(leader, KERNEL_PID_TGID), view),
        .ppid = leader->parent && leader->parent->group_leader
            ? kernel_pid_number_in(process_identity(leader->parent, KERNEL_PID_TGID), view) : 0,
        .process_group = kernel_pid_number_in(process_identity(leader, KERNEL_PID_PGID), view),
        .session_id = kernel_pid_number_in(process_identity(leader, KERNEL_PID_SID), view),
        .identity = identity,
        .tracer_pid=task->ptrace && task->ptrace->tracer
            ? kernel_pid_number_in(process_identity(task->ptrace->tracer, KERNEL_PID_TID), view) : 0,
        .start_ticks = task->proc_start_ticks,
        .state = task->proc_exiting ||
                 task->state == KERNEL_THREAD_STATE_EXITED ||
                 task->state == KERNEL_THREAD_STATE_GROUP_DEAD ||
                 task->state == KERNEL_THREAD_STATE_ZOMBIE ? 'Z' :
                 task->state == KERNEL_THREAD_STATE_RUNNING ||
                 task->state == KERNEL_THREAD_STATE_READY ? 'R' :
                 task->state == KERNEL_THREAD_STATE_STOPPED ? 'T' :
                 task->ptrace && (task->ptrace->phase==TRACE_STOPPED || task->ptrace->phase==TRACE_LISTENING) ? 't' : 'S',
        .child_minor_faults = leader->limits->child_usage.minor_faults,
        .child_major_faults = leader->limits->child_usage.major_faults,
        .signal_pending = task->signal_pending,
        .signal_shared_pending = leader->group_pending,
        .signal_blocked = task->signal_blocked,
        .signal_queued=kernel_signal_pending_count_locked(task),
        .signal_queue_limit=leader->limits->values[KERNEL_RLIMIT_SIGPENDING].current,
        .rss_limit=task->limits->values[KERNEL_RLIMIT_RSS].current,
        .threads = threads,
        .no_new_privs=task->no_new_privs,
        .scheduling_priority = task->scheduling.priority
            ? -(task->scheduling.priority + 1) : 20+task->scheduling.nice,
        .scheduling_nice=task->scheduling.nice,
        .rt_priority = scheduler_base_priority(task),
        .scheduling_policy = (uint32_t)task->scheduling.policy,
    };
    result->namespace_count = tid_id->registry->depth - view->depth + 1U;
    struct kernel_pid_registry *space = tid_id->registry;
    for (unsigned n = result->namespace_count; n; n--, space = space->parent)
        for (unsigned role = 0; role < KERNEL_PID_ROLES; role++)
            result->namespace_ids[n - 1U][role] =
                kernel_pid_number_in(process_identity(task, role), space);
    result->wait_channel_flag = (thread || threads < 2U) &&
        task->state != KERNEL_THREAD_STATE_RUNNING &&
        task->state != KERNEL_THREAD_STATE_READY;
    memcpy(result->comm, task->comm, sizeof(result->comm));
    /* 远端可改写信号动作；查询与修改共用wait域，与其余信号字段一起捕获。 */
    const struct kernel_signal_table *actions =
        (const void *)(uintptr_t)task->signal_table_address;
    if (actions) {
        if (actions->magic != KERNEL_SIGNAL_TABLE_MAGIC) __builtin_trap();
        for (unsigned sig = 0; sig < 64U; sig++) {
            if (actions->actions[sig].handler == KERNEL_SIGNAL_IGN)
                result->signal_ignored |= UINT64_C(1) << sig;
            else if (actions->actions[sig].handler != KERNEL_SIGNAL_DFL)
                result->signal_caught |= UINT64_C(1) << sig;
        }
    }
    }
    result->tty_nr = kernel_tty_rdev(kernel_task_controlling_tty(leader));
    result->tty_pgrp = kernel_tty_foreground_in(kernel_task_controlling_tty(leader), view);
    struct kernel_task_usage usage = {0};
    scheduler_usage_snapshot(task, thread, &usage);
    result->minor_faults = usage.minor_faults;
    result->major_faults = usage.major_faults;
    result->user_ticks = usage.user_ns / 10000000U;
    result->kernel_ticks = usage.kernel_ns / 10000000U;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        result->child_user_ticks = leader->limits->child_usage.user_ns / 10000000U;
        result->child_kernel_ticks = leader->limits->child_usage.kernel_ns / 10000000U;
    }
    if (mm.state == KERNEL_MM_LIVE) {
        struct kernel_mm_proc_memory memory;
        if (kernel_mm_proc_memory_snapshot(&mm, &memory) !=
            KERNEL_MM_STATUS_OK) {
                kernel_credentials_put(result->credentials);result->credentials=0;
                return -KERNEL_EIO;
        }
        result->virtual_bytes = memory.virtual_bytes;
        result->locked_bytes = memory.locked_bytes;
        result->resident_pages = memory.resident_pages;
        result->start_code = memory.start_code;
        result->end_code = memory.end_code;
        result->start_stack = memory.start_stack;
    }
    return 0;
}

int kernel_proc_process_snapshot(kernel_pid_t pid, uint64_t identity,
    struct kernel_proc_process_snapshot *result)
{
    if (pid <= 0 || !identity || !result) return -KERNEL_EINVAL;
    KERNEL_IRQ_SCOPE(proc_irq);
    SCHEDULER_TASK_BORROW(task, find_member(pid, identity));
    return task ? proc_task_snapshot(task, identity, 0, result, &scheduler.identities) : -KERNEL_ENOENT;
}

int kernel_proc_thread_snapshot(kernel_pid_t tid, uint64_t identity,
    struct kernel_proc_process_snapshot *result)
{
    if (tid <= 0 || !identity || !result) return -KERNEL_EINVAL;
    KERNEL_IRQ_SCOPE(proc_irq);
    SCHEDULER_TASK_BORROW(task, find_thread(tid, identity));
    return task ? proc_task_snapshot(task, identity, 1, result, &scheduler.identities) : -KERNEL_ESRCH;
}

int kernel_proc_snapshot_in(struct kernel_pid_registry *view, kernel_pid_t pid,
    uint64_t identity, int thread, struct kernel_proc_process_snapshot *result)
{
    KERNEL_IRQ_SCOPE(proc_irq);
    SCHEDULER_TASK_BORROW(task, thread ? find_thread(pid, identity) : find_member(pid, identity));
    if (!task) return -KERNEL_ENOENT;
    return proc_task_snapshot(task, identity, thread, result, view);
}

static int proc_resource_matches_locked(struct kernel_task *task, kernel_pid_t pid,
                                        uint64_t target)
{
    enum kernel_pid_role role = (target & KERNEL_PROC_THREAD_TARGET) ?
        KERNEL_PID_TID : KERNEL_PID_TGID;
    struct kernel_pid *id = process_identity(task, role);
    return task->accounted && !task->retirement && !task->proc_exiting &&
        process_find_identity(pid, role) == task && id &&
        id->generation == (target & ~KERNEL_PROC_THREAD_TARGET);
}

int kernel_proc_pagemap_open(kernel_pid_t pid, uint64_t identity,
    struct kernel_mm_pagemap *view)
{
    KERNEL_IRQ_SCOPE(proc_irq);
    SCHEDULER_TASK_BORROW(task, find_resource_owner(pid, identity));
    if (!task) return -KERNEL_ESRCH;
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    if (!proc_resource_matches_locked(task, pid, identity) || task->mm.state != KERNEL_MM_LIVE)
        return -KERNEL_ESRCH;
    if (!kernel_credentials_may_access_locked(kernel_task_current(), task, 1))
        return -KERNEL_EACCES;
    /* Open-time authorization and the weak address-space reference are
     * captured together. Later exec or PID reuse cannot retarget the file. */
    kernel_mm_pagemap_hold(&task->mm, view);
    return 0;
}

int kernel_proc_io_snapshot(kernel_pid_t pid, uint64_t identity,
    struct kernel_proc_io_counters *out)
{
    int thread = (identity & KERNEL_PROC_THREAD_TARGET) != 0;
    identity &= ~KERNEL_PROC_THREAD_TARGET;
    KERNEL_IRQ_SCOPE(proc_irq);
    SCHEDULER_TASK_BORROW(task, thread ? find_thread(pid, identity) : find_member(pid, identity));
    if (!task) return -KERNEL_ENOENT;
    PROC_ACCESS(task);
    struct kernel_task_usage usage;
    if (thread) {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        scheduler_usage_thread_locked(task, &usage);
    } else scheduler_group_usage_snapshot(task, 1, &usage);
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        enum kernel_pid_role role = thread ? KERNEL_PID_TID : KERNEL_PID_TGID;
        struct kernel_pid *id = process_identity(task, role);
        if (!task->accounted || task->retirement || process_find_identity(pid, role) != task ||
            !id || id->generation != identity) return -KERNEL_ENOENT;
    }
    int result = proc_access_check(&access);
    if (result) return result;
    *out = (struct kernel_proc_io_counters){
        .rchar = usage.rchar, .wchar = usage.wchar, .syscr = usage.syscr, .syscw = usage.syscw,
        .read_bytes = usage.read_bytes, .write_bytes = usage.write_bytes,
        .cancelled_write_bytes = usage.cancelled_write_bytes,
    };
    return 0;
}

int kernel_task_remote_mm_acquire(int32_t tid,struct kernel_mm *owner)
{
    if(!owner || owner->state!=KERNEL_MM_EMPTY || owner->record)__builtin_trap();
    if(tid<=0)return -KERNEL_ESRCH;
    struct kernel_task *caller=kernel_task_current();
    SCHEDULER_TASK_BORROW(task,kernel_task_find_user_tid(caller,tid));
    if(!task)return -KERNEL_ESRCH;
    KERNEL_RAW_SCOPE(resources,&task->files_access);
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    struct kernel_pid *id=process_identity(task,KERNEL_PID_TID);
    if(!id || !task->accounted || task->retirement || task->proc_exiting ||
       process_find_identity_in(tid,KERNEL_PID_TID,process_pid_active(caller))!=task ||
       task->mm.state!=KERNEL_MM_LIVE)return -KERNEL_ESRCH;
    if(!kernel_credentials_may_access_locked(kernel_task_current(),task,0))return -KERNEL_EPERM;
    if(kernel_mm_acquire(owner,&task->mm))__builtin_trap();
    return 0;
}

int kernel_proc_maps_snapshot(kernel_pid_t pid, uint64_t identity,
    struct kernel_heap *heap, struct kernel_mm_proc_map **rows, uint32_t *count)
{
    if (pid <= 0 || !identity || !heap || !rows || *rows || !count)
        return -KERNEL_EINVAL;
    KERNEL_IRQ_SCOPE(proc_irq);
    SCHEDULER_TASK_BORROW(task, find_resource_owner(pid, identity));
    if (!task) return -KERNEL_ENOENT;
    struct proc_access access __attribute__((cleanup(proc_access_release))) = {0};
    struct kernel_mm mm = {0};
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        if (!proc_resource_matches_locked(task, pid, identity)) return -KERNEL_ENOENT;
        if (!kernel_credentials_may_access_locked(kernel_task_current(), task, 1))
            return -KERNEL_EACCES;
        if (task->mm.state != KERNEL_MM_LIVE) { *count = 0; return 0; }
        /* 凭据与实际MM owner在同一wait域捕获；exec不能在两者之间换映像。 */
        if (kernel_mm_acquire(&mm, &task->mm)) return -KERNEL_EIO;
        access = (struct proc_access){ task,
            kernel_credentials_get(kernel_credentials_task_locked(task)), mm.record,
            kernel_exec_security_get(task->exec_security),1 };
    }
    enum kernel_mm_status status = kernel_mm_proc_maps_snapshot(&mm, heap, rows, count);
    int error = status == KERNEL_MM_STATUS_NO_MEMORY ? -KERNEL_ENOMEM :
                status ? -KERNEL_EIO : 0;
    if (!error) {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        if (!proc_resource_matches_locked(task, pid, identity)) error = -KERNEL_ENOENT;
        else if (task->mm.record != access.mm || task->exec_security != access.security ||
            kernel_credentials_task_locked(task) != access.credentials ||
            !kernel_credentials_may_access_locked(kernel_task_current(), task, 1))
            error = -KERNEL_EACCES;
    }
    if (kernel_mm_release(&mm)) __builtin_trap();
    if (error && *rows) {
        for (uint32_t i = 0; i < *count; i++)
            if ((*rows)[i].file && kernel_open_file_release(&(*rows)[i].file)) __builtin_trap();
        kernel_heap_release(heap, *rows); *rows = 0; *count = 0;
    }
    return error;
}

int kernel_proc_same_group(kernel_pid_t pid,uint64_t identity,int thread)
{
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    struct kernel_task *caller=kernel_task_current();
    struct kernel_task *task=process_find_identity(pid,thread?KERNEL_PID_TID:KERNEL_PID_TGID);
    struct kernel_pid *id=task?process_identity(task,thread?KERNEL_PID_TID:KERNEL_PID_TGID):0;
    return id && id->generation==identity && caller && caller->group_leader==task->group_leader;
}

int kernel_proc_inode_owner(kernel_pid_t pid,uint64_t identity,int thread,uint32_t mode,uint32_t *uid,uint32_t *gid)
{
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    struct kernel_task *task=process_find_identity(pid,thread?KERNEL_PID_TID:KERNEL_PID_TGID);
    struct kernel_pid *id=task?process_identity(task,thread?KERNEL_PID_TID:KERNEL_PID_TGID):0;
    if(!id || id->generation!=identity || !task->accounted || task->retirement)return -KERNEL_ENOENT;
    const struct kernel_credentials *cred=kernel_credentials_task_locked(task);
    int dumpable=mode==(KERNEL_VFS_S_IFDIR|0555U) || kernel_exec_security_dumpable(task->exec_security)==1;
    *uid=dumpable?cred->euid:0;*gid=dumpable?cred->egid:0;return 0;
}

int kernel_proc_process_path_acquire(kernel_pid_t pid, uint64_t identity,
                                     enum kernel_proc_path_kind kind, int fd,
                                     struct kernel_vfs_path **owner)
{
    (void)fd;
    if (pid <= 0 || !identity || !owner || *owner)
        return -KERNEL_EINVAL;
    KERNEL_IRQ_SCOPE(proc_irq);
    SCHEDULER_TASK_BORROW(task, find_resource_owner(pid, identity));
    PROC_ACCESS(task);
    if (!task) {
        return -KERNEL_ENOENT;
    }
    int result = -KERNEL_ENOENT;
    if (kind == KERNEL_PROC_PATH_EXE) {
        if (task->mm.state == KERNEL_MM_LIVE &&
            kernel_mm_executable_path_acquire(&task->mm, owner) ==
                KERNEL_MM_STATUS_OK) result = 0;
    } else if (kind == KERNEL_PROC_PATH_CWD ||
               kind == KERNEL_PROC_PATH_ROOT) {
        KERNEL_FS_PATHS(paths, &task->fs);
        if (paths.root) {
            struct kernel_vfs_path *path = kind == KERNEL_PROC_PATH_CWD
                ? paths.cwd : paths.root;
            if (path && !kernel_vfs_path_acquire(path)) {
                *owner = path;
                result = 0;
            }
        }
    }
    if(!result && proc_access_check(&access)) {
        if(*owner && kernel_vfs_path_release(owner))__builtin_trap();
        return -KERNEL_EACCES;
    }
    return result;
}

int kernel_proc_mounts_snapshot(kernel_pid_t pid,uint64_t identity,struct kernel_fs_mounts *owner)
{
    SCHEDULER_TASK_BORROW(task,find_resource_owner(pid,identity));
    if (!task) return -KERNEL_ENOENT;
    *owner=kernel_fs_context_mounts_snapshot(&task->fs);
    return owner->namespace?0:-KERNEL_ENOENT;
}

int kernel_proc_namespaces_handle(kernel_pid_t pid,uint64_t identity,uint32_t mask,int fscreds,
    struct kernel_namespace *owners[4])
{
    if(pid<=0 || !identity || !owners || owners[0] || owners[1] || owners[2] || owners[3] ||
       !mask || (mask&~(KERNEL_NS_MNT|KERNEL_NS_UTS|KERNEL_NS_IPC|KERNEL_NS_PID|KERNEL_PROC_PID_FOR_CHILDREN)) ||
       ((mask&KERNEL_PROC_PID_FOR_CHILDREN) && !(mask&KERNEL_NS_PID)))return -KERNEL_EINVAL;
    SCHEDULER_TASK_BORROW(task,find_resource_owner(pid,identity));
    struct proc_access access __attribute__((cleanup(proc_access_release)))={0};
    int error=proc_access_begin_mode(task,&access,fscreds);
    if(error)return error;
    struct kernel_namespace *snapshot[4]={0};
    struct kernel_pid_registry *pid_space = NULL;
    struct kernel_fs_mounts mounts __attribute__((cleanup(kernel_fs_mounts_release)))={0};
    {
        KERNEL_RAW_SCOPE(resources,&task->files_access);
        if(mask&KERNEL_NS_MNT)mounts=kernel_fs_context_mounts_snapshot(&task->fs);
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        if(!proc_resource_matches_locked(task,pid,identity))error=-KERNEL_ENOENT;
        else {
            if(mask&KERNEL_NS_MNT)snapshot[0]=kernel_namespace_get(kernel_mount_namespace_base(mounts.namespace));
            if(mask&KERNEL_NS_UTS)snapshot[1]=kernel_namespace_get(task->uts_namespace);
            if(mask&KERNEL_NS_IPC)snapshot[2]=kernel_namespace_get(task->ipc_namespace);
            if(mask&KERNEL_NS_PID) {
                pid_space = mask&KERNEL_PROC_PID_FOR_CHILDREN ?
                    process_pid_children_locked(task) : process_pid_active(task);
                if(pid_space) kernel_pid_registry_get(pid_space);
                else error=-KERNEL_ENOENT;
            }
            if(((mask&KERNEL_NS_MNT)&&!snapshot[0]) || ((mask&KERNEL_NS_UTS)&&!snapshot[1]) ||
               ((mask&KERNEL_NS_IPC)&&!snapshot[2]))error=-KERNEL_ENOENT;
        }
    }
    if(!error && pid_space)
        error=process_pid_namespace_snapshot(pid_space,&scheduler.identity_heap,&snapshot[3]);
    if(pid_space) {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        kernel_pid_registry_put(pid_space);
    }
    if(pid_space) process_identity_collect();
    if(!error) {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        if(!proc_resource_matches_locked(task,pid,identity))error=-KERNEL_ENOENT;
        else if(task->mm.record!=access.mm || task->exec_security!=access.security ||
            kernel_credentials_task_locked(task)!=access.credentials ||
            !kernel_credentials_may_access_locked(kernel_task_current(),task,fscreds))error=-KERNEL_EACCES;
    }
    for(unsigned i=0;i<4;i++) {
        if(error)kernel_namespace_put(snapshot[i]);
        else owners[i]=snapshot[i];
    }
    return error;
}

int kernel_proc_namespaces_identity_handle(struct kernel_pid *id,uint32_t mask,
    struct kernel_namespace *owners[4])
{
    kernel_pid_t canonical;uint64_t generation;
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
      canonical=kernel_pid_number_in(id,&scheduler.identities);generation=id->generation; }
    return kernel_proc_namespaces_handle(canonical,generation|KERNEL_PROC_THREAD_TARGET,
        mask,0,owners);
}

int kernel_proc_namespace_handle(kernel_pid_t pid,uint64_t identity,uint32_t type,
    struct kernel_namespace **owner)
{
    if(!owner || *owner || (type!=KERNEL_NS_MNT && type!=KERNEL_NS_UTS && type!=KERNEL_NS_IPC &&
        type!=KERNEL_NS_PID && type!=(KERNEL_NS_PID|KERNEL_PROC_PID_FOR_CHILDREN)))return -KERNEL_EINVAL;
    struct kernel_namespace *snapshot[4]={0};
    int error=kernel_proc_namespaces_handle(pid,identity,type,1,snapshot);
    if(!error)*owner=snapshot[type==KERNEL_NS_MNT ? 0 : type==KERNEL_NS_UTS ? 1 : type==KERNEL_NS_IPC ? 2 : 3];
    return error;
}

int kernel_proc_next_fd(kernel_pid_t pid, uint64_t identity,
                         int after, int *fd)
{
    if (pid <= 0 || !identity || !fd || after < -1)
        return -KERNEL_EINVAL;
    KERNEL_IRQ_SCOPE(proc_irq);
    SCHEDULER_TASK_BORROW(task, find_resource_owner(pid, identity));
    PROC_ACCESS(task);
    int result = task && kernel_files_is_live(&task->files)
        ? kernel_files_next_open_fd(&task->files, after, fd)
        : -KERNEL_ENOENT;
    return result?result:proc_access_check(&access);
}

int kernel_proc_fd_access_snapshot(kernel_pid_t pid, uint64_t identity,
                                   int fd, int *readable, int *writable)
{
    if (pid <= 0 || !identity || fd < 0 || !readable || !writable)
        return -KERNEL_EINVAL;
    KERNEL_IRQ_SCOPE(proc_irq);
    SCHEDULER_TASK_BORROW(task, find_resource_owner(pid, identity));
    PROC_ACCESS(task);
    KERNEL_FILES_PIN_SCOPE(fd_guard);
    struct kernel_open_file_description *file = task &&
        kernel_files_is_live(&task->files)
        ? kernel_files_hold_fd(&task->files, fd, &fd_guard) : 0;
    if(proc_access_check(&access))return -KERNEL_EACCES;
    if (file) {
        if (kernel_open_file_kind(file) == KERNEL_OPEN_FILE_KIND_EPOLL) {
            *readable = 1;
            *writable = 1;
        } else {
            *readable = kernel_open_file_readable(file);
            *writable = kernel_open_file_writable(file);
        }
    }
    return file ? 0 : -KERNEL_ENOENT;
}

int kernel_proc_fd_path_acquire(kernel_pid_t pid, uint64_t identity,
                                int fd, struct kernel_vfs_path **owner)
{
    if (pid <= 0 || !identity || fd < 0 || !owner || *owner)
        return -KERNEL_EINVAL;
    KERNEL_IRQ_SCOPE(proc_irq);
    SCHEDULER_TASK_BORROW(task, find_resource_owner(pid, identity));
    PROC_ACCESS(task);
    KERNEL_FILES_PIN_SCOPE(fd_guard);
    struct kernel_open_file_description *file = task &&
        kernel_files_is_live(&task->files)
        ? kernel_files_hold_fd(&task->files, fd, &fd_guard) : 0;
    if(proc_access_check(&access))return -KERNEL_EACCES;
    struct kernel_vfs_path *path = file ? kernel_open_file_path(file) : 0;
    int result = -KERNEL_ENOENT;
    if (path && !kernel_vfs_path_acquire(path)) {
        *owner = path;
        result = 0;
    }
    return result;
}

int kernel_proc_fd_pseudo_snapshot(kernel_pid_t pid, uint64_t identity,
                                   int fd,
                                   struct kernel_proc_fd_pseudo *snapshot)
{
    if (pid <= 0 || !identity || fd < 0 || !snapshot)
        return -KERNEL_EINVAL;
    KERNEL_IRQ_SCOPE(proc_irq);
    SCHEDULER_TASK_BORROW(task, find_resource_owner(pid, identity));
    PROC_ACCESS(task);
    KERNEL_FILES_PIN_SCOPE(fd_guard);
    struct kernel_open_file_description *file = task &&
        kernel_files_is_live(&task->files)
        ? kernel_files_hold_fd(&task->files, fd, &fd_guard) : 0;
    if(proc_access_check(&access))return -KERNEL_EACCES;
    int result = file ? -KERNEL_ENOTSUP : -KERNEL_ENOENT;
    if (file && !kernel_open_file_path(file)) {
        snapshot->kind = (uint8_t)kernel_open_file_kind(file);
        snapshot->object_identity = kernel_open_file_pseudo_identity(file);
        result = snapshot->kind==KERNEL_OPEN_FILE_KIND_MQUEUE
            ?kernel_mqueue_name(file,snapshot->name,sizeof(snapshot->name)):0;
        if(snapshot->kind==KERNEL_OPEN_FILE_KIND_BPF){
            const char *name=kernel_bpf_fd_name(file);
            if(!name)__builtin_trap();
            size_t n=strlen(name)+1;if(n>sizeof(snapshot->name))__builtin_trap();
            memcpy(snapshot->name,name,n);
        }
        if(snapshot->kind==KERNEL_OPEN_FILE_KIND_NAMESPACE) {
            struct kernel_namespace *ns=kernel_namespace_file(file);
            if(!ns)__builtin_trap();
            const char *prefix=ns->type==KERNEL_NS_MNT ? "mnt:[" :
                ns->type==KERNEL_NS_UTS ? "uts:[" : ns->type==KERNEL_NS_IPC ? "ipc:[" : ns->type==KERNEL_NS_PID ? "pid:[" : NULL;
            if(!prefix)__builtin_trap();
            memcpy(snapshot->name,prefix,6);
        }
    }
    return result;
}

int kernel_proc_fdinfo_access(kernel_pid_t pid,uint64_t identity)
{
    KERNEL_IRQ_SCOPE(proc_irq);
    SCHEDULER_TASK_BORROW(task,find_resource_owner(pid,identity));
    PROC_ACCESS(task);
    return proc_access_check(&access);
}
static void proc_pidfd_numbers(struct kernel_pid *id, struct kernel_pid_registry *view,
    struct kernel_proc_fdinfo *snapshot)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    snapshot->pid = id->members[KERNEL_PID_TID] ? kernel_pid_number_in(id, view) : -1;
    snapshot->namespace_count = snapshot->pid > 0 ?
        proc_namespace_ids(id, view, snapshot->namespace_ids) : 0;
}

int kernel_proc_fdinfo_snapshot_in(struct kernel_pid_registry *view,kernel_pid_t pid,uint64_t identity,int fd,
    struct kernel_proc_fdinfo *snapshot)
{
    if(pid<=0 || !identity || fd<0 || !snapshot)return -KERNEL_EINVAL;
    KERNEL_IRQ_SCOPE(proc_irq);
    SCHEDULER_TASK_BORROW(task,find_resource_owner(pid,identity));
    PROC_ACCESS(task);
    KERNEL_FILES_PIN_SCOPE(pin);
    struct kernel_open_file_description *file=task && kernel_files_is_live(&task->files)?
        kernel_files_hold_fd(&task->files,fd,&pin):0;
    if(!file)return -KERNEL_ENOENT;
    uint32_t fd_flags;
    {
        KERNEL_FILES_SCOPE(guard,&task->files);
        if((uint64_t)fd>=task->files.record->statistics.capacity ||
            task->files.record->slots[fd].description!=file)return -KERNEL_ENOENT;
        fd_flags=task->files.record->slots[fd].flags;
    }
    struct kernel_vfs_stat stat={0};
    int error=kernel_open_file_path(file)?kernel_vfs_fstat(&file->file,&stat):
        kernel_open_file_pseudo_stat(file,&stat);
    if(error)return error;
    /* fdinfo is an instantaneous metadata read. It may name its own OFD, so
     * taking the target offset mutex would recursively lock the read owner. */
    uint64_t position=__atomic_load_n(&file->offset,__ATOMIC_RELAXED);
    if(proc_access_check(&access))return -KERNEL_EACCES;
    *snapshot=(struct kernel_proc_fdinfo){.position=position,
        .flags=kernel_open_file_flags(file)|((fd_flags&KERNEL_FILES_FD_CLOEXEC)?02000000U:0),
        .mount_id=file->file.mount?file->file.mount->id:0,.inode=stat.ino,
        .pidfd=file->kind==KERNEL_OPEN_FILE_KIND_PIDFD,
        .pid=0};
    if (snapshot->pidfd) {
        struct kernel_pid *id __attribute__((cleanup(kernel_task_identity_end))) =
            kernel_pidfd_identity_ref(file);
        proc_pidfd_numbers(id, view, snapshot);
    }
    if(file->kind==KERNEL_OPEN_FILE_KIND_BPF)
        return kernel_bpf_fdinfo(file,snapshot->extra,sizeof(snapshot->extra),&snapshot->extra_length);
    if (file->kind == KERNEL_OPEN_FILE_KIND_INOTIFY ||
        file->kind == KERNEL_OPEN_FILE_KIND_FANOTIFY) {
        if (kernel_open_file_acquire(file)) __builtin_trap();
        snapshot->notification = file;
    }
    return 0;
}

int kernel_proc_fdinfo_snapshot(kernel_pid_t pid, uint64_t identity, int fd,
    struct kernel_proc_fdinfo *snapshot)
{ return kernel_proc_fdinfo_snapshot_in(&scheduler.identities, pid, identity, fd, snapshot); }

int kernel_proc_fd_pseudo_stat(kernel_pid_t pid, uint64_t identity,
                               int fd, struct kernel_vfs_stat *stat)
{
    if (pid <= 0 || !identity || fd < 0 || !stat)
        return -KERNEL_EINVAL;
    KERNEL_IRQ_SCOPE(proc_irq);
    SCHEDULER_TASK_BORROW(task, find_resource_owner(pid, identity));
    PROC_ACCESS(task);
    KERNEL_FILES_PIN_SCOPE(fd_guard);
    struct kernel_open_file_description *file = task &&
        kernel_files_is_live(&task->files)
        ? kernel_files_hold_fd(&task->files, fd, &fd_guard) : 0;
    if(proc_access_check(&access))return -KERNEL_EACCES;
    /* 伪对象的元数据快照不分配也不等待；解锁后不再访问 OFD。 */
    int result = file ? kernel_open_file_pseudo_stat(file, stat)
                      : -KERNEL_ENOENT;
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
    KERNEL_IRQ_SCOPE(proc_irq);
    SCHEDULER_TASK_BORROW(task, find_resource_owner(pid, identity));
    PROC_ACCESS(task);
    KERNEL_FILES_PIN_SCOPE(fd_guard);
    struct kernel_open_file_description *file = task &&
        kernel_files_is_live(&task->files)
        ? kernel_files_hold_fd(&task->files, fd, &fd_guard) : 0;
    if(proc_access_check(&access))return -KERNEL_EACCES;
    int result = file ? -KERNEL_ENXIO : -KERNEL_ENOENT;
    /* 解锁后 fd 可关闭；先钉住 pipe endpoint 再分配新 OFD。 */
    if (file && kernel_open_file_path(file)) result = -KERNEL_ENOTSUP;
    else if(file && kernel_open_file_kind(file)==KERNEL_OPEN_FILE_KIND_NAMESPACE)
        return kernel_namespace_open(kernel_namespace_file(file),heap,flags,owner);
    else if(file && kernel_open_file_kind(file)==KERNEL_OPEN_FILE_KIND_MQUEUE)
        return kernel_mqueue_reopen(file,heap,flags,owner);
    else if (file &&
             kernel_open_file_kind(file) == KERNEL_OPEN_FILE_KIND_PIPE)
        result = kernel_open_file_pipe_pin(file, flags, &pin);
    if (result || !pin.pipe) return result;
    return kernel_open_file_pipe_finish(heap, &pin, flags, owner);
}

static int proc_process_visible(struct kernel_task *task, struct kernel_task *root,
                                kernel_pid_t pid, uint64_t identity)
{
    if (!scheduler_task_descends_from(task, root, 0) || !proc_group_visible(task)) return 0;
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    /* 存储借用只保证对象未释放；祖先遍历期间exec可转移同一TGID。 */
    return task->accounted && !task->retirement &&
        process_find_identity(pid, KERNEL_PID_TGID) == task &&
        process_identity_generation(task) == identity;
}

int kernel_proc_next_process(kernel_pid_t after, kernel_pid_t *pid,
                             uint64_t *identity)
{
    if (!pid || !identity || after < 0) return -KERNEL_EINVAL;
    KERNEL_IRQ_SCOPE(proc_irq);
    SCHEDULER_TASK_BORROW(root, 0);
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        root = scheduler.init_task;
        if (root) scheduler_task_get_locked(root);
    }
    if (!root) return -KERNEL_ENOENT;
    for (;;) {
        SCHEDULER_TASK_BORROW(task, 0);
        uint64_t token;
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            struct kernel_pid *id = kernel_pid_next_published(&scheduler.identities, after);
            if (!id) return -KERNEL_ENOENT;
            after = id->number;
            token = id->generation;
            task = id->members[KERNEL_PID_TGID] ? id->members[KERNEL_PID_TGID]->task : 0;
            if (task && task->accounted && !task->retirement) scheduler_task_get_locked(task);
            else task = 0;
        }
        if (task && proc_process_visible(task, root, after, token)) {
            *pid = after; *identity = token;
            return 0;
        }
    }
}

int kernel_proc_process_identity_acquire(kernel_pid_t pid,uint64_t generation,
    struct kernel_pid **owner)
{
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    struct kernel_pid *id=kernel_pid_find(&scheduler.identities,pid);
    if(!id || id->generation!=generation || !id->members[KERNEL_PID_TGID])return -KERNEL_ENOENT;
    kernel_pid_get(id);*owner=id;return 0;
}

int kernel_proc_process_identity(kernel_pid_t pid, uint64_t *identity)
{
    if (pid <= 0 || !identity) return -KERNEL_EINVAL;
    KERNEL_IRQ_SCOPE(proc_irq);
    SCHEDULER_TASK_BORROW(task, 0);
    SCHEDULER_TASK_BORROW(root, 0);
    uint64_t token;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        task = process_find_identity(pid, KERNEL_PID_TGID);
        root = scheduler.init_task;
        if (!task || !task->accounted || task->retirement || !root) {
            task = root = 0;
            return -KERNEL_ENOENT;
        }
        token = process_identity_generation(task);
        scheduler_task_get_locked(task);
        scheduler_task_get_locked(root);
    }
    if (!proc_process_visible(task, root, pid, token)) return -KERNEL_ENOENT;
    *identity = token;
    return 0;
}
