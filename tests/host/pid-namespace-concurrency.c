#define _GNU_SOURCE
#include <kernel/pid.h>
#include <kernel/cpu.h>
#include <kernel/sync.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

enum { WORKERS = 4, ROUNDS = 128 };
_Thread_local uintptr_t sync_test_irq;
static _Thread_local struct kernel_cpu *active;
static struct kernel_cpu cpus[WORKERS];
void *sync_test_cpu(void) { return active; }
void *sync_test_current_task(void) { return active->current; }
static struct kernel_raw_lock identity_lock;
static pthread_barrier_t phase;
static struct kernel_pid_registry root, leaf;
static struct kernel_pid_allocator root_numbers, leaf_numbers;
static uint64_t root_bitmap[1], leaf_bitmap[1];
static struct kernel_pid *published, *replacement;
static unsigned winners, conflicts, collected;
struct stored_identity { struct kernel_pid id; struct kernel_pid_binding ancestor; };
static void barrier(void)
{
    int result = pthread_barrier_wait(&phase);
    assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
}
static void *worker(void *argument)
{
    unsigned index = (unsigned)(uintptr_t)argument;
    active = &cpus[index];
    for (unsigned round = 0; round < ROUNDS; round++) {
        struct stored_identity candidate = {0}, reused = {0};
        struct kernel_pid_member roles[KERNEL_PID_ROLES] = {0}, new_role = {0};
        kernel_pid_t requested[] = {7, 11};
        int won;
        barrier();
        {
            KERNEL_RAW_SCOPE(guard, &identity_lock);
            enum kernel_pid_status status = kernel_pid_reserve_numbers(&leaf,
                &candidate.id, sizeof(candidate), requested, 2);
            assert(status == KERNEL_PID_STATUS_OK || status == KERNEL_PID_STATUS_EXISTS);
            won = status == KERNEL_PID_STATUS_OK;
            if (won) winners++; else conflicts++;
            assert(!kernel_pid_find(&root, 11) && !kernel_pid_find(&leaf, 7));
        }
        barrier();
        if (won) {
            KERNEL_RAW_SCOPE(guard, &identity_lock);
            for (unsigned role = 0; role < KERNEL_PID_ROLES; role++)
                kernel_pid_attach(&roles[role], &candidate.id, role, &candidate);
            assert(!kernel_pid_publish_reserved(&candidate.id));
            published = &candidate.id;
            kernel_pid_put(&candidate.id); /* Transfer constructor to roles. */
        }
        barrier();
        struct kernel_pid *borrowed;
        uint64_t generation;
        {
            KERNEL_RAW_SCOPE(guard, &identity_lock);
            borrowed = kernel_pid_find(&leaf, 7);
            assert(borrowed && borrowed == published && kernel_pid_find(&root, 11) == borrowed);
            for (unsigned role = 0; role < KERNEL_PID_ROLES; role++) assert(borrowed->members[role]);
            kernel_pid_get(borrowed); generation = borrowed->generation;
        }
        barrier();
        if (won) {
            KERNEL_RAW_SCOPE(guard, &identity_lock);
            /* Exec-style role transfer must keep every view bound to the same
             * object, rather than constructing a namespace-local duplicate. */
            struct kernel_pid_member moved = {0};
            kernel_pid_transfer(&roles[KERNEL_PID_TID], &moved, &reused);
            assert(kernel_pid_find(&root, 11)->members[KERNEL_PID_TID] == &moved);
            kernel_pid_detach(&moved);
            for (unsigned role = 1; role < KERNEL_PID_ROLES; role++) kernel_pid_detach(&roles[role]);
            assert(!kernel_pid_find(&root, 11) && !kernel_pid_find(&leaf, 7));
            assert(borrowed->published == 2 && borrowed->references == WORKERS);
            assert(!kernel_pid_reserve_numbers(&leaf, &reused.id, sizeof(reused), requested, 2));
            kernel_pid_attach(&new_role, &reused.id, KERNEL_PID_TID, &reused);
            assert(!kernel_pid_publish_reserved(&reused.id));
            replacement = &reused.id;
            kernel_pid_put(&reused.id);
        }
        barrier();
        {
            KERNEL_RAW_SCOPE(guard, &identity_lock);
            assert(borrowed->published == 2 && borrowed->generation == generation);
            assert(kernel_pid_find(&leaf, 7) == replacement && replacement != borrowed);
            assert(replacement->generation > generation);
            kernel_pid_put(borrowed);
        }
        barrier();
        if (won) {
            KERNEL_RAW_SCOPE(guard, &identity_lock);
            kernel_pid_detach(&new_role);
            struct kernel_pid *retired;
            while ((retired = kernel_pid_take_retired(&root))) {
                assert(retired == &candidate.id || retired == &reused.id);
                collected++;
            }
            assert(!root_numbers.allocated && !leaf_numbers.allocated);
            assert(leaf.references == 1 && root.references == 2);
            published = replacement = NULL;
        }
        barrier(); /* No stack owner is reused before retirement is drained. */
    }
    return NULL;
}
int main(void)
{
    assert(sizeof(struct kernel_pid) == 64);
    assert(!kernel_pid_allocator_init(&root_numbers, root_bitmap, 64));
    assert(!kernel_pid_allocator_init(&leaf_numbers, leaf_bitmap, 64));
    assert(!kernel_pid_registry_init(&root, &root_numbers, NULL));
    assert(!kernel_pid_registry_init(&leaf, &leaf_numbers, &root));
    kernel_raw_lock_init(&identity_lock, KERNEL_RAW_RANK_SCHEDULER);
    assert(!pthread_barrier_init(&phase, NULL, WORKERS));
    pthread_t threads[WORKERS];
    for (unsigned i = 0; i < WORKERS; i++) {
        kernel_cpu_initialize(&cpus[i], i, NULL);
        assert(!pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)i));
    }
    for (unsigned i = 0; i < WORKERS; i++) assert(!pthread_join(threads[i], NULL));
    assert(winners == ROUNDS && conflicts == ROUNDS * (WORKERS - 1) && collected == ROUNDS * 2);
    kernel_pid_registry_put(&leaf);
    assert(kernel_pid_take_retired_namespace(&root) == &leaf);
    assert(!kernel_pid_take_retired_namespace(&root));
    kernel_pid_registry_put(&root);
    assert(!root.references);
    assert(!pthread_barrier_destroy(&phase));
    puts("PID namespace canonical roles/concurrent reserve/publish/retire PASS");
}
