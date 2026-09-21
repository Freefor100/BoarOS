#include "abi.h"

void abi_device_cases(void)
{
    struct abi_stat stat;
    unsigned char buffer[8] = {0xa5, 0xa5, 0xa5, 0xa5,
                               0xa5, 0xa5, 0xa5, 0xa5};
    long map = CALL(222, 0, 8192, 3, 0x22, -1, 0);
    abi_require(map >= 0 && SC3(226, map + 4096, 4096, 0) == 0);
    for (int index = 0; index < 4; index++)
        ((unsigned char *)map)[4092 + index] = 0xa5;
    long fd = abi_open("/dev/null", 2);
    abi_record("device.null-open", fd < 0 ? fd : 0, -1, -1, 0, 0, 0);
    if (fd < 0) return;
    abi_record("device.null-write-unmapped", SC3(64, fd, 0x30000, 8),
               -1, -1, 0, 0, 0);
    abi_record("device.null-write-zero-invalid", SC3(64, fd, -1, 0),
               -1, -1, 0, 0, 0);
    abi_record("device.null-read", SC3(63, fd, buffer, 8),
               -1, -1, 0, buffer, sizeof(buffer));
    abi_record("device.null-read-invalid", SC3(63, fd, -1, 8),
               -1, -1, 0, 0, 0);
    abi_record("device.null-read-zero-invalid", SC3(63, fd, -1, 0),
               -1, -1, 0, 0, 0);
    abi_record("device.null-read-partial", SC3(63, fd, map + 4092, 8),
               -1, -1, 0, (void *)(map + 4092), 4);
    abi_record("device.null-write-partial", SC3(64, fd, map + 4092, 8),
               -1, -1, 0, 0, 0);
    abi_record("device.null-pread", SC4(67, fd, buffer, 8, 123),
               -1, -1, 0, 0, 0);
    abi_record("device.null-pwrite-unmapped", SC4(68, fd, 0x30000, 8, 123),
               -1, -1, 0, 0, 0);
    abi_record("device.null-pwrite-zero-invalid", SC4(68, fd, -1, 0, 123),
               -1, -1, 0, 0, 0);
    struct abi_iovec null_vector[2] = {{buffer, 3}, {(void *)0x30000, 5}};
    abi_record("device.null-writev-unmapped", SC3(66, fd, null_vector, 2),
               -1, -1, 0, 0, 0);
    struct abi_iovec null_empty_vector[1] = {{(void *)-1, 0}};
    abi_record("device.null-writev-zero-invalid",
               SC3(66, fd, null_empty_vector, 1),
               -1, -1, 0, 0, 0);
    struct { int fd; short events, revents; } null_poll =
        {(int)fd, 5, 0};
    struct { long sec, nsec; } timeout = {0, 0};
    long poll_result = CALL(73, &null_poll, 1, &timeout, 0, 0, 0);
    abi_record("device.null-poll", poll_result, -1, -1, 0,
               &null_poll.revents, sizeof(null_poll.revents));
    abi_record("device.null-seek", SC3(62, fd, 123, 0),
               -1, -1, 0, 0, 0);
    abi_record("device.null-ioctl-unknown", SC3(29, fd, 0xdead, 0),
               -1, -1, 0, 0, 0);
    long result = SC2(80, fd, &stat);
    abi_record("device.null-stat", result, -1, -1, 0,
               &stat.rdev, sizeof(stat.rdev));
    long epfd = SC1(20, 0);
    struct __attribute__((packed)) { unsigned int events; unsigned long data; }
        event = {1, 123};
    abi_require(epfd >= 0);
    abi_record("device.null-epoll-add", SC4(21, epfd, 1, fd, &event),
               -1, -1, 0, 0, 0);
    abi_require(SC1(57, fd) == 0);
    fd = abi_open("/dev/null", 0x201);
    abi_record("device.null-open-trunc", fd < 0 ? fd : 0,
               -1, -1, 0, 0, 0);
    if (fd >= 0) abi_require(SC1(57, fd) == 0);
    fd = abi_open("/dev/null", 1);
    abi_require(fd >= 0);
    abi_record("device.null-writeonly-read", SC3(63, fd, buffer, 1),
               -1, -1, 0, 0, 0);
    abi_require(SC1(57, fd) == 0);

    fd = abi_open("/dev/zero", 2);
    abi_record("device.zero-open", fd < 0 ? fd : 0, -1, -1, 0, 0, 0);
    if (fd < 0) return;
    abi_record("device.zero-read", SC3(63, fd, buffer, 8),
               -1, -1, 0, buffer, sizeof(buffer));
    abi_record("device.zero-read-invalid", SC3(63, fd, -1, 8),
               -1, -1, 0, 0, 0);
    abi_record("device.zero-read-zero-invalid", SC3(63, fd, -1, 0),
               -1, -1, 0, 0, 0);
    abi_record("device.zero-read-partial", SC3(63, fd, map + 4092, 8),
               -1, -1, 0, (void *)(map + 4092), 4);
    abi_record("device.zero-write-partial", SC3(64, fd, map + 4092, 8),
               -1, -1, 0, 0, 0);
    abi_record("device.zero-pread", SC4(67, fd, buffer, 8, 123),
               -1, -1, 0, buffer, sizeof(buffer));
    abi_record("device.zero-pwrite-unmapped", SC4(68, fd, 0x30000, 8, 123),
               -1, -1, 0, 0, 0);
    abi_record("device.zero-pwrite-zero-invalid", SC4(68, fd, -1, 0, 123),
               -1, -1, 0, 0, 0);
    struct abi_iovec zero_vector[2] = {{buffer, 3}, {buffer + 3, 5}};
    abi_record("device.zero-readv", SC3(65, fd, zero_vector, 2),
               -1, -1, 0, buffer, sizeof(buffer));
    struct { int fd; short events, revents; } zero_poll =
        {(int)fd, 5, 0};
    poll_result = CALL(73, &zero_poll, 1, &timeout, 0, 0, 0);
    abi_record("device.zero-poll", poll_result, -1, -1, 0,
               &zero_poll.revents, sizeof(zero_poll.revents));
    abi_record("device.zero-write-unmapped", SC3(64, fd, 0x30000, 8),
               -1, -1, 0, 0, 0);
    abi_record("device.zero-write-zero-invalid", SC3(64, fd, -1, 0),
               -1, -1, 0, 0, 0);
    struct abi_iovec zero_empty_vector[1] = {{(void *)-1, 0}};
    abi_record("device.zero-writev-zero-invalid",
               SC3(66, fd, zero_empty_vector, 1),
               -1, -1, 0, 0, 0);
    abi_record("device.zero-seek", SC3(62, fd, 123, 0),
               -1, -1, 0, 0, 0);
    abi_record("device.zero-ioctl-unknown", SC3(29, fd, 0xdead, 0),
               -1, -1, 0, 0, 0);
    result = SC2(80, fd, &stat);
    abi_record("device.zero-stat", result, -1, -1, 0,
               &stat.rdev, sizeof(stat.rdev));
    abi_record("device.zero-epoll-add", SC4(21, epfd, 1, fd, &event),
               -1, -1, 0, 0, 0);
    struct abi_iovec epoll_vector[1] = {{buffer, 1}};
    abi_record("epoll.read", SC3(63, epfd, buffer, 1),
               -1, -1, 0, 0, 0);
    abi_record("epoll.readv", SC3(65, epfd, epoll_vector, 1),
               -1, -1, 0, 0, 0);
    abi_record("epoll.pread", SC4(67, epfd, buffer, 1, 0),
               -1, -1, 0, 0, 0);
    abi_record("epoll.write", SC3(64, epfd, buffer, 1),
               -1, -1, 0, 0, 0);
    abi_record("epoll.writev", SC3(66, epfd, epoll_vector, 1),
               -1, -1, 0, 0, 0);
    abi_record("epoll.pwrite", SC4(68, epfd, buffer, 1, 0),
               -1, -1, 0, 0, 0);
    abi_record("epoll.seek", SC3(62, epfd, 123, 0),
               -1, -1, 0, 0, 0);
    result = SC2(80, epfd, &stat);
    abi_record("epoll.stat", result,
               result == 0 ? (long)stat.mode : -1,
               result == 0 ? (long)stat.nlink : -1, 0, 0, 0);
    abi_require(SC1(57, fd) == 0);
    abi_require(SC1(57, epfd) == 0);
    fd = abi_open("/dev/zero", 0x201);
    abi_record("device.zero-open-trunc", fd < 0 ? fd : 0,
               -1, -1, 0, 0, 0);
    if (fd >= 0) abi_require(SC1(57, fd) == 0);
    fd = abi_open("/dev/zero", 0);
    abi_require(fd >= 0);
    abi_record("device.zero-readonly-write", SC3(64, fd, buffer, 1),
               -1, -1, 0, 0, 0);
    abi_require(SC1(57, fd) == 0);
    abi_require(SC2(215, map, 8192) == 0);

    fd = abi_open("/dev/console", 0);
    abi_record("device.console-open-readonly", fd < 0 ? fd : 0,
               -1, -1, 0, 0, 0);
    if (fd < 0) return;
    abi_record("device.console-write-readonly", SC3(64, fd, buffer, 1),
               -1, -1, 0, 0, 0);
    result = SC2(80, fd, &stat);
    abi_record("device.console-stat", result, -1, -1, 0,
               &stat.rdev, sizeof(stat.rdev));
    abi_require(SC1(57, fd) == 0);

    fd = abi_open("/positioned", 0x242);
    abi_require(fd >= 0 && SC3(64, fd, "abc", 3) == 3 &&
                SC3(62, fd, 1, 0) == 1);
    abi_record("positioned.file-write", SC4(68, fd, "Z", 1, 2),
               abi_size(fd), abi_offset(fd), 0, 0, 0);
    abi_require(SC3(62, fd, 0, 0) == 0);
    abi_record("positioned.file-read", SC3(63, fd, buffer, 3),
               abi_size(fd), abi_offset(fd), 0, buffer, 3);
    abi_require(SC1(57, fd) == 0);
}
