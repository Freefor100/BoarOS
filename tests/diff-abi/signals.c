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
static void signal_fault_cases(void);

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
    signal_fault_cases();
}

static volatile unsigned long fault_expected_address;
static volatile int fault_expected_signal, fault_expected_code, fault_repair_mode, fault_count;
static void fault_info_handler(int signal, struct wait_siginfo *info, void *context)
{
    unsigned long *gregs = (void *)((unsigned char *)context + 176);
    unsigned long address = *(unsigned long *)((unsigned char *)info + 16);
    if (signal != fault_expected_signal || info->code != fault_expected_code ||
        address != (fault_expected_address ? fault_expected_address : gregs[0])) abi_exit(91);
    fault_count++;
    if (fault_repair_mode == 1) {
        if (SC3(226, address, ABI_PAGE_SIZE, 3)) abi_exit(92);
    } else if (fault_repair_mode == 2) {
        if ((unsigned long)CALL(222, address, ABI_PAGE_SIZE, 3, 0x32, -1, 0) != address) abi_exit(93);
    } else gregs[0] += 4;
}
static void signal_fault_cases(void)
{
    static const char *names[] = {"signal.fault-accerr", "signal.fault-maperr",
        "signal.fault-bus", "signal.fault-ill", "signal.fault-trap"};
    for (unsigned scenario = 0; scenario < 5; scenario++) {
        long child = CALL(220, 17, 0, 0, 0, 0, 0);
        abi_require(child >= 0);
        if (!child) {
            fault_count = 0; fault_expected_address = 0;
            fault_expected_signal = scenario < 2 ? 11 : scenario == 2 ? 7 : scenario == 3 ? 4 : 5;
            fault_expected_code = scenario == 0 || scenario == 2 ? 2 : 1;
            fault_repair_mode = scenario == 0 ? 1 : scenario == 1 ? 2 : 0;
            struct wait_action action = {(unsigned long)fault_info_handler, 4, 0};
            abi_require(SC4(134, fault_expected_signal, &action, 0, 8) == 0);
            if (scenario < 2) {
                long page = CALL(222, 0, ABI_PAGE_SIZE, 0, 0x22, -1, 0);
                abi_require(page > 0);
                fault_expected_address = page;
                if (scenario == 1) abi_require(SC2(215, page, ABI_PAGE_SIZE) == 0);
                *(volatile unsigned char *)page = 0x6b;
                abi_require(*(volatile unsigned char *)page == 0x6b);
            } else if (scenario == 2) {
                long fd = SC4(56, -100, "/fault-diff", 0102 | 01000, 0600);
                abi_require(fd >= 0);
                long page = CALL(222, 0, ABI_PAGE_SIZE, 1, 2, fd, 0);
                abi_require(page > 0);
                fault_expected_address = page;
                unsigned long value;
#if defined(__loongarch__)
                __asm__ volatile("ld.d %0, %1, 0"
                    : "=r"(value) : "r"(page) : "memory");
#else
                __asm__ volatile(".option push\n.option norvc\nld %0, 0(%1)\n.option pop"
                    : "=r"(value) : "r"(page) : "memory");

#endif
                (void)value;
            } else if (scenario == 3) {
#if defined(__loongarch__)
                __asm__ volatile(".word 0" ::: "memory");
#else
                __asm__ volatile(".option push\n.option norvc\n.word 0\n.option pop" ::: "memory");
#endif
            } else {
#if defined(__loongarch__)
                __asm__ volatile("break 0" ::: "memory");
#else
                __asm__ volatile(".option push\n.option norvc\nebreak\n.option pop" ::: "memory");
#endif
            }
            abi_exit(fault_count == 1 ? 0 : 94);
        }
        int status = 0;
        abi_require(SC4(260, child, &status, 0, 0) == child);
        abi_record(names[scenario], status == 0, -1, -1, 0, 0, 0);
    }
    abi_require(SC3(35, -100, "/fault-diff", 0) == 0);
}
