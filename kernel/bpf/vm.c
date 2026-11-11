#include <kernel/bpf_vm.h>
#include <kernel/errno.h>

/* Keep the byte-count loop out of constant-folded callers: base RV64 has no
 * byte-swap instruction and a compiler builtin would require libgcc helpers. */
__attribute__((noinline)) uint64_t kernel_bpf_vm_byteswap(uint64_t n,unsigned bytes)
{
    uint64_t result=0;
    while(bytes--){result=(result<<8)|(n&255);n>>=8;}
    return result;
}

/* Shared with abstract execution so constant folding and actual execution have
 * identical width, signed division, zero divisor and shift semantics. */
uint64_t kernel_bpf_vm_alu(uint8_t code, int16_t off, int32_t imm,
                           uint64_t a, uint64_t b)
{
    unsigned op=code&0xf0, bits=(code&7)==4 ? 32 : 64;
    uint64_t mask=bits==32 ? UINT32_MAX : UINT64_MAX, r=0;
    a&=mask;b&=mask;
    switch(op) {
    case 0x00:r=a+b;break;
    case 0x10:r=a-b;break;
    case 0x20:r=a*b;break;
    case 0x30:case 0x90:
        if (!b) return op==0x30 ? 0 : a;
        if (off==1) {
            int64_t x=bits==32 ? (int32_t)a : (int64_t)a;
            int64_t y=bits==32 ? (int32_t)b : (int64_t)b;
            /* Negation in unsigned arithmetic also handles INT64_MIN. */
            uint64_t ax=x<0 ? 0-(uint64_t)x : (uint64_t)x;
            uint64_t ay=y<0 ? 0-(uint64_t)y : (uint64_t)y;
            r=op==0x30 ? ax/ay : ax%ay;
            if (op==0x30 ? (x<0)!=(y<0) : x<0) r=0-r;
        } else r=op==0x30 ? a/b : a%b;
        break;
    case 0x40:r=a|b;break;
    case 0x50:r=a&b;break;
    case 0x60:r=a<<(b&(bits-1));break;
    case 0x70:r=a>>(b&(bits-1));break;
    case 0x80:r=0-a;break;
    case 0xa0:r=a^b;break;
    case 0xb0:
        r=b;
        if(off==8)r=(uint64_t)(int64_t)(int8_t)b;
        else if(off==16)r=(uint64_t)(int64_t)(int16_t)b;
        else if(off==32)r=(uint64_t)(int64_t)(int32_t)b;
        break;
    case 0xc0:r=bits==32 ? (uint64_t)(int64_t)((int32_t)a>>(b&31)) :
                                           (uint64_t)((int64_t)a>>(b&63));break;
    case 0xd0: {
        int swap=(code&7)==7 || (code&8);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        if((code&7)==4)swap=!swap;
#endif
        if(imm==16)return swap ? kernel_bpf_vm_byteswap((uint16_t)a,2) : (uint16_t)a;
        if(imm==32)return swap ? kernel_bpf_vm_byteswap((uint32_t)a,4) : (uint32_t)a;
        /* END64 is encoded in the ALU32 class but uses the full register. */
        return swap ? kernel_bpf_vm_byteswap(a,8) : a;
    }
    }
    return r&mask;
}
int kernel_bpf_vm_condition(uint8_t code,uint64_t a,uint64_t b)
{
    int wide=(code&7)==5;
    if(!wide){a=(uint32_t)a;b=(uint32_t)b;}
    int64_t sa=wide ? (int64_t)a : (int32_t)a;
    int64_t sb=wide ? (int64_t)b : (int32_t)b;
    switch(code&0xf0) {
    case 0x10:return a==b;
    case 0x20:return a>b;
    case 0x30:return a>=b;
    case 0x40:return (a&b)!=0;
    case 0x50:return a!=b;
    case 0x60:return sa>sb;
    case 0x70:return sa>=sb;
    case 0xa0:return a<b;
    case 0xb0:return a<=b;
    case 0xc0:return sa<sb;
    case 0xd0:return sa<=sb;
    }
    return 0;
}
static unsigned memory_size(uint8_t code)
{static const uint8_t sizes[4]={4,2,1,8};return sizes[(code>>3)&3];}
static uint64_t read_memory(uint64_t address,unsigned n)
{
    const uint8_t *p=(const uint8_t *)(uintptr_t)address;
    uint64_t value=0;
    for(unsigned i=0;i<n;i++)value|=(uint64_t)p[i]<<(8*i);
    return value;
}
static void write_memory(uint64_t address,unsigned n,uint64_t value)
{
    uint8_t *p=(uint8_t *)(uintptr_t)address;
    for(unsigned i=0;i<n;i++)p[i]=(uint8_t)(value>>(8*i));
}
static uint64_t atomic_memory(uint64_t address,unsigned size,int op,
                              uint64_t value,uint64_t expected)
{
#define ATOMIC(T) do { \
    T *p=(T *)(uintptr_t)address,v=(T)value,e=(T)expected; \
    switch(op&0xf0) { \
    case 0x00:return __atomic_fetch_add(p,v,__ATOMIC_SEQ_CST); \
    case 0x40:return __atomic_fetch_or(p,v,__ATOMIC_SEQ_CST); \
    case 0x50:return __atomic_fetch_and(p,v,__ATOMIC_SEQ_CST); \
    case 0xa0:return __atomic_fetch_xor(p,v,__ATOMIC_SEQ_CST); \
    case 0xe0:return __atomic_exchange_n(p,v,__ATOMIC_SEQ_CST); \
    case 0xf0:__atomic_compare_exchange_n(p,&e,v,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);return e; \
    } \
} while(0)
    if(size==8)ATOMIC(uint64_t);
    else ATOMIC(uint32_t);
#undef ATOMIC
    return 0;
}
int kernel_bpf_vm_execute(const struct kernel_bpf_insn *insns,uint32_t count,
                          const struct kernel_bpf_vm_env *env,
                          const struct kernel_bpf_vm_run *run,
                          struct kernel_bpf_vm_workspace *work,uint64_t *result)
{
    if(!insns || !count || !env || !run || !work || !result)return -KERNEL_EINVAL;
    uint64_t *r=work->registers;
    for(unsigned i=0;i<11;i++)r[i]=0;
    r[1]=(uintptr_t)run->context;
    r[10]=(uintptr_t)(work->frames[0].stack+KERNEL_BPF_STACK_SIZE/8);
    uint32_t pc=0,depth=0;
    while(pc<count) {
        const struct kernel_bpf_insn *i=&insns[pc];
        unsigned c=i->code&7,op=i->code&0xf0,d=i->dst_reg,s=i->src_reg;
        uint64_t b=(i->code&8) ? r[s] : (uint64_t)(int64_t)i->imm;
        if(c==4 || c==7) {
            /* END32's class must not truncate its input for a 64-bit swap. */
            if(op==0xd0 && i->imm==64) {
                uint8_t code=i->code;
                int swap=c==7 || (code&8);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
                if(c==4)swap=!swap;
#endif
                r[d]=swap ? kernel_bpf_vm_byteswap(r[d],8) : r[d];
            } else r[d]=kernel_bpf_vm_alu(i->code,i->off,i->imm,r[d],b);
        } else if(i->code==0x18) {
            if(!s)r[d]=(uint32_t)i->imm|((uint64_t)(uint32_t)insns[pc+1].imm<<32);
            else {
                struct kernel_bpf_vm_map m;
                int e=env->map(env->opaque,pc,i,&m);
                if(e)return e; /* Broken retained relocation is an owner error. */
                r[d]=(uintptr_t)((s==2 || s==6) ? m.value : m.object);
                if(s==2 || s==6)r[d]+=(uint32_t)insns[pc+1].imm;
            }
            pc++;
        } else if(c==0) {
            uint32_t value=0;
            if((i->code&0xe0)==0x80) value=run->packet_length(run->opaque);
            else {
                int64_t off=i->imm;
                if((i->code&0xe0)==0x40)off=(int32_t)((uint32_t)i->imm+(uint32_t)r[s]);
                int error;
                if((env->flags&KERNEL_BPF_VM_CLASSIC) && (i->code&0xe0)==0x20 &&
                   off>=-4096 && off<0 && run->ancillary)
                    error=run->ancillary(run->opaque,(uint32_t)(off+4096),(uint32_t)r[0],(uint32_t)r[7],&value);
                else error=run->packet(run->opaque,off,memory_size(i->code),&value);
                if(error) {
                    *result=0;return 0;
                }
            }
            r[d]=value;
        } else if(c==1) {
            unsigned size=memory_size(i->code);
            uint64_t address=r[s]+(int64_t)i->off;
            uint64_t value;
            if(run->context_read && address>=(uintptr_t)run->context &&
               address-(uintptr_t)run->context<run->context_size)
                value=run->context_read(run->opaque,(int32_t)(address-(uintptr_t)run->context),size);
            else value=read_memory(address,size);
            if((i->code&0xe0)==0x80) {
                if(size==1)value=(uint64_t)(int64_t)(int8_t)value;
                else if(size==2)value=(uint64_t)(int64_t)(int16_t)value;
                else value=(uint64_t)(int64_t)(int32_t)value;
            }
            r[d]=value;
        } else if(c==2 || c==3) {
            unsigned size=memory_size(i->code);
            if((i->code&0xe0)==0xc0) {
                uint64_t old=atomic_memory(r[d]+(int64_t)i->off,size,i->imm,r[s],r[0]);
                if(i->imm==0xf1)r[0]=old;
                else if(i->imm&1)r[s]=old;
            } else write_memory(r[d]+(int64_t)i->off,size,c==2 ?
                                 (uint64_t)(int64_t)i->imm : r[s]);
        } else if(c==5 || c==6) {
            if(op==0x80) {
                if(s==1) {
                    if(depth+1>=KERNEL_BPF_MAX_FRAMES)return -KERNEL_EFAULT;
                    struct kernel_bpf_vm_frame *f=&work->frames[++depth];
                    for(unsigned j=0;j<4;j++)f->saved[j]=r[j+6];
                    f->return_pc=pc+1;
                    r[10]=(uintptr_t)(f->stack+KERNEL_BPF_STACK_SIZE/8);
                    pc=(uint32_t)((int64_t)pc+1+i->imm);continue;
                }
                uint64_t args[5];
                for(unsigned j=0;j<5;j++)args[j]=r[j+1];
                r[0]=run->helper(run->opaque,(uint32_t)i->imm,args);
                for(unsigned j=1;j<=5;j++)r[j]=0;
            } else if(op==0x90) {
                if(!depth){*result=r[0];return 0;}
                struct kernel_bpf_vm_frame *f=&work->frames[depth--];
                for(unsigned j=0;j<4;j++)r[j+6]=f->saved[j];
                for(unsigned j=1;j<=5;j++)r[j]=0;
                r[10]=(uintptr_t)(work->frames[depth].stack+KERNEL_BPF_STACK_SIZE/8);
                pc=f->return_pc;continue;
            } else if(op==0 || kernel_bpf_vm_condition(i->code,r[d],b)) {
                pc=(uint32_t)((int64_t)pc+1+(op==0 && c==6 ? i->imm : i->off));continue;
            }
        } else return -KERNEL_EFAULT;
        pc++;
    }
    return -KERNEL_EFAULT;
}
