#include <kernel/cost.h>
#if BOAROS_COST_DIAGNOSTICS
#include <kernel/errno.h>
#include <string.h>
static void unlock(uint64_t *status) { kernel_cost_unlock(*status); }
#define ATOMIC_SCOPE uint64_t interrupts __attribute__((cleanup(unlock))) = kernel_cost_lock()
struct cost_counter { uint64_t value, samples, maximum; };
#define X(id, name, unit, hist) + hist
/* Use a separate expression so all schemas share one source. */
enum { cost_histograms = 0
#include <kernel/cost.def>
};
#undef X
#define X(id, name, unit, hist) _Static_assert(sizeof(#name) <= 32, "cost metric name budget");
#include <kernel/cost.def>
#undef X
/* 名称按实际长度保存，给新增诊断留空间而保留完整直方图。 */
struct cost_metric_names {
#define X(id, name, unit, hist) char id[sizeof(#name)];
#include <kernel/cost.def>
#undef X
};
static const struct cost_metric_names names = {
#define X(id, name, unit, hist) #name,
#include <kernel/cost.def>
#undef X
};
static const uint16_t name_offsets[] = {
#define X(id, name, unit, hist) offsetof(struct cost_metric_names, id),
#include <kernel/cost.def>
#undef X
};
_Static_assert(sizeof(names) <= UINT16_MAX, "cost name offsets");
static const char *metric_name(unsigned metric)
{ return (const char *)&names + name_offsets[metric]; }
enum cost_unit { UNIT_bytes, UNIT_count, UNIT_entries, UNIT_operations,
    UNIT_pages, UNIT_requests, UNIT_tasks, UNIT_ticks };
/* 单位文字共享，新增只读快照不挤占聚合预算；输出 schema 不变。 */
static const char unit_names[][11] = {
    "bytes","count","entries","operations","pages","requests","tasks","ticks"
};
static const unsigned char units[] = {
#define X(id, name, unit, hist) UNIT_##unit,
#include <kernel/cost.def>
#undef X
};
static const unsigned char histograms[] = {
#define X(id, name, unit, hist) hist,
#include <kernel/cost.def>
#undef X
};
static uint32_t timebase;
void kernel_cost_set_timebase(uint32_t frequency) { __atomic_store_n(&timebase, frequency, __ATOMIC_RELAXED); }
uint32_t kernel_cost_timebase(void) { return __atomic_load_n(&timebase, __ATOMIC_RELAXED); }
static unsigned char histogram_indexes[COST_METRIC_COUNT];
static const char lanes[][11] = {"foreground", "background", "observer"};
static struct {
    struct cost_counter counters[3][COST_METRIC_COUNT];
    uint64_t bins[3][cost_histograms][65];
    uint64_t epoch, owner, start, end, inflight;
    struct kernel_cost_task *boundary_actor;
    uint32_t frequency, fixture, active, pending, overflow, state;
} cost;
static uint32_t aggregate_lock;
static struct kernel_cost_cpu *cpu_states[BOAROS_MAX_CPUS];
static const enum kernel_cost_metric lock_metrics[]={
    COST_LOCK10_ATTEMPTS,COST_LOCK15_ATTEMPTS,COST_LOCK20_ATTEMPTS,
    COST_LOCK30_ATTEMPTS,COST_LOCK40_ATTEMPTS,COST_LOCKOTHER_ATTEMPTS,
    COST_LOCK21_ATTEMPTS,COST_LOCK31_ATTEMPTS,COST_LOCK32_ATTEMPTS,COST_LOCK35_ATTEMPTS
};
enum { cost_storage = sizeof(cost) + sizeof(names) + sizeof(name_offsets) + sizeof(units) + sizeof(unit_names) + sizeof(histograms) + sizeof(histogram_indexes) + sizeof(lanes) + sizeof(timebase) + sizeof(aggregate_lock) + sizeof(cpu_states) + sizeof(lock_metrics) + BOAROS_MAX_CPUS * (sizeof(struct kernel_cost_cpu) + KERNEL_COST_CPU_SERVICE_BYTES) };
_Static_assert(cost_storage <= 96 * 1024, "cost aggregate budget");
static void record(struct kernel_cost_tag tag, enum kernel_cost_metric metric, uint64_t value);
static void register_cpu(struct kernel_cost_cpu *cpu)
{
    if (cpu->registered) return;
    for (unsigned i=0;i<BOAROS_MAX_CPUS;i++) {
        if (!cpu_states[i]) { cpu_states[i]=cpu;cpu->registered=1;return; }
        if (cpu_states[i]==cpu) __builtin_trap();
    }
    __builtin_trap();
}
void kernel_cost_register_cpu(struct kernel_cost_cpu *cpu)
{ ATOMIC_SCOPE;register_cpu(cpu); }
uint64_t kernel_cost_lock(void)
{
    uint64_t enabled=kernel_cost_irq_save();
    struct kernel_cost_cpu *cpu=kernel_cost_cpu_current();
    if (cpu->lock_depth==UINT32_MAX) __builtin_trap();
    /* Observer callbacks recursively capture tags before returning here. */
    if (!cpu->lock_depth++) {
        uint64_t started=kernel_cost_clock();
        /* Remote end can clip an observer stalled before acquiring this lock. */
        __atomic_store_n(&cpu->observer_wait_started,started,__ATOMIC_RELEASE);
        for (;;) {
            while (__atomic_load_n(&aggregate_lock,__ATOMIC_RELAXED)) {}
            if (!__atomic_exchange_n(&aggregate_lock,1,__ATOMIC_ACQUIRE)) break;
        }
        __atomic_store_n(&cpu->observer_wait_started,0,__ATOMIC_RELEASE);
        if (cost.epoch) register_cpu(cpu);
        if (enabled) kernel_cost_irq_observer(started);
        if (cost.active) record((struct kernel_cost_tag){cost.epoch,2},
            COST_OBSERVER_TICKS,kernel_cost_clock()-(started<cost.start ? cost.start : started));
    }
    return enabled;
}
void kernel_cost_unlock(uint64_t enabled)
{
    struct kernel_cost_cpu *cpu=kernel_cost_cpu_current();
    if (!cpu->lock_depth) __builtin_trap();
    if (cpu->lock_depth==1) {
        if (enabled) kernel_cost_irq_enable_now();
        cpu->lock_depth=0;
        __atomic_store_n(&aggregate_lock,0,__ATOMIC_RELEASE);
    } else { if (enabled) __builtin_trap();cpu->lock_depth--; }
    kernel_cost_irq_restore(enabled);
}
void kernel_cost_irq_flush(void)
{
    ATOMIC_SCOPE;
    struct kernel_cost_cpu *cpu=kernel_cost_cpu_current();
    if (cpu->return_pending == 1) {
        uint64_t ticks = cpu->return_timestamp;
        cpu->return_pending = 0;
        kernel_cost_irq_enabled(ticks);
    }
}
void kernel_cost_irq_disabled(uint64_t ticks)
{
    struct kernel_cost_cpu *cpu=kernel_cost_cpu_current();
    /* The remote closer must see the physical boundary even while this CPU
     * waits for the observer lock. Nested observer calls already hold it. */
    if (!cpu->lock_depth)
        __atomic_store_n(&cpu->irq_pending_start,ticks,__ATOMIC_RELEASE);
    ATOMIC_SCOPE;
    kernel_cost_irq_flush();
    (void)__atomic_exchange_n(&cpu->irq_pending_start,0,__ATOMIC_ACQUIRE);
    if (cpu->irq_opened) return;
    cpu->irq_opened = 1; cpu->irq_suppressed = 0; cpu->irq_start = cost.active && ticks < cost.start ? cost.start : ticks; cpu->irq_tag = kernel_cost_capture();
}
void kernel_cost_irq_observer(uint64_t ticks)
{
    ATOMIC_SCOPE;
    kernel_cost_irq_disabled(ticks);
    kernel_cost_cpu_current()->irq_tag.lane = 2;
}
void kernel_cost_irq_suppress(void) { ATOMIC_SCOPE;kernel_cost_cpu_current()->irq_suppressed = 1; }
void kernel_cost_irq_enabled(uint64_t ticks)
{
    ATOMIC_SCOPE;
    struct kernel_cost_cpu *cpu=kernel_cost_cpu_current();
    struct kernel_cost_tag tag = cpu->irq_tag;
    uint64_t start = cpu->irq_start;
    unsigned publish = cpu->irq_opened && !cpu->irq_suppressed && ticks >= start;
    cpu->irq_opened = 0;
    if (publish) kernel_cost_sample_tag(tag, COST_IRQ_OFF_TICKS, ticks - start);
}
void kernel_cost_irq_enable_now(void)
{
    ATOMIC_SCOPE;
    struct kernel_cost_cpu *cpu=kernel_cost_cpu_current();
    kernel_cost_irq_flush();
    cpu->return_timestamp=kernel_cost_clock();
    __atomic_store_n(&cpu->return_pending,1,__ATOMIC_RELEASE);
}
void kernel_cost_irq_return(uint64_t status)
{
    ATOMIC_SCOPE;
    kernel_cost_irq_flush();
    kernel_cost_cpu_current()->return_pending=status & 32 ? 2 : 0;
}
static void add_checked(uint64_t *to, uint64_t value)
{
    if (UINT64_MAX - *to < value) { cost.overflow = 1; *to = UINT64_MAX; }
    else *to += value;
}
uint64_t kernel_cost_epoch(void) { ATOMIC_SCOPE; return cost.epoch; }
struct kernel_cost_tag kernel_cost_task_tag(const struct kernel_cost_task *task)
{
    ATOMIC_SCOPE;
    struct kernel_cost_tag tag = {0};
    if (cost.active && (!task || !task->suppress)) {
        tag.epoch = cost.epoch;
        tag.lane = task && (task->wait_flags & 64) ? 1 :
            cost.fixture || (task && task->epoch == cost.epoch) ? 0 : 1;
    }
    return tag;
}
struct kernel_cost_tag kernel_cost_capture(void)
{
    ATOMIC_SCOPE;
    return kernel_cost_task_tag(kernel_cost_current());
}
static unsigned histogram_index(enum kernel_cost_metric metric)
{
    return histogram_indexes[metric];
}
static void record(struct kernel_cost_tag tag, enum kernel_cost_metric metric, uint64_t value)
{
    if (tag.lane >= 3 || (unsigned)metric >= COST_METRIC_COUNT) __builtin_trap();
    struct cost_counter *counter = &cost.counters[tag.lane][metric];
    add_checked(&counter->value, value);
    add_checked(&counter->samples, 1);
    if (value > counter->maximum) counter->maximum = value;
}
static void observer(uint64_t start)
{
    record((struct kernel_cost_tag){cost.epoch, 2}, COST_OBSERVER_TICKS, kernel_cost_clock() - start);
}
void kernel_cost_add_tag(struct kernel_cost_tag tag, enum kernel_cost_metric metric, uint64_t value)
{
    ATOMIC_SCOPE;
    if (!cost.active || !tag.epoch || tag.epoch != cost.epoch) return;
    uint64_t start = kernel_cost_clock();
    record(tag, metric, value);
    observer(start);
}
void kernel_cost_page_metadata(struct kernel_cost_tag tag, uint64_t checked,
                              uint64_t written, uint64_t ticks)
{
    ATOMIC_SCOPE;
    if (!cost.active || !tag.epoch || tag.epoch != cost.epoch) return;
    uint64_t start = kernel_cost_clock();
    record(tag, COST_PAGE_META_CHECKED, checked);
    record(tag, COST_PAGE_META_WRITTEN, written);
    record(tag, COST_ALLOCATOR_META_TICKS, ticks);
    observer(start);
}
void kernel_cost_raw_lock(struct kernel_cost_tag tag, uint64_t wait, uint64_t hold)
{
    ATOMIC_SCOPE;
    if (!cost.active || !tag.epoch || tag.epoch != cost.epoch) return;
    uint64_t start = kernel_cost_clock();
    record(tag, COST_RAW_WAIT_TICKS, wait);
    record(tag, COST_RAW_HOLD_TICKS, hold);
    observer(start);
}
void kernel_cost_sample_tag(struct kernel_cost_tag tag, enum kernel_cost_metric metric, uint64_t value)
{
    ATOMIC_SCOPE;
    if (!cost.active || !tag.epoch || tag.epoch != cost.epoch) return;
    uint64_t start = kernel_cost_clock();
    record(tag, metric, value);
    if (!histograms[metric]) __builtin_trap();
    unsigned bucket = value ? 1U : 0U;
    uint64_t remaining = value;
    if (remaining >> 32) { bucket += 32; remaining >>= 32; }
    if (remaining >> 16) { bucket += 16; remaining >>= 16; }
    if (remaining >> 8) { bucket += 8; remaining >>= 8; }
    if (remaining >> 4) { bucket += 4; remaining >>= 4; }
    if (remaining >> 2) { bucket += 2; remaining >>= 2; }
    if (remaining >> 1) bucket++;
    add_checked(&cost.bins[tag.lane][histogram_index(metric)][bucket], 1);
    observer(start);
}
struct kernel_cost_io_scope kernel_cost_io_enter(unsigned operation)
{
    ATOMIC_SCOPE;
    struct kernel_cost_io_scope scope = {kernel_cost_current(), 0};
    if (scope.actor) { scope.previous = scope.actor->operation; scope.actor->operation = (scope.actor->operation & 240) | (operation + 1); }
    return scope;
}
void kernel_cost_io_leave(struct kernel_cost_io_scope *scope)
{ ATOMIC_SCOPE; if (scope->actor) scope->actor->operation = scope->previous; }
void kernel_cost_add_io(unsigned offset, uint64_t value)
{
    ATOMIC_SCOPE;
    struct kernel_cost_task *task = kernel_cost_current();
    if (task && (task->operation & 15) && (task->operation & 15) <= 4)
        kernel_cost_add((enum kernel_cost_metric)(COST_FILE_CALLS + ((task->operation & 15) - 1) * 8 + offset), value);
}
struct kernel_cost_io_scope kernel_cost_phase_enter(unsigned phase)
{
    ATOMIC_SCOPE;
    struct kernel_cost_io_scope scope = {kernel_cost_current(), 0};
    if (phase > 7) __builtin_trap();
    if (scope.actor) { scope.previous = scope.actor->operation; scope.actor->operation = (scope.actor->operation & 15) | (phase << 4); }
    return scope;
}
void kernel_cost_add(enum kernel_cost_metric metric, uint64_t value)
{ kernel_cost_add_tag(kernel_cost_capture(), metric, value); }
void kernel_cost_sample(enum kernel_cost_metric metric, uint64_t value)
{ kernel_cost_sample_tag(kernel_cost_capture(), metric, value); }
struct kernel_cost_scope kernel_cost_enter(enum kernel_cost_metric metric)
{
    ATOMIC_SCOPE;
    struct kernel_cost_scope scope = { .tag = kernel_cost_capture(), .metric = metric };
    scope.actor = kernel_cost_current();
    if (scope.actor && scope.actor->suppress) scope.actor = 0;
    if (scope.actor || scope.tag.epoch) {
        scope.start = kernel_cost_clock();
        uint64_t epoch = cost.active || (cost.pending == 1 && scope.actor &&
            scope.actor->scope_epoch == cost.epoch) ? cost.epoch : 0;
        if (epoch) add_checked(&cost.inflight, 1);
        if (scope.actor) {
            if (!scope.actor->depth) scope.actor->scope_epoch = epoch;
            scope.actor->depth++;
        }
    }
    return scope;
}
void kernel_cost_rebase(struct kernel_cost_task *task)
{
    ATOMIC_SCOPE;
    task->run_start = task->ready_start = task->blocked_start = 0;
    task->wait_flags &= (uint8_t)~15U;
    if (task->scope_epoch != cost.epoch) add_checked(&cost.inflight, task->depth);
    task->scope_epoch = cost.epoch;
}
void kernel_cost_join(struct kernel_cost_task *task)
{
    ATOMIC_SCOPE;
    task->epoch = cost.epoch;
    kernel_cost_rebase(task);
}
static void leave_at(struct kernel_cost_scope *scope,uint64_t end)
{
    struct kernel_cost_tag tag = scope->tag;
    struct kernel_cost_task *actor=scope->actor;
    scope->actor=0;scope->tag.epoch=0;
    if (actor) {
        if (!actor->depth) return; /* Cancel consumes abandoned stack scopes. */
        actor->depth--;
        if (actor->scope_epoch != cost.epoch) return;
        tag = kernel_cost_task_tag(actor);
    }
    if (!(cost.active || cost.pending == 1) ||
        (actor ? actor->scope_epoch : tag.epoch) != cost.epoch) return;
    if (!cost.inflight) __builtin_trap();
    cost.inflight--;
    if (!cost.active || !tag.epoch) return;
    uint64_t start = scope->start < cost.start ? cost.start : scope->start;
    if (end<start) return;
    uint64_t elapsed = end - start;
    if (histograms[scope->metric]) kernel_cost_sample_tag(tag, scope->metric, elapsed);
    else kernel_cost_add_tag(tag, scope->metric, elapsed);
}
void kernel_cost_leave(struct kernel_cost_scope *scope)
{ ATOMIC_SCOPE;leave_at(scope,kernel_cost_clock()); }
void kernel_cost_leave_at(struct kernel_cost_scope *scope,uint64_t end)
{ ATOMIC_SCOPE;leave_at(scope,end); }
void kernel_cost_cancel(struct kernel_cost_task *task)
{
    ATOMIC_SCOPE;
    if ((cost.active || cost.pending == 1) && task->scope_epoch == cost.epoch && task->depth) {
        if (cost.inflight < task->depth) __builtin_trap();
        cost.inflight -= task->depth;
        kernel_cost_add_tag(kernel_cost_task_tag(task), COST_CANCELLED, task->depth);
    }
    task->depth = 0; task->wait_rank = 0; task->wait_flags &= 128;
    for (unsigned i=0;i<BOAROS_MAX_CPUS;i++)
        if (cpu_states[i] && cpu_states[i]->running==task) cpu_states[i]->running=0;
}
/* All CPU IRQ records are protected by the aggregate lock. Assembly writes only
 * the local pending timestamp with IRQ disabled; pending=1 publishes that word. */
static void start_window(void)
{
    cost.start=kernel_cost_clock(); cost.active=1;
    for (unsigned i=0;i<BOAROS_MAX_CPUS;i++) {
        struct kernel_cost_cpu *cpu=cpu_states[i];
        if (!cpu) continue;
        if (cpu->running) { cpu->running->run_start=cost.start;cpu->running->wait_flags|=8; }
        if (cpu->irq_opened) {
            unsigned lane=cpu->irq_tag.lane==2;
            cpu->irq_start=cost.start;cpu->irq_tag=kernel_cost_task_tag(cpu->running);
            if (lane) cpu->irq_tag.lane=2;
            cpu->irq_suppressed=0;
        }
    }
}
static void account_at(struct kernel_cost_task *task,uint64_t now);
static void close_window(void)
{
    uint64_t now=kernel_cost_clock();
    for (unsigned i=0;i<BOAROS_MAX_CPUS;i++) {
        struct kernel_cost_cpu *cpu=cpu_states[i];
        if (!cpu) continue;
        if (cpu->running) account_at(cpu->running,now);
        /* Seeing a new wait must also expose its earlier physical IRQ entry.
         * Consume that entry before reading the still earlier return tail. */
        uint64_t waiting=__atomic_load_n(&cpu->observer_wait_started,__ATOMIC_ACQUIRE);
        uint64_t pending=__atomic_exchange_n(&cpu->irq_pending_start,0,__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&cpu->return_pending,__ATOMIC_ACQUIRE)==1) {
            uint64_t end=cpu->return_timestamp;
            cpu->return_pending=0;
            if (end>now) end=now;
            if (cpu->irq_opened && !cpu->irq_suppressed && end>=cpu->irq_start)
                kernel_cost_sample_tag(cpu->irq_tag,COST_IRQ_OFF_TICKS,end-cpu->irq_start);
            cpu->irq_opened=0;
        }
        if (!cpu->irq_opened && pending) {
            cpu->irq_opened=1;cpu->irq_suppressed=0;
            cpu->irq_start=pending<cost.start ? cost.start : pending;
            cpu->irq_tag=kernel_cost_task_tag(cpu->running);
        }
        if (waiting && waiting<cost.start) waiting=cost.start;
        if (!cpu->irq_opened && waiting && now>=waiting) {
            /* A COST lock that itself disabled IRQ has no external hook. */
            cpu->irq_opened=1;cpu->irq_suppressed=0;
            cpu->irq_start=waiting;cpu->irq_tag=(struct kernel_cost_tag){cost.epoch,2};
        }
        if (cpu->irq_opened) {
            if (!cpu->irq_suppressed && now>=cpu->irq_start)
                kernel_cost_sample_tag(cpu->irq_tag,COST_IRQ_OFF_TICKS,now-cpu->irq_start);
            /* Clip this epoch without pretending the remote hart enabled IRQ. */
            cpu->irq_start=now;cpu->irq_tag.epoch=0;
        }
        if (waiting && now>=waiting)
            kernel_cost_add_tag((struct kernel_cost_tag){cost.epoch,2},COST_OBSERVER_TICKS,now-waiting);
    }
    cost.active=0;cost.end=now;
}
int kernel_cost_begin(uint64_t owner, uint32_t frequency, int fixture, int deferred)
{
    ATOMIC_SCOPE;
    if (cost.active || cost.pending) return -KERNEL_EBUSY;
    if (!owner || !frequency) return -KERNEL_EINVAL;
    if (cost.epoch == UINT64_MAX) return -KERNEL_EOVERFLOW;
    uint64_t epoch=cost.epoch+1;
    memset(&cost,0,sizeof(cost));
    unsigned index=0;
    for (unsigned i=0;i<COST_METRIC_COUNT;i++) { histogram_indexes[i]=(unsigned char)index;index+=histograms[i]; }
    cost.epoch=epoch;cost.owner=owner;cost.frequency=frequency;
    cost.fixture=!!fixture;cost.state=1;
    cost.boundary_actor=kernel_cost_current();
    register_cpu(kernel_cost_cpu_current());
    kernel_cost_cpu_current()->running=cost.boundary_actor;
    cost.pending=deferred ? 1 : 0;
    if (!deferred) {
        if (fixture && cost.boundary_actor) kernel_cost_join(cost.boundary_actor);
        start_window();
    }
    return 0;
}
int kernel_cost_end(uint64_t owner, int deferred)
{
    ATOMIC_SCOPE;
    if (!cost.active || cost.pending) return -KERNEL_EINVAL;
    if (owner!=cost.owner) return -KERNEL_EPERM;
    if (cost.inflight) return -KERNEL_EBUSY;
    close_window();
    cost.boundary_actor=kernel_cost_current();
    if (deferred) cost.pending=2;
    else { cost.state=2;cost.boundary_actor=0; }
    return 0;
}
void kernel_cost_boundary(void)
{
    ATOMIC_SCOPE;
    /* A different hart returning to userspace cannot start/finish this syscall. */
    if (kernel_cost_current()!=cost.boundary_actor) return;
    if (cost.pending==1) start_window();
    if (cost.pending==2) { cost.end=kernel_cost_clock();cost.state=2; }
    cost.pending=0;cost.boundary_actor=0;
}
void kernel_cost_abort(uint64_t owner)
{
    ATOMIC_SCOPE;
    if ((cost.active || cost.pending) && owner==cost.owner) {
        if (cost.active) close_window();
        cost.end=kernel_cost_clock();cost.active=cost.pending=0;cost.state=3;cost.boundary_actor=0;
    }
}
void kernel_cost_inherit(struct kernel_cost_task *child, const struct kernel_cost_task *parent)
{ ATOMIC_SCOPE; memset(child, 0, sizeof(*child)); child->epoch = parent->epoch; child->scope_epoch = cost.active || cost.pending == 1 ? cost.epoch : 0; }
unsigned kernel_cost_rank(unsigned rank)
{
    return rank == 10 ? 0 : rank == 15 ? 1 : rank == 20 ? 2 :
           rank == 30 ? 3 : rank == 40 ? 4 : rank == 21 ? 6 :
           rank == 31 ? 7 : rank == 32 ? 8 : rank == 35 ? 9 : 5;
}
enum kernel_cost_metric kernel_cost_lock_metric(unsigned rank)
{
    if (rank>=sizeof(lock_metrics)/sizeof(lock_metrics[0])) __builtin_trap();
    return lock_metrics[rank];
}
static void account_at(struct kernel_cost_task *task,uint64_t now)
{
    uint64_t start=task->run_start<cost.start ? cost.start : task->run_start;
    if ((task->run_start || (task->wait_flags & 8)) && now>=start)
        kernel_cost_add_tag(kernel_cost_task_tag(task),task->wait_flags & 128 ? COST_IDLE_TICKS : COST_RUN_TICKS,now-start);
    task->run_start=now;task->wait_flags|=8;
}
void kernel_cost_account(struct kernel_cost_task *task)
{
    ATOMIC_SCOPE;
    account_at(task,kernel_cost_clock());
}
void kernel_cost_block(struct kernel_cost_task *task)
{
    ATOMIC_SCOPE;
    kernel_cost_account(task);
    task->run_start = 0; task->wait_flags &= (uint8_t)~8U; task->blocked_start = kernel_cost_clock();
}
void kernel_cost_ready(struct kernel_cost_task *task)
{
    ATOMIC_SCOPE;
    if (!task->ready_start) task->ready_start = kernel_cost_clock();
}
void kernel_cost_wake(struct kernel_cost_task *task)
{
    ATOMIC_SCOPE;
    uint64_t now = kernel_cost_clock();
    struct kernel_cost_tag tag = kernel_cost_task_tag(task);
    uint64_t start = task->blocked_start < cost.start ? cost.start : task->blocked_start;
    if (task->blocked_start && now >= start) kernel_cost_sample_tag(tag,COST_BLOCKED_TICKS,now-start);
    kernel_cost_add_tag(tag,COST_WAKES,1);
    task->blocked_start = 0; task->ready_start = now; task->wait_flags |= 2;
    if (task->wait_rank) {
        kernel_cost_add_tag(tag,(enum kernel_cost_metric)(kernel_cost_lock_metric(task->wait_rank-1)+3),1);
        task->wait_flags |= 1;
    }
}
void kernel_cost_timeout(struct kernel_cost_task *task, uint64_t deadline)
{
    ATOMIC_SCOPE;
    task->blocked_start = deadline;
    task->wait_flags |= 4;
    kernel_cost_add_tag(kernel_cost_task_tag(task), COST_DEADLINE_EXPIRED, 1);
}
void kernel_cost_switch(struct kernel_cost_task *previous, struct kernel_cost_task *next)
{
    ATOMIC_SCOPE;
    if (previous->run_start || (previous->wait_flags & 8)) kernel_cost_account(previous);
    previous->run_start = 0; previous->wait_flags &= (uint8_t)~8U;
    uint64_t now = kernel_cost_clock();
    uint64_t start = next->ready_start < cost.start ? cost.start : next->ready_start;
    if (next->ready_start && now >= start)
        kernel_cost_sample_tag(kernel_cost_task_tag(next),COST_READY_TICKS,now-start);
    if ((next->wait_flags & 2) && next->ready_start && now >= start)
        kernel_cost_sample_tag(kernel_cost_task_tag(next), COST_WAKE_TO_RUN, now - start);
    if ((next->wait_flags & 4) && now >= next->blocked_start)
        kernel_cost_sample_tag(kernel_cost_task_tag(next), COST_DEADLINE_TO_RUN, now - next->blocked_start);
    next->blocked_start = 0;
    next->ready_start = 0; next->wait_flags &= (uint8_t)~6U; next->run_start = now; next->wait_flags |= 8;
    if (previous != next) kernel_cost_add_tag(kernel_cost_task_tag(next),COST_SWITCHES,1);
    kernel_cost_cpu_current()->running=next;
}
int kernel_cost_read(unsigned lane, enum kernel_cost_metric metric, uint64_t *value)
{
    ATOMIC_SCOPE;
    if (cost.active || cost.pending) return -KERNEL_EBUSY;
    if (!value || lane >= 3 || (unsigned)metric >= COST_METRIC_COUNT) return -KERNEL_EINVAL;
    *value = cost.counters[lane][metric].value; return 0;
}
struct formatter { char *buffer; size_t length, capacity; int error; };
static void text(struct formatter *f, const char *s)
{
    size_t n = strlen(s);
    if (f->length + n >= f->capacity) { f->error = 1; return; }
    memcpy(f->buffer + f->length, s, n); f->length += n;
}
static void number(struct formatter *f, uint64_t value)
{
    char digits[20]; unsigned n = 0;
    do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (n) { char s[2] = { digits[--n], 0 }; text(f, s); }
}
static void field(struct formatter *f, const char *key, uint64_t value)
{ text(f, key); text(f, "="); number(f, value); text(f, "\n"); }
size_t kernel_cost_format_capacity(void)
{ return 4096U + COST_METRIC_COUNT * 3U * 256U + cost_histograms * 3U * 65U * 100U; }
int kernel_cost_format(char *buffer, size_t capacity)
{
    ATOMIC_SCOPE;
    if (cost.active || cost.pending) return -KERNEL_EBUSY;
    struct formatter f = {buffer, 0, capacity, 0};
    field(&f, "irq_user_prefix_instructions", 7);
    field(&f, "irq_supervisor_prefix_instructions", 6);
    field(&f, "irq_sret_suffix_instructions", 8);
    field(&f, "irq_c_enable_suffix_min_instructions", 9);
    field(&f, "version", 1); field(&f, "epoch", cost.epoch);
    text(&f, "state="); text(&f, cost.state == 2 ? "complete" : cost.state == 3 ? "incomplete" : "idle"); text(&f, "\n");
    text(&f, cost.fixture ? "mode=fixture\n" : "mode=user\n");
    field(&f, "owner", cost.owner); field(&f, "timebase_hz", cost.frequency);
    field(&f, "resolution_ns_numerator", 1000000000); field(&f, "resolution_ns_denominator", cost.frequency);
    field(&f, "start_ticks", cost.start); field(&f, "end_ticks", cost.end);
    field(&f, "storage_bytes", cost_storage); field(&f, "task_bytes", KERNEL_COST_TASK_BYTES);
    field(&f, "overflow", cost.overflow); field(&f, "inflight", cost.inflight);
    for (unsigned lane = 0; lane < 3; ++lane) for (unsigned m = 0; m < COST_METRIC_COUNT; ++m) {
        char key[128]; size_t n = strlen(lanes[lane]); memcpy(key, lanes[lane], n); key[n++] = '.';
        size_t len = strlen(metric_name(m)); memcpy(key + n, metric_name(m), len); n += len; key[n++] = '.'; key[n] = 0;
        text(&f, key); text(&f, "unit="); text(&f, unit_names[units[m]]); text(&f, "\n");
        const char suffix[][8] = {"value", "samples", "max"};
        uint64_t values[] = {cost.counters[lane][m].value, cost.counters[lane][m].samples, cost.counters[lane][m].maximum};
        for (unsigned j = 0; j < 3; ++j) { text(&f, key); field(&f, suffix[j], values[j]); }
        if (histograms[m]) for (unsigned b = 0; b < 65; ++b) {
            text(&f, key); text(&f, "bucket."); number(&f, b); text(&f, "=");
            number(&f, cost.bins[lane][histogram_index((enum kernel_cost_metric)m)][b]); text(&f, "\n");
        }
    }
    if (f.error) return -KERNEL_EOVERFLOW;
    buffer[f.length] = 0; return (int)f.length;
}
#endif
