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
static const char names[][32] = {
#define X(id, name, unit, hist) #name,
#include <kernel/cost.def>
#undef X
};
static const char units[][12] = {
#define X(id, name, unit, hist) #unit,
#include <kernel/cost.def>
#undef X
};
static const unsigned char histograms[] = {
#define X(id, name, unit, hist) hist,
#include <kernel/cost.def>
#undef X
};
static uint32_t timebase;
void kernel_cost_set_timebase(uint32_t frequency) { timebase = frequency; }
uint32_t kernel_cost_timebase(void) { return timebase; }
static unsigned char histogram_indexes[COST_METRIC_COUNT];
static const char lanes[][11] = {"foreground", "background", "observer"};
static struct {
    struct cost_counter counters[3][COST_METRIC_COUNT];
    uint64_t bins[3][cost_histograms][65];
    uint64_t epoch, owner, start, end, inflight;
    uint32_t frequency, fixture, active, pending, overflow, state;
} cost;
enum { cost_storage = sizeof(cost) + sizeof(names) + sizeof(units) + sizeof(histograms) + sizeof(histogram_indexes) + sizeof(lanes) + 256 };
/* Includes a reserve for bridge/IRQ scalars and formatting metadata. */
_Static_assert(cost_storage <= 65536, "cost aggregate budget");
static struct { struct kernel_cost_tag tag; uint64_t start; unsigned opened, suppressed; } irq;
uint64_t kernel_cost_return_timestamp, kernel_cost_return_pending;
void kernel_cost_irq_flush(void)
{
    if (kernel_cost_return_pending == 1) {
        uint64_t ticks = kernel_cost_return_timestamp;
        kernel_cost_return_pending = 0;
        kernel_cost_irq_enabled(ticks);
    }
}
void kernel_cost_irq_disabled(uint64_t ticks)
{
    kernel_cost_irq_flush();
    if (irq.opened) return;
    irq.opened = 1; irq.suppressed = 0; irq.start = ticks; irq.tag = kernel_cost_capture();
}
void kernel_cost_irq_observer(uint64_t ticks)
{
    kernel_cost_irq_disabled(ticks);
    irq.tag.lane = 2;
}
void kernel_cost_irq_suppress(void) { irq.suppressed = 1; }
void kernel_cost_irq_enabled(uint64_t ticks)
{
    struct kernel_cost_tag tag = irq.tag;
    uint64_t start = irq.start;
    unsigned publish = irq.opened && !irq.suppressed && ticks >= start;
    irq.opened = 0;
    if (publish) kernel_cost_sample_tag(tag, COST_IRQ_OFF_TICKS, ticks - start);
}
static void add_checked(uint64_t *to, uint64_t value)
{
    if (UINT64_MAX - *to < value) { cost.overflow = 1; *to = UINT64_MAX; }
    else *to += value;
}
uint64_t kernel_cost_epoch(void) { return cost.epoch; }
struct kernel_cost_tag kernel_cost_task_tag(const struct kernel_cost_task *task)
{
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
    struct kernel_cost_io_scope scope = {kernel_cost_current(), 0};
    if (scope.actor) { scope.previous = scope.actor->operation; scope.actor->operation = (scope.actor->operation & 240) | (operation + 1); }
    return scope;
}
void kernel_cost_io_leave(struct kernel_cost_io_scope *scope)
{ if (scope->actor) scope->actor->operation = scope->previous; }
void kernel_cost_add_io(unsigned offset, uint64_t value)
{
    struct kernel_cost_task *task = kernel_cost_current();
    if (task && (task->operation & 15) && (task->operation & 15) <= 4)
        kernel_cost_add((enum kernel_cost_metric)(COST_FILE_CALLS + ((task->operation & 15) - 1) * 8 + offset), value);
}
struct kernel_cost_io_scope kernel_cost_phase_enter(unsigned phase)
{
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
        if (scope.tag.epoch) add_checked(&cost.inflight, 1);
        if (scope.actor) {
            if (!scope.actor->depth) scope.actor->scope_epoch = scope.tag.epoch;
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
    task->scope_epoch = cost.epoch;
    add_checked(&cost.inflight, task->depth);
}
void kernel_cost_join(struct kernel_cost_task *task)
{
    task->epoch = cost.epoch;
    kernel_cost_rebase(task);
}
void kernel_cost_leave(struct kernel_cost_scope *scope)
{
    ATOMIC_SCOPE;
    struct kernel_cost_tag tag = scope->tag;
    if (scope->actor) {
        if (!scope->actor->depth) return; /* Cancel consumes abandoned stack scopes. */
        scope->actor->depth--;
        if (scope->actor->scope_epoch != cost.epoch) return;
        tag = kernel_cost_task_tag(scope->actor);
    }
    if (!cost.active || !tag.epoch || tag.epoch != cost.epoch) return;
    uint64_t start = scope->start < cost.start ? cost.start : scope->start;
    uint64_t elapsed = kernel_cost_clock() - start;
    if (histograms[scope->metric]) kernel_cost_sample_tag(tag, scope->metric, elapsed);
    else kernel_cost_add_tag(tag, scope->metric, elapsed);
    if (!cost.inflight) __builtin_trap();
    cost.inflight--;
    scope->tag.epoch = 0;
}
void kernel_cost_cancel(struct kernel_cost_task *task)
{
    ATOMIC_SCOPE;
    if (cost.active && task->scope_epoch == cost.epoch && task->depth) {
        if (cost.inflight < task->depth) __builtin_trap();
        cost.inflight -= task->depth;
        kernel_cost_add_tag(kernel_cost_task_tag(task), COST_CANCELLED, task->depth);
    }
    task->depth = 0; task->wait_rank = 0; task->wait_flags &= 128;
}
int kernel_cost_begin(uint64_t owner, uint32_t frequency, int fixture, int deferred)
{
    ATOMIC_SCOPE;
    if (cost.active || cost.pending) return -KERNEL_EBUSY;
    if (!owner || !frequency) return -KERNEL_EINVAL;
    if (cost.epoch == UINT64_MAX) return -KERNEL_EOVERFLOW;
    uint64_t epoch = cost.epoch + 1;
    memset(&cost, 0, sizeof(cost));
    unsigned index = 0;
    for (unsigned i = 0; i < COST_METRIC_COUNT; ++i) { histogram_indexes[i] = (unsigned char)index; index += histograms[i]; }
    cost.epoch = epoch; cost.owner = owner; cost.frequency = frequency;
    cost.fixture = !!fixture; cost.state = 1;
    cost.pending = deferred ? 1 : 0;
    cost.active = !deferred;
    if (cost.active) {
        cost.start = kernel_cost_clock();
        if (fixture) {
            struct kernel_cost_task *task = kernel_cost_current();
            if (task) { kernel_cost_join(task); task->run_start = cost.start; task->wait_flags |= 8; }
            /* 裁剪原有关闭区间，首段只能归属于本窗口。 */
            if (irq.opened) {
                unsigned observer_lane = irq.tag.lane == 2;
                irq.start = cost.start; irq.tag = kernel_cost_task_tag(task);
                if (observer_lane) irq.tag.lane = 2;
                irq.suppressed = 0;
            }
        }
    }
    return 0;
}
int kernel_cost_end(uint64_t owner, int deferred)
{
    ATOMIC_SCOPE;
    if (!cost.active || cost.pending) return -KERNEL_EINVAL;
    if (owner != cost.owner) return -KERNEL_EPERM;
    if (cost.fixture) { struct kernel_cost_task *task = kernel_cost_current(); if (task) kernel_cost_account(task); }
    if (cost.inflight) return -KERNEL_EBUSY;
    if (cost.fixture && !deferred && irq.opened) {
        uint64_t now = kernel_cost_clock();
        if (!irq.suppressed && now >= irq.start)
            kernel_cost_sample_tag(irq.tag, COST_IRQ_OFF_TICKS, now - irq.start);
        /* 结算窗口尾部，不把真实仍关闭的 hart 伪装成已 enable。 */
        irq.start = now; irq.tag.epoch = 0;
    }
    cost.active = 0;
    if (deferred) cost.pending = 2;
    else { cost.end = kernel_cost_clock(); cost.state = 2; }
    return 0;
}
void kernel_cost_boundary(void)
{
    ATOMIC_SCOPE;
    if (cost.pending == 1) { cost.start = kernel_cost_clock(); cost.active = 1; }
    if (cost.pending == 2) { cost.end = kernel_cost_clock(); cost.state = 2; }
    cost.pending = 0;
}
void kernel_cost_abort(uint64_t owner)
{
    ATOMIC_SCOPE;
    if ((cost.active || cost.pending) && owner == cost.owner) {
        cost.end = kernel_cost_clock(); cost.active = cost.pending = 0; cost.state = 3;
    }
}
void kernel_cost_inherit(struct kernel_cost_task *child, const struct kernel_cost_task *parent)
{ memset(child, 0, sizeof(*child)); child->epoch = parent->epoch; }
unsigned kernel_cost_rank(unsigned rank)
{
    return rank == 10 ? 0 : rank == 15 ? 1 : rank == 20 ? 2 :
           rank == 30 ? 3 : rank == 40 ? 4 : 5;
}
void kernel_cost_account(struct kernel_cost_task *task)
{
    uint64_t now = kernel_cost_clock();
    uint64_t start = task->run_start < cost.start ? cost.start : task->run_start;
    if ((task->run_start || (task->wait_flags & 8)) && now >= start)
        kernel_cost_add_tag(kernel_cost_task_tag(task), task->wait_flags & 128 ? COST_IDLE_TICKS : COST_RUN_TICKS, now - start);
    task->run_start = now; task->wait_flags |= 8;
}
void kernel_cost_block(struct kernel_cost_task *task)
{
    kernel_cost_account(task);
    task->run_start = 0; task->wait_flags &= (uint8_t)~8U; task->blocked_start = kernel_cost_clock();
}
void kernel_cost_ready(struct kernel_cost_task *task)
{
    if (!task->ready_start) task->ready_start = kernel_cost_clock();
}
void kernel_cost_wake(struct kernel_cost_task *task)
{
    uint64_t now = kernel_cost_clock();
    struct kernel_cost_tag tag = kernel_cost_task_tag(task);
    uint64_t start = task->blocked_start < cost.start ? cost.start : task->blocked_start;
    if (task->blocked_start && now >= start) kernel_cost_sample_tag(tag,COST_BLOCKED_TICKS,now-start);
    kernel_cost_add_tag(tag,COST_WAKES,1);
    task->blocked_start = 0; task->ready_start = now; task->wait_flags |= 2;
    if (task->wait_rank) {
        kernel_cost_add_tag(tag,(enum kernel_cost_metric)(COST_LOCK10_WAKES+(task->wait_rank-1)*8),1);
        task->wait_flags |= 1;
    }
}
void kernel_cost_timeout(struct kernel_cost_task *task, uint64_t deadline)
{
    task->blocked_start = deadline;
    task->wait_flags |= 4;
    kernel_cost_add_tag(kernel_cost_task_tag(task), COST_DEADLINE_EXPIRED, 1);
}
void kernel_cost_switch(struct kernel_cost_task *previous, struct kernel_cost_task *next)
{
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
    field(&f, "irq_sret_suffix_instructions", 11);
    field(&f, "irq_c_enable_suffix_min_instructions", 9);
    field(&f, "version", 1); field(&f, "epoch", cost.epoch);
    text(&f, "state="); text(&f, cost.state == 2 ? "complete" : cost.state == 3 ? "incomplete" : "idle"); text(&f, "\n");
    text(&f, cost.fixture ? "mode=fixture\n" : "mode=user\n");
    field(&f, "owner", cost.owner); field(&f, "timebase_hz", cost.frequency);
    field(&f, "resolution_ns_numerator", 1000000000); field(&f, "resolution_ns_denominator", cost.frequency);
    field(&f, "start_ticks", cost.start); field(&f, "end_ticks", cost.end);
    field(&f, "storage_bytes", cost_storage); field(&f, "task_bytes", sizeof(struct kernel_cost_task) + 16);
    field(&f, "overflow", cost.overflow); field(&f, "inflight", cost.inflight);
    for (unsigned lane = 0; lane < 3; ++lane) for (unsigned m = 0; m < COST_METRIC_COUNT; ++m) {
        char key[128]; size_t n = strlen(lanes[lane]); memcpy(key, lanes[lane], n); key[n++] = '.';
        size_t len = strlen(names[m]); memcpy(key + n, names[m], len); n += len; key[n++] = '.'; key[n] = 0;
        text(&f, key); text(&f, "unit="); text(&f, units[m]); text(&f, "\n");
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
