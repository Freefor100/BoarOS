#ifndef BOAROS_KERNEL_PTRACE_H
#define BOAROS_KERNEL_PTRACE_H
#include <arch/task.h>
#include <stdint.h>
#include <kernel/wait_internal.h>
struct kernel_ptrace_notice {
    struct kernel_wait_cursor waiters;
    struct kernel_task *tracer,*child;
    int why,signal_status;
};
struct kernel_signal_wait_info;
struct kernel_task;
int64_t kernel_ptrace_request(struct kernel_task *,uint64_t,int32_t,uint64_t,uint64_t);
int kernel_ptrace_syscall_enter(struct kernel_task *,struct arch_trap_frame *);
void kernel_ptrace_syscall_exit(struct kernel_task *,struct arch_trap_frame *,uint64_t);
int kernel_ptrace_stop_requested(struct kernel_task *);
void kernel_ptrace_user_return(struct kernel_task *,struct arch_trap_frame *);
uint32_t kernel_ptrace_signal(struct kernel_task *,struct kernel_signal_wait_info *);
int kernel_ptrace_switch_finish_locked(struct kernel_task *,struct kernel_ptrace_notice *);
void kernel_ptrace_switch_notify(struct kernel_ptrace_notice *);
void kernel_ptrace_dispose(struct kernel_task *);
int kernel_ptrace_group_pending_locked(const struct kernel_task *,unsigned);
int kernel_ptrace_group_stopped_locked(const struct kernel_task *);
int kernel_ptrace_group_prepare_locked(struct kernel_task *,unsigned);
int kernel_ptrace_group_stop(struct kernel_task *,unsigned);
void kernel_ptrace_group_continue_locked(struct kernel_task *);
void kernel_signal_notify_tracer(struct kernel_task *,struct kernel_task *,int why,int status);
int kernel_ptrace_exec_unsafe(struct kernel_task *);
void kernel_ptrace_exec(struct kernel_task *,uint32_t old_tid);
uint32_t kernel_ptrace_tracer_tid_locked(const struct kernel_task *);
#endif
