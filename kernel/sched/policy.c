#include <kernel/sched_policy.h>
#include <kernel/errno.h>
#include <stdint.h>

static uint64_t rr_slice_ns = KERNEL_SCHED_RR_NS;

uint64_t kernel_sched_rr_slice_ns(void)
{ return __atomic_load_n(&rr_slice_ns, __ATOMIC_ACQUIRE); }

int kernel_sched_rr_slice_set_ms(int64_t milliseconds)
{
    if (milliseconds < INT32_MIN || milliseconds > INT32_MAX) return -KERNEL_EINVAL;
    uint64_t ns = milliseconds <= 0 ? KERNEL_SCHED_RR_NS : (uint64_t)milliseconds * UINT64_C(1000000);
    /* Existing tasks keep the remainder already charged to them. New tasks
     * and exhausted slices use the published value, as with Linux's sysctl. */
    __atomic_store_n(&rr_slice_ns, ns, __ATOMIC_RELEASE);
    return 0;
}

/* Linux's per-thread nice weights: equal nice levels receive equal CPU share,
 * while each +1 step reduces weight by approximately 20 percent. */
static const unsigned nice_weights[KERNEL_SCHED_NICE_LEVELS]={
    88761,71755,56483,46273,36291,29154,23254,18705,14949,11916,
    9548,7620,6100,4904,3906,3121,2501,1991,1586,1277,
    1024,820,655,526,423,335,272,215,172,137,
    110,87,70,56,45,36,29,23,18,15
};
unsigned kernel_sched_weight(const struct kernel_sched_policy *state)
{
    if (state->nice < -20 || state->nice > 19) __builtin_trap();
    return state->policy==KERNEL_SCHED_IDLE ? 3U : nice_weights[state->nice-KERNEL_SCHED_NICE_MIN];
}
uint64_t kernel_sched_fair_delta(struct kernel_sched_policy *state,uint64_t ns)
{
    unsigned weight=kernel_sched_weight(state);
    if (weight==1024) return ns;
    uint64_t whole=ns/weight;
    uint64_t fraction=(ns%weight)*1024U+state->fair_remainder;
    uint64_t tail=fraction/weight;
    if(whole>(UINT64_MAX-tail)/1024U)__builtin_trap();
    state->fair_remainder=(uint32_t)(fraction%weight);
    return whole*1024U+tail;
}

int kernel_sched_policy_set(struct kernel_sched_policy *state,
                            int policy, int priority, int keep_policy)
{
    unsigned reset = keep_policy ? state->reset_on_fork
                                 : !!(policy & KERNEL_SCHED_RESET_ON_FORK);
    if (keep_policy) policy = state->policy;
    else policy &= ~KERNEL_SCHED_RESET_ON_FORK;
    if (policy != KERNEL_SCHED_OTHER && policy != KERNEL_SCHED_FIFO &&
        policy != KERNEL_SCHED_RR && policy != KERNEL_SCHED_BATCH &&
        policy != KERNEL_SCHED_IDLE) return -KERNEL_EINVAL;
    if (priority < 0 || priority > 99 ||
        ((policy != KERNEL_SCHED_FIFO && policy != KERNEL_SCHED_RR) != (priority == 0))) return -KERNEL_EINVAL;
    if (!state->rr_remaining_ns && policy == KERNEL_SCHED_RR)
        state->rr_remaining_ns = kernel_sched_rr_slice_ns();
    state->policy = policy;
    state->priority = priority;
    state->reset_on_fork = reset;
    return 0;
}

void kernel_sched_policy_fork(struct kernel_sched_policy *child,
                              const struct kernel_sched_policy *parent)
{
    *child = *parent;
    child->rr_remaining_ns = kernel_sched_rr_slice_ns();
    child->fair_remainder=child->fair_slice_used=0;
    if (child->reset_on_fork) {
        child->fair_slice_ns=0;
        if (child->policy==KERNEL_SCHED_FIFO || child->policy==KERNEL_SCHED_RR) {
            child->policy=KERNEL_SCHED_OTHER;child->priority=0;child->nice=0;
        } else if (child->nice<0) child->nice=0;
        child->reset_on_fork=0;
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
    if (state->policy == KERNEL_SCHED_RR) state->rr_remaining_ns = kernel_sched_rr_slice_ns();
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
