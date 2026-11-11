#include <kernel/sync.h>
#include "../../kernel/bpf/private.h"
#include <kernel/errno.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned allocations;
enum kernel_heap_status kernel_heap_allocate_zeroed(struct kernel_heap *h,size_t count,size_t size,void **out)
{(void)h;*out=calloc(count,size);if(!*out)return KERNEL_HEAP_STATUS_EMPTY;allocations++;return 0;}
void kernel_heap_release(struct kernel_heap *h,void *p){(void)h;assert(p&&allocations);free(p);allocations--;}
void bpf_object_initialize(struct bpf_object *o,struct kernel_heap *h,unsigned kind)
{o->heap=h;o->kind=kind;o->references=1;}
int bpf_object_publish(struct bpf_object *o){o->id=1;return 0;}
void bpf_object_put(struct bpf_object *o)
{struct bpf_btf *b=(void *)o;if(!b->kernel)free(b->data);kernel_heap_release(o->heap,b->offsets);kernel_heap_release(o->heap,b);}
static unsigned char *blob(const uint32_t *records,size_t count,uint32_t *size)
{
    *size=24+count*4+5;unsigned char *p=calloc(1,*size);uint32_t *h=(void *)p;
    h[0]=0x0001eb9f;h[1]=24;h[2]=0;h[3]=count*4;h[4]=count*4;h[5]=5;
    memcpy(p+24,records,count*4);memcpy(p+24+count*4,"\0abc",5);return p;
}
static void check(const uint32_t *records,size_t n,int valid)
{
    uint32_t size;unsigned char *data=blob(records,n,&size);struct bpf_btf *btf=0;
    int r=bpf_btf_parse(0,data,size,&btf);assert((!r)==valid);
    if(valid){assert(btf&&btf->object.kind==BPF_OBJECT_BTF);bpf_object_put(&btf->object);}else{assert(!btf);free(data);}
    assert(!allocations);
}
#define CHECK(a,v) check(a,sizeof(a)/4,v)
int main(void)
{
    const uint32_t integer[]={1,1U<<24,4,32};CHECK(integer,1);
    uint32_t truncated[]={1,1U<<24,4};CHECK(truncated,0);
    uint32_t badref[]={0,2U<<24,2};CHECK(badref,0);
    uint32_t self[]={1,8U<<24,1};CHECK(self,0);
    uint32_t badindex[]={0,3U<<24,0,2,2,1,1,8U<<24,UINT32_MAX};CHECK(badindex,0);
    uint32_t tag[]={1,1U<<24,4,32,1,17U<<24,1,UINT32_MAX};CHECK(tag,0);
    uint32_t reserved[]={1,1U<<24,4,0x8000020};CHECK(reserved,0);

    uint32_t cycle[]={1,4U<<24|1,8,0,1,0};CHECK(cycle,0);
    uint32_t variable_member[]={1,1U<<24,4,32,1,14U<<24,1,1,1,4U<<24|1,4,0,2,0};CHECK(variable_member,0);
    uint32_t ptrcycle[]={1,4U<<24|1,8,0,2,0,0,2U<<24,1};CHECK(ptrcycle,1);
    uint32_t bounds[]={1,1U<<24,4,32,1,4U<<24|1,4,0,1,8};CHECK(bounds,0);
    uint32_t kinds[]={
        1,1U<<24,4,32, /* 1 int */
        0,2U<<24,0, /* 2 pointer */
        0,3U<<24,0,1,1,2, /* 3 array */
        1,4U<<24|1,4,0,1,0, /* 4 struct */
        1,5U<<24|1,4,0,1,0, /* 5 union */
        1,6U<<24|1,4,1,3, /* 6 enum */
        1,7U<<24,0, /* 7 forward */
        1,8U<<24,1,0,9U<<24,1,0,10U<<24,1,0,11U<<24,1, /* 8-11 modifiers */
        1,12U<<24|1,13, /* 12 function */
        0,13U<<24|1,1,1,2, /* 13 prototype */
        1,14U<<24,1,1, /* 14 variable */
        1,15U<<24|1,16,14,4,4, /* 15 section with leading/trailing padding */
        1,16U<<24,8, /* 16 float */
        1,17U<<24,4,UINT32_MAX, /* 17 declaration tag */
        1,18U<<24,1, /* 18 type tag */
        1,19U<<24|1,8,1,1,0, /* 19 enum64 */
    };CHECK(kinds,1);
    uint32_t size;unsigned char *data=blob(kinds,sizeof(kinds)/4,&size);struct bpf_btf *b=0;
    assert(!bpf_btf_parse(0,data,size,&b));uint32_t s;
    assert(!bpf_btf_type_size(b,14,&s)&&s==4);
    assert(!bpf_btf_type_size(b,15,&s)&&s==16);
    assert(!bpf_btf_type_size(b,3,&s)&&s==8);assert(!bpf_btf_type_size(b,18,&s)&&s==4);
    bpf_object_put(&b->object);
    for(unsigned j=0;j<size;j++) {
        data=blob(kinds,sizeof(kinds)/4,&size);b=0;
        assert(bpf_btf_parse(0,data,j,&b)<0);assert(!b);free(data);assert(!allocations);
    }
    for(unsigned j=0;j<size;j++)for(unsigned bit=0;bit<8;bit++) {
        data=blob(kinds,sizeof(kinds)/4,&size);data[j]^=(unsigned char)(1U<<bit);b=0;
        int result=bpf_btf_parse(0,data,size,&b);
        if(result){assert(!b);free(data);}else bpf_object_put(&b->object);
        assert(!allocations);
    }
    data=blob(integer,4,&size);((uint32_t *)data)[5]=UINT32_MAX;b=0;
    assert(bpf_btf_parse(0,data,size,&b)<0);free(data);
    puts("PASS BTF kinds 1-19, references, recursive pointers, by-value cycles, field bounds, array/VAR/DATASEC sizes, truncated records/tables and ownership");
}
