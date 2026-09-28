#include "vfs_objects.h"

#include <arch/riscv/context.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/procfs.h>
#include <kernel/vfs.h>

#include <stdint.h>
#include <string.h>

#define PROC_ROOT_INODE UINT64_C(1)
#define PROC_MEMINFO_INODE UINT64_C(2)
#define PROC_SUPER_MAGIC UINT64_C(0x9fa0)

struct procfs_mount {
    struct kernel_vfs_mount mount;
    struct kernel_vfs_instance instance;
    struct kernel_heap *heap;
    uint64_t flags;
};

static uint64_t next_proc_mount_id = 2U;

static int proc_error(struct kernel_vfs_instance *instance)
{
    (void)instance;
    return 0;
}

static int proc_root(struct kernel_vfs_instance *instance,
                     uint64_t *inode, uint32_t *mode)
{
    (void)instance;
    *inode = PROC_ROOT_INODE;
    *mode = KERNEL_VFS_S_IFDIR | 0555U;
    return 0;
}

static int proc_lookup(struct kernel_vfs_instance *instance, uint64_t parent,
                       const char *name, size_t length,
                       uint64_t *inode, uint32_t *mode)
{
    (void)instance;
    if (parent != PROC_ROOT_INODE) return -KERNEL_ENOTDIR;
    if (length == 7U && !memcmp(name, "meminfo", length)) {
        *inode = PROC_MEMINFO_INODE;
        *mode = KERNEL_VFS_S_IFREG | 0444U;
        return 0;
    }
    return -KERNEL_ENOENT;
}

static int proc_close_node(struct kernel_vfs_node *node)
{
    (void)node;
    return 0;
}

static int proc_open(struct kernel_vfs_mount *mount, const char *path,
                     uint64_t inode, uint32_t mode,
                     struct kernel_vfs_file *file)
{
    struct kernel_vfs_instance *instance = mount->private_data;
    struct kernel_vfs_node *node = 0;
    (void)path;
    if ((inode != PROC_ROOT_INODE && inode != PROC_MEMINFO_INODE) ||
        (inode == PROC_ROOT_INODE &&
         (mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR) ||
        (inode == PROC_MEMINFO_INODE &&
         (mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFREG))
        return -KERNEL_EINVAL;
    enum kernel_heap_status status = kernel_heap_allocate_zeroed(
        instance->heap, 1U, sizeof(*node), (void **)&node);
    if (status != KERNEL_HEAP_STATUS_OK)
        return status == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    node->inode = inode;
    node->mode = mode;
    return kernel_vfs_publish_node(mount, node, file, 0);
}

static int proc_stat(const struct kernel_vfs_file *file,
                     struct kernel_vfs_stat *stat)
{
    *stat = (struct kernel_vfs_stat){
        .dev = file->mount->id,
        .ino = kernel_vfs_file_inode(file),
        .mode = file->mode,
        .nlink = (file->mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFDIR ? 2U : 1U,
        .blksize = BOAROS_PAGE_SIZE,
    };
    return 0;
}

static int proc_statfs(struct kernel_vfs_mount *mount,
                       struct kernel_vfs_statfs *stat)
{
    struct procfs_mount *proc = (struct procfs_mount *)mount;
    *stat = (struct kernel_vfs_statfs){
        .type = PROC_SUPER_MAGIC,
        .block_size = BOAROS_PAGE_SIZE,
        .fsid = mount->id,
        .name_length = 255U,
        .flags = proc->flags & 1U,
    };
    return 0;
}

static int proc_dir_entry(struct kernel_vfs_file *file, uint64_t position,
                          uint64_t *next_position, uint64_t *inode,
                          uint8_t *type, char *name, size_t name_size)
{
    if (kernel_vfs_file_inode(file) != PROC_ROOT_INODE) return -KERNEL_ENOTDIR;
    const char *entry_name;
    if (position == 0U) entry_name = ".";
    else if (position == 1U) entry_name = "..";
    else if (position == 2U) entry_name = "meminfo";
    else return -KERNEL_ENOENT;
    size_t length = strlen(entry_name) + 1U;
    if (name_size < length) return -KERNEL_ERANGE;
    memcpy(name, entry_name, length);
    *next_position = position + 1U;
    *inode = position == 2U ? PROC_MEMINFO_INODE : PROC_ROOT_INODE;
    *type = position == 2U ? 8U : 4U;
    return 0;
}

static size_t decimal(char *buffer, uint64_t value)
{
    char reverse[24];
    size_t length = 0U;
    do {
        reverse[length++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value);
    for (size_t i = 0U; i < length; i++) buffer[i] = reverse[length - i - 1U];
    return length;
}

static size_t append_kib(char *buffer, const char *label, uint64_t pages)
{
    size_t length = strlen(label);
    memcpy(buffer, label, length);
    length += decimal(buffer + length, pages * (BOAROS_PAGE_SIZE / 1024U));
    memcpy(buffer + length, " kB\n", 4U);
    return length + 4U;
}

static int proc_snapshot(struct kernel_vfs_node *node, struct kernel_heap *heap,
                         char **buffer, size_t *length)
{
    if (node->inode != PROC_MEMINFO_INODE) return -KERNEL_EINVAL;
    char *data = 0;
    enum kernel_heap_status status = kernel_heap_allocate(heap, 128U,
                                                           (void **)&data);
    if (status != KERNEL_HEAP_STATUS_OK)
        return status == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    struct physical_page_allocator *allocator = heap->page_allocator;
    size_t used = append_kib(data, "MemTotal:       ",
                              physical_page_total(allocator));
    used += append_kib(data + used, "MemFree:        ",
                       physical_page_available(allocator));
    data[used] = '\0';
    *buffer = data;
    *length = used;
    return 0;
}

static void proc_accessed(struct kernel_vfs_file *file) { (void)file; }

static int proc_unmount(struct kernel_vfs_mount *mount)
{
    struct procfs_mount *proc = (struct procfs_mount *)mount;
    struct kernel_vfs_instance *instance = &proc->instance;
    if (mount->covered_path || mount->root_path || mount->child_mounts ||
        instance->external_files || instance->nodes || instance->paths)
        return -KERNEL_EBUSY;
    struct kernel_heap *heap = proc->heap;
    if (kernel_heap_release(heap, proc) != KERNEL_HEAP_STATUS_OK) __builtin_trap();
    return 0;
}

static struct kernel_vfs_backend proc_backend;

static void initialize_backend(void)
{
    /* Early VFS module tests execute at physical addresses before Sv39. */
    volatile struct kernel_vfs_backend *ops = &proc_backend;
    ops->error = proc_error;
    ops->root = proc_root;
    ops->lookup = proc_lookup;
    ops->close_node = proc_close_node;
    ops->open = proc_open;
    ops->stat = proc_stat;
    ops->statfs = proc_statfs;
    ops->dir_entry = proc_dir_entry;
    ops->snapshot = proc_snapshot;
    ops->accessed = proc_accessed;
    ops->unmount = proc_unmount;
}

int kernel_procfs_create(struct kernel_heap *heap, uint64_t flags,
                         struct kernel_vfs_mount **owner)
{
    if (!heap || !owner || *owner) return -KERNEL_EINVAL;
    struct procfs_mount *proc = 0;
    enum kernel_heap_status status = kernel_heap_allocate_zeroed(
        heap, 1U, sizeof(*proc), (void **)&proc);
    if (status != KERNEL_HEAP_STATUS_OK)
        return status == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    proc->heap = heap;
    proc->flags = flags;
    initialize_backend();
    proc->instance.ops = &proc_backend;
    proc->instance.heap = heap;
    proc->instance.read_only = (flags & 1U) != 0U;
    kernel_mutex_init(&proc->instance.namespace_lock, 20U,
                      (uintptr_t)&proc->instance);
    uintptr_t irq = riscv_interrupt_save();
    if (next_proc_mount_id == UINT64_MAX) {
        riscv_interrupt_restore(irq);
        (void)kernel_heap_release(heap, proc);
        return -KERNEL_EOVERFLOW;
    }
    proc->mount.id = next_proc_mount_id++;
    riscv_interrupt_restore(irq);
    proc->mount.private_data = &proc->instance;
    proc->mount.state = VFS_MOUNT_STATE_LIVE;
    *owner = &proc->mount;
    return 0;
}

int kernel_procfs_is_mount(const struct kernel_vfs_mount *mount)
{
    return mount && mount->private_data &&
           ((struct kernel_vfs_instance *)mount->private_data)->ops ==
               &proc_backend;
}

int kernel_procfs_unmount_children(struct kernel_vfs_mount *root)
{
    if (!root || !root->private_data) return -KERNEL_EINVAL;
    while (root->first_child) {
        struct kernel_vfs_mount *leaf = root->first_child;
        while (leaf->first_child) leaf = leaf->first_child;
        if (!kernel_procfs_is_mount(leaf)) return -KERNEL_ENOTSUP;
        int result = kernel_vfs_mount_detach(leaf, 0);
        if (result) return result;
        if (kernel_vfs_unmount(leaf)) __builtin_trap();
    }
    return 0;
}
