#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <kernel/errno.h>
/* The inherited main is retained only to share its real wait/CPU model. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main inherited_sleep_lock_main
#define __wrap_kernel_raw_lock_acquire inherited_raw_acquire
#define __wrap_kernel_raw_lock_release inherited_raw_release
#define kernel_wait_backend_switch inherited_backend_switch
#include "../host/sleep_lock.c"
#pragma GCC diagnostic pop
#undef main
#undef __wrap_kernel_raw_lock_acquire
#undef __wrap_kernel_raw_lock_release
#undef kernel_wait_backend_switch
static uint64_t clock_ticks=100;
static unsigned mode,attempt_seen,attempt_go,grant_seen,grant_go,watch_cost,cost_waiting;
static unsigned resumed_seen,resume_go,acquired,release_go,release_seen,release_publish_go;
static unsigned finished,quit_worker,worker_depth;
static void wait_flag(unsigned *flag)
{ while(!__atomic_load_n(flag,__ATOMIC_ACQUIRE))sched_yield(); }
static void set_clock(uint64_t now)
{ __atomic_store_n(&clock_ticks,now,__ATOMIC_RELEASE); }
static void publish(unsigned *flag)
{ __atomic_store_n(flag,1,__ATOMIC_RELEASE); }
uint64_t kernel_cost_clock(void) { return __atomic_load_n(&clock_ticks,__ATOMIC_ACQUIRE); }
uint64_t kernel_cost_irq_save(void)
{
    uint64_t old=sync_test_irq;sync_test_irq=0;
    if(host_current.id==1 && mode==5 && release_go && !lock.writer && !lock.readers &&
       !__atomic_exchange_n(&release_seen,1,__ATOMIC_ACQ_REL))wait_flag(&release_publish_go);
    if(host_current.id==1 && __atomic_load_n(&watch_cost,__ATOMIC_ACQUIRE))publish(&cost_waiting);
    return old;
}
void kernel_cost_irq_restore(uint64_t enabled) { if(enabled)sync_test_irq=1; }
struct kernel_cost_cpu *kernel_cost_cpu_current(void) { return &cpu.cost; }
struct kernel_cost_task *kernel_cost_current(void) { return &current.cost; }
void __real_kernel_cost_add(enum kernel_cost_metric,uint64_t);
void __wrap_kernel_cost_add(enum kernel_cost_metric metric,uint64_t value)
{
    if(host_current.id==1 && metric==COST_LOCK31_ATTEMPTS && mode==1) {
        publish(&attempt_seen);wait_flag(&attempt_go);
    }
    __real_kernel_cost_add(metric,value);
}
void __wrap_kernel_raw_lock_acquire(struct kernel_raw_lock *raw,struct kernel_raw_guard *guard)
{
    if(host_current.id==1 && raw==&lock.metadata && (mode==2 || mode==5) && !grant_seen)
        __atomic_store_n(&clock_ticks,200,__ATOMIC_RELEASE);
    inherited_raw_acquire(raw,guard);
}
void __wrap_kernel_raw_lock_release(struct kernel_raw_guard *guard)
{
    unsigned own=host_current.id==1 && guard->lock==&lock.metadata &&
        (mode==2 || mode==5) && lock.writer==&current.io_context && !grant_seen;
    inherited_raw_release(guard);
    if(own) {publish(&grant_seen);wait_flag(&grant_go);}
}
enum kernel_scheduler_status kernel_wait_backend_switch(struct kernel_task *task)
{
    enum kernel_scheduler_status status=inherited_backend_switch(task);
    if(host_current.id==1 && (mode==3 || mode==4)) {
        publish(&resumed_seen);wait_flag(&resume_go);
    }
    return status;
}
static void *boundary_worker(void *unused)
{
    (void)unused;initialize(1);host_current.dispatch=&ready;
    kernel_cost_join(&current.cost);kernel_cost_switch(&cpu.cost.bootstrap,&current.cost);
    struct kernel_lock_guard guard={0};
    if(mode==4)kernel_rwlock_read(&lock,&guard);else kernel_rwlock_write(&lock,&guard);
    assert(guard.owner==&current.io_context && current.io_context.locks==&guard);
    assert(mode==4 ? lock.readers==1 && !lock.writer : lock.writer==&current.io_context);
    publish(&acquired);wait_flag(&release_go);kernel_lock_release(&guard);
    assert(!current.io_context.locks && !cpu.raw_locks && !cpu.preempt_depth && !current.cost.depth);
    assert(current.wait.phase==KERNEL_WAIT_FINISHED);
    worker_depth=current.cost.depth;publish(&finished);wait_flag(&quit_worker);return NULL;
}
int main(int argc,char **argv)
{
    assert(argc==2);mode=(unsigned)atoi(argv[1]);assert(mode>=1 && mode<=5);
    setvbuf(stdout,NULL,_IONBF,0);initialize(0);kernel_wait_domain_init();kernel_rwlock_init(&lock,31,1);
    assert(!kernel_cost_begin(9,10000000,1,0));
    struct kernel_lock_guard initial={0};
    if(mode==3 || mode==4) {kernel_rwlock_write(&lock,&initial);set_clock(200);}
    pthread_t thread;assert(!pthread_create(&thread,NULL,boundary_worker,NULL));
    if(mode==1) {
        wait_flag(&attempt_seen);assert(!lock.writer && !lock.readers);
        uint64_t observer=kernel_cost_lock();publish(&watch_cost);publish(&attempt_go);wait_flag(&cost_waiting);
        assert(!lock.writer && !lock.readers);set_clock(200);__atomic_store_n(&watch_cost,0,__ATOMIC_RELEASE);kernel_cost_unlock(observer);
    } else if(mode==2 || mode==5) {
        wait_flag(&grant_seen);assert(lock.writer && !lock.readers);
        if(mode==2) {
            uint64_t observer=kernel_cost_lock();publish(&watch_cost);publish(&grant_go);wait_flag(&cost_waiting);
            set_clock(900);__atomic_store_n(&watch_cost,0,__ATOMIC_RELEASE);kernel_cost_unlock(observer);
        } else publish(&grant_go);
    } else {
        queued(1);while(!__atomic_load_n(&parked[0],__ATOMIC_ACQUIRE))sched_yield();
        set_clock(300);kernel_lock_release(&initial);wait_flag(&resumed_seen);
        assert(mode==4 ? lock.readers==1 && !lock.writer : lock.writer!=NULL);
        uint64_t observer=kernel_cost_lock();publish(&watch_cost);publish(&resume_go);wait_flag(&cost_waiting);
        set_clock(900);__atomic_store_n(&watch_cost,0,__ATOMIC_RELEASE);kernel_cost_unlock(observer);
    }
    wait_flag(&acquired);cost_waiting=0;
    set_clock(mode==1?400:1200);publish(&release_go);
    if(mode==5) {
        wait_flag(&release_seen);assert(!lock.writer && !lock.readers);
        uint64_t observer=kernel_cost_lock();publish(&watch_cost);publish(&release_publish_go);wait_flag(&cost_waiting);
        set_clock(1600);__atomic_store_n(&watch_cost,0,__ATOMIC_RELEASE);kernel_cost_unlock(observer);
    }
    wait_flag(&finished);assert(!kernel_cost_end(9,0));
    uint64_t wait,hold;assert(!kernel_cost_read(0,COST_LOCK31_WAIT_TICKS,&wait));
    assert(!kernel_cost_read(0,COST_LOCK31_HOLD_TICKS,&hold));
    uint64_t expected_hold=mode==1?200:mode==3 || mode==4?1100:1000;
    printf("resource boundary mode=%u: wait=%llu expected=100 hold=%llu expected=%llu\n",mode,
        (unsigned long long)wait,(unsigned long long)hold,(unsigned long long)expected_hold);
    assert(wait==100 && hold==expected_hold);
    assert(!current.cost.depth && !worker_depth && !lock.writer && !lock.readers && !lock.pending_head &&
           !lock.pending_tail && !lock.handoff_owner && !lock.waiters.registrations && !current.io_context.locks);
    publish(&quit_worker);assert(!pthread_join(thread,NULL));
    puts("PASS actual resource grant/revoke boundaries: delayed observer publication, FIFO token and scope owners returned");
}
