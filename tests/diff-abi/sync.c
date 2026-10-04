#include "abi.h"

void abi_sync_cases(void)
{
    char data[8] = {0};
    long fd = abi_open("/sync-probe", 0102);
    abi_require(fd >= 0);
    abi_require(SC3(64, fd, "sync", 4) == 4);
    abi_record("sync.fsync", SC1(82, fd), abi_size(fd), abi_offset(fd), 0, 0, 0);
    abi_record("sync.fdatasync", SC1(83, fd), -1, -1, 0, 0, 0);
    abi_record("sync.filesystem", SC1(267, fd), abi_size(fd), abi_offset(fd), 0, 0, 0);
    long duplicate = SC1(23, fd);
    abi_require(duplicate >= 0);
    abi_record("sync.filesystem-dup", SC1(267, duplicate), -1, -1, 0, 0, 0);
    abi_require(SC1(57, duplicate) == 0);
    long path = abi_open("/sync-probe", 010000000);
    abi_require(path >= 0);
    abi_record("sync.filesystem-path", SC1(267, path), -1, -1, 0, 0, 0);
    abi_require(SC1(57, path) == 0);
    abi_record("sync.global", SC0(81), -1, -1, 0, 0, 0);
    long other = abi_open("/sync-probe", 0);
    abi_require(other >= 0);
    long count = SC3(63, other, data, sizeof(data));
    abi_record("sync.readonly", SC1(82, other), abi_size(other),
                abi_offset(other), 0, data, count > 0 ? (usize)count : 0);
    abi_require(SC1(57, other) == 0 && SC1(57, fd) == 0);
    long directory = abi_open("/", 0200000);
    abi_require(directory >= 0);
    abi_record("sync.directory", SC1(82, directory), -1, -1, 0, 0, 0);
    abi_record("sync.filesystem-directory", SC1(267, directory), -1, -1, 0, 0, 0);
    abi_require(SC1(57, directory) == 0);
    abi_record("sync.bad-fd", SC1(82, -1), -1, -1, 0, 0, 0);
    abi_record("sync.filesystem-bad-fd", SC1(267, -1), -1, -1, 0, 0, 0);
    int pipefd[2];
    abi_require(SC2(59, pipefd, 0) == 0);
    abi_record("sync.pipe", SC1(83, pipefd[1]), -1, -1, 0, 0, 0);
    abi_record("sync.filesystem-pipe", SC1(267, pipefd[1]), -1, -1, 0, 0, 0);
    abi_require(SC1(57, pipefd[0]) == 0 && SC1(57, pipefd[1]) == 0);
    fd = abi_open("/dev/null", 2);
    abi_require(fd >= 0);
    abi_record("sync.device", SC1(82, fd), -1, -1, 0, 0, 0);
    abi_record("sync.filesystem-device", SC1(267, fd), -1, -1, 0, 0, 0);
    abi_require(SC1(57, fd) == 0);
    int sockets[2];
    abi_require(SC4(199, 1, 1, 0, sockets) == 0);
    abi_record("sync.filesystem-socket", SC1(267, sockets[0]), -1, -1, 0, 0, 0);
    abi_require(SC1(57, sockets[0]) == 0 && SC1(57, sockets[1]) == 0);
    fd = SC1(20, 0);
    abi_require(fd >= 0);
    abi_record("sync.filesystem-epoll", SC1(267, fd), -1, -1, 0, 0, 0);
    abi_require(SC1(57, fd) == 0);
    abi_require(SC3(34, -100, "/sync-tmp", 0700) == 0);
    abi_require(SC5(40, "tmpfs", "/sync-tmp", "tmpfs", 0, 0) == 0);
    fd = abi_open("/sync-tmp/file", 0102);
    abi_require(fd >= 0 && SC3(64, fd, "memory", 6) == 6);
    abi_record("sync.filesystem-tmpfs", SC1(267, fd), abi_size(fd), abi_offset(fd), 0, 0, 0);
    abi_require(SC1(57, fd) == 0);
    abi_record("sync.global-mounted", SC0(81), -1, -1, 0, 0, 0);
    abi_require(SC2(39, "/sync-tmp", 0) == 0);
    abi_require(SC5(40, "tmpfs", "/sync-tmp", "tmpfs", 1, 0) == 0);
    fd = abi_open("/sync-tmp", 0200000);
    abi_require(fd >= 0);
    abi_record("sync.filesystem-readonly", SC1(267, fd), -1, -1, 0, 0, 0);
    abi_require(SC1(57, fd) == 0 && SC2(39, "/sync-tmp", 0) == 0);
    const long flags[] = {010000, 04010000, 04000000};
    const char *names[] = {"sync.dsync-write", "sync.sync-write", "sync.internal-bit"};
    for (usize n = 0; n < 3; n++) {
        fd = abi_open("/sync-probe", 2 | flags[n]);
        abi_require(fd >= 0);
        long result = SC3(64, fd, "disk", 4);
        abi_record(names[n], result, abi_size(fd), abi_offset(fd), 0, 0, 0);
        abi_require(SC1(57, fd) == 0);
    }
}
