#include "private.h"
#include "ptrace_internal.h"
#include <kernel/errno.h>
#include <kernel/uaccess.h>
#include <string.h>

enum { TRACEME=0,PEEKTEXT=1,PEEKDATA,PEEKUSER,POKETEXT,POKEDATA,POKEUSER,CONT,KILL,
    ATTACH=16,DETACH,SYSCALL=24,SETOPTIONS=0x4200,GETEVENTMSG,GETSIGINFO,SETSIGINFO,
    GETREGSET,SETREGSET,SEIZE,INTERRUPT,LISTEN,GETSIGMASK=0x420a,SETSIGMASK,
    GET_SYSCALL_INFO=0x420e };
#define EXITKILL (1U<<20)
#define VALID_OPTIONS (EXITKILL|127U)
#define STOP_STATUS(signal,event) ((event)<<16|(signal)<<8|0x7fU)

static struct kernel_ptrace_state *state(const struct kernel_task *t)
{ return __atomic_load_n(&t->ptrace,__ATOMIC_ACQUIRE); }

static int ensure_state(struct kernel_task *t)
{
    if (state(t)) return 0;
    struct kernel_ptrace_state *s;
    if (kernel_heap_allocate_zeroed(&scheduler.identity_heap,1,sizeof(*s),(void **)&s))
        return -KERNEL_ENOMEM;
    struct kernel_ptrace_state *empty=0;
    if (!__atomic_compare_exchange_n(&t->ptrace,&empty,s,0,__ATOMIC_RELEASE,__ATOMIC_ACQUIRE))
        kernel_heap_release(&scheduler.identity_heap,s);
    return 0;
}

void kernel_ptrace_dispose(struct kernel_task *t)
{
    struct kernel_ptrace_state *s=state(t);
    if (!s) return;
    if (s->tracer || s->first || s->access || s->access_target || s->token.task || s->credentials) __builtin_trap();
    kernel_heap_release(&scheduler.identity_heap,s);
}

int kernel_ptrace_stop_requested(struct kernel_task *task)
{
    struct kernel_ptrace_state *s=state(task);
    if (!s) return 0;
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    return s->pending || s->group_pending;
}

uint32_t kernel_ptrace_tracer_tid_locked(const struct kernel_task *t)
{ return state(t) && t->ptrace->tracer ? t->ptrace->tracer->tid : 0; }

int kernel_ptrace_exec_unsafe(struct kernel_task *t)
{
    if (!state(t)) return 0;
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    return t->ptrace->tracer && !kernel_credentials_capable(t->ptrace->credentials,KERNEL_CAP_SYS_PTRACE);
}

static void link_locked(struct kernel_task *tracer,struct kernel_task *t,
    const struct kernel_credentials *cred,unsigned seized,unsigned options)
{
    struct kernel_ptrace_state *s=t->ptrace;
    s->tracer=tracer;
    s->next=tracer->ptrace->first;
    if (s->next) s->next->ptrace->previous=t;
    tracer->ptrace->first=t;
    if (++tracer->ptrace->epoch==0) __builtin_trap();
    s->credentials=kernel_credentials_get(cred);
    s->seized=seized; s->options=options; s->syscalls=0;
}

static const struct kernel_credentials *unlink_locked(struct kernel_task *t)
{
    struct kernel_ptrace_state *s=t->ptrace;
    if (++s->tracer->ptrace->epoch==0) __builtin_trap();
    if (s->previous) s->previous->ptrace->next=s->next;
    else s->tracer->ptrace->first=s->next;
    if (s->next) s->next->ptrace->previous=s->previous;
    s->next=s->previous=s->tracer=0;
    const struct kernel_credentials *cred=s->credentials;
    s->credentials=0; s->pending=0;
    /* Detach restores a previously accepted job stop even if its signal is
     * masked. The returning executor handles it through normal STOPPED state. */
    if (!t->proc_exiting && t->group_leader && t->group_leader->group_stopped)
        s->group_pending=s->group_signal?s->group_signal:(t->group_leader->wait_status>>8)&127;
    s->group_signal=0;
    return cred;
}

static void resume_locked(struct kernel_ptrace_state *s)
{
    s->unread=0;
    if (!s->token.task) return;
    if (s->access || s->token.task->reaping) { s->release_deferred=1; return; }
    s->phase=TRACE_RUNNING;
    (void)kernel_wait_notify_locked(&s->token,KERNEL_WAIT_WOKEN);
}

void ptrace_terminate_locked(struct kernel_task *t)
{
    if (state(t) && t->ptrace->token.task) resume_locked(t->ptrace);
}

static void request_stop_locked(struct kernel_task *t,unsigned status)
{
    t->ptrace->pending=status;
    if (t->ptrace->phase==TRACE_LISTENING) resume_locked(t->ptrace);
    if (t->state==KERNEL_THREAD_STATE_STOPPED) {
        KERNEL_RUN_SET(runs);
        stopped_unlink_locked(t); t->state=KERNEL_THREAD_STATE_READY;
        if (t->wait.on_cpu) t->wait.ready_pending=1;
        else ready_enqueue_locked(t,0);
    }
#if BOAROS_USER_SMP
    scheduler_signal_request_locked(t);
#endif
    kernel_wait_interrupt_locked(t);
}

static void prepare_notice_locked(struct kernel_task *t,struct kernel_ptrace_notice *notice)
{
    struct kernel_task *tracer=t->ptrace->tracer->group_leader;
    scheduler_task_get_locked(t); scheduler_task_get_locked(tracer);
    struct kernel_ptrace_state *s=t->ptrace;
    unsigned ws=s->status;
    int status;
    switch (s->why) {
    case 1: status=(ws>>8)&255; break; /* CLD_EXITED */
    case 2: case 3: status=ws&127; break;
    case 4: status=(ws>>8)&127; break; /* CLD_TRAPPED */
    case 5: status=s->group_signal ? s->group_signal :
        t->group_leader->group_stopped ? (t->group_leader->wait_status>>8)&127 : 0; break;
    default: __builtin_trap();
    }
    *notice=(struct kernel_ptrace_notice){.tracer=tracer,.child=t,.why=s->why,.signal_status=status};
    (void)kernel_wait_cursor_prepare(&tracer->child_exit_queue,&notice->waiters);
}

int kernel_ptrace_switch_finish_locked(struct kernel_task *t,struct kernel_ptrace_notice *notice)
{
    struct kernel_ptrace_state *s=state(t);
    if (!s || s->phase!=TRACE_PARKING || t->state!=KERNEL_THREAD_STATE_BLOCKED) return 0;
    if (t->wait.on_cpu || !s->tracer || !s->token.task) __builtin_trap();
    s->phase=TRACE_STOPPED; s->unread=1;
    prepare_notice_locked(t,notice);
    return 1;
}

void kernel_ptrace_switch_notify(struct kernel_ptrace_notice *n)
{
    if (n->waiters.queue) kernel_wait_cursor_wake(&n->waiters,1);
    kernel_signal_notify_tracer(n->tracer,n->child,n->why,n->signal_status);
    scheduler_task_put(n->child); scheduler_task_put(n->tracer);
}

/* Only the tracee owns this stack. The token pins execution here until the
 * tracer releases the stop; fatal wakeups wait for an in-flight memory copy. */
static unsigned stop(struct kernel_task *t,unsigned status,uint64_t message,unsigned operation,
    struct kernel_signal_wait_info *info)
{
    struct kernel_wait_token token;
    if (kernel_wait_prepare(0,0,0,&token)) __builtin_trap();
    struct kernel_ptrace_state *s=t->ptrace;
    int park;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        park=s->tracer && !t->proc_exiting && (!t->terminate_requested || status>>16==TRACE_EXIT);
        if (park) {
            s->token=token; s->phase=TRACE_PARKING; s->message=message;
            s->operation=operation; s->status=status; s->resume_signal=0;
            s->why=(status>>16==TRACE_STOP || (!info && s->group_signal &&
                (status>>8)==s->group_signal)) ? 5 : 4;
            s->info=info ? *info : (struct kernel_signal_wait_info){.signal=(status>>8&255)==133 ? 5 : (status>>8)&255,
                .code=(int32_t)(status>>8),.sender=(uint32_t)t->tid,.uid=kernel_credentials_task_locked(t)->uid};
        }
    }
    if (!park) {
        if (kernel_wait_finish(&token)) __builtin_trap();
        return info ? info->signal : 0;
    }
    enum kernel_wait_wake_reason reason;
    if (kernel_wait_park(&token,&reason) || kernel_wait_finish(&token)) __builtin_trap();
    unsigned signal;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        s->token=(struct kernel_wait_token){0};
        s->phase=TRACE_RUNNING; s->unread=0; signal=s->resume_signal;
        if (info && signal) {
            if (s->info.signal==signal) *info=s->info;
            else *info=(struct kernel_signal_wait_info){.signal=signal,.sender=s->resume_sender,.uid=s->resume_uid};
        }
    }
    return signal;
}

static int tracing(struct kernel_task *t,unsigned *options)
{
    if (!state(t)) return 0;
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    if (!t->ptrace->tracer) return 0;
    if (options) *options=t->ptrace->options;
    return 1;
}

static void syscall_stop(struct kernel_task *t,unsigned op,struct arch_trap_frame *frame)
{
    if (!state(t)) return;
    unsigned sig;
    uint64_t original_nr=0;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        struct kernel_ptrace_state *s=t->ptrace;
        if (!s->tracer) return;
        if (op==1) {
#if defined(BOAROS_ARCH_LOONGARCH)
            s->original_arg=frame->regs[4];
#else
            s->original_arg=frame->a0;
#endif
        }
        if (!s->syscalls) return;
        sig=(s->options&1)?133:5;
    }
    if (op==1) {
#if defined(BOAROS_ARCH_LOONGARCH)
        original_nr=frame->regs[11];
        frame->era+=4;
#else
        original_nr=frame->a7;
        frame->sepc+=4;
#endif
    }
    unsigned signal=stop(t,STOP_STATUS(sig,0),op,sig==133?op:0,0);
    if (signal) (void)kernel_signal_send_ptrace_syscall(t,signal);
    if (op==1) {
#if defined(BOAROS_ARCH_LOONGARCH)
        kernel_signal_retire_replaced_restart(t,original_nr,frame->regs[11]);
        frame->era-=4;
        frame->regs[4]=t->ptrace->original_arg;
#else
        kernel_signal_retire_replaced_restart(t,original_nr,frame->a7);
        frame->sepc-=4;
        frame->a0=t->ptrace->original_arg;
#endif
    }
}
int kernel_ptrace_syscall_enter(struct kernel_task *t,struct arch_trap_frame *f)
{
    /* An attach can race the last user-return check, including return from
     * a group stop. Honor its interrupt before deciding this call's mode. */
    kernel_ptrace_user_return(t,f);
    syscall_stop(t,1,f);
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    return !t->terminate_requested;
}
void kernel_ptrace_syscall_exit(struct kernel_task *t,struct arch_trap_frame *f,uint64_t nr)
{ (void)nr; syscall_stop(t,2,f); }

void kernel_ptrace_user_return(struct kernel_task *t,struct arch_trap_frame *f)
{
    (void)f;
    if (!state(t)) return;
    for (;;) {
        unsigned status,group;
        {
            KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
            status=t->ptrace->pending; t->ptrace->pending=0;
            group=t->ptrace->group_pending;
        }
        if (status) stop(t,status,0,0,0);
        else if (group) kernel_signal_ptrace_group_stop(t,group);
        else break;
    }
}
uint32_t kernel_ptrace_signal(struct kernel_task *t,struct kernel_signal_wait_info *info)
{
    if (info->signal==9 || !state(t)) return info->signal;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        if (!t->ptrace->tracer) return info->signal;
        t->ptrace->delivery_active=1; t->ptrace->delivery_continued=0;
    }
    unsigned result=stop(t,STOP_STATUS(info->signal,0),0,0,info);
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        t->ptrace->delivery_active=result>=19 && result<=22;
        if (!t->ptrace->delivery_active) t->ptrace->delivery_continued=0;
    }
    /* SIGCONT can report a seized job-control change while the old delivery
     * stop is held. Publish it before selecting the pending SIGCONT. */
    kernel_ptrace_user_return(t,0);
    return result;
}
int kernel_ptrace_group_pending_locked(const struct kernel_task *t,unsigned sig)
{ return state(t) && t->ptrace->group_pending==sig; }

int kernel_ptrace_group_stopped_locked(const struct kernel_task *t)
{ return state(t) && t->ptrace->tracer && t->ptrace->group_signal; }

int kernel_ptrace_group_prepare_locked(struct kernel_task *t,unsigned sig)
{
    if (!state(t)) return 0;
    if (!t->ptrace->tracer) {
        if (t==kernel_task_current()) t->ptrace->group_pending=0;
        return 0;
    }
    if (t==kernel_task_current()) {
        int cancelled=!t->ptrace->group_signal && !t->ptrace->group_pending &&
            (!t->ptrace->delivery_active || t->ptrace->delivery_continued);
        t->ptrace->delivery_active=t->ptrace->delivery_continued=0;
        if (cancelled) return 2;
        t->ptrace->group_signal=sig; t->ptrace->group_pending=0;
    } else if (!t->ptrace->group_signal && !t->ptrace->group_pending) {
        t->ptrace->group_pending=sig;
        scheduler_signal_request_locked(t);
        kernel_wait_interrupt_locked(t);
    }
    return 1;
}
int kernel_ptrace_group_stop(struct kernel_task *t,unsigned sig)
{
    if (!state(t)) return 0;
    unsigned event;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        if (!t->ptrace->tracer) return 0;
        if (!t->ptrace->group_signal) return 1; /* CONT beat publication. */
        event=t->ptrace->seized?TRACE_STOP:0;
    }
    stop(t,STOP_STATUS(sig,event),0,0,0);
    return 1;
}
void kernel_ptrace_group_continue_locked(struct kernel_task *t)
{
    if (!state(t)) return;
    t->ptrace->group_signal=t->ptrace->group_pending=0;
    if (!t->ptrace->tracer) return;
    if (t->ptrace->delivery_active) t->ptrace->delivery_continued=1;
    if (t->ptrace->seized) request_stop_locked(t,STOP_STATUS(5,TRACE_STOP));
}
void ptrace_event(struct kernel_task *t,unsigned event,uint64_t message)
{
    unsigned options;
    if (tracing(t,&options) && (options&(1U<<event)))
        stop(t,STOP_STATUS(5,event),message,0,0);
}
void kernel_ptrace_exec(struct kernel_task *t,uint32_t old_tid)
{
    unsigned options;
    if (!tracing(t,&options)) return;
    unsigned seized;
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain); seized=t->ptrace->seized; }
    if (options&(1U<<TRACE_EXEC)) ptrace_event(t,TRACE_EXEC,old_tid);
    else if (!seized) (void)kernel_signal_send_task(t,5,0);
}
int ptrace_clone_prepare(struct kernel_task *child)
{ return ensure_state(child); }
int ptrace_clone_locked(struct kernel_task *p,struct kernel_task *c,uint64_t flags)
{
    struct kernel_ptrace_state *s=state(p);
    if (!s || !s->tracer || (flags&0x800000)) return 0;
    unsigned event=(flags&0x4000)?TRACE_VFORK:(flags&255)!=17?TRACE_CLONE:TRACE_FORK;
    unsigned enabled=s->options&(1U<<event);
    if (enabled || (flags&0x2000)) {
        if (!c->ptrace) return -KERNEL_ENOMEM;
        link_locked(s->tracer,c,s->credentials,s->seized,s->options);
        c->ptrace->pending=s->seized?STOP_STATUS(5,TRACE_STOP):STOP_STATUS(19,0);
    }
    return enabled?event:0;
}

static int exec_busy(struct kernel_task *t)
{
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    return !t->proc_exiting && !t->retirement && kernel_task_ptrace_exec_busy_locked(t);
}

static int attach(struct kernel_task *caller,struct kernel_task *target,int32_t requested_tid,uint64_t request,uint64_t addr,uint64_t data)
{
    int self=request==TRACEME;
    SCHEDULER_TASK_BORROW(tracer,0);
    struct kernel_task *t=self?caller:target;
    if (self) {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        struct kernel_pid *creator=t->group_leader->child_creator;
        tracer=creator?scheduler_task_lookup_locked(creator->number,KERNEL_PID_TID,creator->generation):0;
        if (!tracer && t->group_leader->parent) {
            tracer=t->group_leader->parent; scheduler_task_get_locked(tracer);
        }
        if (!tracer) return -KERNEL_EPERM;
    } else {
        tracer=caller;
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        scheduler_task_get_locked(tracer);
    }
    if (request==SEIZE) {
        if (addr || (data&~UINT64_C(0x3000ff))) return -KERNEL_EIO;
        if (data&~(uint64_t)VALID_OPTIONS) return -KERNEL_EINVAL;
    }
    if (ensure_state(t) || ensure_state(tracer)) return -KERNEL_ENOMEM;
    for (;;) {
        {
            KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
            const struct kernel_credentials *cred=kernel_credentials_task_locked(tracer);
            if ((!self && t->tid!=requested_tid) || t->retirement) return -KERNEL_ESRCH;
            if (t->ptrace->tracer || t->group_leader==tracer->group_leader ||
                !t->arch.user_mode || t->proc_exiting || !t->group_leader ||
                t->group_leader->core_session) return -KERNEL_EPERM;
            if (tracer->proc_exiting) return self?0:-KERNEL_EPERM;
            if (self || !kernel_task_ptrace_exec_busy_locked(t)) {
                if (self) {
                    if ((kernel_credentials_task_locked(t)->permitted&~cred->permitted) &&
                        !kernel_credentials_capable(cred,KERNEL_CAP_SYS_PTRACE)) return -KERNEL_EPERM;
                } else if (!kernel_credentials_may_access_locked(tracer,t,0)) return -KERNEL_EPERM;
                /* ptrace_link snapshots current_cred, including for TRACEME. */
                link_locked(tracer,t,kernel_credentials_task_locked(caller),request==SEIZE,request==SEIZE?(unsigned)data:0);
                if (request==ATTACH) request_stop_locked(t,STOP_STATUS(19,0));
                else if (request==SEIZE && t->state==KERNEL_THREAD_STATE_STOPPED)
                    request_stop_locked(t,STOP_STATUS(t->wait_status>>8,TRACE_STOP));
                return 0;
            }
        }
        enum kernel_wait_wake_reason reason;
        if (KERNEL_WAIT_RECHECK(&t->child_exit_queue,0,1,&reason,exec_busy(t))) __builtin_trap();
        if (reason==KERNEL_WAIT_SIGNALLED) {
            kernel_signal_note_syscall_restart(caller);
            caller->syscall_restart.kind=KERNEL_SYSCALL_RESTART_NOINTR;
            return -KERNEL_ERESTARTSYS;
        }
    }
}

static int copy_word_range(struct kernel_task *caller,void *buffer,uint64_t addr,size_t length,int in)
{
    if (!length) return 0;
    size_t copied=0;
    enum kernel_uaccess_status status=in?
        kernel_copy_from_user(&caller->mm,buffer,addr,length,&copied):
        kernel_copy_to_user(&caller->mm,addr,buffer,length,&copied);
    return status || copied!=length ? -KERNEL_EFAULT : 0;
}

static int regset(struct kernel_task *caller,struct kernel_task *t,unsigned note,uint64_t address,int writing)
{
    struct {uint64_t base,length;} iov;
    int error=copy_word_range(caller,&iov,address,sizeof(iov),1);
    if (error) return error;
    size_t len=arch_ptrace_regset_size(note);
    if (!len || (iov.length&7)) return -KERNEL_EINVAL;
    if (iov.length>len) iov.length=len;
    struct kernel_task_io_buffer scratch={0};
    if (kernel_task_io_buffer_acquire(&scratch,caller->mm.allocator)) return -KERNEL_ENOMEM;
    uint64_t *regs=scratch.data;
    arch_ptrace_regset(t,note,regs,&t->ptrace->original_arg,0);
    error=copy_word_range(caller,regs,iov.base,iov.length,writing);
    if (!error && writing && iov.length) arch_ptrace_regset(t,note,regs,&t->ptrace->original_arg,1);
    kernel_task_io_buffer_release(&scratch);
    return error?error:copy_word_range(caller,&iov.length,address+8,8,0);
}

static int64_t syscall_info(struct kernel_task *caller,struct kernel_task *t,uint64_t size,uint64_t data)
{
    struct {
        uint8_t op,pad[3]; uint32_t arch; uint64_t pc,sp,nr,args[6];
    } info={.op=t->ptrace->operation};
    struct arch_trap_frame *f=(void *)(t->stack_high-sizeof(*f));
    info.pc=arch_frame_pc(f);
    arch_ptrace_syscall(f,&info.arch,&info.sp,&info.nr,info.args);
    size_t actual=24;
    if (info.op==1) {
#if defined(BOAROS_ARCH_LOONGARCH)
        info.args[0]=t->ptrace->original_arg;
#endif
        actual=80;
    }
    else if (info.op==2) {
        info.nr=info.args[0];
        info.args[0]=info.nr>=(uint64_t)-4095;
        actual=33;
    }
    int error=copy_word_range(caller,&info,data,size<actual?size:actual,0);
    return error?error:(int64_t)actual;
}

static int64_t stopped_request(struct kernel_task *caller,struct kernel_task *t,
    uint64_t request,uint64_t addr,uint64_t data)
{
    struct kernel_ptrace_state *s=t->ptrace;
    uint64_t word=data;
    switch (request) {
    case GETREGSET: case SETREGSET:
        return regset(caller,t,(unsigned)addr,data,request==SETREGSET);
    case GETEVENTMSG: return copy_word_range(caller,&s->message,data,8,0);
    case GETSIGINFO: case SETSIGINFO: {
        if (!s->info.signal || (s->group_signal && !s->seized)) return -KERNEL_EINVAL;
        if (request==GETSIGINFO) return copy_word_range(caller,&s->info,data,sizeof(s->info),0);
        struct kernel_signal_wait_info update;
        int err=copy_word_range(caller,&update,data,sizeof(update),1);
        if (!err) s->info=update;
        return err;
    }
    case GETSIGMASK: case SETSIGMASK:
        if (addr!=8) return -KERNEL_EINVAL;
        if (request==SETSIGMASK) {
            int err=copy_word_range(caller,&word,data,8,1);
            if (err) return err;
            KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
            t->signal_blocked=word&~KERNEL_SIGNAL_UNBLOCKABLE_MASK; t->signal_restore_mask=0;
            return 0;
        }
        {
            KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
            word=t->signal_restore_mask?t->signal_saved_mask:t->signal_blocked;
        }
        return copy_word_range(caller,&word,data,8,0);
    case GET_SYSCALL_INFO: return syscall_info(caller,t,addr,data);
    case PEEKUSER: case POKEUSER: {
        int result=arch_ptrace_user(t,addr,&word,&s->original_arg,request==POKEUSER);
        return result || request==POKEUSER ? result : copy_word_range(caller,&word,data,8,0);
    }
    case PEEKTEXT: case PEEKDATA: case POKETEXT: case POKEDATA: {
        int writing=request==POKETEXT || request==POKEDATA;
        size_t copied=0;
        /* The attach credentials must still authorize access after exec.
         * A stopped-access owner excludes resumption and MM replacement. */
        {
            KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
            if (kernel_exec_security_dumpable(t->exec_security)!=1 &&
                !kernel_credentials_capable(s->credentials,KERNEL_CAP_SYS_PTRACE)) return -KERNEL_EIO;
        }
        int err=kernel_mm_remote_transfer(&t->mm,addr,&word,8,writing,1,&copied);
        if (err || copied!=8) return -KERNEL_EIO;
        return writing?0:copy_word_range(caller,&word,data,8,0);
    }
    default:return -KERNEL_EIO;
    }
}

int64_t kernel_ptrace_request(struct kernel_task *caller,uint64_t request,int32_t tid,uint64_t addr,uint64_t data)
{
    if (request==TRACEME) return attach(caller,0,tid,request,addr,data);
    SCHEDULER_TASK_BORROW(t,scheduler_task_lookup(tid,KERNEL_PID_TID,0));
    if (!t) return -KERNEL_ESRCH;
    if (request==ATTACH || request==SEIZE) return attach(caller,t,tid,request,addr,data);
    struct kernel_ptrace_state *s=state(t);
    const struct kernel_credentials *detached=0;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        if (!s || s->tracer!=caller || t->proc_exiting) return -KERNEL_ESRCH;
        if (request==INTERRUPT) {
            if (!s->seized) return -KERNEL_EIO;
            request_stop_locked(t,STOP_STATUS(5,TRACE_STOP)); return 0;
        }
        if (request!=KILL && (s->phase!=TRACE_STOPPED || t->wait.on_cpu || s->access))
            return -KERNEL_ESRCH;
        switch (request) {
        case SETOPTIONS:
            if (data&~(uint64_t)VALID_OPTIONS) return -KERNEL_EINVAL;
            s->options=(uint32_t)data; return 0;
        case LISTEN:
            if (!s->seized || s->status>>16!=TRACE_STOP) return -KERNEL_EIO;
            s->phase=TRACE_LISTENING;
            s->unread=0;
            /* A notification can arrive after this stop was reported and
             * before LISTEN. It already owns the next stop; wake it now. */
            if (s->pending) resume_locked(s);
            return 0;
        case CONT: case SYSCALL: case DETACH:
            if (data>64) return -KERNEL_EIO;
            s->resume_signal=(unsigned)data; s->syscalls=request==SYSCALL;
            s->resume_sender=process_identity_number(caller,KERNEL_PID_TGID);
            s->resume_uid=kernel_credentials_task_locked(caller)->uid;
            if (request==DETACH) detached=unlink_locked(t);
            resume_locked(s); break;
        case KILL:break;
        default:
            if (caller->ptrace->access_target) __builtin_trap();
            caller->ptrace->access_target=t; s->access=1; break;
        }
    }
    if (request==KILL) {
        (void)kernel_signal_send_task(t,9,0);
        return 0;
    }
    if (request==DETACH || request==CONT || request==SYSCALL) {
        kernel_credentials_put(detached);
        return 0;
    }
    int64_t result=stopped_request(caller,t,request,addr,data);
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        caller->ptrace->access_target=0; s->access=0;
        if (s->release_deferred) { s->release_deferred=0; resume_locked(s); }
    }
    return result;
}

static int select_tracee(struct kernel_task *tracer,int which,const struct kernel_pid *target,
    unsigned options,struct kernel_task **child,uint32_t *event,int *matching)
{
    SCHEDULER_TASK_BORROW(cursor,0);
    uint64_t epoch=0;
    int saw=0;
    for (;;) {
        struct kernel_task *next;
        {
            KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
            struct kernel_ptrace_state *list=state(tracer);
            if (!list) return 0;
            if (!cursor || epoch!=list->epoch) { next=list->first; saw=0; }
            else next=cursor;
            epoch=list->epoch;
            for (unsigned n=0;next && n<16;n++) {
                struct kernel_task *t=next;
                struct kernel_ptrace_state *s=t->ptrace;
                next=s->next;
                if (which && (!target || process_identity(t,which==2?KERNEL_PID_PGID:KERNEL_PID_TID)!=target))
                    continue;
                saw=1;
                if (!s->unread || t->reaping) continue;
                int final=s->phase==TRACE_FINAL;
                if (final ? !(options&4) : (!(options&(2U|0x10000000U)) || s->phase!=TRACE_STOPPED)) continue;
                t->reaping=1; scheduler_task_get_locked(t);
                *child=t; *event=final?TRACE_WAIT_FINAL:TRACE_WAIT_STOP;
                *matching=1; return 1;
            }
            if (!next) { *matching|=saw; return 0; }
            scheduler_task_get_locked(next);
        }
        if (cursor) scheduler_task_put(cursor);
        cursor=next;
    }
}

int ptrace_select(struct kernel_task *parent,int which,const struct kernel_pid *target,unsigned options,
    struct kernel_task **child,uint32_t *event,int *matching)
{
    int sole;
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain); sole=parent->group_leader->group_members==1; }
    if ((options&0x20000000U) || sole)
        return state(parent) && select_tracee(parent,which,target,options,child,event,matching);
    struct scheduler_group_cursor group;
    scheduler_group_cursor_begin(parent,&group);
    int found=0;
    while (group.task) {
        if (state(group.task) && select_tracee(group.task,which,target,options,child,event,matching)) {
            found=1; break;
        }
        scheduler_group_cursor_next(&group);
    }
    scheduler_group_cursor_end(&group);
    return found;
}

uint32_t ptrace_wait_status_locked(struct kernel_task *t,int consume)
{
    if (consume) t->ptrace->unread=0;
    return t->ptrace->status;
}

int ptrace_publish_final_locked(struct kernel_task *t,struct kernel_ptrace_notice *notice)
{
    if (!state(t) || !t->ptrace->tracer) return 0;
    t->ptrace->phase=TRACE_FINAL; t->ptrace->status=t->wait_status; t->ptrace->unread=1;
    t->ptrace->why=(t->wait_status&127) ? (t->wait_status&128) ? 3 : 2 : 1;
    prepare_notice_locked(t,notice);
    return 1;
}

/* Called by the sole wait claimant, or by an exiting tracer releasing its list. */
void ptrace_consume_final(struct kernel_task *t)
{
    const struct kernel_credentials *cred;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        if (!state(t) || !t->ptrace->tracer) return;
        cred=unlink_locked(t); t->ptrace->unread=0; t->ptrace->phase=TRACE_RUNNING;
    }
    kernel_credentials_put(cred);
}

void ptrace_begin_exit(struct kernel_task *t,uint32_t status)
{
    if (!state(t)) return;
    /* An exceptional exit can abandon a sleeping user copy. Transfer the
     * lookup borrow from that call before detaching any of this tracer's TIDs. */
    SCHEDULER_TASK_BORROW(access,0);
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        access=t->ptrace->access_target;
        if (access) {
            t->ptrace->access_target=0; access->ptrace->access=0;
            if (access->ptrace->release_deferred) {
                access->ptrace->release_deferred=0; resume_locked(access->ptrace);
            }
        }
    }
    ptrace_event(t,TRACE_EXIT,status);
    for (;;) {
        SCHEDULER_TASK_BORROW(child,0);
        const struct kernel_credentials *cred=0;
        unsigned kill,final;
        {
            KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
            child=t->ptrace->first;
            if (!child) break;
            scheduler_task_get_locked(child);
            kill=(child->ptrace->options&EXITKILL) && !child->proc_exiting &&
                child->ptrace->phase!=TRACE_FINAL;
        }
        /* Keep the existing stop while publishing SIGKILL. The group exit
         * path sets terminate_requested before it notifies the wait token. */
        if (kill) (void)kernel_signal_send_task(child,9,0);
        {
            KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
            struct kernel_ptrace_state *s=child->ptrace;
            if (s->tracer!=t) continue;
            final=s->phase==TRACE_FINAL;
            cred=unlink_locked(child);
            s->phase=TRACE_RUNNING; resume_locked(s);
        }
        kernel_credentials_put(cred);
        if (final) process_release_traced_zombie(child);
    }
}

void ptrace_wait_claim_done(struct kernel_task *child)
{
    if (!state(child)) return;
    SCHEDULER_TASK_BORROW(group,0);
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        struct kernel_ptrace_state *s=child->ptrace;
        if (s->release_deferred && !s->access && !child->reaping) {
            s->release_deferred=0; resume_locked(s);
        }
        if (s->unread && s->tracer) {
            group=s->tracer->group_leader; scheduler_task_get_locked(group);
        }
    }
    if (group) (void)kernel_wait_queue_wake_all(&group->child_exit_queue);
}
