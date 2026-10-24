#include <kernel/cpu.h>
#include <kernel/time.h>
#include "../../../kernel/sched/private.h"
#include <assert.h>
#if BOAROS_MAX_CPUS > 1
/* Models without a scheduler/clock have not published secondary CPUs or
 * initialized time. Their object locks take the ordinary sleeping path. */
__attribute__((weak)) struct kernel_scheduler scheduler;
__attribute__((weak)) uint64_t kernel_time_monotonic_ns(void) { return 0; }
__attribute__((weak)) enum kernel_time_status kernel_time_deadline_from_monotonic(
    uint64_t target,uint64_t *deadline)
{ (void)target;(void)deadline;return KERNEL_TIME_STATUS_NOT_INITIALIZED; }
__attribute__((weak)) uint64_t riscv_time_read(void) { return 0; }
/* 宿主等待器直接发布 pthread 可见状态，不模拟硬件 IPI 排队。
 * 有专用 CPU 模型的夹具覆盖这两个平台入口。 */
__attribute__((weak)) void kernel_assert_service_context(void)
{ assert(!kernel_cpu_current()->logical_id); }
__attribute__((weak)) void kernel_cpu_flush_notifications(void)
{ assert(!kernel_cpu_current()->outbound); }
__attribute__((weak)) void kernel_cpu_notify(struct kernel_cpu *cpu, unsigned events)
{ __atomic_fetch_or(&cpu->events, events, __ATOMIC_RELEASE); }
#endif

#include <kernel/posix_timer.h>
#include "../../../kernel/signal_pending.h"
#include <stdlib.h>
#include <kernel/pidfd.h>
/* Fixtures without pidfd OFDs have no observer notifications. The pidfd owner
 * fixture links its real implementation and controls callbacks directly. */
__attribute__((weak)) struct kernel_pid *kernel_pidfd_exit_identity(struct kernel_task *task)
{ (void)task; return 0; }
__attribute__((weak)) void kernel_pidfd_notification_end(struct kernel_pid **id)
{ assert(!*id); }
/* Scheduler/ordinary-signal fixtures publish no POSIX objects. Native timer
 * ownership has its own fixture linked with the actual timer implementation. */
__attribute__((weak)) uint64_t kernel_posix_timer_deadline(void) { return 0; }
__attribute__((weak)) void kernel_posix_timer_expire(uint64_t now) { (void)now;abort(); }
__attribute__((weak)) void kernel_posix_timers_exec(struct kernel_task *task)
{ assert(!task->group_leader || !task->group_leader->posix_timers); }
__attribute__((weak)) void kernel_posix_timers_exit(struct kernel_task *task)
{ assert(!task->posix_timers); }
__attribute__((weak)) void kernel_posix_timers_adopt_locked(struct kernel_task *to,struct kernel_task *from)
{ assert(!to->posix_timers&&!from->posix_timers); }
__attribute__((weak)) void kernel_posix_timer_unignore(struct kernel_task *task,unsigned sig)
{ (void)task;(void)sig;abort(); }
__attribute__((weak)) int posix_timer_record_take_locked(struct signal_queue_record *r,struct kernel_signal_wait_info *info,struct signal_pending_prepared *p)
{ (void)r;(void)info;(void)p;abort(); }
__attribute__((weak)) void posix_timer_record_clear_locked(struct signal_queue_record *r,int ignored,struct signal_pending_prepared *p)
{ (void)r;(void)ignored;(void)p;abort(); }
__attribute__((weak)) void posix_timer_record_complete(struct signal_queue_record *r) { (void)r;abort(); }
__attribute__((weak)) void posix_timer_record_free(struct signal_queue_record *r) { (void)r;abort(); }

__attribute__((weak)) void scheduler_cpu_timer_update_locked(struct kernel_task *t,unsigned thread)
{ assert(!t->cpu_timer_index[thread].timers[0]&&!t->cpu_timer_index[thread].timers[1]&&!t->cpu_timer_index[thread].timers[2]); }
__attribute__((weak)) void scheduler_cpu_timer_expire(void) {}
__attribute__((weak)) void kernel_posix_cpu_target_exit(struct kernel_task *t)
{ assert(!t->cpu_timer_index[0].indexed&&!t->cpu_timer_index[1].indexed); }
