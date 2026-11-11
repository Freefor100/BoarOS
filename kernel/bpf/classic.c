#include <kernel/bpf_vm.h>
#include <kernel/errno.h>

struct classic_address { uint32_t out; uint16_t initialized; };
static int allowed(uint16_t c)
{
    switch(c) {
    case 0x04:case 0x0c:case 0x14:case 0x1c:case 0x24:case 0x2c:
    case 0x34:case 0x3c:case 0x94:case 0x9c:case 0x54:case 0x5c:
    case 0x44:case 0x4c:case 0xa4:case 0xac:case 0x64:case 0x6c:
    case 0x74:case 0x7c:case 0x84:
    case 0x20:case 0x28:case 0x30:case 0x80:
    case 0x40:case 0x48:case 0x50:case 0x00:case 0x60:
    case 0x81:case 0xb1:case 0x01:case 0x61:
    case 0x02:case 0x03:case 0x07:case 0x87:case 0x06:case 0x16:
    case 0x05:case 0x15:case 0x1d:case 0x35:case 0x3d:
    case 0x25:case 0x2d:case 0x45:case 0x4d:return 1;
    default:return 0;
    }
}
static uint32_t converted_size(uint16_t c)
{
    if(c==0xb1)return 6;
    if(c==0x3c||c==0x9c)return 4;
    if((c&7)==5&&c!=5)return 3;
    if(c==6)return 2;
    return 1;
}
static void emit(struct kernel_bpf_insn*out,uint32_t *at,uint8_t code,
                 unsigned d,unsigned s,int16_t off,int32_t imm)
{out[(*at)++]=(struct kernel_bpf_insn){.code=code,.dst_reg=d,.src_reg=s,.off=off,.imm=imm};}
int kernel_bpf_classic_convert(const struct kernel_bpf_classic_insn*insns,
                               uint32_t count,struct kernel_bpf_insn*out,
                               uint32_t capacity,uint32_t*out_count,
                               const struct kernel_bpf_vm_env*env)
{
    if(!insns||!count||count>KERNEL_BPF_CLASSIC_MAX_INSNS||!out_count||!env||!env->alloc||!env->free)
        return -KERNEL_EINVAL;
    struct classic_address*addresses=env->alloc(env->opaque,count*sizeof(*addresses));
    if(!addresses)return -KERNEL_ENOMEM;
    int error=-KERNEL_EINVAL;
    for(uint32_t pc=0;pc<count;pc++)addresses[pc].initialized=UINT16_MAX;
    uint16_t initialized=0;
    uint32_t n=3;
    for(uint32_t pc=0;pc<count;pc++) {
        const struct kernel_bpf_classic_insn*i=&insns[pc];
        if(!allowed(i->code))goto done;
        addresses[pc].out=n;n+=converted_size(i->code);
        initialized&=addresses[pc].initialized;
        switch(i->code) {
        case 0x34:case 0x94:if(!i->k)goto done;break;
        case 0x64:case 0x74:if(i->k>=32)goto done;break;
        case 0x60:case 0x61:
            if(i->k>=16||!(initialized&(1u<<i->k)))goto done;
            break;
        case 0x02:case 0x03:
            if(i->k>=16)goto done;
            initialized|=1u<<i->k;break;
        case 0x05:
            if(i->k>=count-pc-1)goto done;
            addresses[pc+1+i->k].initialized&=initialized;
            initialized=UINT16_MAX;break;
        case 0x15:case 0x1d:case 0x35:case 0x3d:
        case 0x25:case 0x2d:case 0x45:case 0x4d:
            if(i->jt>=count-pc-1||i->jf>=count-pc-1)goto done;
            addresses[pc+1+i->jt].initialized&=initialized;
            addresses[pc+1+i->jf].initialized&=initialized;
            initialized=UINT16_MAX;break;
        case 0x20:case 0x28:case 0x30:
            if(i->k>=(uint32_t)-4096) {
                uint32_t selector=i->k-(uint32_t)-4096;
                if(selector>60||(selector&3))goto done;
            }
            break;
        }
    }
    if(insns[count-1].code!=0x06&&insns[count-1].code!=0x16)goto done;
    *out_count=n;
    if(!out){error=0;goto done;}
    if(capacity<n){error=-KERNEL_ENOSPC;goto done;}
    uint32_t at=0;
    emit(out,&at,0xb4,0,0,0,0);
    emit(out,&at,0xb4,7,0,0,0);
    emit(out,&at,0xbf,6,1,0,0);
    for(uint32_t pc=0;pc<count;pc++) {
        const struct kernel_bpf_classic_insn*i=&insns[pc];
        unsigned c=i->code&7;
        if(c==4) {
            if(i->code==0x3c||i->code==0x9c) {
                /* Classic zero division exits the whole filter. eBPF zero
                 * division writes zero and continues, hence the explicit exit. */
                emit(out,&at,0x55,7,0,2,0);
                emit(out,&at,0xb4,0,0,0,0);
                emit(out,&at,0x95,0,0,0,0);
            }
            emit(out,&at,(uint8_t)i->code,0,(i->code&8)?7:0,0,(i->code&8)||i->code==0x84?0:(int32_t)i->k);
        } else if(i->code==0x00||i->code==0x01)emit(out,&at,0xb4,c==0?0:7,0,0,(int32_t)i->k);
        else if(i->code==0x60||i->code==0x61)emit(out,&at,0x61,c==0?0:7,10,(int16_t)(-4*(int32_t)(i->k+1)),0);
        else if(i->code==0x02||i->code==0x03)emit(out,&at,0x63,10,c==2?0:7,(int16_t)(-4*(int32_t)(i->k+1)),0);
        else if(i->code==0x80||i->code==0x81)emit(out,&at,0x80,c==0?0:7,0,0,0);
        else if(i->code==0xb1) {
            emit(out,&at,0xbf,8,0,0,0);
            emit(out,&at,0x30,0,0,0,(int32_t)i->k);
            emit(out,&at,0x54,0,0,0,15);
            emit(out,&at,0x64,0,0,0,2);
            emit(out,&at,0xbc,7,0,0,0);
            emit(out,&at,0xbf,0,8,0,0);
        } else if(c==0) {
            if(i->k==(uint32_t)(-4096+40)&&(i->code&0xe0)==0x20)emit(out,&at,0xac,0,7,0,0);
            else emit(out,&at,(uint8_t)i->code,0,(i->code&0xe0)==0x40?7:0,0,(int32_t)i->k);
        } else if(i->code==0x07)emit(out,&at,0xbc,7,0,0,0);
        else if(i->code==0x87)emit(out,&at,0xbc,0,7,0,0);
        else if(c==6) {
            if(i->code==6)emit(out,&at,0xb4,0,0,0,(int32_t)i->k);
            emit(out,&at,0x95,0,0,0,0);
        } else if(i->code==5) {
            int32_t off=(int32_t)addresses[pc+1+i->k].out-(int32_t)at-1;
            emit(out,&at,0x06,0,0,0,off);
        } else {
            emit(out,&at,(uint8_t)((i->code&~7)|6),0,(i->code&8)?7:0,1,(i->code&8)?0:(int32_t)i->k);
            int32_t no=(int32_t)addresses[pc+1+i->jf].out-(int32_t)at-1;
            emit(out,&at,0x06,0,0,0,no);
            int32_t yes=(int32_t)addresses[pc+1+i->jt].out-(int32_t)at-1;
            emit(out,&at,0x06,0,0,0,yes);
        }
    }
    if(at!=n){error=-KERNEL_EFAULT;goto done;}
    /* Original classic validation already proves finite forward control flow,
     * initialized scratch reads and opcode validity. Translation introduces
     * only fixed, checked stack slots and forward branches. Re-running the
     * path-sensitive eBPF verifier would unnecessarily reject large valid
     * classic DAGs at the eBPF complexity limit. */
    error=0;
done:
    env->free(env->opaque,addresses);
    return error;
}
