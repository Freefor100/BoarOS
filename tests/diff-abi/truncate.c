#include "abi.h"

static unsigned char contents[8192];

static void bus_case(const char *name, int cow, int protect, int foreign)
{
    long fd = abi_open("/truncate-bus", 2 | 64 | 512);
    abi_require(fd >= 0 && SC3(64, fd, contents, sizeof(contents)) == 8192);
    long map = CALL(222, 0, 8192, 3, 2, fd, 0);
    abi_require(map >= 0);
    volatile unsigned char *p = (void *)(map + 4096);
    abi_require(*p == 'F');
    if (cow) *p = 'C';
    if (protect) abi_require(SC3(226, p, 4096, 0) == 0);
    int gate[2];
    abi_require(SC2(59, gate, 0) == 0);
    long child = CALL(220, 17, 0, 0, 0, 0, 0);
    abi_require(child >= 0);
    if (!child) {
        if (foreign) {
            char token;
            abi_require(SC3(63, gate[0], &token, 1) == 1);
        } else {
            abi_require(SC2(46, fd, 4096) == 0);
        }
        abi_require(SC1(57, fd) == 0);
        if (protect) abi_require(SC3(226, p, 4096, 3) == 0);
        volatile unsigned char value = *p;
        (void)value;
        abi_exit(92);
    }
    abi_require(SC3(35, -100, "/truncate-bus", 0) == 0);
    if (foreign) {
        abi_require(SC2(46, fd, 4096) == 0);
        abi_require(SC3(64, gate[1], "x", 1) == 1);
    }
    int status = 0;
    abi_require(SC4(260, child, &status, 0, 0) == child);
    abi_record(name, 0, abi_size(fd), abi_offset(fd), status, 0, 0);
    abi_require(SC1(57, gate[0]) == 0 && SC1(57, gate[1]) == 0);
    abi_require(SC2(215, map, 8192) == 0 && SC1(57, fd) == 0);
}

void abi_truncate_cases(void)
{
    static const char *names[] = {
        "truncate.local.clean", "truncate.local.cow",
        "truncate.local.protected", "truncate.local.protected-cow",
        "truncate.foreign.clean", "truncate.foreign.cow",
        "truncate.foreign.protected", "truncate.foreign.protected-cow"
    };
    for (usize i = 0; i < sizeof(contents); i++) contents[i] = 'F';
    for (int i = 0; i < 8; i++) bus_case(names[i], i & 1, i & 2, i & 4);
    for (int evict = 0; evict < 2; evict++) {
        long fd = abi_open("/truncate-tail", 2 | 64 | 512);
        abi_require(fd >= 0 && SC3(64, fd, contents, sizeof(contents)) == 8192);
        long other = abi_open("/truncate-tail", 2);
        abi_require(other >= 0);
        long a = CALL(222, 0, 4096, 3, 2, fd, 4096);
        long b = CALL(222, 0, 4096, 3, 2, other, 4096);
        abi_require(a >= 0 && b >= 0);
        volatile unsigned char *clean = (void *)a, *dirty = (void *)b;
        abi_require(clean[100] == 'F');
        dirty[0] = 'D'; dirty[100] = 'P';
        if (evict) {
            abi_require(SC3(62, fd, 0, 0) == 0);
            abi_require(SC3(64, fd, "X", 1) == 1);
        }
        long ret = SC2(46, other, 4096 + 32);
        unsigned char data[] = {clean[0], clean[31], clean[32], clean[100],
                                 dirty[0], dirty[31], dirty[32], dirty[100]};
        abi_record(evict ? "truncate.tail.evicted" : "truncate.tail.cached",
                    ret, abi_size(fd), abi_offset(other), 0, data, sizeof(data));
        ret = SC2(46, other, 8192);
        data[0] = clean[100]; data[1] = dirty[100];
        abi_record(evict ? "truncate.grow.evicted" : "truncate.grow.cached",
                    ret, abi_size(fd), abi_offset(other), 0, data, 2);
        /* O_TRUNC goes through the same mutation reconciliation. */
        long trunc = abi_open("/truncate-tail", 2 | 512);
        abi_require(trunc >= 0 && SC1(57, trunc) == 0);
        abi_require(SC2(46, other, 8192) == 0);
        data[0] = clean[0]; data[1] = dirty[0];
        abi_record(evict ? "truncate.reopen.evicted" : "truncate.reopen.cached",
                    0, abi_size(fd), abi_offset(other), 0, data, 2);
        abi_require(SC2(215, a, 4096) == 0 && SC2(215, b, 4096) == 0);
        abi_require(SC1(57, fd) == 0 && SC1(57, other) == 0);
    }
}
