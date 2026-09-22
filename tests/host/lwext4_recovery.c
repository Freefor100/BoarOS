/* Public filesystem operations over volatile storage, including power cuts
 * inside each block write/barrier rather than normal process shutdown. */
#include "block_fault.h"
#include <ext4.h>
#include <ext4_blockdev.h>
#include <ext4_errno.h>
#include <ext4_fs.h>
#include <ext4_journal.h>
#include <ext4_inode.h>
#include <ext4_orphan.h>
#include <ext4_super.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s (allocation %u, event %u)\n", __LINE__, #x, fail_allocation, event); exit(1); } } while (0)
static struct fault_block disk;
static unsigned event, cut;
static int reorder;
static unsigned allocations, fail_allocation;
void *ext4_user_malloc(size_t size)
{ return ++allocations == fail_allocation ? NULL : malloc(size); }
void *ext4_user_calloc(size_t count, size_t size)
{ return ++allocations == fail_allocation ? NULL : calloc(count, size); }
void *ext4_user_realloc(void *pointer, size_t size)
{ return ++allocations == fail_allocation ? NULL : realloc(pointer, size); }
void ext4_user_free(void *pointer) { free(pointer); }
static void boundary(void)
{
    if (++event != cut) return;
    /* Persist an independently selected latest sector before losing all other
     * unflushed sectors. Earlier successful barriers remain untouched. */
    if (reorder && disk.count) CHECK(fault_block_persist(&disk, disk.count - 1) == 0);
    CHECK(fault_block_crash(&disk) == 0);
    _Exit(75);
}
static int dev_open(struct ext4_blockdev *b) { (void)b; return EOK; }
static int dev_read(struct ext4_blockdev *b, void *p, uint64_t n, uint32_t c)
{ (void)b; return kernel_block_read_at(&disk.device, n * 512, p, (size_t)c * 512) == KERNEL_BLOCK_STATUS_OK ? EOK : EIO; }
static int dev_write(struct ext4_blockdev *b, const void *p, uint64_t n, uint32_t c)
{ (void)b; int r = kernel_block_write_at(&disk.device, n * 512, p, (size_t)c * 512) == KERNEL_BLOCK_STATUS_OK ? EOK : EIO; boundary(); return r; }
static int dev_flush(struct ext4_blockdev *b)
{ (void)b; int r = kernel_block_flush(&disk.device) == KERNEL_BLOCK_STATUS_OK ? EOK : EIO; boundary(); return r; }

static const char payload[] = "Committed directory and file allocation";

/* A committed shrink may still own old mappings through its orphan record.
 * Neither another operation nor a nested operation may make those bytes
 * visible again when the file grows. Live unlink retains its separate owner. */
static void shrink_regrow(struct ext4_fs *fs)
{
    size_t block = fs->bdev->lg_bsize, original = block * 20 + 17;
    unsigned char *bytes = malloc(original + block);
    CHECK(bytes != NULL);
    for (unsigned nested = 0; nested < 2; nested++)
    for (unsigned partial = 0; partial < 2; partial++)
    for (unsigned writing = 0; writing < 2; writing++)
    for (unsigned unlinked = 0; unlinked < 2; unlinked++) {
        ext4_file file;
        size_t count, keep = partial ? block + 7 : 0;
        CHECK(ext4_fopen(&file, "/resize", "w+") == EOK);
        memset(bytes, 0x5a, original);
        CHECK(ext4_fwrite(&file, bytes, original, &count) == EOK && count == original);
        uint32_t ino = file.inode;
        if (unlinked) {
            uint32_t removed; bool orphan;
            CHECK(ext4_funlink_dentry("/resize", &removed, &orphan) == EOK);
            CHECK(removed == ino && orphan);
        }
        CHECK(ext4_transaction_begin("/") == EOK);
        CHECK(ext4_ftruncate(&file, keep) == EOK);
        if (!nested) CHECK(ext4_transaction_end("/") == EOK);
        size_t end = original + 7;
        if (writing) {
            CHECK(ext4_fseek(&file, (int64_t)end - 3, SEEK_SET) == EOK);
            CHECK(ext4_fwrite(&file, "new", 3, &count) == EOK && count == 3);
        } else CHECK(ext4_ftruncate(&file, end) == EOK);
        if (nested) CHECK(ext4_transaction_end("/") == EOK);
        CHECK(ext4_fseek(&file, 0, SEEK_SET) == EOK);
        CHECK(ext4_fread(&file, bytes, end, &count) == EOK && count == end);
        for (size_t i = 0; i < keep; i++) CHECK(bytes[i] == 0x5a);
        for (size_t i = keep; i < end - (writing ? 3 : 0); i++) CHECK(bytes[i] == 0);
        if (writing) CHECK(memcmp(bytes + end - 3, "new", 3) == 0);
        if (unlinked) {
            uint32_t pending;
            CHECK(ext4_orphan_peek(fs, &pending) == EOK && pending == ino);
        }
        CHECK(ext4_fclose(&file) == EOK);
        if (unlinked) CHECK(ext4_orphan_free("/", ino) == EOK);
        else CHECK(ext4_fremove("/resize") == EOK);
    }
    free(bytes);
}

static void shrink_regrow_oom(struct ext4_fs *fs, bool nested, unsigned fail)
{
    size_t original = fs->bdev->lg_bsize * 40 + 17, count;
    unsigned char *bytes = malloc(original + 7);
    CHECK(bytes != NULL);
    memset(bytes, 0x5a, original);
    ext4_file file;
    CHECK(ext4_fopen(&file, "/resize", "w+") == EOK);
    CHECK(ext4_fwrite(&file, bytes, original, &count) == EOK && count == original);
    CHECK(ext4_transaction_begin("/") == EOK);
    CHECK(ext4_ftruncate(&file, 7) == EOK);
    if (!nested) CHECK(ext4_transaction_end("/") == EOK);
    CHECK(ext4_fseek(&file, (int64_t)original + 4, SEEK_SET) == EOK);
    allocations = 0;
    fail_allocation = fail;
    count = 99;
    int r = ext4_fwrite(&file, "new", 3, &count);
    if (nested) {
        if (r == EOK) r = ext4_transaction_end("/");
        else CHECK(ext4_transaction_abort("/", r) == r);
    }
    unsigned attempted = allocations;
    fail_allocation = 0;
    if (r != EOK) {
        CHECK(r == ENOMEM);
        CHECK(fs->jbd_journal->error == EOK);
        if (nested) {
            /* Reopen after outer abort, as required for its cached handle. */
            CHECK(ext4_fclose(&file) == EOK);
            CHECK(ext4_fopen(&file, "/resize", "r+") == EOK);
            CHECK(ext4_fsize(&file) == original);
            CHECK(ext4_fread(&file, bytes, original, &count) == EOK && count == original);
            for (size_t i = 0; i < original; i++) CHECK(bytes[i] == 0x5a);
            CHECK(ext4_ftruncate(&file, 7) == EOK);
        } else CHECK(count == 0 && ext4_fsize(&file) == 7);
        CHECK(ext4_fseek(&file, (int64_t)original + 4, SEEK_SET) == EOK);
        CHECK(ext4_fwrite(&file, "new", 3, &count) == EOK && count == 3);
    }
    CHECK(ext4_fseek(&file, 0, SEEK_SET) == EOK);
    CHECK(ext4_fread(&file, bytes, original + 7, &count) == EOK && count == original + 7);
    for (size_t i = 0; i < 7; i++) CHECK(bytes[i] == 0x5a);
    for (size_t i = 7; i < original + 4; i++) CHECK(bytes[i] == 0);
    CHECK(memcmp(bytes + original + 4, "new", 3) == 0);
    CHECK(ext4_fclose(&file) == EOK);
    CHECK(ext4_fremove("/resize") == EOK);
    free(bytes);
    printf("%u\n", attempted);
}

static void grouped_remove(struct ext4_blockdev *dev)
{
    for (unsigned scenario = 0; scenario < 4; scenario++) {
        struct ext4_fs *fs = dev->fs;
        ext4_file file; size_t count;
        uint64_t free_blocks = ext4_sb_get_free_blocks_cnt(&fs->sb);
        uint32_t free_inodes = ext4_get32(&fs->sb, free_inodes_count);
        CHECK(ext4_fopen(&file, "/remove", "w+") == EOK);
        CHECK(ext4_fwrite(&file, payload, sizeof(payload), &count) == EOK && count == sizeof(payload));
        uint32_t ino = file.inode;
        CHECK(ext4_fclose(&file) == EOK);
        uint64_t allocated_blocks = ext4_sb_get_free_blocks_cnt(&fs->sb);
        uint32_t allocated_inodes = ext4_get32(&fs->sb, free_inodes_count);
        CHECK(ext4_transaction_begin("/") == EOK);
        CHECK(ext4_fremove("/remove") == EOK);
        CHECK(ext4_fopen(&file, "/remove", "r") == ENOENT);
        if (scenario < 2) {
            if (scenario == 0)
                CHECK(ext4_transaction_abort("/", ECANCELED) == ECANCELED);
            else {
                /* A failure before the nested mutator begins still prevents
                 * the outer group from committing its earlier deletion. */
                CHECK(ext4_fremove("/missing") == ENOENT);
                CHECK(ext4_transaction_end("/") == ENOENT);
            }
            CHECK(ext4_sb_get_free_blocks_cnt(&fs->sb) == allocated_blocks);
            CHECK(ext4_get32(&fs->sb, free_inodes_count) == allocated_inodes);
            char bytes[sizeof(payload)];
            CHECK(ext4_fopen(&file, "/remove", "r") == EOK);
            CHECK(ext4_fread(&file, bytes, sizeof(bytes), &count) == EOK);
            CHECK(count == sizeof(payload) && !memcmp(bytes, payload, count));
            CHECK(ext4_fclose(&file) == EOK);
            CHECK(ext4_fremove("/remove") == EOK);
        } else {
            CHECK(ext4_transaction_end("/") == EOK);
            CHECK(ext4_fopen(&file, "/remove", "r") == ENOENT);
            uint32_t pending;
            CHECK(ext4_orphan_peek(fs, &pending) == EOK && pending == ino);
            if (scenario == 2) CHECK(ext4_orphan_recover("/") == EOK);
            else {
                CHECK(ext4_umount("/") == EOK);
                CHECK(ext4_mount("recovery", "/", false) == EOK);
                CHECK(ext4_recover("/") == EOK);
                CHECK(ext4_journal_start("/") == EOK);
                fs = dev->fs;
                CHECK(ext4_fopen(&file, "/remove", "r") == ENOENT);
            }
        }
        uint32_t pending;
        CHECK(ext4_orphan_peek(fs, &pending) == EOK && pending == 0);
        CHECK(ext4_sb_get_free_blocks_cnt(&fs->sb) == free_blocks);
        CHECK(ext4_get32(&fs->sb, free_inodes_count) == free_inodes);
    }
}

int main(int argc, char **argv)
{
    CHECK(argc >= 3);
    CHECK(fault_block_open(&disk, argv[1]) == 0);
    unsigned char scratch[512];
    struct ext4_blockdev_iface iface = {
        .open = dev_open, .close = dev_open, .bread = dev_read,
        .bwrite = dev_write, .flush = dev_flush,
        .ph_bsize = 512, .ph_bcnt = disk.device.capacity_bytes / 512,
        .ph_bbuf = scratch,
    };
    struct ext4_blockdev dev = { .bdif = &iface, .part_size = disk.device.capacity_bytes };
    CHECK(ext4_device_register(&dev, "recovery") == EOK);
    CHECK(ext4_mount("recovery", "/", false) == EOK);
    CHECK(ext4_recover("/") == EOK);
    if (!strcmp(argv[2], "damage-root") || !strcmp(argv[2], "damage-index")) {
        struct ext4_inode_ref inode; ext4_fsblk_t home;
        uint32_t number = EXT4_INODE_ROOT_INDEX;
        if (!strcmp(argv[2], "damage-index")) {
            ext4_dir directory;
            CHECK(ext4_dir_open(&directory, "/created") == EOK);
            number = directory.f.inode;
            CHECK(ext4_dir_close(&directory) == EOK);
        }
        CHECK(ext4_fs_get_inode_ref(dev.fs, number, &inode) == EOK);
        CHECK(ext4_fs_get_inode_dblk_idx(&inode, 0, &home, false) == EOK);
        CHECK(ext4_fs_put_inode_ref(&inode) == EOK);
        unsigned char *block = malloc(dev.lg_bsize);
        CHECK(block != NULL);
        CHECK(ext4_blocks_get_direct(&dev, block, home, 1) == EOK);
        block[dev.lg_bsize - 1] ^= 0x80;
        CHECK(ext4_blocks_set_direct(&dev, block, home, 1) == EOK);
        CHECK(dev_flush(&dev) == EOK);
        free(block);
        return 0;
    }
    if (!strcmp(argv[2], "reject-root") || !strcmp(argv[2], "reject-index")) {
        uint64_t before = disk.writes;
        ext4_file file;
        CHECK(ext4_fopen(&file, !strcmp(argv[2], "reject-root") ?
                        "/missing" : "/created/missing", "r") == EUCLEAN);
        CHECK(disk.writes == before);
        return 0;
    }
    if (!strcmp(argv[2], "verify")) {
        ext4_file file;
        int r = ext4_fopen(&file, "/created/data", "r");
        if (r == EOK) {
            char bytes[sizeof(payload)]; size_t got;
            CHECK(ext4_fsize(&file) == sizeof(payload));
            CHECK(ext4_fread(&file, bytes, sizeof(bytes), &got) == EOK);
            CHECK(got == sizeof(payload) && !memcmp(bytes, payload, got));
            CHECK(ext4_fclose(&file) == EOK);
        } else {
            CHECK(r == ENOENT);
            ext4_dir directory;
            CHECK(ext4_dir_open(&directory, "/created") == ENOENT);
        }
        CHECK(ext4_umount("/") == EOK);
        CHECK(dev_flush(&dev) == EOK);
        return 0;
    }
    CHECK(ext4_journal_start("/") == EOK);
    event = 0;
    if (!strcmp(argv[2], "group-remove")) {
        grouped_remove(&dev);
        CHECK(ext4_umount("/") == EOK);
        puts("PASS: grouped remove commit/recovery/unmount, abort, failure poisoning");
        return 0;
    }
    if (!strcmp(argv[2], "shrink-regrow")) {
        shrink_regrow(dev.fs);
        CHECK(ext4_umount("/") == EOK);
        puts("PASS: pending shrink, nested growth/write, live unlink ownership");
        return 0;
    }
    if (!strcmp(argv[2], "regrow-oom") || !strcmp(argv[2], "regrow-nested-oom")) {
        unsigned fail = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 10) : 0;
        shrink_regrow_oom(dev.fs, !strcmp(argv[2], "regrow-nested-oom"), fail);
        CHECK(ext4_umount("/") == EOK);
        return 0;
    }
    if (!strcmp(argv[2], "oom")) {
        allocations = 0;
        if (argc > 3) fail_allocation = (unsigned)strtoul(argv[3], NULL, 10);
        uint64_t blocks_before = ext4_sb_get_free_blocks_cnt(&dev.fs->sb);
        uint32_t inodes_before = ext4_get32(&dev.fs->sb, free_inodes_count);
        int r = ext4_transaction_begin("/");
        if (r == EOK) {
            ext4_file file = {0}; size_t count;
            r = ext4_dir_mk("/created");
            if (r == EOK) r = ext4_fopen(&file, "/created/data", "w+");
            if (r == EOK) r = ext4_fwrite(&file, payload, sizeof(payload), &count);
            if (file.mp) CHECK(ext4_fclose(&file) == EOK);
            unsigned attempted = allocations;
            if (r == EOK) r = ext4_transaction_end("/");
            else CHECK(ext4_transaction_abort("/", r) == r);
            if (allocations > attempted) attempted = allocations;
            allocations = attempted;
        }
        unsigned attempted = allocations;
        if (r != EOK) {
            CHECK(r == ENOMEM);
            CHECK(ext4_sb_get_free_blocks_cnt(&dev.fs->sb) == blocks_before);
            CHECK(ext4_get32(&dev.fs->sb, free_inodes_count) == inodes_before);
            ext4_dir directory;
            CHECK(ext4_dir_open(&directory, "/created") == ENOENT);
        }
        fail_allocation = 0;
        printf("%u\n", attempted);
        CHECK(fault_block_crash(&disk) == 0);
        return 0;
    }
    if (argc > 3) cut = (unsigned)strtoul(argv[3], NULL, 10);
    if (argc > 4) reorder = atoi(argv[4]);
    uint64_t blocks = ext4_sb_get_free_blocks_cnt(&dev.fs->sb);
    uint32_t inodes = ext4_get32(&dev.fs->sb, free_inodes_count);
    CHECK(ext4_transaction_begin("/") == EOK);
    CHECK(ext4_dir_mk("/created") == EOK);
    ext4_file file; size_t wrote;
    CHECK(ext4_fopen(&file, "/created/data", "w+") == EOK);
    CHECK(ext4_fwrite(&file, payload, sizeof(payload), &wrote) == EOK);
    CHECK(wrote == sizeof(payload));
    CHECK(ext4_fclose(&file) == EOK);
    if (!strcmp(argv[2], "abort")) {
        CHECK(ext4_transaction_abort("/", ECANCELED) == ECANCELED);
        CHECK(ext4_sb_get_free_blocks_cnt(&dev.fs->sb) == blocks);
        CHECK(ext4_get32(&dev.fs->sb, free_inodes_count) == inodes);
        CHECK(ext4_fopen(&file, "/created/data", "r") == ENOENT);
    } else {
        /* Data persistence is the first barrier; fail the log barrier. */
        if (!strcmp(argv[2], "error")) disk.fail_flush = disk.flushes + 2;
        int r = ext4_transaction_end("/");
        if (!strcmp(argv[2], "error")) {
            CHECK(r == EIO);
            CHECK(ext4_dir_mk("/after-error") == EIO);
            CHECK(ext4_journal_stop("/") == EIO);
        } else {
            CHECK(r == EOK);
            CHECK(jbd_journal_purge_cp_trans(dev.fs->jbd_journal, true, false) == EOK);
        }
    }
    printf("%u\n", event);
    CHECK(fault_block_crash(&disk) == 0);
    return 0;
}
