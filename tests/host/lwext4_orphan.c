/* Real orphan records, real JBD transactions, volatile device power cuts. */
#include "block_fault.h"
#include <ext4.h>
#include <ext4_blockdev.h>
#include <ext4_block_group.h>
#include <ext4_crc32.h>
#include <ext4_errno.h>
#include <ext4_inode.h>
#include <ext4_fs.h>
#include <ext4_journal.h>
#include <ext4_orphan.h>
#include <ext4_super.h>
#include <ext4_trans.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static struct fault_block disk;
static int dev_open(struct ext4_blockdev *b) { (void)b; return EOK; }
static int dev_read(struct ext4_blockdev *b, void *p, uint64_t n, uint32_t c)
{ (void)b; return kernel_block_read_at(&disk.device, n * 512, p, (size_t)c * 512) == KERNEL_BLOCK_STATUS_OK ? EOK : EIO; }
static int dev_write(struct ext4_blockdev *b, const void *p, uint64_t n, uint32_t c)
{ (void)b; return kernel_block_write_at(&disk.device, n * 512, p, (size_t)c * 512) == KERNEL_BLOCK_STATUS_OK ? EOK : EIO; }
static int dev_flush(struct ext4_blockdev *b)
{ (void)b; return kernel_block_flush(&disk.device) == KERNEL_BLOCK_STATUS_OK ? EOK : EIO; }

static void begin(struct ext4_fs *fs, struct jbd_journal *journal)
{
    fs->jbd_journal = journal;
    fs->curr_trans = jbd_journal_new_trans(journal);
    CHECK(fs->curr_trans != NULL);
}

/* Exercise the integration contract: include the in-memory superblock in this
 * transaction without writing its home location ahead of commit. */
static int commit(struct ext4_fs *fs)
{
    struct ext4_block block;
    uint32_t size = fs->bdev->lg_bsize;
    if (ext4_sb_feature_ro_com(&fs->sb, EXT4_FRO_COM_METADATA_CSUM))
        fs->sb.checksum = to_le32(ext4_crc32c(EXT4_CRC32_INIT, &fs->sb,
            offsetof(struct ext4_sblock, checksum)));
    CHECK(ext4_block_get(fs->bdev, &block, EXT4_SUPERBLOCK_OFFSET / size) == EOK);
    CHECK(ext4_trans_set_block_dirty(block.buf) == EOK);
    memcpy(block.data + EXT4_SUPERBLOCK_OFFSET % size, &fs->sb, sizeof(fs->sb));
    CHECK(ext4_block_set(fs->bdev, &block) == EOK);
    struct jbd_trans *tx = fs->curr_trans;
    fs->curr_trans = NULL;
    return jbd_journal_commit_trans(fs->jbd_journal, tx);
}

static uint32_t fixture_seed(struct ext4_inode_ref *ref)
{
    uint32_t ino = to_le32(ref->index), gen = ref->inode->generation;
    uint32_t sum = ext4_crc32c(ext4_sb_get_csum_seed(&ref->fs->sb), &ino, sizeof(ino));
    return ext4_crc32c(sum, &gen, sizeof(gen));
}

static void damage_records(struct ext4_fs *fs, const char *kind)
{
    struct ext4_inode_ref ref;
    if (!strncmp(kind, "file-", 5)) {
        CHECK(ext4_fs_get_inode_ref(fs, ext4_get32(&fs->sb, orphan_file_inum), &ref) == EOK);
        ext4_fsblk_t lba;
        CHECK(ext4_fs_get_inode_dblk_idx(&ref, 0, &lba, false) == EOK);
        uint32_t size = fs->bdev->lg_bsize;
        unsigned char *data = malloc(size);
        CHECK(data != NULL);
        CHECK(ext4_blocks_get_direct(fs->bdev, data, lba, 1) == EOK);
        uint32_t *entries = (void *)data;
        uint32_t *tail = (void *)(data + size - 8);
        if (!strcmp(kind, "file-magic")) tail[0] ^= 1;
        else if (!strcmp(kind, "file-checksum")) tail[1] ^= 1;
        else {
            if (!strcmp(kind, "file-range")) entries[0] = to_le32(ext4_get32(&fs->sb, inodes_count) + 1);
            else if (!strcmp(kind, "file-free")) entries[0] = fs->sb.inodes_count;
            else if (!strcmp(kind, "file-duplicate")) { CHECK(entries[0] != 0); entries[1] = entries[0]; }
            else CHECK(!"unknown file damage");
            uint64_t physical = to_le64(lba);
            uint32_t sum = ext4_crc32c(fixture_seed(&ref), &physical, sizeof(physical));
            tail[1] = to_le32(ext4_crc32c(sum, data, size - 8));
        }
        CHECK(ext4_blocks_set_direct(fs->bdev, data, lba, 1) == EOK);
        CHECK(ext4_fs_put_inode_ref(&ref) == EOK);
        free(data);
    } else if (!strncmp(kind, "bitmap-", 7)) {
        uint32_t ino;
        CHECK(ext4_orphan_peek(fs, &ino) == EOK && ino != 0);
        struct ext4_block_group_ref group;
        CHECK(ext4_fs_get_block_group_ref(fs,
            (ino - 1) / ext4_get32(&fs->sb, inodes_per_group), &group) == EOK);
        if (!strcmp(kind, "bitmap-uninit")) {
            group.block_group->flags |= to_le16(EXT4_BLOCK_GROUP_INODE_UNINIT);
            group.dirty = true;
        } else {
            ext4_fsblk_t lba = ext4_bg_get_inode_bitmap(group.block_group, &fs->sb);
            unsigned char *data = malloc(fs->bdev->lg_bsize);
            CHECK(data != NULL);
            CHECK(ext4_blocks_get_direct(fs->bdev, data, lba, 1) == EOK);
            /* Keep the orphan's own bit set; corruption of a different bit
             * must still prevent trusting this allocation bitmap. */
            data[100] ^= 1;
            CHECK(ext4_blocks_set_direct(fs->bdev, data, lba, 1) == EOK);
            free(data);
        }
        CHECK(ext4_fs_put_block_group_ref(&group) == EOK);
    } else if (!strcmp(kind, "mixed-duplicate")) {
        uint32_t ino;
        CHECK(ext4_orphan_peek(fs, &ino) == EOK && ino != 0);
        ext4_set32(&fs->sb, last_orphan, ino);
        CHECK(ext4_sb_write(fs->bdev, &fs->sb) == EOK);
    } else if (!strcmp(kind, "chain-free")) {
        fs->sb.last_orphan = fs->sb.inodes_count;
        CHECK(ext4_sb_write(fs->bdev, &fs->sb) == EOK);
    } else {
        uint32_t ino = ext4_get32(&fs->sb, last_orphan);
        CHECK(ino != 0);
        CHECK(ext4_fs_get_inode_ref(fs, ino, &ref) == EOK);
        ext4_inode_set_del_time(ref.inode, !strcmp(kind, "chain-cycle") ? ino :
            ext4_get32(&fs->sb, inodes_count) + 1);
        ext4_inode_set_csum(&fs->sb, ref.inode, 0);
        uint32_t sum = ext4_crc32c(fixture_seed(&ref), ref.inode,
            ext4_get16(&fs->sb, inode_size));
        ext4_inode_set_csum(&fs->sb, ref.inode, sum);
        CHECK(ext4_blocks_set_direct(fs->bdev, ref.block.data, ref.block.lb_id, 1) == EOK);
        CHECK(ext4_fs_put_inode_ref(&ref) == EOK);
    }
    CHECK(dev_flush(fs->bdev) == EOK);
}

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    CHECK(fault_block_open(&disk, argv[1]) == 0);
    unsigned char scratch[512];
    struct ext4_blockdev_iface iface = {
        .open = dev_open, .close = dev_open, .bread = dev_read,
        .bwrite = dev_write, .flush = dev_flush, .ph_bsize = 512,
        .ph_bcnt = disk.device.capacity_bytes / 512, .ph_bbuf = scratch,
    };
    struct ext4_blockdev dev = { .bdif = &iface, .part_size = disk.device.capacity_bytes };
    CHECK(ext4_device_register(&dev, "orphan-test") == EOK);
    CHECK(ext4_mount("orphan-test", "/", !strcmp(argv[2], "readonly")) == EOK);
    struct ext4_fs *fs = dev.fs;
    struct jbd_fs jfs;
    struct jbd_journal journal;
    CHECK(jbd_get_fs(fs, &jfs) == EOK);
    if (!strcmp(argv[2], "readonly")) {
        ext4_file file;
        struct ext4_inode_ref inode;
        CHECK(ext4_fopen(&file, "/a", "r") == EOK);
        CHECK(ext4_fs_get_inode_ref(fs, file.inode, &inode) == EOK);
        CHECK(ext4_orphan_add(&inode) == EROFS);
        CHECK(ext4_orphan_remove(&inode) == EROFS);
        CHECK(ext4_orphan_set_present(fs, true) == EROFS);
        CHECK(ext4_orphan_validate(fs) == EOK);
        return 0;
    }
    CHECK(jbd_recover(&jfs) == EOK);
    if (!strncmp(argv[2], "damage-", 7)) {
        damage_records(fs, argv[2] + 7);
        return 0;
    }
    if (!strncmp(argv[2], "reject-", 7)) {
        struct ext4_sblock original = fs->sb;
        uint64_t writes = disk.writes;
        int expected = atoi(argv[2] + 7);
        uint32_t untouched = 12345;
        CHECK(ext4_orphan_validate(fs) == expected);
        CHECK(ext4_orphan_peek(fs, &untouched) == expected);
        CHECK(untouched == 12345);
        CHECK(memcmp(&original, &fs->sb, sizeof(original)) == 0);
        CHECK(disk.writes == writes);
        return 0;
    }
    CHECK(ext4_orphan_validate(fs) == EOK);
    {
        ext4_file file;
        struct ext4_inode_ref ref;
        CHECK(ext4_fopen(&file, "/a", "r") == EOK);
        CHECK(ext4_fs_get_inode_ref(fs, file.inode, &ref) == EOK);
        CHECK(ext4_orphan_add(&ref) == EINVAL);
        CHECK(ext4_orphan_remove(&ref) == EINVAL);
        CHECK(ext4_orphan_set_present(fs, true) == EINVAL);
        CHECK(ext4_fs_put_inode_ref(&ref) == EOK);
        CHECK(ext4_fclose(&file) == EOK);
    }
    CHECK(jbd_journal_start(&jfs, &journal) == EOK);
    CHECK(ext4_block_cache_write_back(&dev, 1) == EOK);
    if (!strcmp(argv[2], "fill")) {
        struct ext4_inode_ref table;
        CHECK(ext4_fs_get_inode_ref(fs, ext4_get32(&fs->sb, orphan_file_inum), &table) == EOK);
        uint64_t blocks = ext4_inode_get_size(&fs->sb, table.inode) / dev.lg_bsize;
        unsigned capacity = blocks * (dev.lg_bsize - 8) / 4;
        CHECK(blocks == 1); /* Runner builds a small valid preallocated table. */
        CHECK(ext4_fs_put_inode_ref(&table) == EOK);
        uint32_t last = 0;
        for (unsigned i = 0; i <= capacity; i++) {
            char name[40];
            snprintf(name, sizeof(name), "/fill-%u", i);
            ext4_file file;
            struct ext4_inode_ref ref;
            CHECK(ext4_fopen(&file, name, "r+") == EOK);
            CHECK(ext4_fs_get_inode_ref(fs, file.inode, &ref) == EOK);
            begin(fs, &journal);
            CHECK(ext4_orphan_add(&ref) == EOK);
            last = ref.index;
            CHECK(ext4_fs_put_inode_ref(&ref) == EOK);
            CHECK(commit(fs) == EOK);
            CHECK(ext4_fclose(&file) == EOK);
        }
        CHECK(ext4_get32(&fs->sb, last_orphan) == last);
        CHECK(ext4_orphan_validate(fs) == EOK);
    } else if (!strncmp(argv[2], "add", 3)) {
        ext4_file files[3];
        struct ext4_inode_ref refs[3];
        const char *names[] = {"/a", "/b", "/c"};
        for (unsigned i = 0; i < 3; i++) {
            CHECK(ext4_fopen(&files[i], names[i], "r+") == EOK);
            CHECK(ext4_fs_get_inode_ref(fs, files[i].inode, &refs[i]) == EOK);
        }
        begin(fs, &journal);
        CHECK(ext4_orphan_set_present(fs, true) == EOK);
        for (unsigned i = 0; i < 3; i++) CHECK(ext4_orphan_add(&refs[i]) == EOK);
        CHECK(ext4_orphan_add(&refs[0]) == EOK);
        CHECK(ext4_orphan_set_present(fs, false) == ENOTEMPTY);
        CHECK(ext4_orphan_remove(&refs[1]) == EOK);
        CHECK(ext4_orphan_remove(&refs[1]) == EOK);
        for (unsigned i = 0; i < 3; i++) CHECK(ext4_fs_put_inode_ref(&refs[i]) == EOK);
        if (!strcmp(argv[2], "add-error")) {
            disk.fail_flush = disk.flushes + 1;
            CHECK(commit(fs) == EIO);
        } else CHECK(commit(fs) == EOK);
    } else {
        uint32_t ino, repeat;
        CHECK(ext4_orphan_peek(fs, &ino) == EOK);
        CHECK(ext4_orphan_peek(fs, &repeat) == EOK && repeat == ino);
        if (!strcmp(argv[2], "drain")) {
            while (ino) {
                struct ext4_inode_ref ref;
                CHECK(ext4_fs_get_inode_ref(fs, ino, &ref) == EOK);
                begin(fs, &journal);
                CHECK(ext4_orphan_remove(&ref) == EOK);
                CHECK(ext4_fs_put_inode_ref(&ref) == EOK);
                CHECK(commit(fs) == EOK);
                CHECK(ext4_orphan_peek(fs, &ino) == EOK);
            }
        }
        if (!strcmp(argv[2], "remove-one") || !strcmp(argv[2], "remove-error")) {
            CHECK(ino != 0);
            struct ext4_inode_ref ref;
            CHECK(ext4_fs_get_inode_ref(fs, ino, &ref) == EOK);
            begin(fs, &journal);
            CHECK(ext4_orphan_remove(&ref) == EOK);
            CHECK(ext4_orphan_remove(&ref) == EOK);
            CHECK(ext4_fs_put_inode_ref(&ref) == EOK);
            if (!strcmp(argv[2], "remove-error")) {
                disk.fail_flush = disk.flushes + 1;
                CHECK(commit(fs) == EIO);
            } else CHECK(commit(fs) == EOK);
        } else {
            CHECK((!strcmp(argv[2], "finish") || !strcmp(argv[2], "drain")) && ino == 0);
            begin(fs, &journal);
            CHECK(ext4_orphan_set_present(fs, false) == EOK);
            CHECK(commit(fs) == EOK);
            CHECK(jbd_journal_stop(&journal) == EOK);
            CHECK(jbd_put_fs(&jfs) == EOK);
            fs->jbd_journal = NULL;
            CHECK(ext4_umount("/") == EOK);
            CHECK(dev_flush(&dev) == EOK);
            return 0;
        }
    }
    CHECK(fault_block_crash(&disk) == 0);
    return 0;
}
