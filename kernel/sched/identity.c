#include "private.h"
#include <kernel/errno.h>
#include <kernel/socket_task.h>
#include <kernel/namespace.h>

static void identity_domain_owned(void)
{
    if (!kernel_raw_lock_owned(&kernel_wait_domain)) __builtin_trap();
}

/* Disposal consumes a retired owner only after releasing the scheduler domain.
 * Heap preparation may sleep; reserve and final publication cannot. */
void process_identity_collect(void)
{
    int notify = 0;
    for (;;) {
        struct kernel_pid *id;
        struct kernel_pid_registry *space;
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            struct kernel_pid_registry **link = &scheduler.pid_shutdowns;
            while (*link) {
                struct kernel_pid_registry *pending = *link;
                if (pending->numbers->allocated != 1) { link = &pending->shutdown_next; continue; }
                struct kernel_task *reaper = pending->shutdown_reaper;
                if (!reaper || reaper->exit_queued || reaper->reaping ||
                    reaper->state != KERNEL_THREAD_STATE_EXITED) __builtin_trap();
                *link = pending->shutdown_next;
                pending->shutdown_next = NULL;
                exited_append_locked(reaper);
                notify = 1;
            }
            id = kernel_pid_take_retired(&scheduler.identities);
            space = kernel_pid_take_retired_namespace(&scheduler.identities);
        }
        if (!id && !space) break;
        if (id) kernel_heap_release(&scheduler.identity_heap, id);
        if (space && space->heap) kernel_heap_release(space->heap, space);
    }
    if (notify) kernel_scheduler_notify_cleanup();
}

/* Namespace storage has no reference back to an nsfs snapshot. A snapshot
 * owns the registry; task/identity owners can outlive all snapshots and later
 * open another one without resurrecting an atomic reference count. */
struct pid_namespace_storage {
    struct kernel_pid_registry registry;
    struct kernel_pid_allocator numbers;
    uint64_t bitmap[];
};

struct pid_namespace_snapshot {
    struct kernel_namespace base;
    struct kernel_pid_registry *registry;
};

int process_pid_namespace_create(struct kernel_pid_registry *parent,
    struct kernel_heap *heap, struct kernel_pid_registry **owner)
{
    if (!parent || !heap || !owner || *owner) return -KERNEL_EINVAL;
    struct pid_namespace_storage *storage = NULL;
    size_t size = sizeof(*storage) +
        KERNEL_PID_BITMAP_WORDS(parent->numbers->limit) * sizeof(uint64_t);
    if (kernel_heap_allocate_zeroed(heap, 1, size, (void **)&storage))
        return -KERNEL_ENOMEM;
    enum kernel_pid_status status = kernel_pid_allocator_init(&storage->numbers,
        storage->bitmap, parent->numbers->limit);
    if (status) __builtin_trap();
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        status = kernel_pid_registry_admit(parent);
        if (!status) status = kernel_pid_registry_init(&storage->registry,
            &storage->numbers, parent);
    }
    if (status) {
        kernel_heap_release(heap, storage);
        return status == KERNEL_PID_STATUS_EXHAUSTED ? -KERNEL_ENOSPC : -KERNEL_ENOMEM;
    }
    storage->registry.heap = heap;
    storage->registry.namespace_id = kernel_namespace_allocate_id();
    *owner = &storage->registry;
    return 0;
}

static void pid_namespace_snapshot_destroy(struct kernel_namespace *base)
{
    struct pid_namespace_snapshot *snapshot = (void *)base;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        kernel_pid_registry_put(snapshot->registry);
    }
    kernel_heap_release(base->heap, snapshot);
    process_identity_collect();
}

/* Caller holds a registry reference across allocation, never a raw lock.
 * Root-only users pay no snapshot allocation on ordinary clone or exit. */
int process_pid_namespace_snapshot(struct kernel_pid_registry *space,
    struct kernel_heap *heap, struct kernel_namespace **owner)
{
    if (!space || !heap || !owner || *owner) return -KERNEL_EINVAL;
    struct pid_namespace_snapshot *snapshot = NULL;
    if (kernel_heap_allocate_zeroed(heap, 1, sizeof(*snapshot), (void **)&snapshot))
        return -KERNEL_ENOMEM;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        kernel_pid_registry_get(space);
    }
    snapshot->registry = space;
    snapshot->base = (struct kernel_namespace) {
        .references = 1, .type = KERNEL_NS_PID,
        .id = space->parent ? space->namespace_id : KERNEL_NS_INITIAL_PID_ID,
        .heap = heap, .destroy = pid_namespace_snapshot_destroy,
    };
    if (!snapshot->base.id) __builtin_trap();
    *owner = &snapshot->base;
    return 0;
}

struct kernel_pid_registry *process_pid_namespace_registry(struct kernel_namespace *base)
{
    return base && base->type == KERNEL_NS_PID &&
        base->destroy == pid_namespace_snapshot_destroy ?
        ((struct pid_namespace_snapshot *)base)->registry : NULL;
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
    return kernel_pid_number_in(id, &scheduler.identities);
}

uint64_t process_identity_generation(const struct kernel_task *task)
{
    struct kernel_pid *id = process_identity(task, KERNEL_PID_TGID);
    return id ? id->generation : 0;
}

struct kernel_task *process_find_identity_in(kernel_pid_t number, enum kernel_pid_role role,
    struct kernel_pid_registry *view)
{
    identity_domain_owned();
    struct kernel_pid *id = kernel_pid_find(view, number);
    return id && id->members[role] ? id->members[role]->task : 0;
}

struct kernel_task *process_find_identity(kernel_pid_t number, enum kernel_pid_role role)
{ return process_find_identity_in(number, role, &scheduler.identities); }

void process_identity_sync_locked(struct kernel_task *task)
{
    task->tid = process_identity_number(task, KERNEL_PID_TID);
    task->tid_owned = task->tid != 0;
}

struct kernel_pid_registry *process_pid_active(const struct kernel_task *task)
{
    struct kernel_pid *id = process_identity(task, KERNEL_PID_TID);
    if (!id) id = process_identity(task, KERNEL_PID_TGID);
    return id ? id->registry : NULL;
}

struct kernel_pid_registry *process_pid_children_locked(const struct kernel_task *task)
{
    identity_domain_owned();
    return task->pid_for_children ? task->pid_for_children : process_pid_active(task);
}

int process_pid_children_check_locked(struct kernel_task *task, struct kernel_pid_registry *target)
{
    identity_domain_owned();
    struct kernel_pid_registry *active = process_pid_active(task);
    if (!active || !target || !target->managed || !target->references) return -KERNEL_EINVAL;
    struct kernel_pid_registry *ancestor = target;
    while (ancestor && ancestor->depth > active->depth) ancestor = ancestor->parent;
    return ancestor == active ? 0 : -KERNEL_EINVAL;
}

int process_pid_children_install_locked(struct kernel_task *task, struct kernel_pid_registry *target)
{
    int error = process_pid_children_check_locked(task, target);
    if (error) return error;
    struct kernel_pid_registry *active = process_pid_active(task);
    struct kernel_pid_registry *previous = task->pid_for_children;
    task->pid_for_children = target == active ? NULL : target;
    if (task->pid_for_children) kernel_pid_registry_get(task->pid_for_children);
    if (previous) kernel_pid_registry_put(previous);
    return 0;
}

struct kernel_task *process_pid_reaper_locked(struct kernel_pid_registry *space)
{
    identity_domain_owned();
    struct kernel_pid *id = space ? space->child_reaper : NULL;
    return id && id->members[KERNEL_PID_TGID] ? id->members[KERNEL_PID_TGID]->task : NULL;
}

void process_pid_reaper_disable_locked(struct kernel_task *leader)
{
    identity_domain_owned();
    struct kernel_pid_registry *space = process_pid_active(leader);
    if (space && space->managed && space->child_reaper == process_identity(leader, KERNEL_PID_TGID))
        kernel_pid_registry_disable(space);
}

struct kernel_task *process_pid_namespace_kill_target_locked(struct kernel_pid_registry *space,
    kernel_pid_t *after)
{
    identity_domain_owned();
    struct kernel_pid *id;
    while ((id = kernel_pid_next_published(space, *after))) {
        *after = kernel_pid_number_in(id, space);
        if (!id->members[KERNEL_PID_TGID] || id == space->child_reaper) continue;
        struct kernel_task *task = id->members[KERNEL_PID_TGID]->task;
        /* The TGID owner can already be EXITED while another member still
         * executes. Cleanup has not necessarily converted it to GROUP_DEAD. */
        if (task->accounted && !task->group_exiting &&
            (task->group_members > 1 ||
             (task->state != KERNEL_THREAD_STATE_ZOMBIE &&
              task->state != KERNEL_THREAD_STATE_EXITED &&
              task->state != KERNEL_THREAD_STATE_GROUP_DEAD)))
            return task;
    }
    return NULL;
}

int process_pid_namespace_defer_locked(struct kernel_task *leader)
{
    identity_domain_owned();
    struct kernel_pid_registry *space = process_pid_active(leader);
    if (!space || space->shutdown_reaper != leader || space->accepting ||
        !leader->reaping || !leader->exit_queued || space->shutdown_next)
        __builtin_trap();
    if (space->numbers->allocated == 1) return 0;
    exited_remove_locked(leader);
    leader->reaping = 0;
    space->shutdown_next = scheduler.pid_shutdowns;
    scheduler.pid_shutdowns = space;
    return 1;
}

int process_identity_admit_locked(const struct kernel_task *task)
{
    identity_domain_owned();
    struct kernel_pid *id = process_identity(task, KERNEL_PID_TID);
    if (!id) return 0;
    if (kernel_pid_publish_admit(id)) return -KERNEL_ENOMEM;
    if (id->registry->parent && !id->registry->child_reaper && id->number != 1)
        return -KERNEL_ENOMEM;
    return 0;
}

enum kernel_pid_status process_identity_create_numbers(struct kernel_task *task,
    struct kernel_task *parent, int thread_clone, const kernel_pid_t *requests, size_t count)
{
    if (count > 32 || (count && !requests)) return KERNEL_PID_STATUS_INVALID;
    kernel_pid_t requested = count ? requests[0] : 0;
    struct kernel_pid_registry *space;
    int pinned = 0;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        space = parent ? process_pid_children_locked(parent) : &scheduler.identities;
        if (!space || (thread_clone && space != process_pid_active(parent)))
            return KERNEL_PID_STATUS_INVALID;
        if (space != &scheduler.identities) { kernel_pid_registry_get(space); pinned = 1; }
    }
    struct kernel_pid *id = 0;
    process_identity_collect();
    size_t size = kernel_pid_storage_size(space);
    enum kernel_pid_status status = KERNEL_PID_STATUS_EXHAUSTED;
    if (kernel_heap_allocate_zeroed(&scheduler.identity_heap, 1, size,
                                    (void **)&id) != KERNEL_HEAP_STATUS_OK) goto unpin;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        if (space->parent && !space->child_reaper && requested > 1) {
            status = kernel_pid_registry_admit(space);
            if (!status) status = KERNEL_PID_STATUS_INVALID;
        } else status = kernel_pid_reserve_numbers(space, id, size, requests, count);
        if (status == KERNEL_PID_STATUS_OK) {
            kernel_pid_attach(&task->identities[KERNEL_PID_TID], id, KERNEL_PID_TID, task);
            if (!thread_clone)
                kernel_pid_attach(&task->identities[KERNEL_PID_TGID], id, KERNEL_PID_TGID, task);
            kernel_pid_put(id);
            process_identity_sync_locked(task);
        }
    }
unpin:
    if (pinned) {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        kernel_pid_registry_put(space);
    }
    if (status != KERNEL_PID_STATUS_OK) {
        if (id) kernel_heap_release(&scheduler.identity_heap, id);
        process_identity_collect(); /* A shutdown may be waiting for this reservation. */
    }
    return status;
}

enum kernel_pid_status process_identity_create_number(struct kernel_task *task,
    struct kernel_task *parent, int thread_clone, kernel_pid_t requested)
{
    return process_identity_create_numbers(task, parent, thread_clone,
        requested ? &requested : NULL, requested ? 1 : 0);
}

enum kernel_pid_status process_identity_create(struct kernel_task *task,
    struct kernel_task *parent, int thread_clone)
{ return process_identity_create_number(task, parent, thread_clone, 0); }

void process_identity_commit_locked(struct kernel_task *task)
{
    identity_domain_owned();
    struct kernel_pid *id = task->identities[KERNEL_PID_TID].identity;
    if (!id) return; /* Kernel tasks have no PID identity. */
    if (task->group_leader == task) {
        struct kernel_task *parent = task->parent;
        kernel_pid_attach(&task->identities[KERNEL_PID_PGID],
            parent ? process_identity(parent,KERNEL_PID_PGID) : id,KERNEL_PID_PGID,task);
        kernel_pid_attach(&task->identities[KERNEL_PID_SID],
            parent ? process_identity(parent,KERNEL_PID_SID) : id,KERNEL_PID_SID,task);
    }
    /* Clone admission and every effect below run in one identity-lock scope.
     * Boot task creation targets the initial registry before shutdown exists. */
    if (kernel_pid_publish_reserved(id) != KERNEL_PID_STATUS_OK) __builtin_trap();
    if (id->number == 1 && !id->registry->child_reaper) {
        id->registry->child_reaper = id;
        __atomic_store_n(&task->signal_unkillable,1,__ATOMIC_RELAXED);
    }
}

void process_identity_release_locked(struct kernel_task *task)
{
    identity_domain_owned();
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

void kernel_task_identity_retain(struct kernel_pid *id)
{
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    kernel_pid_get(id);
}

void kernel_task_identity_put(struct kernel_pid *id)
{
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        kernel_pid_put(id);
    }
    process_identity_collect();
}

int kernel_task_identity_get(int32_t pid, struct kernel_pid **owner)
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

int kernel_task_identity_get_user(int32_t pid,struct kernel_pid **owner)
{
    if(!owner)return -KERNEL_EINVAL;
    *owner=NULL;
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    struct kernel_task *caller=kernel_task_current();
    struct kernel_pid *id=pid ? kernel_pid_find(process_pid_active(caller),pid) :
        process_identity(caller,KERNEL_PID_TGID);
    if(!id)return -KERNEL_ESRCH;
    kernel_pid_get(id);*owner=id;return 0;
}

kernel_pid_t kernel_task_identity_user_number(const struct kernel_pid *id)
{
    /* The executing thread keeps its active view; a retained identity keeps
     * its immutable namespace bindings even after its final task role exits. */
    return kernel_pid_number_in(id,process_pid_active(kernel_task_current()));
}

kernel_pid_t kernel_task_lock_owner_user_number(kernel_pid_t number)
{
    if (number <= 0) return number; /* OFD locks have no process owner. */
    struct kernel_pid_registry *view = process_pid_active(kernel_task_current());
    if (view == &scheduler.identities) return number;
    /* A shared file table may outlive the original locker. Linux translates
     * the current binding of its saved integer, including after reuse. */
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    return kernel_pid_number_in(kernel_pid_find(&scheduler.identities, number), view);
}

void kernel_task_identity_replace_tgid(struct kernel_pid **owner,const struct kernel_task *task)
{
    struct kernel_pid *id=process_identity(task,KERNEL_PID_TGID);
    struct kernel_pid *old=*owner;
    if(old==id)return;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        if(id)kernel_pid_get(id);
        if(old)kernel_pid_put(old);
        *owner=id;
    }
    if(old)process_identity_collect();
}
