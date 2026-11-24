#define main file_io_main
#include "file_io_concurrency.c"
#undef main

static void check_ext3(struct kernel_heap *heap, uint64_t dev)
{
    struct kernel_vfs_mount *mount = NULL;
    struct kernel_vfs_path *root = NULL;
    struct kernel_vfs_file file = {0};
    size_t done;
    char data[8];

    assert(!kernel_vfs_disk_create(heap, dev, 1, "/dev/test", "ext3", &mount));
    struct kernel_vfs_instance *instance = mount->private_data;
    assert(!strcmp(instance->ops->type, "ext3") && instance->read_only);
    assert(!kernel_vfs_path_root(mount, heap, &root));
    assert(!kernel_vfs_remount(root, 0, NULL));
    assert(!kernel_vfs_create_at(root, root, "file", 0600, &file, 0));
    assert(!kernel_vfs_pwrite(&file, 0, "persist", 7, &done) && done == 7);
    assert(!kernel_vfs_close(&file));
    assert(!kernel_vfs_remount(root, 1U, NULL));
    assert(!kernel_vfs_path_release(&root));
    assert(!kernel_vfs_unmount(mount));

    mount = NULL;
    assert(!kernel_vfs_disk_create(heap, dev, 0, "/dev/test", "ext3", &mount));
    assert(!kernel_vfs_open(mount, "/file", &file));
    assert(!kernel_vfs_pread(&file, 0, data, sizeof(data), &done, NULL));
    assert(done == 7 && !memcmp(data, "persist", 7));
    assert(!kernel_vfs_close(&file));
    assert(!kernel_vfs_unmount(mount));
}

int main(int argc, char **argv)
{
    assert(argc == 2 || argc == 3);
    mm_test_attach(0);
    mm_test_activate_task();
    struct physical_page_allocator pages = {0};
    struct kernel_heap heap = {0};
    initialize(&pages, &heap, arena, sizeof(arena));
    uint64_t available = physical_page_available(&pages);
    image = open(argv[1], O_RDWR);
    struct stat info;
    assert(image >= 0 && !fstat(image, &info));
    const uint64_t dev = KERNEL_BLOCK_DEVICE_NUMBER(109);
    struct kernel_block_device disk = {
        .read = read_image, .write = write_image, .flush = flush_image,
        .capacity_bytes = info.st_size, .logical_block_size = 512,
        .cache_mode = KERNEL_BLOCK_CACHE_WRITEBACK,
    };
    assert(!kernel_block_register(&disk, dev));
    if (argc == 2) check_ext3(&heap, dev);
    else {
        struct kernel_vfs_mount *mount = NULL;
        int expected = !strcmp(argv[2], "dirty") ? -KERNEL_EUCLEAN : -KERNEL_EINVAL;
        assert(kernel_vfs_disk_create(&heap, dev, 1, "/dev/test", "ext3", &mount) == expected);
        assert(!mount);
    }
    assert(!kernel_block_claimed(&disk) && !kernel_block_unregister(&disk));
    assert(!close(image));
    struct kernel_heap_statistics stats;
    kernel_heap_get_statistics(&heap, &stats);
    assert(!stats.live_allocations && physical_page_available(&pages) == available);
    mm_test_finish();
    puts("PASS ext3 mount, filesystem type, writable transition and ownership");
    return 0;
}
