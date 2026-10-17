#include "../../fs/tmpfs.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

void mm_test_attach(unsigned);
static unsigned char pool[16 * 1024 * 1024] __attribute__((aligned(16384)));
static struct physical_page_allocator pages;
static struct kernel_heap heap;
static unsigned fail_allocation;
static uint64_t clock_ns;

void kernel_console_putc(char c) { fputc(c, stderr); }
void mm_test_unavailable(void) { abort(); }
void mm_test_fence(uint64_t v, int i) { (void)v; (void)i; }
static void *access_page(uint64_t p) { return (void *)(uintptr_t)p; }
static int page_address(const void *p, uint64_t *out) { *out = (uintptr_t)p; return 1; }
uint64_t kernel_time_realtime_ns(void) { return ++clock_ns; }
uint64_t kernel_vfs_allocate_mount_id(void) { static uint64_t id; return ++id; }

enum kernel_heap_status __real_kernel_heap_allocate_zeroed(struct kernel_heap *, size_t, size_t, void **);
enum kernel_heap_status __wrap_kernel_heap_allocate_zeroed(struct kernel_heap *h,
    size_t count, size_t size, void **out)
{
    if (fail_allocation && !--fail_allocation) return KERNEL_HEAP_STATUS_EMPTY;
    return __real_kernel_heap_allocate_zeroed(h, count, size, out);
}

/* Only VFS publication is adapted: tmpfs, memory objects, heap and physical
 * pages are production implementations. Full path integration is tested by
 * the file-I/O and guest suites. */
struct kernel_vfs_node *kernel_vfs_node_find(struct kernel_vfs_instance *i, uint64_t id)
{
    for (struct kernel_vfs_node *n = i->nodes; n; n = n->next)
        if (n->inode == id && !n->retired && !n->closed) return n;
    return 0;
}
void kernel_vfs_node_lock(struct kernel_vfs_node *n, struct kernel_lock_guard *g, int write)
{
    if (write) kernel_rwlock_write(&n->io_lock, g);
    else kernel_rwlock_read(&n->io_lock, g);
}
int kernel_vfs_publish_node(struct kernel_vfs_mount *m, struct kernel_vfs_node *n,
    struct kernel_vfs_file *f, int creating)
{
    (void)creating;
    struct kernel_vfs_instance *i = m->private_data;
    kernel_rwlock_init(&n->io_lock, 30, (uintptr_t)n);
    n->instance = i; n->mount = m; n->references = n->open_files = 1;
    n->next = i->nodes; i->nodes = n; i->external_files++;
    *f = (struct kernel_vfs_file){.mount = m, .private_data = n,
        .mode = n->mode, .state = VFS_FILE_STATE_LIVE};
    return 0;
}
static const struct kernel_vfs_backend *ops(struct kernel_vfs_mount *m)
{ return ((struct kernel_vfs_instance *)m->private_data)->ops; }
static struct kernel_vfs_instance *instance(struct kernel_vfs_mount *m) { return m->private_data; }
static void close_file(struct kernel_vfs_file *f)
{
    struct kernel_vfs_node *n = f->private_data;
    struct kernel_vfs_instance *i = n->instance;
    struct kernel_vfs_node **p = &i->nodes;
    while (*p != n) { assert(*p); p = &(*p)->next; }
    *p = n->next; i->external_files--; n->open_files--;
    if (n->unlinked) i->ops->release_unlinked(n);
    assert(!i->ops->close_node(n));
    kernel_heap_release(i->heap, n); *f = (struct kernel_vfs_file){0};
}
static void open_id(struct kernel_vfs_mount *m, uint64_t id, struct kernel_vfs_file *f)
{ assert(!ops(m)->open(m, 0, id, 0, f)); }
static uint64_t find(struct kernel_vfs_mount *m, uint64_t parent, const char *name, uint32_t *mode)
{
    uint64_t id; uint32_t ignored;
    assert(!ops(m)->lookup(instance(m), parent, name, strlen(name), &id, mode ? mode : &ignored));
    return id;
}
static void absent(struct kernel_vfs_mount *m, uint64_t parent, const char *name)
{
    uint64_t id; uint32_t mode;
    assert(ops(m)->lookup(instance(m), parent, name, strlen(name), &id, &mode) == -KERNEL_ENOENT);
}
static void regular(struct kernel_vfs_mount *m, const char *path)
{ assert(!ops(m)->mknod(instance(m), path, KERNEL_VFS_S_IFREG, 0600, 0)); }
static struct kernel_vfs_stat stat_id(struct kernel_vfs_mount *m, uint64_t id)
{
    struct kernel_vfs_file f = {0}; struct kernel_vfs_stat st;
    open_id(m, id, &f); assert(!ops(m)->stat(&f, &st)); close_file(&f); return st;
}
static struct kernel_vfs_statfs space(struct kernel_vfs_mount *m)
{ struct kernel_vfs_statfs s; assert(!ops(m)->statfs(m, &s)); return s; }
static uint8_t expect_entry(struct kernel_vfs_file *f, uint64_t *pos, const char *expected, uint64_t id)
{
    char name[256]; uint8_t type; uint64_t next, found;
    assert(ops(f->mount)->dir_entry(f, *pos, &next, &found, &type, name, sizeof(name)) == 1);
    assert(!strcmp(name, expected) && found == id && next > *pos); *pos = next; return type;
}
static void expect_end(struct kernel_vfs_file *f, uint64_t pos)
{
    char name[256]; uint8_t type; uint64_t next, found;
    assert(!ops(f->mount)->dir_entry(f, pos, &next, &found, &type, name, sizeof(name)));
}

static void mixed_directories(void)
{
    enum { DIRS = 16, ENTRIES = 160 };
    const uint32_t types[] = {KERNEL_VFS_S_IFDIR, KERNEL_VFS_S_IFREG,
        KERNEL_VFS_S_IFIFO, KERNEL_VFS_S_IFLNK};
    struct kernel_vfs_mount *m = 0;
    assert(!kernel_tmpfs_create(&heap, 0, "nr_inodes=0", &m));
    uint64_t dirs[DIRS], ids[DIRS][ENTRIES]; char path[64], name[32];
    for (unsigned d = 0; d < DIRS; d++) {
        snprintf(path, sizeof(path), "/dir%02u", d); assert(!ops(m)->mkdir(m, path, 0755));
        dirs[d] = find(m, 1, path + 1, 0);
    }
    /* Interleave directories and permute lexical order, forcing both lookup
     * ordering and cookie ordering to differ from allocation order. */
    for (unsigned j = 0; j < ENTRIES; j++) for (unsigned d = 0; d < DIRS; d++) {
        unsigned key = (j * 73) % ENTRIES;
        snprintf(name, sizeof(name), "entry%03u%s", key, key & 1 ? "-tail" : "");
        snprintf(path, sizeof(path), "/dir%02u/%s", d, name);
        if (key % 4 == 0) assert(!ops(m)->mkdir(m, path, 0700));
        else if (key % 4 == 3) assert(!ops(m)->symlink(instance(m), "../target", path));
        else assert(!ops(m)->mknod(instance(m), path, types[key % 4], 0600, 0));
        uint32_t mode; ids[d][key] = find(m, dirs[d], name, &mode);
        assert((mode & KERNEL_VFS_S_IFMT) == types[key % 4]);
    }
    for (unsigned d = 0; d < DIRS; d++) {
        struct kernel_vfs_file directory = {0}; open_id(m, dirs[d], &directory);
        uint64_t pos = 0; expect_entry(&directory, &pos, ".", dirs[d]);
        expect_entry(&directory, &pos, "..", 1);
        for (unsigned j = 0; j < ENTRIES; j++) {
            unsigned key = (j * 73) % ENTRIES;
            snprintf(name, sizeof(name), "entry%03u%s", key, key & 1 ? "-tail" : "");
            uint32_t mode; assert(find(m, dirs[d], name, &mode) == ids[d][key]);
            assert(expect_entry(&directory, &pos, name, ids[d][key]) == types[key % 4] >> 12);
        }
        expect_end(&directory, pos); close_file(&directory);
        struct kernel_vfs_stat st = stat_id(m, dirs[d]);
        assert(st.size == (ENTRIES + 2) * 20 && st.nlink == ENTRIES / 4 + 2);
    }
    assert(ops(m)->rmdir(m, "/dir00") == -KERNEL_ENOTEMPTY);
    for (unsigned j = ENTRIES; j-- > 0;) for (unsigned d = 0; d < DIRS; d++) {
        unsigned key = (j * 73) % ENTRIES;
        if (key % 3) continue;
        snprintf(name, sizeof(name), "entry%03u%s", key, key & 1 ? "-tail" : "");
        snprintf(path, sizeof(path), "/dir%02u/%s", d, name);
        assert(!(key % 4 == 0 ? ops(m)->rmdir(m, path) : ops(m)->unlink(m, path)));
        absent(m, dirs[d], name);
    }
    for (unsigned d = 0; d < DIRS; d++) {
        struct kernel_vfs_file directory = {0}; open_id(m, dirs[d], &directory);
        uint64_t pos = 2; unsigned remaining = 0, subdirs = 0;
        for (unsigned j = 0; j < ENTRIES; j++) {
            unsigned key = (j * 73) % ENTRIES;
            if (!(key % 3)) continue;
            snprintf(name, sizeof(name), "entry%03u%s", key, key & 1 ? "-tail" : "");
            expect_entry(&directory, &pos, name, ids[d][key]); remaining++; subdirs += !(key % 4);
        }
        expect_end(&directory, pos); close_file(&directory);
        struct kernel_vfs_stat st = stat_id(m, dirs[d]);
        assert(st.size == (remaining + 2) * 20 && st.nlink == subdirs + 2);
    }
    assert(!ops(m)->link(instance(m), ids[5][1], 1, "root-alias"));
    assert(!ops(m)->link(instance(m), ids[5][1], dirs[0], "directory-alias"));
    assert(stat_id(m, ids[5][1]).nlink == 3);
    assert(!ops(m)->unmount(m));
    puts("PASS tmpfs mixed directories: lookup, inode opens, cookie enumeration, sizes, links and teardown");
}

static void cookies_and_rename(void)
{
    struct kernel_vfs_mount *m = 0; assert(!kernel_tmpfs_create(&heap, 0, 0, &m));
    assert(!ops(m)->mkdir(m, "/left", 0755) && !ops(m)->mkdir(m, "/right", 0755));
    uint64_t left = find(m, 1, "left", 0), right = find(m, 1, "right", 0);
    regular(m, "/left/alpha"); regular(m, "/left/beta"); regular(m, "/left/gamma");
    uint64_t alpha = find(m, left, "alpha", 0), gamma = find(m, left, "gamma", 0);
    struct kernel_vfs_file dir = {0}, saved = {0}; open_id(m, left, &dir); open_id(m, alpha, &saved);
    uint64_t pos = 2; expect_entry(&dir, &pos, "alpha", alpha); uint64_t resume = pos;
    assert(!ops(m)->unlink(m, "/left/beta"));
    struct kernel_vfs_rename_result result = {0};
    assert(!ops(m)->rename(instance(m), left, "alpha", left, "zeta", 0, &result) && result.changed);
    result = (struct kernel_vfs_rename_result){0};
    assert(!ops(m)->rename(instance(m), left, "gamma", right, "moved", 0, &result) && result.changed);
    regular(m, "/left/delta"); uint64_t delta = find(m, left, "delta", 0);
    for (unsigned seek = 0; seek < 2; seek++) {
        pos = resume; expect_entry(&dir, &pos, "zeta", alpha); expect_entry(&dir, &pos, "delta", delta);
        expect_end(&dir, pos);
    }
    regular(m, "/left/epsilon"); uint64_t epsilon = find(m, left, "epsilon", 0);
    expect_entry(&dir, &pos, "epsilon", epsilon); expect_end(&dir, pos);
    result = (struct kernel_vfs_rename_result){0}; fail_allocation = 1;
    assert(ops(m)->rename(instance(m), right, "moved", left, "zeta", 0, &result) == -KERNEL_ENOMEM);
    assert(!result.changed && find(m, left, "zeta", 0) == alpha && find(m, right, "moved", 0) == gamma);
    assert(!ops(m)->rename(instance(m), right, "moved", left, "zeta", 0, &result));
    assert(result.changed && result.replaced_inode == alpha && result.replaced_last_link);
    struct kernel_vfs_stat st; assert(!ops(m)->stat(&saved, &st) && st.ino == alpha && !st.nlink);
    assert(find(m, left, "zeta", 0) == gamma); absent(m, right, "moved");
    assert(!ops(m)->mkdir(m, "/left/sub", 0755)); uint64_t sub = find(m, left, "sub", 0);
    result = (struct kernel_vfs_rename_result){0};
    assert(ops(m)->rename(instance(m), 1, "left", sub, "cycle", 0, &result) == -KERNEL_EINVAL);
    assert(!ops(m)->rename(instance(m), left, "sub", right, "sub", 0, &result));
    struct kernel_vfs_file child = {0}; open_id(m, sub, &child); uint64_t dotdot = 1;
    expect_entry(&child, &dotdot, "..", right); close_file(&child);
    assert(stat_id(m, left).nlink == 2 && stat_id(m, right).nlink == 3);
    assert(!ops(m)->mkdir(m, "/right/empty", 0755)); uint64_t replaced = find(m, right, "empty", 0);
    result = (struct kernel_vfs_rename_result){0};
    assert(!ops(m)->rename(instance(m), right, "sub", right, "empty", 0, &result));
    assert(result.replaced_inode == replaced && result.replaced_last_link && find(m, right, "empty", 0) == sub);
    assert(stat_id(m, right).nlink == 3 && !ops(m)->rmdir(m, "/right/empty"));
    close_file(&saved); close_file(&dir); assert(!ops(m)->unmount(m));
    puts("PASS tmpfs cookie seek/resume across insert, remove and rename; replacement owners and directory parent links");
}

static void rollback_and_budgets(void)
{
    struct kernel_vfs_mount *m = 0;
    assert(!kernel_tmpfs_create(&heap, 0, "nr_inodes=4,nr_blocks=1", &m));
    struct kernel_heap_statistics before, after;
    struct kernel_vfs_statfs initial = space(m);
    for (unsigned phase = 1;; phase++) {
        kernel_heap_get_statistics(&heap, &before); struct kernel_vfs_file f = {0};
        fail_allocation = phase;
        int status = ops(m)->create(m, "/failed", 0600, &f);
        if (!status) {
            fail_allocation = 0; close_file(&f); assert(!ops(m)->unlink(m, "/failed")); break;
        }
        assert(status == -KERNEL_ENOMEM && !f.private_data);
        absent(m, 1, "failed"); assert(space(m).free_inodes == initial.free_inodes);
        kernel_heap_get_statistics(&heap, &after); assert(after.live_allocations == before.live_allocations);
        assert(stat_id(m, 1).size == 40);
    }
    struct kernel_vfs_file held = {0}; assert(!ops(m)->create(m, "/data", 0600, &held));
    struct kernel_vfs_node *node = held.private_data;
    uint64_t address; int created;
    assert(!kernel_memory_object_get_page(node->memory, 0, &address, &created) && created);
    memset(access_page(address), 0x5a, BOAROS_PAGE_SIZE); assert(!physical_page_release(&pages, address));
    ops(m)->memory_written(node, BOAROS_PAGE_SIZE);
    assert(space(m).free_blocks == 0 && pages.shared_anon_pages == 1);
    assert(!ops(m)->link(instance(m), node->inode, 1, "alias"));
    struct kernel_vfs_rename_result same = {0};
    assert(!ops(m)->rename(instance(m), 1, "data", 1, "alias", 0, &same) && !same.changed);
    assert(find(m, 1, "data", 0) == node->inode && find(m, 1, "alias", 0) == node->inode);
    fail_allocation = 1;
    assert(ops(m)->link(instance(m), node->inode, 1, "failed-link") == -KERNEL_ENOMEM);
    absent(m, 1, "failed-link"); assert(space(m).free_inodes == 1);
    regular(m, "/other");
    assert(ops(m)->link(instance(m), node->inode, 1, "full") == -KERNEL_ENOSPC);
    struct kernel_vfs_file alias = {0}; open_id(m, find(m, 1, "alias", 0), &alias);
    assert(((struct kernel_vfs_node *)alias.private_data)->memory == node->memory);
    struct kernel_vfs_stat st; assert(!ops(m)->stat(&held, &st) && st.nlink == 2);
    assert(!ops(m)->unlink(m, "/data") && space(m).free_inodes == 1);
    assert(!ops(m)->unlink(m, "/alias") && space(m).free_inodes == 1);
    unsigned char bytes[8]; size_t read;
    assert(!ops(m)->pread(node, 0, bytes, sizeof(bytes), &read) && read == sizeof(bytes));
    for (unsigned j = 0; j < sizeof(bytes); j++) assert(bytes[j] == 0x5a);
    assert(ops(m)->unmount(m) == -KERNEL_EBUSY);
    close_file(&alias); assert(space(m).free_blocks == 0); close_file(&held);
    assert(space(m).free_blocks == 1 && space(m).free_inodes == 2 && !pages.shared_anon_pages);
    char target[160]; memset(target, 'x', sizeof(target) - 1); target[sizeof(target) - 1] = 0;
    assert(!ops(m)->symlink(instance(m), target, "/long-link"));
    assert(ops(m)->symlink(instance(m), target, "/over-budget") == -KERNEL_ENOSPC);
    absent(m, 1, "over-budget"); assert(space(m).free_inodes == 1);
    assert(!ops(m)->unmount(m) && !pages.shared_anon_pages);
    puts("PASS tmpfs create/link OOM rollback, hardlink quota, deleted open inode/page ownership and long symlink quota cleanup");
}

static void cookie_exhaustion(void)
{
    struct kernel_vfs_mount *m = 0; assert(!kernel_tmpfs_create(&heap, 0, 0, &m));
    regular(m, "/old"); regular(m, "/destination");
    uint64_t old = find(m, 1, "old", 0), destination = find(m, 1, "destination", 0);
    /* Exhaustion cannot be reached by a finite practical test workload. */
    tm(instance(m))->next_cookie = UINT64_MAX;
    struct kernel_vfs_rename_result result = {0};
    assert(ops(m)->rename(instance(m), 1, "old", 1, "destination", 0, &result) == -KERNEL_EOVERFLOW);
    assert(!result.changed && find(m, 1, "old", 0) == old && find(m, 1, "destination", 0) == destination);
    assert(ops(m)->mknod(instance(m), "/new", KERNEL_VFS_S_IFREG, 0600, 0) == -KERNEL_EOVERFLOW);
    assert(ops(m)->link(instance(m), old, 1, "new") == -KERNEL_EOVERFLOW);
    assert(!ops(m)->unmount(m));
    puts("PASS tmpfs exhausted cookies reject create/link/rename without replacing names");
}

int main(void)
{
    mm_test_attach(0); struct boot_memory_layout layout = {0}; layout.usable_count = 1;
    layout.usable[0].base = (uintptr_t)pool; layout.usable[0].size = sizeof(pool);
    assert(!physical_page_allocator_init(&pages, &layout));
    assert(!physical_page_allocator_bind_access(&pages, access_page));
    assert(!physical_page_allocator_finalize(&pages) && !kernel_heap_init(&heap, &pages, page_address));
    uint64_t baseline = physical_page_available(&pages);
    mixed_directories(); cookies_and_rename(); rollback_and_budgets(); cookie_exhaustion();
    struct kernel_heap_statistics s; kernel_heap_get_statistics(&heap, &s);
    assert(!s.live_allocations && !s.current_pages && !pages.shared_anon_pages);
    assert(physical_page_available(&pages) == baseline && !physical_page_allocator_audit(&pages));
    puts("PASS tmpfs final heap, page and mount owners returned");
}
