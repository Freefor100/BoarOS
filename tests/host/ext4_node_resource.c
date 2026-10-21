#define main original_file_io_main
#include "file_io_concurrency.c"
#undef main
#include "../../fs/ext4_backend.h"
#include <ext4_fs.h>
#include <ext4_journal.h>
#include <stdlib.h>
static _Thread_local void *last_zeroed;
static _Thread_local size_t last_zeroed_size;
struct inode_allocation { uint32_t inode;void *owner; };
static struct inode_allocation inode_allocations[128];
static unsigned inode_allocation_count,qualified;
static uint32_t checked_inode;
static void *watched_owner;
static unsigned watched_frees;
enum kernel_heap_status __real_kernel_heap_allocate_zeroed(struct kernel_heap *,size_t,size_t,void **);
enum kernel_heap_status __wrap_kernel_heap_allocate_zeroed(struct kernel_heap *heap,size_t count,size_t bytes,void **owner)
{
    enum kernel_heap_status result=__real_kernel_heap_allocate_zeroed(heap,count,bytes,owner);
    if(!result) {last_zeroed=*owner;last_zeroed_size=count*bytes;} return result;
}
void __real_kernel_rwlock_init(struct kernel_rwlock *,uint32_t,uintptr_t);
void __wrap_kernel_rwlock_init(struct kernel_rwlock *mutex,uint32_t rank,uintptr_t key)
{
    if(rank==31 && key<=UINT32_MAX) {
        assert((uintptr_t)mutex>=(uintptr_t)last_zeroed && (uintptr_t)mutex-(uintptr_t)last_zeroed<last_zeroed_size);
        assert(inode_allocation_count<128);
        inode_allocations[inode_allocation_count++]=(struct inode_allocation){key,last_zeroed};
    }
    __real_kernel_rwlock_init(mutex,rank,key);
}
void __real_kernel_heap_release(struct kernel_heap *,void *);
void __wrap_kernel_heap_release(struct kernel_heap *heap,void *owner)
{ if(owner==watched_owner) {watched_frees++;watched_owner=NULL;} __real_kernel_heap_release(heap,owner); }
int __real_jbd_trans_set_block_dirty_range(struct jbd_trans *,struct ext4_block *,uint32_t,uint32_t);
int __wrap_jbd_trans_set_block_dirty_range(struct jbd_trans *trans,struct ext4_block *block,uint32_t offset,uint32_t length)
{
    int result=__real_jbd_trans_set_block_dirty_range(trans,block,offset,length);
    if(!result && checked_inode && length==ext4_get16(&trans->operation.fs->sb,inode_size)) {
        struct kernel_lock_guard *g=kernel_io_context_current()->locks;
        while(g && (g->lock->rank!=31 || g->lock->key!=checked_inode))g=g->previous;
        assert(g && g->write && g->owner==kernel_io_context_current());qualified++;
    }
    return result;
}
static unsigned allocations_for(uint32_t inode)
{ unsigned count=0;for(unsigned i=0;i<inode_allocation_count;i++)if(inode_allocations[i].inode==inode)count++;return count; }
static uint64_t fixture_now(void *unused) { (void)unused;return kernel_time_monotonic_ns(); }
static int fail_bind;
static unsigned replay_create,replayed,binds;
static struct boaros_lwext4_resource **last_bound_slot;
int __real_boaros_lwext4_inode_get(struct boaros_lwext4_domain *,uint32_t,struct boaros_lwext4_resource **);
int __wrap_boaros_lwext4_inode_get(struct boaros_lwext4_domain *domain,uint32_t inode,struct boaros_lwext4_resource **owner)
{
    if(fail_bind) {int error=fail_bind;fail_bind=0;return error;}
    int result=__real_boaros_lwext4_inode_get(domain,inode,owner);
    if(!result) {binds++;last_bound_slot=owner;}return result;
}
struct replay_call { int (*apply)(void *);void *context; };
static int abort_prepared_create(void *opaque)
{
    struct replay_call *call=opaque;int result=call->apply(call->context);
    return result?result:ECANCELED;
}
int __real_ext4_metadata_execute(const char *,int (*)(void *),void *);
int __wrap_ext4_metadata_execute(const char *mount,int (*apply)(void *),void *context)
{
    if(replay_create) {
        replay_create=0;struct replay_call call={apply,context};unsigned before=binds;
        assert(__real_ext4_metadata_execute(mount,abort_prepared_create,&call)==ECANCELED);
        assert(binds==before+1 && last_bound_slot && *last_bound_slot);
        /* Replay the same real constructor after complete rollback. Its old
         * handle and inode reference must be returned before constructing again. */
        int result=__real_ext4_metadata_execute(mount,apply,context);
        assert(!result && binds==before+2);replayed++;return result;
    }
    return __real_ext4_metadata_execute(mount,apply,context);
}
static ext4_file *close_fault;
int __real_ext4_fclose(ext4_file *);
int __wrap_ext4_fclose(ext4_file *file)
{ if(file==close_fault)return EIO;return __real_ext4_fclose(file); }
int main(int argc,char **argv)
{
    assert(argc==2);setvbuf(stdout,NULL,_IONBF,0);mm_test_attach(0);mm_test_activate_task();
    struct physical_page_allocator pages={0},backend_pages={0};struct kernel_heap heap={0},backend_heap={0};
    initialize(&pages,&heap,arena,sizeof(arena));initialize(&backend_pages,&backend_heap,backend,sizeof(backend));
    uint64_t available=physical_page_available(&pages),backend_available=physical_page_available(&backend_pages);
    image=open(argv[1],O_RDWR);assert(image>=0);struct stat info;assert(!fstat(image,&info));
    struct kernel_page_cache cache={0};assert(!kernel_page_cache_init(&cache,&heap,&pages));
    struct kernel_block_device disk={.read=read_image,.write=write_image,.flush=flush_image,.capacity_bytes=info.st_size,.logical_block_size=512,.cache_mode=KERNEL_BLOCK_CACHE_WRITEBACK};
    int read_only=getenv("NODE_READ_ONLY")!=NULL;
    struct kernel_vfs_mount mount={0};assert(!kernel_block_register(&disk,KERNEL_BLOCK_DEVICE_NUMBER(106)) && !kernel_vfs_mount_ext4(&mount,&disk,&backend_heap,&cache,read_only));
    struct lwext4_mount_adapter *adapter=mount.private_data;struct kernel_vfs_path *root=NULL;
    assert(!adapter->backend_lock.parallel);
    assert(!kernel_vfs_path_root(&mount,&heap,&root));
    printf("root inode alive before parallel: shared resource retained=%u\n",adapter->backend_lock.resources.rbh_root!=NULL);
    if(read_only || !adapter->device.fs->jbd_journal) {
        unsigned before=inode_allocation_count;struct kernel_vfs_stat stat;
        for(unsigned i=0;i<8;i++)assert(!kernel_vfs_stat_at(root,root,"/",1,&stat));
        assert(!((struct lwext4_node *)root->file.private_data)->inode_resource && inode_allocation_count==before && !adapter->backend_lock.resources.rbh_root);
        printf("PASS read-only/nojournal root and eight stats: inode resource allocations=%u\n",before);
        goto Cleanup;
    }
    assert(((struct lwext4_node *)root->file.private_data)->inode_resource);
    const struct ext4_journal_runtime runtime={.now_ns=fixture_now};
    assert(!ext4_journal_group_enable(adapter->mount_point,&runtime,8*1024*1024));
    boaros_lwext4_enable_parallel(&adapter->backend_lock,adapter->device.fs);adapter->instance.namespace_parallel=1;
    struct kernel_vfs_file file={0};assert(!kernel_vfs_create_at(root,root,"node-owner",0600,&file));
    checked_inode=kernel_vfs_file_inode(&file);unsigned before=allocations_for(checked_inode);
    for(unsigned i=0;i<inode_allocation_count;i++)if(inode_allocations[i].inode==checked_inode)watched_owner=inode_allocations[i].owner;
    printf("created inode=%u resource object allocations=%u owner=%p\n",checked_inode,before,watched_owner);
    assert(before>=1 && watched_owner);watched_frees=0;
    for(unsigned i=0;i<8;i++)assert(!kernel_vfs_file_set_mode(&file,0600|(i&1?040:020)));
    unsigned after=allocations_for(checked_inode);
    printf("actual repeated inode transactions: initial allocations=%u after eight=%u qualified=%u frees while open=%u\n",before,after,qualified,watched_frees);
    assert(qualified==8 && after==before && !watched_frees);
    checked_inode=0;
    struct kernel_vfs_stat stat;unsigned read_before=inode_allocation_count;
    for(unsigned i=0;i<8;i++) {
        struct kernel_vfs_file reader={0};const struct kernel_vfs_open_request request={.flags=KERNEL_VFS_OPEN_FOLLOW};
        assert(!kernel_vfs_open_at(root,root,"node-owner",&request,&reader,NULL));
        assert(!kernel_vfs_stat_at(root,root,"node-owner",1,&stat) && !kernel_vfs_close(&reader));
    }
    assert(inode_allocation_count==read_before);
    puts("PASS eight read-only consumers on a live identity allocate no extra inode resource");
    struct kernel_vfs_file candidate={0};
    assert(!adapter->instance.ops->open(&mount,NULL,kernel_vfs_file_inode(&file),file.mode,&candidate));
    assert(candidate.private_data==file.private_data && !kernel_vfs_close(&candidate));
    unsigned external=adapter->instance.external_files;adapter->instance.external_files=UINT32_MAX;
    assert(adapter->instance.ops->open(&mount,NULL,kernel_vfs_file_inode(&file),file.mode,&candidate)==-KERNEL_EOVERFLOW);
    adapter->instance.external_files=external;assert(!candidate.private_data && !watched_frees);
    fail_bind=ENOMEM;
    assert(kernel_vfs_create_at(root,root,"failed-pin",0600,&candidate)==-KERNEL_ENOMEM);
    assert(!candidate.private_data && kernel_vfs_stat_at(root,root,"failed-pin",1,&stat)==-KERNEL_ENOENT);
    replay_create=1;
    assert(!kernel_vfs_create_at(root,root,"replayed-pin",0600,&candidate) && replayed==1);
    assert(!kernel_vfs_close(&candidate) && !kernel_vfs_unlink_at(root,root,"replayed-pin",0));
    puts("PASS candidate loser/overflow, pre-accept pin ENOMEM rollback and same-constructor abort/replay return their references");
    struct kernel_vfs_node *closing=file.private_data;close_fault=lwext4_node_file(closing);
    assert(!kernel_vfs_close(&file) && adapter->instance.cleanup_nodes && !watched_frees && ((struct lwext4_node *)closing)->inode_resource);
    close_fault=NULL;
    puts("PASS failed close retains inode resource until mount cleanup retries the actual handle");
    assert(!ext4_journal_group_drain(adapter->mount_point));
Cleanup:
    /* Keep the attached root owned by the mount through prepare_detach. Its
     * private handle closes before detach releases that last path identity. */
    struct kernel_vfs_mount *parent=NULL;struct kernel_vfs_path *covered=NULL;
    assert(!kernel_tmpfs_create(&heap,0,"",&parent) &&
           !kernel_vfs_path_root(parent,&heap,&covered) &&
           !kernel_vfs_mount_attach(&mount,covered));
    assert(!kernel_vfs_path_release(&root));
    struct lwext4_node *mounted_root=mount.root_path->file.private_data;
    close_fault=lwext4_node_file(&mounted_root->node);
    assert(kernel_vfs_mount_prepare_detach(&mount,NULL)==-KERNEL_EIO &&
           !mounted_root->node.closed);
    if(!read_only && adapter->device.fs->jbd_journal)assert(mounted_root->inode_resource);
    close_fault=NULL;
    assert(!kernel_vfs_mount_prepare_detach(&mount,NULL));
    printf("prepared attached root: closed=%u resource retained=%u\n",
           mounted_root->node.closed,mounted_root->inode_resource!=NULL);
    assert(!kernel_vfs_mount_detach(&mount,NULL) && !kernel_vfs_unmount(&mount));
    assert(!kernel_vfs_path_release(&covered) && !kernel_vfs_unmount(parent));
    assert(!kernel_page_cache_destroy(&cache) && !kernel_block_unregister(&disk));
    puts("PASS attached root close fault/retry, prepare_detach/detach/unmount returns inode owner");
    assert(physical_page_available(&pages)==available && physical_page_available(&backend_pages)==backend_available);
    close(image);mm_test_finish();puts("PASS live inode resource owner returned");return 0;
}
