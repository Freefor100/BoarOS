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
#define physical_page_allocate signal_owner_unexpected_allocate
#define physical_page_resolve signal_owner_unexpected_resolve
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
#undef physical_page_allocate
#undef physical_page_resolve
#include <stdlib.h>
void signal_owner_unexpected_group_exit(struct kernel_task *task,enum kernel_thread_exit_reason reason,uint64_t status,uint64_t detail)
{ (void)task;(void)reason;(void)status;(void)detail;abort(); }
void signal_owner_unexpected_exit(enum kernel_thread_exit_reason reason,uint64_t status,uint64_t detail)
{ (void)reason;(void)status;(void)detail;abort(); }
int signal_owner_unexpected_orphan(const struct kernel_task *task) { (void)task;abort(); }
enum kernel_scheduler_status signal_owner_unexpected_wake(struct kernel_task *task) { (void)task;abort(); }
enum kernel_scheduler_status signal_owner_unexpected_switch(struct kernel_task *task) { (void)task;abort(); }
void signal_owner_unexpected_stop(struct kernel_task *task) { (void)task;abort(); }
enum physical_page_status signal_owner_unexpected_allocate(struct physical_page_allocator *allocator,uint64_t *address)
{ (void)allocator;(void)address;abort(); }
enum physical_page_status signal_owner_unexpected_resolve(const struct physical_page_allocator *allocator,uint64_t address,void **page)
{ (void)allocator;(void)address;(void)page;abort(); }
#include <assert.h>
#include <pthread.h>
#include <stdio.h>

extern void signal_owner_bind_cpu(struct kernel_cpu *);
static unsigned action_phase;
static _Thread_local unsigned pause_selection;
static struct kernel_task *action_target;
static struct kernel_cpu action_cpu;
static struct signal_queue_record native_record;
static struct kernel_signal_wait_info *native_output;
static unsigned native_stale, native_completions, queue_allocations;
static struct kernel_heap queue_heap;

/* Exercise the selector's output lifetime. Timer reservation/generation and
 * object ownership use the real implementation in test-posix-timer-host. */
int posix_timer_record_take_locked(struct signal_queue_record *record,
    struct kernel_signal_wait_info *info,struct signal_pending_prepared *p)
{
    assert(record==&native_record && kernel_cpu_current()->raw_locks && !native_output);
    if(native_stale){info->signal=0;return 0;}
    native_output=info;record->next=p->completed;p->completed=record;return 1;
}
void posix_timer_record_complete(struct signal_queue_record *record)
{
    assert(record==&native_record && native_output && !kernel_cpu_current()->raw_locks);
    native_output->uid=37;native_output=0;native_completions++;
}

void __real_kernel_raw_lock_release(struct kernel_raw_guard *);
void __wrap_kernel_raw_lock_release(struct kernel_raw_guard *guard)
{
    int pause=pause_selection && guard->lock==&kernel_wait_domain;
    if(pause)pause_selection=0;
    __real_kernel_raw_lock_release(guard);
    if(pause){
        __atomic_store_n(&action_phase,1,__ATOMIC_RELEASE);
        while(__atomic_load_n(&action_phase,__ATOMIC_ACQUIRE)!=2){}
    }
}
static void *replace_selected_action(void *unused)
{
    (void)unused;signal_owner_bind_cpu(&action_cpu);
    while(__atomic_load_n(&action_phase,__ATOMIC_ACQUIRE)!=1){}
    const struct kernel_linux_sigaction replacement={.handler=0x2000,.flags=LINUX_SA_NODEFER,.mask=UINT64_C(1)<<14};
    assert(!kernel_signal_set_action(action_target,17,&replacement,0));
    __atomic_store_n(&action_phase,2,__ATOMIC_RELEASE);
    return 0;
}
static void selected_action_snapshot(struct kernel_task *task,struct kernel_signal_table *table)
{
    struct kernel_cpu *current=kernel_cpu_current();
    kernel_cpu_initialize(&action_cpu,19,0);
    action_cpu.logical_id=1;
    task->signal_blocked=0;task->group_pending=UINT64_C(1)<<16;
    table->actions[16]=(struct kernel_signal_action){.handler=0x1000,.flags=LINUX_SA_RESETHAND,.mask=UINT64_C(1)<<11};
    action_target=task;action_phase=0;
    pthread_t setter;assert(!pthread_create(&setter,0,replace_selected_action,0));
    struct kernel_signal_delivery delivery={0};
    pause_selection=1;
    assert(kernel_signal_select(task,&delivery)==KERNEL_SIGNAL_SELECT_HANDLER);
    assert(!pthread_join(setter,0));
    assert(delivery.handler==0x1000 && delivery.flags==LINUX_SA_RESETHAND);
    assert(delivery.restore_mask==0 && task->signal_blocked==((UINT64_C(1)<<11)|(UINT64_C(1)<<16)));
    assert(table->actions[16].handler==0x2000 && table->actions[16].flags==LINUX_SA_NODEFER);
    assert(!task->group_pending && !current->raw_locks && !action_cpu.raw_locks);
    puts("signal selection keeps the committed action/mask snapshot; later replacement survives SA_RESETHAND");
}
static void selected_payloads(struct kernel_task *task,struct kernel_signal_table *table)
{
    struct kernel_signal_delivery delivery;
    struct kernel_signal_wait_info expected;
    table->actions[10]=(struct kernel_signal_action){.handler=0x1000};
    task->signal_blocked=0;
    memset(&delivery,0xa5,sizeof(delivery));
    kernel_signal_force_fault(task,11,2,UINT64_C(0x123456789));
    assert(kernel_signal_select(task,&delivery)==KERNEL_SIGNAL_SELECT_HANDLER);
    memset(&expected,0,sizeof(expected));expected.signal=11;expected.code=2;expected.address=UINT64_C(0x123456789);
    assert(!memcmp(&delivery.info,&expected,sizeof(expected)) && !task->signal_fault.signal);

    struct signal_pending_prepared p={0};
    signal_pending_prepare_index(task,1,&queue_heap,&p);
    assert(p.index && queue_allocations==1);
    native_record=(struct signal_queue_record){.info={.signal=10,.code=-2,.sender=7,.value=UINT64_C(0x1234567887654321)},
        .timer=(struct kernel_posix_timer *)&native_record};
    table->actions[9]=(struct kernel_signal_action){.handler=0x3000};
    task->signal_blocked=0;
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);signal_pending_install_index_locked(task,1,&p);signal_pending_post_timer_locked(task,1,&native_record);}
    signal_pending_finish(&p);
    memset(&delivery,0xa5,sizeof(delivery));
    assert(kernel_signal_select(task,&delivery)==KERNEL_SIGNAL_SELECT_HANDLER);
    expected=native_record.info;expected.uid=37;
    assert(!memcmp(&delivery.info,&expected,sizeof(expected)) && delivery.handler==0x3000);
    assert(native_completions==1 && !native_output && !task->group_pending);

    native_stale=1;task->signal_blocked=0;
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);signal_pending_post_timer_locked(task,1,&native_record);}
    task->group_pending|=signal_mask(17);
    table->actions[16]=(struct kernel_signal_action){.handler=0x4000};
    memset(&delivery,0xa5,sizeof(delivery));
    assert(kernel_signal_select(task,&delivery)==KERNEL_SIGNAL_SELECT_HANDLER);
    memset(&expected,0,sizeof(expected));expected.signal=17;
    expected.sender=task->group_sender[16];expected.code=task->group_signal_code[16];
    expected.uid=task->group_child_info.uid;expected.status=task->group_child_info.status;
    expected.user_time=task->group_child_times[0];expected.system_time=task->group_child_times[1];
    assert(!memcmp(&delivery.info,&expected,sizeof(expected)) && delivery.handler==0x4000);
    assert(native_completions==1 && !native_output && !task->group_pending);
    native_stale=0;
    unsigned members=task->group_members;task->group_members=1;
    kernel_signal_pending_cleanup(task);task->group_members=members;
    assert(!queue_allocations);
    puts("signal selection publishes complete fault/native payloads and skips stale records before the next handler");
}
void signal_owner_cases(unsigned *lock_calls)
{
    struct kernel_task leader = {0}, member = {0};
    struct kernel_resource_limits limits={0};
    leader.limits=member.limits=&limits;
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
    assert(delivery.info.signal==17 && delivery.info.sender==41 && delivery.info.code==1 && delivery.info.uid==22 && delivery.info.status==37);
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
    unsigned char encoded[128];kernel_signal_encode_info(encoded,&info);
    int32_t value;memcpy(&value,encoded+24,4);assert(value==19);
    memcpy(&value,encoded+20,4);assert(value==23);
    memcpy(&value,encoded+8,4);assert(value==5);
    info=(struct kernel_signal_wait_info){.signal=17};kernel_signal_encode_info(encoded,&info);
    memcpy(&value,encoded+24,4);assert(!value);
    info=(struct kernel_signal_wait_info){.signal=11,.address=UINT64_C(0x123456789)};kernel_signal_encode_info(encoded,&info);
    uint64_t address;memcpy(&address,encoded+16,8);assert(address==UINT64_C(0x123456789));
    assert(!kernel_signal_wait_take(&leader,&info) && !info.signal);
    selected_action_snapshot(&leader,&table);
    selected_payloads(&leader,&table);
    kernel_cpu_current()->current=previous;
    puts("SIGCHLD: blocked default stop policy, protected pending payload and common ABI encoding passed");
    /* The caller's real target owner already covers a self-representing
     * leader; a distinct group container still needs a cross-unlock borrow. */
    assert(signal_send_leader(&leader, &extra,0,0) == &leader && !extra);
    assert(!leader.storage_borrows);
    assert(signal_send_leader(&member, &extra,0,0) == &leader && extra == &leader);
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
    leader.proc_exiting = 1;
    result = signal_group_representative_get(&member);
    assert(result == &member);
    scheduler_task_put(result);
    leader.proc_exiting = 0;
    leader.state = KERNEL_THREAD_STATE_GROUP_DEAD;
    result = signal_group_representative_get(&member);
    assert(result == &member && member.storage_borrows == 1 && !leader.storage_borrows);
    scheduler_task_put(result);
    member.state = KERNEL_THREAD_STATE_EXITED;
    assert(!signal_group_representative_get(&member));
    assert(!leader.storage_borrows && !member.storage_borrows);
    puts("signal target selector: direct leader, dead leader fallback and unique borrow passed");
}

/* Only selected_payloads installs a native queue index. */
enum kernel_heap_status kernel_heap_allocate_zeroed(struct kernel_heap *h,size_t n,size_t size,void **out)
{ assert(h==&queue_heap && !queue_allocations && !kernel_cpu_current()->raw_locks);
  *out=calloc(n,size);assert(*out);queue_allocations++;return KERNEL_HEAP_STATUS_OK; }
void kernel_heap_release(struct kernel_heap *h,void *p)
{ assert(h==&queue_heap && queue_allocations==1 && p && !kernel_cpu_current()->raw_locks);
  free(p);queue_allocations--; }
