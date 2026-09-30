#ifndef BOAROS_KERNEL_SCHED_POLICY_H
#define BOAROS_KERNEL_SCHED_POLICY_H
#include <stdint.h>
#define KERNEL_SCHED_OTHER 0
#define KERNEL_SCHED_FIFO 1
#define KERNEL_SCHED_RR 2
#define KERNEL_SCHED_RESET_ON_FORK 0x40000000
#define KERNEL_SCHED_RR_NS UINT64_C(100000000)
struct kernel_sched_policy {
    int policy, priority;
    unsigned reset_on_fork;
    uint64_t rr_remaining_ns;
};
struct kernel_rt_bandwidth {
    uint64_t period_ns, period_start_ns, consumed_ns, last_account_ns;
    int64_t runtime_ns;
};
int kernel_sched_policy_set(struct kernel_sched_policy *state,
                            int policy, int priority, int keep_policy);
void kernel_sched_policy_fork(struct kernel_sched_policy *child,
                              const struct kernel_sched_policy *parent);
void kernel_sched_policy_charge(struct kernel_sched_policy *state, uint64_t delta_ns);
void kernel_sched_policy_rotate(struct kernel_sched_policy *state);
int kernel_sched_policy_expired(const struct kernel_sched_policy *state);
void kernel_rt_bandwidth_init(struct kernel_rt_bandwidth *state, uint64_t now_ns);
void kernel_rt_bandwidth_account(struct kernel_rt_bandwidth *state,
                                 uint64_t now_ns, int running_rt);
int kernel_rt_bandwidth_eligible(const struct kernel_rt_bandwidth *state);
uint64_t kernel_rt_bandwidth_delay(const struct kernel_rt_bandwidth *state,
                                    int running_rt, int queued_rt);
int kernel_rt_bandwidth_set(struct kernel_rt_bandwidth *state, int runtime_field,
                            int64_t value_us, uint64_t now_ns, int running_rt);
struct kernel_task;
int kernel_task_sched_get(struct kernel_task *caller, int32_t pid,
                           struct kernel_sched_policy *state);
int kernel_task_sched_set(struct kernel_task *caller, int32_t pid,
                           int policy, int priority, int keep_policy);
#endif
