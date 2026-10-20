#include <kernel/files.h>
#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/mm_backend.h>
#include <kernel/page_cache.h>
#include <kernel/tmpfs.h>
#include <kernel/task.h>
#include <kernel/uaccess.h>
#include <kernel/block.h>
#include <kernel/cpu.h>
#include <kernel/errno.h>
#include <kernel/futex.h>
#include <ext4_bcache.h>
#include "../../fs/lwext4_port.h"
#include <kernel/file_mapping.h>
#include <kernel/irq.h>
#include "../../fs/files/private.h"
#include "../../fs/open_file_internal.h"
#include "../../fs/vfs_internal.h"
#include "../../fs/vfs_objects.h"
#include "../../kernel/sched/private.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

void mm_test_attach(unsigned);
void mm_test_bind(struct kernel_mm *);
void mm_test_bind_files(struct kernel_files *);
void mm_test_finish(void);
void mm_test_activate_task(void);
extern _Thread_local uint64_t mm_test_context;
#ifdef LOOP_IO_CASES
static unsigned char arena[64*1024*1024] __attribute__((aligned(16384)));
#else
static unsigned char arena[32*1024*1024] __attribute__((aligned(16384)));
#endif
static unsigned char backend[16*1024*1024] __attribute__((aligned(16384)));
static void *access_page(uint64_t address) { return (void *)(uintptr_t)address; }
static int page_address(const void *page,uint64_t *address) { *address=(uintptr_t)page; return 1; }
static struct kernel_mm mm;
static struct kernel_files files;
static struct kernel_fs_context fs;
static struct kernel_open_file_description *control;
static int image;
static uint64_t context;
static uint64_t shared_mapping;
static struct kernel_files views[4];
static struct kernel_open_file_description *controls[4];
static unsigned pause_access,access_entered,resume_access,stop_done;
static unsigned pause_local_access,local_access_entered,resume_local_access;
#define PACKETS 32U
struct packet { uint32_t actor,sequence; unsigned char payload[24]; };
static pthread_barrier_t launch;
#define FUTEX_ADDRESS UINT64_C(0x600000)
static _Thread_local unsigned futex_role;
static _Thread_local unsigned completed_buffer_wait;
static unsigned futex_registered,futex_edit_ready,futex_word_read;
#ifdef TCP_IO_CASES
static void tcp_wait_preparing(struct kernel_wait_queue *);
static void tcp_wait_prepared(struct kernel_wait_queue *,uint64_t);
static void tcp_wait_parking(const struct kernel_wait_token *);
static void tcp_wait_finishing(const struct kernel_wait_token *);
static void tcp_wait_finished(void);
static void tcp_wait_locking(struct kernel_rwlock *);
static int tcp_batch_unpinning(struct kernel_mm_user_pin *);
static void tcp_batch_unpinned(struct kernel_mm_user_pin *);
#endif
enum kernel_scheduler_status __real_kernel_wait_prepare(struct kernel_wait_queue *,uint64_t,int,struct kernel_wait_token *);
enum kernel_scheduler_status __wrap_kernel_wait_prepare(struct kernel_wait_queue *queue,
    uint64_t deadline,int interruptible,struct kernel_wait_token *token)
{
    assert(!completed_buffer_wait && "completed ext4 load registered an unconditional wait");
#ifdef TCP_IO_CASES
    tcp_wait_preparing(queue);
#endif
    enum kernel_scheduler_status status=__real_kernel_wait_prepare(queue,deadline,interruptible,token);
#ifdef TCP_IO_CASES
    if (!status) tcp_wait_prepared(queue,deadline);
#endif
    if (!status && futex_role==1 && kernel_task_current()->futex_bitset) {
        assert(!kernel_cpu_current()->raw_locks);
        __atomic_store_n(&futex_registered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&futex_edit_ready,__ATOMIC_ACQUIRE)) sched_yield();
    } else if (!status && futex_role==2) {
        /* 实际MM编辑已关闭新pin，并准备等待旧片段归还。 */
        __atomic_store_n(&futex_edit_ready,1,__ATOMIC_RELEASE);
    }
    return status;
}
#ifdef TCP_IO_CASES
enum kernel_scheduler_status __real_kernel_wait_park(const struct kernel_wait_token *,enum kernel_wait_wake_reason *);
enum kernel_scheduler_status __wrap_kernel_wait_park(const struct kernel_wait_token *token,
    enum kernel_wait_wake_reason *reason)
{
    tcp_wait_parking(token);
    return __real_kernel_wait_park(token,reason);
}
enum kernel_scheduler_status __real_kernel_wait_finish(struct kernel_wait_token *);
enum kernel_scheduler_status __wrap_kernel_wait_finish(struct kernel_wait_token *token)
{
    tcp_wait_finishing(token);
    enum kernel_scheduler_status status=__real_kernel_wait_finish(token);
    if (!status) tcp_wait_finished();
    return status;
}
void __real_kernel_rwlock_write(struct kernel_rwlock *,struct kernel_lock_guard *);
void __wrap_kernel_rwlock_write(struct kernel_rwlock *lock,struct kernel_lock_guard *guard)
{
    tcp_wait_locking(lock);
    __real_kernel_rwlock_write(lock,guard);
}
#endif
void __real_riscv_smp_sync_user(uint32_t,uint64_t,uint64_t,int);
void __wrap_riscv_smp_sync_user(uint32_t targets,uint64_t address,uint64_t size,int instructions)
{
    __real_riscv_smp_sync_user(targets,address,size,instructions);
    if (futex_role==2) {
        assert(!kernel_cpu_current()->raw_locks);
        __atomic_store_n(&futex_edit_ready,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&futex_word_read,__ATOMIC_ACQUIRE)) sched_yield();
    }
}
void __real_kernel_mm_unpin_user_page(struct kernel_mm_user_pin *);
void __wrap_kernel_mm_unpin_user_page(struct kernel_mm_user_pin *pin)
{
#ifdef TCP_IO_CASES
    int batch=tcp_batch_unpinning(pin);
#endif
    __real_kernel_mm_unpin_user_page(pin);
#ifdef TCP_IO_CASES
    if(batch) tcp_batch_unpinned(pin);
#endif
    if (futex_role==1 && __atomic_load_n(&futex_registered,__ATOMIC_ACQUIRE))
        __atomic_store_n(&futex_word_read,1,__ATOMIC_RELEASE);
}
static void *futex_waiter(void *unused)
{
    (void)unused;mm_test_attach(1);mm_test_bind(&mm);mm_test_context=context;
    kernel_cpu_current()->logical_id=0;futex_role=1;
    uintptr_t irq=arch_interrupt_save();enum kernel_scheduler_status status;
    assert(!kernel_futex(kernel_task_current(),FUTEX_ADDRESS,128,1,0,0,0,&status) && !status);
    arch_interrupt_restore(irq);
    assert(!kernel_task_current()->futex_bitset && kernel_task_current()->wait.phase==KERNEL_WAIT_FINISHED);
    mm_test_finish();return NULL;
}
static void *futex_editor(void *unused)
{
    (void)unused;mm_test_attach(2);mm_test_bind(&mm);mm_test_context=context;
    kernel_cpu_current()->logical_id=1;futex_role=2;
    while (!__atomic_load_n(&futex_registered,__ATOMIC_ACQUIRE)) sched_yield();
    assert(!kernel_mm_mprotect(&mm,FUTEX_ADDRESS,BOAROS_PAGE_SIZE,KERNEL_MM_READ));
    futex_role=0;
    uintptr_t irq=arch_interrupt_save();enum kernel_scheduler_status status;
    assert(kernel_futex(kernel_task_current(),FUTEX_ADDRESS,129,1,0,0,0,&status)==1 && !status);
    arch_interrupt_restore(irq);
    mm_test_finish();return NULL;
}
static void futex_pin_case(void)
{
    uint64_t address;uint32_t word=1;size_t copied;
    assert(!kernel_mm_mmap_anonymous(&mm,FUTEX_ADDRESS,BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE,
        KERNEL_MM_MAP_FIXED_NOREPLACE,&address) && address==FUTEX_ADDRESS);
    assert(!kernel_copy_to_user(&mm,address,&word,sizeof(word),&copied) && copied==sizeof(word));
    futex_registered=futex_edit_ready=futex_word_read=0;
    pthread_t waiter,editor;
    assert(!pthread_create(&editor,NULL,futex_editor,NULL) && !pthread_create(&waiter,NULL,futex_waiter,NULL));
    assert(!pthread_join(waiter,NULL) && !pthread_join(editor,NULL));
    assert(futex_registered && futex_word_read && futex_edit_ready);
    assert(!kernel_mm_munmap(&mm,address,BOAROS_PAGE_SIZE));
    struct kernel_mm_concurrency_state state;kernel_mm_get_concurrency_state(&mm,&state);
    assert(!state.pins && !state.write_pins && !state.pending_retirement);
    puts("PASS futex registration vs actual mprotect: pin survives final comparison, unpin precedes park, exact wake and final owners");
}
static unsigned lock_set_entered,lock_close_done;
static _Thread_local int pause_lock_set;
int __real_kernel_record_lock_set(struct kernel_record_lock_state *,struct kernel_record_lock **,
    const void *,uint8_t,int32_t,int64_t,int64_t,int16_t,struct kernel_heap *);
int __wrap_kernel_record_lock_set(struct kernel_record_lock_state *state,struct kernel_record_lock **head,
    const void *owner,uint8_t kind,int32_t pid,int64_t first,int64_t last,int16_t type,struct kernel_heap *heap)
{
    if (pause_lock_set) {
        pause_lock_set=0;
        __atomic_store_n(&lock_set_entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&lock_close_done,__ATOMIC_ACQUIRE)) sched_yield();
    }
    return __real_kernel_record_lock_set(state,head,owner,kind,pid,first,last,type,heap);
}
static void *close_before_lock_publish(void *unused)
{
    (void)unused;mm_test_attach(1);mm_test_bind(&mm);mm_test_bind_files(&files);mm_test_context=context;
    while (!__atomic_load_n(&lock_set_entered,__ATOMIC_ACQUIRE)) sched_yield();
    int64_t result;
    assert(!kernel_files_close(&files,0,&result) && !result);
    __atomic_store_n(&lock_close_done,1,__ATOMIC_RELEASE);
    mm_test_finish();return NULL;
}
static void close_lock_case(struct kernel_heap *heap,struct kernel_vfs_mount *mount)
{
    mm_test_attach(0);mm_test_activate_task();mm_test_bind(&mm);mm_test_bind_files(&files);mm_test_context=context;
    files=(struct kernel_files){0};assert(!kernel_files_create(&files,heap));
    int error;
    assert(!kernel_open_file_create_mode(heap,mount,"/close-before-first-lock",0600,&control,&error) && !error);
    control->open_flags=2;
    assert(!kernel_vfs_file_acquire_write(&control->file));
    struct kernel_open_file_description *slot=control;
    assert(!kernel_open_file_acquire(slot) && !kernel_files_install_new_owned_at(&files,0,0,&slot));
    struct { int16_t type,whence; int32_t pad; int64_t start,length; int32_t pid,reserved; } flock={.type=1,.length=1};
    size_t copied;
    assert(!kernel_copy_to_user(&mm,0x200000,&flock,sizeof(flock),&copied) && copied==sizeof(flock));
    struct kernel_task *task=kernel_task_current();
    task->arch.user_mode=1;task->tid=1;task->tid_owned=1;task->group_leader=task;task->group_members=1;
    lock_set_entered=lock_close_done=0;pause_lock_set=1;
    pthread_t closer;assert(!pthread_create(&closer,NULL,close_before_lock_publish,NULL));
    int64_t result;
    assert(!kernel_files_fcntl_lock(&files,&mm,task,0,KERNEL_FILES_F_SETLK,0x200000,&result) && result==-KERNEL_EBADF);
    assert(!pthread_join(closer,NULL));
    task->arch.user_mode=0;
    assert(!files.record->record_locks && !kernel_vfs_node_record_locks(kernel_open_file_node(control))->root);
    mm_test_bind(NULL);mm_test_bind_files(NULL);
    assert(!kernel_files_release(&files) && !kernel_open_file_release(&control));
    puts("PASS first POSIX lock races actual close before publication: EBADF, no inode lock or owner left");
}
static void worker_lock_close_case(struct kernel_heap *heap,struct kernel_vfs_mount *mount,
    struct kernel_page_cache *cache)
{
    struct kernel_open_file_description *locked=NULL,*observer=NULL;
    int error;
    assert(!kernel_open_file_create_mode(heap,mount,"/worker-lock-close",0600,&locked,&error) && !error);
    assert(!kernel_open_file_create(heap,mount,"/worker-lock-close",&observer,&error) && !error);
    struct kernel_record_lock_state *state=kernel_vfs_node_record_locks(kernel_open_file_node(observer));
    {
        KERNEL_IRQ_SCOPE(irq);
        assert(!kernel_record_lock_set(state,&locked->record_locks,locked,1,1,0,7,1,heap));
    }
    assert(state->root && arch_interrupt_is_enabled());
    assert(!kernel_page_cache_start_worker(cache));
    mm_test_attach(0);mm_test_activate_task();mm_test_bind(&mm);mm_test_context=context;
    kernel_cpu_current()->logical_id=1;
    struct kernel_page_cache_concurrency_state before,after;
    kernel_page_cache_get_concurrency_state(cache,&before);
    assert(!kernel_open_file_release(&locked) && !locked && arch_interrupt_is_enabled());
    kernel_page_cache_get_concurrency_state(cache,&after);
    assert(after.executions_completed==before.executions_completed+1 && !state->root);
    assert(!kernel_open_file_release(&observer) && !observer);
    mm_test_finish();mm_test_attach(0);mm_test_activate_task();
    kernel_page_cache_stop_worker(cache);
    puts("PASS final OFD lock release through actual CPU0 worker: short IRQ protection, caller IRQ restored, inode owners returned");
}
static unsigned read_seen[4][PACKETS];
static void *parallel_reader(void *argument)
{
    unsigned id=(unsigned)(uintptr_t)argument;
    mm_test_attach(id+1);kernel_cpu_current()->logical_id=id&1;
    mm_test_bind(&mm);mm_test_bind_files(&views[id]);mm_test_context=context;
    int started=pthread_barrier_wait(&launch);
    assert(!started || started==PTHREAD_BARRIER_SERIAL_THREAD);
    uint64_t buffer=0x200000+id*BOAROS_PAGE_SIZE;
    for(unsigned n=0;n<PACKETS;n++) {
        int64_t result;size_t copied;
        if(n&1) {
            struct kernel_uaccess_iovec iov[2]={{buffer,13},{buffer+13,sizeof(struct packet)-13}};
            assert(!kernel_copy_to_user(&mm,buffer+128,iov,sizeof(iov),&copied) && copied==sizeof(iov));
            assert(!kernel_files_readv(&views[id],&mm,0,buffer+128,2,&result));
        } else assert(!kernel_files_read(&views[id],&mm,0,buffer,sizeof(struct packet),&result));
        struct packet packet;
        assert(result==sizeof(packet) && !kernel_copy_from_user(&mm,&packet,buffer,sizeof(packet),&copied) && copied==sizeof(packet));
        assert(packet.actor<4 && packet.sequence<PACKETS && !__atomic_fetch_add(&read_seen[packet.actor][packet.sequence],1,__ATOMIC_RELAXED));
        for(unsigned j=0;j<sizeof(packet.payload);j++)assert(packet.payload[j]==0x40+packet.actor);
    }
    assert(!kernel_files_release(&views[id]));mm_test_finish();return 0;
}
static void *parallel_writer(void *argument)
{
    unsigned id=(unsigned)(uintptr_t)argument;
    mm_test_attach(id+1); kernel_cpu_current()->logical_id=id&1;
    mm_test_bind(&mm); mm_test_bind_files(&views[id]); mm_test_context=context;
    int started=pthread_barrier_wait(&launch);
    assert(!started || started==PTHREAD_BARRIER_SERIAL_THREAD);
    for (unsigned sequence=0;sequence<PACKETS;sequence++) {
        struct packet packet={.actor=id,.sequence=sequence};
        memset(packet.payload,(int)(0x40+id),sizeof(packet.payload));
        size_t copied; int64_t result;
        assert(!kernel_copy_to_user(&mm,0x200000+id*BOAROS_PAGE_SIZE,&packet,sizeof(packet),&copied) && copied==sizeof(packet));
        assert(!kernel_files_write(&views[id],&mm,id,0x200000+id*BOAROS_PAGE_SIZE,sizeof(packet),&result) && result==sizeof(packet));
    }
    assert(!kernel_files_release(&views[id])); mm_test_finish(); return 0;
}
static void parallel_case(struct kernel_heap *heap,struct kernel_vfs_mount *mount,
    struct kernel_page_cache *cache,unsigned actors,int append)
{
    char name[32]; snprintf(name,sizeof(name),"/parallel-%u-%d",actors,append);
    files=(struct kernel_files){0}; assert(!kernel_files_create(&files,heap));
    int error; int64_t result;
    for (unsigned i=0;i<actors;i++) {
        if (!i) assert(!kernel_open_file_create_mode(heap,mount,name,0600,&controls[i],&error) && !error);
        else if (append) assert(!kernel_open_file_create(heap,mount,name,&controls[i],&error) && !error);
        else { controls[i]=controls[0]; assert(!kernel_open_file_acquire(controls[i])); }
        controls[i]->open_flags=2|(append?KERNEL_FILES_O_APPEND:0);
        if (append || !i) assert(!kernel_vfs_file_acquire_write(&controls[i]->file));
        struct kernel_open_file_description *slot=controls[i];
        assert(!kernel_open_file_acquire(slot) && !kernel_files_install_new_owned_at(&files,i,0,&slot));
    }
    if (cache) assert(!kernel_page_cache_start_worker(cache));
    assert(!kernel_files_check_task_domain(&files,&mm));
    assert(!pthread_barrier_init(&launch,0,actors)); pthread_t threads[4];
    for (unsigned i=0;i<actors;i++) {
        views[i]=(struct kernel_files){0}; assert(!kernel_files_acquire(&views[i],&files));
        assert(!pthread_create(&threads[i],0,parallel_writer,(void *)(uintptr_t)i));
    }
    for (unsigned i=0;i<actors;i++) assert(!pthread_join(threads[i],0));
    assert(!pthread_barrier_destroy(&launch)); mm_test_attach(0); mm_test_activate_task();
    assert(!kernel_files_lseek(&files,0,0,1,&result));
    int64_t total=(int64_t)(actors*PACKETS*sizeof(struct packet));
    if (append) assert(result>=(int64_t)(PACKETS*sizeof(struct packet)) && result<=total &&
        result%(int64_t)sizeof(struct packet)==0);
    else assert(result==total);
    struct packet packets[4*PACKETS]; size_t done;
    assert(!kernel_open_file_pread(controls[0],0,packets,actors*PACKETS*sizeof(*packets),&done) &&
        done==actors*PACKETS*sizeof(*packets));
    if (append) assert(packets[result/sizeof(struct packet)-1].actor==0 &&
        packets[result/sizeof(struct packet)-1].sequence==PACKETS-1);
    unsigned seen[4][PACKETS]={0};
    for (unsigned i=0;i<actors*PACKETS;i++) {
        assert(packets[i].actor<actors && packets[i].sequence<PACKETS &&
            !seen[packets[i].actor][packets[i].sequence]++);
        for (unsigned j=0;j<sizeof(packets[i].payload);j++)
            assert(packets[i].payload[j]==0x40+packets[i].actor);
    }
    if(!append) {
        assert(!kernel_files_lseek(&files,0,0,0,&result) && !result);
        struct kernel_page_cache_concurrency_state before={0},after={0};
        if(cache)kernel_page_cache_get_concurrency_state(cache,&before);
        memset(read_seen,0,sizeof(read_seen));
        assert(!pthread_barrier_init(&launch,0,actors));
        for(unsigned i=0;i<actors;i++) {
            views[i]=(struct kernel_files){0};assert(!kernel_files_acquire(&views[i],&files));
            assert(!pthread_create(&threads[i],0,parallel_reader,(void *)(uintptr_t)i));
        }
        for(unsigned i=0;i<actors;i++)assert(!pthread_join(threads[i],0));
        assert(!pthread_barrier_destroy(&launch));mm_test_attach(0);mm_test_activate_task();
        assert(controls[0]->offset==(uint64_t)total);
        for(unsigned i=0;i<actors;i++)for(unsigned n=0;n<PACKETS;n++)assert(read_seen[i][n]==1);
        if(cache) {
            kernel_page_cache_get_concurrency_state(cache,&after);
            assert(after.requests_queued==before.requests_queued);
        }
        puts("PASS shared OFD read/readv: unique complete packets, one total offset, hot hits stay off CPU0 queue");
    }
    assert(!kernel_files_sync(&files,0,0,&result) && !result);
    assert(!kernel_files_release(&files));
    for (unsigned i=0;i<actors;i++) assert(!kernel_open_file_release(&controls[i]));
    if (cache) kernel_page_cache_stop_worker(cache);
    printf("PASS %u threads %s %s: unique whole packets, offsets, final owners\n",actors,
        cache?"ext4":"tmpfs",append?"independent append OFDs":"shared OFD");
}
static enum kernel_block_status read_image(void *unused,uint64_t offset,void *data,size_t size)
{
    (void)unused; assert(!kernel_cpu_current()->logical_id && !kernel_cpu_current()->raw_locks);
    if (__atomic_exchange_n(&pause_access,0,__ATOMIC_ACQ_REL)) {
        __atomic_store_n(&access_entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&resume_access,__ATOMIC_ACQUIRE)) sched_yield();
    }
    return pread(image,data,size,offset)==(ssize_t)size?KERNEL_BLOCK_STATUS_OK:KERNEL_BLOCK_STATUS_IO;
}
static enum kernel_block_status write_image(void *unused,uint64_t offset,const void *data,size_t size)
{
    (void)unused; assert(!kernel_cpu_current()->raw_locks);
    return pwrite(image,data,size,offset)==(ssize_t)size?KERNEL_BLOCK_STATUS_OK:KERNEL_BLOCK_STATUS_IO;
}
static enum kernel_block_status flush_image(void *unused)
{ (void)unused; return fsync(image)?KERNEL_BLOCK_STATUS_IO:KERNEL_BLOCK_STATUS_OK; }
void __real_kernel_vfs_file_accessed(struct kernel_vfs_file *);
void __wrap_kernel_vfs_file_accessed(struct kernel_vfs_file *file)
{
    if(__atomic_exchange_n(&pause_local_access,0,__ATOMIC_ACQ_REL)) {
        __atomic_store_n(&local_access_entered,1,__ATOMIC_RELEASE);
        while(!__atomic_load_n(&resume_local_access,__ATOMIC_ACQUIRE))sched_yield();
    }
    __real_kernel_vfs_file_accessed(file);
}
struct crossing { unsigned id,shared; enum kernel_files_status status; int64_t result; char bytes[8]; };
static void *crossing_read(void *argument)
{
    struct crossing *call=argument; mm_test_attach(call->id); mm_test_bind(&mm);
    mm_test_bind_files(&files); mm_test_context=context;
    uint64_t buffer=0x200000+call->id*BOAROS_PAGE_SIZE;
    call->status=call->shared ? kernel_files_read(&files,&mm,0,buffer,8,&call->result) :
        kernel_files_pread(&files,&mm,0,buffer,8,0,&call->result);
    if (!call->status && call->result>=0) {
        size_t copied;
        assert(call->result==8 && !kernel_copy_from_user(&mm,call->bytes,buffer,8,&copied) && copied==8);
    }
    mm_test_finish(); return 0;
}
static void *stop_requests(void *argument)
{
    mm_test_attach(0); mm_test_activate_task();
    kernel_page_cache_stop_worker(argument);
    __atomic_store_n(&stop_done,1,__ATOMIC_RELEASE); mm_test_finish(); return 0;
}
static void crossing_case(struct kernel_heap *heap,struct kernel_vfs_mount *mount,
    struct kernel_page_cache *cache)
{
    int error; size_t done; int64_t result;
    files=(struct kernel_files){0}; assert(!kernel_files_create(&files,heap));
    for (unsigned i=0;i<2;i++) {
        assert(!kernel_open_file_create_mode(heap,mount,i?"/new-identity":"/old-identity",0600,&controls[i],&error) && !error);
        controls[i]->open_flags=2; assert(!kernel_vfs_file_acquire_write(&controls[i]->file));
        assert(!kernel_vfs_pwrite(&controls[i]->file,0,i?"replaced":"original",8,&done) && done==8);
        uint64_t observed=0;
        assert(!kernel_vfs_sync(&controls[i]->file,0,&observed) &&
            !kernel_page_cache_invalidate_node(cache,kernel_open_file_node(controls[i])));
        struct kernel_open_file_description *slot=controls[i];
        assert(!kernel_open_file_acquire(slot) && !kernel_files_install_new_owned_at(&files,i,0,&slot));
    }
    assert(!kernel_page_cache_start_worker(cache) && !kernel_files_check_task_domain(&files,&mm));
    access_entered=resume_access=stop_done=0; __atomic_store_n(&pause_access,1,__ATOMIC_RELEASE);
    struct crossing first={.id=1},queued={.id=2}; pthread_t active,pending,stopper;
    assert(!pthread_create(&active,0,crossing_read,&first));
    while (!__atomic_load_n(&access_entered,__ATOMIC_ACQUIRE)) sched_yield();
    /* The caller pins the original OFD; fd0 replacement cannot redirect it. */
    assert(!kernel_files_close(&files,0,&result) && !result && controls[0]->references==2);
    assert(!kernel_files_dup3(&files,1,0,0,&result) && result==0);
    assert(!pthread_create(&pending,0,crossing_read,&queued));
    struct kernel_block_device *device=kernel_vfs_file_block_device(&controls[1]->file);
    assert(device);
    for (;;) {
        unsigned waiting;
        { KERNEL_RAW_SCOPE(guard,&device->io_lock.lock.metadata);
          waiting=device->io_lock.lock.pending_head!=NULL; }
        if (waiting) break;
        sched_yield();
    }
    struct kernel_page_cache_concurrency_state state;
    kernel_page_cache_get_concurrency_state(cache,&state);
    assert(!state.running && !state.pending && !pthread_create(&stopper,0,stop_requests,cache));
    assert(!pthread_join(stopper,0) && __atomic_load_n(&stop_done,__ATOMIC_ACQUIRE));
    assert(controls[0]->references==2);
    __atomic_store_n(&resume_access,1,__ATOMIC_RELEASE);
    assert(!pthread_join(active,0) && !first.status && !memcmp(first.bytes,"original",8));
    assert(!pthread_join(pending,0) && !queued.status && queued.result==8 && !memcmp(queued.bytes,"replaced",8));
    kernel_page_cache_get_concurrency_state(cache,&state);
    assert(!state.pending && !state.running && !state.entry_borrows && state.executions_queued==state.executions_completed);
    mm_test_attach(0); mm_test_activate_task(); assert(!kernel_files_release(&files));
    for (unsigned i=0;i<2;i++) assert(!kernel_open_file_release(&controls[i]));
    puts("PASS caller OFD survives close/reuse and worker stop; physical I/O waiter reads replacement identity; final owners");
}
static void *crossing_seek(void *argument)
{
    struct crossing *call=argument;mm_test_attach(call->id);mm_test_bind(&mm);
    mm_test_bind_files(&files);mm_test_context=context;
    call->status=kernel_files_lseek(&files,0,0,1,&call->result);
    mm_test_finish();return 0;
}
static void borrowed_execution(void *argument)
{ (*(unsigned *)argument)++; }
static void *crossing_truncate(void *argument)
{
    struct crossing *call=argument;mm_test_attach(call->id);mm_test_bind(&mm);
    mm_test_bind_files(&files);mm_test_context=context;
    call->status=kernel_files_ftruncate(&files,0,8,&call->result);
    mm_test_finish();return NULL;
}
static void offset_handoff_case(struct kernel_heap *heap,struct kernel_vfs_mount *mount,
    struct kernel_page_cache *cache)
{
    int error;size_t written;uint64_t observed=0;
    files=(struct kernel_files){0};assert(!kernel_files_create(&files,heap));
    assert(!kernel_open_file_create_mode(heap,mount,"/offset-handoff",0600,&control,&error) && !error);
    control->open_flags=2;
    assert(!kernel_vfs_file_acquire_write(&control->file) &&
        !kernel_vfs_pwrite(&control->file,0,"offset!!",8,&written) && written==8 &&
        !kernel_vfs_sync(&control->file,0,&observed) &&
        !kernel_page_cache_invalidate_node(cache,kernel_open_file_node(control)));
    struct kernel_open_file_description *slot=control;
    assert(!kernel_open_file_acquire(slot) && !kernel_files_install_new_owned_at(&files,0,0,&slot) &&
        !kernel_page_cache_start_worker(cache) && !kernel_files_check_task_domain(&files,&mm));
    {
        unsigned executed=0;
        struct kernel_mm_user_pin pin={0};
        assert(!kernel_mm_pin_user_page(&mm,0x200000,KERNEL_MM_READ,&pin));
        KERNEL_LOCK_SCOPE(offset);
        kernel_mutex_lock(&control->offset_lock,&offset);
        assert(kernel_open_file_sync_range(control,0,UINT64_MAX)==-KERNEL_EIO);
        assert(kernel_page_cache_run_cpu0_borrowing(cache,borrowed_execution,&executed,&offset)==KERNEL_PAGE_CACHE_STATUS_STATE && !executed);
        kernel_mm_unpin_user_page(&pin);
        struct kernel_lock_guard unregistered={.lock=&control->offset_lock.lock,.owner=kernel_io_context_current(),.write=1};
        assert(kernel_page_cache_run_cpu0_borrowing(cache,borrowed_execution,&executed,&unregistered)==KERNEL_PAGE_CACHE_STATUS_STATE && !executed);
        assert(!kernel_page_cache_run_cpu0_borrowing(cache,borrowed_execution,&executed,&offset) && executed==1 &&
            kernel_io_context_current()->locks==&offset && kernel_lock_held(&control->offset_lock.lock,1));
    }
    local_access_entered=resume_local_access=0;__atomic_store_n(&pause_local_access,1,__ATOMIC_RELEASE);
    struct crossing reader={.id=1,.shared=1},seeker={.id=2};pthread_t reading,seeking;
    assert(!pthread_create(&reading,0,crossing_read,&reader));
    while(!__atomic_load_n(&local_access_entered,__ATOMIC_ACQUIRE))sched_yield();
    struct kernel_page_cache_concurrency_state before,after;
    kernel_page_cache_get_concurrency_state(cache,&before);
    assert(!pthread_create(&seeking,0,crossing_seek,&seeker));
    for(;;) {
        unsigned waiting;
        { KERNEL_RAW_SCOPE(guard,&control->offset_lock.lock.metadata);waiting=control->offset_lock.lock.pending_head!=NULL; }
        if(waiting)break;
        sched_yield();
    }
    kernel_page_cache_get_concurrency_state(cache,&after);
    assert(after.executions_queued==before.executions_queued);
    __atomic_store_n(&resume_local_access,1,__ATOMIC_RELEASE);
    assert(!pthread_join(reading,0) && !pthread_join(seeking,0));
    assert(!reader.status && reader.result==8 && !memcmp(reader.bytes,"offset!!",8) &&
        !seeker.status && seeker.result==8 && control->offset==8);
    mm_test_attach(0);mm_test_activate_task();
    assert(!kernel_page_cache_invalidate_node(cache,kernel_open_file_node(control)));
    struct kernel_lock_guard writing={0};
    kernel_vfs_file_write_lock(&control->file,&writing);
    kernel_page_cache_get_concurrency_state(cache,&before);
    struct crossing truncating={.id=2},cold={.id=1};pthread_t truncater;
    assert(!pthread_create(&truncater,NULL,crossing_truncate,&truncating));
    struct kernel_vfs_node *node=kernel_open_file_node(control);
    for (;;) {
        unsigned waiting;
        { KERNEL_RAW_SCOPE(guard,&node->write_operation.lock.metadata);
          waiting=node->write_operation.lock.pending_head!=NULL; }
        if (waiting) break;
        sched_yield();
    }
    kernel_page_cache_get_concurrency_state(cache,&after);
    assert(after.executions_queued==before.executions_queued);
    assert(!pthread_create(&reading,NULL,crossing_read,&cold) && !pthread_join(reading,NULL));
    assert(!cold.status && cold.result==8 && !memcmp(cold.bytes,"offset!!",8));
    kernel_lock_release(&writing);
    assert(!pthread_join(truncater,NULL) && !truncating.status && !truncating.result);
    assert(!kernel_files_release(&files) && !kernel_open_file_release(&control));
    kernel_page_cache_stop_worker(cache);
    puts("PASS shared offset qualification precedes CPU0 queue: held read, waiting seek, cold miss and actual owners finish");
    puts("PASS truncate obtains write-operation qualification before queueing; blocked truncate cannot stop an active writer's cold read");
}
/* 独立同步不占文件位置或整次执行者；两个请求仍共享一个错误游标。 */
static unsigned sync_pause,sync_entered,sync_resume,sync_progress;
static void pause_sync_call(struct kernel_vfs_file *file)
{
    if (control && kernel_vfs_file_node(file)==kernel_open_file_node(control) &&
        __atomic_exchange_n(&sync_pause,0,__ATOMIC_ACQ_REL)) {
        assert(kernel_cpu_current()->logical_id==1 && !kernel_cpu_current()->raw_locks &&
            !kernel_io_context_current()->locks);
        __atomic_store_n(&sync_entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&sync_resume,__ATOMIC_ACQUIRE)) sched_yield();
    }
}
int __real_kernel_vfs_sync_borrowing(struct kernel_vfs_file *,int,uint64_t *,const struct kernel_lock_guard *);
int __wrap_kernel_vfs_sync_borrowing(struct kernel_vfs_file *file,int data_only,uint64_t *observed,
    const struct kernel_lock_guard *borrowed)
{
    pause_sync_call(file);
    return __real_kernel_vfs_sync_borrowing(file,data_only,observed,borrowed);
}
int __real_kernel_vfs_sync_range(struct kernel_vfs_file *,uint64_t,uint64_t,uint64_t *);
int __wrap_kernel_vfs_sync_range(struct kernel_vfs_file *file,uint64_t start,uint64_t end,uint64_t *observed)
{
    pause_sync_call(file);
    return __real_kernel_vfs_sync_range(file,start,end,observed);
}
static void *crossing_sync(void *argument)
{
    struct crossing *call=argument;mm_test_attach(call->id);mm_test_bind(&mm);
    mm_test_bind_files(&files);mm_test_context=context;
    kernel_cpu_current()->logical_id=1;
    if (call->shared) call->result=kernel_open_file_sync_range(control,0,UINT64_MAX);
    else call->status=kernel_files_sync(&files,0,0,&call->result);
    mm_test_finish();return NULL;
}
static void *sync_position_progress(void *argument)
{
    struct crossing *call=argument;mm_test_attach(call->id);mm_test_bind(&mm);
    mm_test_bind_files(&files);mm_test_context=context;
    kernel_cpu_current()->logical_id=1;
    uint64_t buffer=0x200000+call->id*BOAROS_PAGE_SIZE;size_t copied;
    call->status=kernel_files_read(&files,&mm,0,buffer,8,&call->result);
    assert(!call->status && call->result==8 &&
        !kernel_copy_from_user(&mm,call->bytes,buffer,8,&copied) && copied==8 &&
        !memcmp(call->bytes,"syncdata",8));
    call->status=kernel_files_lseek(&files,0,0,1,&call->result);
    assert(!call->status && call->result==8);
    __atomic_store_n(&sync_progress,1,__ATOMIC_RELEASE);
    mm_test_finish();return NULL;
}
static void sync_offset_case(struct kernel_heap *heap,struct kernel_vfs_mount *mount,
    struct kernel_page_cache *cache,int range_first)
{
    int error;size_t done;uint64_t observed=0;int64_t result;
    files=(struct kernel_files){0};assert(!kernel_files_create(&files,heap));
    assert(!kernel_open_file_create_mode(heap,mount,range_first?"/range-position":"/sync-position",0600,&control,&error) && !error);
    control->open_flags=2;
    assert(!kernel_vfs_file_acquire_write(&control->file) &&
        !kernel_vfs_pwrite(&control->file,0,"syncdata",8,&done) && done==8 &&
        !kernel_vfs_sync(&control->file,0,&observed));
    struct kernel_open_file_description *slot=control;
    assert(!kernel_open_file_acquire(slot) && !kernel_files_install_new_owned_at(&files,0,0,&slot));
    /* 先装热页，后端同步不应阻止同OFD的读和SEEK_CUR。 */
    assert(!kernel_open_file_pread(control,0,(char[8]){0},8,&done) && done==8 &&
        !kernel_page_cache_start_worker(cache) && !kernel_files_check_task_domain(&files,&mm));
    kernel_vfs_record_writeback_error(kernel_open_file_node(control),-KERNEL_EIO);
    uint64_t sequence=kernel_vfs_error_sequence(&control->file);
    assert(control->observed_writeback_error!=sequence);
    sync_entered=sync_resume=sync_progress=0;__atomic_store_n(&sync_pause,1,__ATOMIC_RELEASE);
    struct crossing first={.id=1,.shared=(unsigned)range_first},second={.id=2,.shared=(unsigned)!range_first},position={.id=3};
    pthread_t syncing,queued,reader;
    assert(!pthread_create(&syncing,NULL,crossing_sync,&first));
    while (!__atomic_load_n(&sync_entered,__ATOMIC_ACQUIRE)) sched_yield();
    assert(!pthread_create(&reader,NULL,sync_position_progress,&position));
    for (;;) {
        unsigned waiting;
        { KERNEL_RAW_SCOPE(guard,&control->offset_lock.lock.metadata);
          waiting=control->offset_lock.lock.pending_head!=NULL; }
        if (waiting) {
            fputs("sync scope regression: hot read queued on sync-owned offset lock\n",stderr);
            assert(!waiting);
        }
        if (__atomic_load_n(&sync_progress,__ATOMIC_ACQUIRE)) break;
        sched_yield();
    }
    assert(!pthread_join(reader,NULL) && control->offset==8);
    struct kernel_page_cache_concurrency_state before,after;
    kernel_page_cache_get_concurrency_state(cache,&before);
    assert(!before.running && !before.pending && !pthread_create(&queued,NULL,crossing_sync,&second));
    assert(!pthread_join(queued,NULL));
    assert(!second.status && second.result==-KERNEL_EIO && control->observed_writeback_error==sequence);
    __atomic_store_n(&sync_resume,1,__ATOMIC_RELEASE);
    assert(!pthread_join(syncing,NULL));
    assert(!first.status && !first.result &&
        control->observed_writeback_error==sequence && control->offset==8);
    mm_test_attach(0);mm_test_activate_task();
    assert(!kernel_files_sync(&files,0,0,&result) && !result);
    kernel_page_cache_get_concurrency_state(cache,&after);
    assert(!after.pending && !after.running && !after.entry_borrows &&
        after.executions_queued==after.executions_completed && control->references==2);
    assert(!kernel_files_release(&files) && !kernel_open_file_release(&control));
    kernel_page_cache_stop_worker(cache);
    printf("PASS %s waits outside OFD offset: hot read and seek progress, competing sync consumes one error, final owners\n",range_first?"range sync":"fsync");
}
static void bind(unsigned id)
{ mm_test_attach(id); mm_test_bind(&mm); mm_test_bind_files(&files); mm_test_context=context; }
static void mapped_source_write(void)
{
    int64_t result; size_t copied;
    unsigned char bytes[68];
    assert(kernel_open_file_size(control)==68);
    /* 目标尾页已缓存，来源别名尚未缺页；不能先关闭目标写入再等自身装入。 */
    assert(!kernel_files_pwrite(&files,&mm,0,shared_mapping,8,128,&result) && result==8 &&
        kernel_open_file_size(control)==136);
    assert(!kernel_files_ftruncate(&files,0,68,&result) && !result && kernel_open_file_size(control)==68);
    /* 首次读取尚未驻留的同文件映射；fault期间目标没有inode锁或写借用。 */
    assert(!kernel_files_pwrite(&files,&mm,0,shared_mapping,8,4,&result) && result==8);
    enum kernel_uaccess_status access=kernel_copy_from_user(&mm,bytes,shared_mapping,12,&copied);
    if (access || copied!=12 || memcmp(bytes,"origoriginal",12)) {
        fprintf(stderr,"shared source overlap: page=%u memory=%d access=%d copied=%zu bytes=",
            (unsigned)BOAROS_PAGE_SIZE,kernel_open_file_memory_backed(control),access,copied);
        for (unsigned i=0;i<12;i++) fprintf(stderr,"%02x",bytes[i]);
        struct kernel_mm_mapping mapping;
        assert(!kernel_mm_lookup(&mm,shared_mapping,&mapping));
        uint64_t cached; size_t valid;
        assert(!kernel_page_cache_lookup(kernel_open_file_page_cache(control),&control->file,0,&cached,&valid));
        fprintf(stderr," mapped=%llx cached=%llx cache-bytes=",(unsigned long long)mapping.physical_address,(unsigned long long)cached);
        for (unsigned i=0;i<12;i++) fprintf(stderr,"%02x",((unsigned char *)access_page(cached))[i]);
        assert(!physical_page_release(control->heap->page_allocator,cached));
        fputc('\n',stderr); assert(!"shared source overlap contents");
    }
    assert(!kernel_files_pwrite(&files,&mm,0,shared_mapping+4,8,0,&result) && result==8);
    assert(!kernel_copy_from_user(&mm,bytes,shared_mapping,8,&copied) && copied==8 &&
        !memcmp(bytes,"original",8));
    /* 来源位于旧EOF尾部：先复制，再清新洞，不能把来源先归零。 */
    assert(!kernel_copy_to_user(&mm,shared_mapping+96,"fromtail",8,&copied) && copied==8);
    assert(!kernel_files_pwrite(&files,&mm,0,shared_mapping+96,8,128,&result) && result==8 &&
        kernel_open_file_size(control)==136);
    assert(!kernel_copy_from_user(&mm,bytes,shared_mapping+68,sizeof(bytes),&copied) && copied==sizeof(bytes));
    if (!kernel_open_file_memory_backed(control)) for (size_t i=0;i<60;i++) assert(!bytes[i]);
    assert(!memcmp(bytes+60,"fromtail",8));
}
static void *read_and_write(void *unused)
{
    (void)unused; bind(1);
    int64_t result; size_t copied;
    assert(!kernel_files_read(&files,&mm,0,0x100000,8,&result) && result==8);
    char bytes[8];
    assert(!kernel_copy_from_user(&mm,bytes,0x100000,8,&copied) && copied==8 && !memcmp(bytes,"original",8));
    assert(!kernel_files_pwrite(&files,&mm,0,0x100000,8,32,&result) && result==8);
    assert(!kernel_files_lseek(&files,0,0,1,&result) && result==8);
    assert(!kernel_files_pread(&files,&mm,0,0x100000+BOAROS_PAGE_SIZE,8,32,&result) && result==8);
    assert(!kernel_copy_from_user(&mm,bytes,0x100000+BOAROS_PAGE_SIZE,8,&copied) && copied==8 && !memcmp(bytes,"original",8));
    if (shared_mapping) assert(!kernel_mm_msync(&mm,shared_mapping,BOAROS_PAGE_SIZE,4));
    if (!kernel_open_file_memory_backed(control)) {
        assert(!kernel_files_sync(&files,0,0,&result) && !result);
        assert(!kernel_files_ftruncate(&files,0,48,&result) && !result);
        assert(!kernel_files_fstat(&files,&mm,0,0x100080,&result) && !result);
        struct kernel_linux_stat stat;
        assert(!kernel_copy_from_user(&mm,&stat,0x100080,sizeof(stat),&copied) && copied==sizeof(stat) && stat.st_size==48);
    }
    if (!kernel_open_file_memory_backed(control)) {
        struct kernel_mm_user_pin pin={0};
        assert(!kernel_mm_pin_user_page(&mm,0x100000,KERNEL_MM_READ,&pin));
        assert(!kernel_files_pread(&files,&mm,0,0x100000,0,0,&result) && !result);
        kernel_mm_unpin_user_page(&pin);
        assert(!kernel_files_pread(&files,&mm,0,0x100000,0,0,&result) && !result);
    }
    unsigned char sentinel=0xa5;
    assert(!kernel_copy_to_user(&mm,0x100000+BOAROS_PAGE_SIZE,&sentinel,1,&copied) && copied==1);
    assert(!kernel_mm_mprotect(&mm,0x100000+BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE,0));
    assert(!kernel_files_pread(&files,&mm,0,0x100000+BOAROS_PAGE_SIZE-4,8,32,&result) && result==4);
    assert(!kernel_copy_from_user(&mm,bytes,0x100000+BOAROS_PAGE_SIZE-4,4,&copied) &&
        copied==4 && !memcmp(bytes,"orig",4));
    assert(!kernel_files_pwrite(&files,&mm,0,0x100000+BOAROS_PAGE_SIZE-4,8,64,&result) && result==4);
    assert(!kernel_mm_mprotect(&mm,0x100000+BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE));
    assert(!kernel_copy_from_user(&mm,&sentinel,0x100000+BOAROS_PAGE_SIZE,1,&copied) && copied==1 && sentinel==0xa5);
    mapped_source_write();
    assert(!kernel_files_dup(&files,0,&result) && result==1);
    assert(!kernel_files_close(&files,0,&result) && !result);
    assert(!kernel_files_close(&files,1,&result) && !result);
    mm_test_finish(); return 0;
}
static void file_case(struct kernel_heap *heap,struct kernel_vfs_mount *mount,
    struct kernel_page_cache *cache)
{
    struct kernel_mm child={0};
    int error; size_t done;
    files=(struct kernel_files){0};
    fs=(struct kernel_fs_context){0}; assert(!kernel_fs_context_create(&fs,mount,heap));
    assert(!kernel_open_file_create_mode(heap,mount,"/ordinary",0600,&control,&error) && !error);
    control->open_flags=2;
    assert(!kernel_vfs_file_acquire_write(&control->file));
    assert(!kernel_vfs_pwrite(&control->file,0,"original",8,&done) && done==8);
    uint64_t observed=0;
    assert(!kernel_vfs_sync(&control->file,0,&observed));
    shared_mapping=0;
    if (cache) {
        uint64_t address;
        assert(!kernel_mm_munmap(&mm,0x100000,2*BOAROS_PAGE_SIZE));
        assert(!kernel_mm_mmap_anonymous(&mm,0x100000,2*BOAROS_PAGE_SIZE,
            KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED_NOREPLACE,&address));
        unsigned char marker=0x55; size_t copied;
        assert(!kernel_copy_to_user(&mm,0x100000,&marker,1,&copied) && copied==1);
        assert(!kernel_mm_fork(&child,&mm));
        assert(!kernel_page_cache_invalidate_node(cache,kernel_open_file_node(control)));
        assert(!kernel_page_cache_start_worker(cache));
    }
    struct kernel_open_file_description *source=control;
    assert(!kernel_open_file_acquire(source));
    assert(!kernel_file_mapping_prepare(kernel_open_file_node(control)));
    assert(!kernel_mm_mmap_file_private(&mm,&source,0x400000,BOAROS_PAGE_SIZE,0,
        KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_SHARED|KERNEL_MM_MAP_FIXED_NOREPLACE,&shared_mapping) && !source);
    assert(!kernel_files_create(&files,heap));
    struct kernel_open_file_description *slot=control;
    assert(!kernel_open_file_acquire(slot) && !kernel_files_install_new_owned_at(&files,0,0,&slot));
    assert(!kernel_files_check_task_domain(&files,&mm));
    pthread_t actor; assert(!pthread_create(&actor,0,read_and_write,0) && !pthread_join(actor,0));
    mm_test_attach(0); mm_test_activate_task();
    assert(!kernel_files_release(&files));
    assert(!kernel_fs_context_release(&fs));
    if (child.state==KERNEL_MM_LIVE) {
        unsigned char marker=0; size_t copied;
        assert(!kernel_copy_from_user(&child,&marker,0x100000,1,&copied) && copied==1 && marker==0x55);
        assert(!kernel_mm_release(&child));
    }
    if (shared_mapping) assert(!kernel_mm_munmap(&mm,shared_mapping,BOAROS_PAGE_SIZE));
    assert(control->references==1);
    if (cache) {
        struct kernel_page_cache_concurrency_state state;
        kernel_page_cache_get_concurrency_state(cache,&state);
        assert(state.executions_queued && state.executions_completed==state.executions_queued && !state.pending && !state.running);
    }
    char bytes[8];
    assert(!kernel_open_file_pread(control,32,bytes,8,&done) && done==8 && !memcmp(bytes,"original",8));
    assert(!kernel_open_file_pread(control,64,bytes,4,&done) && done==4 && !memcmp(bytes,"orig",4));
    assert(!kernel_open_file_release(&control));
    if (cache) kernel_page_cache_stop_worker(cache);
    puts(cache?"PASS cross-CPU ext4 file I/O, shared offset, positioned prefix and CPU0 final owner":
        "PASS cross-CPU tmpfs file I/O, shared offset, positioned prefix and CPU0 final owner");
}
static void initialize(struct physical_page_allocator *pages,struct kernel_heap *heap,void *pool,size_t size)
{
    struct boot_memory_layout layout={.usable_count=1,.usable={{(uintptr_t)pool,size}}};
    assert(!physical_page_allocator_init(pages,&layout) && !physical_page_allocator_bind_access(pages,access_page) &&
        !physical_page_allocator_finalize(pages) && !kernel_heap_init(heap,pages,page_address));
}
static void sendfile_pipe_case(struct kernel_heap *heap,struct kernel_vfs_mount *mount)
{
    struct kernel_files local={0};
    struct kernel_open_file_description *source=0;
    int error;int64_t result;size_t done;
    unsigned char data[BOAROS_PAGE_SIZE+43],actual[BOAROS_PAGE_SIZE+17];
    for (size_t i=0;i<sizeof(data);i++) data[i]=(unsigned char)(i*17U+3U);
    assert(!kernel_open_file_create_mode(heap,mount,"/sendfile-source",0600,&source,&error) && !error);
    source->open_flags=2;assert(!kernel_vfs_file_acquire_write(&source->file));
    assert(!kernel_vfs_pwrite(&source->file,0,data,sizeof(data),&done) && done==sizeof(data));
    struct kernel_open_file_description *source_file=source;
    assert(!kernel_files_create(&local,heap) && !kernel_files_install_new_owned_at(&local,0,0,&source));
    assert(!kernel_files_pipe2(&local,&mm,0x200000,KERNEL_FILES_O_NONBLOCK,&result) && !result);
    int pair[2];assert(!kernel_copy_from_user(&mm,pair,0x200000,sizeof(pair),&done) && done==sizeof(pair));
    int64_t offset=13;assert(!kernel_copy_to_user(&mm,0x200080,&offset,sizeof(offset),&done));
    assert(!kernel_files_sendfile(&local,&mm,pair[1],0,0x200080,sizeof(actual),&result) && result==sizeof(actual));
    assert(!kernel_copy_from_user(&mm,&offset,0x200080,sizeof(offset),&done) && offset==13+(int64_t)sizeof(actual));
    assert(!kernel_files_read(&local,&mm,pair[0],0x200000,sizeof(actual),&result) && result==sizeof(actual));
    assert(!kernel_copy_from_user(&mm,actual,0x200000,sizeof(actual),&done) && done==sizeof(actual) &&
        !memcmp(actual,data+13,sizeof(actual)));
    assert(!kernel_vfs_ftruncate(&source_file->file,4U*BOAROS_PAGE_SIZE));
    uint64_t physical;size_t valid;
    int memory=kernel_open_file_memory_backed(source_file);
    if (memory) assert(kernel_open_file_lookup_page(source_file,2,&physical,&valid)==KERNEL_PAGE_CACHE_STATUS_NOT_FOUND);
    offset=2U*BOAROS_PAGE_SIZE+7U;
    assert(!kernel_copy_to_user(&mm,0x200080,&offset,sizeof(offset),&done));
    assert(!kernel_files_sendfile(&local,&mm,pair[1],0,0x200080,17,&result) && result==17);
    assert(!kernel_files_read(&local,&mm,pair[0],0x200000,17,&result) && result==17);
    assert(!kernel_copy_from_user(&mm,actual,0x200000,17,&done) && done==17);
    for (size_t i=0;i<17;i++) assert(!actual[i]);
    if (memory) assert(kernel_open_file_lookup_page(source_file,2,&physical,&valid)==KERNEL_PAGE_CACHE_STATUS_NOT_FOUND);
    assert(!kernel_files_release(&local));
    mm_test_finish();mm_test_attach(0);mm_test_activate_task();
    puts("PASS sendfile source-page borrow: unaligned cross-page pipe content, sparse zero bytes and explicit offset");
}

static void vector_boundaries(struct kernel_heap *heap,struct kernel_vfs_mount *mount)
{
    struct kernel_files local={0};
    struct kernel_open_file_description *file=NULL;
    int error; int64_t result; size_t copied,written;
    assert(!kernel_files_create(&local,heap));
    assert(!kernel_open_file_create_mode(heap,mount,"/vector-boundaries",0600,&file,&error) && !error);
    file->open_flags=2;
    assert(!kernel_vfs_file_acquire_write(&file->file));
    assert(!kernel_vfs_pwrite(&file->file,0,"x",1,&written) && written==1);
    assert(!kernel_files_install_new_owned_at(&local,0,0,&file));
    const struct {
        uint64_t base,length,count;
        int64_t read_result,write_result;
    } cases[]={
        {ARCH_MMU_USER_LIMIT,0,1,0,0},
        {ARCH_MMU_USER_LIMIT+1,0,1,-KERNEL_EFAULT,-KERNEL_EFAULT},
        {UINT64_MAX,0,0,0,0},
        {0x100000,INT64_MAX,1,1,2*BOAROS_PAGE_SIZE},
        {0x100000,UINT64_MAX,1,-KERNEL_EINVAL,-KERNEL_EINVAL},
        {0x100000,INT64_MAX,2,-KERNEL_EFAULT,-KERNEL_EFAULT},
    };
    for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        struct kernel_uaccess_iovec iov[2]={{cases[i].base,cases[i].length},{0x100000,1}};
        assert(!kernel_copy_to_user(&mm,0x200080,iov,sizeof(iov),&copied) && copied==sizeof(iov));
        uint64_t user_iov=cases[i].count?0x200080:UINT64_MAX;
        assert(!kernel_files_lseek(&local,0,0,0,&result) && !result);
        assert(!kernel_files_readv(&local,&mm,0,user_iov,cases[i].count,&result) && result==cases[i].read_result);
        assert(!kernel_files_lseek(&local,0,0,0,&result) && !result);
        assert(!kernel_files_writev(&local,&mm,0,user_iov,cases[i].count,&result) && result==cases[i].write_result);
        /* Keep the next case's EOF independent of an earlier accepted prefix. */
        assert(!kernel_files_ftruncate(&local,0,1,&result) && !result);
    }
    assert(!kernel_files_release(&local));
    mm_test_finish();mm_test_attach(0);mm_test_activate_task();
    puts("PASS vector boundaries: empty ranges, signed lengths, single-vector cap before range and multi-vector original-range checks");
}

static void directory_eof(struct kernel_heap *heap,struct kernel_vfs_mount *mount)
{
    assert(!kernel_vfs_mkdir(mount,"/empty-directory",0700));
    struct kernel_open_file_description *directory=NULL;
    int error; int64_t result;
    assert(!kernel_open_file_create(heap,mount,"/empty-directory",&directory,&error) && !error);
    directory->kind=KERNEL_OPEN_FILE_KIND_DIRECTORY;
    struct kernel_open_file_description *borrowed=directory;
    struct kernel_files local={0};
    assert(!kernel_files_create(&local,heap) && !kernel_files_install_new_owned_at(&local,0,0,&directory));
    uint64_t next=UINT64_MAX,ino;uint8_t type;char name[256];
    assert(!kernel_vfs_dir_entry(&borrowed->file,2,&next,&ino,&type,name,sizeof(name)) && next==2);
    assert(!kernel_files_lseek(&local,0,2,0,&result) && result==2);
    for (unsigned i=0;i<3;i++) {
        assert(!kernel_files_getdents(&local,&mm,0,0x200000,4096,&result) && !result);
        assert(!kernel_files_lseek(&local,0,0,1,&result) && result==2);
    }
    assert(!kernel_files_release(&local));
    puts("PASS directory EOF: valid unchanged VFS cookie and repeated getdents preserve the shared offset");
}

static void borrowed_page_case(struct physical_page_allocator *pages)
{
    struct kernel_task_io_buffer scratch={0},borrowed={0};
    assert(!kernel_task_io_buffer_acquire(&scratch,pages));
    uint64_t retained=scratch.physical_address;
    kernel_task_io_buffer_release(&scratch);
    uint64_t before=physical_page_available(pages),physical;
    assert(!physical_page_allocate(pages,&physical));
    kernel_task_io_buffer_take_page(&borrowed,pages,physical);
    assert(borrowed.physical_address==physical && !borrowed.retained_scratch);
    kernel_task_io_buffer_release(&borrowed);
    assert(physical_page_available(pages)==before);
    assert(!kernel_task_io_buffer_acquire(&scratch,pages) && scratch.physical_address==retained);
    kernel_task_io_buffer_release(&scratch);
    mm_test_finish();mm_test_attach(0);mm_test_activate_task();
}

#include "block_file_cases.h"
#ifdef LOOP_IO_CASES
#include "loop_cases.h"
#endif
#ifdef TCP_IO_CASES
#include "tcp_usercopy_cases.h"
#endif
int main(int argc,char **argv)
{
    assert(argc==2); setvbuf(stdout,0,_IONBF,0); mm_test_attach(0); mm_test_activate_task();
    struct physical_page_allocator pages={0},backend_pages={0};
    struct kernel_heap heap={0},backend_heap={0};
    initialize(&pages,&heap,arena,sizeof(arena)); initialize(&backend_pages,&backend_heap,backend,sizeof(backend));
    uint64_t baseline=physical_page_available(&pages),backend_baseline=physical_page_available(&backend_pages);
    borrowed_page_case(&pages);
    struct ext4_bcache done_cache;
    assert(!ext4_bcache_init_dynamic(&done_cache,1,4096));
    struct ext4_buf done_buffer={.bc=&done_cache,.loading=false};
    completed_buffer_wait=1;
    boaros_lwext4_wait(&done_buffer);
    completed_buffer_wait=0;
    assert(!ext4_bcache_fini_dynamic(&done_cache));
    puts("PASS ext4 completion before wait callback: no registration and no sleep");
    struct arch_mmu_page_table kernel={0}; struct arch_mmu_user_space user={0};
    assert(!riscv_sv39_page_table_init(&kernel,&pages)); kernel.state=ARCH_MMU_STATE_ACTIVE;
    assert(!arch_mmu_user_space_init(&user,&pages,&kernel) && !kernel_mm_create(&mm,&user) &&
        !kernel_mm_vma_enable(&mm,&heap) && !kernel_mm_brk_initialize(&mm,0x10000,0x20000));
    assert(!kernel_mm_check_task_domain(&mm) && !kernel_mm_context(&mm,&context));
    mm_test_context=context; /* 当前地址空间API的宿主硬件上下文，不是任务绑定。 */
    uint64_t address;
    assert(!kernel_mm_mmap_anonymous(&mm,0x100000,2*BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE,
        KERNEL_MM_MAP_FIXED_NOREPLACE,&address));
    assert(!kernel_mm_mmap_anonymous(&mm,0x200000,4*BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE,
        KERNEL_MM_MAP_FIXED_NOREPLACE,&address));
    futex_pin_case();
#ifdef TCP_IO_CASES
    unix_receive_deadline_case(&heap);
    unix_receive_error_prefix_case(&heap);
    tcp_nonblock_timeout_case(&heap,0);
    tcp_nonblock_timeout_case(&heap,1);
    tcp_wait_owner_case(&heap,0);
    tcp_wait_owner_case(&heap,1);
    tcp_read_batch_case(&heap);
    tcp_usercopy_case(&heap);
#endif
    struct kernel_vfs_mount *memory=0;
    assert(!kernel_tmpfs_create(&heap,0,"",&memory)); sendfile_pipe_case(&heap,memory); file_case(&heap,memory,0);
    directory_eof(&heap,memory);
    vector_boundaries(&heap,memory);
    raw_block_case(&heap,memory);
#ifdef LOOP_IO_CASES
    loop_case(&heap,memory);
#endif
    close_lock_case(&heap,memory);
    for (unsigned actors=2;actors<=4;actors+=2) for (int append=0;append<=1;append++)
        parallel_case(&heap,memory,0,actors,append);
    assert(!kernel_vfs_unmount(memory));
    image=open(argv[1],O_RDWR); assert(image>=0); struct stat info; assert(!fstat(image,&info));
    struct kernel_page_cache cache={0}; assert(!kernel_page_cache_init(&cache,&heap,&pages));
    struct kernel_block_device disk={.read=read_image,.write=write_image,.flush=flush_image,
        .capacity_bytes=info.st_size,.logical_block_size=512,.cache_mode=KERNEL_BLOCK_CACHE_WRITEBACK};
    assert(!kernel_block_register(&disk,KERNEL_BLOCK_DEVICE_NUMBER(101)));
    struct kernel_vfs_mount mount={0}; assert(!kernel_vfs_mount_ext4(&mount,&disk,&backend_heap,&cache,0));
    raw_mounted_flush_case(&heap,&mount,&disk);
    worker_lock_close_case(&heap,&mount,&cache);
    sendfile_pipe_case(&heap,&mount);
    file_case(&heap,&mount,&cache);
    crossing_case(&heap,&mount,&cache);
    offset_handoff_case(&heap,&mount,&cache);
    sync_offset_case(&heap,&mount,&cache,0);
    sync_offset_case(&heap,&mount,&cache,1);
    for (unsigned actors=2;actors<=4;actors+=2) for (int append=0;append<=1;append++)
        parallel_case(&heap,&mount,&cache,actors,append);
    assert(!kernel_vfs_unmount(&mount) && !kernel_page_cache_destroy(&cache));
    assert(!kernel_block_unregister(&disk)); assert(!close(image));
    mm_test_context=0; /* 页表最终释放在可信kernel上下文。 */
    assert(!kernel_mm_release(&mm) && !physical_page_release(&pages,kernel.root_address));
    struct kernel_heap_statistics statistics;
    kernel_heap_get_statistics(&heap,&statistics); assert(!statistics.live_allocations && !statistics.current_pages);
    kernel_heap_get_statistics(&backend_heap,&statistics); assert(!statistics.live_allocations && !statistics.current_pages);
    assert(!physical_page_allocator_audit(&pages) && !physical_page_allocator_audit(&backend_pages) &&
        physical_page_available(&pages)==baseline && physical_page_available(&backend_pages)==backend_baseline);
    printf("PASS file I/O owners returned: OFD=%zu FD=%zu\n",sizeof(*control),sizeof(struct kernel_files_record));
}
