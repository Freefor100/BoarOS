#include <arch/timer.h>
#include <arch/context.h>
static uint64_t period, tick_deadline, scheduler_deadline;
uint64_t arch_time_read(void)
{ uint64_t value; __asm__ volatile("rdtime.d %0, $zero" : "=r"(value)); return value; }
uint32_t la_timer_frequency(void)
{
    uint64_t base, ratio, index=4;
    __asm__ volatile("cpucfg %0, %1" : "=r"(base) : "r"(index)); index=5;
    __asm__ volatile("cpucfg %0, %1" : "=r"(ratio) : "r"(index));
    uint64_t multiplier=ratio&0xffff, divider=(ratio>>16)&0xffff;
    if (!divider || !multiplier || !base || base*multiplier/divider>UINT32_MAX) return 0;
    return (uint32_t)(base*multiplier/divider);
}
static void arm(void)
{
    uint64_t now=arch_time_read(), deadline=tick_deadline;
    if (scheduler_deadline && (int64_t)(scheduler_deadline-deadline)<0) deadline=scheduler_deadline;
    uint64_t delta=(int64_t)(deadline-now)>0 ? deadline-now : 1;
    /* TCFG 的最低两位是配置，deadline 向上取整而不提前触发。 */
    uint64_t config=((delta+3)&~UINT64_C(3))|1;
    __asm__ volatile("csrwr %0, 0x41" : "+r"(config) :: "memory");
}
enum arch_timer_status la_timer_start(uint32_t frequency,uint32_t ticks)
{
    if (!frequency || !ticks || frequency<ticks) return ARCH_TIMER_STATUS_INVALID_FREQUENCY;
    if (period) return ARCH_TIMER_STATUS_ALREADY_STARTED;
    period=frequency/ticks; tick_deadline=arch_time_read()+period; arm();
    uint64_t mask=UINT64_C(1)<<11, enabled=mask;
    __asm__ volatile("csrxchg %0, %1, 4" : "+r"(enabled) : "r"(mask) : "memory");
    return ARCH_TIMER_STATUS_OK;
}
enum arch_timer_status arch_timer_set_scheduler_deadline(uint64_t deadline)
{
    if (!period) return ARCH_TIMER_STATUS_NOT_STARTED;
    uintptr_t irq=arch_interrupt_save(); scheduler_deadline=deadline; arm(); arch_interrupt_restore(irq);
    return ARCH_TIMER_STATUS_OK;
}
enum arch_timer_status la_timer_interrupt(uint64_t *elapsed)
{
    if (!elapsed) return ARCH_TIMER_STATUS_INVALID_ARGUMENT;
    if (!period) return ARCH_TIMER_STATUS_NOT_STARTED;
    uint64_t clear=1; __asm__ volatile("csrwr %0, 0x44" : "+r"(clear) :: "memory");
    uint64_t now=arch_time_read(); *elapsed=0;
    if ((int64_t)(now-tick_deadline)>=0) {
        *elapsed=1+(now-tick_deadline)/period; tick_deadline+=*elapsed*period;
    }
    if (scheduler_deadline && (int64_t)(now-scheduler_deadline)>=0) scheduler_deadline=0;
    arm(); return ARCH_TIMER_STATUS_OK;
}
