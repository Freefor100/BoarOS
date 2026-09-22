#ifndef BOAROS_TEST_LWEXT4_WRITEBACK_H
#define BOAROS_TEST_LWEXT4_WRITEBACK_H
#include <ext4_block_group.h>

static int check_bitmap_failure(struct host_image *image,
                                struct ext4_blockdev *device)
{
    ext4_file file;
    struct ext4_block_group_ref group;
    size_t count = 0;
    if (ext4_fopen(&file, "/wb-bitmap", "w+") ||
        ext4_fs_get_block_group_ref(device->fs,
            (file.inode - 1) / ext4_get32(&device->fs->sb, inodes_per_group),
            &group)) return 1;
    image->fail_write_offset = ext4_bg_get_block_bitmap(group.block_group,
                                    &device->fs->sb) * device->lg_bsize;
    if (ext4_fs_put_block_group_ref(&group)) return 1;
    image->fail_writes = 1;
    int result = ext4_fwrite(&file, "data", 4, &count);
    if (result != EIO || image->fail_writes != 0) {
        fprintf(stderr, "bitmap failure rc=%d pending=%u\n", result,
                 image->fail_writes);
        return 1;
    }
    if (ext4_cache_flush("/") || ext4_fseek(&file, 0, SEEK_SET) ||
        ext4_fwrite(&file, "data", 4, &count) || count != 4 ||
        ext4_ftruncate(&file, 0) || ext4_fclose(&file) ||
        ext4_fremove("/wb-bitmap")) return 1;
    /* The runner's e2fsck verifies no unowned bit or count remains. */
    return 0;
}

static int check_writeback_errors(struct host_image *image,
                                  struct ext4_blockdev *device)
{
    ext4_file first = {0}, second = {0};
    struct ext4_inode_ref ref;
    ext4_fsblk_t data_block;
    size_t written;
    int result;
    if (check_bitmap_failure(image, device)) return 1;
    if (ext4_fopen(&first, "/wb-a", "w+") ||
        ext4_fs_get_inode_ref(device->fs, first.inode, &ref))
        return report_error("writeback fixture", EIO);
    image->fail_write_offset = ref.block.lb_id * device->lg_bsize;
    if (ext4_fs_put_inode_ref(&ref)) return 1;
    image->fail_writes = 1;
    result = ext4_fwrite(&first, "metadata", 8, &written);
    if (result != EIO || written != 8 || image->fail_writes != 0) {
        fprintf(stderr, "final inode failure: rc=%d written=%zu pending=%u\n",
                 result, written, image->fail_writes);
        return 1;
    }
    if (ext4_file_sync_metadata(&first)) return 1;
    if (ext4_fopen(&second, "/wb-b", "w+") ||
        ext4_fwrite(&second, "B", 1, &written) || written != 1 ||
        ext4_fs_get_inode_ref(device->fs, second.inode, &ref)) return 1;
    result = ext4_fs_get_inode_dblk_idx(&ref, 0, &data_block, true);
    if (ext4_fs_put_inode_ref(&ref) || result || data_block == 0) return 1;
    image->fail_write_offset = data_block * device->lg_bsize;
    if (ext4_cache_write_back("/", 1) || ext4_fseek(&second, 0, SEEK_SET) ||
        ext4_fwrite(&second, "b", 1, &written) || written != 1) return 1;
    image->fail_writes = 10;
    if (ext4_cache_write_back("/", 0) != EIO) return 1;
    unsigned int before = image->failed_writes;
    if (ext4_fseek(&first, 0, SEEK_SET)) return 1;
    result = ext4_fwrite(&first, "A", 1, &written);
    if (result != EOK || written != 1 || image->failed_writes != before) {
        fprintf(stderr, "unrelated dirty buffer: rc=%d writes=%u\n",
                 result, image->failed_writes - before);
        return 1;
    }
    controlled_time = (struct ext4_timestamp){123456789, 123};
    clock_available = true;
    if (ext4_mount_setup_clock("/", controlled_clock) ||
        ext4_file_touch(&first, EXT4_TIME_MTIME | EXT4_TIME_CTIME) ||
        image->failed_writes != before)
        return report_error("unrelated dirty buffer during touch", EIO);
    if (ext4_mount_setup_clock("/", NULL)) return 1;
    image->fail_writes = 0;
    if (ext4_cache_flush("/") || ext4_fclose(&first) || ext4_fclose(&second) ||
        ext4_fremove("/wb-a") || ext4_fremove("/wb-b")) return 1;
    return 0;
}

#endif
