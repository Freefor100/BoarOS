/* Production comparison, lazy sighand and exit barrier; host hardware/lookup seams. */
#include "../../kernel/sched/process.c"
#include "../../kernel/sched/kcmp.c"
#include "../../kernel/sched/signal.c"
#include "../../fs/open_file_internal.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

_Thread_local uintptr_t sync_test_irq;
static _Thread_local struct kernel_cpu cpu={.initialized=KERNEL_CPU_INITIALIZED};
static _Thread_local struct kernel_task *running;
struct kernel_scheduler scheduler;
struct kernel_raw_lock kernel_wait_domain,kernel_vfs_state_lock;
static struct kernel_task tasks[2];
static struct kernel_pid ids[2];
static struct kernel_files_record records[2];
static struct kernel_file_slot slots[2][4];
static struct kernel_open_file_description descriptions[2];
static unsigned pages,borrows,services,denied,change_security,retired,scenario;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static int at_fd,resume_fd,exit_at_barrier,exit_done;
static int64_t reading_result;
void *sync_test_cpu(void){return &cpu;}
void *sync_test_current_task(void){return running;}
void kernel_cpu_flush_notifications(void){assert(!cpu.outbound);}
void kernel_assert_can_block(void){assert(!cpu.raw_locks&&!cpu.preempt_depth);}
void kernel_service_scope_begin(struct kernel_service_scope *scope)
{(void)scope;services++;assert(!"query entered CPU0");}
void kernel_service_scope_end(struct kernel_service_scope *scope){(void)scope;}
struct kernel_task *process_find_identity(kernel_pid_t number,enum kernel_pid_role role)
{assert(role==KERNEL_PID_TID&&cpu.raw_locks);for(unsigned i=0;i<2;i++)if(tasks[i].tid==number&&!retired)return &tasks[i];return 0;}
struct kernel_pid *process_identity(const struct kernel_task *task,enum kernel_pid_role role)
{return task?task->identities[role].identity:0;}
struct kernel_task *scheduler_task_lookup_locked(kernel_pid_t number,enum kernel_pid_role role,uint64_t generation)
{assert(!generation);struct kernel_task *task=process_find_identity(number,role);if(task){task->storage_borrows++;__atomic_fetch_add(&borrows,1,__ATOMIC_RELAXED);}return task;}
void scheduler_task_put(struct kernel_task *task)
{assert(!cpu.raw_locks&&task->storage_borrows);__atomic_fetch_sub(&task->storage_borrows,1,__ATOMIC_RELAXED);__atomic_fetch_sub(&borrows,1,__ATOMIC_RELAXED);}
int kernel_credentials_may_access_locked(const struct kernel_task *from,const struct kernel_task *to,int fs)
{assert(from==&tasks[0]&&(to==&tasks[0]||to==&tasks[1])&&!fs&&cpu.raw_locks);return !(denied&&to==&tasks[1]);}
int kernel_files_is_live(const struct kernel_files *files)
{return files->state==KERNEL_FILES_LIVE&&files->record;}
enum kernel_random_status kernel_random_fill(void *out,size_t size)
{assert(!cpu.raw_locks);for(size_t i=0;i<size;i++)((unsigned char *)out)[i]=(unsigned char)(i*71+11);return 0;}
void clear_page(void *page){memset(page,0,BOAROS_PAGE_SIZE);}
enum physical_page_status physical_page_allocate(struct physical_page_allocator *a,uint64_t *out)
{assert(a==scheduler.allocator&&!cpu.raw_locks);*out=(uintptr_t)aligned_alloc(BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE);assert(*out);pages++;return 0;}
enum physical_page_status physical_page_resolve(const struct physical_page_allocator *a,uint64_t address,void **out)
{assert(a==scheduler.allocator);*out=(void *)(uintptr_t)address;return 0;}
enum physical_page_status physical_page_release(struct physical_page_allocator *a,uint64_t address)
{assert(a==scheduler.allocator&&pages&&!cpu.raw_locks);free((void *)(uintptr_t)address);pages--;return 0;}
enum riscv_direct_map_status riscv_direct_map_va_to_pa(uint64_t va,uint64_t size,uint64_t *pa)
{assert(size==BOAROS_PAGE_SIZE);*pa=va;return 0;}
void kernel_wait_queue_init(struct kernel_wait_queue *queue){memset(queue,0,sizeof(*queue));}
void kernel_wait_queue_pollfree(struct kernel_wait_queue *queue){(void)queue;assert(!cpu.raw_locks);}
enum kernel_scheduler_status kernel_wait_queue_destroy(struct kernel_wait_queue *queue)
{(void)queue;assert(!cpu.raw_locks);return 0;}
void kernel_wait_backend_quiesce(void){assert(!"unexpected signalfd borrower");}
void __real_kernel_raw_lock_acquire(struct kernel_raw_lock *,struct kernel_raw_guard *);
void __wrap_kernel_raw_lock_acquire(struct kernel_raw_lock *lock,struct kernel_raw_guard *guard)
{
    if(running==&tasks[1]&&scenario==3&&lock==&tasks[1].files_access){
        assert(!pthread_mutex_lock(&gate));exit_at_barrier=1;assert(!pthread_cond_broadcast(&changed));assert(!pthread_mutex_unlock(&gate));
    }
    if(running==&tasks[0]&&lock==&records[0].metadata_lock){
        if(change_security){denied=1;change_security=0;}
        if(scenario==3){assert(!pthread_mutex_lock(&gate));at_fd=1;assert(!pthread_cond_broadcast(&changed));
            while(!resume_fd)assert(!pthread_cond_wait(&changed,&gate));
            assert(!pthread_mutex_unlock(&gate));}
    }
    __real_kernel_raw_lock_acquire(lock,guard);
}
static void setup(void)
{
    running=&tasks[0];cpu.current=running;cpu.logical_id=1;
    kernel_raw_lock_init(&kernel_wait_domain,KERNEL_RAW_RANK_SCHEDULER);
    kernel_raw_lock_init(&kernel_vfs_state_lock,KERNEL_RAW_RANK_OBJECT);
    scheduler.allocator=(void *)(uintptr_t)1;kernel_kcmp_initialize();
    for(unsigned i=0;i<2;i++){
        tasks[i].tid=10+(int)i;tasks[i].magic=KERNEL_THREAD_MAGIC;tasks[i].arch.user_mode=1;
        tasks[i].wait.phase=KERNEL_WAIT_FINISHED;ids[i].generation=i+1;
        tasks[i].identities[KERNEL_PID_TID].identity=&ids[i];
        kernel_raw_lock_init(&tasks[i].files_access,KERNEL_RAW_RANK_TASK_FILES);
        kernel_raw_lock_init(&records[i].metadata_lock,KERNEL_RAW_RANK_OBJECT);
        records[i].statistics.capacity=4;records[i].references=1;records[i].slots=slots[i];
        tasks[i].files.record=&records[i];tasks[i].files.state=KERNEL_FILES_LIVE;
        tasks[i].mm.record=(void *)(uintptr_t)(100+i);tasks[i].fs.record=(void *)(uintptr_t)(200+i);
        slots[i][0].description=&descriptions[0];slots[i][1].description=&descriptions[1];
    }
}
static int64_t query(int pid1,int pid2,int type,uint64_t x,uint64_t y)
{int64_t result=kernel_kcmp(&tasks[0],pid1,pid2,type,x,y);assert(!borrows&&!cpu.raw_locks&&!cpu.preempt_depth&&cpu.logical_id==1&&!services);return result;}
static void semantics(void)
{
    assert(query(10,10,KCMP_FILE,0,0)==0);
    assert(query(10,11,KCMP_FILE,UINT64_C(1)<<32,0)==0);
    long order=query(10,11,KCMP_FILE,0,1);assert(order==1||order==2);
    assert(query(11,10,KCMP_FILE,1,0)==3-order);
    assert(query(10,11,KCMP_SYSVSEM,0,0)==0);
    assert(query(10,11,KCMP_FILE,4,0)==-KERNEL_EBADF);
    assert(query(10,11,KCMP_IO,0,0)==-KERNEL_EOPNOTSUPP);
    assert(query(10,11,KCMP_EPOLL_TFD,0,UINT64_MAX)==-KERNEL_EOPNOTSUPP);
    assert(query(-1,11,999,0,0)==-KERNEL_ESRCH);
    denied=1;assert(query(10,11,999,0,0)==-KERNEL_EPERM);denied=0;
    assert(query(10,11,999,0,0)==-KERNEL_EINVAL);
    change_security=1;assert(query(10,11,KCMP_FILE,0,0)==-KERNEL_EPERM);denied=0;
    assert(records[0].references==1&&records[1].references==1);
}
static void sighand(void)
{
    long order=query(10,11,KCMP_SIGHAND,0,0);assert(order==1||order==2);
    assert(!signal_alloc_table(&tasks[0])&&!signal_alloc_table(&tasks[1]));
    assert(query(10,11,KCMP_SIGHAND,0,0)==order);
    tasks[1].proc_exiting=1;
    assert(query(10,11,KCMP_SIGHAND,0,0)==order);
    assert(!kernel_signal_release_table(&tasks[1])&&!kernel_signal_release_table(&tasks[0])&&!pages);
    tasks[1].proc_exiting=0;
}
static void *reader(void *unused)
{(void)unused;running=&tasks[0];cpu.current=running;cpu.logical_id=1;reading_result=kernel_kcmp(running,10,11,KCMP_FILE,0,0);return 0;}
static void *exiter(void *unused)
{
    (void)unused;running=&tasks[1];cpu.current=running;
    process_begin_exit(running,&(const struct kernel_thread_completion){0});
    assert(!pthread_mutex_lock(&gate));exit_done=1;assert(!pthread_cond_broadcast(&changed));assert(!pthread_mutex_unlock(&gate));return 0;
}
static void departure(void)
{
    scenario=3;pthread_t reading,exiting;assert(!pthread_create(&reading,0,reader,0));
    assert(!pthread_mutex_lock(&gate));while(!at_fd)assert(!pthread_cond_wait(&changed,&gate));assert(!pthread_mutex_unlock(&gate));
    assert(!pthread_create(&exiting,0,exiter,0));
    assert(!pthread_mutex_lock(&gate));while(!exit_at_barrier)assert(!pthread_cond_wait(&changed,&gate));
    assert(!exit_done&&tasks[1].files.record==&records[1]);resume_fd=1;assert(!pthread_cond_broadcast(&changed));assert(!pthread_mutex_unlock(&gate));
    assert(!pthread_join(reading,0)&&!pthread_join(exiting,0));assert(reading_result==-KERNEL_EBADF&&!borrows);
    scenario=0;tasks[0].proc_exiting=1;
    for(unsigned type=KCMP_VM;type<=KCMP_SYSVSEM;type++)if(type!=KCMP_IO&&type!=KCMP_SIGHAND)assert(query(10,11,type,0,0)==0);
    retired=1;assert(query(10,11,KCMP_VM,0,0)==-KERNEL_ESRCH);
}
int main(void)
{setup();semantics();sighand();departure();puts("kcmp: order, real FD slots, lazy identity, permission recheck and exit owners passed");return 0;}
