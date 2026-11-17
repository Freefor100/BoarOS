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
#define kernel_ptrace_signal signal_owner_trace_signal
#define kernel_ptrace_tracer_tid_locked signal_owner_trace_tid
#ifndef SIGNAL_SOURCE
#define SIGNAL_SOURCE "../../kernel/sched/signal.c"
#endif
#include <kernel/ptrace.h>
#include SIGNAL_SOURCE
#undef kernel_ptrace_signal
#undef kernel_ptrace_tracer_tid_locked
#undef process_group_request_exit
#undef kernel_user_thread_exit
#undef process_group_is_orphaned
#undef kernel_scheduler_wake_signal
#undef scheduler_switch_current_away
#undef stopped_append_locked
#undef physical_page_allocate
#undef physical_page_resolve
#include <stdlib.h>
enum riscv_direct_map_status riscv_direct_map_va_to_pa(uint64_t va,uint64_t size,uint64_t *pa)
{ (void)va;(void)size;(void)pa;abort(); }
static struct kernel_task *expected_init_exit;
static unsigned init_exit_calls;
void signal_owner_unexpected_group_exit(struct kernel_task *task,enum kernel_thread_exit_reason reason,uint64_t status,uint64_t detail)
{ if(task==expected_init_exit && reason==KERNEL_THREAD_EXIT_SIGNAL && status==9 && !detail) {init_exit_calls++;return;} abort(); }
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
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>

extern void signal_owner_bind_cpu(struct kernel_cpu *);
static struct kernel_heap queue_heap;
static unsigned queue_allocations;
static void (*queue_allocation_hook)(void);
static struct kernel_signal_table *init_reset_table;
static void reset_init_handler_during_allocation(void)
{
    queue_allocation_hook=NULL;
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    init_reset_table->actions[39].handler=KERNEL_SIGNAL_DFL;
}
static struct kernel_task *namespace_transfer_from;
static struct kernel_task namespace_transfer_to;
static void transfer_during_signal_allocation(void)
{
    queue_allocation_hook=NULL;
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    kernel_pid_transfer(&namespace_transfer_from->identities[KERNEL_PID_TID],
        &namespace_transfer_to.identities[KERNEL_PID_TID],&namespace_transfer_to);
}
static struct kernel_task *trace_target;
static struct kernel_signal_table *trace_table;
static unsigned trace_calls,trace_mode;
static struct kernel_signal_wait_info trace_expected;
uint32_t signal_owner_trace_tid(const struct kernel_task *task)
{ return task == trace_target ? 7 : 0; }
uint32_t signal_owner_trace_signal(struct kernel_task *task,struct kernel_signal_wait_info *info)
{
    assert(task==trace_target && !kernel_cpu_current()->raw_locks);
    trace_calls++;
    if(trace_mode==3) {
        assert(!memcmp(info,&trace_expected,sizeof(*info)));
        return info->signal;
    }
    assert(info->signal==10 && !task->signal_blocked);
    if(trace_mode==4) { assert(trace_table->actions[9].handler==KERNEL_SIGNAL_IGN);return 0; }
    assert(trace_table->actions[9].handler==0x5000);
    if(trace_mode==1) {
        trace_table->actions[9]=(struct kernel_signal_action){.handler=0x6000,.flags=LINUX_SA_NODEFER,.mask=signal_mask(14)};
        return info->signal;
    }
    if(trace_mode==2) {
        trace_expected=(struct kernel_signal_wait_info){.signal=12,.code=-1,.sender=37,.uid=42,.value=UINT64_C(0x1234567887654321)};
        *info=trace_expected;task->signal_blocked=signal_mask(12);return 12;
    }
    return 0;
}
static void traced_action_suppression(struct kernel_task *task,struct kernel_signal_table *table)
{
    task->signal_blocked=0;task->signal_pending=signal_mask(10);
    table->actions[9]=(struct kernel_signal_action){.handler=0x5000,.flags=LINUX_SA_RESETHAND,.mask=signal_mask(12)};
    trace_target=task;trace_table=table;trace_calls=trace_mode=0;
    struct kernel_signal_delivery delivery={0};
    assert(kernel_signal_select(task,&delivery)==KERNEL_SIGNAL_SELECT_NONE);
    assert(trace_calls==1 && table->actions[9].handler==0x5000 && !task->signal_blocked);
    task->signal_pending=signal_mask(10);trace_mode=1;
    assert(kernel_signal_select(task,&delivery)==KERNEL_SIGNAL_SELECT_HANDLER);
    assert(delivery.handler==0x6000 && delivery.flags==LINUX_SA_NODEFER &&
        task->signal_blocked==signal_mask(14) && table->actions[9].handler==0x6000);
    task->signal_blocked=0;task->signal_pending=signal_mask(10);trace_mode=4;
    table->actions[9].handler=KERNEL_SIGNAL_IGN;
    assert(!signal_ignored_unblocked(task,10,0));
    assert(kernel_signal_select(task,&delivery)==KERNEL_SIGNAL_SELECT_NONE && trace_calls==3);
    task->signal_pending=signal_mask(10);trace_mode=2;
    table->actions[9]=(struct kernel_signal_action){.handler=0x5000,.flags=LINUX_SA_RESETHAND,.mask=signal_mask(12)};
    table->actions[11]=(struct kernel_signal_action){.handler=0x7000,.flags=LINUX_SA_SIGINFO};
    task->limits->values[KERNEL_RLIMIT_SIGPENDING].current=64;task->files.heap=&queue_heap;
    assert(kernel_signal_select(task,&delivery)==KERNEL_SIGNAL_SELECT_NONE);
    assert(task->signal_pending==signal_mask(12) && table->actions[9].handler==0x5000);
    task->signal_blocked=0;trace_mode=3;
    assert(kernel_signal_select(task,&delivery)==KERNEL_SIGNAL_SELECT_HANDLER && trace_calls==5);
    assert(delivery.handler==0x7000 && !memcmp(&delivery.info,&trace_expected,sizeof(trace_expected)));
    trace_target=0;
    unsigned members=task->group_members;task->group_members=1;
    kernel_signal_pending_cleanup(task);task->group_members=members;task->files.heap=0;
    assert(!queue_allocations);
    puts("traced selector: suppression/RESETHAND/masks, resumed action, ignored signals, blocked full-siginfo requeue passed");
}
static unsigned action_phase;
static _Thread_local unsigned pause_selection;
static struct kernel_task *action_target;
static struct kernel_cpu action_cpu;
static struct signal_queue_record native_record;
static struct kernel_signal_wait_info *native_output;
static unsigned native_stale, native_completions;

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
    signal_pending_prepare_queue(task,1,&queue_heap,&p);
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
static void parent_death_payloads(struct kernel_task *leader, struct kernel_task *member)
{
    struct kernel_task *previous = kernel_cpu_current()->current;
    struct kernel_task cleanup = {0};
    struct kernel_credentials cleanup_cred = {.uid=123, .euid=123};
    cleanup.credentials = &cleanup_cred;
    cleanup.group_leader = &cleanup;
    kernel_cpu_current()->current = &cleanup;
    struct kernel_pid group = {.number=88, .generation=21, .registry=&scheduler.identities, .references=1};
    { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
      kernel_pid_attach(&leader->identities[KERNEL_PID_TGID], &group, KERNEL_PID_TGID, leader); }
    leader->signal_blocked = member->signal_blocked = signal_mask(42);
    leader->signal_wait_mask = member->signal_wait_mask = 0;
    leader->files.heap = &queue_heap;
    leader->limits->values[KERNEL_RLIMIT_SIGPENDING].current = 64;
    kernel_signal_parent_death(leader, &group, 42, 71, 456);
    kernel_signal_parent_death(member, &group, 42, 71, 456);
    assert(leader->group_pending == signal_mask(42) && !member->signal_pending);
    kernel_cpu_current()->current = leader;
    leader->signal_wait_mask = signal_mask(42);
    struct kernel_signal_wait_info info;
    for (unsigned i = 0; i < 2; i++) {
        assert(!kernel_signal_wait_take(leader, &info));
        assert(info.signal == 42 && !info.code && info.sender == 71 && info.uid == 456);
    }
    assert(!kernel_signal_wait_take(leader, &info) && !info.signal);
    kernel_cpu_current()->current = previous;
    unsigned members = leader->group_members;
    leader->group_members = 1;
    kernel_signal_pending_cleanup(leader);
    leader->group_members = members;
    leader->files.heap = 0;
    leader->signal_wait_mask = 0;
    assert(group.references == 2 && !leader->storage_borrows && !member->storage_borrows);
    assert(!queue_allocations);
    { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain); kernel_pid_detach(&leader->identities[KERNEL_PID_TGID]); }
    assert(group.references == 1 && !group.members[KERNEL_PID_TGID]);
    puts("parent death: two RT records in group pending, departing TGID/UID despite unprivileged cleanup sender");
}

/* Actual nested indexes and queues, with sibling local PID collisions. */
static void signal_namespace_cases(void)
{
    struct kernel_pid_registry root={0}, inner={0}, sibling={0};
    struct kernel_pid_allocator alloc[3]={0};uint64_t bits[3][1];
    struct kernel_pid_registry *spaces[]={&root,&inner,&sibling};
    struct {struct kernel_pid id;struct kernel_pid_binding ancestor;} ids[5]={0};
    struct kernel_task tasks[5]={0};
    struct kernel_signal_table tables[5]={0};
    struct kernel_resource_limits limits={0};
    limits.values[KERNEL_RLIMIT_SIGPENDING].current=64;
    struct kernel_task *previous=kernel_cpu_current()->current,*old_tasks=scheduler.all_tasks;
    for(unsigned i=0;i<3;i++) {
        assert(!kernel_pid_allocator_init(&alloc[i],bits[i],64));
        assert(!kernel_pid_registry_init(spaces[i],&alloc[i],i?&root:NULL));
    }
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        for(unsigned i=0;i<5;i++) {
            unsigned ns=i?i<3?1:2:0;
            kernel_pid_t numbers[2]={i?(i%2?1:2):1,i<3?20+(kernel_pid_t)i:28+(kernel_pid_t)i};
            assert(!kernel_pid_reserve_numbers(spaces[ns],&ids[i].id,sizeof(ids[i]),numbers,i?2:1));
            struct kernel_task *t=&tasks[i];
            t->magic=KERNEL_THREAD_MAGIC;t->arch.user_mode=1;t->accounted=1;
            t->state=KERNEL_THREAD_STATE_READY;t->group_leader=t;
            t->group_next=t->group_previous=t;t->group_members=1;
            t->files.heap=&queue_heap;t->limits=&limits;
            t->all_next=i<4?&tasks[i+1]:NULL;
            t->signal_blocked=signal_mask(10)|signal_mask(17);
            tables[i].magic=KERNEL_SIGNAL_TABLE_MAGIC;tables[i].references=1;
            kernel_wait_queue_init(&tables[i].signalfd_waiters);
            t->signal_table_address=(uintptr_t)&tables[i];
            for(unsigned role=0;role<KERNEL_PID_ROLES;role++)
                kernel_pid_attach(&t->identities[role],&ids[i].id,role,t);
            assert(!kernel_pid_publish_reserved(&ids[i].id));
            kernel_pid_put(&ids[i].id);
            t->tid=kernel_pid_number_in(&ids[i].id,&root);
        }
    }
    scheduler.all_tasks=tasks;kernel_cpu_current()->current=&tasks[1];
    /* Linux F_OWNER_PID selects a retained TID then sends to its group,
     * even where GETOWN would report zero; no integer relookup is allowed. */
    kernel_signal_sigio(&ids[2].id,1,0,0,10,7,1);
    struct kernel_signal_wait_info async_info;
    tasks[2].signal_wait_mask=signal_mask(10);kernel_cpu_current()->current=&tasks[2];
    assert(!kernel_signal_wait_take(&tasks[2],&async_info)&&async_info.signal==10);
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);kernel_pid_detach(&tasks[2].identities[KERNEL_PID_TGID]);}
    tasks[2].group_leader=&tasks[1];tasks[2].group_next=tasks[2].group_previous=&tasks[1];
    tasks[1].group_next=tasks[1].group_previous=&tasks[2];tasks[1].group_members=2;
    kernel_signal_sigio(&ids[2].id,1,0,0,10,7,1);
    assert(tasks[1].group_pending==signal_mask(10)&&!tasks[2].signal_pending);
    kernel_cpu_current()->current=&tasks[1];tasks[1].signal_wait_mask=signal_mask(10);
    assert(!kernel_signal_wait_take(&tasks[1],&async_info));
    tasks[1].signal_wait_mask=0;kernel_cpu_current()->current=&tasks[2];
    kernel_signal_sigio(&ids[2].id,0,0,0,10,7,1);
    assert(!kernel_signal_wait_take(&tasks[2],&async_info)&&async_info.signal==10);
    tasks[2].group_leader=&tasks[2];tasks[2].group_next=tasks[2].group_previous=&tasks[2];
    tasks[1].group_next=tasks[1].group_previous=&tasks[1];tasks[1].group_members=1;
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
     kernel_pid_attach(&tasks[2].identities[KERNEL_PID_TGID],&ids[2].id,KERNEL_PID_TGID,&tasks[2]);
     kernel_pid_detach(&tasks[3].identities[KERNEL_PID_PGID]);
     kernel_pid_attach(&tasks[3].identities[KERNEL_PID_PGID],&ids[2].id,KERNEL_PID_PGID,&tasks[3]);
     kernel_pid_detach(&tasks[3].identities[KERNEL_PID_SID]);
     kernel_pid_attach(&tasks[3].identities[KERNEL_PID_SID],&ids[2].id,KERNEL_PID_SID,&tasks[3]);}
    kernel_signal_sigio(&ids[2].id,2,0,0,10,7,1);
    tasks[3].signal_wait_mask=signal_mask(10);
    assert(!kernel_signal_wait_take(&tasks[2],&async_info));
    kernel_cpu_current()->current=&tasks[3];
    assert(!kernel_signal_wait_take(&tasks[3],&async_info));
    assert(!tasks[1].group_pending&&!tasks[4].group_pending);
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
     kernel_pid_detach(&tasks[3].identities[KERNEL_PID_PGID]);
     kernel_pid_attach(&tasks[3].identities[KERNEL_PID_PGID],&ids[3].id,KERNEL_PID_PGID,&tasks[3]);
     kernel_pid_detach(&tasks[3].identities[KERNEL_PID_SID]);
     kernel_pid_attach(&tasks[3].identities[KERNEL_PID_SID],&ids[3].id,KERNEL_PID_SID,&tasks[3]);}
    tasks[2].signal_wait_mask=tasks[3].signal_wait_mask=0;kernel_cpu_current()->current=&tasks[1];
    kernel_signal_pending_cleanup(&tasks[1]);kernel_signal_pending_cleanup(&tasks[2]);kernel_signal_pending_cleanup(&tasks[3]);
    assert(!queue_allocations);
    puts("SIGIO canonical owner: nonleader PID owner and cross-namespace real group membership passed");
    assert(kernel_signal_resolve_targets(&tasks[1],-1)==1);
    assert(kernel_signal_resolve_targets(&tasks[1],22)==0);
    assert(kernel_signal_resolve_targets(&tasks[1],2)==1);
    assert(kernel_signal_resolve_targets(&tasks[1],-2)==1);
    assert(kernel_signal_send_thread(&tasks[1],2,2,0,1)==1);
    assert(kernel_signal_send_thread(&tasks[1],22,2,0,1)==0);
    assert(kernel_signal_send_targets(&tasks[1],-1,10,1)==1);
    assert(tasks[2].group_pending==signal_mask(10) && !tasks[4].group_pending);
    struct kernel_signal_wait_info info;
    kernel_cpu_current()->current=&tasks[2];tasks[2].signal_wait_mask=signal_mask(10);
    assert(!kernel_signal_wait_take(&tasks[2],&info) && info.signal==10 && info.sender==1);
    tasks[2].signal_wait_mask=0;
    kernel_cpu_current()->current=&tasks[0];
    assert(kernel_signal_resolve_targets(&tasks[0],-1)==4);
    assert(kernel_signal_send_targets(&tasks[0],22,10,1)==1);
    kernel_cpu_current()->current=&tasks[2];tasks[2].signal_wait_mask=signal_mask(10);
    assert(!kernel_signal_wait_take(&tasks[2],&info) && info.sender==0 && info.signal==10);
    tasks[2].signal_wait_mask=0;
    /* User payload is preserved in the same view, zeroed only if sender is invisible. */
    struct kernel_signal_wait_info supplied={.signal=10,.code=-1,.sender=777,.value=123};
    kernel_cpu_current()->current=&tasks[1];
    assert(!kernel_signal_queue_info(2,2,1,&supplied));
    kernel_cpu_current()->current=&tasks[2];tasks[2].signal_wait_mask=signal_mask(10);
    assert(!kernel_signal_wait_take(&tasks[2],&info) && info.sender==777 && info.value==123);
    tasks[2].signal_wait_mask=0;kernel_cpu_current()->current=&tasks[0];
    assert(!kernel_signal_queue_info(22,22,1,&supplied));
    kernel_cpu_current()->current=&tasks[2];tasks[2].signal_wait_mask=signal_mask(10);
    assert(!kernel_signal_wait_take(&tasks[2],&info) && !info.sender && info.value==123);
    tasks[2].signal_wait_mask=0;
    /* Force a target TID role transfer across real unlocked queue preparation. */
    kernel_signal_pending_cleanup(&tasks[2]);
    kernel_cpu_current()->current=&tasks[1];
    namespace_transfer_from=&tasks[2];queue_allocation_hook=transfer_during_signal_allocation;
    assert(kernel_signal_queue_info(2,2,1,&supplied)==-KERNEL_ESRCH);
    assert(!queue_allocation_hook && !tasks[2].signal_pending && !queue_allocations);
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        kernel_pid_transfer(&namespace_transfer_to.identities[KERNEL_PID_TID],
                            &tasks[2].identities[KERNEL_PID_TID],&tasks[2]);
    }
    /* Canonical inherited PGID is not a same-number sibling PGID. Linux kill(0)
     * intentionally addresses this shared group even across PID view boundaries. */
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        for(unsigned i=1;i<5;i++) {
            kernel_pid_detach(&tasks[i].identities[KERNEL_PID_PGID]);
            kernel_pid_attach(&tasks[i].identities[KERNEL_PID_PGID],&ids[0].id,KERNEL_PID_PGID,&tasks[i]);
        }
    }
    assert(kernel_signal_resolve_targets(&tasks[1],0)==5);
    assert(kernel_signal_resolve_targets(&tasks[1],-1)==1);
    struct kernel_credentials from={.uid=100,.euid=100,.suid=100},
                              to={.uid=200,.euid=200,.suid=200};
    tasks[1].credentials=&from;tasks[2].credentials=&to;
    assert(kernel_signal_send_thread(&tasks[1],2,2,0,1)==-KERNEL_EPERM);
    tasks[1].credentials=tasks[2].credentials=NULL;
    /* SIGCHLD reports the child in the parent's view, not the child's local PID. */
    kernel_cpu_current()->current=&tasks[0];
    signal_send_child(&tasks[0],&tasks[2],17,1,7,0);
    tasks[0].signal_wait_mask=signal_mask(17);
    assert(!kernel_signal_wait_take(&tasks[0],&info) && info.sender==22 && info.status==7);
    tasks[0].signal_wait_mask=0;
    /* A successful send need not enqueue or terminate an unkillable init. */
    tasks[1].signal_unkillable=1;tasks[1].signal_blocked=tasks[1].signal_wait_mask=0;
    kernel_cpu_current()->current=&tasks[1];
    assert(!kernel_signal_send_task(&tasks[1],9,1));
    assert(!tasks[1].signal_pending && !init_exit_calls);
    assert(!kernel_signal_send_task(&tasks[1],10,1));
    assert(!tasks[1].signal_pending);
    tasks[1].state=KERNEL_THREAD_STATE_STOPPED;
    assert(kernel_signal_send_targets(&tasks[1],1,10,1)==1 && !tasks[1].group_pending);
    tasks[1].state=KERNEL_THREAD_STATE_READY;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        assert(signal_ignored_unblocked(&tasks[1],19,0));
        assert(!signal_ignored_unblocked(&tasks[1],19,1));
        assert(signal_ignored_unblocked(&tasks[0],9,1));
        assert(signal_ignored_unblocked(&tasks[0],19,1));
        assert(signal_init_ignored_locked(&tasks[1],10,1));
    }
    /* Blocking permits observation; after unblocking a default disposition
     * is discarded at delivery, not accidentally treated as fatal. */
    tasks[1].signal_blocked=signal_mask(10);
    assert(!kernel_signal_send_task(&tasks[1],10,1));
    assert(tasks[1].signal_pending==signal_mask(10));
    tasks[1].signal_blocked=0;
    struct kernel_signal_delivery ignored_delivery={0};
    assert(kernel_signal_select(&tasks[1],&ignored_delivery)==KERNEL_SIGNAL_SELECT_NONE);
    assert(!tasks[1].signal_pending);
    tables[1].actions[9].handler=0x1000;
    assert(!kernel_signal_send_task(&tasks[1],10,1));
    assert(kernel_signal_select(&tasks[1],&ignored_delivery)==KERNEL_SIGNAL_SELECT_HANDLER);
    assert(ignored_delivery.handler==0x1000);
    tables[1].actions[9].handler=0;tasks[1].signal_blocked=0;
    /* Real kill admission from an ancestor reaches the exit sink exactly once.
     * The sink is observed, not an assertion of complete reaper execution. */
    kernel_cpu_current()->current=&tasks[0];expected_init_exit=&tasks[1];
    assert(kernel_signal_send_targets(&tasks[0],21,9,1)==1);
    assert(init_exit_calls==1);expected_init_exit=NULL;
    /* Timer/poll payloads have no PID field: being an ancestor does not grant
     * their SI_TIMER/SI_SIGIO notifications the PID-bearing force behavior. */
    supplied=(struct kernel_signal_wait_info){.signal=9,.code=-2};
    assert(!kernel_signal_queue_info(21,21,1,&supplied) && init_exit_calls==1);
    supplied.code=-5;
    assert(!kernel_signal_queue_info(21,21,1,&supplied) && init_exit_calls==1);
    /* A user-supplied zero sender is not authority to bypass immunity. */
    kernel_cpu_current()->current=&tasks[1];
    supplied=(struct kernel_signal_wait_info){.signal=9,.code=-1,.sender=0};
    assert(!kernel_signal_queue_info(1,1,1,&supplied));
    assert(init_exit_calls==1);
    /* Recheck init's disposition after an unlocked allocation changed handler. */
    tables[1].actions[39].handler=0x1000;init_reset_table=&tables[1];
    queue_allocation_hook=reset_init_handler_during_allocation;
    supplied=(struct kernel_signal_wait_info){.signal=40,.code=-1,.sender=1};
    assert(!kernel_signal_queue_info(1,1,1,&supplied));
    assert(!queue_allocation_hook && !tasks[1].signal_pending && !queue_allocations);
    /* Timer queues have no ancestor authority and cannot invoke the kill sink. */
    struct signal_queue_record timer_record={.info={.signal=9,.code=-2}};
    struct kernel_wait_cursor timer_cursor={0};struct kernel_task *timer_wake=NULL;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        assert(signal_timer_post_locked(&ids[1].id,0,&timer_record,&timer_cursor,&timer_wake)==2);
        assert(!timer_wake && !tasks[1].group_pending);
    }
    trace_target=&tasks[1];
    kernel_signal_force_fault(&tasks[1],11,1,0);
    assert(tasks[1].signal_unkillable);
    tasks[1].signal_fault=(struct kernel_signal_fault){0};trace_target=NULL;
    kernel_signal_force_fault(&tasks[1],11,1,0);
    assert(!tasks[1].signal_unkillable && tasks[1].signal_fault.signal==11);
    tasks[1].signal_fault=(struct kernel_signal_fault){0};
    puts("init signal admission: successful ignores, handlers, blocked default, ancestor kill and timer guard passed");
    for(unsigned i=0;i<5;i++)kernel_signal_pending_cleanup(&tasks[i]);
    assert(!queue_allocations);
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        for(unsigned i=0;i<5;i++) {
            assert(!tasks[i].storage_borrows);
            for(unsigned role=0;role<KERNEL_PID_ROLES;role++)kernel_pid_detach(&tasks[i].identities[role]);
        }
        unsigned retired=0;while(kernel_pid_take_retired(&root))retired++;
        assert(retired==5);
        kernel_pid_registry_put(&inner);kernel_pid_registry_put(&sibling);
        retired=0;while(kernel_pid_take_retired_namespace(&root))retired++;
        assert(retired==2 && !root.retired && !root.retired_namespaces);
        kernel_pid_registry_put(&root);
    }
    scheduler.all_tasks=old_tasks;kernel_cpu_current()->current=previous;
    puts("signal PID views: sibling isolation, kill-all, tgkill, queued sender and SIGCHLD passed");
}

void signal_owner_cases(unsigned *lock_calls)
{
    scheduler.initialized = KERNEL_SCHEDULER_INITIALIZED;
    signal_namespace_cases();
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
    struct kernel_signal_table table = {.magic=KERNEL_SIGNAL_TABLE_MAGIC,.references=1};
    kernel_wait_queue_init(&table.signalfd_waiters);
    leader.signal_table_address=(uintptr_t)&table;
    struct kernel_pid_registry draining={0};
    struct kernel_pid identity={.registry=&draining};
    leader.identities[KERNEL_PID_TID].identity=&identity;
    assert(!kernel_signal_child_autoreap(&leader,17));
    kernel_signal_ignore_sigchld(&leader);
    assert(kernel_signal_child_autoreap(&leader,17));
    assert(!kernel_signal_child_autoreap(&leader,10));
    draining.shutdown_reaper=&leader;
    assert(kernel_signal_child_autoreap(&leader,10));
    leader.signal_table_address=0; /* Private default dispositions are lazy. */
    kernel_signal_ignore_sigchld(&leader);
    assert(!leader.signal_table_address && kernel_signal_child_autoreap(&leader,17));
    leader.signal_table_address=(uintptr_t)&table;
    pid_t corrupt=fork();assert(corrupt>=0);
    if(!corrupt){table.magic=0;kernel_signal_ignore_sigchld(&leader);_exit(0);}
    int status;assert(waitpid(corrupt,&status,0)==corrupt && WIFSIGNALED(status) && WTERMSIG(status)==SIGILL);
    draining.shutdown_reaper=&member;
    assert(!kernel_signal_child_autoreap(&leader,10));
    draining.shutdown_reaper=NULL;
    leader.identities[KERNEL_PID_TID].identity=NULL;
    table.actions[16].handler=KERNEL_SIGNAL_DFL;
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
    traced_action_suppression(&leader,&table);
    selected_action_snapshot(&leader,&table);
    selected_payloads(&leader,&table);
    parent_death_payloads(&leader,&member);
    kernel_cpu_current()->current=previous;
    puts("SIGCHLD: blocked default stop policy, protected pending payload and common ABI encoding passed");
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

/* Pending reinjection and native timer cases retain real queue ownership. */
enum kernel_heap_status kernel_heap_allocate_zeroed(struct kernel_heap *h,size_t n,size_t size,void **out)
{ assert((h==&queue_heap || h==&scheduler.identity_heap) && !kernel_cpu_current()->raw_locks);
  *out=calloc(n,size);assert(*out);__atomic_fetch_add(&queue_allocations,1,__ATOMIC_RELAXED);if(queue_allocation_hook)queue_allocation_hook();return KERNEL_HEAP_STATUS_OK; }
void kernel_heap_release(struct kernel_heap *h,void *p)
{ assert((h==&queue_heap || h==&scheduler.identity_heap) && queue_allocations && p && !kernel_cpu_current()->raw_locks);
  free(p);__atomic_fetch_sub(&queue_allocations,1,__ATOMIC_RELAXED); }

enum kernel_heap_status kernel_heap_allocate_zeroed_nosleep(struct kernel_heap *heap,size_t count,size_t size,void **out)
{ return kernel_heap_allocate_zeroed(heap,count,size,out); }

void signal_owner_heap_empty(void)
{ assert(!__atomic_load_n(&queue_allocations,__ATOMIC_RELAXED)); }

void signal_owner_set_allocation_hook(void (*hook)(void))
{ queue_allocation_hook=hook; }
