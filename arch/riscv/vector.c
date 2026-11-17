#include <arch/riscv/vector.h>
#include <arch/riscv/trap.h>
#include <kernel/dtb.h>
#include <kernel/task_state.h>
#include <kernel/heap.h>
#include <kernel/uaccess.h>
#include <kernel/errno.h>
#include <string.h>

uint64_t riscv_vector_vlenb;
/* Kept in assembly with the register moves: the C translation unit never
 * needs a vector target ISA, and ownership tests can model the hardware. */
uintptr_t riscv_vector_status_hw(void);
void riscv_vector_off_hw(void);
uint64_t riscv_vector_probe_hw(void);
void riscv_vector_boot(uint32_t features)
{
    riscv_vector_vlenb=0;
    riscv_vector_off_hw();
    if (!(features&DTB_CPU_ISA_V)) return;
    uint64_t n=riscv_vector_probe_hw();
    /* Architectural RVV 1.0 bounds. Vendor 0.7 encodings are not V. */
    if (n>=16 && n<=8192 && !(n&(n-1))) riscv_vector_vlenb=n;
}
int riscv_vector_check_hart(void)
{
    riscv_vector_off_hw();
    return !riscv_vector_vlenb || riscv_vector_probe_hw()==riscv_vector_vlenb;
}
static int allocate(struct kernel_task *t)
{
    struct riscv_vector_state *v;
    if (kernel_heap_allocate_zeroed_nosleep(t->files.heap,1,
            sizeof(*v)+32*riscv_vector_vlenb,(void **)&v)) return -KERNEL_ENOMEM;
    v->heap=t->files.heap; v->vlenb=riscv_vector_vlenb;
    t->vector=v;
    return 0;
}
void riscv_vector_save(struct kernel_task *t)
{
    if(t->vector && (riscv_vector_status_hw()&RISCV_SSTATUS_VS_MASK)==RISCV_SSTATUS_VS_MASK)
        riscv_vector_save_hw(t->vector);
}
void riscv_vector_restore(struct kernel_task *t)
{
    if(t->vector) riscv_vector_restore_hw(t->vector);
    else riscv_vector_off_hw();
}
void riscv_vector_switch(struct kernel_task *old,struct kernel_task *next)
{
    if(!old->vector && !next->vector)return;
    /* Never retain a cross-hart owner. The outgoing task remains on_cpu
     * until the scheduler handoff; the incoming image is exclusively ours. */
    riscv_vector_save(old);
    riscv_vector_restore(next);
}
int riscv_vector_clone(struct kernel_task *child,struct kernel_task *parent)
{
    child->vector_control=parent->vector_control;
    if(!parent->vector)return 0;
    if(allocate(child))return -KERNEL_ENOMEM;
    riscv_vector_save(parent);
    memcpy(child->vector,parent->vector,sizeof(*child->vector)+32*riscv_vector_vlenb);
    child->vector->heap=child->files.heap;
    return 0;
}
void riscv_vector_release(struct kernel_task *t)
{
    if(!t->vector)return;
    struct riscv_vector_state *v=t->vector;t->vector=0;
    kernel_heap_release(v->heap,v);
}
void riscv_vector_exec(struct kernel_task *t)
{
    riscv_vector_off_hw(); riscv_vector_release(t);
    unsigned next=(t->vector_control>>2)&3;
    t->vector_control=(next ? next : 2) | ((t->vector_control&16) ? (t->vector_control&28) : 0);
}
static int vector_instruction(uint32_t i)
{
    if((i&3)!=3 || (i&0x1f)==0x1f)return 0;
    unsigned op=i&127,width=(i>>12)&7,csr=i>>20;
    if(op==0x57)return 1;
    if(op==7 || op==0x27)return width==0 || width>=5;
    return op==0x73 && ((csr>=8 && csr<=15) || (csr>=0xc20 && csr<=0xc22));
}
int riscv_vector_first_use(struct riscv_trap_frame *f)
{
    struct kernel_task *t=kernel_task_current();
    if(!riscv_vector_vlenb || (t->vector_control&3)==1 || t->vector || (f->sstatus&RISCV_SSTATUS_VS_MASK))return 0;
    uint32_t i=(uint32_t)f->stval;size_t copied;
    if(!i && (kernel_copy_from_user(&t->mm,&i,f->sepc,4,&copied) || copied!=4))return 0;
    if(!vector_instruction(i))return 0;
    if(allocate(t)) {
        kernel_signal_force_fault(t,7,128,0); /* Linux force_sig(SIGBUS), SI_KERNEL. */
        return 1;
    }
    riscv_vector_restore(t);
    return 1; /* retry the same instruction */
}
/* ptrace accepts only executable RVV 1.0 configurations, not vill. Bound
 * by division before multiplication so hostile VL cannot overflow. */
int riscv_vector_csr_valid(const uint64_t *r)
{
    if(r[4]!=riscv_vector_vlenb || (r[3]&~UINT64_C(7)) || (r[2]&~UINT64_C(255)))return 0;
    unsigned sew=(r[2]>>3)&7,lmul=r[2]&7;
    if(sew>3 || lmul==4)return 0;
    uint64_t max=(riscv_vector_vlenb*8)>>(sew+3);
    if(lmul<4)max<<=lmul;else max>>=8-lmul;
    return max && r[1]<=max;
}

int64_t riscv_vector_ptrace(struct kernel_task *caller,struct kernel_task *t,
    uint64_t address,int writing)
{
    struct {uint64_t base,length;} iov;
    size_t copied;
    if(kernel_copy_from_user(&caller->mm,&iov,address,16,&copied) || copied!=16)return -KERNEL_EFAULT;
    if(!riscv_vector_vlenb || (iov.length&7))return -KERNEL_EINVAL;
    if(!t->vector)return -KERNEL_ENODATA;
    size_t len=40+32*riscv_vector_vlenb;
    if(iov.length>len)iov.length=len;
    int error=0;
    if(!writing) {
        /* Target is stopped and exclusively borrowed by ptrace. Transfer
         * straight from its image; GET never allocates a second vector bank. */
        size_t header=iov.length<40 ? iov.length : 40;
        if(header && (kernel_copy_to_user(&caller->mm,iov.base,t->vector,header,&copied) || copied!=header))
            return -KERNEL_EFAULT;
        if(iov.length>40 && (iov.base>UINT64_MAX-40 ||
            kernel_copy_to_user(&caller->mm,iov.base+40,t->vector->regs,iov.length-40,&copied) || copied!=iov.length-40))
            return -KERNEL_EFAULT;
    } else if(iov.length) {
        void *buffer;
        if(kernel_heap_allocate_nosleep(caller->files.heap,len,&buffer))return -KERNEL_ENOMEM;
        memcpy(buffer,t->vector,40);
        memcpy((char *)buffer+40,t->vector->regs,len-40);
        if(kernel_copy_from_user(&caller->mm,buffer,iov.base,iov.length,&copied) || copied!=iov.length)
            error=-KERNEL_EFAULT;
        else if(!riscv_vector_csr_valid(buffer))error=-KERNEL_EINVAL;
        else {
            memcpy(t->vector,buffer,40);
            memcpy(t->vector->regs,(char *)buffer+40,len-40);
        }
        kernel_heap_release(caller->files.heap,buffer);
    }
    if(error)return error;
    return address>UINT64_MAX-8 || kernel_copy_to_user(&caller->mm,address+8,&iov.length,8,&copied) || copied!=8 ? -KERNEL_EFAULT:0;
}

int64_t riscv_vector_control(struct kernel_task *t,unsigned op,uint64_t value)
{
    if(!riscv_vector_vlenb)return -KERNEL_EINVAL;
    if(op==70)return t->vector_control;
    unsigned cur=value&3,next=(value>>2)&3;
    if((value&~UINT64_C(31)) || cur==3 || next==3)return -KERNEL_EINVAL;
    if(cur==1 && (t->vector_control&3)!=1)return -KERNEL_EPERM;
    if(!cur)cur=t->vector_control&3;
    t->vector_control=(unsigned)(value&28)|cur;
    return 0;
}
