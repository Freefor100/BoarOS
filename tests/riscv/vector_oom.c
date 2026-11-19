/* Test-only link wrappers fail the actual bank allocation inside the first
 * vector-owning clone, after the ordinary clone resources have been built. */
#include <arch/riscv/vector.h>
#include <arch/riscv/virt_uart.h>
#include <arch/riscv/trap.h>
#include <kernel/task_state.h>
#include <kernel/heap.h>
#include <kernel/errno.h>
static struct kernel_task *armed;
static unsigned injected,first_injected;
int __real_riscv_vector_first_use(struct riscv_trap_frame *);
int __real_riscv_vector_clone(struct kernel_task *,struct kernel_task *);
enum kernel_heap_status __real_kernel_heap_allocate_zeroed_nosleep(struct kernel_heap *,size_t,size_t,void **);
int __wrap_riscv_vector_first_use(struct riscv_trap_frame *frame)
{
    struct kernel_task *task=kernel_task_current();
    unsigned expected=0;
    int inject=riscv_vector_vlenb && !task->vector &&
        __atomic_compare_exchange_n(&first_injected,&expected,1,0,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED);
    if(inject)__atomic_store_n(&armed,task,__ATOMIC_RELEASE);
    int result=__real_riscv_vector_first_use(frame);
    if(inject) {
        if(__atomic_load_n(&armed,__ATOMIC_ACQUIRE) || result!=1 || task->vector)__builtin_trap();
        virt_uart_puts("BoarOS: VECTOR first-use allocation failure injected\n");
    }
    return result;
}
int __wrap_riscv_vector_clone(struct kernel_task *child,struct kernel_task *parent)
{
    unsigned expected=0;
    int inject=parent->vector && __atomic_compare_exchange_n(&injected,&expected,1,0,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED);
    if(inject)__atomic_store_n(&armed,parent,__ATOMIC_RELEASE);
    int result=__real_riscv_vector_clone(child,parent);
    if(inject) {
        if(__atomic_load_n(&armed,__ATOMIC_ACQUIRE) || result!=-KERNEL_ENOMEM || child->vector)__builtin_trap();
        virt_uart_puts("BoarOS: VECTOR clone allocation failure injected\n");
    }
    return result;
}
enum kernel_heap_status __wrap_kernel_heap_allocate_zeroed_nosleep(struct kernel_heap *h,size_t count,size_t bytes,void **out)
{
    struct kernel_task *owner=__atomic_load_n(&armed,__ATOMIC_ACQUIRE);
    if(owner && owner==kernel_task_current() && count==1 && bytes==48+32*riscv_vector_vlenb) {
        __atomic_store_n(&armed,0,__ATOMIC_RELEASE);
        return KERNEL_HEAP_STATUS_EMPTY;
    }
    return __real_kernel_heap_allocate_zeroed_nosleep(h,count,bytes,out);
}
