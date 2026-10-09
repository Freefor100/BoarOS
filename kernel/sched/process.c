#include <arch/task.h>
#include <arch/context.h>
#include <kernel/errno.h>
#include <kernel/epoll.h>
#include <kernel/exec.h>
#include <kernel/files.h>
#include <kernel/futex.h>
#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/tick.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/pid.h>
#include <kernel/scheduler.h>
#include <kernel/socket.h>
#include <kernel/signal.h>
#include <kernel/task.h>
#include <kernel/uaccess.h>
#include <kernel/tty_task.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../exec_internal.h"
#include "private.h"

#define LINUX_WNOHANG UINT32_C(0x00000001)
#define LINUX_WUNTRACED UINT32_C(0x00000002)
#define LINUX_WCONTINUED UINT32_C(0x00000008)
#define LINUX_WAIT_CONTINUED UINT32_C(0xffff)
#define LINUX___WNOTHREAD UINT32_C(0x20000000)
#define LINUX___WALL UINT32_C(0x40000000)
#define LINUX___WCLONE UINT32_C(0x80000000)
#define LINUX_WAIT4_SUPPORTED_OPTIONS \
    (LINUX_WNOHANG | LINUX_WUNTRACED | LINUX_WCONTINUED | \
     LINUX___WNOTHREAD | LINUX___WALL | LINUX___WCLONE)
#define LINUX_CLONE_VM UINT64_C(0x100)
#define LINUX_CLONE_FS UINT64_C(0x200)
#define LINUX_CLONE_VFORK UINT64_C(0x4000)
#define LINUX_CLONE_THREAD UINT64_C(0x10000)
#define LINUX_CLONE_SETTLS UINT64_C(0x80000)
#define LINUX_CLONE_PARENT_SETTID UINT64_C(0x100000)
#define LINUX_CLONE_CHILD_CLEARTID UINT64_C(0x200000)
#define LINUX_CLONE_CHILD_SETTID UINT64_C(0x1000000)

/* No allocation/yield is permitted between publication and role attachment.
 * SIE=0 alone is not a no-sleep promise: slab preparation precedes this region. */
void process_identity_collect(void)
{
    struct kernel_pid *id;
    while ((id = kernel_pid_take_retired(&scheduler.identities)) != 0)
        (void)kernel_heap_release(&scheduler.identity_heap, id);
}

struct kernel_pid *process_identity(const struct kernel_task *task, enum kernel_pid_role role)
{
    if (!task || (unsigned)role >= KERNEL_PID_ROLES) return 0;
    if (role != KERNEL_PID_TID) task = task->group_leader;
    return task ? task->identities[role].identity : 0;
}

kernel_pid_t process_identity_number(const struct kernel_task *task, enum kernel_pid_role role)
{
    struct kernel_pid *id = process_identity(task, role);
    return id ? id->number : 0;
}

uint64_t process_identity_generation(const struct kernel_task *task)
{
    struct kernel_pid *id = process_identity(task, KERNEL_PID_TGID);
    return id ? id->generation : 0;
}

struct kernel_task *process_find_identity(kernel_pid_t number, enum kernel_pid_role role)
{
    struct kernel_pid *id = kernel_pid_find(&scheduler.identities, number);
    return id && id->members[role] ? id->members[role]->task : 0;
}

static struct kernel_task *session_target(struct kernel_task *caller, kernel_pid_t pid)
{
    struct kernel_task *target = pid ? process_find_identity(pid, KERNEL_PID_TID) : caller;
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
    struct kernel_task *target = session_target(caller, pid);
    return target ? process_identity_number(target, KERNEL_PID_PGID) : -KERNEL_ESRCH;
}

int64_t kernel_task_getsid(struct kernel_task *caller, kernel_pid_t pid)
{
    struct kernel_task *target = session_target(caller, pid);
    return target ? process_identity_number(target, KERNEL_PID_SID) : -KERNEL_ESRCH;
}

int64_t kernel_task_setpgid(struct kernel_task *caller, kernel_pid_t pid, kernel_pid_t pgid)
{
    struct kernel_task *leader = caller->group_leader;
    if (!pid) pid = leader->tid;
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
        group = kernel_pid_find(&scheduler.identities, pgid);
        struct kernel_task *member = group && group->members[KERNEL_PID_PGID]
            ? group->members[KERNEL_PID_PGID]->task : 0;
        if (!member || process_identity(member, KERNEL_PID_SID) !=
                       process_identity(leader, KERNEL_PID_SID)) return -KERNEL_EPERM;
    }
    identity_change_role(target, KERNEL_PID_PGID, group);
    process_identity_collect();
    return 0;
}

int64_t kernel_task_setsid(struct kernel_task *caller)
{
    struct kernel_task *leader = caller->group_leader;
    struct kernel_pid *identity = process_identity(leader, KERNEL_PID_TGID);
    if (leader->session_leader || identity->members[KERNEL_PID_PGID]) return -KERNEL_EPERM;
    kernel_task_tty_clear(leader);
    leader->session_leader = 1;
    identity_change_role(leader, KERNEL_PID_SID, identity);
    identity_change_role(leader, KERNEL_PID_PGID, identity);
    process_identity_collect();
    return identity->number;
}

static int group_orphaned(struct kernel_pid *group, const struct kernel_task *ignored)
{
    if (!group) return 1;
    for (struct kernel_pid_member *link = group->members[KERNEL_PID_PGID]; link; link = link->next) {
        struct kernel_task *member = link->task;
        struct kernel_task *parent = member->parent;
        if (member == ignored || !parent || parent == scheduler.init_task ||
            (member->group_members == 1 &&
             (member->state == KERNEL_THREAD_STATE_EXITED ||
              member->state == KERNEL_THREAD_STATE_ZOMBIE ||
              member->state == KERNEL_THREAD_STATE_GROUP_DEAD))) continue;
        if (process_identity(parent, KERNEL_PID_PGID) != group &&
            process_identity(parent, KERNEL_PID_SID) == process_identity(member, KERNEL_PID_SID))
            return 0;
    }
    return 1;
}

int process_group_is_orphaned(const struct kernel_task *task)
{
    return group_orphaned(process_identity(task, KERNEL_PID_PGID), 0);
}

void process_orphan_notify(struct kernel_task *task, struct kernel_task *old_parent)
{
    /* 非组长线程回收会临时自指 group_leader；只有 TGID 成员代表进程。 */
    if (!task->identities[KERNEL_PID_TGID].identity) return;
    const struct kernel_task *ignored = old_parent ? 0 : task;
    struct kernel_task *parent = old_parent ? old_parent : task->parent;
    struct kernel_pid *group = process_identity(task, KERNEL_PID_PGID);
    if (!parent || process_identity(parent, KERNEL_PID_PGID) == group ||
        process_identity(parent, KERNEL_PID_SID) != process_identity(task, KERNEL_PID_SID) ||
        !group_orphaned(group, ignored)) return;
    int stopped = 0;
    for (struct kernel_pid_member *link = group->members[KERNEL_PID_PGID]; link; link = link->next)
        if (((struct kernel_task *)link->task)->group_stopped) stopped = 1;
    if (!stopped) return;
    /* 两轮发送确保整个组先登记 HUP，再执行 CONT 的恢复动作。 */
    for (unsigned sig = 1; sig <= 18; sig += 17)
        for (struct kernel_pid_member *link = group->members[KERNEL_PID_PGID]; link; link = link->next)
            (void)signal_send_kernel_group(link->task, sig);
}

static void identity_sync_cache(struct kernel_task *task)
{
    task->tid = process_identity_number(task, KERNEL_PID_TID);
    task->tid_owned = task->tid != 0;
}

enum kernel_pid_status process_identity_create(struct kernel_task *task,
    struct kernel_task *parent, int thread_clone)
{
    struct kernel_pid *id = 0;
    process_identity_collect();
    if (kernel_heap_allocate_zeroed(&scheduler.identity_heap, 1, sizeof(*id),
                                    (void **)&id) != KERNEL_HEAP_STATUS_OK)
        return KERNEL_PID_STATUS_EXHAUSTED;
    enum kernel_pid_status status = kernel_pid_publish(&scheduler.identities, id);
    if (status != KERNEL_PID_STATUS_OK) {
        (void)kernel_heap_release(&scheduler.identity_heap, id);
        return status;
    }
    kernel_pid_attach(&task->identities[KERNEL_PID_TID], id, KERNEL_PID_TID, task);
    if (!thread_clone) {
        kernel_pid_attach(&task->identities[KERNEL_PID_TGID], id, KERNEL_PID_TGID, task);
        kernel_pid_attach(&task->identities[KERNEL_PID_PGID],
            parent ? process_identity(parent, KERNEL_PID_PGID) : id, KERNEL_PID_PGID, task);
        kernel_pid_attach(&task->identities[KERNEL_PID_SID],
            parent ? process_identity(parent, KERNEL_PID_SID) : id, KERNEL_PID_SID, task);
    }
    kernel_pid_put(id);
    identity_sync_cache(task);
    return KERNEL_PID_STATUS_OK;
}

void process_identity_release(struct kernel_task *task)
{
    for (unsigned role = 0; role < KERNEL_PID_ROLES; role++)
        if (task->identities[role].identity) kernel_pid_detach(&task->identities[role]);
    if (task->child_creator) {
        kernel_pid_put(task->child_creator);
        task->child_creator = 0;
    }
    identity_sync_cache(task);
}

static void child_creator_change(struct kernel_task *child, struct kernel_pid *creator)
{
    if (creator) kernel_pid_get(creator);
    if (child->child_creator) kernel_pid_put(child->child_creator);
    child->child_creator = creator;
}

static void identity_adopt(struct kernel_task *task, struct kernel_task *leader)
{
    if (task->controlling_tty) kernel_task_tty_clear(task);
    task->controlling_tty = leader->controlling_tty;
    leader->controlling_tty = 0;
    process_identity_release(task);
    for (unsigned role = 0; role < KERNEL_PID_ROLES; role++)
        kernel_pid_transfer(&leader->identities[role], &task->identities[role], task);
    identity_sync_cache(task);
    identity_sync_cache(leader);
}

void process_group_initialize(struct kernel_task *task)
{
    task->group_next = task;
    task->group_previous = task;
    kernel_wait_queue_init(&task->group_wait_queue);
}

static struct kernel_pid *child_reaper_identity(struct kernel_task *leader,
                                     const struct kernel_task *departing)
{
    struct kernel_task *member = leader;
    do {
        if (member != departing && !member->terminate_requested &&
            member->state != KERNEL_THREAD_STATE_EXITED &&
            member->state != KERNEL_THREAD_STATE_ZOMBIE &&
            member->state != KERNEL_THREAD_STATE_GROUP_DEAD)
            return process_identity(member, KERNEL_PID_TID);
        member = member->group_next;
    } while (member != leader);
    return process_identity(leader, KERNEL_PID_TID);
}

static void request_thread_termination(struct kernel_task *task)
{
    if (task->state == KERNEL_THREAD_STATE_EXITED ||
        task->state == KERNEL_THREAD_STATE_ZOMBIE ||
        task->state == KERNEL_THREAD_STATE_GROUP_DEAD) return;
    task->terminate_requested = 1U;
    if (task->vfork_waiting && task->vfork_wait_child != 0) {
        /* Fatal cancellation severs both pointers before the parent's
         * task can disappear. The child retains its independent MM ref. */
        task->vfork_wait_child->vfork_parent = 0;
        task->vfork_wait_child->vfork_child = 0U;
        task->vfork_wait_child = 0;
        task->vfork_waiting = 0U;
        if (task->state == KERNEL_THREAD_STATE_BLOCKED &&
            task->wait_queue == &task->vfork_done_queue) {
            scheduler_wake_task(task, KERNEL_WAIT_SIGNALLED);
        }
    }
    if (task->state == KERNEL_THREAD_STATE_STOPPED) {
        stopped_unlink(task);
        task->state = KERNEL_THREAD_STATE_READY;
        ready_append(task);
    }
    (void)kernel_scheduler_wake_signal(task);
}

void process_group_request_exit(struct kernel_task *task,
                                enum kernel_thread_exit_reason reason,
                                uint64_t status, uint64_t detail)
{
    struct kernel_task *leader = task->group_leader;
    struct kernel_task *member = leader;
    if (!leader->group_exiting) {
        leader->group_exiting = 1U;
        leader->completion.reason = reason;
        leader->completion.status = status;
        leader->completion.detail = detail;
    }
    do {
        request_thread_termination(member);
        member = member->group_next;
    } while (member != leader);
    (void)kernel_wait_queue_wake_all(&leader->group_wait_queue);
}

void kernel_user_group_exit(enum kernel_thread_exit_reason reason,
                            uint64_t status, uint64_t detail)
{
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
    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED ||
        arch_interrupt_is_enabled() ||
        validate_current() != KERNEL_SCHEDULER_STATUS_OK ||
        kernel_cpu_current()->current == &scheduler.idle ||
        kernel_cpu_current()->current->state != KERNEL_THREAD_STATE_RUNNING ||
        kernel_cpu_current()->current->arch.user_mode != 1U) {
        return KERNEL_MM_STATUS_STATE;
    }
    return kernel_mm_resolve_user_fault(&kernel_cpu_current()->current->mm,
                                        virtual_address,
                                        access);
}

static enum kernel_scheduler_status validate_child_list(
    const struct kernel_task *parent,
    const struct kernel_task *required_child)
{
    const struct kernel_task *previous = 0;
    const struct kernel_task *child;
    uint32_t count = 0U;
    int found = required_child == 0;

    if (parent == 0 || parent->magic != KERNEL_THREAD_MAGIC ||
        parent->arch.user_mode != 1U || parent->tid_owned != 1U ||
        parent->tid <= 0 ||
        ((parent->first_child == 0) != (parent->last_child == 0)) ||
        (parent->first_child != 0 &&
         parent->first_child->previous_sibling != 0) ||
        (parent->last_child != 0 &&
         parent->last_child->next_sibling != 0)) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    for (child = parent->first_child;
         child != 0;
         child = child->next_sibling) {
        if (++count > KERNEL_PID_LIMIT || child == parent ||
            child->magic != KERNEL_THREAD_MAGIC || child->idle != 0U ||
            child->arch.user_mode != 1U || child->tid_owned != 1U ||
            child->tid <= 0 || child->parent != parent ||
            child->previous_sibling != previous ||
            child->publish_completion != 0U ||
            child->state < KERNEL_THREAD_STATE_READY ||
            child->state > KERNEL_THREAD_STATE_GROUP_DEAD) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
        if (child == required_child) {
            found = 1;
        }
        previous = child;
    }
    if (previous != parent->last_child || !found) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}

static enum kernel_scheduler_status validate_child_endpoints(
    const struct kernel_task *parent)
{
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

static void child_append(struct kernel_task *parent,
                         struct kernel_task *child)
{
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

static void child_remove(struct kernel_task *parent,
                         struct kernel_task *child)
{
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

void wake_waiting_parent(struct kernel_task *child)
{
    struct kernel_task *parent = child->parent;

    if (parent != 0) {
        (void)kernel_wait_queue_wake_all(&parent->child_exit_queue);
    }
}

/* Notifies a vfork-suspended parent that the child released its shared
 * address space (through exec or exit). */
void process_complete_vfork(struct kernel_task *thread)
{
    if (thread->vfork_child != 0U && thread->vfork_parent != 0) {
        struct kernel_task *parent = thread->vfork_parent;
        /* Failed exec preparation also frees its transaction, but leaves
         * the child in the shared MM. That is not vfork completion. */
        if (thread->mm.state == KERNEL_MM_LIVE &&
            thread->mm.record_page_address ==
                parent->mm.record_page_address) {
            return;
        }
        /* Consume this child's completion before waking; an exec followed
         * by exit must not complete the parent's next vfork. */
        thread->vfork_child = 0U;
        thread->vfork_parent = 0;
        parent->vfork_waiting = 0U;
        parent->vfork_wait_child = 0;
        (void)kernel_wait_queue_wake_one(
            &parent->vfork_done_queue);
    }
}

static void exited_append(struct kernel_task *thread)
{
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        thread->next = 0;
        if (scheduler.exited_tail) scheduler.exited_tail->next = thread;
        else scheduler.exited_head = thread;
        scheduler.exited_tail = thread;
    }
    (void)kernel_wait_queue_wake_all(&scheduler.cleanup_queue);
}

enum kernel_scheduler_status process_group_exec_current(void)
{
    struct kernel_task *task = kernel_cpu_current()->current;
    struct kernel_task *leader = task->group_leader;
    struct kernel_task *member;
    enum kernel_wait_wake_reason reason;
    enum kernel_scheduler_status status;

    /* The first prepared exec owns group teardown. A competing exec must
     * unwind its own prepared image through the ordinary exit cleanup. */
    if (leader->group_exiting || leader->group_execing ||
        task->terminate_requested)
        kernel_user_thread_exit(KERNEL_THREAD_EXIT_SYSCALL, 0U, 0U);
    leader->group_execing = 1U;
    member = leader;
    do {
        if (member != task) request_thread_termination(member);
        member = member->group_next;
    } while (member != leader);
    while (leader->group_members != (task == leader ? 1U : 2U) ||
           (task != leader && leader->state != KERNEL_THREAD_STATE_GROUP_DEAD)) {
        status = KERNEL_WAIT_RECHECK(&leader->group_wait_queue,
                                                 0U, 0, &reason,
                (leader->group_members != (task == leader ? 1U : 2U) || (task != leader && leader->state != KERNEL_THREAD_STATE_GROUP_DEAD)));
        if (status != KERNEL_SCHEDULER_STATUS_OK) return status;
        if (leader->group_exiting)
            kernel_user_thread_exit(KERNEL_THREAD_EXIT_SYSCALL, 0U, 0U);
    }
    if (task != leader) {
        struct kernel_pid *old_identity = process_identity(task, KERNEL_PID_TID);
        kernel_pid_get(old_identity);
        identity_adopt(task, leader);
        task->proc_start_ticks = leader->proc_start_ticks;
        task->parent = leader->parent;
        task->previous_sibling = leader->previous_sibling;
        task->next_sibling = leader->next_sibling;
        if (task->parent != 0) {
            if (task->previous_sibling != 0)
                task->previous_sibling->next_sibling = task;
            else task->parent->first_child = task;
            if (task->next_sibling != 0)
                task->next_sibling->previous_sibling = task;
            else task->parent->last_child = task;
        }
        task->first_child = leader->first_child;
        task->last_child = leader->last_child;
        for (member = task->first_child; member != 0;
             member = member->next_sibling) {
            member->parent = task;
            if (member->child_creator == old_identity)
                child_creator_change(member, process_identity(task, KERNEL_PID_TID));
        }
        kernel_pid_put(old_identity);
        task->session_leader = leader->session_leader;
        task->child_creator = leader->child_creator;
        leader->child_creator = 0;
        task->user_ticks += leader->user_ticks;
        task->kernel_ticks += leader->kernel_ticks;
        task->child_user_ticks = leader->child_user_ticks;
        task->child_kernel_ticks = leader->child_kernel_ticks;
        task->minor_faults += leader->minor_faults;
        task->major_faults += leader->major_faults;
        task->child_minor_faults = leader->child_minor_faults;
        task->child_major_faults = leader->child_major_faults;
        task->group_pending = leader->group_pending;
        kernel_signal_timer_adopt(task, leader);
        task->nofile_limit = leader->nofile_limit;
        task->stack_limit = leader->stack_limit;
        for (unsigned i = 0U; i < KERNEL_SIGNAL_COUNT; i++)
            task->group_sender[i] = leader->group_sender[i];
        for (unsigned i = 0U; i < KERNEL_SIGNAL_COUNT; i++)
            task->group_signal_code[i] = leader->group_signal_code[i];
        task->publish_completion = leader->publish_completion;
        if (scheduler.init_task == leader) scheduler.init_task = task;
        leader->parent = 0;
        leader->first_child = leader->last_child = 0;
        leader->previous_sibling = leader->next_sibling = 0;
        leader->publish_completion = 0U;
        leader->group_leader = 0;
        leader->group_members = 0U;
        leader->state = KERNEL_THREAD_STATE_EXITED;
        exited_append(leader);
        task->group_leader = task;
        task->group_members = 1U;
        process_group_initialize(task);
    }
    task->group_execing = 0U;
    return KERNEL_SCHEDULER_STATUS_OK;
}

static int release_clone_resources(struct kernel_task *thread)
{
    int cleanup_failed = 0;

    if (kernel_signal_release_table(thread) !=
        KERNEL_SIGNAL_STATUS_OK) {
        cleanup_failed = 1;
    }

    if (thread->files.state == KERNEL_FILES_LIVE ||
        thread->files.state == KERNEL_FILES_CLEANUP) {
        if (kernel_files_release(&thread->files) !=
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
        if (kernel_mm_release(&thread->mm) != KERNEL_MM_STATUS_OK) {
            cleanup_failed = 1;
        }
    }
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
    int cleanup_failed;
    enum kernel_scheduler_status stack_status;

    /* No failed construction has published a parent completion channel. */
    thread->vfork_parent = 0;
    thread->vfork_child = 0U;
    stack_status = release_task_stack(thread);
    if (stack_status != KERNEL_SCHEDULER_STATUS_OK) return stack_status;
    cleanup_failed = release_clone_resources(thread);

    process_identity_release(thread);
    thread->group_leader = 0;
    thread->group_members = 0U;
    process_identity_collect();
    if (!cleanup_failed) {
        scheduler_forget_task(thread);
        kernel_task_release_io_scratch(thread);
        (void)physical_page_release(scheduler.allocator,
                                  thread->physical_address);
    }
    if (cleanup_failed) {
        queue_abandoned_clone(thread);
    }
    *linux_result = linux_failure;
    return scheduler_failure;
}

enum kernel_scheduler_status arch_process_clone_current(
    const struct arch_trap_frame *parent_frame,
    uint64_t flags,
    uint64_t child_stack,
    uint64_t parent_tid,
    uint64_t tls,
    uint64_t child_tid,
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
    status = validate_queues();
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }
    parent = kernel_cpu_current()->current;
    if (parent == &scheduler.idle || parent->arch.user_mode != 1U ||
        parent->tid_owned != 1U ||
        parent_frame != (const struct arch_trap_frame *)(
                            parent->stack_high - sizeof(*parent_frame)) ||
        arch_frame_task(parent_frame) != (uintptr_t)parent ||
        arch_frame_pc(parent_frame) > UINT64_MAX - 4U) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    status = validate_child_endpoints(parent);
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }

    status = allocate_task_storage(&child);
    if (status == KERNEL_SCHEDULER_STATUS_NO_MEMORY) {
        *linux_result = -KERNEL_ENOMEM;
        return KERNEL_SCHEDULER_STATUS_OK;
    }
    if (status != KERNEL_SCHEDULER_STATUS_OK) return status;
    child->magic = KERNEL_THREAD_MAGIC;
    child->arch.user_mode = 1U;
    child->state = KERNEL_THREAD_STATE_EXITED;
    child->completion.kind = KERNEL_THREAD_KIND_USER;
    child->completion.reason = KERNEL_THREAD_EXIT_USER_FAULT;

    /* vfork shares the parent address space instead of cloning it; the
     * shared record reference keeps the parent's handle valid across the
     * child's exec or exit. */
    if (vfork || thread_clone) {
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
    if (parent->files.state != KERNEL_FILES_EMPTY) {
        files_status = thread_clone
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
        fs_status = (flags & LINUX_CLONE_FS) != 0U
            ? kernel_fs_context_acquire(&child->fs, &parent->fs)
            : kernel_fs_context_fork(&child->fs, &parent->fs);
        if (fs_status != KERNEL_FS_CONTEXT_STATUS_OK) {
            return finish_clone_failure(
                child,
                fs_status == KERNEL_FS_CONTEXT_STATUS_NO_MEMORY
                    ? -KERNEL_ENOMEM : -KERNEL_EAGAIN,
                linux_result,
                fs_status == KERNEL_FS_CONTEXT_STATUS_NO_MEMORY ||
                        fs_status ==
                            KERNEL_FS_CONTEXT_STATUS_CLEANUP_REQUIRED
                    ? KERNEL_SCHEDULER_STATUS_OK
                    : KERNEL_SCHEDULER_STATUS_INVALID_STATE);
        }
    }
    pid_status = process_identity_create(child, parent, thread_clone);
    if (pid_status != KERNEL_PID_STATUS_OK) {
        return finish_clone_failure(
            child,
            pid_status == KERNEL_PID_STATUS_EXHAUSTED
                ? -KERNEL_EAGAIN : -KERNEL_ENOMEM,
            linux_result,
            pid_status == KERNEL_PID_STATUS_EXHAUSTED
                ? KERNEL_SCHEDULER_STATUS_OK
                : KERNEL_SCHEDULER_STATUS_INVALID_STATE);
    }
    tid = child->tid;
    kernel_sched_policy_fork(&child->scheduling, &parent->scheduling);
    child->proc_start_ticks = kernel_tick_count();
    memcpy(child->comm, parent->comm, sizeof(child->comm));
    child->group_leader = child;
    child->group_members = 1U;
    child->fork_no_exec = 1U;
    child->nofile_limit = parent->group_leader->nofile_limit;
    child->stack_limit = parent->group_leader->stack_limit;
    process_group_initialize(child);
    child->publish_completion = 0U;
    child->vfork_child = vfork;
    child->vfork_parent = vfork ? parent : 0;
    child->completion.tid = tid;
    child->completion.tgid = tid;
    kernel_wait_queue_init(&child->child_exit_queue);
    kernel_wait_queue_init(&child->vfork_done_queue);
    signal_status = thread_clone ? kernel_signal_share(child, parent)
                                 : kernel_signal_fork(child, parent);
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
    mm_status = kernel_mm_context(&child->mm, &child_context);
    if (mm_status != KERNEL_MM_STATUS_OK) {
        return finish_clone_failure(
            child, -KERNEL_EAGAIN, linux_result,
            KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE);
    }
    arch_thread_set_mm(&child->arch, child_context);
    if ((flags & LINUX_CLONE_CHILD_CLEARTID) != 0U)
        child->clear_tid_address = child_tid;
    status = arch_process_prepare_clone(child, parent, parent_frame,
                                         child_stack,
                                         (flags & LINUX_CLONE_SETTLS) != 0U,
                                         tls);
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return finish_clone_failure(
            child, -KERNEL_EAGAIN, linux_result,
            KERNEL_SCHEDULER_STATUS_INVALID_STATE);
    }

    /* Linux does not roll clone back when a SETTID user store faults. */
    {
        size_t copied;
        if ((flags & LINUX_CLONE_PARENT_SETTID) != 0U)
            (void)kernel_copy_to_user(&parent->mm, parent_tid,
                                      &tid, sizeof(tid), &copied);
        if ((flags & LINUX_CLONE_CHILD_SETTID) != 0U)
            child->set_tid_address = child_tid;
    }
    if (thread_clone) {
        struct kernel_task *leader = parent->group_leader;
        child->group_leader = leader;
        child->group_members = 0U;
        child->group_next = leader;
        child->group_previous = leader->group_previous;
        leader->group_previous->group_next = child;
        leader->group_previous = child;
        leader->group_members++;
        child->completion.tgid = leader->tid;
    } else {
        child_creator_change(child, process_identity(parent, KERNEL_PID_TID));
        if (kernel_task_controlling_tty(parent) &&
            kernel_task_tty_set(child, kernel_task_controlling_tty(parent)))
            __builtin_trap();
        child_append(parent->group_leader, child);
    }
    kernel_mm_add_user(&child->mm);
#if BOAROS_COST_DIAGNOSTICS
    kernel_cost_inherit(&child->cost, &parent->cost);
#endif
    child->state = KERNEL_THREAD_STATE_READY;
    ready_append(child);
    *linux_result = tid;

    if (vfork) {
        /* Linux suspends the vfork parent inside the syscall until the
         * child execs or exits; the child wakes vfork_done_queue. */
        kernel_wait_queue_init(&parent->vfork_done_queue);
        parent->vfork_waiting = 1U;
        parent->vfork_wait_child = child;
        while (parent->vfork_waiting != 0U) {
            status = KERNEL_WAIT_RECHECK(&parent->vfork_done_queue,
                                                    0U, 0, &wake_reason,
                (parent->vfork_waiting != 0U));
            if (status != KERNEL_SCHEDULER_STATUS_OK) {
                return status;
            }
        }
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}

static int wait_child_matches(const struct kernel_task *parent,
                              const struct kernel_task *child,
                              int64_t pid)
{
    if (pid > 0) {
        return child->tid == pid;
    }
    if (pid == 0) {
        return process_identity_number(child, KERNEL_PID_PGID) == process_identity_number(parent, KERNEL_PID_PGID);
    }
    if (pid == -1) {
        return 1;
    }
    return process_identity_number(child, KERNEL_PID_PGID) == -pid;
}

static enum kernel_scheduler_status reap_waited_child(
    struct kernel_task *parent,
    struct kernel_task *child,
    uint64_t status_address,
    uint64_t rusage_address,
    int64_t *linux_result)
{
    kernel_pid_t pid = child->tid;
    uint32_t wait_status = child->wait_status;
    uint64_t user_ticks = child->user_ticks + child->child_user_ticks;
    uint64_t kernel_ticks = child->kernel_ticks + child->child_kernel_ticks;
    size_t copied = 0U;
    enum kernel_uaccess_status access_status = KERNEL_UACCESS_STATUS_OK;

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
    process_identity_release(child);
    parent->group_leader->child_user_ticks += user_ticks;
    parent->group_leader->child_kernel_ticks += kernel_ticks;
    parent->group_leader->child_minor_faults +=
        child->minor_faults + child->child_minor_faults;
    parent->group_leader->child_major_faults +=
        child->major_faults + child->child_major_faults;
    child_remove(parent->group_leader, child);
    if (scheduler.init_task == child) {
        scheduler.init_task = 0;
    }
    child->group_leader = 0;
    child->group_members = 0U;
    child->publish_completion = 0U;
    scheduler_forget_task(child);
    kernel_task_release_io_scratch(child);
    (void)physical_page_release(scheduler.allocator,
                              child->physical_address);

    if (status_address != 0U) {
        access_status = kernel_copy_to_user(&parent->mm,
                                            status_address,
                                            &wait_status,
                                            sizeof(wait_status),
                                            &copied);
    }
    if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
        *linux_result = -KERNEL_EFAULT;
        return KERNEL_SCHEDULER_STATUS_OK;
    }
    if (access_status != KERNEL_UACCESS_STATUS_OK ||
        (status_address != 0U && copied != sizeof(wait_status))) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    if (rusage_address != 0U) {
        struct kernel_linux_rusage rusage = {0};
        size_t rusage_copied = 0U;
        rusage.ru_utime.tv_sec = (int64_t)(user_ticks /
                                           KERNEL_TICKS_PER_SECOND);
        rusage.ru_utime.tv_usec =
            (int64_t)(user_ticks % KERNEL_TICKS_PER_SECOND) *
            (int64_t)(1000000U / KERNEL_TICKS_PER_SECOND);
        rusage.ru_stime.tv_sec = (int64_t)(kernel_ticks /
                                           KERNEL_TICKS_PER_SECOND);
        rusage.ru_stime.tv_usec =
            (int64_t)(kernel_ticks % KERNEL_TICKS_PER_SECOND) *
            (int64_t)(1000000U / KERNEL_TICKS_PER_SECOND);
        access_status = kernel_copy_to_user(&parent->mm,
                                            rusage_address,
                                            &rusage,
                                            sizeof(rusage),
                                            &rusage_copied);
        if (access_status != KERNEL_UACCESS_STATUS_OK ||
            rusage_copied != sizeof(rusage)) {
            *linux_result = -KERNEL_EFAULT;
            return KERNEL_SCHEDULER_STATUS_OK;
        }
    }
    *linux_result = pid;
    return KERNEL_SCHEDULER_STATUS_OK;
}

static enum kernel_scheduler_status report_wait_event(
    struct kernel_task *parent,
    struct kernel_task *child,
    uint32_t wait_status,
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
        struct kernel_linux_rusage rusage = {0};

        copied = 0U;
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
    *linux_result = child->tid;
    return KERNEL_SCHEDULER_STATUS_OK;
}

enum kernel_scheduler_status kernel_scheduler_wait4_current(
    int64_t pid,
    uint64_t status_address,
    uint32_t options,
    uint64_t rusage_address,
    int64_t *linux_result)
{
    struct kernel_task *parent;
    enum kernel_wait_wake_reason wake_reason = KERNEL_WAIT_WOKEN;
    enum kernel_scheduler_status status;

    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED) {
        return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (linux_result == 0 || arch_interrupt_is_enabled()) {
        return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    }
    if ((options & ~LINUX_WAIT4_SUPPORTED_OPTIONS) != 0U) {
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_SCHEDULER_STATUS_OK;
    }
    if (pid == INT32_MIN) {
        *linux_result = -KERNEL_ESRCH;
        return KERNEL_SCHEDULER_STATUS_OK;
    }

    for (;;) {
        struct kernel_task *child;
        struct kernel_task *previous = 0;
        uint32_t child_count = 0U;
        int matching_child = 0;

        status = validate_current();
        if (status != KERNEL_SCHEDULER_STATUS_OK) {
            return status;
        }
        status = validate_queues();
        if (status != KERNEL_SCHEDULER_STATUS_OK) {
            return status;
        }
        parent = kernel_cpu_current()->current;
        if (parent == &scheduler.idle || parent->arch.user_mode != 1U ||
            parent->tid_owned != 1U) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
        status = validate_child_endpoints(parent->group_leader);
        if (status != KERNEL_SCHEDULER_STATUS_OK) {
            return status;
        }
        for (child = parent->group_leader->first_child;
             child != 0;
             child = child->next_sibling) {
            if (++child_count > KERNEL_PID_LIMIT || child == parent ||
                child->magic != KERNEL_THREAD_MAGIC ||
                child->idle != 0U || child->arch.user_mode != 1U ||
                child->tid_owned != 1U || child->tid <= 0 ||
                child->parent != parent->group_leader ||
                child->previous_sibling != previous ||
                child->publish_completion != 0U ||
                child->state < KERNEL_THREAD_STATE_READY ||
                child->state > KERNEL_THREAD_STATE_GROUP_DEAD) {
                return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
            }
            previous = child;
            if ((options & LINUX___WNOTHREAD) != 0U &&
                child->child_creator != process_identity(parent, KERNEL_PID_TID)) continue;
            if ((options & LINUX___WCLONE) != 0U &&
                (options & LINUX___WALL) == 0U) {
                continue;
            }
            if (!wait_child_matches(parent, child, pid)) {
                continue;
            }
            matching_child = 1;
            if (child->state == KERNEL_THREAD_STATE_ZOMBIE) {
                return reap_waited_child(parent,
                                         child,
                                         status_address,
                                         rusage_address,
                                         linux_result);
            }
            if (child->group_stopped != 0U &&
                (options & LINUX_WUNTRACED) != 0U &&
                child->stop_notified == 0U) {
                status = report_wait_event(parent,
                                           child,
                                           child->wait_status,
                                           status_address,
                                           rusage_address,
                                           linux_result);
                if (status != KERNEL_SCHEDULER_STATUS_OK ||
                    *linux_result != child->tid) {
                    return status;
                }
                child->stop_notified = 1U;
                return KERNEL_SCHEDULER_STATUS_OK;
            }
            if (child->group_stopped == 0U &&
                (options & LINUX_WCONTINUED) != 0U &&
                child->continue_notified != 0U) {
                status = report_wait_event(parent,
                                           child,
                                           LINUX_WAIT_CONTINUED,
                                           status_address,
                                           rusage_address,
                                           linux_result);
                if (status != KERNEL_SCHEDULER_STATUS_OK ||
                    *linux_result != child->tid) {
                    return status;
                }
                child->continue_notified = 0U;
                return KERNEL_SCHEDULER_STATUS_OK;
            }
        }
        if (previous != parent->group_leader->last_child) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
        if (!matching_child) {
            *linux_result = -KERNEL_ECHILD;
            return KERNEL_SCHEDULER_STATUS_OK;
        }
        if ((options & LINUX_WNOHANG) != 0U) {
            *linux_result = 0;
            return KERNEL_SCHEDULER_STATUS_OK;
        }
        status = KERNEL_WAIT_RECHECK(&parent->group_leader->child_exit_queue,
                                                0U,
                                                1,
                                                &wake_reason,
                (matching_child));
        if (status != KERNEL_SCHEDULER_STATUS_OK) {
            return status;
        }
        if (wake_reason == KERNEL_WAIT_SIGNALLED) {
            kernel_signal_note_syscall_restart(parent);
            *linux_result = -KERNEL_ERESTARTSYS;
            return KERNEL_SCHEDULER_STATUS_OK;
        }
    }
}

static enum kernel_scheduler_status cleanup_user_task_resources(
    struct kernel_task *thread);
static uint32_t user_wait_status(const struct kernel_thread_completion *completion);
static enum kernel_scheduler_status reparent_children(struct kernel_task *parent);

int kernel_scheduler_can_sleep(void)
{
    return scheduler.initialized == KERNEL_SCHEDULER_INITIALIZED &&
           !kernel_cpu_current()->preempt_depth && !kernel_cpu_current()->raw_locks &&
           kernel_cpu_current()->current && kernel_cpu_current()->current != &scheduler.idle;
}
void kernel_scheduler_register_cleanup(void)
{
    if (arch_interrupt_is_enabled() || !kernel_scheduler_can_sleep() ||
        kernel_cpu_current()->current->arch.user_mode || scheduler.cleanup_task) __builtin_trap();
    scheduler.cleanup_task = kernel_cpu_current()->current;
}
void kernel_scheduler_wait_cleanup(uint64_t retry_deadline)
{
    if (arch_interrupt_is_enabled() || kernel_cpu_current()->current != scheduler.cleanup_task) __builtin_trap();
    if (!scheduler.exited_head || retry_deadline) {
        enum kernel_wait_wake_reason reason;
        if (KERNEL_WAIT_RECHECK(&scheduler.cleanup_queue, retry_deadline, 1, &reason,
                (!scheduler.exited_head || retry_deadline)) != KERNEL_SCHEDULER_STATUS_OK)
            __builtin_trap();
    }
}

void kernel_thread_join(struct kernel_thread_join *join)
{
    uintptr_t irq = arch_interrupt_save();
    if (!join || join->task == kernel_cpu_current()->current) __builtin_trap();
    while (join->task && join->task->state != KERNEL_THREAD_STATE_EXITED) {
        enum kernel_wait_wake_reason reason;
        if (KERNEL_WAIT_RECHECK(&join->waiters, 0, 0, &reason,
                (join->task && join->task->state != KERNEL_THREAD_STATE_EXITED))
                != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
    }
    if (join->task) {
        struct kernel_task *task = join->task;
        struct kernel_task **link = &scheduler.exited_head, *previous = 0;
        while (*link && *link != task) { previous = *link; link = &(*link)->next; }
        if (!*link || task->arch.user_mode || task->io_context.locks ||
            task->io_context.backend_depth || task->io_buffer) __builtin_trap();
        *link = task->next;
        if (scheduler.exited_tail == task) scheduler.exited_tail = previous;
        if (release_task_storage(task, KERNEL_SCHEDULER_STATUS_OK)
                != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
    }
    arch_interrupt_restore(irq);
}

int kernel_scheduler_reap_pending(void)
{
    return scheduler.exited_head != 0;
}

int kernel_scheduler_stop_users(void)
{
    int pending = 0;
    /* 仅关机收口枚举现有任务表；内核worker仍须服务用户退出中的I/O。 */
    for (struct kernel_task *task = scheduler.all_tasks; task;
         task = task->all_next) {
        if (task->arch.user_mode != 1U) continue;
        pending = 1;
        if (task->group_leader == task &&
            task->state != KERNEL_THREAD_STATE_EXITED &&
            task->state != KERNEL_THREAD_STATE_ZOMBIE)
            process_group_request_exit(task, KERNEL_THREAD_EXIT_SIGNAL, 9, 0);
    }
    return pending;
}

enum kernel_scheduler_status kernel_scheduler_reap_one(
    struct kernel_thread_completion *completion)
{
    struct kernel_thread_completion result;
    struct kernel_task *thread;
    struct kernel_task *next;
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
    if (scheduler.fatal_status != KERNEL_SCHEDULER_STATUS_OK) {
        return scheduler.fatal_status;
    }
    status = validate_current();
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }
    if (kernel_cpu_current()->current != (scheduler.cleanup_task ? scheduler.cleanup_task : &scheduler.idle)) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    status = validate_queues();
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }


    if (scheduler.exited_head == 0) {
        return KERNEL_SCHEDULER_STATUS_EMPTY;
    }

    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        thread = scheduler.exited_head;
        if (thread->wait.on_cpu || thread->wait.borrows)
            return KERNEL_SCHEDULER_STATUS_BUSY;
        next = thread->next;
    }
    if (thread->magic != KERNEL_THREAD_MAGIC ||
        thread->state != KERNEL_THREAD_STATE_EXITED ||
        thread->idle != 0U ||
        (thread->physical_address & BOAROS_PAGE_MASK) != 0U ||
        thread->publish_completion > 1U) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    /* Request handles may live on the old stack; release them before it. */
    if (thread->io_context.locks || thread->io_context.backend_depth) __builtin_trap();
    if (thread->epoll_wait_request) kernel_epoll_abort_wait(thread->epoll_wait_request);
    if (thread->socket_write_request) kernel_socket_abort_write(thread->socket_write_request);
    if (thread->socket_read_request) kernel_socket_abort_read(thread->socket_read_request);
    if (thread->io_buffer) kernel_task_io_buffer_release(thread->io_buffer);
    /* 阻塞TTY请求仍借用退出任务的栈；先消费真实owner，再归还栈页。 */
    if (thread->tty_request) kernel_tty_abort_request(thread->tty_request);
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
        /* Cleanup can sleep while other exits append behind this task. */
        next = thread->next;
        if (thread->group_leader == thread && thread->group_members > 1U) {
            struct kernel_task *child;
            for (child = thread->first_child; child != 0;
                 child = child->next_sibling)
                if (child->child_creator == process_identity(thread, KERNEL_PID_TID))
                    child_creator_change(child, child_reaper_identity(thread, thread));
            /* Keep the group identity until its last member has left. */
            scheduler.exited_head = next;
            if (next == 0) scheduler.exited_tail = 0;
            thread->next = 0;
            thread->state = KERNEL_THREAD_STATE_GROUP_DEAD;
            (void)kernel_wait_queue_wake_all(&thread->group_wait_queue);
            return KERNEL_SCHEDULER_STATUS_EMPTY;
        }
        if (thread->group_leader != 0 && thread->group_leader != thread) {
            struct kernel_task *leader = thread->group_leader;
            struct kernel_task *child;
            /* Children survive their creator thread. Reassign this wait
             * relationship before its TID can be recycled. */
            for (child = leader->first_child; child != 0;
                 child = child->next_sibling)
                if (child->child_creator == process_identity(thread, KERNEL_PID_TID))
                    child_creator_change(child, child_reaper_identity(leader, thread));
            leader->user_ticks += thread->user_ticks;
            leader->kernel_ticks += thread->kernel_ticks;
            leader->minor_faults += thread->minor_faults;
            leader->major_faults += thread->major_faults;
            thread->group_previous->group_next = thread->group_next;
            thread->group_next->group_previous = thread->group_previous;
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
                exited_append(leader);
                next = thread->next;
            }
            (void)kernel_wait_queue_wake_all(&leader->group_wait_queue);
            thread->group_leader = thread;
            thread->group_members = 1U;
            process_group_initialize(thread);
        }
        if (thread->group_leader == thread &&
            reparent_children(thread) != KERNEL_SCHEDULER_STATUS_OK)
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        if (thread->group_leader == thread && thread->controlling_tty) {
            if (thread->session_leader) kernel_tty_disassociate(thread->controlling_tty, 1);
            else kernel_task_tty_clear(thread);
        }
        /* GROUP_DEAD 的身份仍服务存活成员；最后成员退出才撤销 alarm。 */
        if (thread->group_leader == thread) kernel_signal_timer_cancel(thread);
        process_orphan_notify(thread, 0);
        /* Reparenting may append orphan zombies behind this tail. */
        next = thread->next;
        if (thread->parent != 0) {
            kernel_signal_notify_child_exit(thread);
            if (kernel_signal_child_autoreap(thread->parent)) {
                child_remove(thread->parent, thread);
            }
        }
        if (thread->parent != 0) {
            status = validate_child_list(thread->parent, thread);
            if (status != KERNEL_SCHEDULER_STATUS_OK) {
                return status;
            }
            scheduler.exited_head = next;
            if (next == 0) {
                scheduler.exited_tail = 0;
            }
            thread->next = 0;
            thread->state = KERNEL_THREAD_STATE_ZOMBIE;
            thread->publish_completion = 0U;
            return KERNEL_SCHEDULER_STATUS_EMPTY;
        }
        if (thread->tid_owned != 0U) {
            process_identity_release(thread);
            if (scheduler.init_task == thread) {
                scheduler.init_task = 0;
            }
            thread->group_leader = 0;
            thread->group_members = 0U;
        }
    } else {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    scheduler_forget_task(thread);
    kernel_task_release_io_scratch(thread);
    (void)physical_page_release(scheduler.allocator,
                              thread->physical_address);
    scheduler.exited_head = next;
    if (next == 0) {
        scheduler.exited_tail = 0;
    }
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
    if (scheduler.fatal_status == KERNEL_SCHEDULER_STATUS_OK) {
        scheduler.fatal_status = status;
    }
    if (scheduler.idle_context_saved != 0U &&
        kernel_cpu_current()->current != &scheduler.idle) {
        if (activate_thread_address_space(&scheduler.idle) !=
            KERNEL_SCHEDULER_STATUS_OK) {
            for (;;) {
                arch_cpu_wait();
            }
        }
        kernel_cpu_current()->current = &scheduler.idle;
        arch_fpu_switch(0, &scheduler.idle.fpu);
        arch_context_switch(&scheduler.discard_context,
                             &scheduler.idle.context);
    }

    for (;;) {
        arch_cpu_wait();
    }
}

static enum kernel_scheduler_status cleanup_user_task_resources(
    struct kernel_task *thread)
{
    enum kernel_signal_status signal_status;
    enum kernel_mm_status mm_status;
    enum kernel_exec_status exec_status;
    enum kernel_files_status files_status;
    enum kernel_fs_context_status fs_status;

    if (thread->epoll_wait_request) kernel_epoll_abort_wait(thread->epoll_wait_request);
    if (thread->tty_request) kernel_tty_abort_request(thread->tty_request);
    if (thread->socket_write_request != 0)
        kernel_socket_abort_write(thread->socket_write_request);
    if (thread->socket_read_request != 0)
        kernel_socket_abort_read(thread->socket_read_request);
    if (thread->io_buffer != 0)
        kernel_task_io_buffer_release(thread->io_buffer);

    signal_status = thread->group_leader == thread && thread->group_members > 1U
        ? KERNEL_SIGNAL_STATUS_OK : kernel_signal_release_table(thread);
    if (signal_status != KERNEL_SIGNAL_STATUS_OK) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }

    if (thread->exec_transaction != 0) {
        struct kernel_exec_transaction *transaction =
            thread->exec_transaction;
        struct kernel_heap *heap = transaction->heap;

        /* A sibling may terminate a thread after exec preparation. The
         * prepared image is still owned by this transaction and is aborted. */
        if (transaction->state == KERNEL_EXEC_TRANSACTION_PREPARED)
            transaction->state = KERNEL_EXEC_TRANSACTION_PREPARING;
        exec_status = kernel_exec_transaction_cleanup(transaction);
        if (exec_status != KERNEL_EXEC_STATUS_OK) {
            return exec_status == KERNEL_EXEC_STATUS_CLEANUP_REQUIRED
                       ? KERNEL_SCHEDULER_STATUS_RESOURCE_CLEANUP
                       : KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
        (void)kernel_heap_release(heap, transaction);
        thread->exec_transaction = 0;
    }
    if (thread->files.state == KERNEL_FILES_LIVE ||
        thread->files.state == KERNEL_FILES_CLEANUP) {
        files_status = kernel_files_release(&thread->files);
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
    if (thread->mm.state == KERNEL_MM_LIVE ||
        thread->mm.state == KERNEL_MM_CLEANUP) {
        mm_status = kernel_mm_release(&thread->mm);
        if (mm_status != KERNEL_MM_STATUS_OK) {
            if (mm_status == KERNEL_MM_STATUS_PAGE_ACCESS) {
                return KERNEL_SCHEDULER_STATUS_PAGE_ACCESS;
            }
            if (mm_status == KERNEL_MM_STATUS_CLEANUP_REQUIRED) {
                return KERNEL_SCHEDULER_STATUS_RESOURCE_CLEANUP;
            }
            return mm_status == KERNEL_MM_STATUS_ADDRESS_SPACE
                       ? KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE
                       : KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
    }
    if (thread->mm.state == KERNEL_MM_RELEASED) {
        process_complete_vfork(thread);
    }
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
        if (physical_page_resolve(allocator, physical, &data) !=
            PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
        if (task != 0) {
            task->io_scratch_allocator = allocator;
            task->io_scratch_physical_address = physical;
            task->io_scratch_data = data;
        }
    }
    *buffer = (struct kernel_task_io_buffer){task, allocator, physical, data};
    if (task != 0) task->io_buffer = buffer;
    return KERNEL_TASK_STATUS_OK;
}

void kernel_task_io_buffer_release(struct kernel_task_io_buffer *buffer)
{
    if (buffer == 0 || buffer->allocator == 0) __builtin_trap();
    if (buffer->task != 0) {
        if (buffer->task->io_buffer != buffer ||
            buffer->task->io_scratch_data == 0 ||
            buffer->task->io_scratch_physical_address !=
                buffer->physical_address) __builtin_trap();
        buffer->task->io_buffer = 0;
        /* The retained scratch page stays with the task until teardown. */
    } else if (physical_page_release(buffer->allocator,
                                     buffer->physical_address) !=
               PHYSICAL_PAGE_STATUS_OK) {
        __builtin_trap();
    }
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
    struct kernel_task *task, struct kernel_epoll_wait_request *request)
{
    if (!task || task != kernel_task_current() || !request || task->epoll_wait_request)
        return KERNEL_TASK_STATUS_STATE;
    task->epoll_wait_request = request;
    return KERNEL_TASK_STATUS_OK;
}

enum kernel_task_status kernel_task_epoll_clear(
    struct kernel_task *task, struct kernel_epoll_wait_request *request)
{
    if (!task || !request || task->epoll_wait_request != request)
        return KERNEL_TASK_STATUS_STATE;
    task->epoll_wait_request = 0;
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
        /* A core-producing default action does not mean a dump was written.
         * BoarOS has no core writer; Linux sets bit 7 only after a dump. */
        return (uint32_t)completion->status & UINT32_C(0x7f);
    }
    return fault_wait_status(completion->status);
}

static enum kernel_scheduler_status reparent_children(
    struct kernel_task *parent)
{
    struct kernel_task *new_parent = scheduler.init_task;
    struct kernel_task *child = parent->first_child;
    enum kernel_scheduler_status status;

    status = validate_child_list(parent, 0);
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }

    if (new_parent == parent || new_parent == 0 ||
        new_parent->tid_owned != 1U ||
        new_parent->state == KERNEL_THREAD_STATE_EXITED ||
        new_parent->state == KERNEL_THREAD_STATE_ZOMBIE) {
        new_parent = 0;
    }
    if (new_parent != 0) {
        status = validate_child_list(new_parent, 0);
        if (status != KERNEL_SCHEDULER_STATUS_OK) {
            return status;
        }
    }
    parent->first_child = 0;
    parent->last_child = 0;
    while (child != 0) {
        struct kernel_task *next = child->next_sibling;

        child->parent = 0;
        child_creator_change(child, 0);
        child->previous_sibling = 0;
        child->next_sibling = 0;
        if (new_parent != 0) {
            child_append(new_parent, child);
            child_creator_change(child, child_reaper_identity(new_parent, 0));
            if (child->state == KERNEL_THREAD_STATE_ZOMBIE) {
                kernel_signal_notify_child_exit(child);
                if (kernel_signal_child_autoreap(new_parent)) {
                    child_remove(new_parent, child);
                    child->state = KERNEL_THREAD_STATE_EXITED;
                    child->publish_completion = 0U;
                    exited_append(child);
                }
            }
        } else if (child->state == KERNEL_THREAD_STATE_ZOMBIE) {
            child->state = KERNEL_THREAD_STATE_EXITED;
            child->publish_completion = 0U;
            exited_append(child);
        }
        process_orphan_notify(child, parent);
        child = next;
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}

static void kernel_thread_finish(
    const struct kernel_thread_completion *completion)
    __attribute__((noreturn));

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
    if (status != KERNEL_SCHEDULER_STATUS_OK || current == &scheduler.idle) {
        switch_to_fatal_idle(
            status == KERNEL_SCHEDULER_STATUS_OK
                ? KERNEL_SCHEDULER_STATUS_INVALID_STATE
                : status);
    }
    status = validate_queues();
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        switch_to_fatal_idle(status);
    }

    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        if (current->wait.phase != KERNEL_WAIT_FINISHED || current->wait.nodes || current->wait.borrows)
            __builtin_trap();
    }
    /* proc readers must see departure before clear_child_tid wakes a joiner. */
#if BOAROS_COST_DIAGNOSTICS
    kernel_cost_task_exit();
#endif
    current->proc_exiting = 1U;
    if (!current->group_exiting) current->completion = *completion;
    if (current->arch.user_mode == 1U) {
        kernel_futex_release_robust(current, current->tid);
        kernel_futex_release_mm(current);
        if (arch_mmu_switch_context(scheduler.kernel_context) !=
            ARCH_MMU_STATUS_OK) {
            switch_to_fatal_idle(
                KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE);
        }
        if (!scheduler.cleanup_task) cleanup_status = cleanup_user_task_resources(current);
        current->wait_status = user_wait_status(&current->completion);
    }
    (void)cleanup_status;
    current->state = KERNEL_THREAD_STATE_EXITED;
    exited_append(current);
    if (current->join) (void)kernel_wait_queue_wake_all(&current->join->waiters);

    scheduler_account_runtime();
    struct kernel_cpu *cpu = kernel_cpu_current();
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        next = scheduler.cleanup_task ? ready_best_locked() : 0;
        if (!next) next = &scheduler.idle;
        if (cpu->switch_previous || next->wait.on_cpu || !current->wait.on_cpu) __builtin_trap();
        if (next != &scheduler.idle) {
            ready_remove_locked(next);
            next->state = KERNEL_THREAD_STATE_RUNNING;
        }
        next->wait.on_cpu = 1;
        next->cpu = cpu;
        cpu->current = next;
        cpu->switch_previous = current;
        cpu->rotate_other = 0;
    }
    status = activate_thread_address_space(next);
    if (status != KERNEL_SCHEDULER_STATUS_OK) switch_to_fatal_idle(status);
#if BOAROS_COST_DIAGNOSTICS
    kernel_cost_switch(&current->cost, &next->cost);
#endif
    scheduler_rearm_timer();
    arch_fpu_switch(0, &next->fpu);
    arch_context_switch(&scheduler.discard_context, &next->context);
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
    task->clear_tid_address = address;
    *tid = task->tid;
    return KERNEL_TASK_STATUS_OK;
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

enum kernel_task_status kernel_task_tgid(
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

enum kernel_task_status kernel_task_get_rlimit(
    const struct kernel_task *task, uint32_t resource,
    struct kernel_rlimit64 *limit)
{
    const struct kernel_task *leader;
    kernel_pid_t tgid;

    if (limit == 0 || (resource != KERNEL_RLIMIT_NOFILE &&
                       resource != KERNEL_RLIMIT_STACK))
        return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    if (kernel_task_tgid(task, &tgid) != KERNEL_TASK_STATUS_OK)
        return KERNEL_TASK_STATUS_STATE;
    leader = task->group_leader;
    *limit = resource == KERNEL_RLIMIT_NOFILE ? leader->nofile_limit :
             leader->stack_limit;
    return KERNEL_TASK_STATUS_OK;
}

enum kernel_task_status kernel_task_set_rlimit(
    struct kernel_task *task, uint32_t resource,
    const struct kernel_rlimit64 *limit)
{
    kernel_pid_t tgid;

    if (limit == 0 || limit->current > limit->maximum ||
        (resource == KERNEL_RLIMIT_NOFILE &&
         limit->maximum > KERNEL_RLIMIT_NOFILE_CAP) ||
        (resource == KERNEL_RLIMIT_STACK &&
         limit->maximum > KERNEL_RLIMIT_STACK_CAP) ||
        (resource != KERNEL_RLIMIT_NOFILE &&
         resource != KERNEL_RLIMIT_STACK))
        return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    if (kernel_task_tgid(task, &tgid) != KERNEL_TASK_STATUS_OK)
        return KERNEL_TASK_STATUS_STATE;
    if (resource == KERNEL_RLIMIT_NOFILE)
        task->group_leader->nofile_limit = *limit;
    else task->group_leader->stack_limit = *limit;
    return KERNEL_TASK_STATUS_OK;
}

uint64_t kernel_task_current_stack_limit(void)
{
    struct kernel_task *task = kernel_task_current();
    struct kernel_rlimit64 limit;

    if (task == 0 || task->arch.user_mode != 1U)
        return KERNEL_RLIMIT_STACK_CAP;
    if (kernel_task_get_rlimit(task, KERNEL_RLIMIT_STACK, &limit) !=
        KERNEL_TASK_STATUS_OK) __builtin_trap();
    return limit.current;
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
    task = task->group_leader;
    if (task->parent == 0) {
        *ppid = 0;
        return KERNEL_TASK_STATUS_OK;
    }
    if (task->parent->magic != KERNEL_THREAD_MAGIC ||
        task->parent->tid_owned != 1U || task->parent->tid <= 0) {
        return KERNEL_TASK_STATUS_STATE;
    }
    *ppid = task->parent->tid;
    return KERNEL_TASK_STATUS_OK;
}

void kernel_task_cpu_ticks(const struct kernel_task *task,
                           uint64_t *user_ticks,
                           uint64_t *kernel_ticks,
                           uint64_t *child_user_ticks,
                           uint64_t *child_kernel_ticks)
{
    if (task == 0 || task->magic != KERNEL_THREAD_MAGIC) {
        return;
    }
    *user_ticks = task->user_ticks;
    *kernel_ticks = task->kernel_ticks;
    *child_user_ticks = task->child_user_ticks;
    *child_kernel_ticks = task->child_kernel_ticks;
    if (task->group_leader != 0) {
        const struct kernel_task *leader = task->group_leader;
        const struct kernel_task *member = leader;
        *user_ticks = 0U;
        *kernel_ticks = 0U;
        do {
            *user_ticks += member->user_ticks;
            *kernel_ticks += member->kernel_ticks;
            member = member->group_next;
        } while (member != leader);
        *child_user_ticks = leader->child_user_ticks;
        *child_kernel_ticks = leader->child_kernel_ticks;
    }
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
        task->arch.user_mode != 1U ||
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
        task->arch.user_mode != 1U ||
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

enum kernel_task_status kernel_task_files_borrow(
    struct kernel_task *task,
    struct kernel_files **files)
{
    enum kernel_task_status status;

    if (task == 0 || files == 0) {
        return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    }
    status = validate_task_resource_borrow(task);
    if (status != KERNEL_TASK_STATUS_OK) {
        return status;
    }
    *files = &task->files;
    task->files.nofile_limit =
        (uint32_t)task->group_leader->nofile_limit.current;
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

void kernel_task_prepare_user_return(void)
{
    if (kernel_cpu_schedule_requested(kernel_cpu_current()) && scheduler_reschedule(0, 0) != KERNEL_SCHEDULER_STATUS_OK)
        __builtin_trap();
    struct kernel_task *task = kernel_cpu_current()->current;
    if (task == 0 || task->arch.user_mode != 1U) return;
    uint64_t address = task->set_tid_address;
    task->set_tid_address = 0U;
    if (address) {
        size_t copied;
        /* 在子 MM 激活后处理 COW；坏地址不撤销已发布的 clone。 */
        (void)kernel_copy_to_user(&task->mm, address, &task->tid,
                                  sizeof(task->tid), &copied);
    }
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
