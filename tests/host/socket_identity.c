/* Production identity lookup/refcounts; the fixture supplies caller CPU/heap. */
#include "../../kernel/sched/private.h"
#include <kernel/errno.h>
#include <kernel/socket_task.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void nested_views(struct kernel_task *root)
{
    struct kernel_pid_allocator alloc[2]={0};uint64_t bits[2][KERNEL_PID_BITMAP_WORDS(64)]={0};
    struct kernel_pid_registry spaces[2]={0};struct kernel_task children[2];
    memset(children,0,sizeof(children));
    for(unsigned i=0;i<2;i++) {
        assert(!kernel_pid_allocator_init(&alloc[i],bits[i],64));
        assert(!kernel_pid_registry_init(&spaces[i],&alloc[i],&scheduler.identities));
        {KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);assert(!process_pid_children_install_locked(root,&spaces[i]));}
        publish(&children[i],root,0);
    }
    {KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);assert(!process_pid_children_install_locked(root,&scheduler.identities));}
    cpus[0].current=&children[0];struct kernel_pid *first=NULL,*second=NULL;
    assert(!kernel_task_identity_get_user(1,&first) && first==process_identity(&children[0],KERNEL_PID_TID));
    assert(kernel_task_identity_user_number(first)==1);
    assert(kernel_task_identity_get_user(children[1].tid,&second)==-KERNEL_ESRCH && !second);
    cpus[0].current=&children[1];
    assert(!kernel_task_identity_get_user(1,&second) && second!=first);
    assert(kernel_task_identity_user_number(first)==0 && kernel_task_identity_user_number(second)==1);
    cpus[0].current=root;
    assert(kernel_task_identity_user_number(first)==children[0].tid &&
        kernel_task_identity_user_number(second)==children[1].tid);
    kernel_pid_t old=children[0].tid;
    process_identity_release(&children[0]);process_identity_collect();
    assert(kernel_task_identity_user_number(first)==old && !first->members[KERNEL_PID_TID]);
    kernel_task_identity_put(first);kernel_task_identity_put(second);
    process_identity_release(&children[1]);process_identity_collect();
    {KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
     for(unsigned i=0;i<2;i++){assert(!alloc[i].allocated && spaces[i].references==1);kernel_pid_registry_put(&spaces[i]);}
     unsigned retired=0;while(kernel_pid_take_retired_namespace(&scheduler.identities))retired++;
     assert(retired==2 && !kernel_pid_take_retired(&scheduler.identities));}
    puts("PASS socket identity nested caller views, sibling same PID and retained exited sender mapping");
}

int main(void)
{
    active = &cpus[0];
    kernel_cpu_initialize(active, 0, 0); kernel_cpu_initialize(&cpus[1], 1, 0);
    kernel_raw_lock_init(&kernel_wait_domain, KERNEL_RAW_RANK_SCHEDULER);
    uint64_t bitmap[2];
    assert(!kernel_pid_allocator_init(&scheduler.pid_allocator, bitmap, 128));
    assert(!kernel_pid_registry_init(&scheduler.identities, &scheduler.pid_allocator, NULL));
    struct kernel_task leader = {0}, thread = {0}, remote = {0};
    publish(&leader, NULL, 0); publish(&thread, &leader, 1); publish(&remote, NULL, 0);
    cpus[0].current = &thread; cpus[1].current = &remote;
    struct kernel_pid *group = leader.identities[KERNEL_PID_TGID].identity;
    struct kernel_pid *tid = thread.identities[KERNEL_PID_TID].identity;
    unsigned group_refs = group->references, tid_refs = tid->references;
    struct kernel_pid *owner = NULL;
    assert(!kernel_task_identity_get(0, &owner) && owner == group &&
        group->references == group_refs + 1);
    kernel_task_identity_put(owner); owner = NULL;
    assert(!kernel_task_identity_get(thread.tid, &owner) && owner == tid &&
        tid->references == tid_refs + 1);
    kernel_task_identity_put(owner); owner = NULL;
    assert(group->references == group_refs && tid->references == tid_refs);
    active = &cpus[1];
    assert(!kernel_task_identity_get(0, &owner) &&
        owner == remote.identities[KERNEL_PID_TGID].identity);
    kernel_task_identity_put(owner); owner = NULL;
    active = &cpus[0];
    assert(kernel_task_identity_get(-1, &owner) == -KERNEL_ESRCH && !owner);
    assert(kernel_task_identity_get(127, &owner) == -KERNEL_ESRCH && !owner);
    struct kernel_task private = {0}; private.group_leader = &private;
    assert(!process_identity_create(&private, NULL, 0));
    assert(kernel_task_identity_get(private.tid, &owner) == -KERNEL_ESRCH && !owner);
    process_identity_release(&private); process_identity_collect();

    kernel_pid_t number = thread.tid;
    assert(!kernel_task_identity_get(number, &owner));
    uint64_t generation = owner->generation;
    process_identity_release(&thread);
    struct kernel_pid *after_exit = NULL;
    assert(kernel_task_identity_get(number, &after_exit) == -KERNEL_ESRCH && !after_exit);
    assert(owner->generation == generation && !owner->members[KERNEL_PID_TID] && owner->published == 2);
    struct kernel_task replacement = {0};
    scheduler.pid_allocator.cursor = (uint32_t)number - 1U;
    publish(&replacement, NULL, 0);
    assert(replacement.tid == number);
    assert(!kernel_task_identity_get(number, &after_exit) && after_exit != owner &&
        after_exit->generation != generation);
    kernel_task_identity_put(owner); owner = NULL;
    assert(after_exit->members[KERNEL_PID_TID]->task == &replacement);
    kernel_task_identity_put(after_exit); after_exit = NULL;
    process_identity_release(&replacement);
    nested_views(&leader);
    cpus[0].current = NULL;
    assert(kernel_task_identity_get(0, &owner) == -KERNEL_ESRCH && !owner);
    process_identity_release(&leader); process_identity_release(&remote);
    process_identity_collect();
    assert(!allocated && !scheduler.pid_allocator.allocated);
    assert(scheduler.identities.references == 1);
    puts("PASS socket identity: caller CPU/TGID, explicit TID, private/missing PID, retained exit owner and final refs");
}
