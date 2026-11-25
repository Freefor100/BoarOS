#define main original_file_io_main
#include "file_io_concurrency.c"
#undef main
#include <ext4.h>
#include <ext4_inode.h>
#include <ext4_fs.h>

static _Thread_local unsigned stage_oom,inside_stage;
static unsigned stage_denied;
static void (*original_store)(void *,void *,uint32_t,unsigned,const struct ext4_timestamp [3]);
static bool (*original_changed)(void *,uint32_t);
void *__real_ext4_user_calloc(size_t,size_t);
void *__wrap_ext4_user_calloc(size_t count,size_t bytes)
{
    if (stage_oom && inside_stage) { stage_denied++;return NULL; }
    return __real_ext4_user_calloc(count,bytes);
}
static void observe_store(void *context,void *owner,uint32_t inode,unsigned fields,
    const struct ext4_timestamp values[3])
{
    inside_stage++;original_store(context,owner,inode,fields,values);inside_stage--;
}
static bool observe_changed(void *context,uint32_t inode)
{
    inside_stage++;
    bool result=original_changed(context,inode);
    inside_stage--;
    return result;
}
int __real_ext4_mount_setup_inode_observer(const char *,const struct ext4_inode_observer *);
int __wrap_ext4_mount_setup_inode_observer(const char *path,const struct ext4_inode_observer *observer)
{
    if (!observer) return __real_ext4_mount_setup_inode_observer(path,NULL);
    struct ext4_inode_observer watched=*observer;
    original_store=watched.store;original_changed=watched.changed;
    if (watched.store) watched.store=observe_store;
    if (watched.changed) watched.changed=observe_changed;
    return __real_ext4_mount_setup_inode_observer(path,&watched);
}
static void nonjournal_staging_failure(struct lwext4_mount_adapter *adapter,struct kernel_vfs_file *file)
{
    struct kernel_vfs_stat cached,actual;
    assert(!adapter->device.fs->jbd_journal && !kernel_vfs_fstat(file,&cached));
    stage_oom=1;stage_denied=0;
    int result=kernel_vfs_file_set_mode(file,0640);
    stage_oom=0;
    assert(result==-KERNEL_ENOMEM && stage_denied);
    struct ext4_inode inode;
    assert(!ext4_fraw_inode_fill(lwext4_node_file(file->private_data),&inode));
    assert(!kernel_vfs_fstat(file,&actual) &&
        actual.mode==ext4_inode_get_mode(adapter->superblock,&inode));
    assert((actual.mode&0777)==0640 && actual.mode!=cached.mode);
    assert(!kernel_vfs_file_set_mode(file,0660) && !kernel_vfs_file_set_mode(file,0600));
    puts("PASS nonjournal staging ENOMEM preserves actual changed prefix and invalidates cached attributes");
}

static void check_cached_attributes(struct kernel_vfs_path *root, struct kernel_vfs_file *file)
{
    struct kernel_vfs_stat before, after;
    assert(!kernel_vfs_fstat(file,&before));
    struct kernel_vfs_node *node=file->private_data;
    write_sequence_lock=&node->io_lock;
    write_sequence_queries=0;
    assert(!kernel_vfs_fstat(file,&after) && !write_sequence_queries);
    assert(after.ino==before.ino && after.mode==before.mode &&
        after.size==before.size && after.blocks==before.blocks);

    struct kernel_vfs_file opened={0};
    const struct kernel_vfs_open_request request={
        .flags=KERNEL_VFS_OPEN_FOLLOW,.access=KERNEL_VFS_MAY_READ};
    assert(!kernel_vfs_open_at(root,root,"timestamp",&request,&opened,NULL));
    assert(opened.private_data==node && write_sequence_queries==1);
    write_sequence_lock=NULL;
    assert(!kernel_vfs_close(&opened));
    puts("PASS cached stat skips the inode data lock and open reuses its single write qualification");
}

static struct ext4_timestamp timestamp_now;
static unsigned timestamp_sampling, timestamp_continue;
static _Thread_local unsigned timestamp_pause;
static _Thread_local unsigned timestamp_samples;
static _Thread_local unsigned timestamp_fail;
static unsigned directory_touches;
static _Thread_local uint32_t directory_touch_inode;
static unsigned directory_probe_reads;
static unsigned directory_refresh_before_touch;
int __real_ext4_fs_get_inode_ref_read(struct ext4_fs *, uint32_t, struct ext4_inode_ref *);
int __wrap_ext4_fs_get_inode_ref_read(struct ext4_fs *fs, uint32_t inode, struct ext4_inode_ref *ref)
{
    if (directory_touch_inode && inode==directory_touch_inode) directory_probe_reads++;
    return __real_ext4_fs_get_inode_ref_read(fs,inode,ref);
}
int __real_ext4_file_touch(ext4_file *, unsigned);
int __wrap_ext4_file_touch(ext4_file *file, unsigned fields)
{
    directory_touches++;
    if (directory_refresh_before_touch) {
        struct ext4_timestamp newer[3] = {timestamp_now};
        newer[0].seconds++;
        assert(!ext4_file_set_times(file,EXT4_TIME_ATIME,newer));
    }
    directory_touch_inode=file->inode;
    int result=__real_ext4_file_touch(file, fields);
    directory_touch_inode=0;
    return result;
}

static bool timestamp_clock(struct ext4_timestamp *now)
{
    timestamp_samples++;
    if (timestamp_fail) return false;
    *now = timestamp_now;
    if (timestamp_pause) {
        timestamp_pause = 0;
        __atomic_store_n(&timestamp_sampling, 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&timestamp_continue, __ATOMIC_ACQUIRE)) sched_yield();
    }
    return true;
}
static void *access_during_remount(void *argument)
{
    mm_test_attach(1); mm_test_activate_task();
    timestamp_pause = 1;
    kernel_vfs_file_accessed(argument,0);
    mm_test_finish();
    return NULL;
}
struct timestamp_remount {
    struct kernel_vfs_path *root;
    unsigned completed;
};
static void *remount_during_access(void *argument)
{
    struct timestamp_remount *test = argument;
    mm_test_attach(2); mm_test_activate_task();
    assert(!kernel_vfs_remount(test->root, 1, NULL));
    __atomic_store_n(&test->completed, 1, __ATOMIC_RELEASE);
    mm_test_finish();
    return NULL;
}

static void *directory_read_during_remount(void *argument)
{
    struct kernel_vfs_file *directory=argument;
    struct kernel_vfs_stat stat;
    unsigned char entries[256];
    uint32_t flags=0;
    mm_test_attach(1); mm_test_activate_task();
    assert(!kernel_vfs_fstat(directory,&stat));
    struct kernel_vfs_dir_batch batch={.data=entries,.capacity=sizeof(entries),.position=stat.size};
    timestamp_pause=1;
    assert(!kernel_vfs_read_directory(directory,&batch,&flags) && !batch.used);
    mm_test_finish();
    return NULL;
}

static void remount_directory_during_relatime(struct kernel_vfs_path *root,
    struct kernel_vfs_file *directory,uint64_t policy)
{
    const uint64_t bind=4096U, relatime=2097152U;
    pthread_t reader;
    __atomic_store_n(&timestamp_sampling,0,__ATOMIC_RELEASE);
    __atomic_store_n(&timestamp_continue,0,__ATOMIC_RELEASE);
    assert(!pthread_create(&reader,NULL,directory_read_during_remount,directory));
    while (!__atomic_load_n(&timestamp_sampling,__ATOMIC_ACQUIRE)) sched_yield();
    assert(!kernel_vfs_remount(root,bind|policy,NULL));
    __atomic_store_n(&timestamp_continue,1,__ATOMIC_RELEASE);
    assert(!pthread_join(reader,NULL));
    assert(!kernel_vfs_remount(root,bind|relatime,NULL));
}

static void check_directory_relatime(struct kernel_vfs_path *root)
{
    const struct ext4_timestamp original = timestamp_now;
    struct kernel_vfs_file directory = {0};
    const struct kernel_vfs_open_request request = {.flags = KERNEL_VFS_OPEN_FOLLOW};
    assert(!kernel_vfs_mkdir_at(root, root, "atime-directory", 0700, 0));
    assert(!kernel_vfs_open_at(root, root, "atime-directory", &request, &directory, NULL));
    const struct kernel_vfs_timespec times[2] = {
        {timestamp_now.seconds + 1, 100}, {timestamp_now.seconds, 0}
    };
    assert(!kernel_vfs_file_set_times(&directory, times));
    struct kernel_vfs_stat stat;
    assert(!kernel_vfs_fstat(&directory, &stat));
    directory_touches = 0;
    directory_probe_reads = 0;
    timestamp_now.seconds = times[0].seconds + 86399;
    uint32_t flags = 0;
    unsigned char entries[256];
    struct kernel_vfs_dir_batch batch = {.data=entries,.capacity=sizeof(entries)};
    assert(!kernel_vfs_read_directory(&directory, &batch, &flags) && batch.used);
    assert(batch.position==stat.size);
    batch.used=0;
    for (unsigned i = 0; i < 4; ++i)
        assert(!kernel_vfs_read_directory(&directory, &batch, &flags) && !batch.used);
    assert(!directory_touches && "enumeration should use the inode's already borrowed timestamps");

    timestamp_now.seconds++;
    assert(!kernel_vfs_read_directory(&directory, &batch, &flags) && !batch.used);
    assert(directory_touches == 1 && !kernel_vfs_fstat(&directory, &stat));
    assert(!directory_probe_reads && "known expiration goes directly to the writer recheck");
    assert(stat.atime.seconds == timestamp_now.seconds);
    timestamp_now.seconds+=86400;
    int64_t published=stat.atime.seconds;
    remount_directory_during_relatime(root,&directory,1024U);
    assert(directory_touches==1 && !kernel_vfs_fstat(&directory,&stat) && stat.atime.seconds==published);
    timestamp_now.seconds=published+1;
    remount_directory_during_relatime(root,&directory,16777216U);
    assert(directory_touches==2 && !kernel_vfs_fstat(&directory,&stat) && stat.atime.seconds==timestamp_now.seconds);

    timestamp_now.seconds+=86400;
    directory_refresh_before_touch=1;
    assert(!kernel_vfs_read_directory(&directory,&batch,&flags) && !batch.used);
    directory_refresh_before_touch=0;
    assert(directory_touches==3 && !directory_probe_reads && !kernel_vfs_fstat(&directory,&stat));
    assert(stat.atime.seconds==timestamp_now.seconds+1);
    assert(!kernel_vfs_close(&directory));
    assert(!kernel_vfs_unlink_at(root, root, "atime-directory", 1));
    timestamp_now = original;
    puts("PASS directory relatime reuses the query, honors remount, and rechecks under the writer");
}

int main(int argc, char **argv)
{
    assert(argc == 2 || argc == 3);
    setvbuf(stdout, NULL, _IONBF, 0);
    mm_test_attach(0); mm_test_activate_task();
    struct physical_page_allocator pages = {0}, backend_pages = {0};
    struct kernel_heap heap = {0}, backend_heap = {0};
    initialize(&pages, &heap, arena, sizeof(arena));
    initialize(&backend_pages, &backend_heap, backend, sizeof(backend));
    uint64_t available = physical_page_available(&pages);
    uint64_t backend_available = physical_page_available(&backend_pages);
    image = open(argv[1], O_RDWR); assert(image >= 0);
    struct stat info; assert(!fstat(image, &info));
    struct kernel_page_cache cache = {0};
    assert(!kernel_page_cache_init(&cache, &heap, &pages));
    struct kernel_block_device disk = {.read = read_image, .write = write_image,
        .flush = flush_image, .capacity_bytes = info.st_size, .logical_block_size = 512,
        .cache_mode = KERNEL_BLOCK_CACHE_WRITEBACK};
    struct kernel_vfs_mount mount = {0};
    assert(!kernel_block_register(&disk, KERNEL_BLOCK_DEVICE_NUMBER(105)));
    assert(!kernel_vfs_mount_ext4(&mount, &disk, &backend_heap, &cache, 0));
    struct lwext4_mount_adapter *adapter = mount.private_data;
    assert(!ext4_mount_setup_clock(adapter->mount_point, timestamp_clock));
    timestamp_now = (struct ext4_timestamp){2000000000, 10};
    struct kernel_vfs_path *root = NULL;
    assert(!kernel_vfs_path_root(&mount, &heap, &root));
    if (argc == 2) check_directory_relatime(root);
    struct kernel_vfs_file file = {0};
    const struct kernel_vfs_open_request request = {.flags = KERNEL_VFS_OPEN_FOLLOW};
    if (argc == 3) assert(!kernel_vfs_open_at(root, root, "timestamp", &request, &file, NULL));
    else assert(!kernel_vfs_create_at(root, root, "timestamp", 0600, &file,0));
    if (argc==3) nonjournal_staging_failure(adapter,&file);
    check_cached_attributes(root,&file);
    uint64_t observed_error = kernel_vfs_error_sequence(&file);
    const struct kernel_vfs_timespec explicit[2] = {{2000000100, 20}, {1999999900, 30}};
    assert(!kernel_vfs_file_set_times(&file, explicit));
    struct kernel_vfs_stat before, after;
    assert(!kernel_vfs_fstat(&file, &before));
    unsigned failures = 0;
    /* atime newer than mtime/ctime still expires at the whole-second day boundary. */
    timestamp_now = (struct ext4_timestamp){2000086499, 40};
    kernel_vfs_file_accessed(&file,0);
    assert(!kernel_vfs_fstat(&file, &after));
    assert(after.atime.seconds == before.atime.seconds &&
           after.atime.nanoseconds == before.atime.nanoseconds);
    timestamp_now.seconds++;
    kernel_vfs_file_accessed(&file,0);
    assert(!kernel_vfs_fstat(&file, &after));
    if (after.atime.seconds != timestamp_now.seconds || after.atime.nanoseconds != 40) {
        puts("FAIL relatime: newer atime must update after 24 hours"); failures++;
    } else puts("PASS relatime: skip before 24 hours and update at the boundary");
    struct kernel_vfs_timespec expected_atime = after.atime;
    assert(!kernel_vfs_sync(&file, 0, &observed_error));
    timestamp_now = (struct ext4_timestamp){2000086501, 100};
    assert(!kernel_vfs_file_modified(&file, 0, 0));
    assert(!kernel_vfs_fstat(&file, &before));
    assert(before.mtime.seconds == timestamp_now.seconds && before.mtime.nanoseconds == 100);
    timestamp_now.nanoseconds = 200;
    assert(!kernel_vfs_file_modified(&file, 0, 0));
    assert(!kernel_vfs_fstat(&file, &after));
    if (after.mtime.nanoseconds != 200 || after.ctime.nanoseconds != 200) {
        puts("FAIL writes: same-second mtime and ctime must follow the sampled clock"); failures++;
    } else puts("PASS writes: same-second mtime and ctime update");
    timestamp_fail = 1;
    kernel_vfs_file_accessed(&file,0);
    timestamp_fail = 0;
    assert(!kernel_vfs_fstat(&file, &after));
    assert(after.atime.seconds == expected_atime.seconds &&
           after.atime.nanoseconds == expected_atime.nanoseconds);
    puts("PASS failed clock leaves post-write atime unchanged");
    timestamp_now.nanoseconds = 300;
    timestamp_samples = 0;
    kernel_vfs_file_accessed(&file,0);
    assert(!kernel_vfs_fstat(&file, &after));
    assert(after.atime.seconds == timestamp_now.seconds && after.atime.nanoseconds == 300);
    expected_atime = after.atime;
    if (timestamp_samples != 1) {
        printf("FAIL post-write relatime: expected one clock sample, got %u\n", timestamp_samples);
        failures++;
    } else puts("PASS post-write relatime: one clock sample updates atime");
    assert(!kernel_vfs_sync(&file, 0, &observed_error));
    assert(!kernel_vfs_close(&file));
    assert(!kernel_vfs_open_at(root, root, "timestamp", &request, &file, NULL));
    assert(!kernel_vfs_fstat(&file, &before));
    assert(before.mtime.seconds == after.mtime.seconds &&
           before.mtime.nanoseconds == after.mtime.nanoseconds &&
           before.ctime.seconds == after.ctime.seconds &&
           before.ctime.nanoseconds == after.ctime.nanoseconds &&
           before.atime.seconds == expected_atime.seconds &&
           before.atime.nanoseconds == expected_atime.nanoseconds);
    if (argc == 3) {
        /* 更新必需时，采样已持有mutation资格；真实remount须等待读取者发布。 */
        timestamp_now.nanoseconds = 400;
        assert(!kernel_vfs_file_modified(&file, 0, 0));
        timestamp_now.nanoseconds = 500;
        pthread_t reader, remounter;
        assert(!pthread_create(&reader, NULL, access_during_remount, &file));
        while (!__atomic_load_n(&timestamp_sampling, __ATOMIC_ACQUIRE)) sched_yield();
        struct timestamp_remount remount = {.root = root};
        unsigned parked = mm_test_parked(2);
        assert(!pthread_create(&remounter, NULL, remount_during_access, &remount));
        while (mm_test_parked(2) == parked) sched_yield();
        assert(!__atomic_load_n(&remount.completed, __ATOMIC_ACQUIRE) &&
               !kernel_vfs_mount_is_readonly(&mount));
        __atomic_store_n(&timestamp_continue, 1, __ATOMIC_RELEASE);
        assert(!pthread_join(reader, NULL) && !pthread_join(remounter, NULL));
        assert(kernel_vfs_mount_is_readonly(&mount) && !kernel_vfs_fstat(&file, &after));
        assert(after.atime.seconds == timestamp_now.seconds && after.atime.nanoseconds == 500);
        puts("PASS remount waits for post-write atime sample and publication");
        assert(!kernel_vfs_remount(root, 0, NULL));

        /* 年龄检查尚未取得mutation资格。RO完成后，迟到的采样不得发布。 */
        const struct kernel_vfs_timespec newer_atime[2] = {
            {timestamp_now.seconds + 100, 10}, {timestamp_now.seconds - 100, 20}};
        assert(!kernel_vfs_file_set_times(&file, newer_atime) && !kernel_vfs_fstat(&file, &before));
        timestamp_now.seconds = newer_atime[0].seconds + 86400;
        timestamp_sampling = timestamp_continue = 0;
        assert(!pthread_create(&reader, NULL, access_during_remount, &file));
        while (!__atomic_load_n(&timestamp_sampling, __ATOMIC_ACQUIRE)) sched_yield();
        assert(!kernel_vfs_remount(root, 1, NULL) && kernel_vfs_mount_is_readonly(&mount));
        __atomic_store_n(&timestamp_continue, 1, __ATOMIC_RELEASE);
        assert(!pthread_join(reader, NULL));
        assert(!kernel_vfs_fstat(&file, &after));
        assert(after.atime.seconds == before.atime.seconds &&
               after.atime.nanoseconds == before.atime.nanoseconds);
        assert(!kernel_vfs_close(&file));
        puts("PASS sampled access cannot publish after read-only remount");
    } else {
        assert(!kernel_vfs_close(&file));
        assert(!kernel_vfs_unlink_at(root, root, "timestamp", 0));
    }
    assert(!kernel_vfs_path_release(&root) && !kernel_vfs_unmount(&mount));
    assert(!kernel_page_cache_destroy(&cache) && !kernel_block_unregister(&disk));
    struct kernel_heap_statistics statistics;
    kernel_heap_get_statistics(&heap, &statistics); assert(!statistics.live_allocations);
    kernel_heap_get_statistics(&backend_heap, &statistics); assert(!statistics.live_allocations);
    assert(physical_page_available(&pages) == available &&
           physical_page_available(&backend_pages) == backend_available);
    assert(!close(image)); mm_test_finish();
    puts("PASS timestamp persistence and owner release");
    return failures ? 1 : 0;
}
