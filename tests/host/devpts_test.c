#define BOAROS_ARCH_RISCV_CONTEXT_H
#include <stdint.h>
static inline uintptr_t riscv_interrupt_save(void) { return 0; }
static inline void riscv_interrupt_restore(uintptr_t flags) { (void)flags; }
#include "../../fs/devpts.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static size_t live_allocations;
static int allocation_budget = -1, start_error, stop_error;
static uint64_t clock_ns;

enum kernel_heap_status kernel_heap_allocate_zeroed(struct kernel_heap *heap,
    size_t count, size_t size, void **owner)
{
    (void)heap;
    if (!allocation_budget) return KERNEL_HEAP_STATUS_EMPTY;
    if (allocation_budget > 0) allocation_budget--;
    *owner = calloc(count, size);
    if (!*owner) return KERNEL_HEAP_STATUS_EMPTY;
    live_allocations++;
    return KERNEL_HEAP_STATUS_OK;
}
enum kernel_heap_status kernel_heap_release(struct kernel_heap *heap, void *owner)
{
    (void)heap;
    assert(owner && live_allocations);
    live_allocations--;
    free(owner);
    return KERNEL_HEAP_STATUS_OK;
}
uint64_t kernel_time_realtime_ns(void) { return ++clock_ns; }
uint64_t kernel_vfs_allocate_mount_id(void)
{ static uint64_t id; return ++id; }
void kernel_rwlock_init(struct kernel_rwlock *lock, uint32_t rank, uintptr_t key)
{ *lock = (struct kernel_rwlock){.rank = rank, .key = key}; }
void kernel_rwlock_write(struct kernel_rwlock *lock, struct kernel_lock_guard *g)
{ assert(!g->lock); g->lock = lock; }
void kernel_lock_release(struct kernel_lock_guard *g) { g->lock = 0; }
void kernel_vfs_namespace_lock(struct kernel_vfs_mount *m, struct kernel_lock_guard *g)
{ kernel_mutex_lock(&((struct kernel_vfs_instance *)m->private_data)->namespace_lock, g); }
void kernel_vfs_node_lock(struct kernel_vfs_node *n, struct kernel_lock_guard *g, int w)
{ (void)w; kernel_rwlock_write(&n->io_lock, g); }
struct kernel_vfs_node *kernel_vfs_file_node(const struct kernel_vfs_file *f)
{ return f && f->state == VFS_FILE_STATE_LIVE ? f->private_data : 0; }
int kernel_vfs_path_acquire(struct kernel_vfs_path *p)
{ p->references++; return 0; }
int kernel_pty_mount_start(struct kernel_vfs_mount *m)
{ (void)m; return start_error; }
int kernel_pty_mount_stop(struct kernel_vfs_mount *m)
{ (void)m; return stop_error; }

/* Host adapter holds real backend node/entry owners; path/tree integration is
 * separately exercised by the real RISC-V VFS and U-mode tests. */
int kernel_vfs_publish_node(struct kernel_vfs_mount *m, struct kernel_vfs_node *n,
    struct kernel_vfs_file *f, int creating)
{
    (void)creating;
    struct kernel_vfs_instance *i = m->private_data;
    n->instance = i; n->mount = m; n->references = n->open_files = 1;
    n->next = i->nodes; i->nodes = n; i->external_files++;
    *f = (struct kernel_vfs_file){.private_data = n, .mount = m,
        .mode = n->mode, .state = VFS_FILE_STATE_LIVE};
    return 0;
}
static void close_file(struct kernel_vfs_file *f)
{
    struct kernel_vfs_node *n = f->private_data;
    struct kernel_vfs_instance *i = n->instance;
    struct kernel_vfs_node **p = &i->nodes;
    while (*p != n) { assert(*p); p = &(*p)->next; }
    *p = n->next; i->external_files--; n->open_files--;
    if (n->unlinked) {
        assert(i->ops->release_unlinked);
        i->ops->release_unlinked(n);
    }
    assert(!i->ops->close_node(n));
    assert(kernel_heap_release(i->heap, n) == KERNEL_HEAP_STATUS_OK);
    *f = (struct kernel_vfs_file){0};
}
static void open_id(struct kernel_vfs_mount *m, uint64_t ino,
    struct kernel_vfs_file *f)
{ struct kernel_vfs_instance *i = m->private_data; assert(!i->ops->open(m, 0, ino, 0, f)); }
static void destroy(struct kernel_vfs_mount *m)
{ assert(!((struct kernel_vfs_instance *)m->private_data)->ops->unmount(m)); }
static uint64_t find(struct kernel_vfs_mount *m, const char *name)
{
    uint64_t id; uint32_t mode;
    struct kernel_vfs_instance *i = m->private_data;
    assert(!i->ops->lookup(i, 1, name, strlen(name), &id, &mode));
    return id;
}

static void options_and_isolation(struct kernel_heap *heap)
{
    struct kernel_vfs_mount *a = 0, *b = 0;
    assert(!kernel_devpts_create(heap, 0, 0, &a));
    assert(!kernel_devpts_create(heap, 0,
        "uid=17,gid=23,mode=0620,ptmxmode=0666,max=2,newinstance", &b));
    struct kernel_vfs_file root = {0}, ptmx = {0};
    open_id(a, 1, &root); open_id(a, find(a, "ptmx"), &ptmx);
    struct kernel_vfs_stat st;
    assert(!((struct kernel_vfs_instance *)a->private_data)->ops->stat(&ptmx, &st));
    assert(st.ino == 2 && st.rdev == 0x502 && st.mode == (KERNEL_VFS_S_IFCHR | 0000));
    struct kernel_vfs_statfs fs;
    assert(!((struct kernel_vfs_instance *)a->private_data)->ops->statfs(a, &fs));
    assert(fs.type == 0x1cd1 && fs.block_size == 4096 && fs.name_length == 255);
    assert(kernel_devpts_file_mount(&ptmx) == a && kernel_devpts_file_is_ptmx(&ptmx));
    assert(!kernel_devpts_file_entry(&ptmx) && !kernel_devpts_file_is_ptmx(&root));
    uint64_t next, ino; uint8_t type; char name[20];
    const char *names[] = {".", "..", "ptmx"};
    struct kernel_vfs_instance *i = a->private_data;
    for (unsigned pos = 0; pos < 3; pos++) {
        assert(i->ops->dir_entry(&root, pos, &next, &ino, &type, name, sizeof(name)) == 1);
        assert(!strcmp(name, names[pos]) && next == pos + 1);
    }
    assert(!i->ops->dir_entry(&root, 3, &next, &ino, &type, name, sizeof(name)));
    assert(i->ops->unmount(a) == -KERNEL_EBUSY);
    struct kernel_devpts_entry *ea = 0, *eb = 0, *eb1 = 0, *full = 0;
    int binding_a, binding_b;
    assert(!kernel_devpts_publish(a, &binding_a, &ea));
    assert(!kernel_devpts_publish(b, &binding_b, &eb));
    assert(!kernel_devpts_publish(b, &binding_b, &eb1));
    assert(kernel_devpts_publish(b, &binding_b, &full) == -KERNEL_ENOSPC && !full);
    assert(kernel_devpts_entry_number(ea) == 0 && kernel_devpts_entry_number(eb) == 0);
    struct kernel_vfs_file slave = {0}; open_id(b, find(b, "0"), &slave);
    assert(!((struct kernel_vfs_instance *)b->private_data)->ops->stat(&slave, &st));
    assert(st.uid == 17 && st.gid == 23 && st.rdev == 0x8800 &&
        st.mode == (KERNEL_VFS_S_IFCHR | 0620));
    assert(kernel_devpts_file_entry(&slave) == eb && kernel_devpts_entry_binding(eb) == &binding_b);
    struct kernel_vfs_instance *mutations = b->private_data;
    assert(mutations->ops->mkdir && mutations->ops->unlink && mutations->ops->rmdir);
    assert(mutations->ops->mkdir(b, "/new", 0755) == -KERNEL_EPERM);
    assert(mutations->ops->unlink(b, "/0") == -KERNEL_EPERM);
    assert(mutations->ops->rmdir(b, "/new") == -KERNEL_EPERM);
    assert(mutations->ops->create && mutations->ops->symlink && mutations->ops->mknod);
    struct kernel_vfs_file denied = {0};
    assert(mutations->ops->create(b, "/new", 0600, &denied) == -KERNEL_EACCES);
    assert(mutations->ops->symlink(mutations, "0", "/link") == -KERNEL_EPERM);
    assert(mutations->ops->mknod(mutations, "/node", KERNEL_VFS_S_IFCHR, 0600, 0x103) == -KERNEL_EPERM);
    assert(find(b, "0") == st.ino && kernel_devpts_entry_binding(eb) == &binding_b);
    close_file(&slave); close_file(&ptmx); close_file(&root);
    kernel_devpts_retire(ea); kernel_devpts_entry_release(&ea);
    kernel_devpts_retire(eb); kernel_devpts_entry_release(&eb);
    kernel_devpts_retire(eb1); kernel_devpts_entry_release(&eb1);
    destroy(a); destroy(b);
}

static void stable_identity_and_metadata(struct kernel_heap *heap)
{
    struct kernel_vfs_mount *m = 0;
    assert(!kernel_devpts_create(heap, 0, "max=1", &m));
    int old_pair, new_pair;
    struct kernel_devpts_entry *old = 0, *new = 0;
    assert(!kernel_devpts_publish(m, &old_pair, &old));
    uint64_t old_ino = find(m, "0");
    struct kernel_vfs_file saved = {0}; open_id(m, old_ino, &saved);
    struct kernel_vfs_instance *i = m->private_data;
    assert(!i->ops->set_mode(&saved, 0670));
    assert(!i->ops->set_owner(&saved, 31, 41));
    struct kernel_vfs_timespec ts[2] = {{7, 8}, {9, 10}};
    assert(!i->ops->set_times(&saved, ts));
    kernel_devpts_unpublish(old);
    assert(!((struct kernel_vfs_node *)saved.private_data)->retired);
    uint64_t id; uint32_t mode;
    assert(i->ops->lookup(i, 1, "0", 1, &id, &mode) == -KERNEL_ENOENT);
    assert(!kernel_devpts_entry_binding(old));
    assert(kernel_devpts_publish(m, &new_pair, &new) == -KERNEL_ENOSPC);
    kernel_devpts_retire(old); kernel_devpts_entry_release(&old);
    assert(!kernel_devpts_publish(m, &new_pair, &new));
    assert(kernel_devpts_entry_number(new) == 0 && find(m, "0") != old_ino);
    struct kernel_vfs_stat st;
    assert(!i->ops->stat(&saved, &st));
    assert(st.ino == old_ino && st.nlink == 0 && st.mode == (KERNEL_VFS_S_IFCHR | 0670));
    assert(st.uid == 31 && st.gid == 41 && st.atime.seconds == 7 && st.mtime.nanoseconds == 10);
    assert(!kernel_devpts_entry_binding(kernel_devpts_file_entry(&saved)));
    close_file(&saved);
    kernel_devpts_retire(new); kernel_devpts_entry_release(&new);
    destroy(m);
}

static void parsing_and_rollback(struct kernel_heap *heap)
{
    const char *bad[] = {"bogus", "mode=08", "ptmxmode=", "uid=-1", "gid=4294967295",
        "uid=4294967296", "max=-1", "max=-+0", "max=1048577", "newinstance=1", "mode"};
    for (size_t n = 0; n < sizeof(bad) / sizeof(*bad); n++) {
        struct kernel_vfs_mount *m = 0; size_t before = live_allocations;
        assert(kernel_devpts_create(heap, 0, bad[n], &m) == -KERNEL_EINVAL);
        assert(!m && live_allocations == before);
    }
    struct kernel_vfs_mount *m = 0;
    assert(!kernel_devpts_create(heap, 0,
        ",uid=0x11,,gid=027,mode=10620,ptmxmode=+666,max=0x2,newinstance,", &m));
    int radix_pair; struct kernel_devpts_entry *radix_entry = 0;
    assert(!kernel_devpts_publish(m, &radix_pair, &radix_entry));
    struct kernel_vfs_file radix = {0}; open_id(m, find(m, "0"), &radix);
    struct kernel_vfs_stat st;
    assert(!((struct kernel_vfs_instance *)m->private_data)->ops->stat(&radix, &st));
    assert(st.uid == 17 && st.gid == 23 && st.mode == (KERNEL_VFS_S_IFCHR | 0620));
    close_file(&radix); kernel_devpts_retire(radix_entry);
    kernel_devpts_entry_release(&radix_entry); destroy(m); m = 0;
    allocation_budget = 0;
    assert(kernel_devpts_create(heap, 0, 0, &m) == -KERNEL_ENOMEM && !m);
    allocation_budget = -1; start_error = -KERNEL_ENOMEM;
    assert(kernel_devpts_create(heap, 0, 0, &m) == -KERNEL_ENOMEM && !m && !live_allocations);
    start_error = 0;
    assert(!kernel_devpts_create(heap, 0, "max=0", &m));
    int pair; struct kernel_devpts_entry *entry = 0;
    allocation_budget = 0;
    assert(kernel_devpts_publish(m, &pair, &entry) == -KERNEL_ENOSPC);
    allocation_budget = -1; destroy(m); m = 0;
    assert(!kernel_devpts_create(heap, 0, "max=65", &m));
    size_t before = live_allocations;
    allocation_budget = 0;
    assert(kernel_devpts_publish(m, &pair, &entry) == -KERNEL_ENOMEM && !entry);
    allocation_budget = -1;
    assert(live_allocations == before && !kernel_devpts_publish(m, &pair, &entry));
    assert(kernel_devpts_entry_number(entry) == 0);
    uint64_t ino = find(m, "0");
    struct kernel_vfs_file failed_open = {0};
    struct kernel_vfs_instance *instance = m->private_data;
    size_t published_allocations = live_allocations;
    allocation_budget = 0;
    assert(instance->ops->open(m, 0, ino, 0, &failed_open) == -KERNEL_ENOMEM);
    assert(!failed_open.private_data && live_allocations == published_allocations);
    allocation_budget = -1;
    open_id(m, ino, &failed_open); close_file(&failed_open);
    kernel_devpts_retire(entry); kernel_devpts_entry_release(&entry);
    stop_error = -KERNEL_EBUSY;
    assert(((struct kernel_vfs_instance *)m->private_data)->ops->unmount(m) == -KERNEL_EBUSY);
    assert(live_allocations == before); stop_error = 0; destroy(m);
}

static void readonly_and_root_owner(struct kernel_heap *heap)
{
    struct kernel_vfs_mount *m = 0;
    assert(!kernel_devpts_create(heap, 1, "ptmxmode=0666", &m));
    struct kernel_vfs_path *pin = 0;
    assert(kernel_devpts_mount_root(m, &pin) == -KERNEL_ENODEV && !pin);
    struct kernel_vfs_path attached_root = {.references = 1};
    m->root_path = &attached_root;
    assert(!kernel_devpts_mount_root(m, &pin) && pin == &attached_root && pin->references == 2);
    attached_root.references--; pin = 0;
    m->root_path = 0;
    struct kernel_devpts_entry *entry = 0; int pair;
    assert(!kernel_devpts_publish(m, &pair, &entry));
    struct kernel_vfs_file slave = {0}; open_id(m, find(m, "0"), &slave);
    struct kernel_vfs_instance *i = m->private_data;
    assert(i->ops->set_mode(&slave, 0666) == -KERNEL_EROFS);
    assert(i->ops->set_owner(&slave, 10, 20) == -KERNEL_EROFS);
    assert(i->ops->set_times(&slave, 0) == -KERNEL_EROFS);
    struct kernel_vfs_stat st;
    assert(!i->ops->stat(&slave, &st) && st.mode == (KERNEL_VFS_S_IFCHR | 0600));
    kernel_devpts_unpublish(entry); close_file(&slave);
    kernel_devpts_retire(entry); kernel_devpts_entry_release(&entry);
    destroy(m);
}

static void full_number_range(struct kernel_heap *heap)
{
    const char *options[] = {0, "max=1048576"};
    const unsigned limits[] = {32, 64};
    for (unsigned pass = 0; pass < 2; pass++) {
        struct kernel_vfs_mount *m = 0;
        struct kernel_devpts_entry *entries[64] = {0}, *extra = 0;
        int pair;
        assert(!kernel_devpts_create(heap, 0, options[pass], &m));
        for (unsigned j = 0; j < limits[pass]; j++) {
            assert(!kernel_devpts_publish(m, &pair, &entries[j]));
            assert(kernel_devpts_entry_number(entries[j]) == j);
        }
        assert(kernel_devpts_publish(m, &pair, &extra) == -KERNEL_ENOSPC && !extra);
        for (unsigned j = 0; j < limits[pass]; j++) {
            kernel_devpts_retire(entries[j]); kernel_devpts_entry_release(&entries[j]);
        }
        destroy(m);
    }
}

int main(void)
{
    struct kernel_heap heap = {0};
    options_and_isolation(&heap);
    stable_identity_and_metadata(&heap);
    parsing_and_rollback(&heap);
    readonly_and_root_owner(&heap);
    full_number_range(&heap);
    assert(!live_allocations);
    puts("devpts options, isolation, stable identities, metadata, quota and rollback passed");
    return 0;
}
