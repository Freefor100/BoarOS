#include "../../../kernel/sched/private.h"
#include <assert.h>
#include <sched.h>
#include <pthread.h>

_Thread_local uintptr_t sync_test_irq = 1;
_Thread_local uint64_t mm_test_context;
_Thread_local int mm_test_wrong_owner;
static _Thread_local struct kernel_cpu cpu;
static _Thread_local struct kernel_io_context io, other_io;
struct mm_test_actor { struct kernel_task task; unsigned dispatched; };
static _Thread_local struct mm_test_actor actor;
static _Thread_local unsigned actor_id;
#if BOAROS_MAX_CPUS > 1
static _Thread_local struct kernel_mm *bound_mm;
static _Thread_local struct kernel_files *bound_files;
void mm_test_bind(struct kernel_mm *mm) { bound_mm = mm; }
void mm_test_bind_files(struct kernel_files *files) { bound_files = files; }
enum kernel_task_status kernel_task_files_borrow(struct kernel_task *task, struct kernel_files **files)
{
    if (task!=&actor.task || !bound_mm || !bound_files || !files)
        return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    *files=bound_files; return KERNEL_TASK_STATUS_OK;
}
enum kernel_task_status kernel_task_mm_borrow(const struct kernel_task *task, const struct kernel_mm **mm)
{
    if (task != &actor.task || !bound_mm || !mm) return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    *mm = bound_mm; return KERNEL_TASK_STATUS_OK;
}
#endif
static unsigned parked[16];
static pthread_once_t domain_once = PTHREAD_ONCE_INIT;
unsigned mm_test_parked(unsigned id) { assert(id < 16); return __atomic_load_n(&parked[id], __ATOMIC_ACQUIRE); }
void *sync_test_cpu(void)
{
    cpu.bootstrap_io = mm_test_wrong_owner ? &other_io : &io;
    return &cpu;
}
void mm_test_attach(unsigned id)
{
    kernel_cpu_initialize(&cpu, id, &io);
#if BOAROS_MAX_CPUS > 1
    cpu.logical_id = id;
#endif
    assert(!pthread_once(&domain_once, kernel_wait_domain_init));
    assert(id < 16); actor_id = id;
    actor = (struct mm_test_actor){0};
#if BOAROS_MAX_CPUS > 1
    bound_mm = 0;
    bound_files = 0;
#endif
    actor.task.wait.on_cpu = 1;
    kernel_wait_node_init(&actor.task.default_wait_node, &actor.task);
    sync_test_irq = 1;
}
/* 普通MM调用使用bootstrap I/O owner；实际park运行在TLS调度模型中。 */
struct kernel_task *kernel_task_current(void)
{
#if BOAROS_MAX_CPUS > 1
    if (bound_mm) return &actor.task;
#endif
    return 0;
}
struct kernel_wait_record *kernel_wait_record_of(struct kernel_task *task) { return &task->wait; }
struct kernel_wait_node *kernel_wait_default_node(struct kernel_task *task) { return &task->default_wait_node; }
struct kernel_task *kernel_wait_current_task(void) { return &actor.task; }
int kernel_wait_backend_initialized(void) { return 1; }
int kernel_wait_backend_signal(struct kernel_task *task) { (void)task; return 0; }
uint64_t kernel_wait_backend_time(void) { return 1; }
void kernel_wait_backend_commit(struct kernel_task *task, uint64_t deadline, int interruptible)
{
    (void)deadline; (void)interruptible; task->state = KERNEL_THREAD_STATE_BLOCKED;
    __atomic_fetch_add(&parked[actor_id], 1, __ATOMIC_RELEASE);
}
void kernel_wait_backend_notify(struct kernel_task *task, uint32_t reason)
{ task->wake_reason = reason; task->state = KERNEL_THREAD_STATE_READY; }
void kernel_wait_backend_ready(struct kernel_task *task)
{
    struct mm_test_actor *owner = (struct mm_test_actor *)task;
    __atomic_store_n(&owner->dispatched, 1, __ATOMIC_RELEASE);
}
enum kernel_scheduler_status kernel_wait_backend_switch(struct kernel_task *task)
{
    { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain); kernel_wait_switch_finish_locked(task); }
    while (!__atomic_load_n(&actor.dispatched, __ATOMIC_ACQUIRE)) sched_yield();
    { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain); task->wait.on_cpu = 1; actor.dispatched = 0; }
    return KERNEL_SCHEDULER_STATUS_OK;
}
void kernel_wait_backend_quiesce(void) { sched_yield(); }
