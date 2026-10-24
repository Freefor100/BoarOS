#define BOAROS_HOST_IRQ_STATE 1
#include "../../kernel/sched/private.h"
#include "../../kernel/signal_pending.h"
#include <kernel/posix_timer.h>
#include <kernel/credentials.h>
#include <kernel/time.h>
#include <kernel/errno.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
_Thread_local uintptr_t network_host_irq=RISCV_SSTATUS_SIE;
struct kernel_raw_lock kernel_wait_domain={.rank=KERNEL_RAW_RANK_SCHEDULER,.initialized=KERNEL_RAW_INITIALIZED};
static struct kernel_heap heap;
static struct kernel_task task;
static struct kernel_pid pid;
static struct kernel_pid_member members[2];
static uint64_t now_ns;
static unsigned live,ignored,allocation_calls,allocation_fail,cpu_samples;
static uint64_t cpu_user,cpu_kernel;
static pthread_barrier_t entered,released;
static _Thread_local int hold_clock;
static void gate(pthread_barrier_t *p){int r=pthread_barrier_wait(p);assert(!r||r==PTHREAD_BARRIER_SERIAL_THREAD);}
uint64_t kernel_time_monotonic_ns(void)
{
    if(hold_clock&&!kernel_cpu_current()->raw_locks){hold_clock=0;gate(&entered);gate(&released);}
    return __atomic_load_n(&now_ns,__ATOMIC_RELAXED);
}
uint64_t kernel_time_realtime_ns(void){return kernel_time_monotonic_ns()+1000000000000ULL;}
uint64_t kernel_time_boot_realtime_offset(void){return 1000000000000ULL;}
uint64_t riscv_time_read(void){return kernel_time_monotonic_ns()/100;}
enum kernel_time_status kernel_time_deadline_from_monotonic(uint64_t ns,uint64_t *out)
{if(ns<=kernel_time_monotonic_ns())return KERNEL_TIME_STATUS_DEADLINE_PASSED;*out=(ns+99)/100;return 0;}
void kernel_scheduler_deadline_changed(void){assert(!kernel_cpu_current()->raw_locks);}
void kernel_cpu_flush_notifications(void){assert(!kernel_cpu_current()->outbound);}
struct kernel_cpu *riscv_smp_cpu(uint32_t n){assert(!n);return kernel_cpu_current();}
void kernel_cpu_notify(struct kernel_cpu *cpu,unsigned event){(void)cpu;(void)event;abort();}
enum kernel_heap_status kernel_heap_allocate_zeroed(struct kernel_heap *h,size_t n,size_t size,void **out)
{assert(h==&heap&&!kernel_cpu_current()->raw_locks);allocation_calls++;if(allocation_fail)return KERNEL_HEAP_STATUS_EMPTY;*out=calloc(n,size);assert(*out);__atomic_fetch_add(&live,1,__ATOMIC_RELAXED);return 0;}
void kernel_heap_release(struct kernel_heap *h,void *p)
{assert(h==&heap&&p&&!kernel_cpu_current()->raw_locks);free(p);assert(__atomic_fetch_sub(&live,1,__ATOMIC_RELAXED));}
void scheduler_task_get_locked(struct kernel_task *t){assert(t==&task);t->storage_borrows++;}
void scheduler_task_put(struct kernel_task *t){assert(t==&task);KERNEL_RAW_SCOPE(g,&kernel_wait_domain);assert(t->storage_borrows);t->storage_borrows--;}
struct kernel_pid *process_identity(const struct kernel_task *t,enum kernel_pid_role role)
{assert(t==&task&&(role==KERNEL_PID_TID||role==KERNEL_PID_TGID));return &pid;}
struct kernel_task *process_find_identity(kernel_pid_t n,enum kernel_pid_role role)
{assert(role==KERNEL_PID_TID);return n==1?&task:0;}
void kernel_pid_get(struct kernel_pid *p){assert(p==&pid);p->references++;}
void kernel_task_identity_put(struct kernel_pid *p){assert(p==&pid);KERNEL_RAW_SCOPE(g,&kernel_wait_domain);assert(p->references>1);p->references--;}
int scheduler_cpu_clock_resolve(struct kernel_task *t,int32_t c,int strict,struct scheduler_cpu_clock *out)
{assert(t==&task&&strict);if(c!=2&&c!=3)return -KERNEL_EINVAL;kernel_pid_get(&pid);*out=(struct scheduler_cpu_clock){&pid,2,c==3};return 0;}
int scheduler_cpu_clock_sample(const struct scheduler_cpu_clock *c,struct kernel_task_usage *u)
{assert(c->identity==&pid&&!kernel_cpu_current()->raw_locks);cpu_samples++;
 if(task.proc_exiting)return -KERNEL_ESRCH;
 *u=(struct kernel_task_usage){.user_ns=cpu_user,.kernel_ns=cpu_kernel};return 0;}
unsigned signal_cpu_timer_check_locked(struct kernel_task *t,const struct kernel_task_usage *u)
{assert(t==&task&&u->user_ns==cpu_user&&kernel_cpu_current()->raw_locks);return 0;}
void signal_cpu_timer_notify(struct kernel_task *t,unsigned fired){assert(t==&task&&!fired);}
void signal_timer_notify(struct kernel_task *t,unsigned sig){(void)t;(void)sig;abort();}
int signal_timer_disposition_ignored_locked(struct kernel_pid *p,int thread,unsigned sig)
{assert(p==&pid);(void)thread;(void)sig;return ignored;}
int signal_timer_post_locked(struct kernel_pid *p,int thread,struct signal_queue_record *record,struct kernel_wait_cursor *cursor,struct kernel_task **wake)
{assert(p==&pid);(void)cursor;*wake=0;if(ignored)return 2;signal_pending_post_timer_locked(&task,!thread,record);return 1;}
void kernel_wait_cursor_wake(struct kernel_wait_cursor *cursor,int all){(void)cursor;(void)all;abort();}
enum kernel_scheduler_status kernel_scheduler_wake_signal(struct kernel_task *t){(void)t;abort();}
static unsigned sleep_mode,sleep_notified;
enum kernel_scheduler_status kernel_wait_prepare_locked(struct kernel_wait_queue *q,uint64_t deadline,int interruptible,struct kernel_wait_token *token)
{assert(!q&&!deadline&&interruptible&&kernel_cpu_current()->raw_locks);*token=(struct kernel_wait_token){&task,1};return 0;}
int kernel_wait_notify_locked(const struct kernel_wait_token *token,uint32_t reason)
{assert(token->task==&task&&reason==KERNEL_WAIT_WOKEN&&kernel_cpu_current()->raw_locks);sleep_notified++;return 1;}
enum kernel_scheduler_status kernel_wait_park(const struct kernel_wait_token *token,enum kernel_wait_wake_reason *reason)
{
    assert(token->task==&task&&!kernel_cpu_current()->raw_locks&&task.cpu_sleep);
    if(!sleep_mode){cpu_user=500;scheduler_cpu_timer_expire();assert(sleep_notified==1);*reason=KERNEL_WAIT_WOKEN;}
    else *reason=KERNEL_WAIT_SIGNALLED;
    return 0;
}
enum kernel_scheduler_status kernel_wait_finish(struct kernel_wait_token *token)
{assert(token->task==&task&&!kernel_cpu_current()->raw_locks&&!task.cpu_sleep);*token=(struct kernel_wait_token){0};return 0;}
static uint64_t count(void){KERNEL_RAW_SCOPE(g,&kernel_wait_domain);return kernel_signal_pending_count_locked(&task);}
static void init(void)
{
    task=(struct kernel_task){0};task.group_leader=&task;task.group_members=1;task.files.heap=&heap;
    task.cpu_limit=(struct kernel_rlimit64){KERNEL_RLIM_INFINITY,KERNEL_RLIM_INFINITY};
    task.sigpending_limit=(struct kernel_rlimit64){4096,4096};task.tid=1;
    pid=(struct kernel_pid){.number=1,.references=1,.generation=1};
    for(unsigned r=0;r<2;r++){members[r]=(struct kernel_pid_member){.identity=&pid,.role=r,.task=&task};pid.members[r]=&members[r];}
    ignored=allocation_fail=cpu_samples=0;cpu_user=cpu_kernel=0;now_ns=1000000000;
}
static void finish(void)
{kernel_posix_timers_exec(&task);kernel_signal_pending_cleanup(&task);assert(!count()&&!live&&!task.storage_borrows&&pid.references==1&&!kernel_posix_timer_deadline());}
static int create_timer(int sig)
{
    struct kernel_posix_event event={.notify=0,.signal=sig,.value=0x12345678};int32_t id;
    struct kernel_posix_timer *t;assert(!kernel_posix_timer_prepare(&task,1,&event,&id,&t));kernel_posix_timer_publish(t,1);return id;
}
static void arm(int id,uint64_t value,uint64_t interval)
{
    struct kernel_posix_itimerspec in={.value={value/1000000000,value%1000000000},.interval={interval/1000000000,interval%1000000000}},old;
    assert(!kernel_posix_timer_set(&task,id,1,&in,&old));
}
static void expire(uint64_t now){now_ns=now;kernel_posix_timer_expire(now/100+1);}
static struct kernel_signal_wait_info take(unsigned sig)
{
    struct kernel_signal_wait_info out;struct signal_pending_prepared p={0};
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);signal_pending_take_locked(&task,1,sig,&out,&p);}
    signal_pending_finish(&p);return out;
}
static void reservation_and_mixed(void)
{
    init();int a=create_timer(10),b=create_timer(10);assert(count()==2);
    struct signal_pending_prepared p={0};struct kernel_signal_wait_info info={.signal=10,.code=0,.sender=42};
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);assert(!signal_pending_post_locked(&task,1,&task,&info,0,&p));}
    signal_pending_finish(&p);assert(count()==3);
    task.sigpending_limit.current=0;allocation_fail=1;unsigned allocations=allocation_calls;
    arm(a,now_ns+100,0);arm(b,now_ns+200,0);expire(now_ns+200);
    assert(allocation_calls==allocations);allocation_fail=0;
    assert(take(10).sender==42&&count()==2);
    info=take(10);assert(info.code==-2&&info.sender==(unsigned)a);
    info=take(10);assert(info.code==-2&&info.sender==(unsigned)b&&!task.group_pending&&count()==2);
    assert(!kernel_posix_timer_delete(&task,a)&&!kernel_posix_timer_delete(&task,b));finish();
}
static void stale_and_ignored(void)
{
    init();int id=create_timer(36);arm(id,now_ns+100,100);expire(now_ns+100);
    arm(id,now_ns+1000000,0);assert(!take(36).signal&&count()==1);
    expire(now_ns+1000000);assert(!kernel_posix_timer_delete(&task,id)&&count()==1);
    assert(!take(36).signal&&!count());finish();
    init();ignored=1;id=create_timer(36);arm(id,now_ns+100,100);expire(now_ns+100);
    assert(!task.group_pending&&!kernel_posix_timer_deadline()&&count()==1);
    now_ns+=100000;ignored=0;kernel_posix_timer_unignore(&task,36);expire(now_ns+100);
    struct kernel_signal_wait_info out=take(36);assert(out.signal==36&&out.uid>=1000);
    assert(!kernel_posix_timer_delete(&task,id));finish();
}
static void clear_batch_and_exec(void)
{
    init();int ids[32];for(unsigned i=0;i<32;i++){ids[i]=create_timer(10);arm(ids[i],now_ns+100,100);}
    expire(now_ns+100);assert(count()==32);ignored=1;
    struct signal_pending_prepared p={0};int more;
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);more=signal_pending_ignore_locked(&task,1,UINT64_C(1)<<9,&p);}
    signal_pending_finish(&p);assert(more&&count()==32);
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);assert(!signal_pending_ignore_locked(&task,1,UINT64_C(1)<<9,&p));}
    signal_pending_finish(&p);ignored=0;kernel_posix_timer_unignore(&task,10);
    unsigned seen[32]={0};
    for(unsigned i=0;i<32;i++){struct kernel_signal_wait_info out=take(10);assert(out.signal==10&&out.sender<32&&!seen[out.sender]++);}
    assert(!task.group_pending&&count()==32);
    for(unsigned i=0;i<32;i++)assert(!kernel_posix_timer_delete(&task,ids[i]));
    finish();
    init();int id=create_timer(36);struct kernel_signal_wait_info info={.signal=36,.code=-1,.value=7};
    signal_pending_prepare(&task,1,&task,&heap,&info,1,&p);
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);assert(!signal_pending_post_locked(&task,1,&task,&info,1,&p));}
    signal_pending_finish(&p);info.code=-2;
    signal_pending_prepare(&task,1,&task,&heap,&info,1,&p);
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);assert(!signal_pending_post_locked(&task,1,&task,&info,1,&p));}
    signal_pending_finish(&p);arm(id,now_ns+100,0);expire(now_ns+100);assert(count()==3);
    kernel_posix_timers_exec(&task);signal_pending_flush_timers(&task);assert(count()==1);
    info=take(36);assert(info.signal==36&&info.code==-1&&info.value==7);finish();
}
static int changing_id;
static void *delete_during_completion(void *unused)
{(void)unused;gate(&entered);assert(!kernel_posix_timer_delete(&task,changing_id)&&count()==1);gate(&released);return 0;}
static void completion_owner(void)
{
    init();changing_id=create_timer(36);arm(changing_id,now_ns+100,100);expire(now_ns+100);
    assert(!pthread_barrier_init(&entered,0,2)&&!pthread_barrier_init(&released,0,2));pthread_t other;
    assert(!pthread_create(&other,0,delete_during_completion,0));hold_clock=1;
    struct kernel_signal_wait_info info=take(36);assert(info.signal==36&&info.sender==(unsigned)changing_id);
    assert(!pthread_join(other,0)&&!count()&&!kernel_posix_timer_deadline());
    assert(!pthread_barrier_destroy(&entered)&&!pthread_barrier_destroy(&released));finish();
}
static void *reset_during_completion(void *unused)
{
    (void)unused;gate(&entered);arm(changing_id,now_ns+100,0);expire(now_ns+100);
    assert(count()==1);gate(&released);return 0;
}
static void completion_reset(void)
{
    init();changing_id=create_timer(36);arm(changing_id,now_ns+100,100);expire(now_ns+100);
    assert(!pthread_barrier_init(&entered,0,2)&&!pthread_barrier_init(&released,0,2));pthread_t other;
    assert(!pthread_create(&other,0,reset_during_completion,0));hold_clock=1;
    struct kernel_signal_wait_info out=take(36);assert(out.signal==36);
    assert(!pthread_join(other,0)&&count()==1&&!kernel_posix_timer_overrun(&task,changing_id));
    out=take(36);assert(out.signal==36&&!out.uid&&!task.group_pending);
    assert(!kernel_posix_timer_delete(&task,changing_id));
    assert(!pthread_barrier_destroy(&entered)&&!pthread_barrier_destroy(&released));finish();
}
static void cpu_thresholds(void)
{
    init();int ids[3];struct kernel_posix_event event={.notify=0,.signal=36};
    for(unsigned i=0;i<3;i++) {
        struct kernel_posix_timer *t;assert(!kernel_posix_timer_prepare(&task,2,&event,&ids[i],&t));
        kernel_posix_timer_publish(t,1);arm(ids[i],100+i,100);
    }
    unsigned before=cpu_samples;cpu_user=101;allocation_fail=1;
    scheduler_cpu_timer_expire();assert(cpu_samples==before+1);allocation_fail=0;
    struct kernel_signal_wait_info out=take(36);assert(out.signal==36&&out.sender==(unsigned)ids[0]);
    out=take(36);assert(out.signal==36&&out.sender==(unsigned)ids[1]);
    assert(!task.group_pending);
    cpu_user=1000;scheduler_cpu_timer_expire();out=take(36);assert(out.signal==36&&out.uid>=8);
    task.proc_exiting=1;kernel_posix_cpu_target_exit(&task);assert(!task.cpu_timer_index[0].indexed);
    struct kernel_posix_itimerspec setting={.value={0,1}},old;
    for(unsigned i=0;i<3;i++) {
        assert(!kernel_posix_timer_get(&task,ids[i],&old)&&!old.value.nanoseconds&&!old.interval.nanoseconds);
        assert(kernel_posix_timer_set(&task,ids[i],0,&setting,&old)==-KERNEL_ESRCH);
        assert(!kernel_posix_timer_delete(&task,ids[i]));
    }
    finish();
}
static void changed_uid_reservation(void)
{
    init();int id=create_timer(36);
    struct kernel_credentials user=*kernel_credentials_initial();
    user.uid=1000;user.account=kernel_uid_account_get(&heap,user.uid);assert(user.account);
    task.credentials=&user;
    assert(!count() && kernel_credentials_initial()->account->pending==1);
    arm(id,now_ns+100,0);expire(now_ns+100);
    assert(take(36).signal==36 && !count() && kernel_credentials_initial()->account->pending==1);
    assert(!kernel_posix_timer_delete(&task,id));
    assert(!kernel_credentials_initial()->account->pending);
    task.credentials=kernel_credentials_initial();kernel_uid_account_put(user.account);finish();
}
static void cpu_sleep_owner(void)
{
    init();uint64_t deadline,remaining;unsigned charges=count();
    sleep_mode=sleep_notified=0;
    assert(!kernel_posix_cpu_sleep(&task,2,0,100,&deadline,&remaining));
    assert(deadline==100&&!remaining&&!task.cpu_sleep&&!task.cpu_timer_index[0].indexed&&count()==charges);
    cpu_user=100;sleep_mode=1;
    assert(kernel_posix_cpu_sleep(&task,2,1,800,&deadline,&remaining)==-KERNEL_ERESTARTSYS);
    assert(deadline==800&&remaining==700&&!task.cpu_sleep&&!task.cpu_timer_index[0].indexed&&count()==charges);
    allocation_fail=1;
    assert(kernel_posix_cpu_sleep(&task,2,1,800,&deadline,&remaining)==-KERNEL_ENOMEM);
    allocation_fail=0;finish();
}
int main(void)
{
    cpu_sleep_owner();
    reservation_and_mixed();stale_and_ignored();clear_batch_and_exec();completion_owner();completion_reset();cpu_thresholds();changed_uid_reservation();
    puts("POSIX native reservation, mixed FIFO, stale generation, ignore phase and completion/delete owner passed");return 0;
}
