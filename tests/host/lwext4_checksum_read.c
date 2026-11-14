/* A second public query overlaps the first checksum calculation. */
#define main metadata_main
#include "lwext4_metadata.c"
#undef main
#include <ext4_dir.h>
#include <stddef.h>

static ext4_file *second_reader;
static unsigned char *observed, original[4096];
static size_t observed_size;
static uint32_t observed_seed, observed_index;
static bool armed, changed;
static uint32_t directory_reader, nested_inode, nested_mode;
static int nested_status;
static unsigned crc_calls;
static struct ext4_fs *qualify_fs;
static uint32_t qualify_inode;
static struct ext4_inode_ref qualified_writer;
static const void *qualify_bytes;
uint32_t __real_ext4_crc32c_zeroed(uint32_t, const void *, uint32_t, uint32_t, uint32_t);
uint32_t __wrap_ext4_crc32c_zeroed(uint32_t seed, const void *bytes, uint32_t length,
    uint32_t first, uint32_t second)
{
    uint32_t crc = __real_ext4_crc32c_zeroed(seed, bytes, length, first, second);
    if (bytes == qualify_bytes) {
        qualify_bytes = NULL;
        /* The old verifier has computed its checksum but has not published
         * success. Expose write access before it can publish that result. */
        CHECK(ext4_fs_get_inode_ref(qualify_fs, qualify_inode, &qualified_writer) == EOK);
    }
    return crc;
}
uint32_t __real_ext4_crc32c(uint32_t, const void *, uint32_t);
uint32_t __wrap_ext4_crc32c(uint32_t seed, const void *bytes, uint32_t length)
{
    crc_calls++;
    if (armed && (directory_reader ? bytes == observed :
        length == sizeof(observed_index) && seed == observed_seed &&
        !memcmp(bytes, &observed_index, sizeof(observed_index)))) {
        armed = false;
        changed = memcmp(observed, original, observed_size) != 0;
        if (directory_reader)
            nested_status = ext4_lookup_child("/", directory_reader, "target", 6,
                &nested_inode, &nested_mode);
        else {
            struct ext4_inode inode;
            nested_status = ext4_fraw_inode_fill(second_reader, &inode);
        }
    }
    return __real_ext4_crc32c(seed, bytes, length);
}

static void readonly_directory_queries(struct ext4_fs *fs, struct ext4_blockdev *dev)
{
    uint32_t ino, mode;
    CHECK(ext4_lookup_child("/", EXT4_INODE_ROOT_INDEX, "file", 4, &ino, &mode) == EOK);
    CHECK(ext4_transaction_begin("/") == EOK);
    payload_threshold = dev->lg_bsize;
    payload_allocations = 0;
    for (unsigned i = 0; i < 8; i++)
        CHECK(ext4_lookup_child("/", EXT4_INODE_ROOT_INDEX, "file", 4, &ino, &mode) == EOK);
    printf("directory read: block=%u lookup-payload-allocations=%u\n",
        dev->lg_bsize, payload_allocations);
    CHECK(!payload_allocations);
    struct ext4_inode_ref directory;
    CHECK(ext4_fs_get_inode_ref_read(fs, EXT4_INODE_ROOT_INDEX, &directory) == EOK);
    payload_allocations = 0;
    struct ext4_dir_iter it;
    CHECK(ext4_dir_iterator_init(&it, &directory, 0) == EOK);
    while (it.curr) CHECK(ext4_dir_iterator_next(&it) == EOK);
    CHECK(ext4_dir_iterator_fini(&it) == EOK);
    CHECK(ext4_fs_put_inode_ref(&directory) == EOK);
    printf("directory read: block=%u iterator-payload-allocations=%u\n",
        dev->lg_bsize, payload_allocations);
    CHECK(!payload_allocations);
    payload_threshold = 0;
    ext4_file child;
    CHECK(ext4_fopen(&child, "/directory-private", "w+") == EOK);
    CHECK(ext4_lookup_child("/", EXT4_INODE_ROOT_INDEX, "directory-private", 17,
        &ino, &mode) == EOK && ino == child.inode);
    CHECK(ext4_fclose(&child) == EOK);
    CHECK(ext4_transaction_abort("/", ECANCELED) == ECANCELED);
    CHECK(ext4_lookup_child("/", EXT4_INODE_ROOT_INDEX, "directory-private", 17,
        &ino, &mode) == ENOENT);
}

static void reuse_queries(struct ext4_fs *fs, struct ext4_blockdev *dev)
{
    ext4_file file;
    uint32_t ino, mode;
    CHECK(ext4_lookup_child("/", EXT4_INODE_ROOT_INDEX, "file", 4, &ino, &mode) == EOK);
    CHECK(ext4_fopen_inode(&file, "/", ino, NULL) == EOK);
    ext4_bcache_reclaim(dev->bc);
    struct ext4_inode before, after;
    CHECK(ext4_fraw_inode_fill(&file, &before) == EOK);
    unsigned warmed = crc_calls;
    for (unsigned i = 0; i < 8; i++) {
        CHECK(ext4_fraw_inode_fill(&file, &after) == EOK);
        CHECK(!memcmp(&before, &after, sizeof(before)));
    }
    printf("checksum reuse: block=%u inode=%u warm-query-crcs=%u\n",
        dev->lg_bsize, ext4_get16(&fs->sb, inode_size), crc_calls - warmed);
    CHECK(crc_calls == warmed);

    struct ext4_inode_ref old;
    CHECK(ext4_fs_get_inode_ref_read(fs, ino, &old) == EOK);
    unsigned char old_bytes[512];
    uint32_t isize = ext4_get16(&fs->sb, inode_size);
    CHECK(isize <= sizeof(old_bytes));
    memcpy(old_bytes, old.inode, isize);
    struct ext4_timestamp times[3] = {{1234, 0}, {2345, 0}, {3456, 0}};
    CHECK(ext4_file_set_times(&file, 7, times) == EOK);
    CHECK(!memcmp(old_bytes, old.inode, isize));
    warmed = crc_calls;
    CHECK(ext4_fraw_inode_fill(&file, &after) == EOK);
    CHECK(crc_calls > warmed && to_le32(after.modification_time) == 2345);
    warmed = crc_calls;
    CHECK(ext4_fraw_inode_fill(&file, &before) == EOK && crc_calls == warmed);
    CHECK(ext4_fs_put_inode_ref(&old) == EOK);

    CHECK(ext4_transaction_begin("/") == EOK);
    times[1].seconds = 4567;
    CHECK(ext4_file_set_times(&file, 2, times) == EOK);
    CHECK(ext4_fraw_inode_fill(&file, &after) == EOK && to_le32(after.modification_time) == 4567);
    warmed = crc_calls;
    CHECK(ext4_fraw_inode_fill(&file, &after) == EOK && crc_calls > warmed);
    CHECK(ext4_transaction_abort("/", ECANCELED) == ECANCELED);
    CHECK(ext4_fraw_inode_fill(&file, &after) == EOK && !memcmp(&before, &after, sizeof(after)));

    /* A new read version starts uncached. Force an overlapping writer after
     * this version's checksum, then ensure no late success was remembered. */
    CHECK(ext4_file_set_times(&file, 2, times) == EOK);
    CHECK(ext4_fs_get_inode_ref_read(fs, ino, &old) == EOK);
    struct ext4_timestamp newer[3] = {{0}, {5678, 0}, {0}};
    CHECK(ext4_file_set_times(&file, 2, newer) == EOK);
    /* The address is obtained from the existing accepted owner, without
     * invoking another metadata verifier before the instrumented query. */
    struct ext4_block current_block;
    CHECK(ext4_block_get(dev, &current_block, old.block.lb_id) == EOK);
    qualify_bytes = current_block.data + ((uint8_t *)old.inode - old.block.data);
    qualify_fs = fs; qualify_inode = ino;
    CHECK(ext4_fraw_inode_fill(&file, &after) == EOK && !qualify_bytes);
    CHECK(ext4_fs_put_inode_ref(&old) == EOK && ext4_block_set(dev, &current_block) == EOK);
    warmed = crc_calls;
    CHECK(ext4_fraw_inode_fill(&file, &after) == EOK && crc_calls > warmed);
    uint16_t mode_before = qualified_writer.inode->mode;
    qualified_writer.inode->mode ^= 1;
    CHECK(ext4_fraw_inode_fill(&file, &after) == EUCLEAN);
    qualified_writer.inode->mode = mode_before;
    CHECK(ext4_fs_put_inode_ref(&qualified_writer) == EOK);
    CHECK(ext4_fraw_inode_fill(&file, &after) == EOK && to_le32(after.modification_time) == 5678);

    uint32_t bg = (ino - 1) / ext4_get32(&fs->sb, inodes_per_group);
    struct ext4_block_group_ref group;
    CHECK(ext4_fs_get_block_group_ref_write(fs, bg, &group) == EOK);
    uint16_t checksum = group.block_group->checksum;
    group.block_group->checksum ^= 1;
    CHECK(ext4_fraw_inode_fill(&file, &after) == EUCLEAN);
    group.block_group->checksum = checksum;
    CHECK(ext4_fs_put_block_group_ref(&group) == EOK);
    CHECK(ext4_fraw_inode_fill(&file, &after) == EOK);
    printf("checksum versions: accepted/abort, old reader, late verifier, inode/GDT corruption rejected\n");
    CHECK(ext4_fclose(&file) == EOK);
}

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    if (!strcmp(argv[2], "seed")) return metadata_main(argc, argv);
    bool gdt = !strcmp(argv[2], "gdt");
    bool dx = !strcmp(argv[2], "dx");
    bool reuse = !strcmp(argv[2], "reuse");
    bool read_directory = !strcmp(argv[2], "directory-read");
    CHECK(gdt || dx || reuse || read_directory || !strcmp(argv[2], "inode"));
    CHECK(fault_block_open(&disk, argv[1]) == 0);
    unsigned char scratch[512];
    struct ext4_blockdev_iface iface = {.open=dev_open, .close=dev_open,
        .bread=dev_read, .bwrite=dev_write, .flush=dev_flush, .ph_bsize=512,
        .ph_bcnt=disk.device.capacity_bytes/512, .ph_bbuf=scratch};
    struct ext4_blockdev dev = {.bdif=&iface, .part_size=disk.device.capacity_bytes};
    CHECK(ext4_device_register(&dev, "metadata") == EOK);
    CHECK(ext4_mount("metadata", "/", false) == EOK);
    CHECK(ext4_recover("/") == EOK && ext4_journal_start("/") == EOK);
    struct ext4_fs *fs = dev.fs;
    CHECK(ext4_sb_feature_ro_com(&fs->sb, EXT4_FRO_COM_METADATA_CSUM));
    if (reuse || read_directory) {
        if (reuse) reuse_queries(fs, &dev);
        else readonly_directory_queries(fs, &dev);
        CHECK(ext4_journal_stop("/") == EOK);
        CHECK(ext4_umount("/") == EOK && !live_allocations);
        fault_block_close(&disk);
        return 0;
    }
    if (dx) {
        CHECK(ext4_dir_mk("/checksum-directory") == EOK);
        ext4_file child;
        CHECK(ext4_fopen(&child, "/checksum-directory/target", "w+") == EOK);
        ext4_dir directory;
        CHECK(ext4_dir_open(&directory, "/checksum-directory") == EOK);
        directory_reader = directory.f.inode;
        struct ext4_inode_ref ref;
        CHECK(ext4_fs_get_inode_ref(fs, directory_reader, &ref) == EOK);
        CHECK(ext4_inode_get_flags(ref.inode) & EXT4_INODE_FLAG_INDEX);
        ext4_fsblk_t address;
        CHECK(ext4_fs_get_inode_dblk_idx(&ref, 0, &address, false) == EOK);
        struct ext4_block index;
        CHECK(ext4_block_get(&dev, &index, address) == EOK);
        observed = index.data; observed_size = dev.lg_bsize;
        CHECK(observed_size <= sizeof(original));
        memcpy(original, observed, observed_size);
        struct ext4_dir_idx_climit *limits = (void *)((struct ext4_dir_idx_root *)observed)->en;
        size_t tail = 32 + (size_t)to_le16(limits->limit) * sizeof(struct ext4_dir_idx_entry);
        CHECK(tail + sizeof(struct ext4_dir_idx_tail) <= observed_size);
        CHECK(((struct ext4_dir_idx_tail *)(observed + tail))->checksum);
        uint32_t outer_inode = 0, outer_mode = 0;
        armed = true;
        int outer = ext4_lookup_child("/", directory_reader, "target", 6,
            &outer_inode, &outer_mode);
        printf("checksum read dx: block=%u inode=%u outer=%d second=%d modified=%d\n",
            dev.lg_bsize, ext4_get16(&fs->sb, inode_size), outer, nested_status, changed);
        CHECK(!armed && outer == EOK && nested_status == EOK && !changed);
        CHECK(outer_inode == child.inode && nested_inode == child.inode &&
              outer_mode == nested_mode && (outer_mode & EXT4_INODE_MODE_TYPE_MASK) == EXT4_INODE_MODE_FILE);
        CHECK(!memcmp(original, observed, observed_size));
        /* The immutable verification must still reject an actual bad tail. */
        observed[tail + offsetof(struct ext4_dir_idx_tail, checksum)] ^= 1;
        int corrupt = ext4_lookup_child("/", directory_reader, "target", 6,
            &outer_inode, &outer_mode);
        memcpy(observed, original, observed_size);
        printf("checksum read dx corruption: result=%d\n", corrupt);
        CHECK(corrupt == EUCLEAN);
        CHECK(ext4_lookup_child("/", directory_reader, "target", 6,
            &outer_inode, &outer_mode) == EOK && outer_inode == child.inode);
        directory_reader = 0;
        CHECK(ext4_block_set(&dev, &index) == EOK);
        CHECK(ext4_fs_put_inode_ref(&ref) == EOK);
        CHECK(ext4_dir_close(&directory) == EOK && ext4_fclose(&child) == EOK);
        CHECK(ext4_journal_stop("/") == EOK);
        CHECK(ext4_umount("/") == EOK && !live_allocations);
        fault_block_close(&disk);
        return 0;
    }
    for (unsigned i = 0; i < 32; i++) {
        char name[32]; ext4_file padding;
        snprintf(name, sizeof(name), "/checksum-pad-%u", i);
        CHECK(ext4_fopen(&padding, name, "w+") == EOK && ext4_fclose(&padding) == EOK);
    }
    ext4_file f;
    CHECK(ext4_fopen(&f, "/checksum-target", "w+") == EOK);
    struct ext4_block_group_ref group;
    uint32_t bg = (f.inode - 1) / ext4_get32(&fs->sb, inodes_per_group);
    /* An outstanding writable loan requires real verification on every
     * query, including the intentionally overlapping checksum below. */
    CHECK(ext4_fs_get_block_group_ref_write(fs, bg, &group) == EOK);
    struct ext4_inode_ref inode;
    CHECK(ext4_fs_get_inode_ref(fs, f.inode, &inode) == EOK);
    CHECK(ext4_current_trans(fs) == NULL);
    observed = gdt ? (void *)group.block_group : (void *)inode.inode;
    observed_size = gdt ? ext4_sb_get_desc_size(&fs->sb) : ext4_get16(&fs->sb, inode_size);
    CHECK(observed_size <= sizeof(original));
    memcpy(original, observed, observed_size);
    observed_seed = ext4_sb_get_csum_seed(&fs->sb);
    observed_index = to_le32(gdt ? bg : f.inode);
    second_reader = &f;
    armed = true;
    struct ext4_inode actual;
    memset(&actual, 0xa5, sizeof(actual));
    int outer = ext4_fraw_inode_fill(&f, &actual);
    printf("checksum read %s: block=%u inode=%u outer=%d second=%d modified=%d\n",
        argv[2], dev.lg_bsize, ext4_get16(&fs->sb, inode_size), outer, nested_status, changed);
    CHECK(!armed && outer == EOK && nested_status == EOK && !changed);
    if (!gdt) CHECK(!memcmp(&actual, original, observed_size < sizeof(actual) ? observed_size : sizeof(actual)));
    size_t inode_size=ext4_get16(&fs->sb, inode_size);
    for (size_t i=inode_size;i<sizeof(actual);i++) CHECK(!((unsigned char *)&actual)[i]);
    CHECK(!memcmp(original, observed, observed_size));
    CHECK(ext4_fs_put_inode_ref(&inode) == EOK);
    CHECK(ext4_fs_put_block_group_ref(&group) == EOK);
    CHECK(ext4_fclose(&f) == EOK);
    CHECK(ext4_journal_stop("/") == EOK);
    CHECK(ext4_umount("/") == EOK && !live_allocations);
    fault_block_close(&disk);
    return 0;
}
