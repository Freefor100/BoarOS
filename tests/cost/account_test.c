#include <kernel/cost.h>
#include <assert.h>
#include <stdio.h>
static struct kernel_cost_task foreground, background;
static struct kernel_cost_task *actor=&foreground;
static uint64_t ticks=100;
uint64_t kernel_cost_clock(void) { return ticks; }
uint64_t kernel_cost_lock(void) { return 0; }
void kernel_cost_unlock(uint64_t state) { (void)state; }
struct kernel_cost_task *kernel_cost_current(void) { return actor; }
/* Stable scheduler-event interfaces, implemented by C2. */
void kernel_cost_block(struct kernel_cost_task *task);
void kernel_cost_wake(struct kernel_cost_task *task);
int main(void)
{
    assert(kernel_cost_begin(1,10000000,0,0)==0);
    kernel_cost_join(&foreground);
    kernel_cost_switch(&background,&foreground);
    ticks=110; kernel_cost_block(&foreground);
    actor=&background; kernel_cost_switch(&foreground,&background);
    ticks=990; kernel_cost_wake(&foreground);
    ticks=1000; actor=&foreground; kernel_cost_switch(&background,&foreground);
    ticks=1010; kernel_cost_account(&foreground);
    assert(kernel_cost_end(1,0)==0);
    uint64_t value;
    assert(kernel_cost_read(0,COST_RUN_TICKS,&value)==0 && value==20);
    assert(kernel_cost_read(1,COST_RUN_TICKS,&value)==0 && value==890);
    assert(kernel_cost_read(0,COST_BLOCKED_TICKS,&value)==0 && value==880);
    assert(kernel_cost_read(0,COST_READY_TICKS,&value)==0 && value==10);
    puts("cost runtime excludes sleep and attributes the actual owner");
}
