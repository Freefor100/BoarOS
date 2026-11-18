/* Real ptrace request and stop-publication paths; no host context switch. */
#include "../../kernel/sched/ptrace.c"
#include <assert.h>
#include <stdio.h>

extern void signal_owner_set_allocation_hook(void (*)(void));
static struct kernel_task *allocation_transfer_from;
static struct kernel_task allocation_replacement;
static struct kernel_task *copy_terminate;
static void transfer_during_prepare(void)
{
    signal_owner_set_allocation_hook(NULL);
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    kernel_pid_transfer(&allocation_transfer_from->identities[KERNEL_PID_TID],
        &allocation_replacement.identities[KERNEL_PID_TID],&allocation_replacement);
}
size_t arch_ptrace_regset_size(unsigned note) { (void)note;__builtin_trap(); }
void arch_ptrace_regset(struct kernel_task *t,unsigned note,uint64_t *data,uint64_t *arg,int write)
{ (void)t;(void)note;(void)data;(void)arg;(void)write;__builtin_trap(); }
int arch_ptrace_user(struct kernel_task *t,uint64_t off,uint64_t *word,uint64_t *arg,int write)
{ (void)t;(void)off;(void)word;(void)arg;(void)write;__builtin_trap(); }
void arch_ptrace_syscall(struct arch_trap_frame *f,uint32_t *arch,uint64_t *nr,uint64_t *args,uint64_t *ret)
{ (void)f;(void)arch;(void)nr;(void)args;(void)ret;__builtin_trap(); }
int kernel_mm_remote_transfer(struct kernel_mm *mm,uint64_t address,void *buffer,size_t size,int write,int force,size_t *copied)
{ (void)mm;(void)address;(void)buffer;(void)size;(void)write;(void)force;(void)copied;__builtin_trap(); }
enum kernel_uaccess_status kernel_copy_to_user(struct kernel_mm *mm,uint64_t address,const void *src,size_t size,size_t *copied)
{
    (void)mm;
    if(copy_terminate) {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        assert(copy_terminate->ptrace->access && copy_terminate->reaping);
        ptrace_terminate_locked(copy_terminate);
        assert(copy_terminate->ptrace->release_deferred);
        copy_terminate=NULL;
    }
    memcpy((void *)(uintptr_t)address,src,size);*copied=size;return KERNEL_UACCESS_STATUS_OK;
}
enum kernel_uaccess_status kernel_copy_from_user(struct kernel_mm *mm,void *dst,uint64_t address,size_t size,size_t *copied)
{ (void)mm;memcpy(dst,(void *)(uintptr_t)address,size);*copied=size;return KERNEL_UACCESS_STATUS_OK; }

static void publish_stop(struct kernel_task *task,unsigned event,uint64_t message,const struct kernel_pid *pid)
{
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    assert(prepare_stop_locked(task,STOP_STATUS(5,event),message,0,NULL,pid,(struct kernel_wait_token){0}));
    task->ptrace->phase=TRACE_STOPPED;task->ptrace->unread=1; /* The park/context switch is outside this model. */
}
void pid_namespace_ptrace_cases(struct kernel_task *root,struct kernel_task *inner,
    struct kernel_task *nested,struct kernel_task *sibling,struct kernel_task *child)
{
    kernel_pid_t nested_local=kernel_pid_number_in(process_identity(nested,KERNEL_PID_TID),process_pid_active(inner));
    assert(nested_local!=nested->tid);
    allocation_transfer_from=nested;
    signal_owner_set_allocation_hook(transfer_during_prepare);
    assert(kernel_ptrace_request(inner,SEIZE,nested_local,0,0)==-KERNEL_ESRCH);
    assert(!nested->storage_borrows && !nested->ptrace->tracer);
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        kernel_pid_transfer(&allocation_replacement.identities[KERNEL_PID_TID],
            &nested->identities[KERNEL_PID_TID],nested);
    }
    struct kernel_credentials denied={.uid=123,.euid=123,.suid=123};
    inner->credentials=&denied;
    assert(kernel_ptrace_request(inner,SEIZE,nested_local,0,0)==-KERNEL_EPERM);
    inner->credentials=NULL;
    assert(kernel_ptrace_request(inner,SEIZE,nested_local,0,0)==0);
    assert(nested->ptrace->tracer==inner);
    assert(kernel_ptrace_request(sibling,SEIZE,nested_local,0,0)==-KERNEL_ESRCH);
    assert(kernel_ptrace_request(inner,GETEVENTMSG,nested->tid,0,0)==-KERNEL_ESRCH);
    uint32_t old=kernel_ptrace_exec_old_tid(nested);
    assert(old==(uint32_t)nested_local);
    publish_stop(nested,TRACE_FORK,0,process_identity(child,KERNEL_PID_TID));
    uint64_t message=0;
    assert(!kernel_ptrace_request(inner,GETEVENTMSG,nested_local,0,(uintptr_t)&message));
    assert(message==(uint32_t)kernel_pid_number_in(process_identity(child,KERNEL_PID_TID),process_pid_active(inner)));
    struct kernel_signal_wait_info info={0};
    assert(!kernel_ptrace_request(inner,GETSIGINFO,nested_local,0,(uintptr_t)&info));
    assert(info.sender==1); /* Linux synthetic stop siginfo uses tracee-local TID. */
    uint64_t mask=UINT64_C(1)<<14,observed=0;
    assert(!kernel_ptrace_request(inner,SETSIGMASK,nested_local,8,(uintptr_t)&mask));
    assert(!kernel_ptrace_request(inner,GETSIGMASK,nested_local,8,(uintptr_t)&observed));
    assert(observed==mask);mask=0;
    assert(!kernel_ptrace_request(inner,SETSIGMASK,nested_local,8,(uintptr_t)&mask));
    info.sender=12345;info.code=-1;
    assert(!kernel_ptrace_request(inner,SETSIGINFO,nested_local,0,(uintptr_t)&info));
    assert(nested->ptrace->info.sender==12345);
    assert(!kernel_ptrace_request(inner,DETACH,nested_local,0,10));
    assert(nested->ptrace->resume_sender==0 && !nested->ptrace->tracer);
    assert(!kernel_ptrace_request(root,SEIZE,nested->tid,0,0));
    assert(kernel_ptrace_exec_old_tid(nested)==(uint32_t)nested->tid);
    publish_stop(nested,TRACE_EXEC,old,NULL);
    assert(!kernel_ptrace_request(root,GETEVENTMSG,nested->tid,0,(uintptr_t)&message));
    assert(message==old); /* Exec consumes the pre-adoption scalar, not a PID lookup. */
    assert(!kernel_ptrace_request(root,DETACH,nested->tid,0,0));

    /* TRACEME follows the retained creator identity, not its active-layer
     * integer reinterpreted in the initial namespace. */
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        child->child_creator=process_identity(inner,KERNEL_PID_TID);
        kernel_pid_get(child->child_creator);
    }
    assert(!kernel_ptrace_request(child,TRACEME,0,0,0));
    assert(child->ptrace->tracer==inner);
    /* A stopped user copy and a concurrent wait claimant jointly defer the
     * shutdown wake. The real access release cannot resume the token early. */
    kernel_cpu_current()->current=child;
    struct kernel_wait_token token;
    assert(!kernel_wait_prepare(NULL,0,0,&token));
    kernel_cpu_current()->current=NULL;
    publish_stop(child,TRACE_STOP,0,NULL);
    child->ptrace->token=token;
    struct kernel_task *claimed=NULL;uint32_t event=0;int matching=0;
    assert(ptrace_select(inner,0,NULL,2,&claimed,&event,&matching));
    assert(claimed==child && event==TRACE_WAIT_STOP);
    copy_terminate=child;
    kernel_pid_t access_tid=kernel_pid_number_in(process_identity(child,KERNEL_PID_TID),process_pid_active(inner));
    assert(!kernel_ptrace_request(inner,GETEVENTMSG,access_tid,0,(uintptr_t)&message));
    assert(!copy_terminate && child->ptrace->release_deferred && !child->ptrace->access);
    assert(child->ptrace->phase==TRACE_STOPPED && child->reaping);
    scheduler_task_wait_claim_release(child);ptrace_wait_claim_done(child);scheduler_task_put(child);
    assert(!child->ptrace->release_deferred && child->ptrace->phase==TRACE_RUNNING);
    kernel_cpu_current()->current=child;
    assert(!kernel_wait_finish(&token));
    kernel_cpu_current()->current=NULL;child->ptrace->token=(struct kernel_wait_token){0};
    kernel_pid_t child_local=kernel_pid_number_in(process_identity(child,KERNEL_PID_TID),process_pid_active(inner));
    publish_stop(child,TRACE_STOP,0,NULL);
    assert(!kernel_ptrace_request(inner,DETACH,child_local,0,10));
    assert(child->ptrace->resume_sender==1);
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        kernel_pid_put(child->child_creator);child->child_creator=NULL;
    }
    struct kernel_task tracer_thread={.magic=KERNEL_THREAD_MAGIC,.accounted=1,
        .state=KERNEL_THREAD_STATE_READY,.group_leader=inner};
    tracer_thread.arch.user_mode=1;
    assert(!process_identity_create(&tracer_thread,inner,1));
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        assert(!process_identity_admit_locked(&tracer_thread));process_identity_commit_locked(&tracer_thread);
    }
    assert(!kernel_ptrace_request(&tracer_thread,SEIZE,child_local,0,0));
    publish_stop(child,TRACE_STOP,0,NULL);
    assert(!kernel_ptrace_request(&tracer_thread,DETACH,child_local,0,10));
    assert(child->ptrace->resume_sender==(uint32_t)kernel_pid_number_in(
        process_identity(&tracer_thread,KERNEL_PID_TID),process_pid_active(child)));
    assert(child->ptrace->resume_sender!=1); /* TID, not tracer's TGID. */
    kernel_ptrace_dispose(&tracer_thread);tracer_thread.ptrace=NULL;
    process_identity_release(&tracer_thread);process_identity_collect();
    struct kernel_task *tasks[]={root,inner,nested,child};
    for(unsigned i=0;i<4;i++) {
        assert(!tasks[i]->storage_borrows);
        kernel_ptrace_dispose(tasks[i]);tasks[i]->ptrace=NULL;
    }
    puts("PID ptrace views: request isolation, creator identity, event snapshot and injected sender passed");
}

void pid_namespace_ptrace_final_link(struct kernel_task *tracer,struct kernel_task *child)
{
    assert(!kernel_ptrace_request(tracer,SEIZE,child->tid,0,0));
    struct kernel_ptrace_notice notice;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        child->proc_exiting=1;
        assert(ptrace_publish_final_locked(child,&notice));
    }
    kernel_ptrace_switch_notify(&notice);
}
