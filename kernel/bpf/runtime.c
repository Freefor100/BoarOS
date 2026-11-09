#include "private.h"
#include <kernel/cpu.h>
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/mm.h>
#include <kernel/random.h>
#include <kernel/task.h>
#include <kernel/task_state.h>
#include <kernel/time.h>
#include <kernel/trace_events.h>
#include <arch/task.h>
#include <string.h>

struct run_reservation {struct bpf_ring_reservation ring;uint32_t offset,extent;unsigned active;};
struct execution {
    struct kernel_bpf_vm_workspace vm;
    struct kernel_bpf_program *program;
    struct kernel_task *task;
    const struct kernel_bpf_packet *packet;
    void *context;
    uint64_t cookie;
    uint32_t context_size,busy;
    struct run_reservation reservations[64];
    unsigned char arena[];
};
static void *verify_alloc(void *opaque,size_t size)
{
    struct kernel_bpf_program *p=opaque;void *result=NULL;
    return kernel_heap_allocate(p->object.heap,size,&result)?NULL:result;
}
static void verify_free(void *opaque,void *memory)
{struct kernel_bpf_program *p=opaque;if(memory)kernel_heap_release(p->object.heap,memory);}
static void verify_log(void *opaque,const char *text,size_t size)
{
    struct kernel_bpf_program *p=opaque;
    if(!p->log_capacity)return;
    p->log_total+=size;
    uint32_t capacity=p->log_capacity-1;
    if(size>capacity){text+=size-capacity;size=capacity;p->log_length=0;}
    if(p->log_length+size>capacity){
        uint32_t discard=(uint32_t)(p->log_length+size-capacity);
        memmove(p->verification_log,p->verification_log+discard,p->log_length-discard);
        p->log_length-=discard;
    }
    memcpy(p->verification_log+p->log_length,text,size);
    p->log_length+=(uint32_t)size;p->verification_log[p->log_length]=0;
}
static int verify_map(void *opaque,uint32_t pc,const struct kernel_bpf_insn *instruction,
    struct kernel_bpf_vm_map *out)
{
    (void)instruction;struct kernel_bpf_program *p=opaque;
    uint32_t low=0,high=p->binding_count;
    while(low<high){uint32_t middle=low+(high-low)/2;
        if(p->bindings[middle].pc<pc)low=middle+1;else high=middle;}
    if(low==p->binding_count || p->bindings[low].pc!=pc)return -KERNEL_EINVAL;
    struct bpf_program_binding *binding=&p->bindings[low];
    const struct bpf_map_spec *spec=bpf_map_spec(binding->map->storage);
    *out=(struct kernel_bpf_vm_map){.object=binding->map,.value=binding->value,
        .key_size=spec->key_size,.value_size=spec->value_size,.type=spec->type,
        .flags=(spec->map_flags&128?KERNEL_BPF_MAP_READONLY:0) |
            (spec->map_flags&256?KERNEL_BPF_MAP_WRITEONLY:0) |
            (binding->value && (spec->map_flags&128) && bpf_map_is_frozen(binding->map->storage)?
                KERNEL_BPF_MAP_CONSTANT_VALUE:0)};
    return 0;
}
static int trace_access(uint32_t limit,int32_t offset,uint32_t size,int write,
    struct kernel_bpf_vm_context_access *out)
{
    if(write || offset<8 || (size!=1 && size!=2 && size!=4 && size!=8) ||
        (uint32_t)offset>limit || size>limit-(uint32_t)offset || (uint32_t)offset%size)
        return -KERNEL_EACCES;
    *out=(struct kernel_bpf_vm_context_access){.kind=KERNEL_BPF_CONTEXT_SCALAR};return 0;
}
static int trace_exit_access(void *opaque,int32_t off,uint32_t size,int write,
    struct kernel_bpf_vm_context_access *out)
{(void)opaque;return trace_access(sizeof(struct kernel_trace_sys_exit),off,size,write,out);}
static int context_access(void *opaque,int32_t offset,uint32_t size,int write,
    struct kernel_bpf_vm_context_access *out)
{
    struct kernel_bpf_program *p=opaque;
    *out=(struct kernel_bpf_vm_context_access){.kind=KERNEL_BPF_CONTEXT_SCALAR};
    if(p->type==BPF_TRACEPOINT)return trace_access(sizeof(struct kernel_trace_sys_enter),offset,size,write,out);
    if(p->type==BPF_RAW_TRACEPOINT || p->type==BPF_KPROBE){
        uint32_t limit=p->type==BPF_RAW_TRACEPOINT?16:sizeof(struct arch_trap_frame);
        if(write || offset<0 || (size!=1 && size!=2 && size!=4 && size!=8) ||
            (uint32_t)offset%size || (uint32_t)offset>limit || size>limit-(uint32_t)offset)
            return -KERNEL_EACCES;
        /* Recorded during load verification, before the program is published. */
        uint32_t end=(uint32_t)offset+size;
        if(end>p->max_context_size)p->max_context_size=end;
        return 0;
    }
    /* SOCKET_FILTER has no direct data/data_end access. Its cb fields are
     * private to this packet invocation; other supported metadata is read-only. */
    if(offset<0 || size!=4 || offset%4 || offset>68 || (write && (offset<48 || offset>=68)))
        return -KERNEL_EACCES;
    return 0;
}
#define ARG(kind_) ((struct kernel_bpf_vm_arg){.kind=(kind_)})
#define SIZED(kind_,size_) ((struct kernel_bpf_vm_arg){.kind=(kind_),.size_arg=(size_)})
#define DATA_MAPS ((UINT64_C(1)<<1)|(UINT64_C(1)<<2)|(UINT64_C(1)<<5)|(UINT64_C(1)<<6))
static int helper_prototype(void *opaque,uint32_t id,struct kernel_bpf_vm_helper *h)
{
    struct kernel_bpf_program *p=opaque;*h=(struct kernel_bpf_vm_helper){0};
    switch(id){
    case 1:case 2:case 3:
        h->map_arg=1;h->map_types=DATA_MAPS;h->args[0]=ARG(KERNEL_BPF_ARG_MAP);
        h->args[1]=ARG(KERNEL_BPF_ARG_MAP_KEY);
        if(id==1){h->result=KERNEL_BPF_RETURN_MAP_VALUE_OR_NULL;}
        else {h->map_write=1;if(id==2){h->args[2]=ARG(KERNEL_BPF_ARG_MAP_VALUE);h->args[3]=ARG(KERNEL_BPF_ARG_SCALAR);}}
        return 0;
    case 5:case 7:case 8:case 14:case 15:case 125:return 0;
    case 16:
        h->args[0]=SIZED(KERNEL_BPF_ARG_MEM_WRITE,2);h->args[1]=ARG(KERNEL_BPF_ARG_CONST);return 0;
    case 174:
        if(p->type==BPF_SOCKET_FILTER)return -KERNEL_EACCES;
        h->args[0]=ARG(KERNEL_BPF_ARG_CONTEXT);return 0;
    case 26:
        if(p->type!=BPF_SOCKET_FILTER)return -KERNEL_EACCES;
        h->args[0]=ARG(KERNEL_BPF_ARG_CONTEXT);h->args[1]=ARG(KERNEL_BPF_ARG_SCALAR);
        h->args[2]=SIZED(KERNEL_BPF_ARG_MEM_WRITE,4);h->args[3]=ARG(KERNEL_BPF_ARG_CONST);return 0;
    case 4:case 112:case 113:case 114:case 115:
        if(p->type==BPF_SOCKET_FILTER || !p->gpl)return -KERNEL_EACCES;
        h->args[0]=SIZED(KERNEL_BPF_ARG_MEM_WRITE,2);h->args[1]=ARG(KERNEL_BPF_ARG_CONST);
        h->args[2]=ARG(KERNEL_BPF_ARG_ANY);return 0;
    case 130:
        h->map_arg=1;h->map_types=UINT64_C(1)<<27;h->args[0]=ARG(KERNEL_BPF_ARG_MAP);
        h->args[1]=SIZED(KERNEL_BPF_ARG_MEM_READ,3);h->args[2]=ARG(KERNEL_BPF_ARG_SCALAR);
        h->args[3]=ARG(KERNEL_BPF_ARG_SCALAR);return 0;
    case 131:
        h->map_arg=1;h->map_types=UINT64_C(1)<<27;h->args[0]=ARG(KERNEL_BPF_ARG_MAP);
        h->args[1]=ARG(KERNEL_BPF_ARG_CONST);h->args[2]=ARG(KERNEL_BPF_ARG_SCALAR);
        h->result=KERNEL_BPF_RETURN_RINGBUF_OR_NULL;h->size_arg=2;return 0;
    case 132:case 133:
        h->args[0]=ARG(KERNEL_BPF_ARG_RINGBUF);h->args[1]=ARG(KERNEL_BPF_ARG_SCALAR);
        h->releases_ringbuf=1;return 0;
    case 134:
        h->map_arg=1;h->map_types=UINT64_C(1)<<27;h->args[0]=ARG(KERNEL_BPF_ARG_MAP);
        h->args[1]=ARG(KERNEL_BPF_ARG_SCALAR);return 0;
    default:return -KERNEL_ENOENT;
    }
}
void bpf_runtime_environment(struct kernel_bpf_program *p)
{
    p->environment=(struct kernel_bpf_vm_env){.opaque=p,.program_type=p->type,
        .alloc=verify_alloc,.free=verify_free,.map=verify_map,.helper=helper_prototype,
        .context=context_access,.log=verify_log};
}
int bpf_program_trace_context(struct kernel_bpf_program *p,uint32_t event)
{
    if(p->type!=BPF_TRACEPOINT)return -KERNEL_EINVAL;
    struct kernel_bpf_vm_env env=p->environment;env.log=NULL;
    if(event==KERNEL_TRACE_SYS_EXIT)env.context=trace_exit_access;
    else if(event!=KERNEL_TRACE_SYS_ENTER)return -KERNEL_ENOENT;
    return kernel_bpf_verify(p->instructions,p->count,&env);
}
int bpf_runtime_prepare(struct kernel_bpf_program *p)
{
    uint32_t arena=0;int reserve=0;
    for(uint32_t pc=0;pc<p->count;pc++)if(p->instructions[pc].code==0x85 &&
        !p->instructions[pc].src_reg && p->instructions[pc].imm==131)reserve=1;
    if(reserve)for(uint32_t i=0;i<p->map_count;i++){
        const struct bpf_map_spec *spec=bpf_map_spec(p->maps[i]->storage);
        if(spec->type==BPF_STORAGE_RINGBUF && arena<spec->max_entries)arena=spec->max_entries;
    }
    p->execution_cpus=kernel_cpu_get_boot_count();p->arena_size=arena;
    p->execution_stride=(sizeof(struct execution)+(size_t)arena+15)&~(size_t)15;
    if(!p->execution_cpus || p->execution_stride>SIZE_MAX/p->execution_cpus)return -KERNEL_ENOMEM;
    if(kernel_heap_allocate_zeroed(p->object.heap,p->execution_cpus,p->execution_stride,&p->execution))
        return -KERNEL_ENOMEM;
    p->prepared=1;return 0;
}
void bpf_runtime_release(struct kernel_bpf_program *p)
{
    if(p->execution){
        for(unsigned i=0;i<p->execution_cpus;i++)
            if(((struct execution *)((char *)p->execution+i*p->execution_stride))->busy)__builtin_trap();
        kernel_heap_release(p->object.heap,p->execution);p->execution=NULL;
    }
}
static int kernel_readable(struct execution *e,uint64_t source,size_t size)
{
    if(!size)return 1;
    if(source>UINT64_MAX-size)return 0;
    uint64_t context=(uintptr_t)e->context;
    if(source>=context && source-context<=e->context_size && size<=e->context_size-(source-context))return 1;
    extern unsigned char __kernel_start[],__kernel_end[];
    if(source>=(uintptr_t)__kernel_start && source<=(uintptr_t)__kernel_end &&
        size<=(uintptr_t)__kernel_end-source)return 1;
    uint64_t physical;
    if(arch_direct_map_va_to_pa(source,size,&physical)!=ARCH_DIRECT_MAP_STATUS_OK)return 0;
    struct physical_page_allocator *a=e->program->object.heap->page_allocator;
    for(unsigned i=0;i<a->range_count;i++)
        if(physical>=a->ranges[i].base && physical<=a->ranges[i].end &&
            size<=a->ranges[i].end-physical)return 1;
    return 0;
}
static int probe_copy(struct execution *e,void *out,uint64_t source,uint32_t size,int user,int string)
{
    if(!size)return string?-KERNEL_EINVAL:0;
    memset(out,0,size);
    if(source>UINT64_MAX-size)return -KERNEL_EFAULT;
    struct kernel_mm *mm=NULL;
    if(user && (!e->task || kernel_task_mm_borrow_mutable(e->task,&mm)))return -KERNEL_EFAULT;
    uint32_t done=0;
    while(done<size){
        uint32_t n=size-done;const unsigned char *input;
        struct kernel_mm_user_pin pin={0};
        if(user){
            uint32_t offset=(uint32_t)((source+done)&(BOAROS_PAGE_SIZE-1));
            if(n>BOAROS_PAGE_SIZE-offset)n=BOAROS_PAGE_SIZE-offset;
            if(kernel_mm_try_pin_user_page(mm,source+done,KERNEL_MM_READ,&pin)){
                memset(out,0,size);return -KERNEL_EFAULT;
            }
            input=(const unsigned char *)pin.page+offset;
        }else{
            if(!kernel_readable(e,source+done,n)){memset(out,0,size);return -KERNEL_EFAULT;}
            input=(const void *)(uintptr_t)(source+done);
        }
        uint32_t take=0;int terminated=0;
        if(string){
            while(take<n){unsigned char c=input[take];((unsigned char *)out)[done+take++]=c;
                if(!c){terminated=1;break;}}
        }else{memcpy((char *)out+done,input,n);take=n;}
        if(user)kernel_mm_unpin_user_page(&pin);
        done+=take;
        if(terminated)return (int)done;
    }
    if(string){((char *)out)[size-1]=0;return (int)size;}
    return 0;
}
static unsigned current_cpu(void)
{
#if BOAROS_MAX_CPUS > 1
    return kernel_cpu_current()->logical_id;
#else
    return 0;
#endif
}
static uint64_t reserve_record(struct execution *e,struct bpf_map *map,uint32_t size,uint64_t flags)
{
    unsigned slot=64;uint32_t at=0,extent=(size+7U)&~7U;
    if(size>UINT32_MAX-7U || extent>e->program->arena_size)return 0;
    if(!extent)extent=8;
    for(unsigned i=0;i<64;i++)if(!e->reservations[i].active){slot=i;break;}
    if(slot==64)__builtin_trap();
    for(;;){
        uint32_t next=e->program->arena_size;struct run_reservation *nearest=NULL;
        for(unsigned i=0;i<64;i++)if(e->reservations[i].active && e->reservations[i].offset>=at &&
            (!nearest || e->reservations[i].offset<next)){nearest=&e->reservations[i];next=nearest->offset;}
        if(extent<=next-at)break;
        if(!nearest)return 0;
        at=nearest->offset+nearest->extent;
        if(at>e->program->arena_size || extent>e->program->arena_size-at)return 0;
    }
    struct run_reservation *r=&e->reservations[slot];
    if(bpf_ring_reserve(bpf_map_ring(map),size,flags,e->arena+at,extent,&r->ring))return 0;
    r->offset=at;r->extent=extent;r->active=1;return (uintptr_t)r->ring.data;
}
static uint64_t run_helper(void *opaque,uint32_t id,const uint64_t args[5])
{
    struct execution *e=opaque;unsigned cpu=current_cpu();
    struct bpf_map_object *map=(void *)(uintptr_t)args[0];
    switch(id){
    case 1:return (uintptr_t)bpf_map_lookup_borrowed(map->storage,(const void *)(uintptr_t)args[1],cpu);
    case 2:return (uint64_t)(int64_t)bpf_map_update(map->storage,(const void *)(uintptr_t)args[1],
        (const void *)(uintptr_t)args[2],args[3],0,cpu);
    case 3:return (uint64_t)(int64_t)bpf_map_delete(map->storage,(const void *)(uintptr_t)args[1],0);
    case 5:case 125:return kernel_time_monotonic_ns();
    case 7:{
        uint32_t value;
        if(kernel_random_fill(&value,sizeof(value))!=KERNEL_RANDOM_STATUS_OK)
            value=(uint32_t)(kernel_time_monotonic_ns() ^ e->program->run_count);
        return value;
    }
    case 8:return cpu;
    case 14:{kernel_pid_t tid=0,tgid=0;
        if(e->task){(void)kernel_task_tid(e->task,&tid);(void)kernel_task_tgid(e->task,&tgid);}
        return (uint32_t)tid|((uint64_t)(uint32_t)tgid<<32);}
    case 15:{const struct kernel_credentials *c=kernel_credentials_current();return c->uid|((uint64_t)c->gid<<32);}
    case 16:{void *out=(void *)(uintptr_t)args[0];uint32_t size=(uint32_t)args[1];
        if(size)memset(out,0,size);
        if(!size)return (uint64_t)-KERNEL_EINVAL;
        if(e->task){uint32_t n=size<16?size:16;memcpy(out,e->task->comm,n);((char *)out)[size-1]=0;}
        return 0;}
    case 26:{void *out=(void *)(uintptr_t)args[2];uint32_t size=(uint32_t)args[3];
        if(!e->packet || e->packet->read(e->packet->object,(uint32_t)args[1],out,size)){
            memset(out,0,size);return (uint64_t)-KERNEL_EFAULT;}
        return 0;}
    case 4:case 112:case 113:case 114:case 115:
        return (uint64_t)(int64_t)probe_copy(e,(void *)(uintptr_t)args[0],args[2],(uint32_t)args[1],
            id==112 || id==114,id==114 || id==115);
    case 130:return (uint64_t)(int64_t)bpf_ring_output(bpf_map_ring(map->storage),
        (const void *)(uintptr_t)args[1],(uint32_t)args[2],args[3]);
    case 131:return reserve_record(e,map->storage,(uint32_t)args[1],args[2]);
    case 132:case 133:
        for(unsigned i=0;i<64;i++)if(e->reservations[i].active &&
            (uintptr_t)e->reservations[i].ring.data==args[0]){
            int result=bpf_ring_submit(&e->reservations[i].ring,args[1],id==133);
            e->reservations[i].active=0;return (uint64_t)(int64_t)result;
        }
        __builtin_trap();
    case 134:return bpf_ring_query(bpf_map_ring(map->storage),args[1]);
    case 174:return e->cookie;
    default:__builtin_trap();
    }
}
static int packet_read(void *opaque,int64_t offset,uint32_t size,uint32_t *value)
{
    struct execution *e=opaque;unsigned char bytes[4];
    if(!e->packet || (size!=1 && size!=2 && size!=4))return -1;
    if(offset<0){
        if(offset>=-INT64_C(0x100000))offset=e->packet->network_offset+offset+INT64_C(0x100000);
        else if(offset>=-INT64_C(0x200000))offset=e->packet->link_offset+offset+INT64_C(0x200000);
        else return -1;
    }
    if(e->packet->read(e->packet->object,offset,bytes,size))return -1;
    *value=0;for(unsigned i=0;i<size;i++)*value=(*value<<8)|bytes[i];return 0;
}
static uint32_t packet_length(void *opaque)
{struct execution *e=opaque;return e->packet?e->packet->length:0;}
static int ancillary(void *opaque,uint32_t selector,uint32_t a,uint32_t x,uint32_t *out)
{
    struct execution *e=opaque;const struct kernel_bpf_packet *p=e->packet;
    if(!p)return -1;
    switch(selector){
    case 0:*out=p->protocol;break;
    case 4:*out=p->packet_type;break;
    case 8:*out=p->ifindex;break;
    case 20:*out=p->mark;break;
    case 24:*out=p->queue;break;
    case 28:*out=p->hardware_type;break;
    case 32:*out=p->hash;break;
    case 36:*out=current_cpu();break;
    case 40:*out=a^x;break;
    case 44:*out=p->vlan_tci;break;
    case 48:*out=p->vlan_present;break;
    case 52:*out=p->payload_offset;break;
    case 56:{uint64_t args[5]={0};*out=(uint32_t)run_helper(e,7,args);break;}
    case 60:*out=p->vlan_protocol;break;
    case 12:case 16:{
        uint32_t end=p->length,at=a;
        *out=0;
        if(selector==16){uint16_t header[2];if(p->read(p->object,at,header,4) || header[0]<4 || header[0]>end-at)return 0;
            end=at+header[0];at+=4;}
        while(at<=end && end-at>=4){
            uint16_t header[2];if(p->read(p->object,at,header,4) || header[0]<4 || header[0]>end-at)break;
            if((header[1]&0x3fff)==x){*out=at;break;}
            uint32_t step=(header[0]+3U)&~3U;if(step>end-at)break;at+=step;
        }
        break;
    }
    default:return -1;
    }
    return 0;
}
uint64_t bpf_runtime_run(struct kernel_bpf_program *p,struct kernel_task *task,void *context,
    uint32_t context_size,const struct kernel_bpf_packet *packet,uint64_t cookie)
{
    if(!p->prepared)__builtin_trap();
    kernel_preempt_disable();unsigned cpu=current_cpu();
    if(cpu>=p->execution_cpus)__builtin_trap();
    struct execution *e=(void *)((char *)p->execution+(size_t)cpu*p->execution_stride);
    if(e->busy)__builtin_trap();
    e->busy=1;e->program=p;e->task=task;e->packet=packet;e->context=context;e->context_size=context_size;
    e->cookie=cookie;
    for(uint32_t i=0;i<p->map_count;i++)bpf_map_borrow_begin(p->maps[i]->storage);
    struct kernel_bpf_vm_run run={.opaque=e,.context=context,.context_size=context_size,
        .helper=run_helper,.packet=packet_read,.packet_length=packet_length,.ancillary=ancillary};
    uint64_t result=0,before=kernel_time_monotonic_ns();
    if(kernel_bpf_vm_execute(p->instructions,p->count,&p->environment,&run,&e->vm,&result))__builtin_trap();
    __atomic_fetch_add(&p->run_time,kernel_time_monotonic_ns()-before,__ATOMIC_RELAXED);
    __atomic_fetch_add(&p->run_count,1,__ATOMIC_RELAXED);
    for(unsigned i=0;i<64;i++)if(e->reservations[i].active)__builtin_trap();
    for(uint32_t i=0;i<p->map_count;i++)bpf_map_borrow_end(p->maps[i]->storage);
    e->busy=0;e->task=NULL;e->packet=NULL;e->context=NULL;
    kernel_preempt_enable();return result;
}
void bpf_runtime_packet_context(struct bpf_skb_context *context,const struct kernel_bpf_packet *packet)
{
    *context=(struct bpf_skb_context){.length=packet->length,.packet_type=packet->packet_type,
        .mark=packet->mark,.queue=packet->queue,
        .protocol=((packet->protocol&255)<<8)|(packet->protocol>>8),
        .vlan_present=packet->vlan_present,.vlan_tci=packet->vlan_tci,
        .vlan_protocol=((packet->vlan_protocol&255)<<8)|(packet->vlan_protocol>>8),
        .ingress_ifindex=packet->ingress_ifindex,.ifindex=packet->ifindex,.hash=packet->hash};
}
uint32_t kernel_bpf_filter(struct kernel_bpf_program *p,const struct kernel_bpf_packet *packet)
{
    struct bpf_skb_context context;
    bpf_runtime_packet_context(&context,packet);
    return (uint32_t)bpf_runtime_run(p,kernel_task_current(),&context,sizeof(context),packet,0);
}
