#ifndef BOAROS_KERNEL_COST_H
#define BOAROS_KERNEL_COST_H
#include <stddef.h>
#include <stdint.h>
#ifndef BOAROS_COST_DIAGNOSTICS
#define BOAROS_COST_DIAGNOSTICS 0
#endif
#if BOAROS_COST_DIAGNOSTICS
struct kernel_cost_task {
    uint64_t epoch, run_start, ready_start, blocked_start, scope_epoch;
    uint32_t depth;
    uint8_t suppress, operation, wait_rank, wait_flags;
};
_Static_assert(sizeof(struct kernel_cost_task) + 16 <= 64, "cost task budget");
enum kernel_cost_metric {
#define X(id, name, unit, hist) COST_##id,
#include <kernel/cost.def>
#undef X
    COST_METRIC_COUNT
};
struct kernel_cost_io_scope { struct kernel_cost_task *actor; unsigned previous; };
struct kernel_cost_io_scope kernel_cost_io_enter(unsigned operation);
void kernel_cost_io_leave(struct kernel_cost_io_scope *scope);
void kernel_cost_add_io(unsigned offset, uint64_t value);
#define COST_IO_SCOPE(name, operation) struct kernel_cost_io_scope name __attribute__((cleanup(kernel_cost_io_leave))) = kernel_cost_io_enter(operation)
#define COST_IO_ADD(offset, value) kernel_cost_add_io(offset, value)
struct kernel_cost_tag { uint64_t epoch; unsigned lane; };
struct kernel_cost_scope {
    struct kernel_cost_tag tag;
    uint64_t start;
    struct kernel_cost_task *actor;
    enum kernel_cost_metric metric;
};
uint64_t kernel_cost_clock(void);
uint64_t kernel_cost_lock(void);
void kernel_cost_unlock(uint64_t status);
struct kernel_cost_task *kernel_cost_current(void);
uint64_t kernel_cost_epoch(void);
struct kernel_cost_tag kernel_cost_capture(void);
void kernel_cost_add_tag(struct kernel_cost_tag tag, enum kernel_cost_metric metric, uint64_t value);
void kernel_cost_sample_tag(struct kernel_cost_tag tag, enum kernel_cost_metric metric, uint64_t value);
void kernel_cost_add(enum kernel_cost_metric metric, uint64_t value);
void kernel_cost_sample(enum kernel_cost_metric metric, uint64_t value);
struct kernel_cost_scope kernel_cost_enter(enum kernel_cost_metric metric);
void kernel_cost_leave(struct kernel_cost_scope *scope);
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
#define COST_ADD(metric, value) ((void)0)
#define COST_SAMPLE(metric, value) ((void)0)
#define COST_SCOPE(name, metric) ((void)0)
#endif
#endif
