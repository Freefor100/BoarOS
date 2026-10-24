#include <kernel/avl.h>
#include <kernel/heap.h>
#include <kernel/msg.h>
#include <kernel/raw_lock.h>
#include <kernel/signal.h>
#include <kernel/socket_task.h>
#include <kernel/task_state.h>
#include <kernel/time.h>
#include <kernel/uaccess.h>
#include <kernel/wait_internal.h>
#include "../mm/uaccess_internal.h"
#include <stddef.h>
#include <string.h>

/* Namespace discovery owns one queue reference; queue state is independently
 * locked. Lock order: index(25), queue(30), scheduler/PID domain(40).
 * Message contents never change after import. A request owns every allocation
 * used across usercopy or a wait, including detached receive messages. */
struct msg_message {
    struct msg_message *previous, *next;
    uint32_t references;
    uint64_t size;
    int64_t type;
    unsigned char data[];
};
struct msg_queue {
    struct kernel_avl_node slot_node, key_node;
    struct kernel_raw_lock lock;
    uint32_t references, removed;
    int32_t id;
    struct kernel_ipc64_perm permission;
    int64_t stime, rtime, ctime;
    uint64_t bytes, count, limit;
    struct kernel_pid *spid, *rpid;
    struct msg_message *head, *tail;
    struct kernel_msg_request *send_head, *send_tail, *recv_head, *recv_tail;
};
struct kernel_msg_request {
    struct msg_queue *queue;
    struct kernel_task *task;
    struct kernel_pid *displaced_pid;
    struct msg_message *message, *copy;
    struct kernel_msg_request *previous, *next;
    struct kernel_wait_token token;
    uint32_t sending, queued;
    int result, flags;
    int64_t type;
    uint64_t size;
};
static struct {
    struct kernel_raw_lock lock;
    struct kernel_heap *heap;
    struct kernel_avl_node *slots, *keys;
    uint32_t queues, next_slot;
    uint64_t bytes, messages;
    uint16_t sequence[KERNEL_MSGMNI];
} msg_table;
static void *msg_allocate(size_t size)
{
    void *p = 0;
    enum kernel_heap_status s = kernel_heap_allocate_zeroed(msg_table.heap, 1, size, &p);
    if (s == KERNEL_HEAP_STATUS_EMPTY)
        return 0;
    if (s != KERNEL_HEAP_STATUS_OK)
        __builtin_trap();
    return p;
}
static void msg_free(void *p)
{
    if (p)
        kernel_heap_release(msg_table.heap, p);
}
static int64_t msg_now(void)
{
    return (int64_t)(kernel_time_realtime_ns() / UINT64_C(1000000000));
}
static struct kernel_avl_node *index_find(struct kernel_avl_node *n, uint64_t key)
{
    while (n && n->key != key)
        n = key < n->key ? n->left : n->right;
    return n;
}
static struct msg_queue *slot_queue(struct kernel_avl_node *n)
{
    return n ? (struct msg_queue *)((char *)n - offsetof(struct msg_queue, slot_node)) : 0;
}
static struct msg_queue *key_queue(struct kernel_avl_node *n)
{
    return n ? (struct msg_queue *)((char *)n - offsetof(struct msg_queue, key_node)) : 0;
}
static void queue_get(struct msg_queue *q)
{
    uint32_t old = __atomic_fetch_add(&q->references, 1, __ATOMIC_RELAXED);
    if (!old || old == UINT32_MAX)
        __builtin_trap();
}
static void queue_put(struct msg_queue *q)
{
    if (__atomic_sub_fetch(&q->references, 1, __ATOMIC_ACQ_REL))
        return;
    if (!q->removed || q->head || q->send_head || q->recv_head)
        __builtin_trap();
    if (q->spid)
        kernel_task_identity_put(q->spid);
    if (q->rpid)
        kernel_task_identity_put(q->rpid);
    msg_free(q);
}
static struct msg_queue *queue_lookup(int32_t id, int by_slot)
{
    if (id < 0)
        return 0;
    KERNEL_RAW_SCOPE(g, &msg_table.lock);
    struct msg_queue *q =
        slot_queue(index_find(msg_table.slots, by_slot ? (uint32_t)id : ((uint32_t)id & 32767U)));
    if (q && (by_slot || q->id == id))
        queue_get(q);
    else
        q = 0;
    return q;
}
static int permission(const struct msg_queue *q, const struct kernel_credentials *c,
                      unsigned requested)
{
    const struct kernel_ipc64_perm *p = &q->permission;
    return kernel_ipc_permission(c, p->mode, p->uid, p->gid, p->cuid, p->cgid, requested);
}
static int owner(const struct msg_queue *q, const struct kernel_credentials *c)
{
    return kernel_ipc_owner(c, q->permission.uid, q->permission.cuid);
}
static struct kernel_pid *pid_set(struct kernel_pid **target, struct kernel_task *task, int direct)
{
    /* Linux pipelined_send records a sleeping receiver's TID; dequeue records
     * the executing receiver's TGID. Executing tasks retain their group, and
     * abort must acquire this queue lock before releasing a waiting task. */
    struct kernel_task *identity_task = direct || !task->group_leader ? task : task->group_leader;
    /* 队列的身份引用保留该数字，不能被其他身份复用；同一发送者或
     * 接收者继续操作时不必重复进入全局 PID 域。 */
    if (*target && (*target)->number == identity_task->tid)
        return 0;
    KERNEL_RAW_SCOPE(g, &kernel_wait_domain);
    struct kernel_pid *pid =
        identity_task->identities[direct ? KERNEL_PID_TID : KERNEL_PID_TGID].identity;
    if (!pid)
        __builtin_trap();
    kernel_pid_get(pid);
    struct kernel_pid *old = *target;
    *target = pid;
    /* 将原队列引用交给请求，结束调用或弃栈时再在 raw 外归还。 */
    return old;
}
static struct msg_message *message_create(uint64_t size)
{
    struct msg_message *m = msg_allocate(sizeof(*m) + (size_t)size);
    if (m) {
        m->references = 1;
        m->size = size;
    }
    return m;
}
static void message_get(struct msg_message *m)
{
    uint32_t old = __atomic_fetch_add(&m->references, 1, __ATOMIC_RELAXED);
    if (!old || old == UINT32_MAX)
        __builtin_trap();
}
static void message_put(struct msg_message *m)
{
    if (m && !__atomic_sub_fetch(&m->references, 1, __ATOMIC_ACQ_REL))
        msg_free(m);
}
int kernel_msg_init(struct kernel_heap *heap)
{
    if (!heap)
        return -KERNEL_EINVAL;
    if (msg_table.heap)
        return msg_table.heap == heap ? 0 : -KERNEL_EINVAL;
    kernel_raw_lock_init(&msg_table.lock, 25);
    msg_table.heap = heap;
    return 0;
}
int64_t kernel_msg_get(struct kernel_task *task, int32_t key, int32_t flags)
{
    (void)task;
    if (!msg_table.heap)
        return -KERNEL_ENOSPC;
    const struct kernel_credentials *c = kernel_credentials_current();
    struct msg_queue *candidate = 0;
    int64_t result;
    for (;;) {
        struct msg_queue *found = 0;
        {
            KERNEL_RAW_SCOPE(g, &msg_table.lock);
            if (key != KERNEL_IPC_PRIVATE)
                found = key_queue(index_find(msg_table.keys, (uint32_t)key));
            if (found)
                queue_get(found);
            else if (key != KERNEL_IPC_PRIVATE && !(flags & KERNEL_IPC_CREAT))
                result = -KERNEL_ENOENT;
            else if (msg_table.queues == KERNEL_MSGMNI)
                result = -KERNEL_ENOSPC;
            else if (!candidate)
                result = INT64_MIN;
            else {
                uint32_t slot = msg_table.next_slot;
                while (index_find(msg_table.slots, slot))
                    slot = (slot + 1) % KERNEL_MSGMNI;
                msg_table.next_slot = (slot + 1) % KERNEL_MSGMNI;
                candidate->slot_node.key = slot;
                candidate->key_node.key = (uint32_t)key;
                candidate->permission.seq = msg_table.sequence[slot];
                candidate->id = (int32_t)(((uint32_t)candidate->permission.seq << 15) | slot);
                msg_table.slots = kernel_avl_insert_node(msg_table.slots, &candidate->slot_node);
                if (key != KERNEL_IPC_PRIVATE)
                    msg_table.keys = kernel_avl_insert_node(msg_table.keys, &candidate->key_node);
                msg_table.queues++;
                result = candidate->id;
                candidate = 0;
            }
        }
        if (found) {
            {
                KERNEL_RAW_SCOPE(g, &found->lock);
                if (found->removed)
                    result = -KERNEL_EIDRM;
                else if ((flags & (KERNEL_IPC_CREAT | KERNEL_IPC_EXCL)) ==
                         (KERNEL_IPC_CREAT | KERNEL_IPC_EXCL))
                    result = -KERNEL_EEXIST;
                else {
                    unsigned a = (unsigned)flags & 0777;
                    int e = permission(found, c, (a | (a >> 3) | (a >> 6)) & 7);
                    result = e ? e : found->id;
                }
            }
            queue_put(found);
        }
        if (found || result != INT64_MIN)
            break;
        candidate = msg_allocate(sizeof(*candidate));
        if (!candidate) {
            result = -KERNEL_ENOMEM;
            break;
        }
        kernel_raw_lock_init(&candidate->lock, 30);
        candidate->references = 1;
        candidate->slot_node.height = candidate->key_node.height = 1;
        candidate->limit = KERNEL_MSGMNB;
        candidate->ctime = msg_now();
        candidate->permission = (struct kernel_ipc64_perm){.key = key,
                                                           .uid = c->euid,
                                                           .cuid = c->euid,
                                                           .gid = c->egid,
                                                           .cgid = c->egid,
                                                           .mode = (uint32_t)flags & 0777};
    }
    msg_free(candidate);
    return result;
}
static struct kernel_msg_request *request_create(struct kernel_task *task, int sending)
{
    struct kernel_msg_request *r = msg_allocate(sizeof(*r));
    if (!r)
        return 0;
    r->task = task;
    r->sending = (uint32_t)sending;
    if (task->msg_request)
        __builtin_trap();
    task->msg_request = r;
    return r;
}
static void request_release(struct kernel_msg_request *r)
{
    if (r->queued)
        __builtin_trap();
    if (r->displaced_pid)
        kernel_task_identity_put(r->displaced_pid);
    message_put(r->message);
    message_put(r->copy);
    if (r->queue)
        queue_put(r->queue);
    msg_free(r);
}
static void request_finish(struct kernel_msg_request *r)
{
    if (r->task->msg_request != r)
        __builtin_trap();
    r->task->msg_request = 0;
    request_release(r);
}
static void request_remove(struct kernel_msg_request *r)
{
    if (!r->queued)
        return;
    struct msg_queue *q = r->queue;
    struct kernel_msg_request **head = r->sending ? &q->send_head : &q->recv_head;
    struct kernel_msg_request **tail = r->sending ? &q->send_tail : &q->recv_tail;
    if (r->previous)
        r->previous->next = r->next;
    else
        *head = r->next;
    if (r->next)
        r->next->previous = r->previous;
    else
        *tail = r->previous;
    r->previous = r->next = 0;
    r->queued = 0;
}
static void request_complete(struct kernel_msg_request *r, int result)
{
    request_remove(r);
    r->result = result;
    /* 结果读取和 abort 都要先取队列锁，通知返回前请求与任务仍有效。
     * notify 只更新调度状态；远端 IPI 在最外层 raw 解锁后提交。 */
    if (kernel_wait_notify(&r->token, KERNEL_WAIT_WOKEN))
        __builtin_trap();
}
static int fits(struct msg_queue *q, uint64_t size)
{
    return q->bytes <= q->limit && size <= q->limit - q->bytes && q->count < q->limit;
}
static void wake_senders(struct msg_queue *q)
{
    struct kernel_msg_request *r = q->send_head;
    while (r) {
        struct kernel_msg_request *next = r->next;
        if (fits(q, r->size))
            request_complete(r, 1);
        r = next;
    }
}
static void request_prepare(struct kernel_msg_request *r)
{
    struct msg_queue *q = r->queue;
    struct kernel_msg_request **head = r->sending ? &q->send_head : &q->recv_head;
    struct kernel_msg_request **tail = r->sending ? &q->send_tail : &q->recv_tail;
    if (kernel_wait_prepare(0, 0, 1, &r->token))
        __builtin_trap();
    r->result = 1;
    r->previous = *tail;
    if (*tail)
        (*tail)->next = r;
    else
        *head = r;
    *tail = r;
    r->queued = 1;
}
static int request_wait(struct kernel_msg_request *r)
{
    enum kernel_wait_wake_reason reason = KERNEL_WAIT_WOKEN;
    if (kernel_wait_park(&r->token, &reason))
        __builtin_trap();
    int result;
    {
        KERNEL_RAW_SCOPE(g, &r->queue->lock);
        if (r->queued) {
            request_remove(r);
            r->result = -KERNEL_EINTR;
        }
        result = r->result;
        /* A capacity wake has already unlinked this sender. RMID cannot find
         * it there, so classify removal before a later signal, as Linux does
         * after schedule. A delivered receive retains its completed result. */
        if (r->sending && r->queue->removed)
            result = -KERNEL_EIDRM;
        else if (result == 1 &&
                 (reason == KERNEL_WAIT_SIGNALLED || kernel_signal_has_pending(r->task) ||
                  kernel_signal_termination_requested(r->task)))
            result = -KERNEL_EINTR;
    }
    struct kernel_wait_token token = r->token;
    if (kernel_wait_finish(&token))
        __builtin_trap();
    return result;
}
static int matches(struct msg_message *m, int64_t type, int flags)
{
    if (!type)
        return 1;
    if (type < 0)
        return m->type <= (type == INT64_MIN ? INT64_MAX : -type);
    return flags & KERNEL_MSG_EXCEPT ? m->type != type : m->type == type;
}
static struct msg_message *message_find(struct msg_queue *q, int64_t type, int flags)
{
    struct msg_message *best = 0;
    uint64_t index = 0;
    for (struct msg_message *m = q->head; m; m = m->next) {
        if (flags & KERNEL_MSG_COPY) {
            if (type >= 0 && index == (uint64_t)type)
                return m;
            index++;
            continue;
        }
        if (!matches(m, type, flags))
            continue;
        if (type >= 0 || m->type == 1)
            return m;
        if (!best || m->type < best->type)
            best = m;
    }
    return best;
}
static void message_enqueue(struct msg_queue *q, struct msg_message *m)
{
    m->previous = q->tail;
    if (q->tail)
        q->tail->next = m;
    else
        q->head = m;
    q->tail = m;
    q->count++;
    q->bytes += m->size;
    __atomic_fetch_add(&msg_table.messages, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&msg_table.bytes, m->size, __ATOMIC_RELAXED);
}
static void message_detach(struct msg_queue *q, struct msg_message *m)
{
    if (m->previous)
        m->previous->next = m->next;
    else
        q->head = m->next;
    if (m->next)
        m->next->previous = m->previous;
    else
        q->tail = m->previous;
    m->previous = m->next = 0;
    q->count--;
    q->bytes -= m->size;
    __atomic_fetch_sub(&msg_table.messages, 1, __ATOMIC_RELAXED);
    __atomic_fetch_sub(&msg_table.bytes, m->size, __ATOMIC_RELAXED);
}
static void message_publish(struct kernel_msg_request *sender)
{
    struct msg_queue *q = sender->queue;
    struct msg_message *m = sender->message;
    q->stime = msg_now();
    sender->displaced_pid = pid_set(&q->spid, sender->task, 0);
    struct kernel_msg_request *r = q->recv_head;
    while (r) {
        struct kernel_msg_request *next = r->next;
        if (matches(m, r->type, r->flags)) {
            if (r->size < m->size && !(r->flags & KERNEL_MSG_NOERROR))
                request_complete(r, -KERNEL_E2BIG);
            else {
                r->message = m;
                sender->message = 0;
                q->rtime = msg_now();
                r->displaced_pid = pid_set(&q->rpid, r->task, 1);
                request_complete(r, 0);
                return;
            }
        }
        r = next;
    }
    message_enqueue(q, m);
    sender->message = 0;
}
int64_t kernel_msg_send(struct kernel_task *task, struct kernel_mm *mm, int32_t id, uint64_t msgp,
                        uint64_t size, int32_t flags)
{
    size_t copied;
    int64_t type;
    /* Native Linux reads the type even for invalid ID/size, then validates,
     * imports the complete immutable body, and only then looks up the queue. */
    if (kernel_copy_from_user(mm, &type, msgp, sizeof(type), &copied))
        return -KERNEL_EFAULT;
    if (id < 0 || size > KERNEL_MSGMAX || type < 1)
        return -KERNEL_EINVAL;
    struct kernel_msg_request *r = request_create(task, 1);
    if (!r)
        return -KERNEL_ENOMEM;
    int result = -KERNEL_ENOMEM;
    r->message = message_create(size);
    if (!r->message)
        goto done;
    r->message->type = type;
    r->size = size;
    result = -KERNEL_EFAULT;
    if (size && kernel_copy_from_user(mm, r->message->data, msgp + 8, (size_t)size, &copied))
        goto done;
    result = -KERNEL_EINVAL;
    r->queue = queue_lookup(id, 0);
    if (!r->queue)
        goto done;
    struct kernel_task *leader = task->group_leader ? task->group_leader : task;
    if (!leader->tid) {
        result = -KERNEL_ESRCH;
        goto done;
    }
    for (;;) {
        {
            KERNEL_RAW_SCOPE(g, &r->queue->lock);
            if (r->queue->removed)
                result = -KERNEL_EIDRM;
            else if ((result = permission(r->queue, kernel_credentials_current(), 2))) {
            } else if (fits(r->queue, size)) {
                message_publish(r);
                result = 0;
            } else if (flags & KERNEL_IPC_NOWAIT)
                result = -KERNEL_EAGAIN;
            else {
                request_prepare(r);
                result = 1;
            }
        }
        if (result != 1)
            break;
        result = request_wait(r);
        if (result != 1)
            break;
    }
done:
    request_finish(r);
    return result;
}
int64_t kernel_msg_receive(struct kernel_task *task, struct kernel_mm *mm, int32_t id,
                           uint64_t msgp, uint64_t size, int64_t type, int32_t flags)
{
    if (id < 0 || size > INT64_MAX)
        return -KERNEL_EINVAL;
    if ((flags & KERNEL_MSG_COPY) && ((flags & KERNEL_MSG_EXCEPT) || !(flags & KERNEL_IPC_NOWAIT)))
        return -KERNEL_EINVAL;
    struct kernel_msg_request *r = request_create(task, 0);
    if (!r)
        return -KERNEL_ENOMEM;
    r->type = type;
    r->flags = flags;
    r->size = size;
    size_t copied;
    int64_t result = -KERNEL_ENOMEM;
    if (flags & KERNEL_MSG_COPY) {
        uint64_t n = size > KERNEL_MSGMAX ? KERNEL_MSGMAX : size;
        r->copy = message_create(n);
        if (!r->copy)
            goto done;
        /* prepare_copy reads from the destination start, including its type.
         * This input read precedes queue lookup and read permission checks. */
        result = -KERNEL_EFAULT;
        if (n && kernel_copy_from_user(mm, r->copy->data, msgp, (size_t)n, &copied))
            goto done;
    }
    result = -KERNEL_EINVAL;
    r->queue = queue_lookup(id, 0);
    if (!r->queue)
        goto done;
    for (;;) {
        {
            KERNEL_RAW_SCOPE(g, &r->queue->lock);
            struct msg_queue *q = r->queue;
            if (q->removed)
                result = -KERNEL_EIDRM;
            else if ((result = permission(q, kernel_credentials_current(), 4))) {
            } else {
                struct msg_message *m = message_find(q, type, flags);
                if (m) {
                    if (size < m->size && !(flags & KERNEL_MSG_NOERROR))
                        result = -KERNEL_E2BIG;
                    else if ((flags & KERNEL_MSG_COPY) && r->copy->size < m->size)
                        result = -KERNEL_EINVAL;
                    else {
                        if (flags & KERNEL_MSG_COPY)
                            message_get(m);
                        else {
                            message_detach(q, m);
                            q->rtime = msg_now();
                            r->displaced_pid = pid_set(&q->rpid, task, 0);
                            wake_senders(q);
                        }
                        r->message = m;
                        result = 0;
                    }
                } else if (flags & KERNEL_IPC_NOWAIT)
                    result = -KERNEL_ENOMSG;
                else {
                    request_prepare(r);
                    result = 1;
                }
            }
        }
        if (result != 1)
            break;
        result = request_wait(r);
        if (result != 1)
            break;
    }
    if (!result) {
        struct msg_message *m = r->message;
        uint64_t n = size < m->size ? size : m->size;
        struct kernel_uaccess_write_cursor output = {0};
        /* A queued reference or detached ownership protects these immutable
         * bytes across faulting usercopy; EFAULT still consumes normal receives.
         * 两段分别校验范围，保留正文失败前已写入的类型；同页复用 pin。 */
        if (kernel_uaccess_write_cursor_copy(mm, &output, msgp, &m->type,
                                            sizeof(m->type), &copied) ||
            (n && kernel_uaccess_write_cursor_copy(mm, &output, msgp + 8, m->data,
                                                  (size_t)n, &copied)))
            result = -KERNEL_EFAULT;
        else
            result = (int64_t)n;
        kernel_uaccess_write_cursor_finish(&output);
    }
done:
    request_finish(r);
    return result;
}
static int queue_remove(struct msg_queue *q, const struct kernel_credentials *c)
{
    struct msg_message *messages;
    {
        KERNEL_RAW_SCOPE(index_guard, &msg_table.lock);
        KERNEL_RAW_SCOPE(queue_guard, &q->lock);
        if (q->removed)
            return -KERNEL_EIDRM;
        if (!owner(q, c))
            return -KERNEL_EPERM;
        q->removed = 1;
        msg_table.slots = kernel_avl_remove_node(msg_table.slots, &q->slot_node);
        if (q->permission.key != KERNEL_IPC_PRIVATE)
            msg_table.keys = kernel_avl_remove_node(msg_table.keys, &q->key_node);
        msg_table.queues--;
        msg_table.sequence[q->slot_node.key]++;
        while (q->send_head)
            request_complete(q->send_head, -KERNEL_EIDRM);
        while (q->recv_head)
            request_complete(q->recv_head, -KERNEL_EIDRM);
        messages = q->head;
        q->head = q->tail = 0;
        __atomic_fetch_sub(&msg_table.messages, q->count, __ATOMIC_RELAXED);
        __atomic_fetch_sub(&msg_table.bytes, q->bytes, __ATOMIC_RELAXED);
        q->count = q->bytes = 0;
    }
    while (messages) {
        struct msg_message *m = messages;
        messages = m->next;
        message_put(m);
    }
    queue_put(q);
    return 0;
}
static int32_t info_count(uint64_t n)
{
    return n > INT32_MAX ? INT32_MAX : (int32_t)n;
}
static int64_t msg_info(struct kernel_mm *mm, int32_t cmd, uint64_t arg)
{
    struct kernel_msginfo i = {.msgpool = KERNEL_MSGMNI * KERNEL_MSGMNB / 1024,
                               .msgmap = KERNEL_MSGMNB,
                               .msgmax = KERNEL_MSGMAX,
                               .msgmnb = KERNEL_MSGMNB,
                               .msgmni = KERNEL_MSGMNI,
                               .msgssz = 16,
                               .msgtql = KERNEL_MSGMNB,
                               .msgseg = 65535};
    int64_t last;
    size_t copied;
    {
        KERNEL_RAW_SCOPE(g, &msg_table.lock);
        struct kernel_avl_node *n = kernel_avl_last(msg_table.slots);
        last = n ? (int64_t)n->key : 0;
        if (cmd == KERNEL_MSG_INFO)
            i.msgpool = (int32_t)msg_table.queues;
    }
    if (cmd == KERNEL_MSG_INFO) {
        i.msgmap = info_count(__atomic_load_n(&msg_table.messages, __ATOMIC_RELAXED));
        i.msgtql = info_count(__atomic_load_n(&msg_table.bytes, __ATOMIC_RELAXED));
    }
    return kernel_copy_to_user(mm, arg, &i, sizeof(i), &copied) ? -KERNEL_EFAULT : last;
}
int64_t kernel_msg_ctl(struct kernel_task *task, struct kernel_mm *mm, int32_t id, int32_t cmd,
                       uint64_t arg)
{
    (void)task;
    if (id < 0)
        return -KERNEL_EINVAL;
    /* Native 64-bit msgctl passes the command unchanged, including IPC_64. */
    if (cmd == KERNEL_IPC_INFO || cmd == KERNEL_MSG_INFO)
        return msg_info(mm, cmd, arg);
    if (cmd != KERNEL_IPC_STAT && cmd != KERNEL_MSG_STAT && cmd != KERNEL_MSG_STAT_ANY &&
        cmd != KERNEL_IPC_SET && cmd != KERNEL_IPC_RMID)
        return -KERNEL_EINVAL;
    struct kernel_msqid64_ds d = {0};
    size_t copied;
    if (cmd == KERNEL_IPC_SET && kernel_copy_from_user(mm, &d, arg, sizeof(d), &copied))
        return -KERNEL_EFAULT;
    struct msg_queue *q = queue_lookup(id, cmd == KERNEL_MSG_STAT || cmd == KERNEL_MSG_STAT_ANY);
    if (!q)
        return -KERNEL_EINVAL;
    const struct kernel_credentials *c = kernel_credentials_current();
    int64_t result = 0;
    if (cmd == KERNEL_IPC_RMID) {
        result = queue_remove(q, c);
        queue_put(q);
        return result;
    }
    {
        KERNEL_RAW_SCOPE(g, &q->lock);
        if (q->removed)
            result = -KERNEL_EIDRM;
        else if (cmd == KERNEL_IPC_SET) {
            if (!owner(q, c))
                result = -KERNEL_EPERM;
            else if ((uint32_t)d.msg_qbytes > KERNEL_MSGMNB &&
                     !kernel_credentials_capable(c, KERNEL_CAP_SYS_RESOURCE))
                result = -KERNEL_EPERM;
            else if (d.msg_perm.uid == UINT32_MAX || d.msg_perm.gid == UINT32_MAX)
                result = -KERNEL_EINVAL;
            else {
                q->permission.uid = d.msg_perm.uid;
                q->permission.gid = d.msg_perm.gid;
                q->permission.mode = d.msg_perm.mode & 0777;
                /* Linux msgctl_down receives qbytes through a signed int,
                 * then stores that value in the native unsigned long field. */
                q->limit = (uint64_t)(int64_t)(int32_t)d.msg_qbytes;
                q->ctime = msg_now();
                while (q->recv_head)
                    request_complete(q->recv_head, 1);
                wake_senders(q);
            }
        } else if (cmd != KERNEL_MSG_STAT_ANY && (result = permission(q, c, 4))) {
        } else {
            d.msg_perm = q->permission;
            d.msg_stime = q->stime;
            d.msg_rtime = q->rtime;
            d.msg_ctime = q->ctime;
            d.msg_cbytes = q->bytes;
            d.msg_qnum = q->count;
            d.msg_qbytes = q->limit;
            d.msg_lspid = q->spid ? q->spid->number : 0;
            d.msg_lrpid = q->rpid ? q->rpid->number : 0;
            result = cmd == KERNEL_IPC_STAT ? 0 : q->id;
        }
    }
    queue_put(q);
    if (result >= 0 && cmd != KERNEL_IPC_SET &&
        kernel_copy_to_user(mm, arg, &d, sizeof(d), &copied))
        return -KERNEL_EFAULT;
    return result;
}
void kernel_msg_abort(struct kernel_task *task)
{
    struct kernel_msg_request *r = task->msg_request;
    if (!r)
        return;
    if (r->queue) {
        KERNEL_RAW_SCOPE(g, &r->queue->lock);
        request_remove(r);
    }
    task->msg_request = 0;
    request_release(r);
}
