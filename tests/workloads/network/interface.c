#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <net/if.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
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
    int regular = open("/dev/null", O_RDONLY); CHECK(regular >= 0);
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
    CHECK(ioctl(fd, SIOCGIFFLAGS, &result) == 0 && !(result.ifr_flags & IFF_UP));
    result.ifr_flags |= IFF_UP;
    CHECK(ioctl(fd, SIOCSIFFLAGS, &result) == 0);
    CHECK(ioctl(fd, SIOCGIFFLAGS, &result) == 0 && (result.ifr_flags & IFF_UP));
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
    int fd = socket(AF_INET, SOCK_DGRAM, 0); CHECK(fd >= 0);
    if (set_flags) flags(fd);
    properties(fd, "lo", "127.0.0.1", "255.0.0.0", 65536, 772);
    if (ethernet_ip) properties(fd, "eth0", ethernet_ip, ethernet_mask, ethernet_mtu, 1);
    bad_requests(fd); enumeration(fd, ethernet_ip);
    CHECK(close(fd) == 0);
    puts("NETWORK PASS interface");
    return 0;
}
