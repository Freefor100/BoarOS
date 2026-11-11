#include <kernel/sync.h>
#include "private.h"
#include <kernel/errno.h>
#include <string.h>

struct btf_header { uint16_t magic; uint8_t version,flags; uint32_t length,type_off,type_len,str_off,str_len; };
static struct bpf_btf *vmlinux;
static unsigned kind(const uint32_t *t) { return (t[1] >> 24) & 31; }
static unsigned vlen(const uint32_t *t) { return t[1] & 65535; }
static int alloc(struct kernel_heap *h,size_t n,void **p)
{ return kernel_heap_allocate_zeroed(h,1,n,p) ? -KERNEL_ENOMEM : 0; }
static const uint32_t *record(const struct bpf_btf *b,uint32_t id)
{return (const uint32_t *)(b->data+b->offsets[id]);}
int bpf_btf_record(const struct bpf_btf *b,uint32_t id,const uint32_t **t)
{
    if (!b || !id || id>b->type_count) return -KERNEL_EINVAL;
    *t=(const uint32_t *)(b->data+b->offsets[id]); return 0;
}
static int string_valid(const struct bpf_btf *b,uint32_t off)
{
    const struct btf_header *h=(const void *)b->data;
    /* The complete table's last byte is checked once at load time. */
    return off<h->str_len;
}
static uint32_t record_words(unsigned k,unsigned n)
{
    switch(k) {
    case 1:case 14:case 17:return 4;
    case 3:return 6;
    case 4:case 5:case 15:case 19:return 3+n*3;
    case 6:case 13:return 3+n*2;
    case 2:case 7:case 8:case 9:case 10:case 11:case 12:case 16:case 18:return 3;
    default:return 0;
    }
}
static int resolve_size(const struct bpf_btf *b,uint32_t id,uint32_t *size,int metadata)
{
    uint64_t multiplier=1;
    for(uint32_t depth=0;depth<=b->type_count;depth++) {
        const uint32_t *t;
        if(bpf_btf_record(b,id,&t))return -KERNEL_EINVAL;
        unsigned k=kind(t);
        if(!metadata && (k==14 || k==15))return -KERNEL_EINVAL;
        if(k==8 || k==9 || k==10 || k==11 || k==14 || k==18) { id=t[2];continue; }
        if(k==3) { multiplier*=t[5];if(multiplier>UINT32_MAX)return -KERNEL_E2BIG;id=t[3];continue; }
        uint32_t s=k==2 ? 8 : (k==1 || k==4 || k==5 || k==6 || k==15 || k==16 || k==19) ? t[2] : 0;
        if(!s && k!=4 && k!=5 && k!=15)return -KERNEL_EINVAL;
        if(multiplier*s>UINT32_MAX)return -KERNEL_E2BIG;
        *size=(uint32_t)(multiplier*s);return 0;
    }
    return -KERNEL_ELOOP;
}
int bpf_btf_type_size(const struct bpf_btf *b,uint32_t id,uint32_t *size)
{ return resolve_size(b,id,size,1); }
/* Only by-value edges participate: recursive pointer graphs are legal C. */
static uint32_t value_edge(const uint32_t *t,uint32_t n)
{
    unsigned k=kind(t);
    if(k==4 || k==5)return n<vlen(t)?t[4+n*3]:0;
    if(n)return 0;
    if(k==3)return t[3];
    if(k==8 || k==9 || k==10 || k==11 || k==18)return t[2];
    return 0;
}
static int validate(struct bpf_btf *b)
{
    for(uint32_t id=1;id<=b->type_count;id++) {
        const uint32_t *t=record(b,id);
        unsigned k=kind(t),n=vlen(t),flag=t[1]>>31;
        if(!string_valid(b,t[0]) || (t[1]&0x60ff0000U))return -KERNEL_EINVAL;
        if(k!=4 && k!=5 && k!=6 && k!=7 && k!=19 && flag)return -KERNEL_EINVAL;
        if(k!=4 && k!=5 && k!=6 && k!=13 && k!=15 && k!=19 && k!=12 && n)return -KERNEL_EINVAL;
        if(k==12 && n>2)return -KERNEL_EINVAL;
        if((k==2 || k==3 || k==9 || k==10 || k==11 || k==13) && t[0])return -KERNEL_EINVAL;
        if(k==3 && t[2])return -KERNEL_EINVAL;
        if((k==8 || k==12 || k==14 || k==17 || k==18) &&
            !b->data[((const struct btf_header *)b->data)->length+
                ((const struct btf_header *)b->data)->str_off+t[0]])return -KERNEL_EINVAL;

        if(k==1 && (!t[2] || t[2]>16 || (t[3]&0xf800ff00U) || !(t[3]&255) ||
            ((t[3]>>16)&255)+(t[3]&255)>t[2]*8))return -KERNEL_EINVAL;
        if((k==6 || k==19) && t[2]!=1 && t[2]!=2 && t[2]!=4 && t[2]!=8)return -KERNEL_EINVAL;
        if(k==16 && t[2]!=2 && t[2]!=4 && t[2]!=8 && t[2]!=12 && t[2]!=16)return -KERNEL_EINVAL;
        if(k==7 && t[2])return -KERNEL_EINVAL;
        if(k==2 || (k>=8 && k<=14) || k==17 || k==18) {
            if(t[2]>b->type_count)return -KERNEL_EINVAL;
            if((k==12 || k==14 || k==17) && !t[2])return -KERNEL_EINVAL;
        }
        if(k==12) {const uint32_t *r=record(b,t[2]);if(kind(r)!=13)return -KERNEL_EINVAL;}
        if(k==14 && t[3]>2)return -KERNEL_EINVAL;
        if(k==17) {
            const uint32_t *target=record(b,t[2]);unsigned tk=kind(target);
            if(tk!=4 && tk!=5 && tk!=8 && tk!=12 && tk!=14)return -KERNEL_EINVAL;
            if(t[3]!=UINT32_MAX) {
                if(tk==8 || tk==14)return -KERNEL_EINVAL;
                if(tk==12) {
                    if(!target[2] || target[2]>b->type_count)return -KERNEL_EINVAL;
                    target=record(b,target[2]);
                    if(kind(target)!=13)return -KERNEL_EINVAL;
                }
                if(t[3]>=vlen(target))return -KERNEL_EINVAL;
            }
        }
        if((k==2 || k==8 || k==9 || k==10 || k==11 || k==18) && t[2]) {
            unsigned target=kind(record(b,t[2]));
            if(target==12 || target==14 || target==15 || target==17)return -KERNEL_EINVAL;
        }

        if(k==3) {
            if(!t[3] || t[3]>b->type_count || !t[4] || t[4]>b->type_count)return -KERNEL_EINVAL;
            const uint32_t *r;uint32_t index=t[4];
            for(uint32_t d=0;;d++) {if(d>b->type_count)return -KERNEL_ELOOP;
                if(!index || index>b->type_count)return -KERNEL_EINVAL;
                r=record(b,index);
                if(kind(r)!=8 && kind(r)!=9 && kind(r)!=10 && kind(r)!=11)break;
                index=r[2];if(!index)return -KERNEL_EINVAL;}
            if(kind(r)!=1)return -KERNEL_EINVAL;
        }
        for(unsigned j=0;j<n;j++) {
            const uint32_t *m=t+3+j*((k==4 || k==5 || k==15 || k==19)?3:2);
            if(k==4 || k==5 || k==6 || k==13 || k==19)if(!string_valid(b,m[0]))return -KERNEL_EINVAL;
            if(k==4 || k==5 || k==13) {
                if(m[1]>b->type_count || (!m[1] && (k!=13 || j+1!=n || m[0])))return -KERNEL_EINVAL;
            }
            if(k==15) {const uint32_t *r;
                if(bpf_btf_record(b,m[0],&r) || kind(r)!=14 || (t[2] && ((uint64_t)m[1]+m[2]>t[2])))return -KERNEL_EINVAL;}
        }
    }
    uint8_t *color=0;uint32_t *stack=0,*next=0;
    int result=alloc(b->object.heap,b->type_count+1,(void **)&color);
    if(!result)result=alloc(b->object.heap,(b->type_count+1)*sizeof(uint32_t)*2,(void **)&stack);
    if(result)goto done;
    next=stack+b->type_count+1;
    for(uint32_t root=1;root<=b->type_count;root++) {
        if(color[root])continue;
        uint32_t depth=0;stack[0]=root;next[0]=0;color[root]=1;
        for(;;) {
            const uint32_t *t=record(b,stack[depth]);
            uint32_t child=value_edge(t,next[depth]++);
            if(child) {
                if(color[child]==1){result=-KERNEL_ELOOP;goto done;}
                if(!color[child]){color[child]=1;stack[++depth]=child;next[depth]=0;}
            } else {color[stack[depth]]=2;if(!depth)break;depth--;}
        }
    }
    for(uint32_t id=1;id<=b->type_count;id++) {
        const uint32_t *t=record(b,id);unsigned k=kind(t);
        if(k==3) {uint32_t s;if(resolve_size(b,id,&s,0)){result=-KERNEL_EINVAL;goto done;}}
        if(k==14 || k==13) {
            uint32_t type=t[2],s;
            if(type && resolve_size(b,type,&s,0)){result=-KERNEL_EINVAL;goto done;}
            if(k==13)for(unsigned j=0;j<vlen(t);j++) {
                type=t[4+j*2];
                if(type && resolve_size(b,type,&s,0)){result=-KERNEL_EINVAL;goto done;}
            }
        }

        if(k==4 || k==5)for(unsigned j=0;j<vlen(t);j++) {
            const uint32_t *m=t+3+j*3;uint32_t size;
            if(resolve_size(b,m[1],&size,0)){result=-KERNEL_EINVAL;goto done;}
            uint64_t off=t[1]>>31 ? m[2]&0xffffffU:m[2];
            uint64_t bits=t[1]>>31 ? m[2]>>24:0;
            if(!bits)bits=(uint64_t)size*8;
            if(off+bits>(uint64_t)t[2]*8 || (k==5 && off) || bits>(uint64_t)size*8){result=-KERNEL_EINVAL;goto done;}
        }
    }
done:
    if(stack)kernel_heap_release(b->object.heap,stack);
    if(color)kernel_heap_release(b->object.heap,color);
    return result;
}
int bpf_btf_parse(struct kernel_heap *heap,unsigned char *data,uint32_t size,struct bpf_btf **out)
{
    if(!data || !out || *out || size<sizeof(struct btf_header) || ((uintptr_t)data&3))return -KERNEL_EINVAL;
    const struct btf_header *h=(const void *)data;
    if(h->magic!=0xeb9f || h->version!=1 || h->flags || h->length<sizeof(*h) || h->length>size ||
       (h->length&3) || (h->type_off&3) || (h->type_len&3) ||
       (uint64_t)h->length+h->type_off+h->type_len>size ||
       (uint64_t)h->length+h->str_off+h->str_len>size || !h->str_len ||
       (uint64_t)h->type_off+h->type_len>h->str_off || data[h->length+h->str_off] ||
       data[h->length+h->str_off+h->str_len-1])return -KERNEL_EINVAL;
    uint32_t count=0,pos=0;
    while(pos<h->type_len) {
        if(h->type_len-pos<12)return -KERNEL_EINVAL;
        const uint32_t *t=(const void *)(data+h->length+h->type_off+pos);
        uint32_t words=record_words(kind(t),vlen(t));
        if(!words || (uint64_t)words*4>h->type_len-pos)return -KERNEL_EINVAL;
        pos+=words*4;count++;
    }
    struct bpf_btf *b=0;int r=alloc(heap,sizeof(*b),(void **)&b);if(r)return r;
    bpf_object_initialize(&b->object,heap,BPF_OBJECT_BTF);b->data=data;b->size=size;b->type_count=count;
    r=alloc(heap,((size_t)count+1)*4,(void **)&b->offsets);
    if(!r) {pos=h->length+h->type_off;for(uint32_t id=1;id<=count;id++) {
        b->offsets[id]=pos;const uint32_t *t=(const void *)(data+pos);pos+=record_words(kind(t),vlen(t))*4;}
        r=validate(b);
    }
    if(r){if(b->offsets)kernel_heap_release(heap,b->offsets);kernel_heap_release(heap,b);return r;}
    *out=b;return 0;
}
int bpf_btf_kernel_initialize(struct kernel_heap *heap,const void *data,uint32_t size)
{
    if(vmlinux)return 0;
    struct bpf_btf *b=0;int r=bpf_btf_parse(heap,(unsigned char *)data,size,&b);if(r)return r;
    b->kernel=1;memcpy(b->object.name,"vmlinux",8);
    r=bpf_object_publish(&b->object);if(r){bpf_object_put(&b->object);return r;}
    __atomic_store_n(&vmlinux,b,__ATOMIC_RELEASE);return 0;
}
struct bpf_btf *bpf_btf_kernel(void){return __atomic_load_n(&vmlinux,__ATOMIC_ACQUIRE);}
void bpf_btf_kernel_retire(void)
{
    struct bpf_btf *b=__atomic_exchange_n(&vmlinux,NULL,__ATOMIC_ACQ_REL);
    if(b)bpf_object_put(&b->object);
}
const void *kernel_bpf_kernel_btf(uint32_t *size)
{struct bpf_btf *b=bpf_btf_kernel();*size=b?b->size:0;return b?b->data:NULL;}
