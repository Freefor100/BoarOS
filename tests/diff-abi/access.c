#include "abi.h"

static void access_mount_cases(void);

void abi_access_cases(void)
{
    long directory = abi_open("/", 0 | 0200000);
    abi_require(directory >= 0);
    abi_record("access.root-write", SC3(48, -100, "/", 2),
               -1, -1, 0, 0, 0);
    abi_record("access.file-read", SC3(48, -100, "/data", 4),
               -1, -1, 0, 0, 0);
    abi_record("access.file-exec", SC3(48, -100, "/data", 1),
               -1, -1, 0, 0, 0);
    abi_record("access.relative-write", SC3(48, directory, "data", 2),
               -1, -1, 0, 0, 0);
    abi_record("access.absolute-bad-dirfd", SC3(48, -9, "/data", 0),
               -1, -1, 0, 0, 0);
    abi_record("access.missing", SC3(48, -100, "/no-such-access-file", 0),
               -1, -1, 0, 0, 0);
    abi_record("access.bad-mode", SC3(48, -100, "/data", 8),
               -1, -1, 0, 0, 0);
    abi_record("access.bad-pointer", SC3(48, -100, 1, 0),
               -1, -1, 0, 0, 0);
    long source = SC4(56, -100, "/data", 0400, 0);
    abi_record("open.noctty-regular", source < 0 ? source : 0,
               -1, -1, 0, 0, 0);
    if (source >= 0) abi_require(SC1(57, source) == 0);
    long device = SC4(56, -100, "/dev/null", 0400, 0);
    abi_record("open.noctty-char", device < 0 ? device : 0,
               -1, -1, 0, 0, 0);
    if (device >= 0) abi_require(SC1(57, device) == 0);
    abi_record("open.noctty-bad-combination",
               SC4(56, -100, "/data", 0400 | 0100 | 0200000, 0600),
               -1, -1, 0, 0, 0);
    abi_require(SC1(57, directory) == 0);
    access_mount_cases();
}

static void access_record(const char *name, long dirfd, const char *path, long mode)
{
    abi_record(name, SC3(48, dirfd, path, mode), -1, -1, 0, 0, 0);
}

static void access_mount_cases(void)
{
    /* Fixed Linux fs/open.c::do_faccessat and fs/namei.c::generic_permission:
     * permissions precede readonly rejection; root still needs one executable
     * bit on every nondirectory. The final target determines the mount. */
    abi_require(SC3(34, -100, "/access-proc", 0755) == 0);
    abi_require(SC5(40, "proc", "/access-proc", "proc", 1, 0) == 0);
    long directory = abi_open("/access-proc", 0200000);
    abi_require(directory >= 0);
    abi_require(SC3(36, "/data", -100, "/access-rw-link") == 0);
    access_record("access.rw-to-readonly-dir", -100, "/access-proc", 2);
    access_record("access.rw-to-readonly-file", -100, "/access-proc/self/status", 2);
    access_record("access.readonly-dirfd-to-rw", directory, "../data", 2);
    access_record("access.readonly-dirfd-to-rw-symlink", directory, "../access-rw-link", 2);
    access_record("access.permission-before-readonly", -100, "/access-proc/self/status", 3);
    abi_require(SC4(33, -100, "/access-fifo", 0010600, 0) == 0);
    access_record("access.fifo-exec", -100, "/access-fifo", 1);
    access_record("access.fifo-read-write", -100, "/access-fifo", 6);
    int pipe[2];
    abi_require(SC2(59, pipe, 0) == 0);
    static char path[64] = "/access-proc/self/fd/";
    usize length = sizeof("/access-proc/self/fd/") - 1;
    char digits[24];
    unsigned count = 0;
    unsigned long fd = pipe[0];
    do { digits[count++] = '0' + fd % 10; fd /= 10; } while (fd);
    while (count) path[length++] = digits[--count];
    path[length] = 0;
    access_record("access.proc-pipe-exec", -100, path, 1);
    access_record("access.proc-pipe-read-write", -100, path, 6);
    abi_require(SC1(57, pipe[0]) == 0 && SC1(57, pipe[1]) == 0);
    abi_require(SC3(35, -100, "/access-fifo", 0) == 0);
    abi_require(SC3(35, -100, "/access-rw-link", 0) == 0);
    abi_require(SC1(57, directory) == 0 && SC2(39, "/access-proc", 0) == 0);
    abi_require(SC3(35, -100, "/access-proc", 0x200) == 0);
}
