#include "abi.h"

static void close_link_fd(long fd)
{
    if (fd >= 0) abi_require(SC1(57, fd) == 0);
}

void abi_link_cases(void)
{
    long fd = abi_open("/data", 00400000);
    abi_record("link.nofollow-regular", fd < 0 ? fd : 0,
               -1, -1, 0, 0, 0);
    close_link_fd(fd);

    long ret = SC3(36, "/data", -100, "/link");
    abi_record("link.create", ret, -1, -1, 0, 0, 0);
    if (ret) return;

    char buffer[16] = {0};
    ret = SC4(78, -100, "/link", buffer, sizeof(buffer));
    abi_record("link.readlink", ret, -1, -1, 0,
               buffer, ret > 0 ? (usize)ret : 0);
    ret = SC4(78, -100, "/link", buffer, 3);
    abi_record("link.readlink-short", ret, -1, -1, 0,
               buffer, ret > 0 ? (usize)ret : 0);
    ret = SC4(78, -100, "/link", buffer, 0);
    abi_record("link.readlink-zero", ret, -1, -1, 0, 0, 0);

    fd = abi_open("/link", 0);
    if (fd >= 0) ret = SC3(63, fd, buffer, 8);
    else ret = fd;
    abi_record("link.follow", ret, -1, -1, 0,
               buffer, ret > 0 ? (usize)ret : 0);
    close_link_fd(fd);
    fd = abi_open("/link", 00400000);
    abi_record("link.nofollow-link", fd < 0 ? fd : 0, -1, -1, 0, 0, 0);
    close_link_fd(fd);
    fd = abi_open("/link", 00100 | 00200);
    abi_record("link.excl", fd < 0 ? fd : 0, -1, -1, 0, 0, 0);
    close_link_fd(fd);
    fd = abi_open("/link/", 0);
    abi_record("link.trailing", fd < 0 ? fd : 0, -1, -1, 0, 0, 0);
    close_link_fd(fd);

    struct abi_stat stat;
    ret = SC4(79, -100, "/link", &stat, 0x100);
    abi_record("link.lstat", ret, ret == 0 ? stat.size : -1,
               ret == 0 ? (long)stat.mode : -1, 0, 0, 0);
    ret = SC4(79, -100, "/link", &stat, 0);
    abi_record("link.stat", ret, ret == 0 ? stat.size : -1,
               ret == 0 ? (long)stat.mode : -1, 0, 0, 0);

    abi_require(SC3(34, -100, "/dir", 0755) == 0);
    abi_require(SC3(36, "../data", -100, "/dir/relative") == 0);
    abi_require(SC3(36, "/dir", -100, "/dir-alias") == 0);
    fd = abi_open("/dir/relative", 0);
    if (fd >= 0) ret = SC3(63, fd, buffer, 4);
    else ret = fd;
    abi_record("link.relative-parent", ret, -1, -1, 0,
               buffer, ret > 0 ? (usize)ret : 0);
    close_link_fd(fd);
    fd = abi_open("/dir-alias/relative", 0);
    if (fd >= 0) ret = SC3(63, fd, buffer, 4);
    else ret = fd;
    abi_record("link.intermediate", ret, -1, -1, 0,
               buffer, ret > 0 ? (usize)ret : 0);
    close_link_fd(fd);

    abi_require(SC3(36, "/loop-b", -100, "/loop-a") == 0);
    abi_require(SC3(36, "/loop-a", -100, "/loop-b") == 0);
    fd = abi_open("/loop-a", 0);
    abi_record("link.loop", fd < 0 ? fd : 0, -1, -1, 0, 0, 0);
    close_link_fd(fd);

    abi_require(SC3(36, "/created-through-link", -100, "/dangling") == 0);
    fd = abi_open("/dangling", 00100 | 2);
    abi_record("link.create-through-dangling", fd < 0 ? fd : 0,
               -1, -1, 0, 0, 0);
    close_link_fd(fd);
    fd = abi_open("/created-through-link", 0);
    abi_record("link.dangling-target", fd < 0 ? fd : 0,
               -1, -1, 0, 0, 0);
    close_link_fd(fd);

    ret = SC3(35, -100, "/link", 0);
    abi_record("link.unlink", ret, -1, -1, 0, 0, 0);
    fd = abi_open("/data", 0);
    abi_record("link.target-survives", fd < 0 ? fd : 0,
               -1, -1, 0, 0, 0);
    close_link_fd(fd);

    abi_require(SC3(34, -100, "/parent", 0755) == 0);
    abi_require(SC3(36, "/parent", -100, "/parent-link") == 0);
    ret = SC3(36, "../data", -100, "/parent-link/child");
    abi_record("link.create-via-parent", ret, -1, -1, 0, 0, 0);
    ret = SC4(78, -100, "/parent/child", buffer, sizeof(buffer));
    abi_record("link.read-via-parent", ret, -1, -1, 0,
               buffer, ret > 0 ? (usize)ret : 0);
    ret = SC3(35, -100, "/parent-link/child", 0);
    abi_record("link.unlink-via-parent", ret, -1, -1, 0, 0, 0);
    ret = SC4(79, -100, "/parent/child", &stat, 0x100);
    abi_record("link.parent-child-removed", ret, -1, -1, 0, 0, 0);
    ret = SC4(78, -100, "/data", buffer, sizeof(buffer));
    abi_record("link.readlink-regular", ret, -1, -1, 0, 0, 0);
}
