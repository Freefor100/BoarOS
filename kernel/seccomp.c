#include <kernel/seccomp.h>
#include <kernel/ptrace.h>
#include <kernel/credentials.h>
#include <kernel/signal.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/syscall.h>
#include <kernel/task_state.h>
#include <kernel/uaccess.h>

#define RET_KILL_PROCESS UINT32_C(0x80000000)
#define RET_KILL_THREAD UINT32_C(0)
#define RET_TRAP UINT32_C(0x00030000)
#define RET_ERRNO UINT32_C(0x00050000)
#define RET_TRACE UINT32_C(0x7ff00000)
#define RET_ALLOW UINT32_C(0x7fff0000)
#define ACTION UINT32_C(0xffff0000)
#define PATH_MAX 32768U

struct kernel_seccomp_filter {
    struct kernel_seccomp_filter *previous;
    struct kernel_heap *heap;
    uint32_t references, count, path_count;
    struct kernel_bpf_classic_insn instructions[];
};

static int action_supported(uint32_t action)
{
    return action==RET_KILL_PROCESS || action==RET_KILL_THREAD ||
        action==RET_TRAP || action==RET_ERRNO || action==RET_TRACE || action==RET_ALLOW;
}

int kernel_seccomp_validate(const struct kernel_bpf_classic_insn *insns,
    uint32_t count, const struct kernel_bpf_vm_env *env)
{
    if (!insns || !count || count>KERNEL_BPF_CLASSIC_MAX_INSNS) return -KERNEL_EINVAL;
    for (uint32_t pc=0;pc<count;pc++) {
        const struct kernel_bpf_classic_insn *i=&insns[pc];
        switch (i->code) {
        case 0x20: /* seccomp_data uses native-endian aligned words, not packets. */
            if ((i->k&3) || i->k>sizeof(struct kernel_seccomp_data)-4) return -KERNEL_EINVAL;
            break;
        case 0x28:case 0x30:case 0x40:case 0x48:case 0x50:case 0xb1:
            return -KERNEL_EINVAL;
        case 0x16:
            /* Computed actions could request an unimplemented notification.
             * Reject the program rather than silently downgrade its policy. */
            return -KERNEL_EOPNOTSUPP;
        case 0x06:
            switch (i->k&ACTION) {
            case 0x7fc00000:case 0x7ffc0000:
                return -KERNEL_EOPNOTSUPP; /* USER_NOTIF, LOG */
            default: break; /* Unknown actions deliberately fail closed at run time. */
            }
            break;
        default: break;
        }
    }
    /* Reuse the cBPF opcode, forward-DAG, divisor, shift and definite scratch
     * initialization checks. No socket translation or packet VM is executed. */
    uint32_t translated;
    return kernel_bpf_classic_convert(insns,count,0,0,&translated,env);
}

uint32_t kernel_seccomp_run(const struct kernel_bpf_classic_insn *insns,
    uint32_t count, const struct kernel_seccomp_data *data)
{
    uint32_t a=0,x=0,m[16];
    for (uint32_t pc=0;pc<count;pc++) {
        const struct kernel_bpf_classic_insn *i=&insns[pc];
        uint32_t v=(i->code&8)?x:i->k;
        if ((i->code&7)==4) {
            switch (i->code&0xf0) {
            case 0x00:a+=v;break;
            case 0x10:a-=v;break;
            case 0x20:a*=v;break;
            case 0x30:if(!v)return RET_KILL_THREAD;a/=v;break;
            case 0x40:a|=v;break;
            case 0x50:a&=v;break;
            case 0x60:a<<=v&31;break;
            case 0x70:a>>=v&31;break;
            case 0x80:a=0-a;break;
            case 0x90:if(!v)return RET_KILL_THREAD;a%=v;break;
            case 0xa0:a^=v;break;
            default:return RET_KILL_PROCESS;
            }
        } else switch (i->code) {
        case 0x00:a=i->k;break;
        case 0x01:x=i->k;break;
        case 0x20:__builtin_memcpy(&a,(const char *)data+i->k,4);break;
        case 0x60:a=m[i->k];break;
        case 0x61:x=m[i->k];break;
        case 0x02:m[i->k]=a;break;
        case 0x03:m[i->k]=x;break;
        case 0x80:a=sizeof(*data);break;
        case 0x81:x=sizeof(*data);break;
        case 0x07:x=a;break;
        case 0x87:a=x;break;
        case 0x06:return i->k;
        case 0x05:pc+=i->k;break;
        case 0x15:case 0x1d:pc+=a==v?i->jt:i->jf;break;
        case 0x25:case 0x2d:pc+=a>v?i->jt:i->jf;break;
        case 0x35:case 0x3d:pc+=a>=v?i->jt:i->jf;break;
        case 0x45:case 0x4d:pc+=(a&v)?i->jt:i->jf;break;
        default:return RET_KILL_PROCESS;
        }
    }
    return RET_KILL_PROCESS;
}

static void *allocate(void *heap,size_t size)
{
    void *p=0;
    if (kernel_heap_allocate(heap,size,&p)) return 0;
    return p;
}
static void release(void *heap,void *p) { kernel_heap_release(heap,p); }
static int copy_in(struct kernel_task *task,void *to,uint64_t from,size_t size)
{
    size_t copied=0;
    return kernel_copy_from_user(&task->mm,to,from,size,&copied) || copied!=size ? -KERNEL_EFAULT:0;
}

int64_t kernel_seccomp_set(struct kernel_task *task,uint32_t mode,uint32_t flags,uint64_t address)
{
    if (mode!=1 && mode!=2) return -KERNEL_EINVAL;
    if (flags & ~1U || (mode==1 && flags)) return -KERNEL_EINVAL;
    if (mode==1) {
        if (address) return -KERNEL_EINVAL;
        return kernel_seccomp_install(task,0,0);
    }
    if (!__atomic_load_n(&task->no_new_privs,__ATOMIC_RELAXED) &&
        !kernel_credentials_capable(task->credentials,KERNEL_CAP_SYS_ADMIN)) return -KERNEL_EACCES;
    struct { uint16_t count; uint16_t pad[3]; uint64_t instructions; } program;
    int error=copy_in(task,&program,address,sizeof(program));
    if (error) return error;
    if (!program.count || program.count>KERNEL_BPF_CLASSIC_MAX_INSNS) return -KERNEL_EINVAL;
    struct kernel_heap *heap=task->files.heap;
    struct kernel_seccomp_filter *filter=allocate(heap,sizeof(*filter)+program.count*sizeof(filter->instructions[0]));
    if (!filter) return -KERNEL_ENOMEM;
    error=copy_in(task,filter->instructions,program.instructions,program.count*sizeof(filter->instructions[0]));
    if (!error) {
        const struct kernel_bpf_vm_env env={.opaque=heap,.alloc=allocate,.free=release};
        error=kernel_seccomp_validate(filter->instructions,program.count,&env);
    }
    if (error) {release(heap,filter);return error;}
    filter->heap=heap;filter->references=1;filter->count=program.count;
    int64_t result=kernel_seccomp_install(task,filter,flags);
    if(result) release(heap,filter);
    return result;
}

static int ancestor(struct kernel_seccomp_filter *parent,struct kernel_seccomp_filter *child)
{
    if(!parent)return 1;
    for(;child;child=child->previous)if(child==parent)return 1;
    return 0;
}
static int sync_member(const struct kernel_task *task,const struct kernel_task *caller)
{ return task!=caller && !task->proc_exiting; }

/* The scheduler domain protects all leaf writers and group/exec publication.
 * Every replacement is a descendant of the old leaf. Thus a running task's
 * old snapshot stays reachable without a read-side lock or reference update. */
int64_t kernel_seccomp_attach_locked(struct kernel_task *task,
    struct kernel_seccomp_filter *filter,uint32_t flags)
{
    uint32_t mode=filter?2:1;
    if(task->seccomp_mode && task->seccomp_mode!=mode)return -KERNEL_EINVAL;
    if(!filter) {
        __atomic_store_n(&task->seccomp_mode,1,__ATOMIC_RELEASE);
        return 0;
    }
    uint32_t path=filter->count;
    if(task->seccomp_filter)path+=task->seccomp_filter->path_count+4;
    if(path>PATH_MAX)return -KERNEL_ENOMEM;
    if(flags&1) {
        struct kernel_task *peer=task->group_next;
        for(;peer!=task;peer=peer->group_next) {
            if(!sync_member(peer,task))continue;
            if(peer->seccomp_mode && (peer->seccomp_mode!=2 ||
                !ancestor(peer->seccomp_filter,task->seccomp_filter)))
                return peer->tid?peer->tid:-KERNEL_ESRCH;
        }
    }
    filter->path_count=path;filter->previous=task->seccomp_filter;
    __atomic_store_n(&task->seccomp_filter,filter,__ATOMIC_RELEASE);
    __atomic_store_n(&task->seccomp_mode,2,__ATOMIC_RELEASE);
    if(flags&1) {
        for(struct kernel_task *peer=task->group_next;peer!=task;peer=peer->group_next) {
            if(!sync_member(peer,task))continue;
            struct kernel_seccomp_filter *old=peer->seccomp_filter;
            if(__atomic_fetch_add(&filter->references,1,__ATOMIC_RELAXED)==UINT32_MAX)__builtin_trap();
            if(task->no_new_privs)__atomic_store_n(&peer->no_new_privs,1,__ATOMIC_RELEASE);
            __atomic_store_n(&peer->seccomp_filter,filter,__ATOMIC_RELEASE);
            __atomic_store_n(&peer->seccomp_mode,2,__ATOMIC_RELEASE);
            /* Compatibility proves the new path retains this ancestor. Never
             * allocate/free or run fallible operations after first publication. */
            if(old && __atomic_fetch_sub(&old->references,1,__ATOMIC_ACQ_REL)<=1)__builtin_trap();
        }
    }
    return 0;
}

int64_t kernel_seccomp_syscall(struct kernel_task *task,uint32_t operation,uint32_t flags,uint64_t address)
{
    if (operation<2) return kernel_seccomp_set(task,operation+1,flags,address);
    if (operation!=2 || flags) return -KERNEL_EINVAL;
    uint32_t action;
    int error=copy_in(task,&action,address,sizeof(action));
    if (error) return error;
    return action_supported(action)?0:-KERNEL_EOPNOTSUPP;
}

void kernel_seccomp_clone(struct kernel_task *child,const struct kernel_task *parent)
{
    child->seccomp_mode=parent->seccomp_mode;
    child->seccomp_filter=parent->seccomp_filter;
    if (child->seccomp_filter && __atomic_fetch_add(&child->seccomp_filter->references,1,__ATOMIC_RELAXED)==UINT32_MAX)
        __builtin_trap();
}
void kernel_seccomp_release(struct kernel_task *task)
{
    struct kernel_seccomp_filter *filter=task->seccomp_filter;
    task->seccomp_filter=0;
    while (filter && __atomic_fetch_sub(&filter->references,1,__ATOMIC_ACQ_REL)==1) {
        struct kernel_seccomp_filter *previous=filter->previous;
        release(filter->heap,filter);filter=previous;
    }
}

static int check(struct kernel_task *task,struct kernel_syscall_request *request,
    struct kernel_syscall_result *result,int retrace)
{
    if (__atomic_load_n(&task->seccomp_mode,__ATOMIC_ACQUIRE)==1) {
        if (request->number==63 || request->number==64 || request->number==93 || request->number==139) return 0;
        kernel_seccomp_terminate(9,0);
    }
    uint64_t original_nr=request->number;
    struct kernel_seccomp_data data={.nr=(int32_t)original_nr,
#if defined(__loongarch__)
        .arch=UINT32_C(0xc0000102),
#else
        .arch=UINT32_C(0xc00000f3),
#endif
        .instruction_pointer=request->instruction_pointer};
    for (unsigned i=0;i<6;i++) data.args[i]=request->arguments[i];
    uint32_t verdict=RET_ALLOW;
    for (struct kernel_seccomp_filter *f=__atomic_load_n(&task->seccomp_filter,__ATOMIC_ACQUIRE);f;f=f->previous) {
        uint32_t value=kernel_seccomp_run(f->instructions,f->count,&data);
        /* Signed action ordering gives KILL_PROCESS highest precedence. Equal
         * priority preserves data from the most recently installed filter. */
        if ((int32_t)(value&ACTION)<(int32_t)(verdict&ACTION)) verdict=value;
    }
    switch (verdict&ACTION) {
    case RET_ALLOW:return 0;
    case RET_TRACE:
        if (retrace) return 0;
        if (!kernel_ptrace_seccomp(task,request,result,verdict&0xffff)) {
            kernel_signal_clear_syscall_restart(task);
            result->action=KERNEL_SYSCALL_ACTION_RETURN_FINAL;
            result->value=-KERNEL_ENOSYS;
            return 1;
        }
        if ((int32_t)request->number<0) {
            kernel_signal_clear_syscall_restart(task);
            result->action=KERNEL_SYSCALL_ACTION_RETURN_FINAL;
            return 1;
        }
        /* The saved timeout belongs to restart_syscall, not to a replacement
         * operation. Keeping it would let a later signal rewrite that completed
         * operation's return value/PC. An unchanged restart still needs it. */
        if (original_nr==128 && request->number!=128)
            kernel_signal_clear_syscall_restart(task);
        /* The tracer can change the number/arguments and TSYNC can publish a
         * stronger chain while stopped. Re-read both; a second TRACE is allow,
         * not a recursive event, but every stronger action still takes effect. */
        return check(task,request,result,1);
    case RET_TRAP:
        kernel_signal_clear_syscall_restart(task);
        kernel_signal_force_seccomp(task,data.instruction_pointer,data.nr,data.arch,verdict&0xffff);
        result->action=KERNEL_SYSCALL_ACTION_RETURN_FINAL;
        /* Linux RV64/LA64 syscall_rollback restores orig_a0; no syscall ran. */
        result->value=(int64_t)request->arguments[0];
        return 1;
    case RET_ERRNO:
        kernel_signal_clear_syscall_restart(task);
        result->action=KERNEL_SYSCALL_ACTION_RETURN_FINAL;
        result->value=-(int64_t)((verdict&0xffff)>4095?4095:verdict&0xffff);
        return 1;
    case RET_KILL_THREAD:kernel_seccomp_terminate(31,0);
    default:kernel_seccomp_terminate(31,1);
    }
}

int kernel_seccomp_check(struct kernel_task *task,struct kernel_syscall_request *request,
    struct kernel_syscall_result *result)
{ return check(task,request,result,0); }
