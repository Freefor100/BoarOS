/* Reuse the existing real-device/cache/MM host fixture, with separate cases. */
#include <kernel/files.h>
#define main cache_fixture_main
#include "page_cache_concurrency.c"
#undef main
#include <ext4_extent.h>
#include <ext4_super.h>
#include <ext4_inode.h>
#include <kernel/tmpfs.h>
static unsigned fail_reserve, fail_remove, failed_direct_calls, fail_complete;
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
        assert(!kernel_vfs_pread(file,offset,actual,chunk,&done) && done==chunk);
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
    assert(!ext4_ftruncate(&file,block+17));
    bool changed;
    assert(!ext4_fallocate(&file,1,6*block,2*block,&changed) && changed);
    assert(ext4_fsize(&file)==block+17);
    assert(!ext4_transaction_end(adapter->mount_point));
    assert(!ext4_fallocate(&file,0,0,length,&changed) && changed);
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
    assert(!ext4_transaction_begin(adapter->mount_point) && !ext4_ftruncate(file,0));
    bool changed;
    int reserved=ext4_fallocate(file,0,0,length,&changed);
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
static unsigned punch_started,punch_completed;
static struct kernel_vfs_file *punch_file;
static size_t punch_block;
static void *punch_during_writeback(void *unused)
{
    (void)unused; cache_test_attach(0);
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
    unsigned parked=mm_test_parked(0);
    assert(!pthread_create(&puncher,NULL,punch_during_writeback,NULL));
    wait_progress(&punch_started,1);
    while (mm_test_parked(0)==parked) sched_yield();
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
#ifdef CACHE_MM_CASES
static unsigned prepare_pause,prepare_entered,prepare_resume;
static int replaced_prepare_error;
int __real_kernel_page_cache_prepare_mapping_write(struct kernel_page_cache *,struct kernel_vfs_node *,uint64_t);
int __wrap_kernel_page_cache_prepare_mapping_write(struct kernel_page_cache *cache,struct kernel_vfs_node *node,uint64_t index)
{
    int held=__atomic_exchange_n(&prepare_pause,0,__ATOMIC_ACQ_REL);
    if (held) {
        assert(!kernel_io_context_current()->locks && !kernel_cpu_current()->raw_locks);
        __atomic_store_n(&prepare_entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&prepare_resume,__ATOMIC_ACQUIRE)) sched_yield();
    }
    int error=__real_kernel_page_cache_prepare_mapping_write(cache,node,index);
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
        assert(!kernel_vfs_write_user(&file,&mm,&source,1,BOAROS_PAGE_SIZE,0,0,&result));
        assert(result.access==KERNEL_UACCESS_STATUS_FAULT && !result.written && !result.end_offset);
        struct kernel_vfs_stat stat;assert(!kernel_vfs_fstat(&file,&stat) && !stat.size && (legacy?!stat.blocks:stat.blocks>0));
        uint64_t observed=0;assert(!kernel_vfs_sync(&file,0,&observed) && !kernel_vfs_error_sequence(&file));
        assert(!kernel_vfs_close(&file));
    }
    for (unsigned cleanup=0;cleanup<3;cleanup++) {
        const char *name=cleanup==2?"/fault-close":cleanup?"/fault-unlink":"/fault-truncate";
        struct kernel_vfs_file file={0}; assert(!create_file(mount,name,0600,&file));admission_legacy(mount,&file);
        struct kernel_vfs_statfs before,after;assert(!kernel_vfs_mount_statfs(mount,&before));
        struct kernel_uaccess_iovec source={.base=0x900000,.length=BOAROS_PAGE_SIZE};
        struct kernel_vfs_user_write_result result; fail_complete=1;
        assert(kernel_vfs_write_user(&file,&mm,&source,1,BOAROS_PAGE_SIZE,0,0,&result)==-KERNEL_EIO && !fail_complete);
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
        assert(mapped(&file->file,0).block && mapped(&file->file,0).unwritten);
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
#ifdef CACHE_MM_CASES
    mmap_ranges(&cache,&heap,&pages,&mount);
#endif
mounted_done:
    assert(!kernel_vfs_unmount(&mount) && !kernel_page_cache_destroy(&cache) && !close(image_fd));
    struct kernel_vfs_mount *memory=NULL; struct kernel_vfs_file unsupported={0};
    assert(!kernel_tmpfs_create(&heap,0,NULL,&memory));
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
