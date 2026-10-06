#include <arch/loongarch/fpu.h>
#include <platform/loongarch_virt.h>
#include <string.h>
void la_fpu_save_hw(struct arch_fpu_state *);
void la_fpu_restore_hw(const struct arch_fpu_state *);
static uint32_t configuration(void)
{ uint32_t value,index=2;__asm__ volatile("cpucfg %0,%1":"=r"(value):"r"(index));return value; }
static int supported(unsigned width)
{ return width==1 || (width==2 && (configuration()&64)) || (width==4 && (configuration()&128)); }
uint64_t la_user_hwcap(void)
{ uint32_t config=configuration();return 11U|((config&64) ? 16U : 0U)|((config&128) ? 32U : 0U); }
static void valid(const struct arch_fpu_state *state)
{ if(!state || !supported(state->width) || !supported(state->live_width) || state->width>state->live_width)la_virt_fatal("FP/SIMD owner state"); }
void la_fpu_save(struct arch_fpu_state *state)
{ valid(state);la_fpu_save_hw(state); }
void la_fpu_restore(const struct arch_fpu_state *state)
{ valid(state);la_fpu_restore_hw(state); }
int la_fpu_first_use(unsigned width)
{
    if(!supported(width))return 0;
    struct arch_fpu_state *state=arch_process_fpu_borrow_current();
    if(state->width) {
        if(width<=state->width)la_virt_fatal("FP disabled with live owner");
        la_fpu_save(state);
    }
    if(!state->saved && (width==1 || state->live_width<2)) {
        for(unsigned i=0;i<32;i++)state->regs[i][0]=UINT64_MAX;
        state->fcc=0;state->saved=1;
    }
    /* Linux 首次初始化更宽部分为全1；已 live 的上半部继续来自任务 image。 */
    unsigned old=state->live_width ? state->live_width : 1;
    for(unsigned i=0;i<32;i++)for(unsigned j=old;j<width;j++)state->regs[i][j]=UINT64_MAX;
    if(state->live_width<width)state->live_width=width;
    state->width=width;
    la_fpu_restore(state);return 1;
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
