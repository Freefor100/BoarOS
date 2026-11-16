#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <net/if.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "ipv4-abi:%d: %s errno=%d\n", __LINE__, #x, errno); exit(1); } } while (0)
#define ERROR(call, expected) do { errno = 0; CHECK((call) == -1 && errno == (expected)); } while (0)
static const void *const bad = (void *)(uintptr_t)-1;
static size_t page_size;

static int opt_set(int fd, int level, int option, const void *value, socklen_t size)
{ return (int)syscall(SYS_setsockopt, fd, level, option, value, size); }
static int opt_get(int fd, int level, int option, void *value, socklen_t *size)
{ return (int)syscall(SYS_getsockopt, fd, level, option, value, size); }
static int integer(int fd, int level, int option)
{
    int value = -99;
    socklen_t size = sizeof(value);
    CHECK(!opt_get(fd, level, option, &value, &size) && size == sizeof(value));
    return value;
}
static unsigned char *guarded(void)
{
    unsigned char *p = mmap(NULL, page_size * 2, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED && !mprotect(p + page_size, page_size, PROT_NONE));
    return p;
}
static void loopback_up(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(fd >= 0);
    struct ifreq interface = {.ifr_name = "lo"};
    CHECK(!ioctl(fd, SIOCGIFFLAGS, &interface));
    if (!(interface.ifr_flags & IFF_UP)) {
        interface.ifr_flags |= IFF_UP;
        CHECK(!ioctl(fd, SIOCSIFFLAGS, &interface));
    }
    CHECK(!close(fd));
}

static void scalar_options(void)
{
    for (unsigned kind = 0; kind < 3; kind++) {
        int fd = socket(AF_INET, kind == 0 ? SOCK_DGRAM : kind == 1 ? SOCK_STREAM : SOCK_RAW,
            kind == 2 ? IPPROTO_ICMP : 0);
        CHECK(fd >= 0);
        CHECK(integer(fd, SOL_SOCKET, SO_BROADCAST) == 0);
        int value = -7;
        CHECK(!opt_set(fd, SOL_SOCKET, SO_BROADCAST, &value, sizeof(value)));
        CHECK(integer(fd, SOL_SOCKET, SO_BROADCAST) == 1);
        ERROR(opt_set(fd, SOL_SOCKET, SO_BROADCAST, &value, 3), EINVAL);
        ERROR(opt_set(fd, SOL_SOCKET, SO_BROADCAST, bad, 4), EFAULT);
        socklen_t size = 0;
        CHECK(!opt_get(fd, SOL_SOCKET, SO_BROADCAST, (void *)bad, &size) && !size);
        unsigned char bytes[8];
        memset(bytes, 0xa5, sizeof(bytes)); size = 2;
        CHECK(!opt_get(fd, SOL_SOCKET, SO_BROADCAST, bytes, &size) && size == 2);
        CHECK(bytes[0] == 1 && bytes[1] == 0 && bytes[2] == 0xa5);

        int default_ttl = integer(fd, IPPROTO_IP, IP_TTL);
        CHECK(default_ttl == 64);
        value = 37;
        CHECK(!opt_set(fd, IPPROTO_IP, IP_TTL, &value, 4));
        CHECK(integer(fd, IPPROTO_IP, IP_TTL) == value);
        for (unsigned length = 1; length <= 3; length++) {
            memset(bytes, 0xa5, sizeof(bytes)); size = length;
            CHECK(!opt_get(fd, IPPROTO_IP, IP_TTL, bytes, &size) && size == 1);
            CHECK(bytes[0] == 37 && bytes[1] == 0xa5);
        }
        value = 0; ERROR(opt_set(fd, IPPROTO_IP, IP_TTL, &value, 4), EINVAL);
        value = 256; ERROR(opt_set(fd, IPPROTO_IP, IP_TTL, &value, 4), EINVAL);
        value = -2; ERROR(opt_set(fd, IPPROTO_IP, IP_TTL, &value, 4), EINVAL);
        ERROR(opt_set(fd, IPPROTO_IP, IP_TTL, bad, 0), EINVAL);
        ERROR(opt_set(fd, IPPROTO_IP, IP_TTL, bad, 1), EFAULT);
        unsigned char *p = guarded();
        p[page_size - 1] = 21;
        CHECK(!opt_set(fd, IPPROTO_IP, IP_TTL, p + page_size - 1, 3));
        CHECK(integer(fd, IPPROTO_IP, IP_TTL) == 21);
        ERROR(opt_set(fd, IPPROTO_IP, IP_TTL, p + page_size - 1, 4), EFAULT);
        value = -1;
        CHECK(!opt_set(fd, IPPROTO_IP, IP_TTL, &value, 4));
        CHECK(integer(fd, IPPROTO_IP, IP_TTL) == default_ttl);

        /* SOL_SOCKET copies the value first; SOL_IP publishes its length first. */
        socklen_t *readonly = (void *)p;
        *readonly = 4;
        CHECK(!mprotect(p, page_size, PROT_READ));
        value = 99;
        ERROR(opt_get(fd, SOL_SOCKET, SO_BROADCAST, &value, readonly), EFAULT);
        CHECK(value == 1);
        value = 99;
        ERROR(opt_get(fd, IPPROTO_IP, IP_TTL, &value, readonly), EFAULT);
        CHECK(value == 99);
        size = 3;
        ERROR(opt_get(fd, IPPROTO_IP, IP_TTL, (void *)bad, &size), EFAULT);
        CHECK(size == 1);
        CHECK(!munmap(p, page_size * 2));

        if (kind == 1) {
            value = 9;
            ERROR(opt_set(fd, IPPROTO_IP, IP_MULTICAST_TTL, &value, 4), EINVAL);
        } else {
            CHECK(integer(fd, IPPROTO_IP, IP_MULTICAST_TTL) == 1);
            value = 0;
            CHECK(!opt_set(fd, IPPROTO_IP, IP_MULTICAST_TTL, &value, 4));
            CHECK(integer(fd, IPPROTO_IP, IP_MULTICAST_TTL) == 0);
            value = 255;
            CHECK(!opt_set(fd, IPPROTO_IP, IP_MULTICAST_TTL, &value, 1));
            CHECK(integer(fd, IPPROTO_IP, IP_MULTICAST_TTL) == 255);
            value = -1;
            CHECK(!opt_set(fd, IPPROTO_IP, IP_MULTICAST_TTL, &value, 4));
            CHECK(integer(fd, IPPROTO_IP, IP_MULTICAST_TTL) == 1);
            value = 256;
            ERROR(opt_set(fd, IPPROTO_IP, IP_MULTICAST_TTL, &value, 4), EINVAL);
        }
        CHECK(!close(fd));
    }
    puts("IPV4 ABI PASS scalar-options");
}

static void device_options(void)
{
    for (unsigned family = 0; family < 2; family++) {
        int fd = socket(family ? AF_INET6 : AF_INET, SOCK_DGRAM, 0);
        CHECK(fd >= 0);
        char name[20];
        socklen_t size = 16;
        CHECK(!opt_get(fd, SOL_SOCKET, SO_BINDTODEVICE, (void *)bad, &size) && !size);
        CHECK(!opt_set(fd, SOL_SOCKET, SO_BINDTODEVICE, NULL, 0));
        CHECK(!opt_set(fd, SOL_SOCKET, SO_BINDTODEVICE, "lo", 2));
        memset(name, 0xa5, sizeof(name)); size = 15;
        ERROR(opt_get(fd, SOL_SOCKET, SO_BINDTODEVICE, name, &size), EINVAL);
        CHECK(size == 15 && (unsigned char)name[0] == 0xa5);
        size = sizeof(name);
        CHECK(!opt_get(fd, SOL_SOCKET, SO_BINDTODEVICE, name, &size));
        CHECK(size == 3 && !strcmp(name, "lo") && (unsigned char)name[3] == 0xa5);
        size = 16;
        ERROR(opt_get(fd, SOL_SOCKET, SO_BINDTODEVICE, (void *)bad, &size), EFAULT);
        CHECK(size == 16);
        ERROR(opt_set(fd, SOL_SOCKET, SO_BINDTODEVICE, "boar-missing", 13), ENODEV);
        ERROR(opt_set(fd, SOL_SOCKET, SO_BINDTODEVICE, bad, 1), EFAULT);
        unsigned char *p = guarded();
        unsigned char *tail = p + page_size - 15;
        memset(tail, 0, 15); memcpy(tail, "lo", 2);
        CHECK(!opt_set(fd, SOL_SOCKET, SO_BINDTODEVICE, tail, 16));
        ERROR(opt_set(fd, SOL_SOCKET, SO_BINDTODEVICE, p + page_size - 1, 15), EFAULT);
        CHECK(!opt_set(fd, SOL_SOCKET, SO_BINDTODEVICE, bad, 0));
        size = 0;
        CHECK(!opt_get(fd, SOL_SOCKET, SO_BINDTODEVICE, NULL, &size) && !size);
        CHECK(!munmap(p, page_size * 2) && !close(fd));
    }
    puts("IPV4 ABI PASS device-options");
}

static void multicast_interface(void)
{
    unsigned lo = if_nametoindex("lo");
    CHECK(lo);
    uint32_t address = htonl(INADDR_LOOPBACK), zero = 0;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(fd >= 0);
    CHECK(!opt_set(fd, IPPROTO_IP, IP_MULTICAST_IF, &address, 4));
    uint32_t output = 0; socklen_t size = 4;
    CHECK(!opt_get(fd, IPPROTO_IP, IP_MULTICAST_IF, &output, &size));
    CHECK(size == 4 && output == address);
    struct ip_mreq request = {.imr_multiaddr = {htonl(0xfefefefe)}, .imr_interface = {address}};
    CHECK(!opt_set(fd, IPPROTO_IP, IP_MULTICAST_IF, &request, sizeof(request)));
    struct ip_mreqn indexed = {.imr_ifindex = (int)lo};
    CHECK(!opt_set(fd, IPPROTO_IP, IP_MULTICAST_IF, &indexed, sizeof(indexed)));
    output = 1; size = 4;
    CHECK(!opt_get(fd, IPPROTO_IP, IP_MULTICAST_IF, &output, &size) && !output);
    indexed.imr_address.s_addr = address;
    CHECK(!opt_set(fd, IPPROTO_IP, IP_MULTICAST_IF, &indexed, sizeof(indexed)));
    indexed.imr_ifindex = INT_MAX;
    ERROR(opt_set(fd, IPPROTO_IP, IP_MULTICAST_IF, &indexed, sizeof(indexed)), EADDRNOTAVAIL);
    ERROR(opt_set(fd, IPPROTO_IP, IP_MULTICAST_IF, bad, 3), EINVAL);
    ERROR(opt_set(fd, IPPROTO_IP, IP_MULTICAST_IF, bad, 4), EFAULT);
    unsigned char *p = guarded();
    memcpy(p + page_size - 4, &address, 4);
    CHECK(!opt_set(fd, IPPROTO_IP, IP_MULTICAST_IF, p + page_size - 4, 7));
    ERROR(opt_set(fd, IPPROTO_IP, IP_MULTICAST_IF, p + page_size - 4, 8), EFAULT);
    size = 2; output = 0xa5a5a5a5;
    CHECK(!opt_get(fd, IPPROTO_IP, IP_MULTICAST_IF, &output, &size) && size == 2);
    CHECK(!memcmp(&output, &address, 2) && ((unsigned char *)&output)[2] == 0xa5);
    size = 2;
    ERROR(opt_get(fd, IPPROTO_IP, IP_MULTICAST_IF, (void *)bad, &size), EFAULT);
    CHECK(size == 2);
    uint32_t alias = htonl(UINT32_C(0x7f000002));
    CHECK(!opt_set(fd, IPPROTO_IP, IP_MULTICAST_IF, &alias, sizeof(alias)));
    struct sockaddr_in group = {.sin_family = AF_INET, .sin_port = htons(23456),
        .sin_addr = {htonl(UINT32_C(0xef010101))}};
    CHECK(sendto(fd, "alias", 5, MSG_DONTWAIT, (void *)&group, sizeof(group)) == 5);
    indexed.imr_ifindex = (int)lo;
    indexed.imr_address.s_addr = htonl(UINT32_C(0x0ac80009));
    int raw = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP); CHECK(raw >= 0);
    const int senders[] = {fd, raw};
    for (unsigned i = 0; i < sizeof(senders) / sizeof(senders[0]); i++) {
        /* Indexed selection retains the requested address, but send must
         * resolve it as local before publishing any packet. */
        CHECK(!opt_set(senders[i], IPPROTO_IP, IP_MULTICAST_IF, &indexed, sizeof(indexed)));
        ERROR(sendto(senders[i], "alias", 5, MSG_DONTWAIT, (void *)&group, sizeof(group)), ENETUNREACH);
    }
    CHECK(!close(raw));
    CHECK(!opt_set(fd, IPPROTO_IP, IP_MULTICAST_IF, &zero, 4));
    int tcp = socket(AF_INET, SOCK_STREAM, 0); CHECK(tcp >= 0);
    ERROR(opt_set(tcp, IPPROTO_IP, IP_MULTICAST_IF, &zero, 4), EINVAL);
    CHECK(!close(tcp) && !munmap(p, page_size * 2) && !close(fd));
    puts("IPV4 ABI PASS multicast-interface");
}

static uint16_t checksum(const void *data, size_t size)
{
    const unsigned char *bytes = data;
    uint32_t sum = 0;
    for (; size >= 2; size -= 2, bytes += 2) sum += ((unsigned)bytes[0] << 8) | bytes[1];
    if (size) sum += (unsigned)bytes[0] << 8;
    while (sum >> 16) sum = (sum & 65535) + (sum >> 16);
    return htons((uint16_t)~sum);
}
static void raw_boundaries(void)
{
    ERROR(socket(AF_INET, SOCK_RAW, 0), EPROTONOSUPPORT);
    ERROR(socket(AF_INET, SOCK_RAW, -1), EINVAL);
    ERROR(socket(AF_INET, SOCK_RAW, 263), EINVAL);
    int fd = socket(AF_INET, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_ICMP);
    CHECK(fd >= 0 && (fcntl(fd, F_GETFL) & O_NONBLOCK) && (fcntl(fd, F_GETFD) & FD_CLOEXEC));
    unsigned char request[24] = {8, 0};
    uint16_t id = htons((uint16_t)getpid()), sequence = htons(7), sum;
    memcpy(request + 4, &id, 2); memcpy(request + 6, &sequence, 2);
    for (unsigned i = 8; i < sizeof(request); i++) request[i] = (unsigned char)(i * 7);
    sum = checksum(request, sizeof(request)); memcpy(request + 2, &sum, 2);
    ERROR(send(fd, request, sizeof(request), 0), EDESTADDRREQ);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr = {htonl(INADDR_LOOPBACK)}};
    CHECK(!bind(fd, (void *)&address, sizeof(address)));
    struct sockaddr_in local = {0}; socklen_t size = sizeof(local);
    CHECK(!getsockname(fd, (void *)&local, &size));
    CHECK(local.sin_family == AF_INET && local.sin_addr.s_addr == address.sin_addr.s_addr &&
        local.sin_port == htons(IPPROTO_ICMP));
    unsigned char *large = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(large != MAP_FAILED);
    ERROR(sendto(fd, large, 65536, 0, (void *)&address, sizeof(address)), EMSGSIZE);
    ERROR(syscall(SYS_sendto, fd, bad, sizeof(request), 0, &address, sizeof(address)), EFAULT);
    CHECK(sendto(fd, request, sizeof(request), 0, (void *)&address, sizeof(address)) == (ssize_t)sizeof(request));
    unsigned char packet[128];
    int reply = 0;
    for (unsigned n = 0; n < 4 && !reply; n++) {
        struct pollfd ready = {.fd = fd, .events = POLLIN};
        CHECK(poll(&ready, 1, 2000) == 1 && (ready.revents & POLLIN));
        ssize_t full = recv(fd, packet, 1, MSG_PEEK | MSG_TRUNC);
        CHECK(full >= (ssize_t)(20 + sizeof(request)));
        struct iovec iov = {packet, 1};
        struct msghdr message = {.msg_iov = &iov, .msg_iovlen = 1};
        CHECK(recvmsg(fd, &message, MSG_PEEK) == 1 && (message.msg_flags & MSG_TRUNC));
        struct sockaddr_in peer = {0}; size = sizeof(peer);
        ssize_t received = recvfrom(fd, packet, sizeof(packet), 0, (void *)&peer, &size);
        CHECK(received == full && size == sizeof(peer) && peer.sin_family == AF_INET &&
            peer.sin_addr.s_addr == address.sin_addr.s_addr && !peer.sin_port);
        unsigned header = (packet[0] & 15) * 4;
        CHECK((packet[0] >> 4) == 4 && header >= 20 && received == (ssize_t)(header + sizeof(request)));
        if (packet[header] == 0 && !memcmp(packet + header + 4, request + 4, sizeof(request) - 4)) {
            CHECK(checksum(packet + header, (size_t)received - header) == 0);
            reply = 1;
        }
    }
    CHECK(reply && !munmap(large, 65536) && !close(fd));
    puts("IPV4 ABI PASS raw-boundaries");
}

static void permissions(void)
{
    fflush(NULL);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        /* Native capability v3 layout; standalone musl has no Linux headers. */
        struct { uint32_t version; int32_t pid; } header = {.version = 0x20080522U};
        struct { uint32_t effective, permitted, inheritable; } data[2];
        CHECK(!syscall(SYS_capget, &header, data));
        data[0].effective &= ~(1U << 13); /* CAP_NET_RAW */
        CHECK(!syscall(SYS_capset, &header, data));
        ERROR(socket(AF_INET, SOCK_RAW, IPPROTO_ICMP), EPERM);
        ERROR(socket(AF_INET, SOCK_RAW, 0), EPROTONOSUPPORT);
        ERROR(socket(AF_INET, SOCK_RAW, -1), EINVAL);
        ERROR(socket(AF_INET, SOCK_RAW | 0x100, IPPROTO_ICMP), EINVAL);
        int fd = socket(AF_INET, SOCK_DGRAM, 0); CHECK(fd >= 0);
        CHECK(!opt_set(fd, SOL_SOCKET, SO_BINDTODEVICE, "lo", 3));
        ERROR(opt_set(fd, SOL_SOCKET, SO_BINDTODEVICE, "lo", 3), EPERM);
        ERROR(opt_set(fd, SOL_SOCKET, SO_BINDTODEVICE, NULL, 0), EPERM);
        ERROR(opt_set(fd, SOL_SOCKET, SO_BINDTODEVICE, "boar-missing", 13), ENODEV);
        ERROR(opt_set(fd, SOL_SOCKET, SO_BINDTODEVICE, bad, 1), EFAULT);
        CHECK(!close(fd));
        _exit(0);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    puts("IPV4 ABI PASS permissions");
}

int main(void)
{
    page_size = (size_t)sysconf(_SC_PAGESIZE); CHECK(page_size >= 4096);
    loopback_up();
    scalar_options();
    device_options();
    multicast_interface();
    raw_boundaries();
    permissions();
    puts("IPV4 ABI PASS");
    return 0;
}
