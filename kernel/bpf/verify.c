#include <kernel/bpf_vm.h>
#include <kernel/errno.h>

uint64_t kernel_bpf_vm_byteswap(uint64_t,unsigned);
uint64_t kernel_bpf_vm_alu(uint8_t,int16_t,int32_t,uint64_t,uint64_t);
int kernel_bpf_vm_condition(uint8_t,uint64_t,uint64_t);
enum value_type { UNINIT, SCALAR, CONTEXT, STACK, MAP, VALUE, VALUE_NULL,
                  PACKET, PACKET_END, RING, RING_NULL, DEAD };
struct value {
    union { uint64_t min; int64_t low; };
    union { uint64_t max; int64_t high; };
    uint32_t id, map_pc, size;
    uint16_t flags;
    uint8_t type, frame;
};
struct frame {
    struct value saved[4], spill[KERNEL_BPF_STACK_SIZE/8];
    uint8_t initialized[KERNEL_BPF_STACK_SIZE/8];
    uint8_t known[KERNEL_BPF_STACK_SIZE/8];
    uint8_t bytes[KERNEL_BPF_STACK_SIZE];
    uint32_t return_pc, entry, stack_id;
};
struct state {
    struct state *next;
    struct value r[11];
    struct frame frame[KERNEL_BPF_MAX_FRAMES];
    uint32_t pc, depth, serial, packet_bound;
    uint32_t reservations[64];
};
struct verifier {
    const struct kernel_bpf_vm_env *env;
    const struct kernel_bpf_insn *insns;
    uint32_t count, processed;
    uint8_t *continuation;
    const char *error;
    int errno_value;
};
static void clear(void *data,size_t size)
{uint8_t*p=data;while(size--)*p++=0;}
static void copy(void *to,const void *from,size_t size)
{uint8_t*d=to;const uint8_t*s=from;while(size--)*d++=*s++;}
static void log_text(struct verifier *v,const char *s)
{size_t n=0;while(s[n])n++;if(v->env->log)v->env->log(v->env->opaque,s,n);}
static void log_number(struct verifier*v,uint32_t n)
{char text[11];unsigned p=sizeof(text);do{text[--p]='0'+n%10;n/=10;}while(n);if(v->env->log)v->env->log(v->env->opaque,text+p,sizeof(text)-p);}
static int fail(struct verifier*v,const char*s)
{v->error=s;v->errno_value=-KERNEL_EACCES;return 0;}
static struct value scalar(uint64_t min,uint64_t max)
{return(struct value){.min=min,.max=max,.type=SCALAR};}
static int exact(struct value v){return v.type==SCALAR&&v.min==v.max;}
static unsigned width(uint8_t code)
{static const uint8_t n[4]={4,2,1,8};return n[(code>>3)&3];}
static int target(struct verifier*v,int64_t pc)
{return pc>=0 && pc<v->count && !v->continuation[pc];}
static int syntax(struct verifier*v)
{
    for(uint32_t pc=0;pc<v->count;pc++) {
        const struct kernel_bpf_insn*i=&v->insns[pc];
        if(i->code==0x18) {
            if(pc+1==v->count)return fail(v,"incomplete LD_IMM64 instruction");
            const struct kernel_bpf_insn*tail=&v->insns[++pc];
            if(tail->code||tail->dst_reg||tail->src_reg||tail->off)
                return fail(v,"invalid LD_IMM64 continuation");
            v->continuation[pc]=1;
        }
    }
    for(uint32_t pc=0;pc<v->count;pc++) {
        if(v->continuation[pc])continue;
        const struct kernel_bpf_insn*i=&v->insns[pc];
        unsigned c=i->code&7,op=i->code&0xf0,mode=i->code&0xe0;
        if(i->dst_reg>10||i->src_reg>10)return fail(v,"invalid register");
        if(c==4||c==7) {
            if(i->dst_reg==10 || op>0xd0)return fail(v,"invalid ALU destination or opcode");
            if(op==0xd0) {
                if(i->src_reg||i->off||(i->imm!=16&&i->imm!=32&&i->imm!=64)||
                   (c==7&&(i->code&8)))return fail(v,"invalid endian instruction");
            } else {
                if((i->code&8) ? i->imm!=0 : i->src_reg!=0)return fail(v,"reserved ALU field");
                if(op==0x80 && ((i->code&8)||i->imm))return fail(v,"invalid negation");
                if(i->off && !((op==0x30||op==0x90)&&i->off==1) &&
                   !(op==0xb0&&(i->code&8)&&(i->off==8||i->off==16||(c==7&&i->off==32))))
                    return fail(v,"reserved ALU offset");
                if(!(i->code&8)&&(op==0x30||op==0x90)&&!i->imm)return fail(v,"division by zero");
                if(!(i->code&8)&&(op==0x60||op==0x70||op==0xc0)&&
                   (i->imm<0||i->imm>=(c==4?32:64)))return fail(v,"invalid shift");
            }
        } else if(i->code==0x18) {
            if(i->dst_reg==10||i->off||!(i->src_reg==0||i->src_reg==1||i->src_reg==2||i->src_reg==5||i->src_reg==6))
                return fail(v,"invalid LD_IMM64 fields");
            if((i->src_reg==1||i->src_reg==5)&&v->insns[pc+1].imm)return fail(v,"invalid map immediate");
        } else if(c==0) {
            if(v->env->program_type!=1 || i->off ||
               !((mode==0x20||mode==0x40)&&width(i->code)!=8) ) {
                if(!(v->env->flags&KERNEL_BPF_VM_CLASSIC)||i->code!=0x80||i->off||i->src_reg||i->imm)
                    return fail(v,"invalid packet load");
            }
            if(!(v->env->flags&KERNEL_BPF_VM_CLASSIC)&&i->dst_reg)
                return fail(v,"packet load must target R0");
            if(i->dst_reg==10||(mode!=0x40&&i->src_reg))return fail(v,"invalid packet load registers");
        } else if(c==1||c==2||c==3) {
            if(c==1) {
                if(i->dst_reg==10||i->imm||!(mode==0x60||(mode==0x80&&width(i->code)!=8)))
                    return fail(v,"invalid memory load");
            } else if(mode==0xc0&&c==3) {
                if(width(i->code)!=4&&width(i->code)!=8)return fail(v,"invalid atomic width");
                if(!(i->imm==0x00||i->imm==0x01||i->imm==0x40||i->imm==0x41||i->imm==0x50||i->imm==0x51||
                     i->imm==0xa0||i->imm==0xa1||i->imm==0xe1||i->imm==0xf1))return fail(v,"invalid atomic operation");
                if((i->imm&1)&&i->imm!=0xf1&&i->src_reg==10)return fail(v,"atomic writes frame pointer");
            } else if(mode!=0x60 || (c==2&&i->src_reg)||(c==3&&i->imm))return fail(v,"invalid memory store");
        } else if(c==5||c==6) {
            if(op==0x80) {
                if(c!=5||i->dst_reg||i->off||(i->src_reg!=0&&i->src_reg!=1))return fail(v,"invalid call instruction");
                if(i->src_reg==1&&!target(v,(int64_t)pc+1+i->imm))return fail(v,"invalid subprogram target");
            } else if(op==0x90) {
                if(c!=5||i->dst_reg||i->src_reg||i->off||i->imm)return fail(v,"invalid exit instruction");
            } else if(op==0) {
                if((i->code&8)||i->dst_reg||i->src_reg||(c==5?i->imm:i->off)||
                   !target(v,(int64_t)pc+1+(c==5?i->off:i->imm)))return fail(v,"invalid jump target");
            } else {
                if(!(op==0x10||op==0x20||op==0x30||op==0x40||op==0x50||op==0x60||op==0x70||op==0xa0||op==0xb0||op==0xc0||op==0xd0)||
                   ((i->code&8)?i->imm!=0:i->src_reg!=0)||!target(v,(int64_t)pc+1+i->off))
                    return fail(v,"invalid conditional jump");
            }
        } else return fail(v,"unknown opcode");
    }
    return 1;
}
static int bit(const uint8_t *bits,unsigned n){return(bits[n/8]>>(n%8))&1;}
static void setbit(uint8_t *bits,unsigned n,int value)
{if(value)bits[n/8]|=1u<<(n%8);else bits[n/8]&=~(1u<<(n%8));}
enum memory_operation { MEMORY_READ, MEMORY_WRITE, MEMORY_CHECK_WRITE };
static int memory(struct verifier*v,struct state*s,struct value ptr,int32_t off,
                  uint32_t size,enum memory_operation operation,
                  struct value *data,int atomic)
{
    int write=operation!=MEMORY_READ;
    if(ptr.type==UNINIT)return fail(v,"uninitialized address register");
    if(ptr.type==VALUE_NULL||ptr.type==RING_NULL)return fail(v,"invalid mem access through nullable pointer");
    if(ptr.type==DEAD)return fail(v,"use after ringbuf release");
    if(ptr.type!=STACK&&ptr.type!=CONTEXT&&ptr.type!=VALUE&&ptr.type!=RING&&ptr.type!=PACKET)
        return fail(v,"invalid mem access through scalar or map pointer");
    if(ptr.low<INT32_MIN || ptr.high>INT32_MAX)return fail(v,"unbounded pointer offset");
    int64_t low=ptr.low+off,high=ptr.high+off;
    if(atomic&&(low!=high||((uint64_t)low&(size-1))))return fail(v,"misaligned atomic access");
    if(ptr.type==CONTEXT) {
        struct kernel_bpf_vm_context_access a={0};
        if(low!=high||atomic||!v->env->context||v->env->context(v->env->opaque,(int32_t)low,size,write,&a))
            return fail(v,"invalid bpf_context access");
        if(write) {
            if(data&&data->type!=SCALAR)return fail(v,"pointer leak into context");
        } else if(data) {
            if(a.kind==KERNEL_BPF_CONTEXT_PACKET)*data=(struct value){.type=PACKET,.id=1};
            else if(a.kind==KERNEL_BPF_CONTEXT_PACKET_END)*data=(struct value){.type=PACKET_END,.id=1};
            else *data=scalar(a.min,a.min||a.max?a.max:(size==8?UINT64_MAX:((1ull<<(size*8))-1)));
        }
        return 1;
    }
    if(ptr.type==STACK) {
        if(low<-(int64_t)KERNEL_BPF_STACK_SIZE||high>-(int64_t)size||ptr.frame>s->depth)
            return fail(v,"invalid stack access");
        if(low!=high)return fail(v,"variable stack access is not bounded to one location");
        struct frame*f=&s->frame[ptr.frame];
        unsigned index=KERNEL_BPF_STACK_SIZE+low;
        if(!write||atomic)for(unsigned j=0;j<size;j++)
            if(!bit(f->initialized,index+j))return fail(v,"invalid read from uninitialized stack");
        if(!write) {
            if(!data) {
                for(unsigned j=0;j<size;j++)
                    if(f->spill[(index+j)/8].type&&f->spill[(index+j)/8].type!=SCALAR)
                        return fail(v,"helper read exposes spilled pointer");
                return 1;
            }
            if(size==8&&!(index&7)&&f->spill[index/8].type) {
                *data=f->spill[index/8];return 1;
            }
            uint64_t n=0;int known=1;
            for(unsigned j=0;j<size;j++) {
                if(f->spill[(index+j)/8].type && f->spill[(index+j)/8].type!=SCALAR)
                    return fail(v,"partial read of spilled pointer");
                known&=bit(f->known,index+j);
                if(size<=8)n|=(uint64_t)f->bytes[index+j]<<(8*j);
            }
            *data=known&&size<=8?scalar(n,n):scalar(0,size<8?((1ull<<(8*size))-1):UINT64_MAX);
        } else {
            if(data&&data->type!=SCALAR && (size!=8||(index&7)))return fail(v,"partial pointer spill");
            if(data&&data->type==STACK&&data->frame>ptr.frame)
                return fail(v,"callee stack pointer escapes into caller frame");
            if(operation==MEMORY_CHECK_WRITE)return 1;
            for(unsigned j=index/8;j<=(index+size-1)/8;j++) {
                if(f->spill[j].type&&f->spill[j].type!=SCALAR) {
                    if(atomic)return fail(v,"atomic operation exposes spilled pointer");
                    /* Bytes left from a pointer are not scalar data after a
                     * partial overwrite; every exposed byte must be replaced. */
                    f->initialized[j]=0;f->known[j]=0;
                }
                f->spill[j]=(struct value){0};
            }
            for(unsigned j=0;j<size;j++) {
                setbit(f->initialized,index+j,1);
                setbit(f->known,index+j,data&&exact(*data)&&size<=8&&!atomic);
                if(data&&size<=8)f->bytes[index+j]=(uint8_t)(data->min>>(8*j));
            }
            if(data&&size==8&&!(index&7)&&!atomic)f->spill[index/8]=*data;
        }
        return 1;
    }
    uint32_t bound=ptr.type==PACKET?s->packet_bound:ptr.size;
    if(low<0||high>bound||(uint64_t)size>bound-(uint64_t)high)return fail(v,"invalid access beyond memory bounds");
    if(write&&(ptr.type==PACKET||(ptr.flags&KERNEL_BPF_MAP_READONLY)))return fail(v,"write into read-only memory");
    if((!write||atomic)&&(ptr.flags&KERNEL_BPF_MAP_WRITEONLY))return fail(v,"read from write-only map");
    if(write&&data&&data->type!=SCALAR)return fail(v,"pointer leak into map or ringbuf");
    if(!write&&data){
        if(ptr.type==VALUE && low==high && (ptr.flags&KERNEL_BPF_MAP_CONSTANT_VALUE)){
            struct kernel_bpf_vm_map map={0};
            if(!v->env->map || v->env->map(v->env->opaque,ptr.map_pc,&v->insns[ptr.map_pc],&map) ||
                !map.value || !(map.flags&KERNEL_BPF_MAP_CONSTANT_VALUE))
                return fail(v,"invalid immutable map value");
            const uint8_t *bytes=(const uint8_t *)map.value+low;
            uint64_t n=0;for(unsigned j=0;j<size;j++)n|=(uint64_t)bytes[j]<<(8*j);
            *data=scalar(n,n);
        }else *data=scalar(0,size<8?((1ull<<(size*8))-1):UINT64_MAX);
    }
    return 1;
}
static int get_map(struct verifier*v,struct value r,struct kernel_bpf_vm_map*m)
{
    if(r.type!=MAP)return fail(v,"helper argument expected map pointer");
    if(!v->env->map||v->env->map(v->env->opaque,r.map_pc,&v->insns[r.map_pc],m))return fail(v,"invalid map relocation");
    return 1;
}
static void aliases(struct state*s,uint32_t id,int null,int dead)
{
    for(unsigned group=0;group<=s->depth+1;group++) {
        struct value*p=group? s->frame[group-1].spill:s->r;
        unsigned n=group?KERNEL_BPF_STACK_SIZE/8:11;
        for(unsigned j=0;j<n;j++)if(p[j].id==id &&
            (p[j].type==VALUE_NULL||p[j].type==RING_NULL||p[j].type==RING)) {
            if(dead)p[j].type=DEAD;
            else if(null)p[j]=scalar(0,0);
            else p[j].type=p[j].type==VALUE_NULL?VALUE:RING;
        }
        if(group)for(unsigned j=0;j<4;j++) {
            struct value*q=&s->frame[group-1].saved[j];
            if(q->id==id&&(q->type==VALUE_NULL||q->type==RING_NULL||q->type==RING)) {
                if(dead)q->type=DEAD;else if(null)*q=scalar(0,0);
                else q->type=q->type==VALUE_NULL?VALUE:RING;
            }
        }
    }
    if(null||dead)for(unsigned j=0;j<64;j++)if(s->reservations[j]==id)s->reservations[j]=0;
}
static int helper_call(struct verifier*v,struct state*s,uint32_t id)
{
    struct kernel_bpf_vm_helper h={0};
    int err=v->env->helper?v->env->helper(v->env->opaque,id,&h):-KERNEL_ENOENT;
    if(err) {
        if(err==-KERNEL_EACCES)return fail(v,"program of this type cannot use helper");
        log_text(v,"unknown func ");log_number(v,id);log_text(v,"\n");
        return fail(v,"invalid func unknown helper");
    }
    struct kernel_bpf_vm_map map={0};struct value map_reg={0};
    uint32_t output_sizes[5]={0};
    if(h.map_arg) {
        if(h.map_arg>5)return fail(v,"invalid helper prototype");
        map_reg=s->r[h.map_arg];if(!get_map(v,map_reg,&map))return 0;
        if(h.map_types&&(map.type>=64||!(h.map_types&(1ull<<map.type))))
            return fail(v,"helper is not compatible with map type");
        if((h.map_write&&(map.flags&KERNEL_BPF_MAP_READONLY))||
           (h.map_read&&(map.flags&KERNEL_BPF_MAP_WRITEONLY)))
            return fail(v,"helper violates map access flags");
    }
    for(unsigned a=0;a<5;a++) {
        struct kernel_bpf_vm_arg desc=h.args[a];struct value r=s->r[a+1];
        if(desc.kind==KERNEL_BPF_ARG_NONE)continue;
        if(r.type==UNINIT||r.type==DEAD)return fail(v,"uninitialized helper argument");
        if(desc.kind==KERNEL_BPF_ARG_ANY)continue;
        if(desc.kind==KERNEL_BPF_ARG_MAP) {struct kernel_bpf_vm_map m;if(!get_map(v,r,&m))return 0;continue;}
        if(desc.kind==KERNEL_BPF_ARG_CONTEXT) {if(r.type!=CONTEXT||r.low||r.high)return fail(v,"helper expected context");continue;}
        if(desc.kind==KERNEL_BPF_ARG_SCALAR||desc.kind==KERNEL_BPF_ARG_CONST) {
            if(r.type!=SCALAR||(desc.kind==KERNEL_BPF_ARG_CONST&&!exact(r)))return fail(v,"helper expected scalar argument");
            continue;
        }
        if(desc.kind==KERNEL_BPF_ARG_RINGBUF) {
            if(r.type!=RING||r.low||r.high)return fail(v,"helper expected ringbuf reservation");
            continue;
        }
        uint32_t n=desc.fixed_size;
        if(desc.kind==KERNEL_BPF_ARG_MAP_KEY)n=map.key_size;
        else if(desc.kind==KERNEL_BPF_ARG_MAP_VALUE)n=map.value_size;
        else if(desc.size_arg) {
            if(desc.size_arg>5||s->r[desc.size_arg].type!=SCALAR||s->r[desc.size_arg].max>UINT32_MAX)
                return fail(v,"unbounded helper memory size");
            if(desc.kind==KERNEL_BPF_ARG_MEM_WRITE&&!exact(s->r[desc.size_arg]))
                return fail(v,"helper write size must be constant");
            n=(uint32_t)s->r[desc.size_arg].max;
        }
        if(!n) {
            if(exact(r)&&!r.min)continue;
            /* A zero-byte operation still needs a genuine memory pointer. */
            if(r.type==STACK||r.type==VALUE||r.type==RING||r.type==PACKET)continue;
            return fail(v,"invalid zero-size helper pointer");
        }
        enum memory_operation operation=desc.kind==KERNEL_BPF_ARG_MEM_WRITE?
            MEMORY_CHECK_WRITE:MEMORY_READ;
        if(!memory(v,s,r,0,n,operation,0,0))return 0;
        if(operation==MEMORY_CHECK_WRITE)output_sizes[a]=n;
    }
    struct value result=scalar(0,UINT64_MAX);
    if(h.result==KERNEL_BPF_RETURN_MAP_VALUE_OR_NULL) {
        if(!h.map_arg)return fail(v,"invalid map return prototype");
        result=(struct value){.type=VALUE_NULL,.id=++s->serial,.map_pc=map_reg.map_pc,.size=map.value_size,.flags=(uint16_t)map.flags};
    } else if(h.result==KERNEL_BPF_RETURN_RINGBUF_OR_NULL) {
        uint32_t n=h.fixed_size;
        if(h.size_arg) {
            if(h.size_arg>5||!exact(s->r[h.size_arg])||s->r[h.size_arg].max>UINT32_MAX)
                return fail(v,"ringbuf size must be constant");
            n=(uint32_t)s->r[h.size_arg].max;
        }
        result=(struct value){.type=RING_NULL,.id=++s->serial,.size=n};
        unsigned j;for(j=0;j<64&&s->reservations[j];j++);
        if(j==64)return fail(v,"too many live ringbuf reservations");
        s->reservations[j]=result.id;
    }
    /* Output aliases cannot make pre-call inputs initialized or erase their
     * pointer types. Publish writes only after every argument is validated. */
    for(unsigned a=0;a<5;a++)if(output_sizes[a])
        if(!memory(v,s,s->r[a+1],0,output_sizes[a],MEMORY_WRITE,0,0))return 0;
    if(h.releases_ringbuf)for(unsigned a=0;a<5;a++)
        if(h.args[a].kind==KERNEL_BPF_ARG_RINGBUF)aliases(s,s->r[a+1].id,0,1);
    for(unsigned j=1;j<=5;j++)s->r[j]=(struct value){0};
    s->r[0]=result;
    return 1;
}
static int alu(struct verifier*v,struct state*s,const struct kernel_bpf_insn*i)
{
    struct value *d=&s->r[i->dst_reg];
    unsigned op=i->code&0xf0,c=i->code&7;
    struct value b=(i->code&8)?s->r[i->src_reg]:scalar((uint64_t)(int64_t)i->imm,(uint64_t)(int64_t)i->imm);
    if(op==0xd0) {
        if(d->type!=SCALAR)return fail(v,"endian conversion requires scalar");
        if(exact(*d)) {
            uint64_t n=d->min;
            if(i->imm==64) {
                int swap=c==7||(i->code&8);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
                if(c==4)swap=!swap;
#endif
                n=swap?kernel_bpf_vm_byteswap(n,8):n;
            } else n=kernel_bpf_vm_alu(i->code,i->off,i->imm,n,0);
            *d=scalar(n,n);
        } else *d=scalar(0,i->imm==64?UINT64_MAX:((1ull<<i->imm)-1));
        return 1;
    }
    if(op==0x80)b=scalar(0,0);
    if(b.type==UNINIT||b.type==DEAD||(op!=0xb0&&(d->type==UNINIT||d->type==DEAD)))
        return fail(v,"uninitialized ALU register");
    if(op==0xb0) {
        if(c==7&&!i->off){*d=b;return 1;}
        if(b.type!=SCALAR)return fail(v,"32-bit or sign-extending move of pointer");
        if(exact(b)){uint64_t n=kernel_bpf_vm_alu(i->code,i->off,i->imm,0,b.min);*d=scalar(n,n);}
        else *d=scalar(0,c==4?UINT32_MAX:UINT64_MAX);
        return 1;
    }
    if(d->type!=SCALAR||b.type!=SCALAR) {
        if(c!=7||(op!=0x00&&op!=0x10))return fail(v,"invalid pointer arithmetic");
        if(op==0x10&&d->type==b.type&&(d->type==STACK||d->type==VALUE||d->type==PACKET)&&
           d->id==b.id&&d->frame==b.frame&&d->low==d->high&&b.low==b.high) {
            uint64_t n=(uint64_t)d->low-(uint64_t)b.low;*d=scalar(n,n);return 1;
        }
        if(b.type!=SCALAR||!(d->type==STACK||d->type==VALUE||d->type==RING||d->type==PACKET||d->type==CONTEXT))
            return fail(v,"arithmetic on nullable or invalid pointer");
        int64_t low,high;
        if(exact(b)){low=(int64_t)b.min;high=low;}
        else if(b.max<=INT32_MAX){low=(int64_t)b.min;high=(int64_t)b.max;}
        else return fail(v,"unbounded pointer arithmetic");
        if(low<INT32_MIN||high>INT32_MAX||d->low<INT32_MIN||d->high>INT32_MAX)
            return fail(v,"pointer offset overflow");
        if(op==0x00){d->low+=low;d->high+=high;}
        else {d->low-=high;d->high-=low;}
        if(d->low<INT32_MIN||d->high>INT32_MAX)return fail(v,"pointer offset overflow");
        return 1;
    }
    uint64_t mask=c==4?UINT32_MAX:UINT64_MAX;
    if(exact(*d)&&exact(b)) {
        uint64_t n=kernel_bpf_vm_alu(i->code,i->off,i->imm,d->min,b.min);*d=scalar(n,n);return 1;
    }
    if((i->code&8)&&i->dst_reg==i->src_reg&&(op==0xa0||op==0x10)){*d=scalar(0,0);return 1;}
    if(c==4&&(d->max>UINT32_MAX||b.max>UINT32_MAX)) {
        if(d->max>UINT32_MAX)*d=scalar(0,UINT32_MAX);
        if(b.max>UINT32_MAX)b=exact(b)?scalar((uint32_t)b.min,(uint32_t)b.min):scalar(0,UINT32_MAX);
    }
    if(op==0x00&&d->max<=mask-b.max){d->min+=b.min;d->max+=b.max;}
    else if(op==0x10&&d->min>=b.max){d->min-=b.max;d->max-=b.min;}
    else if(op==0x20&&b.max&&d->max<=mask/b.max){d->min*=b.min;d->max*=b.max;}
    else if(op==0x50)*d=scalar(0,d->max<b.max?d->max:b.max);
    else if(op==0x70&&exact(b))*d=scalar(d->min>>(b.min&(c==4?31:63)),d->max>>(b.min&(c==4?31:63)));
    else if(op==0x60&&exact(b)&&d->max<=(mask>>(b.min&(c==4?31:63)))) {
        unsigned shift=b.min&(c==4?31:63);d->min<<=shift;d->max<<=shift;
    } else if(op==0x30&&!i->off&&b.min)*d=scalar(d->min/b.max,d->max/b.min);
    else if(op==0x90&&!i->off&&b.min)*d=scalar(0,b.max-1);
    else *d=scalar(0,mask);
    return 1;
}
/* Restrict an unsigned interval, preserving an over-approximation when signed
 * bounds or a 32-bit comparison split its representable interval. */
static int refine(struct value*r,uint8_t code,uint64_t n,int truth)
{
    unsigned op=code&0xf0;
    if((code&7)==6) {
        n=(uint32_t)n;
        if(r->max>UINT32_MAX)return 1;
    }
    if(exact(*r))return kernel_bpf_vm_condition(code,r->min,n)==truth;
    if(!truth) {
        switch(op) {
        case 0x10:op=0x50;break;case 0x50:op=0x10;break;
        case 0x20:op=0xb0;break;case 0x30:op=0xa0;break;
        case 0xa0:op=0x30;break;case 0xb0:op=0x20;break;
        case 0x60:op=0xd0;break;case 0x70:op=0xc0;break;
        case 0xc0:op=0x70;break;case 0xd0:op=0x60;break;
        default:return 1;
        }
    }
    if(op==0x60||op==0x70||op==0xc0||op==0xd0) {
        uint64_t sign=(code&7)==6?1ull<<31:1ull<<63;
        uint64_t mask=(code&7)==6?UINT32_MAX:UINT64_MAX;
        /* A signed interval is contiguous in unsigned order within each half.
         * Intersect each half and retain their enclosing unsigned interval. */
        uint64_t lo=0,hi=mask;
        if(op==0x60||op==0x70) {
            if((n&sign)==0){lo=n+(op==0x60);hi=sign-1;if(lo>hi)return 0;}
            else {
                uint64_t neglo=n+(op==0x60);
                if(neglo && r->max>=neglo) {
                    if(r->min>=sign&&r->min<neglo)r->min=neglo;
                    return r->min<=r->max;
                }
                hi=sign-1;
            }
        } else {
            if(n&sign){lo=sign;hi=n-(op==0xc0);if(hi<lo)return 0;}
            else {
                if(r->max>=sign) {
                    if(r->min<sign&&r->min>n-(op==0xc0))r->min=sign;
                    return r->min<=r->max;
                }
                if(op==0xc0&&!n)return 0;
                hi=n-(op==0xc0);
            }
        }
        if(r->min<lo)r->min=lo;
        if(r->max>hi)r->max=hi;
        return r->min<=r->max;
    }
    switch(op) {
    case 0x10:if(n<r->min||n>r->max)return 0;r->min=r->max=n;break;
    case 0x50:
        if(r->min==n){if(n==UINT64_MAX)return 0;r->min++;}
        if(r->max==n){if(!n)return 0;r->max--;}
        break;
    case 0x20:if(n==UINT64_MAX)return 0;if(r->min<=n)r->min=n+1;break;
    case 0x30:if(r->min<n)r->min=n;break;
    case 0xa0:if(!n)return 0;if(r->max>=n)r->max=n-1;break;
    case 0xb0:if(r->max>n)r->max=n;break;
    case 0x40:if(!n)return 0;break;
    }
    return r->min<=r->max;
}
static int branch(struct verifier*v,struct state*s,const struct kernel_bpf_insn*i,int truth)
{
    struct value *a=&s->r[i->dst_reg];
    struct value b=(i->code&8)?s->r[i->src_reg]:scalar((uint64_t)(int64_t)i->imm,(uint64_t)(int64_t)i->imm);
    unsigned op=i->code&0xf0;
    if(a->type==UNINIT||b.type==UNINIT||a->type==DEAD||b.type==DEAD)return -!fail(v,"uninitialized branch register");
    if(a->type==SCALAR&&b.type==SCALAR) {
        if(exact(b))return refine(a,i->code,b.min,truth);
        if(exact(*a)&&(i->code&8)) {
            unsigned reverse=op;
            if(op==0x20)reverse=0xa0;else if(op==0x30)reverse=0xb0;
            else if(op==0xa0)reverse=0x20;else if(op==0xb0)reverse=0x30;
            else if(op==0x60)reverse=0xc0;else if(op==0x70)reverse=0xd0;
            else if(op==0xc0)reverse=0x60;else if(op==0xd0)reverse=0x70;
            return refine(&s->r[i->src_reg],(i->code&0xf)|reverse,a->min,truth);
        }
        return 1;
    }
    if((i->code&7)==6)return -!fail(v,"32-bit comparison of pointer");
    if((a->type==VALUE_NULL||a->type==RING_NULL)&&exact(b)&&!b.min&&(op==0x10||op==0x50)) {
        aliases(s,a->id,truth==(op==0x10),0);return 1;
    }
    if(a->type==PACKET&&b.type==PACKET_END&&a->id==b.id&&
       (op==0x20||op==0x30||op==0xa0||op==0xb0)) {
        int inbounds=(op==0xa0||op==0xb0)?truth:!truth;
        if(inbounds&&a->low>=0&&a->low<=UINT32_MAX) {
            uint64_t end=(uint64_t)a->low+((op==0xa0||op==0x30)?1:0);
            if(end>UINT32_MAX)return -!fail(v,"packet offset overflow");
            if(end>s->packet_bound)s->packet_bound=(uint32_t)end;
        }
        return 1;
    }
    if(a->type==PACKET_END&&b.type==PACKET&&(i->code&8)) {
        struct kernel_bpf_insn reversed=*i;
        reversed.dst_reg=i->src_reg;reversed.src_reg=i->dst_reg;
        unsigned rop=op==0x20?0xa0:op==0x30?0xb0:op==0xa0?0x20:op==0xb0?0x30:op;
        reversed.code=(i->code&0xf)|rop;
        if(rop==op)return -!fail(v,"invalid packet pointer comparison");
        return branch(v,s,&reversed,truth);
    }
    return -!fail(v,"invalid pointer comparison");
}
static int active_reservations(const struct state*s)
{for(unsigned j=0;j<64;j++)if(s->reservations[j])return 1;return 0;}
int kernel_bpf_verify(const struct kernel_bpf_insn*insns,uint32_t count,
                      const struct kernel_bpf_vm_env*env)
{
    if(!env||!env->alloc||!env->free||!insns||!count)return -KERNEL_EINVAL;
    struct verifier v={.env=env,.insns=insns,.count=count,.errno_value=-KERNEL_EACCES};
    if(count>KERNEL_BPF_COMPLEXITY_LIMIT)return -KERNEL_E2BIG;
    if(!(env->program_type==1||env->program_type==2||env->program_type==5||env->program_type==17))return -KERNEL_EINVAL;
    v.continuation=env->alloc(env->opaque,count);
    if(!v.continuation)return -KERNEL_ENOMEM;
    clear(v.continuation,count);
    struct state *s=0,*pending=0;
    if(!syntax(&v))goto done;
    s=env->alloc(env->opaque,sizeof(*s));
    if(!s){v.errno_value=-KERNEL_ENOMEM;v.error="verifier allocation failed";goto done;}
    clear(s,sizeof(*s));
    s->serial=count+1;s->frame[0].stack_id=count+1;s->r[1]=(struct value){.type=CONTEXT};
    s->r[10]=(struct value){.type=STACK,.id=count+1};
    while(s) {
        if(++v.processed>KERNEL_BPF_COMPLEXITY_LIMIT){fail(&v,"BPF program is too large. Processed 1000001 insn");break;}
        if(s->pc>=count||v.continuation[s->pc]){fail(&v,"program falls off end or enters LD_IMM64 continuation");break;}
        const struct kernel_bpf_insn*i=&insns[s->pc];
        unsigned c=i->code&7,op=i->code&0xf0;
        if(c==4||c==7) {if(!alu(&v,s,i))break;}
        else if(i->code==0x18) {
            if(!i->src_reg) {
                uint64_t n=(uint32_t)i->imm|((uint64_t)(uint32_t)insns[s->pc+1].imm<<32);
                s->r[i->dst_reg]=scalar(n,n);
            } else {
                struct kernel_bpf_vm_map m={0};
                if(!env->map||env->map(env->opaque,s->pc,i,&m)){fail(&v,"invalid map fd or index relocation");break;}
                int value=i->src_reg==2||i->src_reg==6;
                uint32_t off=value?(uint32_t)insns[s->pc+1].imm:0;
                if(value&&(!m.value||off>m.value_size)){fail(&v,"invalid global-data map value offset");break;}
                s->r[i->dst_reg]=(struct value){.type=value?VALUE:MAP,.map_pc=s->pc,.id=s->pc+2,
                    .low=off,.high=off,.size=m.value_size,.flags=(uint16_t)m.flags};
            }
            s->pc++;
        } else if(c==0) {
            if((i->code&0xe0)==0x40&&s->r[i->src_reg].type!=SCALAR){fail(&v,"packet indirect offset is not scalar");break;}
            if(active_reservations(s)){fail(&v,"packet-load early exit leaks ringbuf reservation");break;}
            if(!(env->flags&KERNEL_BPF_VM_CLASSIC)&&s->r[6].type!=CONTEXT){fail(&v,"R6 must hold socket context for packet load");break;}
            s->r[i->dst_reg]=scalar(0,UINT32_MAX);
            if(!(env->flags&KERNEL_BPF_VM_CLASSIC))for(unsigned j=1;j<=5;j++)s->r[j]=(struct value){0};
        } else if(c==1) {
            struct value data;
            if(!memory(&v,s,s->r[i->src_reg],i->off,width(i->code),MEMORY_READ,&data,0))break;
            if((i->code&0xe0)==0x80) {
                if(data.type!=SCALAR){fail(&v,"sign-extending pointer load");break;}
                if(exact(data)) {
                    uint64_t n=width(i->code)==1?(uint64_t)(int64_t)(int8_t)data.min:
                        width(i->code)==2?(uint64_t)(int64_t)(int16_t)data.min:(uint64_t)(int64_t)(int32_t)data.min;
                    data=scalar(n,n);
                } else data=scalar(0,UINT64_MAX);
            }
            s->r[i->dst_reg]=data;
        } else if(c==2||c==3) {
            struct value data=c==2?scalar((uint64_t)(int64_t)i->imm,(uint64_t)(int64_t)i->imm):s->r[i->src_reg];
            int atomic=(i->code&0xe0)==0xc0;
            if(data.type==UNINIT||data.type==DEAD){fail(&v,"uninitialized store register");break;}
            if(atomic&&(data.type!=SCALAR||(i->imm==0xf1&&s->r[0].type!=SCALAR))){fail(&v,"atomic requires scalar registers");break;}
            if(!memory(&v,s,s->r[i->dst_reg],i->off,width(i->code),MEMORY_WRITE,&data,atomic))break;
            if(atomic&&(i->imm&1))s->r[i->imm==0xf1?0:i->src_reg]=scalar(0,width(i->code)==4?UINT32_MAX:UINT64_MAX);
        } else if(op==0x80) {
            if(i->src_reg==1) {
                uint32_t destination=(uint32_t)((int64_t)s->pc+1+i->imm);
                unsigned j;for(j=0;j<=s->depth;j++)if(s->frame[j].entry==destination)break;
                if(j<=s->depth){fail(&v,"recursive call is not allowed");break;}
                if(s->depth+1>=KERNEL_BPF_MAX_FRAMES){fail(&v,"call stack exceeds MAX_CALL_FRAMES 16");break;}
                struct frame*f=&s->frame[++s->depth];clear(f,sizeof(*f));
                f->entry=destination;f->return_pc=s->pc+1;f->stack_id=++s->serial;
                for(j=0;j<4;j++)f->saved[j]=s->r[j+6];
                s->r[0]=(struct value){0};s->r[10]=(struct value){.type=STACK,.frame=(uint8_t)s->depth,.id=f->stack_id};
                s->pc=destination;continue;
            }
            if(!helper_call(&v,s,(uint32_t)i->imm))break;
        } else if(op==0x90) {
            if(s->r[0].type==UNINIT||s->r[0].type==DEAD){fail(&v,"R0 is not readable at exit");break;}
            if(s->depth) {
                if(s->r[0].type==STACK&&s->r[0].frame==s->depth){fail(&v,"returning pointer to dead stack frame");break;}
                struct frame*f=&s->frame[s->depth--];
                for(unsigned j=0;j<4;j++)s->r[j+6]=f->saved[j];
                for(unsigned j=1;j<=5;j++)s->r[j]=(struct value){0};
                s->r[10]=(struct value){.type=STACK,.frame=(uint8_t)s->depth,.id=s->frame[s->depth].stack_id};
                s->pc=f->return_pc;continue;
            }
            if(s->r[0].type!=SCALAR){fail(&v,"R0 leaks pointer at program exit");break;}
            if(active_reservations(s)){fail(&v,"Unreleased reference: ringbuf reservation");break;}
            struct state*next=pending;if(next)pending=next->next;
            env->free(env->opaque,s);s=next;continue;
        } else if(op==0) {
            s->pc=(uint32_t)((int64_t)s->pc+1+(c==6?i->imm:i->off));continue;
        } else {
            struct value a=s->r[i->dst_reg];
            struct value b=(i->code&8)?s->r[i->src_reg]:
                scalar((uint64_t)(int64_t)i->imm,(uint64_t)(int64_t)i->imm);
            if(exact(a)&&exact(b)) {
                int take=kernel_bpf_vm_condition(i->code,a.min,b.min);
                s->pc=(uint32_t)((int64_t)s->pc+1+(take?i->off:0));
                continue;
            }
            struct state*other=env->alloc(env->opaque,sizeof(*other));
            if(!other){v.error="verifier state allocation failed";v.errno_value=-KERNEL_ENOMEM;break;}
            copy(other,s,sizeof(*other));
            int yes=branch(&v,s,i,1),no=branch(&v,other,i,0);
            if(yes<0||no<0){env->free(env->opaque,other);break;}
            if(no){other->pc++;other->next=pending;pending=other;}
            else env->free(env->opaque,other);
            if(yes){s->pc=(uint32_t)((int64_t)s->pc+1+i->off);continue;}
            struct state*next=pending;if(next)pending=next->next;
            env->free(env->opaque,s);s=next;continue;
        }
        s->pc++;
    }
done:
    if(s)env->free(env->opaque,s);
    while(pending){struct state*next=pending->next;env->free(env->opaque,pending);pending=next;}
    env->free(env->opaque,v.continuation);
    if(v.error){log_text(&v,v.error);log_text(&v,"\n");}
    log_text(&v,"processed ");log_number(&v,v.processed);log_text(&v," insns (limit 1000000)\n");
    return v.error?v.errno_value:0;
}
