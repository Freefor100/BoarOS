#include "private.h"
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/files.h>
#include <kernel/task.h>
#include <kernel/time.h>
#include <kernel/uaccess.h>
#include <string.h>

static int gpl_license(const char *license)
{
    static const char *const names[]={"GPL","GPL v2","GPL and additional rights",
        "Dual BSD/GPL","Dual MIT/GPL","Dual MPL/GPL"};
    for(unsigned i=0;i<sizeof(names)/sizeof(*names);i++)if(!strcmp(license,names[i]))return 1;
    return 0;
}
static int trailing_zero(const union bpf_attribute *a,size_t last)
{
    const unsigned char *p=(const void *)a;
    for(size_t i=last;i<sizeof(*a);i++)if(p[i])return -KERNEL_EINVAL;
    return 0;
}
static int copy_records(struct kernel_bpf_program *p,struct kernel_mm *mm,uint64_t address,
    uint32_t count,uint32_t size,uint32_t known,void **owner)
{
    if(!count)return size || address?-KERNEL_EINVAL:0;
    if(!p->btf || size<known || size>4096 || count>p->count)return -KERNEL_EINVAL;
    if(kernel_heap_allocate(p->object.heap,(size_t)count*known,owner))return -KERNEL_ENOMEM;
    unsigned char record[64];
    if(known>sizeof(record))__builtin_trap();
    uint32_t previous=0;
    for(uint32_t i=0;i<count;i++){
        int error=bpf_user_copy(mm,address+(uint64_t)i*size,record,known,1);
        if(error)return error;
        uint32_t pc;memcpy(&pc,record,4);
        if(pc>=p->count || (i && pc<=previous) || (!i && known==8 && pc))return -KERNEL_EINVAL;
        previous=pc;
        if(known==8){
            const uint32_t *type;uint32_t id;memcpy(&id,record+4,4);
            if(bpf_btf_record(p->btf,id,&type) || ((type[1]>>24)&31)!=12)return -KERNEL_EINVAL;
        }
        memcpy((char *)*owner+(size_t)i*known,record,known);
        for(uint32_t offset=known;offset<size;){
            uint32_t n=size-offset;if(n>sizeof(record))n=sizeof(record);
            error=bpf_user_copy(mm,address+(uint64_t)i*size+offset,record,n,1);
            if(error)return error;
            for(unsigned j=0;j<n;j++)if(record[j])return -KERNEL_E2BIG;
            offset+=n;
        }
    }
    return 0;
}
static int relocate_maps(struct kernel_bpf_program *p,struct kernel_files *files,
    struct kernel_mm *mm,const union bpf_attribute *a)
{
    uint32_t relocations=0;
    for(uint32_t pc=0;pc<p->count;pc++)if(p->instructions[pc].code==0x18){
        if(pc+1==p->count)return -KERNEL_EINVAL;
        if(p->instructions[pc].src_reg)relocations++;
        pc++;
    }
    if(!relocations)return 0;
    if(kernel_heap_allocate_zeroed(files->heap,relocations,sizeof(*p->bindings),(void **)&p->bindings) ||
        kernel_heap_allocate_zeroed(files->heap,relocations,sizeof(*p->maps),(void **)&p->maps))return -KERNEL_ENOMEM;
    /* Each referenced descriptor is resolved once for this load. The retained
     * identity remains valid across concurrent close and descriptor reuse. */
    struct bpf_map_object **fds=NULL;
    if(kernel_heap_allocate_zeroed(files->heap,1024,sizeof(*fds),(void **)&fds))return -KERNEL_ENOMEM;
    int error=0;
    for(uint32_t pc=0;pc<p->count;pc++){
        struct kernel_bpf_insn *instruction=&p->instructions[pc];
        if(instruction->code!=0x18)continue;
        uint32_t source=instruction->src_reg;
        if(!source){pc++;continue;}
        if(source!=1 && source!=2 && source!=5 && source!=6){error=-KERNEL_EINVAL;break;}
        int32_t fd=instruction->imm;
        if(source==5 || source==6){
            if(fd<0 || !a->program.fd_array ||
                (a->program.fd_array_count && (uint32_t)fd>=a->program.fd_array_count)){
                error=-KERNEL_EINVAL;break;
            }
            error=bpf_user_copy(mm,a->program.fd_array+(uint64_t)(uint32_t)fd*4,&fd,4,1);
            if(error)break;
        }
        if(fd<0 || fd>=1024){error=-KERNEL_EBADF;break;}
        struct bpf_map_object *map=fds[fd];
        if(!map){
            map=(void *)bpf_object_fd(files,fd,BPF_OBJECT_MAP,&error);
            if(!map)break;
            fds[fd]=map;p->maps[p->map_count++]=map;
        }
        void *value=NULL;
        if(source==2 || source==6){
            const struct bpf_map_spec *spec=bpf_map_spec(map->storage);
            if(spec->type!=BPF_STORAGE_ARRAY || spec->max_entries!=1){error=-KERNEL_EINVAL;break;}
            uint32_t key=0;bpf_map_borrow_begin(map->storage);
            value=bpf_map_lookup_borrowed(map->storage,&key,0);
            bpf_map_borrow_end(map->storage);
            if(!value){error=-KERNEL_EINVAL;break;}
        }
        p->bindings[p->binding_count++]=(struct bpf_program_binding){pc,map,value};
        pc++;
    }
    kernel_heap_release(files->heap,fds);return error;
}
int64_t bpf_program_load(struct kernel_task *task,struct kernel_files *files,
    struct kernel_mm *mm,uint64_t user_attribute,uint32_t attribute_size,union bpf_attribute *a)
{
    (void)task;
    if(trailing_zero(a,sizeof(a->program)))return -KERNEL_EINVAL;
    const struct kernel_credentials *credentials=kernel_credentials_current();
    int cap=kernel_credentials_capable(credentials,KERNEL_CAP_BPF) ||
        kernel_credentials_capable(credentials,KERNEL_CAP_SYS_ADMIN);
    if(!cap && kernel_bpf_unprivileged_get())return -KERNEL_EPERM;
    if(a->program.type!=BPF_SOCKET_FILTER && a->program.type!=BPF_TRACEPOINT &&
        a->program.type!=BPF_RAW_TRACEPOINT && a->program.type!=BPF_KPROBE)return -KERNEL_EINVAL;
    if(a->program.type!=BPF_SOCKET_FILTER && (!cap ||
        !(kernel_credentials_capable(credentials,KERNEL_CAP_PERFMON) ||
        kernel_credentials_capable(credentials,KERNEL_CAP_SYS_ADMIN))))return -KERNEL_EPERM;
    if(!a->program.count || a->program.count>(cap?KERNEL_BPF_COMPLEXITY_LIMIT:4096U))return -KERNEL_E2BIG;
    if(a->program.ifindex || a->program.attach_type || a->program.attach_btf_id ||
        a->program.attach_object_fd || a->program.core_count || a->program.core_relocations ||
        a->program.core_record_size || a->program.token_fd || a->program.signature ||
        a->program.signature_size || a->program.keyring || a->program.flags&~3U)return -KERNEL_EINVAL;
    if(a->program.log_level&~15U ||
        (a->program.log_level && (!a->program.log || !a->program.log_size)))return -KERNEL_EINVAL;
    for(unsigned i=0;i<16;i++){
        unsigned char c=a->program.name[i];
        if(!c)break;
        if(i==15 || !((c>='a'&&c<='z') || (c>='A'&&c<='Z') ||
            (c>='0'&&c<='9') || c=='_' || c=='.'))return -KERNEL_EINVAL;
    }
    struct kernel_bpf_program *p=NULL;
    if(kernel_heap_allocate_zeroed(files->heap,1,sizeof(*p),(void **)&p))return -KERNEL_ENOMEM;
    bpf_object_initialize(&p->object,files->heap,BPF_OBJECT_PROGRAM);
    p->type=a->program.type;p->count=a->program.count;p->loaded_at=kernel_time_monotonic_ns();
    memcpy(p->object.name,a->program.name,16);
    int error=0;
    char license[128];size_t copied;
    enum kernel_uaccess_status status=kernel_copy_string_from_user(mm,license,a->program.license,sizeof(license),&copied);
    if(status){error=status==KERNEL_UACCESS_STATUS_FAULT?-KERNEL_EFAULT:-KERNEL_EINVAL;goto done;}
    p->gpl=gpl_license(license);
    if(kernel_heap_allocate(files->heap,(size_t)p->count*sizeof(*p->instructions),(void **)&p->instructions)){
        error=-KERNEL_ENOMEM;goto done;
    }
    error=bpf_user_copy(mm,a->program.insns,p->instructions,(size_t)p->count*sizeof(*p->instructions),1);
    if(error)goto done;
    if(a->program.btf_fd){
        p->btf=(void *)bpf_object_fd(files,(int32_t)a->program.btf_fd,BPF_OBJECT_BTF,&error);
        if(!p->btf)goto done;
    }
    error=copy_records(p,mm,a->program.funcs,a->program.func_count,a->program.func_record_size,8,&p->function_info);
    if(error)goto done;
    p->function_count=a->program.func_count;p->function_size=p->function_count?8:0;
    error=copy_records(p,mm,a->program.lines,a->program.line_count,a->program.line_record_size,16,&p->line_info);
    if(error)goto done;
    p->line_count=a->program.line_count;p->line_size=p->line_count?16:0;
    error=relocate_maps(p,files,mm,a);
    if(error)goto done;
    if(a->program.log_level){
        if(kernel_heap_allocate(files->heap,a->program.log_size,(void **)&p->verification_log)){
            error=-KERNEL_ENOMEM;goto done;
        }
        p->log_capacity=a->program.log_size;p->verification_log[0]=0;
    }
    bpf_runtime_environment(p);
    error=kernel_bpf_verify(p->instructions,p->count,&p->environment);
    if(p->verification_log){
        uint32_t bytes=p->log_length+1;
        int copy=bpf_user_copy(mm,a->program.log,p->verification_log,bytes,0);
        if(copy)error=copy;
        else if(p->log_total+1>p->log_capacity)error=-KERNEL_ENOSPC;
        uint32_t true_size=p->log_total>=UINT32_MAX?UINT32_MAX:(uint32_t)p->log_total+1;
        if(attribute_size>=BPF_END(union bpf_attribute,program.log_true_size)){
            copy=bpf_user_copy(mm,user_attribute+offsetof(union bpf_attribute,program.log_true_size),&true_size,4,0);
            if(copy)error=copy;
        }
        kernel_heap_release(files->heap,p->verification_log);p->verification_log=NULL;
        p->log_capacity=p->log_length=0;p->log_total=0;
    }
    if(error)goto done;
    bpf_program_calculate_tag(p);
    error=bpf_runtime_prepare(p);
    if(!error)error=bpf_object_publish(&p->object);
    if(!error)error=bpf_object_install(files,&p->object,0,1);
done:
    bpf_object_put(&p->object);return error;
}
int kernel_bpf_classic_create(struct kernel_heap *heap,
    const struct kernel_bpf_classic_insn *instructions,uint32_t count,
    struct kernel_bpf_program **out)
{
    if(!count || count>KERNEL_BPF_CLASSIC_MAX_INSNS)return -KERNEL_EINVAL;
    struct kernel_bpf_program *p=NULL;
    if(kernel_heap_allocate_zeroed(heap,1,sizeof(*p),(void **)&p))return -KERNEL_ENOMEM;
    bpf_object_initialize(&p->object,heap,BPF_OBJECT_PROGRAM);
    p->type=BPF_SOCKET_FILTER;p->gpl=1;p->loaded_at=kernel_time_monotonic_ns();
    bpf_runtime_environment(p);p->environment.flags|=KERNEL_BPF_VM_CLASSIC;
    uint32_t converted=0;
    int error=kernel_bpf_classic_convert(instructions,count,NULL,0,&converted,&p->environment);
    if(error)goto done;
    if(kernel_heap_allocate(heap,(size_t)count*sizeof(*p->classic),(void **)&p->classic) ||
        kernel_heap_allocate(heap,(size_t)converted*sizeof(*p->instructions),(void **)&p->instructions)){
        error=-KERNEL_ENOMEM;goto done;
    }
    memcpy(p->classic,instructions,(size_t)count*sizeof(*p->classic));p->classic_count=count;
    error=kernel_bpf_classic_convert(instructions,count,p->instructions,converted,&p->count,&p->environment);
    if(!error)error=bpf_runtime_prepare(p);
    if(error)goto done;
    *out=p;return 0;
done:bpf_object_put(&p->object);return error;
}
struct test_packet {const unsigned char *bytes;uint32_t length;};
static int test_packet_read(void *object,int64_t offset,void *out,uint32_t size)
{
    struct test_packet *p=object;
    /* The execution view starts after Ethernet; LL-relative loads may still
     * borrow the original header, just as skb's reserved headroom does. */
    offset+=14;
    if(offset<0 || (uint64_t)offset>p->length || size>p->length-(uint32_t)offset)return -1;
    memcpy(out,p->bytes+offset,size);return 0;
}
static int test_packet_prepare(struct test_packet *bytes,struct kernel_bpf_packet *packet)
{
    const unsigned char *data=bytes->bytes;
    uint32_t protocol=((uint32_t)data[12]<<8)|data[13];
    if(protocol<1536)
        protocol=bytes->length>=16 && data[14]==255 && data[15]==255?1:4;
    if((protocol==0x0800 && bytes->length<34) ||
        (protocol==0x86dd && bytes->length<54))return -KERNEL_EINVAL;
    unsigned type=0;
    /* loopback's device and broadcast addresses are both zero, so even
     * ff:ff:ff:ff:ff:ff is classified as multicast by eth_type_trans. */
    if(data[0]&1)type=2;
    else{
        /* The synthetic device is loopback, whose Ethernet address is zero. */
        for(unsigned i=0;i<6;i++)if(data[i]){type=3;break;}
    }
    *packet=(struct kernel_bpf_packet){.object=bytes,.length=bytes->length-14,
        .protocol=protocol,.ifindex=1,.packet_type=type,.hardware_type=772,
        .network_offset=0,.link_offset=-14,.read=test_packet_read};
    return 0;
}
int64_t bpf_program_test(struct kernel_task *task,struct kernel_files *files,
    struct kernel_mm *mm,uint64_t user_attribute,uint32_t attribute_size,union bpf_attribute *a)
{
    /* usize controls imported input, not the addresses of command outputs. */
    (void)attribute_size;
    if(trailing_zero(a,BPF_END(union bpf_attribute,test.batch_size)) ||
        a->test.flags || a->test.cpu || a->test.batch_size)
        return -KERNEL_EINVAL;
    int error;
    struct kernel_bpf_program *p=(void *)bpf_object_fd(files,(int32_t)a->test.fd,BPF_OBJECT_PROGRAM,&error);
    if(!p)return error;
    if(p->type!=BPF_SOCKET_FILTER && p->type!=BPF_RAW_TRACEPOINT){error=-KERNEL_EOPNOTSUPP;goto done;}
    if(p->type==BPF_RAW_TRACEPOINT){
        /* Linux MAX_BPF_FUNC_ARGS is 12. Only the verified accesses determine
         * the minimum; a program without context loads accepts zero bytes. */
        if(a->test.data_in || a->test.data_out || a->test.context_out ||
            a->test.duration || a->test.repeat ||
            a->test.context_in_size<p->max_context_size || a->test.context_in_size>12*8){
            error=-KERNEL_EINVAL;goto done;
        }
    }else if(a->test.data_in_size<14 || a->test.context_in_size || a->test.context_out_size ||
        a->test.context_in || a->test.context_out){
        error=-KERNEL_EINVAL;goto done;
    }
    size_t input=p->type==BPF_RAW_TRACEPOINT?a->test.context_in_size:a->test.data_in_size;
    unsigned char *data=NULL;
    if(input && kernel_heap_allocate(files->heap,input,(void **)&data)){error=-KERNEL_ENOMEM;goto done;}
    if(input){error=bpf_user_copy(mm,p->type==BPF_RAW_TRACEPOINT?a->test.context_in:a->test.data_in,data,input,1);if(error)goto release;}
    struct test_packet bytes={data,(uint32_t)input};
    struct kernel_bpf_packet packet;
    struct bpf_skb_context context;
    if(p->type==BPF_SOCKET_FILTER){
        error=test_packet_prepare(&bytes,&packet);if(error)goto release;
        bpf_runtime_packet_context(&context,&packet);
    }
    uint64_t before=kernel_time_monotonic_ns(),result=0;
    unsigned repeat=p->type==BPF_RAW_TRACEPOINT?1:a->test.repeat?a->test.repeat:1;
    for(unsigned i=0;i<repeat;i++){
        if(p->type==BPF_RAW_TRACEPOINT)
            result=bpf_runtime_run(p,task,data,(uint32_t)input,NULL,0);
        /* cb belongs to this skb and survives repeated executions. */
        else result=bpf_runtime_run(p,task,&context,sizeof(context),&packet,0);
    }
    uint32_t duration=(uint32_t)((kernel_time_monotonic_ns()-before)/repeat);
    uint32_t retval=(uint32_t)result;
    if(p->type==BPF_RAW_TRACEPOINT){
        error=bpf_user_copy(mm,user_attribute+offsetof(union bpf_attribute,test.result),&retval,sizeof(retval),0);
        goto release;
    }
    uint32_t required=(uint32_t)input,n=required;
    error=-KERNEL_EFAULT;
    if(a->test.data_out_size && n>a->test.data_out_size){n=a->test.data_out_size;error=-KERNEL_ENOSPC;}
    /* SOCKET_FILTER runs at L3. Linux restores a zero Ethernet prefix for
     * output after the program has finished using the original LL header. */
    memset(data,0,14);
    if(a->test.data_out && bpf_user_copy(mm,a->test.data_out,data,n,0))goto release;
    if(bpf_user_copy(mm,user_attribute+offsetof(union bpf_attribute,test.data_out_size),&required,sizeof(required),0))goto release;
    if(bpf_user_copy(mm,user_attribute+offsetof(union bpf_attribute,test.result),&retval,sizeof(retval),0))goto release;
    if(bpf_user_copy(mm,user_attribute+offsetof(union bpf_attribute,test.duration),&duration,sizeof(duration),0))goto release;
    if(error!=-KERNEL_ENOSPC)error=0;
release:if(data)kernel_heap_release(files->heap,data);
done:bpf_object_put(&p->object);return error;
}
