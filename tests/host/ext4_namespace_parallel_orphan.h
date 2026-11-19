#include <ext4_block_group.h>
#include <ext4_orphan.h>

static unsigned observe_unlink, orphan_adds, direct_frees;
static int fail_direct_free;
int __real_ext4_orphan_add(struct ext4_inode_ref *);
int __wrap_ext4_orphan_add(struct ext4_inode_ref *ref)
{
    if (observe_unlink) orphan_adds++;
    return __real_ext4_orphan_add(ref);
}
int __real_ext4_fs_free_inode(struct ext4_inode_ref *);
int __wrap_ext4_fs_free_inode(struct ext4_inode_ref *ref)
{
    int result = __real_ext4_fs_free_inode(ref);
    if (observe_unlink && !orphan_adds) {
        direct_frees++;
        if (fail_direct_free && !result) return fail_direct_free;
    }
    return result;
}

static void orphan_closed_case(struct lwext4_mount_adapter *adapter,
    struct kernel_vfs_path *root)
{
    struct kernel_vfs_file file = {0};
    const int faults[] = {0, ENOMEM, ENOSPC, EIO};
    for (unsigned i = 0; i < sizeof(faults) / sizeof(*faults); ++i) {
        assert(!kernel_vfs_create_at(root, root, "closed-victim", 0600, &file, 0));
        assert(!kernel_vfs_close(&file));
        orphan_adds = direct_frees = 0;
        observe_unlink = 1; fail_direct_free = faults[i];
        assert(!kernel_vfs_unlink_at(root, root, "closed-victim", 0));
        observe_unlink = fail_direct_free = 0;
        assert(direct_frees == 1 && orphan_adds == !!faults[i]);
    }

    assert(!kernel_vfs_mkdir_at(root, root, "ownerless-dir", 0700, 0));
    orphan_adds = direct_frees = 0; observe_unlink = 1;
    assert(!kernel_vfs_unlink_at(root, root, "ownerless-dir", 1));
    observe_unlink = 0;
    assert(direct_frees == 1 && !orphan_adds);

    struct kernel_vfs_path *path = NULL;
    assert(!kernel_vfs_create_at(root, root, "held-victim", 0600, &file, 0));
    assert(!kernel_vfs_close(&file));
    assert(!kernel_vfs_path_lookup(root, "held-victim", 11, &path));
    uint32_t inode = kernel_vfs_path_inode(path);
    orphan_adds = direct_frees = 0; observe_unlink = 1;
    assert(!kernel_vfs_unlink_at(root, root, "held-victim", 0));
    observe_unlink = 0;
    assert(!direct_frees && orphan_adds == 1);
    bool present;
    assert(!ext4_orphan_contains(adapter->device.fs, inode, &present) && present);
    assert(!kernel_vfs_path_open(path, &file) && !kernel_vfs_close(&file));
    assert(!kernel_vfs_path_release(&path));
    assert(!ext4_orphan_contains(adapter->device.fs, inode, &present) && !present);

    assert(!kernel_vfs_create_at(root, root, "dirty-victim", 0600, &file, 0));
    assert(!kernel_vfs_file_acquire_write(&file));
    size_t written;
    assert(!kernel_vfs_pwrite(&file, 0, "dirty", 5, &written) && written == 5);
    struct kernel_vfs_node *node = kernel_vfs_file_node(&file);
    assert(!kernel_vfs_close(&file) && node->open_files == 1 && node->references > 1);
    orphan_adds = direct_frees = 0; observe_unlink = 1;
    assert(!kernel_vfs_unlink_at(root, root, "dirty-victim", 0));
    observe_unlink = 0;
    assert(!direct_frees && orphan_adds == 1);
    puts("PASS closed unlink/rmdir: one operation, injected rollback, held path and cached data");
}

static void orphan_normal_order_case(struct lwext4_mount_adapter *adapter,
    struct kernel_vfs_path *parent,struct kernel_vfs_path *root)
{
    unsigned total=0;
    for(unsigned mapped=0;mapped<2;mapped++) {
        struct kernel_vfs_file file={0};
        assert(!kernel_vfs_create_at(parent,root,"orphan-order",0600,&file,0) &&
               !kernel_vfs_file_acquire_write(&file));
        uint32_t inode=kernel_vfs_file_inode(&file);
        if(mapped) {
            static const char payload[4096]="retained unlinked data";
            size_t written;
            assert(!kernel_vfs_pwrite(&file,0,payload,sizeof(payload),&written) && written==sizeof(payload));
            if (worker_mode) {
                uint64_t observed=kernel_vfs_error_sequence(&file);
                assert(!kernel_vfs_sync(&file,0,&observed));
            } else {
                /* The manual journal model drives its own progress. Submit
                 * delayed data before draining the accepted metadata. */
                assert(!kernel_page_cache_writeback_range(adapter->instance.page_cache,
                    kernel_vfs_file_node(&file),0,UINT64_MAX));
                assert(!ext4_journal_group_drain(adapter->mount_point));
            }
            adapter->locks.read_lock(adapter->locks.context);
            struct ext4_inode_ref ref;
            assert(!ext4_fs_get_inode_ref(adapter->device.fs,inode,&ref) &&
                   ext4_inode_get_blocks_count(&adapter->device.fs->sb,ref.inode));
            assert(!ext4_fs_put_inode_ref(&ref));
            adapter->locks.unlock(adapter->locks.context);
        }
        orphan_order_aborts=0;orphan_order_watch=1;
        assert(!kernel_vfs_unlink_at(parent,root,"orphan-order",0));
        orphan_order_watch=0;
        unsigned unlink_aborts=orphan_order_aborts;
        struct kernel_vfs_path *found=NULL;
        struct kernel_vfs_stat stat;
        assert(kernel_vfs_path_lookup(parent,"orphan-order",12,&found)==-KERNEL_ENOENT && !found);
        assert(!kernel_vfs_fstat(&file,&stat) && !stat.nlink && stat.size==(mapped?4096U:0U));
        bool present=false;
        adapter->locks.read_lock(adapter->locks.context);
        assert(!ext4_orphan_contains(adapter->device.fs,inode,&present) && present);
        adapter->locks.unlock(adapter->locks.context);
        unsigned existing_aborts=0;
        if(!mapped) {
            orphan_order_aborts=0;orphan_order_watch=1;
            assert(!kernel_vfs_ftruncate(&file,0));
            orphan_order_watch=0;existing_aborts=orphan_order_aborts;
        }
        orphan_order_aborts=0;orphan_order_watch=1;
        assert(!kernel_vfs_close(&file));
        orphan_order_watch=0;
        unsigned close_aborts=orphan_order_aborts;
        adapter->locks.read_lock(adapter->locks.context);
        assert(!ext4_orphan_contains(adapter->device.fs,inode,&present) && !present);
        adapter->locks.unlock(adapter->locks.context);
        assert(!ext4_journal_group_drain(adapter->mount_point));
        printf("orphan normal order: mapped=%u unlink_aborts=%u existing_aborts=%u close_aborts=%u\n",
            mapped,unlink_aborts,existing_aborts,close_aborts);
        total+=unlink_aborts+existing_aborts+close_aborts;
    }
    assert(!total);
    puts("PASS normal unlink/last close: empty and mapped inodes retain/remove real orphan records without abort/replay");
}

struct orphan_snapshot_actor {
    struct kernel_vfs_path *parent,*root;
    struct kernel_vfs_file created;
    struct kernel_rwlock *group_guard;
    uint64_t bitmap;
    uint32_t group;
    unsigned actor,entered,resume,done;
    int result;
};
static _Thread_local struct orphan_snapshot_actor *orphan_snapshot_current;
int __real_ext4_block_get(struct ext4_blockdev *,struct ext4_block *,uint64_t);
int __wrap_ext4_block_get(struct ext4_blockdev *device,struct ext4_block *block,uint64_t lba)
{
    struct orphan_snapshot_actor *actor=orphan_snapshot_current;
    if(actor && !actor->actor && !actor->entered && lba==actor->bitmap) {
        for(struct kernel_lock_guard *guard=kernel_io_context_current()->locks;guard;guard=guard->previous)
            if(guard->lock->rank==35 && guard->lock->key==actor->group)actor->group_guard=guard->lock;
        __atomic_store_n(&actor->entered,1,__ATOMIC_RELEASE);
        while(!__atomic_load_n(&actor->resume,__ATOMIC_ACQUIRE))sched_yield();
    }
    return __real_ext4_block_get(device,block,lba);
}
static void *orphan_snapshot_modify(void *argument)
{
    struct orphan_snapshot_actor *actor=argument;
    mm_test_attach(actor->actor+9);mm_test_activate_task();kernel_cpu_current()->logical_id=actor->actor;
    orphan_snapshot_current=actor;
    actor->result=actor->actor?kernel_vfs_create_at(actor->parent,actor->root,"after-gdt",0600,&actor->created,0):
        kernel_vfs_unlink_at(actor->parent,actor->root,"orphan-victim",0);
    orphan_snapshot_current=NULL;
    __atomic_store_n(&actor->done,1,__ATOMIC_RELEASE);
    mm_test_finish();return NULL;
}
static void orphan_snapshot_case(struct lwext4_mount_adapter *adapter,
    struct kernel_vfs_path *first,struct kernel_vfs_path *second,struct kernel_vfs_path *root)
{
    struct ext4_fs *fs=adapter->device.fs;
    struct kernel_vfs_file file={0};
    assert(!kernel_vfs_create_at(first,root,"orphan-victim",0600,&file,0));
    unsigned per_group=ext4_get32(&fs->sb,inodes_per_group);
    uint32_t group=(kernel_vfs_file_inode(&file)-1)/per_group;
    assert(group==(kernel_vfs_path_inode(second)-1)/per_group);
    adapter->locks.read_lock(adapter->locks.context);
    struct ext4_block_group_ref descriptor;
    assert(!ext4_fs_get_block_group_ref(fs,group,&descriptor));
    uint64_t bitmap=ext4_bg_get_inode_bitmap(descriptor.block_group,&fs->sb);
    assert(!ext4_fs_put_block_group_ref(&descriptor));adapter->locks.unlock(adapter->locks.context);
    struct orphan_snapshot_actor actors[2]={{.parent=first,.root=root,.bitmap=bitmap,.group=group},
        {.parent=second,.root=root,.bitmap=bitmap,.group=group,.actor=1}};
    pthread_t threads[2];assert(!pthread_create(&threads[0],NULL,orphan_snapshot_modify,&actors[0]));
    while(!__atomic_load_n(&actors[0].entered,__ATOMIC_ACQUIRE)) {
        assert(!__atomic_load_n(&actors[0].done,__ATOMIC_ACQUIRE));sched_yield();
    }
    assert(!pthread_create(&threads[1],NULL,orphan_snapshot_modify,&actors[1]));
    if(actors[0].group_guard) {
        for(;;) {
            unsigned waiting;
            {KERNEL_RAW_SCOPE(guard,&actors[0].group_guard->metadata);
             waiting=actors[0].group_guard->pending_head!=NULL;}
            if(waiting)break;
            assert(!__atomic_load_n(&actors[1].done,__ATOMIC_ACQUIRE));sched_yield();
        }
    } else {
        /* The pre-fix sequence accepts B after A borrowed its descriptor but
         * before A obtains the bitmap, manufacturing a checksum mismatch. */
        while(!__atomic_load_n(&actors[1].done,__ATOMIC_ACQUIRE))sched_yield();
        assert(!actors[1].result);
    }
    __atomic_store_n(&actors[0].resume,1,__ATOMIC_RELEASE);
    for(unsigned i=0;i<2;i++)assert(!pthread_join(threads[i],NULL));
    if(actors[0].result || actors[1].result)
        fprintf(stderr,"orphan descriptor/bitmap snapshot: unlink=%d create=%d group=%u guarded=%d\n",
            actors[0].result,actors[1].result,group,actors[0].group_guard!=NULL);
    assert(!actors[0].result && !actors[1].result && actors[0].group_guard);
    assert((kernel_vfs_file_inode(&actors[1].created)-1)/per_group==group);
    assert(!kernel_vfs_close(&file) && !kernel_vfs_close(&actors[1].created) &&
           !kernel_vfs_unlink_at(second,root,"after-gdt",0) &&
           !ext4_journal_group_drain(adapter->mount_point));
    puts("PASS orphan allocation check: GDT and inode bitmap share group qualification; same-group create queues instead of false EUCLEAN");
}
