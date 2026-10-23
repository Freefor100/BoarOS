#include <kernel/credentials.h>
#include "private.h"
#include "../../fs/files/private.h"

#include <arch/context.h>
#include <kernel/errno.h>
#include <kernel/irq.h>
#include <kernel/files.h>
#include <kernel/open_file.h>
#include <kernel/proc_task.h>
#include <kernel/fs_context.h>
#include <kernel/task.h>
#include <kernel/tty_task.h>
#include <kernel/vfs.h>
#include <string.h>

int kernel_credentials_may_access_locked(const struct kernel_task *caller,const struct kernel_task *target,int fscreds)
{
    if(!target)return 0;
    if(!caller || caller->group_leader==target->group_leader)return 1;
    /* access() 的真实ID快照覆盖整个路径解析；跨proc链接也使用
     * 同一个调用者视图。目标仍读取其发布凭据，不继承访问者override。 */
    const struct kernel_credentials *from=caller==kernel_task_current()?
        kernel_credentials_current():kernel_credentials_task_locked(caller),
        *to=kernel_credentials_task_locked(target);
    if(kernel_credentials_capable(from,KERNEL_CAP_SYS_PTRACE))return 1;
    uint32_t uid=fscreds?from->fsuid:from->uid,gid=fscreds?from->fsgid:from->gid;
    uint64_t caps=fscreds?from->effective:from->permitted;
    return uid==to->uid && uid==to->euid && uid==to->suid &&
        gid==to->gid && gid==to->egid && gid==to->sgid &&
        kernel_exec_security_dumpable(target->exec_security)==1 && !(to->permitted&~caps);
}
struct proc_access {
    struct kernel_task *task;
    const struct kernel_credentials *credentials;
    struct kernel_mm_record *mm;
    struct kernel_exec_security *security;
};
static void proc_access_release(struct proc_access *access)
{ if(access->credentials)kernel_credentials_put(access->credentials); kernel_exec_security_put(access->security); }
static int proc_access_begin(struct kernel_task *task,struct proc_access *access)
{
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    if(!task)return -KERNEL_ENOENT;
    if(!kernel_credentials_may_access_locked(kernel_task_current(),task,1))return -KERNEL_EACCES;
    *access=(struct proc_access){task,kernel_credentials_get(kernel_credentials_task_locked(task)),task->mm.record,
        kernel_exec_security_get(task->exec_security)};
    return 0;
}
static int proc_access_check(struct proc_access *access)
{
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    struct kernel_task *task=access->task;
    return task && task->mm.record==access->mm && task->exec_security==access->security &&
        kernel_credentials_task_locked(task)==access->credentials &&
        kernel_credentials_may_access_locked(kernel_task_current(),task,1)?0:-KERNEL_EACCES;
}
#define PROC_ACCESS(task) \
    struct proc_access access __attribute__((cleanup(proc_access_release)))={0}; \
    int access_error=proc_access_begin(task,&access); \
    if(access_error)return access_error

void kernel_proc_task_update_comm(struct kernel_task *task)
{
    struct kernel_vfs_path *path = 0;
    if (kernel_mm_executable_path_acquire(&task->mm, &path) !=
        KERNEL_MM_STATUS_OK) return;
    if (kernel_vfs_path_copy_name(path, task->comm, sizeof(task->comm))) __builtin_trap();
    if (kernel_vfs_path_release(&path)) __builtin_trap();
}

static int proc_visible(const struct kernel_task *task)
{
    if (!task || !task->arch.user_mode || !task->tid_owned ||
        !task->group_leader || !task->group_leader->tid_owned ||
        task->group_leader->tid <= 0 ||
        task->state == KERNEL_THREAD_STATE_GROUP_DEAD) return 0;
    if (task->state != KERNEL_THREAD_STATE_EXITED) return 1;
    /* 子进程在清理期间仍占有 PID；wait 回收前不能暂时丢失数字目录。 */
    return task == task->group_leader && task->group_members == 1U &&
           task->parent != 0;
}

uint32_t kernel_proc_pid_max(void)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    /* 分配器的 limit 是包含端点的编号；proc 的 pid_max 是排他上界。 */
    return scheduler.pid_allocator.limit + 1U;
}

static int proc_group_visible(struct kernel_task *leader)
{
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        if (!leader->accounted || leader->retirement) return 0;
        if (proc_visible(leader)) return 1;
    }
    /* An EXITED/GROUP_DEAD leader remains visible through a live member.
     * The storage borrow crosses segments; each role/state check stays locked. */
    int visible = 0;
    struct scheduler_group_cursor cursor;
    scheduler_group_cursor_begin(leader, &cursor);
    while (cursor.task) {
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            struct kernel_task *member = cursor.task;
            visible = member->accounted && member->group_leader == leader && proc_visible(member);
        }
        if (visible) break;
        scheduler_group_cursor_next(&cursor);
    }
    scheduler_group_cursor_end(&cursor);
    return visible;
}

static struct kernel_task *find_member(kernel_pid_t pid, uint64_t identity)
{
    struct kernel_task *leader;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        leader = process_find_identity(pid, KERNEL_PID_TGID);
        if (!leader || process_identity_generation(leader) != identity ||
            !leader->accounted || leader->retirement) return 0;
        scheduler_task_get_locked(leader);
    }
    int visible = proc_group_visible(leader);
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        visible = visible && leader->accounted && !leader->retirement &&
                  process_find_identity(pid, KERNEL_PID_TGID) == leader &&
                  process_identity_generation(leader) == identity;
    }
    if (!visible) { scheduler_task_put(leader); return 0; }
    return leader;
}

static struct kernel_task *find_resource_owner(kernel_pid_t pid,
                                                uint64_t identity)
{
    struct kernel_task *leader = find_member(pid, identity);
    int departing;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        departing = leader && (leader->retirement || leader->proc_exiting);
    }
    if (departing) { scheduler_task_put(leader); return 0; }
    return leader;
}


static int proc_thread_visible(const struct kernel_task *task)
{
    /* 组长先退出时仍保有TID，直到整组回收或exec转移身份。 */
    return proc_visible(task) || (task && task == task->group_leader &&
        task->arch.user_mode && task->tid_owned && task->accounted &&
        task->group_members > 1U);
}

static struct kernel_task *find_thread(kernel_pid_t tid, uint64_t identity)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    struct kernel_task *task = process_find_identity(tid, KERNEL_PID_TID);
    struct kernel_pid *id = process_identity(task, KERNEL_PID_TID);
    if (!task || !id || id->generation != identity || !task->accounted ||
        task->retirement || !proc_thread_visible(task)) return 0;
    scheduler_task_get_locked(task);
    return task;
}

int kernel_proc_thread_identity(kernel_pid_t tgid, uint64_t group_identity,
    kernel_pid_t tid, uint64_t *identity)
{
    if (tid <= 0 || !identity || tgid < 0 || (tgid && !group_identity))
        return -KERNEL_EINVAL;
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    struct kernel_task *task = process_find_identity(tid, KERNEL_PID_TID);
    if (!task || !task->accounted || task->retirement || !proc_thread_visible(task) ||
        (tgid && (process_identity_number(task, KERNEL_PID_TGID) != tgid ||
                  process_identity_generation(task) != group_identity)))
        return -KERNEL_ENOENT;
    *identity = process_identity(task, KERNEL_PID_TID)->generation;
    return 0;
}

int kernel_proc_thread_group(kernel_pid_t tid, uint64_t identity,
    kernel_pid_t *tgid, uint64_t *group_identity)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    struct kernel_task *task = process_find_identity(tid, KERNEL_PID_TID);
    struct kernel_pid *id = process_identity(task, KERNEL_PID_TID);
    if (!task || !id || id->generation != identity || !task->accounted ||
        task->retirement || !proc_thread_visible(task)) return -KERNEL_ENOENT;
    *tgid = process_identity_number(task, KERNEL_PID_TGID);
    *group_identity = process_identity_generation(task);
    return 0;
}

int kernel_proc_next_thread(kernel_pid_t tgid, uint64_t group_identity,
    kernel_pid_t after, kernel_pid_t *tid, uint64_t *identity)
{
    if (tgid <= 0 || !group_identity || after < 0 || !tid || !identity)
        return -KERNEL_EINVAL;
    KERNEL_IRQ_SCOPE(proc_irq);
    SCHEDULER_TASK_BORROW(leader, find_member(tgid, group_identity));
    if (!leader) return -KERNEL_ENOENT;
    for (;;) {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        if (!leader->accounted || leader->retirement ||
            process_find_identity(tgid, KERNEL_PID_TGID) != leader ||
            process_identity_generation(leader) != group_identity)
            return -KERNEL_ENOENT;
        struct kernel_pid *id = kernel_pid_next_published(&scheduler.identities, after);
        if (!id) return -KERNEL_ENOENT;
        after = id->number;
        struct kernel_task *task = id->members[KERNEL_PID_TID]
            ? id->members[KERNEL_PID_TID]->task : 0;
        if (task && task->accounted && !task->retirement &&
            task->group_leader == leader && proc_thread_visible(task)) {
            *tid = id->number; *identity = id->generation;
            return 0;
        }
    }
}

static int proc_task_snapshot(struct kernel_task *task, uint64_t identity,
    int thread, struct kernel_proc_process_snapshot *result)
{
    struct kernel_task *leader = task->group_leader;
    uint32_t threads = 0;
    {
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    if (leader->retirement || !leader->accounted) return -KERNEL_ENOENT;
    threads = leader->group_members;
    *result = (struct kernel_proc_process_snapshot){
        .credentials=kernel_credentials_get(kernel_credentials_task_locked(task)),
        .pid = task->tid,
        .tgid = leader->tid,
        .ppid = leader->parent && leader->parent->group_leader
            ? leader->parent->group_leader->tid : 0,
        .process_group = process_identity_number(leader, KERNEL_PID_PGID),
        .session_id = process_identity_number(leader, KERNEL_PID_SID),
        .identity = identity,
        .start_ticks = task->proc_start_ticks,
        .state = task->proc_exiting ||
                 task->state == KERNEL_THREAD_STATE_EXITED ||
                 task->state == KERNEL_THREAD_STATE_GROUP_DEAD ||
                 task->state == KERNEL_THREAD_STATE_ZOMBIE ? 'Z' :
                 task->state == KERNEL_THREAD_STATE_RUNNING ||
                 task->state == KERNEL_THREAD_STATE_READY ? 'R' :
                 task->state == KERNEL_THREAD_STATE_STOPPED ? 'T' : 'S',
        .child_minor_faults = leader->child_usage.minor_faults,
        .child_major_faults = leader->child_usage.major_faults,
        .signal_pending = task->signal_pending,
        .signal_shared_pending = leader->group_pending,
        .signal_blocked = task->signal_blocked,
        .signal_queued=kernel_signal_pending_count_locked(task),
        .signal_queue_limit=leader->sigpending_limit.current,
        .threads = threads,
        .no_new_privs=task->no_new_privs,
        .scheduling_priority = task->scheduling.priority
            ? -(task->scheduling.priority + 1) : 20,
        .rt_priority = (uint32_t)task->scheduling.priority,
        .scheduling_policy = (uint32_t)task->scheduling.policy,
    };
    result->wait_channel_flag = (thread || threads < 2U) &&
        task->state != KERNEL_THREAD_STATE_RUNNING &&
        task->state != KERNEL_THREAD_STATE_READY;
    memcpy(result->comm, task->comm, sizeof(result->comm));
    /* 远端可改写信号动作；查询与修改共用wait域，与其余信号字段一起捕获。 */
    const struct kernel_signal_table *actions =
        (const void *)(uintptr_t)task->signal_table_address;
    if (actions) {
        if (actions->magic != KERNEL_SIGNAL_TABLE_MAGIC) __builtin_trap();
        for (unsigned sig = 0; sig < 64U; sig++) {
            if (actions->actions[sig].handler == KERNEL_SIGNAL_IGN)
                result->signal_ignored |= UINT64_C(1) << sig;
            else if (actions->actions[sig].handler != KERNEL_SIGNAL_DFL)
                result->signal_caught |= UINT64_C(1) << sig;
        }
    }
    }
    result->tty_nr = kernel_tty_rdev(kernel_task_controlling_tty(leader));
    result->tty_pgrp = kernel_tty_foreground(kernel_task_controlling_tty(leader));
    struct kernel_task_usage usage = {0};
    scheduler_usage_snapshot(task, thread, &usage);
    result->minor_faults = usage.minor_faults;
    result->major_faults = usage.major_faults;
    result->user_ticks = usage.user_ns / 10000000U;
    result->kernel_ticks = usage.kernel_ns / 10000000U;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        result->child_user_ticks = leader->child_usage.user_ns / 10000000U;
        result->child_kernel_ticks = leader->child_usage.kernel_ns / 10000000U;
    }
    if (!task->proc_exiting && task->mm.state == KERNEL_MM_LIVE) {
        struct kernel_mm_proc_memory memory;
        if (kernel_mm_proc_memory_snapshot(&task->mm, &memory) !=
            KERNEL_MM_STATUS_OK) {
                kernel_credentials_put(result->credentials);result->credentials=0;
                return -KERNEL_EIO;
        }
        result->virtual_bytes = memory.virtual_bytes;
        result->resident_pages = memory.resident_pages;
        result->start_code = memory.start_code;
        result->end_code = memory.end_code;
        result->start_stack = memory.start_stack;
    }
    return 0;
}

int kernel_proc_process_snapshot(kernel_pid_t pid, uint64_t identity,
    struct kernel_proc_process_snapshot *result)
{
    if (pid <= 0 || !identity || !result) return -KERNEL_EINVAL;
    KERNEL_IRQ_SCOPE(proc_irq);
    SCHEDULER_TASK_BORROW(task, find_member(pid, identity));
    return task ? proc_task_snapshot(task, identity, 0, result) : -KERNEL_ENOENT;
}

int kernel_proc_thread_snapshot(kernel_pid_t tid, uint64_t identity,
    struct kernel_proc_process_snapshot *result)
{
    if (tid <= 0 || !identity || !result) return -KERNEL_EINVAL;
    KERNEL_IRQ_SCOPE(proc_irq);
    SCHEDULER_TASK_BORROW(task, find_thread(tid, identity));
    return task ? proc_task_snapshot(task, identity, 1, result) : -KERNEL_ESRCH;
}

int kernel_proc_same_group(kernel_pid_t pid,uint64_t identity,int thread)
{
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    struct kernel_task *caller=kernel_task_current();
    struct kernel_task *task=process_find_identity(pid,thread?KERNEL_PID_TID:KERNEL_PID_TGID);
    struct kernel_pid *id=task?process_identity(task,thread?KERNEL_PID_TID:KERNEL_PID_TGID):0;
    return id && id->generation==identity && caller && caller->group_leader==task->group_leader;
}

int kernel_proc_inode_owner(kernel_pid_t pid,uint64_t identity,int thread,uint32_t mode,uint32_t *uid,uint32_t *gid)
{
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    struct kernel_task *task=process_find_identity(pid,thread?KERNEL_PID_TID:KERNEL_PID_TGID);
    struct kernel_pid *id=task?process_identity(task,thread?KERNEL_PID_TID:KERNEL_PID_TGID):0;
    if(!id || id->generation!=identity || !task->accounted || task->retirement)return -KERNEL_ENOENT;
    const struct kernel_credentials *cred=kernel_credentials_task_locked(task);
    int dumpable=mode==(KERNEL_VFS_S_IFDIR|0555U) || kernel_exec_security_dumpable(task->exec_security)==1;
    *uid=dumpable?cred->euid:0;*gid=dumpable?cred->egid:0;return 0;
}

int kernel_proc_process_path_acquire(kernel_pid_t pid, uint64_t identity,
                                     enum kernel_proc_path_kind kind, int fd,
                                     struct kernel_vfs_path **owner)
{
    (void)fd;
    if (pid <= 0 || !identity || !owner || *owner)
        return -KERNEL_EINVAL;
    KERNEL_IRQ_SCOPE(proc_irq);
    SCHEDULER_TASK_BORROW(task, find_resource_owner(pid, identity));
    PROC_ACCESS(task);
    if (!task) {
        return -KERNEL_ENOENT;
    }
    int result = -KERNEL_ENOENT;
    if (kind == KERNEL_PROC_PATH_EXE) {
        if (task->mm.state == KERNEL_MM_LIVE &&
            kernel_mm_executable_path_acquire(&task->mm, owner) ==
                KERNEL_MM_STATUS_OK) result = 0;
    } else if (kind == KERNEL_PROC_PATH_CWD ||
               kind == KERNEL_PROC_PATH_ROOT) {
        KERNEL_FS_PATHS(paths, &task->fs);
        if (paths.root) {
            struct kernel_vfs_path *path = kind == KERNEL_PROC_PATH_CWD
                ? paths.cwd : paths.root;
            if (path && !kernel_vfs_path_acquire(path)) {
                *owner = path;
                result = 0;
            }
        }
    }
    if(!result && proc_access_check(&access)) {
        if(*owner && kernel_vfs_path_release(owner))__builtin_trap();
        return -KERNEL_EACCES;
    }
    return result;
}

int kernel_proc_next_fd(kernel_pid_t pid, uint64_t identity,
                         int after, int *fd)
{
    if (pid <= 0 || !identity || !fd || after < -1)
        return -KERNEL_EINVAL;
    KERNEL_IRQ_SCOPE(proc_irq);
    SCHEDULER_TASK_BORROW(task, find_resource_owner(pid, identity));
    PROC_ACCESS(task);
    int result = task && kernel_files_is_live(&task->files)
        ? kernel_files_next_open_fd(&task->files, after, fd)
        : -KERNEL_ENOENT;
    return result?result:proc_access_check(&access);
}

int kernel_proc_fd_access_snapshot(kernel_pid_t pid, uint64_t identity,
                                   int fd, int *readable, int *writable)
{
    if (pid <= 0 || !identity || fd < 0 || !readable || !writable)
        return -KERNEL_EINVAL;
    KERNEL_IRQ_SCOPE(proc_irq);
    SCHEDULER_TASK_BORROW(task, find_resource_owner(pid, identity));
    PROC_ACCESS(task);
    KERNEL_FILES_PIN_SCOPE(fd_guard);
    struct kernel_open_file_description *file = task &&
        kernel_files_is_live(&task->files)
        ? kernel_files_hold_fd(&task->files, fd, &fd_guard) : 0;
    if(proc_access_check(&access))return -KERNEL_EACCES;
    if (file) {
        if (kernel_open_file_kind(file) == KERNEL_OPEN_FILE_KIND_EPOLL) {
            *readable = 1;
            *writable = 1;
        } else {
            *readable = kernel_open_file_readable(file);
            *writable = kernel_open_file_writable(file);
        }
    }
    return file ? 0 : -KERNEL_ENOENT;
}

int kernel_proc_fd_path_acquire(kernel_pid_t pid, uint64_t identity,
                                int fd, struct kernel_vfs_path **owner)
{
    if (pid <= 0 || !identity || fd < 0 || !owner || *owner)
        return -KERNEL_EINVAL;
    KERNEL_IRQ_SCOPE(proc_irq);
    SCHEDULER_TASK_BORROW(task, find_resource_owner(pid, identity));
    PROC_ACCESS(task);
    KERNEL_FILES_PIN_SCOPE(fd_guard);
    struct kernel_open_file_description *file = task &&
        kernel_files_is_live(&task->files)
        ? kernel_files_hold_fd(&task->files, fd, &fd_guard) : 0;
    if(proc_access_check(&access))return -KERNEL_EACCES;
    struct kernel_vfs_path *path = file ? kernel_open_file_path(file) : 0;
    int result = -KERNEL_ENOENT;
    if (path && !kernel_vfs_path_acquire(path)) {
        *owner = path;
        result = 0;
    }
    return result;
}

int kernel_proc_fd_pseudo_snapshot(kernel_pid_t pid, uint64_t identity,
                                   int fd,
                                   struct kernel_proc_fd_pseudo *snapshot)
{
    if (pid <= 0 || !identity || fd < 0 || !snapshot)
        return -KERNEL_EINVAL;
    KERNEL_IRQ_SCOPE(proc_irq);
    SCHEDULER_TASK_BORROW(task, find_resource_owner(pid, identity));
    PROC_ACCESS(task);
    KERNEL_FILES_PIN_SCOPE(fd_guard);
    struct kernel_open_file_description *file = task &&
        kernel_files_is_live(&task->files)
        ? kernel_files_hold_fd(&task->files, fd, &fd_guard) : 0;
    if(proc_access_check(&access))return -KERNEL_EACCES;
    int result = file ? -KERNEL_ENOTSUP : -KERNEL_ENOENT;
    if (file && !kernel_open_file_path(file)) {
        snapshot->kind = (uint8_t)kernel_open_file_kind(file);
        snapshot->object_identity = kernel_open_file_pseudo_identity(file);
        result = 0;
    }
    return result;
}

int kernel_proc_fd_pseudo_stat(kernel_pid_t pid, uint64_t identity,
                               int fd, struct kernel_vfs_stat *stat)
{
    if (pid <= 0 || !identity || fd < 0 || !stat)
        return -KERNEL_EINVAL;
    KERNEL_IRQ_SCOPE(proc_irq);
    SCHEDULER_TASK_BORROW(task, find_resource_owner(pid, identity));
    PROC_ACCESS(task);
    KERNEL_FILES_PIN_SCOPE(fd_guard);
    struct kernel_open_file_description *file = task &&
        kernel_files_is_live(&task->files)
        ? kernel_files_hold_fd(&task->files, fd, &fd_guard) : 0;
    if(proc_access_check(&access))return -KERNEL_EACCES;
    /* 伪对象的元数据快照不分配也不等待；解锁后不再访问 OFD。 */
    int result = file ? kernel_open_file_pseudo_stat(file, stat)
                      : -KERNEL_ENOENT;
    return result;
}

int kernel_proc_fd_reopen_link(kernel_pid_t pid, uint64_t identity,
                               int fd, struct kernel_heap *heap,
                               uint32_t flags,
                               struct kernel_open_file_description **owner)
{
    if (pid <= 0 || !identity || fd < 0 || !heap || !owner || *owner)
        return -KERNEL_EINVAL;
    struct kernel_open_file_pipe_pin pin = {0};
    KERNEL_IRQ_SCOPE(proc_irq);
    SCHEDULER_TASK_BORROW(task, find_resource_owner(pid, identity));
    PROC_ACCESS(task);
    KERNEL_FILES_PIN_SCOPE(fd_guard);
    struct kernel_open_file_description *file = task &&
        kernel_files_is_live(&task->files)
        ? kernel_files_hold_fd(&task->files, fd, &fd_guard) : 0;
    if(proc_access_check(&access))return -KERNEL_EACCES;
    int result = file ? -KERNEL_ENXIO : -KERNEL_ENOENT;
    /* 解锁后 fd 可关闭；先钉住 pipe endpoint 再分配新 OFD。 */
    if (file && kernel_open_file_path(file)) result = -KERNEL_ENOTSUP;
    else if (file &&
             kernel_open_file_kind(file) == KERNEL_OPEN_FILE_KIND_PIPE)
        result = kernel_open_file_pipe_pin(file, flags, &pin);
    if (result || !pin.pipe) return result;
    return kernel_open_file_pipe_finish(heap, &pin, flags, owner);
}

static int proc_process_visible(struct kernel_task *task, struct kernel_task *root,
                                kernel_pid_t pid, uint64_t identity)
{
    if (!scheduler_task_descends_from(task, root, 0) || !proc_group_visible(task)) return 0;
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    /* 存储借用只保证对象未释放；祖先遍历期间exec可转移同一TGID。 */
    return task->accounted && !task->retirement &&
        process_find_identity(pid, KERNEL_PID_TGID) == task &&
        process_identity_generation(task) == identity;
}

int kernel_proc_next_process(kernel_pid_t after, kernel_pid_t *pid,
                             uint64_t *identity)
{
    if (!pid || !identity || after < 0) return -KERNEL_EINVAL;
    KERNEL_IRQ_SCOPE(proc_irq);
    SCHEDULER_TASK_BORROW(root, 0);
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        root = scheduler.init_task;
        if (root) scheduler_task_get_locked(root);
    }
    if (!root) return -KERNEL_ENOENT;
    for (;;) {
        SCHEDULER_TASK_BORROW(task, 0);
        uint64_t token;
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            struct kernel_pid *id = kernel_pid_next_published(&scheduler.identities, after);
            if (!id) return -KERNEL_ENOENT;
            after = id->number;
            token = id->generation;
            task = id->members[KERNEL_PID_TGID] ? id->members[KERNEL_PID_TGID]->task : 0;
            if (task && task->accounted && !task->retirement) scheduler_task_get_locked(task);
            else task = 0;
        }
        if (task && proc_process_visible(task, root, after, token)) {
            *pid = after; *identity = token;
            return 0;
        }
    }
}

int kernel_proc_process_identity(kernel_pid_t pid, uint64_t *identity)
{
    if (pid <= 0 || !identity) return -KERNEL_EINVAL;
    KERNEL_IRQ_SCOPE(proc_irq);
    SCHEDULER_TASK_BORROW(task, 0);
    SCHEDULER_TASK_BORROW(root, 0);
    uint64_t token;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        task = process_find_identity(pid, KERNEL_PID_TGID);
        root = scheduler.init_task;
        if (!task || !task->accounted || task->retirement || !root) {
            task = root = 0;
            return -KERNEL_ENOENT;
        }
        token = process_identity_generation(task);
        scheduler_task_get_locked(task);
        scheduler_task_get_locked(root);
    }
    if (!proc_process_visible(task, root, pid, token)) return -KERNEL_ENOENT;
    *identity = token;
    return 0;
}
