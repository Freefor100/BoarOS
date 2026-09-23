#include "abi.h"

struct robust_head {
    void *next;
    long offset;
    void *pending;
};

void abi_robust_cases(void)
{
    struct robust_head head = {0};
    void *observed = (void *)1;
    unsigned long length = 0;
    long result;

    head.next = &head;
    result = SC2(99, &head, sizeof(head) - 1);
    abi_record("robust.set-bad-size", result, -1, -1, 0, 0, 0);
    result = SC2(99, &head, sizeof(head));
    abi_record("robust.set", result, -1, -1, 0, 0, 0);
    result = SC3(100, 0, &observed, &length);
    abi_record("robust.get-self", result, (long)length, -1,
               observed == &head, 0, 0);
    observed = (void *)1;
    result = SC3(100, -1, &observed, &length);
    abi_record("robust.get-missing", result, -1, -1,
               observed == (void *)1, 0, 0);
    length = 0;
    result = SC3(100, 0, (void *)8, &length);
    abi_record("robust.get-head-fault", result, (long)length, -1,
               0, 0, 0);
    observed = (void *)1;
    result = SC3(100, 0, &observed, (void *)8);
    abi_record("robust.get-length-fault", result, -1, -1,
               observed == (void *)1, 0, 0);

    long child = CALL(220, 17, 0, 0, 0, 0, 0);
    abi_require(child >= 0);
    if (!child) {
        observed = (void *)1;
        length = 0;
        result = SC3(100, 0, &observed, &length);
        abi_exit(result == 0 && observed == 0 && length == sizeof(head)
                     ? 0 : 91);
    }
    int status = 0;
    abi_require(SC4(260, child, &status, 0, 0) == child);
    abi_record("robust.fork-reset", 0, -1, -1, status, 0, 0);

    result = SC2(99, 0, sizeof(head));
    abi_record("robust.unregister", result, -1, -1, 0, 0, 0);
    observed = (void *)1;
    result = SC3(100, 0, &observed, &length);
    abi_record("robust.get-unregistered", result, (long)length, -1,
               observed == 0, 0, 0);
}
