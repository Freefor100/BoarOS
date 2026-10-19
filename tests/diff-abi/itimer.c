#include "abi.h"

struct itimer_value { long interval_sec, interval_usec, sec, usec; };
struct itimer_timespec { long sec, ns; };
static const unsigned long timer_mask = (1UL << 25) | (1UL << 26);
static unsigned char timer_stack[16384] __attribute__((aligned(16)));
static volatile unsigned handler_count[2], bad_handler_info;
extern long abi_clone_entry(long, void *, int *, void (*)(void *), void *);

static unsigned long timer_ns(const struct itimer_value *v)
{ return (unsigned long)v->sec * 1000000000UL + v->usec * 1000UL; }
static unsigned long execution_ns(void)
{
    struct itimer_timespec t;
    abi_require(!SC2(113, 3, &t));
    return (unsigned long)t.sec * 1000000000UL + t.ns;
}
static void burn_cpu(unsigned long duration)
{
    unsigned long end = execution_ns() + duration, value = 1;
    do {
        for (unsigned i = 0; i < 200000; ++i) {
            value = (value << 3) ^ (value >> 7) ^ 17;
            __asm__ volatile("" : "+r"(value));
        }
    } while (execution_ns() < end);
}
static void timer_check(const char *name, int condition)
{ abi_record(name, condition, -1, -1, 0, 0, 0); abi_require(condition); }
static long take_timer(int signal)
{
    unsigned long mask = 1UL << (signal - 1);
    struct itimer_timespec zero = {0};
    int info[32] = {0};
    long ret = SC4(137, &mask, info, &zero, 8);
    if (ret > 0) abi_require(ret == signal && info[0] == signal && info[2] == 128 && !info[4]);
    return ret;
}
static void disarm(int which)
{ const struct itimer_value zero = {0}; abi_require(!SC3(103, which, &zero, 0)); }
static void timer_handler(int signal, const int *info, void *context)
{
    (void)context;
    if (signal < 26 || signal > 27 || info[0] != signal || info[2] != 128 || info[4])
        bad_handler_info = 1;
    else handler_count[signal - 26]++;
}

/* 在另一个成员消耗CPU前，设置者退出；进程身份继续拥有计时器。 */
struct timer_worker {
    int mode, which, signal, done, ready, start, pipe_write;
    unsigned long online, elapsed;
};
static void wait_word(int *word)
{
    while (!__atomic_load_n(word, __ATOMIC_ACQUIRE)) {
        long r = CALL(98, word, 0, 0, 0, 0, 0);
        abi_require(r == 0 || r == -11);
    }
}
static void timer_member(void *opaque)
{
    struct timer_worker *w = opaque;
    unsigned long affinity = w->online & 2 ? 2 : 1;
    abi_require(!SC3(122, 0, sizeof(affinity), &affinity));
    if (w->mode == 3) {
        (void)execution_ns();
        __atomic_store_n(&w->ready, 1, __ATOMIC_RELEASE);
        abi_require(SC3(98, &w->ready, 1, 1) >= 0);
        wait_word(&w->start);
        unsigned long start = execution_ns();
        burn_cpu(120000000UL);
        w->elapsed = execution_ns() - start;
        char byte = 1;
        abi_require(SC3(64, w->pipe_write, &byte, 1) == 1);
        abi_exit(0);
    }
    if (w->mode == 0) {
        const struct itimer_value value = {.usec = 150000};
        abi_require(!SC3(103, w->which, &value, 0));
        __atomic_store_n(&w->done, 1, __ATOMIC_RELEASE);
        abi_exit(0);
    }
    if (w->mode == 1) {
        const char *argv[] = {"/init", "itimer-exec", 0};
        const char *env[] = {0};
        abi_require(!SC3(221, "/init", argv, env));
        abi_exit(91);
    }
    burn_cpu(100000000UL);
    abi_require(take_timer(w->signal) == w->signal);
    SC1(94, 57);
    abi_exit(92);
}
void abi_itimer_exec_probe(void)
{
    struct itimer_value state;
    abi_require(!SC2(102, 1, &state) && timer_ns(&state));
    burn_cpu(220000000UL);
    abi_require(take_timer(26) == 26);
    abi_exit(57);
}

void abi_itimer_cases(void)
{
    struct itimer_value state = {0}, before = {0};
    unsigned long saved_mask;
    abi_require(!SC4(135, 0, &timer_mask, &saved_mask, 8));
    long ret = SC2(102, 1, &state);
    abi_record("itimer.virtual-entry", ret, -1, -1, 0, 0, 0);
    abi_require(!ret);
    ret = SC2(102, 2, &state);
    abi_record("itimer.prof-entry", ret, -1, -1, 0, 0, 0);
    abi_require(!ret);
    abi_record("itimer.invalid-kind", SC2(102, 3, &state), -1, -1, 0, 0, 0);
    abi_record("itimer.get-badptr", SC2(102, 1, (void *)-1), -1, -1, 0, 0, 0);
    abi_record("itimer.set-badptr", SC3(103, 1, (void *)-1, 0), -1, -1, 0, 0, 0);
    struct itimer_value bad = {.usec = 1000000};
    abi_record("itimer.invalid-time", SC3(103, 1, &bad, 0), -1, -1, 0, 0, 0);
    for (int which = 1; which <= 2; ++which) {
        int signal = which == 1 ? 26 : 27;
        struct itimer_value value = {.interval_usec = 70000, .usec = 300000};
        abi_require(!SC3(103, which, &value, &before));
        timer_check(which == 1 ? "itimer.virtual-initial" : "itimer.prof-initial",
                    !timer_ns(&before) && !before.interval_sec && !before.interval_usec);
        abi_require(!SC2(102, which, &before));
        struct itimer_timespec delay = {0, 60000000};
        abi_require(!SC2(101, &delay, 0));
        abi_require(!SC2(102, which, &state));
        timer_check(which == 1 ? "itimer.virtual-sleep" : "itimer.prof-sleep",
                    timer_ns(&state) > 250000000UL && timer_ns(&state) <= timer_ns(&before) &&
                    !state.interval_sec && state.interval_usec == 70000);
        value = (struct itimer_value){.usec = 20000};
        abi_record(which == 1 ? "itimer.virtual-old-fault" : "itimer.prof-old-fault",
                    SC3(103, which, &value, (void *)-1), -1, -1, 0, 0, 0);
        abi_require(!SC2(102, which, &state));
        timer_check(which == 1 ? "itimer.virtual-fault-committed" : "itimer.prof-fault-committed",
                    timer_ns(&state) > 0 && timer_ns(&state) <= 40000000UL && !state.interval_usec);
        burn_cpu(90000000UL);
        timer_check(which == 1 ? "itimer.virtual-one-shot" : "itimer.prof-one-shot", take_timer(signal) == signal);
        abi_require(!SC2(102, which, &state));
        timer_check(which == 1 ? "itimer.virtual-expired" : "itimer.prof-expired",
                    !timer_ns(&state) && take_timer(signal) == -11);
        value = (struct itimer_value){.interval_usec = 40000, .usec = 10000};
        abi_require(!SC3(103, which, &value, 0));
        burn_cpu(100000000UL);
        timer_check(which == 1 ? "itimer.virtual-coalesced" : "itimer.prof-coalesced",
                    take_timer(signal) == signal && take_timer(signal) == -11);
        burn_cpu(80000000UL);
        timer_check(which == 1 ? "itimer.virtual-periodic" : "itimer.prof-periodic", take_timer(signal) == signal);
        value = (struct itimer_value){.interval_usec = 91000};
        abi_require(!SC3(103, which, &value, 0) && !SC2(102, which, &state));
        timer_check(which == 1 ? "itimer.virtual-disabled-interval" : "itimer.prof-disabled-interval",
                    !timer_ns(&state) && state.interval_usec == 91000);
        disarm(which);
        value = (struct itimer_value){.interval_usec = 1, .usec = 1};
        abi_require(!SC3(103, which, &value, 0));
        burn_cpu(80000000UL);
        abi_require(take_timer(signal) == signal);
        delay = (struct itimer_timespec){0, 50000000};
        abi_require(!SC2(101, &delay, 0));
        ret = take_timer(signal);
        /* 消费到信号后进入睡眠前仍有少量执行；允许这一窗口产生的一笔，
         * 随后的完整睡眠窗口应保持安静，历史欠账不能按墙钟反复触发。 */
        if (ret == signal) {
            abi_require(!SC2(101, &delay, 0));
            ret = take_timer(signal);
        }
        timer_check(which == 1 ? "itimer.virtual-short-sleep" : "itimer.prof-short-sleep", ret == -11);
        disarm(which);
    }
    /* 两类计时器从相同进程CPU状态开始，PROF另计系统调用处理。 */
    struct itimer_value value = {.sec = 2}, u_before, p_before, u_after, p_after;
    abi_require(!SC3(103, 1, &value, 0) && !SC3(103, 2, &value, 0));
    abi_require(!SC2(102, 1, &u_before) && !SC2(102, 2, &p_before));
    for (unsigned i = 0; i < 20000; ++i) abi_require(SC0(172) > 0);
    abi_require(!SC2(102, 1, &u_after) && !SC2(102, 2, &p_after));
    timer_check("itimer.prof-counts-kernel", timer_ns(&p_before) - timer_ns(&p_after) >
                                           timer_ns(&u_before) - timer_ns(&u_after));
    disarm(1); disarm(2);
    value = (struct itimer_value){.sec = 30};
    abi_require(!SC3(103, 0, &value, 0) && !SC3(103, 1, &value, 0));
    disarm(1);
    abi_require(!SC2(102, 0, &state));
    int independent = timer_ns(&state) > 0;
    abi_require(!SC3(103, 1, &value, 0) && !SC3(103, 2, &value, 0));
    disarm(0);
    abi_require(!SC2(102, 1, &state)); independent &= timer_ns(&state) > 0;
    abi_require(!SC2(102, 2, &state)); independent &= timer_ns(&state) > 0;
    timer_check("itimer.independent-domains", independent);
    disarm(1); disarm(2);
    unsigned long online = 0;
    abi_require(SC3(123, 0, sizeof(online), &online) == sizeof(online));
    unsigned long affinity = 1;
    abi_require(!SC3(122, 0, sizeof(affinity), &affinity));
    int pipefd[2]; abi_require(!SC2(59, pipefd, 0));
    struct timer_worker parallel = {.mode = 3, .online = online, .pipe_write = pipefd[1]};
    int thread_tid = 0;
    abi_require(abi_clone_entry(0x1210f00, timer_stack + sizeof(timer_stack), &thread_tid,
                                timer_member, &parallel) > 0);
    wait_word(&parallel.ready);
    value = (struct itimer_value){.usec = 180000};
    abi_require(!SC3(103, 1, &value, 0) && !SC3(103, 2, &value, 0));
    __atomic_store_n(&parallel.start, 1, __ATOMIC_RELEASE);
    abi_require(SC3(98, &parallel.start, 1, 1) >= 0);
    unsigned long started = execution_ns();
    burn_cpu(120000000UL);
    unsigned long elapsed = execution_ns() - started;
    char byte; abi_require(SC3(63, pipefd[0], &byte, 1) == 1 && byte == 1);
    while (__atomic_load_n(&thread_tid, __ATOMIC_ACQUIRE)) {
        int expected = __atomic_load_n(&thread_tid, __ATOMIC_RELAXED);
        if (!expected) break;
        long r = CALL(98, &thread_tid, 0, expected, 0, 0, 0);
        abi_require(r == 0 || r == -11);
    }
    timer_check("itimer.group-total", elapsed < 180000000UL && parallel.elapsed < 180000000UL &&
                    take_timer(26) == 26 && take_timer(27) == 27);
    abi_require(!SC1(57, pipefd[0]) && !SC1(57, pipefd[1]));
    abi_require(!SC3(122, 0, sizeof(online), &online));
    struct { unsigned long handler, flags, mask; } action = {(unsigned long)timer_handler, 4, 0}, old[2];
    abi_require(!SC4(134, 26, &action, &old[0], 8) && !SC4(134, 27, &action, &old[1], 8));
    value = (struct itimer_value){.interval_usec = 20000, .usec = 10000};
    abi_require(!SC3(103, 1, &value, 0) && !SC3(103, 2, &value, 0));
    abi_require(!SC4(135, 1, &timer_mask, 0, 8));
    burn_cpu(150000000UL);
    abi_require(!SC4(135, 0, &timer_mask, 0, 8));
    disarm(1); disarm(2);
    timer_check("itimer.handlers", handler_count[0] >= 2 && handler_count[1] >= 2 && !bad_handler_info);
    while (take_timer(26) == 26) {}
    while (take_timer(27) == 27) {}
    abi_require(!SC4(134, 26, &old[0], 0, 8) && !SC4(134, 27, &old[1], 0, 8));
    for (int which = 1; which <= 2; ++which) {
        long child = CALL(220, 17, 0, 0, 0, 0, 0); abi_require(child >= 0);
        if (!child) {
            abi_require(!SC4(135, 1, &timer_mask, 0, 8));
            value = (struct itimer_value){.usec = 10000};
            abi_require(!SC3(103, which, &value, 0));
            burn_cpu(200000000UL);
            abi_exit(99);
        }
        int status; abi_require(SC4(260, child, &status, 0, 0) == child);
        timer_check(which == 1 ? "itimer.virtual-default-signal" : "itimer.prof-default-signal",
                    status == 25 + which);
    }
    for (int mode = 0; mode < 3; ++mode) {
        long child = CALL(220, 17, 0, 0, 0, 0, 0);
        abi_require(child >= 0);
        if (!child) {
            value = (struct itimer_value){.usec = 10000};
            if (mode) abi_require(!SC3(103, 1, &value, 0));
            struct timer_worker worker = {.mode = mode, .which = 1, .signal = 26, .online = online};
            int child_tid = 0;
            long tid = abi_clone_entry(0x1210f00, timer_stack + sizeof(timer_stack), &child_tid, timer_member, &worker);
            abi_require(tid > 0);
            if (mode == 2) { abi_exit(0); }
            if (mode == 1) {
                for (;;) SC0(124);
            }
            while (!__atomic_load_n(&worker.done, __ATOMIC_ACQUIRE)) SC0(124);
            while (__atomic_load_n(&child_tid, __ATOMIC_ACQUIRE)) SC0(124);
            burn_cpu(240000000UL);
            abi_require(take_timer(26) == 26);
            abi_exit(57);
        }
        int status;
        abi_require(SC4(260, child, &status, 0, 0) == child);
        timer_check(mode == 0 ? "itimer.member-setter-exit" : mode == 1 ? "itimer.nonleader-exec" :
                    "itimer.leader-exit", status == (57 << 8));
    }
    value = (struct itimer_value){.interval_usec = 70000, .usec = 200000};
    abi_require(!SC3(103, 1, &value, 0) && !SC3(103, 2, &value, 0));
    long child = CALL(220, 17, 0, 0, 0, 0, 0); abi_require(child >= 0);
    if (!child) {
        for (int which = 1; which <= 2; ++which)
            abi_require(!SC2(102, which, &state) && !timer_ns(&state) && !state.interval_usec);
        abi_exit(57);
    }
    int status; abi_require(SC4(260, child, &status, 0, 0) == child);
    timer_check("itimer.fork-disarmed", status == (57 << 8));
    disarm(1); disarm(2);
    while (take_timer(26) == 26) {}
    while (take_timer(27) == 27) {}
    abi_require(!SC4(135, 2, &saved_mask, 0, 8));
}
