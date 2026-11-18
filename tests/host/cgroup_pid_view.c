/* Production cgroup membership and PID indexes; heap/permission/VFS edges modeled. */
#include "../../kernel/sched/cgroup.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
struct kernel_scheduler scheduler;
struct kernel_raw_lock kernel_wait_domain;
static struct kernel_cpu cpu;
_Thread_local uintptr_t sync_test_irq;
void *sync_test_cpu(void){return &cpu;}
void kernel_cpu_flush_notifications(void){assert(!cpu.outbound);}
void *sync_test_current_task(void){return cpu.current;}
struct kernel_task *kernel_task_current(void){return cpu.current;}
static struct kernel_cgroup root={.refs=1,.linked=1},destination={.refs=1,.linked=1};
static struct kernel_heap heap;
static struct kernel_credentials opener;
static unsigned allocations,fail_alloc;
static int denied;
static void (*allocation_hook)(void);
struct kernel_cgroup *kernel_cgroup_root(void){return &root;}
struct kernel_cgroup *kernel_cgroup_get(struct kernel_cgroup *g){assert(g->refs);g->refs++;return g;}
void kernel_cgroup_put(struct kernel_cgroup *g){assert(!cpu.raw_locks&&g->refs>1);g->refs--;}
int kernel_cgroup_migration_permission(struct kernel_cgroup *from,struct kernel_cgroup *to,const struct kernel_credentials *cred)
{assert(cpu.raw_locks&&from&&to&&cred==&opener);return denied?-KERNEL_EACCES:0;}
enum kernel_heap_status kernel_heap_allocate(struct kernel_heap *h,size_t size,void **out)
{assert(h==&heap&&!cpu.raw_locks);if(fail_alloc)return KERNEL_HEAP_STATUS_EMPTY;*out=malloc(size);assert(*out);allocations++;if(allocation_hook){void (*hook)(void)=allocation_hook;allocation_hook=NULL;hook();}return KERNEL_HEAP_STATUS_OK;}
void kernel_heap_release(struct kernel_heap *h,void *p){assert(h==&heap&&!cpu.raw_locks&&allocations);free(p);allocations--;}
static void move_during_allocation(void){assert(!kernel_cgroup_move(&destination,0,0,&opener));}
static void unlink_during_allocation(void){KERNEL_RAW_SCOPE(g,&kernel_wait_domain);destination.linked=0;}
static void expect(struct kernel_cgroup *g,int threads,const char *expected)
{
    char *data=NULL;size_t size=0;
    assert(!kernel_cgroup_members_snapshot(g,threads,&heap,&data,&size));
    assert(size==strlen(expected)&&!memcmp(data,expected,size));kernel_heap_release(&heap,data);
}
int main(void)
{
    kernel_cpu_initialize(&cpu,0,NULL);kernel_raw_lock_init(&kernel_wait_domain,KERNEL_RAW_RANK_SCHEDULER);
    struct kernel_pid_registry inner={0},sibling={0};struct kernel_pid_allocator alloc[3]={0};
    struct kernel_pid_registry *views[]={&scheduler.identities,&inner,&sibling};
    uint64_t bits[3][KERNEL_PID_BITMAP_WORDS(64)]={0};
    for(unsigned i=0;i<3;i++){assert(!kernel_pid_allocator_init(&alloc[i],bits[i],64));assert(!kernel_pid_registry_init(views[i],&alloc[i],i?views[0]:NULL));}
    struct {struct kernel_pid id;struct kernel_pid_binding ancestor;} ids[4]={0};struct kernel_task tasks[4]={0};
    for(unsigned i=0;i<4;i++){
        unsigned ns=i==0?0:i==2?2:1;kernel_pid_t number[]={i==3?2:1,i==2?21:(kernel_pid_t)(10+i)};
        struct kernel_task *t=&tasks[i];t->group_leader=t;t->group_next=t->group_previous=t;t->group_members=1;
        t->accounted=t->arch.user_mode=1;t->all_next=i<3?&tasks[i+1]:NULL;
        assert(!kernel_pid_reserve_numbers(views[ns],&ids[i].id,sizeof(ids[i]),number,i?2:1));
        kernel_pid_attach(&t->identities[KERNEL_PID_TID],&ids[i].id,KERNEL_PID_TID,t);
        if(i<3)kernel_pid_attach(&t->identities[KERNEL_PID_TGID],&ids[i].id,KERNEL_PID_TGID,t);
        assert(!kernel_pid_publish_reserved(&ids[i].id));t->tid=kernel_pid_number_in(&ids[i].id,views[0]);
    }
    tasks[3].group_leader=&tasks[1];tasks[1].group_next=tasks[1].group_previous=&tasks[3];
    tasks[3].group_next=tasks[3].group_previous=&tasks[1];tasks[1].group_members=2;
    scheduler.all_tasks=tasks;cpu.current=&tasks[0];
    expect(&root,0,"1\n11\n21\n");expect(&root,1,"1\n11\n21\n13\n");
    cpu.current=&tasks[1];expect(&root,0,"0\n1\n0\n");expect(&root,1,"0\n1\n0\n2\n");
    assert(kernel_cgroup_move(&destination,21,0,&opener)==-KERNEL_ESRCH);
    assert(kernel_cgroup_move(&destination,1,1,&opener)==-KERNEL_EOPNOTSUPP);
    denied=1;assert(kernel_cgroup_move(&destination,1,0,&opener)==-KERNEL_EACCES&&!destination.members);denied=0;
    assert(!kernel_cgroup_move(&destination,1,0,&opener)&&destination.members==2&&destination.refs==3);
    assert(!tasks[2].cgroup&&tasks[1].cgroup==&destination&&tasks[3].cgroup==&destination);
    expect(&destination,0,"1\n");expect(&destination,1,"1\n2\n");
    cpu.current=&tasks[2];expect(&destination,0,"0\n");expect(&destination,1,"0\n0\n");
    cpu.current=&tasks[1];tasks[1].proc_exiting=1;
    expect(&destination,0,"1\n");expect(&destination,1,"2\n");tasks[1].proc_exiting=0;
    assert(!kernel_cgroup_move(&root,0,0,&opener)&&!destination.members&&destination.refs==1);
    allocation_hook=move_during_allocation;expect(&root,0,"0\n0\n");assert(!allocation_hook&&destination.members==2);
    assert(!kernel_cgroup_move(&root,0,0,&opener));
    char *data=NULL;size_t size=0;allocation_hook=unlink_during_allocation;
    assert(kernel_cgroup_members_snapshot(&destination,0,&heap,&data,&size)==-KERNEL_ENODEV&&!allocations);
    destination.linked=1;
    fail_alloc=1;assert(kernel_cgroup_members_snapshot(&root,0,&heap,&data,&size)==-KERNEL_ENOMEM&&!allocations);fail_alloc=0;
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
     for(unsigned i=0;i<4;i++){
        kernel_pid_detach(&tasks[i].identities[KERNEL_PID_TID]);
        if(i<3)kernel_pid_detach(&tasks[i].identities[KERNEL_PID_TGID]);
        kernel_pid_put(&ids[i].id);
     }
     unsigned n=0;while(kernel_pid_take_retired(views[0]))n++;assert(n==4);
     kernel_pid_registry_put(&inner);kernel_pid_registry_put(&sibling);
     n=0;while(kernel_pid_take_retired_namespace(views[0]))n++;assert(n==2);kernel_pid_registry_put(views[0]);}
    assert(!allocations&&!root.members&&!destination.members);
    puts("PASS cgroup2 current-reader zero rows, sibling same PID, caller-view migration, live-thread representative and owners");return 0;
}
