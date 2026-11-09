#include "private.h"
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/trace_events.h>
#include <string.h>

/* SHA-256 of the stable instruction representation, as in the fixed Linux
 * bpf_prog_calc_tag. No map descriptor or kernel address enters the digest. */
static uint32_t rotate(uint32_t x,unsigned n){return (x>>n)|(x<<(32-n));}
static void sha256_block(uint32_t h[8],const unsigned char bytes[64])
{
    static const uint32_t k[64]={
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    uint32_t w[64];
    for(unsigned i=0;i<16;i++)w[i]=(uint32_t)bytes[i*4]<<24|
        (uint32_t)bytes[i*4+1]<<16|(uint32_t)bytes[i*4+2]<<8|bytes[i*4+3];
    for(unsigned i=16;i<64;i++)w[i]=w[i-16]+(rotate(w[i-15],7)^rotate(w[i-15],18)^(w[i-15]>>3))+
        w[i-7]+(rotate(w[i-2],17)^rotate(w[i-2],19)^(w[i-2]>>10));
    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],v=h[7];
    for(unsigned i=0;i<64;i++){
        uint32_t t=v+(rotate(e,6)^rotate(e,11)^rotate(e,25))+((e&f)^(~e&g))+k[i]+w[i];
        uint32_t u=(rotate(a,2)^rotate(a,13)^rotate(a,22))+((a&b)^(a&c)^(b&c));
        v=g;g=f;f=e;e=d+t;d=c;c=b;b=a;a=t+u;
    }
    h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=v;
}
void bpf_program_calculate_tag(struct kernel_bpf_program *p)
{
    uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    unsigned char block[64]={0};unsigned used=0;int map=0;
    for(uint32_t pc=0;pc<p->count;pc++){
        struct kernel_bpf_insn insn=p->instructions[pc];
        if(!map && insn.code==0x18 && (insn.src_reg==1 || insn.src_reg==2)){
            map=1;insn.imm=0;
        }else if(map && !insn.code && !insn.dst_reg && !insn.src_reg && !insn.off){
            map=0;insn.imm=0;
        }else map=0;
        memcpy(block+used,&insn,8);used+=8;
        if(used==64){sha256_block(h,block);used=0;}
    }
    block[used++]=0x80;
    if(used>56){memset(block+used,0,64-used);sha256_block(h,block);used=0;}
    memset(block+used,0,56-used);
    uint64_t bits=(uint64_t)p->count*64;
    for(unsigned i=0;i<8;i++)block[63-i]=(unsigned char)(bits>>(i*8));
    sha256_block(h,block);
    for(unsigned i=0;i<8;i++)p->tag[i]=(unsigned char)(h[i/4]>>(24-8*(i%4)));
}
static int input(struct kernel_mm *mm,uint64_t address,uint32_t *length,void *buffer,size_t size)
{
    if(*length>BOAROS_PAGE_SIZE)return -KERNEL_E2BIG;
    for(size_t offset=size;offset<*length;){
        unsigned char tail[64];size_t n=*length-offset;if(n>64)n=64;
        int error=bpf_user_copy(mm,address+offset,tail,n,1);if(error)return error;
        for(size_t i=0;i<n;i++)if(tail[i])return -KERNEL_E2BIG;
        offset+=n;
    }
    if(*length>size)*length=(uint32_t)size;
    return *length?bpf_user_copy(mm,address,buffer,*length,1):0;
}
static int copy_bytes(struct kernel_mm *mm,uint64_t address,const void *bytes,size_t wanted,size_t available)
{
    size_t length=wanted<available?wanted:available;
    return length?bpf_user_copy(mm,address,(void *)bytes,length,0):0;
}
static int copy_name(struct kernel_mm *mm,uint64_t address,uint32_t capacity,const char *name)
{
    if(!address != !capacity)return -KERNEL_EINVAL;
    if(!capacity)return 0;
    size_t size=strlen(name)+1;
    if(capacity>=size)return bpf_user_copy(mm,address,(void *)name,size,0);
    int error=copy_bytes(mm,address,name,capacity-1,size);
    char zero=0;
    if(!error)error=bpf_user_copy(mm,address+capacity-1,&zero,1,0);
    return error?error:-KERNEL_ENOSPC;
}
static int program_info(struct kernel_mm *mm,struct kernel_bpf_program *p,uint64_t address,uint32_t *length)
{
    struct bpf_program_information in={0},out={0};
    /* verified_insns measures verifier work, which the VM does not expose. */
    int error=input(mm,address,length,&in,BPF_END(struct bpf_program_information,recursion_misses));
    if(error)return error;
    if(((in.func_count || in.func_record_size) && in.func_record_size!=8) ||
        ((in.line_count || in.line_record_size) && in.line_record_size!=16) ||
        ((in.jited_line_count || in.jited_line_record_size) && in.jited_line_record_size!=8))return -KERNEL_EINVAL;
    out.type=p->type;out.id=p->object.id;memcpy(out.tag,p->tag,8);
    memcpy(out.name,p->object.name,16);out.load_time=p->loaded_at;out.uid=p->object.uid;
    out.flags=p->gpl?1:0;out.map_count=p->map_count;out.map_ids=in.map_ids;
    for(uint32_t i=0;i<in.map_count && i<p->map_count;i++){
        uint32_t id=p->maps[i]->object.id;
        error=bpf_user_copy(mm,in.map_ids+(uint64_t)i*4,&id,4,0);if(error)return error;
    }
    out.btf_id=p->btf?p->btf->object.id:0;
    out.func_record_size=8;out.line_record_size=16;out.jited_line_record_size=8;
    out.run_count=__atomic_load_n(&p->run_count,__ATOMIC_RELAXED);
    out.run_time=__atomic_load_n(&p->run_time,__ATOMIC_RELAXED);
    out.recursion_misses=__atomic_load_n(&p->recursion_misses,__ATOMIC_RELAXED);
    const struct kernel_credentials *credentials=kernel_credentials_current();
    if(kernel_credentials_capable(credentials,KERNEL_CAP_BPF) ||
        kernel_credentials_capable(credentials,KERNEL_CAP_SYS_ADMIN)){
        out.translated_length=p->count*8;out.translated=in.translated;
        uint32_t bytes=in.translated_length<out.translated_length?in.translated_length:out.translated_length;
        uint32_t binding=0;
        for(uint32_t pc=0;pc*8<bytes;pc++){
            struct kernel_bpf_insn insn=p->instructions[pc];
            if(binding<p->binding_count && p->bindings[binding].pc==pc){
                insn.imm=(int32_t)p->bindings[binding++].map->object.id;
                if(insn.src_reg==5)insn.src_reg=1;
                if(insn.src_reg==6)insn.src_reg=2;
            }
            error=copy_bytes(mm,in.translated+(uint64_t)pc*8,&insn,bytes-pc*8,8);if(error)return error;
        }
        out.funcs=in.funcs;out.func_count=p->function_count;
        out.lines=in.lines;out.line_count=p->line_count;
        error=copy_bytes(mm,in.funcs,p->function_info,(size_t)in.func_count*8,(size_t)p->function_count*8);
        if(error)return error;
        error=copy_bytes(mm,in.lines,p->line_info,(size_t)in.line_count*16,(size_t)p->line_count*16);
        if(error)return error;
        out.tag_count=1;out.tags=in.tags;
        if(in.tag_count){error=bpf_user_copy(mm,in.tags,p->tag,8,0);if(error)return error;}
    }
    return *length?bpf_user_copy(mm,address,&out,*length,0):0;
}
int bpf_object_information(struct kernel_mm *mm,struct bpf_object *object,uint64_t address,uint32_t *length)
{
    int error;
    switch(object->kind){
    case BPF_OBJECT_PROGRAM:return program_info(mm,(void *)object,address,length);
    case BPF_OBJECT_MAP:{
        struct bpf_map_information info={0};
        error=input(mm,address,length,&info,sizeof(info));if(error)return error;
        memset(&info,0,sizeof(info));struct bpf_map_object *map=(void *)object;
        const struct bpf_map_spec *spec=bpf_map_spec(map->storage);
        info.type=spec->type;info.id=object->id;info.key_size=spec->key_size;info.value_size=spec->value_size;
        info.max_entries=spec->max_entries;info.flags=spec->map_flags;memcpy(info.name,object->name,16);
        info.btf_id=map->btf?map->btf->object.id:0;info.btf_key_type_id=map->key_type;
        info.btf_value_type_id=map->value_type;info.btf_vmlinux_value_type_id=map->vmlinux_type;
        return *length?bpf_user_copy(mm,address,&info,*length,0):0;
    }
    case BPF_OBJECT_BTF:{
        struct bpf_btf_information info={0};struct bpf_btf *btf=(void *)object;
        error=input(mm,address,length,&info,sizeof(info));if(error)return error;
        error=copy_bytes(mm,info.data,btf->data,info.size,btf->size);if(error)return error;
        info.id=object->id;info.size=btf->size;info.kernel=btf->kernel;
        error=copy_name(mm,info.name,info.name_length,object->name);
        if(error && error!=-KERNEL_ENOSPC)return error;
        info.name_length=(uint32_t)strlen(object->name);
        int copied=*length?bpf_user_copy(mm,address,&info,*length,0):0;
        return copied?copied:error;
    }
    case BPF_OBJECT_LINK:{
        struct bpf_link_information in={0},out={0};struct bpf_link *link=(void *)object;
        /* perf_event.tracepoint ends at offset 48 of the Linux union. */
        error=input(mm,address,length,&in,48);if(error)return error;
        const char *name=link->event->event==KERNEL_TRACE_SYS_ENTER?"sys_enter":"sys_exit";
        error=copy_name(mm,in.payload[1],(uint32_t)in.payload[2],name);if(error)return error;
        out.type=link->type;out.id=object->id;out.program_id=link->program->object.id;
        out.payload[0]=5;out.payload[1]=in.payload[1];out.payload[2]=strlen(name)+1;
        out.payload[3]=link->cookie;
        return *length?bpf_user_copy(mm,address,&out,*length,0):0;
    }
    default:return -KERNEL_EINVAL;
    }
}
struct text_output {char *buffer;size_t capacity,length;};
static void text(struct text_output *out,const char *value)
{
    while(*value){if(out->length<out->capacity)out->buffer[out->length]=*value;out->length++;value++;}
}
static void number(struct text_output *out,uint64_t value,unsigned base)
{
    char digits[32];unsigned count=0;
    do{digits[count++]="0123456789abcdef"[value%base];value/=base;}while(value);
    while(count){char s[2]={digits[--count],0};text(out,s);}
}
static void field(struct text_output *out,const char *key,uint64_t value)
{text(out,key);text(out,":\t");number(out,value,10);text(out,"\n");}
int kernel_bpf_fdinfo(const struct kernel_open_file_description *file,char *buffer,size_t capacity,size_t *length)
{
    struct bpf_object *object=bpf_object_from_file(file);if(!object)return -KERNEL_EINVAL;
    struct text_output out={buffer,capacity,0};
    switch(object->kind){
    case BPF_OBJECT_PROGRAM:{
        struct kernel_bpf_program *p=(void *)object;
        field(&out,"prog_type",p->type);field(&out,"prog_jited",0);text(&out,"prog_tag:\t");
        for(unsigned i=0;i<8;i++){
            char hex[3]={"0123456789abcdef"[p->tag[i]>>4],"0123456789abcdef"[p->tag[i]&15],0};text(&out,hex);
        }
        text(&out,"\n");field(&out,"prog_id",object->id);
        field(&out,"run_time_ns",__atomic_load_n(&p->run_time,__ATOMIC_RELAXED));
        field(&out,"run_cnt",__atomic_load_n(&p->run_count,__ATOMIC_RELAXED));
        field(&out,"recursion_misses",__atomic_load_n(&p->recursion_misses,__ATOMIC_RELAXED));break;
    }
    case BPF_OBJECT_MAP:{
        struct bpf_map_object *map=(void *)object;const struct bpf_map_spec *s=bpf_map_spec(map->storage);
        field(&out,"map_type",s->type);field(&out,"key_size",s->key_size);field(&out,"value_size",s->value_size);
        field(&out,"max_entries",s->max_entries);text(&out,"map_flags:\t0x");number(&out,s->map_flags,16);
        text(&out,"\n");field(&out,"map_id",object->id);field(&out,"frozen",bpf_map_is_frozen(map->storage));break;
    }
    case BPF_OBJECT_BTF:field(&out,"btf_id",object->id);break;
    case BPF_OBJECT_LINK:{
        struct bpf_link *link=(void *)object;text(&out,"link_type:\tperf_event\n");field(&out,"link_id",object->id);
        text(&out,"prog_tag:\t");
        for(unsigned i=0;i<8;i++){
            unsigned char byte=link->program->tag[i];
            char hex[3]={"0123456789abcdef"[byte>>4],"0123456789abcdef"[byte&15],0};text(&out,hex);
        }
        text(&out,"\n");field(&out,"prog_id",link->program->object.id);text(&out,"event_type:\ttracepoint\n");
        text(&out,"tp_name:\t");text(&out,link->event->event==KERNEL_TRACE_SYS_ENTER?"sys_enter":"sys_exit");
        text(&out,"\n");field(&out,"cookie",link->cookie);break;
    }
    case BPF_OBJECT_PERF:{
        struct bpf_perf *event=(void *)object;
        field(&out,"perf_event_attr.type",2);field(&out,"pmu_type",2);
        text(&out,"perf_event_attr.config:\t0x");number(&out,event->event,16);text(&out,"\n");
        text(&out,"perf_event_attr.config1:\t0x0\nperf_event_attr.config2:\t0x0\n"
            "perf_event_attr.config3:\t0x0\nperf_event_attr.config4:\t0x0\n");break;
    }
    default:__builtin_trap();
    }
    *length=out.length;return out.length>capacity?-KERNEL_ENOSPC:0;
}
