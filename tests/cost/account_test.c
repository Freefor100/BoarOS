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
    struct kernel_cost_io_scope io=kernel_cost_io_enter(0);
    assert((foreground.operation & 15)==1);
    struct kernel_cost_io_scope file=kernel_cost_phase_enter(5);
    assert(foreground.operation==81);
    struct kernel_cost_io_scope metadata=kernel_cost_phase_enter(6);
    assert(foreground.operation==97);
    kernel_cost_io_leave(&metadata);assert(foreground.operation==81);
    kernel_cost_io_leave(&file);assert(foreground.operation==1);
    kernel_cost_io_leave(&io);assert(!foreground.operation);
    ticks=110; kernel_cost_block(&foreground);
    actor=&background; kernel_cost_switch(&foreground,&background);
    ticks=990; kernel_cost_wake(&foreground); kernel_cost_timeout(&foreground,980);
    ticks=1000; actor=&foreground; kernel_cost_switch(&background,&foreground);
    ticks=1010; kernel_cost_account(&foreground);
    assert(kernel_cost_end(1,0)==0);
    uint64_t value;
    assert(kernel_cost_read(0,COST_RUN_TICKS,&value)==0 && value==20);
    assert(kernel_cost_read(1,COST_RUN_TICKS,&value)==0 && value==890);
    assert(kernel_cost_read(0,COST_BLOCKED_TICKS,&value)==0 && value==880);
    assert(kernel_cost_read(0,COST_READY_TICKS,&value)==0 && value==10);
    assert(kernel_cost_read(0,COST_DEADLINE_TO_RUN,&value)==0 && value==20);
    foreground = (struct kernel_cost_task){0};actor=&foreground;
    ticks=0;assert(kernel_cost_begin(1,10000000,1,0)==0);
    ticks=100;assert(kernel_cost_end(1,0)==0);
    assert(kernel_cost_read(0,COST_RUN_TICKS,&value)==0 && value==100);
    puts("cost runtime excludes sleep and attributes the actual owner");
}
