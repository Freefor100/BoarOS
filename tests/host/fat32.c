#include <kernel/vfs.h>
#include <kernel/page_cache.h>
#include <kernel/physical_page.h>
#include <kernel/cpu.h>
#include <kernel/page.h>
#include <kernel/errno.h>
#include <kernel/time.h>
#include "../../fs/vfs_objects.h"
#include "../../kernel/sched/private.h"
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <sys/stat.h>

void mm_test_attach(unsigned);
struct kernel_scheduler scheduler = {.initialized=KERNEL_SCHEDULER_INITIALIZED};
static unsigned char pool[32 * 1024 * 1024] __attribute__((aligned(16384)));
static void *access_page(uint64_t p) { return (void *)(uintptr_t)p; }
static int page_address(const void *p, uint64_t *out) { *out = (uintptr_t)p; return 1; }
static int fd, write_fail; static unsigned writes,watch_remote_read,remote_reads;
static size_t largest_read, largest_write;
static enum kernel_block_status rd(void *ctx, uint64_t at, void *p, size_t n)
{
    (void)ctx;
    if (largest_read < n) largest_read = n;
    if (watch_remote_read) {
        assert(!kernel_cpu_current()->logical_id);
        __atomic_fetch_add(&remote_reads,1,__ATOMIC_RELAXED);
    }
    return pread(fd,p,n,at)==(ssize_t)n?0:KERNEL_BLOCK_STATUS_IO;
}
static enum kernel_block_status wr(void *ctx, uint64_t at, const void *p, size_t n)
{
    (void)ctx; writes++;
    if (largest_write < n) largest_write = n;
    return !write_fail && pwrite(fd, p, n, at) == (ssize_t)n ? 0 : KERNEL_BLOCK_STATUS_IO;
}
static enum kernel_block_status fl(void *ctx)
{ (void)ctx; return fsync(fd) ? KERNEL_BLOCK_STATUS_IO : 0; }
enum kernel_time_status kernel_time_deadline_from_monotonic(uint64_t target, uint64_t *deadline)
{
    *deadline = target;
    return target <= kernel_time_monotonic_ns() ? KERNEL_TIME_STATUS_DEADLINE_PASSED : KERNEL_TIME_STATUS_OK;
}
static void sync_file(struct kernel_vfs_file *file)
{ uint64_t observed = kernel_vfs_error_sequence(file); assert(!kernel_vfs_sync(file, 0, &observed)); }
static void contents(struct kernel_vfs_file *file, uint64_t at, const void *expected, size_t n)
{
    unsigned char actual[32768]; size_t done;
    assert(n <= sizeof(actual));
    assert(!kernel_vfs_pread(file, at, actual, n, &done, NULL) && done == n && !memcmp(actual, expected, n));
}
static void *remote_contents(void *argument)
{
    mm_test_attach(1);kernel_cpu_current()->logical_id=1;
    assert(kernel_cpu_current()->logical_id==1);
    unsigned char zeros[17001]={0};
    contents(argument,0,zeros,sizeof(zeros));
    assert(!kernel_cpu_current()->raw_locks && !kernel_io_context_current()->locks);
    return NULL;
}
static void contiguous_transfers(struct kernel_vfs_mount *mount)
{
    struct kernel_vfs_file file={0};
    unsigned char data[32768], byte=0;
    size_t done;
    memset(data,0x31,sizeof(data));
    assert(!kernel_vfs_create(mount,"/batch.bin",0600,&file));
    assert(!kernel_vfs_pwrite(&file,0,data,sizeof(data),&done) && done==sizeof(data));
    sync_file(&file);
    struct kernel_vfs_node *node=kernel_vfs_file_node(&file);
    const struct kernel_vfs_backend *ops=node->instance->ops;
    /* Keep a partial sector in the backend cache, then overwrite a larger
     * range containing it. The next partial read must see the new bytes. */
    assert(!ops->pread(node,1553,&byte,1,&done) && done==1 && byte==0x31);
    memset(data,0x72,sizeof(data));
    largest_write=0;
    assert(!kernel_vfs_pwrite(&file,0,data,sizeof(data),&done) && done==sizeof(data));
    sync_file(&file);
    assert(largest_write>=4096);
    assert(!ops->pread(node,1553,&byte,1,&done) && done==1 && byte==0x72);
    assert(!kernel_page_cache_invalidate_node(node->instance->page_cache,node));
    largest_read=0; contents(&file,0,data,sizeof(data));
    assert(largest_read>=4096);
    assert(!kernel_vfs_unlink(mount,"/batch.bin") && !kernel_vfs_close(&file));
    puts("PASS FAT contiguous I/O: multi-sector requests, preserved content and invalidated partial-sector cache");
}
static void names(struct kernel_vfs_mount *mount, struct kernel_vfs_path *root)
{
    struct kernel_vfs_file a={0},b={0}; size_t done; char path[100];
    assert(!kernel_vfs_mkdir(mount,"/first",0777) && !kernel_vfs_mkdir(mount,"/second",0777));
    assert(!kernel_vfs_mkdir(mount,"/first/sub",0777));
    struct kernel_vfs_path *alias=0;
    assert(!kernel_vfs_path_resolve(root,root,"/FIRST/sub",1,&alias));
    assert(!kernel_vfs_rename_at(root,root,root,"/first/sub","/second/moved directory",0));
    assert(!kernel_vfs_create_at(alias,root,"from-dir-fd",0666,&a) && !kernel_vfs_close(&a));
    assert(!kernel_vfs_unlink(mount,"/second/moved directory/from-dir-fd"));
    assert(!kernel_vfs_path_release(&alias));
    assert(kernel_vfs_rename_at(root,root,root,"/second","/second/moved directory/cycle",0)==-KERNEL_EINVAL);
    for(unsigned j=0;j<40;j++) {
        snprintf(path,sizeof(path),"/second/moved directory/colliding long filename %u.bin",j);
        assert(!kernel_vfs_create(mount,path,0666,&a) && !kernel_vfs_close(&a));
    }
    assert(!kernel_vfs_open(mount,"/second/moved directory",&a));
    uint64_t pos=0,next,id; uint8_t type; char name[256]; unsigned entries=0;
    while(kernel_vfs_dir_entry(&a,pos,&next,&id,&type,name,sizeof(name))>0) { assert(next>pos);pos=next;entries++; }
    assert(entries==42 && !kernel_vfs_close(&a));
    assert(!kernel_vfs_create(mount,"/old",0666,&a) && !kernel_vfs_create(mount,"/dest",0666,&b));
    assert(!kernel_vfs_pwrite(&a,0,"old",3,&done) && done==3);
    assert(!kernel_vfs_pwrite(&b,0,"dst",3,&done) && done==3);
    assert(kernel_vfs_rename_at(root,root,root,"/old","/dest",1)==-KERNEL_EEXIST);
    assert(!kernel_vfs_rename_at(root,root,root,"/old","/dest",0));
    contents(&b,0,"dst",3); contents(&a,0,"old",3);
    assert(!kernel_vfs_close(&b));
    assert(!kernel_vfs_rename_at(root,root,root,"/dest","/DEST",0));
    assert(!kernel_vfs_open(mount,"/dest",&b) && b.private_data==a.private_data);
    assert(!kernel_vfs_close(&a) && !kernel_vfs_close(&b));
    for(unsigned j=0;j<40;j++) {
        snprintf(path,sizeof(path),"/second/moved directory/colliding long filename %u.bin",j);
        assert(!kernel_vfs_unlink(mount,path));
    }
    assert(!kernel_vfs_rmdir(mount,"/second/moved directory") && !kernel_vfs_rmdir(mount,"/second") &&
        !kernel_vfs_rmdir(mount,"/first"));
}
static struct kernel_vfs_timespec persisted_mtime, remote_mtime;
static void metadata(struct kernel_vfs_mount *mount,struct kernel_heap *heap)
{
    struct kernel_vfs_file file={0},dir={0}; struct kernel_vfs_stat stat;
    assert(!kernel_vfs_create(mount,"/readonly",0444,&file));
    assert(!kernel_vfs_fstat(&file,&stat) && !(stat.mode&0222));
    assert(!kernel_vfs_close(&file));
    assert(!kernel_vfs_create(mount,"/remote-time",0666,&file));
    struct kernel_vfs_timespec old_time[2]={{946684800,0},{946684800,0}};
    assert(!kernel_vfs_file_set_times(&file,old_time)); sync_file(&file);
    unsigned before_writes=writes;
    mm_test_attach(1);kernel_cpu_current()->logical_id=1;
    assert(!kernel_vfs_file_modified(&file,0,0)); kernel_vfs_file_accessed(&file);
    assert(writes==before_writes);
    mm_test_attach(0);
    assert(!kernel_vfs_fstat(&file,&stat) && stat.mtime.seconds>old_time[1].seconds+86400);
    remote_mtime=stat.mtime;
    sync_file(&file); assert(writes>before_writes && !kernel_vfs_close(&file));
    assert(!kernel_vfs_mkdir(mount,"/times",0777) && !kernel_vfs_open(mount,"/times",&dir));
    struct kernel_vfs_timespec past[2]={{946684800,0},{946684800,0}};
    assert(!kernel_vfs_file_set_times(&dir,past));
    assert(!kernel_vfs_create(mount,"/times/child",0666,&file) && !kernel_vfs_close(&file));
    assert(!kernel_vfs_fstat(&dir,&stat) && stat.mtime.seconds>past[1].seconds+86400);
    persisted_mtime=stat.mtime; sync_file(&dir); assert(!kernel_vfs_close(&dir));
    assert(!kernel_vfs_open(mount,"/",&dir)); sync_file(&dir);
    struct kernel_heap_statistics before,after; kernel_heap_get_statistics(heap,&before);
    for(unsigned j=0;j<1000;j++) {
        assert(!kernel_vfs_create(mount,"/temporary",0666,&file));
        assert(!kernel_vfs_unlink(mount,"/temporary") && !kernel_vfs_close(&file));
    }
    sync_file(&dir); kernel_heap_get_statistics(heap,&after);
    assert(before.live_allocations==after.live_allocations && before.current_pages==after.current_pages);
    assert(!kernel_vfs_close(&dir));
}
static void disk_full(struct kernel_vfs_mount *mount)
{
    struct kernel_vfs_statfs before,after; struct kernel_vfs_file big={0},small={0};
    assert(!kernel_vfs_create(mount,"/full.bin",0666,&big));
    assert(!kernel_vfs_create(mount,"/prefix.bin",0666,&small));
    assert(!kernel_vfs_mount_statfs(mount,&before));
    assert(kernel_vfs_ftruncate(&big,before.free_blocks*before.block_size+1)==-KERNEL_ENOSPC);
    assert(!kernel_vfs_mount_statfs(mount,&after) && after.free_blocks==before.free_blocks);
    assert(!kernel_vfs_ftruncate(&big,(before.free_blocks-2)*before.block_size));
    unsigned char data[32768]; memset(data,0x53,sizeof(data)); size_t done;
    assert(!kernel_vfs_pwrite(&small,0,data,sizeof(data),&done) && done==2*before.block_size);
    size_t prefix=done;
    assert(kernel_vfs_pwrite(&small,prefix,data,1,&done)==-KERNEL_ENOSPC && !done);
    contents(&small,0,data,prefix); sync_file(&small);
    assert(!kernel_vfs_close(&big) && !kernel_vfs_close(&small));
    assert(!kernel_vfs_unlink(mount,"/full.bin") && !kernel_vfs_unlink(mount,"/prefix.bin"));
    assert(!kernel_vfs_mount_statfs(mount,&after) && after.free_blocks==before.free_blocks);
}
static unsigned word(const unsigned char *p) { return p[0] | (unsigned)p[1]<<8; }
static unsigned dword(const unsigned char *p) { return word(p) | word(p+2)<<16; }
static void stale_directory_tail(void)
{
    unsigned char boot[512],entry[32]={0}; assert(pread(fd,boot,512,0)==512);
    uint64_t data=(word(boot+14)+(uint64_t)boot[16]*dword(boot+36))*word(boot+11);
    data+=(uint64_t)(dword(boot+44)-2)*boot[13]*word(boot+11);
    memcpy(entry,"STALE   TXT",11); entry[11]=32;
    assert(pwrite(fd,entry,32,data+64)==32);
}
static void corrupt_mounts(struct kernel_vfs_mount *mount,struct kernel_block_device *block,
    struct kernel_heap *heap,struct kernel_page_cache *cache)
{
    unsigned char boot[512]; assert(pread(fd,boot,512,0)==512);
    unsigned char invalid[2]={0}; assert(pwrite(fd,invalid,2,11)==2);
    assert(kernel_vfs_mount_fat32(mount,block,heap,cache,0)==-KERNEL_EINVAL && !mount->private_data && !kernel_block_claimed(block));
    assert(pwrite(fd,boot+11,2,11)==2);
    assert(boot[16]==2);
    uint64_t other=(word(boot+14)+(uint64_t)dword(boot+36))*word(boot+11)+8;
    unsigned char saved[4],bad[4]; assert(pread(fd,saved,4,other)==4);
    memcpy(bad,saved,4); bad[0]^=0x80; assert(pwrite(fd,bad,4,other)==4);
    assert(kernel_vfs_mount_fat32(mount,block,heap,cache,0)==-KERNEL_EUCLEAN && !mount->private_data && !kernel_block_claimed(block));
    assert(pwrite(fd,saved,4,other)==4);
}
int main(int argc, char **argv)
{
    assert(argc >= 2); setvbuf(stdout, 0, _IONBF, 0); mm_test_attach(0);
    fd = open(argv[1], O_RDWR); assert(fd >= 0);
    stale_directory_tail();
    struct stat st; assert(!fstat(fd, &st));
    struct kernel_block_device block = {.read=rd, .write=wr, .flush=fl, .capacity_bytes=st.st_size,
        .logical_block_size=512, .cache_mode=KERNEL_BLOCK_CACHE_WRITEBACK};
    assert(!kernel_block_register(&block, 0xfc10));
    struct physical_page_allocator pages = {0}; struct kernel_heap heap = {0};
    struct boot_memory_layout layout = {.usable_count=1, .usable={{(uintptr_t)pool,sizeof(pool)}}};
    assert(!physical_page_allocator_init(&pages, &layout) && !physical_page_allocator_bind_access(&pages, access_page) &&
        !physical_page_allocator_finalize(&pages) && !kernel_heap_init(&heap, &pages, page_address));
    uint64_t baseline = physical_page_available(&pages);
    struct kernel_page_cache cache = {0}; struct kernel_vfs_mount mount = {0};
    assert(!kernel_page_cache_init(&cache, &heap, &pages));
    corrupt_mounts(&mount,&block,&heap,&cache);
    assert(!kernel_vfs_mount_fat32(&mount, &block, &heap, &cache, 0));
    struct kernel_vfs_file file={0}, same={0}; size_t done;
    if(argc==3 && !strcmp(argv[2],"fault")) {
        assert(!kernel_vfs_create(&mount,"/fault",0666,&file) && !kernel_vfs_ftruncate(&file,4096));
        unsigned char data=91; assert(!kernel_vfs_pwrite(&file,0,&data,1,&done) && done==1);
        write_fail=1; unsigned attempted=writes;
        uint64_t mount_observed=kernel_vfs_mount_error_sequence(&mount);
        assert(kernel_vfs_sync_mount(&mount,&mount_observed)==-KERNEL_EIO && writes==attempted+1);
        uint64_t observed=kernel_vfs_error_sequence(&file);
        assert(kernel_vfs_sync(&file,0,&observed)==-KERNEL_EIO);
        assert(!kernel_vfs_close(&file));
        assert(kernel_vfs_unmount(&mount)==-KERNEL_EIO && mount.private_data && kernel_block_claimed(&block));
        assert(kernel_block_unregister(&block)==-KERNEL_EBUSY);
        puts("PASS FAT32 writeback EIO reaches fsync and retains failed mount/cache/device owner");
        return 0;
    }
    struct kernel_vfs_path *root=0; assert(!kernel_vfs_path_root(&mount, &heap, &root));
    contiguous_transfers(&mount);
    assert(!kernel_vfs_mkdir(&mount, "/目录", 0777));
    assert(kernel_vfs_open(&mount,"/STALE.TXT",&same)==-KERNEL_ENOENT);
    assert(!kernel_vfs_create(&mount, "/目录/Long Unicode 文件😀.txt", 0666, &file));
    unsigned char data[32768], zero[32768]={0};
    for (unsigned j=0;j<sizeof(data);j++) data[j]=(j*37U+19U)%251;
    assert(!kernel_vfs_pwrite(&file, 0, data, sizeof(data), &done) && done==sizeof(data));
    sync_file(&file); contents(&file, 0, data, sizeof(data));
    assert(!kernel_vfs_rename_at(root, root, root, "/目录/Long Unicode 文件😀.txt", "/Moved Name.txt", 0));
    assert(!kernel_vfs_open(&mount, "/moved name.TXT", &same));
    assert(file.private_data == same.private_data); contents(&same, 0, data, sizeof(data));
    assert(!kernel_vfs_close(&same));
    assert(!kernel_vfs_ftruncate(&file, 513));
    assert(!kernel_vfs_ftruncate(&file, sizeof(data)));
    contents(&file, 0, data, 513); contents(&file, 513, zero, sizeof(data)-513);
    assert(!kernel_vfs_pwrite(&file, 32000, data, 37, &done) && done==37);
    sync_file(&file);
    assert(!kernel_vfs_unlink(&mount, "/Moved Name.txt"));
    contents(&file, 32000, data, 37);
    assert(!kernel_vfs_pwrite(&file, 0, data, 91, &done) && done==91);
    sync_file(&file); contents(&file, 0, data, 91);
    assert(!kernel_vfs_close(&file));
    assert(!kernel_vfs_create(&mount, "/REUSED.BIN", 0666, &file));
    assert(!kernel_vfs_ftruncate(&file, 17001)); contents(&file, 0, zero, 17001);
    sync_file(&file); assert(!kernel_vfs_close(&file));
    assert(!kernel_vfs_rmdir(&mount, "/目录"));
    names(&mount,root);
    metadata(&mount,&heap);
    disk_full(&mount);
    assert(!kernel_vfs_path_release(&root));
    assert(!kernel_vfs_unmount(&mount));
    assert(!kernel_page_cache_destroy(&cache));
    struct kernel_page_cache read_cache={0};
    assert(!kernel_page_cache_init(&read_cache,&heap,&pages));
    assert(!kernel_vfs_mount_fat32(&mount,&block,&heap,&read_cache,1));
    assert(!kernel_vfs_open(&mount,"/REUSED.BIN",&file));
    assert(!kernel_page_cache_start_worker(&read_cache));
    struct kernel_page_cache_concurrency_state before_read,after_read;
    kernel_page_cache_get_concurrency_state(&read_cache,&before_read);
    watch_remote_read=1;remote_reads=0;
    pthread_t remote;
    assert(!pthread_create(&remote,NULL,remote_contents,&file) && !pthread_join(remote,NULL));
    watch_remote_read=0;
    kernel_page_cache_get_concurrency_state(&read_cache,&after_read);
    assert(remote_reads && after_read.requests_queued>before_read.requests_queued &&
        after_read.requests_queued==after_read.requests_completed);
    kernel_page_cache_stop_worker(&read_cache);
    puts("PASS FAT caller CPU1 retains CPU0 backend policy: actual device reads, content and owners");
    contents(&file,0,zero,17001);
    assert(kernel_vfs_pwrite(&file,0,data,1,&done)==-KERNEL_EROFS && !done);
    assert(!kernel_vfs_close(&file));
    assert(!kernel_vfs_open(&mount,"/times",&file));
    struct kernel_vfs_stat saved; assert(!kernel_vfs_fstat(&file,&saved));
    assert(saved.mtime.seconds==(persisted_mtime.seconds&~INT64_C(1)));
    assert(!kernel_vfs_close(&file));
    assert(!kernel_vfs_open(&mount,"/readonly",&file) && !kernel_vfs_fstat(&file,&saved) && !(saved.mode&0222));
    assert(!kernel_vfs_close(&file));
    assert(!kernel_vfs_open(&mount,"/remote-time",&file) && !kernel_vfs_fstat(&file,&saved));
    assert(saved.mtime.seconds==(remote_mtime.seconds&~INT64_C(1)));
    assert(!kernel_vfs_close(&file) && !kernel_vfs_unmount(&mount) && !kernel_page_cache_destroy(&read_cache));
    assert(!kernel_block_unregister(&block));
    struct kernel_heap_statistics stats; kernel_heap_get_statistics(&heap,&stats);
    assert(!stats.live_allocations && !stats.current_pages && physical_page_available(&pages)==baseline &&
        !physical_page_allocator_audit(&pages));
    assert(!close(fd)); puts("PASS FAT32/VFAT content, names, rename, unlink, truncate, sync and owner return");
}
