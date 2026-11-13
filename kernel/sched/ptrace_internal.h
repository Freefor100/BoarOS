#ifndef BOAROS_PTRACE_INTERNAL_H
#define BOAROS_PTRACE_INTERNAL_H
#include <kernel/ptrace.h>
#include <kernel/signal.h>
#include <kernel/credentials.h>
enum trace_event { TRACE_FORK=1,TRACE_VFORK,TRACE_CLONE,TRACE_EXEC,TRACE_VFORK_DONE,TRACE_EXIT,TRACE_STOP=128 };
enum { TRACE_RUNNING,TRACE_PARKING,TRACE_STOPPED,TRACE_LISTENING,TRACE_FINAL };
enum { TRACE_WAIT_STOP=4,TRACE_WAIT_FINAL };
/* Each indexed child belongs to exactly one tracer TID. All links and stop
 * publications use the scheduler domain. Storage lives until task disposal. */
struct kernel_ptrace_state {
    struct kernel_task *tracer,*first,*next,*previous,*access_target;
    const struct kernel_credentials *credentials;
    struct kernel_wait_token token;
    struct kernel_signal_wait_info info;
    uint64_t message,original_arg,epoch;
    uint32_t phase,status,options,seized,syscalls,operation,pending;
    int why;
    uint32_t unread,resume_signal,resume_sender,resume_uid,access,release_deferred,group_signal,group_pending,delivery_active,delivery_continued;
};
int ptrace_clone_prepare(struct kernel_task *,struct kernel_task *);
unsigned ptrace_clone_locked(struct kernel_task *,struct kernel_task *,uint64_t);
void ptrace_event(struct kernel_task *,unsigned,uint64_t);
void ptrace_begin_exit(struct kernel_task *,uint32_t);
void ptrace_terminate_locked(struct kernel_task *);
int ptrace_publish_final_locked(struct kernel_task *,struct kernel_ptrace_notice *);
void ptrace_consume_final(struct kernel_task *);
void process_release_traced_zombie(struct kernel_task *);
void ptrace_wait_claim_done(struct kernel_task *);
int ptrace_select(struct kernel_task *,int,const struct kernel_pid *,unsigned,struct kernel_task **,uint32_t *,int *);
uint32_t ptrace_wait_status_locked(struct kernel_task *,int);
size_t arch_ptrace_regset_size(unsigned);
void arch_ptrace_regset(struct kernel_task *,unsigned,uint64_t *,uint64_t *,int);
int arch_ptrace_user(struct kernel_task *,uint64_t,uint64_t *,uint64_t *,int);
void arch_ptrace_syscall(struct arch_trap_frame *,uint32_t *,uint64_t *,uint64_t *,uint64_t *);
#endif
