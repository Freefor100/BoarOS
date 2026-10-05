#include "vfs_objects.h"

#include <arch/riscv/context.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/fs_context.h>
#include <kernel/open_file.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/procfs.h>
#include <kernel/tmpfs.h>
#include <kernel/devpts.h>
#include <kernel/proc_task.h>
#include <kernel/scheduler.h>
#include <kernel/task.h>
#include <kernel/tick.h>
#include <kernel/time.h>
#include <kernel/socket.h>
#include <kernel/vfs.h>

#include <stdint.h>
#include <string.h>

#define PROC_ROOT_INODE UINT64_C(1)
#define PROC_MEMINFO_INODE UINT64_C(2)
#define PROC_UPTIME_INODE UINT64_C(3)
#define PROC_SELF_INODE UINT64_C(4)
#define PROC_MOUNTS_INODE UINT64_C(5)
#define PROC_SYS_INODE UINT64_C(6)
#define PROC_KERNEL_INODE UINT64_C(7)
#define PROC_RT_PERIOD_INODE UINT64_C(8)
#define PROC_RT_RUNTIME_INODE UINT64_C(9)
#define PROC_COST_INODE UINT64_C(10)
#define PROC_COST_CONTROL_INODE UINT64_C(11)
#define PROC_NETWORK_STATS_INODE UINT64_C(12)
#define PROC_MEMORY_STATS_INODE UINT64_C(13)
#define PROC_PID_DIR_KIND 1U
#define PROC_PID_EXE_KIND 2U
#define PROC_PID_CWD_KIND 3U
#define PROC_PID_ROOT_KIND 4U
#define PROC_PID_MOUNTS_KIND 5U
#define PROC_PID_STAT_KIND 6U
#define PROC_PID_STATUS_KIND 7U
#define PROC_PID_FD_DIR_KIND 8U
#define PROC_PID_FD_LINK_KIND 9U
#define PROC_SUPER_MAGIC UINT64_C(0x9fa0)

struct procfs_mount {
    struct kernel_vfs_mount mount;
    struct kernel_vfs_instance instance;
    struct kernel_heap *heap;
    uint64_t flags;
};


static uint64_t proc_pid_inode_fd(kernel_pid_t pid, uint64_t identity,
                                  uint8_t kind, uint16_t fd)
{
    if (pid <= 0 || (uint32_t)pid > UINT16_MAX || !identity ||
        identity > (UINT64_MAX >> 30U) || kind > 15U || fd > 1023U)
        __builtin_trap();
    return (identity << 30U) | ((uint64_t)kind << 26U) |
           ((uint64_t)fd << 16U) | (uint32_t)pid;
}

static uint64_t proc_pid_inode(kernel_pid_t pid, uint64_t identity,
                               uint8_t kind)
{
    return proc_pid_inode_fd(pid, identity, kind, 0U);
}

static kernel_pid_t proc_inode_pid(uint64_t inode)
{
    return (kernel_pid_t)(inode & UINT64_C(0xffff));
}

static uint64_t proc_inode_identity(uint64_t inode)
{
    return inode >> 30U;
}

static uint8_t proc_inode_kind(uint64_t inode)
{
    return (uint8_t)((inode >> 26U) & UINT64_C(0x0f));
}

static uint16_t proc_inode_fd(uint64_t inode)
{
    return (uint16_t)((inode >> 16U) & UINT64_C(0x03ff));
}

static uint32_t proc_kind_mode(uint8_t kind)
{
    if (kind == PROC_PID_DIR_KIND) return KERNEL_VFS_S_IFDIR | 0555U;
    if (kind == PROC_PID_FD_DIR_KIND) return KERNEL_VFS_S_IFDIR | 0500U;
    if (kind == PROC_PID_EXE_KIND || kind == PROC_PID_CWD_KIND ||
        kind == PROC_PID_ROOT_KIND || kind == PROC_PID_FD_LINK_KIND)
        return KERNEL_VFS_S_IFLNK | 0777U;
    return KERNEL_VFS_S_IFREG | 0444U;
}

static int proc_parse_fd(const char *name, size_t length, int *fd)
{
    if (!length || length > 4U || (name[0] == '0' && length != 1U))
        return -KERNEL_ENOENT;
    unsigned value = 0U;
    for (size_t i = 0U; i < length; i++) {
        if (name[i] < '0' || name[i] > '9') return -KERNEL_ENOENT;
        value = value * 10U + (unsigned)(name[i] - '0');
    }
    if (value >= 1024U) return -KERNEL_ENOENT;
    *fd = (int)value;
    return 0;
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
    if (parent == PROC_SYS_INODE) {
        if (length != 6U || memcmp(name, "kernel", 6U)) return -KERNEL_ENOENT;
        *inode = PROC_KERNEL_INODE;
        *mode = KERNEL_VFS_S_IFDIR | 0555U;
        return 0;
    }
    if (parent == PROC_KERNEL_INODE) {
        if (length == 18U && !memcmp(name, "sched_rt_period_us", 18U))
            *inode = PROC_RT_PERIOD_INODE;
        else if (length == 19U && !memcmp(name, "sched_rt_runtime_us", 19U))
            *inode = PROC_RT_RUNTIME_INODE;
        else return -KERNEL_ENOENT;
        *mode = KERNEL_VFS_S_IFREG | 0644U;
        return 0;
    }
    if (parent != PROC_ROOT_INODE) {
        uint8_t parent_kind = proc_inode_kind(parent);
        if (parent_kind != PROC_PID_DIR_KIND &&
            parent_kind != PROC_PID_FD_DIR_KIND)
            return -KERNEL_ENOTDIR;
        uint64_t identity;
        kernel_pid_t pid = proc_inode_pid(parent);
        if (kernel_proc_process_identity(pid, &identity) ||
            identity != proc_inode_identity(parent)) return -KERNEL_ESRCH;
        if (parent_kind == PROC_PID_FD_DIR_KIND) {
            int requested, found;
            if (proc_parse_fd(name, length, &requested) ||
                kernel_proc_next_fd(pid, identity, requested - 1, &found) ||
                found != requested) return -KERNEL_ENOENT;
            *inode = proc_pid_inode_fd(pid, identity,
                                        PROC_PID_FD_LINK_KIND,
                                        (uint16_t)requested);
            *mode = proc_kind_mode(PROC_PID_FD_LINK_KIND);
            return 0;
        }
        uint8_t kind;
        if (length == 3U && !memcmp(name, "exe", length))
            kind = PROC_PID_EXE_KIND;
        else if (length == 3U && !memcmp(name, "cwd", length))
            kind = PROC_PID_CWD_KIND;
        else if (length == 4U && !memcmp(name, "root", length))
            kind = PROC_PID_ROOT_KIND;
        else if (length == 6U && !memcmp(name, "mounts", length))
            kind = PROC_PID_MOUNTS_KIND;
        else if (length == 4U && !memcmp(name, "stat", length))
            kind = PROC_PID_STAT_KIND;
        else if (length == 6U && !memcmp(name, "status", length))
            kind = PROC_PID_STATUS_KIND;
        else if (length == 2U && !memcmp(name, "fd", length))
            kind = PROC_PID_FD_DIR_KIND;
        else return -KERNEL_ENOENT;
        *inode = proc_pid_inode(pid, identity, kind);
        *mode = proc_kind_mode(kind);
        return 0;
    }
#if BOAROS_COST_DIAGNOSTICS
    if (length == 16U && !memcmp(name,"boaros_mem_stats",length)) {
        *inode=PROC_MEMORY_STATS_INODE;*mode=KERNEL_VFS_S_IFREG|0444U;return 0;
    }
    if (length == 16U && !memcmp(name,"boaros_net_stats",length)) {
        *inode=PROC_NETWORK_STATS_INODE;*mode=KERNEL_VFS_S_IFREG|0444U;return 0;
    }
    if ((length == 11U && !memcmp(name, "boaros_cost", length)) ||
        (length == 19U && !memcmp(name, "boaros_cost_control", length))) {
        *inode = length == 11U ? PROC_COST_INODE : PROC_COST_CONTROL_INODE;
        *mode = KERNEL_VFS_S_IFREG | (length == 11U ? 0444U : 0200U);
        return 0;
    }
#endif
    if (length == 3U && !memcmp(name, "sys", length)) {
        *inode = PROC_SYS_INODE;
        *mode = KERNEL_VFS_S_IFDIR | 0555U;
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
    if (length == 6U && !memcmp(name, "mounts", length)) {
        *inode = PROC_MOUNTS_INODE;
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
         inode != PROC_MOUNTS_INODE && inode != PROC_SYS_INODE &&
         inode != PROC_KERNEL_INODE && inode != PROC_RT_PERIOD_INODE &&
         inode != PROC_RT_RUNTIME_INODE &&
#if BOAROS_COST_DIAGNOSTICS
         inode != PROC_COST_INODE && inode != PROC_COST_CONTROL_INODE &&
         inode != PROC_NETWORK_STATS_INODE && inode != PROC_MEMORY_STATS_INODE &&
#endif
         (proc_inode_kind(inode) != PROC_PID_DIR_KIND &&
          proc_inode_kind(inode) != PROC_PID_EXE_KIND &&
          proc_inode_kind(inode) != PROC_PID_CWD_KIND &&
          proc_inode_kind(inode) != PROC_PID_ROOT_KIND &&
          proc_inode_kind(inode) != PROC_PID_MOUNTS_KIND &&
          proc_inode_kind(inode) != PROC_PID_STAT_KIND &&
          proc_inode_kind(inode) != PROC_PID_STATUS_KIND &&
          proc_inode_kind(inode) != PROC_PID_FD_DIR_KIND &&
          proc_inode_kind(inode) != PROC_PID_FD_LINK_KIND)) ||
        (inode == PROC_ROOT_INODE &&
         (mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR) ||
        ((inode == PROC_SELF_INODE || inode == PROC_MOUNTS_INODE) &&
         (mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFLNK) ||
        (inode >= (UINT64_C(1) << 30U) &&
         (mode & KERNEL_VFS_S_IFMT) !=
            (proc_kind_mode(proc_inode_kind(inode)) & KERNEL_VFS_S_IFMT)) ||
        ((inode == PROC_MEMINFO_INODE || inode == PROC_UPTIME_INODE) &&
         (mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFREG))
        return -KERNEL_EINVAL;
    if (inode >= (UINT64_C(1) << 30U)) {
        uint64_t identity;
        if (kernel_proc_process_identity(proc_inode_pid(inode), &identity) ||
            identity != proc_inode_identity(inode)) return -KERNEL_ENOENT;
        if (proc_inode_kind(inode) == PROC_PID_FD_LINK_KIND) {
            int found;
            int fd = proc_inode_fd(inode);
            if (kernel_proc_next_fd(proc_inode_pid(inode), identity,
                                     fd - 1, &found) || found != fd)
                return -KERNEL_ENOENT;
        }
    }
    enum kernel_heap_status status = kernel_heap_allocate_zeroed(
        instance->heap, 1U, sizeof(*node), (void **)&node);
    if (status != KERNEL_HEAP_STATUS_OK)
        return status == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    node->generated_control = inode == PROC_RT_PERIOD_INODE ||
                              inode == PROC_RT_RUNTIME_INODE;
#if BOAROS_COST_DIAGNOSTICS
    node->generated_control |= inode == PROC_COST_CONTROL_INODE;
    node->generated_diagnostic = inode == PROC_COST_INODE || inode == PROC_COST_CONTROL_INODE ||
                                 inode == PROC_NETWORK_STATS_INODE || inode == PROC_MEMORY_STATS_INODE;
#endif
    node->inode = inode;
    node->mode = mode;
    return kernel_vfs_publish_node(mount, node, file, 0);
}

static int proc_stat(const struct kernel_vfs_file *file,
                     struct kernel_vfs_stat *stat)
{
    uint64_t inode = kernel_vfs_file_inode(file);
    uint32_t mode = file->mode;
    if (inode >= (UINT64_C(1) << 30U)) {
        uint64_t identity;
        if (kernel_proc_process_identity(proc_inode_pid(inode), &identity) ||
            identity != proc_inode_identity(inode)) return -KERNEL_ENOENT;
        if (proc_inode_kind(inode) == PROC_PID_FD_LINK_KIND) {
            int found;
            int fd = proc_inode_fd(inode);
            if (kernel_proc_next_fd(proc_inode_pid(inode), identity,
                                     fd - 1, &found) || found != fd)
                return -KERNEL_ENOENT;
            int readable, writable;
            if (kernel_proc_fd_access_snapshot(proc_inode_pid(inode),
                identity, fd, &readable, &writable)) return -KERNEL_ENOENT;
            mode = KERNEL_VFS_S_IFLNK |
                (readable ? 0500U : 0U) | (writable ? 0300U : 0U);
        }
    }
    *stat = (struct kernel_vfs_stat){
        .dev = file->mount->id,
        .ino = inode,
        .mode = mode,
        .nlink = (mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFDIR ? 2U : 1U,
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
    /* EOF 不推进 cookie；有效项必须以正值返回。 */
    *next_position = position;
    uint64_t directory = kernel_vfs_file_inode(file);
    if (directory == PROC_SYS_INODE || directory == PROC_KERNEL_INODE) {
        if (position > (directory == PROC_SYS_INODE ? 2U : 3U)) return 0;
        const char *entry = position == 0 ? "." : position == 1 ? ".." :
            directory == PROC_SYS_INODE ? "kernel" : position == 2 ?
            "sched_rt_period_us" : "sched_rt_runtime_us";
        size_t length = strlen(entry) + 1U;
        if (name_size < length) return -KERNEL_ERANGE;
        memcpy(name, entry, length);
        *next_position = position + 1U;
        *inode = position == 0 ? directory : position == 1 ?
            (directory == PROC_SYS_INODE ? PROC_ROOT_INODE : PROC_SYS_INODE) :
            directory == PROC_SYS_INODE ? PROC_KERNEL_INODE : position == 2 ?
            PROC_RT_PERIOD_INODE : PROC_RT_RUNTIME_INODE;
        *type = position < 2 || directory == PROC_SYS_INODE ? 4U : 8U;
        return 1;
    }
    if (directory != PROC_ROOT_INODE &&
        proc_inode_kind(directory) != PROC_PID_DIR_KIND &&
        proc_inode_kind(directory) != PROC_PID_FD_DIR_KIND)
        return -KERNEL_ENOTDIR;
    if (directory != PROC_ROOT_INODE) {
        uint64_t identity;
        if (kernel_proc_process_identity(proc_inode_pid(directory),
                                         &identity) ||
            identity != proc_inode_identity(directory)) return -KERNEL_ENOENT;
        if (proc_inode_kind(directory) == PROC_PID_FD_DIR_KIND) {
            if (position >= 2U) {
                if (position > 1025U) return 0;
                int fd;
                if (kernel_proc_next_fd(proc_inode_pid(directory), identity,
                                         (int)position - 3, &fd))
                    return 0;
                size_t length = decimal(name, (unsigned)fd);
                if (name_size <= length) return -KERNEL_ERANGE;
                name[length] = '\0';
                *next_position = (uint64_t)fd + 3U;
                *inode = proc_pid_inode_fd(proc_inode_pid(directory), identity,
                                           PROC_PID_FD_LINK_KIND, (uint16_t)fd);
                *type = 10U;
                return 1;
            }
            const char *entry = position == 0U ? "." : "..";
            if (name_size < strlen(entry) + 1U) return -KERNEL_ERANGE;
            strcpy(name, entry);
            *next_position = position + 1U;
            *inode = position == 0U ? directory :
                proc_pid_inode(proc_inode_pid(directory), identity,
                               PROC_PID_DIR_KIND);
            *type = 4U;
            return 1;
        }
        if (position > 8U) return 0;
        const char *entry = position == 0U ? "." :
                            position == 1U ? ".." :
                            position == 2U ? "exe" :
                            position == 3U ? "cwd" :
                            position == 4U ? "root" :
                            position == 5U ? "mounts" :
                            position == 6U ? "stat" :
                            position == 7U ? "status" : "fd";
        if (name_size < strlen(entry) + 1U) return -KERNEL_ERANGE;
        strcpy(name, entry);
        *next_position = position + 1U;
        *inode = position == 0U ? directory :
                 position == 1U ? PROC_ROOT_INODE :
                 proc_pid_inode(proc_inode_pid(directory), identity,
                     position == 2U ? PROC_PID_EXE_KIND :
                     position == 3U ? PROC_PID_CWD_KIND :
                     position == 4U ? PROC_PID_ROOT_KIND :
                     position == 5U ? PROC_PID_MOUNTS_KIND :
                     position == 6U ? PROC_PID_STAT_KIND :
                     position == 7U ? PROC_PID_STATUS_KIND :
                                      PROC_PID_FD_DIR_KIND);
        *type = position < 2U || position == 8U ? 4U :
                position >= 5U ? 8U : 10U;
        return 1;
    }
    const char *entry_name;
    if (position == 0U) entry_name = ".";
    else if (position == 1U) entry_name = "..";
    else if (position == 2U) entry_name = "meminfo";
    else if (position == 3U) entry_name = "uptime";
    else if (position == 4U) entry_name = "self";
    else if (position == 5U) entry_name = "mounts";
    else if (position == 6U) entry_name = "sys";
#if BOAROS_COST_DIAGNOSTICS
    else if (position == 7U) entry_name = "boaros_cost";
    else if (position == 8U) entry_name = "boaros_cost_control";
    else if (position == 9U) entry_name = "boaros_net_stats";
    else if (position == 10U) entry_name = "boaros_mem_stats";
    else if (position >= 11U) {
        position -= 4U;
#else
    else if (position >= 7U) {
#endif
        kernel_pid_t pid;
        uint64_t identity;
        if (position - 7U >= INT32_MAX ||
            kernel_proc_next_process((kernel_pid_t)(position - 7U),
                                      &pid, &identity)) return 0;
        size_t length = decimal(name, (uint32_t)pid);
        if (name_size <= length) return -KERNEL_ERANGE;
        name[length] = '\0';
        *next_position = 7U + (uint32_t)pid;
#if BOAROS_COST_DIAGNOSTICS
        *next_position += 4U;
#endif
        *inode = proc_pid_inode(pid, identity, PROC_PID_DIR_KIND);
        *type = 4U;
        return 1;
    } else return 0;
    size_t length = strlen(entry_name) + 1U;
    if (name_size < length) return -KERNEL_ERANGE;
    memcpy(name, entry_name, length);
    *next_position = position + 1U;
    *inode = position == 2U ? PROC_MEMINFO_INODE :
             position == 3U ? PROC_UPTIME_INODE :
             position == 4U ? PROC_SELF_INODE :
             position == 5U ? PROC_MOUNTS_INODE :
             position == 6U ? PROC_SYS_INODE :
#if BOAROS_COST_DIAGNOSTICS
             position == 7U ? PROC_COST_INODE :
             position == 8U ? PROC_COST_CONTROL_INODE :
             position == 9U ? PROC_NETWORK_STATS_INODE :
             position == 10U ? PROC_MEMORY_STATS_INODE :
#endif
             PROC_ROOT_INODE;
    *type = position == 4U || position == 5U ? 10U :
            position >= 2U && position != 6U ? 8U : 4U;
    return 1;
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
    if (node->inode == PROC_MOUNTS_INODE) {
        static const char target[] = "self/mounts";
        size_t length = sizeof(target) - 1U;
        if (length > size) length = size;
        memcpy(buffer, target, length);
        *count = length;
        return 0;
    }
    struct kernel_vfs_path *path = 0;
    uint8_t kind = proc_inode_kind(node->inode);
    enum kernel_proc_path_kind path_kind = kind == PROC_PID_EXE_KIND
        ? KERNEL_PROC_PATH_EXE : kind == PROC_PID_CWD_KIND
        ? KERNEL_PROC_PATH_CWD : kind == PROC_PID_ROOT_KIND
        ? KERNEL_PROC_PATH_ROOT : 0;
    if (!path_kind && kind != PROC_PID_FD_LINK_KIND) return -KERNEL_EINVAL;
    int result;
    if (kind == PROC_PID_FD_LINK_KIND) {
        result = kernel_proc_fd_path_acquire(proc_inode_pid(node->inode),
            proc_inode_identity(node->inode), proc_inode_fd(node->inode), &path);
        if (result) {
            struct kernel_proc_fd_pseudo pseudo = {0};
            result = kernel_proc_fd_pseudo_snapshot(proc_inode_pid(node->inode),
                proc_inode_identity(node->inode), proc_inode_fd(node->inode),
                &pseudo);
            if (result) return result;
            const char *prefix;
            size_t prefix_length;
            uint64_t identity = pseudo.object_identity;
            enum kernel_open_file_kind file_kind = pseudo.kind;
            if (file_kind == KERNEL_OPEN_FILE_KIND_PIPE)
                prefix = "pipe:[";
            else if (file_kind == KERNEL_OPEN_FILE_KIND_SOCKET)
                prefix = "socket:[";
            else if (file_kind == KERNEL_OPEN_FILE_KIND_EPOLL)
                prefix = "anon_inode:[eventpoll]";
            else prefix = 0;
            if (!prefix || (file_kind != KERNEL_OPEN_FILE_KIND_EPOLL &&
                            !identity)) result = -KERNEL_ENOENT;
            else {
                char display[64];
                prefix_length = strlen(prefix);
                memcpy(display, prefix, prefix_length);
                size_t length = prefix_length;
                if (file_kind != KERNEL_OPEN_FILE_KIND_EPOLL) {
                    length += decimal(display + length, identity);
                    display[length++] = ']';
                }
                if (length > size) length = size;
                memcpy(buffer, display, length);
                *count = length;
                result = 0;
            }
            return result;
        }
    } else result = kernel_proc_process_path_acquire(proc_inode_pid(node->inode),
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
    if (node->inode == PROC_SELF_INODE ||
        node->inode == PROC_MOUNTS_INODE) return -KERNEL_ENOTSUP;
    uint8_t kind = proc_inode_kind(node->inode);
    enum kernel_proc_path_kind path_kind = kind == PROC_PID_EXE_KIND
        ? KERNEL_PROC_PATH_EXE : kind == PROC_PID_CWD_KIND
        ? KERNEL_PROC_PATH_CWD : kind == PROC_PID_ROOT_KIND
        ? KERNEL_PROC_PATH_ROOT : 0;
    if (kind == PROC_PID_FD_LINK_KIND) {
        int result = kernel_proc_fd_path_acquire(proc_inode_pid(node->inode),
            proc_inode_identity(node->inode), proc_inode_fd(node->inode), owner);
        if (!result) return 0;
        struct kernel_proc_fd_pseudo pseudo = {0};
        result = kernel_proc_fd_pseudo_snapshot(proc_inode_pid(node->inode),
            proc_inode_identity(node->inode), proc_inode_fd(node->inode),
            &pseudo);
        if (result) return result;
        return pseudo.kind == KERNEL_OPEN_FILE_KIND_PIPE
            ? -KERNEL_ENOTSUP : -KERNEL_ENXIO;
    }
    if (!path_kind) return -KERNEL_ENOTSUP;
    return kernel_proc_process_path_acquire(proc_inode_pid(node->inode),
        proc_inode_identity(node->inode), path_kind, 0, owner);
}

static int proc_reopen_link(struct kernel_vfs_node *node,
                            struct kernel_heap *heap, uint32_t flags,
                            struct kernel_open_file_description **owner)
{
    if (proc_inode_kind(node->inode) != PROC_PID_FD_LINK_KIND)
        return -KERNEL_ENOTSUP;
    return kernel_proc_fd_reopen_link(proc_inode_pid(node->inode),
        proc_inode_identity(node->inode), proc_inode_fd(node->inode), heap,
        flags, owner);
}

static int proc_stat_link(struct kernel_vfs_node *node,
                          struct kernel_vfs_stat *stat)
{
    if (proc_inode_kind(node->inode) != PROC_PID_FD_LINK_KIND)
        return -KERNEL_ENOTSUP;
    return kernel_proc_fd_pseudo_stat(proc_inode_pid(node->inode),
        proc_inode_identity(node->inode), proc_inode_fd(node->inode), stat);
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

static int proc_space(unsigned char c)
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

static unsigned proc_digit(unsigned char c)
{
    return c >= '0' && c <= '9' ? (unsigned)(c - '0') :
           c >= 'a' && c <= 'f' ? c - 'a' + 10U :
           c >= 'A' && c <= 'F' ? c - 'A' + 10U : 16U;
}

static int proc_control(struct kernel_vfs_node *node, int write,
    uint64_t offset, char *buffer, size_t size, size_t *count)
{
    *count = 0;
#if BOAROS_COST_DIAGNOSTICS
    if (node->inode == PROC_COST_CONTROL_INODE) {
        if (!write) return -KERNEL_EACCES;
        int result = kernel_cost_control(buffer, size);
        if (!result) *count = size;
        return result;
    }
#endif
    if (!size) return 0;
    if (!write) {
        if (offset) return 0;
        int64_t period, runtime;
        kernel_scheduler_rt_bandwidth_get(&period, &runtime);
        int64_t value = node->inode == PROC_RT_RUNTIME_INODE ? runtime : period;
        char text[24];
        size_t length = 0;
        if (value < 0) text[length++] = '-';
        length += decimal(text + length, value < 0 ? (uint64_t)-value : (uint64_t)value);
        text[length++] = '\n';
        if (length > size) length = size;
        memcpy(buffer, text, length);
        *count = length;
        return 0;
    }
    /* Linux first-position-only sysctl still copies the whole write first. */
    if (offset) { *count = size; return 0; }
    size_t limit = size > BOAROS_PAGE_SIZE - 1U ? BOAROS_PAGE_SIZE - 1U : size;
    size_t position = 0;
    while (position < limit && proc_space((unsigned char)buffer[position])) position++;
    size_t start = position;
    int negative = position < limit && buffer[position] == '-';
    if (negative) position++;
    if (position == limit || buffer[position] < '0' || buffer[position] > '9')
        return -KERNEL_EINVAL;
    unsigned base = 10;
    if (buffer[position] == '0') {
        base = 8;
        if (position + 2U < limit &&
            (buffer[position + 1U] == 'x' || buffer[position + 1U] == 'X') &&
            proc_digit((unsigned char)buffer[position + 2U]) < 16U) {
            base = 16;
            position += 2U;
        }
    }
    uint64_t value = 0;
    while (position < limit && proc_digit((unsigned char)buffer[position]) < base) {
        unsigned digit = proc_digit((unsigned char)buffer[position]);
        if (value > (UINT64_MAX - digit) / base) return -KERNEL_EINVAL;
        value = value * base + digit;
        if (++position - start >= 21U) return -KERNEL_EINVAL;
    }
    if (position < limit && buffer[position] != ' ' &&
        buffer[position] != '\t' && buffer[position] != '\n') return -KERNEL_EINVAL;
    if (value > (negative ? UINT64_C(2147483648) : INT32_MAX)) return -KERNEL_EINVAL;
    int result = kernel_scheduler_rt_bandwidth_set(
        node->inode == PROC_RT_RUNTIME_INODE, negative ? -(int64_t)value : (int64_t)value);
    if (result) return result;
    while (position < limit && proc_space((unsigned char)buffer[position])) position++;
    *count = size - (limit - position);
    return 0;
}

static size_t append_kib(char *buffer, const char *label, uint64_t bytes)
{
    size_t length = strlen(label);
    memcpy(buffer, label, length);
    length += decimal(buffer + length, bytes / 1024U);
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

struct proc_mount_entry {
    struct kernel_vfs_path *path;
    struct kernel_vfs_path *mount_root;
    const char *source;
    uint8_t is_proc, is_tmpfs, is_devpts;
    uint8_t read_only;
};

static struct kernel_vfs_mount *next_mount(struct kernel_vfs_mount *mount,
                                           struct kernel_vfs_mount *top)
{
    if (mount->first_child) return mount->first_child;
    while (mount != top) {
        if (mount->next_sibling) return mount->next_sibling;
        mount = mount->parent;
    }
    return 0;
}

static void release_mount_entries(struct kernel_heap *heap,
                                   struct proc_mount_entry **owner,
                                   size_t count)
{
    if (!*owner) return;
    for (size_t i = 0U; i < count; i++) {
        if ((*owner)[i].path && kernel_vfs_path_release(&(*owner)[i].path))
            __builtin_trap();
        if ((*owner)[i].mount_root &&
            kernel_vfs_path_release(&(*owner)[i].mount_root))
            __builtin_trap();
    }
    if (kernel_heap_release(heap, *owner) != KERNEL_HEAP_STATUS_OK)
        __builtin_trap();
    *owner = 0;
}

static size_t append_escaped(char *buffer, const char *source)
{
    size_t used = 0U;
    for (; *source; source++) {
        unsigned char c = (unsigned char)*source;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\\' || c == '#') {
            buffer[used++] = '\\';
            buffer[used++] = (char)('0' + ((c >> 6U) & 7U));
            buffer[used++] = (char)('0' + ((c >> 3U) & 7U));
            buffer[used++] = (char)('0' + (c & 7U));
        } else buffer[used++] = (char)c;
    }
    return used;
}

static int proc_mounts_snapshot(struct kernel_vfs_node *node,
                                struct kernel_heap *heap,
                                char **buffer, size_t *length)
{
    const struct kernel_fs_context *fs = 0;
    if (kernel_task_fs_context_borrow(kernel_task_current(), &fs) !=
        KERNEL_TASK_STATUS_OK) return -KERNEL_ENOENT;
    struct kernel_vfs_path *root_path = kernel_fs_context_root(fs);
    if (!root_path) return -KERNEL_ENOENT;
    struct kernel_vfs_mount *top = node->mount;
    while (top->parent) top = top->parent;
    struct proc_mount_entry *entries = 0;
    size_t captured = 0U;
    int result = -KERNEL_EAGAIN;
    for (unsigned attempt = 0U; attempt < 3U; attempt++) {
        size_t count = 0U;
        uintptr_t irq = riscv_interrupt_save();
        for (struct kernel_vfs_mount *it = top; it;
             it = next_mount(it, top)) count++;
        riscv_interrupt_restore(irq);
        if (!count || count > SIZE_MAX / sizeof(*entries) ||
            count > SIZE_MAX / (8U * KERNEL_FS_PATH_MAX + 80U))
            return -KERNEL_EOVERFLOW;
        enum kernel_heap_status allocation = kernel_heap_allocate_zeroed(
            heap, count, sizeof(*entries), (void **)&entries);
        if (allocation != KERNEL_HEAP_STATUS_OK)
            return allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM
                                                          : -KERNEL_EIO;
        irq = riscv_interrupt_save();
        int changed = 0;
        for (struct kernel_vfs_mount *it = top; it;
             it = next_mount(it, top)) {
            if (captured == count) { changed = 1; break; }
            struct kernel_vfs_path *path = it == top ? root_path
                                                     : it->covered_path;
            struct kernel_vfs_path *mount_root = it == top ? root_path
                                                           : it->root_path;
            if (!path || !mount_root || kernel_vfs_path_acquire(path)) {
                changed = 1;
                break;
            }
            if (kernel_vfs_path_acquire(mount_root)) {
                (void)kernel_vfs_path_release(&path);
                changed = 1;
                break;
            }
            entries[captured].path = path;
            entries[captured].mount_root = mount_root;
            entries[captured].source = it->source_name;
            entries[captured].is_proc = kernel_procfs_is_mount(it);
            entries[captured].is_tmpfs = kernel_tmpfs_is_mount(it);
            entries[captured].is_devpts = kernel_devpts_is_mount(it);
            entries[captured].read_only =
                ((struct kernel_vfs_instance *)it->private_data)->read_only;
            captured++;
        }
        riscv_interrupt_restore(irq);
        if (!changed) break;
        release_mount_entries(heap, &entries, captured);
        captured = 0U;
        if (attempt == 2U) return result;
    }
    size_t capacity = captured * (8U * KERNEL_FS_PATH_MAX + 80U);
    char *data = 0, *path_text = 0;
    enum kernel_heap_status allocation = kernel_heap_allocate(heap, capacity,
                                                              (void **)&data);
    if (allocation != KERNEL_HEAP_STATUS_OK) {
        result = allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM
                                                        : -KERNEL_EIO;
        goto Finish;
    }
    allocation = kernel_heap_allocate(heap, KERNEL_FS_PATH_MAX,
                                       (void **)&path_text);
    if (allocation != KERNEL_HEAP_STATUS_OK) {
        result = allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM
                                                        : -KERNEL_EIO;
        goto Finish;
    }
    size_t used = 0U;
    for (size_t i = 0U; i < captured; i++) {
        result = kernel_vfs_path_string(entries[i].path, root_path,
                                        path_text, KERNEL_FS_PATH_MAX);
        if (result == -KERNEL_ENOENT) { result = 0; continue; }
        if (result) goto Finish;
        const char *source = entries[i].source ? entries[i].source :
                             entries[i].is_proc ? "proc" :
                             entries[i].is_tmpfs ? "tmpfs" : entries[i].is_devpts ? "devpts" : "rootfs";
        const char *kind = entries[i].is_proc ? "proc" :
                           entries[i].is_tmpfs ? "tmpfs" : entries[i].is_devpts ? "devpts" : "ext4";
        size_t needed = 4U * strlen(source) + 1U + 4U * strlen(path_text) +
                        1U + strlen(kind) + 9U;
        if (needed > capacity - used) { result = -KERNEL_EOVERFLOW; goto Finish; }
        size_t part;
        used += append_escaped(data + used, source);
        data[used++] = ' ';
        used += append_escaped(data + used, path_text);
        data[used++] = ' ';
        part = strlen(kind);
        memcpy(data + used, kind, part);
        used += part;
        const char *suffix = entries[i].read_only ? " ro 0 0\n" :
                                                    " rw 0 0\n";
        memcpy(data + used, suffix, 8U);
        used += 8U;
    }
    data[used] = '\0';
    *buffer = data;
    *length = used;
    data = 0;
    result = 0;
Finish:
    if (path_text && kernel_heap_release(heap, path_text) !=
                         KERNEL_HEAP_STATUS_OK) __builtin_trap();
    if (data && kernel_heap_release(heap, data) != KERNEL_HEAP_STATUS_OK)
        __builtin_trap();
    release_mount_entries(heap, &entries, captured);
    return result;
}

static size_t append_number_line(char *buffer, const char *label,
                                 uint64_t number, const char *unit)
{
    size_t used = strlen(label);
    memcpy(buffer, label, used);
    used += decimal(buffer + used, number);
    size_t suffix = strlen(unit);
    memcpy(buffer + used, unit, suffix);
    return used + suffix;
}

static int proc_process_snapshot(struct kernel_vfs_node *node,
                                 struct kernel_heap *heap,
                                 char **buffer, size_t *length)
{
    struct kernel_proc_process_snapshot process;
    int result = kernel_proc_process_snapshot(proc_inode_pid(node->inode),
        proc_inode_identity(node->inode), &process);
    if (result) return result;
    char *data = 0;
    enum kernel_heap_status allocation = kernel_heap_allocate(heap, 1024U,
                                                              (void **)&data);
    if (allocation != KERNEL_HEAP_STATUS_OK)
        return allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM
                                                      : -KERNEL_EIO;
    size_t used = 0U;
    if (proc_inode_kind(node->inode) == PROC_PID_STAT_KIND) {
        used = decimal(data, (uint32_t)process.pid);
        data[used++] = ' ';
        data[used++] = '(';
        size_t name = strlen(process.comm);
        memcpy(data + used, process.comm, name);
        used += name;
        data[used++] = ')';
        data[used++] = ' ';
        data[used++] = process.state;
        const uint64_t prefix[] = {
            (uint32_t)process.ppid, (uint32_t)process.process_group,
            (uint32_t)process.session_id, process.tty_nr,
        };
        for (size_t i = 0U; i < sizeof(prefix) / sizeof(prefix[0]); i++) {
            data[used++] = ' ';
            used += decimal(data + used, prefix[i]);
        }
        data[used++] = ' ';
        if (process.tty_pgrp < 0) {
            data[used++] = '-';
            used += decimal(data + used, (uint32_t)(-process.tty_pgrp));
        } else used += decimal(data + used, (uint32_t)process.tty_pgrp);
        const uint64_t fields[] = {
            0U, process.minor_faults, process.child_minor_faults,
            process.major_faults, process.child_major_faults,
            process.user_ticks, process.kernel_ticks,
            process.child_user_ticks, process.child_kernel_ticks,
            (uint64_t)(int64_t)process.scheduling_priority,
            0U, process.threads, 0U, process.start_ticks,
            process.virtual_bytes, process.resident_pages,
            UINT64_MAX, process.start_code, process.end_code,
            process.start_stack, 0U, 0U,
            process.signal_pending, process.signal_blocked,
            process.signal_ignored, process.signal_caught,
            process.wait_channel_flag, 0U, 0U, 17U, 0U, process.rt_priority,
            process.scheduling_policy,
        };
        for (size_t i = 0U; i < sizeof(fields) / sizeof(fields[0]); i++) {
            data[used++] = ' ';
            if (i == 9U && process.scheduling_priority < 0) {
                data[used++] = '-';
                used += decimal(data + used,
                                (uint32_t)-process.scheduling_priority);
            } else used += decimal(data + used, fields[i]);
        }
        data[used++] = '\n';
    } else {
        static const char name_prefix[] = "Name:\t";
        memcpy(data, name_prefix, sizeof(name_prefix) - 1U);
        used = sizeof(name_prefix) - 1U;
        size_t name = strlen(process.comm);
        memcpy(data + used, process.comm, name);
        used += name;
        data[used++] = '\n';
        static const char state_prefix[] = "State:\t";
        memcpy(data + used, state_prefix, sizeof(state_prefix) - 1U);
        used += sizeof(state_prefix) - 1U;
        data[used++] = process.state;
        const char *state_name = process.state == 'R' ? " (running)\n" :
                                 process.state == 'Z' ? " (zombie)\n" :
                                 process.state == 'T' ? " (stopped)\n" :
                                                        " (sleeping)\n";
        size_t state_length = strlen(state_name);
        memcpy(data + used, state_name, state_length);
        used += state_length;
        used += append_number_line(data + used, "Tgid:\t",
                                   (uint32_t)process.pid, "\n");
        used += append_number_line(data + used, "Pid:\t",
                                   (uint32_t)process.pid, "\n");
        used += append_number_line(data + used, "PPid:\t",
                                   (uint32_t)process.ppid, "\n");
        used += append_number_line(data + used, "Threads:\t",
                                   process.threads, "\n");
        used += append_number_line(data + used, "VmSize:\t",
                                   process.virtual_bytes / 1024U, " kB\n");
        used += append_number_line(data + used, "VmRSS:\t",
                                   process.resident_pages *
                                      (BOAROS_PAGE_SIZE / 1024U), " kB\n");
        static const char identity[] =
            "Uid:\t0\t0\t0\t0\nGid:\t0\t0\t0\t0\n";
        memcpy(data + used, identity, sizeof(identity) - 1U);
        used += sizeof(identity) - 1U;
    }
    if (used >= 1024U) __builtin_trap();
    data[used] = '\0';
    *buffer = data;
    *length = used;
    return 0;
}

static int proc_snapshot(struct kernel_vfs_node *node, struct kernel_heap *heap,
                         char **buffer, size_t *length)
{
#if BOAROS_COST_DIAGNOSTICS
    if (node->inode == PROC_MEMORY_STATS_INODE) {
        struct kernel_heap_statistics hs;
        uint64_t irq = riscv_interrupt_save();
        kernel_heap_get_statistics(heap, &hs);
        uint64_t current = heap->page_allocator->total_pages - heap->page_allocator->available_pages;
        uint64_t peak = heap->page_allocator->allocated_peak_pages;
        riscv_interrupt_restore(irq);
        char *data = 0;
        enum kernel_heap_status result = kernel_heap_allocate(heap, 256, (void **)&data);
        if (result != KERNEL_HEAP_STATUS_OK) return result == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
        size_t used = append_number_line(data,"managed_current_bytes=",current * BOAROS_PAGE_SIZE,"\n");
        used += append_number_line(data+used,"managed_peak_bytes=",peak * BOAROS_PAGE_SIZE,"\n");
        used += append_number_line(data+used,"heap_current_bytes=",hs.current_pages * BOAROS_PAGE_SIZE,"\n");
        used += append_number_line(data+used,"heap_peak_bytes=",hs.peak_pages * BOAROS_PAGE_SIZE,"\n");
        if (used >= 256) __builtin_trap();
        data[used] = 0; *buffer = data; *length = used; return 0;
    }
    if (node->inode == PROC_NETWORK_STATS_INODE) {
        static const char *const keys[KERNEL_SOCKET_PROTOCOL_VALUES] = {
            "tcp_write_calls=","tcp_written_bytes=","tcp_xmit=","tcp_recv=","tcp_memerr=",
            "udp_xmit=","udp_recv=","udp_memerr=","heap_used=","heap_max=",
            "active_pcbs=","listen_pcbs=","segments=","udp_pcbs=","pbufs=","counter_bits=",
            "tcp_segment_peak=","pbuf_peak=","pbuf_pool_used=","pbuf_pool_peak=","tcp_active_peak=","tcp_listen_peak="
        };
        char *data=0;uint64_t values[KERNEL_SOCKET_PROTOCOL_VALUES];
        enum kernel_heap_status allocation=kernel_heap_allocate(heap,1024,(void **)&data);
        if (allocation != KERNEL_HEAP_STATUS_OK) return allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
        kernel_socket_protocol_snapshot(values);
        size_t used=0;
        for (unsigned i=0;i<KERNEL_SOCKET_PROTOCOL_VALUES;i++)
            used += append_number_line(data+used,keys[i],values[i],"\n");
        if (used>=1024) __builtin_trap();
        data[used]=0;*buffer=data;*length=used;return 0;
    }
    if (node->inode == PROC_COST_INODE) {
        char *data = 0;
        size_t capacity = kernel_cost_format_capacity();
        enum kernel_heap_status status = kernel_heap_allocate(heap, capacity, (void **)&data);
        if (status != KERNEL_HEAP_STATUS_OK) return status == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
        uint64_t interrupts = riscv_interrupt_save();
        int result = kernel_cost_format(data, capacity);
        riscv_interrupt_restore(interrupts);
        if (result < 0) { if (kernel_heap_release(heap, data) != KERNEL_HEAP_STATUS_OK) __builtin_trap(); return result; }
        *buffer = data; *length = (size_t)result;
        return 0;
    }
#endif
    if (proc_inode_kind(node->inode) == PROC_PID_STAT_KIND ||
        proc_inode_kind(node->inode) == PROC_PID_STATUS_KIND)
        return proc_process_snapshot(node, heap, buffer, length);
    if (proc_inode_kind(node->inode) == PROC_PID_MOUNTS_KIND) {
        uint64_t identity = 0U;
        if (kernel_proc_process_identity(proc_inode_pid(node->inode),
                                         &identity) ||
            identity != proc_inode_identity(node->inode)) return -KERNEL_ENOENT;
        return proc_mounts_snapshot(node, heap, buffer, length);
    }
    if (node->inode != PROC_MEMINFO_INODE &&
        node->inode != PROC_UPTIME_INODE) return -KERNEL_EINVAL;
    char *data = 0;
    enum kernel_heap_status status = kernel_heap_allocate(heap, 512U,
                                                           (void **)&data);
    if (status != KERNEL_HEAP_STATUS_OK)
        return status == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    size_t used = 0U;
    if (node->inode == PROC_MEMINFO_INODE) {
        struct kernel_memory_statistics memory;
        uintptr_t irq = riscv_interrupt_save();
        kernel_memory_snapshot(heap->page_allocator, &memory);
        riscv_interrupt_restore(irq);
        const char *names[] = {"MemTotal:       ", "MemFree:        ",
            "MemAvailable:   ", "Buffers:        ", "Cached:         ",
            "Shmem:          ", "Dirty:          ", "Writeback:      ",
            "SReclaimable:   ", "SwapTotal:      ", "SwapFree:       "};
        uint64_t bytes[] = {memory.total, memory.free, memory.available,
            memory.buffers, memory.cached, memory.shared, memory.dirty,
            memory.writeback, 0, 0, 0};
        for (unsigned i = 0; i < sizeof(bytes) / sizeof(bytes[0]); i++)
            used += append_kib(data + used, names[i], bytes[i]);
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
    ops->reopen_link = proc_reopen_link;
    ops->stat_link = proc_stat_link;
    ops->close_node = proc_close_node;
    ops->open = proc_open;
    ops->stat = proc_stat;
    ops->statfs = proc_statfs;
    ops->dir_entry = proc_dir_entry;
    ops->snapshot = proc_snapshot;
    ops->control = proc_control;
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
    proc->mount.id = kernel_vfs_allocate_mount_id();
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

        int result = kernel_vfs_mount_prepare_detach(leaf, 0);
        if (result) return result;
        result = kernel_vfs_mount_detach(leaf, 0);
        if (result) return result;
        result = kernel_vfs_unmount(leaf);
        if (result) { kernel_vfs_disk_defer_cleanup(leaf); return result; }
    }
    return 0;
}
