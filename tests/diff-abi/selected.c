#include "abi.h"

static void result(const char *id, long value)
{ abi_record(id, value, -1, -1, 0, 0, 0); }
static void close_fd(long fd) { abi_require(SC1(57, fd) == 0); }

static void positioned_vectors(void)
{
    long fd = abi_open("/selected-iov", 2 | 64 | 512);
    abi_require(fd >= 0 && SC3(64, fd, "abcdefghijklmnop", 16) == 16);
    abi_require(SC3(62, fd, 3, 0) == 3);
    char data[8] = {0};
    struct abi_iovec iov[3] = {{data, 2}, {data + 2, 3}, {data + 5, 0}};
    long rc = SC5(69, fd, iov, 3, 6, 0);
    abi_record("selected.preadv.data", rc, abi_size(fd), abi_offset(fd), 0, data, rc > 0 ? rc : 0);
    iov[0] = (struct abi_iovec){"XY", 2};
    iov[1] = (struct abi_iovec){"Z", 1};
    rc = SC5(70, fd, iov, 2, 5, 0);
    abi_record("selected.pwritev.data", rc, abi_size(fd), abi_offset(fd), 0, 0, 0);
    abi_require(SC4(67, fd, data, 8, 3) == 8);
    abi_record("selected.pwritev.content", 0, -1, abi_offset(fd), 0, data, 8);
    result("selected.preadv.negative", SC5(69, -1, 0, 0, -1, 0));
    result("selected.pwritev.negative", SC5(70, -1, 0, 0, -1, 0));
    result("selected.preadv.iovcount", SC5(69, fd, 0, 1025, 0, 0));
    result("selected.pwritev.iovfault", SC5(70, fd, -1, 1, 0, 0));
    result("selected.preadv.empty", SC5(69, fd, -1, 0, 0, 0));
    result("selected.pwritev.empty", SC5(70, fd, -1, 0, 0, 0));
    long map = SC6(222, 0, 2 * ABI_PAGE_SIZE, 3, 0x22, -1, 0);
    abi_require(map > 0 && SC3(226, map + ABI_PAGE_SIZE, ABI_PAGE_SIZE, 0) == 0);
    for (long i = 0; i < 8; ++i) ((char *)map)[ABI_PAGE_SIZE - 8 + i] = 'q';
    iov[0] = (struct abi_iovec){"12", 2};
    iov[1] = (struct abi_iovec){(char *)map + ABI_PAGE_SIZE - 8, 16};
    rc = SC5(70, fd, iov, 2, 32, 0);
    abi_record("selected.pwritev.partial", rc, abi_size(fd), abi_offset(fd), 0, 0, 0);
    iov[0] = (struct abi_iovec){data, 2};
    iov[1] = (struct abi_iovec){(char *)map + ABI_PAGE_SIZE - 8, 16};
    rc = SC5(69, fd, iov, 2, 0, 0);
    abi_record("selected.preadv.partial", rc, abi_size(fd), abi_offset(fd), 0, data, rc > 0 ? 2 : 0);
    iov[0] = (struct abi_iovec){(void *)-1, 1};
    result("selected.preadv.bufferfault", SC5(69, fd, iov, 1, 0, 0));
    result("selected.pwritev.bufferfault", SC5(70, fd, iov, 1, 0, 0));
    abi_require(SC2(215, map, 2 * ABI_PAGE_SIZE) == 0);
    close_fd(fd);
    fd = abi_open("/selected-iov", 2 | 1024);
    abi_require(fd >= 0);
    iov[0] = (struct abi_iovec){"A", 1}; iov[1] = (struct abi_iovec){"B", 1};
    rc = SC5(70, fd, iov, 2, 0, 0);
    abi_record("selected.pwritev.append", rc, abi_size(fd), abi_offset(fd), 0, 0, 0);
    close_fd(fd);
    int pipefd[2]; abi_require(SC2(59, pipefd, 0) == 0);
    result("selected.preadv.pipe", SC5(69, pipefd[0], -1, 1025, 0, 0));
    result("selected.pwritev.pipe", SC5(70, pipefd[1], -1, 1025, 0, 0));
    result("selected.preadv.pipe-write-end", SC5(69, pipefd[1], -1, 1025, 0, 0));
    result("selected.pwritev.pipe-read-end", SC5(70, pipefd[0], -1, 1025, 0, 0));
    result("selected.pread.pipe-write-end", SC4(67, pipefd[1], -1, 1, 0));
    result("selected.pwrite.pipe-read-end", SC4(68, pipefd[0], -1, 1, 0));
    close_fd(pipefd[0]); close_fd(pipefd[1]);
    abi_require(SC3(35, -100, "/selected-iov", 0) == 0);
}

struct selected_flock { short type, whence; long start, length; int pid; };
static void lock_parameters(void)
{
    long fd = abi_open("/selected-lock", 2 | 64 | 512);
    abi_require(fd >= 0 && SC3(62, fd, 1, 0) == 1);
    struct selected_flock f = {.type = 1, .whence = 1, .start = 0x7fffffffffffffffL, .length = 1};
    result("selected.lock.start-overflow", SC3(25, fd, 6, &f));
    f.type = 3;
    result("selected.lock.set-type-after-range", SC3(25, fd, 6, &f));
    result("selected.lock.get-type-before-range", SC3(25, fd, 5, &f));
    f.type = 1; f.pid = 42;
    result("selected.lock.ofd-pid-after-range", SC3(25, fd, 37, &f));
    f.start = 0; f.whence = 7;
    result("selected.lock.device-invalid-parameters", SC3(25, 1, 6, &f));
    result("selected.lock.whence", SC3(25, fd, 6, &f));
    f.whence = 0; f.start = 0x7fffffffffffffffL; f.length = 2;
    result("selected.lock.end-overflow", SC3(25, fd, 6, &f));
    f.start = 1; f.length = -2;
    result("selected.lock.negative-range", SC3(25, fd, 6, &f));
    close_fd(fd);
    fd = abi_open("/selected-lock", 0); abi_require(fd >= 0);
    f = (struct selected_flock){.type=1, .length=1, .pid=42};
    result("selected.lock.ofd-access-before-pid", SC3(25, fd, 37, &f));
    close_fd(fd); abi_require(SC3(35, -100, "/selected-lock", 0) == 0);
}

static void path_parameters(void)
{
    abi_require(SC3(34, -100, "/selected-dir", 0700) == 0);
    long rc = abi_open("/selected-dir", 64);
    result("selected.open.create-directory", rc); if (rc >= 0) close_fd(rc);
    rc = abi_open("/selected-dir", 64 | 128);
    result("selected.open.exclusive-directory", rc); if (rc >= 0) close_fd(rc);
    rc = abi_open("/selected-dir", 64 | 0x10000);
    result("selected.open.directory-create", rc); if (rc >= 0) close_fd(rc);
    long fd = abi_open("/selected-dir", 0x10000); abi_require(fd >= 0);
    abi_require(SC3(35, -100, "/selected-dir", 0x200) == 0);
    char entries[512];
    result("selected.getdents.deleted-directory", SC3(61, fd, entries, sizeof(entries)));
    close_fd(fd);
}

static void time_parameters(void)
{
    int timezone[2] = {17, 19}; long tv[2] = {-1, -1};
    long rc = SC2(169, 0, timezone);
    abi_record("selected.gettimeofday.timezone", rc, timezone[0], timezone[1], 0, 0, 0);
    result("selected.gettimeofday.timezone-fault", SC2(169, 0, -1));
    timezone[0] = 17; timezone[1] = 19;
    rc = SC2(169, -1, timezone);
    abi_record("selected.gettimeofday.timeval-first", rc, timezone[0], timezone[1], 0, 0, 0);
    rc = SC2(169, tv, -1);
    abi_record("selected.gettimeofday.timeval-written", rc, tv[0] >= 0 && tv[1] >= 0 && tv[1] < 1000000, -1, 0, 0, 0);
    long zero[2] = {0, 0};
    result("selected.sleep.thread-clock", SC4(115, 3, 0, zero, 0));
    result("selected.sleep.thread-clock-before-copy", SC4(115, 3, 0, -1, 0));
}

static void map_case(const char *name, long fd, long flags)
{
    long rc = SC6(222, 0, ABI_PAGE_SIZE, 3, flags, fd, 0);
    result(name, rc < 0 ? rc : 0);
    if (rc > 0) abi_require(SC2(215, rc, ABI_PAGE_SIZE) == 0);
}
static void mapping_parameters(void)
{
    long fd = abi_open("/data", 2); abi_require(fd >= 0);
    map_case("selected.mmap.no-type", -1, 0x20);
    map_case("selected.mmap.invalid-type", -1, 0x24);
    map_case("selected.mmap.anonymous-validate", -1, 0x23);
    map_case("selected.mmap.private-unknown", -1, 0x200022);
    map_case("selected.mmap.shared-unknown", fd, 0x200001);
    map_case("selected.mmap.validate", fd, 3);
    map_case("selected.mmap.validate-huge-encoding", fd, 0x40000003);
    map_case("selected.mmap.validate-nonblock", fd, 0x10003);
    map_case("selected.mmap.validate-unknown", fd, 0x200003);
    map_case("selected.mmap.validate-sync", fd, 0x80003);
    map_case("selected.mmap.badfd-before-validation", -1, 0x200003);
    map_case("selected.mmap.badfd-before-type", -1, 0);
    result("selected.mmap.zero-length-before-validation", SC6(222, 0, 0, 3, 0x200003, fd, 0));
    result("selected.mmap.zero-length-anonymous", SC6(222, 0, 0, 3, 0x200023, -1, 0));
    map_case("selected.mmap.huge-regular", fd, 0x40002);
    /* 16 KiB is not a huge-page hstate in either fixed Linux reference.
     * Default huge sizes differ by reference Kconfig; preserve that comparison
     * separately instead of requiring BoarOS to advertise a nonexistent size. */
    map_case("selected.mmap.huge-anonymous-unsupported-size", -1, (14UL << 26) | 0x40022);
    map_case("selected.mmap.huge-shared-anonymous-unsupported-size", -1, (14UL << 26) | 0x40021);
    close_fd(fd);
}

static volatile int signal_seen;
static void handler(int signal) { signal_seen = signal; }
struct selected_action { unsigned long handler, flags, mask; };
static void reset_action(void)
{
    struct selected_action action = {(unsigned long)handler, 0x80000000UL | 0x10000000UL, 1UL << 11};
    struct selected_action old, observed;
    abi_require(SC4(134, 10, &action, &old, 8) == 0);
    abi_require(SC2(130, SC0(178), 10) == 0);
    abi_require(SC4(134, 10, 0, &observed, 8) == 0);
    abi_record("selected.signal.reset-handler", 0, observed.handler, observed.flags == action.flags,
        signal_seen == 10, &observed.mask, sizeof(observed.mask));
    abi_require(SC4(134, 10, &old, 0, 8) == 0);
}

struct selected_epoll_event { unsigned events, pad; unsigned long data; };
static void epoll_nesting(void)
{
    long fd[7]; for (int i = 0; i < 7; ++i) { fd[i] = SC1(20, 0); abi_require(fd[i] >= 0); }
    struct selected_epoll_event event = {1, 0, 42};
    result("selected.epoll.self", SC4(21, fd[0], 1, fd[0], &event));
    for (int i = 0; i < 4; ++i) abi_require(SC4(21, fd[i], 1, fd[i + 1], &event) == 0);
    result("selected.epoll.five", 0);
    result("selected.epoll.sixth", SC4(21, fd[4], 1, fd[5], &event));
    result("selected.epoll.cycle", SC4(21, fd[4], 1, fd[0], &event));
    for (int i = 0; i < 7; ++i) close_fd(fd[i]);
}

struct selected_address { unsigned short family, port; unsigned char ip[4], zero[8]; };
static void socket_parameters(void)
{
    const long input[][3] = {{-1,1,0},{2,11,0},{2,1,-1},{2,2,263},{2,3,0},{2,1,17},{2,2,6},{2,0,0}};
    const char *names[] = {"selected.socket.family", "selected.socket.type", "selected.socket.negative-protocol",
        "selected.socket.large-protocol", "selected.socket.raw-zero", "selected.socket.stream-protocol",
        "selected.socket.datagram-protocol", "selected.socket.zero-type"};
    for (unsigned i = 0; i < sizeof(input)/sizeof(input[0]); ++i) {
        long rc = SC3(198, input[i][0], input[i][1], input[i][2]);
        result(names[i], rc < 0 ? rc : 0); if (rc >= 0) close_fd(rc);
    }
    long receiver = SC3(198, 2, 2, 0), sender = SC3(198, 2, 2 | 0x800, 0);
    abi_require(receiver >= 0 && sender >= 0);
    struct { char name[16]; short flags; char padding[22]; } interface = {.name="lo"};
    abi_require(SC3(29, receiver, 0x8913, &interface) == 0);
    interface.flags |= 1;
    abi_require(SC3(29, receiver, 0x8914, &interface) == 0);
    struct selected_address local = {2, 0, {0,0,0,0}, {0}}; unsigned size = sizeof(local);
    abi_require(SC3(200, receiver, &local, sizeof(local)) == 0);
    abi_require(SC3(204, receiver, &local, &size) == 0);
    abi_record("selected.socket.bind-wildcard", 0, -1, -1, 0, local.ip, 4);
    long rc = SC3(203, sender, &local, sizeof(local));
    result("selected.socket.connect-zero", rc);
    struct selected_address peer = {0}; size = sizeof(peer);
    rc = SC3(205, sender, &peer, &size);
    abi_record("selected.socket.peer-loopback", rc, -1, -1, 0, peer.ip, 4);
    char byte = 0;
    abi_require(SC3(25, receiver, 4, 0x800) == 0);
    rc = SC6(206, sender, "z", 1, 0, &local, sizeof(local));
    result("selected.socket.sendto-zero", rc);
    /* sendto services loopback inline; ppoll gives either stack a bounded drain. */
    struct { int fd; short events, revents; } poll = {(int)receiver, 1, 0};
    long wait[2] = {1, 0};
    abi_require(SC5(73, &poll, 1, wait, 0, 8) >= 0);
    rc = SC6(207, receiver, &byte, 1, 0, 0, 0);
    abi_record("selected.socket.receive-zero", rc, -1, -1, 0, rc > 0 ? &byte : 0, rc > 0 ? 1 : 0);
    close_fd(sender); close_fd(receiver);
    long listener = SC3(198, 2, 1 | 0x800, 0), client = SC3(198, 2, 1, 0);
    abi_require(listener >= 0 && client >= 0);
    local = (struct selected_address){2, 0, {0,0,0,0}, {0}};
    abi_require(SC3(200, listener, &local, sizeof(local)) == 0);
    size = sizeof(local);
    abi_require(SC3(204, listener, &local, &size) == 0 && SC2(201, listener, 4) == 0);
    rc = SC3(203, client, &local, sizeof(local));
    result("selected.socket.tcp-connect-zero", rc);
    long accepted = SC3(202, listener, 0, 0);
    result("selected.socket.tcp-accept-zero", accepted < 0 ? accepted : 0);
    if (accepted >= 0) close_fd(accepted);
    close_fd(client); close_fd(listener);
}

void abi_selected_cases(void)
{
    positioned_vectors(); lock_parameters(); path_parameters(); time_parameters();
    mapping_parameters(); reset_action(); epoll_nesting(); socket_parameters();
}
