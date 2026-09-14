#include "private.h"

#include <arch/riscv/direct_map.h>
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

#define SIGNAL_KILL 9U
#define SIGNAL_CONTINUE 18U
#define SIGNAL_STOP 19U
#define LINUX_WAIT_STOPPED 0x7fU

#define SIGNAL_MASK_KILL_STOP KERNEL_SIGNAL_UNBLOCKABLE_MASK
#define SIGNAL_MASK_STOP \
    ((UINT64_C(1) << 18U) | (UINT64_C(1) << 19U) | \
     (UINT64_C(1) << 20U) | (UINT64_C(1) << 21U))

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
           task->state != KERNEL_THREAD_STATE_EXITED &&
           task->state != KERNEL_THREAD_STATE_GROUP_DEAD;
}

static struct kernel_task *signal_group_leader(
    const struct kernel_task *task)
{
    struct kernel_task *leader;

    if (task == 0 || task == &scheduler.idle ||
        task->magic != KERNEL_THREAD_MAGIC || task->arch.user_mode != 1U) {
        return 0;
    }
    leader = task->group_leader;
    if (leader == 0 || leader->magic != KERNEL_THREAD_MAGIC ||
        leader->arch.user_mode != 1U || leader->group_leader != leader ||
        leader->group_members == 0U || leader->group_next == 0 ||
        leader->group_previous == 0) {
        return 0;
    }
    return leader;
}

static struct kernel_task *signal_group_representative(
    const struct kernel_task *task)
{
    struct kernel_task *leader = signal_group_leader(task);
    struct kernel_task *member;

    if (leader == 0) {
        return 0;
    }
    member = leader;
    do {
        if (signal_dispatchable(member)) {
            return member;
        }
        member = member->group_next;
    } while (member != leader);
    return 0;
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
    table->references = 1U;
    task->signal_table_address = (uintptr_t)page;
    return KERNEL_SIGNAL_STATUS_OK;
}

enum kernel_signal_status kernel_signal_release_table(
    struct kernel_task *task)
{
    struct kernel_signal_table *table;
    uint64_t physical_address;
    void *page;

    if (task == 0 || task->signal_table_address == 0U) {
        return KERNEL_SIGNAL_STATUS_OK;
    }
    page = (void *)task->signal_table_address;
    table = signal_table_of(task);
    if (table == 0 || table->references == 0U) {
        return KERNEL_SIGNAL_STATUS_INVALID_STATE;
    }
    if (table->references > 1U) {
        table->references--;
        task->signal_table_address = 0U;
        return KERNEL_SIGNAL_STATUS_OK;
    }
    if (riscv_direct_map_va_to_pa((uintptr_t)page,
                                  BOAROS_PAGE_SIZE,
                                  &physical_address) !=
        RISCV_DIRECT_MAP_STATUS_OK) {
        return KERNEL_SIGNAL_STATUS_INVALID_STATE;
    }
    (void)physical_page_release(scheduler.allocator, physical_address);
    task->signal_table_address = 0U;
    return KERNEL_SIGNAL_STATUS_OK;
}

static void signal_clear_pending_group(struct kernel_task *task,
                                       uint64_t mask)
{
    struct kernel_task *leader = signal_group_leader(task);
    struct kernel_task *member;

    if (leader == 0) {
        task->signal_pending &= ~mask;
        return;
    }
    leader->group_pending &= ~mask;
    member = leader;
    do {
        member->signal_pending &= ~mask;
        member = member->group_next;
    } while (member != leader);
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
    if (entry->handler == KERNEL_SIGNAL_IGN ||
        (entry->handler == KERNEL_SIGNAL_DFL &&
         signal_default_action(sig) == KERNEL_SIGNAL_DEFAULT_IGNORE)) {
        signal_clear_pending_group(task, signal_mask(sig));
    }
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
    child_table->references = 1U;
    child->signal_table_address = (uintptr_t)child_table;
    return KERNEL_SIGNAL_STATUS_OK;
}

enum kernel_signal_status kernel_signal_share(
    struct kernel_task *child, struct kernel_task *parent)
{
    enum kernel_signal_status status;
    struct kernel_signal_table *table;

    if (child == 0 || parent == 0 || child->magic != KERNEL_THREAD_MAGIC ||
        child->arch.user_mode != 1U || !signal_dispatchable(parent) ||
        child->signal_table_address != 0U) {
        return KERNEL_SIGNAL_STATUS_INVALID_ARGUMENT;
    }
    status = signal_alloc_table(parent);
    if (status != KERNEL_SIGNAL_STATUS_OK) {
        return status;
    }
    table = signal_table_of(parent);
    if (table == 0) {
        return KERNEL_SIGNAL_STATUS_INVALID_STATE;
    }
    if (table->references == UINT32_MAX) {
        return KERNEL_SIGNAL_STATUS_NO_MEMORY;
    }
    table->references++;
    child->signal_table_address = parent->signal_table_address;
    child->signal_blocked = parent->signal_blocked;
    child->signal_pending = 0U;
    memset(child->signal_sender, 0, sizeof(child->signal_sender));
    return KERNEL_SIGNAL_STATUS_OK;
}

int kernel_signal_has_pending(const struct kernel_task *task)
{
    uint64_t pending = task->signal_pending;

    if (signal_group_leader(task) != 0) {
        pending |= task->group_leader->group_pending;
    }
    return (pending & ~task->signal_blocked) != 0U;
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
        (new_mask != 0 && how > LINUX_SIG_SETMASK)) {
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
    if (signal_group_leader(task) != 0) {
        *pending |= task->group_leader->group_pending;
    }
    *pending &= task->signal_blocked;
    return KERNEL_SIGNAL_STATUS_OK;
}

void kernel_signal_reset_on_exec(struct kernel_task *task)
{
    struct kernel_signal_table *table = signal_table_of(task);

    if (table != 0) {
        for (uint32_t index = 0U; index < KERNEL_SIGNAL_COUNT; index++) {
            struct kernel_signal_action *entry = &table->actions[index];

            if (entry->handler != KERNEL_SIGNAL_IGN) {
                entry->handler = KERNEL_SIGNAL_DFL;
                entry->flags = 0U;
                entry->mask = 0U;
            }
        }
    }
    task->signal_restore_mask = 0U;
    kernel_signal_clear_syscall_restart(task);
}

int kernel_signal_wants_sigchld(const struct kernel_task *parent)
{
    const struct kernel_signal_action *entry =
        parent == 0 ? 0 : signal_action_of(parent, 17U);

    /* A blocked default-ignored SIGCHLD is still observable via pending. */
    return parent != 0 &&
           (entry == 0 || entry->handler != KERNEL_SIGNAL_IGN);
}

int kernel_signal_child_autoreap(const struct kernel_task *parent)
{
    const struct kernel_signal_action *entry =
        parent == 0 ? 0 : signal_action_of(parent, 17U);

    return entry != 0 && (entry->handler == KERNEL_SIGNAL_IGN ||
                          (entry->flags & LINUX_SA_NOCLDWAIT) != 0U);
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
           (entry->flags & LINUX_SA_NOCLDSTOP) == 0U;
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

static void signal_ready_unlink(struct kernel_task *task)
{
    struct kernel_task *previous = 0;
    struct kernel_task *member = scheduler.ready_head;

    while (member != 0 && member != task) {
        previous = member;
        member = member->next;
    }
    if (member == 0) {
        return;
    }
    if (previous == 0) {
        scheduler.ready_head = task->next;
    } else {
        previous->next = task->next;
    }
    if (scheduler.ready_tail == task) {
        scheduler.ready_tail = previous;
    }
    task->next = 0;
}

static void signal_resume_stopped(struct kernel_task *task)
{
    stopped_unlink(task);
    task->state = KERNEL_THREAD_STATE_READY;
    ready_append(task);
}

static void signal_prepare_group(struct kernel_task *task, uint32_t sig)
{
    struct kernel_task *leader = signal_group_leader(task);
    struct kernel_task *member;
    uint64_t clear;
    int continued;

    if (leader == 0) {
        return;
    }
    continued = leader->group_stopped != 0U;
    if (sig == SIGNAL_CONTINUE) {
        clear = SIGNAL_MASK_STOP;
    } else if (sig >= SIGNAL_STOP && sig <= 22U) {
        clear = signal_mask(SIGNAL_CONTINUE);
    } else {
        return;
    }
    leader->group_pending &= ~clear;
    member = leader;
    do {
        member->signal_pending &= ~clear;
        if (sig == SIGNAL_CONTINUE &&
            member->state == KERNEL_THREAD_STATE_STOPPED) {
            signal_resume_stopped(member);
        }
        member = member->group_next;
    } while (member != leader);
    if (sig == SIGNAL_CONTINUE) leader->group_stopped = 0U;
    if (sig == SIGNAL_CONTINUE && continued) {
        leader->stop_notified = 0U;
        leader->continue_notified = 1U;
        kernel_signal_notify_child_stop(leader, 1);
    }
}

static int signal_wants_signal(const struct kernel_task *task,
                               uint32_t sig)
{
    if (!signal_dispatchable(task) || task->terminate_requested != 0U ||
        (task->signal_blocked & signal_mask(sig)) != 0U) {
        return 0;
    }
    return task->state != KERNEL_THREAD_STATE_STOPPED ||
           sig == SIGNAL_KILL || sig == SIGNAL_CONTINUE;
}

static struct kernel_task *signal_choose_group_member(
    struct kernel_task *target, uint32_t sig)
{
    struct kernel_task *leader = signal_group_leader(target);
    struct kernel_task *member;

    if (leader == 0) {
        return 0;
    }
    if (signal_wants_signal(target, sig)) {
        return target;
    }
    member = leader;
    do {
        if (signal_wants_signal(member, sig)) {
            return member;
        }
        member = member->group_next;
    } while (member != leader);
    return 0;
}

static int signal_ignored_unblocked(const struct kernel_task *target,
                                    uint32_t sig)
{
    const struct kernel_signal_action *entry;

    if (target == 0 ||
        (target->signal_blocked & signal_mask(sig)) != 0U) {
        return 0;
    }
    entry = signal_action_of(target, sig);
    if (entry != 0 && entry->handler == KERNEL_SIGNAL_IGN) {
        return 1;
    }
    return (entry == 0 || entry->handler == KERNEL_SIGNAL_DFL) &&
           (signal_default_action(sig) == KERNEL_SIGNAL_DEFAULT_IGNORE ||
            signal_default_action(sig) == KERNEL_SIGNAL_DEFAULT_CONTINUE);
}

static enum kernel_signal_status signal_send_one(
    struct kernel_task *target, uint32_t sig, kernel_pid_t sender_tid,
    int process_directed)
{
    struct kernel_task *leader;
    struct kernel_task *wake_target;
    uint64_t *pending;
    uint32_t *senders;
    uint64_t bit;

    if (!signal_dispatchable(target) || sig == 0U ||
        sig > KERNEL_SIGNAL_COUNT) {
        return KERNEL_SIGNAL_STATUS_INVALID_ARGUMENT;
    }
    leader = signal_group_leader(target);
    if (leader == 0) {
        return KERNEL_SIGNAL_STATUS_INVALID_STATE;
    }
    signal_prepare_group(target, sig);
    if (sig == SIGNAL_KILL) {
        process_group_request_exit(target, KERNEL_THREAD_EXIT_SIGNAL,
                                   sig, 0U);
        return KERNEL_SIGNAL_STATUS_OK;
    }
    wake_target = process_directed
                      ? signal_choose_group_member(target, sig)
                      : target;
    if (signal_ignored_unblocked(wake_target, sig)) {
        return KERNEL_SIGNAL_STATUS_OK;
    }
    pending = process_directed ? &leader->group_pending
                               : &target->signal_pending;
    senders = process_directed ? leader->group_sender
                               : target->signal_sender;
    bit = signal_mask(sig);
    if ((*pending & bit) != 0U) {
        return KERNEL_SIGNAL_STATUS_OK;
    }
    *pending |= bit;
    senders[sig - 1U] = (uint32_t)sender_tid;
    if (wake_target != 0 && signal_wants_signal(wake_target, sig) &&
        kernel_scheduler_wake_signal(wake_target) !=
            KERNEL_SCHEDULER_STATUS_OK) {
        return KERNEL_SIGNAL_STATUS_INVALID_STATE;
    }
    return KERNEL_SIGNAL_STATUS_OK;
}

enum kernel_signal_status kernel_signal_send(struct kernel_task *target,
                                             uint32_t sig,
                                             kernel_pid_t sender_tid)
{
    struct kernel_task *representative = signal_group_representative(target);

    if (representative == 0) {
        return KERNEL_SIGNAL_STATUS_INVALID_ARGUMENT;
    }
    return signal_send_one(representative, sig, sender_tid, 1);
}

enum kernel_signal_status kernel_signal_send_task(
    struct kernel_task *target, uint32_t sig, kernel_pid_t sender_tid)
{
    return signal_send_one(target, sig, sender_tid, 0);
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

static struct kernel_task *signal_group_by_tgid(kernel_pid_t tgid)
{
    struct kernel_task *task;

    if (tgid <= 0) {
        return 0;
    }
    if (signal_dispatchable(scheduler.current) &&
        signal_group_leader(scheduler.current) != 0 &&
        scheduler.current->group_leader->tid == tgid) {
        return scheduler.current->group_leader;
    }
    for (task = scheduler.ready_head; task != 0; task = task->next) {
        if (signal_group_leader(task) != 0 &&
            task->group_leader->tid == tgid) {
            return task->group_leader;
        }
    }
    for (task = scheduler.blocked_head; task != 0; task = task->next) {
        if (signal_group_leader(task) != 0 &&
            task->group_leader->tid == tgid) {
            return task->group_leader;
        }
    }
    for (task = scheduler.stopped_head; task != 0; task = task->next) {
        if (signal_group_leader(task) != 0 &&
            task->group_leader->tid == tgid) {
            return task->group_leader;
        }
    }
    return 0;
}

static int signal_matches_target(const struct kernel_task *task,
                                 struct kernel_task *caller,
                                 int64_t pid)
{
    struct kernel_task *leader = signal_group_leader(task);
    struct kernel_task *caller_leader = signal_group_leader(caller);

    if (leader == 0 || signal_group_representative(task) != task) {
        return 0;
    }
    if (pid > 0) {
        return leader->tid == (kernel_pid_t)pid;
    }
    if (pid == 0) {
        return caller_leader != 0 &&
               leader->process_group == caller_leader->process_group;
    }
    if (pid == -1) {
        return leader != caller_leader && leader->tid != 1;
    }
    if (pid == INT32_MIN) {
        return 0;
    }
    return leader->process_group == (kernel_pid_t)(-pid);
}

uint32_t kernel_signal_resolve_targets(struct kernel_task *caller,
                                       int64_t pid)
{
    struct kernel_task *task;
    uint32_t found = 0U;

    if (pid > 0) {
        task = signal_group_by_tgid((kernel_pid_t)pid);
        return task != 0 && signal_group_representative(task) != 0
                   ? 1U : 0U;
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

static struct kernel_task *signal_next_matching_group(
    struct kernel_task *caller, int64_t pid, kernel_pid_t after)
{
    struct kernel_task *best = 0;
    struct kernel_task *task;

#define CONSIDER_SIGNAL_TARGET(candidate)                                    \
    do {                                                                      \
        struct kernel_task *considered = (candidate);                         \
        if (signal_matches_target(considered, caller, pid)) {                 \
            struct kernel_task *leader = considered->group_leader;           \
            if (leader->tid > after &&                                       \
                (best == 0 || leader->tid < best->tid)) {                     \
                best = leader;                                                \
            }                                                                 \
        }                                                                     \
    } while (0)

    if (signal_dispatchable(scheduler.current)) {
        CONSIDER_SIGNAL_TARGET(scheduler.current);
    }
    for (task = scheduler.ready_head; task != 0; task = task->next) {
        CONSIDER_SIGNAL_TARGET(task);
    }
    for (task = scheduler.blocked_head; task != 0; task = task->next) {
        CONSIDER_SIGNAL_TARGET(task);
    }
    for (task = scheduler.stopped_head; task != 0; task = task->next) {
        CONSIDER_SIGNAL_TARGET(task);
    }
#undef CONSIDER_SIGNAL_TARGET
    return best;
}

uint32_t kernel_signal_send_targets(struct kernel_task *caller,
                                    int64_t pid,
                                    uint32_t sig,
                                    kernel_pid_t sender_tid)
{
    struct kernel_task *target;
    kernel_pid_t after = 0;
    uint32_t sent = 0U;

    if (pid > 0) {
        target = signal_group_by_tgid((kernel_pid_t)pid);
        if (target != 0 && signal_group_representative(target) != 0 &&
            kernel_signal_send(target, sig, sender_tid) ==
                KERNEL_SIGNAL_STATUS_OK) {
            sent++;
        }
        return sent;
    }
    while ((target = signal_next_matching_group(caller, pid, after)) != 0) {
        after = target->tid;
        if (kernel_signal_send(target, sig, sender_tid) ==
            KERNEL_SIGNAL_STATUS_OK) {
            sent++;
        }
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
    if (kernel_signal_send_task(target, sig, sender_tid) !=
        KERNEL_SIGNAL_STATUS_OK) {
        return 0U;
    }
    return 1U;
}

/* ---- delivery ---- */

static void signal_terminate(uint32_t sig, int core_dump)
    __attribute__((noreturn));

static void signal_stop_member(struct kernel_task *task, uint32_t sig)
{
    uint64_t bit = signal_mask(sig);

    if (!signal_dispatchable(task) ||
        task->state == KERNEL_THREAD_STATE_STOPPED) {
        return;
    }
    if (task->state == KERNEL_THREAD_STATE_BLOCKED) {
        task->signal_pending |= bit;
        if (kernel_scheduler_wake_signal(task) !=
                KERNEL_SCHEDULER_STATUS_OK ||
            task->state == KERNEL_THREAD_STATE_BLOCKED) {
            return;
        }
    }
    if (task->state == KERNEL_THREAD_STATE_READY) {
        signal_ready_unlink(task);
    } else if (task->state != KERNEL_THREAD_STATE_RUNNING ||
               task != scheduler.current) {
        return;
    }
    task->signal_pending &= ~bit;
    task->wait_status = (uint32_t)(sig << 8) | LINUX_WAIT_STOPPED;
    task->stop_notified = 0U;
    task->continue_notified = 0U;
    task->state = KERNEL_THREAD_STATE_STOPPED;
    stopped_append(task);
}

static void signal_stop_group(struct kernel_task *task, uint32_t sig)
{
    struct kernel_task *leader = signal_group_leader(task);
    struct kernel_task *member;
    int all_stopped = 1;

    if (leader == 0) {
        signal_terminate(11U, 1);
    }
    member = leader;
    do {
        signal_stop_member(member, sig);
        if (signal_dispatchable(member) &&
            member->state != KERNEL_THREAD_STATE_STOPPED) all_stopped = 0;
        member = member->group_next;
    } while (member != leader);
    leader->wait_status = (uint32_t)(sig << 8) | LINUX_WAIT_STOPPED;
    if (all_stopped && !leader->group_stopped) {
        leader->group_stopped = 1U;
        leader->stop_notified = 0U;
        leader->continue_notified = 0U;
        kernel_signal_notify_child_stop(leader, 0);
    }
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

void kernel_signal_clear_syscall_restart(struct kernel_task *task)
{
    task->syscall_restart_kind = KERNEL_SYSCALL_RESTART_NONE;
    task->syscall_restart_deadline = 0U;
    task->syscall_restart_remaining_address = 0U;
}

int kernel_signal_nanosleep_restart(const struct kernel_task *task,
                                    uint64_t *deadline,
                                    uint64_t *remaining_address)
{
    if (task->syscall_restart_kind != KERNEL_SYSCALL_RESTART_NANOSLEEP) {
        return 0;
    }
    *deadline = task->syscall_restart_deadline;
    *remaining_address = task->syscall_restart_remaining_address;
    return 1;
}

enum kernel_signal_restart kernel_signal_restart_decide(
    struct kernel_task *task, int has_handler, uint64_t flags)
{
    uint32_t kind = task->syscall_restart_kind;

    if (kind == KERNEL_SYSCALL_RESTART_NONE) {
        return KERNEL_SIGNAL_RESTART_NONE;
    }
    if (!has_handler && kind == KERNEL_SYSCALL_RESTART_NANOSLEEP) {
        return KERNEL_SIGNAL_RESTART_BLOCK;
    }
    /* Handler syscalls must not inherit an outer restart block. The
     * interrupted register snapshot itself carries a generic retry. */
    kernel_signal_clear_syscall_restart(task);
    if (has_handler && (kind == KERNEL_SYSCALL_RESTART_NANOSLEEP ||
                         (flags & LINUX_SA_RESTART) == 0U)) {
        return KERNEL_SIGNAL_RESTART_INTERRUPTED;
    }
    return KERNEL_SIGNAL_RESTART_RETRY;
}

enum kernel_signal_status kernel_signal_suspend(struct kernel_task *task,
                                                uint64_t mask)
{
    enum kernel_wait_wake_reason reason;

    if (task != scheduler.current || !signal_dispatchable(task)) {
        return KERNEL_SIGNAL_STATUS_INVALID_ARGUMENT;
    }
    task->signal_saved_mask = task->signal_blocked;
    task->signal_restore_mask = 1U;
    task->signal_blocked = mask & ~SIGNAL_MASK_KILL_STOP;
    while (!kernel_signal_has_pending(task) &&
           task->terminate_requested == 0U) {
        if (kernel_scheduler_block_current(0, 0U, 1, &reason) !=
            KERNEL_SCHEDULER_STATUS_OK) {
            task->signal_blocked = task->signal_saved_mask;
            task->signal_restore_mask = 0U;
            return KERNEL_SIGNAL_STATUS_INVALID_STATE;
        }
    }
    /* Keep the temporary mask until delivery. The signal frame contains
     * the pre-suspend mask, which sigreturn restores atomically. */
    return KERNEL_SIGNAL_STATUS_OK;
}

enum kernel_signal_status kernel_signal_set_temporary_mask(
    struct kernel_task *task,
    uint64_t new_mask,
    uint64_t *saved_mask)
{
    if (task == 0 || saved_mask == 0) {
        return KERNEL_SIGNAL_STATUS_INVALID_ARGUMENT;
    }
    *saved_mask = task->signal_blocked;
    task->signal_saved_mask = task->signal_blocked;
    task->signal_restore_mask = 1U;
    task->signal_blocked = new_mask & ~SIGNAL_MASK_KILL_STOP;
    return KERNEL_SIGNAL_STATUS_OK;
}

void kernel_signal_restore_temporary_mask(
    struct kernel_task *task,
    uint64_t saved_mask,
    int interrupted)
{
    if (task == 0) {
        return;
    }
    if (!interrupted) {
        task->signal_blocked = saved_mask;
        task->signal_restore_mask = 0U;
    }
}

enum kernel_signal_select_result kernel_signal_select(
    struct kernel_task *task,
    struct kernel_signal_delivery *delivery)
{
    for (;;) {
        struct kernel_task *leader = signal_group_leader(task);
        uint64_t pending = task->signal_pending & ~task->signal_blocked;
        int shared = 0;
        uint32_t sig;
        const struct kernel_signal_action *entry;

        if (task->terminate_requested != 0U) {
            delivery->exit_reason = (uint32_t)KERNEL_THREAD_EXIT_SYSCALL;
            delivery->exit_status = 0U;
            delivery->exit_detail = 0U;
            return KERNEL_SIGNAL_SELECT_EXIT;
        }
        if (pending == 0U && leader != 0) {
            pending = leader->group_pending & ~task->signal_blocked;
            shared = pending != 0U;
        }
        if (pending == 0U) {
            if (task->signal_restore_mask != 0U) {
                enum kernel_wait_wake_reason reason;

                if (kernel_scheduler_block_current(0, 0U, 1, &reason) !=
                    KERNEL_SCHEDULER_STATUS_OK) {
                    signal_terminate(11U, 1);
                }
                continue;
            }
            return KERNEL_SIGNAL_SELECT_NONE;
        }
        sig = signal_first_set(pending);
        if (shared) {
            leader->group_pending &= ~signal_mask(sig);
        } else {
            task->signal_pending &= ~signal_mask(sig);
        }
        entry = signal_action_of(task, sig);
        if (entry != 0 && entry->handler != KERNEL_SIGNAL_DFL &&
            entry->handler != KERNEL_SIGNAL_IGN) {
            delivery->signal = sig;
            delivery->sender = shared
                                   ? leader->group_sender[sig - 1U]
                                   : task->signal_sender[sig - 1U];
            delivery->handler = entry->handler;
            delivery->flags = entry->flags;
            delivery->restore_mask = task->signal_restore_mask != 0U
                                         ? task->signal_saved_mask
                                         : task->signal_blocked;
            task->signal_restore_mask = 0U;
            task->signal_blocked |= entry->mask |
                ((entry->flags & LINUX_SA_NODEFER) != 0U
                     ? 0U : signal_mask(sig));
            if ((entry->flags & LINUX_SA_RESETHAND) != 0U) {
                struct kernel_signal_action *reset =
                    &signal_table_of(task)->actions[sig - 1U];

                reset->handler = KERNEL_SIGNAL_DFL;
                reset->flags = 0U;
                reset->mask = 0U;
            }
            return KERNEL_SIGNAL_SELECT_HANDLER;
        }
        if (entry != 0 && entry->handler == KERNEL_SIGNAL_IGN) {
            continue;
        }
        switch (signal_default_action(sig)) {
        case KERNEL_SIGNAL_DEFAULT_IGNORE:
        case KERNEL_SIGNAL_DEFAULT_CONTINUE:
            continue;
        case KERNEL_SIGNAL_DEFAULT_STOP:
            signal_stop_group(task, sig);
            continue;
        case KERNEL_SIGNAL_DEFAULT_CORE:
            signal_terminate(sig, 1);
        default:
            signal_terminate(sig, 0);
        }
    }
}
