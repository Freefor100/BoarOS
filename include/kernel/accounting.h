#ifndef BOAROS_KERNEL_ACCOUNTING_H
#define BOAROS_KERNEL_ACCOUNTING_H
#include <stdint.h>
struct kernel_task;
struct kernel_linux_rusage;
struct kernel_cpu_account_scope { struct kernel_task *task; uint32_t restore_mode; };
void kernel_cpu_account_trap_begin(struct kernel_cpu_account_scope *, int user, int interrupt);
void kernel_cpu_account_trap_end(struct kernel_cpu_account_scope *);
void kernel_cpu_account_user_return(void);
int kernel_task_getrusage(struct kernel_task *, int32_t who, struct kernel_linux_rusage *);
int kernel_task_cpu_clock(struct kernel_task *, int32_t clock, int resolution, uint64_t *);
/* I/O在实际发起/首次弄脏时记给原调用者，背景执行沿现有account_task借用。 */
void kernel_task_account_read(uint64_t bytes);
void kernel_task_account_write(uint64_t bytes);
uint64_t kernel_task_read_bytes(const struct kernel_task *);
void kernel_task_account_fault(struct kernel_task *, int major);
#endif
