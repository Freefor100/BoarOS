#include <kernel/sem.h>
#include <kernel/avl.h>
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/raw_lock.h>
#include <kernel/socket_task.h>
#include <kernel/task_state.h>
#include <kernel/time.h>
#include <kernel/uaccess.h>
#include <kernel/wait_internal.h>
#include <stddef.h>
#include <string.h>

/* The index only owns discovery and namespace accounting. Values, requests and
 * undo adjustments belong to a set, so unrelated sets never share their lock.
 * Lock nesting is index(25) -> set(30) -> undo membership(35) -> scheduler(40). */
struct sem_set;
struct sem_undo_entry {
    struct sem_undo_entry *group_next, *group_previous, *set_next, *set_previous;
    uint32_t references;
    struct kernel_sem_undo *group;
    struct sem_set *set;
    int32_t adjustment[];
};
struct kernel_sem_undo {
    struct kernel_raw_lock lock;
    uint32_t owners;
    struct sem_undo_entry *entries;
};
struct sem_member { int32_t value; struct kernel_pid *pid; };
struct sem_set {
    struct kernel_avl_node slot_node, key_node;
    struct kernel_raw_lock lock;
    uint32_t references, removed, nsems;
    int32_t id;
    struct kernel_ipc64_perm permission;
    int64_t otime, ctime;
    struct sem_undo_entry *undo;
    struct kernel_sem_request *head, *tail;
    struct sem_member members[];
};
struct kernel_sem_request {
    struct sem_set *set;
    struct kernel_task *task;
    struct kernel_pid *pid;
    struct sem_undo_entry *undo;
    void *buffer;
    struct kernel_sem_request *previous, *next, *wake_next;
    struct kernel_wait_token token;
    uint32_t references, count, blocking, queued, prepared, alter;
    int result;
    struct kernel_sembuf ops[];
};
static struct {
    struct kernel_raw_lock lock;
    struct kernel_heap *heap;
    struct kernel_avl_node *slots, *keys;
    uint32_t sets, members, next_slot;
    uint16_t sequence[KERNEL_SEMMNI];
} sem_table;

static void *sem_allocate(size_t size)
{
    void *p = 0;
    enum kernel_heap_status status = kernel_heap_allocate_zeroed(sem_table.heap, 1, size, &p);
    if (status == KERNEL_HEAP_STATUS_EMPTY) return 0;
    if (status != KERNEL_HEAP_STATUS_OK) __builtin_trap();
    return p;
}
static void sem_free(void *p) { if (p) kernel_heap_release(sem_table.heap, p); }
static int64_t sem_now(void) { return (int64_t)(kernel_time_realtime_ns() / UINT64_C(1000000000)); }
static struct sem_set *slot_set(struct kernel_avl_node *n)
{ return n ? (struct sem_set *)((char *)n - offsetof(struct sem_set, slot_node)) : 0; }
static struct sem_set *key_set(struct kernel_avl_node *n)
{ return n ? (struct sem_set *)((char *)n - offsetof(struct sem_set, key_node)) : 0; }
static struct kernel_avl_node *index_find(struct kernel_avl_node *n, uint64_t key)
{
    while (n && n->key != key) n = key < n->key ? n->left : n->right;
    return n;
}
static void set_get(struct sem_set *set)
{
    uint32_t old = __atomic_fetch_add(&set->references, 1, __ATOMIC_RELAXED);
    if (!old || old == UINT32_MAX) __builtin_trap();
}
static void set_put(struct sem_set *set)
{
    if (__atomic_sub_fetch(&set->references, 1, __ATOMIC_ACQ_REL)) return;
    if (!set->removed || set->head || set->undo) __builtin_trap();
    for (uint32_t i = 0; i < set->nsems; i++)
        if (set->members[i].pid) kernel_task_identity_put(set->members[i].pid);
    sem_free(set);
}
static struct sem_set *set_lookup(int32_t id, int by_slot)
{
    if (id < 0) return 0;
    KERNEL_RAW_SCOPE(guard, &sem_table.lock);
    uint32_t slot = by_slot ? (uint32_t)id : ((uint32_t)id & 32767U);
    struct sem_set *set = slot_set(index_find(sem_table.slots, slot));
    if (set && (by_slot || set->id == id)) set_get(set);
    else set = 0;
    return set;
}
static void set_descriptor(const struct sem_set *set, struct kernel_semid64_ds *d)
{
    *d = (struct kernel_semid64_ds){.sem_perm = set->permission,
        .sem_otime = set->otime, .sem_ctime = set->ctime, .sem_nsems = set->nsems};
}
int kernel_sem_proc_next(uint32_t *cursor, int32_t *id, struct kernel_semid64_ds *row)
{
    KERNEL_RAW_SCOPE(index, &sem_table.lock);
    struct kernel_avl_node *node = *cursor
        ? kernel_avl_after(sem_table.slots, (uint64_t)*cursor - 1U)
        : kernel_avl_first(sem_table.slots);
    if (!node) return 0;
    struct sem_set *set = slot_set(node);
    KERNEL_RAW_SCOPE(state, &set->lock);
    if (set->removed) __builtin_trap();
    *cursor = (uint32_t)node->key + 1U;
    *id = set->id;
    set_descriptor(set, row);
    return 1;
}
static int permission(const struct sem_set *set, const struct kernel_credentials *cred,
    unsigned requested)
{
    const struct kernel_ipc64_perm *p = &set->permission;
    return kernel_ipc_permission(cred, p->mode, p->uid, p->gid, p->cuid, p->cgid, requested);
}
static int is_owner(const struct sem_set *set, const struct kernel_credentials *cred)
{
    return kernel_ipc_owner(cred, set->permission.uid, set->permission.cuid);
}

static struct kernel_pid *task_pid_get(struct kernel_task *task)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    struct kernel_task *leader = task->group_leader ? task->group_leader : task;
    struct kernel_pid *pid = leader->identities[KERNEL_PID_TGID].identity;
    if (pid) kernel_pid_get(pid);
    return pid;
}
/* Caller also owns the scheduler identity domain. Registry retirement is
 * collected when the operation's own PID reference is put outside raw locks. */
static void member_pid_set(struct sem_member *member, struct kernel_pid *pid)
{
    if (member->pid == pid) return;
    kernel_pid_get(pid);
    if (member->pid) kernel_pid_put(member->pid);
    member->pid = pid;
}
int kernel_sem_init(struct kernel_heap *heap)
{
    if (!heap) return -KERNEL_EINVAL;
    if (sem_table.heap) return sem_table.heap == heap ? 0 : -KERNEL_EINVAL;
    kernel_raw_lock_init(&sem_table.lock, 25);
    sem_table.heap = heap;
    return 0;
}

int64_t kernel_sem_get(struct kernel_task *task, int32_t key, int32_t nsems, int32_t flags)
{
    (void)task;
    if (!sem_table.heap) return -KERNEL_ENOSPC;
    if (nsems < 0 || nsems > (int32_t)KERNEL_SEMMSL) return -KERNEL_EINVAL;
    const struct kernel_credentials *cred = kernel_credentials_current();
    struct sem_set *candidate = 0;
    int64_t result;
    for (;;) {
        struct sem_set *found = 0;
        {
            KERNEL_RAW_SCOPE(guard, &sem_table.lock);
            if (key != KERNEL_IPC_PRIVATE)
                found = key_set(index_find(sem_table.keys, (uint32_t)key));
            if (found) set_get(found);
            else if (key != KERNEL_IPC_PRIVATE && !(flags & KERNEL_IPC_CREAT)) result = -KERNEL_ENOENT;
            else if (!nsems) result = -KERNEL_EINVAL;
            else if (sem_table.sets == KERNEL_SEMMNI ||
                     (uint32_t)nsems > KERNEL_SEMMNS - sem_table.members) result = -KERNEL_ENOSPC;
            else if (!candidate) result = INT64_MIN;
            else {
                uint32_t slot = sem_table.next_slot;
                while (index_find(sem_table.slots, slot)) slot = (slot + 1) % KERNEL_SEMMNI;
                sem_table.next_slot = (slot + 1) % KERNEL_SEMMNI;
                candidate->slot_node.key = slot;
                candidate->key_node.key = (uint32_t)key;
                candidate->permission.seq = sem_table.sequence[slot];
                candidate->id = (int32_t)(((uint32_t)candidate->permission.seq << 15) | slot);
                sem_table.slots = kernel_avl_insert_node(sem_table.slots, &candidate->slot_node);
                if (key != KERNEL_IPC_PRIVATE)
                    sem_table.keys = kernel_avl_insert_node(sem_table.keys, &candidate->key_node);
                sem_table.sets++;
                sem_table.members += (uint32_t)nsems;
                result = candidate->id;
                candidate = 0; /* The namespace now owns the prepared reference. */
            }
        }
        if (found) {
            {
                KERNEL_RAW_SCOPE(guard, &found->lock);
                if (found->removed) result = -KERNEL_EIDRM;
                else if ((flags & (KERNEL_IPC_CREAT | KERNEL_IPC_EXCL)) ==
                         (KERNEL_IPC_CREAT | KERNEL_IPC_EXCL)) result = -KERNEL_EEXIST;
                else if ((uint32_t)nsems > found->nsems) result = -KERNEL_EINVAL;
                else {
                    unsigned access = (unsigned)flags & 0777;
                    int error = permission(found, cred, (access | (access >> 3) | (access >> 6)) & 7);
                    result = error ? error : found->id;
                }
            }
            set_put(found);
        }
        if (found || result != INT64_MIN) break;
        candidate = sem_allocate(sizeof(*candidate) + (size_t)nsems * sizeof(candidate->members[0]));
        if (!candidate) { result = -KERNEL_ENOMEM; break; }
        kernel_raw_lock_init(&candidate->lock, 30);
        candidate->references = 1;
        candidate->slot_node.height = candidate->key_node.height = 1;
        candidate->nsems = (uint32_t)nsems;
        candidate->ctime = sem_now();
        candidate->permission = (struct kernel_ipc64_perm){ .key = key,
            .uid = cred->euid, .cuid = cred->euid, .gid = cred->egid, .cgid = cred->egid,
            .mode = (uint32_t)flags & 0777 };
    }
    sem_free(candidate);
    return result;
}

static struct kernel_sem_undo *undo_create(void)
{
    struct kernel_sem_undo *group = sem_allocate(sizeof(*group));
    if (group) { kernel_raw_lock_init(&group->lock, 35); group->owners = 1; }
    return group;
}
int kernel_sem_clone(struct kernel_task *parent, struct kernel_task *child, int shared)
{
    child->sem_undo = 0;
    child->sem_request = 0;
    if (!shared) return 0;
    if (!parent->sem_undo) {
        parent->sem_undo = undo_create();
        if (!parent->sem_undo) return -KERNEL_ENOMEM;
    }
    struct kernel_sem_undo *group = parent->sem_undo;
    KERNEL_RAW_SCOPE(guard, &group->lock);
    if (!group->owners || group->owners == UINT32_MAX) __builtin_trap();
    group->owners++;
    child->sem_undo = group;
    return 0;
}
static struct sem_undo_entry *undo_find(struct kernel_sem_undo *group, struct sem_set *set)
{
    for (struct sem_undo_entry *entry = group->entries; entry; entry = entry->group_next)
        if (entry->set == set) return entry;
    return 0;
}
static void entry_get(struct sem_undo_entry *entry)
{
    uint32_t old = __atomic_fetch_add(&entry->references, 1, __ATOMIC_RELAXED);
    if (!old || old == UINT32_MAX) __builtin_trap();
}
static void entry_put(struct sem_undo_entry *entry)
{
    if (!__atomic_sub_fetch(&entry->references, 1, __ATOMIC_ACQ_REL)) {
        if (entry->set) __builtin_trap();
        sem_free(entry);
    }
}
static int request_undo(struct kernel_sem_request *request)
{
    struct kernel_task *task = request->task;
    if (!task->sem_undo) {
        task->sem_undo = undo_create();
        if (!task->sem_undo) return -KERNEL_ENOMEM;
    }
    struct kernel_sem_undo *group = task->sem_undo;
    struct sem_set *set = request->set;
    {
        KERNEL_RAW_SCOPE(guard, &group->lock);
        request->undo = undo_find(group, set);
        if (request->undo) entry_get(request->undo);
    }
    if (request->undo) return 0;
    struct sem_undo_entry *candidate = sem_allocate(sizeof(*candidate) +
        (size_t)set->nsems * sizeof(candidate->adjustment[0]));
    if (!candidate) return -KERNEL_ENOMEM;
    int error = 0;
    {
        KERNEL_RAW_SCOPE(set_guard, &set->lock);
        KERNEL_RAW_SCOPE(group_guard, &group->lock);
        if (set->removed) error = -KERNEL_EIDRM;
        else if ((request->undo = undo_find(group, set))) entry_get(request->undo);
        else {
            candidate->references = 2; /* Group membership and this request. */
            candidate->group = group;
            candidate->set = set;
            set_get(set);
            candidate->group_next = group->entries;
            if (group->entries) group->entries->group_previous = candidate;
            group->entries = candidate;
            candidate->set_next = set->undo;
            if (set->undo) set->undo->set_previous = candidate;
            set->undo = candidate;
            request->undo = candidate;
            candidate = 0;
        }
    }
    sem_free(candidate);
    return error;
}
static void request_put(struct kernel_sem_request *request)
{
    if (__atomic_sub_fetch(&request->references, 1, __ATOMIC_ACQ_REL)) return;
    if (request->queued) __builtin_trap();
    if (request->pid) kernel_task_identity_put(request->pid);
    if (request->undo) entry_put(request->undo);
    if (request->set) set_put(request->set);
    sem_free(request->buffer);
    sem_free(request);
}
static void request_remove(struct kernel_sem_request *request)
{
    struct sem_set *set = request->set;
    if (!request->queued) return;
    if (request->previous) request->previous->next = request->next;
    else set->head = request->next;
    if (request->next) request->next->previous = request->previous;
    else set->tail = request->previous;
    request->next = request->previous = 0;
    request->queued = 0;
}
/* Temporary mutations cannot escape the set lock. Roll back the applied prefix
 * in reverse order: this also handles repeated indices without a second state
 * array or a quadratic duplicate search. PID and otime change only on commit. */
static int request_try(struct kernel_sem_request *request)
{
    struct sem_set *set = request->set;
    uint32_t i = 0;
    int error = 0;
    for (; i < request->count; i++) {
        const struct kernel_sembuf *op = &request->ops[i];
        struct sem_member *member = &set->members[op->sem_num];
        int value = member->value + op->sem_op;
        if ((!op->sem_op && member->value) || value < 0) {
            request->blocking = i;
            error = (op->sem_flg & KERNEL_IPC_NOWAIT) ? -KERNEL_EAGAIN : 1;
            break;
        }
        if (value > KERNEL_SEMVMX) { error = -KERNEL_ERANGE; break; }
        if (op->sem_flg & KERNEL_SEM_UNDO) {
            int adjustment = request->undo->adjustment[op->sem_num] - op->sem_op;
            if (adjustment < -KERNEL_SEMAEM - 1 || adjustment > KERNEL_SEMAEM) {
                error = -KERNEL_ERANGE;
                break;
            }
            request->undo->adjustment[op->sem_num] = adjustment;
        }
        member->value = value;
    }
    if (error) {
        while (i) {
            const struct kernel_sembuf *op = &request->ops[--i];
            set->members[op->sem_num].value -= op->sem_op;
            if (op->sem_flg & KERNEL_SEM_UNDO) request->undo->adjustment[op->sem_num] += op->sem_op;
        }
        return error;
    }
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        for (i = 0; i < request->count; i++)
            member_pid_set(&set->members[request->ops[i].sem_num], request->pid);
    }
    set->otime = sem_now();
    return 0;
}
static void request_complete(struct kernel_sem_request *request, int result,
    struct kernel_sem_request **wake)
{
    request_remove(request);
    request->result = result;
    /* Pin before publishing completion; finish/reaping waits for notification
     * to stop touching the token's task even if it wins on another CPU. */
    kernel_wait_task_pin(request->task);
    __atomic_fetch_add(&request->references, 1, __ATOMIC_RELAXED);
    request->wake_next = *wake;
    *wake = request;
}
static void set_update(struct sem_set *set, struct kernel_sem_request **wake)
{
    struct kernel_sem_request *request = set->head;
    while (request) {
        struct kernel_sem_request *next = request->next;
        int result = request_try(request);
        if (result != 1) {
            unsigned changed = !result && request->alter;
            request_complete(request, result, wake);
            /* A later vector can satisfy an earlier blocked vector. */
            if (changed) next = set->head;
        }
        request = next;
    }
}
static void wake_requests(struct kernel_sem_request *wake)
{
    while (wake) {
        struct kernel_sem_request *request = wake;
        wake = request->wake_next;
        kernel_wait_notify(&request->token, KERNEL_WAIT_WOKEN);
        kernel_wait_task_unpin(request->task);
        request_put(request);
    }
}

static struct kernel_sem_request *request_create(struct kernel_task *task,
    struct sem_set *set, uint32_t count)
{
    struct kernel_sem_request *request = sem_allocate(sizeof(*request) +
        (size_t)count * sizeof(request->ops[0]));
    if (!request) return 0;
    request->references = 1;
    request->task = task;
    request->set = set; /* Consume an optional lookup reference, including on abort. */
    request->count = count;
    if (task->sem_request) __builtin_trap();
    task->sem_request = request;
    return request;
}
static void request_finish(struct kernel_sem_request *request)
{
    if (request->task->sem_request != request) __builtin_trap();
    request->task->sem_request = 0;
    request_put(request);
}
int64_t kernel_sem_op(struct kernel_task *task, struct kernel_mm *mm, int32_t id,
    uint64_t ops, uint64_t count, uint64_t timeout_address)
{
    size_t copied;
    struct { int64_t seconds, nanoseconds; } timeout;
    /* Linux imports timeout bytes before count validation, then imports the
     * vector once before validating ID/timeout values or looking up the set. */
    if (timeout_address &&
        kernel_copy_from_user(mm, &timeout, timeout_address, sizeof(timeout), &copied)) return -KERNEL_EFAULT;
    if (count > KERNEL_SEMOPM) return -KERNEL_E2BIG;
    if (!count) return -KERNEL_EINVAL;
    struct kernel_sem_request *request = request_create(task, 0, (uint32_t)count);
    if (!request) return -KERNEL_ENOMEM;
    int result = -KERNEL_EFAULT;
    if (kernel_copy_from_user(mm, request->ops, ops, (size_t)count * sizeof(request->ops[0]), &copied))
        goto done;
    result = -KERNEL_EINVAL;
    if (id < 0) goto done;
    uint64_t deadline = 0;
    int expired = 0;
    if (timeout_address) {
        if (timeout.seconds < 0 || timeout.nanoseconds < 0 || timeout.nanoseconds >= 1000000000)
            goto done;
        unsigned __int128 duration = (unsigned __int128)(uint64_t)timeout.seconds *
            UINT64_C(1000000000) + (uint64_t)timeout.nanoseconds;
        uint64_t now = kernel_time_monotonic_ns();
        uint64_t target = now >= INT64_MAX || duration > (uint64_t)INT64_MAX - now
            ? INT64_MAX : now + (uint64_t)duration;
        enum kernel_time_status status = kernel_time_deadline_from_monotonic(target, &deadline);
        expired = status == KERNEL_TIME_STATUS_DEADLINE_PASSED;
        if (status != KERNEL_TIME_STATUS_OK && !expired) goto done;
    }
    struct sem_set *set = set_lookup(id, 0);
    if (!set) goto done;
    request->set = set;
    unsigned undo = 0;
    for (uint32_t i = 0; i < request->count; i++) {
        if (request->ops[i].sem_num >= set->nsems) { result = -KERNEL_EFBIG; goto done; }
        if (request->ops[i].sem_flg & KERNEL_SEM_UNDO) undo = 1;
        if (request->ops[i].sem_op) request->alter = 1;
    }
    request->pid = task_pid_get(task);
    if (!request->pid) { result = -KERNEL_ESRCH; goto done; }
    if (undo && (result = request_undo(request))) goto done;
    struct kernel_sem_request *wake = 0;
    {
        KERNEL_RAW_SCOPE(guard, &set->lock);
        if (set->removed) result = -KERNEL_EIDRM;
        else if ((result = permission(set, kernel_credentials_current(), request->alter ? 2 : 4))) { }
        else {
            result = request_try(request);
            if (!result && request->alter) set_update(set, &wake);
            else if (result == 1) {
                if (expired) result = -KERNEL_EAGAIN;
                else {
                    if (kernel_wait_prepare(0, deadline, 1, &request->token)) __builtin_trap();
                    request->prepared = 1;
                    request->result = 1;
                    request->previous = set->tail;
                    if (set->tail) set->tail->next = request;
                    else set->head = request;
                    set->tail = request;
                    request->queued = 1;
                }
            }
        }
    }
    wake_requests(wake);
    if (request->prepared) {
        enum kernel_wait_wake_reason reason = KERNEL_WAIT_WOKEN;
        if (kernel_wait_park(&request->token, &reason)) __builtin_trap();
        {
            KERNEL_RAW_SCOPE(guard, &set->lock);
            if (request->queued) {
                request_remove(request);
                request->result = reason == KERNEL_WAIT_TIMEOUT ? -KERNEL_EAGAIN : -KERNEL_EINTR;
            }
            result = request->result;
        }
        /* A notifier may still own the immutable token. Finish a local copy,
         * then clear the request after all notification borrows have ended. */
        struct kernel_wait_token token = request->token;
        if (kernel_wait_finish(&token)) __builtin_trap();
        request->prepared = 0;
    }
done:
    request_finish(request);
    return result;
}

static void undo_unlink(struct sem_undo_entry *entry)
{
    struct sem_set *set = entry->set;
    if (entry->set_previous) entry->set_previous->set_next = entry->set_next;
    else set->undo = entry->set_next;
    if (entry->set_next) entry->set_next->set_previous = entry->set_previous;
    entry->set_previous = entry->set_next = 0;
    struct kernel_sem_undo *group = entry->group;
    if (entry->group_previous) entry->group_previous->group_next = entry->group_next;
    else group->entries = entry->group_next;
    if (entry->group_next) entry->group_next->group_previous = entry->group_previous;
    entry->group_previous = entry->group_next = 0;
    /* Membership readers take the group lock; adjustments use the set lock. */
    entry->set = 0;
}
static int set_remove(struct sem_set *set, const struct kernel_credentials *cred)
{
    {
        KERNEL_RAW_SCOPE(index_guard, &sem_table.lock);
        KERNEL_RAW_SCOPE(set_guard, &set->lock);
        if (set->removed) return -KERNEL_EIDRM;
        if (!is_owner(set, cred)) return -KERNEL_EPERM;
        set->removed = 1;
        sem_table.slots = kernel_avl_remove_node(sem_table.slots, &set->slot_node);
        if (set->permission.key != KERNEL_IPC_PRIVATE)
            sem_table.keys = kernel_avl_remove_node(sem_table.keys, &set->key_node);
        sem_table.sets--;
        sem_table.members -= set->nsems;
        sem_table.sequence[set->slot_node.key]++;
    }
    struct kernel_sem_request *wake = 0;
    struct sem_undo_entry *retired = 0;
    {
        KERNEL_RAW_SCOPE(guard, &set->lock);
        while (set->head) request_complete(set->head, -KERNEL_EIDRM, &wake);
        while (set->undo) {
            struct sem_undo_entry *entry = set->undo;
            KERNEL_RAW_SCOPE(group_guard, &entry->group->lock);
            undo_unlink(entry);
            entry->set_next = retired;
            retired = entry;
        }
    }
    wake_requests(wake);
    while (retired) {
        struct sem_undo_entry *entry = retired;
        retired = entry->set_next;
        entry_put(entry);
        set_put(set);
    }
    set_put(set); /* Namespace ownership ended when discovery was removed. */
    return 0;
}
static void clear_adjustments(struct sem_set *set, int32_t num)
{
    for (struct sem_undo_entry *entry = set->undo; entry; entry = entry->set_next) {
        if (num >= 0) entry->adjustment[num] = 0;
        else memset(entry->adjustment, 0, (size_t)set->nsems * sizeof(entry->adjustment[0]));
    }
}
static int64_t sem_info(struct kernel_mm *mm, int32_t cmd, uint64_t arg)
{
    struct kernel_seminfo info = { .semmap = KERNEL_SEMMNS, .semmni = KERNEL_SEMMNI,
        .semmns = KERNEL_SEMMNS, .semmnu = KERNEL_SEMMNS, .semmsl = KERNEL_SEMMSL,
        .semopm = KERNEL_SEMOPM, .semume = KERNEL_SEMOPM, .semusz = 20,
        .semvmx = KERNEL_SEMVMX, .semaem = KERNEL_SEMAEM };
    size_t copied;
    int64_t max_slot;
    {
        KERNEL_RAW_SCOPE(guard, &sem_table.lock);
        struct kernel_avl_node *last = kernel_avl_last(sem_table.slots);
        max_slot = last ? (int64_t)last->key : 0;
        if (cmd == KERNEL_SEM_INFO) { info.semusz = (int32_t)sem_table.sets; info.semaem = (int32_t)sem_table.members; }
    }
    return kernel_copy_to_user(mm, arg, &info, sizeof(info), &copied) ? -KERNEL_EFAULT : max_slot;
}
int64_t kernel_sem_ctl(struct kernel_task *task, struct kernel_mm *mm, int32_t id,
    int32_t num, int32_t cmd, uint64_t arg)
{
    size_t copied;
    cmd &= ~KERNEL_IPC_64;
    if (id < 0) return -KERNEL_EINVAL;
    if (cmd == KERNEL_IPC_INFO || cmd == KERNEL_SEM_INFO) return sem_info(mm, cmd, arg);
    switch (cmd) {
    case KERNEL_IPC_RMID: case KERNEL_IPC_SET: case KERNEL_IPC_STAT:
    case KERNEL_SEM_GETPID: case KERNEL_SEM_GETVAL: case KERNEL_SEM_GETALL:
    case KERNEL_SEM_GETNCNT: case KERNEL_SEM_GETZCNT: case KERNEL_SEM_SETVAL:
    case KERNEL_SEM_SETALL: case KERNEL_SEM_STAT: case KERNEL_SEM_STAT_ANY: break;
    default: return -KERNEL_EINVAL;
    }
    if (cmd == KERNEL_SEM_SETVAL && (int32_t)arg < 0) return -KERNEL_ERANGE;
    if (cmd == KERNEL_SEM_SETVAL && (int32_t)arg > KERNEL_SEMVMX) return -KERNEL_ERANGE;
    struct kernel_semid64_ds descriptor = {0};
    if (cmd == KERNEL_IPC_SET &&
        kernel_copy_from_user(mm, &descriptor, arg, sizeof(descriptor), &copied)) return -KERNEL_EFAULT;
    struct sem_set *set = set_lookup(id, cmd == KERNEL_SEM_STAT || cmd == KERNEL_SEM_STAT_ANY);
    if (!set) return -KERNEL_EINVAL;
    struct kernel_sem_request *request = 0;
    struct kernel_pid *pid = 0;
    const struct kernel_credentials *cred = kernel_credentials_current();
    int64_t result = 0;
    uint16_t *values = 0;
    if (cmd == KERNEL_SEM_SETVAL && (num < 0 || (uint32_t)num >= set->nsems)) {
        result = -KERNEL_EINVAL;
        goto done;
    }
    if (cmd == KERNEL_SEM_GETALL || cmd == KERNEL_SEM_SETALL) {
        {
            KERNEL_RAW_SCOPE(guard, &set->lock);
            result = set->removed ? -KERNEL_EIDRM :
                permission(set, cred, cmd == KERNEL_SEM_SETALL ? 2 : 4);
        }
        if (result) goto done;
        /* Only array control calls retain an owner across allocation/usercopy.
         * Scalar commands need no scratch; STAT releases its set after snapshot. */
        request = request_create(task, set, 0);
        if (!request) { result = -KERNEL_ENOMEM; goto done; }
        request->buffer = values = sem_allocate((size_t)set->nsems * sizeof(*values));
        if (!values) { result = -KERNEL_ENOMEM; goto done; }
        if (cmd == KERNEL_SEM_SETALL) {
            if (kernel_copy_from_user(mm, values, arg, (size_t)set->nsems * sizeof(*values), &copied)) {
                result = -KERNEL_EFAULT;
                goto done;
            }
            for (uint32_t i = 0; i < set->nsems; i++)
                if (values[i] > KERNEL_SEMVMX) { result = -KERNEL_ERANGE; goto done; }
        }
    }
    if (cmd == KERNEL_SEM_SETVAL || cmd == KERNEL_SEM_SETALL) {
        pid = task_pid_get(task);
        if (request) request->pid = pid;
        if (!pid) { result = -KERNEL_ESRCH; goto done; }
    }
    if (cmd == KERNEL_IPC_RMID) { result = set_remove(set, cred); goto done; }
    struct kernel_sem_request *wake = 0;
    {
        KERNEL_RAW_SCOPE(guard, &set->lock);
        if (set->removed) { result = -KERNEL_EIDRM; goto unlock; }
        if (cmd == KERNEL_IPC_SET) {
            if (!is_owner(set, cred)) { result = -KERNEL_EPERM; goto unlock; }
            if (descriptor.sem_perm.uid == UINT32_MAX || descriptor.sem_perm.gid == UINT32_MAX) {
                result = -KERNEL_EINVAL;
                goto unlock;
            }
            set->permission.uid = descriptor.sem_perm.uid;
            set->permission.gid = descriptor.sem_perm.gid;
            set->permission.mode = descriptor.sem_perm.mode & 0777;
            set->ctime = sem_now();
            goto unlock;
        }
        if (cmd != KERNEL_SEM_STAT_ANY && (result = permission(set, cred,
            cmd == KERNEL_SEM_SETVAL || cmd == KERNEL_SEM_SETALL ? 2 : 4))) goto unlock;
        if (cmd == KERNEL_IPC_STAT || cmd == KERNEL_SEM_STAT || cmd == KERNEL_SEM_STAT_ANY) {
            set_descriptor(set, &descriptor);
            result = cmd == KERNEL_IPC_STAT ? 0 : set->id;
            goto unlock;
        }
        if (cmd == KERNEL_SEM_GETALL) {
            for (uint32_t i = 0; i < set->nsems; i++) values[i] = (uint16_t)set->members[i].value;
            goto unlock;
        }
        if (cmd == KERNEL_SEM_SETALL) {
            clear_adjustments(set, -1);
            {
                KERNEL_RAW_SCOPE(identity_guard, &kernel_wait_domain);
                for (uint32_t i = 0; i < set->nsems; i++) {
                    set->members[i].value = values[i];
                    member_pid_set(&set->members[i], pid);
                }
            }
            set->ctime = sem_now();
            set_update(set, &wake);
            goto unlock;
        }
        if (num < 0 || (uint32_t)num >= set->nsems) { result = -KERNEL_EINVAL; goto unlock; }
        struct sem_member *member = &set->members[num];
        switch (cmd) {
        case KERNEL_SEM_GETPID: result = member->pid ? member->pid->number : 0; break;
        case KERNEL_SEM_GETVAL: result = member->value; break;
        case KERNEL_SEM_GETNCNT: case KERNEL_SEM_GETZCNT:
            for (struct kernel_sem_request *r = set->head; r; r = r->next) {
                const struct kernel_sembuf *op = &r->ops[r->blocking];
                if (op->sem_num == num && (cmd == KERNEL_SEM_GETZCNT ? op->sem_op == 0 : op->sem_op < 0)) result++;
            }
            break;
        case KERNEL_SEM_SETVAL:
            clear_adjustments(set, num);
            member->value = (int32_t)arg;
            {
                KERNEL_RAW_SCOPE(identity_guard, &kernel_wait_domain);
                member_pid_set(member, pid);
            }
            set->ctime = sem_now();
            set_update(set, &wake);
            break;
        default: __builtin_trap();
        }
unlock:;
    }
    wake_requests(wake);
    if (result >= 0) {
        if (cmd == KERNEL_IPC_STAT || cmd == KERNEL_SEM_STAT || cmd == KERNEL_SEM_STAT_ANY) {
            set_put(set);
            return kernel_copy_to_user(mm, arg, &descriptor, sizeof(descriptor), &copied) ? -KERNEL_EFAULT : result;
        } else if (cmd == KERNEL_SEM_GETALL &&
            kernel_copy_to_user(mm, arg, values, (size_t)set->nsems * sizeof(*values), &copied)) result = -KERNEL_EFAULT;
    }
done:
    if (request) request_finish(request);
    else {
        if (pid) kernel_task_identity_put(pid);
        set_put(set);
    }
    return result;
}

void kernel_sem_abort(struct kernel_task *task)
{
    struct kernel_sem_request *request = task->sem_request;
    if (!request) return;
    if (request->set) {
        KERNEL_RAW_SCOPE(guard, &request->set->lock);
        if (request->queued) {
            request_remove(request);
            request->result = -KERNEL_EINTR;
        }
    }
    task->sem_request = 0;
    request_put(request);
}
void kernel_sem_exit(struct kernel_task *task)
{
    kernel_sem_abort(task);
    struct kernel_sem_undo *group = task->sem_undo;
    if (!group) return;
    task->sem_undo = 0;
    {
        KERNEL_RAW_SCOPE(guard, &group->lock);
        if (!group->owners) __builtin_trap();
        if (--group->owners) return;
    }
    struct kernel_pid *pid = task_pid_get(task);
    for (;;) {
        struct sem_undo_entry *entry;
        struct sem_set *set;
        {
            KERNEL_RAW_SCOPE(guard, &group->lock);
            entry = group->entries;
            if (!entry) break;
            entry_get(entry);
            set = entry->set;
            if (!set) __builtin_trap();
            set_get(set);
        }
        struct kernel_sem_request *wake = 0;
        unsigned detached = 0;
        {
            KERNEL_RAW_SCOPE(set_guard, &set->lock);
            {
                KERNEL_RAW_SCOPE(group_guard, &group->lock);
                if (entry->set) { undo_unlink(entry); detached = 1; }
            }
            if (detached && !set->removed) {
                if (!pid) __builtin_trap();
                {
                    KERNEL_RAW_SCOPE(identity_guard, &kernel_wait_domain);
                    for (uint32_t i = 0; i < set->nsems; i++) {
                        if (!entry->adjustment[i]) continue;
                        int value = set->members[i].value + entry->adjustment[i];
                        set->members[i].value = value < 0 ? 0 : value > KERNEL_SEMVMX ? KERNEL_SEMVMX : value;
                        member_pid_set(&set->members[i], pid);
                    }
                }
                set->otime = sem_now();
                set_update(set, &wake);
            }
        }
        wake_requests(wake);
        if (detached) { entry_put(entry); set_put(set); }
        entry_put(entry);
        set_put(set);
    }
    if (pid) kernel_task_identity_put(pid);
    sem_free(group);
}
