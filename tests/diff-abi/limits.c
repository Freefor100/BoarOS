#include "abi.h"

struct abi_limit { unsigned long current, maximum; };

static void touch_large_stack(void)
{
    volatile unsigned char bytes[1048576];
    for (usize i = 0; i < sizeof(bytes); i += 4096) bytes[i] = (unsigned char)i;
}

void abi_limit_exec_probe(void)
{
    struct abi_limit nofile, stack;
    long first = SC4(261, 0, 7, 0, &nofile);
    long second = SC4(261, 0, 3, 0, &stack);
    if (first != 0 || second != 0 || nofile.current != 5 ||
        stack.current != 65535) abi_exit(91);
    stack.current = 2097152;
    if (SC4(261, 0, 3, &stack, 0) != 0) abi_exit(92);
    touch_large_stack();
    abi_exit(0);
}

void abi_limit_cases(void)
{
    struct abi_limit old_nofile, set_nofile, observed;
    long ret = SC4(261, 0, 7, 0, &old_nofile);
    abi_require(ret == 0 && old_nofile.current >= 4 && old_nofile.maximum >= 4);
    set_nofile = old_nofile;
    set_nofile.current = old_nofile.maximum;
    set_nofile.maximum = old_nofile.maximum - 1;
    ret = SC4(261, 0, 7, &set_nofile, 0);
    abi_record("limit.nofile-invalid-range", ret, -1, -1, 0, 0, 0);
    set_nofile = old_nofile;
    set_nofile.current = 3;
    ret = SC4(261, 0, 7, &set_nofile, (void *)1);
    abi_require(SC4(261, 0, 7, 0, &observed) == 0);
    abi_record("limit.nofile-oldfault", ret, observed.current, -1, 0, 0, 0);
    abi_require(SC4(261, 0, 7, &old_nofile, 0) == 0);
    set_nofile = old_nofile;
    set_nofile.current = 4;
    ret = SC4(261, 0, 7, &set_nofile, 0);
    abi_record("limit.nofile-set", ret, -1, -1, 0, 0, 0);
    ret = SC4(261, 0, 7, 0, &observed);
    abi_record("limit.nofile-get", ret, observed.current, -1, 0, 0, 0);

    long fd = abi_open("/data", 0);
    abi_record("limit.nofile-open", fd < 0 ? fd : 0, -1, -1, 0, 0, 0);
    abi_require(fd >= 0);
    ret = abi_open("/data", 0);
    abi_record("limit.nofile-emfile", ret < 0 ? ret : 0, -1, -1, 0, 0, 0);
    if (ret >= 0) abi_require(SC1(57, ret) == 0);
    ret = SC1(23, fd);
    abi_record("limit.nofile-dup", ret < 0 ? ret : 0, -1, -1, 0, 0, 0);
    if (ret >= 0) abi_require(SC1(57, ret) == 0);
    int pair[2];
    ret = SC2(59, pair, 0);
    abi_record("limit.nofile-pipe", ret, -1, -1, 0, 0, 0);
    if (ret == 0) abi_require(SC1(57, pair[0]) == 0 && SC1(57, pair[1]) == 0);
    ret = SC3(25, fd, 0, 4);
    abi_record("limit.nofile-fcntl", ret, -1, -1, 0, 0, 0);

    set_nofile.current = 3;
    abi_require(SC4(261, 0, 7, &set_nofile, 0) == 0);
    unsigned char byte = 0;
    ret = SC3(63, fd, &byte, 1);
    abi_record("limit.nofile-lowered-read", ret, -1, -1, 0,
               ret > 0 ? &byte : 0, ret > 0 ? 1 : 0);
    ret = SC3(24, fd, 4, 0);
    abi_record("limit.nofile-lowered-dup3", ret, -1, -1, 0, 0, 0);
    long child = CALL(220, 17, 0, 0, 0, 0, 0);
    abi_require(child >= 0);
    if (!child) {
        struct abi_limit inherited;
        long result = SC4(261, 0, 7, 0, &inherited);
        abi_exit(result == 0 && inherited.current == 3 ? 0 : 91);
    }
    int status = 0;
    abi_require(SC4(260, child, &status, 0, 0) == child);
    abi_record("limit.nofile-fork-inherit", 0, -1, -1, status, 0, 0);
    abi_require(SC4(261, 0, 7, &old_nofile, 0) == 0);
    ret = SC4(261, 0, 7, 0, &observed);
    abi_record("limit.nofile-restored", ret, observed.current == old_nofile.current,
               -1, 0, 0, 0);
    abi_require(SC1(57, fd) == 0);

    child = CALL(220, 17, 0, 0, 0, 0, 0);
    abi_require(child >= 0);
    if (!child) for (;;) SC0(124); /* Yield until parent sends SIGKILL. */
    struct abi_limit child_before, child_limit = old_nofile;
    child_limit.current = 6;
    ret = SC4(261, child, 7, &child_limit, &child_before);
    abi_record("limit.nofile-other-pid", ret,
               ret == 0 ? child_before.current == old_nofile.current : -1,
               -1, 0, 0, 0);
    abi_require(SC4(261, child, 7, 0, &observed) == 0);
    abi_record("limit.nofile-other-get", 0, observed.current, -1, 0, 0, 0);
    abi_require(SC2(129, child, 9) == 0);
    abi_require(SC4(260, child, &status, 0, 0) == child);

    struct abi_limit old_stack, set_stack;
    ret = SC4(261, 0, 3, 0, &old_stack);
    abi_record("limit.stack-default", ret,
               ret == 0 ? (long)old_stack.current : -1,
               -1, 0, 0, 0);
    set_stack = old_stack;
    set_stack.current = 65535;
    ret = SC4(261, 0, 3, &set_stack, 0);
    abi_record("limit.stack-set", ret, -1, -1, 0, 0, 0);
    child = CALL(220, 17, 0, 0, 0, 0, 0);
    abi_require(child >= 0);
    if (!child) { touch_large_stack(); abi_exit(90); }
    status = 0;
    abi_require(SC4(260, child, &status, 0, 0) == child);
    abi_record("limit.stack-overflow", 0, -1, -1, status, 0, 0);

    child = CALL(220, 17, 0, 0, 0, 0, 0);
    abi_require(child >= 0);
    if (!child) {
        struct abi_limit child_nofile = old_nofile;
        char *arguments[] = {"/init", "limit-exec-probe", 0};
        char *environment[] = {0};
        child_nofile.current = 5;
        if (SC4(261, 0, 7, &child_nofile, 0) != 0) abi_exit(92);
        SC3(221, "/init", arguments, environment);
        abi_exit(93);
    }
    status = 0;
    abi_require(SC4(260, child, &status, 0, 0) == child);
    abi_record("limit.exec-inherit-grow", 0, -1, -1, status, 0, 0);
    abi_require(SC4(261, 0, 3, &old_stack, 0) == 0);
    ret = SC4(261, 0, 3, 0, &observed);
    abi_record("limit.stack-restored", ret,
               observed.current == old_stack.current, -1, 0, 0, 0);
}
