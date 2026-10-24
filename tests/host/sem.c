/* Focused ownership/interleaving fixture: real sem.c, raw_lock.c and park.c;
 * heap, usercopy, PID registry and context-switch machine boundaries are modeled. */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/sem.c"

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
static pthread_barrier_t parked, proceed;
static _Thread_local int block_handshake;
static _Thread_local int abandon_import;
static int abandon_park;
static _Thread_local int wake_on_park = -1;
static _Thread_local int use_permission;
static struct kernel_credentials nobody = {.euid = 40, .egid = 50};
void *sync_test_cpu(void) { return &cpu; }
void *sync_test_current_task(void) { return cpu.current; }
struct kernel_task *kernel_task_current(void) { return cpu.current; }
void kernel_cpu_flush_notifications(void) { assert(!cpu.outbound); }
void kernel_assert_can_block(void) { assert(!cpu.raw_locks); }
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
uint64_t kernel_time_realtime_ns(void) { return time_ns; }
uint64_t kernel_time_monotonic_ns(void) { return time_ns; }
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
    if (abandon_import)
        pthread_exit(0);
    memcpy(d, (void *)(uintptr_t)s, n);
    *c = n;
    return 0;
}
enum kernel_uaccess_status kernel_copy_to_user(struct kernel_mm *m, uint64_t d, const void *s,
                                               size_t n, size_t *c)
{
    return kernel_copy_from_user(m, (void *)(uintptr_t)d, (uintptr_t)s, n, c);
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
struct kernel_wait_record *kernel_wait_record_of(struct kernel_task *t) { return &t->wait; }
struct kernel_wait_node *kernel_wait_default_node(struct kernel_task *t)
{
    return &t->default_wait_node;
}
struct kernel_task *kernel_wait_current_task(void) { return cpu.current; }
int kernel_wait_backend_initialized(void) { return 1; }
int kernel_wait_backend_signal(struct kernel_task *t)
{
    (void)t;
    return 0;
}
uint64_t kernel_wait_backend_time(void) { return time_ns; }
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
void kernel_wait_backend_ready(struct kernel_task *t) { (void)t; }
void kernel_wait_backend_quiesce(void) { sched_yield(); }
static void barrier(pthread_barrier_t *b)
{
    int r = pthread_barrier_wait(b);
    assert(!r || r == PTHREAD_BARRIER_SERIAL_THREAD);
}
enum kernel_scheduler_status kernel_wait_backend_switch(struct kernel_task *t)
{
    if (block_handshake) {
        barrier(&parked);
        barrier(&proceed);
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
static void current(unsigned n) { cpu.current = &task[n]; }
static int make(unsigned n) { return (int)kernel_sem_get(cpu.current, 0, (int)n, 0600); }
static int64_t ctl(int id, int n, int c, uint64_t a)
{
    return kernel_sem_ctl(cpu.current, &cpu.current->mm, id, n, c, a);
}
static int64_t op(int id, struct kernel_sembuf *o, unsigned n)
{
    return kernel_sem_op(cpu.current, &cpu.current->mm, id, (uintptr_t)o, n, 0);
}
static int val(int id, int n) { return (int)ctl(id, n, KERNEL_SEM_GETVAL, 0); }
static void rm(int id) { assert(!ctl(id, 0, KERNEL_IPC_RMID, 0)); }
static int waitid;
static struct kernel_sembuf waitops[3];
static unsigned waitcount;
static int64_t waitresult;
static void *waiter(void *unused)
{
    (void)unused;
    current(1);
    block_handshake = 1;
    waitresult = op(waitid, waitops, waitcount);
    return 0;
}
static pthread_t start_wait(int id, struct kernel_sembuf *ops, unsigned n)
{
    pthread_t t;
    waitid = id;
    waitcount = n;
    memcpy(waitops, ops, n * sizeof(*ops));
    assert(!pthread_create(&t, 0, waiter, 0));
    barrier(&parked);
    return t;
}
static void end_wait(pthread_t t, int expected)
{
    barrier(&proceed);
    assert(!pthread_join(t, 0));
    assert(waitresult == expected && !task[1].sem_request && !task[1].wait.borrows);
}
static _Thread_local struct kernel_raw_lock *pause_lock;
void __real_kernel_raw_lock_acquire(struct kernel_raw_lock *, struct kernel_raw_guard *);
void __wrap_kernel_raw_lock_acquire(struct kernel_raw_lock *lock, struct kernel_raw_guard *guard)
{
    if (lock == pause_lock) {
        pause_lock = 0;
        barrier(&parked);
        barrier(&proceed);
    }
    __real_kernel_raw_lock_acquire(lock, guard);
}
static struct sem_set *exit_set;
static void *exiter(void *unused)
{
    (void)unused;
    current(1);
    pause_lock = &exit_set->lock;
    kernel_sem_exit(&task[1]);
    return 0;
}
static void remove_exit_race(void)
{
    int id = make(1);
    current(1);
    struct kernel_sembuf x = {0, 1, KERNEL_SEM_UNDO};
    assert(!op(id, &x, 1));
    current(0);
    exit_set = set_lookup(id, 0);
    assert(exit_set);
    set_put(exit_set);
    pthread_t t;
    assert(!pthread_create(&t, 0, exiter, 0));
    barrier(&parked);
    rm(id);
    assert(!sem_table.sets && !sem_table.members);
    barrier(&proceed);
    assert(!pthread_join(t, 0));
    assert(!allocations && !task[1].sem_undo);
    puts("PASS RMID wins after last undo owner borrows entry and set");
}
static void basic(void)
{
    int a = make(3), b = make(1);
    assert(a == 0 && b == 1); /* catches index-result vs allocation-sentinel collision */
    uint16_t init[] = {2, 0, 1};
    assert(!ctl(a, 0, KERNEL_SEM_SETALL, (uintptr_t)init));
    struct kernel_sembuf vec[] = {
        {0, -1, KERNEL_SEM_UNDO}, {0, -1, KERNEL_SEM_UNDO}, {1, -1, KERNEL_IPC_NOWAIT}};
    assert(op(a, vec, 3) == -KERNEL_EAGAIN && val(a, 0) == 2 && val(a, 1) == 0);
    vec[2] = (struct kernel_sembuf){2, -1, 0};
    assert(!op(a, vec, 3) && val(a, 0) == 0 && val(a, 2) == 0);
    struct kernel_sembuf overflow[] = {{1, 32767, 0}, {1, 1, 0}};
    assert(op(a, overflow, 2) == -KERNEL_ERANGE && val(a, 1) == 0);
    kernel_sem_exit(cpu.current);
    assert(val(a, 0) == 2);
    kernel_sem_exit(cpu.current);
    struct kernel_semid64_ds ds;
    assert(!ctl(a, 0, KERNEL_IPC_STAT, (uintptr_t)&ds) && ds.sem_nsems == 3 && ds.sem_otime == 1);
    use_permission = 1;
    assert(ctl(a, 0, KERNEL_SEM_GETVAL, 0) == -KERNEL_EACCES);
    assert(ctl(a, 0, KERNEL_IPC_RMID, 0) == -KERNEL_EPERM);
    assert(ctl(a & 32767, 0, KERNEL_SEM_STAT_ANY, (uintptr_t)&ds) == a);
    use_permission = 0;
    rm(a);
    assert(ctl(a, 0, KERNEL_SEM_GETVAL, 0) == -KERNEL_EINVAL);
    rm(b);
    assert(!allocations);
    puts("PASS atomic duplicate rollback, range, permission, info and PID owners");
}
static void undo_share(void)
{
    int id = make(1);
    assert(!ctl(id, 0, KERNEL_SEM_SETVAL, 2));
    assert(!kernel_sem_clone(&task[0], &task[1], 1));
    current(1);
    struct kernel_sembuf x = {0, -1, KERNEL_SEM_UNDO};
    assert(!op(id, &x, 1));
    kernel_sem_exit(&task[1]);
    assert(val(id, 0) == 1);
    current(0);
    kernel_sem_exit(&task[0]);
    assert(val(id, 0) == 2);
    assert(!kernel_sem_clone(&task[0], &task[1], 1));
    assert(!op(id, &x, 1));
    assert(!ctl(id, 0, KERNEL_SEM_SETVAL, 7));
    kernel_sem_exit(&task[1]);
    kernel_sem_exit(&task[0]);
    assert(val(id, 0) == 7);
    x.sem_op = 1;
    assert(!op(id, &x, 1));
    unsigned baseline = allocations;
    rm(id);
    assert(allocations == 1 && baseline > allocations);
    assert(task[0].sem_undo && !task[0].sem_undo->entries);
    kernel_sem_exit(&task[0]);
    assert(!allocations);
    puts("PASS empty clone undo sharing, SETVAL clears, RMID releases entry storage");
}
static void waits(void)
{
    int id = make(2);
    struct kernel_sembuf vec[] = {{0, -1, KERNEL_SEM_UNDO}, {1, -1, 0}};
    assert(!ctl(id, 0, KERNEL_SEM_SETVAL, 1));
    pthread_t t = start_wait(id, vec, 2);
    assert(val(id, 0) == 1 && ctl(id, 1, KERNEL_SEM_GETNCNT, 0) == 1);
    waitops[0].sem_op = 123; /* The parked request owns its imported snapshot. */
    assert(!ctl(id, 1, KERNEL_SEM_SETVAL, 1));
    end_wait(t, 0);
    assert(val(id, 0) == 0 && val(id, 1) == 0);
    kernel_sem_exit(&task[1]);
    assert(val(id, 0) == 1);
    vec[0] = (struct kernel_sembuf){0, 0, 0};
    t = start_wait(id, vec, 1);
    assert(ctl(id, 0, KERNEL_SEM_GETZCNT, 0) == 1);
    rm(id);
    end_wait(t, -KERNEL_EIDRM);
    kernel_sem_exit(&task[1]);
    assert(!allocations);
    id = make(1);
    vec[0] = (struct kernel_sembuf){0, -1, 0};
    wake_on_park = KERNEL_WAIT_SIGNALLED;
    assert(op(id, vec, 1) == -KERNEL_EINTR);
    wake_on_park = KERNEL_WAIT_TIMEOUT;
    assert(op(id, vec, 1) == -KERNEL_EAGAIN);
    wake_on_park = -1;
    rm(id);
    assert(!allocations);
    puts(
        "PASS real prepare/park/finish completion, first blocking count, RMID, signal and timeout");
}
static void oom(void)
{
    for (int n = 0; n < 4; n++) {
        int id = make(1);
        unsigned before = allocations;
        struct kernel_sembuf x = {0, 1, KERNEL_SEM_UNDO};
        allocation_calls = 0;
        fail_at = n;
        int64_t r = op(id, &x, 1);
        fail_at = -1;
        assert(r == 0 || r == -KERNEL_ENOMEM);
        assert(!task[0].sem_request);
        if (r)
            assert(!val(id, 0));
        kernel_sem_exit(&task[0]);
        assert(allocations == before);
        rm(id);
        assert(!allocations);
    }
    fail_at = 0;
    allocation_calls = 0;
    assert(kernel_sem_clone(&task[0], &task[1], 1) == -KERNEL_ENOMEM);
    fail_at = -1;
    assert(!task[0].sem_undo && !task[1].sem_undo && !allocations);
    puts("PASS request/group/entry ENOMEM releases all owners");
}
static void forced_exit(void)
{
    int id = make(1);
    current(1);
    struct kernel_sembuf initial = {0, 1, KERNEL_SEM_UNDO};
    assert(!op(id, &initial, 1));
    current(0);
    struct kernel_sembuf pending = {0, -2, KERNEL_SEM_UNDO};
    pthread_t t = start_wait(id, &pending, 1);
    assert(ctl(id, 0, KERNEL_SEM_GETNCNT, 0) == 1);
    abandon_park = 1;
    barrier(&proceed);
    assert(!pthread_join(t, 0));
    abandon_park = 0;
    kernel_sem_abort(&task[1]);
    assert(!task[1].sem_request && ctl(id, 0, KERNEL_SEM_GETNCNT, 0) == 0);
    kernel_sem_exit(&task[1]);
    assert(val(id, 0) == 0 && ctl(id, 0, KERNEL_SEM_GETPID, 0) == pid[1].number);
    kernel_sem_exit(&task[1]);
    rm(id);
    assert(!allocations);
    puts("PASS forced syscall-stack abandonment releases request and uses exiting TGID");
}
static void limits_and_reuse(void)
{
    struct kernel_seminfo info;
    assert(ctl(0, 0, KERNEL_IPC_INFO, (uintptr_t)&info) >= 0);
    int *ids = calloc((size_t)info.semmni, sizeof(*ids));
    assert(ids);
    for (int i = 0; i < info.semmni; i++) {
        ids[i] = make(1);
        assert(ids[i] >= 0);
    }
    assert(make(1) == -KERNEL_ENOSPC);
    assert(ctl(0, 0, KERNEL_SEM_INFO, (uintptr_t)&info) >= 0 && info.semusz == info.semmni &&
           info.semaem == info.semmni);
    int old = ids[0];
    rm(old);
    ids[0] = make(1);
    assert(ids[0] >= 0 && ids[0] != old);
    assert(ctl(old, 0, KERNEL_SEM_GETVAL, 0) == -KERNEL_EINVAL);
    for (int i = 0; i < info.semmni; i++)
        rm(ids[i]);
    free(ids);
    assert(!allocations);
    puts("PASS published 32000-set quota, namespace counters and ID slot reuse");
}
static void undo_boundaries_and_thread_pid(void)
{
    int id = make(1);
    struct kernel_sembuf x = {0, 32767, KERNEL_SEM_UNDO};
    assert(!op(id, &x, 1));
    x = (struct kernel_sembuf){0, -32767, 0};
    assert(!op(id, &x, 1));
    x = (struct kernel_sembuf){0, 1, KERNEL_SEM_UNDO};
    assert(!op(id, &x, 1));
    x = (struct kernel_sembuf){0, -1, 0};
    assert(!op(id, &x, 1));
    x = (struct kernel_sembuf){0, 1, KERNEL_SEM_UNDO};
    assert(op(id, &x, 1) == -KERNEL_ERANGE && !val(id, 0));
    kernel_sem_exit(&task[0]);
    assert(!val(id, 0));
    assert(!ctl(id, 0, KERNEL_SEM_SETVAL, 32767));
    x = (struct kernel_sembuf){0, -32767, KERNEL_SEM_UNDO};
    assert(!op(id, &x, 1));
    x = (struct kernel_sembuf){0, 32767, 0};
    assert(!op(id, &x, 1));
    kernel_sem_exit(&task[0]);
    assert(val(id, 0) == 32767);
    assert(!kernel_sem_clone(&task[0], &task[1], 1));
    task[1].group_leader = &task[0];
    task[1].identities[KERNEL_PID_TGID].identity = 0;
    current(1);
    x = (struct kernel_sembuf){0, -1, KERNEL_SEM_UNDO};
    assert(!op(id, &x, 1));
    assert(ctl(id, 0, KERNEL_SEM_GETPID, 0) == pid[0].number);
    current(0);
    kernel_sem_exit(&task[0]);
    kernel_sem_exit(&task[1]);
    assert(val(id, 0) == 32767 && ctl(id, 0, KERNEL_SEM_GETPID, 0) == pid[0].number);
    task[1].group_leader = &task[1];
    task[1].identities[KERNEL_PID_TGID].identity = &pid[1];
    rm(id);
    assert(!allocations);
    puts("PASS signed undo boundary, exit clamping and nonleader TGID ownership");
}
static void *import_abandoner(void *unused)
{
    (void)unused;
    current(1);
    abandon_import = 1;
    struct kernel_sembuf operation = {0, 0, 0};
    (void)op(INT32_MAX, &operation, 1);
    assert(!"import must be reached before stale ID lookup");
    return 0;
}
static void import_order(void)
{
    int stale = make(1);
    rm(stale);
    int id = make(1);
    struct {
        int64_t seconds, nanoseconds;
    } negative = {-1, 0};
    struct kernel_sembuf operation = {0, 0, 0};
    assert(kernel_sem_op(cpu.current, &cpu.current->mm, stale, 1, 1, 0) == -KERNEL_EFAULT);
    assert(kernel_sem_op(cpu.current, &cpu.current->mm, -1, 1, 1, 0) == -KERNEL_EFAULT);
    assert(kernel_sem_op(cpu.current, &cpu.current->mm, id, 1, 1, (uintptr_t)&negative) ==
           -KERNEL_EFAULT);
    assert(kernel_sem_op(cpu.current, &cpu.current->mm, -1, 1, 0, 1) == -KERNEL_EFAULT);
    assert(kernel_sem_op(cpu.current, &cpu.current->mm, id, 1, KERNEL_SEMOPM + 1, 1) ==
           -KERNEL_EFAULT);
    assert(kernel_sem_op(cpu.current, &cpu.current->mm, id, 1, KERNEL_SEMOPM + 1,
                         (uintptr_t)&negative) == -KERNEL_E2BIG);
    assert(kernel_sem_op(cpu.current, &cpu.current->mm, id, (uintptr_t)&operation, 1,
                         (uintptr_t)&negative) == -KERNEL_EINVAL);
    assert(!cpu.current->sem_request);
    unsigned before = allocations;
    pthread_t importer;
    assert(!pthread_create(&importer, 0, import_abandoner, 0));
    assert(!pthread_join(importer, 0));
    kernel_sem_abort(&task[1]);
    assert(!task[1].sem_request && allocations == before);
    rm(id);
    assert(!allocations);
    puts("PASS timeout/count/vector/ID validation order and unbound import owner cleanup");
}
int main(void)
{
    kernel_wait_domain_init();
    assert(!kernel_sem_init(&heap));
    for (unsigned i = 0; i < 4; i++) {
        pid[i].number = (int)i + 10;
        pid[i].references = 1;
        task[i].identities[KERNEL_PID_TGID].identity = &pid[i];
        task[i].group_leader = &task[i];
    }
    assert(!pthread_barrier_init(&parked, 0, 2) && !pthread_barrier_init(&proceed, 0, 2));
    current(0);
    basic();
    import_order();
    undo_share();
    waits();
    oom();
    remove_exit_race();
    undo_boundaries_and_thread_pid();
    limits_and_reuse();
    forced_exit();
    for (unsigned i = 0; i < 4; i++)
        assert(pid[i].references == 1);
    assert(!sem_table.sets && !sem_table.members && !allocations);
    puts("SEM CORE HOST PASS");
    return 0;
}
