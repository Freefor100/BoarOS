#include <kernel/cost.h>
#include <assert.h>
#include <stdio.h>
static struct kernel_cost_task foreground, background;
static struct kernel_cost_task *actor=&foreground;
static uint64_t ticks=100;
uint64_t kernel_cost_clock(void){return ticks;}
uint64_t kernel_cost_lock(void){return 0;}
void kernel_cost_unlock(uint64_t s){(void)s;}
struct kernel_cost_task *kernel_cost_current(void){return actor;}
int main(void)
{
    assert(kernel_cost_begin(1,10000000,0,0)==0);kernel_cost_join(&foreground);
    kernel_cost_irq_disabled(100);ticks=110;kernel_cost_irq_disabled(110);
    actor=&background;kernel_cost_irq_disabled(120);ticks=200;kernel_cost_irq_enabled(200);
    actor=&foreground;kernel_cost_irq_disabled(210);kernel_cost_irq_suppress();kernel_cost_irq_enabled(220);
    assert(kernel_cost_end(1,0)==0);uint64_t v;
    assert(kernel_cost_read(0,COST_IRQ_OFF_TICKS,&v)==0 && v==100);
    assert(kernel_cost_read(1,COST_IRQ_OFF_TICKS,&v)==0 && v==0);
    assert(kernel_cost_begin(1,10000000,0,0)==0);kernel_cost_join(&foreground);
    kernel_cost_irq_disabled(230);kernel_cost_irq_enabled(240);assert(kernel_cost_end(1,0)==0);
    assert(kernel_cost_read(0,COST_IRQ_OFF_TICKS,&v)==0 && v==10);
    puts("hart IRQ intervals survive nesting and task changes, exclude control and old epochs");
}
