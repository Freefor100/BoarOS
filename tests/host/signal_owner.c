/* The static production selector is linked in this translation unit; the
 * scheduler domain, group cursor and storage borrow remain real implementations. */
/* Stop/fatal scheduling belongs to the native signal cases. This model must
 * fail if its pending-selection cases enter one of those transitions. */
#define process_group_request_exit signal_owner_unexpected_group_exit
#define kernel_user_thread_exit signal_owner_unexpected_exit
#define process_group_is_orphaned signal_owner_unexpected_orphan
#define kernel_scheduler_wake_signal signal_owner_unexpected_wake
#define scheduler_switch_current_away signal_owner_unexpected_switch
#define stopped_append_locked signal_owner_unexpected_stop
#ifndef SIGNAL_SOURCE
#define SIGNAL_SOURCE "../../kernel/sched/signal.c"
#endif
#include SIGNAL_SOURCE
#undef process_group_request_exit
#undef kernel_user_thread_exit
#undef process_group_is_orphaned
#undef kernel_scheduler_wake_signal
#undef scheduler_switch_current_away
#undef stopped_append_locked
#include <stdlib.h>
void signal_owner_unexpected_group_exit(struct kernel_task *task,enum kernel_thread_exit_reason reason,uint64_t status,uint64_t detail)
{ (void)task;(void)reason;(void)status;(void)detail;abort(); }
void signal_owner_unexpected_exit(enum kernel_thread_exit_reason reason,uint64_t status,uint64_t detail)
{ (void)reason;(void)status;(void)detail;abort(); }
int signal_owner_unexpected_orphan(const struct kernel_task *task) { (void)task;abort(); }
enum kernel_scheduler_status signal_owner_unexpected_wake(struct kernel_task *task) { (void)task;abort(); }
enum kernel_scheduler_status signal_owner_unexpected_switch(struct kernel_task *task) { (void)task;abort(); }
void signal_owner_unexpected_stop(struct kernel_task *task) { (void)task;abort(); }
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
    struct kernel_signal_table table = {.magic=KERNEL_SIGNAL_TABLE_MAGIC,.references=1};
    leader.signal_table_address=(uintptr_t)&table;
    leader.signal_blocked = signal_mask(17);
    assert(signal_wants_sigchld_stop(&leader));
    table.actions[16].flags=LINUX_SA_NOCLDSTOP;
    assert(!signal_wants_sigchld_stop(&leader));
    table.actions[16].flags=0;table.actions[16].handler=KERNEL_SIGNAL_IGN;
    assert(!signal_wants_sigchld_stop(&leader));
    table.actions[16].handler=KERNEL_SIGNAL_DFL;
    struct kernel_task *previous=kernel_cpu_current()->current;
    kernel_cpu_current()->current=&leader;
    struct kernel_signal_delivery delivery;
    *lock_calls=0;
    assert(kernel_signal_select(&leader,&delivery)==KERNEL_SIGNAL_SELECT_NONE);
    assert(*lock_calls==0);
    table.actions[16].handler=0x1000;
    leader.signal_blocked=0;leader.group_pending=signal_mask(17);
    leader.group_sender[16]=41;leader.group_signal_code[16]=1;
    leader.group_child_info=(struct kernel_signal_child_info){.uid=22,.status=37};
    *lock_calls=0;
    assert(kernel_signal_select(&leader,&delivery)==KERNEL_SIGNAL_SELECT_HANDLER);
    assert(*lock_calls==1 && !leader.group_pending);
    assert(delivery.signal==17 && delivery.sender==41 && delivery.code==1 && delivery.uid==22 && delivery.status==37);
    table.actions[16].handler=KERNEL_SIGNAL_DFL;
    leader.signal_blocked=leader.signal_wait_mask=leader.group_pending=signal_mask(17);
    leader.group_sender[16]=42;leader.group_signal_code[16]=5;
    leader.group_child_info=(struct kernel_signal_child_info){.uid=23,.status=19};
    *lock_calls=0;
    assert(kernel_signal_select(&leader,&delivery)==KERNEL_SIGNAL_SELECT_NONE);
    assert(*lock_calls==0 && leader.group_pending==signal_mask(17));
    struct kernel_signal_wait_info info;
    assert(!kernel_signal_wait_take(&leader,&info));
    assert(info.signal==17 && info.sender==42 && info.code==5 && info.uid==23 && info.status==19);
    assert(!leader.group_pending);
    unsigned char encoded[128];kernel_signal_encode_info(encoded,&info,0,0);
    int32_t value;memcpy(&value,encoded+24,4);assert(value==19);
    memcpy(&value,encoded+20,4);assert(value==23);
    memcpy(&value,encoded+8,4);assert(value==5);
    info.code=0;kernel_signal_encode_info(encoded,&info,0,0);
    memcpy(&value,encoded+24,4);assert(!value);
    kernel_signal_encode_info(encoded,&info,1,UINT64_C(0x123456789));
    uint64_t address;memcpy(&address,encoded+16,8);assert(address==UINT64_C(0x123456789));
    assert(!kernel_signal_wait_take(&leader,&info) && !info.signal);
    kernel_cpu_current()->current=previous;
    puts("SIGCHLD: blocked default stop policy, protected pending payload and common ABI encoding passed");
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
