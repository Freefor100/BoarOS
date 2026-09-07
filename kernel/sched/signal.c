#include "private.h"

#include <arch/riscv/direct_map.h>
#include <arch/riscv/fpu.h>
#include <arch/riscv/trap.h>
#include <arch/riscv/user_elf.h>
#include <kernel/errno.h>
#include <kernel/physical_page.h>
#include <kernel/page.h>
#include <kernel/scheduler.h>
#include <kernel/signal.h>
#include <kernel/task.h>
#include <kernel/uaccess.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define LINUX_SIGINFO_SIZE 128U
#define LINUX_SIGINFO_PID_OFFSET 16U
#define LINUX_UCONTEXT_SIZE 688U
#define LINUX_UCONTEXT_SIGMASK_OFFSET 40U
#define LINUX_UCONTEXT_MCONTEXT_OFFSET 168U
#define LINUX_SIGCONTEXT_FP_OFFSET 256U
#define LINUX_SS_DISABLE UINT64_C(2)
#define LINUX_SIGFRAME_SIZE (LINUX_SIGINFO_SIZE + LINUX_UCONTEXT_SIZE)
#define LINUX_WAIT_STOPPED 0x7fU
#define LINUX_SI_USER UINT32_C(0)

#define SIGNAL_KILL 9U
#define SIGNAL_CONTINUE 18U
#define SIGNAL_STOP 19U

/* ptrace register order: index 0 is pc, then ra, sp, gp, tp, t0-t2,
 * s0-s1, a0-a7, s2-s11, t3-t6. */
static const uint32_t signal_gpr_offsets[32] = {
    RISCV_TRAP_FRAME_SEPC,
    RISCV_TRAP_FRAME_RA,
    RISCV_TRAP_FRAME_SP,
    RISCV_TRAP_FRAME_GP,
    RISCV_TRAP_FRAME_TP,
    RISCV_TRAP_FRAME_T0,
    RISCV_TRAP_FRAME_T1,
    RISCV_TRAP_FRAME_T2,
    RISCV_TRAP_FRAME_S0,
    RISCV_TRAP_FRAME_S1,
    RISCV_TRAP_FRAME_A0,
    RISCV_TRAP_FRAME_A1,
    RISCV_TRAP_FRAME_A2,
    RISCV_TRAP_FRAME_A3,
    RISCV_TRAP_FRAME_A4,
    RISCV_TRAP_FRAME_A5,
    RISCV_TRAP_FRAME_A6,
    RISCV_TRAP_FRAME_A7,
    RISCV_TRAP_FRAME_S2,
    RISCV_TRAP_FRAME_S3,
    RISCV_TRAP_FRAME_S4,
    RISCV_TRAP_FRAME_S5,
    RISCV_TRAP_FRAME_S6,
    RISCV_TRAP_FRAME_S7,
    RISCV_TRAP_FRAME_S8,
    RISCV_TRAP_FRAME_S9,
    RISCV_TRAP_FRAME_S10,
    RISCV_TRAP_FRAME_S11,
    RISCV_TRAP_FRAME_T3,
    RISCV_TRAP_FRAME_T4,
    RISCV_TRAP_FRAME_T5,
    RISCV_TRAP_FRAME_T6,
};

#define SIGNAL_MASK_KILL_STOP KERNEL_SIGNAL_UNBLOCKABLE_MASK

static uint64_t signal_mask(uint32_t sig)
{
    return UINT64_C(1) << (sig - 1U);
}

static uint32_t signal_first_set(uint64_t bits)
{
    uint32_t sig = 1U;

    while ((bits & UINT64_C(1)) == 0U) {
        bits >>= 1U;
        sig++;
    }
    return sig;
}

void kernel_signal_note_syscall_restart(struct kernel_task *task)
{
    if (task != 0 && task != &scheduler.idle &&
        task->magic == KERNEL_THREAD_MAGIC && task->arch.user_mode == 1U) {
        if (task->syscall_restart_kind == KERNEL_SYSCALL_RESTART_NONE) {
            task->syscall_restart_kind = KERNEL_SYSCALL_RESTART_GENERIC;
            task->syscall_restart_deadline = 0U;
            task->syscall_restart_remaining_address = 0U;
        }
    }
}

void kernel_signal_note_nanosleep_restart(struct kernel_task *task,
                                          uint64_t deadline,
                                          uint64_t remaining_address)
{
    if (task != 0 && task != &scheduler.idle &&
        task->magic == KERNEL_THREAD_MAGIC && task->arch.user_mode == 1U) {
        task->syscall_restart_kind = KERNEL_SYSCALL_RESTART_NANOSLEEP;
        task->syscall_restart_deadline = deadline;
        task->syscall_restart_remaining_address = remaining_address;
    }
}

enum kernel_signal_default_action {
    KERNEL_SIGNAL_DEFAULT_TERMINATE = 0,
    KERNEL_SIGNAL_DEFAULT_IGNORE,
    KERNEL_SIGNAL_DEFAULT_STOP,
    KERNEL_SIGNAL_DEFAULT_CONTINUE,
    KERNEL_SIGNAL_DEFAULT_CORE,
};

static enum kernel_signal_default_action signal_default_action(
    uint32_t sig)
{
    switch (sig) {
    case 17U: /* SIGCHLD */
    case 23U: /* SIGURG */
    case 28U: /* SIGWINCH */
        return KERNEL_SIGNAL_DEFAULT_IGNORE;
    case SIGNAL_CONTINUE:
        return KERNEL_SIGNAL_DEFAULT_CONTINUE;
    case 20U: /* SIGTSTP */
    case 21U: /* SIGTTIN */
    case 22U: /* SIGTTOU */
    case SIGNAL_STOP:
        return KERNEL_SIGNAL_DEFAULT_STOP;
    case 3U:  /* SIGQUIT */
    case 4U:  /* SIGILL */
    case 5U:  /* SIGTRAP */
    case 6U:  /* SIGABRT */
    case 7U:  /* SIGBUS */
    case 8U:  /* SIGFPE */
    case 11U: /* SIGSEGV */
    case 24U: /* SIGXCPU */
    case 25U: /* SIGXFSZ */
    case 31U: /* SIGSYS */
        return KERNEL_SIGNAL_DEFAULT_CORE;
    default:
        return KERNEL_SIGNAL_DEFAULT_TERMINATE;
    }
}

static int signal_dispatchable(const struct kernel_task *task)
{
    return task != 0 && task != &scheduler.idle &&
           task->magic == KERNEL_THREAD_MAGIC &&
           task->arch.user_mode == 1U &&
           task->state != KERNEL_THREAD_STATE_ZOMBIE &&
           task->state != KERNEL_THREAD_STATE_EXITED;
}

static struct kernel_signal_table *signal_table_of(
    const struct kernel_task *task)
{
    struct kernel_signal_table *table =
        (struct kernel_signal_table *)task->signal_table_address;

    if (table == 0 || table->magic != KERNEL_SIGNAL_TABLE_MAGIC) {
        return 0;
    }
    return table;
}

static const struct kernel_signal_action *signal_action_of(
    const struct kernel_task *task,
    uint32_t sig)
{
    const struct kernel_signal_table *table = signal_table_of(task);

    if (table == 0) {
        return 0;
    }
    return &table->actions[sig - 1U];
}

static enum kernel_signal_status signal_alloc_table(
    struct kernel_task *task)
{
    struct kernel_signal_table *table;
    uint64_t physical_address;
    void *page;
    enum physical_page_status page_status;

    if (task->signal_table_address != 0U) {
        return KERNEL_SIGNAL_STATUS_OK;
    }
    page_status = physical_page_allocate(scheduler.allocator,
                                         &physical_address);
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        return page_status == PHYSICAL_PAGE_STATUS_EMPTY
                   ? KERNEL_SIGNAL_STATUS_NO_MEMORY
                   : KERNEL_SIGNAL_STATUS_INVALID_STATE;
    }
    page_status = physical_page_resolve(scheduler.allocator,
                                        physical_address,
                                        &page);
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        (void)physical_page_release(scheduler.allocator,
                                    physical_address);
        return KERNEL_SIGNAL_STATUS_INVALID_STATE;
    }
    clear_page(page);
    table = page;
    table->magic = KERNEL_SIGNAL_TABLE_MAGIC;
    task->signal_table_address = (uintptr_t)page;
    return KERNEL_SIGNAL_STATUS_OK;
}

enum kernel_signal_status kernel_signal_release_table(
    struct kernel_task *task)
{
    uint64_t physical_address;
    void *page;

    if (task == 0 || task->signal_table_address == 0U) {
        return KERNEL_SIGNAL_STATUS_OK;
    }
    page = (void *)task->signal_table_address;
    if (riscv_direct_map_va_to_pa((uintptr_t)page,
                                  BOAROS_PAGE_SIZE,
                                  &physical_address) !=
        RISCV_DIRECT_MAP_STATUS_OK) {
        return KERNEL_SIGNAL_STATUS_INVALID_STATE;
    }
    if (physical_page_release(scheduler.allocator, physical_address) !=
        PHYSICAL_PAGE_STATUS_OK) {
        return KERNEL_SIGNAL_STATUS_INVALID_STATE;
    }
    task->signal_table_address = 0U;
    return KERNEL_SIGNAL_STATUS_OK;
}

enum kernel_signal_status kernel_signal_get_action(
    struct kernel_task *task,
    uint32_t sig,
    struct kernel_linux_sigaction *action)
{
    const struct kernel_signal_action *entry;

    if (task == 0 || action == 0 || !signal_dispatchable(task) ||
        sig == 0U || sig > KERNEL_SIGNAL_COUNT) {
        return KERNEL_SIGNAL_STATUS_INVALID_ARGUMENT;
    }
    entry = signal_action_of(task, sig);
    action->handler = entry == 0 ? KERNEL_SIGNAL_DFL : entry->handler;
    action->flags = entry == 0 ? 0U : entry->flags;
    action->mask = entry == 0 ? 0U : entry->mask;
    return KERNEL_SIGNAL_STATUS_OK;
}

enum kernel_signal_status kernel_signal_set_action(
    struct kernel_task *task,
    uint32_t sig,
    const struct kernel_linux_sigaction *action)
{
    struct kernel_signal_table *table;
    struct kernel_signal_action *entry;
    enum kernel_signal_status status;

    if (task == 0 || action == 0 || !signal_dispatchable(task) ||
        sig == 0U || sig > KERNEL_SIGNAL_COUNT ||
        sig == SIGNAL_KILL || sig == SIGNAL_STOP) {
        return KERNEL_SIGNAL_STATUS_INVALID_ARGUMENT;
    }
    status = signal_alloc_table(task);
    if (status != KERNEL_SIGNAL_STATUS_OK) {
        return status;
    }
    table = signal_table_of(task);
    if (table == 0) {
        return KERNEL_SIGNAL_STATUS_INVALID_STATE;
    }
    entry = &table->actions[sig - 1U];
    entry->handler = action->handler;
    entry->flags = action->flags;
    entry->mask = action->mask & ~SIGNAL_MASK_KILL_STOP;
    return KERNEL_SIGNAL_STATUS_OK;
}

enum kernel_signal_status kernel_signal_fork(
    struct kernel_task *child,
    const struct kernel_task *parent)
{
    const struct kernel_signal_table *parent_table;
    struct kernel_signal_table *child_table;
    uint64_t physical_address;
    void *page;
    enum physical_page_status page_status;

    if (child == 0 || parent == 0 || parent->magic != KERNEL_THREAD_MAGIC ||
        parent->arch.user_mode != 1U || child->magic != KERNEL_THREAD_MAGIC ||
        child->arch.user_mode != 1U) {
        return KERNEL_SIGNAL_STATUS_INVALID_ARGUMENT;
    }
    child->signal_pending = 0U;
    child->signal_blocked = parent->signal_blocked;
    child->signal_table_address = 0U;
    memset(child->signal_sender, 0, sizeof(child->signal_sender));
    child->stop_notified = 0U;
    child->continue_notified = 0U;
    parent_table = signal_table_of(parent);
    if (parent_table == 0) {
        return KERNEL_SIGNAL_STATUS_OK;
    }
    page_status = physical_page_allocate(scheduler.allocator,
                                         &physical_address);
    if (page_status == PHYSICAL_PAGE_STATUS_EMPTY) {
        return KERNEL_SIGNAL_STATUS_NO_MEMORY;
    }
    if (page_status != PHYSICAL_PAGE_STATUS_OK ||
        physical_page_resolve(scheduler.allocator,
                              physical_address,
                              &page) != PHYSICAL_PAGE_STATUS_OK) {
        if (page_status == PHYSICAL_PAGE_STATUS_OK) {
            (void)physical_page_release(scheduler.allocator,
                                        physical_address);
        }
        return KERNEL_SIGNAL_STATUS_INVALID_STATE;
    }
    clear_page(page);
    child_table = page;
    memcpy(child_table, parent_table, sizeof(*child_table));
    child->signal_table_address = (uintptr_t)child_table;
    return KERNEL_SIGNAL_STATUS_OK;
}

enum kernel_signal_status kernel_signal_get_blocked(
    const struct kernel_task *task,
    uint64_t *blocked)
{
    if (task == 0 || blocked == 0 || !signal_dispatchable(task)) {
        return KERNEL_SIGNAL_STATUS_INVALID_ARGUMENT;
    }
    *blocked = task->signal_blocked;
    return KERNEL_SIGNAL_STATUS_OK;
}

enum kernel_signal_status kernel_signal_update_blocked(
    struct kernel_task *task,
    uint32_t how,
    const uint64_t *new_mask,
    uint64_t *old_mask)
{
    uint64_t mask = 0U;

    if (task == 0 || !signal_dispatchable(task) ||
        how > LINUX_SIG_SETMASK) {
        return KERNEL_SIGNAL_STATUS_INVALID_ARGUMENT;
    }
    if (old_mask != 0) {
        *old_mask = task->signal_blocked;
    }
    if (new_mask == 0) {
        return KERNEL_SIGNAL_STATUS_OK;
    }
    mask = *new_mask & ~SIGNAL_MASK_KILL_STOP;
    if (how == LINUX_SIG_BLOCK) {
        task->signal_blocked |= mask;
    } else if (how == LINUX_SIG_UNBLOCK) {
        task->signal_blocked &= ~mask;
    } else {
        task->signal_blocked = mask;
    }
    return KERNEL_SIGNAL_STATUS_OK;
}

enum kernel_signal_status kernel_signal_get_pending(
    const struct kernel_task *task,
    uint64_t *pending)
{
    if (task == 0 || pending == 0 || !signal_dispatchable(task)) {
        return KERNEL_SIGNAL_STATUS_INVALID_ARGUMENT;
    }
    *pending = task->signal_pending;
    return KERNEL_SIGNAL_STATUS_OK;
}

void kernel_signal_reset_on_exec(struct kernel_task *task)
{
    struct kernel_signal_table *table;
    uint32_t index;

    if (task == 0 || !signal_dispatchable(task)) {
        return;
    }
    table = signal_table_of(task);
    if (table != 0) {
        for (index = 1U; index <= KERNEL_SIGNAL_COUNT; index++) {
            struct kernel_signal_action *entry =
                &table->actions[index - 1U];

            if (entry->handler == KERNEL_SIGNAL_IGN) {
                task->signal_pending &= ~signal_mask(index);
            } else if (entry->handler != KERNEL_SIGNAL_DFL) {
                entry->handler = KERNEL_SIGNAL_DFL;
                entry->flags = 0U;
                entry->mask = 0U;
            }
        }
    } else {
        task->signal_pending &=
            ~(signal_mask(17U) | signal_mask(23U) | signal_mask(28U));
        return;
    }
    task->signal_pending &=
        ~(signal_mask(17U) | signal_mask(23U) | signal_mask(28U));
}

int kernel_signal_wants_sigchld(const struct kernel_task *parent)
{
    const struct kernel_signal_action *entry;

    if (parent == 0) {
        return 0;
    }
    entry = signal_action_of(parent, 17U);
    if (entry == 0 || entry->handler == KERNEL_SIGNAL_DFL ||
        entry->handler == KERNEL_SIGNAL_IGN ||
        (entry->flags & LINUX_SA_NOCLDWAIT) != 0U) {
        return 0;
    }
    return 1;
}

static int signal_wants_sigchld_stop(const struct kernel_task *parent)
{
    const struct kernel_signal_action *entry;

    if (parent == 0) {
        return 0;
    }
    entry = signal_action_of(parent, 17U);
    return entry != 0 && entry->handler != KERNEL_SIGNAL_DFL &&
           entry->handler != KERNEL_SIGNAL_IGN &&
           (entry->flags & (LINUX_SA_NOCLDSTOP | LINUX_SA_NOCLDWAIT)) == 0U;
}

void kernel_signal_notify_child_exit(struct kernel_task *child)
{
    struct kernel_task *parent;

    if (child == 0) {
        return;
    }
    parent = child->parent;
    if (parent != 0 && kernel_signal_wants_sigchld(parent)) {
        (void)kernel_signal_send(parent, 17U, child->tid);
    }
    wake_waiting_parent(child);
}

void kernel_signal_notify_child_stop(struct kernel_task *child,
                                     int continued)
{
    struct kernel_task *parent;

    if (child == 0) {
        return;
    }
    parent = child->parent;
    if (parent != 0 && signal_wants_sigchld_stop(parent)) {
        (void)kernel_signal_send(parent, 17U, child->tid);
    }
    (void)continued;
    wake_waiting_parent(child);
}

static void signal_resume_stopped(struct kernel_task *task, int continued)
{
    stopped_unlink(task);
    task->state = KERNEL_THREAD_STATE_READY;
    task->stop_notified = 0U;
    if (continued) {
        task->continue_notified = 1U;
        kernel_signal_notify_child_stop(task, 1);
    }
    ready_append(task);
}

enum kernel_signal_status kernel_signal_send(struct kernel_task *target,
                                             uint32_t sig,
                                             kernel_pid_t sender_tid)
{
    uint64_t bit;

    if (!signal_dispatchable(target) || sig == 0U ||
        sig > KERNEL_SIGNAL_COUNT) {
        return KERNEL_SIGNAL_STATUS_INVALID_ARGUMENT;
    }
    bit = signal_mask(sig);
    if (sig == SIGNAL_CONTINUE) {
        /* SIGCONT discards pending stop signals and resumes the task. */
        target->signal_pending &=
            ~(signal_mask(20U) | signal_mask(21U) | signal_mask(22U));
        if (target->state == KERNEL_THREAD_STATE_STOPPED) {
            signal_resume_stopped(target, 1);
        }
        {
            const struct kernel_signal_action *entry =
                signal_action_of(target, sig);

            if (entry != 0 && entry->handler != KERNEL_SIGNAL_DFL &&
                entry->handler != KERNEL_SIGNAL_IGN &&
                (target->signal_pending & bit) == 0U) {
                target->signal_pending |= bit;
                target->signal_sender[sig - 1U] = (uint32_t)sender_tid;
                if ((target->signal_blocked & bit) == 0U &&
                    kernel_scheduler_wake_signal(target) !=
                        KERNEL_SCHEDULER_STATUS_OK) {
                    return KERNEL_SIGNAL_STATUS_INVALID_STATE;
                }
            }
        }
        return KERNEL_SIGNAL_STATUS_OK;
    }
    if (target->signal_pending & bit) {
        /* Standard signals merge; the first sender's identity stays. */
        return KERNEL_SIGNAL_STATUS_OK;
    }
    {
        const struct kernel_signal_action *entry = signal_action_of(target,
                                                                     sig);

        if ((entry == 0 || entry->handler == KERNEL_SIGNAL_DFL) &&
            signal_default_action(sig) == KERNEL_SIGNAL_DEFAULT_IGNORE) {
            return KERNEL_SIGNAL_STATUS_OK;
        }
        if (entry != 0 &&
            (entry->handler == KERNEL_SIGNAL_IGN ||
             (sig == 17U && (entry->flags & LINUX_SA_NOCLDWAIT) != 0U))) {
            return KERNEL_SIGNAL_STATUS_OK;
        }
    }
    target->signal_pending |= bit;
    target->signal_sender[sig - 1U] = (uint32_t)sender_tid;
    if (target->state == KERNEL_THREAD_STATE_STOPPED &&
        (sig == SIGNAL_KILL || sig == SIGNAL_CONTINUE)) {
        /* A resumed task applies the signal at its next delivery; this
         * keeps the exit machinery on the current-task path. */
        signal_resume_stopped(target, 0);
    }
    if ((target->signal_blocked & bit) == 0U) {
        if (kernel_scheduler_wake_signal(target) !=
            KERNEL_SCHEDULER_STATUS_OK) {
            return KERNEL_SIGNAL_STATUS_INVALID_STATE;
        }
    }
    return KERNEL_SIGNAL_STATUS_OK;
}

static int signal_is_leader(const struct kernel_task *task)
{
    return signal_dispatchable(task) && task->tid_owned == 1U;
}

struct kernel_task *kernel_signal_find_by_tid(kernel_pid_t tid)
{
    struct kernel_task *task;

    if (signal_dispatchable(scheduler.current) &&
        scheduler.current->tid == tid) {
        return scheduler.current;
    }
    for (task = scheduler.ready_head; task != 0; task = task->next) {
        if (task->tid == tid && signal_dispatchable(task)) {
            return task;
        }
    }
    for (task = scheduler.blocked_head; task != 0; task = task->next) {
        if (task->tid == tid && signal_dispatchable(task)) {
            return task;
        }
    }
    for (task = scheduler.stopped_head; task != 0; task = task->next) {
        if (task->tid == tid && signal_dispatchable(task)) {
            return task;
        }
    }
    return 0;
}

static int signal_matches_target(const struct kernel_task *task,
                                 struct kernel_task *caller,
                                 int64_t pid)
{
    if (!signal_is_leader(task)) {
        return 0;
    }
    if (pid > 0) {
        return task->tid == (kernel_pid_t)pid;
    }
    if (pid == 0) {
        return task->process_group == caller->process_group;
    }
    if (pid == -1) {
        return task != caller && task->tid != 1;
    }
    if (pid == INT32_MIN) {
        return 0;
    }
    return task->process_group == (kernel_pid_t)(-pid);
}

uint32_t kernel_signal_resolve_targets(struct kernel_task *caller,
                                       int64_t pid)
{
    struct kernel_task *task;
    uint32_t found = 0U;

    if (pid > 0) {
        task = kernel_signal_find_by_tid((kernel_pid_t)pid);
        return task != 0 && signal_is_leader(task) ? 1U : 0U;
    }
    if (caller != 0 && caller != &scheduler.idle &&
        signal_matches_target(caller, caller, pid)) {
        found++;
    }
    for (task = scheduler.ready_head; task != 0; task = task->next) {
        if (task != caller && signal_matches_target(task, caller, pid)) {
            found++;
        }
    }
    for (task = scheduler.blocked_head; task != 0; task = task->next) {
        if (task != caller && signal_matches_target(task, caller, pid)) {
            found++;
        }
    }
    for (task = scheduler.stopped_head; task != 0; task = task->next) {
        if (task != caller && signal_matches_target(task, caller, pid)) {
            found++;
        }
    }
    return found;
}

uint32_t kernel_signal_send_targets(struct kernel_task *caller,
                                    int64_t pid,
                                    uint32_t sig,
                                    kernel_pid_t sender_tid)
{
    struct kernel_task *target;
    struct kernel_task *task;
    uint32_t sent = 0U;

    if (pid > 0) {
        target = kernel_signal_find_by_tid((kernel_pid_t)pid);
        if (target != 0 && signal_is_leader(target) &&
            kernel_signal_send(target, sig, sender_tid) ==
                KERNEL_SIGNAL_STATUS_OK) {
            sent++;
        }
        return sent;
    }
    if (caller != 0 && caller != &scheduler.idle &&
        signal_matches_target(caller, caller, pid)) {
        if (kernel_signal_send(caller, sig, sender_tid) ==
            KERNEL_SIGNAL_STATUS_OK) {
            sent++;
        }
    }
    for (task = scheduler.ready_head; task != 0; task = task->next) {
        if (task != caller && signal_matches_target(task, caller, pid) &&
            kernel_signal_send(task, sig, sender_tid) ==
                KERNEL_SIGNAL_STATUS_OK) {
            sent++;
        }
    }
    for (task = scheduler.blocked_head; task != 0;) {
        struct kernel_task *next = task->next;

        if (task != caller && signal_matches_target(task, caller, pid) &&
            kernel_signal_send(task, sig, sender_tid) ==
                KERNEL_SIGNAL_STATUS_OK) {
            sent++;
        }
        task = next;
    }
    for (task = scheduler.stopped_head; task != 0;) {
        struct kernel_task *next = task->next;

        if (task != caller && signal_matches_target(task, caller, pid) &&
            kernel_signal_send(task, sig, sender_tid) ==
                KERNEL_SIGNAL_STATUS_OK) {
            sent++;
        }
        task = next;
    }
    return sent;
}

uint32_t kernel_signal_send_thread(struct kernel_task *caller,
                                   kernel_pid_t tgid,
                                   kernel_pid_t tid,
                                   uint32_t sig,
                                   kernel_pid_t sender_tid)
{
    struct kernel_task *target;

    (void)caller;
    if (tgid < 0 || tid <= 0) {
        return 0U;
    }
    target = kernel_signal_find_by_tid(tid);
    if (!signal_dispatchable(target) || target->group_leader == 0 ||
        (tgid != 0 && target->group_leader->tid != tgid)) {
        return 0U;
    }
    if (sig == 0U) {
        return 1U;
    }
    if (kernel_signal_send(target, sig, sender_tid) !=
        KERNEL_SIGNAL_STATUS_OK) {
        return 0U;
    }
    return 1U;
}

/* ---- delivery ---- */

static void signal_stop_current(struct kernel_task *task, uint32_t sig)
{
    task->wait_status = (uint32_t)(sig << 8) | LINUX_WAIT_STOPPED;
    task->stop_notified = 0U;
    task->continue_notified = 0U;
    task->state = KERNEL_THREAD_STATE_STOPPED;
    stopped_append(task);
    kernel_signal_notify_child_stop(task, 0);
    /* Park exactly like a blocked task: switch away and let SIGCONT or
     * SIGKILL put the task back on the ready queue. */
    (void)scheduler_switch_current_away(task);
}

static void signal_terminate(uint32_t sig, int core_dump)
{
    kernel_user_thread_exit(KERNEL_THREAD_EXIT_SIGNAL,
                            sig,
                            core_dump != 0 ? 1U : 0U);
}

static void signal_clear_restart(struct kernel_task *task)
{
    task->syscall_restart_kind = KERNEL_SYSCALL_RESTART_NONE;
    task->syscall_restart_deadline = 0U;
    task->syscall_restart_remaining_address = 0U;
}

static void signal_restart_without_handler(struct kernel_task *task,
                                           struct riscv_trap_frame *frame)
{
    if (task->syscall_restart_kind == KERNEL_SYSCALL_RESTART_NANOSLEEP) {
        /* restart_syscall(128) consumes the saved absolute deadline. */
        frame->a7 = 128U;
    } else if (task->syscall_restart_kind ==
               KERNEL_SYSCALL_RESTART_GENERIC) {
        signal_clear_restart(task);
    }
}

static void signal_finish_interrupted_syscall(
    struct kernel_task *task,
    struct riscv_trap_frame *frame,
    const struct kernel_signal_action *entry)
{
    if (task->syscall_restart_kind == KERNEL_SYSCALL_RESTART_NONE) {
        return;
    }
    if ((entry->flags & LINUX_SA_RESTART) != 0U) {
        if (task->syscall_restart_kind == KERNEL_SYSCALL_RESTART_NANOSLEEP) {
            frame->a7 = 128U;
        }
        return;
    }
    if (frame->sepc > UINT64_MAX - 4U) {
        signal_terminate(11U, 1);
    }
    frame->a0 = (uint64_t)(int64_t)-KERNEL_EINTR;
    frame->sepc += 4U;
    signal_clear_restart(task);
}

static void signal_fill_gprs(const struct riscv_trap_frame *frame,
                             uint8_t *mcontext)
{
    uint32_t index;

    for (index = 0U; index < 32U; index++) {
        uint64_t value;

        if (index == 0U) {
            value = frame->sepc;
        } else {
            value = *(const uint64_t *)((const uint8_t *)frame +
                                        signal_gpr_offsets[index]);
        }
        memcpy(mcontext + (size_t)index * 8U, &value, sizeof(value));
    }
}

static void signal_fill_fp(struct kernel_task *task, uint8_t *mcontext)
{
    uint32_t index;

    riscv_fpu_state_save(&task->fpu);
    for (index = 0U; index < 32U; index++) {
        memcpy(mcontext + LINUX_SIGCONTEXT_FP_OFFSET +
                   (size_t)index * 8U,
               &task->fpu.regs[index],
               sizeof(uint64_t));
    }
    memcpy(mcontext + LINUX_SIGCONTEXT_FP_OFFSET + 256U,
           &task->fpu.fcsr,
           sizeof(uint64_t));
}

static void signal_build_frame(struct kernel_task *task,
                               struct riscv_trap_frame *frame,
                               uint32_t sig,
                               const struct kernel_signal_action *entry,
                               uint64_t user_sp)
{
    uint8_t buffer[LINUX_SIGFRAME_SIZE];
    uint64_t old_blocked = task->signal_blocked;
    uint32_t sig32 = sig;
    uint32_t code = LINUX_SI_USER;
    uint32_t sender = task->signal_sender[sig - 1U];
    uint32_t ss_disable = (uint32_t)LINUX_SS_DISABLE;
    size_t copied = 0U;
    memset(buffer, 0, sizeof(buffer));
    /* siginfo: SI_USER with the first sender's pid. */
    memcpy(buffer, &sig32, sizeof(sig32));
    memcpy(buffer + 8U, &code, sizeof(code));
    memcpy(buffer + LINUX_SIGINFO_PID_OFFSET, &sender, sizeof(sender));
    /* ucontext: uc_flags 0, uc_link NULL, uc_stack SS_DISABLE. */
    memcpy(buffer + LINUX_SIGINFO_SIZE + 24U,
           &ss_disable,
           sizeof(ss_disable));
    /* uc_sigmask: the mask the interrupted context runs under. */
    memcpy(buffer + LINUX_SIGINFO_SIZE + LINUX_UCONTEXT_SIGMASK_OFFSET,
           &old_blocked,
           sizeof(old_blocked));
    signal_fill_gprs(frame,
                     buffer + LINUX_SIGINFO_SIZE +
                         LINUX_UCONTEXT_MCONTEXT_OFFSET);
    signal_fill_fp(task,
                   buffer + LINUX_SIGINFO_SIZE +
                       LINUX_UCONTEXT_MCONTEXT_OFFSET);

    if (kernel_copy_to_user(&task->mm,
                            user_sp,
                            buffer,
                            LINUX_SIGFRAME_SIZE,
                            &copied) != KERNEL_UACCESS_STATUS_OK ||
        copied != LINUX_SIGFRAME_SIZE) {
        signal_terminate(11U, 1);
    }
    frame->sp = user_sp;
    frame->sepc = entry->handler;
    frame->ra = RISCV_USER_ELF_VDSO_BASE;
    frame->a0 = sig;
    frame->a1 = user_sp;
    frame->a2 = user_sp + LINUX_SIGINFO_SIZE;
    task->signal_blocked |=
        entry->mask | ((entry->flags & LINUX_SA_NODEFER) != 0U
                           ? 0U
                           : signal_mask(sig));
    if ((entry->flags & LINUX_SA_RESETHAND) != 0U) {
        struct kernel_signal_table *table = signal_table_of(task);
        struct kernel_signal_action *reset = &table->actions[sig - 1U];

        reset->handler = KERNEL_SIGNAL_DFL;
        reset->flags = 0U;
        reset->mask = 0U;
    }
}

void kernel_signal_deliver_pending(struct riscv_trap_frame *frame)
{
    struct kernel_task *task = scheduler.current;
    uint64_t pending;
    uint32_t sig;
    const struct kernel_signal_action *entry;

    if (task == 0 || task == &scheduler.idle ||
        task->arch.user_mode != 1U ||
        (frame->sstatus & RISCV_SSTATUS_SPP) != 0U) {
        return;
    }
    for (;;) {
        pending = task->signal_pending & ~task->signal_blocked;
        if (pending == 0U) {
            return;
        }
        sig = signal_first_set(pending);
        task->signal_pending &= ~signal_mask(sig);
        entry = signal_action_of(task, sig);
        if (entry != 0 && entry->handler != KERNEL_SIGNAL_DFL &&
            entry->handler != KERNEL_SIGNAL_IGN) {
            uint64_t user_sp;

            if (frame->sp < LINUX_SIGFRAME_SIZE) {
                signal_terminate(11U, 1);
            }
            user_sp = (frame->sp - LINUX_SIGFRAME_SIZE) &
                      ~(uintptr_t)15U;
            signal_finish_interrupted_syscall(task, frame, entry);

            signal_build_frame(task, frame, sig, entry, user_sp);
            return;
        }
        if (entry != 0 && entry->handler == KERNEL_SIGNAL_IGN) {
            continue;
        }
        switch (signal_default_action(sig)) {
        case KERNEL_SIGNAL_DEFAULT_IGNORE:
        case KERNEL_SIGNAL_DEFAULT_CONTINUE:
            continue;
        case KERNEL_SIGNAL_DEFAULT_STOP:
            signal_stop_current(task, sig);
            return;
        case KERNEL_SIGNAL_DEFAULT_CORE:
            signal_terminate(sig, 1);
            return;
        default:
            signal_terminate(sig, 0);
            return;
        }
    }
}

void kernel_signal_prepare_user_return(struct riscv_trap_frame *frame)
{
    struct kernel_task *task = scheduler.current;

    if (frame == 0 || task == 0 || task == &scheduler.idle ||
        task->arch.user_mode != 1U ||
        (frame->sstatus & RISCV_SSTATUS_SPP) != 0U) {
        return;
    }
    if ((task->signal_pending & ~task->signal_blocked) == 0U) {
        signal_restart_without_handler(task, frame);
        return;
    }
    kernel_signal_deliver_pending(frame);
}

void kernel_signal_restore_current(struct riscv_trap_frame *frame)
{
    struct kernel_task *task = scheduler.current;
    uint8_t mcontext[32U * 8U + 264U];
    uint8_t fp[264U];
    uint64_t sigmask = 0U;
    uint64_t value;
    size_t copied = 0U;
    uint64_t user_sp = frame == 0 ? 0U : frame->sp;
    uint32_t index;
    enum kernel_uaccess_status copy_status;

    if (frame == 0 || task == 0 || task == &scheduler.idle ||
        task->arch.user_mode != 1U ||
        frame->sp > UINT64_MAX -
                          (LINUX_SIGINFO_SIZE +
                           LINUX_UCONTEXT_MCONTEXT_OFFSET +
                           sizeof(mcontext)) ||
        frame->sp < LINUX_SIGINFO_SIZE + LINUX_UCONTEXT_MCONTEXT_OFFSET) {
        signal_terminate(11U, 1);
    }
    copied = 0U;
    copy_status = kernel_copy_from_user(&task->mm,
                                        &sigmask,
                                        user_sp + LINUX_SIGINFO_SIZE +
                                            LINUX_UCONTEXT_SIGMASK_OFFSET,
                                        sizeof(sigmask),
                                        &copied);
    if (copy_status != KERNEL_UACCESS_STATUS_OK ||
        copied != sizeof(sigmask)) {
        signal_terminate(11U, 1);
    }
    copied = 0U;
    copy_status = kernel_copy_from_user(&task->mm,
                                        mcontext,
                                        user_sp + LINUX_SIGINFO_SIZE +
                                            LINUX_UCONTEXT_MCONTEXT_OFFSET,
                                        sizeof(mcontext),
                                        &copied);
    if (copy_status != KERNEL_UACCESS_STATUS_OK ||
        copied != sizeof(mcontext)) {
        signal_terminate(11U, 1);
    }
    task->signal_blocked = sigmask & ~SIGNAL_MASK_KILL_STOP;
    for (index = 0U; index < 32U; index++) {
        memcpy(&value, mcontext + (size_t)index * 8U, sizeof(value));
        if (index == 0U) {
            frame->sepc = value;
        } else {
            *(uint64_t *)((uint8_t *)frame + signal_gpr_offsets[index]) =
                value;
        }
    }
    memcpy(fp, mcontext + LINUX_SIGCONTEXT_FP_OFFSET, sizeof(fp));
    for (index = 0U; index < 32U; index++) {
        memcpy(&task->fpu.regs[index],
               fp + (size_t)index * 8U,
               sizeof(uint64_t));
    }
    memcpy(&task->fpu.fcsr, fp + 256U, sizeof(uint64_t));
    task->fpu.saved = 1U;
    riscv_fpu_state_restore(&task->fpu);
}
