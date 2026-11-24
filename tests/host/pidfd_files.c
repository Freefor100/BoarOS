/* Real pidfd, task departure and FD table; host heap/OFD teardown are seams. */
#include "../../kernel/sched/process.c"
#include "../../kernel/sched/pidfd.c"
#include "../../fs/record_lock.h"
#include "../../fs/vfs_internal.h"
#include <kernel/time.h>
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

_Thread_local uintptr_t sync_test_irq;
static _Thread_local struct kernel_cpu cpu={.initialized=KERNEL_CPU_INITIALIZED};
static _Thread_local struct kernel_task *running;
static struct kernel_task caller,subject;
struct kernel_scheduler scheduler;
static struct kernel_heap heap;
static struct kernel_resource_limits limits;
static struct kernel_credentials credentials;
static struct kernel_exec_security *security=(void *)(uintptr_t)0x1234;
static unsigned allocations,service_entries,task_borrows,credential_refs,security_refs;
static unsigned service_depth;
static unsigned cleanup_failures,ofd_releases,fail_allocation;
static int deny_access,close_source,revoke_after_pin,detach_after_lookup;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static int pin_wait,at_pin,resume_pin,exit_reached,exit_finished;
static int64_t getfd_result;
static int pidfd;

void kernel_mqueue_close_fd(struct kernel_open_file_description *file,struct kernel_task *task)
{ (void)file;(void)task;assert(!"pidfd fixture does not own an mqueue"); }
void *sync_test_cpu(void){return &cpu;}
void *sync_test_current_task(void){return running;}
void kernel_cpu_flush_notifications(void){assert(!cpu.outbound);}
void kernel_assert_can_block(void){assert(!cpu.raw_locks && !cpu.preempt_depth);}
void kernel_preempt_disable(void){assert(!cpu.raw_locks);cpu.preempt_depth++;}
void kernel_preempt_enable(void){assert(!cpu.raw_locks && cpu.preempt_depth);cpu.preempt_depth--;}
void kernel_service_scope_begin(struct kernel_service_scope *s)
{ assert(!s->entered);s->entered=1;service_entries++;service_depth++; }
void kernel_service_scope_end(struct kernel_service_scope *s){if(s->entered){assert(service_depth);service_depth--;}s->entered=0;}
void kernel_assert_service_context(void){assert(service_depth);}
void scheduler_task_get_locked(struct kernel_task *task)
{
    assert(task==&subject && cpu.raw_locks);task_borrows++;
    if(detach_after_lookup)kernel_pid_detach(&task->identities[KERNEL_PID_TID]);
}
void scheduler_task_put(struct kernel_task *task)
{assert(task==&subject && task_borrows && !cpu.preempt_depth);task_borrows--;}
const struct kernel_credentials *kernel_credentials_current(void){return &credentials;}
const struct kernel_credentials *kernel_credentials_task_locked(const struct kernel_task *task)
{assert(task==&subject && cpu.raw_locks);return &credentials;}
const struct kernel_credentials *kernel_credentials_get(const struct kernel_credentials *c)
{assert(c==&credentials);credential_refs++;return c;}
void kernel_credentials_put(const struct kernel_credentials *c)
{if(c){assert(c==&credentials && credential_refs && !cpu.preempt_depth);credential_refs--;}}
struct kernel_exec_security *kernel_exec_security_get(struct kernel_exec_security *s)
{assert(s==security);security_refs++;return s;}
void kernel_exec_security_put(struct kernel_exec_security *s)
{if(s){assert(s==security && security_refs && !cpu.preempt_depth);security_refs--;}}
int kernel_credentials_may_access_locked(const struct kernel_task *from,const struct kernel_task *to,int fs)
{assert(from==running && to==&subject && !fs && cpu.raw_locks);return !deny_access;}
void kernel_rwlock_init(struct kernel_rwlock *lock,uint32_t rank,uintptr_t key)
{*lock=(struct kernel_rwlock){.rank=rank,.key=key};}
enum kernel_heap_status kernel_heap_allocate_zeroed(struct kernel_heap *h,size_t count,size_t size,void **out)
{assert(h==&heap && !cpu.raw_locks);if(fail_allocation && !--fail_allocation)return KERNEL_HEAP_STATUS_EMPTY;*out=calloc(count,size);assert(*out);__atomic_fetch_add(&allocations,1,__ATOMIC_RELAXED);return KERNEL_HEAP_STATUS_OK;}
enum kernel_heap_status kernel_heap_allocate_zeroed_nosleep(struct kernel_heap *h,
    size_t count, size_t size, void **out)
{ return kernel_heap_allocate_zeroed(h, count, size, out); }
void kernel_heap_release(struct kernel_heap *h,void *p)
{assert(h==&heap && p && !cpu.raw_locks);assert(__atomic_fetch_sub(&allocations,1,__ATOMIC_RELAXED));free(p);}
enum kernel_open_file_status kernel_open_file_acquire(struct kernel_open_file_description *file)
{assert(__atomic_fetch_add(&file->references,1,__ATOMIC_ACQUIRE));return KERNEL_OPEN_FILE_STATUS_OK;}
enum kernel_open_file_status kernel_open_file_release(struct kernel_open_file_description **owner)
{
    struct kernel_open_file_description *file=*owner;
    assert(file && __atomic_load_n(&file->references,__ATOMIC_ACQUIRE) && !cpu.raw_locks);
    if(__atomic_load_n(&file->references,__ATOMIC_ACQUIRE)==1 && cleanup_failures){cleanup_failures--;return KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED;}
    if(__atomic_fetch_sub(&file->references,1,__ATOMIC_ACQ_REL)==1){
        if(file->kind==KERNEL_OPEN_FILE_KIND_PIDFD)kernel_pidfd_close(file);
        kernel_credentials_put(file->credentials);kernel_heap_release(file->heap,file);ofd_releases++;
    }
    *owner=0;return KERNEL_OPEN_FILE_STATUS_OK;
}
uint32_t kernel_open_file_flags(const struct kernel_open_file_description *file){return file->open_flags;}
struct kernel_vfs_node *kernel_open_file_node(const struct kernel_open_file_description *file)
{(void)file;return 0;}
void kernel_record_lock_release(struct kernel_record_lock_state *domain,
    struct kernel_record_lock **head,struct kernel_heap *h)
{(void)domain;(void)head;(void)h;assert(!"unexpected record locks");}
struct kernel_record_lock_state *kernel_vfs_node_record_locks(struct kernel_vfs_node *node)
{(void)node;assert(!"unexpected VFS node");return 0;}
void kernel_task_identity_put(struct kernel_pid *id)
{KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);kernel_pid_put(id);}
struct kernel_pid *process_identity(const struct kernel_task *task,enum kernel_pid_role role)
{return task->identities[role].identity;}
struct kernel_pid_registry *process_pid_active(const struct kernel_task *task)
{ struct kernel_pid *id=process_identity(task,KERNEL_PID_TID);return id?id->registry:&scheduler.identities; }
struct kernel_pid *kernel_procfs_process_identity(const struct kernel_vfs_file *file)
{ return file->private_data; }
struct kernel_wait_record *kernel_wait_record_of(struct kernel_task *task){return &task->wait;}
struct kernel_wait_node *kernel_wait_default_node(struct kernel_task *task){return &task->default_wait_node;}
struct kernel_task *kernel_wait_current_task(void){return running;}
int kernel_wait_backend_initialized(void){return 1;}
int kernel_wait_backend_signal(struct kernel_task *task){(void)task;return 0;}
uint64_t kernel_wait_backend_time(void){return 1;}
void kernel_wait_backend_commit(struct kernel_task *task,uint64_t deadline,int interruptible)
{(void)task;(void)deadline;(void)interruptible;}
void kernel_wait_backend_notify(struct kernel_task *task,uint32_t reason){(void)task;(void)reason;}
void kernel_wait_backend_ready(struct kernel_task *task){(void)task;}
enum kernel_scheduler_status kernel_wait_backend_switch(struct kernel_task *task)
{
(void)task;assert(!"unexpected park");return KERNEL_SCHEDULER_STATUS_OK;}
void kernel_wait_backend_quiesce(void){sched_yield();}

static void signal_exit_reached(void)
{
    assert(!pthread_mutex_lock(&gate));exit_reached=1;
    assert(!pthread_cond_broadcast(&changed));assert(!pthread_mutex_unlock(&gate));
}
void __real_kernel_raw_lock_acquire(struct kernel_raw_lock *,struct kernel_raw_guard *);
void __wrap_kernel_raw_lock_acquire(struct kernel_raw_lock *lock,struct kernel_raw_guard *guard)
{
    /* The departure barrier must be reached while the reader still owns its
     * short access. An old unprotected exit instead reaches real release. */
    if(running==&subject && lock->rank==KERNEL_RAW_RANK_TASK_FILES){
        assert(__atomic_load_n(&lock->word,__ATOMIC_ACQUIRE)==1);
        signal_exit_reached();
    }
    __real_kernel_raw_lock_acquire(lock,guard);
}
enum kernel_files_status __real_kernel_files_release(struct kernel_files *);
enum kernel_files_status __wrap_kernel_files_release(struct kernel_files *files)
{
    enum kernel_files_status status=__real_kernel_files_release(files);
    if(pin_wait && files==&subject.files)signal_exit_reached();
    return status;
}
enum kernel_files_status __real_kernel_files_pin(struct kernel_files *,int64_t,
    struct kernel_open_file_description **,int64_t *);
static void *close_target_fd(void *argument)
{
    running=&subject;cpu.current=&subject;
    int64_t result;assert(!kernel_files_close(&subject.files,(intptr_t)argument,&result) && !result);
    return 0;
}
enum kernel_files_status __wrap_kernel_files_pin(struct kernel_files *files,int64_t fd,
    struct kernel_open_file_description **owner,int64_t *result)
{
    if(pin_wait && files==&subject.files){
        assert(cpu.raw_locks && cpu.raw_locks->lock==&subject.files_access);
        assert(!pthread_mutex_lock(&gate));at_pin=1;assert(!pthread_cond_broadcast(&changed));
        while(!resume_pin)assert(!pthread_cond_wait(&changed,&gate));
        assert(!pthread_mutex_unlock(&gate));
    }
    enum kernel_files_status status=__real_kernel_files_pin(files,fd,owner,result);
    if(files==&subject.files && !status && !*result){
        assert(files->record->references==1 && "getfd borrowed a complete FD table owner");
        if(close_source){
            pthread_t closer;assert(!pthread_create(&closer,0,close_target_fd,(void *)(intptr_t)fd));
            assert(!pthread_join(closer,0));
        }
        if(revoke_after_pin){KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);deny_access=1;}
    }
    return status;
}
static void *reader(void *unused)
{(void)unused;running=&caller;cpu.current=&caller;cpu.logical_id=1;getfd_result=kernel_pidfd_getfd(&caller,pidfd,7,0);return 0;}
static void *depart(void *unused)
{
    (void)unused;running=&subject;cpu.current=&subject;
    process_begin_exit(&subject,&(const struct kernel_thread_completion){0});
    assert(!kernel_files_release(&subject.files));
    assert(!pthread_mutex_lock(&gate));exit_finished=1;assert(!pthread_cond_broadcast(&changed));
    assert(!pthread_mutex_unlock(&gate));return 0;
}
static void exit_case(void)
{
    pthread_t reading,exiting;pin_wait=1;
    assert(!pthread_create(&reading,0,reader,0));
    assert(!pthread_mutex_lock(&gate));while(!at_pin)assert(!pthread_cond_wait(&changed,&gate));
    assert(!pthread_mutex_unlock(&gate));
    assert(!pthread_create(&exiting,0,depart,0));
    assert(!pthread_mutex_lock(&gate));while(!exit_reached)assert(!pthread_cond_wait(&changed,&gate));
    assert(subject.files.record && !exit_finished && "exit released real FD storage while getfd was between authorization and pin");
    resume_pin=1;assert(!pthread_cond_broadcast(&changed));assert(!pthread_mutex_unlock(&gate));
    assert(!pthread_join(reading,0) && !pthread_join(exiting,0));
    assert(getfd_result==-KERNEL_ESRCH && !subject.files.record && !task_borrows && !security_refs);
    pin_wait=0;
}
static void unshare_case(void)
{
    struct kernel_open_file_description *source=subject.files.record->slots[7].description;
    assert(!kernel_open_file_acquire(source));
    assert(!kernel_files_install_shared_acquired_at(&caller.files,1,0,&source) && !source);
    struct kernel_files peer={0};assert(!kernel_files_acquire(&peer,&caller.files));
    struct kernel_files_record *original=caller.files.record;
    struct kernel_open_file_description *file=original->slots[1].description;
    unsigned baseline=allocations,references=file->references;
    for(unsigned failure=1;failure<=3;failure++) {
        fail_allocation=failure;
        assert(kernel_task_close_range(&caller,1,1,2)==-KERNEL_ENOMEM && !fail_allocation);
        assert(caller.files.record==original && original->references==2 && !caller.files_change);
        assert(original->slots[1].description==file && file->references==references && allocations==baseline);
    }
    assert(!kernel_task_close_range(&caller,1,1,2|4));
    assert(caller.files.record!=original && !caller.files_change && original->references==1);
    assert(caller.files.record->slots[1].description==file && file->references==references+1);
    assert(caller.files.record->slots[1].flags==KERNEL_FILES_FD_CLOEXEC && !original->slots[1].flags);
    assert(!kernel_task_close_range(&caller,1,UINT32_MAX,0));
    assert(caller.files.record->statistics.current_open_fds==1 && original->slots[1].description==file && file->references==references);
    assert(!kernel_files_release(&caller.files));caller.files=(struct kernel_files){0};
    assert(!kernel_files_move(&caller.files,&peer) && allocations==baseline);
}
static void reserved_cloexec_case(void)
{
    struct kernel_files *files=&caller.files;
    struct kernel_open_file_description *source=subject.files.record->slots[7].description;
    unsigned baseline_cloexec=files->record->statistics.close_on_exec_fds;
    uint32_t first,second;int error;
    assert(!kernel_files_find_free_fd(files,&first,&error) && !error && first==1);
    assert(!kernel_files_close_range(files,first,first,1,&caller));
    assert(files->record->statistics.close_on_exec_fds==baseline_cloexec);
    assert(!kernel_open_file_acquire(source));struct kernel_open_file_description *owner=source;
    assert(!kernel_files_install_shared_acquired_at(files,first,0,&owner) && !owner);
    assert(files->record->slots[first].flags==KERNEL_FILES_FD_CLOEXEC);
    assert(files->record->statistics.current_open_fds==2 && files->record->statistics.close_on_exec_fds==baseline_cloexec+1);

    assert(!kernel_files_find_free_fd(files,&first,&error) && !error && first==2);
    struct kernel_files_fd_reservation cancelled={files,first};
    assert(!kernel_files_close_range(files,first,first,1,&caller));
    assert(files->record->statistics.close_on_exec_fds==baseline_cloexec+1);
    kernel_files_cancel_reservation(&cancelled);
    assert(!kernel_files_find_free_fd(files,&first,&error) && !error && first==2);
    assert(files->record->slots[first].flags==KERNEL_FILES_FD_RESERVED);
    assert(!kernel_files_find_free_fd(files,&second,&error) && !error && second==3);
    struct kernel_files_fd_reservation a={files,first},b={files,second};
    assert(!kernel_files_close_range(files,first,second,1,&caller));
    assert(!kernel_open_file_acquire(source) && !kernel_open_file_acquire(source));
    struct kernel_open_file_description *left=source,*right=source;
    assert(!kernel_files_install_pair_reserved(&a,&b,0,&left,&right));
    assert(!left && !right && !a.files && !b.files);
    assert(files->record->slots[first].flags==KERNEL_FILES_FD_CLOEXEC &&
        files->record->slots[second].flags==KERNEL_FILES_FD_CLOEXEC);
    assert(files->record->statistics.current_open_fds==4 && files->record->statistics.close_on_exec_fds==baseline_cloexec+3);
    assert(!kernel_files_close_range(files,1,3,1,&caller));
    assert(files->record->statistics.close_on_exec_fds==baseline_cloexec+3);
    assert(!kernel_files_close_range(files,1,3,0,&caller));
    assert(files->record->statistics.current_open_fds==1 && files->record->statistics.close_on_exec_fds==baseline_cloexec);
}
static struct kernel_pid *namespace_identity(struct kernel_pid_registry *space,struct kernel_task *task)
{
    struct kernel_pid *id=calloc(1,kernel_pid_storage_size(space));assert(id);
    assert(!kernel_pid_reserve_numbers(space,id,kernel_pid_storage_size(space),&(kernel_pid_t){1},1));
    assert(!kernel_pid_publish_reserved(id));
    kernel_pid_attach(&task->identities[KERNEL_PID_TID],id,KERNEL_PID_TID,task);
    kernel_pid_attach(&task->identities[KERNEL_PID_TGID],id,KERNEL_PID_TGID,task);
    task->tid=kernel_pid_number_in(id,&scheduler.identities);task->tid_owned=1;
    task->group_leader=task;return id;
}
static void signal_target_result(struct kernel_task *task,int fd,int expected,struct kernel_pid *identity)
{
    struct kernel_pid *id=NULL;unsigned scope=99;
    int result=kernel_pidfd_signal_target(task,fd,0,&id,&scope);
    assert(result==expected);
    if(!result){assert(id==identity && scope==1);kernel_task_identity_put(id);}
    else assert(!id && scope==99);
}
static void namespace_case(void)
{
    uint64_t bits[3][1];struct kernel_pid_allocator numbers[3]={0};
    struct kernel_pid_registry a={0},b={0};
    for(unsigned i=0;i<3;i++)assert(!kernel_pid_allocator_init(&numbers[i],bits[i],64));
    assert(!kernel_pid_registry_init(&scheduler.identities,&numbers[0],NULL));
    assert(!kernel_pid_registry_init(&a,&numbers[1],&scheduler.identities));
    assert(!kernel_pid_registry_init(&b,&numbers[2],&scheduler.identities));
    struct kernel_task sibling={.magic=KERNEL_THREAD_MAGIC,.arch.user_mode=1,.limits=&limits};
    struct kernel_task replacement={.magic=KERNEL_THREAD_MAGIC,.arch.user_mode=1,.limits=&limits};
    sibling.state=replacement.state=KERNEL_THREAD_STATE_RUNNING;
    sibling.fs=replacement.fs=caller.fs;
    struct kernel_pid *root=namespace_identity(&scheduler.identities,&caller);
    struct kernel_pid *left=namespace_identity(&a,&subject);
    struct kernel_pid *right=namespace_identity(&b,&sibling);
    assert(left->number==1 && right->number==1 && subject.tid!=sibling.tid);
    subject.exec_security=security;
    struct kernel_open_file_description *source=NULL;
    assert(!kernel_heap_allocate_zeroed(&heap,1,sizeof(*source),(void **)&source));
    source->heap=&heap;source->references=1;source->kind=KERNEL_OPEN_FILE_KIND_REGULAR;source->offset=91;
    assert(!kernel_files_install_new_owned_at(&subject.files,7,0,&source));
    int leftfd=kernel_pidfd_open(&caller,subject.tid,0),rootfd=kernel_pidfd_open(&caller,caller.tid,0);
    assert(leftfd==0 && rootfd==1);
    /* Actual fork-copy shares OFDs, but not namespace authority. */
    assert(!kernel_files_fork(&sibling.files,&caller.files));
    running=&sibling;cpu.current=&sibling;
    signal_target_result(&sibling,leftfd,-KERNEL_EINVAL,NULL);
    signal_target_result(&sibling,rootfd,-KERNEL_EINVAL,NULL);
    int local=kernel_pidfd_open(&sibling,1,0);assert(local==2);
    signal_target_result(&sibling,local,0,right);
    int copied=kernel_pidfd_getfd(&sibling,leftfd,7,0);assert(copied==3);
    assert(sibling.files.record->slots[copied].description==subject.files.record->slots[7].description);
    struct kernel_pid *invalid=NULL;unsigned invalid_scope=99;
    assert(kernel_pidfd_signal_target(&sibling,-1,0,&invalid,&invalid_scope)==-KERNEL_EBADF);
    assert(kernel_pidfd_signal_target(&sibling,-1,3,&invalid,&invalid_scope)==-KERNEL_EINVAL);
    assert(!invalid && invalid_scope==99);
    assert(kernel_pidfd_open(&sibling,subject.tid,0)==-KERNEL_ESRCH);
    /* Transfer the real root OFD through production pin/install operations. */
    running=&caller;cpu.current=&caller;
    struct kernel_open_file_description *transferred=NULL;int64_t error=0;
    assert(!kernel_files_pin(&caller.files,rootfd,&transferred,&error) && !error);
    assert(!kernel_files_install_shared_acquired_at(&subject.files,5,0,&transferred) && !transferred);
    running=&subject;cpu.current=&subject;
    signal_target_result(&subject,5,-KERNEL_EINVAL,NULL);
    running=&caller;cpu.current=&caller;
    signal_target_result(&caller,leftfd,0,left);
    /* Keep old FD owners across role removal and exact local-number reuse. */
    kernel_pid_detach(&subject.identities[KERNEL_PID_TID]);
    kernel_pid_detach(&subject.identities[KERNEL_PID_TGID]);
    struct kernel_pid *fresh=namespace_identity(&a,&replacement);
    assert(fresh->number==left->number && fresh->generation!=left->generation);
    assert(kernel_pidfd_number(caller.files.record->slots[leftfd].description)==-1);
    struct kernel_pid *retained=NULL;int nonblock;
    assert(!kernel_pidfd_wait_target(&sibling.files,leftfd,&retained,&nonblock) && retained==left);
    kernel_task_identity_put(retained);
    signal_target_result(&sibling,leftfd,-KERNEL_EINVAL,NULL);
    assert(!kernel_files_create(&replacement.files,&heap));
    running=&replacement;cpu.current=&replacement;
    int freshfd=kernel_pidfd_open(&replacement,1,0);assert(freshfd==0);
    signal_target_result(&replacement,freshfd,0,fresh);
    running=&caller;cpu.current=&caller;
    assert(!kernel_files_release(&replacement.files));
    assert(!kernel_files_release(&sibling.files));
    assert(!kernel_files_release(&subject.files));
    assert(!kernel_files_release(&caller.files));
    struct kernel_task *tasks[]={&caller,&sibling,&replacement};
    for(unsigned i=0;i<3;i++)for(unsigned role=0;role<2;role++)kernel_pid_detach(&tasks[i]->identities[role]);
    kernel_pid_put(root);kernel_pid_put(left);kernel_pid_put(right);kernel_pid_put(fresh);
    struct kernel_pid *dead;unsigned retired=0;
    while((dead=kernel_pid_take_retired(&scheduler.identities))){free(dead);retired++;}
    assert(retired==4 && !numbers[0].allocated && !numbers[1].allocated && !numbers[2].allocated);
    kernel_pid_registry_put(&a);kernel_pid_registry_put(&b);
    assert(scheduler.identities.references==1 && !kernel_pid_take_retired(&scheduler.identities));
    assert(kernel_pid_take_retired_namespace(&scheduler.identities));
    assert(kernel_pid_take_retired_namespace(&scheduler.identities));
    assert(!kernel_pid_take_retired_namespace(&scheduler.identities));
    assert(!allocations && !task_borrows && !credential_refs && !security_refs && !cpu.preempt_depth);
    puts("PASS pidfd namespace: real FD fork/transfer, sibling same number, ancestor rejection, owner exit/reuse and reference return");
}

int main(int argc,char **argv)
{
    assert(argc==2);running=&caller;cpu.current=&caller;cpu.logical_id=1;
    scheduler.initialized=KERNEL_SCHEDULER_INITIALIZED;
    kernel_wait_domain_init();kernel_wait_record_init(&caller.wait);kernel_wait_record_init(&subject.wait);
    kernel_raw_lock_init(&caller.files_access,KERNEL_RAW_RANK_TASK_FILES);
    kernel_raw_lock_init(&subject.files_access,KERNEL_RAW_RANK_TASK_FILES);
    caller.magic=KERNEL_THREAD_MAGIC;caller.state=KERNEL_THREAD_STATE_RUNNING;caller.arch.user_mode=1;
    caller.fs.state=KERNEL_FS_CONTEXT_LIVE;caller.fs.heap=&heap;caller.fs.record=(void *)(uintptr_t)1;caller.fs.namespace=(void *)(uintptr_t)2;
    caller.limits=&limits;limits.values[KERNEL_RLIMIT_NOFILE].current=1024;
    assert(!kernel_files_create(&caller.files,&heap) && !kernel_files_create(&subject.files,&heap));
    if(!strcmp(argv[1],"namespace")){namespace_case();return 0;}
    uint64_t bitmap[1];struct kernel_pid_allocator numbers={0};struct kernel_pid identity={0};
    assert(!kernel_pid_allocator_init(&numbers,bitmap,1));scheduler.identities.numbers=&numbers;scheduler.identities.next_generation=1;
    assert(!kernel_pid_publish(&scheduler.identities,&identity));
    kernel_pid_attach(&subject.identities[KERNEL_PID_TID],&identity,KERNEL_PID_TID,&subject);
    kernel_pid_attach(&subject.identities[KERNEL_PID_TGID],&identity,KERNEL_PID_TGID,&subject);
    subject.group_leader=subject.group_next=subject.group_previous=&subject;subject.group_members=1;subject.exec_security=security;
    struct kernel_open_file_description *source=0;
    assert(!kernel_heap_allocate_zeroed(&heap,1,sizeof(*source),(void **)&source));
    source->heap=&heap;source->references=1;source->kind=KERNEL_OPEN_FILE_KIND_REGULAR;source->offset=91;
    assert(!kernel_files_install_new_owned_at(&subject.files,7,0,&source));
    pidfd=(int)kernel_pidfd_open(&caller,identity.number,0);assert(pidfd==0);
    service_entries=0;
    if(!strcmp(argv[1],"getfd-local")){
        int received=(int)kernel_pidfd_getfd(&caller,pidfd,7,0);assert(received==1);
        assert(cpu.logical_id==1 && !service_entries && "getfd unnecessarily entered CPU0 service");
        assert(caller.files.record->slots[received].description==subject.files.record->slots[7].description);
        assert(caller.files.record->slots[received].flags==KERNEL_FILES_FD_CLOEXEC);
        assert(caller.files.record->slots[received].description->offset==91);
    }else if(!strcmp(argv[1],"open-local")){
        assert(kernel_pidfd_open(&caller,identity.number,0)==1);
        assert(!service_entries && "pidfd_open unnecessarily entered CPU0 service");
    }else if(!strcmp(argv[1],"nofile-open")){
        limits.values[KERNEL_RLIMIT_NOFILE].current=1;
        assert(kernel_pidfd_open(&caller,identity.number,1)==-KERNEL_EINVAL);
        assert(kernel_pidfd_open(&caller,2,0)==-KERNEL_ESRCH);
        assert(kernel_pidfd_open(&caller,identity.number,0)==-KERNEL_EMFILE && "pidfd_open ignored current NOFILE limit");
    }else if(!strcmp(argv[1],"nofile-getfd")){
        limits.values[KERNEL_RLIMIT_NOFILE].current=1;
        assert(kernel_pidfd_getfd(&caller,-1,7,1)==-KERNEL_EINVAL);
        assert(kernel_pidfd_getfd(&caller,-1,7,0)==-KERNEL_EBADF);
        assert(kernel_pidfd_getfd(&caller,pidfd,7,0)==-KERNEL_EMFILE && "getfd ignored current NOFILE limit");
        deny_access=1;assert(kernel_pidfd_getfd(&caller,pidfd,123,0)==-KERNEL_EPERM);deny_access=0;
        assert(kernel_pidfd_getfd(&caller,pidfd,123,0)==-KERNEL_EBADF);
    }else if(!strcmp(argv[1],"identity")){
        detach_after_lookup=1;
        assert(kernel_pidfd_getfd(&caller,pidfd,7,0)==-KERNEL_ESRCH);
    }else if(!strcmp(argv[1],"recheck")){
        revoke_after_pin=1;
        assert(kernel_pidfd_getfd(&caller,pidfd,7,0)==-KERNEL_EPERM);
        assert(caller.files.record->statistics.current_open_fds==1);
    }else if(!strcmp(argv[1],"cleanup-owner")){
        limits.values[KERNEL_RLIMIT_NOFILE].current=1;close_source=1;cleanup_failures=1;
        assert(kernel_pidfd_getfd(&caller,pidfd,7,0)==-KERNEL_EMFILE);
        assert(!cleanup_failures && caller.files.record->cleanup_files);
        assert(!subject.files.record->statistics.current_open_fds);
        assert(!kernel_files_drain_file_cleanup(&caller.files) && !caller.files.record->cleanup_files);
    }else if(!strcmp(argv[1],"exit"))exit_case();
    else if(!strcmp(argv[1],"reserved-cloexec"))reserved_cloexec_case();
    else if(!strcmp(argv[1],"close-range-unshare"))unshare_case();
    else assert(!"unknown case");
    if(kernel_files_is_live(&subject.files))assert(!kernel_files_release(&subject.files));
    assert(!kernel_files_release(&caller.files));
    if(subject.identities[KERNEL_PID_TID].identity)kernel_pid_detach(&subject.identities[KERNEL_PID_TID]);
    kernel_pid_detach(&subject.identities[KERNEL_PID_TGID]);kernel_pid_put(&identity);
    assert(!allocations && !task_borrows && !credential_refs && !security_refs && !cpu.preempt_depth);
    assert(kernel_pid_take_retired(&scheduler.identities)==&identity && !numbers.allocated);
    printf("PASS pidfd real files %s: OFD/table/PID/security owners returned\n",argv[1]);
}

#if BOAROS_MAX_CPUS > 1
enum kernel_scheduler_status kernel_wait_backend_switch_locked(struct kernel_task *task,
    struct kernel_raw_guard *handoff)
{
    assert(handoff && handoff->lock == &kernel_wait_domain);
    kernel_raw_lock_release(handoff);
    return kernel_wait_backend_switch(task);
}
#endif

enum kernel_time_status kernel_time_deadline_from_realtime(uint64_t ns,uint64_t *deadline)
{(void)ns;(void)deadline;__builtin_trap();}

void process_pid_reaper_disable_locked(struct kernel_task *task)
{ assert(!process_pid_active(task)->parent); }
int kernel_wait_backend_lock_local(const struct kernel_wait_token *token,struct kernel_raw_guard *guard,int domain)
{(void)token;(void)guard;(void)domain;__builtin_trap();}
void kernel_run_set_acquire(struct kernel_run_set *set){(void)set;__builtin_trap();}
void kernel_run_set_release(struct kernel_run_set *set){(void)set;__builtin_trap();}
