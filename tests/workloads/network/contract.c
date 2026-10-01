#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "network:%d: %s errno=%d\n", __LINE__, #x, errno); exit(1); } } while (0)

static void addresses(void)
{
    int udp = socket(AF_INET6, SOCK_DGRAM, 0);
    CHECK(udp >= 0);
    struct ifreq interface = {.ifr_name="lo"};
    CHECK(ioctl(udp, SIOCGIFFLAGS, &interface) == 0);
    interface.ifr_flags |= IFF_UP;
    CHECK(ioctl(udp, SIOCSIFFLAGS, &interface) == 0);
    struct sockaddr_in6 address = {.sin6_family=AF_INET6, .sin6_addr=IN6ADDR_LOOPBACK_INIT};
    CHECK(bind(udp, (void *)&address, sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    CHECK(getsockname(udp, (void *)&address, &length) == 0 && length == sizeof(address));
    CHECK(address.sin6_family == AF_INET6 && address.sin6_port != 0);
    CHECK(sendto(udp, "v6", 2, 0, (void *)&address, sizeof(address)) == 2);
    char data[8]; struct sockaddr_in6 peer; length=sizeof(peer);
    CHECK(recvfrom(udp, data, sizeof(data), 0, (void *)&peer, &length) == 2);
    CHECK(!memcmp(data, "v6", 2) && peer.sin6_family == AF_INET6 && peer.sin6_port == address.sin6_port);
    CHECK(close(udp) == 0);
    for (unsigned mapped=0; mapped<2; mapped++) {
        int listener=socket(AF_INET6, SOCK_STREAM, 0); CHECK(listener>=0);
        memset(&address, 0, sizeof(address)); address.sin6_family=AF_INET6;
        CHECK(bind(listener, (void *)&address, sizeof(address)) == 0 && listen(listener, 4) == 0);
        length=sizeof(address); CHECK(getsockname(listener, (void *)&address, &length)==0);
        int client=socket(mapped?AF_INET:AF_INET6, SOCK_STREAM|SOCK_NONBLOCK, 0); CHECK(client>=0);
        struct sockaddr_in v4={.sin_family=AF_INET, .sin_port=address.sin6_port, .sin_addr={htonl(INADDR_LOOPBACK)}};
        address.sin6_addr=in6addr_loopback;
        CHECK(connect(client, mapped?(void *)&v4:(void *)&address, mapped?sizeof(v4):sizeof(address))==-1 && errno==EINPROGRESS);
        struct pollfd ready={.fd=client, .events=POLLOUT}; CHECK(poll(&ready, 1, 2000)==1 && (ready.revents&POLLOUT));
        length=sizeof(peer); int accepted=accept(listener, (void *)&peer, &length); CHECK(accepted>=0);
        CHECK(length==sizeof(peer) && peer.sin6_family==AF_INET6);
        CHECK(mapped?IN6_IS_ADDR_V4MAPPED(&peer.sin6_addr):IN6_IS_ADDR_LOOPBACK(&peer.sin6_addr));
        CHECK(write(client, "stream", 6)==6 && read(accepted, data, sizeof(data))==6 && !memcmp(data,"stream",6));
        CHECK(close(client)==0 && close(accepted)==0 && close(listener)==0);
    }
    puts("NETWORK PASS addresses: IPv6 UDP/TCP and mapped IPv4 dual-stack accept");
}
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    addresses();
    puts("NETWORK PASS contract");
    return 0;
}
