/* Focused ownership/interleaving fixture: real msg.c, raw_lock.c and park.c;
 * heap, usercopy, PID registry and context-switch machine boundaries are modeled. */
#include "../../kernel/msg.c"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Thread_local uintptr_t sync_test_irq = 2;
static _Thread_local struct kernel_cpu cpu = {.initialized = KERNEL_CPU_INITIALIZED};
static struct kernel_task task[4];
static struct kernel_pid pid[4];
static struct kernel_heap heap;
static unsigned allocations;
static _Thread_local int fail_at = -1;
static _Thread_local unsigned allocation_calls;
static struct kernel_credentials cred = {.euid = 10, .egid = 20};
static uint64_t time_ns = 1000000000;
static _Thread_local int block_handshake;
static pthread_mutex_t handshake_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t handshake_cond = PTHREAD_COND_INITIALIZER;
static unsigned entered, released;
static _Thread_local int abandon_import;
static int abandon_park;
static _Thread_local int abandon_export;
static _Thread_local int pause_export;
static _Thread_local uint64_t output_fault_at;
static _Thread_local struct kernel_uaccess_write_cursor *active_output;
static _Thread_local int wake_on_park = -1;
static _Thread_local int use_permission;
static _Thread_local int pause_before_park, pause_notify;
static _Thread_local struct kernel_task *aborting;
static struct kernel_credentials nobody = {.euid = 40, .egid = 50};
static void hold_at_boundary(void)
{
    unsigned bit = 1U << (unsigned)(cpu.current - task);
    assert(!pthread_mutex_lock(&handshake_lock));
    entered |= bit;
    assert(!pthread_cond_broadcast(&handshake_cond));
    while (!(released & bit))
        assert(!pthread_cond_wait(&handshake_cond, &handshake_lock));
    assert(!pthread_mutex_unlock(&handshake_lock));
}
enum kernel_scheduler_status __real_kernel_wait_park(const struct kernel_wait_token *,
                                                    enum kernel_wait_wake_reason *);
enum kernel_scheduler_status __wrap_kernel_wait_park(const struct kernel_wait_token *token,
                                                    enum kernel_wait_wake_reason *reason)
{
    if (pause_before_park) {
        pause_before_park = 0;
        hold_at_boundary();
    }
    return __real_kernel_wait_park(token, reason);
}
enum kernel_scheduler_status __real_kernel_wait_notify(const struct kernel_wait_token *,
                                                      enum kernel_wait_wake_reason);
enum kernel_scheduler_status __wrap_kernel_wait_notify(const struct kernel_wait_token *token,
                                                      enum kernel_wait_wake_reason reason)
{
    if (pause_notify) {
        pause_notify = 0;
        hold_at_boundary();
    }
    return __real_kernel_wait_notify(token, reason);
}
void __real_kernel_raw_lock_acquire(struct kernel_raw_lock *, struct kernel_raw_guard *);
void __wrap_kernel_raw_lock_acquire(struct kernel_raw_lock *lock, struct kernel_raw_guard *guard)
{
    if (aborting && aborting->msg_request && lock == &aborting->msg_request->queue->lock) {
        assert(!pthread_mutex_lock(&handshake_lock));
        entered |= 1U << (unsigned)(cpu.current - task);
        assert(!pthread_cond_broadcast(&handshake_cond));
        assert(!pthread_mutex_unlock(&handshake_lock));
    }
    __real_kernel_raw_lock_acquire(lock, guard);
}
void *sync_test_cpu(void)
{
    return &cpu;
}
void *sync_test_current_task(void)
{
    return cpu.current;
}
struct kernel_task *kernel_task_current(void)
{
    return cpu.current;
}
void kernel_cpu_flush_notifications(void)
{
    assert(!cpu.outbound);
}
void kernel_assert_can_block(void)
{
    assert(!cpu.raw_locks);
}
const struct kernel_credentials *kernel_credentials_current(void)
{
    return use_permission ? &nobody : &cred;
}
int kernel_credentials_in_group(const struct kernel_credentials *c, uint32_t g)
{
    return c->egid == g;
}
int kernel_credentials_capable(const struct kernel_credentials *c, unsigned cap)
{
    (void)c;
    (void)cap;
    return 0;
}
uint64_t kernel_time_realtime_ns(void)
{
    return time_ns;
}
uint64_t kernel_time_monotonic_ns(void)
{
    return time_ns;
}
enum kernel_time_status kernel_time_deadline_from_monotonic(uint64_t n, uint64_t *d)
{
    *d = n;
    return n <= time_ns ? KERNEL_TIME_STATUS_DEADLINE_PASSED : 0;
}
enum kernel_heap_status kernel_heap_allocate_zeroed(struct kernel_heap *h, size_t n, size_t z,
                                                    void **p)
{
    (void)h;
    assert(!cpu.raw_locks);
    if ((int)allocation_calls++ == fail_at)
        return KERNEL_HEAP_STATUS_EMPTY;
    *p = calloc(n, z);
    assert(*p);
    __atomic_fetch_add(&allocations, 1, __ATOMIC_RELAXED);
    return 0;
}
void kernel_heap_release(struct kernel_heap *h, void *p)
{
    (void)h;
    assert(!cpu.raw_locks && p && allocations);
    __atomic_fetch_sub(&allocations, 1, __ATOMIC_RELAXED);
    free(p);
}
enum kernel_uaccess_status kernel_copy_from_user(struct kernel_mm *m, void *d, uint64_t s, size_t n,
                                                 size_t *c)
{
    (void)m;
    assert(!cpu.raw_locks && c);
    if (s < 4096)
        return KERNEL_UACCESS_STATUS_FAULT;
    if (abandon_import && !--abandon_import)
        pthread_exit(0);
    memcpy(d, (void *)(uintptr_t)s, n);
    *c = n;
    return 0;
}
enum kernel_uaccess_status kernel_copy_to_user(struct kernel_mm *m, uint64_t d, const void *s,
                                               size_t n, size_t *c)
{
    if (pause_export) {
        pause_export = 0;
        unsigned bit = 1U << (unsigned)(cpu.current - task);
        assert(!pthread_mutex_lock(&handshake_lock));
        entered |= bit;
        assert(!pthread_cond_broadcast(&handshake_cond));
        while (!(released & bit))
            assert(!pthread_cond_wait(&handshake_cond, &handshake_lock));
        assert(!pthread_mutex_unlock(&handshake_lock));
    }
    if (abandon_export)
        pthread_exit(0);
    if (d < 4096 || d == output_fault_at)
        return KERNEL_UACCESS_STATUS_FAULT;
    return kernel_copy_from_user(m, (void *)(uintptr_t)d, (uintptr_t)s, n, c);
}
/* This seam checks cursor completion and shares the existing usercopy fault
 * boundary. It does not model physical page pinning or its performance. */
enum kernel_uaccess_status kernel_uaccess_write_cursor_copy(
    struct kernel_mm *m, struct kernel_uaccess_write_cursor *cursor,
    uint64_t destination, const void *source, size_t size, size_t *copied)
{
    assert(!active_output || active_output == cursor);
    active_output = cursor;
    return kernel_copy_to_user(m, destination, source, size, copied);
}
void kernel_uaccess_write_cursor_finish(struct kernel_uaccess_write_cursor *cursor)
{
    assert(active_output == cursor);
    active_output = 0;
}
void kernel_pid_get(struct kernel_pid *p)
{
    assert(cpu.raw_locks && cpu.raw_locks->lock == &kernel_wait_domain && p && p->references);
    p->references++;
}
void kernel_pid_put(struct kernel_pid *p)
{
    assert(cpu.raw_locks && cpu.raw_locks->lock == &kernel_wait_domain && p && p->references > 1);
    p->references--;
}
void kernel_task_identity_put(struct kernel_pid *p)
{
    KERNEL_RAW_SCOPE(g, &kernel_wait_domain);
    kernel_pid_put(p);
}
struct kernel_wait_record *kernel_wait_record_of(struct kernel_task *t)
{
    return &t->wait;
}
struct kernel_wait_node *kernel_wait_default_node(struct kernel_task *t)
{
    return &t->default_wait_node;
}
struct kernel_task *kernel_wait_current_task(void)
{
    return cpu.current;
}
int kernel_wait_backend_initialized(void)
{
    return 1;
}
int kernel_signal_has_pending(const struct kernel_task *t)
{
    return __atomic_load_n(&t->signal_pending, __ATOMIC_RELAXED) != 0;
}
int kernel_signal_termination_requested(const struct kernel_task *t)
{
    return __atomic_load_n(&t->terminate_requested, __ATOMIC_RELAXED) != 0;
}
int kernel_wait_backend_signal(struct kernel_task *t)
{
    (void)t;
    return 0;
}
uint64_t kernel_wait_backend_time(void)
{
    return time_ns;
}
void kernel_wait_backend_commit(struct kernel_task *t, uint64_t d, int i)
{
    (void)d;
    (void)i;
    t->wait.on_cpu = 0;
}
void kernel_wait_backend_notify(struct kernel_task *t, uint32_t r)
{
    (void)t;
    (void)r;
}
void kernel_wait_backend_ready(struct kernel_task *t)
{
    (void)t;
}
void kernel_wait_backend_quiesce(void)
{
    sched_yield();
}
enum kernel_scheduler_status kernel_wait_backend_switch(struct kernel_task *t)
{
    if (block_handshake) {
        unsigned bit = 1U << (unsigned)(t - task);
        assert(!pthread_mutex_lock(&handshake_lock));
        entered |= bit;
        assert(!pthread_cond_broadcast(&handshake_cond));
        while (!(released & bit))
            assert(!pthread_cond_wait(&handshake_cond, &handshake_lock));
        assert(!pthread_mutex_unlock(&handshake_lock));
        if (abandon_park) {
            /* Model forced scheduler teardown: finish the stopped wait on its
             * executing CPU, then abandon the syscall stack without unwinding. */
            struct kernel_wait_token token = {t, t->wait.generation};
            kernel_wait_notify(&token, KERNEL_WAIT_SIGNALLED);
            assert(!kernel_wait_finish(&token));
            pthread_exit(0);
        }
    }
    if (wake_on_park >= 0) {
        struct kernel_wait_token token = {t, t->wait.generation};
        kernel_wait_notify(&token, wake_on_park);
    }
    {
        KERNEL_RAW_SCOPE(g, &kernel_wait_domain);
        assert(t->wait.phase == KERNEL_WAIT_NOTIFIED);
        t->wait.on_cpu = 1;
    }
    return 0;
}
static void current(unsigned n)
{
    cpu.current = &task[n];
}
static int make(void)
{
    int q = (int)kernel_msg_get(cpu.current, 0, 0600);
    assert(q >= 0);
    return q;
}
static int64_t ctl(int q, int cmd, void *p)
{
    return kernel_msg_ctl(cpu.current, &cpu.current->mm, q, cmd, (uintptr_t)p);
}
static void rm(int q)
{
    assert(!ctl(q, KERNEL_IPC_RMID, 0));
}
struct packet {
    int64_t type;
    char data[8192];
};
static int64_t send(int q, struct packet *p, uint64_t n, int flags)
{
    return kernel_msg_send(cpu.current, &cpu.current->mm, q, (uintptr_t)p, n, flags);
}
static int64_t recv(int q, void *p, uint64_t n, int64_t type, int flags)
{
    int64_t result = kernel_msg_receive(cpu.current, &cpu.current->mm, q, (uintptr_t)p, n, type,
                                       flags);
    assert(!active_output);
    return result;
}
static struct kernel_msqid64_ds statq(int q)
{
    struct kernel_msqid64_ds d;
    assert(!ctl(q, KERNEL_IPC_STAT, &d));
    return d;
}
static void limit(int q, uint64_t n)
{
    struct kernel_msqid64_ds d = statq(q);
    d.msg_qbytes = n;
    assert(!ctl(q, KERNEL_IPC_SET, &d));
}
static void clean(void)
{
    assert(!msg_table.queues && !msg_table.messages && !msg_table.bytes && !allocations);
    for (unsigned i = 0; i < 4; i++)
        assert(!task[i].msg_request && !task[i].wait.borrows && pid[i].references == 1);
}
static void basic(void)
{
    struct packet p = {.type = 5, .data = "five"}, o;
    int q = make();
    assert(q == 0);
    assert(!send(q, &p, 4, KERNEL_IPC_NOWAIT));
    p.type = 2;
    memcpy(p.data, "two1", 4);
    assert(!send(q, &p, 4, 0));
    memcpy(p.data, "two2", 4);
    assert(!send(q, &p, 4, 0));
    p.type = 7;
    assert(!send(q, &p, 0, 0));
    assert(recv(q, &o, 4, -6, 0) == 4 && o.type == 2 && !memcmp(o.data, "two1", 4));
    assert(recv(q, &o, 4, 2, 0) == 4 && !memcmp(o.data, "two2", 4));
    assert(recv(q, &o, 0, 5, KERNEL_MSG_EXCEPT) == 0 && o.type == 7);
    assert(recv(q, &o, 4, INT64_MIN, 0) == 4 && o.type == 5);
    assert(recv(q, &o, 4, 0, KERNEL_IPC_NOWAIT) == -KERNEL_ENOMSG);
    p.type = 1;
    assert(!send(q, &p, 6, 0));
    assert(recv(q, &o, 1, 0, KERNEL_IPC_NOWAIT) == -KERNEL_E2BIG && statq(q).msg_qnum == 1);
    assert(recv(q, &o, 1, 0, KERNEL_MSG_NOERROR) == 1 && !statq(q).msg_qnum);
    limit(q, 2);
    assert(!send(q, &p, 0, 0) && !send(q, &p, 0, 0));
    assert(send(q, &p, 0, KERNEL_IPC_NOWAIT) == -KERNEL_EAGAIN);
    assert(!recv(q, &o, 0, 0, 0));
    assert(!send(q, &p, 2, 0));
    assert(send(q, &p, 1, KERNEL_IPC_NOWAIT) == -KERNEL_EAGAIN);
    rm(q);
    clean();
    puts("PASS selection FIFO negative LONG_MIN E2BIG truncation zero-length capacity");
}
static void copy_and_faults(void)
{
    int q = make();
    struct packet p = {.type = 8, .data = "payload"}, o = {0};
    assert(!send(q, &p, 7, 0));
    assert(recv(q, &o, 8192, 0, KERNEL_MSG_COPY | KERNEL_IPC_NOWAIT) == 7 && o.type == 8 &&
           !memcmp(o.data, "payload", 7));
    assert(statq(q).msg_qnum == 1 && !statq(q).msg_rtime);
    assert(recv(q, &o, 1, 0, KERNEL_MSG_COPY | KERNEL_IPC_NOWAIT) == -KERNEL_E2BIG);
    assert(recv(q, &o, 1, 0, KERNEL_MSG_COPY | KERNEL_MSG_NOERROR | KERNEL_IPC_NOWAIT) ==
           -KERNEL_EINVAL);
    assert(recv(q, (void *)1, 8192, 0, KERNEL_MSG_COPY | KERNEL_IPC_NOWAIT) == -KERNEL_EFAULT);
    assert(recv(INT32_MAX, (void *)1, 8192, 0, KERNEL_MSG_COPY | KERNEL_IPC_NOWAIT) ==
           -KERNEL_EFAULT);
    assert(recv(q, &o, 0, 1, KERNEL_MSG_COPY | KERNEL_IPC_NOWAIT) == -KERNEL_ENOMSG);
    assert(recv(q, &o, 0, -1, KERNEL_MSG_COPY | KERNEL_IPC_NOWAIT) == -KERNEL_ENOMSG);
    assert(recv(q, &o, 1, 0, KERNEL_MSG_COPY) == -KERNEL_EINVAL);
    assert(kernel_msg_send(cpu.current, &cpu.current->mm, -1, 1, 8193, 0) == -KERNEL_EFAULT);
    assert(send(-1, &p, 1, 0) == -KERNEL_EINVAL);
    assert(recv(q, (void *)1, 8192, 0, 0) == -KERNEL_EFAULT && !statq(q).msg_qnum);
    for (unsigned copy = 0; copy < 2; copy++) {
        assert(!send(q, &p, 7, 0));
        o.type = -1;
        o.data[0] = 99;
        output_fault_at = (uintptr_t)o.data;
        assert(recv(q, &o, 7, 0, copy ? KERNEL_MSG_COPY | KERNEL_IPC_NOWAIT : 0) ==
               -KERNEL_EFAULT);
        output_fault_at = 0;
        assert(o.type == 8 && o.data[0] == 99 && statq(q).msg_qnum == copy);
        if (copy)
            assert(!recv(q, &o, 0, 0, KERNEL_MSG_NOERROR));
    }
    struct kernel_msginfo i;
    assert(ctl(0, KERNEL_MSG_INFO, &i) >= 0 && i.msgpool == 1 && !i.msgmap && !i.msgtql);
    assert(ctl(q, KERNEL_IPC_STAT | KERNEL_IPC_64, &o) == -KERNEL_EINVAL);
    rm(q);
    clean();
    puts("PASS MSG_COPY prepare input faults ordinal EINVAL E2BIG no mutation and consumed EFAULT");
}
struct wait_case {
    unsigned task;
    int id, sending, flags, permission, export_pause, before_park, notify_pause;
    uint64_t size;
    int64_t type, result;
    struct packet packet;
};
static void *waiter(void *ptr)
{
    struct wait_case *w = ptr;
    current(w->task);
    block_handshake = 1;
    use_permission = w->permission;
    pause_export = w->export_pause;
    pause_before_park = w->before_park;
    pause_notify = w->notify_pause;
    w->result = w->sending ? send(w->id, &w->packet, w->size, w->flags)
                           : recv(w->id, &w->packet, w->size, w->type, w->flags);
    return 0;
}
static pthread_t start(struct wait_case *w)
{
    pthread_t t;
    unsigned bit = 1U << w->task;
    assert(!pthread_mutex_lock(&handshake_lock));
    entered &= ~bit;
    released &= ~bit;
    assert(!pthread_mutex_unlock(&handshake_lock));
    assert(!pthread_create(&t, 0, waiter, w));
    assert(!pthread_mutex_lock(&handshake_lock));
    while (!(entered & bit))
        assert(!pthread_cond_wait(&handshake_cond, &handshake_lock));
    assert(!pthread_mutex_unlock(&handshake_lock));
    return t;
}
static void end(pthread_t t, struct wait_case *w, int64_t result)
{
    assert(!pthread_mutex_lock(&handshake_lock));
    released |= 1U << w->task;
    assert(!pthread_cond_broadcast(&handshake_cond));
    assert(!pthread_mutex_unlock(&handshake_lock));
    assert(!pthread_join(t, 0));
    if (w->result != result)
        fprintf(stderr, "wait result actual=%lld expected=%lld\n", (long long)w->result,
                (long long)result);
    assert(w->result == result && !task[w->task].msg_request && !task[w->task].wait.borrows);
}
static void waits(void)
{
    int q = make();
    struct wait_case small = {.task = 1, .id = q, .size = 1, .type = 3},
                     large = {.task = 2, .id = q, .size = 20, .type = 3};
    pthread_t a = start(&small), b = start(&large);
    struct packet p = {.type = 3, .data = "delivery"};
    assert(!send(q, &p, 8, 0));
    end(a, &small, -KERNEL_E2BIG);
    end(b, &large, 8);
    assert(!memcmp(large.packet.data, "delivery", 8));
    assert(!statq(q).msg_qnum && statq(q).msg_lrpid == pid[2].number);
    limit(q, 0);
    struct wait_case capacity = {
        .task = 1, .id = q, .size = 0, .sending = 1, .packet = {.type = 3}};
    a = start(&capacity);
    /* Capacity is checked before direct delivery, even with a matching receiver. */
    large = (struct wait_case){.task = 2, .id = q, .size = 20, .type = 3};
    b = start(&large);
    assert(send(q, &p, 0, KERNEL_IPC_NOWAIT) == -KERNEL_EAGAIN);
    limit(q, 1);
    /* IPC_SET removes receivers for permission recheck. The sender commits only
     * after its own caller resumes; release receiver after sender publication. */
    end(a, &capacity, 0);
    end(b, &large, 0);
    assert(!statq(q).msg_qnum);
    rm(q);
    clean();
    puts("PASS explicit parked handshake direct E2BIG then delivery and capacity before handoff");
}
static void removal_and_permissions(void)
{
    int q = make();
    struct wait_case w = {.task = 1, .id = q, .size = 8};
    pthread_t t = start(&w);
    rm(q);
    end(t, &w, -KERNEL_EIDRM);
    clean();
    q = make();
    limit(q, 0);
    w = (struct wait_case){.task = 1, .id = q, .sending = 1, .size = 0, .packet = {.type = 1}};
    t = start(&w);
    rm(q);
    end(t, &w, -KERNEL_EIDRM);
    clean();
    q = make();
    struct kernel_msqid64_ds d = statq(q);
    d.msg_perm.mode = 0666;
    assert(!ctl(q, KERNEL_IPC_SET, &d));
    w = (struct wait_case){.task = 1, .id = q, .size = 8, .permission = 1};
    t = start(&w);
    d.msg_perm.mode = 0600;
    assert(!ctl(q, KERNEL_IPC_SET, &d));
    end(t, &w, -KERNEL_EACCES);
    rm(q);
    clean();
    q = make();
    limit(q, 0);
    w = (struct wait_case){.task = 1, .id = q, .sending = 1, .size = 0, .packet = {.type = 1}};
    t = start(&w);
    limit(q, 1);
    task[1].signal_pending = 1;
    end(t, &w, -KERNEL_EINTR);
    task[1].signal_pending = 0;
    assert(!statq(q).msg_qnum);
    rm(q);
    clean();
    /* The sender has already left the wait list after a capacity wake.
     * RMID must still win over a later signal before the sender resumes. */
    q = make();
    limit(q, 0);
    w = (struct wait_case){.task = 1, .id = q, .sending = 1, .size = 0, .packet = {.type = 1}};
    t = start(&w);
    limit(q, 1);
    assert(!task[1].msg_request->queued && task[1].msg_request->result == 1);
    rm(q);
    task[1].signal_pending = 1;
    end(t, &w, -KERNEL_EIDRM);
    task[1].signal_pending = 0;
    clean();
    /* A termination request without a pending signal also cancels retry;
     * the imported message must never be published on this recovery path. */
    q = make();
    limit(q, 0);
    w = (struct wait_case){.task = 1, .id = q, .sending = 1, .size = 0, .packet = {.type = 1}};
    t = start(&w);
    limit(q, 1);
    task[1].terminate_requested = 1;
    end(t, &w, -KERNEL_EINTR);
    task[1].terminate_requested = 0;
    assert(!statq(q).msg_qnum);
    rm(q);
    clean();
    q = make();
    wake_on_park = KERNEL_WAIT_SIGNALLED;
    struct packet p;
    assert(recv(q, &p, 8, 0, 0) == -KERNEL_EINTR);
    limit(q, 0);
    p.type = 1;
    assert(send(q, &p, 0, 0) == -KERNEL_EINTR);
    wake_on_park = -1;
    rm(q);
    clean();
    puts("PASS RMID terminal receiver sender IPC_SET permission reevaluation EINTR and pending "
         "signal after capacity wake");
}
static void oom(void)
{
    for (int n = 0; n < 1; n++) {
        fail_at = n;
        allocation_calls = 0;
        assert(kernel_msg_get(cpu.current, 0, 0600) == -KERNEL_ENOMEM);
        fail_at = -1;
        clean();
    }
    int q = make();
    struct packet p = {.type = 1};
    unsigned before = allocations;
    for (int n = 0; n < 2; n++) {
        fail_at = n;
        allocation_calls = 0;
        assert(send(q, &p, 8, 0) == -KERNEL_ENOMEM);
        fail_at = -1;
        assert(allocations == before && !task[0].msg_request);
    }
    for (int n = 0; n < 2; n++) {
        fail_at = n;
        allocation_calls = 0;
        assert(recv(q, &p, 8, 0, KERNEL_MSG_COPY | KERNEL_IPC_NOWAIT) == -KERNEL_ENOMEM);
        fail_at = -1;
        assert(allocations == before && !task[0].msg_request);
    }
    rm(q);
    clean();
    puts("PASS queue request body MSG_COPY allocation failures release all owners");
}
static int abandoned_id;
static unsigned abandon_mode;
static void *abandoner(void *unused)
{
    (void)unused;
    current(1);
    struct packet p = {.type = 1};
    if (abandon_mode == 0) {
        abandon_import = 2;
        send(abandoned_id, &p, 8, 0);
    } else if (abandon_mode == 1) {
        abandon_export = 1;
        recv(abandoned_id, &p, 8, 0, 0);
    } else if (abandon_mode == 2) {
        abandon_import = 1;
        recv(abandoned_id, &p, 8, 0, KERNEL_MSG_COPY | KERNEL_IPC_NOWAIT);
    } else {
        block_handshake = 1;
        recv(abandoned_id, &p, 8, 0, 0);
    }
    assert(!"must abandon syscall stack");
    return 0;
}
static void forced_exit(void)
{
    struct packet p = {.type = 1};
    for (abandon_mode = 0; abandon_mode < 3; abandon_mode++) {
        int q = make();
        abandoned_id = q;
        if (abandon_mode == 1)
            assert(!send(q, &p, 8, 0));
        unsigned before = allocations;
        pthread_t t;
        assert(!pthread_create(&t, 0, abandoner, 0));
        assert(!pthread_join(t, 0));
        assert(task[1].msg_request);
        kernel_msg_abort(&task[1]);
        assert(!task[1].msg_request);
        if (abandon_mode == 1)
            assert(allocations == before - 1);
        else
            assert(allocations == before);
        rm(q);
        clean();
    }
    int q = make();
    struct wait_case w = {.task = 1, .id = q, .size = 8};
    abandon_park = 1;
    pthread_t t = start(&w);
    assert(!pthread_mutex_lock(&handshake_lock));
    released |= 2;
    assert(!pthread_cond_broadcast(&handshake_cond));
    assert(!pthread_mutex_unlock(&handshake_lock));
    assert(!pthread_join(t, 0));
    abandon_park = 0;
    assert(task[1].msg_request);
    kernel_msg_abort(&task[1]);
    rm(q);
    clean();
    puts("PASS forced exit import COPY preparation detached export and queued wait release "
         "ownership");
}
static void copy_remove_race(void)
{
    for (unsigned copy = 0; copy < 2; copy++) {
        int q = make();
        struct packet p = {.type = 6, .data = "still-owned"};
        assert(!send(q, &p, 11, 0));
        struct wait_case w = {.task = 1,
                              .id = q,
                              .size = 8192,
                              .flags = copy ? (KERNEL_MSG_COPY | KERNEL_IPC_NOWAIT) : 0,
                              .export_pause = 1};
        pthread_t t = start(&w);
        assert(statq(q).msg_qnum == copy);
        rm(q);
        assert(!msg_table.queues && allocations);
        end(t, &w, 11);
        assert(w.packet.type == 6 && !memcmp(w.packet.data, "still-owned", 11));
        clean();
    }
    int q = make();
    struct wait_case w = {.task = 1, .id = q, .size = 8};
    pthread_t t = start(&w);
    struct packet p = {.type = 1};
    assert(!send(q, &p, 8, 0));
    assert(task[1].msg_request->message && !statq(q).msg_qnum);
    rm(q);
    abandon_park = 1;
    assert(!pthread_mutex_lock(&handshake_lock));
    released |= 2;
    assert(!pthread_cond_broadcast(&handshake_cond));
    assert(!pthread_mutex_unlock(&handshake_lock));
    assert(!pthread_join(t, 0));
    abandon_park = 0;
    kernel_msg_abort(&task[1]);
    clean();
    q = make();
    limit(q, 0);
    w = (struct wait_case){
        .task = 1, .id = q, .sending = 1, .size = 8, .packet = {.type = 1, .data = "snapshot"}};
    t = start(&w);
    memcpy(w.packet.data, "changed!", 8);
    limit(q, 8);
    end(t, &w, 0);
    struct packet out;
    assert(recv(q, &out, 8, 0, 0) == 8 && !memcmp(out.data, "snapshot", 8));
    rm(q);
    clean();
    q = make();
    limit(q, 0);
    w = (struct wait_case){.task = 1, .id = q, .sending = 1, .size = 8, .packet = {.type = 1}};
    t = start(&w);
    abandon_park = 1;
    assert(!pthread_mutex_lock(&handshake_lock));
    released |= 2;
    assert(!pthread_cond_broadcast(&handshake_cond));
    assert(!pthread_mutex_unlock(&handshake_lock));
    assert(!pthread_join(t, 0));
    abandon_park = 0;
    kernel_msg_abort(&task[1]);
    rm(q);
    clean();
    puts("PASS immutable COPY and detached receive survive RMID during output; delivered owner "
         "forced exit");
}
static void namespace_and_pid(void)
{
    int q = (int)kernel_msg_get(cpu.current, 123456, KERNEL_IPC_CREAT | KERNEL_IPC_EXCL | 0600);
    assert(q >= 0);
    assert(kernel_msg_get(cpu.current, 123456, 0) == q);
    assert(kernel_msg_get(cpu.current, 123456, KERNEL_IPC_CREAT | KERNEL_IPC_EXCL) ==
           -KERNEL_EEXIST);
    struct kernel_msqid64_ds d = statq(q);
    d.msg_qbytes = UINT64_C(0x100000001);
    assert(!ctl(q, KERNEL_IPC_SET, &d) && statq(q).msg_qbytes == 1);
    d.msg_qbytes = UINT64_C(0xffffffff);
    assert(ctl(q, KERNEL_IPC_SET, &d) == -KERNEL_EPERM);
    task[1].group_leader = &task[0];
    current(1);
    struct packet p = {.type = 1};
    assert(!send(q, &p, 1, 0));
    assert(recv(q, &p, 1, 0, 0) == 1);
    assert(statq(q).msg_lspid == pid[0].number && statq(q).msg_lrpid == pid[0].number);
    current(0);
    task[1].group_leader = &task[1];
    use_permission = 1;
    assert(ctl(q, KERNEL_IPC_STAT, &d) == -KERNEL_EACCES);
    assert(ctl((int)(q & 32767), KERNEL_MSG_STAT_ANY, &d) == q && d.msg_perm.key == 123456);
    assert(send(q, &p, 0, KERNEL_IPC_NOWAIT) == -KERNEL_EACCES &&
           ctl(q, KERNEL_IPC_RMID, 0) == -KERNEL_EPERM);
    use_permission = 0;
    rm(q);
    assert(kernel_msg_get(cpu.current, 123456, 0) == -KERNEL_ENOENT);
    msg_table.next_slot = (uint32_t)q & 32767U;
    int next = make();
    assert(next != q && ((uint32_t)next & 32767U) == ((uint32_t)q & 32767U));
    assert(ctl(q, KERNEL_IPC_STAT, &d) == -KERNEL_EINVAL);
    rm(next);
    clean();
    puts("PASS indexed key generation stale IDs STAT_ANY permissions native qbytes truncation and "
         "thread TGID");
}
static void nonleader_receive_pid(void)
{
    int q = make();
    task[1].group_leader = &task[0];
    struct wait_case w = {.task = 1, .id = q, .size = 8};
    pthread_t t = start(&w);
    struct packet p = {.type = 1};
    assert(!send(q, &p, 8, 0));
    /* Linux direct handoff records the receiver TID, queued receive its TGID. */
    assert(statq(q).msg_lrpid == pid[1].number);
    end(t, &w, 8);
    assert(!send(q, &p, 8, 0));
    current(1);
    assert(recv(q, &p, 8, 0, 0) == 8);
    assert(statq(q).msg_lrpid == pid[0].number);
    current(0);
    task[1].group_leader = &task[1];
    rm(q);
    clean();
    puts("PASS nonleader blocked direct receive records TID while queued receive records TGID");
}
static void *remote_abort(void *target)
{
    current(3);
    aborting = target;
    kernel_msg_abort(aborting);
    return 0;
}
static void completion_ownership(void)
{
    int q = make();
    struct packet p = {.type = 1, .data = "before-park"};
    struct wait_case w = {.task = 1, .id = q, .size = 11, .before_park = 1};
    pthread_t t = start(&w);
    assert(!send(q, &p, 11, 0));
    end(t, &w, 11);
    assert(!memcmp(w.packet.data, "before-park", 11));
    limit(q, 0);
    w = (struct wait_case){.task = 1, .id = q, .sending = 1, .size = 0,
                           .before_park = 1, .packet = {.type = 1}};
    t = start(&w);
    limit(q, 1);
    rm(q);
    end(t, &w, -KERNEL_EIDRM);
    clean();

    q = make();
    /* Stop the receiver's syscall stack before abort. Its request stays on the
     * queue, as it does between scheduler teardown and resource cleanup. */
    abandon_park = 1;
    w = (struct wait_case){.task = 1, .id = q, .size = 8};
    t = start(&w);
    assert(!pthread_mutex_lock(&handshake_lock));
    released |= 2;
    assert(!pthread_cond_broadcast(&handshake_cond));
    assert(!pthread_mutex_unlock(&handshake_lock));
    assert(!pthread_join(t, 0));
    abandon_park = 0;
    assert(task[1].msg_request && task[1].msg_request->queued);
    struct wait_case sender = {.task = 2, .id = q, .sending = 1, .size = 8,
                               .notify_pause = 1, .packet = {.type = 1}};
    pthread_t sending = start(&sender);
    assert(!pthread_mutex_lock(&handshake_lock));
    entered &= ~8U;
    assert(!pthread_mutex_unlock(&handshake_lock));
    pthread_t aborter;
    assert(!pthread_create(&aborter, 0, remote_abort, &task[1]));
    assert(!pthread_mutex_lock(&handshake_lock));
    while (!(entered & 8))
        assert(!pthread_cond_wait(&handshake_cond, &handshake_lock));
    assert(!pthread_mutex_unlock(&handshake_lock));
    end(sending, &sender, 0);
    assert(!pthread_join(aborter, 0));
    assert(!task[1].msg_request && !statq(q).msg_qnum);
    rm(q);
    clean();
    puts("PASS completion before park and remote abort during completed-request notification");
}
static void quota(void)
{
    int *ids = calloc(KERNEL_MSGMNI, sizeof(*ids));
    assert(ids);
    for (unsigned i = 0; i < KERNEL_MSGMNI; i++)
        ids[i] = make();
    assert(kernel_msg_get(cpu.current, 0, 0600) == -KERNEL_ENOSPC);
    struct kernel_msginfo i;
    assert(ctl(0, KERNEL_MSG_INFO, &i) == KERNEL_MSGMNI - 1 && i.msgpool == KERNEL_MSGMNI &&
           !i.msgmap && !i.msgtql);
    for (unsigned n = 0; n < KERNEL_MSGMNI; n++)
        rm(ids[n]);
    free(ids);
    clean();
    puts("PASS published 32000 queue quota and INFO counters return to zero");
}
int main(void)
{
    kernel_wait_domain_init();
    assert(!kernel_msg_init(&heap));
    for (unsigned i = 0; i < 4; i++) {
        pid[i].number = (int)i + 10;
        pid[i].references = 1;
        task[i].tid = pid[i].number;
        task[i].identities[KERNEL_PID_TGID].identity = &pid[i];
        task[i].identities[KERNEL_PID_TID].identity = &pid[i];
        task[i].group_leader = &task[i];
    }
    current(0);
    basic();
    copy_and_faults();
    waits();
    removal_and_permissions();
    oom();
    forced_exit();
    copy_remove_race();
    namespace_and_pid();
    nonleader_receive_pid();
    completion_ownership();
    quota();
    clean();
    puts("MSG CORE HOST PASS");
    return 0;
}
