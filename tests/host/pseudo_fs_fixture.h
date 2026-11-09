/* Shared heap and VFS-node fixture for memory-backed filesystem tests. */
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
static inline void *access_page(uint64_t p) { return (void *)(uintptr_t)p; }
static inline int page_address(const void *p, uint64_t *out) { *out = (uintptr_t)p; return 1; }
uint64_t kernel_time_realtime_ns(void) { return ++clock_ns; }

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
struct kernel_vfs_node *__wrap_kernel_vfs_node_find(struct kernel_vfs_instance *i, uint64_t id)
{
    for (struct kernel_vfs_node *n = i->nodes; n; n = n->next)
        if (n->inode == id && !n->retired && !n->closed) return n;
    return 0;
}
void __wrap_kernel_vfs_node_lock(struct kernel_vfs_node *n, struct kernel_lock_guard *g, int write)
{
    if (write) kernel_rwlock_write(&n->io_lock, g);
    else kernel_rwlock_read(&n->io_lock, g);
}
int __wrap_kernel_vfs_publish_node(struct kernel_vfs_mount *m, struct kernel_vfs_node *n,
    struct kernel_vfs_file *f, int creating)
{
    (void)creating;
    struct kernel_vfs_instance *i = m->private_data;
    kernel_rwlock_init(&n->io_lock, 30, (uintptr_t)n);
    n->instance = i; n->mount = &i->backend_mount; n->references = n->open_files = 1;
    n->next = i->nodes; i->nodes = n; i->external_files++;
    *f = (struct kernel_vfs_file){.mount = m, .private_data = n,
        .mode = n->mode, .state = VFS_FILE_STATE_LIVE};
    return 0;
}
static inline const struct kernel_vfs_backend *ops(struct kernel_vfs_mount *m)
{ return ((struct kernel_vfs_instance *)m->private_data)->ops; }
static inline struct kernel_vfs_instance *instance(struct kernel_vfs_mount *m) { return m->private_data; }
int __wrap_kernel_vfs_nodes_idle(struct kernel_vfs_instance *i, struct kernel_vfs_node *root)
{ return !i->retiring_nodes && i->nodes == root && (!root || !root->next); }
static inline void close_file(struct kernel_vfs_file *f)
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
static inline void open_id(struct kernel_vfs_mount *m, uint64_t id, struct kernel_vfs_file *f)
{ assert(!ops(m)->open(m, 0, id, 0, f)); }
static inline uint64_t find(struct kernel_vfs_mount *m, uint64_t parent, const char *name, uint32_t *mode)
{
    uint64_t id; uint32_t ignored;
    assert(!ops(m)->lookup(instance(m), parent, name, strlen(name), &id, mode ? mode : &ignored));
    return id;
}
static inline void absent(struct kernel_vfs_mount *m, uint64_t parent, const char *name)
{
    uint64_t id; uint32_t mode;
    assert(ops(m)->lookup(instance(m), parent, name, strlen(name), &id, &mode) == -KERNEL_ENOENT);
}
static inline void regular(struct kernel_vfs_mount *m, uint64_t parent, const char *name)
{ assert(!ops(m)->mknod(instance(m), parent, name, KERNEL_VFS_S_IFREG, 0600, 0,0)); }
static inline struct kernel_vfs_stat stat_id(struct kernel_vfs_mount *m, uint64_t id)
{
    struct kernel_vfs_file f = {0}; struct kernel_vfs_stat st;
    open_id(m, id, &f); assert(!ops(m)->stat(&f, &st)); close_file(&f); return st;
}
static inline struct kernel_vfs_statfs space(struct kernel_vfs_mount *m)
{ struct kernel_vfs_statfs s; assert(!ops(m)->statfs(m, &s)); return s; }
static inline uint8_t expect_entry(struct kernel_vfs_file *f, uint64_t *pos, const char *expected, uint64_t id)
{
    char name[256]; uint8_t type; uint64_t next, found;
    assert(ops(f->mount)->dir_entry(f, *pos, &next, &found, &type, name, sizeof(name)) == 1);
    assert(!strcmp(name, expected) && found == id && next > *pos); *pos = next; return type;
}
static inline void expect_end(struct kernel_vfs_file *f, uint64_t pos)
{
    char name[256]; uint8_t type; uint64_t next, found;
    assert(!ops(f->mount)->dir_entry(f, pos, &next, &found, &type, name, sizeof(name)));
}
