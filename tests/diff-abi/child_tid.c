#include "abi.h"
#define CHILD_SET 0x1000000
#define CHILD_CLEAR 0x200000
static unsigned char stack[16384] __attribute__((aligned(16)));
extern long abi_clone_entry(long flags, void *stack, int *tid,
                            void (*fn)(void *), void *arg);
struct child_args { volatile int *tid; int exec; int observed; };
static void child_entry(void *pointer)
{
    struct child_args *a = pointer;
    a->observed = *a->tid == SC0(178);
    if (a->exec) {
        const char *args[] = {"/init", "identity-exec-probe", 0};
        const char *env[] = {0};
        if (a->exec == 2) {
            abi_require(SC3(221, "/missing-tid-interpreter", args, env) == -2);
            abi_require(*a->tid == SC0(178));
            return;
        }
        SC3(221, args[0], args, env);
        abi_exit(91);
    }
}
void abi_child_tid_cases(void)
{
    int private_tid = 73, status = 0;
    long child = CALL(220, 17 | CHILD_SET | CHILD_CLEAR, 0, 0, 0, &private_tid, 0);
    abi_require(child >= 0);
    if (!child) abi_exit(private_tid == SC0(178) ? 0 : 1);
    abi_require(SC4(260, child, &status, 0, 0) == child);
    abi_record("tid.fork-private", private_tid, -1, -1, status, 0, 0);

    volatile int *shared = (void *)CALL(222, 0, 4096, 3, 0x21, -1, 0);
    abi_require((long)shared > 0);
    *shared = 73;
    child = CALL(220, 17 | CHILD_CLEAR, 0, 0, 0, shared, 0);
    abi_require(child >= 0);
    if (!child) abi_exit(0);
    abi_require(SC4(260, child, &status, 0, 0) == child);
    abi_record("tid.fork-shared-page", *shared, -1, -1, status, 0, 0);
    for (int mode = 0; mode < 3; mode++) {
        private_tid = 73;
        struct child_args args = {&private_tid, mode, 0};
        child = abi_clone_entry(17 | 0x4100 | CHILD_SET | CHILD_CLEAR,
                                stack + sizeof(stack), &private_tid, child_entry, &args);
        abi_require(child > 0);
        int resumed_tid = private_tid;
        abi_require(SC4(260, child, &status, 0, 0) == child);
        abi_record(mode == 2 ? "tid.vfork-exec-failure" :
                   mode ? "tid.vfork-exec" : "tid.vfork-exit", resumed_tid,
                   args.observed, -1, status, 0, 0);
    }
    for (int mode = 0; mode < 2; mode++) {
        void *address = mode ? (void *)shared : (void *)8;
        *shared = 73;
        if (mode) abi_require(SC3(226, shared, 4096, 1) == 0);
        child = CALL(220, 17 | CHILD_SET | CHILD_CLEAR, 0, 0, 0, address, 0);
        abi_require(child >= 0);
        if (!child) abi_exit(0);
        abi_require(SC4(260, child, &status, 0, 0) == child);
        abi_record(mode ? "tid.readonly" : "tid.bad-address", 0, -1, -1, status, 0, 0);
        if (mode) abi_require(SC3(226, shared, 4096, 3) == 0);
    }
    private_tid = 73;
    struct child_args args = {&private_tid, 0, 0};
    child = abi_clone_entry(0x10f00 | CHILD_SET | CHILD_CLEAR,
                            stack + sizeof(stack), &private_tid, child_entry, &args);
    abi_require(child > 0);
    struct { long sec, ns; } timeout = {2, 0};
    while (__atomic_load_n(&private_tid, __ATOMIC_ACQUIRE)) {
        int value = __atomic_load_n(&private_tid, __ATOMIC_ACQUIRE);
        if (!value) break;
        long r = CALL(98, &private_tid, 0, value, &timeout, 0, 0);
        abi_require(r == 0 || r == -11 || r == -4);
    }
    abi_record("tid.thread-wake", private_tid, args.observed, -1, 0, 0, 0);
    abi_require(SC2(215, shared, 4096) == 0);
}
