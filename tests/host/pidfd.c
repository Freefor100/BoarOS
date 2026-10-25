/* Real PID/observer/wait implementation; machine switch and heap are seams. */
#include "../../kernel/sched/pidfd.c"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
_Thread_local uintptr_t sync_test_irq;
static _Thread_local struct kernel_cpu cpu={.initialized=KERNEL_CPU_INITIALIZED};
static struct kernel_task waiter,subject;
static struct kernel_heap heap;
static unsigned allocations,callbacks;
static struct pidfd_file *descriptor;
static struct kernel_open_file_description transferred;
static struct kernel_credentials test_credentials;
static struct kernel_exec_security *test_security=(void *)(uintptr_t)0x1234;
static unsigned task_borrows,credential_refs,security_refs,pinned,installed,ofd_released;
static int deny_access,pin_error,replace_files;
static pthread_barrier_t entered,released;
void *sync_test_cpu(void){return &cpu;}
void *sync_test_current_task(void){return &waiter;}
struct kernel_task *kernel_task_current(void){return &waiter;}
void kernel_cpu_flush_notifications(void){assert(!cpu.outbound);}
void kernel_assert_can_block(void){assert(!cpu.raw_locks);}
void kernel_preempt_disable(void){assert(!cpu.raw_locks);cpu.preempt_depth++;}
void kernel_preempt_enable(void){assert(!cpu.raw_locks && cpu.preempt_depth);cpu.preempt_depth--;}
void scheduler_task_get_locked(struct kernel_task *task)
{assert(task==&subject && cpu.raw_locks);task_borrows++;}
void scheduler_task_put(struct kernel_task *task)
{assert(task==&subject && task_borrows && !cpu.preempt_depth);task_borrows--;}
const struct kernel_credentials *kernel_credentials_task_locked(const struct kernel_task *task)
{assert(task==&subject && cpu.raw_locks);return &test_credentials;}
const struct kernel_credentials *kernel_credentials_get(const struct kernel_credentials *c)
{assert(c==&test_credentials);credential_refs++;return c;}
void kernel_credentials_put(const struct kernel_credentials *c)
{assert(!cpu.preempt_depth);if(c){assert(c==&test_credentials && credential_refs);credential_refs--;}}
struct kernel_exec_security *kernel_exec_security_get(struct kernel_exec_security *s)
{assert(s==test_security);security_refs++;return s;}
void kernel_exec_security_put(struct kernel_exec_security *s)
{assert(!cpu.preempt_depth);if(s){assert(s==test_security && security_refs);security_refs--;}}
int kernel_credentials_may_access_locked(const struct kernel_task *caller,const struct kernel_task *target,int fs)
{assert(caller==&waiter && target==&subject && !fs && cpu.raw_locks && cpu.preempt_depth>=2);return !deny_access;}
int kernel_files_is_live(const struct kernel_files *f){return f->record!=0;}
struct kernel_open_file_description *kernel_files_hold_fd(struct kernel_files *files,int64_t fd,struct kernel_files_pin_guard *guard)
{assert(files==&waiter.files && fd==9 && descriptor && !cpu.preempt_depth);(void)guard;return &descriptor->file;}
void kernel_files_pin_guard_release(struct kernel_files_pin_guard *guard){assert(!guard->description);}
uint32_t kernel_open_file_flags(const struct kernel_open_file_description *f){return f->open_flags;}
enum kernel_files_status kernel_files_pin(struct kernel_files *files,int64_t fd,struct kernel_open_file_description **owner,int64_t *result)
{
    assert(files==&subject.files && fd==7 && cpu.raw_locks &&
        cpu.raw_locks->lock==&subject.files_access && cpu.preempt_depth==1);
    pinned++;*result=pin_error;
    if(!pin_error)*owner=&transferred;
    if(replace_files)files->record=(void *)(uintptr_t)0x3456;
    return KERNEL_FILES_STATUS_OK;
}
enum kernel_task_status kernel_task_files_borrow(struct kernel_task *task,struct kernel_files **files)
{assert(task==&waiter && !cpu.raw_locks && !cpu.preempt_depth);*files=&task->files;return KERNEL_TASK_STATUS_OK;}
enum kernel_files_status kernel_files_find_free_fd(struct kernel_files *files,uint32_t *fd,int *error)
{assert(files==&waiter.files && !cpu.preempt_depth);*fd=10;*error=0;return KERNEL_FILES_STATUS_OK;}
enum kernel_files_status kernel_files_install_shared_acquired_at(struct kernel_files *files,uint32_t fd,uint32_t flags,struct kernel_open_file_description **owner)
{assert(files==&waiter.files && fd==10 && flags==KERNEL_FILES_FD_CLOEXEC && *owner==&transferred && !cpu.preempt_depth);*owner=0;installed++;return KERNEL_FILES_STATUS_OK;}
void kernel_files_cancel_reservation(struct kernel_files_fd_reservation *r){assert(!r->files);}
enum kernel_open_file_status kernel_open_file_release(struct kernel_open_file_description **owner)
{assert(*owner==&transferred && !cpu.preempt_depth);*owner=0;ofd_released++;return KERNEL_OPEN_FILE_STATUS_OK;}
void kernel_files_queue_description(struct kernel_files *files,struct kernel_open_file_description *file)
{(void)files;(void)file;assert(!"unexpected cleanup retry");}
struct kernel_pid *process_identity(const struct kernel_task *task,enum kernel_pid_role role)
{return task->identities[role].identity;}
void kernel_task_identity_put(struct kernel_pid *id)
{KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);kernel_pid_put(id);}
void kernel_heap_release(struct kernel_heap *h,void *p)
{assert(h==&heap && p && allocations && !cpu.raw_locks);allocations--;free(p);}
struct kernel_wait_record *kernel_wait_record_of(struct kernel_task *task){return &task->wait;}
struct kernel_wait_node *kernel_wait_default_node(struct kernel_task *task){return &task->default_wait_node;}
struct kernel_task *kernel_wait_current_task(void){return &waiter;}
int kernel_wait_backend_initialized(void){return 1;}
int kernel_wait_backend_signal(struct kernel_task *task){(void)task;return 0;}
uint64_t kernel_wait_backend_time(void){return 1;}
void kernel_wait_backend_commit(struct kernel_task *task,uint64_t deadline,int interruptible)
{(void)task;(void)deadline;(void)interruptible;}
void kernel_wait_backend_notify(struct kernel_task *task,uint32_t reason){(void)task;(void)reason;}
void kernel_wait_backend_ready(struct kernel_task *task){(void)task;}
enum kernel_scheduler_status kernel_wait_backend_switch(struct kernel_task *task)
{(void)task;assert(!"early notification must prevent switch");return KERNEL_SCHEDULER_STATUS_OK;}
void kernel_wait_backend_quiesce(void){sched_yield();}
static void barrier(pthread_barrier_t *b)
{int result=pthread_barrier_wait(b);assert(!result || result==PTHREAD_BARRIER_SERIAL_THREAD);}
static void callback(struct kernel_wait_node *node,uint32_t reason)
{
    assert(node->context==(void *)(uintptr_t)0x1234 && reason==KERNEL_WAIT_WOKEN && !cpu.raw_locks);
    callbacks++;barrier(&entered);barrier(&released);
}
static void *notify(void *arg){notify_identity(arg);return 0;}
int main(void)
{
    kernel_wait_domain_init();kernel_wait_record_init(&waiter.wait);waiter.wait.on_cpu=1;
    kernel_raw_lock_init(&subject.files_access,KERNEL_RAW_RANK_TASK_FILES);
    kernel_wait_node_init(&waiter.default_wait_node,&waiter);
    uint64_t bitmap[1];struct kernel_pid_allocator numbers={0};
    struct kernel_pid_registry registry={.numbers=&numbers,.next_generation=1};
    struct kernel_pid id={0},replacement={0};
    assert(!kernel_pid_allocator_init(&numbers,bitmap,1));
    assert(!kernel_pid_publish(&registry,&id));
    kernel_pid_attach(&subject.identities[KERNEL_PID_TID],&id,KERNEL_PID_TID,&subject);
    kernel_pid_attach(&subject.identities[KERNEL_PID_TGID],&id,KERNEL_PID_TGID,&subject);
    subject.group_leader=&subject;subject.group_members=2;subject.state=KERNEL_THREAD_STATE_GROUP_DEAD;
    struct pidfd_observer *o=calloc(1,sizeof(*o));assert(o);allocations++;
    o->identity=&id;o->heap=&heap;o->references=1;kernel_pid_get(&id);kernel_wait_queue_init(&o->changed);
    observers[observer_bucket(&id)]=o;
    struct pidfd_file file={.observer=o};struct kernel_wait_queue *queue=0;
    file.file.kind=KERNEL_OPEN_FILE_KIND_PIDFD;descriptor=&file;
    subject.exec_security=test_security;subject.files.record=(void *)(uintptr_t)0x2345;
    assert(kernel_pidfd_getfd(&waiter,9,7,0)==10 && pinned==1 && installed==1);
    pin_error=-KERNEL_EBADF;
    assert(kernel_pidfd_getfd(&waiter,9,7,0)==-KERNEL_EBADF && pinned==2);
    pin_error=0;deny_access=1;
    assert(kernel_pidfd_getfd(&waiter,9,7,0)==-KERNEL_EPERM && pinned==2);
    deny_access=0;replace_files=1;
    assert(kernel_pidfd_getfd(&waiter,9,7,0)==-KERNEL_ESRCH && pinned==3 && ofd_released==1);
    replace_files=0;
    assert(!cpu.preempt_depth && !task_borrows && !credential_refs && !security_refs);
    assert(!kernel_pidfd_poll(&file.file,&queue) && queue==&o->changed);
    subject.group_members=1;subject.state=KERNEL_THREAD_STATE_ZOMBIE;
    notify_identity(&id); /* State is visible even when notification preceded registration. */
    assert(kernel_pidfd_poll(&file.file,0)==(KERNEL_POLLIN|KERNEL_POLLRDNORM));
    struct kernel_wait_token token={0};assert(!kernel_wait_prepare(queue,0,1,&token));
    notify_identity(&id);enum kernel_wait_wake_reason reason;
    assert(!kernel_wait_park(&token,&reason) && reason==KERNEL_WAIT_WOKEN && !kernel_wait_finish(&token));
    assert(!waiter.wait.borrows && !queue->registrations && !queue->borrows);
    kernel_pid_detach(&subject.identities[KERNEL_PID_TID]);
    kernel_pid_detach(&subject.identities[KERNEL_PID_TGID]);kernel_pid_put(&id);
    assert(!numbers.allocated && id.published==2 && kernel_pidfd_number(&file.file)==-1);
    assert(kernel_pidfd_getfd(&waiter,9,7,0)==-KERNEL_ESRCH && !cpu.preempt_depth);
    assert(kernel_pidfd_poll(&file.file,0)==(KERNEL_POLLIN|KERNEL_POLLRDNORM|KERNEL_POLLHUP));
    assert(!kernel_pid_publish(&registry,&replacement) && replacement.number==id.number);
    struct kernel_wait_node node;kernel_wait_node_init_callback(&node,callback,(void *)(uintptr_t)0x1234);
    kernel_wait_queue_add(queue,&node);
    assert(!pthread_barrier_init(&entered,0,2) && !pthread_barrier_init(&released,0,2));
    pthread_t thread;assert(!pthread_create(&thread,0,notify,&id));barrier(&entered);
    kernel_wait_queue_remove(&node);
    kernel_pidfd_close(&file.file);assert(allocations==1 && id.references==1);
    barrier(&released);assert(!pthread_join(thread,0));
    assert(callbacks==1 && !allocations && !node.references && !node.borrow_owner);
    assert(!observers[observer_bucket(&id)] && kernel_pid_take_retired(&registry)==&id);
    assert(kernel_pid_find(&registry,1)==&replacement);kernel_pid_put(&replacement);
    assert(kernel_pid_take_retired(&registry)==&replacement && !numbers.allocated);
    puts("PASS pidfd protected getfd pin/recheck, last-thread/readiness, early notification, detached reuse, close during callback and final observer/queue/PID owners");
}
