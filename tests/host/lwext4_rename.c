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
static unsigned allocations, fail_allocation, events, cut, allocation_hits, io_hits;
static int reorder;
static unsigned injected_allocation;
static const char *operation_kind = "setup", *operation_mode = "setup";
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s (kind=%s mode=%s allocation=%u event=%u)\n", __LINE__, #x, operation_kind, operation_mode, injected_allocation, events); exit(1); } } while (0)
void *ext4_user_malloc(size_t n) { if (++allocations == fail_allocation) { allocation_hits++; return NULL; } return malloc(n); }
void *ext4_user_calloc(size_t n, size_t s) { if (++allocations == fail_allocation) { allocation_hits++; return NULL; } return calloc(n, s); }
void *ext4_user_realloc(void *p, size_t n) { if (++allocations == fail_allocation) { allocation_hits++; return NULL; } return realloc(p, n); }
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
{ (void)b; int r = kernel_block_write_at(&disk.device, n * 512, p, (size_t)c * 512) == KERNEL_BLOCK_STATUS_OK ? EOK : EIO; if (disk.fail_write && disk.writes == disk.fail_write) io_hits++; boundary(); return r; }
static int dev_flush(struct ext4_blockdev *b)
{ (void)b; int r = kernel_block_flush(&disk.device) == KERNEL_BLOCK_STATUS_OK ? EOK : EIO; if (disk.fail_flush && disk.flushes == disk.fail_flush) io_hits++; boundary(); return r; }
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
    CHECK(ext4_link_child("/", old, new, "bad", 3) == EPERM);
    CHECK(ext4_link_child("/", source, source, "bad", 3) == ENOTDIR);
    CHECK(ext4_link_child("/", source, new, "target", 6) == EEXIST);
    CHECK(ext4_link_child("/", source, new, "x/y", 3) == EINVAL);
    CHECK(ext4_link_child("/", source, new, ".", 1) == EINVAL);
    CHECK(ext4_transaction_begin("/") == EOK);
    struct ext4_inode_ref capped;
    CHECK(ext4_fs_get_inode_ref(fs, source, &capped) == EOK);
    ext4_inode_set_links_cnt(capped.inode, EXT4_LINK_MAX); capped.dirty = true;
    CHECK(ext4_fs_put_inode_ref(&capped) == EOK);
    CHECK(ext4_link_child("/", source, new, "capped", 6) == EMLINK);
    CHECK(ext4_transaction_end("/") == EMLINK);
    CHECK(links(fs, source) == 1 && !exists("/new/capped"));
    CHECK(ext4_fsymlink("/old/source", "/old/symlink") == EOK);
    uint32_t symlink, symlink_mode;
    CHECK(ext4_lookup_child("/", old, "symlink", 7, &symlink, &symlink_mode) == EOK);
    CHECK(ext4_link_child("/", symlink, new, "symlink", 7) == EOK);
    char link_value[32]; size_t link_length;
    CHECK(ext4_readlink_inode("/", symlink, link_value, sizeof(link_value), &link_length) == EOK);
    CHECK(link_length == 11 && !memcmp(link_value, "/old/source", 11));
    CHECK(links(fs, symlink) == 2 && links(fs, source) == 1);
    CHECK(ext4_fremove("/old/symlink") == EOK);
    CHECK(links(fs, symlink) == 1);
    CHECK(ext4_fremove("/new/symlink") == EOK);
    CHECK(ext4_transaction_begin("/") == EOK);
    CHECK(ext4_link_child("/", source, new, "same", 4) == EOK);
    CHECK(ext4_transaction_abort("/", ECANCELED) == ECANCELED);
    CHECK(!exists("/new/same") && links(fs, source) == 1);
    CHECK(ext4_link_child("/", source, new, "same", 4) == EOK);
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
    CHECK(ext4_link_child("/", victim, new, "alias", 5) == EOK);
    CHECK(rename_entry(old, "source", new, "target", 0, &out) == EOK);
    CHECK(out.replaced_inode == victim && !out.replaced_last_link && out.changed);
    CHECK(links(fs, victim) == 1); content("/new/alias", "TARGET");
    bool orphan; CHECK(ext4_orphan_contains(fs, victim, &orphan) == EOK && !orphan);
    CHECK(ext4_link_child("/", victim, old, "alias", 5) == EOK);
    uint32_t removed; bool dead;
    CHECK(ext4_funlink_dentry("/new/alias", &removed, &dead) == EOK);
    CHECK(removed == victim && !dead && links(fs, victim) == 1);
    content("/old/alias", "TARGET");
    CHECK(ext4_funlink_dentry("/old/alias", &removed, &dead) == EOK && dead);
    CHECK(ext4_link_child("/", victim, old, "resurrect", 9) == ENOENT);
    CHECK(ext4_orphan_free("/", victim) == EOK);

}
static bool lifecycle(const char *kind)
{
    return !strcmp(kind, "unlink-alias") || !strcmp(kind, "rename-alias") ||
           !strcmp(kind, "last-unlink") || !strcmp(kind, "orphan-reclaim");
}
struct link_witness {
    uint64_t free_blocks, owned_blocks;
    uint32_t number, free_inodes;
};
static struct link_witness witness(void)
{
    ext4_file f; size_t count; struct link_witness w;
    CHECK(ext4_fopen(&f, "/link-witness", "r") == EOK);
    CHECK(ext4_fread(&f, &w, sizeof(w), &count) == EOK && count == sizeof(w));
    CHECK(ext4_fclose(&f) == EOK);
    return w;
}
static void seed_links(struct ext4_fs *fs, const char *kind)
{
    CHECK(lifecycle(kind));
    uint32_t source = inode(!strcmp(kind, "rename-alias") ? "/new/target" : "/old/source");
    CHECK(ext4_link_child("/", source, directory("/new"), "alias", 5) == EOK);
    CHECK(links(fs, source) == 2);
    if (!strcmp(kind, "last-unlink") || !strcmp(kind, "orphan-reclaim")) {
        uint32_t removed; bool dead;
        CHECK(ext4_funlink_dentry("/old/source", &removed, &dead) == EOK && removed == source && !dead);
        CHECK(links(fs, source) == 1);
    }
    struct link_witness w = {0};
    ext4_file f; size_t count;
    CHECK(ext4_fopen(&f, "/link-witness", "w+") == EOK);
    CHECK(ext4_fwrite(&f, &w, sizeof(w), &count) == EOK && count == sizeof(w));
    w.number = source;
    w.free_blocks = ext4_sb_get_free_blocks_cnt(&fs->sb);
    w.free_inodes = ext4_get32(&fs->sb, free_inodes_count);
    struct ext4_inode_ref ref;
    CHECK(ext4_fs_get_inode_ref(fs, source, &ref) == EOK);
    w.owned_blocks = ext4_inode_get_blocks_count(&fs->sb, ref.inode) * 512 / fs->bdev->lg_bsize;
    CHECK(w.owned_blocks > 0);
    CHECK(ext4_fs_put_inode_ref(&ref) == EOK);
    CHECK(ext4_fseek(&f, 0, SEEK_SET) == EOK);
    CHECK(ext4_fwrite(&f, &w, sizeof(w), &count) == EOK && count == sizeof(w));
    CHECK(ext4_fclose(&f) == EOK);
    if (!strcmp(kind, "orphan-reclaim")) {
        uint32_t removed; bool dead;
        CHECK(ext4_funlink_dentry("/new/alias", &removed, &dead) == EOK && removed == source && dead);
        bool orphan; CHECK(ext4_orphan_contains(fs, source, &orphan) == EOK && orphan);
        /* 模拟最后 owner 消失前的持久孤儿；不可正常卸载提前完成待测回收。 */
        CHECK(fault_block_crash(&disk) == 0);
    }
}
static void verify_lifecycle(struct ext4_fs *fs, const char *kind, bool old_only)
{
    struct link_witness w = witness();
    bool alias = exists("/new/alias"), orphan;
    CHECK(ext4_orphan_validate(fs) == EOK);
    CHECK(ext4_orphan_contains(fs, w.number, &orphan) == EOK);
    uint32_t pending;
    CHECK(ext4_orphan_peek(fs, &pending) == EOK && pending == (orphan ? w.number : 0));
    CHECK(links(fs, directory("/old")) == 3 && links(fs, directory("/new")) == 3);
    if (!strcmp(kind, "unlink-alias")) {
        if (old_only) CHECK(alias);
        CHECK(inode("/old/source") == w.number && links(fs, w.number) == (alias ? 2U : 1U) && !orphan);
        content("/old/source", "SOURCE");
        if (alias) { CHECK(inode("/new/alias") == w.number); content("/new/alias", "SOURCE"); }
        content("/new/target", "TARGET");
    } else if (!strcmp(kind, "rename-alias")) {
        bool before = exists("/old/source");
        if (old_only) CHECK(before);
        CHECK(alias && inode("/new/alias") == w.number && links(fs, w.number) == (before ? 2U : 1U) && !orphan);
        content("/new/alias", "TARGET");
        if (before) {
            CHECK(inode("/new/target") == w.number);
            content("/old/source", "SOURCE"); content("/new/target", "TARGET");
            CHECK(links(fs, inode("/old/source")) == 1);
        } else {
            uint32_t moved = inode("/new/target");
            CHECK(moved != w.number && links(fs, moved) == 1);
            bool moved_orphan; CHECK(ext4_orphan_contains(fs, moved, &moved_orphan) == EOK && !moved_orphan);
            content("/new/target", "SOURCE");
        }
    } else {
        CHECK(!exists("/old/source")); content("/new/target", "TARGET");
        if (!strcmp(kind, "orphan-reclaim")) CHECK(!alias);
        if (old_only) CHECK(!strcmp(kind, "last-unlink") ? alias : orphan);
        if (alias) {
            CHECK(inode("/new/alias") == w.number && links(fs, w.number) == 1 && !orphan);
            content("/new/alias", "SOURCE");
        } else if (orphan) {
            CHECK(links(fs, w.number) == 0);
            if (!strcmp(kind, "orphan-reclaim")) {
                /* 截块与最终 inode 释放可能分事务；已提交进展仍由 orphan 持有。 */
                struct ext4_inode_ref ref;
                CHECK(ext4_fs_get_inode_ref(fs, w.number, &ref) == EOK);
                uint64_t remaining = ext4_inode_get_blocks_count(&fs->sb, ref.inode) * 512 / fs->bdev->lg_bsize;
                CHECK(ext4_fs_put_inode_ref(&ref) == EOK);
                CHECK(remaining <= w.owned_blocks);
                CHECK(ext4_sb_get_free_blocks_cnt(&fs->sb) == w.free_blocks + w.owned_blocks - remaining);
                CHECK(ext4_get32(&fs->sb, free_inodes_count) == w.free_inodes);
                return;
            }
        } else {
            /* 已释放 inode 的内容不再属于本对象，只核对真实空间与目录守恒。 */
            CHECK(ext4_sb_get_free_blocks_cnt(&fs->sb) == w.free_blocks + w.owned_blocks);
            CHECK(ext4_get32(&fs->sb, free_inodes_count) == w.free_inodes + 1);
            return;
        }
    }
    CHECK(ext4_sb_get_free_blocks_cnt(&fs->sb) == w.free_blocks);
    CHECK(ext4_get32(&fs->sb, free_inodes_count) == w.free_inodes);
}
static int lifecycle_mutate(const char *kind, const struct link_witness *w,
                             uint32_t old, uint32_t new,
                             struct ext4_rename_result *out,
                             uint32_t *removed, bool *dead)
{
    if (!strcmp(kind, "rename-alias"))
        return rename_entry(old, "source", new, "target", 0, out);
    if (!strcmp(kind, "orphan-reclaim")) return ext4_orphan_free("/", w->number);
    return ext4_funlink_dentry("/new/alias", removed, dead);
}

static void verify(struct ext4_fs *fs, const char *kind, bool old_only)
{
    if (lifecycle(kind)) { verify_lifecycle(fs, kind, old_only); return; }
    uint32_t old = directory("/old"), new = directory("/new");
    if (!strcmp(kind, "link") || !strcmp(kind, "link-split")) {
        char path[256];
        snprintf(path, sizeof(path), "/new/%s", !strcmp(kind, "link") ? "linked" : split_name);
        bool linked = exists(path);
        uint32_t source = inode("/old/source");
        if (old_only) CHECK(!linked);
        CHECK(links(fs, source) == (linked ? 2U : 1U));
        content("/old/source", "SOURCE");
        if (linked) { CHECK(inode(path) == source); content(path, "SOURCE"); }
        bool orphan; CHECK(ext4_orphan_contains(fs, source, &orphan) == EOK && !orphan);
        return;
    }
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
    bool reclaim_probe = argc >= 4 && !strcmp(argv[3], "orphan-reclaim") &&
                         strcmp(argv[2], "verify") && strcmp(argv[2], "seed-links");
    if (!reclaim_probe) CHECK(ext4_orphan_recover("/") == EOK);
    if (readonly) {
        struct ext4_rename_result out = { .replaced_inode = 123 };
        CHECK(rename_entry(directory("/old"), "source", directory("/new"), "target", 0, &out) == EROFS);
        CHECK(ext4_link_child("/", inode("/old/source"), directory("/new"), "linked", 6) == EROFS);
        CHECK(out.replaced_inode == 123 && disk.writes == 0);
    } else if (!strcmp(argv[2], "seed")) seed();
    else if (!strcmp(argv[2], "seed-split")) { seed(); seed_split(dev.fs); }
    else if (!strcmp(argv[2], "seed-links")) {
        CHECK(argc >= 4); seed_links(dev.fs, argv[3]);
        if (!strcmp(argv[3], "orphan-reclaim")) return 0;
    }
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
        operation_kind = kind; operation_mode = argv[2];
        if (!strcmp(argv[2], "verify")) verify(dev.fs, kind, false);
        else {
            uint32_t old = directory("/old"), new = directory("/new");
            bool linking = !strcmp(kind, "link") || !strcmp(kind, "link-split");
            uint32_t source = linking ? inode("/old/source") : 0;
            const char *from = !strcmp(kind, "file") ? "source" : "dir";
            const char *to = !strcmp(kind, "link") ? "linked" : !strcmp(kind, "link-split") ? split_name : !strcmp(kind, "file") ? "target" : !strcmp(kind, "split") ? split_name :
                             !strcmp(kind, "insert") ? "moved" : "empty";
            struct link_witness w = {0};
            if (lifecycle(kind)) w = witness();
            uint32_t removed = 123; bool dead = false;
            uint64_t blocks = ext4_sb_get_free_blocks_cnt(&dev.fs->sb);
            uint32_t inodes = ext4_get32(&dev.fs->sb, free_inodes_count);
            allocations = events = allocation_hits = io_hits = 0;
            uint64_t writes_before = disk.writes, flushes_before = disk.flushes;
            if (!strcmp(argv[2], "oom")) fail_allocation = argc > 4 ? strtoul(argv[4], NULL, 10) : 0;
            else if (!strcmp(argv[2], "io-write")) disk.fail_write = disk.writes + (argc > 4 ? strtoul(argv[4], NULL, 10) : 1);
            else if (!strcmp(argv[2], "io-flush")) disk.fail_flush = disk.flushes + (argc > 4 ? strtoul(argv[4], NULL, 10) : 1);
            else { cut = argc > 4 ? strtoul(argv[4], NULL, 10) : 0; reorder = argc > 5 ? atoi(argv[5]) : 0; }
            injected_allocation = fail_allocation;
            struct ext4_rename_result out = { .replaced_inode=123 };
            int r = lifecycle(kind) ? lifecycle_mutate(kind, &w, old, new, &out, &removed, &dead) :
                linking ? ext4_link_child("/", source, new, to, strlen(to)) : rename_entry(old, from, new, to, 0, &out);
            unsigned attempted = allocations;
            fail_allocation = 0;
            if (r == EOK && lifecycle(kind)) {
                if (!strcmp(kind, "rename-alias"))
                    CHECK(out.changed && out.replaced_inode == w.number && !out.replaced_last_link);
                else if (strcmp(kind, "orphan-reclaim"))
                    CHECK(removed == w.number && dead == !strcmp(kind, "last-unlink"));
            }
            if (!strncmp(argv[2], "io-", 3)) {
                disk.fail_write = disk.fail_flush = 0;
                CHECK(io_hits == 1 && r == EIO && out.replaced_inode == 123);
                CHECK(dev.fs->jbd_journal->error == EIO);
                /* 失败后的路径可能已在内存摘除；错误 owner 必须拒绝下一事务。 */
                CHECK((lifecycle(kind) ? ext4_transaction_begin("/") : linking ? ext4_link_child("/", source, new, to, strlen(to)) : rename_entry(old, from, new, to, 0, &out)) == EIO);
                struct ext4_fs *owner = dev.fs;
                CHECK(ext4_umount("/") == EIO && dev.fs == owner);
            } else if (!strcmp(argv[2], "oom")) {
                if (argc > 4 && strtoul(argv[4], NULL, 10)) CHECK(allocation_hits == 1);
                if (r != EOK) {
                    CHECK(r == ENOMEM); CHECK(out.replaced_inode == 123);
                    CHECK(dev.fs->jbd_journal->error == EOK);
                    if (strcmp(kind, "orphan-reclaim")) {
                        CHECK(ext4_sb_get_free_blocks_cnt(&dev.fs->sb) == blocks);
                        CHECK(ext4_get32(&dev.fs->sb, free_inodes_count) == inodes);
                    }
                    verify(dev.fs, kind, true);
                } else verify(dev.fs, kind, false);
                printf("%u\n", attempted);
            } else { CHECK(r == EOK); printf("%u %llu %llu\n", events,
                (unsigned long long)(disk.writes - writes_before), (unsigned long long)(disk.flushes - flushes_before)); }
            CHECK(fault_block_crash(&disk) == 0);
            return 0;
        }
    }
    CHECK(ext4_umount("/") == EOK);
    return 0;
}
