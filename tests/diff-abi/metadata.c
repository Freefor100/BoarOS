#include "abi.h"

#define NOW 1073741823L
#define OMIT 1073741822L
struct fields { long a, an, m, mn, c, cn; };
static struct {
    long low[2], high[2], requested[4];
    struct fields before, after, parent_before, parent_after;
} observation;
struct statfs_fields {
    long type, bsize, blocks, bfree, bavail, files, ffree;
    int fsid[2];
    long namelen, frsize, flags, spare[4];
};
static long parent;

static struct fields snapshot(long fd)
{
    struct abi_stat st;
    abi_require((fd < 0 ? SC4(79, parent, "metadata-link", &st, 0x100) :
                         SC2(80, fd, &st)) == 0);
    return (struct fields){st.atime, st.atime_nsec, st.mtime,
                            st.mtime_nsec, st.ctime, st.ctime_nsec};
}

static void set(const char *id, long fd, const long *times,
                long dirfd, const char *path, long flags)
{
    long delay[2] = {0, 50000000};
    for (int i = 0; i < 4; ++i)
        observation.requested[i] = times ? times[i] : (i & 1 ? NOW : 0);
    observation.before = snapshot(fd);
    observation.parent_before = snapshot(parent);
    abi_require(SC2(113, 0, observation.low) == 0);
    abi_require(SC2(101, delay, 0) == 0);
    long result = SC4(88, dirfd, path, times, flags);
    abi_require(SC2(113, 0, observation.high) == 0);
    observation.after = snapshot(fd);
    observation.parent_after = snapshot(parent);
    abi_record(id, result, -1, -1, 0, &observation, sizeof(observation));
}

static void result(const char *id, long value)
{
    abi_record(id, value, -1, -1, 0, 0, 0);
}

static void stats(const char *id, long fd)
{
    struct statfs_fields st;
    long ret = SC2(44, fd, &st);
    abi_require(ret == 0);
    /* Compare geometry and identity exactly. Free counters are verified by
     * allocation deltas below; Linux additionally reserves an extent pool. */
    long values[9] = {st.type, st.bsize, st.blocks, st.files,
        st.fsid[0], st.fsid[1], st.namelen, st.frsize, st.flags};
    abi_record(id, ret, -1, -1, 0, values, sizeof(values));
    abi_require(st.bfree >= st.bavail && st.bavail >= 0 &&
                st.bfree <= st.blocks && st.ffree <= st.files);
}

void abi_metadata_cases(void)
{
    parent = abi_open("/", 65536);
    long fd = abi_open("/metadata", 2 | 64 | 128);
    abi_require(parent >= 0 && fd >= 0);
    long explicit[4] = {-123, 123456789, 4102444800L, 987654321};
    long now_omit[4] = {0, NOW, -999, OMIT};
    long omit[4] = {-1, OMIT, -1, OMIT};
    long invalid[4] = {0, 1000000000L, 0, 0};
    long extreme[4] = {(-9223372036854775807L - 1), 33, 9223372036854775807L, 44};
    set("utime.explicit", fd, explicit, parent, "metadata", 0);
    set("utime.now-omit", fd, now_omit, fd, 0, 0);
    set("utime.both-omit", fd, omit, -1, (void *)-1L, -1);
    set("utime.null-times", fd, 0, fd, 0, 0);
    set("utime.invalid-nsec", fd, invalid, fd, 0, 0);
    set("utime.empty-fd", fd, explicit, fd, "", 0x1000);
    set("utime.extrema", fd, extreme, fd, 0, 0);
    abi_require(SC3(36, "/metadata", parent, "metadata-link") == 0);
    set("utime.nofollow", -1, explicit, parent, "metadata-link", 0x100);
    result("metadata.bad-fd", SC4(88, -1, 0, invalid, 0));
    result("metadata.missing-invalid", SC4(88, -100, "/missing-metadata", invalid, 0));
    result("metadata.null-cwd", SC4(88, -100, 0, explicit, 0x1000));
    result("metadata.null-flags", SC4(88, fd, 0, explicit, 0x1000));
    result("metadata.empty-no-flag", SC4(88, fd, "", explicit, 0));
    result("metadata.bad-flags", SC4(88, -1, (void *)-1L, explicit, 2));
    long map = CALL(222, 0, 8192, 3, 0x22, -1, 0);
    abi_require(map >= 0 && SC3(226, map + 4096, 4096, 0) == 0);
    ((long *)(map + 4080))[0] = 0;
    ((long *)(map + 4080))[1] = OMIT;
    result("metadata.times-partial-fault", SC4(88, -1, 0, map + 4080, 0));
    stats("metadata.statfs-geometry", fd);
    result("metadata.statfs-bad-path", SC2(43, "/missing-metadata", -1));
    result("metadata.statfs-bad-output", SC2(43, "/metadata", -1));
    result("metadata.fstatfs-bad-fd", SC2(44, -1, -1));
    result("metadata.fstatfs-bad-output", SC2(44, fd, -1));
    abi_require(SC1(82, fd) == 0);
    struct statfs_fields initial, allocated, truncated;
    abi_require(SC2(44, fd, &initial) == 0);
    abi_require(SC3(64, fd, "x", 1) == 1 && SC1(82, fd) == 0);
    abi_require(SC2(44, fd, &allocated) == 0);
    abi_require(SC2(46, fd, 0) == 0 && SC1(82, fd) == 0);
    abi_require(SC2(44, fd, &truncated) == 0);
    long deltas[3] = {initial.bfree - allocated.bfree,
        truncated.bfree - allocated.bfree, initial.ffree - truncated.ffree};
    abi_record("metadata.statfs-allocation", 0, -1, -1, 0, deltas, sizeof(deltas));
    abi_require(SC3(35, parent, "metadata", 0) == 0);
    set("utime.unlinked-fd", fd, explicit, fd, 0, 0);
    stats("metadata.statfs-unlinked", fd);
    abi_require(SC3(35, parent, "metadata-link", 0) == 0);
    abi_require(SC1(57, fd) == 0 && SC1(57, parent) == 0);
    abi_require(SC2(215, map, 8192) == 0);
}
