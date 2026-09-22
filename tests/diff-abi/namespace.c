#include "abi.h"

static void ns_record(const char *name, long result)
{
    abi_record(name, result, -1, -1, 0, 0, 0);
}

static void ns_open(const char *name, long dirfd, const char *path)
{
    long fd = SC4(56, dirfd, path, 0, 0);
    ns_record(name, fd < 0 ? fd : 0);
    if (fd >= 0) abi_require(SC1(57, fd) == 0);
}

static void ns_cwd(const char *name, usize size)
{
    char buffer[128];
    long result = SC2(17, buffer, size);
    abi_record(name, result, -1, -1, 0, buffer, result > 0 ? (usize)result : 0);
}

void abi_namespace_cases(void)
{
    abi_require(SC3(34, -100, "/ns-a", 0700) == 0);
    abi_require(SC3(34, -100, "/ns-b", 0700) == 0);
    long a = SC4(56, -100, "/ns-a", 00200000, 0);
    long b = SC4(56, -100, "/ns-b", 00200000, 0);
    abi_require(a >= 0 && b >= 0);
    ns_record("ns.mkdir-dirfd", SC3(34, a, "child", 0700));
    long child = SC4(56, a, "child", 00200000, 0);
    abi_require(child >= 0);
    ns_record("ns.fchdir", SC1(50, child));
    ns_cwd("ns.cwd", 128);
    ns_cwd("ns.cwd-short", 2);
    ns_record("ns.cwd-fault", SC2(17, 0, 128));
    ns_open("ns.bad-dirfd", -1, "data");
    ns_open("ns.absolute-bad-dirfd", -1, "/data");
    long data = abi_open("/data", 0);
    abi_require(data >= 0);
    ns_open("ns.nondir-dirfd", data, "data");
    ns_record("ns.fchdir-file", SC1(50, data));
    ns_record("ns.fchdir-bad", SC1(50, -1));
    ns_record("ns.chdir-file", SC1(49, "/data"));
    ns_record("ns.chdir-empty", SC1(49, ""));
    ns_record("ns.rename-parent", CALL(276, -100, "/ns-a", -100, "/ns-renamed", 0, 0));
    ns_cwd("ns.cwd-renamed-parent", 128);
    ns_record("ns.rename-cross-parent", CALL(276, a, "child", b, "moved", 0, 0));
    ns_cwd("ns.cwd-moved", 128);
    ns_record("ns.rename-cycle", CALL(276, -100, "/ns-b", child, "cycle", 0, 0));
    long file = SC4(56, child, "temp", 0100 | 2, 0600);
    long target = SC4(56, b, "target", 0100 | 2, 0600);
    abi_require(file >= 0 && target >= 0);
    abi_require(SC3(64, file, "new", 3) == 3 && SC3(64, target, "old", 3) == 3);
    ns_record("ns.rename-noreplace", CALL(276, child, "temp", b, "target", 1, 0));
    ns_record("ns.rename-file-trailing", CALL(276, child, "temp", b, "absent/", 0, 0));
    ns_record("ns.rename-dot", CALL(276, child, ".", b, "other", 0, 0));
    ns_record("ns.rename-noreplace-dot", CALL(276, child, "temp", b, ".", 1, 0));
    ns_record("ns.rename-missing-dot", CALL(276, child, "missing/.", b, "other", 0, 0));
    ns_record("ns.rename-file-dotdot", CALL(276, child, "temp/..", b, "other", 0, 0));
    ns_record("ns.rename-overwrite", CALL(276, child, "temp", b, "target", 0, 0));
    char bytes[3];
    long count = SC4(67, target, bytes, sizeof(bytes), 0);
    abi_record("ns.overwrite-old-open", count, -1, -1, 0, bytes, count > 0 ? (usize)count : 0);
    struct abi_stat stat;
    long ret = SC2(80, target, &stat);
    abi_record("ns.overwrite-old-nlink", ret, ret ? -1L : (long)stat.nlink, -1, 0, 0, 0);
    ns_record("ns.fsync-file", SC1(82, file));
    ns_record("ns.fsync-old-parent", SC1(82, child));
    ns_record("ns.fsync-new-parent", SC1(82, b));
    ns_record("ns.rmdir-cwd", SC3(35, b, "moved", 0x200));
    ns_cwd("ns.cwd-deleted", 128);
    ns_open("ns.open-deleted-dot", child, ".");
    ns_record("ns.create-deleted", SC4(56, child, "missing", 0100 | 2, 0600));
    ns_record("ns.chdir-deleted-parent", SC1(49, ".."));
    ns_cwd("ns.cwd-after-dotdot", 128);
    ns_record("ns.fchdir-deleted", SC1(50, child));
    ret = SC4(79, -100, "", &stat, 0x1000);
    abi_record("ns.stat-empty-cwd", ret, ret ? -1L : (long)stat.nlink, -1, 0, 0, 0);
    abi_require(SC1(49, "/") == 0);
    abi_require(SC1(57, child) == 0 && SC1(57, file) == 0 && SC1(57, target) == 0);
    abi_require(SC1(57, data) == 0 && SC1(57, a) == 0 && SC1(57, b) == 0);
    abi_require(SC3(35, -100, "/ns-b/target", 0) == 0);
    abi_require(SC3(35, -100, "/ns-renamed", 0x200) == 0);
    abi_require(SC3(35, -100, "/ns-b", 0x200) == 0);
}
