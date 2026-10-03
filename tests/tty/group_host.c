#include "../../kernel/sched/private.h"
#include <assert.h>
#include <kernel/errno.h>
#include <kernel/tty_task.h>
#include <stdio.h>

struct kernel_scheduler scheduler;
struct kernel_pid *process_identity(const struct kernel_task *task, enum kernel_pid_role role)
{
    return task->identities[role].identity;
}

int main(void)
{
    struct kernel_pid_allocator numbers = {0};
    uint64_t bitmap[KERNEL_PID_BITMAP_WORDS(32)];
    assert(kernel_pid_allocator_init(&numbers, bitmap, 32) == KERNEL_PID_STATUS_OK);
    scheduler.identities.numbers = &numbers;
    scheduler.identities.next_generation = 1;
    struct kernel_pid sid = {0}, foreign_sid = {0}, group = {0};
    assert(kernel_pid_publish(&scheduler.identities, &sid) == KERNEL_PID_STATUS_OK);
    assert(kernel_pid_publish(&scheduler.identities, &foreign_sid) == KERNEL_PID_STATUS_OK);
    assert(kernel_pid_publish(&scheduler.identities, &group) == KERNEL_PID_STATUS_OK);
    struct kernel_task caller = {0}, same = {0}, foreign = {0};
    kernel_pid_attach(&caller.identities[KERNEL_PID_SID], &sid, KERNEL_PID_SID, &caller);
    kernel_pid_attach(&same.identities[KERNEL_PID_SID], &sid, KERNEL_PID_SID, &same);
    kernel_pid_attach(&foreign.identities[KERNEL_PID_SID], &foreign_sid, KERNEL_PID_SID, &foreign);
    struct kernel_pid *found = &group;
    assert(kernel_task_tty_find_group(&caller, INT32_MAX, &found) == -KERNEL_ESRCH && !found);
    assert(kernel_task_tty_find_group(&caller, 0, &found) == -KERNEL_ESRCH && !found);
    assert(kernel_task_tty_find_group(&caller, group.number, &found) == -KERNEL_EPERM && !found);
    kernel_pid_attach(&same.identities[KERNEL_PID_PGID], &group, KERNEL_PID_PGID, &same);
    uint32_t references = group.references;
    assert(kernel_task_tty_find_group(&caller, group.number, &found) == 0 && found == &group);
    assert(group.references == references);
    kernel_pid_detach(&same.identities[KERNEL_PID_PGID]);
    kernel_pid_attach(&foreign.identities[KERNEL_PID_PGID], &group, KERNEL_PID_PGID, &foreign);
    kernel_pid_attach(&same.identities[KERNEL_PID_TID], &group, KERNEL_PID_TID, &same);
    assert(kernel_task_tty_find_group(&caller, group.number, &found) == -KERNEL_EPERM && !found);
    kernel_pid_detach(&foreign.identities[KERNEL_PID_PGID]);
    assert(kernel_task_tty_find_group(&caller, group.number, &found) == 0 && found == &group);
    kernel_pid_detach(&same.identities[KERNEL_PID_TID]);
    kernel_pid_attach(&foreign.identities[KERNEL_PID_TID], &group, KERNEL_PID_TID, &foreign);
    assert(kernel_task_tty_find_group(&caller, group.number, &found) == -KERNEL_EPERM && !found);
    kernel_pid_detach(&foreign.identities[KERNEL_PID_TID]);
    kernel_pid_detach(&caller.identities[KERNEL_PID_SID]);
    kernel_pid_detach(&same.identities[KERNEL_PID_SID]);
    kernel_pid_detach(&foreign.identities[KERNEL_PID_SID]);
    assert(group.references == 1 && sid.references == 1 && foreign_sid.references == 1);
    kernel_pid_put(&group);
    kernel_pid_put(&sid);
    kernel_pid_put(&foreign_sid);
    assert(!numbers.allocated);
    puts("TTY group host: actual identity lookup/status/PID fallback/borrowed refs pass");
}
