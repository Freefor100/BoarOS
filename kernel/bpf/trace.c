#include "private.h"
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/files.h>
#include <kernel/task.h>
#include <kernel/trace_events.h>
#include <string.h>

/* The list borrows events. A final put unlinks before freeing, while readers
 * acquire only nonzero references under the same lock. Programs and events
 * stay owned across a run; no raw lock spans verification, the VM or uaccess. */
static struct kernel_raw_lock trace_lock={
    .rank=KERNEL_RAW_RANK_OBJECT,.initialized=KERNEL_RAW_INITIALIZED
};
static struct bpf_perf *first_event,*last_event;
struct perf_attribute {
    uint32_t type,size;
    uint64_t config,period,sample_type,read_format,flags;
    uint32_t wakeup,bp_type;
    uint64_t config1,config2,branch_type,regs_user;
    uint32_t stack_user,clock;
    uint64_t regs_intr;
    uint32_t aux_watermark;
    uint16_t max_stack,reserved;
    uint32_t aux_size,aux_action;
    uint64_t signal_data,config3,config4;
};
_Static_assert(sizeof(struct perf_attribute)==144,"perf_event_attr ABI");
static int perf_attributes(struct kernel_mm *mm,uint64_t address,struct perf_attribute *a)
{
    uint32_t size;
    int error=bpf_user_copy(mm,address+4,&size,4,1);
    if(error)return error;
    if(!size)size=64;
    if(size<64 || size>BOAROS_PAGE_SIZE)goto wrong_size;
    error=bpf_user_copy(mm,address,a,size<sizeof(*a)?size:sizeof(*a),1);
    if(error)return error;
    for(uint32_t offset=sizeof(*a);offset<size;){
        unsigned char tail[64];size_t count=size-offset;
        if(count>sizeof(tail))count=sizeof(tail);
        error=bpf_user_copy(mm,address+offset,tail,count,1);
        if(error)return error;
        for(size_t i=0;i<count;i++)if(tail[i])goto wrong_size;
        offset+=(uint32_t)count;
    }
    a->size=size;
    if((a->flags>>40) || a->reserved || (a->aux_action&~7U) ||
        (a->sample_type>>25) || (a->read_format&~31U))return -KERNEL_EINVAL;
    return 0;
wrong_size:
    size=sizeof(*a);
    error=bpf_user_copy(mm,address+4,&size,4,0);
    return error?error:-KERNEL_E2BIG;
}
int64_t kernel_bpf_perf_open(struct kernel_task *task,uint64_t address,
    int32_t pid,int32_t cpu,int32_t group,uint64_t flags)
{
    if(flags&~15ULL)return -KERNEL_EINVAL;
    struct kernel_mm *mm;struct kernel_files *files;
    if(kernel_task_mm_borrow_mutable(task,&mm) || kernel_task_files_borrow(task,&files))
        __builtin_trap();
    struct perf_attribute a={0};int error=perf_attributes(mm,address,&a);
    if(error)return error;
    const struct kernel_credentials *credentials=kernel_credentials_current();
    if(!kernel_credentials_capable(credentials,KERNEL_CAP_PERFMON) &&
        !kernel_credentials_capable(credentials,KERNEL_CAP_SYS_ADMIN))return -KERNEL_EACCES;
    if(a.period>>63)return -KERNEL_EINVAL;
    if(flags&7)return -KERNEL_EOPNOTSUPP;
    if(group!=-1){
        struct bpf_object *object=bpf_object_fd(files,group,BPF_OBJECT_PERF,&error);
        if(!object)return error==-KERNEL_EINVAL?-KERNEL_EBADF:error;
        bpf_object_put(object);return -KERNEL_EOPNOTSUPP;
    }
    struct kernel_task *target=NULL;
    if(pid!=-1){
        kernel_pid_t tid;
        if(pid==0){if(kernel_task_tid(task,&tid))__builtin_trap();pid=(int32_t)tid;}
        target=kernel_task_find_by_tid(pid);
        if(!target)return -KERNEL_ESRCH;
        if(!kernel_task_has_user_context(target)){error=-KERNEL_ESRCH;goto target_error;}
    }
    if(cpu < -1 || (cpu==-1 && pid==-1)){error=-KERNEL_EINVAL;goto target_error;}
    if(cpu>=0 && (uint32_t)cpu>=kernel_cpu_get_boot_count()){
        error=-KERNEL_ENODEV;goto target_error;
    }
    if(a.type!=2){error=-KERNEL_ENOENT;goto target_error;}
    if(a.config!=KERNEL_TRACE_SYS_ENTER && a.config!=KERNEL_TRACE_SYS_EXIT){
        error=-KERNEL_ENOENT;goto target_error;
    }
    /* RAW/period=1/wakeup=1 is the standard libbpf tracepoint attach
     * configuration. BPF output goes to its map ring; no perf sample mmap is
     * advertised. Other PMU, inherited, sample and clock options are absent. */
    if((a.flags&~UINT64_C(1)) || a.period>1 ||
        (a.sample_type && a.sample_type!=(UINT64_C(1)<<10)) ||
        (a.read_format&~UINT64_C(4)) || a.wakeup>1 || a.bp_type ||
        a.config1 || a.config2 || a.branch_type || a.regs_user || a.stack_user ||
        a.clock || a.regs_intr || a.aux_watermark || a.max_stack || a.aux_size ||
        a.aux_action || a.signal_data || a.config3 || a.config4){
        error=-KERNEL_EOPNOTSUPP;goto target_error;
    }
    struct bpf_perf *event=NULL;
    if(kernel_heap_allocate_zeroed(files->heap,1,sizeof(*event),(void **)&event)){
        error=-KERNEL_ENOMEM;goto target_error;
    }
    bpf_object_initialize(&event->object,files->heap,BPF_OBJECT_PERF);
    event->pid=pid;event->cpu=cpu;event->event=(uint32_t)a.config;
    event->target_task=target;event->read_format=a.read_format;event->enabled=!(a.flags&1);
    error=bpf_object_publish(&event->object);
    if(!error){
        KERNEL_RAW_SCOPE(lock,&trace_lock);
        struct bpf_perf *before=last_event;
        while(before && before->object.id>event->object.id)before=before->previous;
        event->previous=before;event->next=before?before->next:first_event;
        if(event->next)event->next->previous=event;else last_event=event;
        if(before)before->next=event;else __atomic_store_n(&first_event,event,__ATOMIC_RELEASE);
        event->registered=1;
    }
    if(!error)error=bpf_object_install(files,&event->object,2,(flags&8)?1:0);
    bpf_object_put(&event->object);return error;
target_error:
    if(target)kernel_task_lookup_put(target);
    return error;
}
void bpf_perf_destroy(struct bpf_perf *event)
{
    struct kernel_bpf_program *program;
    {
        KERNEL_RAW_SCOPE(lock,&trace_lock);
        if(event->link)__builtin_trap();
        if(event->registered){
            if(event->previous)event->previous->next=event->next;else __atomic_store_n(&first_event,event->next,__ATOMIC_RELEASE);
            if(event->next)event->next->previous=event->previous;else last_event=event->previous;
            event->registered=0;
        }
        program=event->program;event->program=NULL;event->enabled=0;
    }
    kernel_bpf_program_put(program);
    if(event->target_task)kernel_task_lookup_put(event->target_task);
}
void bpf_perf_close_file(struct bpf_perf *event)
{
    struct kernel_bpf_program *program=NULL;
    {
        KERNEL_RAW_SCOPE(lock,&trace_lock);
        event->file_closed=1;
        if(!event->link){
            event->enabled=0;program=event->program;event->program=NULL;
        }
    }
    kernel_bpf_program_put(program);
}
static int attach(struct bpf_perf *event,struct kernel_bpf_program *program,struct bpf_link *link)
{
    int error=bpf_program_trace_context(program,event->event);
    if(error)return error;
    KERNEL_RAW_SCOPE(lock,&trace_lock);
    if(event->file_closed)return -KERNEL_ENODEV;
    if(event->program)return -KERNEL_EEXIST;
    unsigned count=0;
    for(struct bpf_perf *p=first_event;p;p=p->next)
        if(p->event==event->event && p->program)count++;
    if(count>=64)return -KERNEL_E2BIG; /* Linux BPF_TRACE_MAX_PROGS */
    kernel_bpf_program_get(program);event->program=program;event->link=link;
    event->cookie=link?link->cookie:0;
    if(link)link->detached=0;
    return 0;
}
int bpf_perf_ioctl(struct bpf_perf *event,struct kernel_task *task,struct kernel_files *files,
    uint64_t command,uint64_t argument)
{
    if(command==UINT64_C(0x40042408)){
        struct kernel_bpf_program *program;
        int error=kernel_bpf_program_fd(files,(int32_t)argument,BPF_TRACEPOINT,&program);
        if(error)return error;
        error=attach(event,program,NULL);kernel_bpf_program_put(program);return error;
    }
    if(command==UINT64_C(0x80082407)){
        struct kernel_mm *mm;
        if(kernel_task_mm_borrow_mutable(task,&mm))__builtin_trap();
        uint64_t id=event->object.id;
        return bpf_user_copy(mm,argument,&id,sizeof(id),0);
    }
    if(command==0x2400 || command==0x2401 || command==0x2403){
        if(argument&~UINT64_C(1))return -KERNEL_EINVAL;
        KERNEL_RAW_SCOPE(lock,&trace_lock);
        if(command==0x2403)event->count=0;else event->enabled=command==0x2400;
        return 0;
    }
    return -KERNEL_ENOTTY;
}
int bpf_perf_read(struct bpf_perf *event,void *buffer,size_t capacity,size_t *length)
{
    uint64_t values[2];size_t size=event->read_format&4?16:8;
    if(capacity<size)return -KERNEL_ENOSPC;
    {
        KERNEL_RAW_SCOPE(lock,&trace_lock);
        values[0]=event->count;values[1]=event->object.id;
    }
    memcpy(buffer,values,size);*length=size;return 0;
}
int64_t bpf_link_create(struct kernel_files *files,union bpf_attribute *a)
{
    if(a->link.attach_type!=41 || a->link.flags)return -KERNEL_EINVAL;
    for(size_t i=BPF_END(union bpf_attribute,link.cookie);i<sizeof(*a);i++)
        if(((const unsigned char *)a)[i])return -KERNEL_EINVAL;
    int error;struct kernel_bpf_program *program;
    error=kernel_bpf_program_fd(files,(int32_t)a->link.program_fd,BPF_TRACEPOINT,&program);
    if(error)return error;
    struct bpf_perf *event=(void *)bpf_object_fd(files,(int32_t)a->link.target_fd,BPF_OBJECT_PERF,&error);
    if(!event){kernel_bpf_program_put(program);return error;}
    struct bpf_link *link=NULL;
    if(kernel_heap_allocate_zeroed(files->heap,1,sizeof(*link),(void **)&link)){
        bpf_object_put(&event->object);kernel_bpf_program_put(program);return -KERNEL_ENOMEM;
    }
    bpf_object_initialize(&link->object,files->heap,BPF_OBJECT_LINK);
    link->event=event;link->program=program;link->cookie=a->link.cookie;
    link->type=7;link->detached=1;
    error=attach(event,program,link);
    if(!error)error=bpf_object_publish(&link->object);
    if(!error)error=bpf_object_install(files,&link->object,2,1);
    bpf_object_put(&link->object);return error;
}
int bpf_link_detach(struct bpf_link *link)
{
    struct kernel_bpf_program *program=NULL;
    {
        KERNEL_RAW_SCOPE(lock,&trace_lock);
        if(link->detached)return 0;
        struct bpf_perf *event=link->event;
        if(event->link!=link || event->program!=link->program)__builtin_trap();
        event->link=NULL;program=event->program;event->program=NULL;event->cookie=0;
        link->detached=1;
        if(event->file_closed)event->enabled=0;
    }
    kernel_bpf_program_put(program);return 0;
}
static int event_get(struct bpf_perf *event)
{
    uint32_t old=__atomic_load_n(&event->object.references,__ATOMIC_RELAXED);
    while(old){
        if(old==UINT32_MAX)__builtin_trap();
        if(__atomic_compare_exchange_n(&event->object.references,&old,old+1,1,
            __ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return 1;
    }
    return 0;
}
static void trace_run(struct kernel_task *task,uint32_t type,void *context,uint32_t size)
{
    kernel_preempt_disable();
#if BOAROS_MAX_CPUS > 1
    unsigned cpu=kernel_cpu_current()->logical_id;
#else
    unsigned cpu=0;
#endif
    struct bpf_perf *event=NULL;uint32_t last=0,limit,accepted=1;
    {
        KERNEL_RAW_SCOPE(lock,&trace_lock);
        limit=last_event?last_event->object.id:0;
    }
    for(;;){
        struct bpf_perf *next=NULL;struct kernel_bpf_program *program=NULL;uint64_t cookie=0;
        {
            KERNEL_RAW_SCOPE(lock,&trace_lock);
            /* Normal iteration is linear. Only concurrent unlink of the
             * current node requires finding the next surviving ID again. */
            next=event && event->registered?event->next:first_event;
            while(next && (next->object.id<=last || next->event!=type ||
                !next->program || !event_get(next)))next=next->next;
            if(next && next->object.id>limit){
                /* The initial upper bound excludes events opened by this run. */
                last=next->object.id;
            }else if(next){
                last=next->object.id;program=next->program;cookie=next->cookie;
                if(program)kernel_bpf_program_get(program);
            }
        }
        if(event)bpf_object_put(&event->object);
        event=next;
        if(!event || last>limit)break;
        if(program)accepted&=(uint32_t)bpf_runtime_run(program,task,context,size,NULL,cookie);
        kernel_bpf_program_put(program);
    }
    if(event)bpf_object_put(&event->object);
    /* Linux attaches TRACEPOINT programs to the shared event_call array.
     * PID, CPU and enabled qualify perf counters, not BPF invocation. */
    if(accepted){
        KERNEL_RAW_SCOPE(lock,&trace_lock);
        for(event=first_event;event && event->object.id<=limit;event=event->next)
            if(event->event==type && event->enabled &&
                (!event->target_task || event->target_task==task) &&
                (event->cpu<0 || (unsigned)event->cpu==cpu) &&
                __atomic_load_n(&event->object.references,__ATOMIC_RELAXED))event->count++;
    }
    kernel_preempt_enable();
}
void kernel_bpf_trace_enter(struct kernel_task *task,uint64_t nr,const uint64_t args[6])
{
    if(!__atomic_load_n(&first_event,__ATOMIC_ACQUIRE))return;
    kernel_pid_t tid;if(kernel_task_tid(task,&tid))__builtin_trap();
    struct kernel_trace_sys_enter entry={.common={.type=KERNEL_TRACE_SYS_ENTER,.pid=tid},.id=(long)nr};
    memcpy(entry.args,args,sizeof(entry.args));trace_run(task,KERNEL_TRACE_SYS_ENTER,&entry,sizeof(entry));
}
void kernel_bpf_trace_exit(struct kernel_task *task,uint64_t nr,int64_t result)
{
    if(!__atomic_load_n(&first_event,__ATOMIC_ACQUIRE))return;
    kernel_pid_t tid;if(kernel_task_tid(task,&tid))__builtin_trap();
    struct kernel_trace_sys_exit entry={.common={.type=KERNEL_TRACE_SYS_EXIT,.pid=tid},.id=(long)nr,.ret=result};
    trace_run(task,KERNEL_TRACE_SYS_EXIT,&entry,sizeof(entry));
}
