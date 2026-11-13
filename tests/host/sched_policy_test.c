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
    struct kernel_sched_policy batch={0},idle={0};
    assert(kernel_sched_policy_set(&batch,3,0,0)==0);
    assert(kernel_sched_policy_set(&idle,5,0,0)==0);
    assert(kernel_sched_policy_set(&idle,5,1,0)==-KERNEL_EINVAL);
    struct kernel_sched_policy normal={0},low={.nice=5};
    assert(kernel_sched_weight(&normal)==1024 && kernel_sched_weight(&low)==335);
    assert(kernel_sched_fair_delta(&normal,1000000)==1000000);
    uint64_t sum=0;for(unsigned i=0;i<1000;i++)sum+=kernel_sched_fair_delta(&low,1);
    assert(sum==1000*1024/335 && low.fair_remainder==1000*1024%335);
    idle.nice=-20;assert(kernel_sched_weight(&idle)==3);
    batch.nice=-5;batch.reset_on_fork=1;kernel_sched_policy_fork(&child,&batch);
    assert(child.policy==3 && child.nice==0 && !child.reset_on_fork);
    batch.nice=7;kernel_sched_policy_fork(&child,&batch);assert(child.nice==7);
    batch.reset_on_fork=0;batch.fair_slice_ns=2000000;
    batch.fair_slice_used=123;batch.fair_remainder=17;
    kernel_sched_policy_fork(&child,&batch);
    assert(child.fair_slice_ns==2000000 && !child.fair_slice_used && !child.fair_remainder);
    batch.reset_on_fork=1;kernel_sched_policy_fork(&child,&batch);
    assert(!child.fair_slice_ns && kernel_sched_fair_slice(&child)==KERNEL_SCHED_FAIR_NS);
    assert(batch.fair_slice_ns==2000000 && batch.fair_slice_used==123 && batch.fair_remainder==17);
    kernel_sched_publish_priority(&normal,0,-5);assert(kernel_sched_wait_priority(&normal)==25);
    kernel_sched_publish_priority(&normal,1,19);assert(kernel_sched_wait_priority(&normal)==41);
    kernel_sched_publish_priority(&normal,0,19);assert(kernel_sched_wait_priority(&normal)==1);
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
