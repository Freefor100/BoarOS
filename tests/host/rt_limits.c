#include "../../kernel/sched/private.h"
#include <kernel/credentials.h>
#include <kernel/sched_policy.h>
#include <kernel/tick.h>
#include <kernel/time.h>
#include <arch/timer.h>
#include <kernel/errno.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
uint64_t kernel_timer_fd_next;
_Thread_local uintptr_t sync_test_irq;
static struct kernel_cpu cpus[2];
static struct kernel_cpu *active;
static struct kernel_task task,peer,idles[2];
static struct kernel_credentials caller_cred,target_cred;
static struct kernel_resource_limits limits;
static uint64_t now,stamp;
static unsigned checks,failed,index_updates,resource_updates,tick_reads;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"RT HOST FAIL line=%d %s\n",__LINE__,#x);failed++;}} while(0)
void *sync_test_cpu(void){return active;}
void *sync_test_current_task(void){return active->current;}
void sync_test_current_task_set(void *p){active->current=p;}
uint64_t kernel_time_monotonic_ns(void){return now;}
uint64_t riscv_time_read(void){return now;}
uint64_t kernel_tick_count(void){tick_reads++;return stamp;}
uint32_t kernel_time_is_initialized(void){return 1;}
enum kernel_time_status kernel_time_deadline_from_monotonic(uint64_t ns,uint64_t *out){*out=ns;return 0;}
enum riscv_timer_status riscv_timer_set_scheduler_deadline(uint64_t v){(void)v;return RISCV_TIMER_STATUS_NOT_STARTED;}
struct kernel_task *deadline_index_first(void){return 0;}
struct kernel_task *process_find_identity(int32_t pid,enum kernel_pid_role role){assert(role==KERNEL_PID_TID);return pid==2?&peer:0;}
void scheduler_task_publish_locked(struct kernel_task *p){assert(!p->accounted);p->accounted=1;}
void scheduler_cpu_timer_update_locked(struct kernel_task *p,unsigned thread)
{assert(active->raw_locks&&thread==1);index_updates++;p->cpu_timer_index[thread].indexed=!!(p->resource_flags&KERNEL_RESOURCE_RTTIME_DUE);}
struct kernel_wait_record *kernel_wait_record_of(struct kernel_task *p){return &p->wait;}
struct kernel_task *kernel_wait_current_task(void){return active->current;}
struct kernel_wait_node *kernel_wait_default_node(struct kernel_task *p){return &p->default_wait_node;}
int kernel_wait_backend_initialized(void){return 1;}
void kernel_wait_backend_ready(struct kernel_task *p){ready_enqueue_locked(p,0);}
void kernel_wait_backend_notify(struct kernel_task *p,uint32_t reason){(void)reason;p->state=KERNEL_THREAD_STATE_READY;}
void kernel_wait_backend_quiesce(void){__builtin_trap();}
static void setup(void)
{
    scheduler=(struct kernel_scheduler){.initialized=KERNEL_SCHEDULER_INITIALIZED};
    task=(struct kernel_task){0};peer=(struct kernel_task){0};memset(idles,0,sizeof(idles));
    limits=(struct kernel_resource_limits){.references=2};
    for(unsigned i=0;i<KERNEL_RLIMIT_COUNT;i++)limits.values[i]=(struct kernel_rlimit64){KERNEL_RLIM_INFINITY,KERNEL_RLIM_INFINITY};
    limits.values[KERNEL_RLIMIT_RTPRIO]=(struct kernel_rlimit64){10,10};
    active=&cpus[0];memset(cpus,0,sizeof(cpus));
    kernel_raw_lock_init(&kernel_wait_domain,KERNEL_RAW_RANK_SCHEDULER);
    for(unsigned i=0;i<2;i++){kernel_cpu_initialize(&cpus[i],i,0);cpus[i].logical_id=i;cpus[i].idle=&idles[i];idles[i].idle=1;idles[i].cpu=&cpus[i];kernel_rt_bandwidth_init(&cpus[i].rt_bandwidth,0);}
    cpus[0].current=&task;cpus[1].current=&idles[1];scheduler.cpus=&cpus[0];cpus[0].scheduler_next=&cpus[1];
    task.cpu=&cpus[0];task.group_leader=&task;task.group_members=1;task.magic=KERNEL_THREAD_MAGIC;task.tid=1;task.tid_owned=1;task.arch.user_mode=1;task.accounted=1;task.state=KERNEL_THREAD_STATE_RUNNING;task.wait.on_cpu=1;task.limits=&limits;task.cpu_mask=3;
    peer=task;peer.tid=2;peer.group_leader=&peer;peer.wait.on_cpu=0;peer.state=KERNEL_THREAD_STATE_READY;
    caller_cred=(struct kernel_credentials){.uid=1001,.euid=1001};target_cred=caller_cred;task.credentials=&caller_cred;peer.credentials=&target_cred;
    now=stamp=0;index_updates=resource_updates=tick_reads=0;
}
static void permission(void)
{
    setup();CHECK(!kernel_task_sched_set(&task,0,1,5,0));CHECK(task.scheduling.priority==5);
    CHECK(!kernel_task_sched_set(&task,0,2,10,0));CHECK(task.scheduling.policy==2);
    CHECK(kernel_task_sched_set(&task,0,2,11,0)==-KERNEL_EPERM);
    limits.values[KERNEL_RLIMIT_RTPRIO].current=0;
    CHECK(!kernel_task_sched_set(&task,0,2,9,0));CHECK(kernel_task_sched_set(&task,0,1,9,0)==-KERNEL_EPERM);
    CHECK(!kernel_task_sched_set(&task,0,2|KERNEL_SCHED_RESET_ON_FORK,9,0));CHECK(kernel_task_sched_set(&task,0,2,9,0)==-KERNEL_EPERM);
    caller_cred.effective=UINT64_C(1)<<KERNEL_CAP_SYS_NICE;
    CHECK(!kernel_task_sched_set(&task,0,1,90,0));caller_cred.effective=0;
    CHECK(!kernel_task_sched_set(&task,0,1,89,0));CHECK(kernel_task_sched_set(&task,0,1,90,0)==-KERNEL_EPERM);
    limits.values[KERNEL_RLIMIT_RTPRIO].current=5;
    CHECK(!kernel_task_sched_set(&task,2,1,4,0));target_cred.uid=target_cred.euid=1002;
    CHECK(kernel_task_sched_set(&task,2,1,3,0)==-KERNEL_EPERM);
}
static void tick_accounting(void)
{
    setup();limits.values[KERNEL_RLIMIT_RTTIME]=(struct kernel_rlimit64){30000,80000};
    stamp=10;kernel_scheduler_charge_ticks(1,1);CHECK(!task.rttime_ticks&&!index_updates);
    CHECK(!tick_reads);
    CHECK(!kernel_sched_policy_set(&task.scheduling,1,10,0));
    kernel_scheduler_charge_ticks(1,1);CHECK(task.rttime_ticks==1&&task.rttime_stamp==10&&!index_updates);
    CHECK(tick_reads==1);
    kernel_scheduler_charge_ticks(1,0);CHECK(task.rttime_ticks==1);
    active=&cpus[1];cpus[0].current=&idles[0];cpus[1].current=&task;task.cpu=active;
    kernel_scheduler_charge_ticks(1,1);CHECK(task.rttime_ticks==1);
    for(stamp=11;stamp<=13;stamp++)kernel_scheduler_charge_ticks(1,1);
    CHECK(task.rttime_ticks==4&&task.rttime_stamp==13&&index_updates==1);
    CHECK((task.resource_flags&KERNEL_RESOURCE_RTTIME_DUE)&&task.cpu_timer_index[1].indexed);
    unsigned fired;{KERNEL_RAW_SCOPE(g,&kernel_wait_domain);fired=scheduler_rt_limit_check_locked(&task);}
    CHECK(fired==4&&!task.cpu_timer_index[1].indexed&&limits.values[KERNEL_RLIMIT_RTTIME].current==1030000);
    CHECK(!(task.resource_flags&KERNEL_RESOURCE_RTTIME_DUE));
    for(stamp=14;stamp<=17;stamp++)kernel_scheduler_charge_ticks(1,1);
    CHECK(!(task.resource_flags&KERNEL_RESOURCE_RTTIME_DUE));
    stamp=18;kernel_scheduler_charge_ticks(1,1);
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);fired=scheduler_rt_limit_check_locked(&task);}
    CHECK(fired==8);
    limits.values[KERNEL_RLIMIT_RTTIME].current=KERNEL_RLIM_INFINITY;stamp=19;kernel_scheduler_charge_ticks(1,1);CHECK(task.rttime_ticks==9);
}
static void departures(void)
{
    for(unsigned state=0;state<5;state++){
        setup();CHECK(!kernel_sched_policy_set(&task.scheduling,1,10,0));task.rttime_ticks=7;task.rttime_stamp=50;
        task.resource_flags=KERNEL_RESOURCE_NPROC_CHARGED|KERNEL_RESOURCE_RTTIME_DUE;
        struct kernel_wait_token token;
        {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);CHECK(!kernel_wait_prepare_locked(0,0,1,&token));}
        if(state==0){CHECK(!kernel_wait_notify(&token,KERNEL_WAIT_WOKEN));CHECK(task.rttime_ticks==7);}
        else if(state<3){KERNEL_RAW_SCOPE(g,&kernel_wait_domain);kernel_wait_record_store(&task.wait,token.generation,KERNEL_WAIT_COMMITTED,KERNEL_WAIT_WOKEN);task.state=KERNEL_THREAD_STATE_BLOCKED;if(state==1)CHECK(kernel_wait_notify_locked(&token,KERNEL_WAIT_WOKEN));}
        else {
            CHECK(!kernel_wait_finish(&token));
            task.state=KERNEL_THREAD_STATE_STOPPED;
            /* SIGCONT can arrive while the stopped owner is still on CPU. */
            if(state==4){task.state=KERNEL_THREAD_STATE_READY;task.wait.ready_pending=1;}
        }
        {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);struct kernel_task *next=scheduler_select_next_locked(active,&task,0);CHECK(next==active->idle);}
        int reset=state==2||state==3;
        CHECK(task.rttime_ticks==(reset?0U:7U));CHECK(task.rttime_stamp==50);
        CHECK(!!(task.resource_flags&KERNEL_RESOURCE_RTTIME_DUE)==!reset);CHECK(task.resource_flags&KERNEL_RESOURCE_NPROC_CHARGED);
    }
    puts("RT HOST phases: early wait/continue retain; actual BLOCKED/STOPPED departure resets");
}
static void uncharged_departures(void)
{
    for(unsigned rt=0;rt<2;rt++)for(unsigned stopped=0;stopped<2;stopped++) {
        setup();
        if(rt)CHECK(!kernel_sched_policy_set(&task.scheduling,1,10,0));
        task.resource_flags=KERNEL_RESOURCE_NPROC_CHARGED;
        task.state=stopped?KERNEL_THREAD_STATE_STOPPED:KERNEL_THREAD_STATE_BLOCKED;
        {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);CHECK(scheduler_select_next_locked(active,&task,0)==active->idle);}
        CHECK(!task.rttime_ticks&&!index_updates);
        CHECK(task.resource_flags==KERNEL_RESOURCE_NPROC_CHARGED);
        CHECK(!resource_updates);
    }
    puts("RT HOST uncharged: OTHER and unlimited RT departures leave resource flags untouched");
}
static void historical_departures(void)
{
    for(unsigned unlimited=0;unlimited<2;unlimited++)for(unsigned stopped=0;stopped<2;stopped++) {
        setup();limits.values[KERNEL_RLIMIT_RTTIME]=(struct kernel_rlimit64){30000,80000};
        task.resource_flags=KERNEL_RESOURCE_NPROC_CHARGED;
        CHECK(!kernel_task_sched_set(&task,0,1,10,0));
        for(stamp=1;stamp<=4;stamp++)kernel_scheduler_charge_ticks(1,1);
        CHECK(task.rttime_ticks==4&&task.rttime_stamp==4);
        CHECK((task.resource_flags&KERNEL_RESOURCE_RTTIME_DUE)&&task.cpu_timer_index[1].indexed);
        if(unlimited) {
            KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
            limits.values[KERNEL_RLIMIT_RTTIME]=(struct kernel_rlimit64){KERNEL_RLIM_INFINITY,KERNEL_RLIM_INFINITY};
        } else CHECK(!kernel_task_sched_set(&task,0,0,0,0));
        kernel_scheduler_charge_ticks(1,1);
        CHECK(task.rttime_ticks==4);
        task.state=stopped?KERNEL_THREAD_STATE_STOPPED:KERNEL_THREAD_STATE_BLOCKED;
        unsigned previous_updates=index_updates;
        {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);CHECK(scheduler_select_next_locked(active,&task,0)==active->idle);}
        CHECK(!task.rttime_ticks&&task.rttime_stamp==4);
        CHECK(task.resource_flags==KERNEL_RESOURCE_NPROC_CHARGED);
        CHECK(!task.cpu_timer_index[1].indexed&&index_updates==previous_updates+1);
    }
    puts("RT HOST history: RT to OTHER and finite to infinity still reset on real departure");
}
int main(void){permission();tick_accounting();departures();uncharged_departures();historical_departures();printf("RT HOST checks=%u failed=%u\n",checks,failed);return !!failed;}

/* Observe the real scheduler's atomic updates without changing their effect. */
static uint32_t resource_fetch_and(uint32_t *word,uint32_t mask,int order)
{
    if(word==&task.resource_flags)resource_updates++;
    return __atomic_fetch_and(word,mask,order);
}
#define __atomic_fetch_and resource_fetch_and
#include "../../kernel/sched/scheduling.c"
#undef __atomic_fetch_and
