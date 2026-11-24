#ifndef BOAROS_KERNEL_ACCOUNTING_H
#define BOAROS_KERNEL_ACCOUNTING_H
#include <stdint.h>
struct kernel_task;
struct kernel_linux_rusage;
enum kernel_cpu_time {
    KERNEL_CPU_TIME_USER, KERNEL_CPU_TIME_NICE, KERNEL_CPU_TIME_SYSTEM,
    KERNEL_CPU_TIME_IDLE, KERNEL_CPU_TIME_IRQ, KERNEL_CPU_TIME_COUNT,
};
struct kernel_cpu_statistics {
    uint64_t ticks[KERNEL_CPU_TIME_COUNT];
    uint64_t interrupts, switches, tasks;
};
/* Return a bitmap of online CPU indices and timing in hardware ticks. */
unsigned kernel_cpu_statistics_snapshot(struct kernel_cpu_statistics *);
void kernel_scheduler_task_statistics(uint64_t *runnable, uint64_t *io_waiters);
struct kernel_task_usage {
    uint64_t user_ns, kernel_ns;
    uint64_t minor_faults, major_faults, maxrss_pages;
    uint64_t voluntary, involuntary, read_bytes, write_bytes;
    uint64_t rchar, wchar, syscr, syscw, cancelled_write_bytes;
};
struct kernel_cpu_account_scope { struct kernel_task *task; uint32_t restore_mode; };
/* 架构trap/返回入口已关闭本地IRQ；数值发布期间不等待、不迁移。 */
void kernel_cpu_account_trap_begin(struct kernel_cpu_account_scope *, int user, int interrupt);
void kernel_cpu_account_interrupt_end(struct kernel_cpu_account_scope *);
static inline void kernel_cpu_account_trap_end(struct kernel_cpu_account_scope *scope)
{
    if (scope->task) kernel_cpu_account_interrupt_end(scope);
}
void kernel_cpu_account_user_return(void);
int kernel_task_getrusage(struct kernel_task *, int32_t who, struct kernel_linux_rusage *);
int kernel_task_cpu_clock(struct kernel_task *, int32_t clock, int resolution, uint64_t *);
int kernel_task_perf_clock_authorize(const struct kernel_task *caller,
    const struct kernel_task *target, int perfmon);
/* Caller owns a task storage borrow, including after exit. */
uint64_t kernel_task_scheduled_ns(const struct kernel_task *);
/* I/O在实际发起/首次弄脏时记给原调用者，背景执行沿现有account_task借用。 */
void kernel_task_account_read(uint64_t bytes);
void kernel_task_account_write(uint64_t bytes);
void kernel_task_account_cancelled_write(uint64_t bytes);
void kernel_task_account_io_return(struct kernel_task *, int64_t completed, unsigned directions);
#define KERNEL_IO_ACCOUNT_READ 1U
#define KERNEL_IO_ACCOUNT_WRITE 2U
#define KERNEL_IO_ACCOUNT_VECTOR 4U
uint64_t kernel_task_read_bytes(const struct kernel_task *);
void kernel_task_account_fault(struct kernel_task *, int major);
#endif
