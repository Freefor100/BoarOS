#include <kernel/aio.h>
#include <kernel/block.h>
#include <kernel/accounting.h>
#include "mount_context.h"
#include "lease.h"
#include "pipe_internal.h"
#include "vfs_objects.h"
#include <kernel/credentials.h>
#include <kernel/hugetlb.h>
#include <arch/mmu.h>
#include <kernel/bpf.h>

#include <arch/context.h>
#include <arch/smp.h>
#if !defined(BOAROS_ARCH_LOONGARCH)
#include <arch/riscv/hwprobe.h>
#endif
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/fanotify.h>
#include <kernel/inotify.h>
#include <kernel/fs_context.h>
#include <kernel/keys.h>
#include <kernel/open_file.h>
#include <kernel/mqueue.h>
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
#include <kernel/uts.h>
#include <kernel/tty_task.h>
#include <kernel/socket.h>
#include <kernel/bpf.h>
#include <kernel/shm.h>
#include <kernel/ipc_namespace.h>
#include <kernel/sem.h>
#include <kernel/msg.h>
#include <kernel/namespace.h>
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
#define PROC_PID_MAX_INODE UINT64_C(14)
#define PROC_THREAD_SELF_INODE UINT64_C(15)
#define PROC_SYSVIPC_INODE UINT64_C(16)
#define PROC_SYSVIPC_SHM_INODE UINT64_C(17)
#define PROC_SYSVIPC_SEM_INODE UINT64_C(18)
#define PROC_SYSVIPC_MSG_INODE UINT64_C(19)
#define PROC_NET_INODE UINT64_C(20)
#define PROC_NET_ROUTE_INODE UINT64_C(21)
#define PROC_UNPRIVILEGED_BPF_INODE UINT64_C(22)
#define PROC_HOSTNAME_INODE UINT64_C(23)
#define PROC_DOMAINNAME_INODE UINT64_C(24)
#define PROC_RR_TIMESLICE_INODE UINT64_C(25)
#define PROC_VM_INODE UINT64_C(26)
#define PROC_NR_HUGEPAGES_INODE UINT64_C(27)
#define PROC_FS_INODE UINT64_C(28)
#define PROC_AIO_NR_INODE UINT64_C(29)
#define PROC_AIO_MAX_NR_INODE UINT64_C(30)
#define PROC_KPAGEFLAGS_INODE UINT64_C(31)
#define PROC_DROP_CACHES_INODE UINT64_C(32)
#define PROC_CACHE_PRESSURE_INODE UINT64_C(33)
#define PROC_FILESYSTEMS_INODE UINT64_C(34)
#define PROC_LEASE_BREAK_TIME_INODE UINT64_C(35)
#define PROC_PIPE_MAX_SIZE_INODE UINT64_C(36)
#define PROC_CPUINFO_INODE UINT64_C(37)
#define PROC_KEYS_INODE UINT64_C(38)
#define PROC_KEY_QUOTA_FIRST_INODE UINT64_C(39)
#define PROC_SHM_LIMIT_FIRST_INODE (PROC_KEY_QUOTA_FIRST_INODE + KERNEL_KEY_QUOTA_COUNT)
#define PROC_STAT_INODE (PROC_SHM_LIMIT_FIRST_INODE + KERNEL_SHM_LIMIT_COUNT)
#define PROC_DISKSTATS_INODE (PROC_STAT_INODE + 1U)

static const char *const proc_key_quota_names[KERNEL_KEY_QUOTA_COUNT] = {
    [KERNEL_KEY_QUOTA_MAXKEYS] = "maxkeys",
    [KERNEL_KEY_QUOTA_MAXBYTES] = "maxbytes",
    [KERNEL_KEY_QUOTA_ROOT_MAXKEYS] = "root_maxkeys",
    [KERNEL_KEY_QUOTA_ROOT_MAXBYTES] = "root_maxbytes",
};

static int proc_is_key_quota(uint64_t inode)
{
    return inode - PROC_KEY_QUOTA_FIRST_INODE < KERNEL_KEY_QUOTA_COUNT;
}

static const char *const proc_shm_limit_names[KERNEL_SHM_LIMIT_COUNT] = {
    [KERNEL_SHM_LIMIT_MAX] = "shmmax",
    [KERNEL_SHM_LIMIT_ALL] = "shmall",
    [KERNEL_SHM_LIMIT_MNI] = "shmmni",
};

static int proc_is_shm_limit(uint64_t inode)
{
    return inode - PROC_SHM_LIMIT_FIRST_INODE < KERNEL_SHM_LIMIT_COUNT;
}

static const struct proc_root_entry {
    const char *name;
    uint64_t inode;
    uint8_t type;
} proc_root_entries[] = {
    {".", PROC_ROOT_INODE, 4}, {"..", PROC_ROOT_INODE, 4},
    {"meminfo", PROC_MEMINFO_INODE, 8}, {"uptime", PROC_UPTIME_INODE, 8},
    {"self", PROC_SELF_INODE, 10}, {"mounts", PROC_MOUNTS_INODE, 10},
    {"sys", PROC_SYS_INODE, 4}, {"thread-self", PROC_THREAD_SELF_INODE, 10},
    {"sysvipc", PROC_SYSVIPC_INODE, 4}, {"net", PROC_NET_INODE, 4},
    {"kpageflags", PROC_KPAGEFLAGS_INODE, 8},
    {"filesystems", PROC_FILESYSTEMS_INODE, 8},
    {"cpuinfo", PROC_CPUINFO_INODE, 8},
    {"stat", PROC_STAT_INODE, 8}, {"diskstats", PROC_DISKSTATS_INODE, 8},
#if BOAROS_COST_DIAGNOSTICS
    {"boaros_cost", PROC_COST_INODE, 8}, {"boaros_cost_control", PROC_COST_CONTROL_INODE, 8},
    {"boaros_net_stats", PROC_NETWORK_STATS_INODE, 8}, {"boaros_mem_stats", PROC_MEMORY_STATS_INODE, 8},
#endif
};
#define PROC_ROOT_STATIC_COUNT (sizeof(proc_root_entries)/sizeof(proc_root_entries[0]))
#define PROC_TASK_DIR_KIND 10U
#define PROC_TID_DIR_KIND 11U
#define PROC_TID_STAT_KIND 12U
#define PROC_TID_STATUS_KIND 13U
#define PROC_PID_MAPS_KIND 14U
#define PROC_PID_FDINFO_DIR_KIND 15U
#define PROC_PID_FDINFO_KIND 0U /* Dynamic identity distinguishes it from static inodes. */
#define PROC_PID_DIR_KIND 1U
#define PROC_PID_EXE_KIND 2U
#define PROC_PID_CWD_KIND 3U
#define PROC_PID_ROOT_KIND 4U
#define PROC_PID_MOUNTS_KIND 5U
#define PROC_PID_STAT_KIND 6U
#define PROC_PID_STATUS_KIND 7U
#define PROC_PID_FD_DIR_KIND 8U
#define PROC_PID_FD_LINK_KIND 9U
#define PROC_PID_NS_DIR_KIND 16U
#define PROC_PID_MNT_NS_KIND 17U
#define PROC_PID_UTS_NS_KIND 19U
#define PROC_PID_IPC_NS_KIND 20U
#define PROC_PID_COMM_KIND 21U
#define PROC_TID_COMM_KIND 22U
#define PROC_PID_CGROUP_KIND 23U
#define PROC_TID_CGROUP_KIND 24U
#define PROC_PID_FILTER_KIND 25U
#define PROC_FILTER_BITS 0x306U
#define PROC_PID_PID_NS_KIND 26U
#define PROC_PID_CHILDREN_NS_KIND 27U
#define PROC_PID_IO_KIND 28U
#define PROC_IO_BITS 0x308U
#define PROC_PID_PAGEMAP_KIND 29U
#define PROC_PAGEMAP_BITS 0x30aU
#define PROC_PID_TIMERSLACK_KIND 30U
#define PROC_TIMERSLACK_BITS 0x310U
#define PROC_TIMERSLACK_WRITE_MAX 66U
#define PROC_CGROUP_BITS 0x304U
#define PROC_COMM_BITS 0x302U
#define PROC_NS_DIR_BITS 0x100U
#define PROC_MNT_NS_BITS 0x200U
#define PROC_UTS_NS_BITS 0x202U
#define PROC_IPC_NS_BITS 0x204U
#define PROC_PID_NS_BITS 0x206U
#define PROC_CHILDREN_NS_BITS 0x208U
#define PROC_MOUNTINFO_BITS 0x300U
#define PROC_PID_MOUNTINFO_KIND 18U
#define PROC_SUPER_MAGIC UINT64_C(0x9fa0)

struct procfs_mount {
    struct kernel_vfs_mount mount;
    struct kernel_vfs_instance instance;
    struct kernel_heap *heap;
    uint64_t flags;
    struct kernel_pid_registry *pid_view;
};

static uint32_t proc_namespace_type(uint8_t kind)
{
    return kind==PROC_PID_MNT_NS_KIND ? KERNEL_NS_MNT :
        kind==PROC_PID_UTS_NS_KIND ? KERNEL_NS_UTS :
        kind==PROC_PID_IPC_NS_KIND ? KERNEL_NS_IPC :
        kind==PROC_PID_PID_NS_KIND ? KERNEL_NS_PID :
        kind==PROC_PID_CHILDREN_NS_KIND ? KERNEL_NS_PID|KERNEL_PROC_PID_FOR_CHILDREN : 0;
}

static const char *proc_namespace_name(uint8_t kind)
{
    return kind==PROC_PID_MNT_NS_KIND ? "mnt" : kind==PROC_PID_UTS_NS_KIND ? "uts" :
        kind==PROC_PID_PID_NS_KIND ? "pid" : kind==PROC_PID_CHILDREN_NS_KIND ? "pid_for_children" : "ipc";
}


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
    if (kind == PROC_PID_IO_KIND)
        return proc_pid_inode_fd(pid, identity, PROC_PID_MOUNTS_KIND, PROC_IO_BITS);
    if (kind == PROC_PID_PAGEMAP_KIND)
        return proc_pid_inode_fd(pid, identity, PROC_PID_MOUNTS_KIND, PROC_PAGEMAP_BITS);
    if (kind == PROC_PID_TIMERSLACK_KIND)
        return proc_pid_inode_fd(pid, identity, PROC_PID_MOUNTS_KIND, PROC_TIMERSLACK_BITS);
    if (kind == PROC_PID_FILTER_KIND)
        return proc_pid_inode_fd(pid, identity, PROC_PID_MOUNTS_KIND,
            PROC_FILTER_BITS);
    if (kind == PROC_PID_CGROUP_KIND || kind == PROC_TID_CGROUP_KIND)
        return proc_pid_inode_fd(pid, identity, PROC_PID_MOUNTS_KIND,
            PROC_CGROUP_BITS | (kind == PROC_TID_CGROUP_KIND));
    if (kind == PROC_PID_COMM_KIND || kind == PROC_TID_COMM_KIND)
        return proc_pid_inode_fd(pid, identity, PROC_PID_MOUNTS_KIND,
            PROC_COMM_BITS | (kind == PROC_TID_COMM_KIND));
    if (kind == PROC_PID_NS_DIR_KIND || proc_namespace_type(kind) || kind==PROC_PID_MOUNTINFO_KIND)
        return proc_pid_inode_fd(pid,identity,PROC_PID_MOUNTS_KIND,
            kind==PROC_PID_NS_DIR_KIND?PROC_NS_DIR_BITS:kind==PROC_PID_MNT_NS_KIND?PROC_MNT_NS_BITS:
            kind==PROC_PID_UTS_NS_KIND?PROC_UTS_NS_BITS:kind==PROC_PID_IPC_NS_KIND?PROC_IPC_NS_BITS:
            kind==PROC_PID_PID_NS_KIND?PROC_PID_NS_BITS:kind==PROC_PID_CHILDREN_NS_KIND?PROC_CHILDREN_NS_BITS:PROC_MOUNTINFO_BITS);
    if (kind == PROC_TID_STAT_KIND || kind == PROC_TID_STATUS_KIND)
        return proc_pid_inode_fd(pid, identity,
            kind == PROC_TID_STAT_KIND ? PROC_PID_STAT_KIND : PROC_PID_STATUS_KIND, 1U);
    return proc_pid_inode_fd(pid, identity, kind, 0U);
}

/* Kinds 12 and 13 encode thread fd leaves; thread stat/status use an
 * otherwise unused fd bit. This preserves all PID generation and fd bits. */
static uint64_t proc_thread_resource_inode(kernel_pid_t pid, uint64_t identity,
                                            uint8_t kind, uint16_t fd)
{
    if (kind == PROC_PID_NS_DIR_KIND || proc_namespace_type(kind) ||
        kind == PROC_PID_MOUNTINFO_KIND || kind == PROC_PID_IO_KIND || kind == PROC_PID_PAGEMAP_KIND)
        return proc_pid_inode(pid,identity,kind)|(UINT64_C(1)<<16U);
    if (kind == PROC_PID_FD_LINK_KIND || kind == PROC_PID_FDINFO_KIND)
        return proc_pid_inode_fd(pid, identity,
            kind == PROC_PID_FD_LINK_KIND ? 12U : 13U, fd);
    return proc_pid_inode_fd(pid, identity, kind, 1U);
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
    uint8_t kind = (uint8_t)((inode >> 26U) & UINT64_C(0x0f));
    if (kind == 12U) return PROC_PID_FD_LINK_KIND;
    if (kind == 13U) return PROC_PID_FDINFO_KIND;
    if (kind == PROC_PID_MOUNTS_KIND) {
        unsigned encoded=(unsigned)(inode>>16U)&0x3feU;
        if(encoded==PROC_NS_DIR_BITS)return PROC_PID_NS_DIR_KIND;
        if(encoded==PROC_MNT_NS_BITS)return PROC_PID_MNT_NS_KIND;
        if(encoded==PROC_UTS_NS_BITS)return PROC_PID_UTS_NS_KIND;
        if(encoded==PROC_IPC_NS_BITS)return PROC_PID_IPC_NS_KIND;
        if(encoded==PROC_PID_NS_BITS)return PROC_PID_PID_NS_KIND;
        if(encoded==PROC_CHILDREN_NS_BITS)return PROC_PID_CHILDREN_NS_KIND;
        if(encoded==PROC_MOUNTINFO_BITS)return PROC_PID_MOUNTINFO_KIND;
        if(encoded==PROC_TIMERSLACK_BITS)return PROC_PID_TIMERSLACK_KIND;
        if(encoded==PROC_FILTER_BITS)return PROC_PID_FILTER_KIND;
        if(encoded==PROC_IO_BITS)return PROC_PID_IO_KIND;
        if(encoded==PROC_PAGEMAP_BITS)return PROC_PID_PAGEMAP_KIND;
        if(encoded==PROC_CGROUP_BITS)return (inode & (UINT64_C(1)<<16U)) ? PROC_TID_CGROUP_KIND : PROC_PID_CGROUP_KIND;
        if(encoded==PROC_COMM_BITS)return (inode & (UINT64_C(1)<<16U)) ? PROC_TID_COMM_KIND : PROC_PID_COMM_KIND;
    }
    if ((inode & (UINT64_C(1) << 16U)) && kind == PROC_PID_STAT_KIND)
        return PROC_TID_STAT_KIND;
    if ((inode & (UINT64_C(1) << 16U)) && kind == PROC_PID_STATUS_KIND)
        return PROC_TID_STATUS_KIND;
    return kind;
}

static uint16_t proc_inode_fd(uint64_t inode)
{
    return (uint16_t)((inode >> 16U) & UINT64_C(0x03ff));
}

static uint32_t proc_kind_mode(uint8_t kind)
{
    if (kind == PROC_PID_DIR_KIND || kind == PROC_TASK_DIR_KIND || kind == PROC_PID_NS_DIR_KIND ||
        kind == PROC_TID_DIR_KIND) return KERNEL_VFS_S_IFDIR | 0555U;
    if (kind == PROC_PID_FD_DIR_KIND || kind==PROC_PID_FDINFO_DIR_KIND) return KERNEL_VFS_S_IFDIR | 0500U;
    if (kind == PROC_PID_EXE_KIND || kind == PROC_PID_CWD_KIND ||
        kind == PROC_PID_ROOT_KIND || kind == PROC_PID_FD_LINK_KIND || proc_namespace_type(kind))
        return KERNEL_VFS_S_IFLNK | 0777U;
    if (kind == PROC_PID_IO_KIND || kind == PROC_PID_PAGEMAP_KIND)
        return KERNEL_VFS_S_IFREG | 0400U;
    if (kind == PROC_PID_TIMERSLACK_KIND) return KERNEL_VFS_S_IFREG | 0666U;
    return KERNEL_VFS_S_IFREG | ((kind == PROC_PID_COMM_KIND || kind == PROC_TID_COMM_KIND || kind == PROC_PID_FILTER_KIND) ? 0644U : 0444U);
}

static int proc_inode_is_thread(uint64_t inode)
{
    uint8_t raw = (uint8_t)((inode >> 26U) & 15U);
    return raw == PROC_TID_DIR_KIND || raw == 12U || raw == 13U ||
        ((inode & (UINT64_C(1) << 16U)) &&
         (raw == PROC_PID_EXE_KIND || raw == PROC_PID_CWD_KIND ||
          raw == PROC_PID_ROOT_KIND || raw == PROC_PID_MAPS_KIND ||
          raw == PROC_PID_FD_DIR_KIND || raw == PROC_PID_FDINFO_DIR_KIND ||
          raw == PROC_PID_STAT_KIND || raw == PROC_PID_STATUS_KIND || raw == PROC_PID_MOUNTS_KIND));
}

static uint64_t proc_resource_identity(uint64_t inode)
{
    return proc_inode_identity(inode) |
        (proc_inode_is_thread(inode) ? KERNEL_PROC_THREAD_TARGET : 0U);
}

static int proc_inode_identity_current(uint64_t inode, uint64_t *identity)
{
    return proc_inode_is_thread(inode)
        ? kernel_proc_thread_identity(0, 0, proc_inode_pid(inode), identity)
        : kernel_proc_process_identity(proc_inode_pid(inode), identity);
}

static int proc_parse_pid(const char *name, size_t length, kernel_pid_t *pid)
{
    if (!length || length > 5U || name[0] == '0') return -KERNEL_ENOENT;
    uint32_t value = 0;
    for (size_t i = 0; i < length; i++) {
        if (name[i] < '0' || name[i] > '9') return -KERNEL_ENOENT;
        value = value * 10U + (unsigned)(name[i] - '0');
    }
    if (value > UINT16_MAX) return -KERNEL_ENOENT;
    *pid = (kernel_pid_t)value;
    return 0;
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
    if (parent == PROC_NET_INODE) {
        if (length != 5U || memcmp(name, "route", 5U)) return -KERNEL_ENOENT;
        *inode = PROC_NET_ROUTE_INODE;
        *mode = KERNEL_VFS_S_IFREG | 0444U;
        return 0;
    }
    if (parent == PROC_SYSVIPC_INODE) {
        if (length != 3U) return -KERNEL_ENOENT;
        if (!memcmp(name, "shm", 3U)) *inode = PROC_SYSVIPC_SHM_INODE;
        else if (!memcmp(name, "sem", 3U)) *inode = PROC_SYSVIPC_SEM_INODE;
        else if (!memcmp(name, "msg", 3U)) *inode = PROC_SYSVIPC_MSG_INODE;
        else return -KERNEL_ENOENT;
        *mode = KERNEL_VFS_S_IFREG | 0444U;
        return 0;
    }
    if (parent == PROC_SYS_INODE) {
        if (length==6U && !memcmp(name,"kernel",6U)) *inode=PROC_KERNEL_INODE;
        else if (length==2U && !memcmp(name,"fs",2U)) *inode=PROC_FS_INODE;
        else if (length==2U && !memcmp(name,"vm",2U)) *inode=PROC_VM_INODE;
        else return -KERNEL_ENOENT;
        *mode = KERNEL_VFS_S_IFDIR | 0555U;
        return 0;
    }
    if (parent==PROC_FS_INODE) {
        if (length==6 && !memcmp(name,"aio-nr",6)) *inode=PROC_AIO_NR_INODE;
        else if (length==10 && !memcmp(name,"aio-max-nr",10)) *inode=PROC_AIO_MAX_NR_INODE;
        else if (length==16 && !memcmp(name,"lease-break-time",16)) *inode=PROC_LEASE_BREAK_TIME_INODE;
        else if (length==13 && !memcmp(name,"pipe-max-size",13)) *inode=PROC_PIPE_MAX_SIZE_INODE;
        else return -KERNEL_ENOENT;
        *mode=KERNEL_VFS_S_IFREG|(*inode==PROC_AIO_NR_INODE?0444U:0644U);
        return 0;
    }
    if (parent==PROC_VM_INODE) {
        if (length==12U && !memcmp(name,"nr_hugepages",12U)) *inode=PROC_NR_HUGEPAGES_INODE;
        else if (length==11U && !memcmp(name,"drop_caches",11U)) *inode=PROC_DROP_CACHES_INODE;
        else if (length==18U && !memcmp(name,"vfs_cache_pressure",18U)) *inode=PROC_CACHE_PRESSURE_INODE;
        else return -KERNEL_ENOENT;
        *mode=KERNEL_VFS_S_IFREG|(*inode==PROC_DROP_CACHES_INODE ? 0200U : 0644U);
        return 0;
    }
    if (parent == PROC_KEYS_INODE) {
        for (unsigned i = 0; i < KERNEL_KEY_QUOTA_COUNT; i++) {
            const char *entry = proc_key_quota_names[i];
            if (length == strlen(entry) && !memcmp(name, entry, length)) {
                *inode = PROC_KEY_QUOTA_FIRST_INODE + i;
                *mode = KERNEL_VFS_S_IFREG | 0644U;
                return 0;
            }
        }
        return -KERNEL_ENOENT;
    }
    if (parent == PROC_KERNEL_INODE) {
        if (length == 4U && !memcmp(name, "keys", 4U)) {
            *inode = PROC_KEYS_INODE;
            *mode = KERNEL_VFS_S_IFDIR | 0555U;
            return 0;
        }
        for (unsigned i = 0; i < KERNEL_SHM_LIMIT_COUNT; i++) {
            if (length == 6U && !memcmp(name, proc_shm_limit_names[i], 6U)) {
                *inode = PROC_SHM_LIMIT_FIRST_INODE + i;
                *mode = KERNEL_VFS_S_IFREG | 0644U;
                return 0;
            }
        }
        if (length == 18U && !memcmp(name, "sched_rt_period_us", 18U))
            *inode = PROC_RT_PERIOD_INODE;
        else if (length == 19U && !memcmp(name, "sched_rt_runtime_us", 19U))
            *inode = PROC_RT_RUNTIME_INODE;
        else if (length == 21U && !memcmp(name, "sched_rr_timeslice_ms", 21U))
            *inode = PROC_RR_TIMESLICE_INODE;
        else if (length == 7U && !memcmp(name, "pid_max", 7U))
            *inode = PROC_PID_MAX_INODE;
        else if(length==25U && !memcmp(name,"unprivileged_bpf_disabled",25U))
            *inode=PROC_UNPRIVILEGED_BPF_INODE;
        else if (length == 8U && !memcmp(name, "hostname", 8U))
            *inode = PROC_HOSTNAME_INODE;
        else if (length == 10U && !memcmp(name, "domainname", 10U))
            *inode = PROC_DOMAINNAME_INODE;
        else return -KERNEL_ENOENT;
        *mode = KERNEL_VFS_S_IFREG |
            (*inode == PROC_PID_MAX_INODE ? 0444U : 0644U);
        return 0;
    }
    if (parent != PROC_ROOT_INODE) {
        uint8_t parent_kind = proc_inode_kind(parent);
        if (parent_kind != PROC_PID_DIR_KIND &&
            parent_kind != PROC_PID_FD_DIR_KIND &&
            parent_kind != PROC_PID_FDINFO_DIR_KIND &&
            parent_kind != PROC_PID_NS_DIR_KIND &&
            parent_kind != PROC_TASK_DIR_KIND && parent_kind != PROC_TID_DIR_KIND)
            return -KERNEL_ENOTDIR;
        uint64_t identity;
        kernel_pid_t pid = proc_inode_pid(parent);
        if (proc_inode_identity_current(parent, &identity) ||
            identity != proc_inode_identity(parent)) {
            /* Linux线程目录由lookup报告缺失，进程目录先走PID权限检查。 */
            return parent_kind == PROC_TID_DIR_KIND ? -KERNEL_ENOENT : -KERNEL_ESRCH;
        }
        if (parent_kind == PROC_PID_NS_DIR_KIND) {
            if(length!=3U && !(length==16U && !memcmp(name,"pid_for_children",16)))return -KERNEL_ENOENT;
            uint8_t kind=length==16U ? PROC_PID_CHILDREN_NS_KIND :!memcmp(name,"mnt",3) ? PROC_PID_MNT_NS_KIND :
                !memcmp(name,"uts",3) ? PROC_PID_UTS_NS_KIND :
                !memcmp(name,"ipc",3) ? PROC_PID_IPC_NS_KIND :
                !memcmp(name,"pid",3) ? PROC_PID_PID_NS_KIND : 0;
            if(!kind)return -KERNEL_ENOENT;
            *inode=proc_pid_inode(pid,identity,kind)|
                (parent & (UINT64_C(1)<<16U));
            *mode=proc_kind_mode(kind);
            return 0;
        }
        if (parent_kind == PROC_TASK_DIR_KIND) {
            kernel_pid_t tid;
            uint64_t thread_identity;
            if (proc_parse_pid(name, length, &tid) ||
                kernel_proc_view_lookup(((struct procfs_mount *)instance->backend_data)->pid_view,
                    tid, pid, identity, 1, &tid, &thread_identity))
                return -KERNEL_ENOENT;
            *inode = proc_pid_inode(tid, thread_identity, PROC_TID_DIR_KIND);
            *mode = proc_kind_mode(PROC_TID_DIR_KIND);
            return 0;
        }
        if (parent_kind == PROC_TID_DIR_KIND) {
            uint8_t kind = length == 4U && !memcmp(name, "stat", length)
                ? PROC_TID_STAT_KIND : length == 6U && !memcmp(name, "status", length)
                ? PROC_TID_STATUS_KIND : length == 3U && !memcmp(name, "exe", length)
                ? PROC_PID_EXE_KIND : length == 3U && !memcmp(name, "cwd", length)
                ? PROC_PID_CWD_KIND : length == 4U && !memcmp(name, "root", length)
                ? PROC_PID_ROOT_KIND : length == 4U && !memcmp(name, "maps", length)
                ? PROC_PID_MAPS_KIND : length == 2U && !memcmp(name, "fd", length)
                ? PROC_PID_FD_DIR_KIND : length == 6U && !memcmp(name, "fdinfo", length)
                ? PROC_PID_FDINFO_DIR_KIND : length == 2U && !memcmp(name,"ns",length)
                ? PROC_PID_NS_DIR_KIND : length == 6U && !memcmp(name,"mounts",length)
                ? PROC_PID_MOUNTS_KIND : length==9U && !memcmp(name,"mountinfo",length)
                ? PROC_PID_MOUNTINFO_KIND : length == 4U && !memcmp(name,"comm",length)
                ? PROC_TID_COMM_KIND : length == 6U && !memcmp(name,"cgroup",length)
                ? PROC_TID_CGROUP_KIND : length == 2U && !memcmp(name,"io",length)
                ? PROC_PID_IO_KIND : length == 7U && !memcmp(name,"pagemap",length)
                ? PROC_PID_PAGEMAP_KIND : 0;
            if (!kind) return -KERNEL_ENOENT;
            if (kind != PROC_TID_STAT_KIND && kind != PROC_TID_STATUS_KIND && kind != PROC_TID_COMM_KIND && kind != PROC_TID_CGROUP_KIND &&
                kernel_proc_thread_resources_live(pid, identity))
                return -KERNEL_ENOENT;
            *inode = kind == PROC_TID_STAT_KIND || kind == PROC_TID_STATUS_KIND || kind == PROC_TID_COMM_KIND || kind == PROC_TID_CGROUP_KIND
                ? proc_pid_inode(pid, identity, kind)
                : proc_thread_resource_inode(pid, identity, kind, 0);
            *mode = proc_kind_mode(kind);
            return 0;
        }
        if (parent_kind == PROC_PID_FD_DIR_KIND || parent_kind==PROC_PID_FDINFO_DIR_KIND) {
            int requested, found;
            if (proc_parse_fd(name, length, &requested) ||
                kernel_proc_next_fd(pid, proc_resource_identity(parent), requested - 1, &found) ||
                found != requested) return -KERNEL_ENOENT;
            uint8_t leaf = parent_kind == PROC_PID_FD_DIR_KIND ? PROC_PID_FD_LINK_KIND : PROC_PID_FDINFO_KIND;
            *inode = proc_inode_is_thread(parent)
                ? proc_thread_resource_inode(pid, identity, leaf, (uint16_t)requested)
                : proc_pid_inode_fd(pid, identity, leaf, (uint16_t)requested);
            *mode = proc_kind_mode(parent_kind==PROC_PID_FD_DIR_KIND?PROC_PID_FD_LINK_KIND:PROC_PID_FDINFO_KIND);
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
        else if(length==9U && !memcmp(name,"mountinfo",length))
            kind=PROC_PID_MOUNTINFO_KIND;
        else if (length == 4U && !memcmp(name, "stat", length))
            kind = PROC_PID_STAT_KIND;
        else if (length == 6U && !memcmp(name, "status", length))
            kind = PROC_PID_STATUS_KIND;
        else if (length == 2U && !memcmp(name,"io",length))
            kind = PROC_PID_IO_KIND;
        else if (length == 7U && !memcmp(name,"pagemap",length))
            kind = PROC_PID_PAGEMAP_KIND;
        else if (length == 13U && !memcmp(name, "timerslack_ns", length))
            kind = PROC_PID_TIMERSLACK_KIND;
        else if (length == 15U && !memcmp(name, "coredump_filter", length))
            kind = PROC_PID_FILTER_KIND;
        else if (length == 6U && !memcmp(name, "cgroup", length))
            kind = PROC_PID_CGROUP_KIND;
        else if (length == 4U && !memcmp(name, "comm", length))
            kind = PROC_PID_COMM_KIND;
        else if (length == 4U && !memcmp(name, "maps", length))
            kind = PROC_PID_MAPS_KIND;
        else if (length == 4U && !memcmp(name, "task", length))
            kind = PROC_TASK_DIR_KIND;
        else if (length == 2U && !memcmp(name, "fd", length))
            kind = PROC_PID_FD_DIR_KIND;
        else if (length == 6U && !memcmp(name,"fdinfo",length))
            kind = PROC_PID_FDINFO_DIR_KIND;
        else if (length == 2U && !memcmp(name,"ns",length))
            kind = PROC_PID_NS_DIR_KIND;
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
    if (length == 7U && !memcmp(name, "sysvipc", length)) {
        *inode = PROC_SYSVIPC_INODE;
        *mode = KERNEL_VFS_S_IFDIR | 0555U;
        return 0;
    }
    if (length == 3U && !memcmp(name, "net", length)) {
        *inode = PROC_NET_INODE;
        *mode = KERNEL_VFS_S_IFDIR | 0555U;
        return 0;
    }
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
    if (length == 7U && !memcmp(name, "cpuinfo", length)) {
        *inode = PROC_CPUINFO_INODE;
        *mode = KERNEL_VFS_S_IFREG | 0444U;
        return 0;
    }
    if (length == 4U && !memcmp(name, "stat", length)) {
        *inode = PROC_STAT_INODE;
        *mode = KERNEL_VFS_S_IFREG | 0444U;
        return 0;
    }
    if (length == 9U && !memcmp(name, "diskstats", length)) {
        *inode = PROC_DISKSTATS_INODE;
        *mode = KERNEL_VFS_S_IFREG | 0444U;
        return 0;
    }
    if (length==11U && !memcmp(name,"filesystems",length)) {
        *inode=PROC_FILESYSTEMS_INODE;
        *mode=KERNEL_VFS_S_IFREG|0444U;
        return 0;
    }
    if (length == 10U && !memcmp(name,"kpageflags",length)) {
        *inode=PROC_KPAGEFLAGS_INODE;
        *mode=KERNEL_VFS_S_IFREG | 0400U;
        return 0;
    }
    if (length == 6U && !memcmp(name, "uptime", length)) {
        *inode = PROC_UPTIME_INODE;
        *mode = KERNEL_VFS_S_IFREG | 0444U;
        return 0;
    }
    if (length == 11U && !memcmp(name, "thread-self", length)) {
        *inode = PROC_THREAD_SELF_INODE;
        *mode = KERNEL_VFS_S_IFLNK | 0777U;
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
    kernel_pid_t pid;
    if (proc_parse_pid(name, length, &pid)) return -KERNEL_ENOENT;
    uint64_t identity = 0U;
    if (kernel_proc_view_lookup(((struct procfs_mount *)instance->backend_data)->pid_view,
            pid, 0, 0, 0, &pid, &identity))
        return -KERNEL_ENOENT;
    *inode = proc_pid_inode((kernel_pid_t)pid, identity,
                            PROC_PID_DIR_KIND);
    *mode = KERNEL_VFS_S_IFDIR | 0555U;
    return 0;
}

static int proc_close_node(struct kernel_vfs_node *node)
{
    if(node->backend_data) {
        kernel_task_identity_put(node->backend_data);
        node->backend_data=0;
    }
    return 0;
}

static int proc_open(struct kernel_vfs_mount *mount, const char *path,
                     uint64_t inode, uint32_t mode,
                     struct kernel_vfs_file *file)
{
    struct kernel_vfs_instance *instance = mount->private_data;
    struct kernel_vfs_node *node = 0;
    (void)path;
    if ((inode != PROC_ROOT_INODE && inode != PROC_MEMINFO_INODE && inode != PROC_CPUINFO_INODE &&
         inode != PROC_STAT_INODE && inode != PROC_DISKSTATS_INODE &&
         inode != PROC_KPAGEFLAGS_INODE &&
         inode != PROC_DROP_CACHES_INODE && inode != PROC_CACHE_PRESSURE_INODE && inode != PROC_FILESYSTEMS_INODE &&
         inode != PROC_UPTIME_INODE && inode != PROC_SELF_INODE && inode != PROC_THREAD_SELF_INODE &&
         inode != PROC_MOUNTS_INODE && inode != PROC_SYS_INODE &&
         inode != PROC_FS_INODE && inode != PROC_AIO_NR_INODE && inode != PROC_AIO_MAX_NR_INODE &&
         inode != PROC_LEASE_BREAK_TIME_INODE && inode != PROC_PIPE_MAX_SIZE_INODE &&
         inode != PROC_KERNEL_INODE && inode != PROC_VM_INODE &&
         inode != PROC_KEYS_INODE && !proc_is_key_quota(inode) && !proc_is_shm_limit(inode) &&
         inode != PROC_NR_HUGEPAGES_INODE && inode != PROC_RT_PERIOD_INODE &&
         inode != PROC_RT_RUNTIME_INODE && inode != PROC_RR_TIMESLICE_INODE && inode != PROC_PID_MAX_INODE &&
         inode != PROC_UNPRIVILEGED_BPF_INODE &&
         inode != PROC_HOSTNAME_INODE && inode != PROC_DOMAINNAME_INODE &&
         inode != PROC_SYSVIPC_INODE && inode != PROC_SYSVIPC_SHM_INODE &&
         inode != PROC_SYSVIPC_SEM_INODE && inode != PROC_SYSVIPC_MSG_INODE &&
         inode != PROC_NET_INODE && inode != PROC_NET_ROUTE_INODE &&
#if BOAROS_COST_DIAGNOSTICS
         inode != PROC_COST_INODE && inode != PROC_COST_CONTROL_INODE &&
         inode != PROC_NETWORK_STATS_INODE && inode != PROC_MEMORY_STATS_INODE &&
#endif
         (proc_inode_kind(inode) != PROC_PID_DIR_KIND &&
          proc_inode_kind(inode) != PROC_PID_EXE_KIND &&
          proc_inode_kind(inode) != PROC_PID_CWD_KIND &&
          proc_inode_kind(inode) != PROC_PID_ROOT_KIND &&
          proc_inode_kind(inode) != PROC_PID_MOUNTS_KIND &&
          proc_inode_kind(inode) != PROC_PID_MOUNTINFO_KIND &&
          proc_inode_kind(inode) != PROC_PID_STAT_KIND &&
          proc_inode_kind(inode) != PROC_PID_STATUS_KIND &&
          proc_inode_kind(inode) != PROC_PID_IO_KIND &&
          proc_inode_kind(inode) != PROC_PID_PAGEMAP_KIND &&
          proc_inode_kind(inode) != PROC_PID_FD_DIR_KIND &&
          proc_inode_kind(inode) != PROC_PID_FD_LINK_KIND &&
          proc_inode_kind(inode) != PROC_TASK_DIR_KIND &&
          proc_inode_kind(inode) != PROC_TID_DIR_KIND &&
          proc_inode_kind(inode) != PROC_TID_STAT_KIND &&
          proc_inode_kind(inode) != PROC_TID_STATUS_KIND &&
          proc_inode_kind(inode) != PROC_PID_TIMERSLACK_KIND &&
          proc_inode_kind(inode) != PROC_PID_FILTER_KIND &&
          proc_inode_kind(inode) != PROC_PID_CGROUP_KIND &&
          proc_inode_kind(inode) != PROC_TID_CGROUP_KIND &&
          proc_inode_kind(inode) != PROC_PID_COMM_KIND &&
          proc_inode_kind(inode) != PROC_TID_COMM_KIND &&
          proc_inode_kind(inode) != PROC_PID_MAPS_KIND &&
          proc_inode_kind(inode) != PROC_PID_FDINFO_DIR_KIND &&
          proc_inode_kind(inode) != PROC_PID_NS_DIR_KIND &&
          !proc_namespace_type(proc_inode_kind(inode)) &&
          !(inode >= (UINT64_C(1)<<30U) && proc_inode_kind(inode)==PROC_PID_FDINFO_KIND))) ||
        ((inode == PROC_ROOT_INODE || inode == PROC_SYSVIPC_INODE || inode == PROC_NET_INODE ||
          inode == PROC_SYS_INODE || inode == PROC_KERNEL_INODE || inode == PROC_VM_INODE || inode == PROC_FS_INODE ||
          inode == PROC_KEYS_INODE) &&
         (mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR) ||
        ((inode == PROC_SELF_INODE || inode == PROC_THREAD_SELF_INODE || inode == PROC_MOUNTS_INODE) &&
         (mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFLNK) ||
        (inode >= (UINT64_C(1) << 30U) &&
         (mode & KERNEL_VFS_S_IFMT) !=
            (proc_kind_mode(proc_inode_kind(inode)) & KERNEL_VFS_S_IFMT)) ||
        ((inode == PROC_MEMINFO_INODE || inode == PROC_CPUINFO_INODE ||
          inode == PROC_STAT_INODE || inode == PROC_DISKSTATS_INODE ||
          inode == PROC_KPAGEFLAGS_INODE || inode == PROC_UPTIME_INODE ||
          inode == PROC_DROP_CACHES_INODE || inode == PROC_CACHE_PRESSURE_INODE || inode == PROC_FILESYSTEMS_INODE ||
          inode == PROC_AIO_NR_INODE || inode == PROC_AIO_MAX_NR_INODE ||
          inode == PROC_LEASE_BREAK_TIME_INODE || inode == PROC_PIPE_MAX_SIZE_INODE ||
          inode == PROC_PID_MAX_INODE || inode == PROC_NR_HUGEPAGES_INODE || inode == PROC_SYSVIPC_SHM_INODE ||
          inode == PROC_SYSVIPC_SEM_INODE || inode == PROC_SYSVIPC_MSG_INODE ||
          inode == PROC_NET_ROUTE_INODE || inode == PROC_HOSTNAME_INODE ||
          inode == PROC_DOMAINNAME_INODE || proc_is_key_quota(inode) || proc_is_shm_limit(inode)) &&
         (mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFREG))
        return -KERNEL_EINVAL;
    if (inode >= (UINT64_C(1) << 30U)) {
        uint64_t identity;
        if (proc_inode_identity_current(inode, &identity) ||
            identity != proc_inode_identity(inode)) return -KERNEL_ENOENT;
        if (proc_inode_kind(inode) == PROC_PID_FD_LINK_KIND || proc_inode_kind(inode)==PROC_PID_FDINFO_KIND) {
            int found;
            int fd = proc_inode_fd(inode);
            if (kernel_proc_next_fd(proc_inode_pid(inode), proc_resource_identity(inode),
                                     fd - 1, &found) || found != fd)
                return -KERNEL_ENOENT;
        }
    }
    enum kernel_heap_status status = kernel_heap_allocate_zeroed(
        instance->heap, 1U, sizeof(*node), (void **)&node);
    if (status != KERNEL_HEAP_STATUS_OK)
        return status == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    node->generated_control = (inode == PROC_RT_PERIOD_INODE ||
                              inode == PROC_RT_RUNTIME_INODE || inode == PROC_RR_TIMESLICE_INODE ||
                              inode==PROC_UNPRIVILEGED_BPF_INODE || inode==PROC_NR_HUGEPAGES_INODE || inode==PROC_AIO_MAX_NR_INODE ||
                              inode==PROC_LEASE_BREAK_TIME_INODE || inode==PROC_PIPE_MAX_SIZE_INODE ||
                              inode == PROC_HOSTNAME_INODE || inode == PROC_DOMAINNAME_INODE ||
                              proc_is_key_quota(inode) || proc_is_shm_limit(inode))
        ? KERNEL_VFS_CONTROL_READ | KERNEL_VFS_CONTROL_WRITE : 0;
    if (inode==PROC_AIO_NR_INODE) node->generated_control=KERNEL_VFS_CONTROL_READ;
    if (inode==PROC_DROP_CACHES_INODE)
        node->generated_control=KERNEL_VFS_CONTROL_WRITE|KERNEL_VFS_CONTROL_SYSTEM;
    if (inode==PROC_CACHE_PRESSURE_INODE)
        node->generated_control=KERNEL_VFS_CONTROL_READ|KERNEL_VFS_CONTROL_WRITE;
    if (proc_inode_kind(inode) == PROC_PID_TIMERSLACK_KIND) {
        node->generated_control = KERNEL_VFS_CONTROL_READ | KERNEL_VFS_CONTROL_WRITE;
        node->control_write_prefix = PROC_TIMERSLACK_WRITE_MAX | KERNEL_VFS_CONTROL_PREFIX_POSITIONED;
    }
    if (proc_inode_kind(inode) == PROC_PID_FILTER_KIND) {
        node->generated_control = KERNEL_VFS_CONTROL_READ | KERNEL_VFS_CONTROL_WRITE;
        node->control_write_prefix = 34 | KERNEL_VFS_CONTROL_PREFIX_POSITIONED;
    }
    if (proc_inode_kind(inode) == PROC_PID_COMM_KIND || proc_inode_kind(inode) == PROC_TID_COMM_KIND) {
        node->generated_control = KERNEL_VFS_CONTROL_WRITE;
        node->control_write_prefix = 15;
    }
#if BOAROS_COST_DIAGNOSTICS
    if (inode == PROC_COST_CONTROL_INODE) node->generated_control = KERNEL_VFS_CONTROL_WRITE | KERNEL_VFS_CONTROL_READ;
    node->generated_diagnostic = inode == PROC_COST_INODE || inode == PROC_COST_CONTROL_INODE ||
                                 inode == PROC_NETWORK_STATS_INODE || inode == PROC_MEMORY_STATS_INODE;
#endif
    node->inode = inode;
    node->mode = mode;
    if(inode>=(UINT64_C(1)<<30U)) {
        int error=kernel_proc_inode_owner(proc_inode_pid(inode),proc_inode_identity(inode),
            proc_inode_is_thread(inode),mode,&node->uid,&node->gid);
        if(error){kernel_heap_release(instance->heap,node);return error;}
        if(proc_inode_kind(inode)==PROC_PID_DIR_KIND) {
            struct kernel_pid *owner=0;
            error=kernel_proc_process_identity_acquire(proc_inode_pid(inode),
                proc_inode_identity(inode),&owner);
            if(error){kernel_heap_release(instance->heap,node);return error;}
            node->backend_data=owner;
        }
    }
    return kernel_vfs_publish_node(mount, node, file, 0);
}

static int proc_permission(struct kernel_vfs_node *node,const struct kernel_credentials *cred,unsigned mask)
{
    if (node->inode == PROC_HOSTNAME_INODE || node->inode == PROC_DOMAINNAME_INODE ||
        node->inode == PROC_NR_HUGEPAGES_INODE || node->inode == PROC_AIO_MAX_NR_INODE ||
        node->inode == PROC_AIO_NR_INODE || node->inode == PROC_CACHE_PRESSURE_INODE ||
        node->inode == PROC_LEASE_BREAK_TIME_INODE || node->inode == PROC_PIPE_MAX_SIZE_INODE ||
        node->inode == PROC_DROP_CACHES_INODE || proc_is_key_quota(node->inode) ||
        proc_is_shm_limit(node->inode)) {
        /* sysctl table permissions use effective UID and grant no DAC bypass. */
        if (node->inode==PROC_DROP_CACHES_INODE)
            return mask & ~(cred->euid==0 ? 2U : 0U) & 7U ? -KERNEL_EACCES : 0;
        unsigned granted = cred->euid == 0 && node->inode != PROC_AIO_NR_INODE ? 6U : 4U;
        return mask & ~granted & 7U ? -KERNEL_EACCES : 0;
    }
    if(node->inode>=(UINT64_C(1)<<30U) &&
        (proc_inode_kind(node->inode)==PROC_PID_FDINFO_DIR_KIND ||
         proc_inode_kind(node->inode)==PROC_PID_FDINFO_KIND)) {
        int error=kernel_proc_fdinfo_access(proc_inode_pid(node->inode),proc_resource_identity(node->inode));
        if(error)return error;
    }
    if (proc_inode_kind(node->inode) == PROC_TID_COMM_KIND && !(mask & 1U) &&
        kernel_proc_same_group(proc_inode_pid(node->inode), proc_inode_identity(node->inode), 1)) return 0;
    uint32_t uid=0,gid=0;
    if(node->inode>=(UINT64_C(1)<<30U)) {
        int result=kernel_proc_inode_owner(proc_inode_pid(node->inode),proc_inode_identity(node->inode),
            proc_inode_is_thread(node->inode),node->mode,&uid,&gid);
        if(result)return result;
    }
    int result=kernel_vfs_dac_permission(node->mode,uid,gid,mask,cred);
    /* 降权使proc FD目录属主回到root，调用线程仍可查询本组FD。
     * 链接访问还会核对实际对象，不改变stat展示的真实属主。 */
    if(result==-KERNEL_EACCES && proc_inode_kind(node->inode)==PROC_PID_FD_DIR_KIND &&
        kernel_proc_same_group(proc_inode_pid(node->inode),proc_inode_identity(node->inode),proc_inode_is_thread(node->inode)))return 0;
    return result;
}

static int proc_stat(const struct kernel_vfs_file *file,
                     struct kernel_vfs_stat *stat)
{
    uint64_t inode = kernel_vfs_file_inode(file);
    uint32_t mode = file->mode,uid=0,gid=0;
    if (inode >= (UINT64_C(1) << 30U)) {
        uint64_t identity;
        if (proc_inode_identity_current(inode, &identity) ||
            identity != proc_inode_identity(inode)) return -KERNEL_ENOENT;
        if (proc_inode_kind(inode) == PROC_PID_FD_LINK_KIND) {
            int found;
            int fd = proc_inode_fd(inode);
            if (kernel_proc_next_fd(proc_inode_pid(inode), proc_resource_identity(inode),
                                     fd - 1, &found) || found != fd)
                return -KERNEL_ENOENT;
            int readable, writable;
            if (kernel_proc_fd_access_snapshot(proc_inode_pid(inode),
                proc_resource_identity(inode), fd, &readable, &writable)) return -KERNEL_ENOENT;
            mode = KERNEL_VFS_S_IFLNK |
                (readable ? 0500U : 0U) | (writable ? 0300U : 0U);
        }
    }
    if(inode>=(UINT64_C(1)<<30U)) {
        int error=kernel_proc_inode_owner(proc_inode_pid(inode),proc_inode_identity(inode),
            proc_inode_is_thread(inode),mode,&uid,&gid);
        if(error)return error;
    }
    *stat = (struct kernel_vfs_stat){
        .uid=uid,.gid=gid,
        .dev = ((struct kernel_vfs_instance *)file->mount->private_data)->backend_mount.id,
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
    struct procfs_mount *proc = ((struct kernel_vfs_instance *)mount->private_data)->backend_data;
    *stat = (struct kernel_vfs_statfs){
        .type = PROC_SUPER_MAGIC,
        .block_size = BOAROS_PAGE_SIZE,
        .fsid = mount->id,
        .name_length = 255U,
        .flags = proc->instance.read_only ? 1U : 0U,
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
    if (directory == PROC_NET_INODE) {
        const char *entries[] = {".", "..", "route"};
        if (position >= 3U) return 0;
        size_t length = strlen(entries[position]) + 1U;
        if (name_size < length) return -KERNEL_ERANGE;
        memcpy(name, entries[position], length);
        *next_position = position + 1U;
        *inode = position == 0 ? directory : position == 1 ? PROC_ROOT_INODE : PROC_NET_ROUTE_INODE;
        *type = position < 2 ? 4U : 8U;
        return 1;
    }
    if (directory == PROC_SYSVIPC_INODE) {
        static const char *const entries[] = {".", "..", "shm", "sem", "msg"};
        if (position >= sizeof(entries) / sizeof(entries[0])) return 0;
        size_t length = strlen(entries[position]) + 1U;
        if (name_size < length) return -KERNEL_ERANGE;
        memcpy(name, entries[position], length);
        *next_position = position + 1U;
        *inode = position == 0 ? directory : position == 1 ? PROC_ROOT_INODE :
            PROC_SYSVIPC_SHM_INODE + position - 2U;
        *type = position < 2U ? 4U : 8U;
        return 1;
    }
    if (directory==PROC_VM_INODE || directory==PROC_FS_INODE || directory==PROC_SYS_INODE) {
        static const char *const sys[] = {".","..","kernel","vm","fs"};
        static const uint64_t sys_ids[] = {PROC_SYS_INODE,PROC_ROOT_INODE,PROC_KERNEL_INODE,PROC_VM_INODE,PROC_FS_INODE};
        static const char *const fs[] = {".","..","aio-nr","aio-max-nr","lease-break-time","pipe-max-size"};
        static const uint64_t fs_ids[] = {PROC_FS_INODE,PROC_SYS_INODE,PROC_AIO_NR_INODE,PROC_AIO_MAX_NR_INODE,
            PROC_LEASE_BREAK_TIME_INODE,PROC_PIPE_MAX_SIZE_INODE};
        static const char *const vm[] = {".","..","nr_hugepages","drop_caches","vfs_cache_pressure"};
        static const uint64_t vm_ids[] = {PROC_VM_INODE,PROC_SYS_INODE,PROC_NR_HUGEPAGES_INODE,
            PROC_DROP_CACHES_INODE,PROC_CACHE_PRESSURE_INODE};
        const char *const *entries=directory==PROC_SYS_INODE?sys:directory==PROC_FS_INODE?fs:vm;
        const uint64_t *ids=directory==PROC_SYS_INODE?sys_ids:directory==PROC_FS_INODE?fs_ids:vm_ids;
        if (position>=(directory==PROC_FS_INODE?sizeof(fs)/sizeof(fs[0]):5U)) return 0;
        size_t length=strlen(entries[position])+1;
        if (name_size<length) return -KERNEL_ERANGE;
        memcpy(name,entries[position],length); *next_position=position+1;
        *inode=ids[position]; *type=position<2 || directory==PROC_SYS_INODE?4U:8U;
        return 1;
    }
    if (directory == PROC_KERNEL_INODE) {
        static const struct proc_root_entry entries[] = {
            {".", PROC_KERNEL_INODE, 4}, {"..", PROC_SYS_INODE, 4},
            {"sched_rt_period_us", PROC_RT_PERIOD_INODE, 8},
            {"sched_rt_runtime_us", PROC_RT_RUNTIME_INODE, 8},
            {"pid_max", PROC_PID_MAX_INODE, 8}, {"unprivileged_bpf_disabled", PROC_UNPRIVILEGED_BPF_INODE, 8},
            {"hostname", PROC_HOSTNAME_INODE, 8}, {"domainname", PROC_DOMAINNAME_INODE, 8},
            {"sched_rr_timeslice_ms", PROC_RR_TIMESLICE_INODE, 8}, {"keys", PROC_KEYS_INODE, 4},
        };
        const uint64_t fixed = sizeof(entries) / sizeof(entries[0]);
        if (position >= fixed + KERNEL_SHM_LIMIT_COUNT) return 0;
        const char *entry = position < fixed ? entries[position].name : proc_shm_limit_names[position - fixed];
        size_t length = strlen(entry) + 1U;
        if (name_size < length) return -KERNEL_ERANGE;
        memcpy(name, entry, length);
        *next_position = position + 1U;
        *inode = position < fixed ? entries[position].inode : PROC_SHM_LIMIT_FIRST_INODE + position - fixed;
        *type = position < fixed ? entries[position].type : 8U;
        return 1;
    }
    if (directory == PROC_KEYS_INODE) {
        if (position >= 2U + KERNEL_KEY_QUOTA_COUNT) return 0;
        const char *entry = position == 0 ? "." : position == 1 ? ".." :
            proc_key_quota_names[position - 2U];
        size_t length = strlen(entry) + 1U;
        if (name_size < length) return -KERNEL_ERANGE;
        memcpy(name, entry, length);
        *next_position = position + 1U;
        *inode = position == 0 ? directory : position == 1 ? PROC_KERNEL_INODE :
            PROC_KEY_QUOTA_FIRST_INODE + position - 2U;
        *type = position < 2 ? 4U : 8U;
        return 1;
    }
    if (directory != PROC_ROOT_INODE &&
        proc_inode_kind(directory) != PROC_PID_DIR_KIND &&
        proc_inode_kind(directory) != PROC_PID_NS_DIR_KIND &&
        proc_inode_kind(directory) != PROC_PID_FD_DIR_KIND &&
        proc_inode_kind(directory) != PROC_PID_FDINFO_DIR_KIND &&
        proc_inode_kind(directory) != PROC_TASK_DIR_KIND &&
        proc_inode_kind(directory) != PROC_TID_DIR_KIND)
        return -KERNEL_ENOTDIR;
    if (directory != PROC_ROOT_INODE) {
        uint64_t identity;
        if (proc_inode_identity_current(directory, &identity) ||
            identity != proc_inode_identity(directory)) return -KERNEL_ENOENT;
        uint8_t kind = proc_inode_kind(directory);
        if (kind==PROC_PID_NS_DIR_KIND) {
            if (position>6U) return 0;
            uint8_t leaf=position==2 ? PROC_PID_MNT_NS_KIND :
                position==3 ? PROC_PID_UTS_NS_KIND : position==4 ? PROC_PID_IPC_NS_KIND :
                position==5 ? PROC_PID_PID_NS_KIND : PROC_PID_CHILDREN_NS_KIND;
            const char *entry=position==0?".":position==1?"..":proc_namespace_name(leaf);
            if (name_size<=strlen(entry)) return -KERNEL_ERANGE;
            strcpy(name,entry);
            *next_position=position+1;
            *type=position>=2U?10U:4U;
            *inode=position==0?directory:proc_pid_inode(proc_inode_pid(directory),identity,
                position>=2?leaf:
                proc_inode_is_thread(directory)?PROC_TID_DIR_KIND:PROC_PID_DIR_KIND);
            if (position>=2) *inode|=directory&(UINT64_C(1)<<16U);
            return 1;
        }
        if (kind == PROC_TASK_DIR_KIND || kind == PROC_TID_DIR_KIND) {
            if (kind == PROC_TASK_DIR_KIND && position >= 2U) {
                kernel_pid_t tid, number;
                uint64_t token;
                if (position - 2U >= INT32_MAX ||
                    kernel_proc_view_next(((struct procfs_mount *)((struct kernel_vfs_node *)file->private_data)->instance->backend_data)->pid_view,
                        (kernel_pid_t)(position - 2U), proc_inode_pid(directory), identity, 1,
                        &number, &tid, &token)) return 0;
                size_t length = decimal(name, (uint32_t)number);
                if (name_size <= length) return -KERNEL_ERANGE;
                name[length] = 0;
                *next_position = (uint32_t)number + 2U;
                *inode = proc_pid_inode(tid, token, PROC_TID_DIR_KIND);
                *type = 4U;
                return 1;
            }
            if (position > 16U) return 0;
            const char *entry = position == 0 ? "." : position == 1 ? ".." :
                position == 2 ? "stat" : position == 3 ? "status" :
                position == 4 ? "exe" : position == 5 ? "cwd" :
                position == 6 ? "root" : position == 7 ? "maps" :
                position == 8 ? "fd" : position==9?"fdinfo":position==10?"ns":position==11?"mounts":position==12?"mountinfo":position==13?"comm":position==14?"cgroup":position==15?"io":"pagemap";
            if (name_size <= strlen(entry)) return -KERNEL_ERANGE;
            strcpy(name, entry);
            *next_position = position + 1;
            *type = position < 2 || (position >= 8 && position<=10) ? 4U :
                    position >= 4 && position <= 6 ? 10U : 8U;
            if (!position) *inode = directory;
            else if (position == 1 && kind == PROC_TASK_DIR_KIND)
                *inode = proc_pid_inode(proc_inode_pid(directory), identity, PROC_PID_DIR_KIND);
            else if (position == 1) {
                kernel_pid_t tgid;
                uint64_t group_identity;
                int error = kernel_proc_thread_group(proc_inode_pid(directory), identity,
                    &tgid, &group_identity);
                if (error) return error;
                *inode = proc_pid_inode(tgid, group_identity, PROC_TASK_DIR_KIND);
            } else if (position < 4 || position == 13 || position == 14) *inode = proc_pid_inode(proc_inode_pid(directory), identity,
                position == 2 ? PROC_TID_STAT_KIND : position == 3 ? PROC_TID_STATUS_KIND : position == 13 ? PROC_TID_COMM_KIND : PROC_TID_CGROUP_KIND);
            else *inode = proc_thread_resource_inode(proc_inode_pid(directory), identity,
                position == 4 ? PROC_PID_EXE_KIND : position == 5 ? PROC_PID_CWD_KIND :
                position == 6 ? PROC_PID_ROOT_KIND : position == 7 ? PROC_PID_MAPS_KIND :
                position == 8 ? PROC_PID_FD_DIR_KIND : position==9?PROC_PID_FDINFO_DIR_KIND:
                position==10?PROC_PID_NS_DIR_KIND:position==11?PROC_PID_MOUNTS_KIND:
                position==12?PROC_PID_MOUNTINFO_KIND:position==15?PROC_PID_IO_KIND:PROC_PID_PAGEMAP_KIND, 0);
            return 1;
        }
        if (proc_inode_kind(directory) == PROC_PID_FD_DIR_KIND ||
            proc_inode_kind(directory)==PROC_PID_FDINFO_DIR_KIND) {
            if (position >= 2U) {
                if (position > 1025U) return 0;
                int fd;
                if (kernel_proc_next_fd(proc_inode_pid(directory), proc_resource_identity(directory),
                                         (int)position - 3, &fd))
                    return 0;
                size_t length = decimal(name, (unsigned)fd);
                if (name_size <= length) return -KERNEL_ERANGE;
                name[length] = '\0';
                *next_position = (uint64_t)fd + 3U;
                uint8_t leaf = kind == PROC_PID_FD_DIR_KIND ? PROC_PID_FD_LINK_KIND : PROC_PID_FDINFO_KIND;
                *inode = proc_inode_is_thread(directory)
                    ? proc_thread_resource_inode(proc_inode_pid(directory), identity, leaf, (uint16_t)fd)
                    : proc_pid_inode_fd(proc_inode_pid(directory), identity, leaf, (uint16_t)fd);
                *type = proc_inode_kind(directory)==PROC_PID_FD_DIR_KIND?10U:8U;
                return 1;
            }
            const char *entry = position == 0U ? "." : "..";
            if (name_size < strlen(entry) + 1U) return -KERNEL_ERANGE;
            strcpy(name, entry);
            *next_position = position + 1U;
            *inode = position == 0U ? directory :
                proc_pid_inode(proc_inode_pid(directory), identity,
                    proc_inode_is_thread(directory) ? PROC_TID_DIR_KIND : PROC_PID_DIR_KIND);
            *type = 4U;
            return 1;
        }
        if (position > 19U) return 0;
        const char *entry = position == 0U ? "." :
                            position == 1U ? ".." :
                            position == 2U ? "exe" :
                            position == 3U ? "cwd" :
                            position == 4U ? "root" :
                            position == 5U ? "mounts" :
                            position == 6U ? "stat" :
                            position == 7U ? "status" : position == 8U ? "fd" :
                            position == 9U ? "task" : position==10U?"maps":position==11U?"fdinfo":position==12U?"ns":position==13U?"mountinfo":position==14U?"comm":position==15U?"cgroup":position==16U?"coredump_filter":position==17U?"io":position==18U?"pagemap":"timerslack_ns";
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
                     position == 8U ? PROC_PID_FD_DIR_KIND :
                     position == 9U ? PROC_TASK_DIR_KIND : position==10U?PROC_PID_MAPS_KIND:
                     position==11U?PROC_PID_FDINFO_DIR_KIND:position==12U?PROC_PID_NS_DIR_KIND:position==13U?PROC_PID_MOUNTINFO_KIND:position==14U?PROC_PID_COMM_KIND:position==15U?PROC_PID_CGROUP_KIND:position==16U?PROC_PID_FILTER_KIND:position==17U?PROC_PID_IO_KIND:
                     position==18U?PROC_PID_PAGEMAP_KIND:PROC_PID_TIMERSLACK_KIND);
        *type = position < 2U || position == 8U || position == 9U || position==11U || position==12U ? 4U :
                position >= 5U ? 8U : 10U;
        return 1;
    }
    if (position >= PROC_ROOT_STATIC_COUNT) {
        uint64_t cookie=position-PROC_ROOT_STATIC_COUNT;
        kernel_pid_t pid, number;
        uint64_t identity;
        if (cookie >= INT32_MAX ||
            kernel_proc_view_next(((struct procfs_mount *)((struct kernel_vfs_node *)file->private_data)->instance->backend_data)->pid_view,
                (kernel_pid_t)cookie, 0, 0, 0, &number, &pid, &identity)) return 0;
        size_t length = decimal(name, (uint32_t)number);
        if (name_size <= length) return -KERNEL_ERANGE;
        name[length] = '\0';
        *next_position = PROC_ROOT_STATIC_COUNT + (uint32_t)number;
        *inode = proc_pid_inode(pid, identity, PROC_PID_DIR_KIND);
        *type = 4U;
        return 1;
    }
    const struct proc_root_entry *entry=&proc_root_entries[position];
    size_t length = strlen(entry->name) + 1U;
    if (name_size < length) return -KERNEL_ERANGE;
    memcpy(name, entry->name, length);
    *next_position = position + 1U;
    *inode = entry->inode;
    *type = entry->type;
    return 1;
}

static int proc_readlink(struct kernel_vfs_node *node, char *buffer,
                         size_t size, size_t *count)
{
    if (!buffer || !count) return -KERNEL_EINVAL;
    if (node->inode == PROC_SELF_INODE || node->inode == PROC_THREAD_SELF_INODE) {
        kernel_pid_t tgid, tid;
        if (kernel_proc_view_self(((struct procfs_mount *)node->instance->backend_data)->pid_view,
            &tgid, &tid)) return -KERNEL_ENOENT;
        char digits[56];
        size_t length = decimal(digits, (uint32_t)tgid);
        if (node->inode == PROC_THREAD_SELF_INODE) {
            memcpy(digits + length, "/task/", 6U);
            length += 6U;
            length += decimal(digits + length, (uint32_t)tid);
        }
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
    if (proc_namespace_type(kind)) {
        struct kernel_namespace *ns=0;
        int error=kernel_proc_namespace_handle(proc_inode_pid(node->inode),
            proc_resource_identity(node->inode),proc_namespace_type(kind),&ns);
        if (error) return error;
        char text[32];memcpy(text,proc_namespace_name(kind),3);memcpy(text+3,":[",2);
        size_t length=5+decimal(text+5,ns->id);
        kernel_namespace_put(ns);
        text[length++]=']';
        *count=length>size?size:length;
        memcpy(buffer,text,*count);
        return 0;
    }
    enum kernel_proc_path_kind path_kind = kind == PROC_PID_EXE_KIND
        ? KERNEL_PROC_PATH_EXE : kind == PROC_PID_CWD_KIND
        ? KERNEL_PROC_PATH_CWD : kind == PROC_PID_ROOT_KIND
        ? KERNEL_PROC_PATH_ROOT : 0;
    if (!path_kind && kind != PROC_PID_FD_LINK_KIND) return -KERNEL_EINVAL;
    int result;
    if (kind == PROC_PID_FD_LINK_KIND) {
        result = kernel_proc_fd_path_acquire(proc_inode_pid(node->inode),
            proc_resource_identity(node->inode), proc_inode_fd(node->inode), &path);
        if (result) {
            struct kernel_proc_fd_pseudo pseudo = {0};
            result = kernel_proc_fd_pseudo_snapshot(proc_inode_pid(node->inode),
                proc_resource_identity(node->inode), proc_inode_fd(node->inode),
                &pseudo);
            if (result) return result;
            if(pseudo.kind==KERNEL_OPEN_FILE_KIND_MQUEUE || pseudo.kind==KERNEL_OPEN_FILE_KIND_BPF){
                size_t n=strlen(pseudo.name);if(n>size)n=size;
                memcpy(buffer,pseudo.name,n);*count=n;return 0;
            }
            const char *prefix;
            size_t prefix_length;
            uint64_t identity = pseudo.object_identity;
            enum kernel_open_file_kind file_kind = pseudo.kind;
            if (file_kind == KERNEL_OPEN_FILE_KIND_PIPE)
                prefix = "pipe:[";
            else if (file_kind == KERNEL_OPEN_FILE_KIND_NAMESPACE)
                prefix = pseudo.name;
            else if (file_kind == KERNEL_OPEN_FILE_KIND_SOCKET)
                prefix = "socket:[";
            else if (file_kind == KERNEL_OPEN_FILE_KIND_FS_CONTEXT)
                prefix = "anon_inode:[fscontext]";
            else if (file_kind == KERNEL_OPEN_FILE_KIND_EPOLL)
                prefix = "anon_inode:[eventpoll]";
            else if (file_kind == KERNEL_OPEN_FILE_KIND_INOTIFY)
                prefix = "anon_inode:inotify";
            else if (file_kind == KERNEL_OPEN_FILE_KIND_FANOTIFY)
                prefix = "anon_inode:[fanotify]";
            else if (file_kind == KERNEL_OPEN_FILE_KIND_USERFAULTFD)
                prefix = "anon_inode:[userfaultfd]";
            else if (file_kind == KERNEL_OPEN_FILE_KIND_IO_URING)
                prefix = "anon_inode:[io_uring]";
            else if (file_kind == KERNEL_OPEN_FILE_KIND_SECRETMEM)
                prefix = "/secretmem (deleted)";
            else if (file_kind == KERNEL_OPEN_FILE_KIND_EVENTFD)
                prefix = "anon_inode:[eventfd]";
            else if (file_kind == KERNEL_OPEN_FILE_KIND_TIMERFD)
                prefix = "anon_inode:[timerfd]";
            else if (file_kind == KERNEL_OPEN_FILE_KIND_SIGNALFD)
                prefix = "anon_inode:[signalfd]";
            else if (file_kind==KERNEL_OPEN_FILE_KIND_PIDFD)
                prefix="anon_inode:[pidfd]";
            else prefix = 0;
            int numbered = file_kind == KERNEL_OPEN_FILE_KIND_PIPE ||
                file_kind == KERNEL_OPEN_FILE_KIND_NAMESPACE || file_kind == KERNEL_OPEN_FILE_KIND_SOCKET;
            if (!prefix || (numbered && !identity)) result = -KERNEL_ENOENT;
            else {
                char display[64];
                prefix_length = strlen(prefix);
                memcpy(display, prefix, prefix_length);
                size_t length = prefix_length;
                if (numbered) {
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
        proc_resource_identity(node->inode), path_kind, 0, &path);
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
    {
    KERNEL_FS_PATHS(paths, fs);
    int mq_deleted=0;
    if(kernel_mqueue_fs_is_mount(kernel_vfs_path_mount(path))){
        struct kernel_vfs_stat stat;
        if(!kernel_vfs_path_stat(path,&stat))mq_deleted=!stat.nlink;
    }
    result = kernel_vfs_path_link_string(path, paths.root,
                                         text, KERNEL_FS_PATH_MAX);
    if (!result) {
        size_t length = strlen(text);
        /* A shared mqueue name may be removed through a different mount or
         * mq_unlink, independently of this mount-local cached path edge. */
        if(mq_deleted&&(length<10||strcmp(text+length-10," (deleted)"))){
            if(length+11>KERNEL_FS_PATH_MAX)result=-KERNEL_ENAMETOOLONG;
            else{memcpy(text+length," (deleted)",11);length+=10;}
        }
        if(result){kernel_heap_release(node->instance->heap,text);goto Finish;}
        if (length > size) length = size;
        memcpy(buffer, text, length);
        *count = length;
    }
    kernel_heap_release(node->instance->heap, text);
    }
Finish:
    if (kernel_vfs_path_release(&path)) __builtin_trap();
    return result;
}

static int proc_follow_link(struct kernel_vfs_node *node, unsigned resolve,
                            struct kernel_vfs_path **owner)
{
    if (node->inode == PROC_SELF_INODE || node->inode == PROC_THREAD_SELF_INODE ||
        node->inode == PROC_MOUNTS_INODE) return -KERNEL_ENOTSUP;
    uint8_t kind = proc_inode_kind(node->inode);
    if (proc_namespace_type(kind)) {
        if (resolve & KERNEL_VFS_RESOLVE_NO_MAGICLINKS) return -KERNEL_ELOOP;
        if (resolve & (KERNEL_VFS_RESOLVE_SCOPED|KERNEL_VFS_RESOLVE_NO_XDEV)) return -KERNEL_EXDEV;
        return -KERNEL_ENOENT; /* Namespace object has no path inside the proc mount. */
    }
    enum kernel_proc_path_kind path_kind = kind == PROC_PID_EXE_KIND
        ? KERNEL_PROC_PATH_EXE : kind == PROC_PID_CWD_KIND
        ? KERNEL_PROC_PATH_CWD : kind == PROC_PID_ROOT_KIND
        ? KERNEL_PROC_PATH_ROOT : 0;
    int result;
    int pathless=0;
    if (kind == PROC_PID_FD_LINK_KIND) {
        result=kernel_proc_fd_path_acquire(proc_inode_pid(node->inode),
            proc_resource_identity(node->inode),proc_inode_fd(node->inode),owner);
        if (result) {
            struct kernel_proc_fd_pseudo pseudo={0};
            result=kernel_proc_fd_pseudo_snapshot(proc_inode_pid(node->inode),
                proc_resource_identity(node->inode),proc_inode_fd(node->inode),&pseudo);
            if (result) return result;
            /* Pathless pseudo-objects live outside this proc mount. The old
             * unscoped caller may reopen a pipe or mqueue through its OFD. */
            pathless=pseudo.kind==KERNEL_OPEN_FILE_KIND_PIPE ||
                pseudo.kind==KERNEL_OPEN_FILE_KIND_MQUEUE ? -KERNEL_ENOENT : -KERNEL_ENXIO;
        }
    } else if (path_kind) {
        result=kernel_proc_process_path_acquire(proc_inode_pid(node->inode),
            proc_resource_identity(node->inode),path_kind,0,owner);
        if (result) return result;
    } else return -KERNEL_ENOTSUP;
    if (resolve & KERNEL_VFS_RESOLVE_NO_MAGICLINKS) result=-KERNEL_ELOOP;
    else if ((resolve & KERNEL_VFS_RESOLVE_SCOPED) ||
             (pathless && (resolve & KERNEL_VFS_RESOLVE_NO_XDEV))) result=-KERNEL_EXDEV;
    else return pathless;
    if (*owner && kernel_vfs_path_release(owner)) __builtin_trap();
    return result;
}

static int proc_reopen_link(struct kernel_vfs_node *node,
                            struct kernel_heap *heap, uint32_t flags,
                            struct kernel_open_file_description **owner)
{
    uint32_t type=proc_namespace_type(proc_inode_kind(node->inode));
    if (type) {
        struct kernel_namespace *ns=0;
        int error=kernel_proc_namespace_handle(proc_inode_pid(node->inode),
            proc_resource_identity(node->inode),type,&ns);
        if(error)return error;
        error=kernel_namespace_open(ns,heap,flags,owner);
        kernel_namespace_put(ns);
        return error;
    }
    if (proc_inode_kind(node->inode) != PROC_PID_FD_LINK_KIND)
        return -KERNEL_ENOTSUP;
    return kernel_proc_fd_reopen_link(proc_inode_pid(node->inode),
        proc_resource_identity(node->inode), proc_inode_fd(node->inode), heap,
        flags, owner);
}

static int proc_stat_link(struct kernel_vfs_node *node,
                          struct kernel_vfs_stat *stat)
{
    uint32_t type=proc_namespace_type(proc_inode_kind(node->inode));
    if (type) {
        struct kernel_namespace *ns=0;
        int error=kernel_proc_namespace_handle(proc_inode_pid(node->inode),
            proc_resource_identity(node->inode),type,&ns);
        if (error) return error;
        *stat=(struct kernel_vfs_stat){.mode=KERNEL_VFS_S_IFREG|0444U,.nlink=1,
            .ino=ns->id,.blksize=BOAROS_PAGE_SIZE};
        kernel_namespace_put(ns);
        return 0;
    }
    if (proc_inode_kind(node->inode) != PROC_PID_FD_LINK_KIND)
        return -KERNEL_ENOTSUP;
    return kernel_proc_fd_pseudo_stat(proc_inode_pid(node->inode),
        proc_resource_identity(node->inode), proc_inode_fd(node->inode), stat);
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

static int proc_huge_snapshot(struct physical_page_allocator *allocator,
    struct kernel_hstate_snapshot *snapshot)
{
    *snapshot=(struct kernel_hstate_snapshot){.order=ARCH_MMU_HUGE_ORDER};
    struct kernel_hstate *state=NULL;
    enum kernel_hugetlb_status status=kernel_hstate_get(allocator,&state);
    if (status==KERNEL_HUGETLB_NOT_FOUND) return 0;
    if (status) return -KERNEL_EIO;
    kernel_hstate_snapshot(state,snapshot);
    kernel_hstate_put(state);
    return 0;
}

/* Linux kstrtoull_from_user accepts an optional plus and terminal newline. */
static int proc_parse_unsigned(const char *buffer, size_t size, unsigned base,
                               uint64_t *value)
{
    char text[PROC_TIMERSLACK_WRITE_MAX + 1U];
    if (size > sizeof(text) - 1U) size = sizeof(text) - 1U;
    memcpy(text, buffer, size); text[size] = 0;
    const char *p = text;
    if (*p == '+') p++;
    if (!base) {
        base = 10;
        if (*p == '0') {
            base = 8;
            if ((p[1] == 'x' || p[1] == 'X') && proc_digit(p[2]) < 16) {
                base = 16;
                p += 2;
            }
        }
    }
    uint64_t result = 0; unsigned digits = 0; int overflow = 0;
    while (proc_digit(*p) < base) {
        unsigned digit = proc_digit(*p++); digits++;
        if (result > (UINT64_MAX - digit) / base) overflow = 1;
        result = result * base + digit;
    }
    if (overflow) return -KERNEL_ERANGE;
    if (!digits) return -KERNEL_EINVAL;
    if (*p == '\n') p++;
    if (*p) return -KERNEL_EINVAL;
    *value = result;
    return 0;
}

static int proc_control(struct kernel_vfs_node *node, void *open_context, int write,
    uint64_t *offset, char *buffer, size_t size, size_t *count)
{
    if (proc_inode_kind(node->inode) == PROC_PID_TIMERSLACK_KIND) {
        uint64_t value = 0;
        *count = 0;
        int error = write ? proc_parse_unsigned(buffer, size, 10, &value) : 0;
        if (!error) error = kernel_proc_timerslack(proc_inode_pid(node->inode),
            proc_inode_identity(node->inode), &value, write);
        if (error) return error;
        if (write) {
            *count = size;
            return 0;
        }
        char text[21];
        size_t length = decimal(text, value);
        text[length++] = '\n';
        if (*offset >= length) return 0;
        length -= (size_t)*offset;
        if (length > size) length = size;
        memcpy(buffer, text + *offset, length);
        *offset += length;
        *count = length;
        return 0;
    }
    if (proc_inode_kind(node->inode) == PROC_PID_FILTER_KIND) {
        uint64_t parsed = 0;
        *count = 0;
        int error = write ? proc_parse_unsigned(buffer, size > 34 ? 34 : size, 0, &parsed) : 0;
        if (error) return error;
        if (parsed > UINT32_MAX) return -KERNEL_ERANGE;
        unsigned value = (unsigned)parsed;
        int result = kernel_proc_coredump_filter(proc_inode_pid(node->inode),
            proc_inode_identity(node->inode), proc_inode_is_thread(node->inode), &value, write);
        if (result < 0) return result;
        if (write) { *count = size; return 0; }
        if (!result || *offset >= 9) return 0;
        char text[9];
        for (unsigned i = 0; i < 8; i++) text[i] = "0123456789abcdef"[(value >> ((7-i)*4)) & 15];
        text[8] = '\n';
        size_t length = 9 - (size_t)*offset;
        if (length > size) length = size;
        memcpy(buffer, text + *offset, length); *offset += length; *count = length;
        return 0;
    }
    if (proc_inode_kind(node->inode) == PROC_PID_COMM_KIND || proc_inode_kind(node->inode) == PROC_TID_COMM_KIND) {
        if (!write || size > 15U) return -KERNEL_EINVAL;
        char name[16] = {0};
        if (size) memcpy(name, buffer, size);
        int error = kernel_proc_comm(proc_inode_pid(node->inode), proc_inode_identity(node->inode),
            proc_inode_is_thread(node->inode), name, 1);
        if (!error) *count = size;
        return error;
    }

    *count = 0;
#if BOAROS_COST_DIAGNOSTICS
    if (node->inode == PROC_COST_CONTROL_INODE) {
        if (!write) return -KERNEL_EACCES;
        int result = kernel_cost_control(buffer, size);
        if (!result) { *count = size; *offset += size; }
        return result;
    }
#endif
    if (!size) return 0;
    if (node->inode == PROC_HOSTNAME_INODE || node->inode == PROC_DOMAINNAME_INODE) {
        enum kernel_uts_name which = node->inode == PROC_HOSTNAME_INODE ?
            KERNEL_UTS_HOSTNAME : KERNEL_UTS_DOMAINNAME;
        if (write) {
            size_t length = 0;
            while (length < size && length < KERNEL_UTS_NAME_MAX &&
                   buffer[length] && buffer[length] != '\n') length++;
            if (kernel_uts_name_write(kernel_task_uts_namespace(),which, *offset, buffer, length)) *offset += size;
            *count = size;
        } else {
            struct kernel_utsname name;
            kernel_uts_snapshot(kernel_task_uts_namespace(),&name);
            const char *field = which == KERNEL_UTS_HOSTNAME ? name.nodename : name.domainname;
            size_t length = strlen(field);
            if (*offset > length) return 0;
            length -= (size_t)*offset;
            if (length > size) length = size;
            if (length) memcpy(buffer, field + *offset, length);
            if (length < size) buffer[length++] = '\n';
            *count = length;
            *offset += length;
        }
        return 0;
    }
    int shm_limit = proc_is_shm_limit(node->inode);
    if (!write) {
        if (*offset) return 0;
        if (shm_limit) {
            uint64_t value;
            int result = kernel_ipc_shm_limit_read(open_context,
                node->inode - PROC_SHM_LIMIT_FIRST_INODE, &value);
            if (result) return result;
            char text[21];
            size_t length = decimal(text, value);
            text[length++] = '\n';
            if (size < length) length = size;
            memcpy(buffer, text, length);
            *count = length;
            *offset += length;
            return 0;
        }
        int64_t value;
        if (node->inode==PROC_AIO_NR_INODE || node->inode==PROC_AIO_MAX_NR_INODE) {
            char text[24];
            size_t length=decimal(text,kernel_aio_quota(node->inode==PROC_AIO_MAX_NR_INODE));
            text[length++]='\n';
            if (size<length) length=size;
            memcpy(buffer,text,length);
            *count=length;
            *offset+=length;
            return 0;
        }
        if (node->inode==PROC_NR_HUGEPAGES_INODE) {
            struct kernel_hstate_snapshot snapshot;
            int error=proc_huge_snapshot(node->instance->heap->page_allocator,&snapshot);
            if (error) return error;
            value=(int64_t)snapshot.persistent;
        } else if (node->inode == PROC_LEASE_BREAK_TIME_INODE)
            value = kernel_vfs_lease_break_time();
        else if (node->inode == PROC_PIPE_MAX_SIZE_INODE)
            value = kernel_pipe_max_size();
        else if (node->inode == PROC_CACHE_PRESSURE_INODE)
            value = kernel_vfs_cache_pressure();
        else if (proc_is_key_quota(node->inode))
            value = kernel_keys_quota_get(node->inode - PROC_KEY_QUOTA_FIRST_INODE);
        else if (node->inode == PROC_RR_TIMESLICE_INODE)
            value = (int64_t)(kernel_sched_rr_slice_ns() / UINT64_C(1000000));
        else if (node->inode == PROC_UNPRIVILEGED_BPF_INODE)
            value = kernel_bpf_unprivileged_get();
        else {
            int64_t period, runtime;
            kernel_scheduler_rt_bandwidth_get(&period, &runtime);
            value = node->inode == PROC_RT_RUNTIME_INODE ? runtime : period;
        }
        char text[24];
        size_t length = 0;
        if (value < 0) text[length++] = '-';
        length += decimal(text + length, value < 0 ? (uint64_t)-value : (uint64_t)value);
        text[length++] = '\n';
        if (length > size) length = size;
        memcpy(buffer, text, length);
        *count = length;
        *offset += length;
        return 0;
    }
    /* Linux first-position-only sysctl still copies the whole write first. */
    if (*offset) { *count = size; *offset += size; return 0; }
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
    if (node->inode!=PROC_NR_HUGEPAGES_INODE && node->inode!=PROC_AIO_MAX_NR_INODE &&
        node->inode!=PROC_PIPE_MAX_SIZE_INODE && !shm_limit &&
        value > (negative ? UINT64_C(2147483648) : INT32_MAX)) return -KERNEL_EINVAL;
    int result;
    if (node->inode==PROC_DROP_CACHES_INODE) {
        if (negative || !value || value>4) return -KERNEL_EINVAL;
        kernel_vfs_drop_caches(node->instance->heap->page_allocator,(unsigned)value);
        result=0;
    } else if (node->inode==PROC_CACHE_PRESSURE_INODE) {
        if (negative && value) return -KERNEL_EINVAL;
        kernel_vfs_set_cache_pressure((unsigned)value);
        result=0;
    } else if (node->inode==PROC_LEASE_BREAK_TIME_INODE) {
        kernel_vfs_set_lease_break_time((int)(negative ? -(int64_t)value : (int64_t)value));
        result=0;
    } else if (node->inode==PROC_PIPE_MAX_SIZE_INODE) {
        result=negative ? -KERNEL_EINVAL : kernel_pipe_set_max_size(value);
    } else if (shm_limit) {
        result = negative ? -KERNEL_EINVAL : kernel_ipc_shm_limit_set(open_context,
            node->inode - PROC_SHM_LIMIT_FIRST_INODE, value);
    } else if (proc_is_key_quota(node->inode)) {
        result = negative ? -KERNEL_EINVAL :
            kernel_keys_quota_set(node->inode - PROC_KEY_QUOTA_FIRST_INODE, (unsigned)value);
    } else if (node->inode==PROC_AIO_MAX_NR_INODE) {
        if (negative) return -KERNEL_EINVAL;
        kernel_aio_set_max(value);
        result=0;
    } else if (node->inode==PROC_NR_HUGEPAGES_INODE) {
        if (negative) return -KERNEL_EINVAL;
        struct kernel_hstate *state=NULL;
        struct kernel_heap *heap=node->instance->heap;
        enum kernel_hugetlb_status status=kernel_hstate_get_or_create(heap,heap->page_allocator,
            ARCH_MMU_HUGE_ORDER,&state);
        if (status) return status==KERNEL_HUGETLB_NO_MEMORY?-KERNEL_ENOMEM:-KERNEL_EIO;
        status=kernel_hstate_resize(state,value);
        kernel_hstate_put(state);
        /* Linux reports the attainable pool via the next read, not a false
         * allocation guarantee when physical fragmentation limits growth. */
        result=status==KERNEL_HUGETLB_OK || status==KERNEL_HUGETLB_NO_MEMORY?0:-KERNEL_EINVAL;
    } else if(node->inode==PROC_UNPRIVILEGED_BPF_INODE)
        result=negative || value>2?-KERNEL_EINVAL:kernel_bpf_unprivileged_set((unsigned)value);
    else if (node->inode == PROC_RR_TIMESLICE_INODE)
        result = kernel_sched_rr_slice_set_ms(negative ? -(int64_t)value : (int64_t)value);
    else result = kernel_scheduler_rt_bandwidth_set(
        node->inode == PROC_RT_RUNTIME_INODE, negative ? -(int64_t)value : (int64_t)value);
    if (result) return result;
    while (position < limit && proc_space((unsigned char)buffer[position])) position++;
    *count = size - (limit - position);
    *offset += *count;
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
    char *names; /* Mountpoint followed by filesystem root, captured together. */
    struct kernel_vfs_path *path;
    struct kernel_vfs_path *mount_root;
    const char *source;
    const char *bpf_type;
    uint64_t id,parent,device,shared,master,dominating;
    uint8_t unbindable,super_read_only;
    uint8_t is_proc, is_tmpfs, is_devpts;
    uint8_t read_only,nosuid,nodev,noexec,nosymfollow,atime_policy;
};

static void release_mount_entries(struct kernel_heap *heap,
                                   struct proc_mount_entry **owner,
                                   size_t count)
{
    if (!*owner) return;
    for (size_t i = 0U; i < count; i++) {
        if((*owner)[i].names)kernel_heap_release(heap,(*owner)[i].names);
        if ((*owner)[i].path && kernel_vfs_path_release(&(*owner)[i].path))
            __builtin_trap();
        if ((*owner)[i].mount_root &&
            kernel_vfs_path_release(&(*owner)[i].mount_root))
            __builtin_trap();
    }
    kernel_heap_release(heap, *owner);
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
    KERNEL_LOCK_SCOPE(topology);kernel_vfs_mount_topology_lock(&topology);
    struct kernel_fs_mounts mounts __attribute__((cleanup(kernel_fs_mounts_release)))={0};
    int result=kernel_proc_mounts_snapshot(proc_inode_pid(node->inode),
        proc_resource_identity(node->inode),&mounts);
    if (result) return result;
    struct kernel_vfs_path *root_path=mounts.root;
    struct kernel_vfs_mount *top=kernel_vfs_path_mount(mounts.top);
    char *data=NULL,*path_text=NULL;
    struct proc_mount_entry *entries = 0;
    size_t captured = 0U;
    int information=proc_inode_kind(node->inode)==PROC_PID_MOUNTINFO_KIND;
    {
        size_t count=0;
        for(struct kernel_vfs_mount *it=top;it;it=kernel_vfs_mount_next(it,top))count++;
        if(!count || count>SIZE_MAX/sizeof(*entries) || count>SIZE_MAX/(12U*KERNEL_FS_PATH_MAX+256U))return -KERNEL_EOVERFLOW;
        enum kernel_heap_status hs=kernel_heap_allocate_zeroed(heap,count,sizeof(*entries),(void **)&entries);
        if(hs)return hs==KERNEL_HEAP_STATUS_EMPTY?-KERNEL_ENOMEM:-KERNEL_EIO;
        hs=kernel_heap_allocate(heap,2U*KERNEL_FS_PATH_MAX,(void **)&path_text);
        if(hs){result=hs==KERNEL_HEAP_STATUS_EMPTY?-KERNEL_ENOMEM:-KERNEL_EIO;goto Finish;}
        for(struct kernel_vfs_mount *it=top;it;it=kernel_vfs_mount_next(it,top)) {
            struct kernel_vfs_path *path=it==top?mounts.top:it->root_path;
            struct kernel_vfs_path *mount_root=it==top?mounts.top:it->root_path;
            if(!path || !mount_root || kernel_vfs_path_acquire(path) || kernel_vfs_path_acquire(mount_root))__builtin_trap();
            struct proc_mount_entry *e=&entries[captured++];
            e->path=path;e->mount_root=mount_root;e->source=it->source_name;
            struct kernel_vfs_instance *instance=it->private_data;
            e->bpf_type=kernel_vfs_mount_type(it);
            e->is_proc=kernel_procfs_is_mount(it);e->is_tmpfs=kernel_tmpfs_is_mount(it);e->is_devpts=kernel_devpts_is_mount(it);
            e->nosuid=it->nosuid;e->nodev=it->nodev;e->noexec=it->noexec;e->atime_policy=it->atime_policy;
            e->nosymfollow=it->nosymfollow;
            e->read_only=kernel_vfs_mount_is_readonly(it);e->super_read_only=instance->read_only;
            e->id=it->id;e->parent=it->parent?it->parent->id:it->id;e->device=instance->device_id;
            e->shared=it->propagation_group;e->master=it->master?it->master->propagation_group:0;
            e->unbindable=it->unbindable;
            e->dominating=kernel_vfs_mount_dominating_group(it,root_path);
            result=kernel_vfs_path_string(path,root_path,path_text,KERNEL_FS_PATH_MAX);
            if(result==-KERNEL_ENOENT){result=0;continue;}
            if(result)goto Finish;
            path_text[KERNEL_FS_PATH_MAX]=0;
            if(information) {
                result=kernel_vfs_path_filesystem_string(mount_root,path_text+KERNEL_FS_PATH_MAX,KERNEL_FS_PATH_MAX);
                if(result)goto Finish;
            }
            size_t first=strlen(path_text)+1,second=strlen(path_text+KERNEL_FS_PATH_MAX)+1;
            hs=kernel_heap_allocate(heap,first+second,(void **)&e->names);
            if(hs){result=hs==KERNEL_HEAP_STATUS_EMPTY?-KERNEL_ENOMEM:-KERNEL_EIO;goto Finish;}
            memcpy(e->names,path_text,first);memcpy(e->names+first,path_text+KERNEL_FS_PATH_MAX,second);
        }
    }
    kernel_lock_scope_release(&topology);
    kernel_heap_release(heap,path_text);path_text=NULL;
    size_t capacity = captured * (12U * KERNEL_FS_PATH_MAX + 256U);
    enum kernel_heap_status allocation = kernel_heap_allocate(heap, capacity,
                                                              (void **)&data);
    if (allocation != KERNEL_HEAP_STATUS_OK) {
        result = allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM
                                                        : -KERNEL_EIO;
        goto Finish;
    }
    size_t used = 0U;
    for (size_t i = 0U; i < captured; i++) {
        if(!entries[i].names)continue;
        const char *mountpoint=entries[i].names;
        const char *filesystem_root=mountpoint+strlen(mountpoint)+1;
        const char *source = entries[i].source ? entries[i].source :
                             entries[i].bpf_type ? entries[i].bpf_type :
                             entries[i].is_proc ? "proc" :
                             entries[i].is_tmpfs ? "tmpfs" : entries[i].is_devpts ? "devpts" : "rootfs";
        const char *kind = entries[i].bpf_type ? entries[i].bpf_type :
                           entries[i].is_proc ? "proc" :
                           entries[i].is_tmpfs ? "tmpfs" : entries[i].is_devpts ? "devpts" : "ext4";
        if(information) {
            /* Capacity reserves escaped source, mountpoint, filesystem root
             * and bounded numeric fields for each captured entry. */
            used+=decimal(data+used,entries[i].id);data[used++]=' ';
            used+=decimal(data+used,entries[i].parent);data[used++]=' ';
            used+=decimal(data+used,((entries[i].device>>8U)&0xfffU)|((entries[i].device>>32U)&UINT64_C(0xfffff000)));data[used++]=':';
            used+=decimal(data+used,(entries[i].device&255U)|((entries[i].device>>12U)&UINT64_C(0xffffff00)));data[used++]=' ';
            used+=append_escaped(data+used,filesystem_root);data[used++]=' ';
            used+=append_escaped(data+used,mountpoint);
        }
        const char *options[]={entries[i].read_only?" ro":" rw",
            entries[i].nosuid?",nosuid":"",entries[i].nodev?",nodev":"",
            entries[i].noexec?",noexec":"",entries[i].nosymfollow?",nosymfollow":"",
            entries[i].atime_policy&VFS_ATIME_NONE?",noatime":
                entries[i].atime_policy&VFS_ATIME_STRICT?"":",relatime",
            entries[i].atime_policy&VFS_ATIME_NODIR?",nodiratime":""," 0 0\n"};
        if(information) {
            for(size_t j=0;j+1<sizeof(options)/sizeof(options[0]);j++) {
                size_t n=strlen(options[j]);memcpy(data+used,options[j],n);used+=n;
            }
            const char *labels[]={" shared:"," master:"," propagate_from:"};
            uint64_t values[]={entries[i].shared,entries[i].master,
                entries[i].dominating!=entries[i].master?entries[i].dominating:0};
            for(unsigned j=0;j<3;j++)if(values[j]) {
                size_t n=strlen(labels[j]);memcpy(data+used,labels[j],n);used+=n;used+=decimal(data+used,values[j]);
            }
            if(entries[i].unbindable){memcpy(data+used," unbindable",11);used+=11;}
            memcpy(data+used," - ",3);used+=3;
            size_t n=strlen(kind);memcpy(data+used,kind,n);used+=n;data[used++]=' ';
            used+=append_escaped(data+used,source);
            const char *super=entries[i].super_read_only?" ro\n":" rw\n";
            n=strlen(super);memcpy(data+used,super,n);used+=n;
            continue;
        }
        size_t suffix_length=0;
        for (size_t j=0;j<sizeof(options)/sizeof(options[0]);j++) suffix_length+=strlen(options[j]);
        size_t needed = 4U * strlen(source) + 1U + 4U * strlen(mountpoint) +
                        1U + strlen(kind) + suffix_length + 1U;
        if (needed > capacity - used) { result = -KERNEL_EOVERFLOW; goto Finish; }
        size_t part;
        used += append_escaped(data + used, source);
        data[used++] = ' ';
        used += append_escaped(data + used, mountpoint);
        data[used++] = ' ';
        part = strlen(kind);
        memcpy(data + used, kind, part);
        used += part;
        for (size_t j=0;j<sizeof(options)/sizeof(options[0]);j++) {
            part=strlen(options[j]);memcpy(data+used,options[j],part);used+=part;
        }
    }
    data[used] = '\0';
    *buffer = data;
    *length = used;
    data = 0;
    result = 0;
Finish:
    kernel_lock_scope_release(&topology);
    if (path_text) kernel_heap_release(heap, path_text);
    if (data) kernel_heap_release(heap, data);
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

static void proc_credential_release(const struct kernel_credentials **cred)
{ kernel_credentials_put(*cred); }

static int proc_process_snapshot(struct kernel_vfs_node *node,
                                 struct kernel_heap *heap,
                                 char **buffer, size_t *length)
{
    struct kernel_proc_process_snapshot process;
    int thread = proc_inode_is_thread(node->inode);
    int result = kernel_proc_snapshot_in(((struct procfs_mount *)node->instance->backend_data)->pid_view,
        proc_inode_pid(node->inode), proc_inode_identity(node->inode), thread, &process);
    if (result) return result;
    const struct kernel_credentials *cred __attribute__((cleanup(proc_credential_release)))=process.credentials;
    size_t capacity=1536U+process.namespace_count*KERNEL_PID_ROLES*12U+(cred->groups?(size_t)cred->groups->count*11U:0U);
    char *data = 0;
    enum kernel_heap_status allocation = kernel_heap_allocate(heap, capacity,
                                                              (void **)&data);
    if (allocation != KERNEL_HEAP_STATUS_OK)
        return allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM
                                                      : -KERNEL_EIO;
    size_t used = 0U;
    if (proc_inode_kind(node->inode) == PROC_PID_STAT_KIND ||
        proc_inode_kind(node->inode) == PROC_TID_STAT_KIND) {
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
            (uint64_t)(int64_t)process.scheduling_nice, process.threads, 0U, process.start_ticks,
            process.virtual_bytes, process.resident_pages,
            process.rss_limit, process.start_code, process.end_code,
            process.start_stack, 0U, 0U,
            process.signal_pending & UINT64_C(0x7fffffff), process.signal_blocked & UINT64_C(0x7fffffff),
            process.signal_ignored & UINT64_C(0x7fffffff), process.signal_caught & UINT64_C(0x7fffffff),
            process.wait_channel_flag, 0U, 0U, 17U, 0U, process.rt_priority,
            process.scheduling_policy,
        };
        for (size_t i = 0U; i < sizeof(fields) / sizeof(fields[0]); i++) {
            data[used++] = ' ';
            if (i == 29U && thread) {
                data[used++] = '-'; data[used++] = '1';
            } else if (i == 9U && process.scheduling_priority < 0) {
                data[used++] = '-';
                used += decimal(data + used,
                                (uint32_t)-process.scheduling_priority);
            } else if(i==10U && process.scheduling_nice<0) {
                data[used++]='-';used+=decimal(data+used,(uint32_t)-process.scheduling_nice);
            } else used += decimal(data + used, fields[i]);
        }
        data[used++] = '\n';
    } else {
        static const char name_prefix[] = "Name:\t";
        memcpy(data, name_prefix, sizeof(name_prefix) - 1U);
        used = sizeof(name_prefix) - 1U;
        for (const char *it = process.comm; *it; it++) {
            if (*it == '\n' || *it == '\\') {
                data[used++] = '\\';
                data[used++] = *it == '\n' ? 'n' : '\\';
            } else data[used++] = *it;
        }
        data[used++] = '\n';
        static const char state_prefix[] = "State:\t";
        memcpy(data + used, state_prefix, sizeof(state_prefix) - 1U);
        used += sizeof(state_prefix) - 1U;
        data[used++] = process.state;
        const char *state_name = process.state == 'R' ? " (running)\n" :
                                 process.state == 'Z' ? " (zombie)\n" :
                                 process.state == 'T' ? " (stopped)\n" :
                                 process.state == 't' ? " (tracing stop)\n" :
                                                        " (sleeping)\n";
        size_t state_length = strlen(state_name);
        memcpy(data + used, state_name, state_length);
        used += state_length;
        used += append_number_line(data + used, "Tgid:\t",
                                   (uint32_t)process.tgid, "\n");
        used += append_number_line(data + used, "Pid:\t",
                                   (uint32_t)process.pid, "\n");
        used += append_number_line(data + used, "PPid:\t",
                                   (uint32_t)process.ppid, "\n");
        used += append_number_line(data + used, "TracerPid:\t",
                                   (uint32_t)process.tracer_pid, "\n");
        const char *namespace_labels[KERNEL_PID_ROLES] = {"NSpid:\t", "NStgid:\t", "NSpgid:\t", "NSsid:\t"};
        for (unsigned role = 0; role < KERNEL_PID_ROLES; role++) {
            size_t part = strlen(namespace_labels[role]);
            memcpy(data + used, namespace_labels[role], part); used += part;
            for (unsigned n = 0; n < process.namespace_count; n++) {
                if (n) data[used++] = '\t';
                used += decimal(data + used, process.namespace_ids[n][role]);
            }
            data[used++] = '\n';
        }
        used += append_number_line(data + used, "Threads:\t",
                                   process.threads, "\n");
        used += append_number_line(data + used, "VmSize:\t",
                                   process.virtual_bytes / 1024U, " kB\n");
        used += append_number_line(data + used, "VmLck:\t", process.locked_bytes / 1024U, " kB\n");
        used += append_number_line(data + used, "VmRSS:\t",
                                   process.resident_pages *
                                      (BOAROS_PAGE_SIZE / 1024U), " kB\n");
        /* 当前没有 swap 后端，驻留页不会换出到交换设备。 */
        used += append_number_line(data + used, "VmSwap:\t", 0U, " kB\n");
        used += append_number_line(data+used,"SigQ:\t",process.signal_queued,"/");
        used += append_number_line(data+used,"",process.signal_queue_limit,"\n");
        const char *mask_names[] = {"SigPnd:\t", "ShdPnd:\t", "SigBlk:\t", "SigIgn:\t", "SigCgt:\t"};
        const uint64_t masks[] = {process.signal_pending, process.signal_shared_pending,
            process.signal_blocked, process.signal_ignored, process.signal_caught};
        for (unsigned i = 0; i < 5U; i++) {
            size_t label = strlen(mask_names[i]);
            memcpy(data + used, mask_names[i], label); used += label;
            for (unsigned digit = 16U; digit; digit--)
                data[used++] = "0123456789abcdef"[(masks[i] >> ((digit - 1U) * 4U)) & 15U];
            data[used++] = '\n';
        }
        const uint32_t ids[8]={cred->uid,cred->euid,cred->suid,cred->fsuid,
            cred->gid,cred->egid,cred->sgid,cred->fsgid};
        for(unsigned i=0;i<8;i++)used+=append_number_line(data+used,
            i==0?"Uid:\t":i==4?"Gid:\t":"",ids[i],i%4==3?"\n":"\t");
        memcpy(data+used,"Groups:\t",8);used+=8;
        if(cred->groups)for(unsigned i=0;i<cred->groups->count;i++)
            used+=append_number_line(data+used,"",cred->groups->ids[i]," ");
        data[used++]='\n';
        const char *cap_names[]={"CapInh:\t","CapPrm:\t","CapEff:\t","CapBnd:\t","CapAmb:\t"};
        const uint64_t caps[]={cred->inheritable,cred->permitted,cred->effective,cred->bounding,cred->ambient};
        for(unsigned i=0;i<5;i++) {
            size_t label=strlen(cap_names[i]);memcpy(data+used,cap_names[i],label);used+=label;
            for(unsigned d=16;d;d--)data[used++]="0123456789abcdef"[(caps[i]>>((d-1)*4))&15];
            data[used++]='\n';
        }
        used+=append_number_line(data+used,"NoNewPrivs:\t",process.no_new_privs,"\n");
    }
    if (used >= capacity) __builtin_trap();
    data[used] = '\0';
    *buffer = data;
    *length = used;
    return 0;
}

static size_t proc_hex(char *text, uint64_t value, unsigned width)
{
    unsigned digits = 1;
    for (uint64_t rest = value >> 4; rest; rest >>= 4) digits++;
    if (digits < width) digits = width;
    for (unsigned i = 0; i < digits; i++)
        text[i] = "0123456789abcdef"[(value >> ((digits - i - 1U) * 4U)) & 15U];
    return digits;
}

static int proc_maps_snapshot(struct kernel_vfs_node *node,
    struct kernel_heap *heap, char **buffer, size_t *length)
{
    struct kernel_mm_proc_map *rows = 0;
    uint32_t count = 0;
    int error = kernel_proc_maps_snapshot(proc_inode_pid(node->inode),
        proc_resource_identity(node->inode), heap, &rows, &count);
    if (error) return error;
    char *data = 0, *path_text = 0;
    size_t capacity = (size_t)count * 80U + 1U, used = 0;
    enum kernel_heap_status allocation = kernel_heap_allocate(heap, capacity, (void **)&data);
    if (allocation) { error = allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO; goto Finish; }
    if (count) {
        allocation = kernel_heap_allocate(heap, KERNEL_FS_PATH_MAX, (void **)&path_text);
        if (allocation) { error = allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO; goto Finish; }
    }
    const struct kernel_fs_context *fs = 0;
    if (count && kernel_task_fs_context_borrow(kernel_task_current(), &fs)) {
        error = -KERNEL_ENOENT; goto Finish;
    }
    for (uint32_t i = 0; i < count; i++) {
        struct kernel_mm_proc_map *row = &rows[i];
        struct kernel_vfs_stat stat = {0};
        const char *name = "";
        if(row->file && kernel_open_file_kind(row->file)==KERNEL_OPEN_FILE_KIND_BPF){
            error=kernel_open_file_pseudo_stat(row->file,&stat);if(error)goto Finish;
            name=kernel_bpf_fd_name(row->file);if(!name){error=-KERNEL_EIO;goto Finish;}
        } else if (row->file && kernel_open_file_kind(row->file) == KERNEL_OPEN_FILE_KIND_SECRETMEM) {
            error = kernel_open_file_pseudo_stat(row->file, &stat);
            if (error) goto Finish;
            name = "/secretmem (deleted)";
        } else if (row->file && kernel_open_file_kind(row->file) == KERNEL_OPEN_FILE_KIND_IO_URING) {
            error = kernel_open_file_pseudo_stat(row->file, &stat);
            if (error) goto Finish;
            name = "anon_inode:[io_uring]";
        }else if (row->file) {
            const struct kernel_vfs_file *file = kernel_open_file_vfs_borrow(row->file);
            struct kernel_vfs_path *path = kernel_open_file_path(row->file);
            if (!file || !path) { error = -KERNEL_EIO; goto Finish; }
            error = kernel_vfs_fstat(file, &stat);
            if (error) goto Finish;
            /* OFD引用固定真实后备，路径格式化不占有MM gate。 */
            {
                KERNEL_FS_PATHS(paths, fs);
                error = kernel_vfs_path_link_string(path, paths.root, path_text, KERNEL_FS_PATH_MAX);
            }
            if (error) goto Finish;
            name = path_text;
        } else if (row->role == KERNEL_VMA_ROLE_HEAP) name = "[heap]";
        else if (row->role == KERNEL_VMA_ROLE_STACK) name = "[stack]";
        else if (row->role == KERNEL_VMA_ROLE_VDSO) name = "[vdso]";
        else if (row->role == KERNEL_VMA_ROLE_VVAR) name = "[vvar]";
        char header[128];
        size_t part = proc_hex(header, row->start, 8);
        header[part++] = '-'; part += proc_hex(header + part, row->end, 8);
        header[part++] = ' ';
        header[part++] = row->permissions & KERNEL_MM_READ ? 'r' : '-';
        header[part++] = row->permissions & KERNEL_MM_WRITE ? 'w' : '-';
        header[part++] = row->permissions & KERNEL_MM_EXECUTE ? 'x' : '-';
        header[part++] = row->shared ? 's' : 'p';
        header[part++] = ' '; part += proc_hex(header + part, row->offset, 8);
        header[part++] = ' ';
        uint32_t major = ((stat.dev >> 8) & 0xfff) | ((stat.dev >> 32) & 0xfffff000);
        uint32_t minor = (stat.dev & 0xff) | ((stat.dev >> 12) & 0xffffff00);
        part += proc_hex(header + part, major, 2);
        header[part++] = ':'; part += proc_hex(header + part, minor, 2);
        header[part++] = ' '; part += decimal(header + part, stat.ino);
        header[part++] = ' ';
        if (*name) while (part < 73U) header[part++] = ' ';
        size_t name_length = strlen(name), escaped = name_length;
        for (size_t j = 0; j < name_length; j++) if (name[j] == '\n') escaped += 3U;
        size_t needed = used + part + escaped + 2U;
        if (needed > capacity) {
            size_t grown = capacity;
            while (grown < needed) {
                if (grown > SIZE_MAX / 2U) { error = -KERNEL_EOVERFLOW; goto Finish; }
                grown *= 2U;
            }
            char *replacement = 0;
            allocation = kernel_heap_allocate(heap, grown, (void **)&replacement);
            if (allocation) { error = allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO; goto Finish; }
            memcpy(replacement, data, used); kernel_heap_release(heap, data);
            data = replacement; capacity = grown;
        }
        memcpy(data + used, header, part); used += part;
        for (size_t j = 0; j < name_length; j++) {
            if (name[j] == '\n') { memcpy(data + used, "\\012", 4); used += 4; }
            else data[used++] = name[j];
        }
        data[used++] = '\n';
    }
    data[used] = 0; *buffer = data; *length = used; data = 0;
Finish:
    if (path_text) kernel_heap_release(heap, path_text);
    if (data) kernel_heap_release(heap, data);
    for (uint32_t i = 0; i < count; i++)
        if (rows[i].file && kernel_open_file_release(&rows[i].file)) __builtin_trap();
    if (rows) kernel_heap_release(heap, rows);
    return error;
}

static size_t proc_ipc_column(char *text, uint64_t value, unsigned width,
                              unsigned base, unsigned spaces, int negative)
{
    char digits[24];
    unsigned count = 0;
    do { digits[count++] = (char)('0' + value % base); value /= base; } while (value);
    size_t used = 0;
    while (spaces--) text[used++] = ' ';
    for (unsigned n = count + (unsigned)negative; n < width; n++) text[used++] = ' ';
    if (negative) text[used++] = '-';
    while (count) text[used++] = digits[--count];
    return used;
}

static size_t proc_ipc_row(uint64_t inode, struct kernel_namespace *ns,struct kernel_pid_registry *view,uint32_t *cursor, char *line)
{
    union {
        struct kernel_msqid64_ds msg;
        struct kernel_semid64_ds sem;
        struct kernel_shmid64_ds shm;
    } row = {0};
    int32_t id;
    uint64_t rss = 0;
    int found = inode == PROC_SYSVIPC_MSG_INODE ? kernel_msg_proc_next(ns,view,cursor, &id, &row.msg) :
        inode == PROC_SYSVIPC_SEM_INODE ? kernel_sem_proc_next(ns,cursor, &id, &row.sem) :
        kernel_shm_proc_next(ns,view,cursor, &id, &row.shm, &rss);
    if (!found) return 0;
    const struct kernel_ipc64_perm *permission = inode == PROC_SYSVIPC_MSG_INODE
        ? &row.msg.msg_perm : inode == PROC_SYSVIPC_SEM_INODE ? &row.sem.sem_perm : &row.shm.shm_perm;
    size_t used = 0;
#define IPC_COLUMN(value, width, base, spaces) \
    (used += proc_ipc_column(line + used, (uint64_t)(value), width, base, spaces, 0))
    int64_t key = permission->key;
    used += proc_ipc_column(line, (uint64_t)(key < 0 ? -key : key), 10, 10, 0, key < 0);
    IPC_COLUMN((uint32_t)id, 10, 10, 1);
    IPC_COLUMN(permission->mode, 4, 8, 2);
    if (inode == PROC_SYSVIPC_MSG_INODE) {
        IPC_COLUMN(row.msg.msg_cbytes, 10, 10, 2);
        IPC_COLUMN(row.msg.msg_qnum, 10, 10, 1);
        IPC_COLUMN((uint32_t)row.msg.msg_lspid, 5, 10, 1);
        IPC_COLUMN((uint32_t)row.msg.msg_lrpid, 5, 10, 1);
    } else if (inode == PROC_SYSVIPC_SEM_INODE) {
        IPC_COLUMN(row.sem.sem_nsems, 10, 10, 1);
    } else {
        IPC_COLUMN(row.shm.shm_segsz, 21, 10, 1);
        IPC_COLUMN((uint32_t)row.shm.shm_cpid, 5, 10, 1);
        IPC_COLUMN((uint32_t)row.shm.shm_lpid, 5, 10, 1);
        IPC_COLUMN(row.shm.shm_nattch, 5, 10, 2);
    }
    IPC_COLUMN(permission->uid, 5, 10, 1);
    IPC_COLUMN(permission->gid, 5, 10, 1);
    IPC_COLUMN(permission->cuid, 5, 10, 1);
    IPC_COLUMN(permission->cgid, 5, 10, 1);
    if (inode == PROC_SYSVIPC_MSG_INODE) {
        IPC_COLUMN(row.msg.msg_stime, 10, 10, 1);
        IPC_COLUMN(row.msg.msg_rtime, 10, 10, 1);
        IPC_COLUMN(row.msg.msg_ctime, 10, 10, 1);
    } else if (inode == PROC_SYSVIPC_SEM_INODE) {
        IPC_COLUMN(row.sem.sem_otime, 10, 10, 1);
        IPC_COLUMN(row.sem.sem_ctime, 10, 10, 1);
    } else {
        IPC_COLUMN(row.shm.shm_atime, 10, 10, 1);
        IPC_COLUMN(row.shm.shm_dtime, 10, 10, 1);
        IPC_COLUMN(row.shm.shm_ctime, 10, 10, 1);
        IPC_COLUMN(rss, 21, 10, 1);
        IPC_COLUMN(0, 21, 10, 1); /* The shared-memory object has no swap backing. */
    }
#undef IPC_COLUMN
    line[used++] = '\n';
    /* Native 64-bit fields, including maximal UID/time/RSS values, fit 384B. */
    if (used >= 384U) __builtin_trap();
    return used;
}

static int proc_sysvipc_snapshot(uint64_t inode,struct kernel_namespace *ns,struct kernel_pid_registry *view,struct kernel_heap *heap,
                                  char **buffer, size_t *length)
{
    if(!ns || ns->type!=KERNEL_NS_IPC)__builtin_trap();
    const char *header = inode == PROC_SYSVIPC_MSG_INODE
        ? "       key      msqid perms      cbytes       qnum lspid lrpid   uid   gid  cuid  cgid      stime      rtime      ctime\n"
        : inode == PROC_SYSVIPC_SEM_INODE
        ? "       key      semid perms      nsems   uid   gid  cuid  cgid      otime      ctime\n"
        : "       key      shmid perms                  size  cpid  lpid nattch   uid   gid  cuid  cgid      atime      dtime      ctime                   rss                  swap\n";
    size_t capacity = BOAROS_PAGE_SIZE, used = strlen(header);
    char *data = 0;
    enum kernel_heap_status status = kernel_heap_allocate(heap, capacity, (void **)&data);
    if (status != KERNEL_HEAP_STATUS_OK)
        return status == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    memcpy(data, header, used);
    uint32_t cursor = 0;
    char line[384];
    size_t part;
    /* Each iterator copies one row while its actual state is protected. Text
     * allocation and formatting hold no IPC lock or object reference. */
    while ((part = proc_ipc_row(inode,ns,view,&cursor, line))) {
        if (used + part + 1U > capacity) {
            char *replacement = 0;
            status = kernel_heap_allocate(heap, capacity * 2U, (void **)&replacement);
            if (status != KERNEL_HEAP_STATUS_OK) {
                kernel_heap_release(heap, data);
                return status == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
            }
            memcpy(replacement, data, used);
            kernel_heap_release(heap, data);
            data = replacement;
            capacity *= 2U;
        }
        memcpy(data + used, line, part);
        used += part;
    }
    data[used] = 0;
    *buffer = data;
    *length = used;
    return 0;
}

static int proc_fdinfo_snapshot(struct kernel_vfs_node *node,struct kernel_heap *heap,
    char **buffer,size_t *length)
{
    struct kernel_proc_fdinfo info;
    int error=kernel_proc_fdinfo_snapshot_in(((struct procfs_mount *)node->instance->backend_data)->pid_view,proc_inode_pid(node->inode),proc_resource_identity(node->inode),
        proc_inode_fd(node->inode),&info);
    if(error)return error;
    char *data=0;
    enum kernel_heap_status allocation=kernel_heap_allocate(heap,256+info.namespace_count*12U+info.extra_length,(void **)&data);
    if (allocation) {
        if (info.notification && kernel_open_file_release(&info.notification)) __builtin_trap();
        return allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    }
    size_t used=append_number_line(data,"pos:\t",info.position,"\n");
    memcpy(data+used,"flags:\t",7);used+=7;
    char octal[24];unsigned n=0;uint64_t flags=info.flags;
    do{octal[n++]=(char)('0'+(flags&7U));flags>>=3;}while(flags);
    data[used++]='0';while(n)data[used++]=octal[--n];data[used++]='\n';
    used+=append_number_line(data+used,"mnt_id:\t",info.mount_id,"\n");
    used+=append_number_line(data+used,"ino:\t",info.inode,"\n");
    if(info.pidfd) {
        const char *labels[]={"Pid:\t","NSpid:\t"};
        for(unsigned i=0;i<2;i++) {
            size_t part=strlen(labels[i]);memcpy(data+used,labels[i],part);used+=part;
            if(info.pid<0)data[used++]='-';
            used+=decimal(data+used,(uint32_t)(info.pid<0?-info.pid:info.pid));
            if (i == 1 && info.namespace_count > 1)
                for (unsigned n = 1; n < info.namespace_count; n++) {
                    data[used++]='\t';
                    used+=decimal(data+used,info.namespace_ids[n]);
                }
            data[used++]='\n';
        }
    }
    if(info.extra_length){memcpy(data+used,info.extra,info.extra_length);used+=info.extra_length;}
    if (info.notification) {
        if (kernel_open_file_kind(info.notification) == KERNEL_OPEN_FILE_KIND_INOTIFY)
            error = kernel_inotify_fdinfo(info.notification, heap, &data, &used);
        else error = kernel_fanotify_fdinfo(info.notification, heap, &data, &used);
        if (kernel_open_file_release(&info.notification)) __builtin_trap();
        if (error) {
            kernel_heap_release(heap, data);
            return error;
        }
    }
    data[used]=0;*buffer=data;*length=used;return 0;
}

static size_t proc_route_row(char *out, const struct kernel_socket_interface *s,
                             uint32_t destination, uint32_t gateway, uint32_t mask)
{
    size_t used = strlen(s->name);
    memcpy(out, s->name, used); out[used++] = '\t';
    used += proc_hex(out + used, destination, 8); out[used++] = '\t';
    used += proc_hex(out + used, gateway, 8);
    const char *flags = gateway ? "\t0003\t0\t0\t0\t" : "\t0001\t0\t0\t0\t";
    size_t part = strlen(flags); memcpy(out + used, flags, part); used += part;
    used += proc_hex(out + used, mask, 8);
    const char suffix[] = "\t0\t0\t0\n";
    memcpy(out + used, suffix, sizeof(suffix) - 1U);
    return used + sizeof(suffix) - 1U;
}

static int proc_route_snapshot(struct kernel_heap *heap, char **buffer, size_t *length)
{
    const char header[] = "Iface\tDestination\tGateway\tFlags\tRefCnt\tUse\tMetric\tMask\tMTU\tWindow\tIRTT\n";
    char *data = 0;
    enum kernel_heap_status status = kernel_heap_allocate(heap, 1024U, (void **)&data);
    if (status != KERNEL_HEAP_STATUS_OK)
        return status == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    size_t used = sizeof(header) - 1U;
    memcpy(data, header, used);
    for (uint32_t i = 0;; i++) {
        struct kernel_socket_interface s;
        int result = kernel_socket_interface_nth(i, &s);
        if (result == -KERNEL_ENODEV) break;
        if (result) { kernel_heap_release(heap, data); return result; }
        if (!(s.flags & 1U) || !s.address) continue;
        if (used + 160U >= 1024U) { kernel_heap_release(heap, data); return -KERNEL_ENOSPC; }
        if (s.default_route) used += proc_route_row(data + used, &s, 0, s.gateway, 0);
        if (!(s.flags & 8U))
            used += proc_route_row(data + used, &s, s.address & s.netmask, 0, s.netmask);
    }
    data[used] = 0; *buffer = data; *length = used;
    return 0;
}

static int proc_io_snapshot(struct kernel_vfs_node *node, struct kernel_heap *heap,
    char **buffer, size_t *length)
{
    struct kernel_proc_io_counters counters;
    int error = kernel_proc_io_snapshot(proc_inode_pid(node->inode),
        proc_resource_identity(node->inode), &counters);
    if (error) return error;
    char *data;
    if (kernel_heap_allocate(heap, 256U, (void **)&data)) return -KERNEL_ENOMEM;
    size_t used = append_number_line(data, "rchar: ", counters.rchar, "\n");
    used += append_number_line(data + used, "wchar: ", counters.wchar, "\n");
    used += append_number_line(data + used, "syscr: ", counters.syscr, "\n");
    used += append_number_line(data + used, "syscw: ", counters.syscw, "\n");
    used += append_number_line(data + used, "read_bytes: ", counters.read_bytes, "\n");
    used += append_number_line(data + used, "write_bytes: ", counters.write_bytes, "\n");
    used += append_number_line(data + used, "cancelled_write_bytes: ", counters.cancelled_write_bytes, "\n");
    *buffer = data;
    *length = used;
    return 0;
}

#if !defined(BOAROS_ARCH_LOONGARCH)
static size_t proc_cpuinfo_riscv_isa(char *text, const char *label, uint64_t extensions)
{
    static const struct {
        unsigned bit;
        const char *name;
    } suffixes[] = {
        {RISCV_HWPROBE_FD, "fd"}, {RISCV_HWPROBE_C, "c"}, {RISCV_HWPROBE_V, "v"},
        {RISCV_HWPROBE_ZICBOZ, "_zicboz"}, {0, "_zicsr_zifencei"},
        {RISCV_HWPROBE_ZBA, "_zba"}, {RISCV_HWPROBE_ZBB, "_zbb"},
        {RISCV_HWPROBE_ZBS, "_zbs"},
    };
    size_t used = strlen(label);
    memcpy(text, label, used);
    memcpy(text + used, "rv64ima", sizeof("rv64ima") - 1U);
    used += sizeof("rv64ima") - 1U;
    for (unsigned i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        if (suffixes[i].bit && !(extensions & suffixes[i].bit))
            continue;
        size_t bytes = strlen(suffixes[i].name);
        memcpy(text + used, suffixes[i].name, bytes);
        used += bytes;
    }
    text[used++] = '\n';
    return used;
}
#endif

static int proc_cpuinfo_snapshot(struct kernel_heap *heap, char **buffer, size_t *length)
{
    const struct kernel_cpu *online[BOAROS_MAX_CPUS] = {0};
#if BOAROS_MAX_CPUS > 1
    unsigned count = 0;
    for (unsigned i = 0; i < kernel_cpu_get_boot_count(); i++) {
        const struct kernel_cpu *cpu = arch_smp_cpu(i);
        if (cpu && kernel_cpu_online(cpu)) {
            online[i] = cpu;
            count++;
        }
    }
#else
    unsigned count = 1;
    online[0] = kernel_cpu_current();
#endif

    /* Each record has two decimal uint64_t fields and bounded text. */
#if defined(BOAROS_ARCH_LOONGARCH)
    static const char isa[] = "ISA\t\t\t: loongarch64\n";
    const size_t record_max = 2U * 20U + sizeof("processor\t\t: \nglobal_id\t\t: \n\n") +
        sizeof(isa);
#else
    static const char mmu[] = "mmu\t\t: sv39\n";
    static const char *const id_labels[] = {"mvendorid\t: 0x", "marchid\t\t: 0x", "mimpid\t\t: 0x"};
    const size_t record_max = 2U * 20U + 3U * 16U + sizeof(
        "processor\t: \nhart\t\t: \nisa\t\t: \nmvendorid\t: 0x\nmarchid\t\t: 0x\nmimpid\t\t: 0x\n"
        "hart isa\t: \n\n") + sizeof(mmu) + 2U * sizeof(
        "rv64imafdcv_zicboz_zicsr_zifencei_zba_zbb_zbs");
    uint64_t common = UINT64_MAX;
    for (unsigned i = 0; i < BOAROS_MAX_CPUS; i++) {
        if (online[i])
            common &= riscv_hwprobe_get_cpu_info(i)->extensions;
    }
#endif
    char *data = NULL;
    enum kernel_heap_status status = kernel_heap_allocate(heap, count * record_max + 1U,
                                                          (void **)&data);
    if (status != KERNEL_HEAP_STATUS_OK)
        return status == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;

    size_t used = 0;
    for (unsigned i = 0; i < BOAROS_MAX_CPUS; i++) {
        if (!online[i])
            continue;
#if defined(BOAROS_ARCH_LOONGARCH)
        used += append_number_line(data + used, "processor\t\t: ", i, "\n");
        used += append_number_line(data + used, "global_id\t\t: ", online[i]->hardware_id, "\n");
        memcpy(data + used, isa, sizeof(isa) - 1U);
        used += sizeof(isa) - 1U;
#else
        const struct riscv_hwprobe_cpu_info *info = riscv_hwprobe_get_cpu_info(i);
        used += append_number_line(data + used, "processor\t: ", i, "\n");
        used += append_number_line(data + used, "hart\t\t: ", online[i]->hardware_id, "\n");
        used += proc_cpuinfo_riscv_isa(data + used, "isa\t\t: ", common);
        memcpy(data + used, mmu, sizeof(mmu) - 1U);
        used += sizeof(mmu) - 1U;
        if (info->ids_valid) {
            for (unsigned j = 0; j < sizeof(id_labels) / sizeof(id_labels[0]); j++) {
                size_t bytes = strlen(id_labels[j]);
                memcpy(data + used, id_labels[j], bytes);
                used += bytes;
                used += proc_hex(data + used, info->ids[j], 0);
                data[used++] = '\n';
            }
        }
        used += proc_cpuinfo_riscv_isa(data + used, "hart isa\t: ", info->extensions);
#endif
        data[used++] = '\n';
    }
    data[used] = '\0';
    *buffer = data;
    *length = used;
    return 0;
}

static size_t proc_cpu_stat_row(char *text, const struct kernel_cpu_statistics *cpu, int index)
{
    memcpy(text, "cpu", 3);
    size_t used = 3;
    if (index >= 0) used += decimal(text + used, (unsigned)index);
    else text[used++] = ' ';
    /* BoarOS tracks time spent running idle tasks and IRQ handlers directly.
     * I/O-wait time, softirq, steal, guest and guest_nice have no time provider. */
    const uint64_t ticks[] = {
        cpu->ticks[KERNEL_CPU_TIME_USER], cpu->ticks[KERNEL_CPU_TIME_NICE],
        cpu->ticks[KERNEL_CPU_TIME_SYSTEM], cpu->ticks[KERNEL_CPU_TIME_IDLE],
        0, cpu->ticks[KERNEL_CPU_TIME_IRQ], 0, 0, 0, 0,
    };
    for (unsigned i = 0; i < sizeof(ticks) / sizeof(ticks[0]); i++) {
        text[used++] = ' ';
        /* Linux USER_HZ is 100 on both BoarOS architectures. */
        used += decimal(text + used, kernel_time_ticks_to_ns(ticks[i]) / UINT64_C(10000000));
    }
    text[used++] = '\n';
    return used;
}

static int proc_system_stat_snapshot(struct kernel_heap *heap, char **buffer, size_t *length)
{
    enum { ROW_BYTES = 3 + 20 + 10 * 21 + 1, SUMMARY_BYTES = 256 };
    struct kernel_cpu_statistics records[BOAROS_MAX_CPUS];
    struct kernel_cpu_statistics total = {0};
    unsigned online = kernel_cpu_statistics_snapshot(records);
    for (unsigned i = 0; i < BOAROS_MAX_CPUS; i++) {
        if (!(online & (1U << i))) continue;
        for (unsigned t = 0; t < KERNEL_CPU_TIME_COUNT; t++)
            total.ticks[t] += records[i].ticks[t];
        total.interrupts += records[i].interrupts;
        total.switches += records[i].switches;
        total.tasks += records[i].tasks;
    }
    uint64_t runnable, blocked;
    kernel_scheduler_task_statistics(&runnable, &blocked);
    char *data;
    if (kernel_heap_allocate(heap, (BOAROS_MAX_CPUS + 1U) * ROW_BYTES + SUMMARY_BYTES,
            (void **)&data)) return -KERNEL_ENOMEM;
    size_t used = proc_cpu_stat_row(data, &total, -1);
    for (unsigned i = 0; i < BOAROS_MAX_CPUS; i++)
        if (online & (1U << i))
            used += proc_cpu_stat_row(data + used, &records[i], (int)i);

    /* The interrupt total includes local and external interrupts. There is no
     * global Linux IRQ-number table or separate software-interrupt subsystem. */
    used += append_number_line(data + used, "intr ", total.interrupts, "\n");
    used += append_number_line(data + used, "ctxt ", total.switches, "\n");
    used += append_number_line(data + used, "btime ",
        kernel_time_boot_realtime_offset() / UINT64_C(1000000000), "\n");
    used += append_number_line(data + used, "processes ", total.tasks, "\n");
    used += append_number_line(data + used, "procs_running ", runnable, "\n");
    used += append_number_line(data + used, "procs_blocked ", blocked, "\n");
    static const char softirq[] = "softirq 0 0 0 0 0 0 0 0 0 0 0\n";
    memcpy(data + used, softirq, sizeof(softirq));
    *buffer = data;
    *length = used + sizeof(softirq) - 1U;
    return 0;
}

static size_t proc_diskstats_row(char *text, const struct kernel_block_device *device)
{
    char name[32];
    uint64_t number = device->device_number;
    if (!kernel_block_device_name(number, name)) return 0;

    struct kernel_block_usage usage;
    uint64_t fields[KERNEL_BLOCK_STAT_FIELDS];
    kernel_block_usage_snapshot(device, &usage);
    kernel_block_stat_values(&usage, fields);

    uint64_t major = (number >> 8U) & UINT64_C(0xfff);
    uint64_t minor = (number & UINT64_C(0xff)) | ((number >> 12U) & UINT64_C(0xfff00));
    size_t used = decimal(text, major);
    text[used++] = ' ';
    used += decimal(text + used, minor);
    text[used++] = ' ';
    size_t name_bytes = strlen(name);
    memcpy(text + used, name, name_bytes);
    used += name_bytes;
    for (unsigned i = 0; i < KERNEL_BLOCK_STAT_FIELDS; i++) {
        text[used++] = ' ';
        used += decimal(text + used, fields[i]);
    }
    text[used++] = '\n';
    return used;
}

static int proc_diskstats_snapshot(struct kernel_heap *heap, char **buffer, size_t *length)
{
    /* A successful enumeration pins each registration and its immutable
     * number/generation. No registry protection is held when sampling or formatting. */
    struct kernel_block_device **devices = NULL;
    size_t count, capacity = 0;
    int error;
    while ((error = kernel_block_snapshot(devices, capacity, &count)) == -KERNEL_ENOSPC) {
        if (devices) kernel_heap_release(heap, devices);
        if (kernel_heap_allocate_zeroed(heap, count, sizeof(*devices), (void **)&devices))
            return -KERNEL_ENOMEM;
        capacity = count;
    }
    if (error) {
        if (devices) kernel_heap_release(heap, devices);
        return error;
    }

    enum { ROW_BYTES = 2 * 21 + 32 + KERNEL_BLOCK_STAT_FIELDS * 21 + 1 };
    size_t used = 0;
    char *data;
    if (count > (SIZE_MAX - 1U) / ROW_BYTES ||
        kernel_heap_allocate(heap, count * ROW_BYTES + 1U, (void **)&data)) {
        error = -KERNEL_ENOMEM;
    } else {
        for (size_t i = 0; i < count; i++)
            used += proc_diskstats_row(data + used, devices[i]);
        data[used] = 0;
        *buffer = data;
        *length = used;
    }
    for (size_t i = 0; i < count; i++) kernel_block_put(devices[i]);
    if (devices) kernel_heap_release(heap, devices);
    return error;
}

static int proc_snapshot(struct kernel_vfs_node *node, struct kernel_heap *heap,
                         struct kernel_namespace *ipc_namespace,struct kernel_pid_registry *ipc_pid_view,char **buffer, size_t *length)
{
    if (proc_inode_kind(node->inode) == PROC_PID_IO_KIND)
        return proc_io_snapshot(node, heap, buffer, length);
    if (proc_inode_kind(node->inode) == PROC_PID_CGROUP_KIND || proc_inode_kind(node->inode) == PROC_TID_CGROUP_KIND)
        return kernel_proc_cgroup(proc_inode_pid(node->inode),proc_inode_identity(node->inode),
            proc_inode_is_thread(node->inode),heap,buffer,length);
    if (proc_inode_kind(node->inode) == PROC_PID_COMM_KIND || proc_inode_kind(node->inode) == PROC_TID_COMM_KIND) {
        char name[16];
        int error = kernel_proc_comm(proc_inode_pid(node->inode), proc_inode_identity(node->inode),
            proc_inode_is_thread(node->inode), name, 0);
        if (error) return error;
        size_t used = strlen(name);
        char *data = 0;
        enum kernel_heap_status status = kernel_heap_allocate(heap, used + 2U, (void **)&data);
        if (status) return status == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
        memcpy(data, name, used);
        data[used++] = '\n';
        data[used] = 0;
        *buffer = data; *length = used;
        return 0;
    }

    if (node->inode == PROC_NET_ROUTE_INODE)
        return proc_route_snapshot(heap, buffer, length);
    if (node->inode == PROC_STAT_INODE)
        return proc_system_stat_snapshot(heap, buffer, length);
    if (node->inode == PROC_DISKSTATS_INODE)
        return proc_diskstats_snapshot(heap, buffer, length);
    if (node->inode == PROC_CPUINFO_INODE)
        return proc_cpuinfo_snapshot(heap, buffer, length);
    if(node->inode>=(UINT64_C(1)<<30U) && proc_inode_kind(node->inode)==PROC_PID_FDINFO_KIND)
        return proc_fdinfo_snapshot(node,heap,buffer,length);
    if (node->inode == PROC_SYSVIPC_SHM_INODE || node->inode == PROC_SYSVIPC_SEM_INODE ||
        node->inode == PROC_SYSVIPC_MSG_INODE)
        return proc_sysvipc_snapshot(node->inode,ipc_namespace,ipc_pid_view,heap,buffer,length);
    if (proc_inode_kind(node->inode) == PROC_PID_MAPS_KIND)
        return proc_maps_snapshot(node, heap, buffer, length);
#if BOAROS_COST_DIAGNOSTICS
    if (node->inode == PROC_MEMORY_STATS_INODE) {
        struct kernel_heap_statistics hs;
        uint64_t irq = arch_interrupt_save();
        kernel_heap_get_statistics(heap, &hs);
        uint64_t current = heap->page_allocator->total_pages - heap->page_allocator->available_pages;
        uint64_t peak = heap->page_allocator->allocated_peak_pages;
        arch_interrupt_restore(irq);
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
        uint64_t interrupts = arch_interrupt_save();
        int result = kernel_cost_format(data, capacity);
        arch_interrupt_restore(interrupts);
        if (result < 0) { kernel_heap_release(heap, data); return result; }
        *buffer = data; *length = (size_t)result;
        return 0;
    }
#endif
    if (proc_inode_kind(node->inode) == PROC_PID_STAT_KIND ||
        proc_inode_kind(node->inode) == PROC_PID_STATUS_KIND ||
        proc_inode_kind(node->inode) == PROC_TID_STAT_KIND ||
        proc_inode_kind(node->inode) == PROC_TID_STATUS_KIND)
        return proc_process_snapshot(node, heap, buffer, length);
    if (proc_inode_kind(node->inode) == PROC_PID_MOUNTS_KIND ||
        proc_inode_kind(node->inode)==PROC_PID_MOUNTINFO_KIND) {
        uint64_t identity = 0U;
        if (proc_inode_identity_current(node->inode,&identity) ||
            identity != proc_inode_identity(node->inode)) return -KERNEL_ENOENT;
        return proc_mounts_snapshot(node, heap, buffer, length);
    }
    if (node->inode != PROC_MEMINFO_INODE && node->inode != PROC_FILESYSTEMS_INODE &&
        node->inode != PROC_UPTIME_INODE &&
        node->inode != PROC_PID_MAX_INODE) return -KERNEL_EINVAL;
    char *data = 0;
    enum kernel_heap_status status = kernel_heap_allocate(heap, 1024U,
                                                           (void **)&data);
    if (status != KERNEL_HEAP_STATUS_OK)
        return status == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    size_t used = 0U;
    if (node->inode == PROC_FILESYSTEMS_INODE) {
        int block;
        const char *type;
        for (size_t i=0;(type=kernel_mount_registered_type(i,&block));i++) {
            const char *prefix=block ? "\t" : "nodev\t";
            size_t prefix_length=strlen(prefix), type_length=strlen(type);
            size_t bytes=prefix_length+type_length+1;
            if (bytes>=1024U-used) {
                kernel_heap_release(heap,data);
                return -KERNEL_EOVERFLOW;
            }
            memcpy(data+used,prefix,prefix_length);
            memcpy(data+used+prefix_length,type,type_length);
            data[used+bytes-1]='\n';
            used+=bytes;
        }
    } else if (node->inode == PROC_MEMINFO_INODE) {
        struct kernel_memory_statistics memory;
        uintptr_t irq = arch_interrupt_save();
        kernel_memory_snapshot(heap->page_allocator, &memory);
        arch_interrupt_restore(irq);
        const char *names[] = {"MemTotal:       ", "MemFree:        ",
            "MemAvailable:   ", "Buffers:        ", "Cached:         ",
            "Shmem:          ", "Dirty:          ", "Writeback:      ",
            "SReclaimable:   ", "SwapTotal:      ", "SwapFree:       "};
        uint64_t bytes[] = {memory.total, memory.free, memory.available,
            memory.buffers, memory.cached, memory.shared, memory.dirty,
            memory.writeback, 0, 0, 0};
        for (unsigned i = 0; i < sizeof(bytes) / sizeof(bytes[0]); i++)
            used += append_kib(data + used, names[i], bytes[i]);
        used+=append_kib(data+used,"AnonHugePages:  ",
            physical_page_thp_pages(heap->page_allocator)*BOAROS_PAGE_SIZE);
        struct kernel_hstate_snapshot huge;
        int error=proc_huge_snapshot(heap->page_allocator,&huge);
        if (error) { kernel_heap_release(heap,data); return error; }
        const char *huge_names[]={"HugePages_Total: ","HugePages_Free:  ","HugePages_Rsvd:  ","HugePages_Surp:  "};
        const uint64_t counts[]={huge.total,huge.free,huge.reserved,huge.surplus};
        for (unsigned i=0;i<4;i++) {
            size_t n=strlen(huge_names[i]); memcpy(data+used,huge_names[i],n); used+=n;
            used+=decimal(data+used,counts[i]); data[used++]='\n';
        }
        used+=append_kib(data+used,"Hugepagesize:    ",ARCH_MMU_HUGE_SIZE);
    } else if (node->inode == PROC_PID_MAX_INODE) {
        used = decimal(data, kernel_proc_pid_max());
        data[used++] = '\n';
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
    struct procfs_mount *proc = ((struct kernel_vfs_instance *)mount->private_data)->backend_data;
    struct kernel_vfs_instance *instance = &proc->instance;
    if (mount->covered_path || mount->root_path || mount->child_mounts ||
        instance->external_files || !kernel_vfs_nodes_idle(instance, NULL) || instance->paths)
        return -KERNEL_EBUSY;
    struct kernel_heap *heap = proc->heap;
    kernel_proc_view_release(proc->pid_view);
    kernel_heap_release(heap, proc);
    return 0;
}

static struct kernel_vfs_backend proc_backend;

static void initialize_backend(void)
{
    /* Early VFS module tests execute at physical addresses before Sv39. */
    volatile struct kernel_vfs_backend *ops = &proc_backend;
    ops->type="proc";
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
    ops->permission=proc_permission;
    ops->statfs = proc_statfs;
    ops->dir_entry = proc_dir_entry;
    ops->snapshot = proc_snapshot;
    ops->control = proc_control;
    ops->accessed = proc_accessed;
    ops->unmount = proc_unmount;
}

int kernel_procfs_create_in(struct kernel_heap *heap, uint64_t flags,
    struct kernel_pid_registry *view, struct kernel_vfs_mount **owner)
{
    if (!heap || !owner || *owner) return -KERNEL_EINVAL;
    struct procfs_mount *proc = 0;
    enum kernel_heap_status status = kernel_heap_allocate_zeroed(
        heap, 1U, sizeof(*proc), (void **)&proc);
    if (status != KERNEL_HEAP_STATUS_OK)
        return status == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    proc->heap = heap;
    proc->flags = flags;
    if (view) { kernel_proc_view_retain(view); proc->pid_view = view; }
    else proc->pid_view = kernel_proc_view_acquire();
    initialize_backend();
    proc->instance.ops = &proc_backend;
    proc->instance.heap = heap;
    proc->instance.backend_data = proc;
    proc->instance.read_only = (flags & 1U) != 0U;
    kernel_rwlock_init(&proc->instance.reconfiguration, 19, (uintptr_t)&proc->instance);
    kernel_mutex_init(&proc->instance.namespace_lock, 20U,
                      (uintptr_t)&proc->instance);
    kernel_raw_lock_init(&proc->instance.error_lock, KERNEL_RAW_RANK_OBJECT);
    proc->mount.id = kernel_vfs_allocate_mount_id();
    proc->mount.nosuid=!!(flags&2U);
    proc->mount.private_data = &proc->instance;
    proc->mount.state = VFS_MOUNT_STATE_LIVE;
    kernel_vfs_mount_initialize(&proc->mount);
    *owner = &proc->mount;
    return 0;
}

int kernel_procfs_create(struct kernel_heap *heap, uint64_t flags,
    struct kernel_vfs_mount **owner)
{ return kernel_procfs_create_in(heap, flags, NULL, owner); }

int kernel_procfs_is_mount(const struct kernel_vfs_mount *mount)
{
    return mount && mount->private_data &&
           ((struct kernel_vfs_instance *)mount->private_data)->ops ==
               &proc_backend;
}

int kernel_procfs_ipc_file(const struct kernel_vfs_file *file)
{
    if(!file || !kernel_procfs_is_mount(file->mount))return 0;
    const struct kernel_vfs_node *node=file->private_data;
    return node && (node->inode==PROC_SYSVIPC_SHM_INODE ||
        node->inode==PROC_SYSVIPC_SEM_INODE || node->inode==PROC_SYSVIPC_MSG_INODE);
}

int kernel_procfs_shm_limits_open(const struct kernel_vfs_file *file, struct kernel_shm_limits **owner)
{
    if (!file || !kernel_procfs_is_mount(file->mount)) return 0;
    const struct kernel_vfs_node *node = file->private_data;
    return node && proc_is_shm_limit(node->inode) ?
        kernel_ipc_shm_limits_open(kernel_task_ipc_namespace(), owner) : 0;
}

int kernel_procfs_kpageflags_file(const struct kernel_vfs_file *file)
{
    if (!file || !kernel_procfs_is_mount(file->mount)) return 0;
    const struct kernel_vfs_node *node=file->private_data;
    return node && node->inode==PROC_KPAGEFLAGS_INODE;
}

int kernel_procfs_pagemap_open(const struct kernel_vfs_file *file,
    struct kernel_heap *heap, struct kernel_mm_pagemap **owner)
{
    const struct kernel_vfs_node *node = file->private_data;
    if (!node || !kernel_procfs_is_mount(file->mount) ||
        proc_inode_kind(node->inode) != PROC_PID_PAGEMAP_KIND) return 0;
    struct kernel_mm_pagemap *view;
    if (kernel_heap_allocate(heap, sizeof(*view), (void **)&view)) return -KERNEL_ENOMEM;
    int error = kernel_proc_pagemap_open(proc_inode_pid(node->inode),
        proc_resource_identity(node->inode), view);
    if (error) kernel_heap_release(heap, view);
    else *owner = view;
    return error;
}

struct kernel_pid *kernel_procfs_process_identity(const struct kernel_vfs_file *file)
{
    if(!file || !kernel_procfs_is_mount(file->mount))return 0;
    const struct kernel_vfs_node *node=file->private_data;
    return node && node->inode>=(UINT64_C(1)<<30U) &&
        proc_inode_kind(node->inode)==PROC_PID_DIR_KIND?node->backend_data:0;
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
