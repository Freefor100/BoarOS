/* Ownership/failure model of production vector.c; register instructions are
 * exercised by the separate QEMU guest, not by these hardware stand-ins. */
#include <arch/riscv/vector.h>
#include <arch/riscv/trap.h>
#include <kernel/task_state.h>
#include <kernel/heap.h>
#include <kernel/uaccess.h>
#include <kernel/errno.h>
#include <kernel/dtb.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static struct kernel_task current,child,idle;
static struct kernel_heap heap;
static uint64_t hw_status,hw_vlen=16;
static unsigned char hw[262144];
static unsigned allocs,saves,restores,live,fail,signal_number,reads;
struct kernel_task *kernel_task_current(void){return &current;}
uintptr_t riscv_vector_status_hw(void){reads++;return hw_status;}
void riscv_vector_off_hw(void){hw_status=0;}
uint64_t riscv_vector_probe_hw(void){return hw_vlen;}
void riscv_vector_save_hw(struct riscv_vector_state *v)
{assert(v&&v->vlenb==riscv_vector_vlenb);memcpy(v->regs,hw,32*v->vlenb);hw_status=0x400;saves++;}
void riscv_vector_restore_hw(const struct riscv_vector_state *v)
{assert(v&&v->vlenb==riscv_vector_vlenb);memcpy(hw,v->regs,32*v->vlenb);hw_status=0x400;restores++;}
enum kernel_heap_status kernel_heap_allocate_zeroed_nosleep(struct kernel_heap *h,size_t a,size_t b,void **p)
{assert(h==&heap);allocs++;if(fail)return KERNEL_HEAP_STATUS_EMPTY;*p=calloc(a,b);assert(*p);live++;return 0;}
enum kernel_heap_status kernel_heap_allocate_nosleep(struct kernel_heap *h,size_t n,void **p)
{return kernel_heap_allocate_zeroed_nosleep(h,1,n,p);}
void kernel_heap_release(struct kernel_heap *h,void *p)
{assert(h==&heap && p && live);live--;free(p);}
enum kernel_uaccess_status kernel_copy_from_user(struct kernel_mm *m,void *d,uint64_t s,size_t n,size_t *done)
{(void)m;if(!s){*done=0;return KERNEL_UACCESS_STATUS_FAULT;}memcpy(d,(void *)(uintptr_t)s,n);*done=n;return 0;}
enum kernel_uaccess_status kernel_copy_to_user(struct kernel_mm *m,uint64_t d,const void *s,size_t n,size_t *done)
{(void)m;if(!d){*done=0;return KERNEL_UACCESS_STATUS_FAULT;}memcpy((void *)(uintptr_t)d,s,n);*done=n;return 0;}
void kernel_signal_force_fault(struct kernel_task *t,uint32_t sig,int32_t code,uint64_t pc)
{assert(t==&current);(void)code;(void)pc;signal_number=sig;}
static struct riscv_trap_frame first={.stval=0xc2202573};
int main(void)
{
    current.files.heap=child.files.heap=&heap;current.vector_control=2;
    riscv_vector_boot(0);assert(!riscv_vector_vlenb && !riscv_vector_first_use(&first));
    for(unsigned n=16;n<=8192;n*=2) {
        hw_vlen=n;riscv_vector_boot(DTB_CPU_ISA_V);assert(riscv_vector_vlenb==n);
        assert(riscv_vector_check_hart());hw_vlen=n*2;assert(!riscv_vector_check_hart());hw_vlen=n;
        uint64_t empty[5];struct {uint64_t base,length;} empty_io={(uintptr_t)empty,sizeof(empty)};
        assert(riscv_vector_ptrace(&current,&child,(uintptr_t)&empty_io,0)==-KERNEL_ENODATA);
        unsigned a=allocs,r=reads;
        riscv_vector_switch(&current,&idle);assert(allocs==a && reads==r);
        struct riscv_trap_frame invalid={.stval=0x13};assert(!riscv_vector_first_use(&invalid));
        fail=1;signal_number=0;assert(riscv_vector_first_use(&first));assert(signal_number==7&&!current.vector&&!live);
        fail=0;assert(riscv_vector_first_use(&first)&&current.vector&&live==1);
        assert(!current.vector->vtype && !current.vector->vstart && !current.vector->vl && !current.vector->vcsr);
        for(unsigned i=0;i<32*n;i++)assert(!hw[i]);
        memset(hw,0x39,32*n);hw_status=0x600;
        fail=1;assert(riscv_vector_clone(&child,&current)==-KERNEL_ENOMEM&&!child.vector&&live==1);
        fail=0;assert(!riscv_vector_clone(&child,&current)&&live==2);
        assert(!memcmp(child.vector->regs,hw,32*n));
        unsigned saved=saves;riscv_vector_save(&current);assert(saved==saves);
        memset(hw,0x7a,32*n);hw_status=0x600;riscv_vector_switch(&current,&child);
        for(unsigned i=0;i<32*n;i++)assert(hw[i]==0x39&&current.vector->regs[i]==0x7a);
        uint64_t rset[5]={0,UINT64_MAX,0,0,n};assert(!riscv_vector_csr_valid(rset));
        rset[1]=n;assert(riscv_vector_csr_valid(rset));rset[4]=n*2;assert(!riscv_vector_csr_valid(rset));
        size_t bytes=40+32*n;unsigned char *buf=malloc(bytes);assert(buf);
        struct {uint64_t base,length;} io={(uintptr_t)buf,bytes};
        assert(!riscv_vector_ptrace(&current,&child,(uintptr_t)&io,0));
        assert(io.length==bytes&&!memcmp(buf+40,child.vector->regs,bytes-40));
        fail=1;assert(!riscv_vector_ptrace(&current,&child,(uintptr_t)&io,0));assert(riscv_vector_ptrace(&current,&child,(uintptr_t)&io,1)==-KERNEL_ENOMEM&&live==2);fail=0;free(buf);
        riscv_vector_release(&child);riscv_vector_exec(&current);assert(!live&&!current.vector&&!child.vector&&!hw_status);
    }
    for(unsigned n=0;n<16;n++){riscv_vector_vlenb=0;hw_vlen=n;riscv_vector_boot(DTB_CPU_ISA_V);assert(!riscv_vector_vlenb);}
    const unsigned invalid[]={24,8193,16384};
    for(unsigned i=0;i<3;i++){hw_vlen=invalid[i];riscv_vector_boot(DTB_CPU_ISA_V);assert(!riscv_vector_vlenb);}
    assert(restores&&saves);puts("RVV host allocation, failure, all VLEN bounds, CSR and ownership PASS");
}
