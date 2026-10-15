#include "abi.h"

static void scalar_ranges(void)
{
    long fd = abi_open("/boundary-range", 2 | 64 | 512);
    long map = SC6(222, 0x10000000, 2 * ABI_PAGE_SIZE, 3, 0x32, -1, 0);
    abi_require(fd >= 0 && map == 0x10000000);
    for (usize i = 0; i < ABI_PAGE_SIZE; i++) ((unsigned char *)map)[i] = 'x';
    abi_require(SC3(226, map + ABI_PAGE_SIZE, ABI_PAGE_SIZE, 0) == 0);
    struct abi_stat before, after;
    abi_require(SC2(80, fd, &before) == 0);
    long ret = SC3(64, fd, map, (usize)-1);
    abi_require(SC2(80, fd, &after) == 0);
    abi_record("boundary.write-overflow", ret, after.size, abi_offset(fd),
        before.mtime == after.mtime && before.mtime_nsec == after.mtime_nsec &&
        before.ctime == after.ctime && before.ctime_nsec == after.ctime_nsec, 0, 0);
    abi_require(SC2(46, fd, 0) == 0 && SC3(62, fd, 0, 0) == 0);
    abi_require(SC2(80, fd, &before) == 0);
    ret = SC4(68, fd, map, (usize)-1, 0);
    abi_require(SC2(80, fd, &after) == 0);
    abi_record("boundary.pwrite-overflow", ret, after.size, abi_offset(fd),
        before.mtime == after.mtime && before.mtime_nsec == after.mtime_nsec &&
        before.ctime == after.ctime && before.ctime_nsec == after.ctime_nsec, 0, 0);
    abi_require(SC2(46, fd, 0) == 0);
    const char *ids[] = {"boundary.read-zero-high", "boundary.write-zero-high",
        "boundary.pread-zero-high", "boundary.pwrite-zero-high"};
    const long nr[] = {63, 64, 67, 68};
    for (unsigned i = 0; i < 4; i++) {
        ret = SC4(nr[i], fd, (void *)-1, 0, 0);
        abi_record(ids[i], ret, abi_size(fd), abi_offset(fd), 0, 0, 0);
    }
    for (unsigned i = 0; i < 4; i++) abi_require(SC4(nr[i], fd, 0, 0, 0) == 0);
    abi_record("boundary.scalar-zero-null", 0, abi_size(fd), abi_offset(fd), 0, 0, 0);
    abi_require(SC2(215, map, 2 * ABI_PAGE_SIZE) == 0 && SC1(57, fd) == 0);
    abi_require(SC3(35, -100, "/boundary-range", 0) == 0);
}

static void setfl_preserves_flags(void)
{
    const long flags[] = {0x101000, 0x1000, 0x10000, 0x20000};
    const char *ids[] = {"boundary.setfl-sync", "boundary.setfl-dsync",
        "boundary.setfl-directory", "boundary.setfl-nofollow"};
    for (unsigned i = 0; i < 4; i++) {
        long fd = abi_open(i == 2 ? "/" : "/boundary-flags",
            flags[i] | (i == 2 ? 0 : 2 | 64 | 512));
        abi_require(fd >= 0);
        long original = SC3(25, fd, 3, 0), alias = SC1(23, fd);
        abi_require(original >= 0 && alias >= 0);
        long ret = SC3(25, fd, 4, original | 0x800);
        long observed = SC3(25, alias, 3, 0);
        abi_record(ids[i], ret, observed == (original | 0x800), -1, 0, 0, 0);
        /* Access mode/open-only bits cannot mutate the shared OFD. */
        ret = SC3(25, alias, 4, 64 | (original ^ 3));
        observed = SC3(25, fd, 3, 0);
        abi_record(i == 0 ? "boundary.setfl-sync-clear" :
            i == 1 ? "boundary.setfl-dsync-clear" :
            i == 2 ? "boundary.setfl-directory-clear" :
                     "boundary.setfl-nofollow-clear",
            ret, observed == original, -1, 0, 0, 0);
        abi_require(SC1(57, alias) == 0 && SC1(57, fd) == 0);
    }
    abi_require(SC3(35, -100, "/boundary-flags", 0) == 0);
}

static void socket_import_ranges(void)
{
    long map = SC6(222, 0x10000000, ABI_PAGE_SIZE, 3, 0x32, -1, 0);
    abi_require(map == 0x10000000);
    long ret = SC6(207, -1, map, (usize)-1, 0, 0, 0);
    abi_record("boundary.recvfrom-huge-badfd", ret, -1, -1, 0, 0, 0);
    ret = SC6(206, -1, map, (usize)-1, 0, 0, 0);
    abi_record("boundary.sendto-huge-badfd", ret, -1, -1, 0, 0, 0);
    int pair[2];
    abi_require(SC4(199, 1, 2, 0, pair) == 0);
    abi_require(SC6(206, pair[0], "k", 1, 0, 0, 0) == 1);
    ret = SC6(207, pair[1], map, (usize)-1, 0x40, 0, 0);
    abi_record("boundary.recvfrom-huge-message", ret, -1, -1, 0,
        (void *)map, ret > 0 ? 1 : 0);
    /* Numeric rejection must not consume the next message. */
    abi_require(SC6(206, pair[0], "z", 1, 0, 0, 0) == 1);
    ret = SC6(207, pair[1], (void *)-1, (usize)-1, 0x40, 0, 0);
    abi_record("boundary.recvfrom-huge-high", ret, -1, -1, 0, 0, 0);
    ret = SC6(207, pair[1], map, 1, 0x40, 0, 0);
    abi_record("boundary.recvfrom-after-rejection", ret, -1, -1, 0,
        (void *)map, ret > 0 ? 1 : 0);
    abi_require(SC1(57, pair[0]) == 0 && SC1(57, pair[1]) == 0);
    abi_require(SC2(215, map, ABI_PAGE_SIZE) == 0);
}

void abi_boundary_cases(void)
{
    scalar_ranges();
    setfl_preserves_flags();
    socket_import_ranges();
}
