#define main original_file_io_main
#include "file_io_concurrency.c"
#undef main
#include <ext4_fs.h>
#include <ext4_journal.h>
#include <ext4_inode.h>
#include <ext4_super.h>
#include <kernel/time.h>
#include <stdlib.h>

struct parallel_actor {
    struct kernel_vfs_file *existing;
    struct ext4_operation *operation;
    struct kernel_rwlock *resource;
    uint64_t lba;
    uint32_t offset,mode;
    unsigned actor,entered,resume,done,abort;
    int error;
};
static _Thread_local struct parallel_actor *parallel_current;
static unsigned checkpoint_pause,checkpoint_entered,checkpoint_resume,checkpoint_calls;
static unsigned fail_thread_start,startup_pause,startup_entered,startup_resume,startup_done,startup_flush_done;
static int startup_expected;
enum kernel_scheduler_status __real_kernel_thread_create_joinable(
    void (*)(void *),void *,struct kernel_thread_join *);
enum kernel_scheduler_status __wrap_kernel_thread_create_joinable(
    void (*entry)(void *),void *argument,struct kernel_thread_join *join)
{
    if(startup_pause && !--startup_pause) {
        __atomic_store_n(&startup_entered,1,__ATOMIC_RELEASE);
        while(!__atomic_load_n(&startup_resume,__ATOMIC_ACQUIRE))sched_yield();
    }
    if (fail_thread_start && !--fail_thread_start) return KERNEL_SCHEDULER_STATUS_NO_MEMORY;
    return __real_kernel_thread_create_joinable(entry,argument,join);
}
int __real_jbd_journal_checkpoint(struct jbd_journal *);
int __wrap_jbd_journal_checkpoint(struct jbd_journal *journal)
{
    assert(kernel_io_context_current()->background_reclaim &&
           !kernel_cpu_current()->logical_id && !kernel_io_context_current()->backend_state_depth);
    __atomic_fetch_add(&checkpoint_calls,1,__ATOMIC_RELAXED);
    if (__atomic_exchange_n(&checkpoint_pause,0,__ATOMIC_ACQ_REL)) {
        __atomic_store_n(&checkpoint_entered,1,__ATOMIC_RELEASE);
        while(!__atomic_load_n(&checkpoint_resume,__ATOMIC_ACQUIRE))sched_yield();
    }
    return __real_jbd_journal_checkpoint(journal);
}
int __real_jbd_trans_set_block_dirty_range(struct jbd_trans *,struct ext4_block *,uint32_t,uint32_t);
int __wrap_jbd_trans_set_block_dirty_range(struct jbd_trans *trans,struct ext4_block *block,
    uint32_t offset,uint32_t length)
{
    int result=__real_jbd_trans_set_block_dirty_range(trans,block,offset,length);
    struct parallel_actor *actor=parallel_current;
    struct ext4_fs *fs=trans->operation.fs;
    if (result || !actor || actor->entered || length!=ext4_get16(&fs->sb,inode_size) ||
        (ext4_inode_get_mode(&fs->sb,(struct ext4_inode *)(block->data+offset)) &
         KERNEL_VFS_S_IFMT)!=KERNEL_VFS_S_IFREG) return result;
    assert(ext4_current_operation(fs)==&trans->operation);
    actor->operation=&trans->operation;actor->lba=block->lb_id;actor->offset=offset;
    actor->resource=kernel_io_context_current()->locks->lock;
    assert(actor->resource->rank==31U);
    __atomic_store_n(&actor->entered,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&actor->resume,__ATOMIC_ACQUIRE))sched_yield();
    return actor->abort?ECANCELED:result;
}
static void *parallel_modify(void *argument)
{
    struct parallel_actor *actor=argument;
    mm_test_attach(actor->actor+1);mm_test_activate_task();kernel_cpu_current()->logical_id=actor->actor;
    parallel_current=actor;
    actor->error=kernel_vfs_file_set_mode(actor->existing,actor->mode);
    parallel_current=NULL;
    __atomic_store_n(&actor->done,1,__ATOMIC_RELEASE);
    mm_test_finish();return NULL;
}
static void wait_entered(struct parallel_actor *actor)
{
    while(!__atomic_load_n(&actor->entered,__ATOMIC_ACQUIRE)) {
        if(__atomic_load_n(&actor->done,__ATOMIC_ACQUIRE)) {
            fprintf(stderr,"actor %u returned %d before private inode modification\n",actor->actor,actor->error);
            assert(0);
        }
        sched_yield();
    }
}
static void wait_done(struct parallel_actor *actor)
{
    while(!__atomic_load_n(&actor->done,__ATOMIC_ACQUIRE))sched_yield();
    assert(actor->error==(actor->abort?-ECANCELED:0));
}
static void *parallel_start_workers(void *argument)
{
    mm_test_attach(5);mm_test_activate_task();
    struct lwext4_mount_adapter *adapter=argument;
    assert(kernel_vfs_start_journal_worker(adapter->mount)==startup_expected);
    __atomic_store_n(&startup_done,1,__ATOMIC_RELEASE);
    mm_test_finish();return NULL;
}
static void *parallel_startup_flush(void *argument)
{
    mm_test_attach(6);mm_test_activate_task();
    struct lwext4_mount_adapter *adapter=argument;
    assert(!kernel_vfs_sync_block_device(adapter->block));
    __atomic_store_n(&startup_flush_done,1,__ATOMIC_RELEASE);
    mm_test_finish();return NULL;
}
static void parallel_worker_startup(struct lwext4_mount_adapter *adapter,unsigned failure)
{
    pthread_t starting,flushing;
    startup_expected=failure?-KERNEL_ENOMEM:0;fail_thread_start=failure?2:0;
    startup_pause=2;startup_entered=startup_resume=startup_done=startup_flush_done=0;
    assert(!pthread_create(&starting,NULL,parallel_start_workers,adapter));
    while(!__atomic_load_n(&startup_entered,__ATOMIC_ACQUIRE))sched_yield();
    while(!__atomic_load_n(&adapter->checkpoint_work.head,__ATOMIC_ACQUIRE))sched_yield();
    assert(!adapter->journal_started && !adapter->backend_lock.parallel);
    assert(!pthread_create(&flushing,NULL,parallel_startup_flush,adapter));
    for(;;) {
        unsigned waiting;
        {KERNEL_RAW_SCOPE(guard,&adapter->instance.namespace_lock.lock.metadata);
         waiting=adapter->instance.namespace_lock.lock.pending_head!=NULL;}
        if(waiting)break;
        assert(!__atomic_load_n(&startup_flush_done,__ATOMIC_ACQUIRE));
        sched_yield();
    }
    assert(!startup_done && !startup_flush_done);
    __atomic_store_n(&startup_resume,1,__ATOMIC_RELEASE);
    assert(!pthread_join(starting,NULL) && !pthread_join(flushing,NULL));
    assert(startup_done && startup_flush_done);
    puts(failure?
        "PASS startup ENOMEM: checkpoint joins before serial reuse; queued global block flush completes and the next startup can retry":
        "PASS startup publication: real checkpoint waits for both workers; global block flush queues before parallel activation and completes afterwards");
}
static void *parallel_drain(void *argument)
{
    mm_test_attach(4);mm_test_activate_task();
    struct lwext4_mount_adapter *adapter=argument;
    assert(!ext4_journal_group_drain(adapter->mount_point));
    mm_test_finish();return NULL;
}
static void *parallel_lookup(void *argument)
{
    mm_test_attach(3);mm_test_activate_task();kernel_cpu_current()->logical_id=1;
    struct parallel_actor *actor=argument;
    struct kernel_vfs_stat stat;
    assert(!kernel_vfs_fstat(actor->existing,&stat) && (stat.mode&0777U)==actor->mode);
    mm_test_finish();return NULL;
}
static void worker_checkpoint_lookup(struct lwext4_mount_adapter *adapter,struct parallel_actor *actor)
{
    pthread_t draining,lookup;
    assert(!pthread_create(&draining,NULL,parallel_drain,adapter));
    while(!__atomic_load_n(&checkpoint_entered,__ATOMIC_ACQUIRE))sched_yield();
    /* The actual CPU0 checkpoint owns immutable accepted images. A CPU1
     * FD query must finish while that worker is paused before its home I/O. */
    assert(!pthread_create(&lookup,NULL,parallel_lookup,actor) && !pthread_join(lookup,NULL));
    __atomic_store_n(&checkpoint_resume,1,__ATOMIC_RELEASE);
    assert(!pthread_join(draining,NULL));
}
static void inode_location(struct lwext4_mount_adapter *adapter,uint32_t inode,
    uint64_t *lba,uint32_t *offset)
{
    adapter->locks.read_lock(adapter->locks.context);
    struct ext4_inode_ref ref;
    assert(!ext4_fs_get_inode_ref(adapter->device.fs,inode,&ref));
    *lba=ref.block.lb_id;*offset=(uint32_t)((uint8_t *)ref.inode-ref.block.data);
    assert(!ext4_fs_put_inode_ref(&ref));
    adapter->locks.unlock(adapter->locks.context);
}
static void run_pair(struct lwext4_mount_adapter *adapter,struct parallel_actor actors[2])
{
    pthread_t threads[2];
    assert(!pthread_create(&threads[0],NULL,parallel_modify,&actors[0]));
    wait_entered(&actors[0]);
    assert(!pthread_create(&threads[1],NULL,parallel_modify,&actors[1]));
    wait_entered(&actors[1]);
    assert(actors[0].operation!=actors[1].operation && actors[0].resource!=actors[1].resource);
    assert(actors[0].lba==actors[1].lba && actors[0].offset!=actors[1].offset);
    ext4_fs_state_lock(adapter->device.fs);
    assert(adapter->device.fs->active_operations==2);
    ext4_fs_state_unlock(adapter->device.fs);
    checkpoint_entered=checkpoint_resume=0;
    __atomic_store_n(&checkpoint_pause,1,__ATOMIC_RELEASE);
    __atomic_store_n(&actors[1].resume,1,__ATOMIC_RELEASE);wait_done(&actors[1]);
    assert(!actors[0].done);
    worker_checkpoint_lookup(adapter,&actors[1]);
    ext4_fs_state_lock(adapter->device.fs);
    assert(adapter->device.fs->active_operations==1 && adapter->device.fs->jbd_journal->durable_sequence);
    ext4_fs_state_unlock(adapter->device.fs);
    __atomic_store_n(&actors[0].resume,1,__ATOMIC_RELEASE);wait_done(&actors[0]);
    for(unsigned i=0;i<2;i++)assert(!pthread_join(threads[i],NULL));
    assert(!ext4_journal_group_drain(adapter->mount_point));
    for(unsigned i=0;i<2;i++) {
        struct kernel_vfs_stat stat;
        assert(!kernel_vfs_fstat(actors[i].existing,&stat));
        assert((stat.mode&0777U)==(actors[i].abort?0600U:actors[i].mode));
    }
}
int main(int argc,char **argv)
{
    assert(argc==2);setvbuf(stdout,NULL,_IONBF,0);mm_test_attach(0);mm_test_activate_task();
    struct physical_page_allocator pages={0},backend_pages={0};
    struct kernel_heap heap={0},backend_heap={0};
    initialize(&pages,&heap,arena,sizeof(arena));initialize(&backend_pages,&backend_heap,backend,sizeof(backend));
    uint64_t available=physical_page_available(&pages),backend_available=physical_page_available(&backend_pages);
    image=open(argv[1],O_RDWR);assert(image>=0);struct stat info;assert(!fstat(image,&info));
    struct kernel_page_cache cache={0};assert(!kernel_page_cache_init(&cache,&heap,&pages));
    struct kernel_block_device disk={.read=read_image,.write=write_image,.flush=flush_image,
        .capacity_bytes=info.st_size,.logical_block_size=512,.cache_mode=KERNEL_BLOCK_CACHE_WRITEBACK};
    struct kernel_vfs_mount mount={0};
    assert(!kernel_block_register(&disk,KERNEL_BLOCK_DEVICE_NUMBER(105)) &&
           !kernel_vfs_mount_ext4(&mount,&disk,&backend_heap,&cache,0));
    struct lwext4_mount_adapter *adapter=mount.private_data;struct ext4_fs *fs=adapter->device.fs;
    assert(fs->jbd_journal && fs->orphan_index && !fs->active_operations);
    parallel_worker_startup(adapter,1);
    assert(!adapter->journal_started && !adapter->checkpoint_started &&
           !adapter->backend_lock.parallel && !fs->jbd_journal->grouped && !fs->active_operations);
    parallel_worker_startup(adapter,0);
    assert(adapter->journal_started && adapter->checkpoint_started && adapter->backend_lock.parallel);
    struct kernel_vfs_path *root=NULL;
    assert(!kernel_vfs_path_root(&mount,&heap,&root));
    struct kernel_vfs_file files[8]={0};
    char names[8][32];uint64_t lbas[8];uint32_t offsets[8];
    unsigned first=UINT32_MAX,second=UINT32_MAX;
    for(unsigned i=0;i<8;i++) {
        snprintf(names[i],sizeof(names[i]),"packed-%u",i);
        assert(!kernel_vfs_create_at(root,root,names[i],0600,&files[i]));
        inode_location(adapter,kernel_vfs_file_inode(&files[i]),&lbas[i],&offsets[i]);
        for(unsigned j=0;j<i;j++)if(lbas[j]==lbas[i] && first==UINT32_MAX) {first=j;second=i;}
    }
    assert(first!=UINT32_MAX && offsets[first]!=offsets[second]);
    assert(!ext4_journal_group_drain(adapter->mount_point));
    for(unsigned cancel=0;cancel<2;cancel++) {
        struct parallel_actor actors[2]={{.existing=&files[first],.actor=0,.abort=cancel,.mode=0640},
                                        {.existing=&files[second],.actor=1,.mode=0620}};
        run_pair(adapter,actors);
        printf("PASS real packed inode FD writers: simultaneous private operations; B checkpoints while A %s; CPU1 fstat completes during CPU0 checkpoint\n",cancel?"aborts":"remains active");
        for(unsigned i=0;i<2;i++)assert(!kernel_vfs_file_set_mode(actors[i].existing,0600));
        assert(!ext4_journal_group_drain(adapter->mount_point));
    }
    for(unsigned i=0;i<8;i++)assert(!kernel_vfs_close(&files[i]) &&
                                  !kernel_vfs_unlink_at(root,root,names[i],0));
    assert(!ext4_journal_group_drain(adapter->mount_point) && checkpoint_calls>=2);
    assert(!kernel_vfs_path_release(&root) && !kernel_vfs_unmount(&mount) &&
           !kernel_page_cache_destroy(&cache) && !kernel_block_unregister(&disk));
    struct kernel_heap_statistics statistics;
    kernel_heap_get_statistics(&heap,&statistics);assert(!statistics.live_allocations && !statistics.current_pages);
    kernel_heap_get_statistics(&backend_heap,&statistics);assert(!statistics.live_allocations && !statistics.current_pages);
    assert(!physical_page_allocator_audit(&pages) && !physical_page_allocator_audit(&backend_pages) &&
           physical_page_available(&pages)==available && physical_page_available(&backend_pages)==backend_available);
    assert(!close(image));mm_test_finish();
    puts("PASS real parallel FD backend: workers stopped and all mount, inode, resource, cache and allocator owners returned");
    return 0;
}
