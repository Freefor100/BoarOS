#include <kernel/sched_policy.h>
#include <kernel/errno.h>
#include <assert.h>
#include <stdio.h>
#define MS UINT64_C(1000000)
int main(void)
{
    struct kernel_sched_policy fifo={0}, rr={0}, child={0};
    assert(kernel_sched_policy_set(&fifo,1,50,0)==0);
    assert(kernel_sched_policy_set(&rr,2,50,0)==0);
    kernel_sched_policy_charge(&fifo,1000*MS);
    assert(!kernel_sched_policy_expired(&fifo));
    kernel_sched_policy_charge(&rr,40*MS);
    /* Higher priority runs elsewhere: only this task's execution is charged. */
    assert(kernel_sched_policy_set(&rr,1,50,0)==0);
    assert(kernel_sched_policy_set(&rr,2,50,0)==0);
    assert(rr.rr_remaining_ns==60*MS); /* policy round-trip does not refill */
    kernel_sched_policy_charge(&rr,59*MS);
    assert(!kernel_sched_policy_expired(&rr));
    kernel_sched_policy_charge(&rr,1*MS);
    assert(kernel_sched_policy_expired(&rr));
    kernel_sched_policy_rotate(&rr);
    assert(rr.rr_remaining_ns==100*MS);
    assert(kernel_sched_policy_set(&fifo,1|0x40000000,99,0)==0);
    kernel_sched_policy_fork(&child,&fifo);
    assert(child.policy==0 && child.priority==0 && !child.reset_on_fork);
    assert(fifo.policy==1 && fifo.priority==99 && fifo.reset_on_fork);
    assert(kernel_sched_policy_set(&rr,0,1,0)==-KERNEL_EINVAL);
    assert(kernel_sched_policy_set(&rr,1,0,0)==-KERNEL_EINVAL);
    assert(kernel_sched_policy_set(&rr,2,100,0)==-KERNEL_EINVAL);
    struct kernel_rt_bandwidth budget;
    kernel_rt_bandwidth_init(&budget,0);
    kernel_rt_bandwidth_account(&budget,940*MS,1);
    assert(kernel_rt_bandwidth_eligible(&budget));
    kernel_rt_bandwidth_account(&budget,950*MS,1);
    assert(!kernel_rt_bandwidth_eligible(&budget));
    kernel_rt_bandwidth_account(&budget,999*MS,0);
    assert(!kernel_rt_bandwidth_eligible(&budget));
    kernel_rt_bandwidth_account(&budget,1000*MS,0);
    assert(kernel_rt_bandwidth_eligible(&budget) && budget.consumed_ns==0);
    kernel_rt_bandwidth_account(&budget,1100*MS,1);
    kernel_rt_bandwidth_account(&budget,1500*MS,0);
    assert(budget.consumed_ns==100*MS);
    assert(kernel_rt_bandwidth_set(&budget,1,50000,1500*MS,0)==0);
    assert(!kernel_rt_bandwidth_eligible(&budget) && budget.consumed_ns==100*MS);
    assert(kernel_rt_bandwidth_set(&budget,1,-1,1500*MS,0)==0);
    assert(kernel_rt_bandwidth_eligible(&budget));
    assert(kernel_rt_bandwidth_set(&budget,1,50000,1500*MS,0)==0);
    assert(!kernel_rt_bandwidth_eligible(&budget));
    assert(kernel_rt_bandwidth_set(&budget,0,2000000,1500*MS,0)==0);
    assert(budget.consumed_ns==100*MS && !kernel_rt_bandwidth_eligible(&budget));
    kernel_rt_bandwidth_account(&budget,10000*MS,0);
    assert(budget.consumed_ns==0); /* Missed periods do not accumulate credit. */
    assert(kernel_rt_bandwidth_set(&budget,0,0,10000*MS,0)==-KERNEL_EINVAL);
    assert(kernel_rt_bandwidth_set(&budget,1,-2,10000*MS,0)==-KERNEL_EINVAL);
    assert(kernel_rt_bandwidth_set(&budget,1,2000001,10000*MS,0)==-KERNEL_EINVAL);
    assert(kernel_rt_bandwidth_set(&budget,0,INT64_C(2147483648),10000*MS,0)==-KERNEL_EINVAL);
    uint64_t phase=budget.period_start_ns;
    assert(kernel_rt_bandwidth_set(&budget,0,2000000,10000*MS,0)==0);
    assert(budget.period_start_ns==phase);
    assert(kernel_rt_bandwidth_set(&budget,1,0,10000*MS,0)==0);
    assert(!kernel_rt_bandwidth_eligible(&budget));
    assert(kernel_rt_bandwidth_set(&budget,1,50000,10000*MS,0)==0);
    kernel_rt_bandwidth_account(&budget,10001*MS,1);
    assert(budget.consumed_ns==1*MS);
    kernel_rt_bandwidth_init(&budget,0);
    assert(kernel_rt_bandwidth_set(&budget,1,250,0,0)==0);
    assert(kernel_rt_bandwidth_set(&budget,0,500,0,0)==0);
    kernel_rt_bandwidth_account(&budget,100000,1);
    assert(kernel_rt_bandwidth_delay(&budget,1,1)==150000);
    kernel_rt_bandwidth_account(&budget,250000,1);
    assert(!kernel_rt_bandwidth_eligible(&budget));
    assert(kernel_rt_bandwidth_delay(&budget,0,1)==250000);
    kernel_rt_bandwidth_account(&budget,500000,0);
    assert(kernel_rt_bandwidth_eligible(&budget));
    assert(kernel_rt_bandwidth_delay(&budget,1,1)==250000);
    puts("scheduler policy execution accounting PASS");
}
