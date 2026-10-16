#include "abi.h"

struct futex_timeout { long seconds, nanoseconds; };

#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_CLOCK_REALTIME 256

void abi_futex_bitset_cases(void)
{
    int word = 1;
    const struct futex_timeout expired = {0, 0};
    const struct futex_timeout pause = {0, 20000000};
    long result;

    result = CALL(98, &word, FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG,
                  1, &expired, 0, 0);
    abi_record("futex.bitset-zero-wait", result, -1, -1, 0, 0, 0);
    result = CALL(98, &word, FUTEX_WAKE_BITSET | FUTEX_PRIVATE_FLAG,
                  1, 0, 0, 0);
    abi_record("futex.bitset-zero-wake", result, -1, -1, 0, 0, 0);
    result = CALL(98, &word, FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG,
                  0, &expired, 0, 2);
    abi_record("futex.bitset-mismatch", result, -1, -1, 0, 0, 0);
    result = CALL(98, &word, FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG,
                  1, &expired, 0, 2);
    abi_record("futex.bitset-monotonic-expired", result, -1, -1, 0, 0, 0);
    result = CALL(98, &word, FUTEX_WAIT_BITSET | FUTEX_CLOCK_REALTIME,
                  1, &expired, 0, 2);
    abi_record("futex.bitset-realtime-expired", result, -1, -1, 0, 0, 0);
    result = CALL(98, &word, FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG,
                  1, (void *)8, 0, 2);
    abi_record("futex.bitset-timeout-fault", result, -1, -1, 0, 0, 0);
    result = CALL(98, &word, FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG,
                  1, (void *)8, 0, 0);
    abi_record("futex.bitset-timeout-before-mask", result, -1, -1, 0, 0, 0);
    result = CALL(98, (char *)&word + 1,
                  FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG,
                  1, (void *)8, 0, 2);
    abi_record("futex.bitset-timeout-before-address", result, -1, -1, 0, 0, 0);
    result = CALL(98, &word, FUTEX_WAKE_BITSET | FUTEX_CLOCK_REALTIME,
                  1, 0, 0, 2);
    abi_record("futex.bitset-clock-invalid-wake", result, -1, -1, 0, 0, 0);

    long shared = CALL(222, 0, ABI_PAGE_SIZE, 3, 0x21, -1, 0);
    int ready[2], status = 0;
    char signal;
    abi_require(shared >= 0 && SC2(59, ready, 0) == 0);
    long child = CALL(220, 17, 0, 0, 0, 0, 0);
    abi_require(child >= 0);
    if (child == 0) {
        struct futex_timeout deadline;

        SC1(57, ready[0]);
        if (SC2(113, 1, &deadline) != 0) abi_exit(20);
        deadline.seconds += 2;
        abi_require(SC3(64, ready[1], "r", 1) == 1);
        abi_exit(CALL(98, shared, FUTEX_WAIT_BITSET, 0,
                       &deadline, 0, 2) == 0 ? 0 : 21);
    }
    abi_require(SC1(57, ready[1]) == 0);
    abi_require(SC3(63, ready[0], &signal, 1) == 1);
    abi_require(SC2(101, &pause, 0) == 0);
    result = CALL(98, shared, FUTEX_WAKE_BITSET, 1, 0, 0, 1);
    abi_record("futex.bitset-nonintersect", result, -1, -1, 0, 0, 0);
    long matching = CALL(98, shared, FUTEX_WAKE_BITSET, 1, 0, 0, 2);
    abi_require(SC4(260, child, &status, 0, 0) == child);
    abi_record("futex.bitset-intersect", matching, -1, -1,
               status, 0, 0);
    abi_require(SC1(57, ready[0]) == 0);
    abi_require(SC2(215, shared, ABI_PAGE_SIZE) == 0);

    shared = CALL(222, 0, ABI_PAGE_SIZE, 3, 0x21, -1, 0);
    abi_require(shared >= 0 && SC2(59, ready, 0) == 0);
    child = CALL(220, 17, 0, 0, 0, 0, 0);
    abi_require(child >= 0);
    if (child == 0) {
        struct futex_timeout deadline;

        SC1(57, ready[0]);
        if (SC2(113, 1, &deadline) != 0) abi_exit(20);
        deadline.seconds += 2;
        abi_require(SC3(64, ready[1], "r", 1) == 1);
        abi_exit(CALL(98, shared, FUTEX_WAIT_BITSET, 0,
                       &deadline, 0, 2) == 0 ? 0 : 21);
    }
    abi_require(SC1(57, ready[1]) == 0);
    abi_require(SC3(63, ready[0], &signal, 1) == 1);
    abi_require(SC2(101, &pause, 0) == 0);
    long requeued = CALL(98, shared, 3, 0, 1, shared + 4, 0);
    long nonintersect = CALL(98, shared + 4, FUTEX_WAKE_BITSET,
                             1, 0, 0, 1);
    matching = CALL(98, shared + 4, FUTEX_WAKE_BITSET,
                    1, 0, 0, 2);
    abi_require(SC4(260, child, &status, 0, 0) == child);
    abi_record("futex.bitset-requeue", requeued, nonintersect,
               matching, status, 0, 0);
    abi_require(SC1(57, ready[0]) == 0);
    abi_require(SC2(215, shared, ABI_PAGE_SIZE) == 0);
}

static long fork_wait(void *word, int read_end, int write_end,
                      void *unmap_first)
{
    long child = CALL(220, 17, 0, 0, 0, 0, 0);

    abi_require(child >= 0);
    if (child == 0) {
        struct futex_timeout timeout = {2, 0};

        SC1(57, read_end);
        if (unmap_first != 0)
            abi_require(SC2(215, unmap_first, ABI_PAGE_SIZE) == 0);
        abi_require(SC3(64, write_end, "r", 1) == 1);
        abi_exit(CALL(98, word, 0, 0, &timeout, 0, 0) == 0 ? 0 : 21);
    }
    return child;
}

static long wait_until_queued(void *source, void *target, int requeue)
{
    long result = 0;

    for (int attempt = 0; attempt < 10000 && result == 0; attempt++) {
        result = requeue
                     ? CALL(98, source, 3, 0, 1, target, 0)
                     : CALL(98, source, 1, 1, 0, 0, 0);
        if (result == 0) SC0(124);
    }
    return result;
}

static void backing_wait_case(const char *name, long source, long alias,
                              long wrong_offset, long other)
{
    int ready[2], status = 0;
    char signal;
    abi_require(SC2(59, ready, 0) == 0);
    long child = CALL(220, 17, 0, 0, 0, 0, 0);
    abi_require(child >= 0);
    if (!child) {
        struct futex_timeout deadline;
        abi_require(SC1(57, ready[0]) == 0 && SC2(113, 1, &deadline) == 0);
        deadline.seconds += 2;
        abi_require(SC3(64, ready[1], "r", 1) == 1);
        abi_exit(CALL(98, source, FUTEX_WAIT_BITSET, 0, &deadline, 0, 2) == 0 ? 0 : 21);
    }
    abi_require(SC1(57, ready[1]) == 0);
    abi_require(SC3(63, ready[0], &signal, 1) == 1);
    /* 实际 requeue 成功才说明子进程已经登记，随后检查隔离与目标唤醒。 */
    long moved = wait_until_queued((void *)alias, (void *)(alias + 4), 1);
    long isolated = CALL(98, wrong_offset + 4, 1, 1, 0, 0, 0) |
                    CALL(98, other + 4, 1, 1, 0, 0, 0) |
                    CALL(98, alias + 4, 1 | FUTEX_PRIVATE_FLAG, 1, 0, 0, 0);
    isolated |= CALL(98, alias + 4, FUTEX_WAKE_BITSET, 1, 0, 0, 1);
    long woken = CALL(98, alias + 4, FUTEX_WAKE_BITSET, 1, 0, 0, 2);
    abi_require(SC4(260, child, &status, 0, 0) == child);
    abi_record(name, moved, isolated, woken, status, 0, 0);
    abi_require(SC1(57, ready[0]) == 0);
}

static void file_robust_case(long owner_map, long waiter_map)
{
    struct robust_head { void *next; long offset; void *pending; };
    struct robust_node { void *next; unsigned word; };
    struct robust_node *node = (void *)(owner_map + 64);
    int owner_ready[2], release[2], waiter_ready[2], owner_status, waiter_status;
    char signal;
    abi_require(SC2(59, owner_ready, 0) == 0 && SC2(59, release, 0) == 0 &&
                SC2(59, waiter_ready, 0) == 0);
    long owner = CALL(220, 17, 0, 0, 0, 0, 0);
    abi_require(owner >= 0);
    if (!owner) {
        struct robust_head head = {node, 8, 0};
        node->next = &head;
        node->word = (unsigned)SC0(178) | 0x80000000U;
        abi_require(SC2(99, &head, sizeof(head)) == 0 &&
                    SC3(64, owner_ready[1], "r", 1) == 1 &&
                    SC3(63, release[0], &signal, 1) == 1);
        abi_exit(0);
    }
    abi_require(SC3(63, owner_ready[0], &signal, 1) == 1);
    long word = waiter_map + 72;
    unsigned expected = *(volatile unsigned *)word;
    long waiter = CALL(220, 17, 0, 0, 0, 0, 0);
    abi_require(waiter >= 0);
    if (!waiter) {
        const struct futex_timeout timeout = {2, 0};
        abi_require(SC3(64, waiter_ready[1], "r", 1) == 1);
        abi_exit(CALL(98, word, 0, expected, &timeout, 0, 0) == 0 ? 0 : 22);
    }
    abi_require(SC3(63, waiter_ready[0], &signal, 1) == 1);
    long moved = wait_until_queued((void *)word, (void *)(word + 4), 1);
    long restored = wait_until_queued((void *)(word + 4), (void *)word, 1);
    abi_require(SC3(64, release[1], "x", 1) == 1 &&
                SC4(260, owner, &owner_status, 0, 0) == owner &&
                SC4(260, waiter, &waiter_status, 0, 0) == waiter);
    abi_record("futex.file-robust", moved, restored, owner_status,
               waiter_status, 0, 0);
    abi_record("futex.file-owner-died", node->word == 0xc0000000U,
               -1, -1, 0, 0, 0);
    for (unsigned i=0; i<2; i++)
        abi_require(SC1(57, owner_ready[i]) == 0 && SC1(57, release[i]) == 0 &&
                    SC1(57, waiter_ready[i]) == 0);
}

static void file_and_sysv_cases(void)
{
    long fd = abi_open("/futex-data", 2 | 64 | 128);
    long other_fd = abi_open("/futex-other", 2 | 64 | 128);
    abi_require(fd >= 0 && other_fd >= 0);
    abi_require(SC2(46, fd, 2 * ABI_PAGE_SIZE) == 0 &&
                SC2(46, other_fd, ABI_PAGE_SIZE) == 0);
    abi_require(SC5(37, -100, "/futex-data", -100, "/futex-link", 0) == 0);
    long independent = abi_open("/futex-data", 2);
    long linked = abi_open("/futex-link", 2);
    abi_require(independent >= 0 && linked >= 0);
    long a = CALL(222, 0, 2 * ABI_PAGE_SIZE, 3, 1, fd, 0);
    long b = CALL(222, 0, 2 * ABI_PAGE_SIZE, 3, 1, fd, 0);
    long c = CALL(222, 0, 2 * ABI_PAGE_SIZE, 3, 1, independent, 0);
    long d = CALL(222, 0, ABI_PAGE_SIZE, 3, 1, linked, ABI_PAGE_SIZE);
    long other = CALL(222, 0, ABI_PAGE_SIZE, 3, 1, other_fd, 0);
    abi_require(a >= 0 && b >= 0 && c >= 0 && d >= 0 && other >= 0);
    abi_require(SC1(57, fd) == 0 && SC1(57, independent) == 0 &&
                SC1(57, linked) == 0 && SC1(57, other_fd) == 0);
    abi_require(SC3(35, -100, "/futex-data", 0) == 0 &&
                SC3(35, -100, "/futex-link", 0) == 0);
    backing_wait_case("futex.file-alias", a + ABI_PAGE_SIZE,
                      b + ABI_PAGE_SIZE, b, other);
    backing_wait_case("futex.file-independent-open", a + ABI_PAGE_SIZE,
                      c + ABI_PAGE_SIZE, c, other);
    backing_wait_case("futex.file-hardlink-offset", a + ABI_PAGE_SIZE,
                      d, c, other);
    int ready[2], status;
    char signal;
    abi_require(SC2(59, ready, 0) == 0);
    long child = fork_wait((void *)d, ready[0], ready[1], 0);
    abi_require(SC1(57, ready[1]) == 0 && SC3(63, ready[0], &signal, 1) == 1);
    long moved = wait_until_queued((void *)d, (void *)(d + 4), 1);
    abi_require(SC2(129, child, 9) == 0 && SC4(260, child, &status, 0, 0) == child);
    abi_record("futex.file-killed", moved, CALL(98, d + 4, 1, 1, 0, 0, 0),
               -1, status, 0, 0);
    abi_require(SC1(57, ready[0]) == 0);
    file_robust_case(a + ABI_PAGE_SIZE, d);
    const struct futex_timeout expired = {0, 0};
    abi_record("futex.file-timeout", CALL(98, d, 0, 0, &expired, 0, 0),
               -1, -1, 0, 0, 0);
    abi_require(SC2(215, a, 2 * ABI_PAGE_SIZE) == 0 &&
                SC2(215, b, 2 * ABI_PAGE_SIZE) == 0 &&
                SC2(215, c, 2 * ABI_PAGE_SIZE) == 0 &&
                SC2(215, d, ABI_PAGE_SIZE) == 0);
    long id = SC3(194, 0, 2 * ABI_PAGE_SIZE, 0600 | 01000);
    abi_require(id >= 0);
    a = SC3(196, id, 0, 0);
    b = SC3(196, id, 0, 0);
    abi_require(a >= 0 && b >= 0 && SC3(195, id, 0, 0) == 0);
    backing_wait_case("futex.sysv-alias", a + ABI_PAGE_SIZE,
                      b + ABI_PAGE_SIZE, b, other);
    abi_require(SC1(197, a) == 0 && SC1(197, b) == 0 &&
                SC2(215, other, ABI_PAGE_SIZE) == 0);
    abi_require(SC3(35, -100, "/futex-other", 0) == 0);
}

void abi_futex_shared_cases(void)
{
    long shared = CALL(222, 0, (2 * ABI_PAGE_SIZE), 3, 0x21, -1, 0);
    int ready[2], status = 0;
    char signal;
    long child, result;

    abi_require(shared >= 0 && SC2(59, ready, 0) == 0);
    child = fork_wait((void *)(shared + ABI_PAGE_SIZE), ready[0], ready[1],
                      (void *)shared);
    abi_require(SC1(57, ready[1]) == 0);
    abi_require(SC3(63, ready[0], &signal, 1) == 1);
    result = wait_until_queued((void *)(shared + ABI_PAGE_SIZE), 0, 0);
    abi_require(SC4(260, child, &status, 0, 0) == child);
    abi_record("futex.shared-wake", result, -1, -1, status, 0, 0);
    abi_require(SC1(57, ready[0]) == 0);
    abi_require(SC2(215, shared, (2 * ABI_PAGE_SIZE)) == 0);

    shared = CALL(222, 0, ABI_PAGE_SIZE, 3, 0x21, -1, 0);
    long target = CALL(222, 0, ABI_PAGE_SIZE, 3, 0x22, -1, 0);
    abi_require(shared >= 0 && target >= 0 && SC2(59, ready, 0) == 0);
    child = fork_wait((void *)shared, ready[0], ready[1], 0);
    abi_require(SC1(57, ready[1]) == 0);
    abi_require(SC3(63, ready[0], &signal, 1) == 1);
    result = wait_until_queued((void *)shared, (void *)target, 1);
    long source_wake = CALL(98, shared, 1, 1, 0, 0, 0);
    abi_require(SC2(215, shared, ABI_PAGE_SIZE) == 0);
    long target_wake = CALL(98, target, 1, 1, 0, 0, 0);
    abi_require(SC4(260, child, &status, 0, 0) == child);
    abi_record("futex.shared-requeue", result, source_wake,
               target_wake, status, 0, 0);
    abi_require(SC1(57, ready[0]) == 0);
    abi_require(SC2(215, target, ABI_PAGE_SIZE) == 0);

    result = CALL(98, 8, 1, 1, 0, 0, 0);
    abi_record("futex.shared-bad-wake", result, -1, -1, 0, 0, 0);
    shared = CALL(222, 0, ABI_PAGE_SIZE, 3, 0x21, -1, 0);
    abi_require(shared >= 0 && SC3(226, shared, ABI_PAGE_SIZE, 0) == 0);
    result = CALL(98, shared, 1, 1, 0, 0, 0);
    abi_record("futex.shared-protected-wake", result, -1, -1, 0, 0, 0);
    abi_require(SC2(215, shared, ABI_PAGE_SIZE) == 0);
    shared = CALL(222, 0, ABI_PAGE_SIZE, 3, 0x21, -1, 0);
    abi_require(shared >= 0);
    *(volatile unsigned *)shared = 0;
    abi_require(SC3(226, shared, ABI_PAGE_SIZE, 4) == 0);
    abi_record("futex.shared-exec-only-wake", CALL(98, shared, 1, 1, 0, 0, 0),
               -1, -1, 0, 0, 0);
    abi_require(SC2(215, shared, ABI_PAGE_SIZE) == 0);
    file_and_sysv_cases();
}
