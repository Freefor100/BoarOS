#define main original_file_io_main
#include "file_io_concurrency.c"
#undef main
#include <ext4.h>

static struct ext4_timestamp timestamp_now;
static unsigned timestamp_sampling, timestamp_continue;
static _Thread_local unsigned timestamp_pause;
static bool timestamp_clock(struct ext4_timestamp *now)
{
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
    kernel_vfs_file_accessed(argument);
    mm_test_finish();
    return NULL;
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
    struct kernel_vfs_file file = {0};
    const struct kernel_vfs_open_request request = {.flags = KERNEL_VFS_OPEN_FOLLOW};
    if (argc == 3) assert(!kernel_vfs_open_at(root, root, "timestamp", &request, &file, NULL));
    else assert(!kernel_vfs_create_at(root, root, "timestamp", 0600, &file));
    uint64_t observed_error = kernel_vfs_error_sequence(&file);
    const struct kernel_vfs_timespec explicit[2] = {{2000000100, 20}, {1999999900, 30}};
    assert(!kernel_vfs_file_set_times(&file, explicit));
    struct kernel_vfs_stat before, after;
    assert(!kernel_vfs_fstat(&file, &before));
    unsigned failures = 0;
    /* atime newer than mtime/ctime still expires at the whole-second day boundary. */
    timestamp_now = (struct ext4_timestamp){2000086499, 40};
    kernel_vfs_file_accessed(&file);
    assert(!kernel_vfs_fstat(&file, &after));
    assert(after.atime.seconds == before.atime.seconds &&
           after.atime.nanoseconds == before.atime.nanoseconds);
    timestamp_now.seconds++;
    kernel_vfs_file_accessed(&file);
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
        /* Sampling does not publish a mutation. Finish RO remount before the
         * paused reader proceeds, then check it cannot dirty the RO fs. */
        pthread_t reader;
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
