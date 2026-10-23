#ifndef BOAROS_KERNEL_COST_H
#define BOAROS_KERNEL_COST_H
#include <stddef.h>
#include <stdint.h>
#include <kernel/cpu_config.h>
#ifndef BOAROS_COST_DIAGNOSTICS
#define BOAROS_COST_DIAGNOSTICS 0
#endif
#if BOAROS_COST_DIAGNOSTICS
struct kernel_cost_task {
    uint64_t epoch, run_start, ready_start, blocked_start, scope_epoch;
    uint32_t depth;
    uint8_t suppress, operation, wait_rank, wait_flags;
};
/* Two backend guards and four inline resource request guards live in the
 * task's IO context. SMP service accounting retains two further timestamps. */
#define KERNEL_COST_TASK_BYTES (sizeof(struct kernel_cost_task) + 6U * 16U + (BOAROS_USER_SMP ? 16U : 0U))
#define KERNEL_COST_CPU_SERVICE_BYTES (BOAROS_USER_SMP ? 5U * sizeof(uint64_t) : 0U)
_Static_assert(KERNEL_COST_TASK_BYTES <= 160, "cost task budget");
enum kernel_cost_metric {
#define X(id, name, unit, hist) COST_##id,
#include <kernel/cost.def>
#undef X
    COST_METRIC_COUNT
};
struct kernel_cost_io_scope { struct kernel_cost_task *actor; unsigned previous; };
struct kernel_cost_io_scope kernel_cost_io_enter(unsigned operation);
struct kernel_cost_io_scope kernel_cost_phase_enter(unsigned phase);
void kernel_cost_io_leave(struct kernel_cost_io_scope *scope);
void kernel_cost_add_io(unsigned offset, uint64_t value);
#define COST_IO_SCOPE(name, operation) struct kernel_cost_io_scope name __attribute__((cleanup(kernel_cost_io_leave))) = kernel_cost_io_enter(operation)
#define COST_IO_ADD(offset, value) kernel_cost_add_io(offset, value)
#define COST_PHASE_SCOPE(name, phase) struct kernel_cost_io_scope name __attribute__((cleanup(kernel_cost_io_leave))) = kernel_cost_phase_enter(phase)
struct kernel_cost_tag { uint64_t epoch; unsigned lane; };
/* The first three words are used before trap register restoration. */
struct kernel_cost_cpu {
    uint64_t trap_timestamp, return_timestamp, return_pending;
    struct kernel_cost_tag irq_tag;
    uint64_t irq_start;
    struct kernel_cost_task *running;
    uint32_t irq_opened, irq_suppressed, lock_depth, registered;
    struct kernel_cost_task bootstrap;
    uint64_t observer_wait_started;
    /* Physical IRQ entry publishes its supplied boundary before aggregate wait. */
    uint64_t irq_pending_start;
};
_Static_assert(sizeof(struct kernel_cost_cpu) == 136, "cost CPU storage");
struct kernel_cost_cpu *kernel_cost_cpu_current(void);
void kernel_cost_register_cpu(struct kernel_cost_cpu *cpu);
struct kernel_cost_scope {
    struct kernel_cost_tag tag;
    uint64_t start;
    struct kernel_cost_task *actor;
    enum kernel_cost_metric metric;
};
uint64_t kernel_cost_clock(void);
void kernel_cost_irq_disabled(uint64_t ticks);
void kernel_cost_irq_enabled(uint64_t ticks);
void kernel_cost_irq_suppress(void);
void kernel_cost_irq_enable_now(void);
void kernel_cost_irq_return(uint64_t status);
void kernel_cost_irq_flush(void);
void kernel_cost_irq_observer(uint64_t ticks);
uint64_t kernel_cost_lock(void);
void kernel_cost_unlock(uint64_t status);
/* These two hooks change only the local IRQ bit and never observe COST. */
uint64_t kernel_cost_irq_save(void);
void kernel_cost_irq_restore(uint64_t enabled);
struct kernel_cost_task *kernel_cost_current(void);
uint64_t kernel_cost_epoch(void);
struct kernel_cost_tag kernel_cost_capture(void);
void kernel_cost_add_tag(struct kernel_cost_tag tag, enum kernel_cost_metric metric, uint64_t value);
/* One metadata region publishes three counters with one bounded observer update. */
void kernel_cost_page_metadata(struct kernel_cost_tag tag, uint64_t checked,
                              uint64_t written, uint64_t ticks);
void kernel_cost_raw_lock(struct kernel_cost_tag tag, unsigned rank, uint64_t wait, uint64_t hold);
void kernel_cost_sample_tag(struct kernel_cost_tag tag, enum kernel_cost_metric metric, uint64_t value);
void kernel_cost_add(enum kernel_cost_metric metric, uint64_t value);
void kernel_cost_sample(enum kernel_cost_metric metric, uint64_t value);
struct kernel_cost_scope kernel_cost_enter(enum kernel_cost_metric metric);
void kernel_cost_leave(struct kernel_cost_scope *scope);
/* A resource publisher supplies the instant its physical qualification ended. */
void kernel_cost_leave_at(struct kernel_cost_scope *scope, uint64_t end);
void kernel_cost_cancel(struct kernel_cost_task *task);
struct kernel_cost_tag;
void kernel_cost_join(struct kernel_cost_task *task);
void kernel_cost_rebase(struct kernel_cost_task *task);
void kernel_cost_block(struct kernel_cost_task *task);
void kernel_cost_wake(struct kernel_cost_task *task);
void kernel_cost_timeout(struct kernel_cost_task *task, uint64_t deadline);
void kernel_cost_ready(struct kernel_cost_task *task);
struct kernel_cost_tag kernel_cost_task_tag(const struct kernel_cost_task *task);
unsigned kernel_cost_rank(unsigned rank);
enum kernel_cost_metric kernel_cost_lock_metric(unsigned rank);
int kernel_cost_begin(uint64_t owner, uint32_t frequency, int fixture, int deferred);
int kernel_cost_end(uint64_t owner, int deferred);
void kernel_cost_boundary(void);
void kernel_cost_abort(uint64_t owner);
int kernel_cost_format(char *buffer, size_t capacity);
int kernel_cost_read(unsigned lane, enum kernel_cost_metric metric, uint64_t *value);
size_t kernel_cost_format_capacity(void);
void kernel_cost_set_timebase(uint32_t frequency);
uint32_t kernel_cost_timebase(void);
void kernel_cost_account(struct kernel_cost_task *task);
void kernel_cost_switch(struct kernel_cost_task *previous, struct kernel_cost_task *next);
void kernel_cost_syscall(uint64_t number, int64_t fd);
struct kernel_files;
int kernel_files_cost_descriptor(struct kernel_files *files, int64_t fd);
/* Scheduler bridge: no persistent object references in diagnostic storage. */
int kernel_cost_control(const char *command, size_t size);
void kernel_cost_user_return(void);
void kernel_cost_task_exit(void);
void kernel_cost_inherit(struct kernel_cost_task *child, const struct kernel_cost_task *parent);
#define COST_ADD(metric, value) kernel_cost_add(COST_##metric, (value))
#define COST_SAMPLE(metric, value) kernel_cost_sample(COST_##metric, (value))
#define COST_SCOPE(name, metric) struct kernel_cost_scope name __attribute__((cleanup(kernel_cost_leave))) = kernel_cost_enter(COST_##metric)
#else
#define COST_IO_SCOPE(name, operation) ((void)0)
#define COST_IO_ADD(offset, value) ((void)0)
#define COST_PHASE_SCOPE(name, phase) ((void)0)
#define COST_ADD(metric, value) ((void)0)
#define COST_SAMPLE(metric, value) ((void)0)
#define COST_SCOPE(name, metric) ((void)0)
#endif
#endif
