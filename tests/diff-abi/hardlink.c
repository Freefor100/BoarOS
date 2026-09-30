#include "abi.h"

static void result(const char *name, long value)
{ abi_record(name, value, -1, -1, 0, 0, 0); }
static long link_at(long oldfd, const char *old, long newfd, const char *new, long flags)
{ return SC5(37, oldfd, old, newfd, new, flags); }
static void remove_at(long fd, const char *name)
{ abi_require(SC3(35, fd, name, 0) == 0); }
static void stat_pair(const char *id, long first, long second)
{
    struct abi_stat a, b;
    abi_require(SC2(80, first, &a) == 0 && SC2(80, second, &b) == 0);
    abi_record(id, a.dev == b.dev && a.ino == b.ino, a.nlink, b.nlink, 0, 0, 0);
}
void abi_hardlink_cases(void)
{
    long first = abi_open("/hard-source", 0102);
    abi_require(first >= 0 && SC3(64, first, "seed", 4) == 4);
    long r = link_at(-100, "/hard-source", -100, "/hard-alias", 0);
    result("hard.create", r);
    if (r) { abi_require(SC1(57, first) == 0); return; }
    long second = abi_open("/hard-alias", 2); abi_require(second >= 0);
    stat_pair("hard.identity", first, second);
    abi_require(SC4(68, second, "X", 1, 1) == 1);
    char bytes[16] = {0}; r = SC4(67, first, bytes, 4, 0);
    abi_record("hard.shared-data", r, -1, -1, 0, bytes, r > 0 ? (usize)r : 0);
    long mapping = SC6(222, 0, 4096, 3, 1, first, 0);
    abi_require((unsigned long)mapping < (unsigned long)-4095);
    ((volatile char *)mapping)[2] = 'M';
    r = SC4(67, second, bytes, 4, 0);
    abi_record("hard.shared-mapping", r, -1, -1, 0, bytes, r > 0 ? (usize)r : 0);
    abi_require(SC2(215, mapping, 4096) == 0);
    struct { short type, whence; long start, length; int pid; } lock = {1, 0, 0, 4, 0};
    abi_require(SC3(25, first, 37, &lock) == 0);
    result("hard.ofd-lock-conflict", SC3(25, second, 37, &lock));
    lock.type = 2; abi_require(SC3(25, first, 37, &lock) == 0);
    result("hard.exists", link_at(-100, "/hard-source", -100, "/hard-alias", 0));
    result("hard.flags", link_at(-100, "/hard-source", -100, "/hard-flags", 0x100));
    abi_require(SC3(34, -100, "/hard-dir-source", 0755) == 0);
    result("hard.directory", link_at(-100, "/hard-dir-source", -100, "/hard-dir", 0));
    abi_require(SC3(35, -100, "/hard-dir-source", 0x200) == 0);
    result("hard.missing", link_at(-100, "/hard-missing", -100, "/hard-alias", 0));
    result("hard.empty-no-flag", link_at(first, "", -100, "/hard-empty", 0));
    result("hard.empty-bad-fd", link_at(-1, "", -100, "/hard-empty", 0x1000));
    result("hard.empty-cwd", link_at(-100, "", -100, "/hard-empty", 0x1000));
    result("hard.empty-target", link_at(first, "", -100, "", 0x1000));
    result("hard.source-fault", link_at(-100, (const char *)1, -100, "/hard-empty", 0));
    result("hard.target-fault", link_at(-100, "/hard-source", -100, (const char *)1, 0));
    int pipes[2]; abi_require(SC2(59, pipes, 0) == 0);
    result("hard.pipe-fd", link_at(pipes[0], "", -100, "/hard-pipe", 0x1000));
    result("hard.pipe-existing", link_at(pipes[0], "", -100, "/hard-alias", 0x1000));
    abi_require(SC1(57, pipes[0]) == 0 && SC1(57, pipes[1]) == 0);
    result("hard.relative-old-fd", link_at(-1, "hard-source", -100, "/hard-empty", 0));
    result("hard.relative-new-fd", link_at(-100, "/hard-source", -1, "hard-empty", 0));
    result("hard.source-trailing", link_at(-100, "/hard-source/", -100, "/hard-empty", 0));
    result("hard.target-trailing-missing", link_at(-100, "/hard-source", -100, "/hard-empty/", 0));
    result("hard.target-trailing-existing", link_at(-100, "/hard-source", -100, "/hard-alias/", 0));
    result("hard.target-dot", link_at(-100, "/hard-source", -100, "/dev/.", 0));
    result("hard.absolute-fds", link_at(-1, "/hard-source", -1, "/hard-absolute", 0));
    remove_at(-100, "/hard-absolute");
    abi_require(SC3(36, "/hard-source", -100, "/hard-symlink") == 0);
    result("hard.symlink-self", link_at(-100, "/hard-symlink", -100, "/hard-symlink-alias", 0));
    struct abi_stat a, b;
    abi_require(SC4(79, -100, "/hard-symlink", &a, 0x100) == 0);
    abi_require(SC4(79, -100, "/hard-symlink-alias", &b, 0x100) == 0);
    abi_record("hard.symlink-identity", a.ino == b.ino, a.nlink, b.mode & 0170000, 0, 0, 0);
    result("hard.symlink-follow", link_at(-100, "/hard-symlink", -100, "/hard-follow", 0x400));
    long followed = abi_open("/hard-follow", 0); abi_require(followed >= 0);
    stat_pair("hard.follow-identity", first, followed); abi_require(SC1(57, followed) == 0);
    remove_at(-100, "/hard-follow");
    abi_require(SC3(36, "/hard-absent", -100, "/hard-dangling") == 0);
    result("hard.dangling-source", link_at(-100, "/hard-dangling", -100, "/hard-dangling-alias", 0));
    result("hard.dangling-follow", link_at(-100, "/hard-dangling", -100, "/hard-dangling-follow", 0x400));
    result("hard.dangling-target", link_at(-100, "/hard-source", -100, "/hard-dangling", 0));
    remove_at(-100, "/hard-dangling-alias"); remove_at(-100, "/hard-dangling");
    abi_require(SC5(276, -100, "/hard-source", -100, "/hard-renamed", 0) == 0);
    result("hard.fd-renamed", link_at(first, "", -100, "/hard-fd", 0x1000));
    remove_at(-100, "/hard-renamed");
    result("hard.fd-detached-alias", link_at(first, "", -100, "/hard-fd-detached", 0x1400));
    remove_at(-100, "/hard-fd-detached"); remove_at(-100, "/hard-fd");
    stat_pair("hard.remaining-alias", first, second);
    remove_at(-100, "/hard-alias");
    result("hard.fd-zero-link", link_at(first, "", -100, "/hard-resurrect", 0x1000));
    abi_require(SC2(80, first, &a) == 0);
    abi_record("hard.zero-links", 0, a.nlink, a.size, 0, 0, 0);
    abi_require(SC1(57, first) == 0 && SC1(57, second) == 0);
    remove_at(-100, "/hard-symlink"); remove_at(-100, "/hard-symlink-alias");
    abi_require(SC3(34, -100, "/hard-mount", 0755) == 0);
    abi_require(SC5(40, "none", "/hard-mount", "tmpfs", 0, 0) == 0);
    long dir = abi_open("/hard-mount", 0200000); abi_require(dir >= 0);
    first = SC4(56, dir, "source", 0102, 0600); abi_require(first >= 0);
    result("hard.tmpfs-create", link_at(dir, "source", dir, "alias", 0));
    second = SC4(56, dir, "alias", 0, 0); abi_require(second >= 0);
    stat_pair("hard.tmpfs-identity", first, second);
    result("hard.cross-mount", link_at(dir, "source", -100, "/hard-cross", 0));
    remove_at(dir, "source");
    result("hard.tmpfs-fd-detached", link_at(first, "", dir, "fd", 0x1000));
    remove_at(dir, "alias"); remove_at(dir, "fd");
    result("hard.tmpfs-fd-zero", link_at(first, "", dir, "resurrect", 0x1000));
    abi_require(SC1(57, first) == 0 && SC1(57, second) == 0 && SC1(57, dir) == 0);
    abi_require(SC2(39, "/hard-mount", 0) == 0);
    abi_require(SC3(35, -100, "/hard-mount", 0x200) == 0);
}
