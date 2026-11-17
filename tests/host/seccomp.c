#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel/seccomp.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/task_state.h>
#include <kernel/syscall.h>
#include <kernel/credentials.h>
#include <kernel/uaccess.h>
static int live,fail_after=-1,signal_seen,group_seen;
static jmp_buf killed;
enum kernel_heap_status kernel_heap_allocate(struct kernel_heap *h,size_t n,void **p)
{(void)h;if(fail_after==0)return KERNEL_HEAP_STATUS_EMPTY;if(fail_after>0)--fail_after;*p=malloc(n);if(!*p)return KERNEL_HEAP_STATUS_EMPTY;++live;return 0;}
void kernel_heap_release(struct kernel_heap *h,void *p){(void)h;--live;free(p);}
int kernel_credentials_capable(const struct kernel_credentials *c,uint32_t cap){(void)c;(void)cap;return 0;}
enum kernel_uaccess_status kernel_copy_from_user(struct kernel_mm *mm,void *to,uint64_t from,size_t n,size_t *copied)
{(void)mm;if(from<4096){*copied=0;return KERNEL_UACCESS_STATUS_FAULT;}memcpy(to,(void *)(uintptr_t)from,n);*copied=n;return 0;}
void kernel_seccomp_terminate(unsigned signal,int group){signal_seen=signal;group_seen=group;longjmp(killed,1);}
static void *alloc(void *h,size_t n){void *p=0;return kernel_heap_allocate(h,n,&p)?0:p;}
static void drop(void *h,void *p){kernel_heap_release(h,p);}
#define I(c,jt,jf,k) {c,jt,jf,k}
static int attach(struct kernel_task *t,struct kernel_bpf_classic_insn *insns,unsigned count)
{struct {uint16_t n,pad[3];uint64_t p;} f={.n=count,.p=(uintptr_t)insns};return kernel_seccomp_set(t,2,0,(uintptr_t)&f);}
int main(void)
{
    const struct kernel_bpf_vm_env env={.alloc=alloc,.free=drop};
    struct kernel_bpf_classic_insn a[]={I(0x20,0,0,16),I(0x02,0,0,0),I(0x01,0,0,3),
        I(0x0c,0,0,0),I(0x24,0,0,4),I(0x34,0,0,2),I(0x94,0,0,7),
        I(0x54,0,0,3),I(0x44,0,0,8),I(0xa4,0,0,2),I(0x64,0,0,2),I(0x74,0,0,1),
        I(0x15,0,1,18),I(0x06,0,0,0x7fff0000),I(0x06,0,0,0)};
    struct kernel_seccomp_data data={.args={2}};
    assert(!kernel_seccomp_validate(a,sizeof(a)/sizeof(a[0]),&env));
    assert(kernel_seccomp_run(a,sizeof(a)/sizeof(a[0]),&data)==0x7fff0000);
    struct kernel_bpf_classic_insn bad[]={I(0x15,0,1,1),I(0x02,0,0,0),I(0x60,0,0,0),I(0x06,0,0,0)};
    assert(kernel_seccomp_validate(bad,4,&env)==-KERNEL_EINVAL);
    struct kernel_bpf_classic_insn ret[]={I(0x06,0,0,0x7fff0000)};
    struct kernel_task *parent=calloc(1,sizeof(*parent)),*child=calloc(1,sizeof(*child));
    assert(attach(parent,ret,1)==-KERNEL_EACCES);parent->no_new_privs=1;
    for(int i=0;i<2;i++){fail_after=i;assert(attach(parent,ret,1)==-KERNEL_ENOMEM);assert(!parent->seccomp_filter&&!parent->seccomp_mode&&!live);}
    fail_after=-1;assert(!attach(parent,ret,1));kernel_seccomp_clone(child,parent);child->no_new_privs=1;assert(live==1);
    ret[0].k=0x50000|17;assert(!attach(parent,ret,1));assert(live==2);
    struct kernel_syscall_request request={.number=172};struct kernel_syscall_result result;
    assert(kernel_seccomp_check(parent,&request,&result)==1&&result.value==-17);
    assert(!kernel_seccomp_check(child,&request,&result));
    ret[0].k=0x12340000;assert(!attach(parent,ret,1));
    assert(kernel_seccomp_check(parent,&request,&result)==1&&result.value==-17);
    assert(!attach(child,ret,1));
    if(!setjmp(killed)){kernel_seccomp_check(child,&request,&result);assert(0);}assert(signal_seen==31&&group_seen==1);
    kernel_seccomp_release(parent);assert(live==2);kernel_seccomp_release(child);assert(!live);
    parent->seccomp_mode=0;
    ret[0].k=0x30000;assert(attach(parent,ret,1)==-KERNEL_EOPNOTSUPP);
    ret[0].code=0x16;assert(attach(parent,ret,1)==-KERNEL_EOPNOTSUPP);assert(!live);
    struct kernel_bpf_classic_insn *large=calloc(4096,sizeof(*large));
    large[4095]=(struct kernel_bpf_classic_insn)I(0x06,0,0,0x7fff0000);
    for(unsigned i=0;i<7;i++)assert(!attach(parent,large,4096));
    assert(attach(parent,large,4096)==-KERNEL_ENOMEM);assert(live==7);
    kernel_seccomp_release(parent);assert(!live);free(large);
    free(parent);free(child);puts("SECCOMP host PASS validator/interpreter/OOM/chain ownership");
}
