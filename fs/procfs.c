#include "vfs_objects.h"

#include <arch/riscv/context.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/fs_context.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/procfs.h>
#include <kernel/proc_task.h>
#include <kernel/scheduler.h>
#include <kernel/task.h>
#include <kernel/tick.h>
#include <kernel/time.h>
#include <kernel/vfs.h>

#include <stdint.h>
#include <string.h>

#define PROC_ROOT_INODE UINT64_C(1)
#define PROC_MEMINFO_INODE UINT64_C(2)
#define PROC_UPTIME_INODE UINT64_C(3)
#define PROC_SELF_INODE UINT64_C(4)
#define PROC_PID_DIR_KIND 1U
#define PROC_PID_EXE_KIND 2U
#define PROC_PID_CWD_KIND 3U
#define PROC_PID_ROOT_KIND 4U
#define PROC_SUPER_MAGIC UINT64_C(0x9fa0)

struct procfs_mount {
    struct kernel_vfs_mount mount;
    struct kernel_vfs_instance instance;
    struct kernel_heap *heap;
    uint64_t flags;
};

static uint64_t next_proc_mount_id = 2U;

static uint64_t proc_pid_inode(kernel_pid_t pid, uint64_t identity,
                               uint8_t kind)
{
    return (identity << 24U) | ((uint64_t)kind << 16U) | (uint32_t)pid;
}

static kernel_pid_t proc_inode_pid(uint64_t inode)
{
    return (kernel_pid_t)(inode & UINT64_C(0xffff));
}

static uint64_t proc_inode_identity(uint64_t inode)
{
    return inode >> 24U;
}

static uint8_t proc_inode_kind(uint64_t inode)
{
    return (uint8_t)((inode >> 16U) & UINT64_C(0xff));
}

static size_t decimal(char *buffer, uint64_t value);

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
    if (parent != PROC_ROOT_INODE) {
        if (proc_inode_kind(parent) != PROC_PID_DIR_KIND)
            return -KERNEL_ENOTDIR;
        uint64_t identity;
        kernel_pid_t pid = proc_inode_pid(parent);
        if (kernel_proc_process_identity(pid, &identity) ||
            identity != proc_inode_identity(parent)) return -KERNEL_ENOENT;
        uint8_t kind;
        if (length == 3U && !memcmp(name, "exe", length))
            kind = PROC_PID_EXE_KIND;
        else if (length == 3U && !memcmp(name, "cwd", length))
            kind = PROC_PID_CWD_KIND;
        else if (length == 4U && !memcmp(name, "root", length))
            kind = PROC_PID_ROOT_KIND;
        else return -KERNEL_ENOENT;
        *inode = proc_pid_inode(pid, identity, kind);
        *mode = KERNEL_VFS_S_IFLNK | 0777U;
        return 0;
    }
    if (length == 7U && !memcmp(name, "meminfo", length)) {
        *inode = PROC_MEMINFO_INODE;
        *mode = KERNEL_VFS_S_IFREG | 0444U;
        return 0;
    }
    if (length == 6U && !memcmp(name, "uptime", length)) {
        *inode = PROC_UPTIME_INODE;
        *mode = KERNEL_VFS_S_IFREG | 0444U;
        return 0;
    }
    if (length == 4U && !memcmp(name, "self", length)) {
        *inode = PROC_SELF_INODE;
        *mode = KERNEL_VFS_S_IFLNK | 0777U;
        return 0;
    }
    if (length == 0U || length > 5U || name[0] == '0')
        return -KERNEL_ENOENT;
    uint32_t pid = 0U;
    for (size_t i = 0U; i < length; i++) {
        if (name[i] < '0' || name[i] > '9') return -KERNEL_ENOENT;
        pid = pid * 10U + (uint32_t)(name[i] - '0');
    }
    uint64_t identity = 0U;
    if (kernel_proc_process_identity((kernel_pid_t)pid, &identity))
        return -KERNEL_ENOENT;
    *inode = proc_pid_inode((kernel_pid_t)pid, identity,
                            PROC_PID_DIR_KIND);
    *mode = KERNEL_VFS_S_IFDIR | 0555U;
    return 0;
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
    if ((inode != PROC_ROOT_INODE && inode != PROC_MEMINFO_INODE &&
         inode != PROC_UPTIME_INODE && inode != PROC_SELF_INODE &&
         (proc_inode_kind(inode) != PROC_PID_DIR_KIND &&
          proc_inode_kind(inode) != PROC_PID_EXE_KIND &&
          proc_inode_kind(inode) != PROC_PID_CWD_KIND &&
          proc_inode_kind(inode) != PROC_PID_ROOT_KIND)) ||
        (inode == PROC_ROOT_INODE &&
         (mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR) ||
        (inode == PROC_SELF_INODE &&
         (mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFLNK) ||
        (inode >= (UINT64_C(1) << 24U) &&
         (mode & KERNEL_VFS_S_IFMT) !=
            (proc_inode_kind(inode) == PROC_PID_DIR_KIND
                ? KERNEL_VFS_S_IFDIR : KERNEL_VFS_S_IFLNK)) ||
        ((inode == PROC_MEMINFO_INODE || inode == PROC_UPTIME_INODE) &&
         (mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFREG))
        return -KERNEL_EINVAL;
    if (inode >= (UINT64_C(1) << 24U)) {
        uint64_t identity;
        if (kernel_proc_process_identity(proc_inode_pid(inode), &identity) ||
            identity != proc_inode_identity(inode)) return -KERNEL_ENOENT;
    }
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
    uint64_t inode = kernel_vfs_file_inode(file);
    if (inode >= (UINT64_C(1) << 24U)) {
        uint64_t identity;
        if (kernel_proc_process_identity(proc_inode_pid(inode), &identity) ||
            identity != proc_inode_identity(inode)) return -KERNEL_ENOENT;
    }
    *stat = (struct kernel_vfs_stat){
        .dev = file->mount->id,
        .ino = inode,
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
    uint64_t directory = kernel_vfs_file_inode(file);
    if (directory != PROC_ROOT_INODE &&
        proc_inode_kind(directory) != PROC_PID_DIR_KIND)
        return -KERNEL_ENOTDIR;
    if (directory != PROC_ROOT_INODE) {
        uint64_t identity;
        if (kernel_proc_process_identity(proc_inode_pid(directory),
                                         &identity) ||
            identity != proc_inode_identity(directory)) return -KERNEL_ENOENT;
        if (position > 4U) return -KERNEL_ENOENT;
        const char *entry = position == 0U ? "." :
                            position == 1U ? ".." :
                            position == 2U ? "exe" :
                            position == 3U ? "cwd" : "root";
        if (name_size < strlen(entry) + 1U) return -KERNEL_ERANGE;
        strcpy(name, entry);
        *next_position = position + 1U;
        *inode = position == 0U ? directory :
                 position == 1U ? PROC_ROOT_INODE :
                 proc_pid_inode(proc_inode_pid(directory), identity,
                     position == 2U ? PROC_PID_EXE_KIND :
                     position == 3U ? PROC_PID_CWD_KIND :
                                      PROC_PID_ROOT_KIND);
        *type = position < 2U ? 4U : 10U;
        return 0;
    }
    const char *entry_name;
    if (position == 0U) entry_name = ".";
    else if (position == 1U) entry_name = "..";
    else if (position == 2U) entry_name = "meminfo";
    else if (position == 3U) entry_name = "uptime";
    else if (position == 4U) entry_name = "self";
    else if (position >= 5U) {
        kernel_pid_t pid;
        uint64_t identity;
        if (position - 5U >= INT32_MAX ||
            kernel_proc_next_process((kernel_pid_t)(position - 5U),
                                      &pid, &identity)) return -KERNEL_ENOENT;
        size_t length = decimal(name, (uint32_t)pid);
        if (name_size <= length) return -KERNEL_ERANGE;
        name[length] = '\0';
        *next_position = 5U + (uint32_t)pid;
        *inode = proc_pid_inode(pid, identity, PROC_PID_DIR_KIND);
        *type = 4U;
        return 0;
    } else return -KERNEL_ENOENT;
    size_t length = strlen(entry_name) + 1U;
    if (name_size < length) return -KERNEL_ERANGE;
    memcpy(name, entry_name, length);
    *next_position = position + 1U;
    *inode = position == 2U ? PROC_MEMINFO_INODE :
             position == 3U ? PROC_UPTIME_INODE :
             position == 4U ? PROC_SELF_INODE : PROC_ROOT_INODE;
    *type = position == 4U ? 10U : position >= 2U ? 8U : 4U;
    return 0;
}

static int proc_readlink(struct kernel_vfs_node *node, char *buffer,
                         size_t size, size_t *count)
{
    if (!buffer || !count) return -KERNEL_EINVAL;
    if (node->inode == PROC_SELF_INODE) {
        kernel_pid_t tgid;
        if (kernel_task_tgid(kernel_task_current(), &tgid) !=
            KERNEL_TASK_STATUS_OK) return -KERNEL_ENOENT;
        char digits[24];
        size_t length = decimal(digits, (uint32_t)tgid);
        if (length > size) length = size;
        memcpy(buffer, digits, length);
        *count = length;
        return 0;
    }
    struct kernel_vfs_path *path = 0;
    uint8_t kind = proc_inode_kind(node->inode);
    enum kernel_proc_path_kind path_kind = kind == PROC_PID_EXE_KIND
        ? KERNEL_PROC_PATH_EXE : kind == PROC_PID_CWD_KIND
        ? KERNEL_PROC_PATH_CWD : kind == PROC_PID_ROOT_KIND
        ? KERNEL_PROC_PATH_ROOT : 0;
    if (!path_kind) return -KERNEL_EINVAL;
    int result = kernel_proc_process_path_acquire(proc_inode_pid(node->inode),
        proc_inode_identity(node->inode), path_kind, 0, &path);
    if (result) return result;
    const struct kernel_fs_context *fs = 0;
    if (kernel_task_fs_context_borrow(kernel_task_current(), &fs) !=
        KERNEL_TASK_STATUS_OK) {
        result = -KERNEL_ENOENT;
        goto Finish;
    }
    char *text = 0;
    enum kernel_heap_status allocation = kernel_heap_allocate(node->instance->heap,
        KERNEL_FS_PATH_MAX, (void **)&text);
    if (allocation != KERNEL_HEAP_STATUS_OK) {
        result = allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM
                                                        : -KERNEL_EIO;
        goto Finish;
    }
    result = kernel_vfs_path_link_string(path, kernel_fs_context_root(fs),
                                         text, KERNEL_FS_PATH_MAX);
    if (!result) {
        size_t length = strlen(text);
        if (length > size) length = size;
        memcpy(buffer, text, length);
        *count = length;
    }
    if (kernel_heap_release(node->instance->heap, text) !=
        KERNEL_HEAP_STATUS_OK) __builtin_trap();
Finish:
    if (kernel_vfs_path_release(&path)) __builtin_trap();
    return result;
}

static int proc_follow_link(struct kernel_vfs_node *node,
                            struct kernel_vfs_path **owner)
{
    if (node->inode == PROC_SELF_INODE) return -KERNEL_ENOTSUP;
    uint8_t kind = proc_inode_kind(node->inode);
    enum kernel_proc_path_kind path_kind = kind == PROC_PID_EXE_KIND
        ? KERNEL_PROC_PATH_EXE : kind == PROC_PID_CWD_KIND
        ? KERNEL_PROC_PATH_CWD : kind == PROC_PID_ROOT_KIND
        ? KERNEL_PROC_PATH_ROOT : 0;
    if (!path_kind) return -KERNEL_ENOTSUP;
    return kernel_proc_process_path_acquire(proc_inode_pid(node->inode),
        proc_inode_identity(node->inode), path_kind, 0, owner);
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

static size_t append_hundredths(char *buffer, uint64_t value)
{
    size_t length = decimal(buffer, value / 100U);
    buffer[length++] = '.';
    buffer[length++] = (char)('0' + value / 10U % 10U);
    buffer[length++] = (char)('0' + value % 10U);
    return length;
}

static int proc_snapshot(struct kernel_vfs_node *node, struct kernel_heap *heap,
                         char **buffer, size_t *length)
{
    if (node->inode != PROC_MEMINFO_INODE &&
        node->inode != PROC_UPTIME_INODE) return -KERNEL_EINVAL;
    char *data = 0;
    enum kernel_heap_status status = kernel_heap_allocate(heap, 128U,
                                                           (void **)&data);
    if (status != KERNEL_HEAP_STATUS_OK)
        return status == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    size_t used = 0U;
    if (node->inode == PROC_MEMINFO_INODE) {
        struct physical_page_allocator *allocator = heap->page_allocator;
        used = append_kib(data, "MemTotal:       ",
                          physical_page_total(allocator));
        used += append_kib(data + used, "MemFree:        ",
                           physical_page_available(allocator));
    } else {
        uint64_t uptime = kernel_time_monotonic_ns() / 10000000U;
        uint64_t idle_ticks = kernel_scheduler_idle_ticks();
        uint64_t idle = (idle_ticks / KERNEL_TICKS_PER_SECOND) * 100U +
                        (idle_ticks % KERNEL_TICKS_PER_SECOND) * 100U /
                            KERNEL_TICKS_PER_SECOND;
        used = append_hundredths(data, uptime);
        data[used++] = ' ';
        used += append_hundredths(data + used, idle);
        data[used++] = '\n';
    }
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
    ops->readlink = proc_readlink;
    ops->follow_link = proc_follow_link;
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
