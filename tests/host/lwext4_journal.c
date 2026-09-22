/* Real lwext4/JBD writes over a device with volatile sector writes. */
#include "block_fault.h"
#include <ext4.h>
#include <ext4_blockdev.h>
#include <ext4_fs.h>
#include <ext4_inode.h>
#include <ext4_journal.h>
#include <ext4_errno.h>
#include <ext4_misc.h>
#include <ext4_crc32.h>
#include <ext4_trans.h>
#include <ext4_super.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static struct fault_block disk;
static uint64_t reads, fail_read;
static int open_device(struct ext4_blockdev *b) { (void)b; return EOK; }
static int read_device(struct ext4_blockdev *b, void *p, uint64_t n, uint32_t c)
{ (void)b; if (++reads == fail_read) return EIO; return kernel_block_read_at(&disk.device, n * 512, p, (size_t)c * 512) == KERNEL_BLOCK_STATUS_OK ? EOK : EIO; }
static int write_device(struct ext4_blockdev *b, const void *p, uint64_t n, uint32_t c)
{ (void)b; return kernel_block_write_at(&disk.device, n * 512, p, (size_t)c * 512) == KERNEL_BLOCK_STATUS_OK ? EOK : EIO; }
static int flush_device(struct ext4_blockdev *b)
{ (void)b; return kernel_block_flush(&disk.device) == KERNEL_BLOCK_STATUS_OK ? EOK : EIO; }

/* Corrupt actual on-disk records independently of the writer/replay code. */
static void mutate_log(struct ext4_blockdev *dev, struct jbd_fs *jfs,
                       const char *kind)
{
    uint32_t sequence = to_be32(jfs->sb.sequence);
    if (!strncmp(kind, "second-", 7)) { sequence++; kind += 7; }
    uint32_t wanted = !strncmp(kind, "revoke", 6) ? JBD_REVOKE_BLOCK :
                      !strncmp(kind, "commit", 6) ? JBD_COMMIT_BLOCK :
                      JBD_DESCRIPTOR_BLOCK;
    unsigned char *data = malloc(dev->lg_bsize);
    CHECK(data != NULL);
    uint32_t position = to_be32(jfs->sb.start);
    for (; position < to_be32(jfs->sb.maxlen); position++) {
        ext4_fsblk_t physical;
        CHECK(jbd_inode_bmap(jfs, position, &physical) == EOK);
        CHECK(ext4_blocks_get_direct(dev, data, physical, 1) == EOK);
        struct jbd_bhdr *hdr = (void *)data;
        if (to_be32(hdr->magic) != JBD_MAGIC_NUMBER ||
            to_be32(hdr->blocktype) != wanted || to_be32(hdr->sequence) != sequence) continue;
        bool repair_checksum = false;
        if (!strcmp(kind, "data-csum")) {
            CHECK(jbd_inode_bmap(jfs, position + 1, &physical) == EOK);
            CHECK(ext4_blocks_get_direct(dev, data, physical, 1) == EOK);
            data[100] ^= 0x80;
        } else if (!strcmp(kind, "commit-zero")) {
            memset(data, 0, dev->lg_bsize);
        } else if (!strcmp(kind, "commit-padding")) {
            data[dev->lg_bsize - 1] ^= 0x80;
        } else if (!strncmp(kind, "commit-time", 11)) {
            struct jbd_commit_header *commit = (void *)data;
            commit->commit_sec = to_be64(!strcmp(kind, "commit-time-high") ? 100 : 50);
            commit->chksum[0] = 0;
            uint32_t sum = ext4_crc32c(EXT4_CRC32_INIT, jfs->sb.uuid, UUID_SIZE);
            commit->chksum[0] = to_be32(ext4_crc32c(sum, data, dev->lg_bsize));
        } else if (!strcmp(kind, "commit-csum")) {
            ((struct jbd_commit_header *)data)->chksum[0] ^= 1;
        } else if (!strncmp(kind, "revoke-count", 12)) {
            uint32_t count = !strcmp(kind, "revoke-count-low") ?
                sizeof(struct jbd_revoke_header) - 1 :
                !strcmp(kind, "revoke-count-high") ? dev->lg_bsize :
                sizeof(struct jbd_revoke_header) + 1;
            ((struct jbd_revoke_header *)data)->count = to_be32(count);
            repair_checksum = true;
        } else if (!strcmp(kind, "descriptor-no-last")) {
            struct jbd_block_tag3 *tag = (void *)(hdr + 1);
            tag->flags = to_be32(to_be32(tag->flags) & ~JBD_FLAG_LAST_TAG);
            unsigned char *next = (void *)(tag + 1);
            next += UUID_SIZE;
            unsigned char *end = data + dev->lg_bsize - sizeof(struct jbd_block_tail);
            while (next + sizeof(*tag) <= end) {
                struct jbd_block_tag3 *extra = (void *)next;
                *extra = *tag;
                extra->flags = to_be32(JBD_FLAG_SAME_UUID);
                next += sizeof(*tag);
            }
            repair_checksum = true;
        } else if (!strcmp(kind, "descriptor-target")) {
            struct jbd_block_tag3 *tag = (void *)(hdr + 1);
            tag->blocknr = to_be32(dev->lg_bcnt);
            repair_checksum = true;
        } else {
            data[dev->lg_bsize - 1] ^= 0x80;
        }
        if (repair_checksum) {
            struct jbd_block_tail *tail = (void *)(data + dev->lg_bsize - sizeof(*tail));
            tail->checksum = 0;
            uint32_t sum = ext4_crc32c(EXT4_CRC32_INIT, jfs->sb.uuid, UUID_SIZE);
            tail->checksum = to_be32(ext4_crc32c(sum, data, dev->lg_bsize));
        }
        CHECK(ext4_blocks_set_direct(dev, data, physical, 1) == EOK);
        CHECK(kernel_block_flush(&disk.device) == KERNEL_BLOCK_STATUS_OK);
        free(data);
        return;
    }
    CHECK(!"journal record to mutate was missing");
}

int main(int argc, char **argv)
{
    CHECK(argc >= 3);
    CHECK(fault_block_open(&disk, argv[1]) == 0);
    unsigned char physical[512];
    struct ext4_blockdev_iface iface = {
        .open = open_device, .close = open_device,
        .bread = read_device, .bwrite = write_device,
        .ph_bsize = 512, .ph_bcnt = disk.device.capacity_bytes / 512,
        .ph_bbuf = physical,
        .flush = flush_device,
    };
    struct ext4_blockdev dev = { .bdif = &iface, .part_size = disk.device.capacity_bytes };
    CHECK(ext4_device_register(&dev, "journal-test") == EOK);
    CHECK(ext4_mount("journal-test", "/", !strcmp(argv[2], "recover-readonly")) == EOK);
    struct jbd_fs jfs;
    struct jbd_journal journal;
    CHECK(jbd_get_fs(dev.fs, &jfs) == EOK);
    if (!strcmp(argv[2], "recover-readonly")) {
        CHECK(jfs.sb.start != 0);
        uint64_t writes = disk.writes;
        CHECK(jbd_recover(&jfs) == EROFS);
        CHECK(disk.writes == writes);
        CHECK(jfs.sb.start != 0);
        return 0;
    }
    if (!strcmp(argv[2], "recover-no-super")) {
        CHECK(dev.fs->super_replay_required);
        uint32_t start = jfs.sb.start;
        CHECK(jbd_recover(&jfs) == EUCLEAN);
        CHECK(jfs.sb.start == start);
        CHECK(dev.fs->super_replay_required);
        CHECK(fault_block_crash(&disk) == 0);
        return 0;
    }
    if (!strcmp(argv[2], "recover-corrupt")) {
        ext4_fsblk_t journal_sb;
        struct jbd_sb before, after;
        CHECK(jbd_inode_bmap(&jfs, 0, &journal_sb) == EOK);
        CHECK(ext4_blocks_get_direct(&dev, &before, journal_sb, 1) == EOK);
        CHECK(jbd_recover(&jfs) == atoi(argv[3]));
        CHECK(jfs.sb.start == before.start && before.start != 0);
        CHECK(fault_block_crash(&disk) == 0);
        CHECK(ext4_blocks_get_direct(&dev, &after, journal_sb, 1) == EOK);
        CHECK(memcmp(&before, &after, sizeof(before)) == 0);
        return 0;
    }
    if (dev.fs->super_replay_required && !strncmp(argv[2], "recover", 7)) {
        CHECK(jbd_recover(&jfs) == EOK);
        CHECK(!dev.fs->super_replay_required);
    }
    ext4_file file;
    CHECK(ext4_fopen(&file, "/probe", "r+") == EOK);
    struct ext4_inode_ref inode;
    ext4_fsblk_t home;
    CHECK(ext4_fs_get_inode_ref(dev.fs, file.inode, &inode) == EOK);
    CHECK(ext4_fs_get_inode_dblk_idx(&inode, 0, &home, false) == EOK);
    CHECK(ext4_fs_put_inode_ref(&inode) == EOK);
    if (!strcmp(argv[2], "mutate")) {
        mutate_log(&dev, &jfs, argv[3]);
        return 0;
    }
    if (!strncmp(argv[2], "recover", 7)) {
        if (!strcmp(argv[2], "recover-write-error")) disk.fail_write = disk.writes + 1;
        if (!strcmp(argv[2], "recover-flush-error")) disk.fail_flush = disk.flushes + 1;
        if (strstr(argv[2], "error")) {
            CHECK(jbd_recover(&jfs) == EIO);
            CHECK(fault_block_crash(&disk) == 0);
            return 0;
        }
        CHECK(jbd_recover(&jfs) == EOK);
        CHECK(!dev.fs->super_replay_required);
        if (!strcmp(argv[2], "recover-crash")) CHECK(fault_block_crash(&disk) == 0);
        CHECK(jbd_put_fs(&jfs) == EOK);
        unsigned char data[1024];
        CHECK(ext4_blocks_get_direct(&dev, data, home, 1) == EOK);
        CHECK(data[0] == (unsigned char)atoi(argv[3]));
        if (!strcmp(argv[2], "recover-metadata")) {
            CHECK(ext4_fs_get_inode_ref(dev.fs, file.inode, &inode) == EOK);
            CHECK((ext4_inode_get_mode(&dev.fs->sb, inode.inode) & 0777) == 0600);
            CHECK(ext4_fs_put_inode_ref(&inode) == EOK);
        }
        CHECK(ext4_fclose(&file) == EOK);
        CHECK(ext4_umount("/") == EOK);
        CHECK(kernel_block_flush(&disk.device) == KERNEL_BLOCK_STATUS_OK);
        return 0;
    }
    if (!strcmp(argv[2], "no-flush")) {
        iface.flush = NULL;
        CHECK(jbd_journal_start(&jfs, &journal) == ENOTSUP);
        return 0;
    }
    if (!strncmp(argv[2], "checksum2", 9) || !strncmp(argv[2], "checksum3", 9)) {
        jfs.sb.feature_incompat = to_be32(JBD_FEATURE_INCOMPAT_REVOKE |
            (!strncmp(argv[2], "checksum2", 9) ? JBD_FEATURE_INCOMPAT_CSUM_V2 : JBD_FEATURE_INCOMPAT_CSUM_V3));
        jfs.sb.checksum_type = JBD_CRC32C_CHKSUM;
        if (strstr(argv[2], "64"))
            jfs.sb.feature_incompat |= to_be32(JBD_FEATURE_INCOMPAT_64BIT);
    }
    if (!strcmp(argv[2], "unsupported")) {
        jfs.sb.feature_incompat = to_be32(JBD_FEATURE_INCOMPAT_ASYNC_COMMIT);
        CHECK(jbd_journal_start(&jfs, &journal) == ENOTSUP);
        return 0;
    }
    CHECK(jbd_journal_start(&jfs, &journal) == EOK);
    CHECK(ext4_block_cache_write_back(&dev, 1) == EOK);
    if (!strcmp(argv[2], "abort-fresh")) {
        /* Aborting a first modification must restore the live cache as well
         * as leave disk unchanged. No prior committed log can supply it. */
        struct jbd_trans *tx = jbd_journal_new_trans(&journal);
        CHECK(tx != NULL);
        dev.fs->jbd_journal = &journal;
        dev.fs->curr_trans = tx;
        struct ext4_block block;
        CHECK(ext4_trans_block_get(&dev, &block, home) == EOK);
        memset(block.data, 'A', dev.lg_bsize);
        CHECK(ext4_trans_set_block_dirty(block.buf) == EOK);
        dev.fs->curr_trans = NULL;
        jbd_journal_free_trans(&journal, tx, true);
        /* A held inode/table reference must still point at the rolled-back
         * contents; invalidating and waiting for the next get is too late. */
        CHECK(block.data[0] == 'O');
        CHECK(ext4_block_set(&dev, &block) == EOK);
        CHECK(fault_block_crash(&disk) == 0);
        return 0;
    }
    uint64_t before = disk.flushes;
    unsigned iterations = !strcmp(argv[2], "wrap") ? 1600 :
        !strcmp(argv[2], "checksum3-two") ? 2 : 1;
    for (unsigned iteration = 0; iteration < iterations; iteration++) {
    struct jbd_trans *tx = jbd_journal_new_trans(&journal);
    CHECK(tx != NULL);
    if (!strcmp(argv[2], "super-record") || !strcmp(argv[2], "bad-super")) {
        struct ext4_block super;
        dev.fs->jbd_journal = &journal;
        dev.fs->curr_trans = tx;
        CHECK(ext4_trans_block_get(&dev, &super,
              EXT4_SUPERBLOCK_OFFSET / dev.lg_bsize) == EOK);
        struct ext4_sblock *sb = (void *)(super.data + EXT4_SUPERBLOCK_OFFSET % dev.lg_bsize);
        ext4_sb_set_csum(sb);
        if (!strcmp(argv[2], "bad-super")) sb->checksum ^= 1;
        CHECK(ext4_trans_set_block_dirty(super.buf) == EOK);
        CHECK(ext4_block_set(&dev, &super) == EOK);
        dev.fs->curr_trans = NULL;
    }
    if (!strcmp(argv[2], "metadata")) {
        dev.fs->jbd_journal = &journal;
        dev.fs->curr_trans = tx;
        CHECK(ext4_fs_get_inode_ref(dev.fs, file.inode, &inode) == EOK);
        ext4_inode_set_mode(&dev.fs->sb, inode.inode, 0100600);
        inode.dirty = true;
        CHECK(ext4_fs_put_inode_ref(&inode) == EOK);
        dev.fs->curr_trans = NULL;
    }
    struct ext4_block block;
    CHECK(ext4_block_get(&dev, &block, home) == EOK);
    memset(block.data, !strcmp(argv[2], "checksum3-two") && iteration ? 'S' : 'N', dev.lg_bsize);
    CHECK(jbd_trans_set_block_dirty(tx, &block) == EOK);
    CHECK(ext4_block_set(&dev, &block) == EOK);
    if (strstr(argv[2], "wide")) {
        /* Force more than one descriptor, preserving all home contents.
         * Recovery must parse every descriptor boundary, including LAST. */
        for (unsigned i = 0; i < 200; i++) {
            CHECK(ext4_block_get(&dev, &block, 24000 + i) == EOK);
            CHECK(jbd_trans_get_write_access(tx, &block) == EOK);
            CHECK(jbd_trans_set_block_dirty(tx, &block) == EOK);
            CHECK(ext4_block_set(&dev, &block) == EOK);
        }
    }
    if (!strcmp(argv[2], "oversize")) {
        unsigned capacity = to_be32(jfs.sb.maxlen) - to_be32(jfs.sb.first);
        for (unsigned i = 0; i < capacity; i++) {
            CHECK(ext4_block_get_noread(&dev, &block, 24000 + i) == EOK);
            memset(block.data, 'X', dev.lg_bsize);
            CHECK(jbd_trans_set_block_dirty(tx, &block) == EOK);
            CHECK(ext4_block_set(&dev, &block) == EOK);
        }
    }
    if (!strcmp(argv[2], "error") && argc > 3) disk.fail_flush = disk.flushes + (uint64_t)atoi(argv[3]);
    if (!strcmp(argv[2], "write-error")) disk.fail_write = disk.writes + (uint64_t)atoi(argv[3]);
    if (!strncmp(argv[2], "checksum3-revoke", 16))
        CHECK(jbd_trans_revoke_block(tx, home + 1) == EOK);
    uint32_t allocation_start = journal.last;
    int rc = jbd_journal_commit_trans(&journal, tx);
    if (!strcmp(argv[2], "oversize")) {
        CHECK(rc == ENOSPC);
        CHECK(journal.error == EOK && journal.failed_trans == NULL);
        CHECK(journal.last == allocation_start);
        struct jbd_trans *retry = jbd_journal_new_trans(&journal);
        CHECK(retry != NULL);
        CHECK(jbd_journal_commit_trans(&journal, retry) == EOK);
    } else if (!strcmp(argv[2], "error") || !strcmp(argv[2], "write-error")) {
        CHECK(rc == EIO);
    } else {
        CHECK(rc == EOK);
        CHECK(disk.flushes >= before + 2);
        CHECK(jbd_journal_sync(&journal, journal.committed_id) == EOK);
        CHECK(jbd_journal_sync(&journal, journal.committed_id + 1) == EAGAIN);
        if (!strcmp(argv[2], "abort-read-error")) {
            struct jbd_trans *aborted = jbd_journal_new_trans(&journal);
            CHECK(aborted != NULL);
            CHECK(ext4_block_get(&dev, &block, home) == EOK);
            memset(block.data, 'A', dev.lg_bsize);
            CHECK(jbd_trans_set_block_dirty(aborted, &block) == EOK);
            CHECK(ext4_block_set(&dev, &block) == EOK);
            fail_read = reads + 1;
            jbd_journal_free_trans(&journal, aborted, true);
            CHECK(jbd_journal_sync(&journal, journal.committed_id) == EIO);
        }
        if (!strcmp(argv[2], "checkpoint-error") || !strcmp(argv[2], "checkpoint-flush-error")) {
            if (!strcmp(argv[2], "checkpoint-error")) disk.fail_write = disk.writes + (uint64_t)atoi(argv[3]);
            else disk.fail_flush = disk.flushes + (uint64_t)atoi(argv[3]);
            CHECK(jbd_journal_purge_cp_trans(&journal, true, false) == EIO);
            CHECK(jbd_journal_sync(&journal, journal.committed_id) == EIO);
            CHECK(jbd_journal_stop(&journal) == EIO);
        } else if (!strcmp(argv[2], "checkpoint")) {
            CHECK(jbd_journal_purge_cp_trans(&journal, true, false) == EOK);
        }
    }
    }
    /* Terminating without stop/unmount deliberately models a power cut. */
    CHECK(fault_block_crash(&disk) == 0);
    return 0;
}
