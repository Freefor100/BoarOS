/* The static production selector is linked in this translation unit; the
 * scheduler domain, group cursor and storage borrow remain real implementations. */
#include "../../kernel/sched/signal.c"
#include <assert.h>
#include <stdio.h>
void signal_owner_cases(unsigned *lock_calls)
{
    struct kernel_task leader = {0}, member = {0};
    leader.magic = member.magic = KERNEL_THREAD_MAGIC;
    leader.arch.user_mode = member.arch.user_mode = 1;
    leader.state = member.state = KERNEL_THREAD_STATE_READY;
    leader.group_leader = member.group_leader = &leader;
    leader.group_members = 2;
    leader.group_next = leader.group_previous = &member;
    member.group_next = member.group_previous = &leader;
    struct kernel_task *extra = 0;
    /* The caller's real target owner already covers a self-representing
     * leader; a distinct group container still needs a cross-unlock borrow. */
    assert(signal_send_leader(&leader, &extra) == &leader && !extra);
    assert(!leader.storage_borrows);
    assert(signal_send_leader(&member, &extra) == &leader && extra == &leader);
    assert(leader.storage_borrows == 1);
    scheduler_task_put(extra);
    assert(!leader.storage_borrows);
    *lock_calls = 0;
    struct kernel_task *result = signal_group_representative_get(&member);
    assert(result == &leader && leader.storage_borrows == 1);
    /* An eligible protected leader needs no separately owned traversal. */
    assert(*lock_calls <= 1);
    scheduler_task_put(result);
    assert(!leader.storage_borrows && !member.storage_borrows);
    leader.state = KERNEL_THREAD_STATE_GROUP_DEAD;
    result = signal_group_representative_get(&member);
    assert(result == &member && member.storage_borrows == 1 && !leader.storage_borrows);
    scheduler_task_put(result);
    member.state = KERNEL_THREAD_STATE_EXITED;
    assert(!signal_group_representative_get(&member));
    assert(!leader.storage_borrows && !member.storage_borrows);
    puts("signal target selector: direct leader, dead leader fallback and unique borrow passed");
}
