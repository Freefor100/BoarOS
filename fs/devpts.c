#include "devpts_internal.h"
#include <arch/context.h>
#include <kernel/errno.h>
#include <kernel/page.h>
#include <kernel/time.h>
#include <string.h>

#define DEVPTS_ROOT_INODE UINT64_C(1)
#define DEVPTS_PTMX_INODE UINT64_C(2)
#define DEVPTS_SUPER_MAGIC UINT64_C(0x1cd1)
#define DEVPTS_BLOCK_SIZE UINT64_C(1024)
/* references/linux/include/linux/tty.h: NR_UNIX98_PTY_MAX, MINORBITS=20. */
#define DEVPTS_OPTION_MAX UINT32_C(1048576)

static struct kernel_vfs_backend devpts_backend;

static struct devpts_mount *devpts(const struct kernel_vfs_mount *mount)
{
    if (!mount || !mount->private_data) return 0;
    struct kernel_vfs_instance *i = mount->private_data;
    return i->ops == &devpts_backend ? i->backend_data : 0;
}
int kernel_devpts_is_mount(const struct kernel_vfs_mount *mount)
{ return devpts(mount) != 0; }

static struct kernel_vfs_timespec timestamp(void)
{
    uint64_t ns = kernel_time_realtime_ns();
    return (struct kernel_vfs_timespec){(int64_t)(ns / 1000000000U),
        (int64_t)(ns % 1000000000U)};
}
static int allocate(struct devpts_mount *m, size_t size, void **owner)
{
    enum kernel_heap_status status = kernel_heap_allocate_zeroed(
        m->instance.heap, 1U, size, owner);
    return status == KERNEL_HEAP_STATUS_OK ? 0 :
        status == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
}
static void dispose(struct devpts_mount *m, void *owner)
{
    if (kernel_heap_release(m->instance.heap, owner) != KERNEL_HEAP_STATUS_OK)
        __builtin_trap();
}

static struct kernel_devpts_entry *entry_inode(struct devpts_mount *m, uint64_t ino)
{
    if (ino == DEVPTS_ROOT_INODE) return &m->root;
    if (ino == DEVPTS_PTMX_INODE) return &m->ptmx;
    for (struct kernel_devpts_entry *e = m->entries; e; e = e->next)
        if (e->stat.ino == ino) return e;
    return 0;
}
static struct kernel_devpts_entry *entry_number(struct devpts_mount *m, uint32_t number)
{
    for (struct kernel_devpts_entry *e = m->entries; e; e = e->next)
        if (e->published && e->number == number) return e;
    return 0;
}
static int parse_number(const char *p, size_t n, unsigned base, uint32_t *value)
{
    if (!n) return -KERNEL_EINVAL;
    if (p[0] == '+') { p++; n--; }
    if (!n) return -KERNEL_EINVAL;
    if (p[n - 1] == '\n') n--;
    if (!n) return -KERNEL_EINVAL;
    if (!base) {
        base = p[0] == '0' ? 8U : 10U;
        if (n > 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            base = 16U; p += 2; n -= 2;
        }
    }
    uint32_t v = 0;
    for (size_t j = 0; j < n; j++) {
        unsigned digit = p[j] >= 'a' && p[j] <= 'f' ? (unsigned)(p[j] - 'a') + 10U :
            p[j] >= 'A' && p[j] <= 'F' ? (unsigned)(p[j] - 'A') + 10U :
            (unsigned)(p[j] - '0');
        if (digit >= base || v > (UINT32_MAX - digit) / base)
            return -KERNEL_EINVAL;
        v = v * base + digit;
    }
    *value = v;
    return 0;
}
static int parse_index(const char *name, size_t n, uint32_t *number)
{
    if (!n || (n > 1 && name[0] == '0') || name[0] == '+' || name[n - 1] == '\n')
        return -KERNEL_ENOENT;
    return parse_number(name, n, 10U, number) ? -KERNEL_ENOENT : 0;
}
static int options(struct devpts_mount *m, const char *input)
{
    m->slave_mode = 0600U;
    m->max = KERNEL_DEVPTS_DEFAULT_MAX;
    for (const char *p = input ? input : ""; *p;) {
        const char *end = p;
        while (*end && *end != ',') end++;
        size_t n = (size_t)(end - p);
        /* Linux generic_parse_monolithic skips empty comma-separated values. */
        if (!n) { p = end + 1; continue; }
        if (n == 11U && !memcmp(p, "newinstance", n)) {
            p = *end ? end + 1 : end;
            continue;
        }
        const char *eq = p;
        while (eq < end && *eq != '=') eq++;
        if (eq == end) return -KERNEL_EINVAL;
        size_t key = (size_t)(eq - p);
        int mode = key == 4U && !memcmp(p, "mode", key);
        int ptmxmode = key == 8U && !memcmp(p, "ptmxmode", key);
        int max = key == 3U && !memcmp(p, "max", key);
        const char *value = eq + 1;
        size_t length = (size_t)(end - value);
        int negative = max && length && *value == '-';
        if (negative) {
            value++; length--;
            if (length && *value == '+') return -KERNEL_EINVAL;
        }
        uint32_t v;
        if (parse_number(value, length, mode || ptmxmode ? 8U : 0U, &v) ||
            (negative && v)) return -KERNEL_EINVAL;
        if (mode) m->slave_mode = v & 07777U;
        else if (ptmxmode) m->ptmx.stat.mode = KERNEL_VFS_S_IFCHR | (v & 07777U);
        else if (key == 3U && !memcmp(p, "uid", key) && v != UINT32_MAX) m->uid = v;
        else if (key == 3U && !memcmp(p, "gid", key) && v != UINT32_MAX) m->gid = v;
        else if (max && v <= DEVPTS_OPTION_MAX) m->max = v;
        else return -KERNEL_EINVAL;
        p = *end ? end + 1 : end;
    }
    return 0;
}

int kernel_devpts_entry_acquire(struct kernel_devpts_entry *entry)
{
    if (!entry) return -KERNEL_EINVAL;
    uintptr_t irq = arch_interrupt_save();
    int result = (!entry->references && !entry->embedded) ||
        entry->references == UINT32_MAX ? -KERNEL_EOVERFLOW : 0;
    if (!result) entry->references++;
    arch_interrupt_restore(irq);
    return result;
}
void kernel_devpts_entry_release(struct kernel_devpts_entry **owner)
{
    if (!owner || !*owner) __builtin_trap();
    struct kernel_devpts_entry *e = *owner;
    struct devpts_mount *m = e->mount;
    uintptr_t irq = arch_interrupt_save();
    if (!e->references) __builtin_trap();
    e->references--;
    *owner = 0;
    int collect = !e->references && !e->embedded;
    if (collect) {
        if (e->published || e->reserved || e->binding) __builtin_trap();
        struct kernel_devpts_entry **link = &m->entries;
        while (*link && *link != e) link = &(*link)->next;
        if (!*link) __builtin_trap();
        *link = e->next;
    }
    arch_interrupt_restore(irq);
    if (collect) dispose(m, e);
}

int kernel_devpts_publish(struct kernel_vfs_mount *mount, void *pair,
    struct kernel_devpts_entry **owner)
{
    struct devpts_mount *m = devpts(mount);
    if (!m || !pair || !owner || *owner) return -KERNEL_EINVAL;
    KERNEL_LOCK_SCOPE(guard);
    kernel_vfs_namespace_lock(mount, &guard);
    if (m->instance.quiescing) return -KERNEL_EIO;
    uintptr_t irq = arch_interrupt_save();
    uint32_t limit = m->max < KERNEL_DEVPTS_LIMIT ? m->max : KERNEL_DEVPTS_LIMIT;
    uint32_t index = 0;
    while (index < limit && (m->reserved_numbers & (UINT64_C(1) << index))) index++;
    int result = index == limit ? -KERNEL_ENOSPC :
        m->next_inode == UINT64_MAX ? -KERNEL_EOVERFLOW : 0;
    arch_interrupt_restore(irq);
    if (result) return result;
    struct kernel_devpts_entry *e = 0;
    result = allocate(m, sizeof(*e), (void **)&e);
    if (result) return result;
    struct kernel_vfs_timespec time = timestamp();
    irq = arch_interrupt_save();
    /* namespace 锁串行化发布；回收只能释放编号，不会占用选定的空槽。 */
    if (m->reserved_numbers & (UINT64_C(1) << index)) __builtin_trap();
    {
        e->mount = m; e->number = index; e->references = 1;
        e->binding = pair; e->published = e->reserved = 1;
        e->stat = (struct kernel_vfs_stat){.dev = mount->id,
            .ino = m->next_inode++, .mode = KERNEL_VFS_S_IFCHR | m->slave_mode,
            .uid = m->uid, .gid = m->gid, .nlink = 1,
            .rdev = UINT64_C(0x8800) + index, .blksize = DEVPTS_BLOCK_SIZE,
            .atime = time, .mtime = time, .ctime = time};
        m->reserved_numbers |= UINT64_C(1) << index;
        e->next = m->entries; m->entries = e;
        m->root.stat.ctime = m->root.stat.mtime = time;
        *owner = e;
    }
    arch_interrupt_restore(irq);
    return 0;
}
void kernel_devpts_unpublish(struct kernel_devpts_entry *entry)
{
    if (!entry || !entry->references || entry->embedded) __builtin_trap();
    struct kernel_vfs_timespec time = timestamp();
    uintptr_t irq = arch_interrupt_save();
    if (entry->published) {
        entry->published = 0; entry->binding = 0;
        entry->stat.nlink = 0; entry->stat.ctime = time;
        struct devpts_mount *m = entry->mount;
        m->root.stat.mtime = m->root.stat.ctime = time;
        /* 下线只改变目录可见性；旧 inode 可继续 stat/chmod，但不会重新绑定。 */
        for (struct kernel_vfs_node *n = m->instance.nodes; n; n = n->next)
            if (n->backend_data == entry) n->unlinked = 1;
    }
    arch_interrupt_restore(irq);
}
void kernel_devpts_retire(struct kernel_devpts_entry *entry)
{
    kernel_devpts_unpublish(entry);
    uintptr_t irq = arch_interrupt_save();
    if (entry->reserved) {
        uint64_t bit = UINT64_C(1) << entry->number;
        if (!(entry->mount->reserved_numbers & bit)) __builtin_trap();
        entry->mount->reserved_numbers &= ~bit;
        entry->reserved = 0;
    }
    arch_interrupt_restore(irq);
}
uint32_t kernel_devpts_entry_number(const struct kernel_devpts_entry *entry)
{ if (!entry || !entry->references || entry->embedded) __builtin_trap(); return entry->number; }
void *kernel_devpts_entry_binding(const struct kernel_devpts_entry *entry)
{ return entry && entry->references && entry->published ? entry->binding : 0; }

struct kernel_vfs_mount *kernel_devpts_file_mount(const struct kernel_vfs_file *file)
{
    struct kernel_vfs_node *n = kernel_vfs_file_node(file);
    return n && devpts(n->mount) ? n->mount : 0;
}
struct kernel_devpts_entry *kernel_devpts_file_entry(const struct kernel_vfs_file *file)
{
    struct kernel_vfs_node *n = kernel_vfs_file_node(file);
    return n && devpts(n->mount) && n->inode > DEVPTS_PTMX_INODE ? n->backend_data : 0;
}
int kernel_devpts_file_is_ptmx(const struct kernel_vfs_file *file)
{
    struct kernel_vfs_node *n = kernel_vfs_file_node(file);
    return n && devpts(n->mount) && n->inode == DEVPTS_PTMX_INODE;
}
int kernel_devpts_mount_root(struct kernel_vfs_mount *mount, struct kernel_vfs_path **owner)
{
    struct devpts_mount *m = devpts(mount);
    if (!m || !owner || *owner) return -KERNEL_EINVAL;
    KERNEL_LOCK_SCOPE(guard);
    kernel_vfs_namespace_lock(mount, &guard);
    if (m->instance.quiescing) return -KERNEL_EIO;
    uintptr_t irq = arch_interrupt_save();
    struct kernel_vfs_path *root_path = mount->root_path;
    int result = root_path ? kernel_vfs_path_acquire(root_path) : -KERNEL_ENODEV;
    if (!result) *owner = root_path;
    arch_interrupt_restore(irq);
    return result;
}
void *kernel_devpts_mount_private(const struct kernel_vfs_mount *mount)
{ struct devpts_mount *m = devpts(mount); return m ? m->pty_private : 0; }
void kernel_devpts_mount_set_private(struct kernel_vfs_mount *mount, void *private_data)
{ struct devpts_mount *m = devpts(mount); if (!m) __builtin_trap(); m->pty_private = private_data; }

static int no_error(struct kernel_vfs_instance *instance) { (void)instance; return 0; }
/* Fixed Linux simple_dir_inode_operations has no namespace mutation methods.
 * namei.c returns EACCES for create and EPERM for the remaining operations. */
static int denied_create(struct kernel_vfs_mount *mount, const char *path,
    uint32_t mode, struct kernel_vfs_file *file)
{
    (void)path; (void)mode; (void)file;
    return devpts(mount)->instance.read_only ? -KERNEL_EROFS : -KERNEL_EACCES;
}
static int denied_path(struct kernel_vfs_mount *mount, const char *path)
{
    (void)path;
    return devpts(mount)->instance.read_only ? -KERNEL_EROFS : -KERNEL_EPERM;
}
static int denied_mkdir(struct kernel_vfs_mount *mount, const char *path, uint32_t mode)
{ (void)mode; return denied_path(mount, path); }
static int denied_symlink(struct kernel_vfs_instance *instance,
    const char *target, const char *path)
{
    (void)target; (void)path;
    return instance->read_only ? -KERNEL_EROFS : -KERNEL_EPERM;
}
static int denied_mknod(struct kernel_vfs_instance *instance, const char *path,
    uint32_t type, uint32_t mode, uint32_t device)
{
    (void)path; (void)mode; (void)device;
    return instance->read_only ? -KERNEL_EROFS :
        type == KERNEL_VFS_S_IFREG ? -KERNEL_EACCES : -KERNEL_EPERM;
}
static int denied_rename(struct kernel_vfs_instance *instance,
    uint64_t old_parent, const char *old_name, uint64_t new_parent,
    const char *new_name, unsigned flags, struct kernel_vfs_rename_result *result)
{
    (void)old_parent; (void)old_name; (void)new_parent; (void)new_name;
    (void)flags; (void)result;
    return instance->read_only ? -KERNEL_EROFS : -KERNEL_EPERM;
}
static int root(struct kernel_vfs_instance *instance, uint64_t *ino, uint32_t *mode)
{
    struct devpts_mount *m = instance->backend_data;
    *ino = DEVPTS_ROOT_INODE; *mode = m->root.stat.mode;
    return 0;
}
static int lookup(struct kernel_vfs_instance *instance, uint64_t parent,
    const char *name, size_t length, uint64_t *ino, uint32_t *mode)
{
    if (parent != DEVPTS_ROOT_INODE) return -KERNEL_ENOTDIR;
    struct devpts_mount *m = instance->backend_data;
    uintptr_t irq = arch_interrupt_save();
    struct kernel_devpts_entry *e = 0;
    if (length == 4U && !memcmp(name, "ptmx", 4U)) e = &m->ptmx;
    else {
        uint32_t index;
        if (!parse_index(name, length, &index)) e = entry_number(m, index);
    }
    int result = e ? 0 : -KERNEL_ENOENT;
    if (e) { *ino = e->stat.ino; *mode = e->stat.mode; }
    arch_interrupt_restore(irq);
    return result;
}
static int open_inode(struct kernel_vfs_mount *mount, const char *path,
    uint64_t ino, uint32_t mode, struct kernel_vfs_file *file)
{
    (void)mode;
    struct devpts_mount *m = devpts(mount);
    if (!ino) {
        if (!path || *path != '/') return -KERNEL_EINVAL;
        while (*path == '/') path++;
        if (!*path) ino = DEVPTS_ROOT_INODE;
        else {
            int result = lookup(&m->instance, DEVPTS_ROOT_INODE,
                path, strlen(path), &ino, &mode);
            if (result) return result;
        }
    }
    uintptr_t irq = arch_interrupt_save();
    struct kernel_devpts_entry *e = entry_inode(m, ino);
    int result = e ? kernel_devpts_entry_acquire(e) : -KERNEL_ENOENT;
    arch_interrupt_restore(irq);
    if (result) return result;
    struct kernel_vfs_node *node = 0;
    result = allocate(m, sizeof(*node), (void **)&node);
    if (result) { kernel_devpts_entry_release(&e); return result; }
    irq = arch_interrupt_save();
    node->inode = e->stat.ino; node->mode = e->stat.mode;
    node->backend_data = e;
    if (!e->published) node->unlinked = 1;
    arch_interrupt_restore(irq);
    return kernel_vfs_publish_node(mount, node, file, 0);
}
static int close_node(struct kernel_vfs_node *node)
{
    struct kernel_devpts_entry *e = node->backend_data;
    node->backend_data = 0;
    kernel_devpts_entry_release(&e);
    return 0;
}
static void release_unlinked(struct kernel_vfs_node *node)
{
    /* 无磁盘 orphan；entry 引用保留旧元数据，最后 node 关闭即可归还。 */
    if (node->unlinked && !node->open_files) node->retired = 1;
}
static int stat_inode(const struct kernel_vfs_file *file, struct kernel_vfs_stat *stat)
{
    struct kernel_vfs_node *n = file->private_data;
    uintptr_t irq = arch_interrupt_save();
    *stat = ((struct kernel_devpts_entry *)n->backend_data)->stat;
    arch_interrupt_restore(irq);
    return 0;
}
static int statfs(struct kernel_vfs_mount *mount, struct kernel_vfs_statfs *stat)
{
    struct devpts_mount *m = devpts(mount);
    *stat = (struct kernel_vfs_statfs){.type = DEVPTS_SUPER_MAGIC,
        .block_size = BOAROS_PAGE_SIZE, .name_length = 255U,
        .fsid = mount->id, .flags = m->instance.read_only ? 1U : 0U};
    return 0;
}
static size_t decimal(char *buffer, uint32_t value)
{
    char reverse[10]; size_t n = 0;
    do { reverse[n++] = (char)('0' + value % 10U); value /= 10U; } while (value);
    for (size_t j = 0; j < n; j++) buffer[j] = reverse[n - j - 1];
    buffer[n] = 0;
    return n;
}
static int dir_entry(struct kernel_vfs_file *file, uint64_t position,
    uint64_t *next, uint64_t *ino, uint8_t *type, char *name, size_t capacity)
{
    struct kernel_vfs_node *n = file->private_data;
    if (n->inode != DEVPTS_ROOT_INODE) return -KERNEL_ENOTDIR;
    struct devpts_mount *m = n->instance->backend_data;
    char text[12]; uint64_t found;
    if (position < 3U) {
        const char *fixed = position == 0 ? "." : position == 1 ? ".." : "ptmx";
        strcpy(text, fixed); found = position;
        *ino = position < 2U ? DEVPTS_ROOT_INODE : DEVPTS_PTMX_INODE;
        *type = position < 2U ? KERNEL_VFS_DT_DIR : KERNEL_VFS_DT_CHR;
    } else {
        uintptr_t irq = arch_interrupt_save();
        struct kernel_devpts_entry *best = 0;
        for (struct kernel_devpts_entry *e = m->entries; e; e = e->next)
            if (e->published && (uint64_t)e->number + 3U >= position &&
                (!best || e->number < best->number)) best = e;
        if (!best) { arch_interrupt_restore(irq); return 0; }
        found = (uint64_t)best->number + 3U;
        *ino = best->stat.ino; *type = KERNEL_VFS_DT_CHR;
        decimal(text, best->number);
        arch_interrupt_restore(irq);
    }
    size_t length = strlen(text);
    if (length >= capacity) return -KERNEL_ENAMETOOLONG;
    memcpy(name, text, length + 1U); *next = found + 1U;
    return 1;
}
static int set_mode(struct kernel_vfs_file *file, uint32_t mode)
{
    struct kernel_vfs_node *n = file->private_data;
    KERNEL_LOCK_SCOPE(guard);
    kernel_vfs_node_lock(n, &guard, 1);
    if (n->instance->read_only) return -KERNEL_EROFS;
    struct kernel_vfs_timespec time = timestamp();
    uintptr_t irq = arch_interrupt_save();
    struct kernel_devpts_entry *e = n->backend_data;
    e->stat.mode = (e->stat.mode & KERNEL_VFS_S_IFMT) | (mode & 07777U);
    n->mode = file->mode = e->stat.mode; e->stat.ctime = time;
    arch_interrupt_restore(irq);
    return 0;
}
static int set_owner(struct kernel_vfs_file *file, uint32_t uid, uint32_t gid)
{
    struct kernel_vfs_node *n = file->private_data;
    KERNEL_LOCK_SCOPE(guard);
    kernel_vfs_node_lock(n, &guard, 1);
    if (n->instance->read_only) return -KERNEL_EROFS;
    struct kernel_vfs_timespec time = timestamp();
    uintptr_t irq = arch_interrupt_save();
    struct kernel_devpts_entry *e = n->backend_data;
    if (uid != UINT32_MAX) e->stat.uid = uid;
    if (gid != UINT32_MAX) e->stat.gid = gid;
    e->stat.mode = kernel_vfs_chown_mode(e->stat.mode);
    n->mode = file->mode = e->stat.mode; e->stat.ctime = time;
    arch_interrupt_restore(irq);
    return 0;
}
static int set_times(struct kernel_vfs_file *file, const struct kernel_vfs_timespec times[2])
{
    if (times) for (unsigned j = 0; j < 2U; j++)
        if (times[j].nanoseconds != KERNEL_VFS_UTIME_NOW &&
            times[j].nanoseconds != KERNEL_VFS_UTIME_OMIT &&
            (times[j].nanoseconds < 0 || times[j].nanoseconds >= 1000000000))
            return -KERNEL_EINVAL;
    if (times && times[0].nanoseconds == KERNEL_VFS_UTIME_OMIT &&
        times[1].nanoseconds == KERNEL_VFS_UTIME_OMIT) return 0;
    struct kernel_vfs_node *n = file->private_data;
    KERNEL_LOCK_SCOPE(guard);
    kernel_vfs_node_lock(n, &guard, 1);
    if (n->instance->read_only) return -KERNEL_EROFS;
    struct kernel_vfs_timespec time = timestamp();
    uintptr_t irq = arch_interrupt_save();
    struct kernel_devpts_entry *e = n->backend_data;
    if (!times || times[0].nanoseconds != KERNEL_VFS_UTIME_OMIT)
        e->stat.atime = !times || times[0].nanoseconds == KERNEL_VFS_UTIME_NOW ? time : times[0];
    if (!times || times[1].nanoseconds != KERNEL_VFS_UTIME_OMIT)
        e->stat.mtime = !times || times[1].nanoseconds == KERNEL_VFS_UTIME_NOW ? time : times[1];
    e->stat.ctime = time;
    arch_interrupt_restore(irq);
    return 0;
}
static void accessed(struct kernel_vfs_file *file) { (void)file; }
static int sync_metadata(struct kernel_vfs_node *node, int data_only)
{ (void)node; (void)data_only; return 0; }
static int prepare_unmount(struct kernel_vfs_mount *mount)
{
    struct devpts_mount *m = devpts(mount);
    int result = m->worker_started ? kernel_pty_mount_stop(mount) : 0;
    if (!result) m->worker_started = 0;
    return result;
}
static int unmount(struct kernel_vfs_mount *mount)
{
    struct devpts_mount *m = devpts(mount);
    if (mount->covered_path || mount->root_path || mount->child_mounts ||
        m->instance.external_files || m->instance.nodes || m->instance.cleanup_nodes ||
        m->instance.paths || m->entries || m->reserved_numbers ||
        m->root.references || m->ptmx.references) return -KERNEL_EBUSY;
    int result = prepare_unmount(mount);
    if (result) return result;
    if (m->pty_private) __builtin_trap();
    dispose(m, m);
    return 0;
}
static void initialize_backend(void)
{
    /* Early VFS tests execute before Sv39; assign callback addresses at runtime. */
    volatile struct kernel_vfs_backend *ops = &devpts_backend;
    ops->error = no_error; ops->root = root; ops->lookup = lookup;
    ops->open = open_inode; ops->close_node = close_node;
    ops->release_unlinked = release_unlinked;
    ops->create = denied_create; ops->mkdir = denied_mkdir;
    ops->unlink = denied_path; ops->rmdir = denied_path;
    ops->symlink = denied_symlink; ops->mknod = denied_mknod; ops->rename = denied_rename;
    ops->stat = stat_inode; ops->statfs = statfs; ops->dir_entry = dir_entry;
    ops->set_mode = set_mode; ops->set_owner = set_owner; ops->set_times = set_times;
    ops->accessed = accessed; ops->writeback_allowed = no_error;
    ops->sync_metadata = sync_metadata; ops->flush = no_error;
    ops->prepare_unmount = prepare_unmount; ops->unmount = unmount;
}
int kernel_devpts_create(struct kernel_heap *heap, uint64_t flags,
    const char *input, struct kernel_vfs_mount **owner)
{
    if (!heap || !owner || *owner) return -KERNEL_EINVAL;
    struct devpts_mount *m = 0;
    enum kernel_heap_status status = kernel_heap_allocate_zeroed(heap, 1U,
        sizeof(*m), (void **)&m);
    if (status != KERNEL_HEAP_STATUS_OK)
        return status == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    m->instance.heap = heap;
    m->ptmx.stat.mode = KERNEL_VFS_S_IFCHR;
    int result = options(m, input);
    if (result) { dispose(m, m); return result; }
    initialize_backend();
    m->instance.ops = &devpts_backend; m->instance.backend_data = m;
    m->instance.read_only = (flags & 1U) != 0;
    kernel_mutex_init(&m->instance.namespace_lock, 20U, (uintptr_t)&m->instance);
    m->mount.private_data = &m->instance; m->mount.id = kernel_vfs_allocate_mount_id();
    m->mount.state = VFS_MOUNT_STATE_LIVE;
    m->root.mount = m; m->root.embedded = m->root.published = 1;
    struct kernel_vfs_timespec time = timestamp();
    m->root.stat = (struct kernel_vfs_stat){.dev = m->mount.id,
        .ino = DEVPTS_ROOT_INODE, .mode = KERNEL_VFS_S_IFDIR | 0755U,
        .nlink = 2U, .blksize = DEVPTS_BLOCK_SIZE,
        .atime = time, .mtime = time, .ctime = time};
    m->ptmx.mount = m; m->ptmx.embedded = m->ptmx.published = 1;
    m->ptmx.stat.dev = m->mount.id; m->ptmx.stat.ino = DEVPTS_PTMX_INODE;
    m->ptmx.stat.nlink = 1U; m->ptmx.stat.rdev = UINT64_C(0x502);
    m->ptmx.stat.blksize = DEVPTS_BLOCK_SIZE;
    m->ptmx.stat.atime = m->ptmx.stat.mtime = m->ptmx.stat.ctime = time;
    m->next_inode = 3U;
    result = kernel_pty_mount_start(&m->mount);
    if (result) { if (m->pty_private) __builtin_trap(); dispose(m, m); return result; }
    m->worker_started = 1;
    *owner = &m->mount;
    return 0;
}
