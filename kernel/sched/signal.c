#include <arch/task.h>
#include "private.h"

#include <kernel/errno.h>
#include <kernel/physical_page.h>
#include <kernel/page.h>
#include <kernel/scheduler.h>
#include <kernel/signal.h>
#include <kernel/task.h>
#include <kernel/time.h>
#include <kernel/uaccess.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SIGNAL_KILL 9U
#define SIGNAL_CONTINUE 18U
#define SIGNAL_STOP 19U
#define LINUX_WAIT_STOPPED 0x7fU

void kernel_signal_encode_info(void *output,
    const struct kernel_signal_wait_info *info, int fault, uint64_t fault_address)
{
    uint8_t *data = output;
    memset(data, 0, 128);
    memcpy(data, &info->signal, 4);
    memcpy(data + 8, &info->code, 4);
    if (fault) memcpy(data + 16, &fault_address, 8);
    else {
        memcpy(data + 16, &info->sender, 4);
        memcpy(data + 20, &info->uid, 4);
        if (info->signal == 17 && info->code > 0 && info->code <= 6)
            memcpy(data + 24, &info->status, 4);
    }
}

static enum kernel_signal_status signal_send_one(
    struct kernel_task *, uint32_t, kernel_pid_t, int, int16_t,
    const struct kernel_signal_child_info *);

static struct kernel_task *real_timers;
static void real_timer_remove(struct kernel_task *leader)
{
    if (!leader->real_timer_active) return;
    struct kernel_task **link = &real_timers;
    while (*link != leader) {
        if (!*link) __builtin_trap();
        link = &(*link)->real_timer_next;
    }
    *link = leader->real_timer_next;
    leader->real_timer_next = 0;
    leader->real_timer_active = 0;
}

static void real_timer_insert(struct kernel_task *leader)
{
    struct kernel_task **link = &real_timers;
    while (*link && (*link)->real_timer_deadline <= leader->real_timer_deadline)
        link = &(*link)->real_timer_next;
    leader->real_timer_next = *link;
    *link = leader;
    leader->real_timer_active = 1;
}

static void real_timer_rearm(struct kernel_task *leader)
{
    if (!leader || leader->real_timer_active || !leader->real_timer_interval) return;
    uint64_t now = kernel_time_monotonic_ns();
    uint64_t deadline = leader->real_timer_deadline;
    uint64_t interval = leader->real_timer_interval;
    /* 在 SIGALRM 消费时转到下一期限；阻塞信号不会产生密集 timer 噪声。 */
    uint64_t advance = deadline <= now ? (now - deadline) / interval + 1 : 0;
    deadline = advance > (UINT64_MAX - deadline) / interval ? UINT64_MAX : deadline + advance * interval;
    leader->real_timer_deadline = deadline;
    real_timer_insert(leader);
}

void kernel_signal_timer_get(struct kernel_task *task, uint64_t *remaining, uint64_t *interval)
{
    struct kernel_task *leader = task->group_leader;
    uint64_t now = kernel_time_monotonic_ns();
    *remaining = leader->real_timer_active ?
        (leader->real_timer_deadline > now ? leader->real_timer_deadline - now : 1000) : 0;
    *interval = leader->real_timer_interval;
}

void kernel_signal_timer_cancel(struct kernel_task *leader)
{
    real_timer_remove(leader);
    leader->real_timer_deadline = leader->real_timer_interval = 0;
}

void kernel_signal_timer_set(struct kernel_task *task, uint64_t value, uint64_t interval,
    uint64_t *old_remaining, uint64_t *old_interval)
{
    kernel_signal_timer_get(task, old_remaining, old_interval);
    struct kernel_task *leader = task->group_leader;
    kernel_signal_timer_cancel(leader);
    if (!value) return;
    uint64_t now = kernel_time_monotonic_ns();
    leader->real_timer_deadline = value > UINT64_MAX - now ? UINT64_MAX : now + value;
    leader->real_timer_interval = interval;
    real_timer_insert(leader);
}

void kernel_signal_timer_adopt(struct kernel_task *task, struct kernel_task *old_leader)
{
    uint32_t active = old_leader->real_timer_active;
    real_timer_remove(old_leader);
    task->real_timer_deadline = old_leader->real_timer_deadline;
    task->real_timer_interval = old_leader->real_timer_interval;
    old_leader->real_timer_deadline = old_leader->real_timer_interval = 0;
    if (active) real_timer_insert(task);
}

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
    if (task != 0 && task != kernel_cpu_current()->idle &&
        task->magic == KERNEL_THREAD_MAGIC && task->arch.user_mode == 1U) {
        if (task->syscall_restart.kind == KERNEL_SYSCALL_RESTART_NONE) {
            task->syscall_restart = (struct kernel_syscall_restart_state){
                .kind = KERNEL_SYSCALL_RESTART_GENERIC,
            };
        }
    }
}

void kernel_signal_note_nanosleep_restart(struct kernel_task *task,
                                          uint64_t deadline,
                                          uint64_t remaining_address)
{
    if (task != 0 && task != kernel_cpu_current()->idle &&
        task->magic == KERNEL_THREAD_MAGIC && task->arch.user_mode == 1U) {
        task->syscall_restart = (struct kernel_syscall_restart_state){
            .kind = KERNEL_SYSCALL_RESTART_NANOSLEEP,
            .value.nanosleep = {
                .deadline = deadline,
                .remaining_address = remaining_address,
            },
        };
    }
}

void kernel_signal_note_futex_timed_restart(struct kernel_task *task,
                                            uint64_t address,
                                            uint32_t operation,
                                            uint32_t expected,
                                            uint64_t deadline_ns,
                                            uint32_t bitset)
{
    if (task != 0 && task != kernel_cpu_current()->idle &&
        task->magic == KERNEL_THREAD_MAGIC && task->arch.user_mode == 1U) {
        task->syscall_restart = (struct kernel_syscall_restart_state){
            .kind = KERNEL_SYSCALL_RESTART_FUTEX_TIMED,
            .value.futex_timed = {
                .address = address,
                .deadline_ns = deadline_ns,
                .operation = operation,
                .expected = expected,
                .bitset = bitset,
            },
        };
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

static int signal_dispatchable_locked(const struct kernel_task *task)
{
    return task != 0 && task != kernel_cpu_current()->idle &&
           task->magic == KERNEL_THREAD_MAGIC &&
           task->arch.user_mode == 1U &&
           task->state != KERNEL_THREAD_STATE_ZOMBIE &&
           task->state != KERNEL_THREAD_STATE_EXITED &&
           task->state != KERNEL_THREAD_STATE_GROUP_DEAD;
}

static int signal_dispatchable(const struct kernel_task *task)
{
    /* Running owner keeps its immutable prefix and local execution state;
     * foreign targets still require the shared metadata domain. */
    if (task && arch_current_thread_get() == task && kernel_cpu_current()->current == task)
        return !task->idle && task->magic == KERNEL_THREAD_MAGIC && task->arch.user_mode == 1;

    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    return signal_dispatchable_locked(task);
}

static struct kernel_task *signal_group_leader(
    const struct kernel_task *task)
{
    struct kernel_task *leader;

    if (task == 0 || task == kernel_cpu_current()->idle ||
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

static struct kernel_task *signal_group_representative_get(struct kernel_task *task)
{
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        struct kernel_task *leader = task ? task->group_leader : 0;
        if (!leader || leader->retirement || leader->group_leader != leader) return 0;
        /* 组长可接收时，发现与取得唯一结果借用在同一保护下完成；
         * 不另建只用来再次发现同一组长的游标owner。 */
        if (signal_dispatchable_locked(leader)) {
            scheduler_task_get_locked(leader);
            return leader;
        }
    }
    struct scheduler_group_cursor cursor;
    scheduler_group_cursor_begin(task, &cursor);
    struct kernel_task *result = 0;
    while (cursor.task) {
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            if (cursor.task->group_leader == cursor.leader && signal_dispatchable_locked(cursor.task)) {
                result = cursor.task;
                scheduler_task_get_locked(result);
            }
        }
        if (result) break;
        scheduler_group_cursor_next(&cursor);
    }
    scheduler_group_cursor_end(&cursor);
    return result;
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
    if (arch_direct_map_va_to_pa((uintptr_t)page,
                                  BOAROS_PAGE_SIZE,
                                  &physical_address) !=
        ARCH_DIRECT_MAP_STATUS_OK) {
        return KERNEL_SIGNAL_STATUS_INVALID_STATE;
    }
    (void)physical_page_release(scheduler.allocator, physical_address);
    task->signal_table_address = 0U;
    return KERNEL_SIGNAL_STATUS_OK;
}

static void signal_clear_pending_group(struct kernel_task *task,
                                       uint64_t mask)
{
    struct scheduler_group_cursor cursor;
    scheduler_group_cursor_begin(task, &cursor);
    struct kernel_task *leader = cursor.leader;
    if (!leader) {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        task->signal_pending &= ~mask;
        return;
    }
    { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain); leader->group_pending &= ~mask; }
    while (cursor.task) {
        { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain); cursor.task->signal_pending &= ~mask; }
        scheduler_group_cursor_next(&cursor);
    }
    scheduler_group_cursor_end(&cursor);
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
    child->signal_fault = (struct kernel_signal_fault){0};
    child->signal_pending = 0U;
    child->signal_child_info = (struct kernel_signal_child_info){0};
    child->signal_blocked = parent->signal_blocked;
    child->signal_wait_mask = 0U;
    child->signal_table_address = 0U;
    memset(child->signal_sender, 0, sizeof(child->signal_sender));
    memset(child->signal_code, 0, sizeof(child->signal_code));
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
    child->signal_fault = (struct kernel_signal_fault){0};
    child->signal_pending = 0U;
    child->signal_child_info = (struct kernel_signal_child_info){0};
    child->signal_wait_mask = 0U;
    memset(child->signal_sender, 0, sizeof(child->signal_sender));
    memset(child->signal_code, 0, sizeof(child->signal_code));
    return KERNEL_SIGNAL_STATUS_OK;
}

int kernel_signal_has_pending(const struct kernel_task *task)
{
    uint64_t pending = task->signal_pending;

    if (signal_group_leader(task) != 0) {
        pending |= task->group_leader->group_pending;
    }
    return (pending & (~task->signal_blocked | task->signal_wait_mask)) != 0U;
}

int kernel_signal_termination_requested(const struct kernel_task *task)
{
    return task != 0 && task->terminate_requested != 0U;
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
#if BOAROS_USER_SMP
    struct kernel_task *leader = signal_group_leader(task);
    if ((task->signal_pending | (leader ? leader->group_pending : 0)) & ~task->signal_blocked) {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        scheduler_user_request_locked(task);
    }
#endif
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

enum kernel_signal_status kernel_signal_wait_begin(struct kernel_task *task,
                                                   uint64_t mask)
{
    if (task != kernel_cpu_current()->current || !signal_dispatchable(task) ||
        task->signal_wait_mask != 0U) return KERNEL_SIGNAL_STATUS_INVALID_ARGUMENT;
    task->signal_wait_mask = mask & ~SIGNAL_MASK_KILL_STOP;
    return KERNEL_SIGNAL_STATUS_OK;
}

enum kernel_signal_status kernel_signal_wait_take(
    struct kernel_task *task, struct kernel_signal_wait_info *info)
{
    struct kernel_task *leader;
    uint64_t matching;
    uint32_t sig;
    int shared = 0;

    if (task != kernel_cpu_current()->current || !signal_dispatchable(task) || info == 0)
        return KERNEL_SIGNAL_STATUS_INVALID_ARGUMENT;
    memset(info, 0, sizeof(*info));
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        matching = task->signal_pending & task->signal_wait_mask;
        leader = signal_group_leader(task);
        if (matching == 0U && leader != 0) {
            matching = leader->group_pending & task->signal_wait_mask;
            shared = matching != 0U;
        }
        if (matching == 0U) return KERNEL_SIGNAL_STATUS_OK;
        sig = signal_first_set(matching);
        struct kernel_task *owner = shared ? leader : task;
        if (shared) leader->group_pending &= ~signal_mask(sig);
        else task->signal_pending &= ~signal_mask(sig);
        info->sender = shared ? owner->group_sender[sig - 1] : owner->signal_sender[sig - 1];
        info->code = shared ? owner->group_signal_code[sig - 1] : owner->signal_code[sig - 1];
        if (sig == 17) {
            struct kernel_signal_child_info child = shared ? owner->group_child_info : owner->signal_child_info;
            info->uid = child.uid;
            info->status = child.status;
        }
        info->signal = sig;
    }
    if (shared && sig == 14U) real_timer_rearm(leader);
    return KERNEL_SIGNAL_STATUS_OK;
}

void kernel_signal_wait_end(struct kernel_task *task)
{
    if (task != 0) task->signal_wait_mask = 0U;
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
    task->signal_fault = (struct kernel_signal_fault){0};
    task->signal_restore_mask = 0U;
    task->signal_wait_mask = 0U;
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
    return entry == 0 || (entry->handler != KERNEL_SIGNAL_IGN &&
                          (entry->flags & LINUX_SA_NOCLDSTOP) == 0U);
}

static void signal_send_child(struct kernel_task *parent, struct kernel_task *child,
    int16_t code, int32_t status)
{
    SCHEDULER_TASK_BORROW(representative, signal_group_representative_get(parent));
    struct kernel_signal_child_info info = {.uid = 0, .status = status};
    if (representative)
        (void)signal_send_one(representative, 17, child->tid, 1, code, &info);
}

void kernel_signal_notify_child_exit(struct kernel_task *child)
{
    SCHEDULER_TASK_BORROW(parent, 0);

    if (child == 0) {
        return;
    }
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        parent = child->parent;
        if (parent) scheduler_task_get_locked(parent);
    }
    if (parent != 0 && kernel_signal_wants_sigchld(parent)) {
        uint32_t status = child->wait_status;
        signal_send_child(parent, child,
            (status & 0x7fU) ? ((status & 0x80U) ? 3 : 2) : 1,
            (status & 0x7fU) ? (int32_t)(status & 0x7fU) : (int32_t)((status >> 8) & 0xffU));
    }
    wake_waiting_parent(child);
}

void kernel_signal_notify_child_stop(struct kernel_task *child,
                                     int continued)
{
    SCHEDULER_TASK_BORROW(parent, 0);

    if (child == 0) {
        return;
    }
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        parent = child->parent;
        if (parent) scheduler_task_get_locked(parent);
    }
    if (parent != 0 && signal_wants_sigchld_stop(parent)) {
        signal_send_child(parent, child, continued ? 6 : 5,
            continued ? SIGNAL_CONTINUE : (child->wait_status >> 8) & 0xffU);
    }
    wake_waiting_parent(child);
}

static void signal_resume_stopped(struct kernel_task *task)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    if (task->state != KERNEL_THREAD_STATE_STOPPED) return;
    stopped_unlink_locked(task);
    task->state = KERNEL_THREAD_STATE_READY;
    if (task->wait.on_cpu) task->wait.ready_pending = 1;
    else ready_enqueue_locked(task, 0);
}

static void signal_prepare_group(struct kernel_task *task, uint32_t sig)
{
    struct kernel_task *leader;
    struct scheduler_group_cursor cursor;
    uint64_t clear;
    int continued;

    if (sig == SIGNAL_CONTINUE) {
        clear = SIGNAL_MASK_STOP;
    } else if (sig >= SIGNAL_STOP && sig <= 22U) {
        clear = signal_mask(SIGNAL_CONTINUE);
    } else {
        return;
    }
    scheduler_group_cursor_begin(task, &cursor);
    leader = cursor.leader;
    if (!leader) return;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        continued = leader->group_stopped != 0;
    }
    { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain); leader->group_pending &= ~clear; }
    while (cursor.task) {
        struct kernel_task *member = cursor.task;
        { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain); member->signal_pending &= ~clear; }
        if (sig == SIGNAL_CONTINUE) signal_resume_stopped(member);
        scheduler_group_cursor_next(&cursor);
    }
    if (sig == SIGNAL_CONTINUE) {
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            leader->group_stopped = 0;
            if (continued) {
                leader->stop_notified = 0;
                leader->continue_notified = 1;
            }
        }
        if (continued) kernel_signal_notify_child_stop(leader, 1);
    }
    scheduler_group_cursor_end(&cursor);
}

static int signal_wants_signal_locked(const struct kernel_task *task,
                               uint32_t sig)
{
    if (!signal_dispatchable_locked(task) || task->terminate_requested != 0U ||
        ((task->signal_blocked & signal_mask(sig)) != 0U &&
         (task->signal_wait_mask & signal_mask(sig)) == 0U)) {
        return 0;
    }
    return task->state != KERNEL_THREAD_STATE_STOPPED ||
           sig == SIGNAL_KILL || sig == SIGNAL_CONTINUE;
}

static int signal_wants_signal(const struct kernel_task *task, uint32_t sig)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    return signal_wants_signal_locked(task, sig);
}

static struct kernel_task *signal_choose_group_member_get(struct kernel_task *target, uint32_t sig)
{
    struct scheduler_group_cursor cursor;
    scheduler_group_cursor_begin(target, &cursor);
    struct kernel_task *result = 0;
    while (cursor.task) {
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            if (cursor.task->group_leader == cursor.leader && signal_wants_signal_locked(cursor.task, sig)) {
                result = cursor.task;
                scheduler_task_get_locked(result);
            }
        }
        if (result) break;
        scheduler_group_cursor_next(&cursor);
    }
    scheduler_group_cursor_end(&cursor);
    return result;
}

static int signal_ignored_unblocked(const struct kernel_task *target,
                                    uint32_t sig)
{
    const struct kernel_signal_action *entry;

    if (target != 0 &&
        (target->signal_wait_mask & signal_mask(sig)) != 0U) return 0;
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

/* The caller keeps the target alive by execution or a lookup/cursor borrow.
 * Only a distinct group container needs another cross-unlock storage owner. */
static struct kernel_task *signal_send_leader(struct kernel_task *target,
                                             struct kernel_task **borrowed)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    struct kernel_task *leader = target ? target->group_leader : 0;
    if (!leader || leader->retirement || leader->group_leader != leader) return 0;
    if (leader != target) {
        scheduler_task_get_locked(leader);
        *borrowed = leader;
    }
    return leader;
}

static enum kernel_signal_status signal_send_one(
    struct kernel_task *target, uint32_t sig, kernel_pid_t sender_tid,
    int process_directed, int16_t code,
    const struct kernel_signal_child_info *child_info)
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
    SCHEDULER_TASK_BORROW(group_owner, 0);
    leader = signal_send_leader(target, &group_owner);
    if (leader == 0) {
        return KERNEL_SIGNAL_STATUS_INVALID_STATE;
    }
    signal_prepare_group(target, sig);
    if (sig == SIGNAL_KILL) {
        process_group_request_exit(target, KERNEL_THREAD_EXIT_SIGNAL,
                                   sig, 0U);
        return KERNEL_SIGNAL_STATUS_OK;
    }
    SCHEDULER_TASK_BORROW(selected, 0);
    wake_target = target;
    if (process_directed && !signal_wants_signal(target, sig)) {
        selected = signal_choose_group_member_get(target, sig);
        wake_target = selected;
    }
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        if (signal_ignored_unblocked(wake_target, sig)) return KERNEL_SIGNAL_STATUS_OK;
        pending = process_directed ? &leader->group_pending : &target->signal_pending;
        senders = process_directed ? leader->group_sender : target->signal_sender;
        bit = signal_mask(sig);
        /* 位与载荷共同发布；标准信号合并时保留尚未消费的首个事件。 */
        if ((*pending & bit) != 0U) return KERNEL_SIGNAL_STATUS_OK;
        senders[sig - 1] = (uint32_t)sender_tid;
        if (process_directed) leader->group_signal_code[sig - 1] = code;
        else target->signal_code[sig - 1] = code;
        if (sig == 17) {
            struct kernel_signal_child_info info = child_info ? *child_info : (struct kernel_signal_child_info){0};
            if (process_directed) leader->group_child_info = info;
            else target->signal_child_info = info;
        }
        *pending |= bit;
#if BOAROS_USER_SMP
        scheduler_user_request_locked(wake_target);
#endif
    }
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
    SCHEDULER_TASK_BORROW(representative, signal_group_representative_get(target));

    if (representative == 0) {
        return KERNEL_SIGNAL_STATUS_INVALID_ARGUMENT;
    }
    return signal_send_one(representative, sig, sender_tid, 1, 0, 0);
}

enum kernel_signal_status signal_send_kernel_group(struct kernel_task *target, uint32_t sig)
{
    SCHEDULER_TASK_BORROW(representative, signal_group_representative_get(target));
    return representative ? signal_send_one(representative, sig, 0, 1, 128, 0)
        : KERNEL_SIGNAL_STATUS_INVALID_ARGUMENT;
}

void kernel_signal_timer_expire(void)
{
    uint64_t now = kernel_time_monotonic_ns();
    while (real_timers && real_timers->real_timer_deadline <= now) {
        struct kernel_task *leader = real_timers;
        real_timer_remove(leader);
        SCHEDULER_TASK_BORROW(representative, signal_group_representative_get(leader));
        if (representative && signal_send_one(representative, 14U, 0, 1, 128, 0) != KERNEL_SIGNAL_STATUS_OK)
            __builtin_trap();
    }
}

enum kernel_signal_status kernel_signal_send_task(
    struct kernel_task *target, uint32_t sig, kernel_pid_t sender_tid)
{
    return signal_send_one(target, sig, sender_tid, 0, -6, 0);
}

struct kernel_task *kernel_signal_find_by_tid(kernel_pid_t tid)
{
    struct kernel_task *task = scheduler_task_lookup(tid, KERNEL_PID_TID, 0);
    if (task && !signal_dispatchable(task)) {
        scheduler_task_put(task);
        return 0;
    }
    return task;
}

static uint32_t signal_target_group(struct kernel_task *leader, uint32_t sig,
                                     kernel_pid_t sender, int deliver)
{
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        if (!leader || !leader->accounted || leader->retirement || !leader->identities[KERNEL_PID_TGID].identity)
            return 0;
    }
    /* zombie 仍是可查询的进程；没有可运行成员时信号成功但不入队。 */
    if (!deliver) return 1;
    SCHEDULER_TASK_BORROW(representative, signal_group_representative_get(leader));
    if (!representative) return 1;
    return signal_send_one(representative, sig, sender, 1, 0, 0) == KERNEL_SIGNAL_STATUS_OK;
}

static uint32_t signal_target_identities(struct kernel_task *caller, int64_t pid,
                                         uint32_t sig, kernel_pid_t sender, int deliver)
{
    uint32_t count = 0;
    if (pid > 0) {
        SCHEDULER_TASK_BORROW(target, scheduler_task_lookup((kernel_pid_t)pid, KERNEL_PID_TID, 0));
        return target && target->accounted ?
            signal_target_group(target->group_leader, sig, sender, deliver) : 0;
    }
    if (pid == INT32_MIN) return 0;
    kernel_pid_t group_number = pid ? (kernel_pid_t)-pid : kernel_task_getpgid(caller, 0);
    struct scheduler_task_cursor cursor;
    scheduler_task_cursor_begin(&cursor);
    while (cursor.task) {
        struct kernel_task *task = cursor.task;
        int selected;
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            selected = task->accounted && !task->retirement &&
                task->identities[KERNEL_PID_TGID].identity &&
                (pid == -1 ? task->tid > 1 && task != caller->group_leader :
                 process_identity_number(task, KERNEL_PID_PGID) == group_number);
        }
        if (selected) count += signal_target_group(task, sig, sender, deliver);
        scheduler_task_cursor_next(&cursor);
    }

    return count;
}

uint32_t kernel_signal_resolve_targets(struct kernel_task *caller, int64_t pid)
{
    return signal_target_identities(caller, pid, 0, 0, 0);
}

uint32_t kernel_signal_send_targets(struct kernel_task *caller, int64_t pid,
                                    uint32_t sig, kernel_pid_t sender_tid)
{
    return signal_target_identities(caller, pid, sig, sender_tid, 1);
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
    SCHEDULER_TASK_BORROW(borrow, kernel_signal_find_by_tid(tid));
    target = borrow;
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
    int blocked;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        if (!signal_dispatchable_locked(task) || task->state==KERNEL_THREAD_STATE_STOPPED)
            return;
        task->signal_pending |= bit;
        blocked=task->state==KERNEL_THREAD_STATE_BLOCKED;
    }
    if (blocked && kernel_scheduler_wake_signal(task)!=KERNEL_SCHEDULER_STATUS_OK)
        return;
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    if (!signal_dispatchable_locked(task) || task->state==KERNEL_THREAD_STATE_STOPPED)
        return;
#if BOAROS_USER_SMP
    if (task!=kernel_cpu_current()->current && task->wait.on_cpu) {
        /* 最终提交点复查执行owner：READY可在锁外被CPU1接纳，
         * 停止请求必须交给该运行者，不能丢掉已消费的组信号。 */
        scheduler_user_request_locked(task);
        return;
    }
#endif
    if (task->state == KERNEL_THREAD_STATE_READY) {
        if (task->ready_node.queued) ready_remove_locked(task);
    } else if (task->state != KERNEL_THREAD_STATE_RUNNING ||
               task != kernel_cpu_current()->current) {
        return;
    }
    task->signal_pending &= ~bit;
    task->wait_status = (uint32_t)(sig << 8) | LINUX_WAIT_STOPPED;
    task->stop_notified = 0U;
    task->continue_notified = 0U;
    task->state = KERNEL_THREAD_STATE_STOPPED;
    stopped_append_locked(task);
}

static void signal_stop_group(struct kernel_task *task, uint32_t sig)
{
    struct kernel_task *leader = signal_group_leader(task);
    struct scheduler_group_cursor cursor;
    int all_stopped = 1;

    if (leader == 0) {
        signal_terminate(11U, 1);
    }
    scheduler_group_cursor_begin(task, &cursor);
    while (cursor.task) {
        struct kernel_task *member = cursor.task;
        signal_stop_member(member, sig);
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            if (signal_dispatchable_locked(member) && member->state != KERNEL_THREAD_STATE_STOPPED) all_stopped = 0;
        }
        scheduler_group_cursor_next(&cursor);
    }
    scheduler_group_cursor_end(&cursor);
    int notify = 0;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        leader->wait_status = (uint32_t)(sig << 8) | LINUX_WAIT_STOPPED;
        if (all_stopped && !leader->group_stopped) {
            leader->group_stopped = 1;
            leader->stop_notified = leader->continue_notified = 0;
            notify = 1;
        }
    }
    if (notify) kernel_signal_notify_child_stop(leader, 0);
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
    task->syscall_restart = (struct kernel_syscall_restart_state){
        .kind = KERNEL_SYSCALL_RESTART_NONE,
    };
}

int kernel_signal_nanosleep_restart(const struct kernel_task *task,
                                    uint64_t *deadline,
                                    uint64_t *remaining_address)
{
    if (task->syscall_restart.kind != KERNEL_SYSCALL_RESTART_NANOSLEEP) {
        return 0;
    }
    *deadline = task->syscall_restart.value.nanosleep.deadline;
    *remaining_address =
        task->syscall_restart.value.nanosleep.remaining_address;
    return 1;
}

int kernel_signal_futex_timed_restart(const struct kernel_task *task,
                                      uint64_t *address,
                                      uint32_t *operation,
                                      uint32_t *expected,
                                      uint64_t *deadline_ns,
                                      uint32_t *bitset)
{
    if (task->syscall_restart.kind != KERNEL_SYSCALL_RESTART_FUTEX_TIMED)
        return 0;
    *address = task->syscall_restart.value.futex_timed.address;
    *operation = task->syscall_restart.value.futex_timed.operation;
    *expected = task->syscall_restart.value.futex_timed.expected;
    *deadline_ns = task->syscall_restart.value.futex_timed.deadline_ns;
    *bitset = task->syscall_restart.value.futex_timed.bitset;
    return 1;
}

enum kernel_signal_restart kernel_signal_restart_decide(
    struct kernel_task *task, int has_handler, uint64_t flags)
{
    uint32_t kind = task->syscall_restart.kind;

    if (kind == KERNEL_SYSCALL_RESTART_NONE) {
        return KERNEL_SIGNAL_RESTART_NONE;
    }
    if (!has_handler &&
        (kind == KERNEL_SYSCALL_RESTART_NANOSLEEP ||
         kind == KERNEL_SYSCALL_RESTART_FUTEX_TIMED)) {
        return KERNEL_SIGNAL_RESTART_BLOCK;
    }
    /* Handler syscalls must not inherit an outer restart block. The
     * interrupted register snapshot itself carries a generic retry. */
    kernel_signal_clear_syscall_restart(task);
    if (has_handler && (kind == KERNEL_SYSCALL_RESTART_NANOSLEEP ||
                         kind == KERNEL_SYSCALL_RESTART_FUTEX_TIMED ||
                         (flags & LINUX_SA_RESTART) == 0U)) {
        return KERNEL_SIGNAL_RESTART_INTERRUPTED;
    }
    return KERNEL_SIGNAL_RESTART_RETRY;
}

enum kernel_signal_status kernel_signal_suspend(struct kernel_task *task,
                                                uint64_t mask)
{
    enum kernel_wait_wake_reason reason;

    if (task != kernel_cpu_current()->current || !signal_dispatchable(task)) {
        return KERNEL_SIGNAL_STATUS_INVALID_ARGUMENT;
    }
    task->signal_saved_mask = task->signal_blocked;
    task->signal_restore_mask = 1U;
    task->signal_blocked = mask & ~SIGNAL_MASK_KILL_STOP;
    while (!kernel_signal_has_pending(task) &&
           task->terminate_requested == 0U) {
        if (KERNEL_WAIT_RECHECK(0, 0U, 1, &reason,
                (1)) !=
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

void kernel_signal_force_fault(struct kernel_task *task, uint32_t sig,
    int32_t code, uint64_t address)
{
    KERNEL_SERVICE_SCOPE(service);
    if (task != kernel_cpu_current()->current || !signal_dispatchable(task) ||
        !sig || sig > KERNEL_SIGNAL_COUNT || task->signal_fault.signal) __builtin_trap();
    const struct kernel_signal_action *entry = signal_action_of(task, sig);
    uint64_t mask = signal_mask(sig);
    if ((task->signal_blocked & mask) || (entry && entry->handler == KERNEL_SIGNAL_IGN)) {
        /* 致命同步 fault 不能被阻塞/忽略后无限重试同一指令。 */
        struct kernel_signal_table *table = signal_table_of(task);
        if (table) table->actions[sig - 1] = (struct kernel_signal_action){0};
        task->signal_blocked &= ~mask;
    }
    task->signal_fault = (struct kernel_signal_fault){sig, code, address};
#if BOAROS_USER_SMP
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    scheduler_user_request_locked(task);
#endif
}

enum kernel_signal_select_result kernel_signal_select(
    struct kernel_task *task,
    struct kernel_signal_delivery *delivery)
{
    for (;;) {
        /* 无事件只观察可交付的pending位，不消费载荷；不借用等待者的
         * wait_mask规则。真正取走事件仍在下面同一调度保护内完成。 */
        struct kernel_task *leader = signal_group_leader(task);
        uint64_t observed = task->signal_pending | (leader ? leader->group_pending : 0);
        if (!task->signal_fault.signal && !task->terminate_requested &&
            !task->signal_restore_mask && !(observed & ~task->signal_blocked))
            return KERNEL_SIGNAL_SELECT_NONE;

        struct kernel_signal_fault fault;
        struct kernel_signal_wait_info info = {0};
        uint64_t pending;
        int shared = 0;
        uint32_t sig = 0;
        const struct kernel_signal_action *entry;
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            leader = signal_group_leader(task);
            fault = task->signal_fault;
            pending = fault.signal ? signal_mask(fault.signal) : task->signal_pending & ~task->signal_blocked;
            if (task->terminate_requested) {
                delivery->exit_reason = KERNEL_THREAD_EXIT_SYSCALL;
                delivery->exit_status = delivery->exit_detail = 0;
                return KERNEL_SIGNAL_SELECT_EXIT;
            }
            if (!pending && leader) {
                pending = leader->group_pending & ~task->signal_blocked;
                shared = pending != 0;
            }
            if (pending) {
                sig = signal_first_set(pending);
                if (fault.signal) task->signal_fault = (struct kernel_signal_fault){0};
                else if (shared) leader->group_pending &= ~signal_mask(sig);
                else task->signal_pending &= ~signal_mask(sig);
                info.signal = sig;
                info.sender = shared ? leader->group_sender[sig - 1] : task->signal_sender[sig - 1];
                info.code = fault.signal ? fault.code : shared ? leader->group_signal_code[sig - 1] : task->signal_code[sig - 1];
                if (!fault.signal && sig == 17) {
                    struct kernel_signal_child_info child = shared ? leader->group_child_info : task->signal_child_info;
                    info.uid = child.uid;
                    info.status = child.status;
                }
            }
        }
        if (shared && sig == 14) real_timer_rearm(leader);
        if (pending == 0U) {
            if (task->signal_restore_mask != 0U) {
                enum kernel_wait_wake_reason reason;

                if (KERNEL_WAIT_RECHECK(0, 0U, 1, &reason,
                (1)) !=
                    KERNEL_SCHEDULER_STATUS_OK) {
                    signal_terminate(11U, 1);
                }
                continue;
            }
            return KERNEL_SIGNAL_SELECT_NONE;
        }
        entry = signal_action_of(task, sig);
        if (entry != 0 && entry->handler != KERNEL_SIGNAL_DFL &&
            entry->handler != KERNEL_SIGNAL_IGN) {
            delivery->signal = sig;
            delivery->sender = info.sender;
            delivery->code = info.code;
            delivery->uid = info.uid;
            delivery->status = info.status;
            delivery->fault = fault.signal != 0;
            delivery->fault_address = fault.address;
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
            if (sig != SIGNAL_STOP && process_group_is_orphaned(task)) break;
            signal_stop_group(task, sig);
            continue;
        case KERNEL_SIGNAL_DEFAULT_CORE:
            signal_terminate(sig, 1);
        default:
            signal_terminate(sig, 0);
        }
    }
}
