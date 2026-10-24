/* Production identity lookup/refcounts; the fixture supplies caller CPU/heap. */
#include "../../kernel/sched/private.h"
#include <kernel/errno.h>
#include <kernel/socket_task.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

struct kernel_scheduler scheduler;
struct kernel_raw_lock kernel_wait_domain;
_Thread_local uintptr_t sync_test_irq;
static struct kernel_cpu cpus[2];
static _Thread_local struct kernel_cpu *active;
static unsigned allocated;
void *sync_test_cpu(void) { return active; }
void *sync_test_current_task(void) { return active->current; }
struct kernel_task *kernel_task_current(void) { return active->current; }
enum kernel_heap_status kernel_heap_allocate_zeroed(struct kernel_heap *heap,
    size_t count, size_t size, void **owner)
{
    (void)heap; assert(!active->raw_locks);
    *owner = calloc(count, size);
    if (!*owner) return KERNEL_HEAP_STATUS_EMPTY;
    allocated++; return KERNEL_HEAP_STATUS_OK;
}
void kernel_heap_release(struct kernel_heap *heap, void *owner)
{ (void)heap; assert(!active->raw_locks && allocated); allocated--; free(owner); }

static void publish(struct kernel_task *task, struct kernel_task *parent, int thread)
{
    task->group_leader = thread ? parent : task;
    assert(!process_identity_create(task, parent, thread));
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    process_identity_commit_locked(task);
}

int main(void)
{
    active = &cpus[0];
    kernel_cpu_initialize(active, 0, 0); kernel_cpu_initialize(&cpus[1], 1, 0);
    kernel_raw_lock_init(&kernel_wait_domain, KERNEL_RAW_RANK_SCHEDULER);
    uint64_t bitmap[2];
    assert(!kernel_pid_allocator_init(&scheduler.pid_allocator, bitmap, 128));
    scheduler.identities = (struct kernel_pid_registry){
        .numbers = &scheduler.pid_allocator, .next_generation = 1};
    struct kernel_task leader = {0}, thread = {0}, remote = {0};
    publish(&leader, NULL, 0); publish(&thread, &leader, 1); publish(&remote, NULL, 0);
    cpus[0].current = &thread; cpus[1].current = &remote;
    struct kernel_pid *group = leader.identities[KERNEL_PID_TGID].identity;
    struct kernel_pid *tid = thread.identities[KERNEL_PID_TID].identity;
    unsigned group_refs = group->references, tid_refs = tid->references;
    struct kernel_pid *owner = NULL;
    assert(!kernel_task_socket_identity(0, &owner) && owner == group &&
        group->references == group_refs + 1);
    kernel_task_identity_put(owner); owner = NULL;
    assert(!kernel_task_socket_identity(thread.tid, &owner) && owner == tid &&
        tid->references == tid_refs + 1);
    kernel_task_identity_put(owner); owner = NULL;
    assert(group->references == group_refs && tid->references == tid_refs);
    active = &cpus[1];
    assert(!kernel_task_socket_identity(0, &owner) &&
        owner == remote.identities[KERNEL_PID_TGID].identity);
    kernel_task_identity_put(owner); owner = NULL;
    active = &cpus[0];
    assert(kernel_task_socket_identity(-1, &owner) == -KERNEL_ESRCH && !owner);
    assert(kernel_task_socket_identity(127, &owner) == -KERNEL_ESRCH && !owner);
    struct kernel_task private = {0}; private.group_leader = &private;
    assert(!process_identity_create(&private, NULL, 0));
    assert(kernel_task_socket_identity(private.tid, &owner) == -KERNEL_ESRCH && !owner);
    process_identity_release(&private); process_identity_collect();

    kernel_pid_t number = thread.tid;
    assert(!kernel_task_socket_identity(number, &owner));
    uint64_t generation = owner->generation;
    process_identity_release(&thread);
    struct kernel_pid *after_exit = NULL;
    assert(kernel_task_socket_identity(number, &after_exit) == -KERNEL_ESRCH && !after_exit);
    assert(owner->generation == generation && !owner->members[KERNEL_PID_TID] && owner->published == 2);
    struct kernel_task replacement = {0};
    scheduler.pid_allocator.cursor = (uint32_t)number - 1U;
    publish(&replacement, NULL, 0);
    assert(replacement.tid == number);
    assert(!kernel_task_socket_identity(number, &after_exit) && after_exit != owner &&
        after_exit->generation != generation);
    kernel_task_identity_put(owner); owner = NULL;
    assert(after_exit->members[KERNEL_PID_TID]->task == &replacement);
    kernel_task_identity_put(after_exit); after_exit = NULL;
    process_identity_release(&replacement);
    cpus[0].current = NULL;
    assert(kernel_task_socket_identity(0, &owner) == -KERNEL_ESRCH && !owner);
    process_identity_release(&leader); process_identity_release(&remote);
    process_identity_collect();
    assert(!allocated && !scheduler.pid_allocator.allocated);
    puts("PASS socket identity: caller CPU/TGID, explicit TID, private/missing PID, retained exit owner and final refs");
}
