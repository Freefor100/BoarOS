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
#endif
#ifdef FILE_IO_CASES
static _Thread_local int force_task;
struct kernel_scheduler scheduler={.initialized=KERNEL_SCHEDULER_INITIALIZED};
void *sync_test_current_task(void) { return cpu.current; }
void mm_test_activate_task(void) { force_task=1; cpu.current=&actor.task; }
void mm_test_finish(void)
{
    struct kernel_io_context *current=kernel_io_context_current();
    assert(!actor.task.io_buffer && !current->locks && !current->user_pins && !current->cache_writes && !cpu.raw_locks);
    if (actor.task.io_scratch_data) {
        assert(!physical_page_release(actor.task.io_scratch_allocator,actor.task.io_scratch_physical_address));
        actor.task.io_scratch_data=0;
    }
    bound_mm=0; bound_files=0; force_task=0; cpu.current=0;
}
#endif
#if BOAROS_MAX_CPUS > 1
void mm_test_bind(struct kernel_mm *mm) {
    bound_mm = mm;
#ifdef FILE_IO_CASES
    actor.task.controlled=!!mm; actor.task.bound_mm_context=!!mm;
    actor.task.mm=mm?*mm:(struct kernel_mm){0};
    cpu.current=mm || force_task?&actor.task:0;
#endif
}
void mm_test_bind_files(struct kernel_files *files) {
    bound_files = files;
#ifdef FILE_IO_CASES
    actor.task.files=files?*files:(struct kernel_files){0};
#endif
}
#ifndef FILE_IO_CASES
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
    /* actor_id 区分等待者；CPU 归属由 activity、文件或缓存夹具显式设置。 */
    cpu.logical_id = 0;
#endif
    assert(!pthread_once(&domain_once, kernel_wait_domain_init));
    assert(id < 16); actor_id = id;
    actor = (struct mm_test_actor){0};
#ifdef FILE_IO_CASES
    force_task=0;
    actor.task.magic=KERNEL_THREAD_MAGIC; actor.task.state=KERNEL_THREAD_STATE_RUNNING;
    actor.task.cpu=&cpu;
#endif
#if BOAROS_MAX_CPUS > 1
    bound_mm = 0;
    bound_files = 0;
#endif
    actor.task.wait.on_cpu = 1;
    kernel_wait_node_init(&actor.task.default_wait_node, &actor.task);
    sync_test_irq = 1;
}
/* 普通MM调用使用bootstrap I/O owner；实际park运行在TLS调度模型中。 */
#ifndef FILE_IO_CASES
void *sync_test_current_task(void) { return kernel_task_current(); }
int kernel_task_has_user_context(const struct kernel_task *task)
{ return task && task->arch.user_mode==1; }
struct kernel_task *kernel_task_current(void)
{
#if BOAROS_MAX_CPUS > 1
    if (bound_mm) return &actor.task;
#endif
    return 0;
}
#endif
#if BOAROS_USER_SMP && !defined(FILE_IO_CASES)
/* 这个MM模型只提供匿名/文件访问；真正需要迁移到CPU0的SysV业务另测。
 * 未进入的范围没有归还动作，意外请求迁移则直接失败。 */
void mm_test_unavailable(void);
int kernel_scheduler_user_service_active(void)
{
    struct kernel_task *task=kernel_task_current();
    return !cpu.logical_id && task && task->user_service;
}
void kernel_service_scope_begin(struct kernel_service_scope *scope)
{
    assert(!scope->entered);
    if (kernel_task_has_user_context(kernel_task_current()) && !kernel_scheduler_user_service_active())
        mm_test_unavailable();
}
void kernel_service_scope_end(struct kernel_service_scope *scope)
{ assert(!scope->entered); }
#endif
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
{ task->wait.reason = reason; task->state = KERNEL_THREAD_STATE_READY; }
void kernel_wait_backend_ready(struct kernel_task *task)
{
    struct mm_test_actor *owner = (struct mm_test_actor *)task;
    __atomic_store_n(&owner->dispatched, 1, __ATOMIC_RELEASE);
}
enum kernel_scheduler_status kernel_wait_backend_switch(struct kernel_task *task)
{
    { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain); kernel_wait_switch_finish_locked(task); }
    while (!__atomic_load_n(&actor.dispatched, __ATOMIC_ACQUIRE)) sched_yield();
    { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain); task->wait.on_cpu = 1;
      task->state=KERNEL_THREAD_STATE_RUNNING; actor.dispatched = 0; }
    return KERNEL_SCHEDULER_STATUS_OK;
}
void kernel_wait_backend_quiesce(void) { sched_yield(); }

#if BOAROS_MAX_CPUS > 1
/* 普通宿主 MM actor 只使用本地翻译；跨 CPU 确认由 activity 夹具接管。 */
__attribute__((weak)) void riscv_smp_sync_user(uint32_t targets, uint64_t address,
                                             uint64_t size, int instructions)
{
    assert(!(targets & ~1U));
    mm_test_fence(size == BOAROS_PAGE_SIZE ? address : UINT64_MAX, 0);
    if (instructions) mm_test_fence(0, 1);
}
#endif
