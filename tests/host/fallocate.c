/* Reuse the existing real-device/cache/MM host fixture, with separate cases. */
#define main cache_fixture_main
#include "page_cache_concurrency.c"
#undef main
#include <ext4_extent.h>
#include <ext4_super.h>
#include <ext4_inode.h>
#include <kernel/tmpfs.h>
static unsigned fail_reserve, fail_remove;
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
    struct kernel_vfs_mount mount={0}; assert(!kernel_vfs_mount_ext4(&mount,&device,&backend_heap,&cache,0));
    basic_ranges(&cache,&mount); pending_truncate(&mount);
    if (lwext4_instance(mount.private_data)->device.fs->jbd_journal) rollback_ranges(&mount);
    else nonjournal_failure(&mount);
    active_writeback(&mount);
#ifdef CACHE_MM_CASES
    mmap_ranges(&cache,&heap,&pages,&mount);
#endif
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
    struct kernel_heap_statistics stats; kernel_heap_get_statistics(&backend_heap,&stats);
    assert(!stats.live_allocations && !stats.current_pages && !physical_page_allocator_audit(&backend_pages) &&
        physical_page_available(&backend_pages)==backend_baseline);
    puts("PASS fallocate owners returned");
    return 0;
}
