/* Restartable reclamation after the final i_size is already durable. */
#include "block_fault.h"
#include <ext4.h>
#include <ext4_bcache.h>
#include <ext4_blockdev.h>
#include <ext4_errno.h>
#include <ext4_fs.h>
#include <ext4_inode.h>
#include <ext4_journal.h>
#include <ext4_orphan.h>
#include <ext4_super.h>
#include <ext4_truncate.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static struct fault_block disk;
static unsigned event, cut;
static bool reorder;
static void boundary(void)
{
    if (++event != cut) return;
    if (reorder && disk.count) CHECK(fault_block_persist(&disk, disk.count - 1) == 0);
    CHECK(fault_block_crash(&disk) == 0);
    _Exit(75);
}
static int dev_open(struct ext4_blockdev *dev) { (void)dev; return EOK; }
static int dev_read(struct ext4_blockdev *dev, void *buf, uint64_t lba, uint32_t n)
{ (void)dev; return kernel_block_read_at(&disk.device, lba * 512, buf, (size_t)n * 512) == KERNEL_BLOCK_STATUS_OK ? EOK : EIO; }
static int dev_write(struct ext4_blockdev *dev, const void *buf, uint64_t lba, uint32_t n)
{ (void)dev; int r = kernel_block_write_at(&disk.device, lba * 512, buf, (size_t)n * 512) == KERNEL_BLOCK_STATUS_OK ? EOK : EIO; boundary(); return r; }
static int dev_flush(struct ext4_blockdev *dev)
{ (void)dev; int r = kernel_block_flush(&disk.device) == KERNEL_BLOCK_STATUS_OK ? EOK : EIO; boundary(); return r; }

static size_t sparse_blocks(struct ext4_fs *fs, uint32_t *blocks)
{
    uint64_t n = fs->bdev->lg_bsize / sizeof(uint32_t);
    uint64_t triple = EXT4_INODE_DIRECT_BLOCK_COUNT + n + n * n;
    uint32_t values[] = { 128, 512, 4096, 8192, 12000,
        (uint32_t)(triple + 9), (uint32_t)(triple + n + 3) };
    memcpy(blocks, values, sizeof(values));
    return sizeof(values) / sizeof(values[0]);
}

/* Isolate this primitive from the production orphan recovery loop: replay only
 * JBD, release its inode reference, then discard clean pre-replay cache data. */
static void replay(struct ext4_fs *fs)
{
    struct jbd_fs *jfs = calloc(1, sizeof(*jfs));
    CHECK(jfs != NULL);
    CHECK(jbd_get_fs(fs, jfs) == EOK);
    CHECK(jbd_recover(jfs) == EOK);
    CHECK(jbd_put_fs(jfs) == EOK);
    free(jfs);
    ext4_bcache_cleanup(fs->bdev->bc);
    CHECK(ext4_sb_read(fs->bdev, &fs->sb) == EOK);
}

static void prepare(struct ext4_fs *fs, uint64_t target, bool sparse, bool unlink)
{
    uint32_t bsize = fs->bdev->lg_bsize;
    unsigned char *bytes = malloc(bsize);
    CHECK(bytes != NULL);
    memset(bytes, 0x6d, bsize);
    ext4_file file;
    CHECK(ext4_fopen(&file, "/victim", "w+") == EOK);
    size_t wrote;
    for (unsigned i = 0; i < 80; ++i)
        CHECK(ext4_fwrite(&file, bytes, bsize, &wrote) == EOK && wrote == bsize);
    if (sparse) {
        uint32_t blocks[8]; size_t count = sparse_blocks(fs, blocks);
        for (size_t i = 0; i < count; ++i) {
            CHECK(ext4_fseek(&file, (uint64_t)blocks[i] * bsize, SEEK_SET) == EOK);
            int result = ext4_fwrite(&file, bytes, bsize, &wrote);
            if (result != EOK) fprintf(stderr, "sparse write block %u returned %d\n", blocks[i], result);
            CHECK(result == EOK && wrote == bsize);
        }
    }
    free(bytes);
    uint32_t ino = file.inode;
    CHECK(ext4_fclose(&file) == EOK);
    CHECK(ext4_transaction_begin("/") == EOK);
    if (unlink) {
        uint32_t unlinked; bool orphan;
        CHECK(ext4_funlink_dentry("/victim", &unlinked, &orphan) == EOK);
        CHECK(unlinked == ino && orphan);
    }
    struct ext4_inode_ref ref;
    CHECK(ext4_fs_get_inode_ref(fs, ino, &ref) == EOK);
    CHECK(ext4_orphan_add(&ref) == EOK);
    ext4_inode_set_size(ref.inode, target);
    ref.dirty = true;
    CHECK(ext4_fs_put_inode_ref(&ref) == EOK);
    CHECK(ext4_transaction_end("/") == EOK);
}

static bool step(struct ext4_fs *fs)
{
    uint32_t ino;
    CHECK(ext4_orphan_peek(fs, &ino) == EOK);
    if (!ino) return true;
    CHECK(ext4_transaction_begin("/") == EOK);
    struct ext4_inode_ref ref;
    CHECK(ext4_fs_get_inode_ref(fs, ino, &ref) == EOK);
    uint64_t target = ext4_inode_get_size(&fs->sb, ref.inode);
    uint64_t before = ext4_inode_get_blocks_count(&fs->sb, ref.inode);
    bool done = false;
    CHECK(ext4_orphan_truncate_step(&ref, &done) == EOK);
    CHECK(ext4_inode_get_size(&fs->sb, ref.inode) == target);
    if (!done) CHECK(ext4_inode_get_blocks_count(&fs->sb, ref.inode) < before);
    if (done) {
        /* Fixture has no xattrs. Its retained prefix is contiguous, so a
         * bounded amount of tree metadata may remain beside the data. */
        uint64_t blocks = ext4_inode_get_blocks_count(&fs->sb, ref.inode);
        uint64_t keep = (target + fs->bdev->lg_bsize - 1) / fs->bdev->lg_bsize;
        CHECK(blocks <= (keep + (keep ? 4 : 0)) * (fs->bdev->lg_bsize / 512));
        CHECK(ext4_orphan_remove(&ref) == EOK);
        if (!ext4_inode_get_links_cnt(ref.inode)) {
            ext4_inode_set_del_time(ref.inode, UINT32_MAX);
            ref.dirty = true;
            CHECK(ext4_fs_free_inode(&ref) == EOK);
        }
    }
    CHECK(ext4_fs_put_inode_ref(&ref) == EOK);
    CHECK(ext4_transaction_end("/") == EOK);
    return done;
}

static void verify(struct ext4_fs *fs, uint64_t target, bool unlink)
{
    uint32_t orphan;
    CHECK(ext4_orphan_peek(fs, &orphan) == EOK && !orphan);
    ext4_file file;
    if (unlink) { CHECK(ext4_fopen(&file, "/victim", "r") == ENOENT); return; }
    CHECK(ext4_fopen(&file, "/victim", "r")==EOK);
    CHECK(ext4_fsize(&file) == target);
    unsigned char *bytes = malloc(fs->bdev->lg_bsize);
    CHECK(bytes != NULL);
    for (uint64_t offset = 0; offset < target;) {
        size_t amount = target - offset < fs->bdev->lg_bsize ?
            (size_t)(target - offset) : fs->bdev->lg_bsize, got;
        CHECK(ext4_fread(&file, bytes, amount, &got) == EOK && got == amount);
        for (size_t i = 0; i < got; ++i) CHECK(bytes[i] == 0x6d);
        offset += got;
    }
    free(bytes);
    CHECK(ext4_fclose(&file) == EOK);
}

int main(int argc, char **argv)
{
    CHECK(argc >= 5);
    CHECK(fault_block_open(&disk, argv[1]) == 0);
    unsigned char physical[512];
    struct ext4_blockdev_iface iface = { .open = dev_open, .close = dev_open,
        .bread = dev_read, .bwrite = dev_write, .flush = dev_flush, .ph_bsize = 512,
        .ph_bcnt = disk.device.capacity_bytes / 512, .ph_bbuf = physical };
    struct ext4_blockdev dev = { .bdif = &iface, .part_size = disk.device.capacity_bytes };
    CHECK(ext4_device_register(&dev, "truncate") == EOK);
    CHECK(ext4_mount("truncate", "/", false) == EOK);
    replay(dev.fs);
    CHECK(ext4_journal_start("/") == EOK);
    uint64_t target = strtoull(argv[3], NULL, 10);
    bool unlink = !strcmp(argv[4], "unlinked");
    if (!strcmp(argv[2], "prepare")) {
        prepare(dev.fs, target, strcmp(argv[4], "dense") != 0, unlink);
        CHECK(fault_block_crash(&disk) == 0);
        return 0;
    }
    event = 0;
    if (argc > 5) cut = (unsigned)strtoul(argv[5], NULL, 10);
    if (argc > 6) reorder = atoi(argv[6]) != 0;
    if (!strcmp(argv[2], "step")) {
        bool done = step(dev.fs);
        printf("%u %u\n", done ? 1 : 0, event);
        CHECK(fault_block_crash(&disk) == 0);
        return 0;
    }
    for (unsigned limit = 0; !step(dev.fs); ++limit) CHECK(limit < 1000);
    verify(dev.fs, target, unlink);
    CHECK(ext4_journal_stop("/") == EOK);
    CHECK(ext4_umount("/") == EOK);
    fault_block_close(&disk);
    return 0;
}
