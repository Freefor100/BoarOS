#include <kernel/pid.h>
#include <assert.h>
#include <stdio.h>

/* Releasing a task must not recycle numbers owned by another role or a pin. */
int main(void)
{
    uint64_t bitmap[1];
    struct kernel_pid_allocator numbers = {0};
    struct kernel_pid_registry registry = {.numbers = &numbers, .next_generation = 1};
    struct kernel_pid first = {0}, second = {0};
    struct kernel_pid_member tid = {0}, tgid = {0}, group = {0}, adopted = {0};
    struct kernel_pid_member session = {0}, peer = {0};
    int leader, survivor;
    assert(kernel_pid_allocator_init(&numbers, bitmap, 1) == KERNEL_PID_STATUS_OK);
    assert(kernel_pid_publish(&registry, &first) == KERNEL_PID_STATUS_OK);
    uint64_t generation = first.generation;
    kernel_pid_attach(&tid, &first, KERNEL_PID_TID, &leader);
    kernel_pid_attach(&group, &first, KERNEL_PID_PGID, &survivor);
    kernel_pid_attach(&tgid, &first, KERNEL_PID_TGID, &leader);
    kernel_pid_attach(&session, &first, KERNEL_PID_SID, &survivor);
    kernel_pid_attach(&peer, &first, KERNEL_PID_PGID, &leader);
    kernel_pid_detach(&tid);
    assert(!first.members[KERNEL_PID_TID]);
    assert(kernel_pid_find(&registry, 1) == &first);
    assert(kernel_pid_publish(&registry, &second) == KERNEL_PID_STATUS_EXHAUSTED);
    assert(!second.registry && numbers.allocated == 1);
    kernel_pid_detach(&peer);
    kernel_pid_transfer(&group, &adopted, &survivor);
    assert(first.members[KERNEL_PID_PGID]->task == &survivor);
    kernel_pid_detach(&adopted);
    assert(kernel_pid_publish(&registry, &second) == KERNEL_PID_STATUS_EXHAUSTED);
    kernel_pid_detach(&tgid);
    kernel_pid_detach(&session);
    kernel_pid_put(&first);
    assert(!kernel_pid_find(&registry, 1));
    assert(kernel_pid_take_retired(&registry) == &first);
    assert(kernel_pid_publish(&registry, &second) == KERNEL_PID_STATUS_OK);
    assert(second.number == 1 && second.generation > generation);
    kernel_pid_put(&second);
    assert(numbers.allocated == 0);
    assert(kernel_pid_take_retired(&registry) == &second);
    assert(!kernel_pid_take_retired(&registry));
    registry.next_generation = (UINT64_MAX >> 30U) + 1;
    struct kernel_pid overflow = {0};
    assert(kernel_pid_publish(&registry, &overflow) == KERNEL_PID_STATUS_EXHAUSTED);
    assert(numbers.allocated == 0 && !overflow.registry);
    puts("pid object lifetime PASS");
}
