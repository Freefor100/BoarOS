#include "private.h"

#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/process_acct.h>
#include <kernel/rlimit.h>
#include <kernel/time.h>
#include <kernel/tty_task.h>
#include <kernel/uaccess.h>
#include <kernel/vfs.h>
#include <stddef.h>
#include <string.h>

#define ACCT_TICK_NS UINT64_C(10000000)
#define ACCT_PRESSURE_INTERVAL_NS UINT64_C(30000000000)
#define ACCT_SUSPEND_PERCENT 2U
#define ACCT_RESUME_PERCENT 4U

struct acct_v3 {
    uint8_t flags, version;
    uint16_t tty;
    uint32_t exitcode, uid, gid, pid, ppid, btime;
    uint32_t etime;
    uint16_t utime, stime, mem, io, rw, minflt, majflt, swaps;
    char comm[16];
};
_Static_assert(sizeof(struct acct_v3) == 64 && offsetof(struct acct_v3, comm) == 48,
               "Linux acct_v3 layout");
enum { ACCT_FORK = 1U, ACCT_CORE = 8U, ACCT_SIGNAL = 16U };

struct kernel_process_acct {
    struct kernel_process_acct *next, **previous;
    struct kernel_pid_registry *space;
    struct kernel_heap *heap;
    struct kernel_vfs_file file;
    const struct kernel_credentials *credentials;
    uint64_t next_pressure_check;
    int running;
};

static struct kernel_mutex accounting_lock;
static struct kernel_process_acct *sinks;
static unsigned enabled, accounting_lock_ready;

void kernel_process_acct_lock(struct kernel_lock_guard *guard)
{
    if (!__atomic_load_n(&accounting_lock_ready, __ATOMIC_ACQUIRE)) {
        KERNEL_RAW_SCOPE(domain, &kernel_wait_domain);
        if (!accounting_lock_ready) {
            kernel_mutex_init(&accounting_lock, 10, (uintptr_t)&accounting_lock);
            __atomic_store_n(&accounting_lock_ready, 1U, __ATOMIC_RELEASE);
        }
    }
    if (!kernel_lock_held(&accounting_lock.lock, 1))
        kernel_mutex_lock(&accounting_lock, guard);
}

int kernel_process_acct_enabled(void)
{
    return __atomic_load_n(&enabled, __ATOMIC_ACQUIRE) != 0;
}

/* BSD comp_t stores a 13-bit significand and a 3-bit base-eight exponent. */
static uint16_t acct_comp(uint64_t n)
{
    unsigned exponent = 0;
    unsigned round = 0;
    while (n > 8191U) {
        round = (n & 4U) != 0;
        n >>= 3U;
        exponent++;
    }
    if (round && ++n > 8191U) {
        n >>= 3U;
        exponent++;
    }
    return exponent > 7 ? UINT16_MAX : (uint16_t)((exponent << 13) | n);
}

/* The v3 elapsed-time field takes IEEE 754 bits and truncates excess bits.
 * Accounting code must avoid floating point in kernel mode. */
static uint32_t acct_elapsed(uint64_t ticks)
{
    if (!ticks) return 0;
    unsigned exponent = 190;
    while (!(ticks >> 63U)) {
        ticks <<= 1U;
        exponent--;
    }
    return (exponent << 23U) | ((uint32_t)(ticks >> 40U) & 0x7fffffU);
}

static void acct_make_record(struct kernel_task *task, struct acct_v3 *record, int closing)
{
    struct kernel_task *leader = task->group_leader;
    struct kernel_resource_limits *limits = leader->limits;
    struct kernel_task_usage usage;
    if (closing) scheduler_group_usage_snapshot(leader, 0, &usage);
    uint64_t now = kernel_time_raw_ns();
    uint64_t elapsed = now >= limits->acct_start_ns ? now - limits->acct_start_ns : 0;
    uint64_t wall_seconds = kernel_time_realtime_ns() / UINT64_C(1000000000);
    uint64_t elapsed_seconds = elapsed / UINT64_C(1000000000);
    uint64_t start_seconds = wall_seconds > elapsed_seconds ? wall_seconds - elapsed_seconds : 0;
    uint64_t memory_kib = limits->acct_memory_kib;
    if (closing) {
        struct kernel_mm_proc_memory memory;
        if (!kernel_mm_proc_memory_snapshot(&task->mm, &memory))
            memory_kib = memory.virtual_bytes / 1024U;
    }

    KERNEL_RAW_SCOPE(domain, &kernel_wait_domain);
    if (!closing) {
        struct kernel_task_usage own;
        usage = limits->dead_usage;
        scheduler_usage_thread_locked(task, &own);
        scheduler_usage_add(&usage, &own);
    }
    const struct kernel_credentials *cred = kernel_credentials_task_locked(task);
    int use_last_thread = !closing && limits->acct_last_nonleader;
    uint32_t status = limits->acct_leader_status;
    uint64_t tty = kernel_tty_rdev(leader->controlling_tty);
    *record = (struct acct_v3){
        .flags = (leader->fork_no_exec ? ACCT_FORK : 0) |
                 ((status & 0x7fU) ? ACCT_SIGNAL : 0) |
                 ((status & 0x80U) ? ACCT_CORE : 0),
        .version = 3,
        .tty = (uint16_t)tty,
        .exitcode = status,
        .uid = use_last_thread ? limits->acct_last_uid : cred->uid,
        .gid = use_last_thread ? limits->acct_last_gid : cred->gid,
        .btime = start_seconds > UINT32_MAX ? UINT32_MAX : (uint32_t)start_seconds,
        .etime = acct_elapsed(elapsed / ACCT_TICK_NS),
        .utime = acct_comp(usage.user_ns / ACCT_TICK_NS),
        .stime = acct_comp(usage.kernel_ns / ACCT_TICK_NS),
        .mem = acct_comp(memory_kib),
        .minflt = acct_comp(usage.minor_faults),
        .majflt = acct_comp(usage.major_faults),
    };
    memcpy(record->comm, use_last_thread ? limits->acct_last_comm : task->comm,
           sizeof(record->comm));
}

/* Read the identities while the scheduler owns both task links, then release
 * that short domain before touching the file or checking disk space. */
static int acct_record_pids(struct kernel_task *task, struct kernel_pid_registry *space,
                            struct acct_v3 *record)
{
    KERNEL_RAW_SCOPE(domain, &kernel_wait_domain);
    struct kernel_task *leader = task->group_leader;
    record->pid = (uint32_t)kernel_pid_number_in(process_identity(leader, KERNEL_PID_TGID), space);
    record->ppid = leader->parent ?
        (uint32_t)kernel_pid_number_in(process_identity(leader->parent, KERNEL_PID_TGID), space) : 0;
    return record->pid != 0;
}

static void acct_write(struct kernel_process_acct *sink, const struct acct_v3 *record)
{
    const struct kernel_credentials *previous = kernel_credentials_override(sink->credentials);
    uint64_t now = kernel_time_raw_ns();
    if (now >= sink->next_pressure_check) {
        struct kernel_vfs_statfs stat;
        if (!kernel_vfs_mount_statfs(sink->file.mount, &stat)) {
            unsigned percent = sink->running ? ACCT_SUSPEND_PERCENT : ACCT_RESUME_PERCENT;
            uint64_t threshold = stat.blocks / 100U * percent +
                                 stat.blocks % 100U * percent / 100U;
            sink->running = !stat.blocks || (sink->running ? stat.available_blocks > threshold :
                                                           stat.available_blocks >= threshold);
            sink->next_pressure_check = now + ACCT_PRESSURE_INTERVAL_NS;
        }
    }
    if (sink->running) {
        int error = kernel_vfs_append_record(&sink->file, record, sizeof(*record));
        if (error == -KERNEL_ENOSPC) {
            sink->running = 0;
            sink->next_pressure_check = now + ACCT_PRESSURE_INTERVAL_NS;
        }
    }
    kernel_credentials_override(previous);
}

static void acct_release_sink(struct kernel_process_acct *sink)
{
    const struct kernel_credentials *previous = kernel_credentials_override(sink->credentials);
    (void)kernel_vfs_close(&sink->file);
    kernel_credentials_override(previous);
    kernel_credentials_put(sink->credentials);
    kernel_heap_release(sink->heap, sink);
}

static void acct_close(struct kernel_process_acct *sink)
{
    if (!sink) return;
    if (sink->space->accounting == sink) sink->space->accounting = NULL;
    *sink->previous = sink->next;
    if (sink->next) sink->next->previous = sink->previous;
    __atomic_sub_fetch(&enabled, 1U, __ATOMIC_RELEASE);
    acct_release_sink(sink);
}

struct acct_request {
    const struct kernel_fs_context *fs;
    const char *path;
};

static int acct_prepare(struct kernel_task *caller, uint64_t address,
                        struct acct_request *request)
{
    struct kernel_mm *mm;
    if (kernel_task_fs_context_borrow(caller, &request->fs) ||
        kernel_task_mm_borrow_mutable(caller, &mm)) return -KERNEL_EFAULT;
    char *path = NULL;
    if (kernel_heap_allocate(request->fs->heap, KERNEL_FS_PATH_MAX, (void **)&path))
        return -KERNEL_ENOMEM;
    request->path = path;
    size_t length;
    enum kernel_uaccess_status status =
        kernel_copy_string_from_user(mm, path, address, KERNEL_FS_PATH_MAX, &length);
    return status == KERNEL_UACCESS_STATUS_TOO_LONG ? -KERNEL_ENAMETOOLONG :
        status ? -KERNEL_EFAULT : !length ? -KERNEL_ENOENT : 0;
}

static void acct_release_request(struct acct_request *request)
{
    if (request->path) kernel_heap_release(request->fs->heap, (void *)request->path);
}

static int acct_open(struct acct_request *request, struct kernel_pid_registry *space,
                     struct kernel_process_acct **owner)
{
    const struct kernel_fs_context *fs = request->fs;
    struct kernel_process_acct *sink = NULL;
    if (kernel_heap_allocate_zeroed(fs->heap, 1, sizeof(*sink), (void **)&sink))
        return -KERNEL_ENOMEM;
    KERNEL_FS_PATHS(paths, fs);
    int error = kernel_vfs_acct_open(paths.cwd, paths.root, request->path, &sink->file);
    if (error) {
        kernel_heap_release(fs->heap, sink);
        return error;
    }
    sink->space = space;
    sink->heap = fs->heap;
    sink->credentials = kernel_credentials_get(kernel_credentials_current());
    *owner = sink;
    return 0;
}

int kernel_process_acct_set(struct kernel_task *task, uint64_t name)
{
    if (!kernel_credentials_capable(kernel_credentials_current(), KERNEL_CAP_SYS_PACCT))
        return -KERNEL_EPERM;
    struct acct_request request __attribute__((cleanup(acct_release_request))) = {0};
    if (name) {
        int error = acct_prepare(task, name, &request);
        if (error) return error;
    }
    struct kernel_pid_registry *space;
    {
        KERNEL_RAW_SCOPE(domain, &kernel_wait_domain);
        space = process_pid_active(task);
    }
    if (!space) return -KERNEL_ESRCH;
    KERNEL_SERVICE_SCOPE(service);
    struct kernel_process_acct *candidate = NULL;
    if (name) {
        /* A conflicting lease can belong to a task whose exit needs the
         * accounting lock before the reaper can release its resources. */
        int error = acct_open(&request, space, &candidate);
        if (error) return error;
    }
    KERNEL_LOCK_SCOPE(guard);
    kernel_process_acct_lock(&guard);
    struct kernel_process_acct *old = space->accounting;
    if (candidate) {
        int error = kernel_vfs_acct_publish(&candidate->file);
        if (error) {
            acct_release_sink(candidate);
            return error;
        }
        candidate->next = sinks;
        candidate->previous = &sinks;
        if (sinks) sinks->previous = &candidate->next;
        sinks = candidate;
        space->accounting = candidate;
        __atomic_add_fetch(&enabled, 1U, __ATOMIC_RELEASE);
    }
    if (old) {
        struct acct_v3 record;
        acct_make_record(task, &record, 1);
        if (acct_record_pids(task, space, &record)) acct_write(old, &record);
        acct_close(old);
    }
    return 0;
}

void kernel_process_acct_collect_memory(struct kernel_task *task)
{
    if (task->mm.state != KERNEL_MM_LIVE) return;
    struct kernel_task *leader = task->group_leader;
    if (!leader || !leader->limits) return;
    if (leader->group_members != 1 &&
        (leader->group_members != 2 || leader->state != KERNEL_THREAD_STATE_GROUP_DEAD))
        return;
    struct kernel_mm_proc_memory memory;
    if (!kernel_mm_proc_memory_snapshot(&task->mm, &memory))
        leader->limits->acct_memory_kib = memory.virtual_bytes / 1024U;
}

void kernel_process_acct_exit(struct kernel_task *leader)
{
    if (!kernel_process_acct_enabled()) return;
    struct kernel_pid_registry *space;
    int namespace_init;
    {
        KERNEL_RAW_SCOPE(domain, &kernel_wait_domain);
        space = process_pid_active(leader);
        namespace_init = space && space->parent &&
            space->child_reaper == process_identity(leader, KERNEL_PID_TGID);
    }
    KERNEL_LOCK_SCOPE(guard);
    kernel_process_acct_lock(&guard);
    struct acct_v3 record;
    int prepared = 0;
    for (struct kernel_pid_registry *view = space; view; view = view->parent) {
        struct kernel_process_acct *sink = view->accounting;
        if (!sink) continue;
        if (!prepared) {
            acct_make_record(leader, &record, 0);
            prepared = 1;
        }
        if (acct_record_pids(leader, view, &record)) acct_write(sink, &record);
        if (namespace_init && view == space) acct_close(sink);
    }
}

int kernel_process_acct_has_mount(const struct kernel_vfs_mount *mount)
{
    for (const struct kernel_process_acct *sink = sinks; sink; sink = sink->next)
        if (sink->file.mount == mount) return 1;
    return 0;
}

void kernel_process_acct_unmount(struct kernel_vfs_mount *mount)
{
    struct kernel_process_acct *sink = sinks;
    while (sink) {
        struct kernel_process_acct *next = sink->next;
        if (sink->file.mount == mount) acct_close(sink);
        sink = next;
    }
}

void kernel_process_acct_shutdown(void)
{
    if (!kernel_process_acct_enabled()) return;
    KERNEL_LOCK_SCOPE(guard);
    kernel_process_acct_lock(&guard);
    while (sinks) acct_close(sinks);
}
