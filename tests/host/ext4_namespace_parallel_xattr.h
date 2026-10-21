#include <ext4_crc32.h>
#include <ext4_trans.h>
#include <ext4_balloc.h>
#include <ext4_orphan.h>

static int namespace_ea_result(int result,int release)
{return result?result:release;}

struct namespace_ea_actor {
    struct kernel_vfs_file *file;
    struct kernel_vfs_path *root;
    struct kernel_rwlock *resource;
    struct kernel_lock_guard *held;
    uint64_t shared;
    uint32_t group;
    unsigned actor,abort,unlink,truncate,aborts,entered,resume,done;
    int result;
};
static _Thread_local struct namespace_ea_actor *namespace_ea_current;
int __real_jbd_trans_set_block_dirty(struct jbd_trans *,struct ext4_block *);
int __wrap_jbd_trans_set_block_dirty(struct jbd_trans *trans,struct ext4_block *block)
{
    int result=__real_jbd_trans_set_block_dirty(trans,block);
    struct namespace_ea_actor *actor=namespace_ea_current;
    if(result || !actor || actor->entered || block->lb_id!=actor->shared)return result;
    actor->held=kernel_io_context_current()->locks;
    for(struct kernel_lock_guard *guard=actor->held;guard;guard=guard->previous)
        if(guard->lock->rank==35 && guard->lock->key==actor->group)actor->resource=guard->lock;
    assert(actor->resource);
    __atomic_store_n(&actor->entered,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&actor->resume,__ATOMIC_ACQUIRE))sched_yield();
    return actor->abort?ECANCELED:result;
}
static void *namespace_ea_chown(void *argument)
{
    struct namespace_ea_actor *actor=argument;
    mm_test_attach(actor->actor+7);mm_test_activate_task();kernel_cpu_current()->logical_id=actor->actor;
    namespace_ea_current=actor;
    orphan_order_watch=actor->truncate;orphan_order_aborts=0;
    actor->result=actor->unlink?kernel_vfs_unlink_at(actor->root,actor->root,"ea-parallel-b",0):
        actor->truncate?kernel_vfs_ftruncate(actor->file,0):
        kernel_vfs_file_set_owner(actor->file,70001+actor->actor,80001+actor->actor);
    actor->aborts=orphan_order_aborts;orphan_order_watch=0;
    namespace_ea_current=NULL;
    __atomic_store_n(&actor->done,1,__ATOMIC_RELEASE);
    mm_test_finish();return NULL;
}
struct namespace_ea_seed {
    struct lwext4_mount_adapter *adapter;
    struct kernel_vfs_file *files;
    const char *path;
    uint64_t shared;
};
static int namespace_ea_seed_apply(void *argument)
{
    struct namespace_ea_seed *seed=argument;
    struct ext4_fs *fs=seed->adapter->device.fs;
    uint32_t capability[5]={0x02000000,1,0,0,0};unsigned char padding[48]={1};
    int result=ext4_setxattr(seed->path,"user.padding",12,padding,sizeof(padding));
    if(!result)result=ext4_setxattr(seed->path,"security.capability",19,capability,sizeof(capability));
    if(result)return result;
    struct ext4_inode first;
    result=ext4_fraw_inode_fill(lwext4_node_file(seed->files[0].private_data),&first);
    if(result)return result;
    seed->shared=ext4_inode_get_file_acl(&first,&fs->sb);assert(seed->shared);
    struct ext4_inode_ref second;
    result=ext4_fs_get_inode_ref(fs,kernel_vfs_file_inode(&seed->files[1]),&second);
    if(result)return result;
    result=ext4_operation_require_resource(fs,EXT4_RESOURCE_BLOCK_GROUP,
        ext4_balloc_get_bgid_of_block(&fs->sb,seed->shared));
    if(result)return namespace_ea_result(result,ext4_fs_put_inode_ref(&second));
    struct ext4_block block;
    result=ext4_trans_block_get(fs->bdev,&block,seed->shared);
    if(result)return namespace_ea_result(result,ext4_fs_put_inode_ref(&second));
    struct {uint32_t magic,refcount,blocks,hash,checksum,reserved[3];} header;
    memcpy(&header,block.data,sizeof(header));assert(to_le32(header.refcount)==1);
    header.refcount=to_le32(2);header.checksum=0;memcpy(block.data,&header,sizeof(header));
    uint64_t number=to_le64(seed->shared);
    uint32_t crc=ext4_crc32c(ext4_sb_get_csum_seed(&fs->sb),&number,sizeof(number));
    header.checksum=to_le32(ext4_crc32c(crc,block.data,ext4_sb_get_block_size(&fs->sb)));
    memcpy(block.data,&header,sizeof(header));
    result=ext4_trans_set_block_dirty(block.buf);
    result=namespace_ea_result(result,ext4_block_set(fs->bdev,&block));
    if(!result) {
        ext4_inode_set_file_acl(second.inode,&fs->sb,seed->shared);
        ext4_inode_set_blocks_count(&fs->sb,second.inode,
            ext4_inode_get_blocks_count(&fs->sb,second.inode)+ext4_sb_get_block_size(&fs->sb)/512);
        second.dirty=true;
    }
    return namespace_ea_result(result,ext4_fs_put_inode_ref(&second));
}
static void namespace_parallel_xattr_case(struct lwext4_mount_adapter *adapter,struct kernel_vfs_path *root)
{
    for(unsigned action=0;action<3;action++)for(unsigned abort=0;abort<2;abort++) {
        bool removing=action==1,truncating=action==2;
        struct kernel_vfs_file files[2]={{0},{0}};
        assert(!kernel_vfs_create_at(root,root,"ea-parallel-a",0600,&files[0]) &&
               !kernel_vfs_create_at(root,root,"ea-parallel-b",0600,&files[1]));
        if(truncating) {
            static const char payload[4096]="shared EA truncate payload";
            size_t written;
            assert(!kernel_vfs_file_acquire_write(&files[1]) &&
                   !kernel_vfs_pwrite(&files[1],0,payload,sizeof(payload),&written) && written==sizeof(payload));
        }
        char paths[2][96];
        for(unsigned i=0;i<2;i++)snprintf(paths[i],sizeof(paths[i]),"%sea-parallel-%c",adapter->mount_point,'a'+i);
        struct namespace_ea_seed seed={adapter,files,paths[0],0};
        assert(!ext4_metadata_execute(adapter->mount_point,namespace_ea_seed_apply,&seed));
        assert(!ext4_journal_group_drain(adapter->mount_point));
        if(removing)assert(!kernel_vfs_close(&files[1]));
        uint32_t group=ext4_balloc_get_bgid_of_block(&adapter->device.fs->sb,seed.shared);
        struct namespace_ea_actor actors[2]={{.file=&files[0],.shared=seed.shared,.group=group,.abort=abort},
            {.file=&files[1],.root=root,.shared=seed.shared,.group=group,.actor=1,.unlink=removing,.truncate=truncating}};
        pthread_t threads[2];assert(!pthread_create(&threads[0],NULL,namespace_ea_chown,&actors[0]));
        while(!__atomic_load_n(&actors[0].entered,__ATOMIC_ACQUIRE)) {
            assert(!__atomic_load_n(&actors[0].done,__ATOMIC_ACQUIRE));sched_yield();
        }
        assert(!pthread_create(&threads[1],NULL,namespace_ea_chown,&actors[1]));
        for(;;) {
            unsigned waiting=0;
            /* unlink/truncate may wait for their inode bitmap's group before
             * reaching the shared EA group; A's paused guards remain live. */
            for(struct kernel_lock_guard *held=actors[0].held;held;held=held->previous) {
                if(held->lock->rank!=35)continue;
                {KERNEL_RAW_SCOPE(guard,&held->lock->metadata);
                 waiting=held->lock->pending_head!=NULL;}
                if(waiting)break;
            }
            if(waiting)break;
            assert(!__atomic_load_n(&actors[1].entered,__ATOMIC_ACQUIRE) &&
                   !__atomic_load_n(&actors[1].done,__ATOMIC_ACQUIRE));sched_yield();
        }
        __atomic_store_n(&actors[0].resume,1,__ATOMIC_RELEASE);
        while(!__atomic_load_n(&actors[0].done,__ATOMIC_ACQUIRE))sched_yield();
        while(!__atomic_load_n(&actors[1].entered,__ATOMIC_ACQUIRE) &&
              !__atomic_load_n(&actors[1].done,__ATOMIC_ACQUIRE))sched_yield();
        __atomic_store_n(&actors[1].resume,1,__ATOMIC_RELEASE);
        for(unsigned i=0;i<2;i++)assert(!pthread_join(threads[i],NULL));
        assert(actors[0].result==(abort?-ECANCELED:0) && !actors[1].result);
        if(truncating) {
            struct kernel_vfs_stat stat;
            assert(!kernel_vfs_fstat(&files[1],&stat) && !stat.size && stat.nlink==1);
            bool present=true;
            adapter->locks.read_lock(adapter->locks.context);
            assert(!ext4_orphan_contains(adapter->device.fs,kernel_vfs_file_inode(&files[1]),&present) && !present);
            struct ext4_inode_ref inode;
            assert(!ext4_fs_get_inode_ref(adapter->device.fs,kernel_vfs_file_inode(&files[1]),&inode));
            assert(ext4_inode_get_blocks_count(&adapter->device.fs->sb,inode.inode)==
                   (ext4_inode_get_file_acl(inode.inode,&adapter->device.fs->sb)?adapter->device.lg_bsize/512:0));
            assert(!ext4_fs_put_inode_ref(&inode));
            adapter->locks.unlock(adapter->locks.context);
            printf("shared external EA truncate: A %s, B aborts=%u\n",abort?"aborted":"accepted",actors[1].aborts);
            assert(!actors[1].aborts);
        }
        uint32_t value[5],capability[5]={0x02000000,1,0,0,0};size_t length;
        for(unsigned i=0;i<2;i++) {
            int result=ext4_getxattr(paths[i],"security.capability",19,value,sizeof(value),&length);
            if(i && removing)assert(result==ENOENT);
            else if(!i && abort)assert(!result && length==sizeof(capability) && !memcmp(value,capability,sizeof(value)));
            else assert(result==ENODATA);
            if(!i || !removing)assert(!kernel_vfs_close(&files[i]));
        }
        assert(!kernel_vfs_unlink_at(root,root,"ea-parallel-a",0) &&
               (removing || !kernel_vfs_unlink_at(root,root,"ea-parallel-b",0)) &&
               !ext4_journal_group_drain(adapter->mount_point));
        printf("PASS shared external EA: different inode %s waits for block-group qualification; A %s, B accepted, capability owners correct\n",
            removing?"final unlink":truncating?"truncate":"chown/COW",abort?"aborted":"accepted");
    }
}
