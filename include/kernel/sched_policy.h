#ifndef BOAROS_KERNEL_SCHED_POLICY_H
#define BOAROS_KERNEL_SCHED_POLICY_H
#include <stdint.h>
#define KERNEL_SCHED_OTHER 0
#define KERNEL_SCHED_FIFO 1
#define KERNEL_SCHED_RR 2
#define KERNEL_SCHED_BATCH 3
#define KERNEL_SCHED_IDLE 5
#define KERNEL_SCHED_FAIR_NS UINT64_C(10000000)
#define KERNEL_SCHED_FAIR_MIN_NS UINT64_C(100000)
#define KERNEL_SCHED_FAIR_MAX_NS UINT64_C(100000000)
#define KERNEL_SCHED_WAKE_NS UINT64_C(1000000)
#define KERNEL_SCHED_NICE_MIN (-20)
#define KERNEL_SCHED_NICE_LEVELS 40
#define KERNEL_SCHED_RESET_ON_FORK 0x40000000
#define KERNEL_SCHED_RR_NS UINT64_C(100000000)
struct kernel_sched_policy {
    int policy;
    unsigned reset_on_fork;
    union {
        uint64_t execution_priority;
        struct { int priority,nice; };
    };
    uint64_t rr_remaining_ns;
    uint32_t fair_remainder, fair_slice_used;
    uint32_t fair_slice_ns; /* Zero selects the default request. */
};
static inline uint32_t kernel_sched_fair_slice(const struct kernel_sched_policy *state)
{ return state->fair_slice_ns ? state->fair_slice_ns : KERNEL_SCHED_FAIR_NS; }
/* MQ consumes the effective priority and saved nice as one publication.
 * RT/PI ranks dominate every normal nice level without taking a run lock. */
static inline void kernel_sched_publish_priority(struct kernel_sched_policy *state,int priority,int nice)
{
    uint64_t value=(uint32_t)priority|((uint64_t)(uint32_t)nice<<32);
    __atomic_store_n(&state->execution_priority,value,__ATOMIC_RELEASE);
}
static inline int kernel_sched_wait_priority(const struct kernel_sched_policy *state)
{
    uint64_t value=__atomic_load_n(&state->execution_priority,__ATOMIC_ACQUIRE);
    int priority=(int32_t)value,nice=(int32_t)(value>>32);
    return priority?40+priority:20-nice;
}
struct kernel_rt_bandwidth {
    uint64_t period_ns, period_start_ns, consumed_ns, last_account_ns;
    int64_t runtime_ns;
};
struct kernel_rt_configuration {
    uint64_t period_ns;
    int64_t runtime_ns;
};
uint64_t kernel_sched_rr_slice_ns(void);
int kernel_sched_rr_slice_set_ms(int64_t milliseconds);
unsigned kernel_sched_weight(const struct kernel_sched_policy *);
uint64_t kernel_sched_fair_delta(struct kernel_sched_policy *,uint64_t actual_ns);
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
struct kernel_sched_attr {
    uint32_t size,policy;
    uint64_t flags;
    int32_t nice;
    uint32_t priority;
    uint64_t runtime,deadline,period;
    uint32_t util_min,util_max;
};
struct kernel_task;
int kernel_task_sched_setattr(struct kernel_task *,int32_t,const struct kernel_sched_attr *);
int64_t kernel_task_priority(struct kernel_task *,int setting,int32_t which,int32_t who,int32_t nice);
int kernel_task_sched_get(struct kernel_task *caller, int32_t pid,
                           struct kernel_sched_policy *state);
int kernel_task_sched_set(struct kernel_task *caller, int32_t pid,
                           int policy, int priority, int keep_policy);
#endif
