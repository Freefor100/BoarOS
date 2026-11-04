#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <net/if.h>
#include <net/route.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* Linux LP64 UAPI shared by the native reference and RV64 probe. */
_Static_assert(sizeof(struct ifreq) == 40, "LP64 ifreq");
_Static_assert(sizeof(struct ifconf) == 16, "LP64 ifconf");
_Static_assert(offsetof(struct ifconf, ifc_buf) == 8, "LP64 ifconf pointer");
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "interface:%d: %s errno=%d\n", __LINE__, #x, errno); exit(1); } } while (0)

static struct ifreq named(const char *name)
{
    struct ifreq result;
    memset(&result, 0xa5, sizeof(result));
    CHECK(strlen(name) < IFNAMSIZ);
    memcpy(result.ifr_name, name, strlen(name) + 1);
    return result;
}

static void preserved(const struct ifreq *result, const struct ifreq *before,
                      size_t changed)
{
    CHECK(!memcmp((const char *)result + IFNAMSIZ + changed,
                  (const char *)before + IFNAMSIZ + changed,
                  sizeof(*result) - IFNAMSIZ - changed));
    CHECK(!memcmp(result->ifr_name, before->ifr_name, IFNAMSIZ - 1));
    CHECK(result->ifr_name[IFNAMSIZ - 1] == 0);
}

static void properties(int fd, const char *name, const char *ip,
                       const char *mask, int mtu, int hardware_type)
{
    struct ifreq result = named(name), before = result;
    CHECK(ioctl(fd, SIOCGIFADDR, &result) == 0);
    struct sockaddr_in *address = (void *)&result.ifr_addr;
    CHECK(address->sin_family == AF_INET && address->sin_port == 0);
    CHECK(address->sin_addr.s_addr == inet_addr(ip));
    for (size_t i = 0; i < sizeof(address->sin_zero); i++) CHECK(address->sin_zero[i] == 0);
    preserved(&result, &before, sizeof(*address));

    result = named(name); before = result;
    CHECK(ioctl(fd, SIOCGIFNETMASK, &result) == 0);
    address = (void *)&result.ifr_netmask;
    CHECK(address->sin_family == AF_INET && address->sin_port == 0);
    CHECK(address->sin_addr.s_addr == inet_addr(mask));
    for (size_t i = 0; i < sizeof(address->sin_zero); i++) CHECK(address->sin_zero[i] == 0);
    preserved(&result, &before, sizeof(*address));

    result = named(name); before = result;
    CHECK(ioctl(fd, SIOCGIFMTU, &result) == 0 && result.ifr_mtu == mtu);
    preserved(&result, &before, sizeof(result.ifr_mtu));
    result = named(name); before = result;
    CHECK(ioctl(fd, SIOCGIFFLAGS, &result) == 0);
    CHECK(result.ifr_flags & IFF_UP);
    CHECK((result.ifr_flags & IFF_LOOPBACK) == (hardware_type == 772 ? IFF_LOOPBACK : 0));
    preserved(&result, &before, sizeof(result.ifr_flags));
    result = named(name); before = result;
    CHECK(ioctl(fd, SIOCGIFHWADDR, &result) == 0 && result.ifr_hwaddr.sa_family == hardware_type);
    unsigned char *mac = (void *)result.ifr_hwaddr.sa_data;
    if (hardware_type == 772) {
        for (size_t i = 0; i < 6; i++) CHECK(mac[i] == 0);
    } else {
        CHECK(!(mac[0] & 1) && (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]));
    }
    preserved(&result, &before, 8);

    result = named(name); before = result;
    CHECK(ioctl(fd, SIOCGIFINDEX, &result) == 0 && result.ifr_ifindex > 0);
    preserved(&result, &before, sizeof(result.ifr_ifindex));
    int index = result.ifr_ifindex;
    memset(&result, 0xa5, sizeof(result)); result.ifr_ifindex = index; before = result;
    CHECK(ioctl(fd, SIOCGIFNAME, &result) == 0 && !strcmp(result.ifr_name, name));
    CHECK(!memcmp((char *)&result + IFNAMSIZ, (char *)&before + IFNAMSIZ, sizeof(result) - IFNAMSIZ));
    CHECK(result.ifr_name[IFNAMSIZ - 1] == 0);
}

static void bad_requests(int fd)
{
    const unsigned long queries[] = {SIOCGIFFLAGS, SIOCGIFADDR, SIOCGIFNETMASK,
                                     SIOCGIFHWADDR, SIOCGIFMTU, SIOCGIFINDEX};
    long page = sysconf(_SC_PAGESIZE); CHECK(page > 0);
    char *guard = mmap(NULL, 2 * (size_t)page, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); CHECK(guard != MAP_FAILED);
    CHECK(mprotect(guard + page, (size_t)page, PROT_NONE) == 0);
    struct ifreq *partial = (void *)(guard + page - 24);
    memset(partial, 0, 24); memcpy(partial->ifr_name, "lo", 3);
    for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); i++) {
        struct ifreq result = named("missing-if"), before = result;
        CHECK(ioctl(fd, queries[i], &result) == -1 && errno == ENODEV);
        CHECK(!memcmp(&result, &before, sizeof(result)));
        CHECK(ioctl(fd, queries[i], NULL) == -1 && errno == EFAULT);
        CHECK(ioctl(fd, queries[i], partial) == -1 && errno == EFAULT);
    }
    struct ifreq result = named("lo:any");
    CHECK(ioctl(fd, SIOCGIFFLAGS, &result) == 0 && (result.ifr_flags & IFF_LOOPBACK));
    result = named("lo:any");
    CHECK(ioctl(fd, SIOCGIFADDR, &result) == -1 && errno == EADDRNOTAVAIL);
    result = named("lo:any");
    CHECK(ioctl(fd, SIOCGIFNETMASK, &result) == -1 && errno == EADDRNOTAVAIL);
    memset(&result, 'x', sizeof(result));
    CHECK(ioctl(fd, SIOCGIFINDEX, &result) == -1 && errno == ENODEV);
    const int bad_indices[] = {0, -1, INT_MAX};
    for (size_t i = 0; i < sizeof(bad_indices) / sizeof(bad_indices[0]); i++) {
        result = named("lo"); result.ifr_ifindex = bad_indices[i];
        CHECK(ioctl(fd, SIOCGIFNAME, &result) == -1 && errno == ENODEV);
    }
    CHECK(ioctl(fd, SIOCGIFNAME, NULL) == -1 && errno == EFAULT);
    CHECK(ioctl(-1, SIOCGIFADDR, NULL) == -1 && errno == EBADF);
    int regular = open("/init", O_RDONLY); CHECK(regular >= 0);
    CHECK(ioctl(regular, SIOCGIFADDR, NULL) == -1 && errno == ENOTTY);
    CHECK(close(regular) == 0);

    struct ifreq *readonly = (void *)guard; *readonly = named("lo");
    CHECK(mprotect(guard, (size_t)page, PROT_READ) == 0);
    CHECK(ioctl(fd, SIOCGIFMTU, readonly) == -1 && errno == EFAULT);
    CHECK(munmap(guard, 2 * (size_t)page) == 0);
}

static void enumeration(int fd, const char *ethernet_ip)
{
    struct ifconf query; memset(&query, 0xa5, sizeof(query));
    query.ifc_len = -1; query.ifc_buf = NULL;
    struct ifconf before = query;
    CHECK(ioctl(fd, SIOCGIFCONF, &query) == 0 && query.ifc_len >= 40 && query.ifc_len % 40 == 0);
    int total = query.ifc_len;
    CHECK(!memcmp((char *)&query + sizeof(query.ifc_len), (char *)&before + sizeof(query.ifc_len),
                  sizeof(query) - sizeof(query.ifc_len)));
    struct ifreq *records = malloc((size_t)total); CHECK(records != NULL);
    memset(records, 0xa5, (size_t)total);
    query.ifc_len = total; query.ifc_req = records;
    CHECK(ioctl(fd, SIOCGIFCONF, &query) == 0 && query.ifc_len == total);
    int lo = 0, ethernet = 0;
    for (int i = 0; i < total / 40; i++) {
        struct sockaddr_in *address = (void *)&records[i].ifr_addr;
        CHECK(memchr(records[i].ifr_name, 0, IFNAMSIZ) && address->sin_family == AF_INET);
        if (!strcmp(records[i].ifr_name, "lo")) {
            CHECK(address->sin_addr.s_addr == inet_addr("127.0.0.1")); lo++;
        }
        if (ethernet_ip && !strcmp(records[i].ifr_name, "eth0")) {
            CHECK(address->sin_addr.s_addr == inet_addr(ethernet_ip)); ethernet++;
        }
    }
    CHECK(lo == 1 && (!ethernet_ip || ethernet == 1));
    const int lengths[] = {INT_MIN, -1, 0, 1, 39, 40, 41};
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        struct ifreq record; memset(&record, 0xa5, sizeof(record));
        query.ifc_len = lengths[i]; query.ifc_req = &record;
        CHECK(ioctl(fd, SIOCGIFCONF, &query) == 0 && query.ifc_len == (lengths[i] >= 40 ? 40 : 0));
        if (lengths[i] >= 40) CHECK(!memcmp(&record, records, sizeof(record)));
        else for (size_t j = 0; j < sizeof(record); j++) CHECK(((unsigned char *)&record)[j] == 0xa5);
        query.ifc_len = lengths[i]; query.ifc_buf = NULL;
        CHECK(ioctl(fd, SIOCGIFCONF, &query) == 0 && query.ifc_len == total);
    }
    CHECK(ioctl(fd, SIOCGIFCONF, NULL) == -1 && errno == EFAULT);
    query.ifc_len = 39; query.ifc_buf = (void *)(uintptr_t)1;
    CHECK(ioctl(fd, SIOCGIFCONF, &query) == 0 && query.ifc_len == 0);
    query.ifc_len = 40; query.ifc_buf = (void *)(uintptr_t)1;
    CHECK(ioctl(fd, SIOCGIFCONF, &query) == -1 && errno == EFAULT && query.ifc_len == 40);

    long page = sysconf(_SC_PAGESIZE); CHECK(page > 0);
    char *guard = mmap(NULL, 2 * (size_t)page, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); CHECK(guard != MAP_FAILED);
    CHECK(mprotect(guard + page, (size_t)page, PROT_NONE) == 0);
    query.ifc_len = 40; query.ifc_buf = guard + page - 20;
    CHECK(ioctl(fd, SIOCGIFCONF, &query) == -1 && errno == EFAULT && query.ifc_len == 40);
    CHECK(!memcmp(query.ifc_buf, records, 20));
    if (total >= 80) {
        query.ifc_len = 80; query.ifc_buf = guard + page - 40;
        CHECK(ioctl(fd, SIOCGIFCONF, &query) == -1 && errno == EFAULT && query.ifc_len == 80);
        CHECK(!memcmp(query.ifc_buf, records, 40));
    }
    struct ifconf *readonly = (void *)guard; *readonly = (struct ifconf){.ifc_len = 0};
    CHECK(mprotect(guard, (size_t)page, PROT_READ) == 0);
    CHECK(ioctl(fd, SIOCGIFCONF, readonly) == -1 && errno == EFAULT);
    CHECK(munmap(guard, 2 * (size_t)page) == 0); free(records);
}

static void flags(int fd)
{
    struct ifreq result = named("lo");
    CHECK(ioctl(fd, SIOCGIFFLAGS, &result) == 0);
    result.ifr_flags &= ~IFF_UP;
    CHECK(ioctl(fd, SIOCSIFFLAGS, &result) == 0);
    CHECK(ioctl(fd, SIOCGIFFLAGS, &result) == 0 && !(result.ifr_flags & (IFF_UP | IFF_RUNNING)));
    result.ifr_flags |= IFF_UP;
    CHECK(ioctl(fd, SIOCSIFFLAGS, &result) == 0);
    CHECK(ioctl(fd, SIOCGIFFLAGS, &result) == 0 && (result.ifr_flags & IFF_UP));
}

static void configure_loopback(int fd)
{
    int udp = socket(AF_INET, SOCK_DGRAM, 0), listener = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(udp >= 0 && listener >= 0);
    struct sockaddr_in bound = {.sin_family = AF_INET, .sin_addr.s_addr = inet_addr("127.0.0.1")};
    CHECK(!bind(udp, (void *)&bound, sizeof(bound)));
    CHECK(!bind(listener, (void *)&bound, sizeof(bound)) && !listen(listener, 1));
    socklen_t size = sizeof(bound);
    CHECK(!getsockname(listener, (void *)&bound, &size));
    int client = socket(AF_INET, SOCK_STREAM, 0); CHECK(client >= 0);
    CHECK(!connect(client, (void *)&bound, sizeof(bound)));
    int accepted = accept(listener, NULL, NULL); CHECK(accepted >= 0);
    struct ifreq req = named("lo");
    struct sockaddr_in *address = (void *)&req.ifr_addr;
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    address->sin_addr.s_addr = inet_addr("127.0.0.2");
    CHECK(ioctl(fd, SIOCSIFADDR, &req) == 0);
    size = sizeof(bound);
    CHECK(!getsockname(udp, (void *)&bound, &size) && bound.sin_addr.s_addr == inet_addr("127.0.0.1"));
    size = sizeof(bound);
    CHECK(!getsockname(listener, (void *)&bound, &size) && bound.sin_addr.s_addr == inet_addr("127.0.0.1"));
    size = sizeof(bound);
    CHECK(!getsockname(accepted, (void *)&bound, &size) && bound.sin_addr.s_addr == inet_addr("127.0.0.1"));
    int socket_error = -1; size = sizeof(socket_error);
    CHECK(!getsockopt(accepted, SOL_SOCKET, SO_ERROR, &socket_error, &size) && !socket_error);
    req = named("lo");
    CHECK(ioctl(fd, SIOCGIFADDR, &req) == 0);
    CHECK(((struct sockaddr_in *)&req.ifr_addr)->sin_addr.s_addr == inet_addr("127.0.0.2"));
    address = (void *)&req.ifr_addr;
    address->sin_addr.s_addr = inet_addr("127.0.0.1");
    CHECK(ioctl(fd, SIOCSIFADDR, &req) == 0);
    CHECK(!close(client) && !close(accepted) && !close(udp) && !close(listener));

    req = named("lo"); address = (void *)&req.ifr_netmask;
    memset(address, 0, sizeof(*address)); address->sin_family = AF_INET;
    address->sin_addr.s_addr = inet_addr("255.0.0.0");
    CHECK(ioctl(fd, SIOCSIFNETMASK, &req) == 0);
    req = named("lo"); req.ifr_mtu = 65536;
    CHECK(ioctl(fd, SIOCSIFMTU, &req) == 0);
    req.ifr_mtu = 32768;
    CHECK(ioctl(fd, SIOCSIFMTU, &req) == 0);
    req = named("lo"); CHECK(ioctl(fd, SIOCGIFMTU, &req) == 0 && req.ifr_mtu == 32768);
    req.ifr_mtu = 65536; CHECK(ioctl(fd, SIOCSIFMTU, &req) == 0);
    req.ifr_mtu = -1; CHECK(ioctl(fd, SIOCSIFMTU, &req) == -1 && errno == EINVAL);
    req = named("lo"); address = (void *)&req.ifr_netmask;
    memset(address, 0, sizeof(*address)); address->sin_family = AF_INET;
    address->sin_addr.s_addr = inet_addr("255.0.255.0");
    CHECK(ioctl(fd, SIOCSIFNETMASK, &req) == -1 && errno == EINVAL);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        if (setuid(65534)) _exit(2);
        req = named("lo"); req.ifr_mtu = 1400;
        _exit(ioctl(fd, SIOCSIFMTU, &req) == -1 && errno == EPERM ? 0 : 3);
    }
    int status; CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
}

static void default_route(int fd)
{
    struct rtentry route = {0};
    struct sockaddr_in *dst = (void *)&route.rt_dst;
    struct sockaddr_in *gateway = (void *)&route.rt_gateway;
    dst->sin_family = AF_INET;
    gateway->sin_family = AF_INET;
    gateway->sin_addr.s_addr = inet_addr("127.0.0.2");
    route.rt_flags = RTF_UP | RTF_GATEWAY;
    route.rt_dev = "lo";
    gateway->sin_addr.s_addr = inet_addr("192.0.2.1");
    CHECK(ioctl(fd, SIOCADDRT, &route) == -1 && errno == ENETUNREACH);
    gateway->sin_addr.s_addr = inet_addr("127.0.0.2");
    CHECK(ioctl(fd, SIOCADDRT, &route) == 0);
    CHECK(ioctl(fd, SIOCADDRT, &route) == -1 && errno == EEXIST);
    FILE *routes = fopen("/proc/net/route", "r"); CHECK(routes != NULL);
    char line[256], device[64]; unsigned long destination, gw, mask;
    unsigned flags; int found = 0;
    CHECK(fgets(line, sizeof(line), routes) != NULL);
    while (fgets(line, sizeof(line), routes)) {
        if (sscanf(line, "%63s %lx %lx %x %*u %*u %*u %lx", device,
                   &destination, &gw, &flags, &mask) == 5 &&
            !strcmp(device, "lo") && !destination && gw == inet_addr("127.0.0.2") &&
            (flags & 3U) == 3U && !mask) found++;
    }
    CHECK(!fclose(routes) && found == 1);
    memset(&route.rt_gateway, 0, sizeof(route.rt_gateway));
    route.rt_flags = RTF_UP; route.rt_dev = NULL;
    CHECK(ioctl(fd, SIOCDELRT, &route) == 0);
}

static void run_busybox(char *const arguments[])
{
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) { execv("/busybox", arguments); _exit(127); }
    int status; CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
}

static void busybox_configuration(int fd)
{
    char *set[] = {"busybox", "ifconfig", "lo", "127.0.0.2", "netmask", "255.0.0.0", "mtu", "32768", NULL};
    char *add[] = {"busybox", "route", "add", "default", "gw", "127.0.0.2", "dev", "lo", NULL};
    char *show[] = {"busybox", "route", "-n", NULL};
    char *del[] = {"busybox", "route", "del", "default", "gw", "127.0.0.2", "dev", "lo", NULL};
    char *restore[] = {"busybox", "ifconfig", "lo", "127.0.0.1", "netmask", "255.0.0.0", "mtu", "65536", NULL};
    run_busybox(set);
    struct ifreq req = named("lo");
    CHECK(!ioctl(fd, SIOCGIFADDR, &req) && ((struct sockaddr_in *)&req.ifr_addr)->sin_addr.s_addr == inet_addr("127.0.0.2"));
    req = named("lo"); CHECK(!ioctl(fd, SIOCGIFMTU, &req) && req.ifr_mtu == 32768);
    run_busybox(add); run_busybox(show); run_busybox(del); run_busybox(restore);
    req = named("eth0");
    if (ioctl(fd, SIOCGIFINDEX, &req) == 0) {
        char *eth[] = {"busybox", "ifconfig", "eth0", "10.77.0.2", "netmask", "255.255.255.0", "mtu", "1400", NULL};
        char *eth_add[] = {"busybox", "route", "add", "default", "gw", "10.77.0.1", "dev", "eth0", NULL};
        char *eth_del[] = {"busybox", "route", "del", "default", "gw", "10.77.0.1", "dev", "eth0", NULL};
        run_busybox(eth);
        req = named("eth0");
        CHECK(!ioctl(fd, SIOCGIFADDR, &req) && ((struct sockaddr_in *)&req.ifr_addr)->sin_addr.s_addr == inet_addr("10.77.0.2"));
        req = named("eth0"); CHECK(!ioctl(fd, SIOCGIFMTU, &req) && req.ifr_mtu == 1400);
        run_busybox(eth_add); run_busybox(show); run_busybox(eth_del);
        puts("NETWORK PASS busybox-eth0");
    }
    puts("NETWORK PASS busybox-ipv4");
}

static void remote_configure(int fd)
{
    cpu_set_t previous, remote;
    CHECK(!sched_getaffinity(0, sizeof(previous), &previous));
    if (!CPU_ISSET(1, &previous)) return;
    CPU_ZERO(&remote); CPU_SET(1, &remote);
    CHECK(!sched_setaffinity(0, sizeof(remote), &remote));
    struct ifreq req = named("lo"); req.ifr_mtu = 32768;
    CHECK(!ioctl(fd, SIOCSIFMTU, &req));
    req = named("lo"); CHECK(!ioctl(fd, SIOCGIFMTU, &req) && req.ifr_mtu == 32768);
    req.ifr_mtu = 65536; CHECK(!ioctl(fd, SIOCSIFMTU, &req));
    CHECK(!sched_setaffinity(0, sizeof(previous), &previous));
    puts("NETWORK PASS remote-ipv4");
}

int main(int argc, char **argv)
{
    int set_flags = 0, ethernet_mtu = 0;
    const char *ethernet_ip = NULL, *ethernet_mask = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--set-flags")) set_flags = 1;
        else if (!strcmp(argv[i], "--eth0") && i + 3 < argc) {
            ethernet_ip = argv[++i]; ethernet_mask = argv[++i]; ethernet_mtu = atoi(argv[++i]);
            CHECK(inet_addr(ethernet_ip) != INADDR_NONE && inet_addr(ethernet_mask) != INADDR_NONE && ethernet_mtu > 0);
        } else CHECK(!"usage: interface [--set-flags] [--eth0 ADDRESS NETMASK MTU]");
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    if (getpid() == 1) {
        CHECK(mkdir("/proc", 0555) == 0 || errno == EEXIST);
        CHECK(mount("proc", "/proc", "proc", 0, NULL) == 0 || errno == EBUSY);
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0); CHECK(fd >= 0);
    flags(fd);
    configure_loopback(fd);
    default_route(fd);
    remote_configure(fd);
    if (access("/busybox", X_OK) == 0) busybox_configuration(fd);
    if (set_flags) flags(fd);
    properties(fd, "lo", "127.0.0.1", "255.0.0.0", 65536, 772);
    if (ethernet_ip) properties(fd, "eth0", ethernet_ip, ethernet_mask, ethernet_mtu, 1);
    bad_requests(fd); enumeration(fd, ethernet_ip);
    CHECK(close(fd) == 0);
    puts("NETWORK PASS interface");
    return 0;
}
