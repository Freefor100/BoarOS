#include "abi.h"

struct futex_timeout { long seconds, nanoseconds; };

static long fork_wait(void *word, int read_end, int write_end,
                      void *unmap_first)
{
    long child = CALL(220, 17, 0, 0, 0, 0, 0);

    abi_require(child >= 0);
    if (child == 0) {
        struct futex_timeout timeout = {2, 0};

        SC1(57, read_end);
        if (unmap_first != 0)
            abi_require(SC2(215, unmap_first, 4096) == 0);
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

void abi_futex_shared_cases(void)
{
    long shared = CALL(222, 0, 8192, 3, 0x21, -1, 0);
    int ready[2], status = 0;
    char signal;
    long child, result;

    abi_require(shared >= 0 && SC2(59, ready, 0) == 0);
    child = fork_wait((void *)(shared + 4096), ready[0], ready[1],
                      (void *)shared);
    abi_require(SC1(57, ready[1]) == 0);
    abi_require(SC3(63, ready[0], &signal, 1) == 1);
    result = wait_until_queued((void *)(shared + 4096), 0, 0);
    abi_require(SC4(260, child, &status, 0, 0) == child);
    abi_record("futex.shared-wake", result, -1, -1, status, 0, 0);
    abi_require(SC1(57, ready[0]) == 0);
    abi_require(SC2(215, shared, 8192) == 0);

    shared = CALL(222, 0, 4096, 3, 0x21, -1, 0);
    long target = CALL(222, 0, 4096, 3, 0x22, -1, 0);
    abi_require(shared >= 0 && target >= 0 && SC2(59, ready, 0) == 0);
    child = fork_wait((void *)shared, ready[0], ready[1], 0);
    abi_require(SC1(57, ready[1]) == 0);
    abi_require(SC3(63, ready[0], &signal, 1) == 1);
    result = wait_until_queued((void *)shared, (void *)target, 1);
    long source_wake = CALL(98, shared, 1, 1, 0, 0, 0);
    abi_require(SC2(215, shared, 4096) == 0);
    long target_wake = CALL(98, target, 1, 1, 0, 0, 0);
    abi_require(SC4(260, child, &status, 0, 0) == child);
    abi_record("futex.shared-requeue", result, source_wake,
               target_wake, status, 0, 0);
    abi_require(SC1(57, ready[0]) == 0);
    abi_require(SC2(215, target, 4096) == 0);

    result = CALL(98, 8, 1, 1, 0, 0, 0);
    abi_record("futex.shared-bad-wake", result, -1, -1, 0, 0, 0);
    shared = CALL(222, 0, 4096, 3, 0x21, -1, 0);
    abi_require(shared >= 0 && SC3(226, shared, 4096, 0) == 0);
    result = CALL(98, shared, 1, 1, 0, 0, 0);
    abi_record("futex.shared-protected-wake", result, -1, -1, 0, 0, 0);
    abi_require(SC2(215, shared, 4096) == 0);
}
