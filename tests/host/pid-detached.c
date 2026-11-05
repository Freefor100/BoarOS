#include <kernel/pid.h>
#include <assert.h>
#include <stdio.h>
int main(void)
{
    _Static_assert(sizeof(struct kernel_pid) == 64, "PID allocation class");
    uint64_t bitmap[1]; struct kernel_pid_allocator numbers = {0};
    struct kernel_pid_registry registry = {.numbers=&numbers,.next_generation=1};
    struct kernel_pid old={0}, fresh={0}; struct kernel_pid_member tid={0}, group={0};
    int task;
    assert(!kernel_pid_allocator_init(&numbers,bitmap,1));
    assert(!kernel_pid_publish(&registry,&old));
    kernel_pid_attach(&tid,&old,KERNEL_PID_TID,&task);
    kernel_pid_attach(&group,&old,KERNEL_PID_PGID,&task);
    kernel_pid_detach(&tid);
    assert(numbers.allocated==1 && kernel_pid_find(&registry,1)==&old);
    kernel_pid_detach(&group);
    assert(!numbers.allocated && !kernel_pid_find(&registry,1));
    assert(old.references==1 && old.published==2);
    kernel_pid_get(&old); kernel_pid_put(&old);
    assert(!kernel_pid_publish(&registry,&fresh));
    assert(fresh.number==old.number && fresh.generation!=old.generation);
    kernel_pid_put(&old);
    assert(kernel_pid_find(&registry,1)==&fresh && numbers.allocated==1);
    assert(kernel_pid_take_retired(&registry)==&old);
    kernel_pid_put(&fresh); assert(kernel_pid_take_retired(&registry)==&fresh);
    assert(!numbers.allocated);
    puts("PASS detached PID keeps storage, releases number after final role, and cannot alias reused identity");
}
