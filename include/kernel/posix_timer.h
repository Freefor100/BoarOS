#ifndef BOAROS_KERNEL_POSIX_TIMER_H
#define BOAROS_KERNEL_POSIX_TIMER_H
#include <stdint.h>
struct kernel_task;
struct kernel_posix_timer;
struct kernel_posix_event {
    uint64_t value;
    int32_t signal, notify, tid;
    uint8_t padding[44];
};
struct kernel_posix_timespec { int64_t seconds, nanoseconds; };
struct kernel_posix_itimerspec {
    struct kernel_posix_timespec interval, value;
};
int kernel_posix_timer_prepare(struct kernel_task *,int32_t,
    const struct kernel_posix_event *,int32_t *,struct kernel_posix_timer **);
void kernel_posix_timer_publish(struct kernel_posix_timer *,int);
int kernel_posix_timer_get(struct kernel_task *,int32_t,struct kernel_posix_itimerspec *);
int kernel_posix_timer_overrun(struct kernel_task *,int32_t);
int kernel_posix_timer_set(struct kernel_task *,int32_t,int,
    const struct kernel_posix_itimerspec *,struct kernel_posix_itimerspec *);
int kernel_posix_timer_delete(struct kernel_task *,int32_t);
uint64_t kernel_posix_timer_deadline(void);
void kernel_posix_timer_expire(uint64_t);
void kernel_posix_timer_unignore(struct kernel_task *,unsigned);
void kernel_posix_timers_exec(struct kernel_task *);
void kernel_posix_timers_exit(struct kernel_task *);
void kernel_posix_timers_adopt_locked(struct kernel_task *,struct kernel_task *);
#endif
