/* Inspect emitted note bytes. The register payload is architecture-specific
 * and is exercised by the two guest consumers; only that edge is modeled. */
#include "../../kernel/sched/coredump.c"
#include <assert.h>
#include <stdio.h>

struct kernel_scheduler scheduler;
struct kernel_raw_lock kernel_wait_domain;
static struct kernel_cpu cpu;
_Thread_local uintptr_t sync_test_irq;
void *sync_test_cpu(void){return &cpu;}
void *sync_test_current_task(void){return cpu.current;}
void kernel_cpu_flush_notifications(void){assert(!cpu.outbound);}
size_t arch_ptrace_regset_size(unsigned kind){assert(kind==1);return 8;}
void arch_ptrace_regset(struct kernel_task *task,unsigned kind,uint64_t *regs,uint64_t *original,int set)
{(void)original;assert(task && kind==1 && !set);regs[0]=42;}

static void check_note(struct kernel_task *task,int32_t tid,int32_t parent,int32_t pgid,int32_t sid)
{
    unsigned char note[256]={0};int32_t ids[4];
    size_t size=core_note(task,6,note);
    assert(size>=20+48);memcpy(ids,note+20+32,sizeof(ids));
    assert(ids[0]==tid && ids[1]==parent && ids[2]==pgid && ids[3]==sid);
}

int main(void)
{
    kernel_cpu_initialize(&cpu,0,NULL);kernel_raw_lock_init(&kernel_wait_domain,KERNEL_RAW_RANK_SCHEDULER);
    struct kernel_pid_registry child={0},*root=&scheduler.identities;
    struct kernel_pid_allocator alloc[2]={0};uint64_t bits[2][KERNEL_PID_BITMAP_WORDS(64)]={0};
    for(unsigned i=0;i<2;i++)assert(!kernel_pid_allocator_init(&alloc[i],bits[i],64));
    assert(!kernel_pid_registry_init(root,&alloc[0],NULL));
    assert(!kernel_pid_registry_init(&child,&alloc[1],root));
    struct {struct kernel_pid id;struct kernel_pid_binding ancestor;} ids[4]={0};
    struct kernel_task leader={.group_leader=&leader},thread={.group_leader=&leader};
    for(unsigned i=0;i<4;i++){
        kernel_pid_t numbers[]={(kernel_pid_t)i+1,(kernel_pid_t)i+23};
        assert(!kernel_pid_reserve_numbers(i?&child:root,&ids[i].id,sizeof(ids[i]),numbers,i?2:1));
        assert(!kernel_pid_publish_reserved(&ids[i].id));
    }
    kernel_pid_attach(&leader.identities[KERNEL_PID_TID],&ids[1].id,KERNEL_PID_TID,&leader);
    kernel_pid_attach(&thread.identities[KERNEL_PID_TID],&ids[2].id,KERNEL_PID_TID,&thread);
    kernel_pid_attach(&leader.identities[KERNEL_PID_PGID],&ids[3].id,KERNEL_PID_PGID,&leader);
    kernel_pid_attach(&leader.identities[KERNEL_PID_SID],&ids[0].id,KERNEL_PID_SID,&leader);
    leader.child_creator=&ids[0].id;
    check_note(&leader,2,0,4,0);check_note(&thread,3,0,4,0);
    leader.child_creator=&ids[3].id;check_note(&thread,3,4,4,0);
    puts("PASS core PRSTATUS inner thread/group, outside creator/session and visible creator");return 0;
}
