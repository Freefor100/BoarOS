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
static const char *const names[] = {
#define X(id, name, unit, hist) #name,
#include <kernel/cost.def>
#undef X
};
static const char *const units[] = {
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
static const char *const lanes[] = {"foreground", "background", "observer"};
static struct {
    struct cost_counter counters[3][COST_METRIC_COUNT];
    uint64_t bins[3][cost_histograms][65];
    uint64_t epoch, owner, start, end, inflight;
    uint32_t frequency, fixture, active, pending, overflow, state;
} cost;
_Static_assert(sizeof(cost) + sizeof(names) + sizeof(units) + sizeof(histograms) <= 65536, "cost aggregate budget");
static void add_checked(uint64_t *to, uint64_t value)
{
    if (UINT64_MAX - *to < value) { cost.overflow = 1; *to = UINT64_MAX; }
    else *to += value;
}
uint64_t kernel_cost_epoch(void) { return cost.epoch; }
struct kernel_cost_tag kernel_cost_capture(void)
{
    ATOMIC_SCOPE;
    struct kernel_cost_task *task = kernel_cost_current();
    struct kernel_cost_tag tag = {0};
    if (cost.active && (!task || !task->suppress)) {
        tag.epoch = cost.epoch;
        tag.lane = cost.fixture || (task && task->epoch == cost.epoch) ? 0 : 1;
    }
    return tag;
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
    if (scope.actor) { scope.previous = scope.actor->operation; scope.actor->operation = operation + 1; }
    return scope;
}
void kernel_cost_io_leave(struct kernel_cost_io_scope *scope)
{ if (scope->actor) scope->actor->operation = scope->previous; }
void kernel_cost_add_io(unsigned offset, uint64_t value)
{
    struct kernel_cost_task *task = kernel_cost_current();
    if (task && task->operation && task->operation <= 4)
        kernel_cost_add((enum kernel_cost_metric)(COST_FILE_CALLS + (task->operation - 1) * 8 + offset), value);
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
void kernel_cost_join(struct kernel_cost_task *task)
{
    ATOMIC_SCOPE;
    task->epoch = cost.epoch;
    task->run_start = task->ready_start = task->blocked_start = 0;
    task->scope_epoch = cost.epoch;
    add_checked(&cost.inflight, task->depth);
}
void kernel_cost_leave(struct kernel_cost_scope *scope)
{
    ATOMIC_SCOPE;
    struct kernel_cost_tag tag = scope->tag;
    if (scope->actor) {
        if (!scope->actor->depth) return; /* Cancel consumes abandoned stack scopes. */
        scope->actor->depth--;
        if (scope->actor->scope_epoch != cost.epoch) return;
        tag = (struct kernel_cost_tag){cost.epoch, scope->actor->epoch == cost.epoch ? 0 : 1};
    }
    if (!tag.epoch || tag.epoch != cost.epoch) return;
    uint64_t start = scope->start < cost.start ? cost.start : scope->start;
    kernel_cost_sample_tag(tag, scope->metric, kernel_cost_clock() - start);
    if (!cost.inflight) __builtin_trap();
    cost.inflight--;
    scope->tag.epoch = 0;
}
void kernel_cost_cancel(struct kernel_cost_task *task)
{
    ATOMIC_SCOPE;
    if (task->scope_epoch == cost.epoch && task->depth) {
        if (cost.inflight < task->depth) __builtin_trap();
        cost.inflight -= task->depth;
        kernel_cost_add_tag((struct kernel_cost_tag){cost.epoch, 0}, COST_CANCELLED, task->depth);
    }
    task->depth = 0;
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
    if (cost.active) cost.start = kernel_cost_clock();
    return 0;
}
int kernel_cost_end(uint64_t owner, int deferred)
{
    ATOMIC_SCOPE;
    if (!cost.active || cost.pending) return -KERNEL_EINVAL;
    if (owner != cost.owner) return -KERNEL_EPERM;
    if (cost.inflight) return -KERNEL_EBUSY;
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
    field(&f, "version", 1); field(&f, "epoch", cost.epoch);
    text(&f, "state="); text(&f, cost.state == 2 ? "complete" : cost.state == 3 ? "incomplete" : "idle"); text(&f, "\n");
    text(&f, cost.fixture ? "mode=fixture\n" : "mode=user\n");
    field(&f, "owner", cost.owner); field(&f, "timebase_hz", cost.frequency);
    field(&f, "resolution_ns_numerator", 1000000000); field(&f, "resolution_ns_denominator", cost.frequency);
    field(&f, "start_ticks", cost.start); field(&f, "end_ticks", cost.end);
    field(&f, "storage_bytes", sizeof(cost)); field(&f, "task_bytes", sizeof(struct kernel_cost_task));
    field(&f, "overflow", cost.overflow); field(&f, "inflight", cost.inflight);
    for (unsigned lane = 0; lane < 3; ++lane) for (unsigned m = 0; m < COST_METRIC_COUNT; ++m) {
        char key[128]; size_t n = strlen(lanes[lane]); memcpy(key, lanes[lane], n); key[n++] = '.';
        size_t len = strlen(names[m]); memcpy(key + n, names[m], len); n += len; key[n++] = '.'; key[n] = 0;
        text(&f, key); text(&f, "unit="); text(&f, units[m]); text(&f, "\n");
        const char *suffix[] = {"value", "samples", "max"};
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
