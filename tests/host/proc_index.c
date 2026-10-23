/* Production proc queries and PID identities; scheduler fixtures own storage. */
#include "../../kernel/sched/proc.c"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

struct kernel_scheduler scheduler;
struct kernel_raw_lock kernel_wait_domain;
_Thread_local uintptr_t sync_test_irq;
static struct kernel_cpu cpus[2];
static _Thread_local struct kernel_cpu *active;
static struct kernel_task tasks[6];
static struct kernel_pid ids[7];
static uint64_t bitmap[2];
static unsigned all_steps, ancestry_steps;
static int invalidate_ancestry, reset_actions;
static pthread_barrier_t action_barrier;
static struct kernel_signal_table actions;
void *sync_test_cpu(void) { return active; }
void *sync_test_current_task(void) { return active->current; }
void scheduler_task_get_locked(struct kernel_task *task)
{ assert(active->raw_locks && task->accounted); task->storage_borrows++; }
void scheduler_task_put(struct kernel_task *task)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    assert(task->storage_borrows); task->storage_borrows--;
}
void scheduler_task_cursor_begin(struct scheduler_task_cursor *cursor)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    cursor->task = scheduler.all_tasks;
    if (cursor->task) scheduler_task_get_locked(cursor->task);
}
void scheduler_task_cursor_next(struct scheduler_task_cursor *cursor)
{
    struct kernel_task *old = cursor->task;
    all_steps++;
    { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
      cursor->task = old->all_next;
      if (cursor->task) scheduler_task_get_locked(cursor->task); }
    scheduler_task_put(old);
}
void scheduler_task_cursor_end(struct scheduler_task_cursor *cursor)
{ if (cursor->task) scheduler_task_put(cursor->task); cursor->task = 0; }
void scheduler_group_cursor_begin(struct kernel_task *task, struct scheduler_group_cursor *cursor)
{
    *cursor = (struct scheduler_group_cursor){0};
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    cursor->leader = cursor->task = task->group_leader;
    scheduler_task_get_locked(cursor->leader);
}
void scheduler_group_cursor_next(struct scheduler_group_cursor *cursor)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    cursor->task = cursor->task->group_next;
    if (cursor->task == cursor->leader) cursor->task = 0;
}
void scheduler_group_cursor_end(struct scheduler_group_cursor *cursor)
{ scheduler_task_put(cursor->leader); *cursor = (struct scheduler_group_cursor){0}; }
int scheduler_task_descends_from(struct kernel_task *task, const struct kernel_task *root, uint64_t identity)
{
    (void)identity;
    assert(!active->raw_locks && task->storage_borrows && root->storage_borrows);
    ancestry_steps++;
    int result = 0;
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    for (struct kernel_task *at = task; at; at = at->group_leader->parent)
        if (at == root) { result = 1; break; }
    if (invalidate_ancestry && task == &tasks[1]) {
        invalidate_ancestry = 0;
        kernel_pid_detach(&task->identities[KERNEL_PID_TGID]);
    }
    return result;
}
uint64_t kernel_signal_pending_count_locked(void) { assert(active->raw_locks); return 0; }
struct kernel_tty *kernel_task_controlling_tty(const struct kernel_task *task)
{
    (void)task;
    if (reset_actions) {
        reset_actions = 0;
        /* The remote reset commits after the metadata capture, before formatting. */
        pthread_barrier_wait(&action_barrier);
        pthread_barrier_wait(&action_barrier);
    }
    return 0;
}
uint64_t kernel_tty_rdev(const struct kernel_tty *tty) { (void)tty; return 0; }
int32_t kernel_tty_foreground(const struct kernel_tty *tty) { (void)tty; return -1; }
void scheduler_usage_snapshot(const struct kernel_task *task, int thread, struct kernel_task_usage *usage)
{ (void)task; (void)thread; *usage = (struct kernel_task_usage){0}; }
enum kernel_mm_status kernel_mm_proc_memory_snapshot(const struct kernel_mm *mm, struct kernel_mm_proc_memory *result)
{ (void)mm; (void)result; abort(); }

static void setup(void)
{
    scheduler = (struct kernel_scheduler){0};
    memset(tasks, 0, sizeof(tasks)); memset(ids, 0, sizeof(ids));
    assert(kernel_pid_allocator_init(&scheduler.pid_allocator, bitmap, 128) == KERNEL_PID_STATUS_OK);
    scheduler.identities = (struct kernel_pid_registry){.numbers = &scheduler.pid_allocator, .next_generation = 1};
    for (unsigned i = 0; i < 6; i++) {
        struct kernel_task *task = &tasks[i];
        assert(kernel_pid_publish(&scheduler.identities, &ids[i]) == KERNEL_PID_STATUS_OK);
        task->arch.user_mode = task->accounted = task->tid_owned = 1;
        task->tid = ids[i].number; task->state = KERNEL_THREAD_STATE_READY;
        task->group_leader = task->group_next = task->group_previous = task;
        task->group_members = 1; task->parent = i ? &tasks[0] : 0;
        task->all_next = i < 5 ? &tasks[i + 1] : 0;
        kernel_pid_attach(&task->identities[KERNEL_PID_TID], &ids[i], KERNEL_PID_TID, task);
        kernel_pid_attach(&task->identities[KERNEL_PID_TGID], &ids[i], KERNEL_PID_TGID, task);
    }
    /* Task 3 is a member of task 2; task 4 is outside init's ancestry. */
    kernel_pid_detach(&tasks[2].identities[KERNEL_PID_TGID]);
    tasks[2].group_leader = &tasks[1]; tasks[2].parent = 0;
    tasks[1].group_next = tasks[1].group_previous = &tasks[2];
    tasks[2].group_next = tasks[2].group_previous = &tasks[1];
    tasks[1].group_members = 2;
    tasks[3].parent = 0;
    /* An identity can remain published for a process-group/session owner. */
    kernel_pid_detach(&tasks[4].identities[KERNEL_PID_TID]);
    kernel_pid_detach(&tasks[4].identities[KERNEL_PID_TGID]);
    assert(kernel_pid_reserve(&scheduler.identities, &ids[6]) == KERNEL_PID_STATUS_OK);
    scheduler.init_task = scheduler.all_tasks = &tasks[0];
    all_steps = ancestry_steps = 0;
}
static void borrows_returned(void)
{ for (unsigned i = 0; i < 6; i++) assert(!tasks[i].storage_borrows); }
static void lookup_cases(void)
{
    setup();
    uint64_t token = 0; kernel_pid_t pid = 0;
    assert(!kernel_proc_process_identity(2, &token) && token == ids[1].generation);
    printf("known PID: all-task steps=%u ancestry candidates=%u\n", all_steps, ancestry_steps); fflush(stdout);
    assert(!all_steps && ancestry_steps == 1);
    assert(kernel_proc_process_identity(3, &token) == -KERNEL_ENOENT);
    assert(kernel_proc_process_identity(4, &token) == -KERNEL_ENOENT);
    assert(kernel_proc_process_identity(5, &token) == -KERNEL_ENOENT);
    assert(kernel_proc_process_identity(7, &token) == -KERNEL_ENOENT);
    const kernel_pid_t expected[] = {1, 2, 6};
    for (unsigned i = 0; i < 3; i++) {
        assert(!kernel_proc_next_process(pid, &pid, &token)); assert(pid == expected[i]);
    }
    assert(kernel_proc_next_process(pid, &pid, &token) == -KERNEL_ENOENT);
    assert(!kernel_proc_next_process(1, &pid, &token) && pid == 2);
    assert(!all_steps);
    pid = 0;
    assert(!kernel_proc_next_thread(2, ids[1].generation, pid, &pid, &token) && pid == 2);
    assert(!kernel_proc_next_thread(2, ids[1].generation, pid, &pid, &token) && pid == 3);
    assert(kernel_proc_next_thread(2, ids[1].generation, pid, &pid, &token) == -KERNEL_ENOENT);
    assert(!all_steps);
    tasks[1].state = KERNEL_THREAD_STATE_GROUP_DEAD;
    assert(!kernel_proc_process_identity(2, &token));
    assert(!kernel_proc_next_thread(2, ids[1].generation, 0, &pid, &token) && pid == 2);
    tasks[2].state = KERNEL_THREAD_STATE_EXITED;
    assert(kernel_proc_process_identity(2, &token) == -KERNEL_ENOENT);
    tasks[1].state = KERNEL_THREAD_STATE_EXITED; tasks[1].group_members = 1;
    assert(!kernel_proc_process_identity(2, &token));
    tasks[1].state = KERNEL_THREAD_STATE_READY;
    invalidate_ancestry = 1;
    assert(kernel_proc_process_identity(2, &token) == -KERNEL_ENOENT);
    borrows_returned();
    puts("PASS proc PID/TID lookup, ordered continuation, ancestry, residual/private identity, leader exit, identity recheck and returned borrows");
}
static void *remote_reset(void *unused)
{
    (void)unused; active = &cpus[1];
    pthread_barrier_wait(&action_barrier);
    { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
      memset(actions.actions, 0, sizeof(actions.actions)); }
    pthread_barrier_wait(&action_barrier);
    return 0;
}
static void snapshot_case(void)
{
    setup(); actions = (struct kernel_signal_table){.magic = KERNEL_SIGNAL_TABLE_MAGIC};
    actions.actions[1].handler = KERNEL_SIGNAL_IGN;
    actions.actions[62].handler = 0x1000;
    tasks[1].signal_table_address = (uintptr_t)&actions;
    tasks[1].signal_pending = UINT64_C(1) << 63;
    pthread_t remote; assert(!pthread_barrier_init(&action_barrier, 0, 2));
    reset_actions = 1; assert(!pthread_create(&remote, 0, remote_reset, 0));
    struct kernel_proc_process_snapshot result;
    assert(!kernel_proc_process_snapshot(2, ids[1].generation, &result));
    assert(!pthread_join(remote, 0)); assert(!pthread_barrier_destroy(&action_barrier));
    printf("snapshot before remote reset: ignored=%llx caught=%llx\n",
        (unsigned long long)result.signal_ignored, (unsigned long long)result.signal_caught); fflush(stdout);
    assert(result.signal_ignored == 2 && result.signal_caught == (UINT64_C(1) << 62));
    assert(result.signal_pending == (UINT64_C(1) << 63)); borrows_returned();
    puts("PASS proc signal snapshot retains metadata captured before remote action reset");
}
int main(int argc, char **argv)
{
    active = &cpus[0]; kernel_cpu_initialize(active, 0, 0); kernel_cpu_initialize(&cpus[1], 1, 0);
    kernel_raw_lock_init(&kernel_wait_domain, KERNEL_RAW_RANK_SCHEDULER);
    if (argc > 1 && !strcmp(argv[1], "snapshot")) snapshot_case();
    else lookup_cases();
    return 0;
}
