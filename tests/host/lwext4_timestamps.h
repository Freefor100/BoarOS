/* Exercise the public live-inode API against actual ext4 images. */
static struct ext4_timestamp controlled_time;
static bool clock_available;

static bool controlled_clock(struct ext4_timestamp *now)
{
    *now = controlled_time;
    return clock_available;
}

static int64_t disk_seconds(uint32_t base, uint32_t extra, bool extended)
{
    return (int64_t)(int32_t)to_le32(base) +
        (extended ? ((int64_t)(to_le32(extra) & 3U) << 32) : 0);
}

static int check_timestamp_behavior(bool readonly)
{
    static const int64_t seconds[] = {
        INT64_C(-2147483649), INT64_C(-2147483648), -1, 0,
        INT64_C(2147483647), INT64_C(2147483648), INT64_C(4294967295),
        INT64_C(4294967296), INT64_C(15032385535), INT64_C(15032385536)
    };
    struct ext4_sblock *superblock;
    struct ext4_inode before = {0}, after = {0};
    ext4_file file;
    bool extended;
    if (ext4_get_sblock("/", &superblock) != EOK ||
        ext4_fopen2(&file, "/boaros-probe", readonly ? O_RDONLY : O_RDWR) != EOK ||
        ext4_fraw_inode_fill(&file, &before) != EOK)
        return report_error("timestamp fixture", EIO);
    extended = ext4_get16(superblock, inode_size) >= 256;
    if (ext4_mount_setup_clock("/", controlled_clock) != EOK)
        return report_error("timestamp clock", EIO);
    clock_available = false;
    if (ext4_file_touch(&file, EXT4_TIME_ATIME | EXT4_TIME_RELATIME) != EOK ||
        ext4_fraw_inode_fill(&file, &after) != EOK ||
        memcmp(&before, &after, sizeof(before)))
        return report_error("uninitialized clock changed inode", EIO);
    clock_available = true;
    controlled_time = (struct ext4_timestamp){2200000000, 123456789};
    if (readonly) {
        if (ext4_file_touch(&file, EXT4_TIME_ATIME | EXT4_TIME_RELATIME) != EOK ||
            ext4_file_touch(&file, EXT4_TIME_MTIME | EXT4_TIME_CTIME) != EROFS ||
            ext4_fraw_inode_fill(&file, &after) != EOK ||
            memcmp(&before, &after, sizeof(before)))
            return report_error("readonly timestamp mutation", EIO);
    } else {
        for (size_t n = 0; n < sizeof(seconds) / sizeof(seconds[0]); n++) {
            int64_t maximum = extended ? INT64_C(15032385535) : INT32_MAX;
            int64_t expected = seconds[n];
            uint32_t nanoseconds = extended ? 123456789U : 0U;
            controlled_time = (struct ext4_timestamp){seconds[n], 123456789};
            if (expected < INT32_MIN) { expected = INT32_MIN; nanoseconds = 0; }
            if (expected > maximum) {
                expected = maximum;
                nanoseconds = extended ? 999999999U : 0U;
            }
            if (ext4_file_touch(&file, 7U) != EOK ||
                ext4_fraw_inode_fill(&file, &after) != EOK ||
                disk_seconds(after.access_time, after.atime_extra, extended) != expected ||
                disk_seconds(after.modification_time, after.mtime_extra, extended) != expected ||
                disk_seconds(after.change_inode_time, after.ctime_extra, extended) != expected ||
                (extended && (to_le32(after.mtime_extra) >> 2) != nanoseconds))
                return report_error("signed timestamp encoding", EIO);
        }
        controlled_time = (struct ext4_timestamp){1700000000, 17};
        if (ext4_file_touch(&file, 7U) != EOK) return 1;
        controlled_time.seconds++;
        if (ext4_file_touch(&file, EXT4_TIME_ATIME | EXT4_TIME_RELATIME) != EOK ||
            ext4_fraw_inode_fill(&file, &before) != EOK ||
            disk_seconds(before.access_time, before.atime_extra, extended) != controlled_time.seconds)
            return report_error("relatime mtime equality", EIO);
        controlled_time.seconds++;
        if (ext4_file_touch(&file, EXT4_TIME_ATIME | EXT4_TIME_RELATIME) != EOK ||
            ext4_fraw_inode_fill(&file, &after) != EOK ||
            before.access_time != after.access_time || before.atime_extra != after.atime_extra)
            return report_error("relatime skip", EIO);
        controlled_time.seconds += 86399;
        if (ext4_file_touch(&file, EXT4_TIME_ATIME | EXT4_TIME_RELATIME) != EOK ||
            ext4_fraw_inode_fill(&file, &after) != EOK ||
            disk_seconds(after.access_time, after.atime_extra, extended) != controlled_time.seconds)
            return report_error("relatime 24-hour update", EIO);
    }
    if (ext4_fclose(&file) != EOK || ext4_mount_setup_clock("/", NULL) != EOK)
        return report_error("timestamp close", EIO);
    return 0;
}
