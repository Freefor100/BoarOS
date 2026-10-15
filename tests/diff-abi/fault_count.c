#include "abi.h"

static char stat_buffer[4096];
static unsigned long minor_faults(void)
{
    long fd = abi_open("/fault-proc/self/stat", 0);
    abi_require(fd >= 0);
    long count = SC3(63, fd, stat_buffer, sizeof(stat_buffer));
    abi_require(count > 0 && SC1(57, fd) == 0);
    usize at = 0;
    for (long i = 0; i < count; i++) if (stat_buffer[i] == ')') at = (usize)i + 2;
    abi_require(at != 0);
    for (unsigned field = 3; field < 10; field++) {
        while (at < (usize)count && stat_buffer[at] != ' ') at++;
        while (at < (usize)count && stat_buffer[at] == ' ') at++;
    }
    unsigned long value = 0;
    abi_require(at < (usize)count && stat_buffer[at] >= '0' && stat_buffer[at] <= '9');
    while (at < (usize)count && stat_buffer[at] >= '0' && stat_buffer[at] <= '9')
        value = value * 10 + stat_buffer[at++] - '0';
    return value;
}

void abi_fault_count_cases(void)
{
    abi_require(SC3(34, -100, "/fault-proc", 0700) == 0);
    abi_require(SC3(34, -100, "/fault-tmpfs", 0700) == 0);
    abi_require(SC5(40, "proc", "/fault-proc", "proc", 0, 0) == 0);
    abi_require(SC5(40, "tmpfs", "/fault-tmpfs", "tmpfs", 0, "size=1M") == 0);
    long fd = abi_open("/fault-tmpfs/file", 2 | 64 | 512);
    abi_require(fd >= 0 && SC2(46, fd, 2 * ABI_PAGE_SIZE) == 0);
    (void)minor_faults(); /* Warm the sampler buffer before either interval. */
    long shared = SC6(222, 0, 2 * ABI_PAGE_SIZE, 3, 1, fd, 0);
    abi_require(shared >= 0);
    unsigned long before = minor_faults();
    volatile unsigned char first = ((volatile unsigned char *)shared)[0];
    volatile unsigned char second = ((volatile unsigned char *)shared)[ABI_PAGE_SIZE];
    abi_require(first == 0 && second == 0);
    abi_record("boundary.tmpfs-shared-minflt", minor_faults() - before >= 2,
        -1, -1, 0, 0, 0);
    abi_require(SC2(215, shared, 2 * ABI_PAGE_SIZE) == 0);
    long private = SC6(222, 0, 2 * ABI_PAGE_SIZE, 3, 2, fd, 0);
    abi_require(private >= 0);
    first = ((volatile unsigned char *)private)[0];
    second = ((volatile unsigned char *)private)[ABI_PAGE_SIZE];
    before = minor_faults();
    ((volatile unsigned char *)private)[0] = 'a';
    ((volatile unsigned char *)private)[ABI_PAGE_SIZE] = 'b';
    abi_record("boundary.tmpfs-private-cow-minflt", minor_faults() - before >= 2,
        -1, -1, 0, 0, 0);
    abi_require(SC2(215, private, 2 * ABI_PAGE_SIZE) == 0);
    before = minor_faults();
    long ret = SC3(64, fd, 0x10000000, 1); /* Numerically valid, unmapped. */
    abi_record("boundary.tmpfs-failed-fault-count", ret,
        minor_faults() == before, -1, 0, 0, 0);
    abi_require(SC1(57, fd) == 0 && SC3(35, -100, "/fault-tmpfs/file", 0) == 0);
    abi_require(SC2(39, "/fault-tmpfs", 0) == 0 && SC2(39, "/fault-proc", 0) == 0);
    abi_require(SC3(35, -100, "/fault-tmpfs", 0x200) == 0);
    abi_require(SC3(35, -100, "/fault-proc", 0x200) == 0);
}
