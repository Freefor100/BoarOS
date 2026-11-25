#if defined(__riscv)
#include <arch/riscv/vector.h>
#endif
#include "../../fs/files/private.h"
#include "../../fs/cgroup_internal.h"
#include "../../fs/open_file_internal.h"
#include <kernel/reboot.h>
#include <kernel/namespace.h>
#include <kernel/ipc_namespace.h>
#include <kernel/uts.h>
#include <kernel/proc_task.h>
#include <kernel/posix_timer.h>
#include <kernel/credentials.h>
#include <kernel/keys.h>
#include <arch/task.h>
#include <arch/context.h>
#include <kernel/errno.h>
#include <kernel/process_acct.h>
#include <kernel/irq.h>
#include <kernel/epoll.h>
#include <kernel/exec.h>
#include <kernel/fanotify.h>
#include <kernel/files.h>
#include <kernel/futex.h>
#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/random.h>
#include <kernel/mm.h>
#include <kernel/vfs.h>
#include <kernel/msg.h>
#include <kernel/mqueue.h>
#include <kernel/tick.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/pid.h>
#include <kernel/pidfd.h>
#include <kernel/scheduler.h>
#include <kernel/socket.h>
#include <kernel/socket_call.h>
#include <kernel/sem.h>
#include <kernel/signal.h>
#include <kernel/task.h>
#include <kernel/page_cache.h>
#include <kernel/uaccess.h>
#include <kernel/tty_task.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../exec_internal.h"
#include "private.h"
#include <kernel/seccomp.h>
#include "ptrace_internal.h"
#include <kernel/remote_memory.h>
#include <kernel/io_uring.h>

#define LINUX_WNOHANG UINT32_C(0x00000001)
#define LINUX_WUNTRACED UINT32_C(0x00000002)
#define LINUX_WSTOPPED UINT32_C(0x00000002)
#define LINUX_WEXITED UINT32_C(0x00000004)
#define LINUX_WCONTINUED UINT32_C(0x00000008)
#define LINUX_WNOWAIT UINT32_C(0x01000000)
#define LINUX_WAIT_CONTINUED UINT32_C(0xffff)
#define LINUX___WNOTHREAD UINT32_C(0x20000000)
#define LINUX___WALL UINT32_C(0x40000000)
#define LINUX___WCLONE UINT32_C(0x80000000)

#define LINUX_P_ALL 0
#define LINUX_P_PID 1
#define LINUX_P_PGID 2
#define LINUX_P_PIDFD 3

#define LINUX_WAIT4_SUPPORTED_OPTIONS \
    (LINUX_WNOHANG | LINUX_WUNTRACED | LINUX_WCONTINUED | \
     LINUX___WNOTHREAD | LINUX___WALL | LINUX___WCLONE)

#define LINUX_WAITID_SUPPORTED_OPTIONS \
    (LINUX_WNOHANG | LINUX_WNOWAIT | LINUX_WEXITED | LINUX_WSTOPPED | \
     LINUX_WCONTINUED | LINUX___WNOTHREAD | LINUX___WALL | LINUX___WCLONE)
#define LINUX_CLONE_VM UINT64_C(0x100)
#define LINUX_CLONE_FS UINT64_C(0x200)
#define LINUX_CLONE_NEWNS UINT64_C(0x20000)
#define LINUX_CLONE_NEWUTS UINT64_C(0x04000000)
#define LINUX_CLONE_NEWPID UINT64_C(0x20000000)
#define LINUX_CLONE_NEWIPC UINT64_C(0x08000000)
#define LINUX_CLONE_FILES UINT64_C(0x400)
#define LINUX_CLONE_SIGHAND UINT64_C(0x800)
#define LINUX_CLONE_CLEAR_SIGHAND UINT64_C(0x100000000)
#define LINUX_CLONE_VFORK UINT64_C(0x4000)
#define LINUX_CLONE_SYSVSEM UINT64_C(0x40000)
#define LINUX_CLONE_THREAD UINT64_C(0x10000)
#define LINUX_CLONE_SETTLS UINT64_C(0x80000)
#define LINUX_CLONE_PARENT_SETTID UINT64_C(0x100000)
#define LINUX_CLONE_CHILD_CLEARTID UINT64_C(0x200000)
#define LINUX_CLONE_CHILD_SETTID UINT64_C(0x1000000)

static struct kernel_task *session_target(struct kernel_task *caller, kernel_pid_t pid)
{
    struct kernel_task *target = pid ? process_find_identity_in(pid, KERNEL_PID_TID, process_pid_active(caller)) : caller;
    return target && target->accounted && target->group_leader ? target : 0;
}

static void identity_change_role(struct kernel_task *task, enum kernel_pid_role role,
                                 struct kernel_pid *replacement)
{
    if (task->identities[role].identity == replacement) return;
    kernel_pid_get(replacement);
    kernel_pid_detach(&task->identities[role]);
    kernel_pid_attach(&task->identities[role], replacement, role, task);
    kernel_pid_put(replacement);
}

int64_t kernel_task_getpgid(struct kernel_task *caller, kernel_pid_t pid)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    struct kernel_task *target = session_target(caller, pid);
    return target ? kernel_pid_number_in(process_identity(target, KERNEL_PID_PGID), process_pid_active(caller)) : -KERNEL_ESRCH;
}

int64_t kernel_task_getsid(struct kernel_task *caller, kernel_pid_t pid)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    struct kernel_task *target = session_target(caller, pid);
    return target ? kernel_pid_number_in(process_identity(target, KERNEL_PID_SID), process_pid_active(caller)) : -KERNEL_ESRCH;
}

static int64_t setpgid_locked(struct kernel_task *caller, kernel_pid_t pid, kernel_pid_t pgid)
{
    struct kernel_task *leader = caller->group_leader;
    if (!pid) pid = kernel_pid_number_in(process_identity(leader, KERNEL_PID_TGID), process_pid_active(caller));
    if (!pgid) pgid = pid;
    if (pgid < 0) return -KERNEL_EINVAL;
    struct kernel_task *target = session_target(caller, pid);
    if (!target) return -KERNEL_ESRCH;
    if (target != target->group_leader) return -KERNEL_EINVAL;
    if (target->parent == leader) {
        if (process_identity(target, KERNEL_PID_SID) != process_identity(leader, KERNEL_PID_SID))
            return -KERNEL_EPERM;
        if (!target->fork_no_exec) return -KERNEL_EACCES;
    } else if (target != leader) return -KERNEL_ESRCH;
    if (target->session_leader) return -KERNEL_EPERM;
    struct kernel_pid *group = process_identity(target, KERNEL_PID_TGID);
    if (pgid != pid) {
        group = kernel_pid_find(process_pid_active(caller), pgid);
        struct kernel_task *member = group && group->members[KERNEL_PID_PGID]
            ? group->members[KERNEL_PID_PGID]->task : 0;
        if (!member || process_identity(member, KERNEL_PID_SID) !=
                       process_identity(leader, KERNEL_PID_SID)) return -KERNEL_EPERM;
    }
    identity_change_role(target, KERNEL_PID_PGID, group);
    return 0;
}

int64_t kernel_task_setpgid(struct kernel_task *caller, kernel_pid_t pid, kernel_pid_t pgid)
{
    int64_t result;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        result = setpgid_locked(caller, pid, pgid);
    }
    process_identity_collect();
    return result;
}

int64_t kernel_task_setsid(struct kernel_task *caller)
{
    int64_t result;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        struct kernel_task *leader = caller->group_leader;
        struct kernel_pid *identity = process_identity(leader, KERNEL_PID_TGID);
        if (leader->session_leader || identity->members[KERNEL_PID_PGID]) return -KERNEL_EPERM;
        leader->session_leader = 1;
        identity_change_role(leader, KERNEL_PID_SID, identity);
        identity_change_role(leader, KERNEL_PID_PGID, identity);
        result = kernel_pid_number_in(identity, process_pid_active(caller));
    }
    kernel_task_tty_clear(caller);
    process_identity_collect();
    return result;
}

static int group_orphaned(struct kernel_pid *group, const struct kernel_task *ignored)
{
    if (!group) return 1;
    int orphaned = 1;
    struct scheduler_task_cursor cursor;
    scheduler_task_cursor_begin(&cursor);
    while (cursor.task) {
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            struct kernel_task *member = cursor.task;
            struct kernel_task *parent = member->parent;
            if (member->accounted && member->identities[KERNEL_PID_PGID].identity == group &&
                member != ignored && parent &&
                /* Only global init is excluded from POSIX orphan checks;
                 * a nested init can still connect two groups in one session. */
                parent != process_pid_reaper_locked(&scheduler.identities) &&
                !(member->group_members == 1 && (member->state == KERNEL_THREAD_STATE_EXITED ||
                  member->state == KERNEL_THREAD_STATE_ZOMBIE || member->state == KERNEL_THREAD_STATE_GROUP_DEAD)) &&
                process_identity(parent, KERNEL_PID_PGID) != group &&
                process_identity(parent, KERNEL_PID_SID) == process_identity(member, KERNEL_PID_SID))
                orphaned = 0;
        }
        if (!orphaned) break;
        scheduler_task_cursor_next(&cursor);
    }
    scheduler_task_cursor_end(&cursor);
    return orphaned;
}

int process_group_is_orphaned(const struct kernel_task *task)
{
    struct kernel_pid *group __attribute__((cleanup(kernel_task_identity_end))) =
        kernel_task_tty_identity(task, KERNEL_PID_PGID);
    return group_orphaned(group, 0);
}

void process_orphan_notify(struct kernel_task *task, struct kernel_task *old_parent)
{
    struct kernel_pid *group __attribute__((cleanup(kernel_task_identity_end))) = 0;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        if (!task->identities[KERNEL_PID_TGID].identity) return;
        struct kernel_task *parent = old_parent ? old_parent : task->parent;
        group = process_identity(task, KERNEL_PID_PGID);
        if (!parent || process_identity(parent, KERNEL_PID_PGID) == group ||
            process_identity(parent, KERNEL_PID_SID) != process_identity(task, KERNEL_PID_SID)) {
            group = 0;
            return;
        }
        kernel_pid_get(group);
    }
    if (!group_orphaned(group, old_parent ? 0 : task)) return;
    int stopped = 0;
    struct scheduler_task_cursor cursor;
    scheduler_task_cursor_begin(&cursor);
    while (cursor.task) {
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            struct kernel_task *member = cursor.task;
            if (member->accounted && member->identities[KERNEL_PID_PGID].identity == group && member->group_stopped)
                stopped = 1;
        }
        scheduler_task_cursor_next(&cursor);
    }
    if (stopped) {
        kernel_task_tty_signal_group(group, 1);
        kernel_task_tty_signal_group(group, 18);
    }
}

static void child_creator_change_locked(struct kernel_task *child, struct kernel_pid *creator)
{
    if (creator) kernel_pid_get(creator);
    if (child->child_creator) kernel_pid_put(child->child_creator);
    child->child_creator = creator;
}

static void identity_adopt(struct kernel_task *task, struct kernel_task *leader)
{
    task->controlling_tty = leader->controlling_tty;
    leader->controlling_tty = 0;
    process_identity_release_locked(task);
    for (unsigned role = 0; role < KERNEL_PID_ROLES; role++)
        kernel_pid_transfer(&leader->identities[role], &task->identities[role], task);
    process_identity_sync_locked(task);
    process_identity_sync_locked(leader);
}

void process_group_initialize(struct kernel_task *task)
{
    task->group_next = task;
    task->group_previous = task;
    kernel_wait_queue_init(&task->group_wait_queue);
}

int kernel_task_set_parent_death_signal(struct kernel_task *task, uint64_t sig)
{
    if (sig > KERNEL_SIGNAL_COUNT) return -KERNEL_EINVAL;
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    task->parent_death_signal = (uint32_t)sig;
    return 0;
}

uint32_t kernel_task_get_parent_death_signal(struct kernel_task *task)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    return task->parent_death_signal;
}

void kernel_task_set_subreaper(struct kernel_task *task, int enabled)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    task->group_leader->child_subreaper = !!enabled;
}

uint32_t kernel_task_get_subreaper(struct kernel_task *task)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    return task->group_leader->child_subreaper;
}

static void request_thread_termination_locked(struct kernel_task *task)
{
    if (task->state == KERNEL_THREAD_STATE_EXITED || task->state == KERNEL_THREAD_STATE_ZOMBIE ||
        task->state == KERNEL_THREAD_STATE_GROUP_DEAD || task->retirement) return;
    __atomic_store_n(&task->terminate_requested,1,__ATOMIC_RELAXED);
#if BOAROS_USER_SMP
    scheduler_user_request_locked(task);
#endif
    if (task->vfork_waiting && task->vfork_wait_child) {
        task->vfork_wait_child->vfork_parent = 0;
        task->vfork_wait_child->vfork_child = 0;
        task->vfork_wait_child = 0;
        task->vfork_waiting = 0;
        if (task->state == KERNEL_THREAD_STATE_BLOCKED && __atomic_load_n(&task->default_wait_node.queue,__ATOMIC_ACQUIRE) == &task->vfork_done_queue) {
            struct kernel_wait_token token = {task, kernel_wait_record_generation(&task->wait)};
            (void)kernel_wait_notify_locked(&token, KERNEL_WAIT_SIGNALLED);
        }
    }
    if (task->state == KERNEL_THREAD_STATE_STOPPED) {
        KERNEL_RUN_SET(runs);
        stopped_unlink_locked(task);
        task->state = KERNEL_THREAD_STATE_READY;
        if (task->wait.on_cpu) task->wait.ready_pending = 1;
        else ready_enqueue_locked(task, 0);
    }
    ptrace_terminate_locked(task);
    kernel_seccomp_abort_wait_locked(task);
    kernel_wait_interrupt_locked(task);
}

static void terminate_group_members(struct kernel_task *leader, struct kernel_task *except)
{
    struct scheduler_group_cursor cursor;
    scheduler_group_cursor_begin(leader, &cursor);
    while (cursor.task) {
        struct kernel_task *task = cursor.task;
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            if (task->accounted && task->group_leader == leader && task != except)
                request_thread_termination_locked(task);
        }
        scheduler_group_cursor_next(&cursor);
    }
    scheduler_group_cursor_end(&cursor);
}

void process_group_request_exit(struct kernel_task *task,
    enum kernel_thread_exit_reason reason, uint64_t status, uint64_t detail)
{
    struct kernel_task *leader;
    struct kernel_wait_cursor cursor;
    int single = 0, notify = 0;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        leader = task->group_leader;
        if (!leader) __builtin_trap();
        if (!leader->group_exiting) {
            leader->group_exiting = 1;
            process_pid_reaper_disable_locked(leader);
            /* Exec may have adopted another TID since task construction.
             * Commit the current group identity with its final exit result. */
            leader->completion.tid = leader->tid;
            leader->completion.tgid = leader->tid;
            leader->completion.reason = reason;
            leader->completion.status = status;
            leader->completion.detail = detail;
        }
        single = leader->group_members == 1;
        if (single) {
            if (leader != task || leader->group_next != leader || leader->group_previous != leader) __builtin_trap();
            request_thread_termination_locked(leader);
            notify = kernel_wait_cursor_prepare(&leader->group_wait_queue, &cursor);
            if (notify) scheduler_task_get_locked(leader);
        } else scheduler_task_get_locked(leader);
    }
    /* Caller owns task storage. A sole member needs no global traversal or
     * cross-unlock borrow unless notification actually has a cursor owner. */
    if (single) {
        if (notify) {
            kernel_wait_cursor_wake(&cursor, 1);
            scheduler_task_put(leader);
        }
        return;
    }
    terminate_group_members(leader, 0);
    (void)kernel_wait_queue_wake_all(&leader->group_wait_queue);
    scheduler_task_put(leader);
}

void kernel_user_group_exit(enum kernel_thread_exit_reason reason,
                            uint64_t status, uint64_t detail)
{
#if BOAROS_USER_SMP
    if (kernel_task_has_user_context(kernel_task_current()) && !kernel_scheduler_user_service_active())
        kernel_scheduler_enter_user_service();
#endif
    if (reason == KERNEL_THREAD_EXIT_SYSCALL)
        process_group_request_exit(kernel_cpu_current()->current, reason, status, detail);
    kernel_user_thread_exit(reason, status, detail);
}

struct arch_fpu_state *arch_process_fpu_borrow_current(void)
{
    return &kernel_cpu_current()->current->fpu;
}

enum kernel_mm_status kernel_scheduler_resolve_current_user_fault(
    uint64_t virtual_address,
    uint32_t access)
{
    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED || arch_interrupt_is_enabled())
        return KERNEL_MM_STATUS_STATE;
    /* CPU借用只覆盖IRQ关闭的入口校验；缺页可能睡眠，后续只使用
     * 当前执行owner固定的task/MM，不把CPU借用带入缺页服务。 */
    struct kernel_cpu *cpu = kernel_cpu_current();
    struct kernel_task *current = cpu->current;
    if (check_running_on_cpu(cpu) != KERNEL_SCHEDULER_STATUS_OK ||
        current == cpu->idle || current->state != KERNEL_THREAD_STATE_RUNNING ||
        current->arch.user_mode != 1U || current->mm.state != KERNEL_MM_LIVE ||
        cpu->active_mm_record != current->mm.record) {
        return KERNEL_MM_STATUS_STATE;
    }
    return kernel_mm_resolve_owned_user_fault(&current->mm,current,virtual_address,access);
}

static enum kernel_scheduler_status validate_child_endpoints(
    const struct kernel_task *parent)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    if (parent == 0 || parent->magic != KERNEL_THREAD_MAGIC ||
        parent->arch.user_mode != 1U || parent->tid_owned != 1U ||
        parent->tid <= 0 ||
        ((parent->first_child == 0) != (parent->last_child == 0)) ||
        (parent->first_child != 0 &&
         parent->first_child->previous_sibling != 0) ||
        (parent->last_child != 0 &&
         (parent->last_child->next_sibling != 0 ||
          parent->last_child->parent != parent))) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}

static void child_append_locked(struct kernel_task *parent,
                         struct kernel_task *child)
{
    /* 在实际改链处核对局部关系，退出与收养不再预扫整个父子树。 */
    if (!parent || !child || parent == child || child->parent ||
        child->previous_sibling || child->next_sibling ||
        ((parent->first_child == 0) != (parent->last_child == 0)) ||
        (parent->first_child &&
         (parent->first_child->previous_sibling || parent->first_child->parent != parent)) ||
        (parent->last_child &&
         (parent->last_child->next_sibling || parent->last_child->parent != parent)))
        __builtin_trap();
    if (++parent->children_epoch == 0) __builtin_trap();
    child->parent = parent;
    child->previous_sibling = parent->last_child;
    child->next_sibling = 0;
    if (parent->last_child == 0) {
        parent->first_child = child;
    } else {
        parent->last_child->next_sibling = child;
    }
    parent->last_child = child;
}

static void child_remove_locked(struct kernel_task *parent,
                         struct kernel_task *child)
{
    if (!parent || !child || parent == child || child->parent != parent ||
        !parent->first_child || !parent->last_child ||
        child->previous_sibling == child || child->next_sibling == child ||
        (child->previous_sibling ?
            (child->previous_sibling->parent != parent ||
             child->previous_sibling->next_sibling != child) :
            parent->first_child != child) ||
        (child->next_sibling ?
            (child->next_sibling->parent != parent ||
             child->next_sibling->previous_sibling != child) :
            parent->last_child != child))
        __builtin_trap();
    if (++parent->children_epoch == 0) __builtin_trap();
    if (child->previous_sibling == 0) {
        parent->first_child = child->next_sibling;
    } else {
        child->previous_sibling->next_sibling = child->next_sibling;
    }
    if (child->next_sibling == 0) {
        parent->last_child = child->previous_sibling;
    } else {
        child->next_sibling->previous_sibling =
            child->previous_sibling;
    }
    child->parent = 0;
    child->previous_sibling = 0;
    child->next_sibling = 0;
}

static void child_remove(struct kernel_task *parent, struct kernel_task *child)
{ KERNEL_RAW_SCOPE(guard, &kernel_wait_domain); child_remove_locked(parent, child); }

void wake_waiting_parent(struct kernel_task *child)
{
    SCHEDULER_TASK_BORROW(parent, 0);
    struct kernel_wait_cursor cursor;
    int notify = 0;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        struct kernel_task *target = child->parent;
        if (target) {
            notify = kernel_wait_cursor_prepare(&target->child_exit_queue, &cursor);
            /* 空队列/单任务通知已在域内结束；只有跨解锁的游标才需固定容器。 */
            if (notify) { scheduler_task_get_locked(target); parent = target; }
        }
    }
    if (notify) kernel_wait_cursor_wake(&cursor, 1);
}

/* Notifies a vfork-suspended parent that the child released its shared
 * address space (through exec or exit). */
void process_complete_vfork(struct kernel_task *thread)
{
    SCHEDULER_TASK_BORROW(parent, 0);
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        parent = thread->vfork_parent;
        if (!parent) return;
        if (!thread->vfork_child || !parent->vfork_waiting ||
            parent->vfork_wait_child != thread) __builtin_trap();
        scheduler_task_get_locked(parent);
        thread->vfork_child = 0;
        thread->vfork_parent = 0;
        parent->vfork_waiting = 0;
        parent->vfork_wait_child = 0;
    }
    (void)kernel_wait_queue_wake_one(&parent->vfork_done_queue);
}

static int vfork_waiting(const struct kernel_task *task)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    return task->vfork_waiting;
}

static void exited_append(struct kernel_task *thread)
{
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        exited_append_locked(thread);
    }
    (void)kernel_wait_queue_wake_all(&scheduler.cleanup_queue);
}

static int group_exec_waiting(struct kernel_task *task, struct kernel_task *leader)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    return leader->group_members != (task == leader ? 1U : 2U) ||
           (task != leader && leader->state != KERNEL_THREAD_STATE_GROUP_DEAD);
}

enum kernel_scheduler_status process_group_exec_current(void)
{
    struct kernel_task *task = kernel_cpu_current()->current;
    struct kernel_task *leader = task->group_leader;
    enum kernel_wait_wake_reason reason;
    enum kernel_scheduler_status status;

    /* The first prepared exec owns group teardown. A competing exec must
     * unwind its own prepared image through the ordinary exit cleanup. */
    int rejected;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        rejected = leader->group_exiting || leader->group_execing || task->terminate_requested;
        if (!rejected) leader->group_execing = 1;
    }
    if (rejected) kernel_user_thread_exit(KERNEL_THREAD_EXIT_SYSCALL, 0, 0);
    terminate_group_members(leader, task);
    while (group_exec_waiting(task, leader)) {
        status = KERNEL_WAIT_RECHECK(&leader->group_wait_queue, 0, 0, &reason,
                                     group_exec_waiting(task, leader));
        if (status != KERNEL_SCHEDULER_STATUS_OK) return status;
        if (leader->group_exiting)
            kernel_user_thread_exit(KERNEL_THREAD_EXIT_SYSCALL, 0U, 0U);
    }
    if (task != leader) {
        /* The dying leader loses its numeric identity here. The execing
         * thread keeps its own trace relationship across identity_adopt. */
        ptrace_consume_final(leader);
        if (task->controlling_tty) kernel_task_tty_clear(task);
        kernel_signal_timer_adopt(task, leader);
        struct kernel_pid *old_identity;
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            old_identity = process_identity(task, KERNEL_PID_TID);
            kernel_pid_get(old_identity);
            identity_adopt(task, leader);
            /* 身份转移即释放旧容器的线程配额，存储回收可继续延后。 */
            kernel_nproc_release_locked(leader);
            task->proc_start_ticks = leader->proc_start_ticks;
            task->exit_signal = 17U;
            task->parent = leader->parent;
            task->previous_sibling = leader->previous_sibling;
            task->next_sibling = leader->next_sibling;
            if (task->parent) {
                if (++task->parent->children_epoch == 0) __builtin_trap();
                if (task->previous_sibling) task->previous_sibling->next_sibling = task;
                else task->parent->first_child = task;
                if (task->next_sibling) task->next_sibling->previous_sibling = task;
                else task->parent->last_child = task;
            }
        task->session_leader = leader->session_leader;
        task->child_subreaper = leader->child_subreaper;
        __atomic_store_n(&task->signal_unkillable,leader->signal_unkillable,__ATOMIC_RELAXED);
        task->child_creator = leader->child_creator;
        leader->child_creator = 0;
        scheduler_usage_add(&task->limits->dead_usage, &leader->usage);
        __atomic_store_n(&task->group_pending,leader->group_pending,__ATOMIC_RELAXED);
        task->group_child_info = leader->group_child_info;
        if(task->limits!=leader->limits)__builtin_trap();
        kernel_signal_pending_adopt_locked(task,leader);
        for (unsigned i = 0U; i < KERNEL_SIGNAL_COUNT; i++)
            task->group_sender[i] = leader->group_sender[i];
        for (unsigned i = 0U; i < KERNEL_SIGNAL_COUNT; i++)
            task->group_signal_code[i] = leader->group_signal_code[i];
        task->publish_completion = leader->publish_completion;
        if (scheduler.init_task == leader) scheduler.init_task = task;
            /* The old container stays owned until every child link moves.
             * Its group alias resolves the adopted parent identity meanwhile. */
            task->group_leader = task;
            leader->group_leader = task;
            kernel_posix_timers_adopt_locked(task,leader);
        }
        for (;;) {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            struct kernel_task *child = leader->first_child;
            if (!child) break;
            child_remove_locked(leader, child);
            child_append_locked(task, child);
            if (child->child_creator == old_identity)
                child_creator_change_locked(child, process_identity(task, KERNEL_PID_TID));
        }
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            leader->parent = 0;
            leader->previous_sibling = leader->next_sibling = 0;
            leader->publish_completion = 0;
            leader->group_leader = 0;
            leader->group_members = 0;
            leader->state = KERNEL_THREAD_STATE_EXITED;
            exited_append_locked(leader);
            task->group_members = 1;
            if (++task->group_epoch == 0) __builtin_trap();
            process_group_initialize(task);
        }
        kernel_pidfd_notification_end(&old_identity);
        (void)kernel_wait_queue_wake_all(&scheduler.cleanup_queue);
    }
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        task->exit_signal = 17U;
        task->group_execing = 0;
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}

void kernel_task_namespaces_initial(struct kernel_task *task)
{
    if(task->uts_namespace || task->ipc_namespace)__builtin_trap();
    task->uts_namespace=kernel_namespace_get(kernel_uts_initial());
    task->ipc_namespace=kernel_namespace_get(kernel_ipc_initial());
}
void kernel_task_namespaces_release(struct kernel_task *task)
{
    if(!task->uts_namespace && !task->ipc_namespace && !task->pid_for_children)return;
    struct kernel_namespace *uts,*ipc;
    int had_pid_override;
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
      uts=task->uts_namespace;ipc=task->ipc_namespace;
      task->uts_namespace=task->ipc_namespace=0;
      had_pid_override=task->pid_for_children!=NULL;
      if(task->pid_for_children) {
          kernel_pid_registry_put(task->pid_for_children);task->pid_for_children=NULL;
      } }
    kernel_namespace_put(uts);kernel_namespace_put(ipc);
    if(had_pid_override)process_identity_collect();
}
struct kernel_namespace *kernel_task_uts_namespace(void)
{
    struct kernel_task *task=kernel_task_current();
    if(task && task->arch.user_mode && !task->uts_namespace)__builtin_trap();
    return task && task->uts_namespace?task->uts_namespace:kernel_uts_initial();
}
struct kernel_namespace *kernel_task_ipc_namespace(void)
{
    struct kernel_task *task=kernel_task_current();
    if(task && task->arch.user_mode && !task->ipc_namespace)__builtin_trap();
    return task && task->ipc_namespace?task->ipc_namespace:kernel_ipc_initial();
}
/* A successful quota charge belongs to child, including on later errors:
 * the common clone unwind releases it with the other child resources. */
int process_clone_admit(struct kernel_task *child, struct kernel_task *parent, uint64_t flags)
{
    int error = kernel_nproc_acquire(child, parent);
    if (error) return error;
    if ((flags & (LINUX_CLONE_NEWNS | LINUX_CLONE_NEWUTS | LINUX_CLONE_NEWIPC | LINUX_CLONE_NEWPID)) &&
        !kernel_credentials_capable(kernel_credentials_current(), KERNEL_CAP_SYS_ADMIN))
        return -KERNEL_EPERM;
    return 0;
}

static int clone_namespaces(struct kernel_task *child,struct kernel_task *parent,uint64_t flags)
{
    if(!parent->uts_namespace || !parent->ipc_namespace)__builtin_trap();
    int result=0;
    if(flags&LINUX_CLONE_NEWUTS)
        result=kernel_uts_clone(parent->uts_namespace,parent->files.heap,&child->uts_namespace);
    else child->uts_namespace=kernel_namespace_get(parent->uts_namespace);
    if(!result) {
        if(flags&LINUX_CLONE_NEWIPC)
            result=kernel_ipc_namespace_create(parent->files.heap,&child->ipc_namespace);
        else child->ipc_namespace=kernel_namespace_get(parent->ipc_namespace);
    }
    return result;
}

static int release_clone_resources(struct kernel_task *thread)
{
    int cleanup_failed = 0;
    kernel_sem_exit(thread);
    kernel_ioprio_exit(thread);

    if (kernel_signal_release_table(thread) !=
        KERNEL_SIGNAL_STATUS_OK) {
        cleanup_failed = 1;
    }

    if (thread->files.state == KERNEL_FILES_LIVE ||
        thread->files.state == KERNEL_FILES_CLEANUP) {
        if (kernel_files_release_for_task(&thread->files,thread) !=
            KERNEL_FILES_STATUS_OK) {
            cleanup_failed = 1;
        }
    }
    if (thread->fs.state == KERNEL_FS_CONTEXT_LIVE ||
        thread->fs.state == KERNEL_FS_CONTEXT_CLEANUP) {
        if (kernel_fs_context_release(&thread->fs) !=
            KERNEL_FS_CONTEXT_STATUS_OK) {
            cleanup_failed = 1;
        }
    }
    if (thread->mm.state == KERNEL_MM_LIVE ||
        thread->mm.state == KERNEL_MM_CLEANUP) {
        scheduler_usage_save_mm(thread);
        if (kernel_mm_release(&thread->mm) != KERNEL_MM_STATUS_OK) {
            cleanup_failed = 1;
        }
    }
    if(!cleanup_failed)kernel_task_namespaces_release(thread);
    return cleanup_failed;
}

static void queue_abandoned_clone(struct kernel_task *thread)
{
    thread->completion.kind = KERNEL_THREAD_KIND_USER;
    thread->completion.reason = KERNEL_THREAD_EXIT_USER_FAULT;
    thread->completion.tid = thread->tid;
    thread->completion.tgid = thread->tid;
    thread->completion.status = 0U;
    thread->completion.detail = 0U;
    thread->publish_completion = 0U;
    thread->state = KERNEL_THREAD_STATE_EXITED;
    exited_append(thread);
}

static enum kernel_scheduler_status finish_clone_failure(
    struct kernel_task *thread,
    int64_t linux_failure,
    int64_t *linux_result,
    enum kernel_scheduler_status scheduler_failure)
{
    KERNEL_SERVICE_SCOPE(service);
    int cleanup_failed;
    enum kernel_scheduler_status stack_status;

    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);kernel_nproc_release_locked(thread); }
    /* No failed construction has published a parent completion channel. */
    thread->vfork_parent = 0;
    thread->vfork_child = 0U;
    if (thread->controlling_tty) kernel_task_tty_clear(thread);
    if (thread->arch.kernel_sp) {
        stack_status = release_task_stack(thread);
        if (stack_status != KERNEL_SCHEDULER_STATUS_OK) return stack_status;
    } else {
        /* The private pages never acquired a kernel stack mapping. */
        (void)physical_page_release_order(scheduler.allocator,
                                          thread->stack_physical_address, KERNEL_STACK_ORDER);
        thread->stack_physical_address = KERNEL_THREAD_NO_PAGE;
        thread->stack_low = thread->stack_high = 0;
    }
    cleanup_failed = release_clone_resources(thread);

    process_identity_release(thread);
    thread->group_leader = 0;
    thread->group_members = 0U;
    process_identity_collect();
    if (!cleanup_failed) {
        scheduler_forget_task(thread);
        kernel_task_release_io_scratch(thread);
        scheduler_task_retire(thread);
        scheduler_task_collect();
    }
    if (cleanup_failed) {
        queue_abandoned_clone(thread);
    }
    *linux_result = linux_failure;
    return scheduler_failure;
}

static void clone_cgroup_end(struct kernel_cgroup **group)
{ if (*group) kernel_cgroup_put(*group); }

enum kernel_scheduler_status arch_process_clone_current(
    const struct arch_trap_frame *parent_frame,
    uint64_t flags,
    uint64_t child_stack,
    uint64_t parent_tid,
    uint64_t tls,
    uint64_t child_tid,
    uint64_t pidfd_address,
    const int32_t *requested_tids, size_t requested_count,
    int32_t cgroup_fd,
    int64_t *linux_result)
{
    struct kernel_task *parent;
    struct kernel_task *child;
    uint64_t child_context;
    kernel_pid_t tid;
    uint32_t vfork = (flags & (LINUX_CLONE_VM | LINUX_CLONE_VFORK)) ==
                     (LINUX_CLONE_VM | LINUX_CLONE_VFORK);
    int thread_clone = (flags & LINUX_CLONE_THREAD) != 0U;
    enum kernel_wait_wake_reason wake_reason = KERNEL_WAIT_WOKEN;
    enum kernel_mm_status mm_status;
    enum kernel_files_status files_status;
    enum kernel_fs_context_status fs_status;
    enum kernel_pid_status pid_status;
    enum kernel_signal_status signal_status;
    enum kernel_scheduler_status status;
    struct kernel_pidfd_prepared pidfd __attribute__((cleanup(kernel_pidfd_cancel_prepared))) = {0};

    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED) {
        return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (parent_frame == 0 || linux_result == 0 ||
        arch_interrupt_is_enabled()) {
        return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    }
    status = validate_current();
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }
    parent = kernel_cpu_current()->current;
    if (parent == kernel_cpu_current()->idle || parent->arch.user_mode != 1U ||
        parent->tid_owned != 1U ||
        parent_frame != (const struct arch_trap_frame *)(
                            parent->stack_high - sizeof(*parent_frame)) ||
        arch_frame_task(parent_frame) != (uintptr_t)parent ||
        arch_frame_pc(parent_frame) > UINT64_MAX - 4U) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    /* Only the running caller can change its for-children override. Reject a
     * split thread group before allocating a task or charging RLIMIT_NPROC. */
    if (thread_clone && ((flags & LINUX_CLONE_NEWPID) || parent->pid_for_children)) {
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_SCHEDULER_STATUS_OK;
    }
    status = allocate_task_pages(&child);
    if (status == KERNEL_SCHEDULER_STATUS_NO_MEMORY) {
        *linux_result = -KERNEL_ENOMEM;
        return KERNEL_SCHEDULER_STATUS_OK;
    }
    if (status != KERNEL_SCHEDULER_STATUS_OK) return status;
    child->magic = KERNEL_THREAD_MAGIC;
    /* f_cred 身份用于空路径 linkat；普通 fork 与 Linux 一样产生独立
     * credential 对象，补充组和 UID 账户仍共享真实引用。 */
    if (thread_clone && !parent->credentials->thread_keyring) {
        child->credentials = kernel_credentials_get(parent->credentials);
    } else {
        struct kernel_credentials *candidate = kernel_credentials_clone(parent->files.heap,
            parent->credentials);
        if (candidate) kernel_keys_credentials_fork(candidate, thread_clone);
        child->credentials = candidate;
    }
    if (!child->credentials)
        return finish_clone_failure(child,-KERNEL_ENOMEM,linux_result,KERNEL_SCHEDULER_STATUS_OK);
    int admitted=process_clone_admit(child,parent,flags);
    if(admitted)return finish_clone_failure(child,admitted,linux_result,KERNEL_SCHEDULER_STATUS_OK);
    child->limits=thread_clone?kernel_resource_limits_get(parent->limits):
        kernel_resource_limits_create(parent->limits->heap,parent->limits);
    if(!child->limits)
        return finish_clone_failure(child,-KERNEL_ENOMEM,linux_result,KERNEL_SCHEDULER_STATUS_OK);
    int namespace_result=clone_namespaces(child,parent,flags);
    if(namespace_result)
        return finish_clone_failure(child,namespace_result,linux_result,KERNEL_SCHEDULER_STATUS_OK);
    int sem_result=kernel_sem_clone(parent,child,(flags & UINT64_C(0x40000))!=0);
    if (sem_result)
        return finish_clone_failure(child,sem_result,linux_result,KERNEL_SCHEDULER_STATUS_OK);
    int io_result=kernel_ioprio_clone(parent,child,(flags & UINT64_C(0x80000000))!=0);
    if(io_result)
        return finish_clone_failure(child,io_result,linux_result,KERNEL_SCHEDULER_STATUS_OK);
    child->personality=parent->personality;
    child->arch.user_mode = 1U;
    child->state = KERNEL_THREAD_STATE_EXITED;
    child->completion.kind = KERNEL_THREAD_KIND_USER;
    child->completion.reason = KERNEL_THREAD_EXIT_USER_FAULT;

    /* The MM owner follows CLONE_VM, independently of thread-group identity. */
    if (flags & LINUX_CLONE_VM) {
        mm_status = kernel_mm_acquire(&child->mm, &parent->mm);
    } else {
        mm_status = kernel_mm_fork(&child->mm, &parent->mm);
    }
    if (mm_status != KERNEL_MM_STATUS_OK) {
        return finish_clone_failure(
            child,
            mm_status == KERNEL_MM_STATUS_NO_MEMORY
                ? -KERNEL_ENOMEM : -KERNEL_EAGAIN,
            linux_result,
            mm_status == KERNEL_MM_STATUS_NO_MEMORY ||
                    mm_status == KERNEL_MM_STATUS_CLEANUP_REQUIRED
                ? KERNEL_SCHEDULER_STATUS_OK
                : (mm_status == KERNEL_MM_STATUS_PAGE_ACCESS
                       ? KERNEL_SCHEDULER_STATUS_PAGE_ACCESS
                       : (mm_status == KERNEL_MM_STATUS_ADDRESS_SPACE
                              ? KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE
                              : KERNEL_SCHEDULER_STATUS_INVALID_STATE)));
    }
    /* dumpable 随实际共享的映像变化，任务退出释放 MM 后仍保留该引用。 */
    child->exec_security=(flags & LINUX_CLONE_VM)?kernel_exec_security_get(parent->exec_security):
        kernel_exec_security_create(parent->files.heap,kernel_exec_security_dumpable(parent->exec_security));
    if(!child->exec_security)
        return finish_clone_failure(child,-KERNEL_ENOMEM,linux_result,KERNEL_SCHEDULER_STATUS_OK);
    if (parent->files.state != KERNEL_FILES_EMPTY) {
        files_status = (flags & LINUX_CLONE_FILES)
            ? kernel_files_acquire(&child->files, &parent->files)
            : kernel_files_fork(&child->files, &parent->files);
        if (files_status != KERNEL_FILES_STATUS_OK) {
            return finish_clone_failure(
                child,
                files_status == KERNEL_FILES_STATUS_NO_MEMORY
                    ? -KERNEL_ENOMEM : -KERNEL_EAGAIN,
                linux_result,
                files_status == KERNEL_FILES_STATUS_NO_MEMORY ||
                        files_status ==
                            KERNEL_FILES_STATUS_CLEANUP_REQUIRED
                    ? KERNEL_SCHEDULER_STATUS_OK
                    : KERNEL_SCHEDULER_STATUS_INVALID_STATE);
        }
        if (flags & LINUX_CLONE_NEWNS) {
            KERNEL_SERVICE_SCOPE(mount_service);
            int error=kernel_fs_context_fork_mount_namespace(&child->fs,&parent->fs);
            if (error) return finish_clone_failure(child,error,linux_result,KERNEL_SCHEDULER_STATUS_OK);
            fs_status=KERNEL_FS_CONTEXT_STATUS_OK;
        } else fs_status = (flags & LINUX_CLONE_FS) != 0U
            ? kernel_fs_context_acquire(&child->fs, &parent->fs)
            : kernel_fs_context_fork(&child->fs, &parent->fs);
        if (fs_status != KERNEL_FS_CONTEXT_STATUS_OK) {
            return finish_clone_failure(
                child,
                fs_status == KERNEL_FS_CONTEXT_STATUS_NO_MEMORY
                    ? -KERNEL_ENOMEM : -KERNEL_EAGAIN,
                linux_result,
                fs_status == KERNEL_FS_CONTEXT_STATUS_NO_MEMORY ||
                        fs_status == KERNEL_FS_CONTEXT_STATUS_BUSY ||
                        fs_status ==
                            KERNEL_FS_CONTEXT_STATUS_CLEANUP_REQUIRED
                    ? KERNEL_SCHEDULER_STATUS_OK
                    : KERNEL_SCHEDULER_STATUS_INVALID_STATE);
        }
    }
    int may_set_tid = 0;
    if (requested_count) {
        const struct kernel_credentials *cred = kernel_credentials_current();
        may_set_tid = kernel_credentials_capable(cred, KERNEL_CAP_SYS_ADMIN) ||
                      kernel_credentials_capable(cred, KERNEL_CAP_CHECKPOINT_RESTORE);
    }
    pid_status = (flags & LINUX_CLONE_NEWPID)
        ? process_identity_create_new_namespace(child, parent, requested_tids, requested_count, may_set_tid)
        : process_identity_create_numbers(child, parent, thread_clone, requested_tids, requested_count, may_set_tid);
    if (pid_status != KERNEL_PID_STATUS_OK) {
        return finish_clone_failure(
            child,
            pid_status == KERNEL_PID_STATUS_PERMISSION ? -KERNEL_EPERM :
            pid_status == KERNEL_PID_STATUS_EXISTS ? -KERNEL_EEXIST :
            pid_status == KERNEL_PID_STATUS_DEPTH_LIMIT ? -KERNEL_ENOSPC :
            pid_status == KERNEL_PID_STATUS_INVALID ? -KERNEL_EINVAL :
            pid_status == KERNEL_PID_STATUS_EXHAUSTED ? -KERNEL_EAGAIN : -KERNEL_ENOMEM,
            linux_result,
            (pid_status == KERNEL_PID_STATUS_NO_MEMORY || pid_status == KERNEL_PID_STATUS_PERMISSION ||
             pid_status == KERNEL_PID_STATUS_EXHAUSTED || pid_status == KERNEL_PID_STATUS_EXISTS ||
             pid_status == KERNEL_PID_STATUS_INVALID || pid_status == KERNEL_PID_STATUS_DEPTH_LIMIT ||
             pid_status == KERNEL_PID_STATUS_NAMESPACE_DEAD)
                ? KERNEL_SCHEDULER_STATUS_OK
                : KERNEL_SCHEDULER_STATUS_INVALID_STATE);
    }
    tid = child->tid;
    /* The child identity is private; the executing parent's active namespace
     * cannot change. Completion bookkeeping below still uses root tid. */
    kernel_pid_t parent_visible_tid = kernel_pid_number_in(
        process_identity(child, KERNEL_PID_TID), process_pid_active(parent));
    if (!parent_visible_tid) __builtin_trap();
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
#if BOAROS_USER_SMP
        child->cpu_mask = parent->cpu_mask;
#endif
        struct kernel_sched_policy inherited;
        scheduler_policy_snapshot_locked(parent, &inherited);
        kernel_sched_policy_fork(&child->scheduling,&inherited);
        child->timer_slack_ns = child->default_timer_slack_ns =
            kernel_task_timerslack_get(parent);
    }
    child->proc_start_ticks = kernel_tick_count();
    if (!thread_clone) child->limits->acct_start_ns = kernel_time_raw_ns();
    child->group_leader = child;
    child->group_members = 1U;
    child->fork_no_exec = 1U;
    child->exit_signal = (uint8_t)(flags & UINT64_C(0xff));
    process_group_initialize(child);
    child->publish_completion = 0U;
    child->vfork_child = vfork;
    child->vfork_parent = vfork ? parent : 0;
    child->completion.tid = tid;
    child->completion.tgid = tid;
    kernel_wait_queue_init(&child->child_exit_queue);
    kernel_wait_queue_init(&child->vfork_done_queue);
    signal_status = (flags & LINUX_CLONE_SIGHAND) ? kernel_signal_share(child, parent)
                                 : kernel_signal_fork(child, parent,(flags & LINUX_CLONE_CLEAR_SIGHAND)!=0);
    if (signal_status != KERNEL_SIGNAL_STATUS_OK) {
        return finish_clone_failure(
            child,
            signal_status == KERNEL_SIGNAL_STATUS_NO_MEMORY
                ? -KERNEL_ENOMEM : -KERNEL_EAGAIN,
            linux_result,
            signal_status == KERNEL_SIGNAL_STATUS_NO_MEMORY
                ? KERNEL_SCHEDULER_STATUS_OK
                : KERNEL_SCHEDULER_STATUS_INVALID_STATE);
    }
    child->signal_stack = parent->signal_stack;
    if ((flags & LINUX_CLONE_VM) && !(flags & LINUX_CLONE_VFORK))
        kernel_signal_stack_reset(child);
    if (flags & LINUX_CLONE_VM) {
        /* The child acquired the running parent's address space, including its root. */
        child_context = arch_thread_mm(&parent->arch);
    } else {
        mm_status = kernel_mm_context(&child->mm, &child_context);
        if (mm_status != KERNEL_MM_STATUS_OK) {
            return finish_clone_failure(
                child, -KERNEL_EAGAIN, linux_result,
                KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE);
        }
    }
    arch_thread_set_mm(&child->arch, child_context);
    if ((flags & LINUX_CLONE_CHILD_CLEARTID) != 0U)
        child->clear_tid_address = child_tid;
    if (flags & UINT64_C(0x1000)) {
        int error=kernel_pidfd_prepare_clone(parent,process_identity(child,KERNEL_PID_TID),thread_clone,&pidfd);
        if (error) return finish_clone_failure(child,error,linux_result,KERNEL_SCHEDULER_STATUS_OK);
    }

    /* Controlling TTY inheritance shares the lifecycle executor with setsid
     * and hangup. Thread stack setup has its own short protection. */
    KERNEL_SERVICE_SCOPE_IF(service, !thread_clone || vfork);
    status = map_task_stack(child);
    if (status != KERNEL_SCHEDULER_STATUS_OK)
        return finish_clone_failure(child, -KERNEL_ENOMEM, linux_result,
            status == KERNEL_SCHEDULER_STATUS_NO_MEMORY ? KERNEL_SCHEDULER_STATUS_OK : status);
    status = arch_process_prepare_clone(child, parent, parent_frame,
                                         child_stack,
                                         (flags & LINUX_CLONE_SETTLS) != 0U,
                                         tls);
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return finish_clone_failure(child,
            status == KERNEL_SCHEDULER_STATUS_NO_MEMORY ? -KERNEL_ENOMEM : -KERNEL_EAGAIN,
            linux_result, status == KERNEL_SCHEDULER_STATUS_NO_MEMORY ?
                KERNEL_SCHEDULER_STATUS_OK : KERNEL_SCHEDULER_STATUS_INVALID_STATE);
    }

    if (pidfd.files) {
        size_t copied;
        if (kernel_copy_to_user(&parent->mm, pidfd_address, &pidfd.fd, sizeof(pidfd.fd), &copied) ||
            copied != sizeof(pidfd.fd))
            return finish_clone_failure(child, -KERNEL_EFAULT, linux_result, KERNEL_SCHEDULER_STATUS_OK);
    }

    if (!(flags & LINUX_CLONE_VM)) child->rseq = parent->rseq;
    if (flags & LINUX_CLONE_CHILD_SETTID) child->set_tid_address = child_tid;
    if (!thread_clone && kernel_task_controlling_tty(parent) &&
        kernel_task_tty_set(child, kernel_task_controlling_tty(parent))) __builtin_trap();
    kernel_mm_add_user(&child->mm);
#if BOAROS_COST_DIAGNOSTICS
    kernel_cost_inherit(&child->cost, &parent->cost);
#endif
    struct kernel_cgroup *destination __attribute__((cleanup(clone_cgroup_end))) = 0;
    if (cgroup_fd >= 0) {
        struct kernel_open_file_description *file = 0;
        int64_t error = 0;
        enum kernel_files_status pinned = kernel_files_pin(&parent->files,cgroup_fd,&file,&error);
        if (pinned != KERNEL_FILES_STATUS_OK) error = -KERNEL_EBADF;
        if (!error) error = kernel_cgroup_from_file(file,&destination);
        if (file && kernel_open_file_release(&file)) __builtin_trap();
        if (error) {
            (void)kernel_mm_remove_user(&child->mm);
            return finish_clone_failure(child,error,linux_result,KERNEL_SCHEDULER_STATUS_OK);
        }
    }
    struct kernel_pid *trace_child __attribute__((cleanup(kernel_task_identity_end))) = NULL;
    unsigned trace_event=0;
    int rejected;
    for (;;) {
        /* A CLONE_FILES child can inspect the descriptor as soon as queued.
         * File-table rank precedes the scheduler rank. */
        struct kernel_raw_guard files_guard __attribute__((cleanup(kernel_raw_scope_release)))={0};
        if(pidfd.files) kernel_raw_lock_acquire(kernel_pidfd_clone_files_lock(&pidfd),&files_guard);
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        struct kernel_task *leader = parent->group_leader;
        rejected = kernel_reboot_requested_locked() || parent->retirement || parent->terminate_requested ||
                   leader->group_exiting || leader->group_execing;
        if (rejected) { rejected = -KERNEL_EAGAIN; break; }
        /* Namespace shutdown is a recoverable clone rejection, before ptrace,
         * cgroup, pidfd, child links or any published identity is changed. */
        rejected = process_identity_admit_locked(child);
        if (rejected) break;
        if (destination) {
            struct kernel_cgroup *source = parent->cgroup ? parent->cgroup : kernel_cgroup_root();
            rejected = !destination->linked ? -KERNEL_ENODEV :
                kernel_cgroup_permission(source,destination,kernel_credentials_current());
            if (!rejected && thread_clone && source != destination) rejected = -KERNEL_EOPNOTSUPP;
            if (rejected) break;
        }
        /* Attach may race the fallible allocations above. Choose the child
         * trace link at publication, outside-lock allocation only if needed. */
        int event = ptrace_clone_locked(parent, child, flags);
        if (event == -KERNEL_ENOMEM) {
            kernel_raw_lock_release(&guard);
            if (files_guard.lock) kernel_raw_lock_release(&files_guard);
            if (ptrace_clone_prepare(child)) {
                (void)kernel_mm_remove_user(&child->mm);
                return finish_clone_failure(child, -KERNEL_ENOMEM, linux_result, KERNEL_SCHEDULER_STATUS_OK);
            }
            continue;
        }
        /* Install group membership before making the child runnable. */
        struct kernel_cgroup *chosen = destination ? destination : parent->cgroup;
        if (chosen && chosen != kernel_cgroup_root()) {
            child->cgroup = kernel_cgroup_get(chosen);
            chosen->members++;
        }
        /* Publish the inherited policy together with the child so a
         * concurrent TSYNC cannot miss an unpublished descendant. */
        child->no_new_privs=parent->no_new_privs;
        kernel_seccomp_clone(child,parent);
        trace_event = (unsigned)event;
        if(trace_event || vfork) {
            trace_child=process_identity(child,KERNEL_PID_TID);
            kernel_pid_get(trace_child);
        }
        memcpy(child->comm, parent->comm, sizeof(child->comm));
        if(pidfd.files) kernel_pidfd_install_prepared_locked(&pidfd);
        if (thread_clone) {
            child->group_leader = leader;
            child->group_members = 0;
            child->group_next = leader;
            child->group_previous = leader->group_previous;
            leader->group_previous->group_next = child;
            leader->group_previous = child;
            leader->group_members++;
            if (++leader->group_epoch == 0) __builtin_trap();
            child->completion.tgid = leader->tid;
#if BOAROS_USER_SMP
            if (kernel_signal_has_pending(child)) scheduler_signal_request_locked(child);
#endif
        } else {
            child_creator_change_locked(child, process_identity(parent, KERNEL_PID_TID));
            child_append_locked(parent->group_leader, child);
        }
        if (vfork) {
            parent->vfork_waiting = 1;
            parent->vfork_wait_child = child;
        }
        if(!thread_clone && child->limits->values[KERNEL_RLIMIT_CPU].current!=KERNEL_RLIM_INFINITY)
            scheduler_cpu_timer_update_locked(child,0);
        if (flags & LINUX_CLONE_PARENT_SETTID) {
            /* The user store may fault or sleep. PID, pidfd and group links
             * commit together; only this caller can first enqueue a NEW child. */
            child->state = KERNEL_THREAD_STATE_NEW;
            scheduler_task_publish_locked(child);
        } else {
            child->state = KERNEL_THREAD_STATE_READY;
            ready_enqueue_locked(child, 0);
        }
        break;
    }
    if (rejected) {
        (void)kernel_mm_remove_user(&child->mm);
        return finish_clone_failure(child, rejected, linux_result, KERNEL_SCHEDULER_STATUS_OK);
    }
    if (flags & LINUX_CLONE_PARENT_SETTID) {
        size_t copied;
        (void)kernel_copy_to_user(&parent->mm, parent_tid, &parent_visible_tid, sizeof(parent_visible_tid), &copied);
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        if (child->state != KERNEL_THREAD_STATE_NEW) __builtin_trap();
        child->state = KERNEL_THREAD_STATE_READY;
#if BOAROS_USER_SMP
        if (kernel_signal_has_pending(child)) scheduler_signal_request_locked(child);
#endif
        ready_enqueue_locked(child, 0);
    }
    *linux_result = parent_visible_tid;
    if (trace_event) ptrace_event_pid(parent,trace_event,trace_child);

    if (vfork) {
        /* Linux suspends the vfork parent inside the syscall until the
         * child execs or exits; the child wakes vfork_done_queue. */
        while (vfork_waiting(parent)) {
            status = KERNEL_WAIT_RECHECK(&parent->vfork_done_queue,
                                                    0U, 0, &wake_reason,
                vfork_waiting(parent));
            if (status != KERNEL_SCHEDULER_STATUS_OK) {
                return status;
            }
        }
        ptrace_event_pid(parent,TRACE_VFORK_DONE,trace_child);
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}

static struct kernel_pid *wait_target_get(struct kernel_task *parent, int32_t which,
                                           int64_t upid)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    if (which == LINUX_P_ALL) return 0;
    /* 一次调用持有同一个identity；setpgid和数字重用都不改变等待目标。 */
    struct kernel_pid *identity = which == LINUX_P_PGID && !upid
        ? process_identity(parent, KERNEL_PID_PGID)
        : kernel_pid_find(process_pid_active(parent), (kernel_pid_t)upid);
    if (identity) kernel_pid_get(identity);
    return identity;
}

static int wait_child_valid(const struct kernel_task *parent, const struct kernel_task *child)
{
    return child != parent && child->magic == KERNEL_THREAD_MAGIC &&
        !child->idle && child->arch.user_mode == 1 && child->tid_owned && child->tid > 0 &&
        !child->publish_completion &&
        child->state >= KERNEL_THREAD_STATE_READY && child->state <= KERNEL_THREAD_STATE_NEW;
}

static int wait_claim_child_locked(struct kernel_task *parent, struct kernel_task *child,
    uint32_t options, struct kernel_task **selected, uint32_t *event, int *matching)
{
    if (((options & LINUX___WNOTHREAD) && child->child_creator != process_identity(parent, KERNEL_PID_TID)) ||
        (!(options & LINUX___WALL) && ((child->exit_signal != 17U) != !!(options & LINUX___WCLONE))))
        return 0;
    uint32_t kind;
    if (child->state == KERNEL_THREAD_STATE_ZOMBIE) {
        if (!(options & LINUX_WEXITED)) return 0;
        *matching = 1;
        if (child->reaping || kernel_ptrace_tracer_tid_locked(child)) return 0;
        kind = 1;
    } else {
        *matching = 1;
        /* A stop in this parent's trace list is the sole stop report. */
        int traced_here = child->ptrace && child->ptrace->tracer &&
            child->ptrace->tracer->group_leader == child->parent;
        kind = !traced_here && child->group_stopped &&
            (options & (LINUX_WUNTRACED | LINUX_WSTOPPED)) && !child->stop_notified ? 2 :
            !child->group_stopped && (options & LINUX_WCONTINUED) && child->continue_notified ? 3 : 0;
        if (!kind || child->reaping) return 0;
    }
    child->reaping = 1;
    scheduler_task_get_locked(child);
    *selected = child;
    *event = kind;
    return 1;
}

static enum kernel_scheduler_status retire_waited_zombie(
    struct kernel_task *parent,
    struct kernel_task *child,
    const struct kernel_task_usage *usage)
{
    struct kernel_pid *exit_identity __attribute__((cleanup(kernel_pidfd_notification_end))) =
        kernel_pidfd_exit_identity(child);
    kernel_pid_t pid = child->tid;

    if (child->state != KERNEL_THREAD_STATE_ZOMBIE ||
        child->stack_physical_address != KERNEL_THREAD_NO_PAGE ||
        child->stack_low != 0U || child->stack_high != 0U ||
        child->parent != parent->group_leader || child->tid_owned != 1U || pid <= 0 ||
        child->mm.state != KERNEL_MM_RELEASED ||
        (child->files.state != KERNEL_FILES_EMPTY &&
         child->files.state != KERNEL_FILES_RELEASED) ||
        (child->fs.state != KERNEL_FS_CONTEXT_EMPTY &&
         child->fs.state != KERNEL_FS_CONTEXT_RELEASED) ||
        child->exec_transaction != 0) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    /* Selected zombie has a unique wait claim and a real storage borrow.
     * Scratch disposal stays outside rank40, before the retirement transaction. */
    kernel_task_release_io_scratch(child);
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        scheduler_usage_add(&parent->group_leader->limits->child_usage, usage);
        child_remove_locked(parent->group_leader, child);
        if (scheduler.init_task == child) {
            scheduler.init_task = 0;
        }
        child->group_leader = 0;
        child->group_members = 0U;
        child->publish_completion = 0U;
        struct kernel_wait_cursor unused;
        if (scheduler_forget_task_locked(child, &unused)) __builtin_trap();
        if (!child->storage_borrows) __builtin_trap();
        scheduler_task_retire_locked(child);
    }
    process_identity_collect();
    (void)kernel_wait_queue_wake_all(&parent->group_leader->child_exit_queue);
    return KERNEL_SCHEDULER_STATUS_OK;
}

static enum kernel_scheduler_status reap_waited_child(
    struct kernel_task *parent,
    struct kernel_task *child,
    uint64_t status_address,
    uint64_t rusage_address,
    int64_t *linux_result)
{
    kernel_pid_t pid = kernel_pid_number_in(process_identity(child, KERNEL_PID_TID),
                                             process_pid_active(parent));
    uint32_t wait_status = child->wait_status;
    struct kernel_task_usage usage = {0};
    scheduler_usage_snapshot_mm(child, -2, &usage);
    struct kernel_linux_rusage rusage;
    scheduler_usage_format(&usage, &rusage);
    enum kernel_scheduler_status status = retire_waited_zombie(parent, child, &usage);
    if (status != KERNEL_SCHEDULER_STATUS_OK) return status;

    size_t copied = 0U;
    enum kernel_uaccess_status access_status = KERNEL_UACCESS_STATUS_OK;
    if (status_address != 0U) {
        access_status = kernel_copy_to_user(&parent->mm,
                                            status_address,
                                            &wait_status,
                                            sizeof(wait_status),
                                            &copied);
        if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
            *linux_result = -KERNEL_EFAULT;
            return KERNEL_SCHEDULER_STATUS_OK;
        }
        if (access_status != KERNEL_UACCESS_STATUS_OK ||
            copied != sizeof(wait_status)) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
    }
    if (rusage_address != 0U) {
        access_status = kernel_copy_to_user(&parent->mm,
                                            rusage_address,
                                            &rusage,
                                            sizeof(rusage),
                                            &copied);
        if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
            *linux_result = -KERNEL_EFAULT;
            return KERNEL_SCHEDULER_STATUS_OK;
        }
        if (access_status != KERNEL_UACCESS_STATUS_OK ||
            copied != sizeof(rusage)) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
    }
    *linux_result = pid;
    return KERNEL_SCHEDULER_STATUS_OK;
}

static enum kernel_scheduler_status report_wait_event(
    struct kernel_task *parent,
    struct kernel_task *child,
    uint32_t wait_status,
    kernel_pid_t visible_pid,
    uint64_t status_address,
    uint64_t rusage_address,
    int64_t *linux_result)
{
    size_t copied = 0U;
    enum kernel_uaccess_status access_status;

    if (status_address != 0U) {
        access_status = kernel_copy_to_user(&parent->mm,
                                            status_address,
                                            &wait_status,
                                            sizeof(wait_status),
                                            &copied);
        if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
            *linux_result = -KERNEL_EFAULT;
            return KERNEL_SCHEDULER_STATUS_OK;
        }
        if (access_status != KERNEL_UACCESS_STATUS_OK ||
            copied != sizeof(wait_status)) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
    }
    if (rusage_address != 0U) {
        struct kernel_task_usage usage = {0};
        struct kernel_linux_rusage rusage;
        scheduler_usage_snapshot_mm(child, -2, &usage);
        scheduler_usage_format(&usage, &rusage);
        access_status = kernel_copy_to_user(&parent->mm,
                                            rusage_address,
                                            &rusage,
                                            sizeof(rusage),
                                            &copied);
        if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
            *linux_result = -KERNEL_EFAULT;
            return KERNEL_SCHEDULER_STATUS_OK;
        }
        if (access_status != KERNEL_UACCESS_STATUS_OK ||
            copied != sizeof(rusage)) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
    }
    *linux_result = visible_pid;
    return KERNEL_SCHEDULER_STATUS_OK;
}

static enum kernel_scheduler_status report_waitid_event(
    struct kernel_task *parent,
    int32_t signo,
    int32_t cause,
    int32_t pid,
    uint32_t uid,
    int32_t status_code,
    uint64_t rusage_address,
    uint64_t infop_address,
    const struct kernel_linux_rusage *usage,
    int64_t *linux_result)
{
    size_t copied = 0U;
    enum kernel_uaccess_status access_status;

    if (usage && rusage_address != 0U) {
        copied = 0U;
        access_status = kernel_copy_to_user(&parent->mm,
                                            rusage_address,
                                            usage,
                                            sizeof(*usage),
                                            &copied);
        if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
            *linux_result = -KERNEL_EFAULT;
            return KERNEL_SCHEDULER_STATUS_OK;
        }
        if (access_status != KERNEL_UACCESS_STATUS_OK ||
            copied != sizeof(*usage)) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
    }

    if (infop_address != 0U) {
        if (kernel_user_range_check(infop_address, 128U) != KERNEL_UACCESS_STATUS_OK) {
            *linux_result = -KERNEL_EFAULT;
            return KERNEL_SCHEDULER_STATUS_OK;
        }
        int32_t head[3] = {signo, 0, cause};
        copied = 0U;
        access_status = kernel_copy_to_user(&parent->mm,
                                            infop_address,
                                            head,
                                            sizeof(head),
                                            &copied);
        if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
            *linux_result = -KERNEL_EFAULT;
            return KERNEL_SCHEDULER_STATUS_OK;
        }
        if (access_status != KERNEL_UACCESS_STATUS_OK ||
            copied != sizeof(head)) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }

        int32_t body[3] = {pid, (int32_t)uid, status_code};
        copied = 0U;
        access_status = kernel_copy_to_user(&parent->mm,
                                            infop_address + 16U,
                                            body,
                                            sizeof(body),
                                            &copied);
        if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
            *linux_result = -KERNEL_EFAULT;
            return KERNEL_SCHEDULER_STATUS_OK;
        }
        if (access_status != KERNEL_UACCESS_STATUS_OK ||
            copied != sizeof(body)) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
    }

    return KERNEL_SCHEDULER_STATUS_OK;
}

static enum kernel_scheduler_status select_wait_child(struct kernel_task *parent,
    int32_t which, const struct kernel_pid *target, uint32_t options, struct kernel_task **selected, uint32_t *event, int *matching)
{
    *selected = 0;
    *event = 0;
    *matching = 0;
    if (which == LINUX_P_PID || which == LINUX_P_PIDFD) {
        /* The caller holds the identity even if its member has exited or
         * changed on exec. Read the current member and claim under one lock. */
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        if (!parent->group_leader) return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        const struct kernel_pid_member *member = target ? target->members[KERNEL_PID_TID] : 0;
        struct kernel_task *child = member ? member->task : 0;
        if (!child || ptrace_select_target_locked(parent,child,options,selected,event,matching) ||
            child->parent != parent->group_leader) return KERNEL_SCHEDULER_STATUS_OK;
        if (!wait_child_valid(parent,child)) return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        (void)wait_claim_child_locked(parent,child,options,selected,event,matching);
        return KERNEL_SCHEDULER_STATUS_OK;
    }
    if (ptrace_select(parent,which,target,options,selected,event,matching))
        return KERNEL_SCHEDULER_STATUS_OK;
    int trace_matching=*matching;
    struct kernel_task *leader = parent->group_leader;
    SCHEDULER_TASK_BORROW(owner, 0);
    uint64_t epoch = 0;
    uintptr_t previous = 0;
    uint32_t count = 0;
    int restart = 1;
    for (;;) {
        struct kernel_task *next_owner = 0;
        int done = 0;
        enum kernel_scheduler_status status = KERNEL_SCHEDULER_STATUS_OK;
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            if (!leader) return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
            if (leader->children_epoch != epoch) restart = 1;
            if (restart) {
                if (!leader || leader->magic != KERNEL_THREAD_MAGIC || !leader->tid_owned ||
                    ((leader->first_child == 0) != (leader->last_child == 0)) ||
                    (leader->first_child && leader->first_child->previous_sibling) ||
                    (leader->last_child && leader->last_child->next_sibling))
                    return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
                epoch = leader->children_epoch;
                count = previous = 0;
                *matching = trace_matching;
            }
            struct kernel_task *child = restart ? leader->first_child : owner;
            restart = 0;
            for (unsigned n = 0; child && n < 16; n++) {
                if (++count > KERNEL_PID_LIMIT || child->parent != leader || !wait_child_valid(parent,child) ||
                    (uintptr_t)child->previous_sibling != previous) {
                    status = KERNEL_SCHEDULER_STATUS_INVALID_STATE;
                    break;
                }
                if ((which == LINUX_P_ALL || (target && process_identity(child,KERNEL_PID_PGID)==target)) &&
                    wait_claim_child_locked(parent,child,options,selected,event,matching))
                    return KERNEL_SCHEDULER_STATUS_OK;
                previous = (uintptr_t)child;
                child = child->next_sibling;
            }
            if (status == KERNEL_SCHEDULER_STATUS_OK) {
                if (!child) {
                    done = 1;
                    if (previous != (uintptr_t)leader->last_child) status = KERNEL_SCHEDULER_STATUS_INVALID_STATE;
                } else {
                    scheduler_task_get_locked(child);
                    next_owner = child;
                }
            }
        }
        if (owner) scheduler_task_put(owner);
        owner = next_owner;
        if (status != KERNEL_SCHEDULER_STATUS_OK || done) return status;
    }
}

static void wait_claim_end(struct kernel_task **task)
{
    if (!*task) return;
    scheduler_task_wait_claim_release(*task);
    ptrace_wait_claim_done(*task);
}

/* 返回的child同时带有storage borrow和wait claim，由调用者保持到输出完成。 */
static enum kernel_scheduler_status wait_for_child(struct kernel_task *parent,
    int32_t which, const struct kernel_pid *target, uint32_t options,
    struct kernel_task **child, uint32_t *event, int64_t *linux_result)
{
    enum kernel_scheduler_status status = validate_current();
    if (status != KERNEL_SCHEDULER_STATUS_OK) return status;
    for (;;) {
        struct kernel_wait_token token = {0};
        int matching;
        status = select_wait_child(parent, which, target, options, child, event, &matching);
        if (status == KERNEL_SCHEDULER_STATUS_OK && !*child && matching && !(options & LINUX_WNOHANG)) {
            status = kernel_wait_prepare(&parent->group_leader->child_exit_queue, 0, 1, &token);
            if (status != KERNEL_SCHEDULER_STATUS_OK) return status;
            status = select_wait_child(parent, which, target, options, child, event, &matching);
        }
        if (status != KERNEL_SCHEDULER_STATUS_OK || *child || !matching || (options & LINUX_WNOHANG)) {
            enum kernel_scheduler_status finished = token.task ? kernel_wait_finish(&token) : KERNEL_SCHEDULER_STATUS_OK;
            if (status != KERNEL_SCHEDULER_STATUS_OK) return status;
            if (finished != KERNEL_SCHEDULER_STATUS_OK) return finished;
            *linux_result = *child || matching ? 0 : -KERNEL_ECHILD;
            return KERNEL_SCHEDULER_STATUS_OK;
        }
        enum kernel_wait_wake_reason reason;
        status = kernel_wait_park(&token, &reason);
        enum kernel_scheduler_status finished = kernel_wait_finish(&token);
        if (status != KERNEL_SCHEDULER_STATUS_OK) return status;
        if (finished != KERNEL_SCHEDULER_STATUS_OK) return finished;
        if (reason == KERNEL_WAIT_SIGNALLED) {
            *linux_result = -KERNEL_ERESTARTSYS;
            return KERNEL_SCHEDULER_STATUS_OK;
        }
    }
}

/* The tracer used a separate wait index. Release to the parent that owns the
 * child list, or resume ordinary retirement of an unparented thread. */
void process_release_traced_zombie(struct kernel_task *child)
{
    SCHEDULER_TASK_BORROW(parent,0);
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        parent=child->parent;
        if (parent) scheduler_task_get_locked(parent);
    }
    if (parent) kernel_signal_notify_child_exit(child);
    int autoreap=parent && kernel_signal_child_autoreap(parent,child->exit_signal);
    int cleanup=0;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        if (child->state!=KERNEL_THREAD_STATE_ZOMBIE) return;
        if (autoreap && child->parent==parent) child_remove_locked(parent,child);
        if (!child->parent) {
            child->state=KERNEL_THREAD_STATE_EXITED;
            exited_append_locked(child);
            cleanup=1;
        }
    }
    if (cleanup) (void)kernel_wait_queue_wake_all(&scheduler.cleanup_queue);
}

static int finish_traced_wait(struct kernel_task *parent,struct kernel_task *child)
{
    ptrace_consume_final(child);
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        if (child->parent==parent->group_leader) return 1;
    }
    process_release_traced_zombie(child);
    return 0;
}

enum kernel_scheduler_status kernel_scheduler_wait4_current(int64_t pid,
    uint64_t status_address, uint32_t options, uint64_t rusage_address, int64_t *linux_result)
{
    KERNEL_SERVICE_SCOPE(service);
    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED) return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    if (!linux_result || arch_interrupt_is_enabled()) return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    if (options & ~LINUX_WAIT4_SUPPORTED_OPTIONS) { *linux_result = -KERNEL_EINVAL; return KERNEL_SCHEDULER_STATUS_OK; }
    if (pid == INT32_MIN) { *linux_result = -KERNEL_ESRCH; return KERNEL_SCHEDULER_STATUS_OK; }
    struct kernel_task *parent = kernel_cpu_current()->current;
    if (parent == kernel_cpu_current()->idle || parent->arch.user_mode != 1 || !parent->tid_owned)
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    int32_t which = pid == -1 ? LINUX_P_ALL : pid > 0 ? LINUX_P_PID : LINUX_P_PGID;
    int64_t upid = pid < -1 ? -pid : pid;
    struct kernel_pid *target __attribute__((cleanup(kernel_task_identity_end))) =
        wait_target_get(parent, which, upid);
    SCHEDULER_TASK_BORROW(child, 0);
    struct kernel_task *claim __attribute__((cleanup(wait_claim_end))) = 0;
    uint32_t event;
    enum kernel_scheduler_status status = wait_for_child(parent, which, target,
        options | LINUX_WEXITED | UINT32_C(0x10000000), &child, &event, linux_result);
    claim = child;
    if (status != KERNEL_SCHEDULER_STATUS_OK || !child) return status;
    if (event==TRACE_WAIT_FINAL && finish_traced_wait(parent,child)) event=1;
    if (event == 1) return reap_waited_child(parent, child, status_address, rusage_address, linux_result);
    uint32_t reported;
    kernel_pid_t visible_pid;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        /* Capture identity with the event before user copies can sleep. */
        visible_pid = kernel_pid_number_in(process_identity(child, KERNEL_PID_TID),
                                           process_pid_active(parent));
        if (event==TRACE_WAIT_STOP) reported=ptrace_wait_status_locked(child,1);
        else reported = event == 2 || event==TRACE_WAIT_FINAL ? child->wait_status : LINUX_WAIT_CONTINUED;
        if (event == 2) child->stop_notified = 1;
        else if (event==3) child->continue_notified = 0;
    }
    return report_wait_event(parent, child, reported, visible_pid, status_address, rusage_address, linux_result);
}

enum kernel_scheduler_status kernel_scheduler_waitid_current(int32_t which,
    int64_t upid, uint64_t infop_address, uint32_t options,
    uint64_t rusage_address, int64_t *linux_result)
{
    KERNEL_SERVICE_SCOPE(service);
    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED) return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    if (!linux_result || arch_interrupt_is_enabled()) return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    struct kernel_task *parent = kernel_cpu_current()->current;
    if (parent == kernel_cpu_current()->idle || parent->arch.user_mode != 1 || !parent->tid_owned)
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;

    *linux_result = 0;
    if ((options & ~LINUX_WAITID_SUPPORTED_OPTIONS) ||
        !(options & (LINUX_WEXITED | LINUX_WSTOPPED | LINUX_WCONTINUED)))
        *linux_result = -KERNEL_EINVAL;
    else switch (which) {
    case LINUX_P_ALL: break;
    case LINUX_P_PID:
        if (upid <= 0) *linux_result = -KERNEL_EINVAL;
        break;
    case LINUX_P_PGID:
        if (upid < 0) *linux_result = -KERNEL_EINVAL;
        break;
    case LINUX_P_PIDFD:
        if(upid<0)*linux_result=-KERNEL_EINVAL;
        break;
    default: *linux_result = -KERNEL_EINVAL; break;
    }

    struct kernel_pid *target __attribute__((cleanup(kernel_task_identity_end))) = 0;
    SCHEDULER_TASK_BORROW(child, 0);
    struct kernel_task *claim __attribute__((cleanup(wait_claim_end))) = 0;
    uint32_t event = 0;
    int pidfd_nonblock=0;
    enum kernel_scheduler_status status;
    if (!*linux_result) {
        if(which==LINUX_P_PIDFD)
            *linux_result=kernel_pidfd_wait_target(&parent->files,(int32_t)upid,&target,&pidfd_nonblock);
        else target = wait_target_get(parent, which, upid);
        if(!*linux_result) {
            status = wait_for_child(parent, which, target,
                options|(pidfd_nonblock?LINUX_WNOHANG:0), &child, &event, linux_result);
            claim = child;
            if (status != KERNEL_SCHEDULER_STATUS_OK) return status;
            if(!child && !*linux_result && pidfd_nonblock && !(options&LINUX_WNOHANG))
                *linux_result=-KERNEL_EAGAIN;
        }
    }

    int32_t cause = 0, status_code = 0, child_pid = 0;
    uint32_t child_uid = 0;
    struct kernel_task_usage usage = {0};
    struct kernel_linux_rusage rusage;
    if (child) {
        scheduler_usage_snapshot_mm(child, -2, &usage);
        scheduler_usage_format(&usage, &rusage);
        uint32_t ws;
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            child_pid = kernel_pid_number_in(process_identity(child, KERNEL_PID_TID),
                                             process_pid_active(parent));
            child_uid = kernel_credentials_task_locked(child)->uid;
            ws = event==TRACE_WAIT_STOP ? ptrace_wait_status_locked(child,!(options&LINUX_WNOWAIT)) : child->wait_status;
            if (event == 2 && !(options & LINUX_WNOWAIT)) child->stop_notified = 1;
            if (event == 3 && !(options & LINUX_WNOWAIT)) child->continue_notified = 0;
        }
        if (event==TRACE_WAIT_FINAL && !(options&LINUX_WNOWAIT) && finish_traced_wait(parent,child)) event=1;
        if (event == 1 || event==TRACE_WAIT_FINAL) {
            cause = !(ws & 0x7fU) ? 1 /* CLD_EXITED */ : (ws & 0x80U) ? 3 /* CLD_DUMPED */ : 2 /* CLD_KILLED */;
            status_code = (int32_t)(cause == 1 ? (ws >> 8U) & 0xffU : ws & 0x7fU);
            if (event==1 && !(options & LINUX_WNOWAIT)) {
                status = retire_waited_zombie(parent, child, &usage);
                if (status != KERNEL_SCHEDULER_STATUS_OK) return status;
            }
        } else if (event==TRACE_WAIT_STOP) {
            cause=4; status_code=(int32_t)(ws>>8);
        } else if (event == 2) {
            cause = 5; /* CLD_STOPPED */
            status_code = (int32_t)((ws >> 8U) & 0xffU);
        } else {
            cause = 6; /* CLD_CONTINUED */
            status_code = 18; /* SIGCONT */
        }
    }
    /* Linux在EINVAL/ECHILD/被信号中断时也输出六个零字段；复制错误覆盖原errno。
     * 有事件时先复制rusage，再复制siginfo；已消费事件不因EFAULT回滚。 */
    return report_waitid_event(parent, child ? 17 : 0, cause,
        child_pid, child_uid, status_code, rusage_address, infop_address,
        child ? &rusage : 0, linux_result);
}


static enum kernel_scheduler_status cleanup_user_task_resources(
    struct kernel_task *thread);
static uint32_t user_wait_status(const struct kernel_thread_completion *completion);
static void reparent_children(struct kernel_task *departing);
static int process_pid_namespace_drain(struct kernel_task *leader);

int kernel_scheduler_can_sleep(void)
{
    return scheduler.initialized == KERNEL_SCHEDULER_INITIALIZED &&
           !kernel_cpu_current()->preempt_depth && !kernel_cpu_current()->raw_locks &&
           kernel_cpu_current()->current && kernel_cpu_current()->current != kernel_cpu_current()->idle;
}
void kernel_scheduler_register_cleanup(void)
{
    if (arch_interrupt_is_enabled() || !kernel_scheduler_can_sleep() ||
        kernel_cpu_current()->current->arch.user_mode) __builtin_trap();
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    if (scheduler.cleanup_task) __builtin_trap();
    scheduler.cleanup_task = kernel_cpu_current()->current;
}
void kernel_scheduler_notify_cleanup(void)
{ KERNEL_IRQ_SCOPE(notify_irq); (void)kernel_wait_queue_wake_all(&scheduler.cleanup_queue); }
static int cleanup_waiting(uint64_t retry_deadline)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    return retry_deadline ||
        ((!kernel_reboot_requested_locked() || scheduler.shutdown_waiting) &&
         !scheduler.shutdown_progress && !scheduler_cleanup_ready_locked());
}

void kernel_scheduler_wait_cleanup(uint64_t retry_deadline)
{
    if (arch_interrupt_is_enabled() || kernel_cpu_current()->current != scheduler.cleanup_task) __builtin_trap();
    if (cleanup_waiting(retry_deadline)) {
        enum kernel_wait_wake_reason reason;
        if (KERNEL_WAIT_RECHECK(&scheduler.cleanup_queue, retry_deadline, 1, &reason,
                (cleanup_waiting(retry_deadline))) != KERNEL_SCHEDULER_STATUS_OK)
            __builtin_trap();
    }
}

static void join_notifications_finish(struct kernel_thread_join *join)
{
    if (!join->waiters.initialized) return;
    for (;;) {
        {
            KERNEL_IRQ_LOCK_SCOPE(guard,&join->waiters.lock);
            if (!join->waiters.borrows) return;
        }
        /* The task link is gone, but a remote switch-finish cursor may still
         * unlock this stack/worker-owned queue after its last node release. */
        kernel_wait_backend_quiesce();
    }
}
void kernel_thread_join(struct kernel_thread_join *join)
{
    kernel_assert_service_context();
    KERNEL_IRQ_SCOPE(join_irq);
    if (!join) __builtin_trap();
    int empty;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        empty=!join->task;
    }
    if (empty) {join_notifications_finish(join);return;}
    for (;;) {
        struct kernel_wait_token token;
        enum kernel_wait_wake_reason reason;
        struct kernel_task *task = 0;
        if (kernel_wait_prepare(&join->waiters, 0, 0, &token) != KERNEL_SCHEDULER_STATUS_OK)
            __builtin_trap();
        int pending;
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            if (join->task == kernel_cpu_current()->current) __builtin_trap();
            enum kernel_scheduler_status claim = scheduler_join_claim_locked(join, &task);
            pending = claim == KERNEL_SCHEDULER_STATUS_BUSY;
            if (claim != KERNEL_SCHEDULER_STATUS_OK && claim != KERNEL_SCHEDULER_STATUS_EMPTY && !pending)
                __builtin_trap();
        }
        if (pending && kernel_wait_park(&token, &reason) != KERNEL_SCHEDULER_STATUS_OK)
            __builtin_trap();
        if (kernel_wait_finish(&token) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
        if (!pending) {
            if (task && release_task_storage(task, KERNEL_SCHEDULER_STATUS_OK) !=
                        KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
            join_notifications_finish(join);
            return;
        }
    }
}

int kernel_scheduler_reap_pending(void)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    return scheduler_cleanup_ready_locked();
}

int kernel_scheduler_stop_users(void)
{
    int pending = 0;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        /* 本次扫描重查真实owner。清掉旧进展后，末项释放若先于等待登记
         * 完成，仍可由登记前/后的条件复查观察到；唤醒本身不是持久事件。 */
        scheduler.shutdown_waiting = 1;
        scheduler.shutdown_progress = 0;
    }
    struct scheduler_task_cursor cursor;
    scheduler_task_cursor_begin(&cursor);
    while (cursor.task) {
        struct kernel_task *task = cursor.task;
        int terminate = 0;
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            if (task->accounted && task->arch.user_mode == 1) {
                pending = 1;
                terminate = task->group_leader == task &&
                    task->state != KERNEL_THREAD_STATE_EXITED && task->state != KERNEL_THREAD_STATE_ZOMBIE;
            }
        }
        if (terminate) process_group_request_exit(task, KERNEL_THREAD_EXIT_SIGNAL, 9, 0);
        scheduler_task_cursor_next(&cursor);
    }
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        pending |= scheduler.retired_pending != 0;
    }
    if (!pending) (void)kernel_stack_cache_reclaim(scheduler.allocator);
    return pending;
}

static void reap_claim_end(struct kernel_task **task)
{
    if (!*task) return;
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    scheduler_task_claim_release_locked(*task);
}

/* 这些句柄借用旧调用栈；先结束端点请求，再归还请求可能使用的暂存页。 */
static int cleanup_task_io_requests(struct kernel_task *thread)
{
    if (thread->remote_memory_request && kernel_remote_memory_abort(thread)) return -KERNEL_EIO;
    kernel_files_abort_flock(thread);
    if(thread->pi_request)kernel_futex_pi_abort(thread);
    kernel_futex_abort(thread);
    kernel_posix_cpu_sleep_abort(thread);
    kernel_mqueue_abort(thread);
    kernel_msg_abort(thread);
    kernel_sem_abort(thread);
    if (thread->epoll_wait_request) kernel_epoll_abort_wait(thread->epoll_wait_request);
    if (thread->socket_write_request) kernel_socket_abort_write(thread->socket_write_request);
    if (thread->socket_read_request) kernel_socket_abort_read(thread->socket_read_request);
    if (thread->socket_call) kernel_socket_call_abort(thread->socket_call);
    if (thread->tty_request) kernel_tty_abort_request(thread->tty_request);
    if (thread->fanotify_read) {
        struct kernel_fanotify_read *read = thread->fanotify_read;
        thread->fanotify_read = NULL;
        kernel_fanotify_read_abort(read);
    }
    if (thread->io_buffer) kernel_task_io_buffer_release(thread->io_buffer);
    if (thread->io_uring_call && kernel_io_uring_abort(thread)) return -KERNEL_EIO;
    return 0;
}

enum kernel_scheduler_status kernel_scheduler_reap_one(
    struct kernel_thread_completion *completion)
{
    kernel_assert_service_context();
    struct kernel_thread_completion result;
    struct kernel_task *thread;
    uint32_t publish;
    enum kernel_scheduler_status status;

    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED) {
        return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (completion == 0) {
        return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    }
    if (arch_interrupt_is_enabled()) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    if (scheduler_failure() != KERNEL_SCHEDULER_STATUS_OK) {
        return scheduler_failure();
    }
    status = validate_current();
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }
    if (kernel_cpu_current()->current != (scheduler.cleanup_task ? scheduler.cleanup_task : kernel_cpu_current()->idle)) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
#if BOAROS_LOCK_DEBUG
    status = validate_queues();
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }
#endif
    scheduler_task_collect();
    struct kernel_task *claim __attribute__((cleanup(reap_claim_end))) = 0;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        thread = scheduler.exited_head;
        if (!thread) return KERNEL_SCHEDULER_STATUS_EMPTY;
        status = scheduler_task_claim_exit_locked(thread);
        if (status != KERNEL_SCHEDULER_STATUS_OK) return status;
        claim = thread;
    }
    if (thread->magic != KERNEL_THREAD_MAGIC ||
        thread->state != KERNEL_THREAD_STATE_EXITED ||
        thread->idle != 0U ||
        (thread->physical_address & BOAROS_PAGE_MASK) != 0U ||
        thread->publish_completion > 1U) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    struct kernel_pid *exit_identity __attribute__((cleanup(kernel_pidfd_notification_end))) =
        kernel_pidfd_exit_identity(thread);
    /* Request handles may live on the old stack; release them before it. */
    if (thread->io_context.locks || thread->io_context.backend_depth) __builtin_trap();
    if (cleanup_task_io_requests(thread)) return KERNEL_SCHEDULER_STATUS_RESOURCE_CLEANUP;
    kernel_ioprio_exit(thread);
    kernel_posix_timers_exit(thread);
    kernel_signal_pending_cleanup(thread);
    status = release_task_stack(thread);
    if (status != KERNEL_SCHEDULER_STATUS_OK) return status;
    result = thread->completion;
    publish = thread->publish_completion;
    if (result.kind == KERNEL_THREAD_KIND_KERNEL) {
        if ((publish == 0U && !thread->join) ||
            result.reason != KERNEL_THREAD_EXIT_RETURNED ||
            result.tid != 0 || result.tgid != 0 ||
            result.status != 0U || result.detail != 0U) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
    } else if (result.kind == KERNEL_THREAD_KIND_USER) {
        if (result.reason != KERNEL_THREAD_EXIT_SYSCALL &&
            result.reason != KERNEL_THREAD_EXIT_USER_FAULT &&
            result.reason != KERNEL_THREAD_EXIT_RESOURCE &&
            result.reason != KERNEL_THREAD_EXIT_SIGNAL) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
        if (result.reason == KERNEL_THREAD_EXIT_RESOURCE &&
            result.status != KERNEL_THREAD_RESOURCE_NO_MEMORY) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
        if (result.reason == KERNEL_THREAD_EXIT_SIGNAL &&
            (result.status == 0U || result.status > 64U)) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
        if (publish != 0U &&
            ((thread->tid_owned != 0U &&
             (thread->group_leader == 0 ||
              result.tid != thread->tid ||
              result.tgid != thread->group_leader->tid)) ||
            (thread->tid_owned == 0U &&
             (result.tid <= 0 || result.tgid <= 0)))) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
        status = cleanup_user_task_resources(thread);
        if (status != KERNEL_SCHEDULER_STATUS_OK) {
            return status;
        }
        /* The final namespace init cannot be published to its outer parent
         * until descendants (including externally parented zombies) are gone.
         * A deferred reaper retains metadata, not a worker or user stack. */
        if (thread->group_leader == thread && thread->group_members == 1) {
            if(process_pid_namespace_drain(thread)) {
                claim = 0; /* drain detached this EXITED owner and released its claim */
                return KERNEL_SCHEDULER_STATUS_EMPTY;
            }
            /* Namespace reboot replaces SIGKILL only after final drain. */
            result=thread->completion;
            /* An orphaned or traced zombie may visit cleanup again. */
            if (!thread->limits->acct_finalized) {
                thread->limits->acct_finalized = 1U;
                kernel_process_acct_exit(thread);
            }
        }
        /* The creator still owns its TID, credentials and group identity here.
         * A leader returning from GROUP_DEAD has already moved its children. */
        if (thread->group_leader) reparent_children(thread);
        /* Cleanup can sleep while other exits append behind this task. */
        if (thread->group_leader == thread && thread->group_members > 1U) {
            /* Keep the group identity until its last member has left. */
            {
                KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
                exited_remove_locked(thread);
                thread->state = KERNEL_THREAD_STATE_GROUP_DEAD;
            }
            (void)kernel_wait_queue_wake_all(&thread->group_wait_queue);
            return KERNEL_SCHEDULER_STATUS_EMPTY;
        }
        if (thread->group_leader != 0 && thread->group_leader != thread) {
            struct kernel_task *leader = thread->group_leader;
            {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            struct kernel_task_usage departed;
            scheduler_usage_thread_locked(thread, &departed);
            scheduler_usage_add(&leader->limits->dead_usage, &departed);
            /* 与退出线程从组目录移除处在同一可观察边界。 */
            kernel_nproc_release_locked(thread);
            thread->group_previous->group_next = thread->group_next;
            thread->group_next->group_previous = thread->group_previous;
            if (!leader->group_members || ++leader->group_epoch == 0) __builtin_trap();
            leader->group_members--;
            if (leader->group_members == 1U &&
                leader->state == KERNEL_THREAD_STATE_GROUP_DEAD) {
                if (!leader->group_exiting) {
                    leader->completion.reason = thread->completion.reason;
                    leader->completion.status = thread->completion.status;
                    leader->completion.detail = thread->completion.detail;
                }
                leader->wait_status = user_wait_status(&leader->completion);
                leader->state = KERNEL_THREAD_STATE_EXITED;
                exited_append_locked(leader);
                    }
            thread->group_leader = thread;
            thread->group_members = 1U;
            process_group_initialize(thread);
            }
            (void)kernel_wait_queue_wake_all(&leader->group_wait_queue);
        }
        if (thread->group_leader == thread && thread->controlling_tty) {
            if (thread->session_leader) kernel_tty_disassociate(thread->controlling_tty, 1);
            else kernel_task_tty_clear(thread);
        }
        /* GROUP_DEAD 的身份仍服务存活成员；最后成员退出才撤销 alarm。 */
        if (thread->group_leader == thread) kernel_signal_timer_cancel(thread);
        process_orphan_notify(thread, 0);
        /* Reparenting may append orphan zombies behind this tail. */
        struct kernel_ptrace_notice trace_notice;
        int traced;
        {
            KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
            traced=ptrace_publish_final_locked(thread,&trace_notice);
            if (traced) {
                exited_remove_locked(thread);
                thread->state=KERNEL_THREAD_STATE_ZOMBIE;
                thread->reaping=0;
                if (thread->parent) thread->publish_completion=0;
                claim=0;
            }
        }
        if (traced) {
            kernel_ptrace_switch_notify(&trace_notice);
            return KERNEL_SCHEDULER_STATUS_EMPTY;
        }
        if (thread->parent != 0) {
            kernel_signal_notify_child_exit(thread);
            if (kernel_signal_child_autoreap(thread->parent,thread->exit_signal)) {
                child_remove(thread->parent, thread);
            }
        }
        if (thread->parent != 0) {
            {
                KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
                struct kernel_task *parent = thread->parent;
                if (!parent || parent->magic != KERNEL_THREAD_MAGIC ||
                    (thread->previous_sibling ? thread->previous_sibling->next_sibling != thread : parent->first_child != thread) ||
                    (thread->next_sibling ? thread->next_sibling->previous_sibling != thread : parent->last_child != thread))
                    return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
                exited_remove_locked(thread);
                thread->state = KERNEL_THREAD_STATE_ZOMBIE;
                thread->publish_completion = 0;
                thread->reaping = 0;
                claim = 0;
            }
            wake_waiting_parent(thread);
            return KERNEL_SCHEDULER_STATUS_EMPTY;
        }
        if (thread->tid_owned) {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            kernel_nproc_release_locked(thread);
            process_identity_release_locked(thread);
            if (scheduler.init_task == thread) scheduler.init_task = 0;
            thread->group_leader = 0;
            thread->group_members = 0;
        }
    } else {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    struct kernel_wait_cursor join_cursor;
    int notify_join;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        exited_remove_locked(thread);
        thread->reaping = 0;
        notify_join = scheduler_forget_task_locked(thread, &join_cursor);
        claim = 0;
    }
    if (notify_join) kernel_wait_cursor_wake(&join_cursor, 1);
    process_identity_collect();
    kernel_task_release_io_scratch(thread);
    scheduler_task_retire(thread);
    scheduler_task_collect();
    if (publish != 0U) {
        *completion = result;
        return KERNEL_SCHEDULER_STATUS_OK;
    }
    return KERNEL_SCHEDULER_STATUS_EMPTY;
}

static void switch_to_fatal_idle(
    enum kernel_scheduler_status status) __attribute__((noreturn));

static void switch_to_fatal_idle(enum kernel_scheduler_status status)
{
    enum kernel_scheduler_status expected = KERNEL_SCHEDULER_STATUS_OK;
    (void)__atomic_compare_exchange_n(&scheduler.fatal_status, &expected, status, 0,
                                      __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    if (kernel_cpu_current()->idle_context_saved != 0U &&
        kernel_cpu_current()->current != kernel_cpu_current()->idle) {
        if (activate_thread_address_space(kernel_cpu_current()->idle) !=
            KERNEL_SCHEDULER_STATUS_OK) {
            for (;;) {
                arch_cpu_wait();
            }
        }
        kernel_cpu_current()->current = kernel_cpu_current()->idle;
        arch_fpu_switch(0, &kernel_cpu_current()->idle->fpu);
#if defined(__riscv)
        riscv_vector_restore(kernel_cpu_current()->idle);
#endif
        arch_context_switch(&kernel_cpu_current()->discard_context,
                             &kernel_cpu_current()->idle->context);
    }

    for (;;) {
        arch_cpu_wait();
    }
}

static enum kernel_scheduler_status cleanup_user_mm(struct kernel_task *thread)
{
    enum kernel_mm_status mm_status;
    if (thread->mm.state != KERNEL_MM_LIVE && thread->mm.state != KERNEL_MM_CLEANUP)
        return KERNEL_SCHEDULER_STATUS_OK;
    scheduler_usage_save_mm(thread);
    mm_status = kernel_mm_release_for_task(&thread->mm,thread);
    if (mm_status == KERNEL_MM_STATUS_OK) return KERNEL_SCHEDULER_STATUS_OK;
    if (mm_status == KERNEL_MM_STATUS_PAGE_ACCESS) return KERNEL_SCHEDULER_STATUS_PAGE_ACCESS;
    if (mm_status == KERNEL_MM_STATUS_CLEANUP_REQUIRED) return KERNEL_SCHEDULER_STATUS_RESOURCE_CLEANUP;
    return mm_status == KERNEL_MM_STATUS_ADDRESS_SPACE
               ? KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE
               : KERNEL_SCHEDULER_STATUS_INVALID_STATE;
}

struct kernel_files_change {
    struct kernel_files candidate, retired;
    struct kernel_heap *heap;
};

static enum kernel_files_status finish_files_change(struct kernel_task *task)
{
    struct kernel_files_change *change=task->files_change;
    if (!change) return KERNEL_FILES_STATUS_OK;
    struct kernel_files *owners[]={&change->candidate,&change->retired};
    for(unsigned i=0;i<2;i++) {
        if(owners[i]->state!=KERNEL_FILES_LIVE && owners[i]->state!=KERNEL_FILES_CLEANUP) continue;
        enum kernel_files_status status=kernel_files_release_for_task(owners[i],task);
        if(status) return status;
    }
    struct kernel_heap *heap=change->heap;
    task->files_change=0;
    kernel_heap_release(heap,change);
    return KERNEL_FILES_STATUS_OK;
}

int64_t kernel_task_close_range(struct kernel_task *task, uint32_t first,
    uint32_t last, uint32_t flags)
{
    if(first>last || (flags&~6U)) return -KERNEL_EINVAL;
    struct kernel_files *files;
    if(kernel_task_files_borrow(task,&files)!=KERNEL_TASK_STATUS_OK) return -KERNEL_EBADF;
    KERNEL_SERVICE_SCOPE_IF(service, (flags&2U) || task->files_change);
    if(finish_files_change(task)) return -KERNEL_EIO;
    if((flags&2U) && kernel_files_is_shared(files)) {
        struct kernel_files_change *change=0;
        enum kernel_heap_status allocated=kernel_heap_allocate_zeroed(files->heap,1,sizeof(*change),(void **)&change);
        if(allocated==KERNEL_HEAP_STATUS_EMPTY) return -KERNEL_ENOMEM;
        if(allocated) __builtin_trap();
        change->heap=files->heap;
        task->files_change=change;
        enum kernel_files_status status=(flags&4U)
            ? kernel_files_fork(&change->candidate,files)
            : kernel_files_fork_except(&change->candidate,files,first,last);
        if(status) {
            if(finish_files_change(task)!=KERNEL_FILES_STATUS_OK) return -KERNEL_EIO;
            return status==KERNEL_FILES_STATUS_NO_MEMORY ? -KERNEL_ENOMEM : -KERNEL_EIO;
        }
        {
            KERNEL_RAW_SCOPE(access,&task->files_access);
            change->retired=task->files;
            task->files=change->candidate;
            change->candidate=(struct kernel_files){0};
        }
        files=&task->files;
    }
    enum kernel_files_status status=kernel_files_close_range(files,first,last,!!(flags&4U),task);
    if(status) __builtin_trap();
    /* 失败清理owner保留在任务中，退出沿同一路径重试。 */
    (void)finish_files_change(task);
    return 0;
}

static enum kernel_scheduler_status cleanup_user_task_resources(
    struct kernel_task *thread)
{
    enum kernel_scheduler_status mm_status;
    enum kernel_exec_status exec_status;
    enum kernel_files_status files_status;
    enum kernel_fs_context_status fs_status;

    if (kernel_process_acct_enabled()) kernel_process_acct_collect_memory(thread);
    files_status=finish_files_change(thread);
    if(files_status) return files_status==KERNEL_FILES_STATUS_CLEANUP_REQUIRED
        ? KERNEL_SCHEDULER_STATUS_RESOURCE_CLEANUP : KERNEL_SCHEDULER_STATUS_INVALID_STATE;

    int vfork_waiter;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        vfork_waiter=thread->vfork_child && thread->vfork_parent;
    }
    if (vfork_waiter) {
        /* User execution and pins have stopped; unrelated close retries must
         * not keep the parent waiting for this shared-MM owner. */
        mm_status=cleanup_user_mm(thread);
        if (mm_status != KERNEL_SCHEDULER_STATUS_OK) return mm_status;
        if (thread->mm.state == KERNEL_MM_RELEASED) process_complete_vfork(thread);
    }

    if (thread->exec_transaction != 0) {
        struct kernel_exec_transaction *transaction =
            thread->exec_transaction;
        struct kernel_heap *heap = transaction->heap;

        /* A sibling may terminate a thread after exec preparation. The
         * prepared image is still owned by this transaction and is aborted. */
        kernel_task_exec_abort(thread,transaction);
        exec_status = kernel_exec_transaction_cleanup(transaction);
        if (exec_status != KERNEL_EXEC_STATUS_OK) {
            return exec_status == KERNEL_EXEC_STATUS_CLEANUP_REQUIRED
                       ? KERNEL_SCHEDULER_STATUS_RESOURCE_CLEANUP
                       : KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
        {
            KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
            thread->exec_transaction=0;
        }
        kernel_heap_release(heap, transaction);
    }
    if (thread->files.state == KERNEL_FILES_LIVE ||
        thread->files.state == KERNEL_FILES_CLEANUP) {
        files_status = kernel_files_release_for_task(&thread->files,thread);
        if (files_status != KERNEL_FILES_STATUS_OK) {
            return files_status == KERNEL_FILES_STATUS_CLEANUP_REQUIRED
                       ? KERNEL_SCHEDULER_STATUS_RESOURCE_CLEANUP
                       : KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
    }
    if (thread->files.state != KERNEL_FILES_EMPTY &&
        thread->files.state != KERNEL_FILES_RELEASED) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    if (thread->fs.state == KERNEL_FS_CONTEXT_LIVE ||
        thread->fs.state == KERNEL_FS_CONTEXT_CLEANUP) {
        fs_status = kernel_fs_context_release(&thread->fs);
        if (fs_status != KERNEL_FS_CONTEXT_STATUS_OK) {
            return fs_status == KERNEL_FS_CONTEXT_STATUS_CLEANUP_REQUIRED
                       ? KERNEL_SCHEDULER_STATUS_RESOURCE_CLEANUP
                       : KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
    }
    if (thread->fs.state != KERNEL_FS_CONTEXT_EMPTY &&
        thread->fs.state != KERNEL_FS_CONTEXT_RELEASED) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    mm_status=cleanup_user_mm(thread);
    if (mm_status != KERNEL_SCHEDULER_STATUS_OK) return mm_status;
    kernel_task_namespaces_release(thread);
    /* MM/FD/cwd已归还，脱树挂载的最后owner可在此安全停止后端。 */
    (void)kernel_vfs_reap_mounts();
    return (thread->mm.state == KERNEL_MM_RELEASED ||
            (thread->publish_completion == 0U &&
             thread->mm.state == KERNEL_MM_EMPTY))
               ? KERNEL_SCHEDULER_STATUS_OK
               : KERNEL_SCHEDULER_STATUS_INVALID_STATE;
}

enum kernel_task_status kernel_task_io_buffer_acquire(
    struct kernel_task_io_buffer *buffer, struct physical_page_allocator *allocator)
{
    struct kernel_task *task = kernel_task_current();
    if (buffer == 0 || buffer->allocator != 0 || allocator == 0 ||
        (task != 0 && task->io_buffer != 0)) __builtin_trap();
    uint64_t physical;
    void *data;
    if (task != 0 && task->io_scratch_data != 0) {
        /* The task keeps one scratch page across calls; only the allocator
         * identity is re-checked because every caller resolves the same
         * physical-page owner. */
        if (task->io_scratch_allocator != allocator) __builtin_trap();
        physical = task->io_scratch_physical_address;
        data = task->io_scratch_data;
    } else {
        enum physical_page_status status =
            physical_page_allocate(allocator, &physical);
        if (status == PHYSICAL_PAGE_STATUS_EMPTY)
            return KERNEL_TASK_STATUS_RESOURCE_UNAVAILABLE;
        if (status != PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
        if (physical_page_map_owned(allocator, physical, &data) !=
            PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
        if (task != 0) {
            task->io_scratch_allocator = allocator;
            task->io_scratch_physical_address = physical;
            task->io_scratch_data = data;
        }
    }
    *buffer = (struct kernel_task_io_buffer){.task=task, .allocator=allocator,
        .physical_address=physical, .data=data, .retained_scratch=1};
    if (task != 0) task->io_buffer = buffer;
    return KERNEL_TASK_STATUS_OK;
}

void kernel_task_io_buffer_take_page(struct kernel_task_io_buffer *buffer,
    struct physical_page_allocator *allocator, uint64_t physical_address,const void *mapped)
{
    struct kernel_task *task = kernel_task_current();
    if (!buffer || buffer->allocator || !allocator || (task && task->io_buffer))
        __builtin_trap();
    void *data=(void *)mapped;
    if (!data && physical_page_map_owned(allocator, physical_address, &data)) __builtin_trap();
    *buffer = (struct kernel_task_io_buffer){.task=task, .allocator=allocator,
        .physical_address=physical_address, .data=data};
    if (task) task->io_buffer=buffer;
}

void kernel_task_io_buffer_take_read_pages(struct kernel_task_io_buffer *buffer,
    struct kernel_page_cache *cache, struct kernel_page_cache_read_page *pages, size_t count)
{
    struct kernel_task *task = kernel_task_current();
    if (task && task->io_buffer) __builtin_trap();
    *buffer = (struct kernel_task_io_buffer){.task=task, .read_batch={cache,pages,count}};
    if (task) task->io_buffer = buffer;
}

void kernel_task_io_buffer_release(struct kernel_task_io_buffer *buffer)
{
    if (!buffer) __builtin_trap();
    if (!buffer->allocator) {
        if (!buffer->read_batch.cache) __builtin_trap();
        if (buffer->task) {
            if (buffer->task->io_buffer != buffer) __builtin_trap();
            buffer->task->io_buffer = NULL;
        }
        kernel_page_cache_put_batch(buffer->read_batch.cache, buffer->read_batch.pages,
                                    buffer->read_batch.count);
        *buffer = (struct kernel_task_io_buffer){0};
        return;
    }
    if (buffer->wipe_bytes) kernel_random_erase(buffer->data,buffer->wipe_bytes);
    if (buffer->task) {
        if (buffer->task->io_buffer != buffer) __builtin_trap();
        if (buffer->retained_scratch &&
            (!buffer->task->io_scratch_data ||
             buffer->task->io_scratch_physical_address != buffer->physical_address))
            __builtin_trap();
        buffer->task->io_buffer = 0;
    }
    /* 常驻scratch继续归任务；其余引用在完成、信号或forced exit时立即归还。 */
    if ((!buffer->task || !buffer->retained_scratch) &&
        physical_page_release(buffer->allocator, buffer->physical_address))
        __builtin_trap();
    *buffer = (struct kernel_task_io_buffer){0};
}

/* Final storage release frees the retained scratch page; callers run on a
 * trusted stack after every live request handle was already released. */
void kernel_task_release_io_scratch(struct kernel_task *task)
{
    if (task == 0 || task->io_buffer != 0) __builtin_trap();
    if (task->io_scratch_data == 0) return;
    if (physical_page_release(task->io_scratch_allocator,
                              task->io_scratch_physical_address) !=
        PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
    task->io_scratch_allocator = 0;
    task->io_scratch_physical_address = 0;
    task->io_scratch_data = 0;
}

enum kernel_task_status kernel_task_epoll_register(
    struct kernel_task *task, struct kernel_epoll_wait_request *request,
    struct kernel_epoll_wait_request **previous)
{
    if (!task || task != kernel_task_current() || !request || !previous)
        return KERNEL_TASK_STATUS_STATE;
    *previous = task->epoll_wait_request;
    task->epoll_wait_request = request;
    return KERNEL_TASK_STATUS_OK;
}

enum kernel_task_status kernel_task_epoll_clear(
    struct kernel_task *task, struct kernel_epoll_wait_request *request,
    struct kernel_epoll_wait_request *previous)
{
    if (!task || !request || task->epoll_wait_request != request)
        return KERNEL_TASK_STATUS_STATE;
    task->epoll_wait_request = previous;
    return KERNEL_TASK_STATUS_OK;
}

enum kernel_task_status kernel_task_socket_call_register(struct kernel_task *task, struct kernel_socket_call *call)
{
    if (!task || task != kernel_task_current() || !call || task->socket_call)
        return KERNEL_TASK_STATUS_STATE;
    task->socket_call = call;
    return KERNEL_TASK_STATUS_OK;
}

void kernel_task_fanotify_read_set(struct kernel_task *task, struct kernel_fanotify_read *read)
{
    if (task != kernel_task_current() || task->fanotify_read) __builtin_trap();
    task->fanotify_read = read;
}

void kernel_task_fanotify_read_clear(struct kernel_task *task, struct kernel_fanotify_read *read)
{
    if (task->fanotify_read != read) __builtin_trap();
    task->fanotify_read = NULL;
}
enum kernel_task_status kernel_task_socket_call_clear(struct kernel_task *task, struct kernel_socket_call *call)
{
    if (!task || !call || task->socket_call != call) return KERNEL_TASK_STATUS_STATE;
    task->socket_call = 0;
    return KERNEL_TASK_STATUS_OK;
}

enum kernel_task_status kernel_task_socket_read_register(
    struct kernel_task *task, struct kernel_socket_read_request *request)
{
    if (task == 0 || request == 0 || task->socket_read_request != 0 ||
        task != kernel_task_current()) return KERNEL_TASK_STATUS_STATE;
    task->socket_read_request = request;
    return KERNEL_TASK_STATUS_OK;
}

enum kernel_task_status kernel_task_socket_read_clear(
    struct kernel_task *task, struct kernel_socket_read_request *request)
{
    if (task == 0 || request == 0 || task->socket_read_request != request)
        return KERNEL_TASK_STATUS_STATE;
    task->socket_read_request = 0;
    return KERNEL_TASK_STATUS_OK;
}

static uint32_t fault_wait_status(uint64_t scause)
{
    switch (scause) {
    case 2U:
        return 4U;  /* SIGILL */
    case 3U:
        return 5U;  /* SIGTRAP */
    case 4U:
    case 6U:
        return 7U;  /* SIGBUS */
    default:
        return 11U; /* SIGSEGV */
    }
}

static uint32_t user_wait_status(
    const struct kernel_thread_completion *completion)
{
    if (completion->reason == KERNEL_THREAD_EXIT_SYSCALL) {
        return (uint32_t)((completion->status & UINT64_C(0xff)) << 8U);
    }
    if (completion->reason == KERNEL_THREAD_EXIT_RESOURCE) {
        return 9U; /* SIGKILL */
    }
    if (completion->reason == KERNEL_THREAD_EXIT_SIGNAL) {
        return ((uint32_t)completion->status & UINT32_C(0x7f)) |
            (completion->detail ? UINT32_C(0x80) : 0);
    }
    return fault_wait_status(completion->status);
}

static struct kernel_task *live_reaper_locked(struct kernel_task *leader,
                                              struct kernel_task *departing)
{
    if (!leader || leader->group_exiting) return 0;
    struct kernel_task *member = leader;
    do {
        if (member != departing && !member->terminate_requested && !member->proc_exiting &&
            member->state != KERNEL_THREAD_STATE_EXITED && member->state != KERNEL_THREAD_STATE_ZOMBIE &&
            member->state != KERNEL_THREAD_STATE_GROUP_DEAD)
            return member;
        member = member->group_next;
    } while (member != leader);
    return 0;
}

static struct kernel_task *child_reaper_locked(struct kernel_task *departing)
{
    struct kernel_task *leader = departing->group_leader;
    struct kernel_task *member = live_reaper_locked(leader, departing);
    if (member) return member;
    struct kernel_pid_registry *space = process_pid_active(departing);
    for (struct kernel_task *ancestor = leader->parent; ancestor; ancestor = ancestor->parent) {
        if (process_pid_active(ancestor) != space) break;
        /* During a nonleader exec the old child-list owner briefly aliases
         * the new leader. Both designate the same natural parent group. */
        ancestor = ancestor->group_leader;
        if (ancestor->child_subreaper) {
            member = live_reaper_locked(ancestor, departing);
            if (member) return member;
        }
    }
    struct kernel_task *reaper = process_pid_reaper_locked(space);
    if (space && space->parent && !space->accepting && reaper)
        return reaper; /* Dying namespace init still owns adoption and autoreap. */
    return live_reaper_locked(reaper, departing);
}

static struct kernel_task *parent_death_notices_locked(struct kernel_task *child,
                                                       struct kernel_task *departing)
{
    struct kernel_task *head = 0, **tail = &head;
    struct kernel_task *member = child;
    do {
        uint32_t sig = member->parent_death_signal;
        if (sig && kernel_signal_permitted_locked(departing, member, sig)) {
            /* Only reap_one transfers a dead creator's children. Its sole
             * cleanup executor drains this list before its next transfer. */
            if (member->parent_death_notice || member->parent_death_next) __builtin_trap();
            scheduler_task_get_locked(member);
            member->parent_death_notice = sig;
            *tail = member;
            tail = &member->parent_death_next;
        }
        member = member->group_next;
    } while (member != child);
    return head;
}

static void reparent_children(struct kernel_task *departing)
{
    struct kernel_task *parent = departing->group_leader;
    if (validate_child_endpoints(parent) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
    SCHEDULER_TASK_BORROW(cursor, 0);
    uint64_t epoch = 0;
    for (;;) {
        SCHEDULER_TASK_BORROW(child, 0);
        SCHEDULER_TASK_BORROW(new_parent, 0);
        struct kernel_task *next, *notices = 0;
        struct kernel_pid *group __attribute__((cleanup(kernel_task_identity_end))) = 0;
        kernel_pid_t sender = 0;
        uint32_t uid = 0;
        int moved = 0, zombie = 0;
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            struct kernel_task *item = cursor && epoch == parent->children_epoch
                ? cursor : parent->first_child;
            if (!item) return;
            next = item->next_sibling;
            if (next) scheduler_task_get_locked(next);
            if (item->child_creator == process_identity(departing, KERNEL_PID_TID)) {
                struct kernel_task *reaper = child_reaper_locked(departing);
                child = item;
                scheduler_task_get_locked(child);
                notices = parent_death_notices_locked(child, departing);
                if (notices) {
                    group = process_identity(child, KERNEL_PID_TGID);
                    kernel_pid_get(group);
                    sender = kernel_pid_number_in(process_identity(departing, KERNEL_PID_TGID),
                                                   process_pid_active(child));
                    uid = kernel_credentials_task_locked(departing)->uid;
                }
                child_creator_change_locked(child, process_identity(reaper, KERNEL_PID_TID));
                if (!reaper || reaper->group_leader != parent) {
                    moved = 1;
                    child_remove_locked(parent, child);
                    if (reaper) {
                        new_parent = reaper->group_leader;
                        scheduler_task_get_locked(new_parent);
                        child_append_locked(new_parent, child);
                    }
                    child->exit_signal = 17U;
                    zombie = child->state == KERNEL_THREAD_STATE_ZOMBIE &&
                             !kernel_ptrace_tracer_tid_locked(child);
                    if (zombie && !new_parent) {
                        child->state = KERNEL_THREAD_STATE_EXITED;
                        child->publish_completion = 0;
                        exited_append_locked(child);
                    }
                }
            }
            epoch = parent->children_epoch;
        }
        if (cursor) scheduler_task_put(cursor);
        cursor = next;
        while (notices) {
            struct kernel_task *member = notices;
            notices = member->parent_death_next;
            uint32_t sig = member->parent_death_notice;
            member->parent_death_next = 0;
            member->parent_death_notice = 0;
            kernel_signal_parent_death(member, group, sig, sender, uid);
            scheduler_task_put(member);
        }
        if (zombie && new_parent) {
            kernel_signal_notify_child_exit(child);
            if (kernel_signal_child_autoreap(new_parent,17U)) {
                KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
                if (child->parent == new_parent && child->state == KERNEL_THREAD_STATE_ZOMBIE &&
                    !child->reaping) {
                    child_remove_locked(new_parent, child);
                    child->state = KERNEL_THREAD_STATE_EXITED;
                    child->publish_completion = 0;
                    exited_append_locked(child);
                }
            }
        }
        if (moved) {
            if (zombie) (void)kernel_wait_queue_wake_all(&scheduler.cleanup_queue);
            process_orphan_notify(child, parent);
        }
        if (!cursor) return;
    }
}

int kernel_task_reboot_namespace(struct kernel_task *caller,enum kernel_reboot_action action)
{
    SCHEDULER_TASK_BORROW(init,NULL);
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        struct kernel_pid_registry *space=process_pid_active(caller);
        if(!space || !space->parent)return 0;
        if(action<KERNEL_REBOOT_RESTART || action>KERNEL_REBOOT_POWER_OFF)return -KERNEL_EINVAL;
        struct kernel_pid *id=space->child_reaper;
        init=id && id->members[KERNEL_PID_TGID]?id->members[KERNEL_PID_TGID]->task:NULL;
        if(!init)return -KERNEL_ESRCH;
        scheduler_task_get_locked(init);
        space->reboot_signal=action==KERNEL_REBOOT_RESTART?1U:2U;
    }
    /* Trusted SIGKILL uses this same group-exit transaction. The registry
     * keeps the reported HUP/INT separate until every descendant is gone. */
    process_group_request_exit(init,KERNEL_THREAD_EXIT_SIGNAL,9,0);
    return 1;
}

static void namespace_reboot_completion_locked(struct kernel_task *leader,
    struct kernel_pid_registry *space)
{
    if(space->reboot_signal) {
        leader->completion.reason=KERNEL_THREAD_EXIT_SIGNAL;
        leader->completion.status=space->reboot_signal;
        leader->completion.detail=0;
        leader->wait_status=user_wait_status(&leader->completion);
        leader->limits->acct_leader_status=leader->wait_status;
    }
}

/* Called only by the cleanup owner after the init group has one remaining
 * metadata container and no MM/FD/stack resources. Never wait on this worker:
 * it is also responsible for retiring the namespace's other tasks. */
static int process_pid_namespace_drain(struct kernel_task *leader)
{
    struct kernel_pid_registry *space;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        space = process_pid_active(leader);
        if (!space || !space->parent ||
            space->child_reaper != process_identity(leader, KERNEL_PID_TGID)) return 0;
        if (space->shutdown_reaper) {
            if (space->shutdown_reaper != leader || space->numbers->allocated != 1)
                __builtin_trap();
            namespace_reboot_completion_locked(leader,space);
            leader->limits->acct_leader_status = leader->wait_status;
            return 0;
        }
        kernel_pid_registry_disable(space);
        space->shutdown_reaper = leader;
    }
    kernel_signal_ignore_sigchld(leader);
    kernel_pid_t after = 1;
    for (;;) {
        SCHEDULER_TASK_BORROW(target, NULL);
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            target = process_pid_namespace_kill_target_locked(space, &after);
            if (!target) break;
            scheduler_task_get_locked(target);
        }
        if (target) process_group_request_exit(target, KERNEL_THREAD_EXIT_SIGNAL, 9, 0);
    }
    /* Children that were already zombies predate SIGCHLD ignore. A real wait
     * claim avoids racing a remote tracer release or an existing waiter. */
    for (;;) {
        SCHEDULER_TASK_BORROW(child, NULL);
        struct kernel_task *claim __attribute__((cleanup(wait_claim_end))) = NULL;
        uint32_t event;
        int matching;
        if (select_wait_child(leader, LINUX_P_ALL, NULL, LINUX_WEXITED|LINUX___WALL,
                              &child, &event, &matching) != KERNEL_SCHEDULER_STATUS_OK)
            __builtin_trap();
        claim = child;
        if (!child) break;
        if (event == TRACE_WAIT_FINAL && finish_traced_wait(leader, child)) event = 1;
        if (event != 1) __builtin_trap();
        struct kernel_task_usage usage = {0};
        scheduler_usage_snapshot_mm(child, -2, &usage);
        if (retire_waited_zombie(leader, child, &usage) != KERNEL_SCHEDULER_STATUS_OK)
            __builtin_trap();
    }
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        /* No new identities can publish now. A pre-admission reservation is
         * awaited too; its rollback runs the same collection notification. */
        int deferred=process_pid_namespace_defer_locked(leader);
        if(!deferred)namespace_reboot_completion_locked(leader,space);
        return deferred;
    }
}

static void wake_local_cleanup_locked(void)
{
    struct kernel_wait_queue *queue = &scheduler.cleanup_queue;
    struct kernel_wait_token token = {0};
    {
        KERNEL_IRQ_LOCK_SCOPE(registrations, &queue->lock);
        const struct kernel_wait_node *node = queue->head;
        if (node) token = (struct kernel_wait_token){node->task, node->generation};
    }
    /* The cleanup queue's only waiter is the local reaper. It cannot run
     * and retire its wait while this CPU is selecting the next task. */
    if (token.task) (void)kernel_wait_notify_locked(&token, KERNEL_WAIT_WOKEN);
}

static void kernel_thread_finish(
    const struct kernel_thread_completion *completion)
    __attribute__((noreturn));

static void process_begin_exit(struct kernel_task *current,
    const struct kernel_thread_completion *completion)
{
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        if (current->cgroup) {
            if (!current->cgroup->members) __builtin_trap();
            current->cgroup->members--;
        }
        if (kernel_wait_record_phase(&current->wait) != KERNEL_WAIT_FINISHED || current->wait.nodes || kernel_wait_record_borrows(&current->wait))
            __builtin_trap();
        current->proc_exiting = 1;
        if (current->arch.user_mode && current->group_leader && current->limits) {
            /* The scheduler publishes exits here. Cleanup can finish in a
             * different order or revisit a leader after its siblings leave. */
            struct kernel_resource_limits *limits = current->limits;
            limits->acct_last_nonleader = current != current->group_leader;
            if (limits->acct_last_nonleader) {
                const struct kernel_credentials *cred = kernel_credentials_task_locked(current);
                limits->acct_last_uid = cred->uid;
                limits->acct_last_gid = cred->gid;
                memcpy(limits->acct_last_comm, current->comm, sizeof(limits->acct_last_comm));
            }
        }
        if (current->group_leader && !live_reaper_locked(current->group_leader, current))
            process_pid_reaper_disable_locked(current->group_leader);
        if (!current->group_exiting) current->completion = *completion;
    }
    /* 退出状态先于资源释放发布；kcmp把已离开的资源视为NULL，getfd
     * 拒绝新访问。存量读者结束后原owner才释放句柄，后端清理在锁外。 */
    KERNEL_RAW_SCOPE(files, &current->files_access);
}

static void kernel_thread_finish(
    const struct kernel_thread_completion *completion)
{
    kernel_assert_can_block();
    struct kernel_task *current;
    struct kernel_task *next;
    enum kernel_scheduler_status cleanup_status =
        KERNEL_SCHEDULER_STATUS_OK;
    enum kernel_scheduler_status status;

    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED ||
        arch_interrupt_is_enabled()) {
        switch_to_fatal_idle(KERNEL_SCHEDULER_STATUS_INVALID_STATE);
    }
    current = kernel_cpu_current()->current;
    status = validate_current();
    if (status != KERNEL_SCHEDULER_STATUS_OK || current == kernel_cpu_current()->idle) {
        switch_to_fatal_idle(
            status == KERNEL_SCHEDULER_STATUS_OK
                ? KERNEL_SCHEDULER_STATUS_INVALID_STATE
                : status);
    }
#if BOAROS_LOCK_DEBUG
    status = validate_queues();
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        switch_to_fatal_idle(status);
    }
#endif

    if (current->arch.user_mode) process_core_rendezvous(current);
    if (current->arch.user_mode) ptrace_begin_exit(current,user_wait_status(completion));
    current->rseq = (struct kernel_rseq){0};
    process_begin_exit(current, completion);
    /* Undo is visible before publishing exit or waking a child-TID joiner. */
    if (current->arch.user_mode) kernel_sem_exit(current);
    if (current->arch.user_mode) kernel_signal_task_exit(current);
    /* proc readers must see departure before clear_child_tid wakes a joiner. */
#if BOAROS_COST_DIAGNOSTICS
    kernel_cost_task_exit();
#endif
    int last_mm_user = 0;
    if (current->arch.user_mode == 1U) {
        kernel_pid_t robust_tid;
        if(kernel_task_user_tid(current,&robust_tid)!=KERNEL_TASK_STATUS_OK)__builtin_trap();
        kernel_futex_release_robust(current, robust_tid);
        last_mm_user = !kernel_futex_release_mm(current);
        if (!scheduler.cleanup_task) {
            /* Inline cleanup can release the current root before switching. */
            if (kernel_mm_activate(0, scheduler.kernel_context) != KERNEL_MM_STATUS_OK)
                switch_to_fatal_idle(KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE);
            cleanup_status = cleanup_task_io_requests(current)
                ? KERNEL_SCHEDULER_STATUS_RESOURCE_CLEANUP
                : cleanup_user_task_resources(current);
        }
        current->wait_status = user_wait_status(&current->completion);
        if (current->group_leader == current && current->limits)
            current->limits->acct_leader_status = current->wait_status;
    }
    (void)cleanup_status;
    struct kernel_cpu *cpu = kernel_cpu_current();
    struct kernel_wait_cursor cleanup_cursor;
    int notify_cleanup = 0;
    uint64_t next_deadline;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        KERNEL_RUN_SET(runs);
        current->state = KERNEL_THREAD_STATE_EXITED;
        exited_append_locked(current);
        scheduler_account_runtime_locked();
        struct kernel_task *cleanup = scheduler.cleanup_task;
        if (cleanup && kernel_wait_queue_registrations(&scheduler.cleanup_queue)) {
#if BOAROS_MAX_CPUS > 1
            if (cleanup->cpu_mask != (1U << cpu->logical_id))
                notify_cleanup = kernel_wait_cursor_prepare(&scheduler.cleanup_queue, &cleanup_cursor);
            else
#endif
                wake_local_cleanup_locked();
        }
        /* Include the local reaper in this decision. The exiting task's
         * stack remains owned until switch_finish runs on the new stack. */
        (void)kernel_cpu_consume_schedule(cpu);
        next = scheduler_select_next_locked(cpu, current, scheduler.cleanup_task != 0);
        if (!next) __builtin_trap();
        next_deadline = scheduler_deadline_locked(cpu);
    }
    if (notify_cleanup) kernel_wait_cursor_wake(&cleanup_cursor, 1);
    if (last_mm_user && next->mm.state != KERNEL_MM_LIVE) {
        /* A kernel successor need not borrow the dying user root. The
         * reaper would switch away from it before releasing that root. */
        status = kernel_mm_activate(NULL, scheduler.kernel_context)
            ? KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE : KERNEL_SCHEDULER_STATUS_OK;
    } else {
        status = activate_thread_address_space(next);
    }
    if (status != KERNEL_SCHEDULER_STATUS_OK) switch_to_fatal_idle(status);
#if BOAROS_COST_DIAGNOSTICS
    kernel_cost_switch(&current->cost, &next->cost);
#endif
    scheduler_arm_timer(next_deadline);
    arch_fpu_switch(0, &next->fpu);
#if defined(__riscv)
    riscv_vector_restore(next);
#endif
    scheduler_cpu_time_switch(current, next);
    arch_context_switch(&kernel_cpu_current()->discard_context, &next->context);
    kernel_scheduler_switch_finish();

    switch_to_fatal_idle(KERNEL_SCHEDULER_STATUS_INVALID_STATE);
}

void kernel_thread_exit(void)
{
    const struct kernel_thread_completion completion = {
        .kind = KERNEL_THREAD_KIND_KERNEL,
        .reason = KERNEL_THREAD_EXIT_RETURNED,
        .tid = 0,
        .tgid = 0,
        .status = 0U,
        .detail = 0U,
    };

    kernel_thread_finish(&completion);
}

void kernel_user_thread_exit(
    enum kernel_thread_exit_reason reason,
    uint64_t status,
    uint64_t detail)
{
#if BOAROS_USER_SMP
    if (kernel_task_has_user_context(kernel_task_current()) && !kernel_scheduler_user_service_active())
        kernel_scheduler_enter_user_service();
#endif
    struct kernel_thread_completion completion = {
        .kind = KERNEL_THREAD_KIND_USER,
        .reason = reason,
        .tid = 0,
        .tgid = 0,
        .status = status,
        .detail = detail,
    };

    if (reason != KERNEL_THREAD_EXIT_SYSCALL &&
        reason != KERNEL_THREAD_EXIT_USER_FAULT &&
        reason != KERNEL_THREAD_EXIT_RESOURCE &&
        reason != KERNEL_THREAD_EXIT_SIGNAL) {
        switch_to_fatal_idle(KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT);
    }
    if (reason == KERNEL_THREAD_EXIT_RESOURCE &&
        status != KERNEL_THREAD_RESOURCE_NO_MEMORY) {
        switch_to_fatal_idle(KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT);
    }
    if (reason == KERNEL_THREAD_EXIT_SIGNAL &&
        (status == 0U || status > 64U)) {
        switch_to_fatal_idle(KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT);
    }
    if (kernel_cpu_current()->current == 0 ||
        kernel_cpu_current()->current->arch.user_mode != 1U) {
        switch_to_fatal_idle(KERNEL_SCHEDULER_STATUS_INVALID_STATE);
    }
    if (reason == KERNEL_THREAD_EXIT_SIGNAL && detail) {
        detail = process_core_dump(kernel_cpu_current()->current, (unsigned)status);
        completion.detail = detail;
    }
    completion.tid = kernel_cpu_current()->current->tid;
    completion.tgid = kernel_cpu_current()->current->group_leader->tid;
    if (reason != KERNEL_THREAD_EXIT_SYSCALL)
        process_group_request_exit(kernel_cpu_current()->current, reason, status, detail);
    kernel_thread_finish(&completion);
}

struct kernel_task *kernel_task_current(void)
{
    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED) return 0;
    /* CPU字段只在短IRQ区借用；返回任务自身的稳定owner，而非CPU记录借用。 */
    uintptr_t irq = arch_interrupt_save();
    struct kernel_task *current = kernel_cpu_current()->current;
    if (arch_current_thread_get() != current) current = 0;
    arch_interrupt_restore(irq);
    return current;
}

enum kernel_task_status kernel_task_set_tid_address(
    struct kernel_task *task,
    uint64_t address,
    kernel_pid_t *tid)
{
    if (task == 0 || tid == 0) {
        return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    }
    enum kernel_task_status status = kernel_task_user_tid(task, tid);
    if (status == KERNEL_TASK_STATUS_OK) task->clear_tid_address = address;
    return status;
}

enum kernel_task_status kernel_task_tid(
    const struct kernel_task *task,
    kernel_pid_t *tid)
{
    if (task == 0 || tid == 0) {
        return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    }
    if (task->magic != KERNEL_THREAD_MAGIC || task->tid_owned != 1U ||
        task->tid <= 0) {
        return KERNEL_TASK_STATUS_STATE;
    }
    *tid = task->tid;
    return KERNEL_TASK_STATUS_OK;
}

static enum kernel_task_status task_tgid_locked(
    const struct kernel_task *task,
    kernel_pid_t *tgid)
{
    const struct kernel_task *leader;

    if (task == 0 || tgid == 0) {
        return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    }
    leader = task->group_leader;
    if (task->magic != KERNEL_THREAD_MAGIC || task->tid_owned != 1U ||
        leader == 0 || leader->magic != KERNEL_THREAD_MAGIC ||
        leader->tid_owned != 1U || leader->tid <= 0 ||
        leader->group_leader != leader || leader->group_members == 0U) {
        return KERNEL_TASK_STATUS_STATE;
    }
    *tgid = leader->tid;
    return KERNEL_TASK_STATUS_OK;
}

static inline const struct kernel_task *running_group_leader(const struct kernel_task *task)
{
    const struct kernel_task *leader=task->group_leader;
    return task->magic==KERNEL_THREAD_MAGIC && task->tid_owned && leader &&
        leader->magic==KERNEL_THREAD_MAGIC && leader->tid_owned && leader->tid>0 ?
        leader : NULL;
}

enum kernel_task_status kernel_task_tgid(const struct kernel_task *task, kernel_pid_t *tgid)
{
    if (!task || !tgid) return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    /* 当前执行者持有自己的组成员资格。组长退出仍保留身份，非组长
     * exec 必须等本线程停止后才能转移它；读取自身 TGID 不需全局锁。
     * 远端查询仍在调度域取得一致快照。 */
    if (task==arch_current_thread_get() && task->arch.user_mode) {
        const struct kernel_task *leader=running_group_leader(task);
        if (!leader) return KERNEL_TASK_STATUS_STATE;
        *tgid=leader->tid;
        return KERNEL_TASK_STATUS_OK;
    }
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    return task_tgid_locked(task, tgid);
}

enum kernel_task_status kernel_task_user_tid(const struct kernel_task *task, kernel_pid_t *tid)
{
    enum kernel_task_status status = kernel_task_tid(task, tid);
    if (status != KERNEL_TASK_STATUS_OK) return status;
    if (task == arch_current_thread_get() && task->arch.user_mode) {
        *tid = process_identity(task, KERNEL_PID_TID)->number;
        return KERNEL_TASK_STATUS_OK;
    }
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    struct kernel_pid *id = process_identity(task, KERNEL_PID_TID);
    if (!id) return KERNEL_TASK_STATUS_STATE;
    *tid = id->number;
    return KERNEL_TASK_STATUS_OK;
}

enum kernel_task_status kernel_task_user_tgid(const struct kernel_task *task, kernel_pid_t *tgid)
{
    if (!task || !tgid) return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    if (task == arch_current_thread_get() && task->arch.user_mode) {
        const struct kernel_task *leader=running_group_leader(task);
        if (!leader) return KERNEL_TASK_STATUS_STATE;
        /* A running member pins the group identity, including an exited
         * leader. Thread creation keeps TID and TGID in the same active
         * namespace. setns only changes the namespace for future children. */
        const struct kernel_pid *group=leader->identities[KERNEL_PID_TGID].identity;
        if (!group) return KERNEL_TASK_STATUS_STATE;
        *tgid=group->number;
        return *tgid>0 ? KERNEL_TASK_STATUS_OK : KERNEL_TASK_STATUS_STATE;
    }
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    enum kernel_task_status status = task_tgid_locked(task, tgid);
    if (status != KERNEL_TASK_STATUS_OK) return status;
    *tgid = kernel_pid_number_in(process_identity(task, KERNEL_PID_TGID), process_pid_active(task));
    return *tgid ? KERNEL_TASK_STATUS_OK : KERNEL_TASK_STATUS_STATE;
}

static struct kernel_rlimit64 *task_limit_slot(struct kernel_task *leader, uint32_t resource)
{
    return resource<KERNEL_RLIMIT_COUNT ? &leader->limits->values[resource] : 0;
}

enum kernel_task_status kernel_task_get_rlimit(
    const struct kernel_task *task, uint32_t resource,
    struct kernel_rlimit64 *limit)
{
    kernel_pid_t tgid;

    if (limit == NULL)
        return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    if (task_tgid_locked(task, &tgid) != KERNEL_TASK_STATUS_OK)
        return KERNEL_TASK_STATUS_STATE;
    struct kernel_rlimit64 *slot = task_limit_slot(task->group_leader, resource);
    if (!slot) return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    *limit = *slot;
    return KERNEL_TASK_STATUS_OK;
}

int kernel_task_prlimit(struct kernel_task *task,uint32_t resource,
    const struct kernel_rlimit64 *replacement,struct kernel_rlimit64 *before,int can_raise)
{
    kernel_pid_t tgid;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        if(task_tgid_locked(task,&tgid)!=KERNEL_TASK_STATUS_OK)return -KERNEL_ESRCH;
        struct kernel_task *caller=kernel_task_current();
        const struct kernel_credentials *from=kernel_credentials_task_locked(caller),
            *to=kernel_credentials_task_locked(task);
        if(caller!=task && !kernel_credentials_capable(from,KERNEL_CAP_SYS_RESOURCE) &&
           !(from->uid==to->uid && from->uid==to->euid && from->uid==to->suid &&
             from->gid==to->gid && from->gid==to->egid && from->gid==to->sgid))return -KERNEL_EPERM;
        struct kernel_rlimit64 *slot=task_limit_slot(task->group_leader,resource);
        if(!slot || (replacement&&replacement->current>replacement->maximum))return -KERNEL_EINVAL;
        if(replacement && ((resource==KERNEL_RLIMIT_NOFILE&&replacement->maximum>KERNEL_RLIMIT_NOFILE_CAP) ||
           (replacement->maximum>slot->maximum&&!can_raise)))return -KERNEL_EPERM;
        if(before)*before=*slot;
        if(replacement) {
            __atomic_store_n(&slot->current,replacement->current,__ATOMIC_RELAXED);
            __atomic_store_n(&slot->maximum,replacement->maximum,__ATOMIC_RELAXED);
            if(resource==KERNEL_RLIMIT_CPU)scheduler_cpu_timer_update_locked(task->group_leader,0);
        }
    }
    if(replacement&&resource==KERNEL_RLIMIT_CPU)kernel_posix_timer_changed();
    return 0;
}

uint64_t kernel_task_current_fsize_limit(void)
{
    struct kernel_task *task = kernel_task_current();
    if (!kernel_task_has_user_context(task)) return KERNEL_RLIM_INFINITY;
    /* 当前执行owner保持组身份；限额接纳只读软值，不取得整组快照锁。 */
    return __atomic_load_n(&task->limits->values[KERNEL_RLIMIT_FSIZE].current, __ATOMIC_RELAXED);
}

enum kernel_task_status kernel_task_memory_limits(
    const struct kernel_task *task, struct kernel_memory_limits *limits)
{
    kernel_pid_t tgid;
    if (!limits) return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    if (task_tgid_locked(task, &tgid) != KERNEL_TASK_STATUS_OK)
        return KERNEL_TASK_STATUS_STATE;
    const struct kernel_task *leader = task->group_leader;
    *limits = (struct kernel_memory_limits){leader->limits->values[KERNEL_RLIMIT_AS].current,
        leader->limits->values[KERNEL_RLIMIT_DATA].current, leader->limits->values[KERNEL_RLIMIT_DATA].maximum};
    return KERNEL_TASK_STATUS_OK;
}

void kernel_task_current_memory_limits(struct kernel_memory_limits *limits)
{
    struct kernel_task *task = kernel_task_current();
    *limits = (struct kernel_memory_limits){KERNEL_RLIM_INFINITY,
        KERNEL_RLIM_INFINITY, KERNEL_RLIM_INFINITY};
    if (!kernel_task_has_user_context(task)) return;
    /* 当前执行者保活组身份；接纳与Linux rlimit读取一样按字段取值。 */
    const struct kernel_resource_limits *record=task->limits;
    limits->address_space = __atomic_load_n(&record->values[KERNEL_RLIMIT_AS].current, __ATOMIC_RELAXED);
    limits->data_soft = __atomic_load_n(&record->values[KERNEL_RLIMIT_DATA].current, __ATOMIC_RELAXED);
    limits->data_hard = __atomic_load_n(&record->values[KERNEL_RLIMIT_DATA].maximum, __ATOMIC_RELAXED);
}

uint64_t kernel_task_current_memlock_limit(void)
{
    struct kernel_task *task = kernel_task_current();
    if (!kernel_task_has_user_context(task) ||
        kernel_credentials_capable(kernel_credentials_current(), KERNEL_CAP_IPC_LOCK)) return KERNEL_RLIM_INFINITY;
    return __atomic_load_n(&task->limits->values[KERNEL_RLIMIT_MEMLOCK].current, __ATOMIC_RELAXED);
}

uint64_t kernel_task_current_stack_limit(void)
{
    struct kernel_task *task = kernel_task_current();
    if (!kernel_task_has_user_context(task)) return KERNEL_RLIMIT_STACK_DEFAULT;
    return __atomic_load_n(&task->limits->values[KERNEL_RLIMIT_STACK].current,__ATOMIC_RELAXED);
}

enum kernel_task_status kernel_task_ppid(
    const struct kernel_task *task,
    kernel_pid_t *ppid)
{
    if (task == 0 || ppid == 0) {
        return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    }
    if (task->magic != KERNEL_THREAD_MAGIC || task->tid_owned != 1U ||
        task->tid <= 0 || task->arch.user_mode != 1U) {
        return KERNEL_TASK_STATUS_STATE;
    }
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    struct kernel_pid_registry *view = process_pid_active(task);
    task = task->group_leader;
    if (task->parent == 0) {
        *ppid = 0;
        return KERNEL_TASK_STATUS_OK;
    }
    const struct kernel_task *parent = task->parent->group_leader
        ? task->parent->group_leader : task->parent;
    if (parent->magic != KERNEL_THREAD_MAGIC ||
        parent->tid_owned != 1U || parent->tid <= 0) {
        return KERNEL_TASK_STATUS_STATE;
    }
    *ppid = kernel_pid_number_in(process_identity(parent, KERNEL_PID_TGID), view);
    return KERNEL_TASK_STATUS_OK;
}

void kernel_task_cpu_ticks(const struct kernel_task *task,
                           uint64_t *user_ticks, uint64_t *kernel_ticks,
                           uint64_t *child_user_ticks, uint64_t *child_kernel_ticks)
{
    if (!task || task->magic != KERNEL_THREAD_MAGIC) __builtin_trap();
    struct kernel_task_usage usage = {0}, children = {0};
    scheduler_usage_snapshot(task, 0, &usage);
    scheduler_usage_snapshot(task, -1, &children);
    *user_ticks = usage.user_ns / 10000000U;
    *kernel_ticks = usage.kernel_ns / 10000000U;
    *child_user_ticks = children.user_ns / 10000000U;
    *child_kernel_ticks = children.kernel_ns / 10000000U;
}

enum kernel_task_status kernel_task_mm_borrow(
    const struct kernel_task *task,
    const struct kernel_mm **mm)
{
    if (task == 0 || mm == 0) {
        return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    }
    if (task != kernel_cpu_current()->current ||
        task->magic != KERNEL_THREAD_MAGIC ||
        task->state != KERNEL_THREAD_STATE_RUNNING ||
        (task->arch.user_mode != 1U
#if BOAROS_MAX_CPUS > 1
         && !(task->controlled && task->bound_mm_context && task->arch.user_mode == 0U)
#endif
        ) ||
        task->mm.state != KERNEL_MM_LIVE) {
        return KERNEL_TASK_STATUS_STATE;
    }
    *mm = &task->mm;
    return KERNEL_TASK_STATUS_OK;
}

enum kernel_task_status kernel_task_mm_borrow_mutable(
    struct kernel_task *task,
    struct kernel_mm **mm)
{
    if (task == 0 || mm == 0) {
        return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    }
    if (task != kernel_cpu_current()->current ||
        task->magic != KERNEL_THREAD_MAGIC ||
        task->state != KERNEL_THREAD_STATE_RUNNING ||
        (task->arch.user_mode != 1U
#if BOAROS_MAX_CPUS > 1
         && !(task->controlled && task->bound_mm_context && task->arch.user_mode == 0U)
#endif
        ) ||
        task->mm.state != KERNEL_MM_LIVE) {
        return KERNEL_TASK_STATUS_STATE;
    }
    *mm = &task->mm;
    return KERNEL_TASK_STATUS_OK;
}

enum kernel_task_status validate_task_resource_borrow(
    const struct kernel_task *task)
{
    if (task != kernel_cpu_current()->current ||
        task->magic != KERNEL_THREAD_MAGIC ||
        task->state != KERNEL_THREAD_STATE_RUNNING ||
        task->arch.user_mode != 1U) {
        return KERNEL_TASK_STATUS_STATE;
    }
    if (task->files.state == KERNEL_FILES_EMPTY &&
        task->fs.state == KERNEL_FS_CONTEXT_EMPTY) {
        return KERNEL_TASK_STATUS_RESOURCE_UNAVAILABLE;
    }
    if (!kernel_files_is_live(&task->files) ||
        !kernel_fs_context_is_live(&task->fs)) {
        return KERNEL_TASK_STATUS_STATE;
    }
    return KERNEL_TASK_STATUS_OK;
}

int kernel_task_has_user_context(const struct kernel_task *task)
{
    return task && task==kernel_cpu_current()->current && task->magic==KERNEL_THREAD_MAGIC &&
        task->state==KERNEL_THREAD_STATE_RUNNING && task->arch.user_mode==1U;
}

enum kernel_task_status kernel_task_files_borrow(
    struct kernel_task *task,
    struct kernel_files **files)
{
    enum kernel_task_status status;

    if (task == 0 || files == 0) {
        return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    }
#if BOAROS_MAX_CPUS > 1
    if (task==kernel_cpu_current()->current && task->magic==KERNEL_THREAD_MAGIC &&
        task->state==KERNEL_THREAD_STATE_RUNNING && task->controlled &&
        task->bound_mm_context && task->arch.user_mode==0U) {
        if (task->files.state!=KERNEL_FILES_LIVE || !task->files.record)
            return KERNEL_TASK_STATUS_RESOURCE_UNAVAILABLE;
        *files=&task->files;
        return KERNEL_TASK_STATUS_OK;
    }
#endif
    status = validate_task_resource_borrow(task);
    if (status != KERNEL_TASK_STATUS_OK) {
        return status;
    }
    *files = &task->files;
    task->files.nofile_limit =
        (uint32_t)__atomic_load_n(&task->limits->values[KERNEL_RLIMIT_NOFILE].current, __ATOMIC_RELAXED);
    return KERNEL_TASK_STATUS_OK;
}

enum kernel_task_status kernel_task_file_io_borrow(
    struct kernel_task *task,struct kernel_files **files,struct kernel_mm **mm)
{
    if (!task || !files || !mm) return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
#if BOAROS_MAX_CPUS > 1
    if (!task->arch.user_mode) {
        enum kernel_task_status status=kernel_task_files_borrow(task,files);
        return status ? status : kernel_task_mm_borrow_mutable(task,mm);
    }
#endif
    enum kernel_task_status status=validate_task_resource_borrow(task);
    if (status) return status;
    if (task->mm.state!=KERNEL_MM_LIVE) return KERNEL_TASK_STATUS_STATE;
    /* This operation does not reserve a descriptor. The allocation limit is
     * sampled by the file-table entry points that can install a new FD. */
    *files=&task->files;
    *mm=&task->mm;
    return KERNEL_TASK_STATUS_OK;
}

enum kernel_task_status kernel_task_file_path_borrow(struct kernel_task *task,
    struct kernel_files **files,const struct kernel_fs_context **fs,struct kernel_mm **mm,
    bool allocates_fd)
{
    if (!task || !files || !fs || !mm) return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    enum kernel_task_status status=validate_task_resource_borrow(task);
    if (status) return status;
    if (task->mm.state!=KERNEL_MM_LIVE) return KERNEL_TASK_STATUS_STATE;
    if (allocates_fd) task->files.nofile_limit=(uint32_t)__atomic_load_n(
        &task->limits->values[KERNEL_RLIMIT_NOFILE].current,__ATOMIC_RELAXED);
    *files=&task->files;
    *fs=&task->fs;
    *mm=&task->mm;
    return KERNEL_TASK_STATUS_OK;
}

enum kernel_task_status kernel_task_fs_context_borrow(
    const struct kernel_task *task,
    const struct kernel_fs_context **fs)
{
    enum kernel_task_status status;

    if (task == 0 || fs == 0) {
        return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    }
    status = validate_task_resource_borrow(task);
    if (status != KERNEL_TASK_STATUS_OK) {
        return status;
    }
    *fs = &task->fs;
    return KERNEL_TASK_STATUS_OK;
}

/* All fallible preparation precedes undo consumption and publication. The
 * access qualification gives pidfd readers one combined FS/UTS/IPC/PID view. */
static void install_namespaces(struct kernel_task *task,struct kernel_fs_context *fs,
    struct kernel_namespace **uts,struct kernel_namespace **ipc,struct kernel_pid_registry *pid,
    int detach_sem,int shared_paths,int fresh_pid)
{
    struct kernel_fs_context previous={0};
    struct kernel_namespace *old_uts=0,*old_ipc=0;
    if(detach_sem)kernel_sem_exit(task);
    {
        KERNEL_RAW_SCOPE(access,&task->files_access);
        if(fs->state==KERNEL_FS_CONTEXT_LIVE) {
            if(shared_paths)kernel_fs_context_exchange_paths_namespace(&task->fs,fs,&previous);
            else kernel_fs_context_exchange(&task->fs,fs,&previous);
        }
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        if(fresh_pid) {
            /* The caller just created this child of its active namespace;
             * transfer the only reference, without a redundant get/put. */
            task->pid_for_children=pid;
        } else if(pid && process_pid_children_install_locked(task,pid))__builtin_trap();
        if(*uts){old_uts=task->uts_namespace;task->uts_namespace=*uts;*uts=0;}
        if(*ipc){old_ipc=task->ipc_namespace;task->ipc_namespace=*ipc;*ipc=0;}
    }
    if(previous.state==KERNEL_FS_CONTEXT_LIVE && kernel_fs_context_release(&previous))__builtin_trap();
    kernel_namespace_put(old_uts);kernel_namespace_put(old_ipc);
    if(pid && !fresh_pid)process_identity_collect();
}
int kernel_task_unshare_namespaces(struct kernel_task *task,uint64_t flags)
{
    const uint64_t namespaces=KERNEL_NS_MNT|KERNEL_NS_UTS|KERNEL_NS_IPC|KERNEL_NS_PID;
    if(flags&~(LINUX_CLONE_FS|LINUX_CLONE_THREAD|namespaces|LINUX_CLONE_SYSVSEM))return -KERNEL_EINVAL;
    if(!flags)return 0;
    /* In the pinned Linux ksys_unshare NEWPID alone does not imply THREAD. */
    if(flags&LINUX_CLONE_THREAD) {
        if(!(flags&KERNEL_NS_PID))return -KERNEL_EINVAL; /* No standalone thread unshare. */
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        if(task->group_leader->group_members!=1)return -KERNEL_EINVAL;
    }
    int denied=(flags&namespaces) &&
        !kernel_credentials_capable(kernel_credentials_current(),KERNEL_CAP_SYS_ADMIN);
    struct kernel_fs_context fs={0};struct kernel_namespace *uts=0,*ipc=0;
    struct kernel_pid_registry *pid=NULL;int error=0;
    /* NEWNS copies even exclusive FS state before the namespace privilege
     * check. On denial prepare only the FS record, never the mount tree. */
    if(denied && (flags&KERNEL_NS_MNT)) {
        enum kernel_fs_context_status status=kernel_fs_context_fork(&fs,&task->fs);
        if(status)error=status==KERNEL_FS_CONTEXT_STATUS_NO_MEMORY?-KERNEL_ENOMEM:-KERNEL_EINVAL;
    } else if(flags&(LINUX_CLONE_FS|KERNEL_NS_MNT))
        error=kernel_fs_context_prepare_unshare(&fs,&task->fs,!!(flags&KERNEL_NS_MNT));
    if(!error && denied)error=-KERNEL_EPERM;
    if(!error && (flags&KERNEL_NS_UTS))error=kernel_uts_clone(task->uts_namespace,task->files.heap,&uts);
    if(!error && (flags&KERNEL_NS_IPC))error=kernel_ipc_namespace_create(task->files.heap,&ipc);
    if(!error && (flags&KERNEL_NS_PID)) {
        /* The running caller owns its active identity throughout unshare. */
        struct kernel_pid_registry *active=process_pid_active(task);
        if(!active || task->pid_for_children)error=-KERNEL_EINVAL;
        else if(active->depth==KERNEL_PID_NAMESPACE_MAX_DEPTH)error=-KERNEL_ENOSPC;
        else error=process_pid_namespace_create(active,&scheduler.identity_heap,&pid);
    }
    if(!error)install_namespaces(task,&fs,&uts,&ipc,pid,!!(flags&(KERNEL_NS_IPC|LINUX_CLONE_SYSVSEM)),0,pid!=NULL);
    if(fs.state==KERNEL_FS_CONTEXT_LIVE && kernel_fs_context_release(&fs))__builtin_trap();
    kernel_namespace_put(uts);kernel_namespace_put(ipc);return error;
}
int kernel_task_setns(struct kernel_task *task,int32_t fd,uint32_t type)
{
    KERNEL_FILES_PIN_SCOPE(pin);
    struct kernel_open_file_description *file=kernel_files_hold_fd(&task->files,fd,&pin);
    if(!file)return -KERNEL_EBADF;
    const uint32_t all=KERNEL_NS_MNT|KERNEL_NS_UTS|KERNEL_NS_IPC|KERNEL_NS_PID;
    struct kernel_namespace *next[4]={0},*direct=kernel_namespace_file(file);
    uint32_t mask=type;int error=0;
    if(direct) {
        if(!(direct->type&all) || (type && type!=direct->type) || (kernel_open_file_flags(file)&010000000U))return -KERNEL_EINVAL;
        mask=direct->type;
        next[mask==KERNEL_NS_MNT?0:mask==KERNEL_NS_UTS?1:mask==KERNEL_NS_IPC?2:3]=kernel_namespace_get(direct);
    } else if(file->kind!=KERNEL_OPEN_FILE_KIND_PIDFD || !mask || (mask&~all))return -KERNEL_EINVAL;
    const struct kernel_credentials *cred=kernel_credentials_current();
    if(!kernel_credentials_capable(cred,KERNEL_CAP_SYS_ADMIN) ||
        ((mask&KERNEL_NS_MNT) && !kernel_credentials_capable(cred,KERNEL_CAP_SYS_CHROOT)))error=-KERNEL_EPERM;
    if(!error && !direct) {
        struct kernel_pid *id __attribute__((cleanup(kernel_task_identity_end)))=kernel_pidfd_identity_ref(file);
        error=id?kernel_proc_namespaces_identity_handle(id,mask,next):-KERNEL_ESRCH;
        if(error==-KERNEL_ENOENT)error=-KERNEL_ESRCH;
        else if(error==-KERNEL_EACCES)error=-KERNEL_EPERM;
    }
    struct kernel_pid_registry *pid=process_pid_namespace_registry(next[3]);
    if(!error && (mask&KERNEL_NS_PID)) {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        error=process_pid_children_check_locked(task,pid);
    }
    struct kernel_fs_context fs={0};
    int shared_paths=(mask&KERNEL_NS_MNT) && (mask&~KERNEL_NS_MNT);
    if(!error && next[0])error=kernel_fs_context_prepare_namespace(&fs,&task->fs,
        (struct kernel_mount_namespace *)next[0],shared_paths);
    if(!error)install_namespaces(task,&fs,&next[1],&next[2],pid,!!(mask&KERNEL_NS_IPC),shared_paths,0);
    if(fs.state==KERNEL_FS_CONTEXT_LIVE && kernel_fs_context_release(&fs))__builtin_trap();
    for(unsigned i=0;i<4;i++)kernel_namespace_put(next[i]);
    return error;
}

int kernel_task_prepare_user_return(void)
{
    struct kernel_cpu *cpu = kernel_cpu_current();
    if (kernel_cpu_schedule_requested(cpu)) {
        if (scheduler_reschedule(0, 0, 0) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
        cpu = kernel_cpu_current();
    }
    struct kernel_task *task = cpu->current;
    if (task == 0 || task->arch.user_mode != 1U) return 0;
    if (task->files.state==KERNEL_FILES_LIVE) (void)kernel_files_drain_file_cleanup(&task->files);
    kernel_mm_drain_sources(&task->mm);
    (void)kernel_vfs_reap_mounts();
    uint64_t address = task->set_tid_address;
    if (address) {
        task->set_tid_address = 0U;
        size_t copied;
        /* Each SETTID store uses its recipient's view. Faulting here does
         * not roll back the already-published clone. */
        kernel_pid_t tid = process_identity(task, KERNEL_PID_TID)->number;
        (void)kernel_copy_to_user(&task->mm, address, &tid, sizeof(tid), &copied);
    }
#if BOAROS_USER_SMP
    /* 业务准备已持有当前执行者；共用一次通知读取，普通返回不另作信号查询。 */
    uint32_t work=__atomic_load_n(&task->user_return_pending,__ATOMIC_ACQUIRE);
    int service=task->user_service || (work & KERNEL_USER_RETURN_CPU0);
    int signal=(work & KERNEL_USER_RETURN_SIGNAL)!=0;
    if (signal) (void)kernel_scheduler_take_signal_work();
    if (!service) return signal;
    kernel_scheduler_enter_user_service();
    kernel_scheduler_ack_user_service();
#endif
    return 1;
}

enum kernel_task_status kernel_task_socket_write_register(
    struct kernel_task *task, struct kernel_socket_write_request *request)
{
    if (!task || task != kernel_task_current() || !request || task->socket_write_request) return KERNEL_TASK_STATUS_STATE;
    task->socket_write_request = request;
    return KERNEL_TASK_STATUS_OK;
}
enum kernel_task_status kernel_task_socket_write_clear(
    struct kernel_task *task, struct kernel_socket_write_request *request)
{
    if (!task || !request || task->socket_write_request != request) return KERNEL_TASK_STATUS_STATE;
    task->socket_write_request = 0;
    return KERNEL_TASK_STATUS_OK;
}

/* 调用者持有调度域；共享执行安全状态与凭据在同一可观察边界更新。 */
void kernel_credentials_changed_locked(struct kernel_task *task,
    const struct kernel_credentials *old, const struct kernel_credentials *replacement)
{
    kernel_nproc_credentials_changed_locked(task,old,replacement);
    if (old->euid != replacement->euid || old->egid != replacement->egid ||
        old->fsuid != replacement->fsuid || old->fsgid != replacement->fsgid ||
        (replacement->permitted & ~old->permitted)) {
        task->parent_death_signal = 0;
        if (task->exec_security) kernel_exec_security_set_dumpable(task->exec_security, 0);
    }
}
int kernel_credentials_dumpable_get(struct kernel_task *task)
{
    return (int)kernel_exec_security_dumpable(task->exec_security);
}
void kernel_credentials_dumpable_set(struct kernel_task *task, unsigned value)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    kernel_exec_security_set_dumpable(task->exec_security, value);
}

unsigned kernel_task_fs_group_users(struct kernel_task *task,
    const struct kernel_fs_context_record *record)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    unsigned count = 0;
    struct kernel_task *member = task;
    do {
        count += member->fs.state == KERNEL_FS_CONTEXT_LIVE && member->fs.record == record;
        member = member->group_next;
    } while (member && member != task);
    return count;
}

void kernel_seccomp_terminate(unsigned signal,int group)
{
#if BOAROS_USER_SMP
    if (!kernel_scheduler_user_service_active()) kernel_scheduler_enter_user_service();
#endif
    struct kernel_task *task=kernel_task_current();
    struct kernel_thread_completion completion={.kind=KERNEL_THREAD_KIND_USER,
        .reason=KERNEL_THREAD_EXIT_SIGNAL,.tid=task->tid,.tgid=task->group_leader->tid,
        .status=signal};
    int single;
    {KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);single=task->group_leader->group_members==1;}
    if (signal==31 && (group || single)) completion.detail=process_core_dump(task,signal);
    if (group) process_group_request_exit(task,completion.reason,signal,completion.detail);
    kernel_thread_finish(&completion);
}
