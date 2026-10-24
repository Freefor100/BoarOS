#include "private.h"
#include <kernel/errno.h>
#include <kernel/socket_task.h>

static void identity_domain_owned(void)
{
    struct kernel_raw_guard *guard = kernel_cpu_current()->raw_locks;
    if (!guard || guard->lock != &kernel_wait_domain) __builtin_trap();
}

/* Disposal consumes a retired owner only after releasing the scheduler domain.
 * Heap preparation may sleep; reserve and final publication cannot. */
void process_identity_collect(void)
{
    struct kernel_pid *id;
    for (;;) {
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            id = kernel_pid_take_retired(&scheduler.identities);
        }
        if (!id) return;
        kernel_heap_release(&scheduler.identity_heap, id);
    }
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
    struct kernel_raw_guard *guard = kernel_cpu_current()->raw_locks;
    if (!guard || guard->lock != &kernel_wait_domain) __builtin_trap();
    struct kernel_pid *id = kernel_pid_find(&scheduler.identities, number);
    return id && id->members[role] ? id->members[role]->task : 0;
}

void process_identity_sync_locked(struct kernel_task *task)
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
    enum kernel_pid_status status;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        status = kernel_pid_reserve(&scheduler.identities, id);
        if (status == KERNEL_PID_STATUS_OK) {
            kernel_pid_attach(&task->identities[KERNEL_PID_TID], id, KERNEL_PID_TID, task);
            if (!thread_clone) {
                kernel_pid_attach(&task->identities[KERNEL_PID_TGID], id, KERNEL_PID_TGID, task);
                task->prepared_group = parent ? process_identity(parent, KERNEL_PID_PGID) : id;
                task->prepared_session = parent ? process_identity(parent, KERNEL_PID_SID) : id;
                if (parent) {
                    kernel_pid_get(task->prepared_group);
                    kernel_pid_get(task->prepared_session);
                }
            }
            kernel_pid_put(id);
            process_identity_sync_locked(task);
        }
    }
    if (status != KERNEL_PID_STATUS_OK)
        kernel_heap_release(&scheduler.identity_heap, id);
    return status;
}

void process_identity_commit_locked(struct kernel_task *task)
{
    identity_domain_owned();
    struct kernel_pid *id = task->identities[KERNEL_PID_TID].identity;
    if (!id) return; /* Kernel tasks have no PID identity. */
    if (task->prepared_group) {
        kernel_pid_attach(&task->identities[KERNEL_PID_PGID], task->prepared_group,
                          KERNEL_PID_PGID, task);
        if (task->prepared_group != id) kernel_pid_put(task->prepared_group);
        task->prepared_group = 0;
        kernel_pid_attach(&task->identities[KERNEL_PID_SID], task->prepared_session,
                          KERNEL_PID_SID, task);
        if (task->prepared_session != id) kernel_pid_put(task->prepared_session);
        task->prepared_session = 0;
    }
    if (kernel_pid_publish_reserved(id) != KERNEL_PID_STATUS_OK) __builtin_trap();
}

void process_identity_release_locked(struct kernel_task *task)
{
    identity_domain_owned();
    if (task->prepared_group) {
        struct kernel_pid *id = task->identities[KERNEL_PID_TID].identity;
        if (task->prepared_group != id) kernel_pid_put(task->prepared_group);
        if (task->prepared_session != id) kernel_pid_put(task->prepared_session);
        task->prepared_group = task->prepared_session = 0;
    }
    for (unsigned role = 0; role < KERNEL_PID_ROLES; role++)
        if (task->identities[role].identity) kernel_pid_detach(&task->identities[role]);
    if (task->child_creator) {
        kernel_pid_put(task->child_creator);
        task->child_creator = 0;
    }
    process_identity_sync_locked(task);
}


void process_identity_release(struct kernel_task *task)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    process_identity_release_locked(task);
}

void kernel_task_identity_put(struct kernel_pid *id)
{
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        kernel_pid_put(id);
    }
    process_identity_collect();
}

int kernel_task_socket_identity(int32_t pid, struct kernel_pid **owner)
{
    if (!owner) return -KERNEL_EINVAL;
    *owner = 0;
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    struct kernel_pid *identity = pid == 0
        ? process_identity(kernel_task_current(), KERNEL_PID_TGID)
        : kernel_pid_find(&scheduler.identities, pid);
    if (!identity) return -KERNEL_ESRCH;
    kernel_pid_get(identity);
    *owner = identity;
    return 0;
}
