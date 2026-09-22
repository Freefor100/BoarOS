/* Inode-based rename over real ext4/JBD and volatile 512-byte storage. */
#include "block_fault.h"
#include <ext4.h>
#include <ext4_fs.h>
#include <ext4_inode.h>
#include <ext4_orphan.h>
#include <ext4_super.h>
#include <ext4_journal.h>
#include <ext4_dir.h>
#include <ext4_dir_idx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct fault_block disk;
static const char split_name[] =
    "moved-abcdefghijklmnopqrstuvwxyz-abcdefghijklmnopqrstuvwxyz-abcdefghijklmnopqrstuvwxyz-"
    "abcdefghijklmnopqrstuvwxyz-abcdefghijklmnopqrstuvwxyz-abcdefghijklmnopqrstuvwxyz-"
    "abcdefghijklmnopqrstuvwxyz-abcdefghijklmnopqrstuv";
static unsigned allocations, fail_allocation, events, cut;
static int reorder;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s (allocation=%u event=%u)\n", __LINE__, #x, fail_allocation, events); exit(1); } } while (0)
void *ext4_user_malloc(size_t n) { return ++allocations == fail_allocation ? NULL : malloc(n); }
void *ext4_user_calloc(size_t n, size_t s) { return ++allocations == fail_allocation ? NULL : calloc(n, s); }
void *ext4_user_realloc(void *p, size_t n) { return ++allocations == fail_allocation ? NULL : realloc(p, n); }
void ext4_user_free(void *p) { free(p); }
static void boundary(void)
{
    if (++events != cut) return;
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
static uint32_t inode(const char *path)
{
    ext4_file f; CHECK(ext4_fopen(&f, path, "r") == EOK);
    uint32_t n = f.inode; CHECK(ext4_fclose(&f) == EOK); return n;
}
static uint32_t directory(const char *path)
{
    ext4_dir d; CHECK(ext4_dir_open(&d, path) == EOK);
    uint32_t n = d.f.inode; CHECK(ext4_dir_close(&d) == EOK); return n;
}
static bool exists(const char *path)
{
    ext4_file f; int r = ext4_fopen(&f, path, "r");
    if (r == EOK) CHECK(ext4_fclose(&f) == EOK);
    else CHECK(r == ENOENT);
    return r == EOK;
}
static void write_file(const char *path, const char *value)
{
    ext4_file f; size_t n;
    CHECK(ext4_fopen(&f, path, "w+") == EOK);
    CHECK(ext4_fwrite(&f, value, strlen(value) + 1, &n) == EOK && n == strlen(value) + 1);
    CHECK(ext4_fclose(&f) == EOK);
}
static void read_file(ext4_file *f, const char *value)
{
    char bytes[80]; size_t n;
    CHECK(ext4_fseek(f, 0, SEEK_SET) == EOK);
    CHECK(ext4_fread(f, bytes, sizeof(bytes), &n) == EOK);
    CHECK(n == strlen(value) + 1 && !memcmp(bytes, value, n));
}
static void content(const char *path, const char *value)
{
    ext4_file f; CHECK(ext4_fopen(&f, path, "r") == EOK);
    read_file(&f, value); CHECK(ext4_fclose(&f) == EOK);
}
static unsigned links(struct ext4_fs *fs, uint32_t ino)
{
    struct ext4_inode_ref ref; CHECK(ext4_fs_get_inode_ref(fs, ino, &ref) == EOK);
    unsigned n = ext4_inode_get_links_cnt(ref.inode);
    CHECK(ext4_fs_put_inode_ref(&ref) == EOK); return n;
}
static uint32_t dotdot(struct ext4_fs *fs, uint32_t ino)
{
    struct ext4_inode_ref ref; struct ext4_block block;
    CHECK(ext4_fs_get_inode_ref(fs, ino, &ref) == EOK);
    ext4_fsblk_t home; CHECK(ext4_fs_get_inode_dblk_idx(&ref, 0, &home, false) == EOK);
    CHECK(ext4_block_get(fs->bdev, &block, home) == EOK);
    struct ext4_dir_en *dot = (void *)block.data;
    struct ext4_dir_en *parent = (void *)(block.data + to_le16(dot->entry_len));
    CHECK(parent->name_len == 2 && parent->name[0] == '.' && parent->name[1] == '.');
    uint32_t n = to_le32(parent->inode);
    CHECK(ext4_block_set(fs->bdev, &block) == EOK);
    CHECK(ext4_fs_put_inode_ref(&ref) == EOK); return n;
}
static int rename_entry(uint32_t old, const char *from, uint32_t new,
                        const char *to, unsigned flags, struct ext4_rename_result *out)
{ return ext4_rename_child("/", old, from, strlen(from), new, to, strlen(to), flags, out); }
static void seed(void)
{
    CHECK(ext4_dir_mk("/old") == EOK); CHECK(ext4_dir_mk("/new") == EOK);
    CHECK(ext4_dir_mk("/old/dir") == EOK); CHECK(ext4_dir_mk("/new/empty") == EOK);
    write_file("/old/source", "SOURCE"); write_file("/new/target", "TARGET");
    write_file("/old/dir/item", "CHILD");
}
static void seed_split(struct ext4_fs *fs)
{
    uint32_t old = directory("/old"), new = directory("/new");
    for (unsigned i = 0; i < 512; i++) {
        char name[240], path[256];
        memset(name, 'a' + i % 26, 220); name[220] = 0;
        snprintf(name, 12, "fill-%05u-", i); name[11] = 'x';
        snprintf(path, sizeof(path), "/new/%s", name);
        write_file(path, "FILL");
        uint64_t before = ext4_sb_get_free_blocks_cnt(&fs->sb);
        CHECK(ext4_transaction_begin("/") == EOK);
        struct ext4_rename_result out;
        CHECK(rename_entry(old, "dir", new, split_name, 0, &out) == EOK);
        bool allocated = ext4_sb_get_free_blocks_cnt(&fs->sb) < before;
        CHECK(ext4_transaction_abort("/", ECANCELED) == ECANCELED);
        CHECK(ext4_sb_get_free_blocks_cnt(&fs->sb) == before);
        if (allocated) return;
    }
    CHECK(!"could not build a directory that must grow on rename");
}
static void basic(struct ext4_fs *fs)
{
    uint32_t old = directory("/old"), new = directory("/new");
    uint32_t source = inode("/old/source"), target = inode("/new/target");
    struct ext4_rename_result out = { .replaced_inode = 123, .changed = true };
    CHECK(rename_entry(old, "source", new, "target", 2, &out) == EINVAL);
    CHECK(out.replaced_inode == 123);
    CHECK(rename_entry(old, "missing", new, "target", 0, &out) == ENOENT);
    CHECK(rename_entry(old, "source", new, "target", 1, &out) == EEXIST);
    CHECK(rename_entry(old, "source", new, "empty", 0, &out) == EISDIR);
    CHECK(rename_entry(old, "dir", new, "target", 0, &out) == ENOTDIR);
    CHECK(rename_entry(old, "source", source, "x", 0, &out) == ENOTDIR);
    CHECK(rename_entry(old, ".", new, "x", 0, &out) == EINVAL);
    CHECK(rename_entry(old, "source", new, "x/y", 0, &out) == EINVAL);
    CHECK(rename_entry(old, "dir", directory("/old/dir"), "cycle", 0, &out) == EINVAL);
    CHECK(rename_entry(new, "empty", old, "dir", 0, &out) == ENOTEMPTY);
    CHECK(rename_entry(old, "source", old, "source", 0, &out) == EOK && !out.changed);
    CHECK(rename_entry(old, "source", old, "source", 1, &out) == EEXIST);
    CHECK(ext4_flink("/old/source", "/new/same") == EOK);
    CHECK(rename_entry(old, "source", new, "same", 0, &out) == EOK && !out.changed);
    CHECK(exists("/old/source") && links(fs, source) == 2);
    CHECK(rename_entry(old, "source", new, "same", 1, &out) == EEXIST);
    CHECK(ext4_fremove("/new/same") == EOK);
    uint64_t blocks = ext4_sb_get_free_blocks_cnt(&fs->sb);
    uint32_t free_inodes = ext4_get32(&fs->sb, free_inodes_count);
    CHECK(ext4_transaction_begin("/") == EOK);
    CHECK(rename_entry(old, "source", new, "target", 0, &out) == EOK);
    CHECK(ext4_transaction_abort("/", ECANCELED) == ECANCELED);
    CHECK(ext4_sb_get_free_blocks_cnt(&fs->sb) == blocks);
    CHECK(ext4_get32(&fs->sb, free_inodes_count) == free_inodes);
    content("/old/source", "SOURCE"); content("/new/target", "TARGET");
    CHECK(ext4_transaction_begin("/") == EOK);
    CHECK(rename_entry(old, "source", new, "target", 0, &out) == EOK);
    CHECK(rename_entry(old, "missing", new, "other", 0, &out) == ENOENT);
    CHECK(ext4_transaction_end("/") == ENOENT);
    content("/old/source", "SOURCE"); content("/new/target", "TARGET");
    ext4_file held; CHECK(ext4_fopen(&held, "/new/target", "r+") == EOK);
    CHECK(rename_entry(old, "source", new, "target", 0, &out) == EOK);
    CHECK(out.changed && out.replaced_inode == target && out.replaced_last_link);
    CHECK(!exists("/old/source") && inode("/new/target") == source);
    read_file(&held, "TARGET"); CHECK(links(fs, target) == 0);
    bool orphan; CHECK(ext4_orphan_contains(fs, target, &orphan) == EOK && orphan);
    CHECK(ext4_sb_get_free_blocks_cnt(&fs->sb) == blocks);
    CHECK(ext4_fclose(&held) == EOK); CHECK(ext4_orphan_free("/", target) == EOK);
    CHECK(links(fs, old) == 3 && links(fs, new) == 3);
    CHECK(rename_entry(old, "dir", new, "empty", 0, &out) == EOK);
    CHECK(out.changed && out.replaced_last_link && out.replaced_inode);
    CHECK(links(fs, old) == 2 && links(fs, new) == 3);
    CHECK(dotdot(fs, directory("/new/empty")) == new);
    content("/new/empty/item", "CHILD");
    CHECK(ext4_orphan_free("/", out.replaced_inode) == EOK);
    CHECK(rename_entry(new, "empty", new, "moved", 0, &out) == EOK);
    CHECK(links(fs, new) == 3 && dotdot(fs, directory("/new/moved")) == new);
    CHECK(ext4_dir_mk("/new/another") == EOK);
    CHECK(rename_entry(new, "moved", new, "another", 0, &out) == EOK);
    CHECK(links(fs, new) == 3);
    CHECK(ext4_orphan_free("/", out.replaced_inode) == EOK);
    CHECK(rename_entry(new, "target", new, "renamed", 1, &out) == EOK);
    CHECK(out.changed && !out.replaced_inode && !out.replaced_last_link);
    content("/new/renamed", "SOURCE");
    CHECK(ext4_fsymlink("/old/source", "/old/link") == EOK);
    uint32_t sym, moved, mode;
    CHECK(ext4_lookup_child("/", old, "link", 4, &sym, &mode) == EOK);
    CHECK(rename_entry(old, "link", new, "link", 0, &out) == EOK);
    CHECK(ext4_lookup_child("/", new, "link", 4, &moved, &mode) == EOK && moved == sym);
    char name[40]; size_t length;
    CHECK(ext4_readlink("/new/link", name, sizeof(name), &length) == EOK);
    CHECK(length == strlen("/old/source") && !memcmp(name, "/old/source", length));
}

static void wide(struct ext4_fs *fs)
{
    uint32_t parent = directory("/new");
    for (unsigned i = 0; i < 180; i++) {
        char old[240], new[240], path[260];
        memset(old, 'a' + i % 26, 220); old[220] = 0;
        snprintf(old, 12, "old-%06u-", i); old[11] = 'x';
        memset(new, 'a' + (i + 7) % 26, 220); new[220] = 0;
        snprintf(new, 12, "new-%06u-", i); new[11] = 'y';
        snprintf(path, sizeof(path), "/new/%s", old);
        write_file(path, "WIDE");
        struct ext4_rename_result out;
        CHECK(rename_entry(parent, old, parent, new, 0, &out) == EOK && out.changed);
        CHECK(!exists(path));
        snprintf(path, sizeof(path), "/new/%s", new);
        content(path, "WIDE");
    }
    CHECK(links(fs, parent) == 3);
}

static void damage(struct ext4_fs *fs, const char *kind)
{
    struct ext4_inode_ref ref;
    uint32_t number = directory(!strcmp(kind, "parent") ? "/old/dir" :
                                !strcmp(kind, "entry") ? "/old" : "/new/empty");
    if (!strcmp(kind, "parent")) {
        CHECK(ext4_transaction_begin("/") == EOK);
        CHECK(ext4_fs_get_inode_ref(fs, number, &ref) == EOK);
        CHECK(ext4_dir_reparent(&ref, directory("/old"), directory("/new")) == EOK);
        CHECK(ext4_fs_put_inode_ref(&ref) == EOK);
        CHECK(ext4_transaction_end("/") == EOK);
        return;
    }
    CHECK(ext4_fs_get_inode_ref(fs, number, &ref) == EOK);
    if (!strcmp(kind, "entry")) {
        struct ext4_dir_search_result found;
        CHECK(ext4_dir_find_entry(&found, &ref, "source", 6) == EOK);
        ext4_dir_en_set_entry_len(found.dentry, 4);
        ext4_dir_set_csum(&ref, (void *)found.block.data);
        CHECK(ext4_blocks_set_direct(fs->bdev, found.block.data, found.block.lb_id, 1) == EOK);
        CHECK(ext4_dir_destroy_result(&ref, &found) == EOK);
    } else {
        ext4_fsblk_t home;
        unsigned index = ext4_inode_has_flag(ref.inode, EXT4_INODE_FLAG_INDEX) ? 1 : 0;
        CHECK(ext4_fs_get_inode_dblk_idx(&ref, index, &home, false) == EOK);
        unsigned char *bytes = malloc(fs->bdev->lg_bsize); CHECK(bytes != NULL);
        CHECK(ext4_blocks_get_direct(fs->bdev, bytes, home, 1) == EOK);
        bytes[fs->bdev->lg_bsize - 1] ^= 0x80;
        CHECK(ext4_blocks_set_direct(fs->bdev, bytes, home, 1) == EOK);
        free(bytes);
    }
    CHECK(ext4_fs_put_inode_ref(&ref) == EOK);
    CHECK(dev_flush(fs->bdev) == EOK);
}
static void hardlink(struct ext4_fs *fs)
{
    uint32_t old = directory("/old"), new = directory("/new");
    uint32_t victim = inode("/new/target"); struct ext4_rename_result out;
    CHECK(ext4_flink("/new/target", "/new/alias") == EOK);
    CHECK(rename_entry(old, "source", new, "target", 0, &out) == EOK);
    CHECK(out.replaced_inode == victim && !out.replaced_last_link && out.changed);
    CHECK(links(fs, victim) == 1); content("/new/alias", "TARGET");
    bool orphan; CHECK(ext4_orphan_contains(fs, victim, &orphan) == EOK && !orphan);
}
static void verify(struct ext4_fs *fs, const char *kind, bool old_only)
{
    uint32_t old = directory("/old"), new = directory("/new");
    bool before = !strcmp(kind, "file") ? exists("/old/source") : exists("/old/dir/item");
    if (old_only) CHECK(before);
    if (!strcmp(kind, "file")) {
        if (before) { content("/old/source", "SOURCE"); content("/new/target", "TARGET"); }
        else content("/new/target", "SOURCE");
        CHECK(links(fs, old) == 3 && links(fs, new) == 3);
    } else {
        char target[256], item[270];
        snprintf(target, sizeof(target), "/new/%s", !strcmp(kind, "split") ? split_name :
                 !strcmp(kind, "insert") ? "moved" : "empty");
        snprintf(item, sizeof(item), "%s/item", target);
        if (before) {
            content("/old/dir/item", "CHILD"); CHECK(!exists(item));
            CHECK(dotdot(fs, directory("/old/dir")) == old);
            CHECK(links(fs, old) == 3 && links(fs, new) == 3);
        } else {
            content(item, "CHILD"); CHECK(dotdot(fs, directory(target)) == new);
            CHECK(links(fs, old) == 2 && links(fs, new) ==
                  ((!strcmp(kind, "insert") || !strcmp(kind, "split")) ? 4U : 3U));
        }
    }
}
int main(int argc, char **argv)
{
    CHECK(argc >= 3); CHECK(fault_block_open(&disk, argv[1]) == 0);
    unsigned char scratch[512];
    struct ext4_blockdev_iface iface = { .open=dev_open, .close=dev_open,
        .bread=dev_read, .bwrite=dev_write, .flush=dev_flush, .ph_bsize=512,
        .ph_bcnt=disk.device.capacity_bytes/512, .ph_bbuf=scratch };
    struct ext4_blockdev dev = { .bdif=&iface, .part_size=disk.device.capacity_bytes };
    CHECK(ext4_device_register(&dev, "rename") == EOK);
    bool readonly = !strcmp(argv[2], "readonly");
    CHECK(ext4_mount("rename", "/", readonly) == EOK);
    CHECK(ext4_recover("/") == EOK); CHECK(ext4_journal_start("/") == EOK);
    CHECK(ext4_orphan_recover("/") == EOK);
    if (readonly) {
        struct ext4_rename_result out = { .replaced_inode = 123 };
        CHECK(rename_entry(directory("/old"), "source", directory("/new"), "target", 0, &out) == EROFS);
        CHECK(out.replaced_inode == 123 && disk.writes == 0);
    } else if (!strcmp(argv[2], "seed")) seed();
    else if (!strcmp(argv[2], "seed-split")) { seed(); seed_split(dev.fs); }
    else if (!strcmp(argv[2], "basic")) basic(dev.fs);
    else if (!strcmp(argv[2], "hardlink")) hardlink(dev.fs);
    else if (!strcmp(argv[2], "wide")) wide(dev.fs);
    else if (!strcmp(argv[2], "damage")) { CHECK(argc >= 4); damage(dev.fs, argv[3]); }
    else if (!strcmp(argv[2], "reject")) {
        CHECK(argc >= 4);
        uint32_t old = directory("/old"), new = directory("/new");
        uint64_t writes = disk.writes;
        struct ext4_rename_result out = { .replaced_inode = 123 };
        if (!strcmp(argv[3], "leaf")) {
            uint32_t removed = 0;
            CHECK(ext4_fdir_unlink_dentry("/new/empty", &removed) == EUCLEAN);
            CHECK(removed == 0 && disk.writes == writes);
        }
        CHECK(rename_entry(old, !strcmp(argv[3], "entry") ? "source" : "dir", new,
                           !strcmp(argv[3], "entry") ? "target" : "empty", 0, &out) == EUCLEAN);
        CHECK(disk.writes == writes && out.replaced_inode == 123);
        return 0;
    }
    else {
        CHECK(argc >= 4); const char *kind = argv[3];
        if (!strcmp(argv[2], "verify")) verify(dev.fs, kind, false);
        else {
            uint32_t old = directory("/old"), new = directory("/new");
            const char *from = !strcmp(kind, "file") ? "source" : "dir";
            const char *to = !strcmp(kind, "file") ? "target" : !strcmp(kind, "split") ? split_name :
                             !strcmp(kind, "insert") ? "moved" : "empty";
            uint64_t blocks = ext4_sb_get_free_blocks_cnt(&dev.fs->sb);
            uint32_t inodes = ext4_get32(&dev.fs->sb, free_inodes_count);
            allocations = events = 0;
            if (!strcmp(argv[2], "oom")) fail_allocation = argc > 4 ? strtoul(argv[4], NULL, 10) : 0;
            else if (!strcmp(argv[2], "io-write")) disk.fail_write = disk.writes + 1;
            else if (!strcmp(argv[2], "io-flush")) disk.fail_flush = disk.flushes + (argc > 4 ? strtoul(argv[4], NULL, 10) : 1);
            else { cut = argc > 4 ? strtoul(argv[4], NULL, 10) : 0; reorder = argc > 5 ? atoi(argv[5]) : 0; }
            struct ext4_rename_result out = { .replaced_inode=123 };
            int r = rename_entry(old, from, new, to, 0, &out);
            unsigned attempted = allocations;
            fail_allocation = 0;
            if (!strncmp(argv[2], "io-", 3)) {
                disk.fail_write = disk.fail_flush = 0;
                CHECK(r == EIO && out.replaced_inode == 123);
                CHECK(dev.fs->jbd_journal->error == EIO);
                CHECK(rename_entry(old, from, new, to, 0, &out) == EIO);
                struct ext4_fs *owner = dev.fs;
                CHECK(ext4_umount("/") == EIO && dev.fs == owner);
            } else if (!strcmp(argv[2], "oom")) {
                if (r != EOK) {
                    CHECK(r == ENOMEM); CHECK(out.replaced_inode == 123);
                    CHECK(dev.fs->jbd_journal->error == EOK);
                    CHECK(ext4_sb_get_free_blocks_cnt(&dev.fs->sb) == blocks);
                    CHECK(ext4_get32(&dev.fs->sb, free_inodes_count) == inodes);
                    verify(dev.fs, kind, true);
                } else verify(dev.fs, kind, false);
                printf("%u\n", attempted);
            } else { CHECK(r == EOK); printf("%u\n", events); }
            CHECK(fault_block_crash(&disk) == 0);
            return 0;
        }
    }
    CHECK(ext4_umount("/") == EOK);
    return 0;
}
