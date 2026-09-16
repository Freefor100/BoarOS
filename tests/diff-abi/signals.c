#include "abi.h"

struct wait_timespec { long seconds, nanoseconds; };
struct wait_siginfo {
    int signo, error, code, padding;
    int pid;
    unsigned uid;
    unsigned char extra[104];
};
struct wait_action { unsigned long handler, flags, mask; };
static volatile int handled_signal;

static void wait_handler(int signal)
{
    handled_signal = signal;
}

static void record_wait(const char *name, long result,
                        const struct wait_siginfo *info, long expected_pid)
{
    abi_record(name, result,
               result > 0 ? info->signo : -1,
               result > 0 ? info->code : -1,
               result > 0 && info->pid == expected_pid,
               result > 0 ? &info->uid : 0,
               result > 0 ? sizeof(info->uid) : 0);
}

void abi_signal_wait_cases(void)
{
    const unsigned long mask = 1UL << (10 - 1);
    const struct wait_timespec zero = {0, 0};
    const struct wait_timespec brief = {0, 1000000};
    const struct wait_timespec invalid = {0, 1000000000L};
    const struct wait_timespec second = {1, 0};
    struct wait_siginfo info;
    unsigned long old_mask = 0;
    long pid = SC0(172), tid = SC0(178), result;

    result = SC4(137, &mask, &info, &zero, 16);
    abi_record("signal.wait-size", result, -1, -1, 0, 0, 0);
    result = SC4(137, (void *)-1, &info, &zero, 8);
    abi_record("signal.wait-mask-fault", result, -1, -1, 0, 0, 0);
    result = SC4(137, &mask, &info, &invalid, 8);
    abi_record("signal.wait-timeout-invalid", result, -1, -1, 0, 0, 0);
    result = SC4(137, &mask, &info, &zero, 8);
    abi_record("signal.wait-empty", result, -1, -1, 0, 0, 0);
    result = SC4(137, &mask, &info, &brief, 8);
    abi_record("signal.wait-timed-out", result, -1, -1, 0, 0, 0);

    abi_require(SC4(135, 0, &mask, &old_mask, 8) == 0);
    abi_require(SC2(130, tid, 10) == 0);
    result = SC4(137, &mask, &info, &zero, 8);
    record_wait("signal.wait-thread-pending", result, &info, pid);
    abi_require(SC2(129, pid, 10) == 0);
    result = SC4(137, &mask, &info, &zero, 8);
    record_wait("signal.wait-group-pending", result, &info, pid);

    long child = CALL(220, 17, 0, 0, 0, 0, 0);
    abi_require(child >= 0);
    if (!child) {
        long parent = SC0(173);
        abi_exit(SC2(129, parent, 10) == 0 ? 0 : 91);
    }
    result = SC4(137, &mask, &info, &second, 8);
    record_wait("signal.wait-child-delivery", result, &info, child);
    int status = 0;
    abi_require(SC4(260, child, &status, 0, 0) == child && status == 0);

    abi_require(SC2(129, pid, 10) == 0);
    result = SC4(137, &mask, (void *)-1, &zero, 8);
    abi_record("signal.wait-info-fault", result, -1, -1, 0, 0, 0);
    result = SC4(137, &mask, &info, &zero, 8);
    abi_record("signal.wait-info-consumed", result, -1, -1, 0, 0, 0);

    struct wait_action action = {(unsigned long)wait_handler, 0, 0};
    abi_require(SC4(134, 12, &action, 0, 8) == 0);
    child = CALL(220, 17, 0, 0, 0, 0, 0);
    abi_require(child >= 0);
    if (!child) {
        const struct wait_timespec delay = {0, 50000000};
        abi_require(SC2(101, &delay, 0) == 0);
        abi_exit(SC2(129, SC0(173), 12) == 0 ? 0 : 92);
    }
    result = SC4(137, &mask, &info, &second, 8);
    abi_record("signal.wait-interrupted", result, -1, -1,
               handled_signal, 0, 0);
    abi_require(SC4(260, child, &status, 0, 0) == child && status == 0);
    abi_require(SC4(134, 12, 0, 0, 8) == 0);
    abi_require(SC4(135, 2, &old_mask, 0, 8) == 0);
}
