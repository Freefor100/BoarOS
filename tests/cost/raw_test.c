#include <kernel/cost.h>
#include <kernel/raw_lock.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
_Thread_local uintptr_t sync_test_irq = 1;
static struct kernel_cpu cpu;
static struct kernel_cost_task task;
static uint64_t ticks;
void *sync_test_cpu(void) { return &cpu; }
uint64_t kernel_cost_clock(void) { return ++ticks; }
uint64_t kernel_cost_lock(void) { return arch_interrupt_save(); }
void kernel_cost_unlock(uint64_t saved) { arch_interrupt_restore(saved); }
struct kernel_cost_task *kernel_cost_current(void) { return &task; }
int main(void)
{
    struct kernel_raw_lock lock;
    kernel_cpu_initialize(&cpu, 7, 0);
    kernel_raw_lock_init(&lock, KERNEL_RAW_RANK_PAGE);
    assert(kernel_cost_begin(1, 10000000, 1, 0) == 0);
    { KERNEL_RAW_SCOPE(guard, &lock); ticks += 7; }
    assert(sync_test_irq && !cpu.preempt_depth && !cpu.raw_locks);
    assert(kernel_cost_end(1, 0) == 0);
    uint64_t wait, hold;
    assert(kernel_cost_read(0, COST_RAW_WAIT_TICKS, &wait) == 0 && wait > 0);
    assert(kernel_cost_read(0, COST_RAW_HOLD_TICKS, &hold) == 0 && hold >= 7);
    size_t capacity = kernel_cost_format_capacity();
    char *output = malloc(capacity); assert(output);
    assert(kernel_cost_format(output, capacity) > 0);
    assert(strstr(output, "foreground.raw_wait_ticks.samples=1\n"));
    assert(strstr(output, "foreground.raw_hold_ticks.samples=1\n"));
    free(output);
    { KERNEL_RAW_SCOPE(guard, &lock); ticks += 99; }
    uint64_t unchanged;
    assert(kernel_cost_read(0, COST_RAW_HOLD_TICKS, &unchanged) == 0 && unchanged == hold);
    puts("raw cost: acquisition/hold intervals, restored IRQ and inactive epochs passed");
}
