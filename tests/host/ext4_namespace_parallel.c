#define main original_file_io_main
#include "file_io_concurrency.c"
#undef main
#include <ext4_fs.h>
#include <ext4_journal.h>
#include <ext4_inode.h>
#include <ext4_super.h>
#include <ext4_block_group.h>
#include <kernel/time.h>
#include <kernel/credentials.h>
#include <stdlib.h>

static unsigned stat_lookup_watch,stat_lookup_reads,stat_lookup_opens,stat_lookup_io_locks,stat_lookup_allocations;
static unsigned borrowed_path_watch,borrowed_path_state_locks;
static _Thread_local unsigned inode_geometry_watch,inode_geometry_reads,inode_geometry_fail;
static uint64_t inode_geometry_block;
static struct ext4_blockdev *inode_geometry_device;
int __real_ext4_block_get_range(struct ext4_blockdev *,struct ext4_block *,uint64_t,uint32_t,uint32_t,enum ext4_block_access);
int __wrap_ext4_block_get_range(struct ext4_blockdev *device,struct ext4_block *block,
    uint64_t lba,uint32_t offset,uint32_t length,enum ext4_block_access access)
{
    if (inode_geometry_watch && device==inode_geometry_device && lba==inode_geometry_block) {
        inode_geometry_reads++;
        if (inode_geometry_fail) return EIO;
    }
    return __real_ext4_block_get_range(device,block,lba,offset,length,access);
}
void __real_kernel_raw_lock_acquire(struct kernel_raw_lock *,struct kernel_raw_guard *);
void __wrap_kernel_raw_lock_acquire(struct kernel_raw_lock *lock,struct kernel_raw_guard *guard)
{
    if (borrowed_path_watch && lock==&kernel_vfs_state_lock) borrowed_path_state_locks++;
    __real_kernel_raw_lock_acquire(lock,guard);
}
static struct kernel_rwlock *removed_directory_lock;
static unsigned removed_entered,removed_continue;
static _Thread_local int removed_walker;
enum kernel_heap_status __real_kernel_heap_allocate_zeroed(struct kernel_heap *,size_t,size_t,void **);
enum kernel_heap_status __wrap_kernel_heap_allocate_zeroed(struct kernel_heap *heap,size_t n,size_t size,void **out)
{
    if (stat_lookup_watch)stat_lookup_allocations++;
    return __real_kernel_heap_allocate_zeroed(heap,n,size,out);
}
void __real_kernel_rwlock_read(struct kernel_rwlock *,struct kernel_lock_guard *);
void __wrap_kernel_rwlock_read(struct kernel_rwlock *lock,struct kernel_lock_guard *guard)
{
    if (stat_lookup_watch && lock->rank==30) stat_lookup_io_locks++;
    if (removed_walker && lock==removed_directory_lock) {
        removed_walker=0;__atomic_store_n(&removed_entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&removed_continue,__ATOMIC_ACQUIRE))sched_yield();
    }
    __real_kernel_rwlock_read(lock,guard);
}
int __real_ext4_fs_get_inode_ref_read(struct ext4_fs *,uint32_t,struct ext4_inode_ref *);
int __wrap_ext4_fs_get_inode_ref_read(struct ext4_fs *fs,uint32_t inode,struct ext4_inode_ref *ref)
{
    if (stat_lookup_watch) stat_lookup_reads++;
    return __real_ext4_fs_get_inode_ref_read(fs,inode,ref);
}
int __real_ext4_fopen_inode(ext4_file *,const char *,uint32_t,struct ext4_inode *);
int __wrap_ext4_fopen_inode(ext4_file *file,const char *mount,uint32_t inode,struct ext4_inode *initial)
{
    if (stat_lookup_watch) stat_lookup_opens++;
    return __real_ext4_fopen_inode(file,mount,inode,initial);
}
static void inode_geometry_reuse_case(struct lwext4_mount_adapter *adapter)
{
    struct ext4_fs *fs=adapter->device.fs;
    const uint32_t number=EXT4_INODE_ROOT_INDEX;
    uint32_t index=(number-1)/ext4_get32(&fs->sb,inodes_per_group);
    adapter->locks.lock(adapter->locks.context);
    struct ext4_block_group_ref group;
    assert(!ext4_fs_get_block_group_ref(fs,index,&group));
    inode_geometry_device=fs->bdev;inode_geometry_block=group.block.lb_id;
    assert(!ext4_fs_put_block_group_ref(&group));
    ext4_fs_reset_inode_locations(fs);
    inode_geometry_watch=1;inode_geometry_reads=0;inode_geometry_fail=1;
    struct ext4_inode_ref failed;
    assert(ext4_fs_get_inode_ref_read(fs,number,&failed)==EIO);
    assert(ext4_fs_get_inode_ref_read(fs,number,&failed)==EIO && inode_geometry_reads==2);
    inode_geometry_reads=0;inode_geometry_fail=0;
    for(unsigned i=0;i<32;i++) {
        struct ext4_inode_ref inode;
        assert(!ext4_fs_get_inode_ref_read(fs,number,&inode) && inode.index==number);
        assert(!ext4_fs_put_inode_ref(&inode));
    }
    inode_geometry_watch=0;
    printf("inode location reuse: GDT reads=%u\n",inode_geometry_reads);
    assert(inode_geometry_reads<=1);
    assert(!ext4_fs_get_block_group_ref_write(fs,index,&group));
    uint16_t checksum=group.block_group->checksum;
    group.block_group->checksum^=1;
    struct ext4_inode_ref inode;
    assert(ext4_fs_get_inode_ref_read(fs,number,&inode)==EUCLEAN);
    group.block_group->checksum=checksum;
    uint64_t table=ext4_bg_get_inode_table_first_block(group.block_group,&fs->sb);
    ext4_bg_set_inode_table_first_block(group.block_group,&fs->sb,ext4_sb_get_blocks_cnt(&fs->sb));
    ext4_fs_update_bg_csum(&fs->sb,index,group.block_group);
    assert(ext4_fs_get_inode_ref_read(fs,number,&inode)==EUCLEAN);
    ext4_bg_set_inode_table_first_block(group.block_group,&fs->sb,table);
    group.block_group->checksum=checksum;
    assert(!ext4_fs_put_block_group_ref(&group));
    inode_geometry_watch=1;inode_geometry_reads=0;
    for(unsigned i=0;i<32;i++) {
        assert(!ext4_fs_get_inode_ref_read(fs,number,&inode));
        assert(!ext4_fs_put_inode_ref(&inode));
    }
    inode_geometry_watch=0;
    assert(inode_geometry_reads==32);
    adapter->locks.unlock(adapter->locks.context);
    puts("PASS inode location reuse: pristine facts, writable GDT corruption and original verification fallback");
}
static void directory_reuse_case(struct kernel_vfs_path *root)
{
    borrowed_path_watch=1;borrowed_path_state_locks=0;
    for (unsigned i=0;i<32;i++) {
        struct kernel_vfs_path *borrowed=root;
        assert(!kernel_vfs_path_acquire(borrowed) && !kernel_vfs_path_release(&borrowed));
    }
    borrowed_path_watch=0;
    printf("borrowed path returns: state locks=%u\n",borrowed_path_state_locks);
    assert(!borrowed_path_state_locks);
    struct kernel_vfs_file f={0};struct kernel_vfs_stat before,after;
    struct kernel_vfs_instance *instance=root->file.mount->private_data;
    assert(!kernel_vfs_mkdir_at(root,root,"reuse-parent",0755,0));
    assert(!kernel_vfs_mkdir_at(root,root,"reuse-parent/child",0700,0));
    assert(!kernel_vfs_create_at(root,root,"reuse-parent/child/file",0600,&f,0));
    assert(!kernel_vfs_close(&f));
    assert(!kernel_vfs_stat_at(root,root,"reuse-parent/child/file",1,&before));
    stat_lookup_watch=1;stat_lookup_reads=stat_lookup_opens=stat_lookup_io_locks=stat_lookup_allocations=0;
    for(unsigned i=0;i<32;i++) {
        assert(!kernel_vfs_stat_at(root,root,"reuse-parent/child/file",1,&after));
        assert(after.ino==before.ino && after.mode==before.mode);
    }
    stat_lookup_watch=0;
    printf("directory reuse: inode reads=%u opens=%u inode I/O locks=%u\n",stat_lookup_reads,stat_lookup_opens,stat_lookup_io_locks);
    assert(!stat_lookup_opens && !stat_lookup_reads);
    assert(stat_lookup_io_locks==32);
    printf("path query zeroed allocations=%u\n",stat_lookup_allocations);
    assert(!stat_lookup_allocations);
    struct kernel_credentials *lower=kernel_credentials_clone(instance->heap,kernel_credentials_current());
    assert(lower);lower->fsuid=70001;lower->fsgid=70001;lower->effective=0;
    const struct kernel_credentials *previous=kernel_credentials_override(lower);
    assert(kernel_vfs_stat_at(root,root,"reuse-parent/child/file",1,&after)==-KERNEL_EACCES);
    assert(kernel_credentials_override(previous)==lower);
    struct kernel_vfs_path *held=NULL;
    assert(!kernel_vfs_path_resolve(root,root,"reuse-parent/child",1,&held));
    assert(!kernel_vfs_path_set_mode(held,0711));
    previous=kernel_credentials_override(lower);
    assert(!kernel_vfs_stat_at(root,root,"reuse-parent/child/file",1,&after));
    assert(kernel_credentials_override(previous)==lower);kernel_credentials_put(lower);
    assert(!kernel_vfs_stat_at(root,root,"reuse-parent/child",1,&after) && (after.mode&0777)==0711);
    assert(!kernel_vfs_rename_at(root,root,root,"reuse-parent","reuse-renamed",0));
    assert(kernel_vfs_stat_at(root,root,"reuse-parent/child/file",1,&after)==-KERNEL_ENOENT);
    assert(!kernel_vfs_stat_at(root,root,"reuse-renamed/child/file",1,&after) && after.ino==before.ino);
    (void)kernel_vfs_reclaim_clean(instance->heap->page_allocator,1);
    assert(!instance->dentry_cache_count);
    assert(!kernel_vfs_stat_at(held,root,"file",1,&after) && after.ino==before.ino);
    assert(!kernel_vfs_path_release(&held));
    assert(!kernel_vfs_unlink_at(root,root,"reuse-renamed/child/file",0));
    assert(!kernel_vfs_unlink_at(root,root,"reuse-renamed/child",1));
    assert(!kernel_vfs_mkdir_at(root,root,"reuse-renamed/child",0750,0));
    assert(!kernel_vfs_stat_at(root,root,"reuse-renamed/child",1,&after) && (after.mode&0777)==0750);
    assert(!kernel_vfs_unlink_at(root,root,"reuse-renamed/child",1));
    assert(!kernel_vfs_unlink_at(root,root,"reuse-renamed",1));
    puts("PASS directory reuse: stable lookup, current mode, rename, pressure, detached identity and final reclamation");
}
static void lookup_attributes_case(struct kernel_vfs_path *root)
{
    struct kernel_vfs_file file={0};
    struct kernel_vfs_stat stat,live;
    assert(!kernel_vfs_create_at(root,root,"stat-only",0640,&file,0));
    uint64_t inode=kernel_vfs_file_inode(&file);
    assert(!kernel_vfs_close(&file));
    stat_lookup_watch=1;stat_lookup_reads=stat_lookup_opens=0;
    assert(!kernel_vfs_stat_at(root,root,"stat-only",1,&stat));
    stat_lookup_watch=0;
    assert(stat.ino==inode && (stat.mode&0777)==0640 && !stat.size);
    assert(!stat_lookup_reads && !stat_lookup_opens);
    assert(kernel_vfs_stat_at(root,root,"stat-only/",1,&stat)==-KERNEL_ENOTDIR);
    assert(!kernel_vfs_symlink_at(root,root,"stat-only","stat-link"));
    assert(!kernel_vfs_stat_at(root,root,"stat-link",0,&stat) && (stat.mode&KERNEL_VFS_S_IFMT)==KERNEL_VFS_S_IFLNK);
    assert(!kernel_vfs_stat_at(root,root,"stat-link",1,&stat) && stat.ino==inode);
    assert(kernel_vfs_stat_at(root,root,"stat-link/",0,&stat)==-KERNEL_ENOTDIR);
    assert(kernel_vfs_stat_at(root,root,"stat-absent",1,&stat)==-KERNEL_ENOENT);
    assert(!kernel_vfs_create_at(root,root,"stat-live",0600,&file,0));
    struct kernel_vfs_path *source=NULL;
    assert(!kernel_vfs_path_lookup(root,"stat-live",9,&source));
    assert(!kernel_vfs_link_at(source,root,root,"stat-hard"));
    size_t written;
    assert(!kernel_vfs_pwrite(&file,0,"live EOF",8,&written) && written==8);
    assert(!kernel_vfs_fstat(&file,&live));
    stat_lookup_watch=1;stat_lookup_reads=stat_lookup_opens=0;
    assert(!kernel_vfs_stat_at(root,root,"stat-hard",1,&stat));
    stat_lookup_watch=0;
    printf("live hard-link stat: inode reads=%u opens=%u\n",stat_lookup_reads,stat_lookup_opens);
    assert(stat_lookup_reads==1 && !stat_lookup_opens);
    assert(stat.ino==live.ino && stat.size==8 && stat.blocks==live.blocks &&
        stat.mtime.seconds==live.mtime.seconds && stat.mtime.nanoseconds==live.mtime.nanoseconds);
    assert(!kernel_vfs_path_release(&source) && !kernel_vfs_close(&file));
    struct kernel_vfs_instance *instance=root->file.mount->private_data;
    struct kernel_vfs_mount *mounted=NULL;
    struct kernel_vfs_path *covered=NULL,*visible=NULL;
    assert(!kernel_vfs_mkdir_at(root,root,"stat-covered",0700,0));
    assert(!kernel_vfs_path_lookup(root,"stat-covered",12,&covered));
    assert(!kernel_tmpfs_create(instance->heap,0,"",&mounted) && !kernel_vfs_mount_attach(mounted,covered));
    assert(!kernel_vfs_path_resolve(root,root,"stat-covered",1,&visible) && !kernel_vfs_path_stat(visible,&live));
    assert(!kernel_vfs_stat_at(root,root,"stat-covered",1,&stat) && stat.dev==live.dev && stat.ino==live.ino);
    assert(!kernel_vfs_path_release(&visible) && !kernel_vfs_mount_prepare_detach(mounted,NULL) &&
        !kernel_vfs_mount_detach(mounted,NULL) && !kernel_vfs_unmount(mounted));
    assert(!kernel_vfs_path_release(&covered) && !kernel_vfs_unlink_at(root,root,"stat-covered",1));
    const char *names[]={"stat-only","stat-link","stat-live","stat-hard"};
    for(unsigned i=0;i<4;i++) assert(!kernel_vfs_unlink_at(root,root,names[i],0));
    puts("PASS final stat reuses one directory/inode lookup without opening the child; symlink, trailing slash and live hard-link attributes preserved");
}

static void directory_eof_case(struct kernel_vfs_path *root)
{
    struct kernel_vfs_file dir = {0}, child = {0};
    const struct kernel_vfs_open_request request = {.flags=KERNEL_VFS_OPEN_DIRECTORY};
    assert(!kernel_vfs_mkdir_at(root,root,"enumerated",0700,0));
    assert(!kernel_vfs_open_at(root,root,"enumerated",&request,&dir,NULL));
    struct kernel_vfs_stat stat;
    assert(!kernel_vfs_fstat(&dir,&stat));
    unsigned char records[512];
    struct kernel_vfs_dir_batch batch = {.data=records,.capacity=sizeof(records)};
    assert(!kernel_vfs_read_directory(&dir,&batch,NULL) && batch.used);
    uint64_t original_eof = batch.position;
    assert(original_eof==stat.size);
    batch.used=0;
    stat_lookup_watch=1;stat_lookup_reads=0;
    assert(!kernel_vfs_read_directory(&dir,&batch,NULL) && !batch.used);
    stat_lookup_watch=0;
    assert(!stat_lookup_reads && "unchanged directory EOF should reuse accepted inode size");

    char name[244];
    memset(name,'n',sizeof(name)-1);
    for (unsigned i=0;i<32;i++) {
        snprintf(name+sizeof(name)-3,3,"%02u",i);
        assert(!kernel_vfs_create_at(dir.path,root,name,0600,&child,0));
        assert(!kernel_vfs_close(&child));
    }
    assert(kernel_vfs_read_directory(&dir,&batch,NULL)>=0 && batch.used && batch.position>original_eof);
    for (unsigned i=0;i<32;i++) {
        snprintf(name+sizeof(name)-3,3,"%02u",i);
        assert(!kernel_vfs_unlink_at(dir.path,root,name,0));
    }
    assert(!kernel_vfs_close(&dir));
    assert(!kernel_vfs_unlink_at(root,root,"enumerated",1));
    puts("PASS directory EOF reuses accepted size and observes subsequent growth");
}
static void inode_attribute_lifecycle(struct lwext4_mount_adapter *adapter,struct kernel_vfs_path *root)
{
    struct kernel_vfs_file file={0};struct kernel_vfs_path *path=NULL;
    struct kernel_vfs_stat stat;
    assert(!kernel_vfs_create_at(root,root,"attributes",0600,&file,0));
    assert(!kernel_vfs_path_lookup(root,"attributes",10,&path));
    assert(!kernel_vfs_fstat(&file,&stat) && stat.nlink==1 && !stat.size);
    assert(!kernel_vfs_file_set_mode(&file,0640));
    assert(!kernel_vfs_file_set_owner(&file,70001,70002));
    assert(!kernel_vfs_fstat(&file,&stat) && (stat.mode&0777)==0640 &&
        stat.uid==70001 && stat.gid==70002);
    assert(!kernel_vfs_link_at(path,root,root,"attributes-link"));
    assert(!kernel_vfs_fstat(&file,&stat) && stat.nlink==2);
    assert(!kernel_vfs_unlink_at(root,root,"attributes-link",0));
    assert(!kernel_vfs_fstat(&file,&stat) && stat.nlink==1);
    size_t written;
    assert(!kernel_vfs_pwrite(&file,0,"content",7,&written) && written==7);
    assert(!kernel_vfs_fstat(&file,&stat) && stat.size==7 && stat.blocks);
    assert(!kernel_page_cache_writeback(kernel_vfs_file_page_cache(&file),file.private_data));
    assert(!ext4_journal_group_drain(adapter->mount_point));
    assert(!kernel_vfs_fstat(&file,&stat) && stat.size==7 && stat.blocks);
    assert(!kernel_vfs_ftruncate(&file,0));
    assert(!kernel_vfs_fstat(&file,&stat) && !stat.size && !stat.blocks);
    assert(!kernel_vfs_unlink_at(root,root,"attributes",0));
    assert(!kernel_vfs_fstat(&file,&stat) && !stat.nlink);
    assert(!kernel_vfs_path_release(&path) && !kernel_vfs_close(&file));
    puts("PASS live inode attributes: chmod/chown, link/unlink, delayed/allocated blocks, truncate and unlinked FD");
}

struct removed_walk { struct kernel_vfs_path *root;uint64_t inode; };
static void *walk_removed_directory(void *argument)
{
    struct removed_walk *test=argument;struct kernel_vfs_path *found=NULL;
    mm_test_attach(7);mm_test_activate_task();kernel_cpu_current()->logical_id=1;
    removed_walker=1;
    assert(!kernel_vfs_path_resolve(test->root,test->root,"walk-removed/../walk-stable",1,&found));
    assert(kernel_vfs_path_inode(found)==test->inode && !kernel_vfs_path_release(&found));
    mm_test_finish();return NULL;
}
static void removed_directory_qualification(struct kernel_vfs_path *root)
{
    struct kernel_vfs_path *directory=NULL;struct kernel_vfs_file file={0};
    assert(!kernel_vfs_mkdir_at(root,root,"walk-removed",0700,0));
    assert(!kernel_vfs_path_resolve(root,root,"walk-removed",1,&directory));
    removed_directory_lock=&((struct kernel_vfs_node *)directory->file.private_data)->namespace_lock;
    assert(!kernel_vfs_path_release(&directory));
    assert(!kernel_vfs_create_at(root,root,"walk-stable",0600,&file,0));
    struct removed_walk test={root,kernel_vfs_file_inode(&file)};
    assert(!kernel_vfs_close(&file));removed_entered=removed_continue=0;
    pthread_t walking;assert(!pthread_create(&walking,NULL,walk_removed_directory,&test));
    while (!__atomic_load_n(&removed_entered,__ATOMIC_ACQUIRE))sched_yield();
    assert(!kernel_vfs_unlink_at(root,root,"walk-removed",1));
    __atomic_store_n(&removed_continue,1,__ATOMIC_RELEASE);
    assert(!pthread_join(walking,NULL) && !kernel_vfs_unlink_at(root,root,"walk-stable",0));
    removed_directory_lock=NULL;
    puts("PASS removed intermediate directory: last dentry close follows namespace qualification return");
}

struct parallel_actor {
    struct kernel_vfs_path *parent,*root;
    struct kernel_vfs_file file,*existing;
    struct ext4_operation *operation;
    struct kernel_rwlock *resource;
    uint64_t lba;
    uint32_t offset,mode;
    unsigned actor,entered,resume,done,abort;
    int error;
};
static _Thread_local struct parallel_actor *parallel_current;
static _Thread_local unsigned orphan_order_watch, orphan_order_aborts;
static _Thread_local unsigned rename_order_watch,rename_order_aborts,rename_order_retries;
void __real_jbd_journal_free_trans_state_held(struct jbd_journal *,struct jbd_trans *,bool);
void __wrap_jbd_journal_free_trans_state_held(struct jbd_journal *journal,struct jbd_trans *trans,bool abort)
{
    if(orphan_order_watch && abort)orphan_order_aborts++;
    if(rename_order_watch && abort) {
        rename_order_aborts++;
        if(kernel_io_context_current()->backend_resources.retry)rename_order_retries++;
    }
    __real_jbd_journal_free_trans_state_held(journal,trans,abort);
}
static unsigned worker_mode,checkpoint_pause,checkpoint_entered,checkpoint_resume,checkpoint_calls;
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
    if (worker_mode) {
        assert(kernel_io_context_current()->background_reclaim &&
               !kernel_cpu_current()->logical_id && !kernel_io_context_current()->backend_state_depth);
        __atomic_fetch_add(&checkpoint_calls,1,__ATOMIC_RELAXED);
        if (__atomic_exchange_n(&checkpoint_pause,0,__ATOMIC_ACQ_REL)) {
            __atomic_store_n(&checkpoint_entered,1,__ATOMIC_RELEASE);
            while(!__atomic_load_n(&checkpoint_resume,__ATOMIC_ACQUIRE))sched_yield();
        }
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
    assert(actor->resource->rank==(actor->existing?31U:35U));
    __atomic_store_n(&actor->entered,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&actor->resume,__ATOMIC_ACQUIRE))sched_yield();
    return actor->abort?ECANCELED:result;
}
static void *parallel_modify(void *argument)
{
    struct parallel_actor *actor=argument;
    mm_test_attach(actor->actor+1);mm_test_activate_task();kernel_cpu_current()->logical_id=actor->actor;
    parallel_current=actor;
    if(actor->existing)actor->error=kernel_vfs_file_set_mode(actor->existing,actor->mode);
    else {
        const struct kernel_vfs_open_request request={.flags=KERNEL_VFS_OPEN_CREATE|
            KERNEL_VFS_OPEN_EXCLUSIVE|KERNEL_VFS_OPEN_FOLLOW,.mode=0600};
        actor->error=kernel_vfs_open_at(actor->parent,actor->root,"created",&request,&actor->file,NULL);
    }
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
static uint64_t parallel_now(void *unused)
{ (void)unused;return kernel_time_monotonic_ns(); }
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
    assert(!adapter->journal_started && !adapter->backend_lock.parallel && !adapter->instance.namespace_parallel);
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
    struct kernel_vfs_path *found=NULL;
    if (actor->existing) {
        struct kernel_vfs_stat stat;
        assert(!kernel_vfs_fstat(actor->existing,&stat) && (stat.mode&0777U)==actor->mode);
    } else {
        assert(!kernel_vfs_path_lookup(actor->parent,"created",7,&found) && found==actor->file.path);
        assert(!kernel_vfs_path_release(&found));
    }
    mm_test_finish();return NULL;
}
static void worker_checkpoint_lookup(struct lwext4_mount_adapter *adapter,struct parallel_actor *actor)
{
    pthread_t draining,lookup;
    assert(!pthread_create(&draining,NULL,parallel_drain,adapter));
    while(!__atomic_load_n(&checkpoint_entered,__ATOMIC_ACQUIRE))sched_yield();
    /* The actual CPU0 checkpoint owns immutable accepted images. A CPU1
     * lookup must finish while that worker is paused before its home I/O. */
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
static uint64_t group_location(struct lwext4_mount_adapter *adapter,uint32_t group)
{
    adapter->locks.read_lock(adapter->locks.context);
    struct ext4_block_group_ref ref;
    assert(!ext4_fs_get_block_group_ref(adapter->device.fs,group,&ref));
    uint64_t lba=ref.block.lb_id;
    assert(!ext4_fs_put_block_group_ref(&ref));
    adapter->locks.unlock(adapter->locks.context);return lba;
}
static void run_pair(struct lwext4_mount_adapter *adapter,struct parallel_actor actors[2],
    int same_group,int freeze)
{
    pthread_t threads[2];
    assert(!pthread_create(&threads[0],NULL,parallel_modify,&actors[0]));
    wait_entered(&actors[0]);
    assert(!pthread_create(&threads[1],NULL,parallel_modify,&actors[1]));
    if(same_group) {
        for(;;) {
            unsigned waiting;
            {KERNEL_RAW_SCOPE(guard,&actors[0].resource->metadata);
             waiting=actors[0].resource->pending_head!=NULL;}
            if(waiting)break;
            assert(!__atomic_load_n(&actors[1].entered,__ATOMIC_ACQUIRE) &&
                   !__atomic_load_n(&actors[1].done,__ATOMIC_ACQUIRE));
            sched_yield();
        }
        __atomic_store_n(&actors[0].resume,1,__ATOMIC_RELEASE);wait_done(&actors[0]);
        wait_entered(&actors[1]);
        assert(actors[0].resource==actors[1].resource);
    } else {
        wait_entered(&actors[1]);
        assert(actors[0].operation!=actors[1].operation && actors[0].resource!=actors[1].resource);
        ext4_fs_state_lock(adapter->device.fs);
        assert(adapter->device.fs->active_operations==2);
        ext4_fs_state_unlock(adapter->device.fs);
    }
    if (freeze && worker_mode) {
        checkpoint_entered=checkpoint_resume=0;
        __atomic_store_n(&checkpoint_pause,1,__ATOMIC_RELEASE);
    }
    __atomic_store_n(&actors[1].resume,1,__ATOMIC_RELEASE);wait_done(&actors[1]);
    if(freeze) {
        assert(!same_group && !actors[0].done);
        if(worker_mode)worker_checkpoint_lookup(adapter,&actors[1]);
        else assert(!ext4_journal_group_commit(adapter->mount_point,true));
        ext4_fs_state_lock(adapter->device.fs);
        assert(adapter->device.fs->active_operations==1 && adapter->device.fs->jbd_journal->durable_sequence);
        ext4_fs_state_unlock(adapter->device.fs);
    }
    __atomic_store_n(&actors[0].resume,1,__ATOMIC_RELEASE);wait_done(&actors[0]);
    for(unsigned i=0;i<2;i++)assert(!pthread_join(threads[i],NULL));
    assert(!ext4_journal_group_drain(adapter->mount_point));
    for(unsigned i=0;i<2;i++) {
        struct kernel_vfs_stat stat;
        if(actors[i].existing) {
            assert(!kernel_vfs_fstat(actors[i].existing,&stat));
            assert((stat.mode&0777U)==(actors[i].abort?0600U:actors[i].mode));
        } else {
            struct kernel_vfs_path *found=NULL;
            int result=kernel_vfs_path_lookup(actors[i].parent,"created",7,&found);
            if(actors[i].abort)assert(result==-KERNEL_ENOENT && !found && !actors[i].file.private_data);
            else {
                assert(!result && found==actors[i].file.path);
                assert(!kernel_vfs_path_release(&found) && !kernel_vfs_close(&actors[i].file));
                assert(!kernel_vfs_unlink_at(actors[i].parent,actors[i].root,"created",0));
            }
        }
    }
    assert(!ext4_journal_group_drain(adapter->mount_point));
}
struct rename_inode_actor {
    struct kernel_vfs_path *root;
    struct kernel_rwlock *resource;
    unsigned waiting,done;
    int result;
};
static _Thread_local struct rename_inode_actor *rename_inode_current;
static void rename_inode_wait(const struct kernel_wait_token *token)
{
    struct rename_inode_actor *actor=rename_inode_current;
    if(!actor)return;
    struct kernel_wait_queue *queue;
    {KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
     queue=kernel_wait_default_node(token->task)->queue;}
    if(queue==&actor->resource->waiters)
        __atomic_store_n(&actor->waiting,1,__ATOMIC_RELEASE);
}
static void *rename_inode_run(void *argument)
{
    struct rename_inode_actor *actor=argument;
    mm_test_attach(3);mm_test_activate_task();kernel_cpu_current()->logical_id=1;
    rename_inode_current=actor;
    actor->result=kernel_vfs_rename_at(actor->root,actor->root,actor->root,
        "rename31-source","rename31-target",0);
    rename_inode_current=NULL;
    __atomic_store_n(&actor->done,1,__ATOMIC_RELEASE);
    mm_test_finish();return NULL;
}
static void rename_inode_case(struct lwext4_mount_adapter *adapter,struct kernel_vfs_path *root)
{
    for(unsigned abort=0;abort<2;abort++) {
        struct kernel_vfs_file file={0};size_t done;
        assert(!kernel_vfs_create_at(root,root,"rename31-source",0600,&file,0));
        assert(!kernel_vfs_pwrite(&file,0,"original",8,&done) && done==8 &&
               !ext4_journal_group_drain(adapter->mount_point));
        struct kernel_vfs_stat before,after;
        assert(!kernel_vfs_fstat(&file,&before));
        struct parallel_actor modifier={.existing=&file,.actor=0,.mode=0640,.abort=abort};
        pthread_t modifying,renaming;
        assert(!pthread_create(&modifying,NULL,parallel_modify,&modifier));wait_entered(&modifier);
        assert(modifier.resource->rank==31 && modifier.resource->key==kernel_vfs_file_inode(&file));
        /* Rename first waits for the source VFS data owner, before its
         * backend inode qualifier. Observe that actual first wait. */
        struct rename_inode_actor rename={.root=root,.resource=&((struct kernel_vfs_node *)file.private_data)->io_lock};
        assert(!pthread_create(&renaming,NULL,rename_inode_run,&rename));
        while(!__atomic_load_n(&rename.waiting,__ATOMIC_ACQUIRE)) {
            assert(!__atomic_load_n(&rename.done,__ATOMIC_ACQUIRE));sched_yield();
        }
        assert(!__atomic_load_n(&rename.done,__ATOMIC_ACQUIRE));
        __atomic_store_n(&modifier.resume,1,__ATOMIC_RELEASE);
        assert(!pthread_join(modifying,NULL) && !pthread_join(renaming,NULL));
        assert(modifier.error==(abort?-ECANCELED:0) && !rename.result);
        assert(!ext4_journal_group_drain(adapter->mount_point) && !kernel_vfs_fstat(&file,&after));
        assert(after.ino==before.ino && after.nlink==1 && after.size==8 &&
               (after.mode&0777U)==(abort?0600U:0640U) &&
               after.mtime.seconds==before.mtime.seconds && after.mtime.nanoseconds==before.mtime.nanoseconds);
        char bytes[8];assert(!kernel_vfs_pread(&file,0,bytes,sizeof(bytes),&done, NULL) &&
            done==8 && !memcmp(bytes,"original",8));
        struct kernel_vfs_path *found=NULL;
        assert(kernel_vfs_path_lookup(root,"rename31-source",15,&found)==-KERNEL_ENOENT && !found);
        assert(!kernel_vfs_path_lookup(root,"rename31-target",15,&found) &&
               found->file.private_data==file.private_data && !kernel_vfs_path_release(&found));
        assert(!kernel_vfs_close(&file) && !kernel_vfs_unlink_at(root,root,"rename31-target",0) &&
               !ext4_journal_group_drain(adapter->mount_point));
        printf("PASS rename metadata owner: source chmod %s, rename queues on actual inode31; mode/mtime/nlink/open identity and contents retained\n",abort?"aborted":"accepted");
    }
}
struct namespace_loop_actor {
    struct kernel_vfs_path *parent,*root;
    unsigned actor,progress;
};
static unsigned namespace_loop_gate;
static void *namespace_reuse_loop(void *argument)
{
    struct namespace_loop_actor *actor=argument;
    mm_test_attach(actor->actor+1);mm_test_activate_task();kernel_cpu_current()->logical_id=actor->actor;
    for(unsigned round=1;round<=32;round++) {
        while(__atomic_load_n(&namespace_loop_gate,__ATOMIC_ACQUIRE)<round*2-1)sched_yield();
        struct kernel_vfs_file file={0};
        assert(!kernel_vfs_create_at(actor->parent,actor->root,"looped",0600,&file,0) && !kernel_vfs_close(&file));
        __atomic_store_n(&actor->progress,round*2-1,__ATOMIC_RELEASE);
        while(__atomic_load_n(&namespace_loop_gate,__ATOMIC_ACQUIRE)<round*2)sched_yield();
        assert(!kernel_vfs_unlink_at(actor->parent,actor->root,"looped",0));
        __atomic_store_n(&actor->progress,round*2,__ATOMIC_RELEASE);
    }
    mm_test_finish();return NULL;
}
static void namespace_reuse_loop_case(struct lwext4_mount_adapter *adapter,
    struct kernel_vfs_path *first,struct kernel_vfs_path *second,struct kernel_vfs_path *root)
{
    namespace_loop_gate=0;
    struct namespace_loop_actor actors[2]={{.parent=first,.root=root},{.parent=second,.root=root,.actor=1}};
    pthread_t threads[2];
    for(unsigned i=0;i<2;i++)assert(!pthread_create(&threads[i],NULL,namespace_reuse_loop,&actors[i]));
    for(unsigned phase=1;phase<=64;phase++) {
        __atomic_store_n(&namespace_loop_gate,phase,__ATOMIC_RELEASE);
        for(unsigned i=0;i<2;i++)while(__atomic_load_n(&actors[i].progress,__ATOMIC_ACQUIRE)<phase)sched_yield();
        assert(!ext4_journal_group_drain(adapter->mount_point));
    }
    for(unsigned i=0;i<2;i++)assert(!pthread_join(threads[i],NULL));
    puts("PASS independent-directory reuse: 32 paired create/close/unlink rounds cross freeze/checkpoint and inode bitmap reuse");
}
#include "ext4_namespace_parallel_xattr.h"
#include "ext4_namespace_parallel_orphan.h"
#include "ext4_namespace_parallel_notify.h"
#include "ext4_namespace_parallel_reservation.h"
#include "ext4_namespace_parallel_rename.h"
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
    if (getenv("EXT4_NAMESPACE_REUSE_ONLY")) inode_geometry_reuse_case(adapter);
    worker_mode=getenv("EXT4_NAMESPACE_PARALLEL_WORKERS")!=NULL;
    if(worker_mode) {
        parallel_worker_startup(adapter,1);
        assert(!adapter->journal_started && !adapter->checkpoint_started &&
               !adapter->backend_lock.parallel && !adapter->instance.namespace_parallel &&
               !fs->jbd_journal->grouped && !fs->active_operations);
        parallel_worker_startup(adapter,0);
        unsigned parallel=kernel_cpu_get_boot_count()>1;
        assert(adapter->journal_started && adapter->checkpoint_started &&
               !!adapter->backend_lock.parallel==parallel && !!adapter->instance.namespace_parallel==parallel);
    } else {
        const struct ext4_journal_runtime runtime={.now_ns=parallel_now};
        assert(!ext4_journal_group_enable(adapter->mount_point,&runtime,8*1024*1024));
        serial_reservation_case(adapter,1);
        boaros_lwext4_enable_parallel(&adapter->backend_lock,fs);adapter->instance.namespace_parallel=1;
        serial_reservation_case(adapter,0);
    }
    struct kernel_vfs_path *root=NULL,*directories[65]={0};
    assert(!kernel_vfs_path_root(&mount,&heap,&root));
    if (getenv("EXT4_NAMESPACE_REUSE_ONLY")) {
        directory_reuse_case(root);removed_directory_qualification(root);lookup_attributes_case(root);inode_attribute_lifecycle(adapter,root);
        rename_order_case(adapter,root);rename_order_directory_case(adapter,root);
        rename_inode_case(adapter,root);goto Finish;}
    lookup_attributes_case(root);
    directory_eof_case(root);
    if (getenv("EXT4_NAMESPACE_STAT_ONLY")) goto Finish;
    worker_notification_case(adapter,root);
    rename_order_case(adapter,root);
    rename_order_directory_case(adapter,root);
    rename_inode_case(adapter,root);
    unsigned groups=ext4_block_group_cnt(&fs->sb),per_group=ext4_get32(&fs->sb,inodes_per_group);
    assert(groups>=2 && groups<65);
    unsigned first[65];for(unsigned i=0;i<65;i++)first[i]=UINT32_MAX;
    unsigned same[2]={UINT32_MAX,UINT32_MAX},different[2]={0,UINT32_MAX},count=0;
    char names[65][32];
    for(unsigned i=0;i<=groups;i++) {
        snprintf(names[i],sizeof(names[i]),"parallel-%u",i);
        assert(!kernel_vfs_mkdir_at(root,root,names[i],0700,0));
        assert(!kernel_vfs_path_lookup(root,names[i],strlen(names[i]),&directories[i]));count++;
        unsigned group=(kernel_vfs_path_inode(directories[i])-1)/per_group;
        if(first[group]!=UINT32_MAX && same[0]==UINT32_MAX) {same[0]=first[group];same[1]=i;}
        if(first[group]==UINT32_MAX)first[group]=i;
        if(i && group!=(kernel_vfs_path_inode(directories[0])-1)/per_group && different[1]==UINT32_MAX)
            different[1]=i;
    }
    assert(same[0]!=UINT32_MAX && different[1]!=UINT32_MAX);
    unsigned g0=(kernel_vfs_path_inode(directories[different[0]])-1)/per_group;
    unsigned g1=(kernel_vfs_path_inode(directories[different[1]])-1)/per_group;
    assert(group_location(adapter,g0)==group_location(adapter,g1));
    assert(!ext4_journal_group_drain(adapter->mount_point));
    for(unsigned cancel=0;cancel<2;cancel++) {
        struct parallel_actor actors[2]={{.parent=directories[different[0]],.root=root,.actor=0,.abort=cancel},
                                         {.parent=directories[different[1]],.root=root,.actor=1}};
        run_pair(adapter,actors,0,1);
        printf("PASS real different-group create: simultaneous private operations, packed GDT, B accepted/frozen, A %s\n",cancel?"aborted":"accepted");
    }
    struct parallel_actor same_actors[2]={{.parent=directories[same[0]],.root=root,.actor=0},
                                        {.parent=directories[same[1]],.root=root,.actor=1}};
    run_pair(adapter,same_actors,1,0);
    puts("PASS real same-group create: independent parents pass VFS, allocation qualification queues the second operation");
    orphan_normal_order_case(adapter,directories[same[0]],root);
    orphan_snapshot_case(adapter,directories[same[0]],directories[same[1]],root);
    namespace_reuse_loop_case(adapter,directories[different[0]],directories[different[1]],root);
    struct kernel_vfs_file packed[2]={{0},{0}};
    assert(!kernel_vfs_create_at(directories[0],root,"packed-a",0600,&packed[0],0) &&
           !kernel_vfs_create_at(directories[0],root,"packed-b",0600,&packed[1],0));
    uint64_t lbas[2];uint32_t offsets[2];
    for(unsigned i=0;i<2;i++)inode_location(adapter,kernel_vfs_file_inode(&packed[i]),&lbas[i],&offsets[i]);
    assert(lbas[0]==lbas[1] && offsets[0]!=offsets[1]);
    struct parallel_actor packed_actors[2]={{.existing=&packed[0],.actor=0,.abort=1,.mode=0640},
                                          {.existing=&packed[1],.actor=1,.mode=0620}};
    run_pair(adapter,packed_actors,0,1);
    puts("PASS real packed inode records: two inode writers overlap, B survives freeze and A abort");
    for(unsigned i=0;i<2;i++)assert(!kernel_vfs_close(&packed[i]));
    assert(!kernel_vfs_unlink_at(directories[0],root,"packed-a",0) &&
           !kernel_vfs_unlink_at(directories[0],root,"packed-b",0));
    namespace_parallel_xattr_case(adapter,root);
    for(unsigned i=0;i<count;i++)assert(!kernel_vfs_path_release(&directories[i]) &&
                                      !kernel_vfs_unlink_at(root,root,names[i],1));
    assert(!ext4_journal_group_drain(adapter->mount_point));
    if(worker_mode) {
        assert(checkpoint_calls>=3);
        puts("PASS real CPU0 journal/checkpoint workers: CPU1 lookup completes during checkpoint and another private operation");
    }
Finish:
    assert(!kernel_vfs_path_release(&root) && !kernel_vfs_unmount(&mount) &&
           !kernel_page_cache_destroy(&cache) && !kernel_block_unregister(&disk));
    assert(physical_page_available(&pages)==available && physical_page_available(&backend_pages)==backend_available);
    close(image);mm_test_finish();puts("PASS real parallel namespace: all mount, inode, resource and allocator owners returned");
    return 0;
}
