#include <kernel/credentials.h>
#include <kernel/pidfd.h>
#include <arch/task.h>
#include "private.h"
#include "../signal_pending.h"
#include <kernel/posix_timer.h>

#include <kernel/errno.h>
#include <kernel/irq.h>
#include <kernel/physical_page.h>
#include <kernel/page.h>
#include <kernel/scheduler.h>
#include <kernel/signal.h>
#include <kernel/task.h>
#include <kernel/tty_task.h>
#include <kernel/tick.h>
#include <kernel/time.h>
#include <kernel/uaccess.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SIGNAL_KILL 9U
#define SIGNAL_CONTINUE 18U
#define SIGNAL_STOP 19U
#define LINUX_WAIT_STOPPED 0x7fU

void kernel_signal_encode_info(void *output, const struct kernel_signal_wait_info *info)
{ memcpy(output,info,sizeof(*info)); }


static enum kernel_signal_status signal_send_one(
    struct kernel_task *, uint32_t, kernel_pid_t, int, int16_t,
    const struct kernel_signal_wait_info *, int queued, uint64_t thread_generation);

static struct kernel_task *real_timers;

static uint64_t timer_add(uint64_t a, uint64_t b)
{ return b > UINT64_MAX - a ? UINT64_MAX : a + b; }

static void cpu_timer_sample(struct kernel_task *leader, uint64_t samples[2])
{
    struct kernel_task_usage usage;
    scheduler_group_usage_snapshot(leader, 0, &usage);
    samples[0] = usage.user_ns;
    samples[1] = timer_add(usage.user_ns, usage.kernel_ns);
}

static uint64_t cpu_timer_remaining(uint64_t expires, uint64_t now, int setting)
{
    if (!expires) return 0;
    if (expires < now || (setting && expires == now))
        return UINT64_C(1000000000) / KERNEL_TICKS_PER_SECOND;
    return expires - now;
}
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
    KERNEL_SERVICE_SCOPE(service);
    KERNEL_IRQ_SCOPE(irq);
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

void kernel_signal_timer_get(struct kernel_task *task, uint32_t which,
    uint64_t *remaining, uint64_t *interval)
{
    struct kernel_task *leader = task->group_leader;
    if (which) {
        uint64_t samples[2] = {0};
        if (which > 2) __builtin_trap();
        uint64_t expires = leader->cpu_timers[which - 1].expires;
        if (expires) cpu_timer_sample(leader, samples);
        *remaining = cpu_timer_remaining(expires, samples[which - 1], 0);
        *interval = leader->cpu_timers[which - 1].interval;
        return;
    }
    uint64_t now = kernel_time_monotonic_ns();
    *remaining = leader->real_timer_active ?
        (leader->real_timer_deadline > now ? leader->real_timer_deadline - now : 1000) : 0;
    *interval = leader->real_timer_interval;
}

void kernel_signal_timer_cancel(struct kernel_task *leader)
{
    uintptr_t irq = arch_interrupt_save();
    real_timer_remove(leader);
    leader->real_timer_deadline = leader->real_timer_interval = 0;
    {KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
     memset(leader->cpu_timers, 0, sizeof(leader->cpu_timers));
     scheduler_cpu_timer_update_locked(leader,0);}
    arch_interrupt_restore(irq);
}

void kernel_signal_timer_set(struct kernel_task *task, uint32_t which,
    uint64_t value, uint64_t interval,
    uint64_t *old_remaining, uint64_t *old_interval)
{
    struct kernel_task *leader = task->group_leader;
    if (which) {
        uint64_t samples[2] = {0};
        if (which > 2) __builtin_trap();
        unsigned index = which - 1;
        uint64_t expires = leader->cpu_timers[index].expires;
        if (expires || value) cpu_timer_sample(leader, samples);
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        *old_remaining = cpu_timer_remaining(expires, samples[index], 1);
        *old_interval = leader->cpu_timers[index].interval;
        /* Linux给首次CPU到期加一tick，使采样计数至少前进一次；周期保留原值。 */
        leader->cpu_timers[index].expires = value ? timer_add(samples[index],
            timer_add(value, UINT64_C(1000000000) / KERNEL_TICKS_PER_SECOND)) : 0;
        leader->cpu_timers[index].interval = interval;
        scheduler_cpu_timer_update_locked(leader,0);
        return;
    }
    kernel_signal_timer_get(task, which, old_remaining, old_interval);
    real_timer_remove(leader);
    leader->real_timer_deadline = leader->real_timer_interval = 0;
    if (!value) return;
    uint64_t now = kernel_time_monotonic_ns();
    leader->real_timer_deadline = value > UINT64_MAX - now ? UINT64_MAX : now + value;
    leader->real_timer_interval = interval;
    real_timer_insert(leader);
}

void kernel_signal_timer_adopt(struct kernel_task *task, struct kernel_task *old_leader)
{
    uintptr_t irq = arch_interrupt_save();
    uint32_t active = old_leader->real_timer_active;
    real_timer_remove(old_leader);
    task->real_timer_deadline = old_leader->real_timer_deadline;
    task->real_timer_interval = old_leader->real_timer_interval;
    old_leader->real_timer_deadline = old_leader->real_timer_interval = 0;
    if (active) real_timer_insert(task);
    {KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
     memcpy(task->cpu_timers, old_leader->cpu_timers, sizeof(task->cpu_timers));
     memset(old_leader->cpu_timers, 0, sizeof(old_leader->cpu_timers));
     scheduler_cpu_timer_update_locked(old_leader,0);}
    /* The adopted TGID and its native thresholds are published together later. */
    arch_interrupt_restore(irq);
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
    const uint64_t synchronous=(UINT64_C(1)<<3)|(UINT64_C(1)<<4)|(UINT64_C(1)<<6)|
        (UINT64_C(1)<<7)|(UINT64_C(1)<<10)|(UINT64_C(1)<<30);
    if(bits&synchronous)bits&=synchronous;
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
#if BOAROS_USER_SMP
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        scheduler_signal_request_locked(task);
#endif
    }
}

void kernel_signal_note_nanosleep_restart(struct kernel_task *task,
                                          int32_t clock_id, int absolute,
                                          uint64_t deadline,
                                          uint64_t remaining_address)
{
    if (task != 0 && task != kernel_cpu_current()->idle &&
        task->magic == KERNEL_THREAD_MAGIC && task->arch.user_mode == 1U) {
        task->syscall_restart = (struct kernel_syscall_restart_state){
            .kind = KERNEL_SYSCALL_RESTART_NANOSLEEP,
            .value.nanosleep = {
                .clock_id = clock_id,
                .absolute = absolute,
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
        if (!leader->proc_exiting && signal_dispatchable_locked(leader)) {
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
            if (cursor.task->group_leader == cursor.leader && !cursor.task->proc_exiting && signal_dispatchable_locked(cursor.task)) {
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
    kernel_wait_queue_init(&table->signalfd_waiters);
    /* 远端发送者在同一保护下取得已完整初始化的信号状态。 */
    { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
      task->signal_table_address = (uintptr_t)page; }
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
    kernel_wait_queue_pollfree(&table->signalfd_waiters);
    for (;;) {
        enum kernel_scheduler_status status=kernel_wait_queue_destroy(&table->signalfd_waiters);
        if (!status) break;
        if (status!=KERNEL_SCHEDULER_STATUS_BUSY) __builtin_trap();
        kernel_wait_backend_quiesce();
    }
    (void)physical_page_release(scheduler.allocator, physical_address);
    task->signal_table_address = 0U;
    return KERNEL_SIGNAL_STATUS_OK;
}

static void clear_task_pending(struct kernel_task *task,int shared,uint64_t mask,int ignored)
{
    for(;;){struct signal_pending_prepared p={0};int more;
        {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);more=ignored?signal_pending_ignore_locked(task,shared,mask,&p):signal_pending_clear_locked(task,shared,mask,&p);}
        signal_pending_finish(&p);if(!more)break;
    }
}
static void signal_clear_pending_group(struct kernel_task *task,uint64_t mask)
{
    struct scheduler_group_cursor cursor;scheduler_group_cursor_begin(task,&cursor);
    if(!cursor.leader)clear_task_pending(task,0,mask,1);
    else {
        clear_task_pending(cursor.leader,1,mask,1);
        while(cursor.task){clear_task_pending(cursor.task,0,mask,1);scheduler_group_cursor_next(&cursor);}
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
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    entry = signal_action_of(task, sig);
    action->handler = entry == 0 ? KERNEL_SIGNAL_DFL : entry->handler;
    action->flags = entry == 0 ? 0U : entry->flags;
    action->mask = entry == 0 ? 0U : entry->mask;
    return KERNEL_SIGNAL_STATUS_OK;
}

enum kernel_signal_status kernel_signal_set_action(
    struct kernel_task *task,
    uint32_t sig,
    const struct kernel_linux_sigaction *action,
    struct kernel_linux_sigaction *old_action)
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
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        table = signal_table_of(task);
        if (table == 0) return KERNEL_SIGNAL_STATUS_INVALID_STATE;
        entry = &table->actions[sig - 1U];
        if (old_action) *old_action=(struct kernel_linux_sigaction){entry->handler,entry->flags,entry->mask};
        *entry=(struct kernel_signal_action){action->handler,action->flags,action->mask & ~SIGNAL_MASK_KILL_STOP};
    }
    if (action->handler == KERNEL_SIGNAL_IGN ||
        (action->handler == KERNEL_SIGNAL_DFL &&
         signal_default_action(sig) == KERNEL_SIGNAL_DEFAULT_IGNORE)) {
        signal_clear_pending_group(task, signal_mask(sig));
    }
    if(task->group_leader->posix_timers&&action->handler!=KERNEL_SIGNAL_IGN&&
        !(action->handler==KERNEL_SIGNAL_DFL&&signal_default_action(sig)==KERNEL_SIGNAL_DEFAULT_IGNORE))
        kernel_posix_timer_unignore(task,sig);
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
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        child_table->magic=KERNEL_SIGNAL_TABLE_MAGIC;
        memcpy(child_table->actions,parent_table->actions,sizeof(child_table->actions));
    }
    child_table->references = 1U;
    kernel_wait_queue_init(&child_table->signalfd_waiters);
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
    uint64_t pending = __atomic_load_n(&task->signal_pending,__ATOMIC_RELAXED);

    if (signal_group_leader(task) != 0) {
        pending |= __atomic_load_n(&task->group_leader->group_pending,__ATOMIC_RELAXED);
    }
    return (pending & (~__atomic_load_n(&task->signal_blocked,__ATOMIC_RELAXED) |
                        __atomic_load_n(&task->signal_wait_mask,__ATOMIC_RELAXED))) != 0U;
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
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    *blocked = task->signal_blocked;
    return KERNEL_SIGNAL_STATUS_OK;
}

static void signal_retarget_shared(struct kernel_task *task,uint64_t pending);

static uint64_t signal_set_blocked_locked(struct kernel_task *task,uint64_t mask)
{
    uint64_t old=task->signal_blocked;
    task->signal_blocked=mask & ~SIGNAL_MASK_KILL_STOP;
    struct kernel_task *leader=signal_group_leader(task);
#if BOAROS_USER_SMP
    if ((task->signal_pending | (leader ? leader->group_pending : 0)) & ~task->signal_blocked)
        scheduler_signal_request_locked(task);
#endif
    return leader && leader->group_members>1 ? leader->group_pending & task->signal_blocked & ~old : 0;
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
    uint64_t retarget;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        if (old_mask) *old_mask=task->signal_blocked;
        if (!new_mask) return KERNEL_SIGNAL_STATUS_OK;
        mask=*new_mask;
        if (how==LINUX_SIG_BLOCK) mask|=task->signal_blocked;
        else if (how==LINUX_SIG_UNBLOCK) mask=task->signal_blocked & ~mask;
        retarget=signal_set_blocked_locked(task,mask);
    }
    signal_retarget_shared(task,retarget);
    return KERNEL_SIGNAL_STATUS_OK;
}

enum kernel_signal_status kernel_signal_get_pending(
    const struct kernel_task *task,
    uint64_t *pending)
{
    if (task == 0 || pending == 0 || !signal_dispatchable(task)) {
        return KERNEL_SIGNAL_STATUS_INVALID_ARGUMENT;
    }
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
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
    if (task != kernel_cpu_current()->current || !signal_dispatchable(task))
        return KERNEL_SIGNAL_STATUS_INVALID_ARGUMENT;
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    if (task->signal_wait_mask) return KERNEL_SIGNAL_STATUS_INVALID_ARGUMENT;
    task->signal_wait_mask = mask & ~SIGNAL_MASK_KILL_STOP;
    return KERNEL_SIGNAL_STATUS_OK;
}

static uint32_t signal_take_locked(struct kernel_task *task,uint64_t mask,
    struct kernel_signal_wait_info *info,struct signal_pending_prepared *prepared,
    struct kernel_task **rearm)
{
    uint64_t matching=task->signal_pending & mask;
    struct kernel_task *leader=signal_group_leader(task);
    int shared=0;
    if (!matching && leader) { matching=leader->group_pending & mask;shared=!!matching; }
    if (!matching) return 0;
    uint32_t sig=signal_first_set(matching);
    signal_pending_take_locked(shared?leader:task,shared,sig,info,prepared);
    if (shared && sig==14U) *rearm=leader;
    return info->signal?sig:KERNEL_SIGNAL_COUNT+1;
}
enum kernel_signal_status kernel_signal_wait_take(
    struct kernel_task *task, struct kernel_signal_wait_info *info)
{
    if (task!=kernel_cpu_current()->current || !signal_dispatchable(task) || !info)
        return KERNEL_SIGNAL_STATUS_INVALID_ARGUMENT;
    for(;;) {
        struct signal_pending_prepared p={0};struct kernel_task *rearm=0;uint32_t sig;
        memset(info,0,sizeof(*info));
        {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);sig=signal_take_locked(task,task->signal_wait_mask,info,&p,&rearm);}
        signal_pending_finish(&p);
        if(rearm)real_timer_rearm(rearm);
        if(sig<=KERNEL_SIGNAL_COUNT)return KERNEL_SIGNAL_STATUS_OK;
    }
}

struct kernel_wait_queue *kernel_signal_fd_source(struct kernel_task *task)
{
    if (signal_alloc_table(task)) return 0;
    return &signal_table_of(task)->signalfd_waiters;
}
uint64_t kernel_signal_fd_pending_locked(struct kernel_task *task,uint64_t mask)
{
    struct kernel_task *leader=signal_group_leader(task);
    return (task->signal_pending | (leader?leader->group_pending:0)) & mask;
}
int kernel_signal_fd_take(struct kernel_task *task,const uint64_t *mask,
    struct kernel_signal_wait_info *info,int nonblock)
{
    struct kernel_wait_queue *queue=kernel_signal_fd_source(task);
    if (!queue) return -KERNEL_ENOMEM;
    for (;;) {
        struct signal_pending_prepared p __attribute__((cleanup(signal_pending_finish)))={0};
        struct kernel_task *rearm=0;struct kernel_wait_token token={0};uint32_t sig;
        {
            KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
            sig=signal_take_locked(task,*mask,info,&p,&rearm);
            if (!sig) {
                if (nonblock) return -KERNEL_EAGAIN;
                COST_ADD(SIGNAL_FD_PARKS,1);
                if (kernel_wait_prepare_locked(queue,0,1,&token)) __builtin_trap();
                kernel_wait_default_node(task)->event_mask=mask;
            }
        }
        signal_pending_finish(&p);
        if (rearm) real_timer_rearm(rearm);
        if(sig>KERNEL_SIGNAL_COUNT)continue;
        if (sig) return 0;
        enum kernel_wait_wake_reason reason;
        if (kernel_wait_park(&token,&reason) || kernel_wait_finish(&token)) __builtin_trap();
        if (reason==KERNEL_WAIT_SIGNALLED) return -KERNEL_ERESTARTSYS;
    }
}

void kernel_signal_wait_end(struct kernel_task *task)
{
    if (!task) return;
    uint64_t retarget=0;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        struct kernel_task *leader=signal_group_leader(task);
        /* 等待集合曾让屏蔽信号选择本线程；退出时交还尚未消费的部分。 */
        if (leader && leader->group_members>1)
            retarget=leader->group_pending & task->signal_wait_mask & task->signal_blocked;
        task->signal_wait_mask=0;
    }
    signal_retarget_shared(task,retarget);
}

void kernel_signal_reset_on_exec(struct kernel_task *task)
{
    kernel_posix_timers_exec(task);
    signal_pending_flush_timers(task);
    kernel_signal_stack_reset(task);
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
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
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    const struct kernel_signal_action *entry =
        parent == 0 ? 0 : signal_action_of(parent, 17U);

    /* A blocked default-ignored SIGCHLD is still observable via pending. */
    return parent != 0 &&
           (entry == 0 || entry->handler != KERNEL_SIGNAL_IGN);
}

int kernel_signal_child_autoreap(const struct kernel_task *parent)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    const struct kernel_signal_action *entry =
        parent == 0 ? 0 : signal_action_of(parent, 17U);

    return entry != 0 && (entry->handler == KERNEL_SIGNAL_IGN ||
                          (entry->flags & LINUX_SA_NOCLDWAIT) != 0U);
}

static int signal_wants_sigchld_stop(const struct kernel_task *parent)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
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
    struct kernel_signal_wait_info info = {.signal=17,.sender=(uint32_t)child->tid,.code=code,.status=status};
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        info.uid=kernel_credentials_task_locked(child)->uid;
        scheduler_task_cpu_time_locked(child,&info.user_time,&info.system_time);
        if(code>=1 && code<=3) {
            info.user_time=timer_add(info.user_time,child->dead_usage.user_ns);
            info.system_time=timer_add(info.system_time,child->dead_usage.kernel_ns);
        }
        info.user_time/=UINT64_C(10000000);info.system_time/=UINT64_C(10000000);
    }
    if (representative)
        (void)signal_send_one(parent, 17, child->tid, 1, code, &info, 0, 0);
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
#if BOAROS_USER_SMP
    struct kernel_task *leader=signal_group_leader(task);
    if ((task->signal_pending | (leader ? leader->group_pending : 0)) & ~task->signal_blocked)
        scheduler_signal_request_locked(task);
#endif
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
    clear_task_pending(leader,1,clear,0);
    while (cursor.task) {
        struct kernel_task *member = cursor.task;
        clear_task_pending(member,0,clear,0);
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

static uint64_t signal_receivable_locked(const struct kernel_task *task)
{
    if (!signal_dispatchable_locked(task) || task->terminate_requested || task->proc_exiting) return 0;
    uint64_t mask=~task->signal_blocked | task->signal_wait_mask;
    if (task->state==KERNEL_THREAD_STATE_STOPPED)
        mask&=signal_mask(SIGNAL_KILL)|signal_mask(SIGNAL_CONTINUE);
    return mask;
}

static int signal_wants_signal_locked(const struct kernel_task *task,uint32_t sig)
{
    return (signal_receivable_locked(task) & signal_mask(sig))!=0;
}

static void signal_retarget_shared(struct kernel_task *task,uint64_t pending)
{
    if (!pending) return;
    struct scheduler_group_cursor cursor;
    scheduler_group_cursor_begin(task,&cursor);
    while (cursor.task && pending) {
        struct kernel_task *member=cursor.task;
        uint64_t accepted=0;
        {
            KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
            pending&=cursor.leader->group_pending;
            if (member->group_leader==cursor.leader) {
                accepted=pending & cursor.leader->group_pending & signal_receivable_locked(member);
                if (accepted) {
                    pending&=~accepted;
#if BOAROS_USER_SMP
                    scheduler_signal_request_locked(member);
#endif
                }
            }
        }
        /* 游标持有真实task存储；唤醒沿原等待登记，使睡眠成员也能消费组信号。 */
        if (accepted && kernel_scheduler_wake_signal(member)!=KERNEL_SCHEDULER_STATUS_OK)
            __builtin_trap();
        scheduler_group_cursor_next(&cursor);
    }
    scheduler_group_cursor_end(&cursor);
}

void kernel_signal_task_exit(struct kernel_task *task)
{
    kernel_posix_cpu_target_exit(task);
    uint64_t pending=0;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        struct kernel_task *leader=signal_group_leader(task);
        if (leader && leader->group_members>1) pending=leader->group_pending;
    }
    signal_retarget_shared(task,pending);
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

static int signal_permitted_locked(struct kernel_task *caller,struct kernel_task *target,unsigned sig)
{
    if(!caller || caller->group_leader==target->group_leader)return 1;
    const struct kernel_credentials *from=kernel_credentials_task_locked(caller),
        *to=kernel_credentials_task_locked(target);
    if(from->uid==to->uid || from->uid==to->suid || from->euid==to->uid || from->euid==to->suid ||
        kernel_credentials_capable(from,KERNEL_CAP_KILL))return 1;
    return sig==SIGNAL_CONTINUE &&
        process_identity(caller,KERNEL_PID_SID)==process_identity(target,KERNEL_PID_SID);
}

static enum kernel_signal_status signal_send_one(
    struct kernel_task *target, uint32_t sig, kernel_pid_t sender_tid,
    int process_directed, int16_t code,
    const struct kernel_signal_wait_info *provided, int queued, uint64_t thread_generation)
{
    if (!target || !sig || sig>KERNEL_SIGNAL_COUNT) return KERNEL_SIGNAL_STATUS_INVALID_ARGUMENT;
    struct kernel_task *executing=kernel_task_current();
    struct kernel_task *sender=(sender_tid || provided) && code<=0?executing:0;
    struct signal_pending_prepared p __attribute__((cleanup(signal_pending_finish)))={0};
    struct kernel_signal_wait_info info={.signal=sig,.sender=(uint32_t)sender_tid,.code=code};
    struct kernel_pid *identity __attribute__((cleanup(kernel_task_identity_end)))=0;
    SCHEDULER_TASK_BORROW(group_owner,0);
    if(process_directed || target!=executing) {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        /* Authorize and acquire the identity/container in the same snapshot.
         * Allocation may sleep, so publication still rechecks permission. */
        if(sender && !signal_permitted_locked(sender,target,sig))
            return KERNEL_SIGNAL_STATUS_PERMISSION;
        struct kernel_task *leader=target->group_leader;
        if(!leader || leader->retirement || leader->group_leader!=leader)
            return KERNEL_SIGNAL_STATUS_INVALID_STATE;
        if(process_directed) {
            identity=process_identity(leader,KERNEL_PID_TGID);
            if(!identity)return KERNEL_SIGNAL_STATUS_INVALID_STATE;
            kernel_pid_get(identity);
        } else if(sig>=32 || queued || thread_generation) {
            struct kernel_pid *id=process_identity(target,KERNEL_PID_TID);
            if(!id || (thread_generation && thread_generation!=id->generation))
                return KERNEL_SIGNAL_STATUS_INVALID_STATE;
            thread_generation=id->generation;
        }
        /* The executing member already keeps its group container alive.
         * A borrowed remote task only protects its storage, so retain its
         * distinct group owner across unlocked preparation. */
        if(leader!=target && target!=executing) {
            scheduler_task_get_locked(leader);group_owner=leader;
        }
    }
    /* The executing thread cannot change its TID/group or credentials
     * until this call returns; no extra container or identity owner is needed.
     * A supplied generation still identifies the original queued target. */
    if(!process_directed && target==executing && thread_generation) {
        struct kernel_pid *id=process_identity(target,KERNEL_PID_TID);
        if(!id || thread_generation!=id->generation)
            return KERNEL_SIGNAL_STATUS_INVALID_STATE;
    }
    signal_prepare_group(target,sig);
    if(sig==SIGNAL_KILL) {process_group_request_exit(target,KERNEL_THREAD_EXIT_SIGNAL,sig,0);return KERNEL_SIGNAL_STATUS_OK;}
    if(provided)info=*provided;
    else if(sender)info.uid=kernel_credentials_current()->uid;
    int prepared=0,needs_prepare=sig>=32 || queued || info.uid;
    for (;;) {
        SCHEDULER_TASK_BORROW(group,0);
        SCHEDULER_TASK_BORROW(receiver,0);
        struct kernel_task *actual=target;
        if(process_directed) {
            {
                KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
                group=identity->members[KERNEL_PID_TGID]?identity->members[KERNEL_PID_TGID]->task:0;
                if(group)scheduler_task_get_locked(group);
            }
            if(!group)return KERNEL_SIGNAL_STATUS_INVALID_STATE;
            receiver=signal_group_representative_get(group);
            if(!receiver)return KERNEL_SIGNAL_STATUS_OK;
            actual=receiver;
        }
        if(!prepared && needs_prepare) {
            struct kernel_heap *heap;
            {KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);heap=actual->proc_exiting?0:actual->files.heap;}
            /* The group container may already have exited. A live receiver's
             * root heap supplies candidates; the container only owns state. */
            signal_pending_prepare(process_directed?group:actual,process_directed,target,heap,&info,1,&p);
            prepared=1;
        }
        SCHEDULER_TASK_BORROW(selected,0);
        struct kernel_task *wake_target=actual;
        if(process_directed&&!signal_wants_signal(actual,sig))wake_target=selected=signal_choose_group_member_get(actual,sig);
        uint64_t retarget=0;struct kernel_wait_cursor fd_cursor={0};
        {
            KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
            /* Allocation can sleep through representative exit or nonleader
             * exec. Re-resolve the same held identity, never a recycled PID. */
            if(process_directed && (actual->proc_exiting || !signal_dispatchable_locked(actual) || actual->group_leader!=group ||
                !identity->members[KERNEL_PID_TGID] || identity->members[KERNEL_PID_TGID]->task!=group))continue;
            if(!process_directed) {
                /* 任务借用保活存储；代次核对识别分配期间 exec 更换
                 * TID，无需为不跨解锁访问的 PID 对象另增引用。 */
                struct kernel_pid *id=process_identity(actual,KERNEL_PID_TID);
                if(thread_generation && (!id || id->generation!=thread_generation))
                    return KERNEL_SIGNAL_STATUS_INVALID_STATE;
                if(!signal_dispatchable_locked(actual))return KERNEL_SIGNAL_STATUS_INVALID_STATE;
                if(actual->proc_exiting)return KERNEL_SIGNAL_STATUS_OK;
            }
            if(process_directed && (!wake_target || wake_target->group_leader!=group || !signal_wants_signal_locked(wake_target,sig))) {
                wake_target=0;retarget=signal_mask(sig);
            }
            /* 接收者只决定唤醒谁；权限和 UID 额度始终使用原始目标线程。 */
            if(sender && sender!=target && !signal_permitted_locked(sender,target,sig))return KERNEL_SIGNAL_STATUS_PERMISSION;
            if(signal_ignored_unblocked(wake_target,sig))return KERNEL_SIGNAL_STATUS_OK;
            /* 改UID后，已有内联事件仍属于旧账户。仅这种混合账户
             * 需要在锁外补备记录；同账户标准信号复用当前发布保护。 */
            if(!prepared && signal_pending_needs_record_locked(process_directed?group:actual,process_directed,target,&info,queued)) {
                needs_prepare=1;continue;
            }
            int posted=signal_pending_post_locked(process_directed?group:actual,process_directed,target,&info,queued,&p);
            if(posted<0)return KERNEL_SIGNAL_STATUS_QUEUE_FULL;
            if(posted>0)return KERNEL_SIGNAL_STATUS_OK;
            struct kernel_signal_table *table=signal_table_of(actual);
            if(table)(void)kernel_wait_cursor_prepare_event_locked(&table->signalfd_waiters,&fd_cursor,signal_mask(sig),process_directed?0:actual);
#if BOAROS_USER_SMP
            scheduler_signal_request_locked(wake_target);
#endif
        }
        if(fd_cursor.queue)kernel_wait_cursor_wake(&fd_cursor,1);
        signal_retarget_shared(actual,retarget);
        /* Publication already requests local signal handling. The current
         * executor is running here and needs no external wait-token wake. */
        if(wake_target && wake_target!=executing && signal_wants_signal(wake_target,sig) &&
           kernel_scheduler_wake_signal(wake_target))return KERNEL_SIGNAL_STATUS_INVALID_STATE;
        return KERNEL_SIGNAL_STATUS_OK;
    }
}

static struct kernel_task *timer_receiver_locked(struct kernel_pid *id,int thread,unsigned sig,struct kernel_task **wake)
{
    enum kernel_pid_role role=thread?KERNEL_PID_TID:KERNEL_PID_TGID;
    struct kernel_task *target=id->members[role]?id->members[role]->task:0;
    if(!target)return 0;
    struct kernel_task *actual=0;*wake=0;
    if(thread){if(signal_dispatchable_locked(target)&&!target->proc_exiting)actual=target;}
    else {
        struct kernel_task *cursor=target;
        do {
            if(signal_dispatchable_locked(cursor)&&!cursor->proc_exiting){
                if(!actual)actual=cursor;
                if(!*wake&&signal_wants_signal_locked(cursor,sig))*wake=cursor;
            }
            cursor=cursor->group_next;
        }while(cursor!=target);
    }
    if(thread&&actual&&signal_wants_signal_locked(actual,sig))*wake=actual;
    return actual;
}
int signal_timer_disposition_ignored_locked(struct kernel_pid *id,int thread,unsigned sig)
{
    struct kernel_task *wake=0,*actual=timer_receiver_locked(id,thread,sig,&wake);
    if(!actual)return -1;
    const struct kernel_signal_action *action=signal_action_of(actual,sig);
    return (action&&action->handler==KERNEL_SIGNAL_IGN)||
        ((!action||action->handler==KERNEL_SIGNAL_DFL)&&(signal_default_action(sig)==KERNEL_SIGNAL_DEFAULT_IGNORE||signal_default_action(sig)==KERNEL_SIGNAL_DEFAULT_CONTINUE));
}
int signal_timer_post_locked(struct kernel_pid *id,int thread,struct signal_queue_record *record,
    struct kernel_wait_cursor *cursor,struct kernel_task **wake)
{
    struct kernel_task *actual=timer_receiver_locked(id,thread,record->info.signal,wake);
    if(!actual){*wake=0;return 0;}
    unsigned sig=record->info.signal;
    int control=sig==SIGNAL_KILL||sig==SIGNAL_CONTINUE||(sig>=SIGNAL_STOP&&sig<=22U);
    int ignored=signal_ignored_unblocked(*wake,sig);
    /* Group effects apply even when CONT has no pending handler. Keep the
     * actual identity's storage until the unlocked group traversal ends. */
    if(control&&!*wake)*wake=actual;
    if(ignored||sig==SIGNAL_KILL) {
        if(control)scheduler_task_get_locked(*wake);else *wake=0;
        return ignored?2:0;
    }
    struct kernel_task *pending=thread?actual:actual->group_leader;
    signal_pending_post_timer_locked(pending,!thread,record);
    struct kernel_signal_table *table=signal_table_of(actual);
    if(table)(void)kernel_wait_cursor_prepare_event_locked(&table->signalfd_waiters,cursor,signal_mask(record->info.signal),thread?actual:0);
    if(*wake){scheduler_task_get_locked(*wake);
#if BOAROS_USER_SMP
        scheduler_signal_request_locked(*wake);
#endif
    }
    return 1;
}
void signal_timer_notify(struct kernel_task *task,unsigned sig)
{
    signal_prepare_group(task,sig);
    if(sig==SIGNAL_KILL)process_group_request_exit(task,KERNEL_THREAD_EXIT_SIGNAL,sig,0);
    else (void)kernel_scheduler_wake_signal(task);
}
enum kernel_signal_status kernel_signal_send(struct kernel_task *target,
                                             uint32_t sig,
                                             kernel_pid_t sender_tid)
{
    SCHEDULER_TASK_BORROW(representative, signal_group_representative_get(target));

    if (representative == 0) {
        return KERNEL_SIGNAL_STATUS_INVALID_ARGUMENT;
    }
    return signal_send_one(target, sig, sender_tid, 1, 0, 0, 0, 0);
}

int kernel_signal_queue_info(int32_t group,int32_t pid,int thread,
    const struct kernel_signal_wait_info *info)
{
    SCHEDULER_TASK_BORROW(target,0);
    uint64_t generation;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        target=scheduler_task_lookup_locked(pid,KERNEL_PID_TID,0);
        if(!target)return -KERNEL_ESRCH;
        generation=process_identity(target,KERNEL_PID_TID)->generation;
        if(!target->group_leader || (thread && target->group_leader->tid!=group))return -KERNEL_ESRCH;
        if(info->signal>KERNEL_SIGNAL_COUNT)return -KERNEL_EINVAL;
        if(!signal_permitted_locked(kernel_task_current(),target,info->signal))return -KERNEL_EPERM;
        if(!info->signal || (thread && !signal_dispatchable_locked(target)))return 0;
    }
    enum kernel_signal_status status;
    if(thread)status=signal_send_one(target,info->signal,info->sender,0,info->code,info,1,generation);
    else {
        SCHEDULER_TASK_BORROW(representative,signal_group_representative_get(target));
        /* A live zombie identity accepts the signal without creating a record. */
        status=representative?signal_send_one(target,info->signal,info->sender,1,info->code,info,1,0)
            :KERNEL_SIGNAL_STATUS_OK;
    }
    return status==KERNEL_SIGNAL_STATUS_OK?0:status==KERNEL_SIGNAL_STATUS_PERMISSION?-KERNEL_EPERM:status==KERNEL_SIGNAL_STATUS_QUEUE_FULL?-KERNEL_EAGAIN:-KERNEL_ESRCH;
}

enum kernel_signal_status signal_send_kernel_group(struct kernel_task *target, uint32_t sig)
{
    SCHEDULER_TASK_BORROW(representative, signal_group_representative_get(target));
    return representative ? signal_send_one(target, sig, 0, 1, 128, 0, 0, 0)
        : KERNEL_SIGNAL_STATUS_INVALID_ARGUMENT;
}

unsigned signal_cpu_timer_check_locked(struct kernel_task *leader,const struct kernel_task_usage *usage)
{
    /* Sleep does not turn a periodic timer's old CPU debt into wall-clock work. */
    int running=0;
    for(struct kernel_cpu *cpu=scheduler.cpus;cpu;cpu=cpu->scheduler_next) {
        struct kernel_task *owner=cpu->accounting_owner;
        if(owner&&!owner->idle&&owner->group_leader==leader){running=1;break;}
    }
    if(!running)return 0;
    uint64_t samples[2]={usage->user_ns,timer_add(usage->user_ns,usage->kernel_ns)};
    unsigned fired=0;
    for(unsigned i=0;i<2;i++) {
        uint64_t expires=leader->cpu_timers[i].expires;
        if(!expires||samples[i]<expires)continue;
        uint64_t interval=leader->cpu_timers[i].interval;
        leader->cpu_timers[i].expires=interval?timer_add(expires,interval):0;
        fired|=1U<<i;
    }
    uint64_t soft=leader->limits->values[KERNEL_RLIMIT_CPU].current,hard=leader->limits->values[KERNEL_RLIMIT_CPU].maximum;
    if(soft!=KERNEL_RLIM_INFINITY) {
        /* RLIMIT_CPU和itimers共用本次整组PROF样本；硬限制先于软信号。 */
        if(hard!=KERNEL_RLIM_INFINITY && samples[1]>=hard*UINT64_C(1000000000))
            return 8U;
        if(samples[1]>=soft*UINT64_C(1000000000)) {
            __atomic_store_n(&leader->limits->values[KERNEL_RLIMIT_CPU].current,soft+1,__ATOMIC_RELAXED);
            fired|=4U;
        }
    }
    scheduler_cpu_timer_update_locked(leader,0);
    return fired;
}
void signal_cpu_timer_notify(struct kernel_task *leader,unsigned fired)
{
    if(!fired)return;
    SCHEDULER_TASK_BORROW(representative,signal_group_representative_get(leader));
    if(!representative)return;
    if(fired&8U){signal_timer_notify(leader,9U);return;}
    for(unsigned i=0;i<2;i++)if((fired&(1U<<i))&&
        signal_send_one(leader,26U+i,0,1,128,0,0,0)!=KERNEL_SIGNAL_STATUS_OK)__builtin_trap();
    if((fired&4U)&&signal_send_one(leader,24U,0,1,128,0,0,0)!=KERNEL_SIGNAL_STATUS_OK)__builtin_trap();
}
void kernel_signal_timer_expire(void)
{
    if(real_timers) {
        uint64_t now=kernel_time_monotonic_ns();
        while(real_timers&&real_timers->real_timer_deadline<=now) {
            struct kernel_task *leader=real_timers;real_timer_remove(leader);
            SCHEDULER_TASK_BORROW(representative,signal_group_representative_get(leader));
            if(representative&&signal_send_one(leader,14U,0,1,128,0,0,0)!=KERNEL_SIGNAL_STATUS_OK)__builtin_trap();
        }
    }
    scheduler_cpu_timer_expire();
}

enum kernel_signal_status kernel_signal_send_task(
    struct kernel_task *target, uint32_t sig, kernel_pid_t sender_tid)
{
    return signal_send_one(target, sig, sender_tid, 0, -6, 0, 0, 0);
}

enum kernel_signal_status kernel_signal_send_file_limit(struct kernel_task *target)
{
    kernel_pid_t sender;
    if (kernel_task_tgid(target, &sender) != KERNEL_TASK_STATUS_OK)
        return KERNEL_SIGNAL_STATUS_INVALID_STATE;
    return signal_send_one(target, 25U, sender, 0, 0, NULL, 0, 0);
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

static int64_t signal_target_group(struct kernel_task *caller,struct kernel_task *target, uint32_t sig,
                                     kernel_pid_t sender, int deliver)
{
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        struct kernel_task *leader=target?target->group_leader:0;
        if (!target || !target->accounted || target->retirement || !leader ||
            !leader->accounted || leader->retirement || !leader->identities[KERNEL_PID_TGID].identity)
            return 0;
        if(!signal_permitted_locked(caller,target,sig))return -KERNEL_EPERM;
    }
    /* zombie 仍是可查询的进程；没有可运行成员时信号成功但不入队。 */
    if (!deliver) return 1;
    SCHEDULER_TASK_BORROW(representative, signal_group_representative_get(target));
    if (!representative) return 1;
    enum kernel_signal_status status=signal_send_one(target,sig,sender,1,0,0,0,0);
    return status==KERNEL_SIGNAL_STATUS_OK?1:status==KERNEL_SIGNAL_STATUS_PERMISSION?-KERNEL_EPERM:0;
}

static int64_t signal_target_identities(struct kernel_task *caller, int64_t pid,
                                         uint32_t sig, kernel_pid_t sender, int deliver)
{
    int64_t count = 0;int denied=0;
    if (pid > 0) {
        SCHEDULER_TASK_BORROW(target, scheduler_task_lookup((kernel_pid_t)pid, KERNEL_PID_TID, 0));
        return target && target->accounted ?
            signal_target_group(caller,target, sig, sender, deliver) : 0;
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
        if (selected) {
            int64_t result=signal_target_group(caller,task,sig,sender,deliver);
            if(result<0)denied=1;else count+=result;
        }
        scheduler_task_cursor_next(&cursor);
    }

    return count?count:denied?-KERNEL_EPERM:0;
}

int64_t kernel_signal_resolve_targets(struct kernel_task *caller, int64_t pid)
{
    return signal_target_identities(caller, pid, 0, 0, 0);
}

int64_t kernel_signal_send_targets(struct kernel_task *caller, int64_t pid,
                                    uint32_t sig, kernel_pid_t sender_tid)
{
    return signal_target_identities(caller, pid, sig, sender_tid, 1);
}

static int64_t signal_identity_target(struct kernel_pid *id,unsigned role,
    struct kernel_task *selected,uint32_t sig,const struct kernel_signal_wait_info *supplied)
{
    SCHEDULER_TASK_BORROW(target,0);
    struct kernel_task *caller=kernel_task_current();
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        if(selected) {
            if(selected->retirement || process_identity(selected,KERNEL_PID_PGID)!=id ||
                !selected->identities[KERNEL_PID_TGID].identity)return -KERNEL_ESRCH;
            target=selected;
        } else {
            /* A THREAD pidfd still names its actual TID when the caller
             * selects group-directed delivery. Scope changes queue choice,
             * not the retained identity's lookup role. */
            struct kernel_pid_member *member=id->members[KERNEL_PID_TID];
            if(!member)return -KERNEL_ESRCH;
            target=member->task;
        }
        scheduler_task_get_locked(target);
        if(!signal_permitted_locked(caller,target,sig))return -KERNEL_EPERM;
        if(!sig)return 0;
        if(role==KERNEL_PID_TID &&
            (target->proc_exiting || !signal_dispatchable_locked(target)))return 0;
    }
    int process=role!=KERNEL_PID_TID;
    if(process) {
        SCHEDULER_TASK_BORROW(representative,signal_group_representative_get(target));
        if(!representative)return 0; /* A zombie accepts without queueing. */
    }
    struct kernel_signal_wait_info info;
    if(supplied)info=*supplied;
    else info=(struct kernel_signal_wait_info){.signal=sig,
        .sender=(uint32_t)(process?caller->group_leader->tid:caller->tid),
        .uid=kernel_credentials_current()->uid,.code=process?0:-6};
    enum kernel_signal_status status=signal_send_one(target,sig,(kernel_pid_t)info.sender,
        process,info.code,&info,supplied!=0,process?0:id->generation);
    if(!process && status==KERNEL_SIGNAL_STATUS_INVALID_STATE) {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        if(id->members[KERNEL_PID_TID] && id->members[KERNEL_PID_TID]->task==target &&
            (target->proc_exiting || !signal_dispatchable_locked(target))) {
            /* Exit can win during unlocked signal preparation. The retained
             * TID still exists, so Linux accepts and drops the notification. */
            return signal_permitted_locked(caller,target,sig)?0:-KERNEL_EPERM;
        }
    }
    return status==KERNEL_SIGNAL_STATUS_OK?0:status==KERNEL_SIGNAL_STATUS_PERMISSION?-KERNEL_EPERM:
        status==KERNEL_SIGNAL_STATUS_QUEUE_FULL?-KERNEL_EAGAIN:-KERNEL_ESRCH;
}
int64_t kernel_signal_send_identity(struct kernel_pid *id,unsigned scope,
    uint32_t sig,const struct kernel_signal_wait_info *info)
{
    if(sig>KERNEL_SIGNAL_COUNT)return -KERNEL_EINVAL;
    if(scope<2)return signal_identity_target(id,scope?KERNEL_PID_TGID:KERNEL_PID_TID,0,sig,info);
    int delivered=0,denied=0;
    struct scheduler_task_cursor cursor;
    scheduler_task_cursor_begin(&cursor);
    while(cursor.task) {
        int selected;
        {
            KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
            selected=!cursor.task->retirement && cursor.task->identities[KERNEL_PID_TGID].identity &&
                process_identity(cursor.task,KERNEL_PID_PGID)==id;
        }
        if(selected) {
            int64_t result=signal_identity_target(id,KERNEL_PID_PGID,cursor.task,sig,info);
            if(!result)delivered=1;
            else if(result==-KERNEL_EPERM)denied=1;
        }
        scheduler_task_cursor_next(&cursor);
    }
    return delivered?0:denied?-KERNEL_EPERM:-KERNEL_ESRCH;
}

int64_t kernel_signal_send_thread(struct kernel_task *caller,
                                   kernel_pid_t tgid,
                                   kernel_pid_t tid,
                                   uint32_t sig,
                                   kernel_pid_t sender_tid)
{
    struct kernel_task *target;

    if (tgid < 0 || tid <= 0) {
        return 0U;
    }
    SCHEDULER_TASK_BORROW(borrow,0);
    /* Execution owns this task and its TID until the syscall ends; remote
     * lookup still takes a storage borrow and validates the requested group. */
    if(caller==kernel_task_current() && caller->tid==tid)target=caller;
    else target=borrow=kernel_signal_find_by_tid(tid);
    if (!signal_dispatchable(target) || target->group_leader == 0 ||
        (tgid != 0 && target->group_leader->tid != tgid)) {
        return 0U;
    }
    if (sig == 0U) {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        return signal_permitted_locked(caller,target,sig)?1:-KERNEL_EPERM;
    }
    /* 实际发送由 signal_send_one 在分配前授权、发布时复核；探测信号
     * 没有该后续步骤，只有它需要在此单独检查。 */
    enum kernel_signal_status status=kernel_signal_send_task(target,sig,sender_tid);
    if(status==KERNEL_SIGNAL_STATUS_PERMISSION)return -KERNEL_EPERM;
    if(status==KERNEL_SIGNAL_STATUS_QUEUE_FULL)return -KERNEL_EAGAIN;
    if(status!=KERNEL_SIGNAL_STATUS_OK)return 0;

    return 1U;
}

/* ---- delivery ---- */

static void signal_terminate(uint32_t sig, int core_dump)
    __attribute__((noreturn));

static void signal_stop_member(struct kernel_task *task, uint32_t sig)
{
    struct signal_pending_prepared p __attribute__((cleanup(signal_pending_finish)))={0};
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
    while(signal_pending_clear_locked(task,0,bit,&p)) {}
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
                                    int32_t *clock_id,
                                    uint64_t *deadline,
                                    uint64_t *remaining_address)
{
    if (task->syscall_restart.kind != KERNEL_SYSCALL_RESTART_NANOSLEEP) {
        return 0;
    }
    *clock_id = task->syscall_restart.value.nanosleep.clock_id;
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
    if (!has_handler && kind == KERNEL_SYSCALL_RESTART_NANOSLEEP &&
        task->syscall_restart.value.nanosleep.absolute) {
        kernel_signal_clear_syscall_restart(task);
        return KERNEL_SIGNAL_RESTART_RETRY;
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
    uint64_t retarget;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        task->signal_saved_mask = task->signal_blocked;
        task->signal_restore_mask = 1U;
        retarget=signal_set_blocked_locked(task,mask);
    }
    signal_retarget_shared(task,retarget);
    while (!kernel_signal_has_pending(task) &&
           task->terminate_requested == 0U) {
        if (KERNEL_WAIT_RECHECK(0, 0U, 1, &reason,
                (1)) !=
            KERNEL_SCHEDULER_STATUS_OK) {
            {
                KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
                retarget=signal_set_blocked_locked(task,task->signal_saved_mask);
                task->signal_restore_mask = 0U;
            }
            signal_retarget_shared(task,retarget);
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
    uint64_t retarget;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        *saved_mask = task->signal_blocked;
        task->signal_saved_mask = task->signal_blocked;
        task->signal_restore_mask = 1U;
        retarget=signal_set_blocked_locked(task,new_mask);
    }
    signal_retarget_shared(task,retarget);
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
        uint64_t retarget;
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            retarget=signal_set_blocked_locked(task,saved_mask);
            task->signal_restore_mask = 0U;
        }
        signal_retarget_shared(task,retarget);
    }
}

void kernel_signal_force_fault(struct kernel_task *task, uint32_t sig,
    int32_t code, uint64_t address)
{
    if (task != kernel_cpu_current()->current || !signal_dispatchable(task) ||
        !sig || sig > KERNEL_SIGNAL_COUNT || task->signal_fault.signal) __builtin_trap();
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
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
    scheduler_signal_request_locked(task);
#endif
}

static inline int signal_return_work(const struct kernel_task *task)
{
    struct kernel_task *leader = signal_group_leader(task);
    uint64_t observed = __atomic_load_n(&task->signal_pending,__ATOMIC_RELAXED) |
        (leader ? __atomic_load_n(&leader->group_pending,__ATOMIC_RELAXED) : 0);
    return task->signal_fault.signal || task->terminate_requested ||
           task->signal_restore_mask || (observed & ~__atomic_load_n(&task->signal_blocked,__ATOMIC_RELAXED));
}

/* 事件交付需要的临时载荷和保存寄存器留在有事件的路径，普通返回只检查位。
 * 每次继续处理忽略/停止等事件前仍重查，真正消费保留原调度域保护。 */
static __attribute__((noinline)) enum kernel_signal_select_result signal_select_active(
    struct kernel_task *task, struct kernel_signal_delivery *delivery)
{
    KERNEL_SERVICE_SCOPE_IF(return_service, 0);
    do {
        struct kernel_task *leader;
        struct kernel_signal_fault fault;
        struct kernel_signal_wait_info *info = &delivery->info;
        uint64_t pending;
        int shared = 0;
        int needs_service=0;
        uint64_t retarget=0;
        uint32_t sig = 0;
        struct signal_pending_prepared p={0};
        struct kernel_signal_action action = {0};
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
                const struct kernel_signal_action *entry=signal_action_of(task,sig);
                if (entry) action=*entry;
                if (action.handler==KERNEL_SIGNAL_DFL && signal_default_action(sig)==KERNEL_SIGNAL_DEFAULT_STOP &&
                    !kernel_scheduler_user_service_active()) {
                    /* 保留pending跨迁移；途中SIGCONT可取消停止，CPU0重新选择。 */
                    needs_service=1;
                } else {
                    if(fault.signal) {
                        task->signal_fault=(struct kernel_signal_fault){0};
                        *info=(struct kernel_signal_wait_info){.signal=sig,.code=fault.code,.address=fault.address};
                    } else signal_pending_take_locked(shared?leader:task,shared,sig,info,&p);
                    if (info->signal && action.handler!=KERNEL_SIGNAL_DFL && action.handler!=KERNEL_SIGNAL_IGN) {
                        /* pending、disposition、mask和RESETHAND在同一次选择中提交。
                         * 直接写入本线程delivery，原生定时器完成也更新同一载荷；
                         * 解锁后构帧不再读取共享action指针。 */
                        delivery->handler=action.handler;
                        delivery->flags=action.flags;
                        delivery->restore_mask=task->signal_restore_mask ? task->signal_saved_mask : task->signal_blocked;
                        task->signal_restore_mask=0;
                        retarget=signal_set_blocked_locked(task,task->signal_blocked | action.mask |
                                                           ((action.flags & LINUX_SA_NODEFER) ? 0 : signal_mask(sig)));
                        if (action.flags & LINUX_SA_RESETHAND)
                            signal_table_of(task)->actions[sig-1].handler=KERNEL_SIGNAL_DFL;
                    }
                }
            }
        }
        if (needs_service) {
            kernel_service_scope_begin(&return_service);
            continue;
        }
        signal_pending_finish(&p);
        if(pending&&!info->signal)continue;
        signal_retarget_shared(task,retarget);
        if (shared && sig == 14) real_timer_rearm(leader);
        if (pending == 0U) {
            if (task->signal_restore_mask != 0U) {
                kernel_service_scope_begin(&return_service);
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
        if (action.handler != KERNEL_SIGNAL_DFL && action.handler != KERNEL_SIGNAL_IGN) {
            return KERNEL_SIGNAL_SELECT_HANDLER;
        }
        if (action.handler == KERNEL_SIGNAL_IGN) {
            continue;
        }
        switch (signal_default_action(sig)) {
        case KERNEL_SIGNAL_DEFAULT_IGNORE:
        case KERNEL_SIGNAL_DEFAULT_CONTINUE:
            continue;
        case KERNEL_SIGNAL_DEFAULT_STOP: {
            if (sig != SIGNAL_STOP && process_group_is_orphaned(task)) break;
            signal_stop_group(task, sig);
            continue;
        }
        case KERNEL_SIGNAL_DEFAULT_CORE:
            signal_terminate(sig, 1);
        default:
            signal_terminate(sig, 0);
        }
    } while (signal_return_work(task));
    return KERNEL_SIGNAL_SELECT_NONE;
}

enum kernel_signal_select_result kernel_signal_select(
    struct kernel_task *task, struct kernel_signal_delivery *delivery)
{
    if (!signal_return_work(task)) return KERNEL_SIGNAL_SELECT_NONE;
    return signal_select_active(task, delivery);
}
