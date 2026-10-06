#include <arch/loongarch/fpu.h>
#include <platform/loongarch_virt.h>
#include <string.h>
void la_fpu_first_use(void)
{
    struct arch_fpu_state *state=arch_process_fpu_borrow_current();
    if(state->saved)la_virt_fatal("FP disabled with live owner");
    memset(state,0,sizeof(*state));state->saved=1;
    for(unsigned i=0;i<32;i++)state->regs[i]=UINT64_MAX; /* Linux _init_fpu 的初始 NaN image。 */
    la_fpu_restore(state);
}
uint32_t la_fpu_take_exception(void)
{
    uint32_t csr;__asm__ volatile("movfcsr2gr %0,$fcsr0":"=r"(csr));
    uint32_t pending=csr&((csr&31U)<<24);
    uint32_t cleared=csr&~pending;
    __asm__ volatile("movgr2fcsr $fcsr0,%0"::"r"(cleared));
    /* Linux 清除启用的 Cause，却按原始 Cause 集合选择 si_code。 */
    if(csr&(1U<<28))return 7; /* FPE_FLTINV 优先于其他同次原因。 */
    if(csr&(1U<<27))return 3;
    if(csr&(1U<<26))return 4;
    if(csr&(1U<<25))return 5;
    if(csr&(1U<<24))return 6;
    return 14; /* FPE_FLTUNK */
}
