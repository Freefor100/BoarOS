#include "abi.h"

struct abi_flock {
    short type, whence;
    long start, length;
    int pid;
};
struct lock_pollfd { int fd; short events, revents; };
struct lock_timespec { long seconds, nanoseconds; };
struct lock_action { unsigned long handler, flags, mask; };
static volatile int lock_signal_seen;

static void lock_signal_handler(int signal)
{
    lock_signal_seen = signal;
}

_Static_assert(sizeof(struct abi_flock) == 32, "RV64 flock ABI");

void abi_file_lock_exec_probe(unsigned mode)
{
    long fd = abi_open("/data", 2);
    if (fd < 0) abi_exit(92);
    struct abi_flock lock = { .type = 0, .whence = 0,
                              .start = 900 + mode, .length = 1 };
    if (SC3(25, fd, 36, &lock) != 0) abi_exit(93);
    int expected = (mode % 2) ? 2 : 1;
    abi_exit(lock.type == expected ? 0 : 94);
}

void abi_file_lock_cases(void)
{
    long first = abi_open("/data", 2);
    long second = abi_open("/data", 2);
    struct abi_flock lock = { .type = 1, .whence = 0,
                             .start = 17, .length = 9 };
    abi_require(first >= 0 && second >= 0);

    long result = SC3(25, first, 37, &lock);
    abi_record("lock.ofd.first", result, -1, -1, 0, 0, 0);
    result = SC3(25, second, 37, &lock);
    abi_record("lock.ofd.conflict", result, -1, -1, 0, 0, 0);

    lock.type = 0;
    result = SC3(25, second, 36, &lock);
    abi_record("lock.ofd.query", result, lock.type, lock.start,
               lock.length, 0, 0);

    lock.type = 2;
    lock.pid = 0;
    result = SC3(25, first, 37, &lock);
    abi_record("lock.ofd.unlock", result, -1, -1, 0, 0, 0);
    lock.type = 1;
    result = SC3(25, second, 37, &lock);
    abi_record("lock.ofd.after-unlock", result, -1, -1, 0, 0, 0);

    abi_require(SC1(57, first) == 0 && SC1(57, second) == 0);

    first = abi_open("/data", 2);
    second = abi_open("/data", 2);
    abi_require(first >= 0 && second >= 0);
    long duplicate = SC1(23, first);
    abi_require(duplicate >= 0);
    lock = (struct abi_flock){ .type = 1, .whence = 0,
                               .start = 60, .length = 1 };
    abi_require(SC3(25, first, 37, &lock) == 0);
    abi_require(SC1(57, first) == 0);
    lock.type = 0;
    result = SC3(25, second, 36, &lock);
    abi_record("lock.ofd.dup-holds", result, lock.type, -1, 0, 0, 0);
    abi_require(SC1(57, duplicate) == 0);
    lock.type = 0;
    result = SC3(25, second, 36, &lock);
    abi_record("lock.ofd.dup-final-close", result, lock.type, -1, 0, 0, 0);
    abi_require(SC1(57, second) == 0);

    first = abi_open("/data", 2);
    second = abi_open("/data", 2);
    abi_require(first >= 0 && second >= 0);
    duplicate = SC1(23, first);
    abi_require(duplicate >= 0);
    lock = (struct abi_flock){ .type = 1, .whence = 0,
                               .start = 70, .length = 1 };
    abi_require(SC3(25, duplicate, 6, &lock) == 0);
    abi_require(SC1(57, first) == 0);
    lock.type = 0;
    result = SC3(25, second, 36, &lock);
    abi_record("lock.traditional.dup-any-close", result, lock.type,
               -1, 0, 0, 0);
    abi_require(SC1(57, duplicate) == 0 && SC1(57, second) == 0);

    first = abi_open("/data", 2);
    long replaced = abi_open("/sparse", 2);
    second = abi_open("/sparse", 2);
    abi_require(first >= 0 && replaced >= 0 && second >= 0);
    lock = (struct abi_flock){ .type = 1, .whence = 0,
                               .start = 85, .length = 1 };
    abi_require(SC3(25, replaced, 37, &lock) == 0);
    abi_require(SC3(24, first, replaced, 0) == replaced);
    lock.type = 0;
    result = SC3(25, second, 36, &lock);
    abi_record("lock.ofd.dup-replace-final", result, lock.type,
               -1, 0, 0, 0);
    abi_require(SC1(57, first) == 0 && SC1(57, replaced) == 0 &&
                SC1(57, second) == 0);

    first = abi_open("/lockgone", 2 | 64 | 128);
    second = abi_open("/lockgone", 2);
    abi_require(first >= 0 && second >= 0);
    lock = (struct abi_flock){ .type = 1, .whence = 0,
                               .start = 0, .length = 0 };
    abi_require(SC3(25, first, 37, &lock) == 0);
    abi_require(SC3(35, -100, "/lockgone", 0) == 0);
    lock.type = 0;
    result = SC3(25, second, 36, &lock);
    abi_record("lock.ofd.unlinked-held", result, lock.type,
               -1, 0, 0, 0);
    long new_file = abi_open("/lockgone", 2 | 64 | 128);
    abi_require(new_file >= 0);
    lock.type = 1;
    result = SC3(25, new_file, 37, &lock);
    abi_record("lock.ofd.recreated-independent", result, -1,
               -1, 0, 0, 0);
    abi_require(SC1(57, first) == 0 && SC1(57, second) == 0 &&
                SC1(57, new_file) == 0);

    first = abi_open("/data", 2);
    second = abi_open("/data", 2);
    abi_require(first >= 0 && second >= 0);
    lock = (struct abi_flock){ .type = 1, .whence = 0,
                               .start = 400, .length = -20 };
    result = SC3(25, first, 37, &lock);
    abi_record("lock.negative-length", result, -1, -1, 0, 0, 0);
    lock = (struct abi_flock){ .type = 0, .whence = 0,
                               .start = 380, .length = 1 };
    result = SC3(25, second, 36, &lock);
    abi_record("lock.negative-query", result, lock.type, lock.start,
               lock.length, 0, 0);
    lock = (struct abi_flock){ .type = 2, .whence = 0,
                               .start = 400, .length = -20 };
    abi_require(SC3(25, first, 37, &lock) == 0);
    lock = (struct abi_flock){ .type = 1, .whence = 0,
                               .start = 1000, .length = 0 };
    abi_require(SC3(25, first, 37, &lock) == 0);
    lock = (struct abi_flock){ .type = 0, .whence = 0,
                               .start = 9223372036854775806L,
                               .length = 1 };
    result = SC3(25, second, 36, &lock);
    abi_record("lock.to-eof", result, lock.type, lock.start,
               lock.length, 0, 0);
    lock = (struct abi_flock){ .type = 1, .whence = 0,
                               .start = 9223372036854775807L,
                               .length = 2 };
    result = SC3(25, second, 37, &lock);
    abi_record("lock.overflow", result, -1, -1, 0, 0, 0);
    lock = (struct abi_flock){ .type = 0, .whence = 0,
                               .start = -1, .length = 1 };
    result = SC3(25, second, 37, &lock);
    abi_record("lock.negative-start", result, -1, -1, 0, 0, 0);
    lock.pid = 1;
    result = SC3(25, second, 37, &lock);
    abi_record("lock.ofd.nonzero-pid", result, -1, -1, 0, 0, 0);
    result = SC3(25, second, 37, (void *)1);
    abi_record("lock.bad-pointer", result, -1, -1, 0, 0, 0);
    result = SC3(25, -1, 37, (void *)1);
    abi_record("lock.bad-fd-and-pointer", result, -1, -1, 0, 0, 0);
    abi_require(SC1(57, first) == 0 && SC1(57, second) == 0);

    first = abi_open("/data", 0);
    second = abi_open("/data", 1);
    abi_require(first >= 0 && second >= 0);
    lock = (struct abi_flock){ .type = 1, .whence = 0,
                               .start = 0, .length = 1 };
    result = SC3(25, first, 6, &lock);
    abi_record("lock.readonly-write", result, -1, -1, 0, 0, 0);
    lock.type = 0;
    result = SC3(25, second, 6, &lock);
    abi_record("lock.writeonly-read", result, -1, -1, 0, 0, 0);
    abi_require(SC1(57, first) == 0 && SC1(57, second) == 0);

    first = abi_open("/data", 2);
    second = abi_open("/data", 2);
    abi_require(first >= 0 && second >= 0);
    lock = (struct abi_flock){ .type = 1, .whence = 0,
                               .start = 100, .length = 20 };
    result = SC3(25, first, 6, &lock);
    abi_record("lock.posix.first", result, -1, -1, 0, 0, 0);
    result = SC3(25, second, 6, &lock);
    abi_record("lock.posix.same-table", result, -1, -1, 0, 0, 0);
    lock.type = 0;
    result = SC3(25, second, 36, &lock);
    abi_record("lock.posix.vs-ofd", result, lock.type,
               lock.start, lock.length, 0, 0);
    abi_require(SC1(57, second) == 0);
    second = abi_open("/data", 2);
    abi_require(second >= 0);
    lock = (struct abi_flock){ .type = 0, .whence = 0,
                               .start = 100, .length = 20 };
    result = SC3(25, second, 36, &lock);
    abi_record("lock.posix.any-close", result, lock.type,
               lock.start, lock.length, 0, 0);

    lock = (struct abi_flock){ .type = 1, .whence = 0,
                               .start = 200, .length = 20 };
    abi_require(SC3(25, first, 37, &lock) == 0);
    lock = (struct abi_flock){ .type = 2, .whence = 0,
                               .start = 205, .length = 6 };
    result = SC3(25, first, 37, &lock);
    abi_record("lock.ofd.split", result, -1, -1, 0, 0, 0);
    lock = (struct abi_flock){ .type = 0, .whence = 0,
                               .start = 200, .length = 5 };
    result = SC3(25, second, 36, &lock);
    abi_record("lock.ofd.left", result, lock.type,
               lock.start, lock.length, 0, 0);
    lock = (struct abi_flock){ .type = 0, .whence = 0,
                               .start = 205, .length = 6 };
    result = SC3(25, second, 36, &lock);
    abi_record("lock.ofd.hole", result, lock.type,
               lock.start, lock.length, 0, 0);
    lock = (struct abi_flock){ .type = 0, .whence = 0,
                               .start = 211, .length = 9 };
    result = SC3(25, second, 36, &lock);
    abi_record("lock.ofd.right", result, lock.type,
               lock.start, lock.length, 0, 0);
    abi_require(SC1(57, first) == 0 && SC1(57, second) == 0);

    /* A child has a distinct POSIX owner even though fork shares its OFD. */
    first = abi_open("/data", 2);
    abi_require(first >= 0);
    lock = (struct abi_flock){ .type = 1, .whence = 0,
                               .start = 500, .length = 1 };
    abi_require(SC3(25, first, 6, &lock) == 0);
    int ready[2], done[2];
    abi_require(SC2(59, ready, 0) == 0 && SC2(59, done, 0) == 0);
    long child = CALL(220, 17, 0, 0, 0, 0, 0);
    abi_require(child >= 0);
    if (!child) {
        long outcome;
        abi_require(SC1(57, ready[0]) == 0 && SC1(57, done[0]) == 0);
        abi_require(SC3(64, ready[1], "r", 1) == 1);
        outcome = SC3(25, first, 7, &lock);
        abi_require(SC3(64, done[1], &outcome, sizeof(outcome)) ==
                    (long)sizeof(outcome));
        abi_exit(0);
    }
    abi_require(SC1(57, ready[1]) == 0 && SC1(57, done[1]) == 0);
    char marker;
    abi_require(SC3(63, ready[0], &marker, 1) == 1 && marker == 'r');
    struct lock_pollfd waiter = { .fd = done[0], .events = 1 };
    struct lock_timespec deadline = { .seconds = 0, .nanoseconds = 200000000 };
    result = CALL(73, &waiter, 1, &deadline, 0, 0, 0);
    abi_record("lock.wait.before-release", result, -1, -1, 0, 0, 0);
    lock.type = 2;
    abi_require(SC3(25, first, 6, &lock) == 0);
    long outcome;
    abi_require(SC3(63, done[0], &outcome, sizeof(outcome)) ==
                (long)sizeof(outcome));
    abi_record("lock.wait.after-release", outcome, -1, -1, 0, 0, 0);
    int child_status = 0;
    abi_require(SC4(260, child, &child_status, 0, 0) == child &&
                child_status == 0);
    abi_require(SC1(57, first) == 0 && SC1(57, ready[0]) == 0 &&
                SC1(57, done[0]) == 0);

    first = abi_open("/data", 2);
    abi_require(first >= 0);
    lock = (struct abi_flock){ .type = 1, .whence = 0,
                               .start = 800, .length = 1 };
    abi_require(SC3(25, first, 6, &lock) == 0);
    abi_require(SC2(59, ready, 0) == 0 && SC2(59, done, 0) == 0);
    child = CALL(220, 17, 0, 0, 0, 0, 0);
    abi_require(child >= 0);
    if (!child) {
        abi_require(SC1(57, ready[0]) == 0 && SC1(57, done[0]) == 0);
        lock.start = 801;
        abi_require(SC3(25, first, 6, &lock) == 0);
        abi_require(SC3(64, ready[1], "r", 1) == 1);
        lock.start = 800;
        outcome = SC3(25, first, 7, &lock);
        abi_require(SC3(64, done[1], &outcome, sizeof(outcome)) ==
                    (long)sizeof(outcome));
        abi_exit(0);
    }
    abi_require(SC1(57, ready[1]) == 0 && SC1(57, done[1]) == 0);
    abi_require(SC3(63, ready[0], &marker, 1) == 1 && marker == 'r');
    waiter = (struct lock_pollfd){ .fd = done[0], .events = 1 };
    deadline = (struct lock_timespec){ .seconds = 0,
                                       .nanoseconds = 200000000 };
    result = CALL(73, &waiter, 1, &deadline, 0, 0, 0);
    abi_require(result == 0);
    lock.start = 801;
    result = SC3(25, first, 7, &lock);
    abi_record("lock.deadlock.detect", result, -1, -1, 0, 0, 0);
    lock.start = 800;
    lock.type = 2;
    abi_require(SC3(25, first, 6, &lock) == 0);
    abi_require(SC3(63, done[0], &outcome, sizeof(outcome)) ==
                (long)sizeof(outcome));
    abi_record("lock.deadlock.release", outcome, -1, -1, 0, 0, 0);
    abi_require(SC4(260, child, &child_status, 0, 0) == child &&
                child_status == 0);
    abi_require(SC1(57, first) == 0 && SC1(57, ready[0]) == 0 &&
                SC1(57, done[0]) == 0);

    for (unsigned restart = 0; restart < 2; ++restart) {
        first = abi_open("/data", 2);
        abi_require(first >= 0);
        lock = (struct abi_flock){ .type = 1, .whence = 0,
                                   .start = 950 + restart, .length = 1 };
        abi_require(SC3(25, first, 6, &lock) == 0);
        abi_require(SC2(59, ready, 0) == 0 && SC2(59, done, 0) == 0);
        child = CALL(220, 17, 0, 0, 0, 0, 0);
        abi_require(child >= 0);
        if (!child) {
            struct lock_action action = {
                (unsigned long)lock_signal_handler,
                restart ? 0x10000000UL : 0, 0
            };
            if (SC4(134, 12, &action, 0, 8) != 0) abi_exit(98);
            abi_require(SC1(57, ready[0]) == 0 && SC1(57, done[0]) == 0);
            abi_require(SC3(64, ready[1], "r", 1) == 1);
            long results[2];
            results[0] = SC3(25, first, 7, &lock);
            results[1] = lock_signal_seen;
            abi_require(SC3(64, done[1], results, sizeof(results)) ==
                        (long)sizeof(results));
            abi_exit(0);
        }
        abi_require(SC1(57, ready[1]) == 0 && SC1(57, done[1]) == 0);
        abi_require(SC3(63, ready[0], &marker, 1) == 1 && marker == 'r');
        waiter = (struct lock_pollfd){ .fd = done[0], .events = 1 };
        deadline = (struct lock_timespec){ .seconds = 0,
                                           .nanoseconds = 200000000 };
        abi_require(CALL(73, &waiter, 1, &deadline, 0, 0, 0) == 0);
        abi_require(SC2(129, child, 12) == 0);
        if (restart) {
            waiter = (struct lock_pollfd){ .fd = done[0], .events = 1 };
            abi_require(CALL(73, &waiter, 1, &deadline, 0, 0, 0) == 0);
            lock.type = 2;
            abi_require(SC3(25, first, 6, &lock) == 0);
        }
        long results[2];
        abi_require(SC3(63, done[0], results, sizeof(results)) ==
                    (long)sizeof(results));
        abi_record(restart ? "lock.signal.restart" : "lock.signal.interrupt",
                   results[0], results[1], -1, 0, 0, 0);
        abi_require(SC4(260, child, &child_status, 0, 0) == child &&
                    child_status == 0);
        abi_require(SC1(57, first) == 0 && SC1(57, ready[0]) == 0 &&
                    SC1(57, done[0]) == 0);
    }

    static const char *exec_names[] = {
        "lock.exec.traditional-keep", "lock.exec.traditional-cloexec",
        "lock.exec.ofd-keep", "lock.exec.ofd-cloexec"
    };
    static const char *exec_arguments[] = {
        "lock-exec-0", "lock-exec-1", "lock-exec-2", "lock-exec-3"
    };
    for (unsigned mode = 0; mode < 4; ++mode) {
        child = CALL(220, 17, 0, 0, 0, 0, 0);
        abi_require(child >= 0);
        if (!child) {
            first = abi_open("/data", 2 | ((mode % 2) ? 02000000 : 0));
            if (first < 0) abi_exit(95);
            lock = (struct abi_flock){ .type = 1, .whence = 0,
                                       .start = 900 + mode, .length = 1 };
            if (SC3(25, first, mode >= 2 ? 37 : 6, &lock) != 0)
                abi_exit(96);
            const char *arguments[] = {"/init", exec_arguments[mode], 0};
            const char *environment[] = {0};
            SC3(221, "/init", arguments, environment);
            abi_exit(97);
        }
        child_status = 0;
        abi_require(SC4(260, child, &child_status, 0, 0) == child);
        abi_record(exec_names[mode], 0, -1, -1, child_status, 0, 0);
    }
}
