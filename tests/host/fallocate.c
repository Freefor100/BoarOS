/* Reuse the existing real-device/cache/MM host fixture, with separate cases. */
#include <kernel/files.h>
#define main cache_fixture_main
#include "page_cache_concurrency.c"
#undef main
#include <ext4_extent.h>
#include <ext4_super.h>
#include <ext4_inode.h>
#include <ext4_xattr.h>
#include <kernel/tmpfs.h>
#include <kernel/credentials.h>
#include "../../fs/pipe_internal.h"
static unsigned credential_pin_pause,credential_pin_entered,credential_pin_resume;
enum kernel_uaccess_status __real_kernel_uaccess_pin_user_page(struct kernel_mm *,uint64_t,uint32_t,struct kernel_mm_user_pin *);
enum kernel_uaccess_status __wrap_kernel_uaccess_pin_user_page(struct kernel_mm *mm,uint64_t address,uint32_t access,struct kernel_mm_user_pin *pin)
{
    if (access==KERNEL_MM_READ && kernel_cpu_current()->hardware_id==1 &&
        __atomic_exchange_n(&credential_pin_pause,0,__ATOMIC_ACQ_REL)) {
        __atomic_store_n(&credential_pin_entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&credential_pin_resume,__ATOMIC_ACQUIRE)) sched_yield();
    }
    return __real_kernel_uaccess_pin_user_page(mm,address,access,pin);
}
static unsigned fail_capability_read,fail_capability_remove;
int __real_ext4_xattr_get(struct ext4_inode_ref *,uint8_t,const char *,size_t,void *,size_t,size_t *);
int __wrap_ext4_xattr_get(struct ext4_inode_ref *ref,uint8_t index,const char *name,size_t length,void *out,size_t capacity,size_t *size)
{
    if (index==6 && length==10 && !memcmp(name,"capability",10) && fail_capability_read) {
        fail_capability_read=0;return EIO;
    }
    return __real_ext4_xattr_get(ref,index,name,length,out,capacity,size);
}
int __real_ext4_xattr_remove(struct ext4_inode_ref *,uint8_t,const char *,size_t);
int __wrap_ext4_xattr_remove(struct ext4_inode_ref *ref,uint8_t index,const char *name,size_t length)
{
    if (index==6 && length==10 && !memcmp(name,"capability",10) && fail_capability_remove) {
        fail_capability_remove=0;return ENOMEM;
    }
    return __real_ext4_xattr_remove(ref,index,name,length);
}
static unsigned fail_delayed_allocation;
int __real_ext4_fs_get_inode_dblk_range(struct ext4_inode_ref *,ext4_lblk_t,uint32_t,bool,struct ext4_inode_block_range *);
int __wrap_ext4_fs_get_inode_dblk_range(struct ext4_inode_ref *ref,ext4_lblk_t first,uint32_t count,bool create,struct ext4_inode_block_range *range)
{
    int r=__real_ext4_fs_get_inode_dblk_range(ref,first,count,create,range);
    if (!r && create && fail_delayed_allocation) {fail_delayed_allocation=0;return ENOMEM;}
    return r;
}
static unsigned fail_reserve, fail_remove, failed_direct_calls, fail_complete;
static unsigned metadata_probe,metadata_open_calls;
static unsigned credential_create_pause,credential_create_entered,credential_create_resume,credential_chmod_done;
static unsigned socket_create_pause,socket_create_entered,socket_create_resume,socket_replace_done;
static const char *metadata_failed_step;
int __real_ext4_transaction_begin(const char *);
int __wrap_ext4_transaction_begin(const char *mount)
{ int r=__real_ext4_transaction_begin(mount);if(metadata_probe && r)metadata_failed_step="begin";return r; }
int __real_ext4_fopen2(ext4_file *,const char *,int);
int __wrap_ext4_fopen2(ext4_file *file,const char *path,int flags)
{ if(metadata_probe)metadata_open_calls++;int r=__real_ext4_fopen2(file,path,flags);if(metadata_probe && r)metadata_failed_step="open";return r; }
int __real_ext4_create_child(const char *,uint32_t,const char *,uint32_t,uint32_t,const char *,ext4_file *,struct ext4_inode *,uint32_t uid,uint32_t gid);
int __wrap_ext4_create_child(const char *mp,uint32_t parent,const char *name,uint32_t mode,uint32_t device,const char *target,ext4_file *file,struct ext4_inode *initial,uint32_t uid,uint32_t gid)
{
    if (socket_create_pause && !strcmp(name,"socket-race")) {
        __atomic_store_n(&socket_create_entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&socket_create_resume,__ATOMIC_ACQUIRE)) sched_yield();
    }
    if (credential_create_pause && !strcmp(name,"accepted")) {
        __atomic_store_n(&credential_create_entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&credential_create_resume,__ATOMIC_ACQUIRE)) sched_yield();
    }
    if(metadata_probe)metadata_open_calls++;
    int r=__real_ext4_create_child(mp,parent,name,mode,device,target,file,initial,uid,gid);
    if(metadata_probe && r)metadata_failed_step="create";
    return r;
}
int __real_ext4_transaction_end(const char *);
int __wrap_ext4_transaction_end(const char *mount)
{ int r=__real_ext4_transaction_end(mount);if(metadata_probe && r)metadata_failed_step="end";return r; }
int __real_ext4_file_complete_write(ext4_file *);
int __wrap_ext4_file_complete_write(ext4_file *file)
{ if (fail_complete) { fail_complete=0; return EIO; } return __real_ext4_file_complete_write(file); }
static ext4_fsblk_t fail_direct_block;
int __real_ext4_blocks_set_direct(struct ext4_blockdev *,const void *,uint64_t,uint32_t);
int __wrap_ext4_blocks_set_direct(struct ext4_blockdev *dev,const void *bytes,uint64_t block,uint32_t count)
{
    if (fail_direct_block && block<=fail_direct_block && fail_direct_block-block<count) {
        fail_direct_block=0; failed_direct_calls++; return EIO;
    }
    return __real_ext4_blocks_set_direct(dev,bytes,block,count);
}
int __real_ext4_extent_reserve_range(struct ext4_inode_ref *,ext4_lblk_t,uint32_t,struct ext4_inode_block_range *);
int __wrap_ext4_extent_reserve_range(struct ext4_inode_ref *ref,ext4_lblk_t first,uint32_t count,struct ext4_inode_block_range *range)
{
    if (fail_reserve && !--fail_reserve) return ENOMEM;
    return __real_ext4_extent_reserve_range(ref,first,count,range);
}
int __real_ext4_extent_remove_space(struct ext4_inode_ref *,ext4_lblk_t,ext4_lblk_t);
int __wrap_ext4_extent_remove_space(struct ext4_inode_ref *ref,ext4_lblk_t first,ext4_lblk_t last)
{
    if (fail_remove && !--fail_remove) return ENOMEM;
    return __real_ext4_extent_remove_space(ref,first,last);
}
static uint64_t allocated(struct kernel_vfs_file *file)
{
    struct kernel_vfs_stat stat;
    assert(!kernel_vfs_fstat(file,&stat));
    return stat.blocks;
}
static struct ext4_inode_block_range mapped(struct kernel_vfs_file *file,uint32_t block)
{
    struct kernel_vfs_node *node=kernel_vfs_file_node(file);
    struct ext4_fs *fs=lwext4_instance(node->instance)->device.fs;
    struct ext4_inode_ref ref;
    struct ext4_inode_block_range range;
    assert(!ext4_fs_get_inode_ref(fs,lwext4_node_file(node)->inode,&ref));
    assert(!ext4_extent_get_range(&ref,block,64,false,&range));
    assert(!ext4_fs_put_inode_ref(&ref));
    return range;
}
static void expect_bytes(struct kernel_vfs_file *file,const unsigned char *expected,size_t length)
{
    unsigned char actual[BOAROS_PAGE_SIZE];
    for (size_t offset=0;offset<length;) {
        size_t chunk=length-offset<sizeof(actual)?length-offset:sizeof(actual),done;
        assert(!kernel_vfs_pread(file,offset,actual,chunk,&done, NULL) && done==chunk);
        if (memcmp(actual,expected+offset,chunk)) {
            for (size_t i=0;i<chunk;i++) if (actual[i]!=expected[offset+i]) {
                fprintf(stderr,"byte %zu: %x != %x\n",offset+i,actual[i],expected[offset+i]); break;
            }
            abort();
        }
        offset+=chunk;
    }
}
static int create_file(struct kernel_vfs_mount *mount,const char *name,uint32_t mode,struct kernel_vfs_file *file)
{
    struct lwext4_mount_adapter *adapter=lwext4_instance(mount->private_data);
    if (adapter->device.fs->jbd_journal) return kernel_vfs_create(mount,name,mode,file);
    struct kernel_vfs_path *root=NULL;
    assert(!kernel_vfs_path_root(mount,adapter->instance.heap, &root));
    const uint32_t types[]={KERNEL_VFS_S_IFREG,KERNEL_VFS_S_IFIFO,KERNEL_VFS_S_IFCHR};
    for (unsigned i=0;i<sizeof(types)/sizeof(types[0]);i++) {
        assert(kernel_vfs_mknod_at(root,root,"/nojournal-denied",types[i]|0600,0)==-KERNEL_EOPNOTSUPP);
        struct kernel_vfs_file missing={0};
        assert(kernel_vfs_open(mount,"/nojournal-denied",&missing)==-KERNEL_ENOENT);
    }
    assert(!kernel_vfs_path_release(&root));
    /* Namespace create+chmod needs an atomic journal group in the VFS backend.
     * Seed nojournal fixtures through the library, then exercise real VFS I/O. */
    char path[96]; snprintf(path,sizeof(path),"%s%s",adapter->mount_point,name+1);
    ext4_file seeded;
    int result=ext4_fopen(&seeded,path,"w+");
    if (!result) result=ext4_fclose(&seeded);
    return result ? -result : kernel_vfs_open(mount,name,file);
}
static void basic_ranges(struct kernel_page_cache *cache,struct kernel_vfs_mount *mount)
{
    struct kernel_vfs_file file={0};
    int created=create_file(mount,"/ranges",0600,&file);
    if (created) fprintf(stderr,"create ranges error=%d\n",created);
    assert(!created);
    size_t block=kernel_vfs_file_node(&file)->instance->writeback_block_size;
    uint64_t before=allocated(&file);
    struct kernel_vfs_statfs free_before,free_after;
    assert(!kernel_vfs_mount_statfs(mount,&free_before));
    assert(!kernel_vfs_fallocate(&file,1,3*block+17,9*block));
    assert(kernel_vfs_node_size(kernel_vfs_file_node(&file))==0);
    assert(allocated(&file)==before+10*block/512);
    assert(!kernel_vfs_mount_statfs(mount,&free_after) && free_before.free_blocks-free_after.free_blocks==10);
    assert(!mapped(&file,0).block && mapped(&file,3).unwritten && mapped(&file,12).unwritten);
    assert(!kernel_vfs_fallocate(&file,0,0,16*block+73));
    assert(kernel_vfs_node_size(kernel_vfs_file_node(&file))==16*block+73);
    unsigned char *zero=calloc(1,16*block+73); assert(zero);
    expect_bytes(&file,zero,16*block+73); free(zero);
    unsigned char marker=0x75; size_t done;
    assert(!kernel_vfs_pwrite(&file,4*block+13,&marker,1,&done) && done==1);
    uint64_t observed=0; int synced=kernel_vfs_sync(&file,0,&observed);
    if (synced) fprintf(stderr,"first unwritten conversion sync=%d\n",synced);
    assert(!synced);
    assert(mapped(&file,3).unwritten && !mapped(&file,4).unwritten && mapped(&file,5).unwritten);
    assert(!kernel_vfs_fallocate(&file,1,3*block,3*block));
    assert(mapped(&file,3).unwritten && !mapped(&file,4).unwritten && mapped(&file,5).unwritten);
    assert(!kernel_vfs_close(&file));
    assert(!create_file(mount,"/mixed",0600,&file));
    assert(!kernel_vfs_pwrite(&file,0,&marker,1,&done) && done==1);
    assert(!kernel_vfs_fallocate(&file,1,block,block));
    assert(!mapped(&file,0).unwritten && mapped(&file,0).count==1 && mapped(&file,1).unwritten);
    assert(!kernel_vfs_close(&file));
    assert(!create_file(mount,"/punch",0600,&file));
    size_t length=4*BOAROS_PAGE_SIZE+117;
    unsigned char *expected=malloc(length); assert(expected);
    for (size_t i=0;i<length;i++) expected[i]=(unsigned char)(i*37+(i>>9));
    assert(!kernel_vfs_pwrite(&file,0,expected,length,&done) && done==length);
    /* Buffered EOF must reach the backend even when the punch is far below it. */
    uint64_t offset=block+31,end=4*block+47;
    assert(!kernel_vfs_fallocate(&file,3,offset,end-offset));
    memset(expected+offset,0,end-offset);
    expect_bytes(&file,expected,length);
    assert(kernel_vfs_node_size(kernel_vfs_file_node(&file))==length);
    assert(!mapped(&file,2).block && !mapped(&file,3).block);
    assert(mapped(&file,1).block && mapped(&file,4).block);
    assert(!kernel_page_cache_invalidate_node(cache,kernel_vfs_file_node(&file)));
    expect_bytes(&file,expected,length);
    /* Same-block boundaries, then a punch past EOF, preserve neighboring bytes. */
    assert(!kernel_vfs_fallocate(&file,3,11,17)); memset(expected+11,0,17);
    assert(!kernel_vfs_fallocate(&file,3,length-13,3*block)); memset(expected+length-13,0,13);
    expect_bytes(&file,expected,length);
    assert(!kernel_vfs_fallocate(&file,3,length+block,block));
    assert(kernel_vfs_node_size(kernel_vfs_file_node(&file))==length);
    assert(kernel_vfs_fallocate(&file,0,0,0)==-KERNEL_EINVAL);
    assert(kernel_vfs_fallocate(&file,0,INT64_MAX,1)==-KERNEL_EFBIG);
    assert(kernel_vfs_fallocate(&file,8,0,1)==-KERNEL_EOPNOTSUPP);
    assert(kernel_vfs_fallocate(&file,2,0,1)==-KERNEL_EOPNOTSUPP);
    free(expected); assert(!kernel_vfs_close(&file));
    assert(!create_file(mount,"/unlinked-reservation",0600,&file));
    assert(!kernel_vfs_fallocate(&file,1,block,8*block) && allocated(&file)==8*block/512);
    assert(!kernel_vfs_ftruncate(&file,0) && !allocated(&file));
    assert(!kernel_vfs_fallocate(&file,1,0,block));
    assert(!kernel_vfs_unlink(mount,"/unlinked-reservation"));
    assert(!kernel_vfs_fallocate(&file,1,8*block,block));
    assert(mapped(&file,0).block && mapped(&file,0).unwritten && mapped(&file,8).block);
    assert(!kernel_vfs_fallocate(&file,0,0,9*block));
    assert(allocated(&file)==9*block/512);
    assert(!kernel_vfs_ftruncate(&file,0) && !allocated(&file));
    assert(!kernel_vfs_close(&file));
    puts("PASS ranges: real unwritten reservations/free counts, KEEP_SIZE/EOF, mixed states, partial/full-block holes and reload");
}
static void rollback_ranges(struct kernel_vfs_mount *mount)
{
    struct kernel_vfs_file file={0}; assert(!create_file(mount,"/rollback",0600,&file));
    size_t block=kernel_vfs_file_node(&file)->instance->writeback_block_size,done;
    unsigned char marker=0x8e;
    assert(!kernel_vfs_pwrite(&file,4*block+7,&marker,1,&done) && done==1);
    uint64_t observed=0; assert(!kernel_vfs_sync(&file,0,&observed));
    uint64_t size=kernel_vfs_node_size(kernel_vfs_file_node(&file)),blocks=allocated(&file);
    struct kernel_vfs_statfs before,after;
    assert(!kernel_vfs_mount_statfs(mount,&before));
    fail_reserve=2;
    assert(kernel_vfs_fallocate(&file,0,0,12*block)==-KERNEL_ENOMEM && !fail_reserve);
    assert(kernel_vfs_node_size(kernel_vfs_file_node(&file))==size && allocated(&file)==blocks);
    assert(!mapped(&file,0).block && mapped(&file,4).block);
    assert(!kernel_vfs_mount_statfs(mount,&after) && before.free_blocks==after.free_blocks);
    assert(kernel_vfs_fallocate(&file,1,0,64*1024*1024)==-KERNEL_ENOSPC);
    assert(kernel_vfs_node_size(kernel_vfs_file_node(&file))==size && allocated(&file)==blocks);
    assert(!mapped(&file,0).block && mapped(&file,4).block);
    assert(!kernel_vfs_mount_statfs(mount,&after) && before.free_blocks==after.free_blocks);
    unsigned char *expected=malloc(8*block); assert(expected);
    memset(expected,0x4c,8*block);
    assert(!kernel_vfs_pwrite(&file,0,expected,8*block,&done) && done==8*block);
    assert(!kernel_vfs_sync(&file,0,&observed)); blocks=allocated(&file);
    fail_remove=1;
    assert(kernel_vfs_fallocate(&file,3,13,6*block)==-KERNEL_ENOMEM && !fail_remove);
    assert(!kernel_page_cache_invalidate_node(kernel_vfs_file_node(&file)->instance->page_cache,kernel_vfs_file_node(&file)));
    expect_bytes(&file,expected,8*block);
    assert(allocated(&file)==blocks && mapped(&file,1).block);
    free(expected); assert(!kernel_vfs_close(&file));
    puts("PASS rollback: failure after reservation and partial-boundary zero restores bytes, mappings, bitmap/free counts and EOF");
}
static void pending_truncate(struct kernel_vfs_mount *mount)
{
    struct lwext4_mount_adapter *adapter=lwext4_instance(mount->private_data);
    if (!adapter->device.fs->jbd_journal) return;
    size_t block=adapter->device.lg_bsize,length=8*block,done;
    unsigned char *bytes=malloc(length); assert(bytes); memset(bytes,0x5e,length);
    char path[96]; snprintf(path,sizeof(path),"%spending-truncate",adapter->mount_point);
    ext4_file file; assert(!ext4_fopen(&file,path,"w+"));
    assert(!ext4_fwrite(&file,bytes,length,&done) && done==length);
    assert(!ext4_transaction_begin(adapter->mount_point));
    assert(!ext4_ftruncate(&file,block+17, UINT32_MAX));
    bool changed;
    assert(!ext4_fallocate(&file,1,6*block,2*block,&changed, UINT32_MAX) && changed);
    assert(ext4_fsize(&file)==block+17);
    assert(!ext4_transaction_end(adapter->mount_point));
    assert(!ext4_fallocate(&file,0,0,length,&changed, UINT32_MAX) && changed);
    memset(bytes+block+17,0,length-block-17);
    unsigned char *actual=malloc(length); assert(actual);
    assert(!ext4_fpread(&file,0,actual,length,&done) && done==length && !memcmp(bytes,actual,length));
    free(actual); free(bytes);
    assert(!ext4_fclose(&file) && !ext4_fremove(path));
    puts("PASS pending truncate: grouped shrink reclamation precedes new reservation and growth; old suffix stays zero");
}
static void nonjournal_failure(struct kernel_vfs_mount *mount)
{
    struct kernel_vfs_file file={0}; assert(!create_file(mount,"/nonjournal-space",0600,&file));
    struct kernel_vfs_statfs before,after;
    assert(!kernel_vfs_mount_statfs(mount,&before));
    assert(kernel_vfs_fallocate(&file,1,0,64*1024*1024)==-KERNEL_ENOSPC);
    assert(!kernel_vfs_node_size(kernel_vfs_file_node(&file)) && allocated(&file));
    assert(!kernel_vfs_ftruncate(&file,0) && !allocated(&file));
    assert(!kernel_vfs_mount_statfs(mount,&after) && before.free_blocks==after.free_blocks);
    assert(!kernel_vfs_close(&file));
    puts("PASS nonjournal partial failure: ENOSPC retains actual reservation prefix; same-size truncate returns its blocks");
}
static void fill_free_space(struct kernel_vfs_mount *mount,struct kernel_vfs_file *filler,
    struct kernel_vfs_file *last,size_t block)
{
    struct kernel_vfs_statfs stat; assert(!kernel_vfs_mount_statfs(mount,&stat));
    uint64_t offset=0,step=stat.free_blocks;
    while (step) {
        int result=kernel_vfs_fallocate(filler,1,offset,step*block);
        if (result==-KERNEL_ENOSPC) { step/=2; continue; }
        assert(!result); offset+=step*block;
        assert(!kernel_vfs_mount_statfs(mount,&stat)); step=stat.free_blocks;
    }
    assert(!kernel_vfs_mount_statfs(mount,&stat));
    if (stat.free_blocks) assert(stat.free_blocks==1 && !kernel_vfs_fallocate(last,1,0,block));
    assert(!kernel_vfs_mount_statfs(mount,&stat) && !stat.free_blocks);
}
static void reserved_write_full(struct kernel_page_cache *cache,struct kernel_vfs_mount *mount,
    unsigned count,int middle,int grouped,int fail_zero)
{
    struct kernel_vfs_file victim={0},filler={0},last={0};
    assert(!create_file(mount,"/reserved-full",0600,&victim));
    assert(!create_file(mount,"/filler",0600,&filler) && !create_file(mount,"/last-block",0600,&last));
    size_t block=kernel_vfs_file_node(&victim)->instance->writeback_block_size,done;
    unsigned second=middle?count-1:2;
    assert(!kernel_vfs_fallocate(&victim,0,0,count*block));
    uint64_t observed=0;
    unsigned char value=0x45;
    assert(!kernel_vfs_pwrite(&victim,0,&value,1,&done) && done==1 && !kernel_vfs_sync(&victim,0,&observed));
    value=0x67;
    assert(!kernel_vfs_pwrite(&victim,second*block,&value,1,&done) && done==1 && !kernel_vfs_sync(&victim,0,&observed));
    assert(!mapped(&victim,0).unwritten && mapped(&victim,1).unwritten && !mapped(&victim,second).unwritten);
    if (!middle) assert(mapped(&victim,3).unwritten);
    struct lwext4_mount_adapter *adapter=lwext4_instance(mount->private_data);
    unsigned char *poison=malloc(block); assert(poison); memset(poison,0xc3,block);
    for (unsigned logical=1;logical<count;logical++) {
        struct ext4_inode_block_range range=mapped(&victim,logical);
        if (range.unwritten) assert(!ext4_blocks_set_direct(&adapter->device,poison,range.block,1));
    }
    free(poison);
    fill_free_space(mount,&filler,&last,block);
    if (grouped) assert(adapter->device.fs->jbd_journal && !kernel_vfs_start_journal_worker(mount));
    uint64_t blocks=allocated(&victim);
    if (fail_zero) fail_direct_block=mapped(&victim,3).block+1;
    value=0x89;
    assert(!kernel_vfs_pwrite(&victim,4*block,&value,1,&done) && done==1);
    int synced=kernel_vfs_sync(&victim,0,&observed);
    fprintf(stderr,"reserved write at full disk: sync=%d block=%zu allocated=%llu\n",synced,block,(unsigned long long)blocks);
    if (fail_zero) {
        assert(synced==-KERNEL_EIO && failed_direct_calls==1 && !fail_direct_block);
        assert(mapped(&victim,4).unwritten && allocated(&victim)==blocks);
        unsigned char *old=calloc(count,block),*actual=malloc(count*block); assert(old && actual);
        old[0]=0x45; old[second*block]=0x67;
        assert(!ext4_fpread(lwext4_node_file(kernel_vfs_file_node(&victim)),0,actual,count*block,&done) &&
            done==count*block && !memcmp(actual,old,count*block));
        free(old); free(actual);
        synced=kernel_vfs_sync(&victim,0,&observed);
    }
    assert(!synced && allocated(&victim)==blocks);
    assert(!kernel_page_cache_invalidate_node(cache,kernel_vfs_file_node(&victim)));
    unsigned char *expected=calloc(count,block); assert(expected);
    expected[0]=0x45; expected[second*block]=0x67; expected[4*block]=0x89;
    expect_bytes(&victim,expected,count*block); free(expected);
    assert(!kernel_vfs_ftruncate(&filler,0) && !kernel_vfs_ftruncate(&last,0));
    assert(!kernel_vfs_close(&filler) && !kernel_vfs_close(&last) && !kernel_vfs_close(&victim));
    puts("PASS reserved full disk: conversion needs no new space; initialized neighbors and zero unwritten bytes persist");
}
static void reserved_abort_reuse(struct kernel_page_cache *cache,struct kernel_vfs_mount *mount)
{
    struct lwext4_mount_adapter *adapter=lwext4_instance(mount->private_data);
    assert(adapter->device.fs->jbd_journal && !adapter->device.fs->jbd_journal->grouped);
    struct kernel_vfs_file victim={0},filler={0},last={0};
    assert(!create_file(mount,"/abort-reuse",0600,&victim));
    assert(!create_file(mount,"/filler",0600,&filler) && !create_file(mount,"/last-block",0600,&last));
    size_t block=adapter->device.lg_bsize,length=16*block,done;
    unsigned char *expected=malloc(length); assert(expected); memset(expected,0x5e,length);
    assert(!kernel_vfs_pwrite(&victim,0,expected,length,&done) && done==length);
    uint64_t observed=0; assert(!kernel_vfs_sync(&victim,0,&observed));
    assert(!kernel_page_cache_invalidate_node(cache,kernel_vfs_file_node(&victim)));
    fill_free_space(mount,&filler,&last,block);
    ext4_file *file=lwext4_node_file(kernel_vfs_file_node(&victim));
    KERNEL_LOCK_SCOPE(io); kernel_vfs_node_lock(kernel_vfs_file_node(&victim),&io,1);
    assert(!ext4_transaction_begin(adapter->mount_point) && !ext4_ftruncate(file,0, UINT32_MAX));
    bool changed;
    int reserved=ext4_fallocate(file,0,0,length,&changed, UINT32_MAX);
    fprintf(stderr,"private free/reuse reservation=%d\n",reserved);
    assert(!reserved || reserved==ENOSPC);
    if (!reserved) {
        unsigned char value=0x45;
        assert(!ext4_fpwrite(file,0,&value,1,&done) && done==1);
        assert(!ext4_fpwrite(file,2*block,&value,1,&done) && done==1);
        assert(!ext4_fpwrite(file,4*block,&value,1,&done) && done==1);
    }
    assert(ext4_transaction_abort(adapter->mount_point,ECANCELED)==(reserved?reserved:ECANCELED));
    kernel_lock_release(&io);
    assert(!kernel_vfs_close(&victim) && !kernel_vfs_open(mount,"/abort-reuse",&victim));
    expect_bytes(&victim,expected,length); free(expected);
    assert(!kernel_vfs_ftruncate(&filler,0) && !kernel_vfs_ftruncate(&last,0));
    assert(!kernel_vfs_close(&filler) && !kernel_vfs_close(&last) && !kernel_vfs_close(&victim));
    puts("PASS private free/reuse abort: retained allocation owners protect original initialized contents");
}
static void delayed_allocation(struct kernel_page_cache *cache, struct kernel_vfs_mount *mount)
{
    struct kernel_vfs_file file={0};
    assert(!create_file(mount,"/delayed",0600,&file));
    struct ext4_fs *fs=lwext4_instance(mount->private_data)->device.fs;
    size_t block=kernel_vfs_file_node(&file)->instance->writeback_block_size;
    uint64_t baseline=fs->delayed_blocks;
    /* Two real preparations may share a block before either source commits.
     * Cancelling one source must preserve its peer's accepted or live range. */
    {
        KERNEL_LOCK_SCOPE(io);kernel_vfs_node_lock(kernel_vfs_file_node(&file),&io,1);
        ext4_file *raw=lwext4_node_file(kernel_vfs_file_node(&file));
        void *a=NULL,*b=NULL;size_t ready;
        assert(!ext4_file_reserve_write(raw,0,2*block,&ready,&a) && ready==2*block);
        assert(!ext4_file_reserve_write(raw,block,block,&ready,&b) && ready==block && b);
        ext4_file_finish_reservation(raw,b,2*block);
        ext4_file_finish_reservation(raw,a,0);
        assert(fs->delayed_blocks==baseline+1);
        ext4_file_release_reservations(raw,0);
        assert(fs->delayed_blocks==baseline);
        assert(!ext4_file_reserve_write(raw,0,2*block,&ready,&a));
        assert(!ext4_file_reserve_write(raw,0,2*block,&ready,&b));
        ext4_file_finish_reservation(raw,a,0);
        assert(fs->delayed_blocks==baseline+2);
        ext4_file_finish_reservation(raw,b,0);
        assert(fs->delayed_blocks==baseline);
        assert(!ext4_file_reserve_write(raw,0,block,&ready,&a));
        ext4_file_accept_reservation(raw,a,block);
        ext4_file_release_reservations(raw,0);
        assert(!ext4_file_reserve_write(raw,0,block,&ready,&b));
        ext4_file_finish_reservation(raw,a,block);
        ext4_file_finish_reservation(raw,b,0);
        assert(fs->delayed_blocks==baseline);
    }
    unsigned char bytes[16*1024];memset(bytes,0x53,sizeof(bytes));size_t done;
    assert(!kernel_vfs_pwrite(&file,0,bytes,sizeof(bytes),&done) && done==sizeof(bytes));
    /* 可见数据归页缓存；此时磁盘映射仍应为空。 */
    struct ext4_inode_block_range hole=mapped(&file,0);
    fprintf(stderr,"delalloc before sync physical=%llu allocated=%llu\n",
        (unsigned long long)hole.block,(unsigned long long)allocated(&file));
    assert(!hole.block && fs->delayed_blocks==baseline+sizeof(bytes)/block);
    expect_bytes(&file,bytes,sizeof(bytes));
    assert(!kernel_vfs_pwrite(&file,block,bytes,block,&done) && done==block);
    assert(fs->delayed_blocks==baseline+sizeof(bytes)/block);
    fail_delayed_allocation=1;
    assert(kernel_page_cache_writeback(cache,kernel_vfs_file_node(&file))==-KERNEL_ENOMEM && !fail_delayed_allocation);
    assert(!mapped(&file,0).block && fs->delayed_blocks==baseline+sizeof(bytes)/block);
    uint64_t observed=0;
    assert(kernel_vfs_sync(&file,0,&observed)==-KERNEL_ENOMEM);
    assert(!kernel_vfs_sync(&file,0,&observed) && fs->delayed_blocks==baseline);
    assert(mapped(&file,0).block);
    assert(!kernel_page_cache_invalidate_node(cache,kernel_vfs_file_node(&file)));
    expect_bytes(&file,bytes,sizeof(bytes));
    assert(!kernel_vfs_ftruncate(&file,0));
    assert(!kernel_vfs_pwrite(&file,0,bytes,sizeof(bytes),&done) && done==sizeof(bytes));
    assert(!kernel_vfs_ftruncate(&file,0));
    assert(!mapped(&file,0).block && fs->delayed_blocks==baseline);
    assert(!kernel_vfs_sync(&file,0,&observed) && !kernel_vfs_close(&file));
    puts("PASS delayed allocation: cache visibility, no physical allocation before sync, batched conversion and truncate");
}

struct admission_fill {
    struct kernel_vfs_file files[2];
    size_t acknowledged[2];
    int error;
};
static void admission_legacy(struct kernel_vfs_mount *mount,struct kernel_vfs_file *file)
{
    struct lwext4_mount_adapter *adapter=lwext4_instance(mount->private_data);
    struct kernel_vfs_node *node=kernel_vfs_file_node(file);
    KERNEL_LOCK_SCOPE(io); kernel_vfs_node_lock(node,&io,1);
    int journal=adapter->device.fs->jbd_journal!=NULL;
    if (journal) assert(!ext4_transaction_begin(adapter->mount_point));
    struct ext4_inode_ref ref; assert(!ext4_fs_get_inode_ref(adapter->device.fs,node->inode,&ref));
    ext4_inode_clear_flag(ref.inode,EXT4_INODE_FLAG_EXTENTS); memset(ref.inode->blocks,0,sizeof(ref.inode->blocks));ref.dirty=true;
    assert(!ext4_fs_put_inode_ref(&ref));
    assert(!(journal?ext4_transaction_end(adapter->mount_point):ext4_cache_flush(adapter->mount_point)));
}
static void admission_prefix(struct kernel_page_cache *cache,struct kernel_vfs_mount *mount,int legacy)
{
    struct kernel_vfs_file target={0},filler={0},last={0},spare={0};
    assert(!create_file(mount,"/prefix",0600,&target) && !create_file(mount,"/filler",0600,&filler) &&
        !create_file(mount,"/last",0600,&last) && !create_file(mount,"/spare",0600,&spare));
    if (legacy) admission_legacy(mount,&target);
    size_t block=kernel_vfs_file_node(&target)->instance->writeback_block_size;
    assert(!kernel_vfs_fallocate(&last,0,0,2*block));
    fill_free_space(mount,&filler,&spare,block);
    assert(!kernel_vfs_ftruncate(&last,0));
    unsigned char bytes[4*BOAROS_PAGE_SIZE];memset(bytes,0x37,sizeof(bytes));size_t done;
    assert(!kernel_vfs_pwrite(&target,0,bytes,sizeof(bytes),&done));
    fprintf(stderr,"partial storage admission: accepted=%zu expected=%zu legacy=%d\n",done,2*block,legacy);
    assert(done==2*block && kernel_vfs_file_size(&target)==done);
    uint64_t observed=0; assert(!kernel_vfs_sync(&target,0,&observed) && !kernel_vfs_error_sequence(&target));
    assert(!kernel_page_cache_invalidate_node(cache,kernel_vfs_file_node(&target)));
    expect_bytes(&target,bytes,done);
    assert(kernel_vfs_pwrite(&target,done,bytes,1,&done)==-KERNEL_ENOSPC && !done);
    assert(!kernel_vfs_sync(&target,0,&observed));
    assert(!kernel_vfs_close(&target) && !kernel_vfs_close(&filler) && !kernel_vfs_close(&last) && !kernel_vfs_close(&spare));
    puts("PASS partial storage admission: exact filesystem-block prefix, unchanged rejected EOF, sync and reloaded bytes");
}
static void admission_fill_disk(void *argument)
{
    struct admission_fill *fill=argument;
    unsigned char bytes[128*1024];
    for (unsigned step=0;step<4096;step++) {
        unsigned which=step%2; memset(bytes,0x61+which,sizeof(bytes)); size_t done;
        int error=kernel_vfs_pwrite(&fill->files[which],fill->acknowledged[which],bytes,sizeof(bytes),&done);
        fill->acknowledged[which]+=done;
        if (error) { assert(error==-KERNEL_ENOSPC && !done); fill->error=error; return; }
    }
    assert(!"filesystem did not become full");
}
static void admission_full(struct kernel_page_cache *cache,struct kernel_vfs_mount *mount,int legacy)
{
    struct admission_fill fill={0};
    assert(!create_file(mount,"/accepted-a",0600,&fill.files[0]) &&
        !create_file(mount,"/accepted-b",0600,&fill.files[1]));
    if (legacy) admission_legacy(mount,&fill.files[1]);
    assert(!kernel_page_cache_start_worker(cache) && !kernel_vfs_start_journal_worker(mount));
    assert(!kernel_page_cache_run_cpu0(cache,admission_fill_disk,&fill) && fill.error==-KERNEL_ENOSPC);
    for (unsigned which=0;which<2;which++) {
        uint64_t observed=0; int synced=kernel_vfs_sync(&fill.files[which],0,&observed);
        fprintf(stderr,"full admission inode=%u acknowledged=%zu fsync=%d\n",which,fill.acknowledged[which],synced);
        assert(!synced && !kernel_vfs_error_sequence(&fill.files[which]));
        struct kernel_vfs_stat stat; assert(!kernel_vfs_fstat(&fill.files[which],&stat) && stat.size==fill.acknowledged[which]);
        assert(!kernel_page_cache_invalidate_node(cache,kernel_vfs_file_node(&fill.files[which])));
        unsigned char *expected=malloc(fill.acknowledged[which]);assert(expected);
        memset(expected,0x61+which,fill.acknowledged[which]);
        expect_bytes(&fill.files[which],expected,fill.acknowledged[which]); free(expected);
        size_t done; assert(kernel_vfs_pwrite(&fill.files[which],fill.acknowledged[which],"!",1,&done)==-KERNEL_ENOSPC && !done);
        assert(!kernel_vfs_pwrite(&fill.files[which],0,"+",1,&done) && done==1 && !kernel_vfs_sync(&fill.files[which],0,&observed));
        assert(!kernel_vfs_close(&fill.files[which]));
    }
    kernel_page_cache_stop_worker(cache);
    puts("PASS full admission: ENOSPC rejects unaccepted bytes; all prior inodes fsync/reload exact prefixes, overwrite and clean shutdown");
}
static struct kernel_rwlock *reclaim_held_lock;
static unsigned reclaim_lock_ready,reclaim_lock_resume,reclaim_allocate_done,reclaim_backend_ready,reclaim_probe_start;
static struct kernel_vfs_mount *reclaim_mount;
static enum physical_page_status reclaim_result;
static uint64_t *reclaim_held_pages;
static size_t reclaim_held_count;
static void *hold_reclaim_metadata(void *unused)
{
    (void)unused;cache_test_attach(2);KERNEL_LOCK_SCOPE(guard);
    kernel_rwlock_write(reclaim_held_lock,&guard);
    __atomic_store_n(&reclaim_lock_ready,1,__ATOMIC_RELEASE);
    while (!__atomic_load_n(&reclaim_lock_resume,__ATOMIC_ACQUIRE)) sched_yield();
    return NULL;
}
static void *allocate_under_backend(void *unused)
{
    (void)unused;cache_test_attach(1);kernel_cpu_current()->logical_id=0;
    struct lwext4_mount_adapter *adapter=lwext4_instance(reclaim_mount->private_data);
    struct kernel_page_cache *cache=adapter->instance.page_cache;
    adapter->locks.lock(adapter->locks.context);
    __atomic_store_n(&reclaim_backend_ready,1,__ATOMIC_RELEASE);
    while (!__atomic_load_n(&reclaim_probe_start,__ATOMIC_ACQUIRE)) sched_yield();
    uint64_t page;
    reclaim_result=physical_page_allocate(cache->allocator,&page);
    if (!reclaim_result) {
        reclaim_held_pages[reclaim_held_count++]=page;
        assert(!physical_page_available(cache->allocator));
        {
            KERNEL_LOCK_SCOPE(held);kernel_rwlock_write(reclaim_held_lock,&held);
            assert(physical_page_allocate(cache->allocator,&page)==PHYSICAL_PAGE_STATUS_EMPTY);
        }
        {
            struct kernel_mutex same_rank;kernel_mutex_init(&same_rank,41,(uintptr_t)&same_rank);
            KERNEL_LOCK_SCOPE(held);kernel_mutex_lock(&same_rank,&held);
            assert(physical_page_allocate(cache->allocator,&page)==PHYSICAL_PAGE_STATUS_EMPTY);
        }
        while (!physical_page_allocate(cache->allocator,&page)) reclaim_held_pages[reclaim_held_count++]=page;
        struct kernel_page_cache_statistics stats;kernel_page_cache_get_statistics(cache,&stats);
        assert(!stats.current_pages && !physical_page_available(cache->allocator));
    }
    adapter->locks.unlock(adapter->locks.context);
    __atomic_store_n(&reclaim_allocate_done,1,__ATOMIC_RELEASE);
    return NULL;
}
static void namespace_reclaim(struct kernel_page_cache *cache,struct kernel_vfs_mount *mount)
{
    struct lwext4_mount_adapter *adapter=lwext4_instance(mount->private_data);
    if (adapter->device.fs->jbd_journal) assert(!ext4_journal_group_drain(adapter->mount_point));
    kernel_page_cache_stop_worker(cache);
    struct kernel_page_cache_statistics stats;
    observe_cache_metadata=1;kernel_page_cache_get_statistics(cache,&stats);observe_cache_metadata=0;
    assert(stats.current_pages && cache_metadata_lock);
    reclaim_held_lock=cache_metadata_lock;reclaim_mount=mount;
    reclaim_held_pages=calloc(physical_page_total(cache->allocator),sizeof(*reclaim_held_pages));assert(reclaim_held_pages);
    reclaim_held_count=0;
    reclaim_lock_ready=reclaim_lock_resume=reclaim_allocate_done=reclaim_backend_ready=reclaim_probe_start=0;
    pthread_t holder,allocator;
    assert(!pthread_create(&allocator,NULL,allocate_under_backend,NULL));
    wait_progress(&reclaim_backend_ready,1);
    while (physical_page_available(cache->allocator)) {
        assert(!physical_page_allocate(cache->allocator,&reclaim_held_pages[reclaim_held_count]));reclaim_held_count++;
    }
    uint64_t cached=stats.current_pages;
    do {
        assert(!physical_page_allocate(cache->allocator,&reclaim_held_pages[reclaim_held_count]));reclaim_held_count++;
        kernel_page_cache_get_statistics(cache,&stats);
    } while (stats.current_pages==cached);
    assert(stats.current_pages && !physical_page_available(cache->allocator));
    assert(!pthread_create(&holder,NULL,hold_reclaim_metadata,NULL));
    wait_progress(&reclaim_lock_ready,1);unsigned parked=mm_test_parked(1);
    __atomic_store_n(&reclaim_probe_start,1,__ATOMIC_RELEASE);
    while (!__atomic_load_n(&reclaim_allocate_done,__ATOMIC_ACQUIRE) && mm_test_parked(1)==parked) sched_yield();
    fprintf(stderr,"busy clean reclaim: done=%u result=%u\n",reclaim_allocate_done,reclaim_result);
    assert(!reclaim_allocate_done);
    __atomic_store_n(&reclaim_lock_resume,1,__ATOMIC_RELEASE);
    assert(!pthread_join(holder,NULL) && !pthread_join(allocator,NULL) && !reclaim_result);
    for (size_t i=0;i<reclaim_held_count;i++) assert(!physical_page_release(cache->allocator,reclaim_held_pages[i]));
    free(reclaim_held_pages);
    puts("PASS backend clean reclaim: busy metadata waits, clean pages allocate under backend lock without I/O, no-clean exhaustion remains EMPTY");
}
static void namespace_pressure(struct kernel_page_cache *cache,struct kernel_vfs_mount *mount,int direct)
{
    assert(!kernel_page_cache_start_worker(cache) && !kernel_vfs_start_journal_worker(mount));
    size_t length=4*1024*1024,chunk=128*1024;
    unsigned char *bytes=malloc(length);assert(bytes);memset(bytes,0x68,length);
    for (unsigned index=0;index<(direct?1:48);index++) {
        char name[32];snprintf(name,sizeof(name),"/namespace-%u",index);
        struct kernel_vfs_file file={0};
        int error=create_file(mount,name,0600,&file);
        fprintf(stderr,"namespace index=%u create=%d free=%llu\n",index,error,
            (unsigned long long)physical_page_available(cache->allocator));
        assert(!error);
        for (size_t offset=0;offset<length;offset+=chunk) {
            size_t done;
            error=kernel_vfs_pwrite(&file,offset,bytes+offset,chunk,&done);
            if (error || done!=chunk) {
                struct kernel_vfs_statfs space;assert(!kernel_vfs_mount_statfs(mount,&space));
                struct kernel_memory_statistics memory;kernel_memory_snapshot(cache->allocator,&memory);
                fprintf(stderr,"namespace write failure index=%u offset=%zu error=%d prefix=%zu disk-free=%llu free=%llu cached=%llu dirty=%llu reclaimable=%llu\n",
                    index,offset,error,done,(unsigned long long)space.free_blocks,(unsigned long long)memory.free,
                    (unsigned long long)memory.cached,(unsigned long long)memory.dirty,(unsigned long long)memory.reclaimable);
            }
            assert(!error && done==chunk);
        }
        uint64_t observed=0;error=kernel_vfs_sync(&file,0,&observed);
        fprintf(stderr,"namespace index=%u sync=%d free=%llu\n",index,error,
            (unsigned long long)physical_page_available(cache->allocator));
        assert(!error);
        assert(!kernel_vfs_close(&file));
        error=kernel_vfs_open(mount,name,&file);
        fprintf(stderr,"namespace index=%u reopen=%d free=%llu\n",index,error,
            (unsigned long long)physical_page_available(cache->allocator));
        assert(!error);
        expect_bytes(&file,bytes,length);
        assert(!kernel_vfs_close(&file));
    }
    free(bytes);
    namespace_reclaim(cache,mount);
    puts("PASS namespace pressure: unified allocator writes then creates/reopens files and returns all owners");
}
static uint64_t metadata_clock(void *unused) { (void)unused;return 1; }
static void metadata_limit(struct jbd_journal *journal,size_t block)
{
    struct ext4_fs *fs = journal->jbd_fs->inode_ref.fs;
    ext4_fs_state_read_lock(fs);
    jbd_memory_set_limit(journal,jbd_memory_used(journal));
    (void)jbd_journal_operation_room(journal,NULL,0,0,NULL,0);
    assert(!jbd_memory_cached(journal));
    size_t occupied=jbd_memory_used(journal),margin=block;
    do { jbd_memory_set_limit(journal,occupied+margin);margin+=block; }
    while (!jbd_journal_operation_room(journal,NULL,0,0,NULL,0));
    ext4_fs_state_unlock(fs);
}
static enum kernel_block_status (*metadata_original_read)(void *,uint64_t,void *,size_t);
static unsigned metadata_read_error;
static enum kernel_block_status metadata_device_read(void *context,uint64_t offset,void *bytes,size_t length)
{
    if (metadata_read_error) { metadata_read_error=0;return KERNEL_BLOCK_STATUS_IO; }
    return metadata_original_read(context,offset,bytes,length);
}
struct metadata_fault {
    struct lwext4_mount_adapter *adapter;
    void *bytes;
    ext4_fsblk_t block;
    unsigned calls,io;
};
static int apply_metadata_fault(void *context)
{
    struct metadata_fault *fault=context;fault->calls++;
    if (fault->io) return ext4_blocks_get_direct(&fault->adapter->device,fault->bytes,fault->block,1);
    void *allocation=NULL;
    assert(kernel_heap_allocate(fault->adapter->instance.heap,2*BOAROS_PAGE_SIZE,&allocation)==KERNEL_HEAP_STATUS_EMPTY);
    return ENOMEM;
}
struct metadata_nested { const char *mount;ext4_file file;unsigned calls; };
static int apply_metadata_nested(void *context)
{
    struct metadata_nested *request=context;request->calls++;
    return ext4_create_child(request->mount,EXT4_INODE_ROOT_INDEX,"budget-nested",0100640,0,NULL,&request->file,NULL,0,0);
}
static void namespace_budget(struct kernel_vfs_mount *mount)
{
    struct lwext4_mount_adapter *adapter=lwext4_instance(mount->private_data);
    struct jbd_journal *journal=adapter->device.fs->jbd_journal;assert(journal && !journal->grouped);
    size_t block=adapter->device.lg_bsize,length=32*block,done;
    unsigned char *bytes=malloc(length);assert(bytes);memset(bytes,0x47,length);
    char path[96];snprintf(path,sizeof(path),"%sbudget-seed",adapter->mount_point);
    ext4_file seed;assert(!ext4_fopen(&seed,path,"w+") && !ext4_fwrite(&seed,bytes,length,&done) && done==length);
    struct kernel_vfs_file source={0},victim={0},removed={0},directory={0};
    assert(!kernel_vfs_mkdir(mount,"/budget-left",0700) && !kernel_vfs_mkdir(mount,"/budget-right",0700) &&
        !kernel_vfs_mkdir(mount,"/budget-empty",0700));
    assert(!create_file(mount,"/budget-left/source",0600,&source) && !create_file(mount,"/budget-right/victim",0600,&victim) &&
        !create_file(mount,"/budget-unlink",0600,&removed) && !kernel_vfs_open(mount,"/budget-empty",&directory));
    char owner_path[96];snprintf(owner_path,sizeof(owner_path),"%sbudget-left/source",adapter->mount_point);
    unsigned char padding[48]={1};uint32_t capability[5]={0x02000000,1,0,0,0};
    assert(!ext4_setxattr(owner_path,"user.padding",12,padding,sizeof(padding)) &&
        !ext4_setxattr(owner_path,"security.capability",19,capability,sizeof(capability)));
    struct ext4_inode owner_inode;ext4_file *owner=lwext4_node_file(kernel_vfs_file_node(&source));
    assert(!ext4_fraw_inode_fill(owner,&owner_inode) && ext4_inode_get_file_acl(&owner_inode,&adapter->device.fs->sb));
    uint64_t observed=0;
    assert(!kernel_vfs_pwrite(&source,0,"S",1,&done) && done==1 && !kernel_vfs_sync(&source,0,&observed));observed=0;
    assert(!kernel_vfs_pwrite(&victim,0,"V",1,&done) && done==1 && !kernel_vfs_sync(&victim,0,&observed));observed=0;
    assert(!kernel_vfs_pwrite(&removed,0,"U",1,&done) && done==1 && !kernel_vfs_sync(&removed,0,&observed));
    struct kernel_vfs_path *root=NULL;assert(!kernel_vfs_path_root(mount,adapter->instance.heap,&root));
    struct ext4_journal_runtime runtime={.now_ns=metadata_clock};
    assert(!ext4_journal_group_enable(adapter->mount_point,&runtime,4*1024*1024));
    static const char *operations[]={"create","mkdir","mknod","rename","link","symlink","unlink","rmdir","chown"};
    for (unsigned operation=0;operation<9;operation++) {
        jbd_memory_set_limit(journal,4*1024*1024);assert(!ext4_journal_group_drain(adapter->mount_point));
        memset(bytes,0x69+operation,length);assert(!ext4_fpwrite(&seed,0,bytes,length,&done) && done==length);
        metadata_limit(journal,block);
        uint64_t before=jbd_memory_budget_failures(journal),checkpoint=journal->checkpoint_sequence;
        metadata_probe=1;metadata_open_calls=0;metadata_failed_step=NULL;
        struct kernel_vfs_file file={0};int result;
        switch (operation) {
        case 0:result=create_file(mount,"/budget-created",0640,&file);break;
        case 1:result=kernel_vfs_mkdir(mount,"/budget-created-dir",0710);break;
        case 2:result=kernel_vfs_mknod_at(root,root,"/budget-created-fifo",KERNEL_VFS_S_IFIFO|0620,0);break;
        case 3:result=kernel_vfs_rename_at(root,root,root,"/budget-left/source","/budget-right/victim",0);break;
        case 4:result=kernel_vfs_link_at(source.path,root,root,"/budget-hardlink");break;
        case 5:result=kernel_vfs_symlink_at(root,root,"/budget-hardlink","/budget-symlink");break;
        case 6:result=kernel_vfs_unlink(mount,"/budget-unlink");break;
        case 7:result=kernel_vfs_rmdir(mount,"/budget-empty");break;
        default:result=kernel_vfs_file_set_owner(&source,12345,23456);break;
        }
        metadata_probe=0;
        fprintf(stderr,"metadata budget %s result=%d step=%s attempts=%u failures=%llu checkpoint=%llu->%llu\n",
            operations[operation],result,metadata_failed_step?metadata_failed_step:"none",metadata_open_calls,(unsigned long long)(jbd_memory_budget_failures(journal)-before),
            (unsigned long long)checkpoint,(unsigned long long)journal->checkpoint_sequence);
        assert(!result);
        assert(jbd_memory_budget_failures(journal)>before && journal->checkpoint_sequence>checkpoint);
        if (operation==8) {
            uint64_t target;
            assert(!ext4_file_sync_metadata_prepare(owner,false,&target) &&
                target==(journal->accepted_sequence>journal->durable_sequence?journal->accepted_sequence:0));
        }
        jbd_memory_set_limit(journal,4*1024*1024);assert(!ext4_journal_group_drain(adapter->mount_point));
        struct kernel_vfs_stat stat;
        if (!operation) assert(!kernel_vfs_fstat(&file,&stat) && (stat.mode&0777)==0640 && !kernel_vfs_close(&file));
        if (operation==1) assert(!kernel_vfs_open(mount,"/budget-created-dir",&file) && !kernel_vfs_fstat(&file,&stat) && (stat.mode&0777)==0710 && !kernel_vfs_close(&file));
        if (operation==2) assert(!kernel_vfs_open(mount,"/budget-created-fifo",&file) && !kernel_vfs_fstat(&file,&stat) && stat.mode==(KERNEL_VFS_S_IFIFO|0620) && !kernel_vfs_close(&file));
        if (operation==3) {
            assert(kernel_vfs_open(mount,"/budget-left/source",&file)==-KERNEL_ENOENT);
            assert(!kernel_vfs_open(mount,"/budget-right/victim",&file) && kernel_vfs_file_node(&file)==kernel_vfs_file_node(&source) && !kernel_vfs_close(&file));
            assert(!kernel_vfs_fstat(&victim,&stat) && !stat.nlink);expect_bytes(&victim,(const unsigned char *)"V",1);
        }
        if (operation==4) assert(!kernel_vfs_fstat(&source,&stat) && stat.nlink==2);
        if (operation==5) { char target[64];assert(!kernel_vfs_readlink_at(root,root,"/budget-symlink",target,sizeof(target),&done) && done==strlen("/budget-hardlink") && !memcmp(target,"/budget-hardlink",done)); }
        if (operation==6) { assert(!kernel_vfs_fstat(&removed,&stat) && !stat.nlink);expect_bytes(&removed,(const unsigned char *)"U",1); }
        if (operation==7) assert(!kernel_vfs_fstat(&directory,&stat) && !stat.nlink);
        if (operation==8) {
            assert(!ext4_fraw_inode_fill(owner,&owner_inode) && ext4_inode_get_uid(&owner_inode)==12345 && ext4_inode_get_gid(&owner_inode)==23456);
            snprintf(owner_path,sizeof(owner_path),"%sbudget-right/victim",adapter->mount_point);
            size_t size;assert(ext4_getxattr(owner_path,"security.capability",19,capability,sizeof(capability),&size)==ENODATA);
        }
    }
    assert(!kernel_vfs_path_release(&root) && !kernel_vfs_close(&source) && !kernel_vfs_close(&victim) && !kernel_vfs_close(&removed) && !kernel_vfs_close(&directory));
    jbd_memory_set_limit(journal,4*1024*1024);assert(!ext4_journal_group_drain(adapter->mount_point));
    struct kernel_vfs_statfs space_before,space_after;
    assert(!kernel_vfs_mount_statfs(mount,&space_before));
    metadata_limit(journal,block);
    uint64_t before=jbd_memory_budget_failures(journal),checkpoint=journal->checkpoint_sequence;
    struct kernel_vfs_file absent={0};metadata_open_calls=0;metadata_probe=1;
    int result=create_file(mount,"/budget-too-large",0600,&absent);metadata_probe=0;
    fprintf(stderr,"metadata no older owner: result=%d attempts=%u failures=%llu\n",result,metadata_open_calls,(unsigned long long)(jbd_memory_budget_failures(journal)-before));
    assert(result==-KERNEL_ENOMEM && metadata_open_calls==1 && jbd_memory_budget_failures(journal)>before && journal->checkpoint_sequence==checkpoint);
    jbd_memory_set_limit(journal,4*1024*1024);assert(!ext4_journal_group_drain(adapter->mount_point));
    assert(kernel_vfs_open(mount,"/budget-too-large",&absent)==-KERNEL_ENOENT && !kernel_vfs_mount_statfs(mount,&space_after));
    assert(space_before.free_blocks==space_after.free_blocks && space_before.free_inodes==space_after.free_inodes);

    assert(!ext4_fpwrite(&seed,0,bytes,length,&done) && done==length);metadata_limit(journal,block);
    struct metadata_nested nested={.mount=adapter->mount_point};checkpoint=journal->checkpoint_sequence;
    assert(!ext4_transaction_begin(adapter->mount_point));
    assert(ext4_metadata_execute(adapter->mount_point,apply_metadata_nested,&nested)==ENOMEM && nested.calls==1 && journal->checkpoint_sequence==checkpoint);
    assert(ext4_transaction_abort(adapter->mount_point,ENOMEM)==ENOMEM);
    if (nested.file.mp) assert(!ext4_fclose(&nested.file));
    jbd_memory_set_limit(journal,4*1024*1024);assert(!ext4_journal_group_drain(adapter->mount_point));
    assert(kernel_vfs_open(mount,"/budget-nested",&absent)==-KERNEL_ENOENT);

    assert(!ext4_fpwrite(&seed,0,bytes,length,&done) && done==length);
    struct ext4_inode_ref inode;struct ext4_inode_block_range range;
    assert(!ext4_fs_get_inode_ref(adapter->device.fs,seed.inode,&inode) && !ext4_fs_get_inode_dblk_range(&inode,0,1,false,&range) && range.block && !ext4_fs_put_inode_ref(&inode));
    struct metadata_fault fault={.adapter=adapter,.block=range.block,.bytes=bytes,.io=1};
    before=jbd_memory_budget_failures(journal);checkpoint=journal->checkpoint_sequence;
    metadata_original_read=adapter->block->read;adapter->block->read=metadata_device_read;metadata_read_error=1;
    result=ext4_metadata_execute(adapter->mount_point,apply_metadata_fault,&fault);adapter->block->read=metadata_original_read;
    assert(result==EIO && fault.calls==1 && !metadata_read_error && jbd_memory_budget_failures(journal)==before && journal->checkpoint_sequence==checkpoint && !journal->error);
    uint64_t *held=calloc(physical_page_total(adapter->instance.heap->page_allocator),sizeof(*held));assert(held);size_t count=0;
    while (!physical_page_allocate(adapter->instance.heap->page_allocator,&held[count])) count++;
    fault.io=0;fault.calls=0;
    result=ext4_metadata_execute(adapter->mount_point,apply_metadata_fault,&fault);
    assert(result==ENOMEM && fault.calls==1 && jbd_memory_budget_failures(journal)==before && journal->checkpoint_sequence==checkpoint);
    for (size_t i=0;i<count;i++) assert(!physical_page_release(adapter->instance.heap->page_allocator,held[i]));
    free(held);jbd_memory_set_limit(journal,4*1024*1024);assert(!ext4_journal_group_drain(adapter->mount_point));
    unsigned char *actual=malloc(length);assert(actual);
    assert(!ext4_fpread(&seed,0,actual,length,&done) && done==length && !memcmp(bytes,actual,length));
    free(actual);free(bytes);assert(!ext4_fclose(&seed));
    puts("PASS metadata budget: namespace retries keep modes/identity/live orphans; oversized/nested/real OOM/device EIO retain errors and old data");
}
static unsigned punch_started,punch_completed;
static struct kernel_vfs_file *punch_file;
static size_t punch_block;
static void *punch_during_writeback(void *unused)
{
    /* actor2 在 CPU1，等待 CPU0 worker 归还旧写回版本。 */
    (void)unused; cache_test_attach(2);
    __atomic_store_n(&punch_started,1,__ATOMIC_RELEASE);
    assert(!kernel_vfs_fallocate(punch_file,3,punch_block,3*punch_block));
    __atomic_store_n(&punch_completed,1,__ATOMIC_RELEASE);
    return NULL;
}
static void active_writeback(struct kernel_vfs_mount *mount)
{
    struct kernel_vfs_file file={0}; assert(!create_file(mount,"/parallel",0600,&file));
    size_t length=4*BOAROS_PAGE_SIZE,done;
    unsigned char *expected=malloc(length); assert(expected); memset(expected,0x9b,length);
    assert(!kernel_vfs_pwrite(&file,0,expected,length,&done) && done==length);
    pause_write=1; write_entered=resume_write=sync_done=0;
    pthread_t writer,puncher;
    assert(!pthread_create(&writer,NULL,sync_file,&file));
    wait_progress(&write_entered,1);
    punch_file=&file; punch_block=kernel_vfs_file_node(&file)->instance->writeback_block_size;
    unsigned parked=mm_test_parked(2);
    assert(!pthread_create(&puncher,NULL,punch_during_writeback,NULL));
    wait_progress(&punch_started,1);
    while (mm_test_parked(2)==parked) sched_yield();
    assert(!punch_completed);
    __atomic_store_n(&resume_write,1,__ATOMIC_RELEASE);
    assert(!pthread_join(writer,NULL) && !pthread_join(puncher,NULL));
    memset(expected+punch_block,0,3*punch_block); expect_bytes(&file,expected,length);
    assert(!kernel_page_cache_invalidate_node(kernel_vfs_file_node(&file)->instance->page_cache,kernel_vfs_file_node(&file)));
    expect_bytes(&file,expected,length);
    assert(!mapped(&file,1).block && !mapped(&file,2).block && !mapped(&file,3).block);
    free(expected); assert(!kernel_vfs_close(&file));
    puts("PASS active writeback: handshake waits for old I/O, then punched mappings stay absent and outside bytes survive");
}
struct orphan_writeback { struct kernel_page_cache *cache; struct kernel_vfs_node *node; uint64_t length; };
static unsigned orphan_close_done;
static void *orphan_writeback(void *argument)
{
    struct orphan_writeback *request=argument;
    cache_test_attach(0);
    assert(!kernel_page_cache_writeback_range(request->cache,request->node,0,request->length));
    assert(!kernel_vfs_node_release(&request->node));
    return NULL;
}
static void *orphan_last_close(void *argument)
{
    cache_test_attach(2);
    assert(!kernel_vfs_close(argument));
    __atomic_store_n(&orphan_close_done,1,__ATOMIC_RELEASE);
    return NULL;
}
static void unlink_during_writeback(struct kernel_page_cache *cache,struct kernel_vfs_mount *mount)
{
    struct kernel_vfs_file file={0};size_t done;
    assert(!create_file(mount,"/unlink-active",0600,&file));
    unsigned char bytes[4096];memset(bytes,0x76,sizeof(bytes));
    assert(!kernel_vfs_pwrite(&file,0,bytes,sizeof(bytes),&done) && done==sizeof(bytes));
    assert(!kernel_vfs_unlink(mount,"/unlink-active"));
    struct orphan_writeback request={cache,kernel_vfs_file_node(&file),sizeof(bytes)};
    assert(!kernel_vfs_node_acquire(request.node));
    pause_write=1;write_entered=resume_write=orphan_close_done=0;
    pthread_t writer,closer;
    assert(!pthread_create(&writer,NULL,orphan_writeback,&request));
    wait_progress(&write_entered,1);
    unsigned parked=mm_test_parked(2);
    assert(!pthread_create(&closer,NULL,orphan_last_close,&file));
    while (mm_test_parked(2)==parked) sched_yield();
    assert(!__atomic_load_n(&orphan_close_done,__ATOMIC_ACQUIRE));
    __atomic_store_n(&resume_write,1,__ATOMIC_RELEASE);
    assert(!pthread_join(writer,NULL) && !pthread_join(closer,NULL));
    assert(orphan_close_done && !request.node);
    cache_test_attach(0);
    assert(!create_file(mount,"/unlink-active",0600,&file));
    memset(bytes,0x29,sizeof(bytes));
    assert(!kernel_vfs_pwrite(&file,0,bytes,sizeof(bytes),&done) && done==sizeof(bytes));
    uint64_t error=0;assert(!kernel_vfs_sync(&file,0,&error));expect_bytes(&file,bytes,sizeof(bytes));
    assert(!kernel_vfs_close(&file) && !kernel_vfs_unlink(mount,"/unlink-active"));
    puts("PASS unlink/final CPU1 close waits for old writeback, retires snapshots and preserves reused inode data");
}
#ifdef CACHE_MM_CASES
static unsigned prepare_pause,prepare_entered,prepare_resume;
static int replaced_prepare_error;
int __real_kernel_page_cache_prepare_mapping_write(struct kernel_page_cache *,struct kernel_vfs_node *,uint64_t,struct kernel_page_cache_reservation *);
int __wrap_kernel_page_cache_prepare_mapping_write(struct kernel_page_cache *cache,struct kernel_vfs_node *node,uint64_t index,struct kernel_page_cache_reservation *reservation)
{
    int held=__atomic_exchange_n(&prepare_pause,0,__ATOMIC_ACQ_REL);
    if (held) {
        assert(!kernel_io_context_current()->locks && !kernel_cpu_current()->raw_locks);
        __atomic_store_n(&prepare_entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&prepare_resume,__ATOMIC_ACQUIRE)) sched_yield();
    }
    int error=__real_kernel_page_cache_prepare_mapping_write(cache,node,index,reservation);
    if (held) replaced_prepare_error=error;
    return error;
}
struct replaced_fault {
    struct kernel_mm *mm;
    uint64_t address;
    size_t done;
    enum kernel_uaccess_status result;
};
static void *replace_fault_actor(void *argument)
{
    struct replaced_fault *fault=argument;
    cache_test_attach(1); mm_test_bind(fault->mm);
    assert(!kernel_mm_context(fault->mm,&mm_test_context));
    unsigned char byte=0xa7;
    fault->result=kernel_copy_to_user(fault->mm,fault->address,&byte,1,&fault->done);
    mm_test_bind(NULL);mm_test_context=0;
    return NULL;
}
static void admission_fault(struct kernel_page_cache *cache,struct kernel_heap *heap,
    struct physical_page_allocator *pages,struct kernel_vfs_mount *mount)
{
    struct arch_mmu_page_table kernel={0};struct arch_mmu_user_space space={0};struct kernel_mm mm={0};
    assert(!riscv_sv39_page_table_init(&kernel,pages));kernel.state=ARCH_MMU_STATE_ACTIVE;
    assert(!arch_mmu_user_space_init(&space,pages,&kernel) && !kernel_mm_create(&mm,&space) && !kernel_mm_vma_enable(&mm,heap));
    assert(!kernel_mm_context(&mm,&mm_test_context) && !kernel_mm_brk_initialize(&mm,0x10000,0x20000));
    for (unsigned legacy=0;legacy<2;legacy++) {
        struct kernel_vfs_file file={0};assert(!create_file(mount,legacy?"/fault-legacy":"/fault-extents",0600,&file));
        if (legacy) admission_legacy(mount,&file);
        struct kernel_uaccess_iovec source={.base=0x900000,.length=BOAROS_PAGE_SIZE};
        struct kernel_vfs_user_write_result result;
        assert(!kernel_vfs_write_user(&file,&mm,&source,1,BOAROS_PAGE_SIZE,0,0,NULL,&result));
        assert(result.access==KERNEL_UACCESS_STATUS_FAULT && !result.written && !result.end_offset);
        struct kernel_vfs_stat stat;assert(!kernel_vfs_fstat(&file,&stat) && !stat.size && (!legacy && lwext4_instance(mount->private_data)->device.fs->jbd_journal ? !stat.blocks : (legacy?!stat.blocks:stat.blocks>0)));
        if (!legacy) {
            assert(!kernel_vfs_ftruncate(&file,3*BOAROS_PAGE_SIZE));
            assert(!kernel_vfs_write_user(&file,&mm,&source,1,BOAROS_PAGE_SIZE,1,123,NULL,&result));
            assert(result.access==KERNEL_UACCESS_STATUS_FAULT && !result.written);
            assert(!((struct lwext4_node *)kernel_vfs_file_node(&file))->delalloc.blocks);
        }
        uint64_t observed=0;assert(!kernel_vfs_sync(&file,0,&observed) && !kernel_vfs_error_sequence(&file));
        assert(!kernel_vfs_close(&file));
    }
    for (unsigned cleanup=0;cleanup<3;cleanup++) {
        const char *name=cleanup==2?"/fault-close":cleanup?"/fault-unlink":"/fault-truncate";
        struct kernel_vfs_file file={0}; assert(!create_file(mount,name,0600,&file));admission_legacy(mount,&file);
        struct kernel_vfs_statfs before,after;assert(!kernel_vfs_mount_statfs(mount,&before));
        struct kernel_uaccess_iovec source={.base=0x900000,.length=BOAROS_PAGE_SIZE};
        struct kernel_vfs_user_write_result result; fail_complete=1;
        assert(kernel_vfs_write_user(&file,&mm,&source,1,BOAROS_PAGE_SIZE,0,0,NULL,&result)==-KERNEL_EIO && !fail_complete);
        assert(result.access==KERNEL_UACCESS_STATUS_FAULT && !result.written && !kernel_vfs_error_sequence(&file));
        struct kernel_vfs_stat stat;assert(!kernel_vfs_fstat(&file,&stat) && !stat.size && stat.blocks);
        if (cleanup==1) assert(!kernel_vfs_unlink(mount,name));
        else if (!cleanup) assert(!kernel_vfs_ftruncate(&file,0));
        assert(!kernel_vfs_close(&file) && !kernel_vfs_mount_statfs(mount,&after));
        if (cleanup!=2) assert(after.free_blocks==before.free_blocks);
    }
    {
        struct kernel_vfs_file file={0}; assert(!create_file(mount,"/legacy-wb-error",0600,&file));admission_legacy(mount,&file);
        unsigned char bytes[BOAROS_PAGE_SIZE];memset(bytes,0x59,sizeof(bytes));size_t done;
        write_error=1;
        assert(!kernel_vfs_pwrite(&file,0,bytes,sizeof(bytes),&done) && done==sizeof(bytes) && !write_error);
        uint64_t observed=0;assert(kernel_vfs_sync(&file,0,&observed)==-KERNEL_EIO && !kernel_vfs_sync(&file,0,&observed));
        assert(!kernel_page_cache_invalidate_node(cache,kernel_vfs_file_node(&file)));
        expect_bytes(&file,bytes,sizeof(bytes));assert(!kernel_vfs_close(&file));
    }
    mm_test_bind(NULL);mm_test_context=0;
    assert(!kernel_mm_release(&mm) && !physical_page_release(pages,kernel.root_address));
    struct kernel_block_device *disk=lwext4_instance(mount->private_data)->block;
    if (!lwext4_instance(mount->private_data)->device.fs->jbd_journal) {
        fail_complete=1;
        assert(kernel_vfs_unmount(mount)==-KERNEL_EIO && !fail_complete);
    }
    assert(!kernel_vfs_unmount(mount) && !kernel_page_cache_destroy(cache));
    *mount=(struct kernel_vfs_mount){0}; *cache=(struct kernel_page_cache){0};
    assert(!kernel_page_cache_init(cache,heap,pages) && !kernel_vfs_mount_ext4(mount,disk,&backend_heap,cache,0));
    struct kernel_vfs_file closed={0};struct kernel_vfs_stat stat;
    assert(!kernel_vfs_open(mount,"/fault-close",&closed) && !kernel_vfs_fstat(&closed,&stat));
    fprintf(stderr,"closed legacy cleanup after remount: size=%llu blocks=%llu\n",(unsigned long long)stat.size,(unsigned long long)stat.blocks);
    assert(!stat.size && !stat.blocks && !kernel_vfs_close(&closed));
    puts("PASS source fault: unchanged EOF; legacy blocks returned on completion/truncate/unlink/close and unmount retry; real WB error retains acknowledged bytes and all owners return");
}
static void admission_mmap(struct kernel_page_cache *cache,struct kernel_heap *heap,
    struct physical_page_allocator *pages,struct kernel_vfs_mount *mount)
{
    struct kernel_vfs_file created={0},filler={0},last={0},spare={0};
    assert(!create_file(mount,"/mmap-full",0600,&created) && !kernel_vfs_ftruncate(&created,BOAROS_PAGE_SIZE) && !kernel_vfs_close(&created));
    assert(!create_file(mount,"/fill",0600,&filler) && !create_file(mount,"/last",0600,&last) && !create_file(mount,"/spare",0600,&spare));
    size_t block=kernel_vfs_file_node(&filler)->instance->writeback_block_size;
    int partial=BOAROS_PAGE_SIZE>block;
    if (partial) assert(!kernel_vfs_fallocate(&last,0,0,block));
    fill_free_space(mount,&filler,&spare,block);
    if (partial) assert(!kernel_vfs_ftruncate(&last,0));
    struct kernel_open_file_description *file=NULL; int error;
    assert(!kernel_open_file_create(heap,mount,"/mmap-full",&file,&error) && !error); file->open_flags=2;
    struct arch_mmu_page_table kernel={0}; struct arch_mmu_user_space space={0}; struct kernel_mm mm={0};
    assert(!riscv_sv39_page_table_init(&kernel,pages));kernel.state=ARCH_MMU_STATE_ACTIVE;
    assert(!arch_mmu_user_space_init(&space,pages,&kernel) && !kernel_mm_create(&mm,&space) && !kernel_mm_vma_enable(&mm,heap));
    assert(!kernel_mm_context(&mm,&mm_test_context) && !kernel_mm_brk_initialize(&mm,0x10000,0x20000)); uint64_t shared,private;
    struct kernel_open_file_description *source=file; assert(!kernel_open_file_acquire(source));
    assert(!kernel_mm_mmap_file_private(&mm,&source,0x40000,BOAROS_PAGE_SIZE,0,KERNEL_MM_READ|KERNEL_MM_WRITE,
        KERNEL_MM_MAP_FIXED|KERNEL_MM_MAP_SHARED,&shared));
    source=file;assert(!kernel_open_file_acquire(source));
    assert(!kernel_mm_mmap_file_private(&mm,&source,0x80000,BOAROS_PAGE_SIZE,0,KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED,&private));
    assert(!disk_read(&mm,shared) && !disk_read(&mm,private));
    enum kernel_mm_status result=kernel_mm_resolve_user_fault(&mm,shared,KERNEL_MM_WRITE);
    fprintf(stderr,"full shared mmap: result=%u partial=%d\n",result,partial);
    assert(result==KERNEL_MM_STATUS_BUS_FAULT && !kernel_vfs_error_sequence(&file->file));
    struct kernel_mm_mapping mapping; assert(!kernel_mm_lookup(&mm,shared,&mapping) && !(mapping.permissions&KERNEL_MM_WRITE));
    unsigned char byte=0x91;size_t done;
    assert(!kernel_copy_to_user(&mm,private,&byte,1,&done) && done==1 && disk_read(&mm,private)==byte && !disk_read(&mm,shared));
    uint64_t observed=0;assert(!kernel_vfs_sync(&file->file,0,&observed));
    if (partial) {
        if (lwext4_instance(mount->private_data)->device.fs->jbd_journal) assert(!mapped(&file->file,0).block);
        else assert(mapped(&file->file,0).block && mapped(&file->file,0).unwritten);
        assert(!kernel_vfs_pwrite(&file->file,0,"!",1,&done) && done==1 && !kernel_vfs_sync(&file->file,0,&observed));
        assert(disk_read(&mm,shared)=='!' && disk_read(&mm,private)==byte);
    }
    assert(!kernel_page_cache_start_worker(cache));
    prepare_pause=1;prepare_entered=prepare_resume=0;replaced_prepare_error=0;
    struct replaced_fault fault={.mm=&mm,.address=shared};pthread_t writer;
    assert(!pthread_create(&writer,NULL,replace_fault_actor,&fault));
    wait_progress(&prepare_entered,1);
    uint64_t replacement;
    assert(!kernel_mm_mmap_anonymous(&mm,shared,BOAROS_PAGE_SIZE,
        KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED,&replacement) && replacement==shared);
    __atomic_store_n(&prepare_resume,1,__ATOMIC_RELEASE);
    assert(!pthread_join(writer,NULL));
    fprintf(stderr,"replaced shared mapping: old prepare=%d access=%u done=%zu\n",
        replaced_prepare_error,fault.result,fault.done);
    assert(replaced_prepare_error==-KERNEL_ENOSPC && !fault.result && fault.done==1 && disk_read(&mm,shared)==0xa7);
    /* A successful reservation can also lose the VMA race. The old file
     * stays open, so cancellation must return space before final close. */
    assert(!kernel_vfs_ftruncate(&filler,0) && !kernel_vfs_ftruncate(&spare,0));
    source=file;assert(!kernel_open_file_acquire(source));
    assert(!kernel_mm_mmap_file_private(&mm,&source,shared,BOAROS_PAGE_SIZE,0,
        KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED|KERNEL_MM_MAP_SHARED,&replacement));
    assert(disk_read(&mm,shared)==(partial?'!':0));
    struct ext4_fs *fs=lwext4_instance(mount->private_data)->device.fs;
    uint64_t reserved_before=fs->delayed_blocks;
    prepare_pause=1;prepare_entered=prepare_resume=0;replaced_prepare_error=-1;
    fault=(struct replaced_fault){.mm=&mm,.address=shared};
    assert(!pthread_create(&writer,NULL,replace_fault_actor,&fault));wait_progress(&prepare_entered,1);
    assert(!kernel_mm_mmap_anonymous(&mm,shared,BOAROS_PAGE_SIZE,
        KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED,&replacement));
    __atomic_store_n(&prepare_resume,1,__ATOMIC_RELEASE);assert(!pthread_join(writer,NULL));
    assert(!replaced_prepare_error && !fault.result && fault.done==1);
    assert(fs->delayed_blocks==reserved_before);
    kernel_page_cache_stop_worker(cache);
    mm_test_bind(NULL);mm_test_context=0;
    assert(!kernel_mm_release(&mm) && !kernel_open_file_release(&file) && !physical_page_release(pages,kernel.root_address));
    assert(!kernel_vfs_close(&filler) && !kernel_vfs_close(&last) && !kernel_vfs_close(&spare));
    (void)cache;
    puts("PASS mmap admission: full valid page required before writable PTE; private COW survives full disk, no dirty error");
}
static void mmap_ranges(struct kernel_page_cache *cache,struct kernel_heap *heap,
    struct physical_page_allocator *pages,struct kernel_vfs_mount *mount)
{
    (void)cache;
    struct arch_mmu_page_table kernel={0}; struct arch_mmu_user_space space={0}; struct kernel_mm mm={0};
    assert(!riscv_sv39_page_table_init(&kernel,pages)); kernel.state=ARCH_MMU_STATE_ACTIVE;
    assert(!arch_mmu_user_space_init(&space,pages,&kernel) && !kernel_mm_create(&mm,&space) && !kernel_mm_vma_enable(&mm,heap));
    assert(!kernel_mm_context(&mm,&mm_test_context) && !kernel_mm_brk_initialize(&mm,0x10000,0x20000));
    struct kernel_vfs_file created={0};
    assert(!create_file(mount,"/mmap-hole",0600,&created) && !kernel_vfs_close(&created));
    struct kernel_open_file_description *file=0; int error;
    assert(!kernel_open_file_create(heap,mount,"/mmap-hole",&file,&error) && !error); file->open_flags=2;
    size_t length=2*BOAROS_PAGE_SIZE,done;
    unsigned char *expected=malloc(length); assert(expected); memset(expected,0x35,length);
    assert(!kernel_vfs_pwrite(&file->file,0,expected,length,&done) && done==length);
    struct kernel_open_file_description *source=file; uint64_t shared,private;
    assert(!kernel_open_file_acquire(source));
    assert(!kernel_mm_mmap_file_private(&mm,&source,0x40000,length,0,KERNEL_MM_READ|KERNEL_MM_WRITE,
        KERNEL_MM_MAP_FIXED|KERNEL_MM_MAP_SHARED,&shared));
    source=file; assert(!kernel_open_file_acquire(source));
    assert(!kernel_mm_mmap_file_private(&mm,&source,0x80000,length,0,KERNEL_MM_READ|KERNEL_MM_WRITE,
        KERNEL_MM_MAP_FIXED,&private));
    assert(disk_read(&mm,shared+17)==0x35 && disk_read(&mm,private+17)==0x35);
    assert(disk_read(&mm,private+BOAROS_PAGE_SIZE+17)==0x35);
    unsigned char value=0x71;
    assert(!kernel_copy_to_user(&mm,shared+23,&value,1,&done) && done==1); expected[23]=value;
    value=0xa7; assert(!kernel_copy_to_user(&mm,private+17,&value,1,&done) && done==1);
    assert(!kernel_vfs_fallocate(&file->file,3,11,BOAROS_PAGE_SIZE+29));
    memset(expected+11,0,BOAROS_PAGE_SIZE+29);
    assert(disk_read(&mm,shared+23)==0 && disk_read(&mm,shared+10)==0x35);
    assert(disk_read(&mm,private+17)==0xa7 && disk_read(&mm,private+23)==0x71);
    assert(disk_read(&mm,private+BOAROS_PAGE_SIZE+17)==0);
    value=0x99; assert(!kernel_copy_to_user(&mm,shared+31,&value,1,&done) && done==1); expected[31]=value;
    assert(!kernel_mm_msync(&mm,shared,length,4)); expect_bytes(&file->file,expected,length);
    mm_test_bind(0); mm_test_context=0;
    assert(!kernel_mm_release(&mm) && !kernel_open_file_release(&file) && !physical_page_release(pages,kernel.root_address));
    free(expected);
    puts("PASS mmap: shared dirty aliases refault zeros and can write again; private COW survives, clean private view follows file");
}
#endif
struct credential_create_race {
    struct kernel_vfs_path *root,*directory;
    struct kernel_credentials user;
    int create_result,chmod_result;
};
static void *credential_create_thread(void *argument)
{
    struct credential_create_race *race=argument;cache_test_attach(1);
    (void)kernel_credentials_override(&race->user);
    struct kernel_vfs_file file={0};
    race->create_result=kernel_vfs_create_at(race->root,race->root,"/qualified/accepted",0600,&file);
    if (!race->create_result) assert(!kernel_vfs_close(&file));
    (void)kernel_credentials_override(NULL);return NULL;
}
static void *credential_chmod_thread(void *argument)
{
    struct credential_create_race *race=argument;cache_test_attach(2);
    race->chmod_result=kernel_vfs_path_set_mode(race->directory,0);
    __atomic_store_n(&credential_chmod_done,1,__ATOMIC_RELEASE);return NULL;
}
static void credential_namespace_race(struct kernel_vfs_mount *mount)
{
    struct lwext4_mount_adapter *adapter=lwext4_instance(mount->private_data);
    struct ext4_journal_runtime runtime={.now_ns=metadata_clock};
    assert(!ext4_journal_group_enable(adapter->mount_point,&runtime,4*1024*1024));
    boaros_lwext4_enable_parallel(&adapter->backend_lock,adapter->device.fs);
    adapter->instance.namespace_parallel=1;
    struct credential_create_race race={.user=*kernel_credentials_current()};
    race.user.uid=race.user.euid=race.user.suid=race.user.fsuid=1000;
    race.user.gid=race.user.egid=race.user.sgid=race.user.fsgid=100;
    race.user.effective=race.user.permitted=0;
    assert(!kernel_vfs_path_root(mount,adapter->instance.heap,&race.root));
    assert(!kernel_vfs_mkdir_at(race.root,race.root,"/qualified",0777));
    assert(!kernel_vfs_path_resolve(race.root,race.root,"/qualified",1,&race.directory));
    credential_create_pause=1;credential_create_entered=credential_create_resume=credential_chmod_done=0;
    pthread_t creator,changer;
    assert(!pthread_create(&creator,NULL,credential_create_thread,&race));
    wait_progress(&credential_create_entered,1);
    unsigned parked=mm_test_parked(2);
    assert(!pthread_create(&changer,NULL,credential_chmod_thread,&race));
    while (mm_test_parked(2)==parked) sched_yield();
    assert(!__atomic_load_n(&credential_chmod_done,__ATOMIC_ACQUIRE));
    __atomic_store_n(&credential_create_resume,1,__ATOMIC_RELEASE);
    assert(!pthread_join(creator,NULL) && !pthread_join(changer,NULL));
    credential_create_pause=0;
    assert(!race.create_result && !race.chmod_result);
    struct kernel_vfs_stat stat;
    assert(!kernel_vfs_stat_at(race.root,race.root,"/qualified/accepted",1,&stat) && stat.uid==1000 && stat.gid==100);
    (void)kernel_credentials_override(&race.user);
    struct kernel_vfs_file denied={0};
    assert(kernel_vfs_create_at(race.root,race.root,"/qualified/denied",0600,&denied)==-KERNEL_EACCES);
    (void)kernel_credentials_override(NULL);
    assert(!kernel_vfs_path_release(&race.directory) && !kernel_vfs_path_release(&race.root));
    assert(!ext4_journal_group_drain(adapter->mount_point));
    puts("PASS credentials interleave: parent chmod blocks behind accepted create; next create observes revoked directory access; owner and resources survive parallel transaction publication");
}

/* Linux notify_change creates ATTR_MODE only when old set-ID bits change;
 * setattr_prepare then checks the requested group for the remaining SGID. */
static void credential_chown_groups(struct kernel_vfs_mount *mount)
{
    const struct kernel_credentials *root=kernel_credentials_current();
    struct kernel_credentials user=*root;
    user.uid=user.euid=user.suid=user.fsuid=1000;
    user.gid=user.egid=user.sgid=user.fsgid=100;
    user.effective=user.permitted=UINT64_C(1)<<KERNEL_CAP_CHOWN;
    user.inheritable=user.ambient=0;
    struct kernel_vfs_file file={0};
    struct kernel_pipe *pipe=NULL;
    assert(!kernel_vfs_create(mount,"/chown-groups",0600,&file));
    assert(!kernel_pipe_create(((struct kernel_vfs_instance *)mount->private_data)->heap,&pipe));
    static const struct {uint32_t mode,old_gid,new_gid,expected;} cases[]={
        {06600,100,200,0600}, {02600,100,200,02600},
        {06600,100,100,02600}, {02600,200,100,0600},
    };
    for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) for (unsigned anonymous=0;anonymous<2;anonymous++) {
        (void)kernel_credentials_override(root);
        assert(!(anonymous ? kernel_pipe_set_owner(pipe,1000,cases[i].old_gid) :
            kernel_vfs_file_set_owner(&file,1000,cases[i].old_gid)));
        assert(!(anonymous ? kernel_pipe_set_mode(pipe,cases[i].mode) :
            kernel_vfs_file_set_mode(&file,cases[i].mode)));
        (void)kernel_credentials_override(&user);
        assert(!(anonymous ? kernel_pipe_set_owner(pipe,UINT32_MAX,cases[i].new_gid) :
            kernel_vfs_file_set_owner(&file,UINT32_MAX,cases[i].new_gid)));
        struct kernel_vfs_stat stat;
        assert(!(anonymous ? kernel_pipe_stat(pipe,&stat) : kernel_vfs_fstat(&file,&stat)));
        if ((stat.mode&07777)!=cases[i].expected)
            fprintf(stderr,"chown group case=%u pipe=%u mode=%o expected=%o oldgid=%u newgid=%u\n",
                i,anonymous,stat.mode&07777,cases[i].expected,cases[i].old_gid,cases[i].new_gid);
        assert(stat.uid==1000 && stat.gid==cases[i].new_gid && (stat.mode&07777)==cases[i].expected);
    }
    (void)kernel_credentials_override(root);
    assert(!kernel_vfs_close(&file) && !kernel_pipe_destroy_unowned(pipe));
    (void)kernel_credentials_override(NULL);
    puts("PASS chown old/new group: set-ID removal filters new group only when MODE changes; actual file and pipe metadata");
}

struct credential_write_race {
    struct kernel_vfs_file *file;
    struct kernel_mm *mm;
    struct kernel_credentials user;
    struct kernel_vfs_user_write_result written;
    unsigned change,changed;
    int write_result,change_result;
};
static void *credential_paused_writer(void *argument)
{
    struct credential_write_race *race=argument;cache_test_attach(1);
    mm_test_bind(race->mm);assert(!kernel_mm_context(race->mm,&mm_test_context));
    (void)kernel_credentials_override(&race->user);
    struct kernel_uaccess_iovec source={.base=0x10000,.length=BOAROS_PAGE_SIZE+17};
    race->write_result=kernel_vfs_write_user(race->file,race->mm,&source,1,source.length,0,0,NULL,&race->written);
    (void)kernel_credentials_override(NULL);mm_test_bind(NULL);mm_test_context=0;return NULL;
}
static void *credential_attribute_changer(void *argument)
{
    struct credential_write_race *race=argument;cache_test_attach(2);
    if (race->change==0) race->change_result=kernel_vfs_file_set_mode(race->file,04666);
    else if (race->change==1) race->change_result=kernel_vfs_path_set_mode(race->file->path,04666);
    else if (race->change==2) race->change_result=kernel_vfs_file_set_owner(race->file,2000,200);
    else race->change_result=kernel_vfs_path_set_owner(race->file->path,2000,200);
    __atomic_store_n(&race->changed,1,__ATOMIC_RELEASE);return NULL;
}
static void credential_write_attributes(struct kernel_heap *heap,struct physical_page_allocator *pages,
    struct kernel_vfs_mount *mount)
{
    struct arch_mmu_page_table kernel={0};struct arch_mmu_user_space space={0};struct kernel_mm mm={0};
    assert(!riscv_sv39_page_table_init(&kernel,pages));kernel.state=ARCH_MMU_STATE_ACTIVE;
    assert(!arch_mmu_user_space_init(&space,pages,&kernel) && !kernel_mm_create(&mm,&space) && !kernel_mm_vma_enable(&mm,heap));
    assert(!kernel_mm_context(&mm,&mm_test_context) && !kernel_mm_brk_initialize(&mm,0x10000,0x20000));
    uint64_t end;assert(!kernel_mm_brk(&mm,0x10000+2*BOAROS_PAGE_SIZE,&end));
    mm_test_bind(&mm);
    unsigned char bytes[BOAROS_PAGE_SIZE+17];memset(bytes,0x59,sizeof(bytes));size_t copied;
    assert(!kernel_copy_to_user(&mm,0x10000,bytes,sizeof(bytes),&copied) && copied==sizeof(bytes));
    mm_test_bind(NULL);mm_test_context=0;
    for (unsigned change=0;change<4;change++) {
        char name[48];snprintf(name,sizeof(name),"/write-attributes-%u",change);
        struct kernel_vfs_file file={0};assert(!kernel_vfs_create(mount,name,04666,&file));
        struct credential_write_race race={.file=&file,.mm=&mm,.change=change,.user=*kernel_credentials_current()};
        race.user.uid=race.user.euid=race.user.suid=race.user.fsuid=1000;
        race.user.gid=race.user.egid=race.user.sgid=race.user.fsgid=100;
        race.user.effective=race.user.permitted=race.user.inheritable=race.user.ambient=0;
        credential_pin_pause=1;credential_pin_entered=credential_pin_resume=0;
        pthread_t writer,changer;assert(!pthread_create(&writer,NULL,credential_paused_writer,&race));
        wait_progress(&credential_pin_entered,1);
        struct kernel_vfs_stat stat;assert(!kernel_vfs_fstat(&file,&stat) && !(stat.mode&06000) && !stat.size);
        unsigned parked=mm_test_parked(2);
        assert(!pthread_create(&changer,NULL,credential_attribute_changer,&race));
        while (mm_test_parked(2)==parked && !__atomic_load_n(&race.changed,__ATOMIC_ACQUIRE)) sched_yield();
        unsigned premature=__atomic_load_n(&race.changed,__ATOMIC_ACQUIRE);
        __atomic_store_n(&credential_pin_resume,1,__ATOMIC_RELEASE);
        assert(!pthread_join(writer,NULL) && !pthread_join(changer,NULL));
        assert(!race.write_result && !race.change_result && !race.written.access && race.written.written==sizeof(bytes));
        expect_bytes(&file,bytes,sizeof(bytes));
        assert(!kernel_vfs_fstat(&file,&stat) && stat.size==sizeof(bytes));
        if (change<2) assert((stat.mode&07777)==04666);
        else assert(stat.uid==2000 && stat.gid==200 && !(stat.mode&06000));
        assert(!kernel_vfs_close(&file));
        if (premature) fprintf(stderr,"attribute change=%u completed while writer awaited source pin; modified bytes followed permission publication\n",change);
        assert(!premature);
    }
    assert(!kernel_mm_release(&mm) && !physical_page_release(pages,kernel.root_address));
    puts("PASS write/attributes interleave: FD/path chmod/chown wait for all source fragments; final contents, permissions and owners retained");
}

static void credential_vfs_cases(struct kernel_vfs_mount *mount)
{
    const struct kernel_credentials *root=kernel_credentials_current();
    struct kernel_credentials user=*root,other=*root;
    user.uid=user.euid=user.suid=user.fsuid=1000;
    user.gid=user.egid=user.sgid=user.fsgid=100;
    user.effective=user.permitted=user.inheritable=user.ambient=0;
    other=user;other.uid=other.euid=other.suid=other.fsuid=1001;
    struct kernel_vfs_path *path=NULL;
    assert(!kernel_vfs_path_root(mount,((struct kernel_vfs_instance *)mount->private_data)->heap,&path));
    assert(!kernel_vfs_mkdir_at(path,path,"/identities",0777));
    assert(!kernel_vfs_mkdir_at(path,path,"/sticky",01777));
    assert(!kernel_vfs_mkdir_at(path,path,"/group",02777));
    struct kernel_vfs_path *group=NULL;
    assert(!kernel_vfs_path_resolve(path,path,"/group",1,&group));
    assert(!kernel_vfs_path_set_owner(group,UINT32_MAX,200));
    assert(!kernel_vfs_path_set_mode(group,02777));
    assert(!kernel_vfs_path_release(&group));
    struct kernel_vfs_file root_file={0};
    assert(!kernel_vfs_create_at(path,path,"/sticky/root",0600,&root_file));
    assert(!kernel_vfs_close(&root_file));
    (void)kernel_credentials_override(&user);
    struct kernel_vfs_open_request create={.flags=KERNEL_VFS_OPEN_CREATE|KERNEL_VFS_OPEN_EXCLUSIVE|KERNEL_VFS_OPEN_FOLLOW,.access=6};
    struct kernel_vfs_file file={0};int created=0;struct kernel_vfs_stat stat;
    assert(!kernel_vfs_open_at(path,path,"/identities/zero",&create,&file,&created) && created);
    assert(!kernel_vfs_fstat(&file,&stat) && stat.uid==1000 && stat.gid==100 && !(stat.mode&0777));
    size_t done=0;
    assert(!kernel_vfs_pwrite(&file,0,"owner",5,&done) && done==5);
    struct kernel_vfs_file denied={0};
    struct kernel_vfs_open_request read={.flags=KERNEL_VFS_OPEN_FOLLOW,.access=4};
    assert(kernel_vfs_open_at(path,path,"/identities/zero",&read,&denied,NULL)==-KERNEL_EACCES);
    struct kernel_vfs_open_request directory=read;directory.flags |= KERNEL_VFS_OPEN_DIRECTORY;
    assert(kernel_vfs_open_at(path,path,"/identities/zero",&directory,&denied,NULL)==-KERNEL_ENOTDIR);
    struct kernel_vfs_open_request only_path={.flags=KERNEL_VFS_OPEN_PATH_ONLY};
    assert(!kernel_vfs_open_at(path,path,"/identities/zero",&only_path,&denied,NULL));
    assert(!kernel_vfs_path_release(&denied.path));memset(&denied,0,sizeof(denied));
    assert(!kernel_vfs_file_set_mode(&file,0600));
    assert(!kernel_vfs_access_at(path,path,"/identities/zero",6,1));
    assert(kernel_vfs_access_at(path,path,"/identities/zero",1,1)==-KERNEL_EACCES);
    assert(kernel_vfs_file_set_owner(&file,0,UINT32_MAX)==-KERNEL_EPERM);
    assert(kernel_vfs_file_set_owner(&file,UINT32_MAX,200)==-KERNEL_EPERM);
    assert(kernel_vfs_unlink_at(path,path,"/sticky/root",0)==-KERNEL_EPERM);
    assert(!kernel_vfs_create_at(path,path,"/sticky/own",0600,&denied));
    assert(!kernel_vfs_close(&denied));
    assert(!kernel_vfs_unlink_at(path,path,"/sticky/own",0));
    assert(!kernel_vfs_create_at(path,path,"/group/inherited",02770,&denied));
    assert(!kernel_vfs_fstat(&denied,&stat) && stat.uid==1000 && stat.gid==200 && !(stat.mode&02000));
    assert(!kernel_vfs_close(&denied));
    (void)kernel_credentials_override(&other);
    assert(kernel_vfs_file_set_mode(&file,0666)==-KERNEL_EPERM);
    assert(!kernel_vfs_file_set_owner(&file,UINT32_MAX,UINT32_MAX));
    assert(kernel_vfs_file_set_owner(&file,1000,UINT32_MAX)==-KERNEL_EPERM);
    assert(kernel_vfs_file_set_owner(&file,UINT32_MAX,100)==-KERNEL_EPERM);
    assert(kernel_vfs_access_at(path,path,"/identities/zero",4,1)==-KERNEL_EACCES);
    assert(kernel_vfs_path_truncate(file.path,0,NULL)==-KERNEL_EACCES);
    /* The existing opened description keeps the access granted to its opener. */
    assert(!kernel_vfs_pwrite(&file,5,"fd",2,&done) && done==2);
    (void)kernel_credentials_override(root);
    assert(!kernel_vfs_file_set_mode(&file,06777));
    (void)kernel_credentials_override(&other);
    assert(kernel_vfs_file_set_owner(&file,UINT32_MAX,UINT32_MAX)==-KERNEL_EPERM);
    (void)kernel_credentials_override(&user);
    assert(!kernel_vfs_pwrite(&file,0,"x",1,&done) && done==1);
    assert(!kernel_vfs_fstat(&file,&stat) && !(stat.mode&06000));
    (void)kernel_credentials_override(root);
    assert(!kernel_vfs_file_set_mode(&file,06777));
    (void)kernel_credentials_override(&user);
    assert(!kernel_vfs_ftruncate(&file,1));
    assert(!kernel_vfs_fstat(&file,&stat) && !(stat.mode&06000));
    (void)kernel_credentials_override(root);
    assert(!kernel_vfs_file_set_mode(&file,06777));
    (void)kernel_credentials_override(&user);
    struct kernel_vfs_size_limit limit={.maximum=0};
    assert(kernel_vfs_ftruncate_borrowing(&file,4096,NULL,&limit)==-KERNEL_EFBIG && limit.exceeded);
    assert(!kernel_vfs_fstat(&file,&stat) && (stat.mode&06000)==06000);
    if (!kernel_tmpfs_is_mount(mount)) {
        limit.exceeded=0;
        assert(kernel_vfs_fallocate_borrowing(&file,0,0,4096,NULL,&limit)==-KERNEL_EFBIG && limit.exceeded);
        assert(!kernel_vfs_fstat(&file,&stat) && !(stat.mode&06000));
        (void)kernel_credentials_override(root);
        assert(!kernel_vfs_file_set_mode(&file,06777));
        (void)kernel_credentials_override(&user);
        size_t block=lwext4_instance(mount->private_data)->device.lg_bsize;
        uint64_t before_blocks=allocated(&file),before_size=kernel_vfs_file_size(&file);
        fail_reserve=1;
        assert(kernel_vfs_fallocate(&file,1,8*block,block)==-KERNEL_ENOMEM && !fail_reserve);
        assert(allocated(&file)==before_blocks && kernel_vfs_file_size(&file)==before_size);
        assert(!kernel_vfs_fstat(&file,&stat) && !(stat.mode&06000));
    }
    (void)kernel_credentials_override(root);
    assert(!kernel_vfs_close(&file));
    if (!kernel_tmpfs_is_mount(mount)) {
        struct lwext4_mount_adapter *adapter=lwext4_instance(mount->private_data);
        char name[100];snprintf(name,sizeof(name),"%sidentities/capability",adapter->mount_point);
        ext4_file raw={0};
        assert(!ext4_fopen(&raw,name,"w+") && !ext4_file_set_mode(&raw,0755) && !ext4_fclose(&raw));
        uint32_t cap[6]={0x02000001,1,0,0,0,0};
        unsigned char padding[48]={1};
        assert(!ext4_setxattr(name,"user.padding",12,padding,sizeof(padding)));
        assert(!ext4_setxattr(name,"security.capability",19,cap,20));
        assert(!ext4_fopen(&raw,name,"r+"));
        struct ext4_inode initial,after;
        assert(!ext4_fraw_inode_fill(&raw,&initial) && ext4_inode_get_file_acl(&initial,&adapter->device.fs->sb));
        assert(!ext4_transaction_begin(adapter->mount_point));
        assert(!ext4_file_remove_privileges(&raw,0100700));
        assert(ext4_transaction_abort(adapter->mount_point,ECANCELED)==ECANCELED);
        assert(!ext4_fraw_inode_fill(&raw,&after) && !memcmp(&initial,&after,sizeof(initial)));
        size_t length=0;uint32_t restored[6]={0};
        assert(!ext4_file_capability(&raw,restored,sizeof(restored),&length) && length==20 && !memcmp(restored,cap,20));
        assert(!ext4_fclose(&raw));
        assert(!kernel_vfs_open_executable(mount,"/identities/capability",&file));
        struct kernel_vfs_exec_metadata exec;
        fail_capability_read=1;
        assert(kernel_vfs_exec_metadata(&file,&exec,1)==-KERNEL_EIO && !fail_capability_read);
        assert(!kernel_vfs_exec_metadata(&file,&exec,1) && exec.filecap_present && exec.filecap_size==20 && !memcmp(exec.filecap,cap,20));
        assert(exec.uid==0 && exec.gid==0 && exec.readable);
        mount->nosuid=1;
        assert(!kernel_vfs_exec_metadata(&file,&exec,1) && exec.nosuid && !exec.filecap_present);
        mount->nosuid=0;assert(!kernel_vfs_close(&file));
        assert(!kernel_vfs_open(mount,"/identities/capability",&file));
        fail_capability_remove=1;done=0;
        assert(kernel_vfs_pwrite(&file,0,"x",1,&done)==-KERNEL_ENOMEM && !done && !fail_capability_remove);
        assert(!ext4_file_capability(lwext4_node_file(file.private_data),restored,sizeof(restored),&length) && length==20);
        assert(!kernel_vfs_pwrite(&file,0,"x",1,&done));
        size_t bytes=0;assert(ext4_file_capability(lwext4_node_file(file.private_data),cap,sizeof(cap),&bytes)==ENODATA);
        assert(!kernel_vfs_close(&file));
    }
    assert(!kernel_vfs_path_release(&path));
    (void)kernel_credentials_override(NULL);
    puts("PASS credentials VFS: numeric owner/group, mode=000 creation, DAC/O_PATH/sticky/SGID, retained FD rights, set-ID clearing and inode filecap snapshot");
}

#include "vfs_socket_path.h"

int main(int argc,char **argv)
{
    assert(argc==3); setvbuf(stdout,NULL,_IONBF,0); cache_test_attach(0);
    struct physical_page_allocator pages={0}; struct kernel_heap heap={0}; struct kernel_page_cache cache={0};
    struct boot_memory_layout layout={.usable_count=1,.usable={{(uintptr_t)pool,sizeof(pool)}}};
    assert(!physical_page_allocator_init(&pages,&layout) && !physical_page_allocator_bind_access(&pages,access_page) &&
        !physical_page_allocator_finalize(&pages) && !kernel_heap_init(&heap,&pages,page_address));
    uint64_t baseline=physical_page_available(&pages);
    struct boot_memory_layout backend_layout={.usable_count=1,.usable={{(uintptr_t)backend_pool,sizeof(backend_pool)}}};
    assert(!physical_page_allocator_init(&backend_pages,&backend_layout) && !physical_page_allocator_bind_access(&backend_pages,access_page) &&
        !physical_page_allocator_finalize(&backend_pages) && !kernel_heap_init(&backend_heap,&backend_pages,page_address));
    uint64_t backend_baseline=physical_page_available(&backend_pages);
    assert(!kernel_page_cache_init(&cache,&heap,&pages)); current_cache=&cache;
    image_fd=open(argv[1],O_RDWR); assert(image_fd>=0); struct stat stat; assert(!fstat(image_fd,&stat));
    struct kernel_block_device device={.read=image_read,.write=image_write,.flush=image_flush,
        .capacity_bytes=stat.st_size,.logical_block_size=512,.cache_mode=KERNEL_BLOCK_CACHE_WRITEBACK};
    struct kernel_vfs_mount mount={0}; assert(!kernel_vfs_mount_ext4(&mount,&device,
        !strncmp(argv[2],"namespace-",10)?&heap:&backend_heap,&cache,0));
    if (!strcmp(argv[2],"delalloc")) {delayed_allocation(&cache,&mount);goto mounted_done;}
    if (!strcmp(argv[2],"vfs-socket")) {vfs_socket_paths(&mount);goto mounted_done;}
    if (!strcmp(argv[2],"credential-race")) {credential_namespace_race(&mount);goto mounted_done;}
    if (!strcmp(argv[2],"credential-chown")) {credential_chown_groups(&mount);goto mounted_done;}
    if (!strcmp(argv[2],"credential-write-race")) {credential_write_attributes(&heap,&pages,&mount);goto mounted_done;}
    if (!strcmp(argv[2],"vfs-credentials")) {credential_vfs_cases(&mount);goto mounted_done;}
    if (!strcmp(argv[2],"namespace-budget")) { namespace_budget(&mount);goto mounted_done; }
    if (!strncmp(argv[2],"namespace-",10)) {
        namespace_pressure(&cache,&mount,!strcmp(argv[2],"namespace-reclaim"));goto mounted_done;
    }
#ifdef CACHE_MM_CASES
    if (!strcmp(argv[2],"admission-fault")) {
        admission_fault(&cache,&heap,&pages,&mount); goto mounted_done;
    }
    if (!strcmp(argv[2],"admission-mmap")) {
        admission_mmap(&cache,&heap,&pages,&mount); goto mounted_done;
    }
#endif
    if (!strncmp(argv[2],"admission-",10)) {
        if (!strncmp(argv[2],"admission-prefix",16))
            admission_prefix(&cache,&mount,!strcmp(argv[2],"admission-prefix-legacy"));
        else admission_full(&cache,&mount,!strcmp(argv[2],"admission-legacy"));
        goto mounted_done;
    }
    if (!strcmp(argv[2],"reserved-abort-reuse")) {
        reserved_abort_reuse(&cache,&mount);
        goto mounted_done;
    }
    if (!strncmp(argv[2],"reserved-",9)) {
        int large=!strcmp(argv[2],"reserved-large");
        reserved_write_full(&cache,&mount,large?2048:16,!strcmp(argv[2],"reserved-middle"),
            large,!strcmp(argv[2],"reserved-zero-error"));
        goto mounted_done;
    }
    basic_ranges(&cache,&mount); pending_truncate(&mount);
    if (lwext4_instance(mount.private_data)->device.fs->jbd_journal) rollback_ranges(&mount);
    else nonjournal_failure(&mount);
    active_writeback(&mount);
    if (lwext4_instance(mount.private_data)->device.fs->jbd_journal) unlink_during_writeback(&cache,&mount);
#ifdef CACHE_MM_CASES
    mmap_ranges(&cache,&heap,&pages,&mount);
#endif
mounted_done:
    assert(!kernel_vfs_unmount(&mount) && !kernel_page_cache_destroy(&cache) && !close(image_fd));
    struct kernel_vfs_mount *memory=NULL; struct kernel_vfs_file unsupported={0};
    assert(!kernel_tmpfs_create(&heap,0,NULL,&memory));
    if (!strcmp(argv[2],"vfs-socket")) vfs_socket_paths(memory);
    if (!strcmp(argv[2],"vfs-credentials")) credential_vfs_cases(memory);
    if (!strcmp(argv[2],"credential-chown")) credential_chown_groups(memory);
    if (!strcmp(argv[2],"credential-write-race")) credential_write_attributes(&heap,&pages,memory);
    assert(!kernel_vfs_create(memory,"/unsupported",0600,&unsupported));
    assert(kernel_vfs_fallocate(&unsupported,0,0,4096)==-KERNEL_EOPNOTSUPP);
    assert(kernel_vfs_fallocate(&unsupported,1,0,4096)==-KERNEL_EOPNOTSUPP);
    assert(kernel_vfs_fallocate(&unsupported,3,0,4096)==-KERNEL_EOPNOTSUPP);
    assert(!kernel_vfs_file_block_device(&unsupported));
    assert(!kernel_vfs_close(&unsupported) && !kernel_vfs_unmount(memory));
    assert(!physical_page_allocator_audit(&pages) && physical_page_available(&pages)==baseline);
    struct kernel_heap_statistics stats;
    if (!strncmp(argv[2],"namespace-",10)) {
        kernel_heap_get_statistics(&heap,&stats);assert(!stats.live_allocations && !stats.current_pages);
    }
    kernel_heap_get_statistics(&backend_heap,&stats);
    assert(!stats.live_allocations && !stats.current_pages && !physical_page_allocator_audit(&backend_pages) &&
        physical_page_available(&backend_pages)==backend_baseline);
    puts("PASS fallocate owners returned");
    return 0;
}
