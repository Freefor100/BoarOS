#include "abi.h"

/* Raw observations, not normalized timestamps. The comparator receives
 * operation clock bounds plus file and parent timestamps before/after. */
struct timestamp_fields { long atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns; };
struct timestamp_observation {
    long low[2], high[2];
    struct timestamp_fields before, after, parent_before, parent_after;
};
static struct timestamp_observation observation;
static long parent_fd;

static struct timestamp_fields snapshot(long fd)
{
    struct abi_stat st;
    if (fd < 0) return (struct timestamp_fields){0};
    abi_require(SC2(80, fd, &st) == 0);
    return (struct timestamp_fields){st.atime, st.atime_nsec,
        st.mtime, st.mtime_nsec, st.ctime, st.ctime_nsec};
}

static void before(long fd)
{
    long delay[2] = {0, 50000000};
    observation.before = snapshot(fd);
    observation.parent_before = snapshot(parent_fd);
    abi_require(SC2(113, 0, observation.low) == 0);
    /* Linux current_time may use a coarse clock. Include the preceding
     * delay in the recorded operation interval instead of dropping time. */
    abi_require(SC2(101, delay, 0) == 0);
}

static void after(const char *name, long fd, long result)
{
    abi_require(SC2(113, 0, observation.high) == 0);
    observation.after = snapshot(fd);
    observation.parent_after = snapshot(parent_fd);
    abi_record(name, result, abi_size(fd), abi_offset(fd), 0,
               &observation, sizeof(observation));
}

void abi_timestamp_cases(void)
{
    long fd, read_only, result;
    char byte;
    long map = CALL(222, 0, 8192, 3, 0x22, -1, 0);
    abi_require(map >= 0 && SC3(226, map + 4096, 4096, 0) == 0);
    *(char *)(map + 4095) = 'P';
    parent_fd = abi_open("/", 65536);
    abi_require(parent_fd >= 0);
    before(-1);
    fd = abi_open("/timestamp-file", 2 | 64 | 128);
    abi_require(fd >= 0);
    after("time.create", fd, 0);
    read_only = abi_open("/timestamp-file", 0);
    abi_require(read_only >= 0);

    before(fd); result = SC3(64, fd, "", 0);
    after("time.write-zero", fd, result);
    before(fd); result = SC3(64, read_only, "x", 1);
    after("time.write-denied", fd, result);
    before(fd); result = SC3(64, fd, "A", 1);
    after("time.write", fd, result);
    abi_require(SC3(62, fd, 0, 0) == 0);
    before(fd); result = SC3(63, fd, &byte, 1);
    after("time.read", fd, result);
    before(fd); result = SC3(64, fd, map + 4096, 1);
    after("time.write-fault", fd, result);
    before(fd); result = SC4(67, fd, &byte, 1, 0);
    after("time.cached-pread", fd, result);
    before(fd); result = SC3(63, fd, map + 4096, 0);
    after("time.read-zero", fd, result);

    abi_require(SC3(64, fd, map + 4096, 1) == -14);
    before(fd); result = SC4(67, fd, &byte, 1, 1);
    after("time.pread-eof", fd, result);
    abi_require(SC3(64, fd, map + 4096, 1) == -14);
    abi_require(SC3(62, fd, 0, 0) == 0);
    before(fd); result = SC3(63, fd, map + 4096, 1);
    after("time.read-fault", fd, result);
    before(fd); result = SC3(64, fd, map + 4095, 2);
    after("time.write-partial", fd, result);
    before(fd); result = SC2(46, fd, 1);
    after("time.truncate-same", fd, result);
    before(fd); result = SC2(46, fd, 4096);
    after("time.truncate-grow", fd, result);
    before(fd); result = SC2(46, fd, 1);
    after("time.truncate-shrink", fd, result);
    before(fd); result = SC3(35, -100, "/timestamp-file", 0);
    after("time.unlink", fd, result);
    before(fd); result = SC3(64, fd, "U", 1);
    after("time.unlinked-write", fd, result);
    abi_require(SC3(62, fd, 0, 0) == 0);
    before(fd); result = SC3(63, fd, &byte, 1);
    after("time.unlinked-read", fd, result);
    struct abi_stat maximum_stat;
    abi_require(SC2(80, fd, &maximum_stat) == 0);
    long maxbytes = ((1UL << 32) - 1) * (unsigned long)maximum_stat.blksize;
    abi_require(SC3(62, fd, maxbytes, 0) == maxbytes);
    before(fd); result = SC3(64, fd, "X", 1);
    after("time.write-maxbytes", fd, result);
    abi_require(SC3(62, fd, 0, 0) == 0);
    abi_require(SC3(64, fd, map + 4096, 1) == -14);
    before(fd); result = SC4(67, fd, -4096L, 1, 0);
    after("time.pread-invalid", fd, result);
    before(fd); result = SC4(67, fd, -4096L, 0, 0);
    after("time.pread-invalid-zero", fd, result);
    before(fd); result = SC4(67, fd, -4096L, 1, abi_size(fd));
    after("time.pread-invalid-eof", fd, result);
    before(fd); result = SC4(67, fd, map, -1L, 0);
    after("time.pread-wrap", fd, result);
    abi_require(SC1(57, read_only) == 0 && SC1(57, fd) == 0 &&
                 SC1(57, parent_fd) == 0 && SC2(215, map, 8192) == 0);
}
