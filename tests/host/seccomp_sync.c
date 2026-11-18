#define SECCOMP_SCHEDULER_FIXTURE
#define main scalar_main
#include "seccomp.c"
#undef main
#include "../../kernel/sched/private.h"
#include "../../kernel/exec_internal.h"
#include <pthread.h>
#include <kernel/ptrace.h>
static pthread_mutex_t domain=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t event=PTHREAD_COND_INITIALIZER;
static _Thread_local struct kernel_cpu cpu;
static _Thread_local struct kernel_task *current;
static int parked,woken,timing,notifications;
static _Thread_local pthread_barrier_t *install_gate;
struct kernel_raw_lock kernel_wait_domain;
void *sync_test_cpu(void){cpu.initialized=KERNEL_CPU_INITIALIZED;return &cpu;}
void kernel_raw_lock_acquire(struct kernel_raw_lock *l,struct kernel_raw_guard *g)
{assert(l==&kernel_wait_domain);
 if(install_gate){pthread_barrier_t *gate=install_gate;install_gate=0;int r=pthread_barrier_wait(gate);assert(!r||r==PTHREAD_BARRIER_SERIAL_THREAD);}
 pthread_mutex_lock(&domain);g->lock=l;g->previous=cpu.raw_locks;cpu.raw_locks=g;}
void kernel_raw_lock_release(struct kernel_raw_guard *g)
{assert(cpu.raw_locks==g);cpu.raw_locks=g->previous;g->lock=0;pthread_mutex_unlock(&domain);}
void scheduler_task_get_locked(struct kernel_task *t){assert(kernel_raw_lock_owned(&kernel_wait_domain));t->storage_borrows++;}
void scheduler_task_put(struct kernel_task *t)
{pthread_mutex_lock(&domain);assert(t->storage_borrows);t->storage_borrows--;pthread_mutex_unlock(&domain);}
void kernel_signal_note_syscall_restart(struct kernel_task *t){t->syscall_restart.kind=KERNEL_SYSCALL_RESTART_GENERIC;}
enum kernel_scheduler_status kernel_wait_prepare(struct kernel_wait_queue *q,uint64_t deadline,int interruptible,struct kernel_wait_token *token)
{(void)q;assert(!deadline&&!interruptible);pthread_mutex_lock(&domain);
 if(timing==3){current->terminate_requested=1;kernel_seccomp_abort_wait_locked(current);assert(!notifications);}
 current->wait.interruptible=0;kernel_wait_record_store(&current->wait,1,KERNEL_WAIT_PREPARED,KERNEL_WAIT_WOKEN);
 *token=(struct kernel_wait_token){current,1};pthread_mutex_unlock(&domain);return 0;}
enum kernel_scheduler_status kernel_wait_park(const struct kernel_wait_token *t,enum kernel_wait_wake_reason *r)
{
    pthread_mutex_lock(&domain);
    if(timing==2)kernel_wait_record_store(&current->wait,t->generation,KERNEL_WAIT_COMMITTED,KERNEL_WAIT_WOKEN);
    if(timing==4){current->terminate_requested=1;kernel_seccomp_abort_wait_locked(current);}
    parked=1;pthread_cond_broadcast(&event);
    while(!woken)pthread_cond_wait(&event,&domain);
    *r=current->terminate_requested?KERNEL_WAIT_SIGNALLED:KERNEL_WAIT_WOKEN;
    pthread_mutex_unlock(&domain);return 0;
}
enum kernel_scheduler_status kernel_wait_finish(struct kernel_wait_token *t)
{pthread_mutex_lock(&domain);kernel_wait_record_store(&t->task->wait,1,KERNEL_WAIT_FINISHED,KERNEL_WAIT_WOKEN);
 if(timing==5){int old=notifications;current->terminate_requested=1;kernel_seccomp_abort_wait_locked(current);assert(old==notifications);}
 pthread_mutex_unlock(&domain);return 0;}
int kernel_wait_notify_locked(const struct kernel_wait_token *t,uint32_t reason)
{unsigned phase=kernel_wait_record_phase(&t->task->wait);
 if(t->generation!=kernel_wait_record_generation(&t->task->wait)||
    (phase!=KERNEL_WAIT_PREPARED&&phase!=KERNEL_WAIT_COMMITTED))return 0;
 assert(reason==KERNEL_WAIT_SIGNALLED);notifications++;woken=1;
 kernel_wait_record_store(&t->task->wait,t->generation,KERNEL_WAIT_NOTIFIED,reason);
 pthread_cond_broadcast(&event);return 1;}
enum kernel_scheduler_status kernel_wait_queue_wake_all(struct kernel_wait_queue *q)
{(void)q;pthread_mutex_lock(&domain);woken=1;pthread_cond_broadcast(&event);pthread_mutex_unlock(&domain);return 0;}
struct installation {struct kernel_task *task;unsigned error;int result;pthread_barrier_t *gate;};
static void *install_thread(void *p)
{
    struct installation *i=p;current=i->task;install_gate=i->gate;
    struct kernel_bpf_classic_insn f[]={I(0x20,0,0,0),I(0x15,0,1,i->error==17?172:173),
        I(0x06,0,0,0x50000|i->error),I(0x06,0,0,0x7fff0000)};
    i->result=attach_flags(i->task,f,4,1);return 0;
}
static void init_group(struct kernel_task *a,struct kernel_task *b)
{
    memset(a,0,sizeof(*a));memset(b,0,sizeof(*b));
    a->tid=1;b->tid=2;a->no_new_privs=1;
    a->group_leader=b->group_leader=a;a->group_next=b;b->group_next=a;
}
static void exec_wait_case(int when)
{
    struct kernel_task *a=calloc(1,sizeof(*a)),*b=calloc(1,sizeof(*b));init_group(a,b);
    struct kernel_exec_transaction tx={.state=KERNEL_EXEC_TRANSACTION_PREPARING};
    b->exec_transaction=&tx;parked=woken=notifications=0;timing=when;
    a->signal_pending=UINT64_C(1)<<9; /* Ordinary handler must remain deferred. */
    struct installation i={.task=a,.error=17};pthread_t thread;assert(!pthread_create(&thread,0,install_thread,&i));
    if(when!=3&&when!=4) {
        pthread_mutex_lock(&domain);while(!parked)pthread_cond_wait(&event,&domain);
        assert(!a->seccomp_mode&&!b->seccomp_mode&&!b->no_new_privs&&b->storage_borrows==1);
        struct kernel_wait_token stale={a,2};assert(!kernel_wait_notify_locked(&stale,KERNEL_WAIT_SIGNALLED)&&!notifications);
        if(when==1||when==2){a->terminate_requested=1;kernel_seccomp_abort_wait_locked(a);}
        pthread_mutex_unlock(&domain);
        if(when==0||when==5)kernel_task_exec_abort(b,&tx);
    }
    assert(!pthread_join(thread,0));assert(!b->storage_borrows&&!a->seccomp_exec_waiting);
    if(when) {
        assert(i.result==-KERNEL_EINTR&&!a->seccomp_mode&&!b->seccomp_mode);
        assert(!a->seccomp_exec_waiting);
    } else assert(!i.result&&a->seccomp_filter==b->seccomp_filter&&b->no_new_privs);
    kernel_seccomp_release(a);kernel_seccomp_release(b);assert(!live);free(a);free(b);
}

static void parallel_and_clone(void)
{
    struct kernel_task *a=calloc(1,sizeof(*a)),*b=calloc(1,sizeof(*b)),*child=calloc(1,sizeof(*child));
    init_group(a,b);b->no_new_privs=1;timing=0;
    pthread_barrier_t gate;assert(!pthread_barrier_init(&gate,0,2));
    struct installation one={.task=a,.error=17,.gate=&gate},two={.task=b,.error=33,.gate=&gate};
    pthread_t x,y;assert(!pthread_create(&x,0,install_thread,&one)&&!pthread_create(&y,0,install_thread,&two));
    assert(!pthread_join(x,0)&&!pthread_join(y,0)&&!pthread_barrier_destroy(&gate));
    assert(!one.result&&!two.result&&a->seccomp_filter==b->seccomp_filter);
    struct kernel_syscall_request q={.number=172};struct kernel_syscall_result r;
    assert(kernel_seccomp_check(a,&q,&r)&&r.value==-17);
    q.number=173;assert(kernel_seccomp_check(a,&q,&r)&&r.value==-33);
    /* Child storage existed before both publications. The real clone now
     * inherits only at its scheduler-locked publication point, not prepare. */
    {KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);kernel_seccomp_clone(child,a);}
    assert(child->seccomp_filter==a->seccomp_filter);
    assert(kernel_seccomp_check(child,&q,&r)&&r.value==-33);
    kernel_seccomp_release(a);kernel_seccomp_release(b);
    q.number=172;assert(kernel_seccomp_check(child,&q,&r)&&r.value==-17);
    kernel_seccomp_release(child);assert(!live);free(a);free(b);free(child);
}
int main(void)
{
    /* Exercise the production scheduler/exec wait, not a TSYNC-only model. */
    for(int when=0;when<6;when++)exec_wait_case(when);
    parallel_and_clone();
    puts("SECCOMP TSYNC host PASS exec abort/termination/atomic policy/borrow lifetime");
    return 0;
}
