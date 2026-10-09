/* A second public inode query observes the first checksum calculation. */
#define main metadata_main
#include "lwext4_metadata.c"
#undef main

static ext4_file *second_reader;
static unsigned char *observed, original[4096];
static size_t observed_size;
static uint32_t observed_seed, observed_index;
static bool armed, changed;
static int nested_status;
uint32_t __real_ext4_crc32c(uint32_t, const void *, uint32_t);
uint32_t __wrap_ext4_crc32c(uint32_t seed, const void *bytes, uint32_t length)
{
    if (armed && length == sizeof(observed_index) && seed == observed_seed &&
        !memcmp(bytes, &observed_index, sizeof(observed_index))) {
        armed = false;
        changed = memcmp(observed, original, observed_size) != 0;
        struct ext4_inode inode;
        nested_status = ext4_fraw_inode_fill(second_reader, &inode);
    }
    return __real_ext4_crc32c(seed, bytes, length);
}

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    if (!strcmp(argv[2], "seed")) return metadata_main(argc, argv);
    bool gdt = !strcmp(argv[2], "gdt");
    CHECK(gdt || !strcmp(argv[2], "inode"));
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
    for (unsigned i = 0; i < 32; i++) {
        char name[32]; ext4_file padding;
        snprintf(name, sizeof(name), "/checksum-pad-%u", i);
        CHECK(ext4_fopen(&padding, name, "w+") == EOK && ext4_fclose(&padding) == EOK);
    }
    ext4_file f;
    CHECK(ext4_fopen(&f, "/checksum-target", "w+") == EOK);
    struct ext4_block_group_ref group;
    uint32_t bg = (f.inode - 1) / ext4_get32(&fs->sb, inodes_per_group);
    CHECK(ext4_fs_get_block_group_ref(fs, bg, &group) == EOK);
    struct ext4_inode_ref inode;
    CHECK(ext4_fs_get_inode_ref(fs, f.inode, &inode) == EOK);
    CHECK(fs->curr_trans == NULL);
    observed = gdt ? (void *)group.block_group : (void *)inode.inode;
    observed_size = gdt ? ext4_sb_get_desc_size(&fs->sb) : ext4_get16(&fs->sb, inode_size);
    CHECK(observed_size <= sizeof(original));
    memcpy(original, observed, observed_size);
    observed_seed = ext4_sb_get_csum_seed(&fs->sb);
    observed_index = to_le32(gdt ? bg : f.inode);
    second_reader = &f;
    armed = true;
    struct ext4_inode actual;
    int outer = ext4_fraw_inode_fill(&f, &actual);
    printf("checksum read %s: block=%u inode=%u outer=%d second=%d modified=%d\n",
        argv[2], dev.lg_bsize, ext4_get16(&fs->sb, inode_size), outer, nested_status, changed);
    CHECK(!armed && outer == EOK && nested_status == EOK && !changed);
    CHECK(!memcmp(original, observed, observed_size));
    CHECK(ext4_fs_put_inode_ref(&inode) == EOK);
    CHECK(ext4_fs_put_block_group_ref(&group) == EOK);
    CHECK(ext4_fclose(&f) == EOK);
    CHECK(ext4_journal_stop("/") == EOK);
    CHECK(ext4_umount("/") == EOK && !live_allocations);
    fault_block_close(&disk);
    return 0;
}
