#include "abi.h"

static long mode_of(long descriptor)
{
    struct abi_stat stat;
    abi_require(SC2(80, descriptor, &stat) == 0);
    return stat.mode & 07777;
}

static int ctime_changed(const struct abi_stat *old, const struct abi_stat *now)
{
    return now->ctime != old->ctime ||
           now->ctime_nsec != old->ctime_nsec;
}

void abi_mode_cases(void)
{
    long previous = SC1(166, 0022);
    abi_record("mode.umask-initial", previous, -1, -1, 0, 0, 0);
    abi_record("mode.umask-set", SC1(166, 0077), -1, -1, 0, 0, 0);
    long created = SC4(56, -100, "/mode-create", 0100 | 0200 | 1, 0777);
    abi_record("mode.create-under-umask", created < 0 ? created : 0,
               created >= 0 ? mode_of(created) : -1, -1, 0, 0, 0);
    if (created >= 0) abi_require(SC1(57, created) == 0);
    long made = SC3(34, -100, "/mode-dir", 0777);
    long directory = made == 0 ? abi_open("/mode-dir", 0 | 0200000) : made;
    abi_record("mode.mkdir-under-umask", made,
               directory >= 0 ? mode_of(directory) : -1, -1, 0, 0, 0);
    if (directory >= 0) abi_require(SC1(57, directory) == 0);
    abi_record("mode.umask-restore", SC1(166, 0022), -1, -1, 0, 0, 0);

    char *const arguments[] = { "/data", 0 };
    char *const environment[] = { 0 };
    abi_record("mode.exec-denied", SC3(221, "/data", arguments, environment),
               -1, -1, 0, 0, 0);
    long fd = abi_open("/data", 0);
    abi_require(fd >= 0);
    struct abi_stat before, after;
    abi_require(SC2(80, fd, &before) == 0);
    long delay[2] = { 0, 20000000 };
    abi_require(SC2(101, delay, 0) == 0);
    long result = SC3(53, -100, "/data", 0755);
    abi_require(SC2(80, fd, &after) == 0);
    abi_record("mode.fchmodat", result, after.mode & 07777,
               ctime_changed(&before, &after), 0, 0, 0);
    abi_record("mode.exec-after-chmod", SC3(221, "/data", arguments, environment),
               -1, -1, 0, 0, 0);

    result = SC2(52, fd, 0644);
    abi_record("mode.fchmod", result, mode_of(fd), -1, 0, 0, 0);
    abi_record("mode.exec-after-restore", SC3(221, "/data", arguments, environment),
               -1, -1, 0, 0, 0);
    abi_record("mode.fchmod-bad-fd", SC2(52, -9, 0755), -1, -1, 0, 0, 0);
    abi_record("mode.fchmodat-missing", SC3(53, -100, "/no-mode-file", 0755),
               -1, -1, 0, 0, 0);
    abi_record("mode.fchmodat-bad-pointer", SC3(53, -100, 1, 0755),
               -1, -1, 0, 0, 0);
    abi_record("mode.fchmodat-bad-dirfd", SC3(53, -9, "data", 0755),
               -1, -1, 0, 0, 0);
    result = SC3(53, -9, "/data", 0100755);
    abi_record("mode.fchmodat-high-bits", result, mode_of(fd), -1, 0, 0, 0);
    result = SC2(52, fd, 0644);
    abi_record("mode.fchmod-reset", result, mode_of(fd), -1, 0, 0, 0);
    abi_require(SC1(57, fd) == 0);

    long unlinked = SC4(56, -100, "/mode-unlinked", 0100 | 0200 | 2, 0600);
    abi_require(unlinked >= 0);
    abi_require(SC3(35, -100, "/mode-unlinked", 0) == 0);
    result = SC2(52, unlinked, 0755);
    abi_record("mode.fchmod-unlinked", result, mode_of(unlinked), -1, 0, 0, 0);
    abi_require(SC1(57, unlinked) == 0);

    long child = CALL(220, 17, 0, 0, 0, 0, 0);
    abi_require(child >= 0);
    if (!child) {
        long old = SC1(166, 0077);
        abi_exit(old == 0022 ? 0 : 3);
    }
    int status = 0;
    abi_require(SC4(260, child, &status, 0, 0) == child);
    abi_record("mode.umask-fork", SC1(166, 0022), -1, -1, status, 0, 0);

    child = CALL(220, 0x200 | 17, 0, 0, 0, 0, 0);
    abi_require(child >= 0);
    if (!child) {
        long old = SC1(166, 0033);
        abi_exit(old == 0022 ? 0 : 3);
    }
    status = 0;
    abi_require(SC4(260, child, &status, 0, 0) == child);
    abi_record("mode.umask-shared-fs", SC1(166, previous), -1, -1,
               status, 0, 0);
}
