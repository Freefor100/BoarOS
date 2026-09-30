#include <kernel/sched_policy.h>
#include <kernel/errno.h>
#include <stdint.h>

int kernel_sched_policy_set(struct kernel_sched_policy *state,
                            int policy, int priority, int keep_policy)
{
    unsigned reset = keep_policy ? state->reset_on_fork
                                 : !!(policy & KERNEL_SCHED_RESET_ON_FORK);
    if (keep_policy) policy = state->policy;
    else policy &= ~KERNEL_SCHED_RESET_ON_FORK;
    if (policy != KERNEL_SCHED_OTHER && policy != KERNEL_SCHED_FIFO &&
        policy != KERNEL_SCHED_RR) return -KERNEL_EINVAL;
    if (priority < 0 || priority > 99 ||
        ((policy == KERNEL_SCHED_OTHER) != (priority == 0))) return -KERNEL_EINVAL;
    if (!state->rr_remaining_ns && policy == KERNEL_SCHED_RR)
        state->rr_remaining_ns = KERNEL_SCHED_RR_NS;
    state->policy = policy;
    state->priority = priority;
    state->reset_on_fork = reset;
    return 0;
}

void kernel_sched_policy_fork(struct kernel_sched_policy *child,
                              const struct kernel_sched_policy *parent)
{
    *child = *parent;
    child->rr_remaining_ns = KERNEL_SCHED_RR_NS;
    if (child->reset_on_fork) {
        child->policy = KERNEL_SCHED_OTHER;
        child->priority = 0;
        child->reset_on_fork = 0;
    }
}

void kernel_sched_policy_charge(struct kernel_sched_policy *state, uint64_t delta_ns)
{
    if (state->policy != KERNEL_SCHED_RR) return;
    state->rr_remaining_ns = delta_ns >= state->rr_remaining_ns
                                ? 0 : state->rr_remaining_ns - delta_ns;
}

void kernel_sched_policy_rotate(struct kernel_sched_policy *state)
{
    if (state->policy == KERNEL_SCHED_RR) state->rr_remaining_ns = KERNEL_SCHED_RR_NS;
}

int kernel_sched_policy_expired(const struct kernel_sched_policy *state)
{
    return state->policy == KERNEL_SCHED_RR && !state->rr_remaining_ns;
}

void kernel_rt_bandwidth_init(struct kernel_rt_bandwidth *state, uint64_t now_ns)
{
    state->period_ns = UINT64_C(1000000000);
    state->runtime_ns = INT64_C(950000000);
    state->period_start_ns = state->last_account_ns = now_ns;
    state->consumed_ns = 0;
}

void kernel_rt_bandwidth_account(struct kernel_rt_bandwidth *state,
                                 uint64_t now_ns, int running_rt)
{
    if (now_ns < state->last_account_ns) __builtin_trap();
    uint64_t delta = now_ns - state->last_account_ns;
    uint64_t since_start = now_ns - state->period_start_ns;
    if (since_start >= state->period_ns) {
        /* 丢弃旧窗口余额，只计本窗口内确实运行的部分。 */
        state->period_start_ns = now_ns - since_start % state->period_ns;
        state->consumed_ns = 0;
        if (state->last_account_ns < state->period_start_ns)
            delta = now_ns - state->period_start_ns;
    }
    if (running_rt) {
        state->consumed_ns = UINT64_MAX - state->consumed_ns < delta
            ? UINT64_MAX : state->consumed_ns + delta;
    }
    state->last_account_ns = now_ns;
}

int kernel_rt_bandwidth_eligible(const struct kernel_rt_bandwidth *state)
{
    return state->runtime_ns < 0 || state->consumed_ns < (uint64_t)state->runtime_ns;
}

int kernel_rt_bandwidth_set(struct kernel_rt_bandwidth *state, int runtime_field,
                            int64_t value_us, uint64_t now_ns, int running_rt)
{
    kernel_rt_bandwidth_account(state, now_ns, running_rt);
    if ((runtime_field && value_us < -1) || (!runtime_field && value_us <= 0) ||
        value_us > INT32_MAX) return -KERNEL_EINVAL;
    uint64_t period = runtime_field ? state->period_ns : (uint64_t)value_us * 1000U;
    int64_t runtime = runtime_field ? (value_us < 0 ? -1 : value_us * 1000)
                                    : state->runtime_ns;
    if (runtime >= 0 && (uint64_t)runtime > period) return -KERNEL_EINVAL;
    if (period != state->period_ns) state->period_start_ns = now_ns;
    /* 配置修改不能获得免费预算；禁用后重新启用仍保留本窗口已消费量。 */
    state->period_ns = period;
    state->runtime_ns = runtime;
    return 0;
}

uint64_t kernel_rt_bandwidth_delay(const struct kernel_rt_bandwidth *state,
                                    int running_rt, int queued_rt)
{
    if (state->runtime_ns <= 0) return 0;
    uint64_t period_left = state->period_ns -
        (state->last_account_ns - state->period_start_ns);
    if (!kernel_rt_bandwidth_eligible(state)) return queued_rt || running_rt ? period_left : 0;
    if (!running_rt) return 0;
    uint64_t budget_left = (uint64_t)state->runtime_ns - state->consumed_ns;
    return budget_left < period_left ? budget_left : period_left;
}
