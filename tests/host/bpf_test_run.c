/* Real TEST_RUN, verifier and runtime; host heap/copy/CPU services supply the
 * execution boundary. Helpers outside this fixture's programs trap on entry. */
#include "../../kernel/bpf/private.h"
#include <kernel/files.h>
#include <kernel/errno.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct kernel_heap heap;
static struct kernel_files files={.heap=&heap};
static struct kernel_bpf_program program;
static unsigned allocations,preempt,writes;
static uintptr_t output_base;
static unsigned output_start,output_end,fail_write;
static size_t write_offsets[4];
static union bpf_attribute user;
enum kernel_heap_status kernel_heap_allocate(struct kernel_heap *h,size_t n,void **out)
{assert(h==&heap);*out=malloc(n);assert(*out);allocations++;return 0;}
enum kernel_heap_status kernel_heap_allocate_zeroed(struct kernel_heap *h,size_t n,size_t size,void **out)
{assert(h==&heap);*out=calloc(n,size);assert(*out);allocations++;return 0;}
void kernel_heap_release(struct kernel_heap *h,void *p){assert(h==&heap && allocations);allocations--;free(p);}
struct bpf_object *bpf_object_fd(struct kernel_files *f,int32_t fd,unsigned kind,int *error)
{assert(f==&files && fd==7 && kind==BPF_OBJECT_PROGRAM);program.object.references++;*error=0;return &program.object;}
void bpf_object_put(struct bpf_object *o){assert(o==&program.object && o->references>1);o->references--;}
int bpf_user_copy(struct kernel_mm *mm,uint64_t a,void *p,size_t n,int from)
{
    (void)mm;
    if(!from && a>=output_base && a<output_base+sizeof(user)){
        assert(writes<4);write_offsets[writes++]=a-output_base;
        if(a<output_base+output_start || a+n>output_base+output_end || writes==fail_write)return -KERNEL_EFAULT;
    }
    if(from)memcpy(p,(void *)(uintptr_t)a,n);else memcpy((void *)(uintptr_t)a,p,n);return 0;
}
unsigned kernel_cpu_get_boot_count(void){return 1;}
void kernel_preempt_disable(void){assert(!preempt);preempt++;}
void kernel_preempt_enable(void){assert(preempt);preempt--;}
struct kernel_task *kernel_task_current(void){return NULL;}
uint64_t kernel_time_monotonic_ns(void){static uint64_t n;return ++n;}
#define I(c,d,s,o,i) {c,d,s,o,i}
#define EXIT I(0x95,0,0,0,0)
static void load(unsigned type,struct kernel_bpf_insn *code,unsigned count)
{
    program=(struct kernel_bpf_program){.object={.heap=&heap,.references=1},.type=type,.instructions=code,.count=count};
    bpf_runtime_environment(&program);assert(!kernel_bpf_verify(code,count,&program.environment));
    assert(!bpf_runtime_prepare(&program));
}
static void unload(void){bpf_runtime_release(&program);assert(!allocations && program.object.references==1);}
static int run(union bpf_attribute input,unsigned size,unsigned start,unsigned end,unsigned fail)
{
    user=input;output_base=(uintptr_t)&user;output_start=start;output_end=end;fail_write=fail;writes=0;
    return bpf_program_test(NULL,&files,NULL,(uintptr_t)&user,size,&input);
}
int main(void)
{
    struct kernel_bpf_insn raw[]={I(0x79,0,1,8,0),EXIT};uint64_t ctx[12]={0,91};
    load(17,raw,2);assert(program.max_context_size==16);
    union bpf_attribute a={.test={.fd=7,.context_in=(uintptr_t)ctx,.context_in_size=8}};
    assert(run(a,80,0,80,0)==-KERNEL_EINVAL && !program.run_count && !writes);
    a.test.context_in_size=17;assert(!run(a,80,4,8,0) && user.test.result==91 && writes==1 && write_offsets[0]==4);
    a.test.context_in_size=97;assert(run(a,80,0,80,0)==-KERNEL_EINVAL);unload();
    struct kernel_bpf_insn constant[]={I(0xb7,0,0,0,73),EXIT};load(17,constant,2);
    a=(union bpf_attribute){.test={.fd=7}};
    assert(!run(a,4,4,8,0) && user.test.result==73 && !program.max_context_size);
    ((unsigned char *)&a)[76]=1;assert(run(a,80,0,80,0)==-KERNEL_EINVAL && !writes);
    unload();
    unsigned char data[34]={0},out[34];data[12]=8;data[14]=0x45;
    struct kernel_bpf_insn len[]={I(0x61,0,1,0,0),EXIT};load(1,len,2);
    a=(union bpf_attribute){.test={.fd=7,.data_in_size=34,.data_in=(uintptr_t)data,.data_out=(uintptr_t)out}};
    assert(!run(a,40,0,40,0) && user.test.result==20 && user.test.data_out_size==34);
    assert(writes==3 && write_offsets[0]==12 && write_offsets[1]==4 && write_offsets[2]==36);
    assert(!memcmp(out,(unsigned char[14]){0},14) && out[14]==0x45);
    a.test.result=99;a.test.data_out_size=0;assert(run(a,40,0,40,2)==-KERNEL_EFAULT);
    assert(writes==2 && user.test.data_out_size==34 && user.test.result==99);
    a.test.data_out_size=1;assert(run(a,40,0,40,0)==-KERNEL_ENOSPC);unload();
    struct kernel_bpf_insn abs[]={I(0xbf,6,1,0,0),I(0x30,0,0,0,-0x200000+12),EXIT};
    load(1,abs,3);a.test.data_out_size=34;assert(!run(a,40,0,40,0) && user.test.result==8);unload();
    struct kernel_bpf_insn cb[]={I(0x61,0,1,48,0),I(0x04,0,0,0,1),I(0x63,1,0,48,0),EXIT};
    load(1,cb,4);a.test.repeat=3;assert(!run(a,40,0,40,0) && user.test.result==3);unload();
    puts("BPF TEST_RUN core PASS: verifier context bounds, actual VM, field writes and Ethernet view");
}
