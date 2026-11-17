#include "../../../kernel/sched/private.h"
#include <assert.h>
#include <kernel/errno.h>
#include <kernel/tty_task.h>
#include <stdio.h>

struct kernel_scheduler scheduler;
struct kernel_raw_lock kernel_wait_domain;
static struct kernel_cpu cpu;
_Thread_local uintptr_t sync_test_irq;
void *sync_test_cpu(void) { return &cpu; }
struct kernel_pid *process_identity(const struct kernel_task *task, enum kernel_pid_role role)
{
    return task->identities[role].identity;
}

struct kernel_pid_registry *process_pid_active(const struct kernel_task *task)
{ struct kernel_pid *id=task->identities[KERNEL_PID_TID].identity; return id?id->registry:&scheduler.identities; }

int main(void)
{
    kernel_cpu_initialize(&cpu, 7, 0);
    kernel_raw_lock_init(&kernel_wait_domain, KERNEL_RAW_RANK_SCHEDULER);
    struct kernel_pid_allocator numbers = {0};
    uint64_t bitmap[KERNEL_PID_BITMAP_WORDS(32)];
    assert(kernel_pid_allocator_init(&numbers, bitmap, 32) == KERNEL_PID_STATUS_OK);
    assert(!kernel_pid_registry_init(&scheduler.identities,&numbers,NULL));
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
    assert(group.references == references + 1);
    kernel_pid_put(found);
    kernel_pid_attach(&foreign.identities[KERNEL_PID_PGID], &group, KERNEL_PID_PGID, &foreign);
    kernel_pid_detach(&same.identities[KERNEL_PID_PGID]);
    kernel_pid_attach(&same.identities[KERNEL_PID_TID], &group, KERNEL_PID_TID, &same);
    assert(kernel_task_tty_find_group(&caller, group.number, &found) == -KERNEL_EPERM && !found);
    kernel_pid_detach(&foreign.identities[KERNEL_PID_PGID]);
    assert(kernel_task_tty_find_group(&caller, group.number, &found) == 0 && found == &group);
    kernel_pid_put(found);
    kernel_pid_attach(&foreign.identities[KERNEL_PID_TID], &group, KERNEL_PID_TID, &foreign);
    kernel_pid_detach(&same.identities[KERNEL_PID_TID]);
    assert(kernel_task_tty_find_group(&caller, group.number, &found) == -KERNEL_EPERM && !found);
    kernel_pid_detach(&foreign.identities[KERNEL_PID_TID]);
    struct kernel_pid_registry views[2]={0};struct kernel_pid_allocator alloc[2]={0};
    uint64_t bits[2][KERNEL_PID_BITMAP_WORDS(32)]={0};
    struct {struct kernel_pid id;struct kernel_pid_binding ancestor;} ids[2]={0};
    struct kernel_task actors[2]={0};
    for(unsigned i=0;i<2;i++){
        assert(!kernel_pid_allocator_init(&alloc[i],bits[i],32));
        assert(!kernel_pid_registry_init(&views[i],&alloc[i],&scheduler.identities));
        kernel_pid_t requested[]={1,(kernel_pid_t)(7+i)};
        assert(!kernel_pid_reserve_numbers(&views[i],&ids[i].id,sizeof(ids[i]),requested,2));
        assert(!kernel_pid_publish_reserved(&ids[i].id));
        kernel_pid_attach(&actors[i].identities[KERNEL_PID_TID],&ids[i].id,KERNEL_PID_TID,&actors[i]);
        kernel_pid_attach(&actors[i].identities[KERNEL_PID_PGID],&ids[i].id,KERNEL_PID_PGID,&actors[i]);
        kernel_pid_attach(&actors[i].identities[KERNEL_PID_SID],&sid,KERNEL_PID_SID,&actors[i]);
    }
    for(unsigned i=0;i<2;i++){
        assert(!kernel_task_tty_find_group(&actors[i],1,&found)&&found==&ids[i].id);kernel_pid_put(found);
        assert(kernel_task_tty_find_group(&actors[i],(kernel_pid_t)(8-i),&found)==-KERNEL_ESRCH);
    }
    assert(!kernel_task_tty_find_group(&caller,7,&found)&&found==&ids[0].id);kernel_pid_put(found);
    for(unsigned i=0;i<2;i++){
        kernel_pid_detach(&actors[i].identities[KERNEL_PID_TID]);
        kernel_pid_detach(&actors[i].identities[KERNEL_PID_PGID]);
        kernel_pid_detach(&actors[i].identities[KERNEL_PID_SID]);kernel_pid_put(&ids[i].id);
    }
    while(kernel_pid_take_retired(&scheduler.identities)){}
    for(unsigned i=0;i<2;i++)kernel_pid_registry_put(&views[i]);
    while(kernel_pid_take_retired_namespace(&scheduler.identities)){}
    kernel_pid_detach(&caller.identities[KERNEL_PID_SID]);
    kernel_pid_detach(&same.identities[KERNEL_PID_SID]);
    kernel_pid_detach(&foreign.identities[KERNEL_PID_SID]);
    assert(group.references == 1 && sid.references == 1 && foreign_sid.references == 1);
    kernel_pid_put(&group);
    kernel_pid_put(&sid);
    kernel_pid_put(&foreign_sid);
    assert(!numbers.allocated);
    while(kernel_pid_take_retired(&scheduler.identities)){}
    assert(!scheduler.identities.retired&&!scheduler.identities.retired_namespaces);
    puts("TTY group host: actual identity lookup/status/PID fallback/owned lookup refs pass");
}
