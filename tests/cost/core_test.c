#include <kernel/cost.h>
#include <kernel/errno.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct kernel_cost_task actor;
static uint64_t clock_tick = 100;
uint64_t kernel_cost_lock(void) { return 0; }
void kernel_cost_unlock(uint64_t status) { (void)status; }
uint64_t kernel_cost_clock(void) { return clock_tick; }
struct kernel_cost_task *kernel_cost_current(void) { return &actor; }
static char output[1048576];
int main(void)
{
    assert(kernel_cost_end(9, 0) == -KERNEL_EINVAL);
    assert(kernel_cost_begin(9, 10000000, 1, 0) == 0);
    uint64_t first = kernel_cost_epoch();
    actor.epoch = first;
    assert(first == 1);
    assert(kernel_cost_begin(9, 10000000, 1, 0) == -KERNEL_EBUSY);
    kernel_cost_add(COST_OPERATIONS, 2);
    struct kernel_cost_scope scope = kernel_cost_enter(COST_OPERATION_TICKS);
    clock_tick += 16;
    assert(kernel_cost_end(10, 0) == -KERNEL_EPERM);
    assert(kernel_cost_end(9, 0) == -KERNEL_EBUSY);
    kernel_cost_leave(&scope);
    assert(kernel_cost_end(9, 0) == 0);
    assert(kernel_cost_format(output, sizeof(output)) > 0);
    assert(strstr(output, "version=1\n"));
    assert(strstr(output, "foreground.operations.value=2\n"));
    assert(strstr(output, "foreground.operation_ticks.max=16\n"));
    assert(strstr(output, "foreground.operation_ticks.bucket.5=1\n"));
    assert(kernel_cost_begin(9, 10000000, 0, 0) == 0);
    assert(kernel_cost_epoch() == first + 1);
    /* Old actor cannot contaminate a reused window's foreground. */
    kernel_cost_add(COST_OPERATIONS, 1);
    actor.epoch = kernel_cost_epoch();
    kernel_cost_sample(COST_OPERATION_TICKS, 0);
    kernel_cost_sample(COST_OPERATION_TICKS, UINT64_MAX);
    kernel_cost_add(COST_OPERATIONS, UINT64_MAX);
    kernel_cost_add(COST_OPERATIONS, 1);
    assert(kernel_cost_end(9, 0) == 0);
    assert(kernel_cost_format(output, sizeof(output)) > 0);
    assert(strstr(output, "overflow=1\n"));
    assert(strstr(output, "background.operations.value=1\n"));
    assert(strstr(output, "foreground.operation_ticks.bucket.64=1\n"));
    assert(kernel_cost_begin(9, 10000000, 1, 0) == 0);
    actor.epoch = kernel_cost_epoch();
    scope = kernel_cost_enter(COST_OPERATION_TICKS);
    kernel_cost_cancel(&actor);
    kernel_cost_abort(9);
    kernel_cost_leave(&scope); /* Stale/cancelled stack scope is harmless. */
    assert(kernel_cost_format(output, sizeof(output)) > 0);
    assert(strstr(output, "state=incomplete\n"));
    assert(kernel_cost_begin(9, 10000000, 0, 1) == 0);
    actor.epoch = kernel_cost_epoch();
    kernel_cost_add(COST_OPERATIONS, 10); /* Pending begin is not sampled. */
    assert(kernel_cost_begin(9, 10000000, 0, 1) == -KERNEL_EBUSY);
    kernel_cost_boundary();
    assert(kernel_cost_format(output, sizeof(output)) == -KERNEL_EBUSY);
    kernel_cost_add(COST_OPERATIONS, 1);
    actor.suppress = 1;
    kernel_cost_add(COST_OPERATIONS, 20); /* Observer/control path is excluded. */
    assert(kernel_cost_end(9, 1) == 0);
    assert(kernel_cost_format(output, sizeof(output)) == -KERNEL_EBUSY);
    kernel_cost_boundary(); actor.suppress = 0;
    assert(kernel_cost_format(output, sizeof(output)) > 0);
    assert(strstr(output, "foreground.operations.value=1\n"));
    assert(kernel_cost_format(output, 10) == -KERNEL_EOVERFLOW);
    for (unsigned background = 0; background < 2; background++) {
        assert(kernel_cost_begin(9, 10000000, 0, 0) == 0);
        actor.epoch = background ? kernel_cost_epoch() : 0;
        actor.wait_flags = background ? 64 : 0;
        scope = kernel_cost_enter(COST_OPERATION_TICKS);
        kernel_cost_cancel(&actor);
        assert(kernel_cost_end(9, 0) == 0);
        uint64_t value;
        assert(kernel_cost_read(0, COST_CANCELLED, &value) == 0 && value == 0);
        assert(kernel_cost_read(1, COST_CANCELLED, &value) == 0 && value == 1);
        kernel_cost_leave(&scope);
    }
    puts("cost core contract passed");
}
