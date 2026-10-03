#ifndef BOAROS_KERNEL_TTY_TASK_H
#define BOAROS_KERNEL_TTY_TASK_H
#include <kernel/pid.h>
#include <stdint.h>
struct kernel_task;
struct kernel_tty;
struct kernel_tty_request;
/* Borrowed under the single-hart publication lock. One ref per thread group. */
struct kernel_tty *kernel_task_controlling_tty(const struct kernel_task *task);
int kernel_task_tty_set(struct kernel_task *task, struct kernel_tty *tty);
void kernel_task_tty_clear(struct kernel_task *task);
void kernel_task_tty_clear_session(struct kernel_tty *tty, struct kernel_pid *session);
struct kernel_pid *kernel_task_tty_identity(const struct kernel_task *task,
                                            enum kernel_pid_role role);
struct kernel_pid *kernel_task_tty_find_group(struct kernel_task *task, kernel_pid_t number);
int kernel_task_tty_session_leader(const struct kernel_task *task);
int kernel_task_tty_signal_ignored(const struct kernel_task *task, unsigned signal);
int kernel_task_tty_group_orphaned(const struct kernel_task *task);
void kernel_task_tty_signal_group(struct kernel_pid *group, unsigned signal);
void kernel_task_tty_signal_session(struct kernel_tty *tty, struct kernel_pid *session);
void kernel_task_tty_request_set(struct kernel_task *task, struct kernel_tty_request *request);
void kernel_task_tty_request_clear(struct kernel_task *task,
                                   struct kernel_tty_request *request);
void kernel_tty_get(struct kernel_tty *tty);
void kernel_tty_put(struct kernel_tty *tty);
uint64_t kernel_tty_rdev(const struct kernel_tty *tty);
int32_t kernel_tty_foreground(const struct kernel_tty *tty);
void kernel_tty_disassociate(struct kernel_tty *tty, int on_exit);
void kernel_tty_abort_request(struct kernel_tty_request *request);
#endif
