#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/tcp.h>
#include <limits.h>
#include <sys/time.h>
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
static int get_int(int fd, int level, int option)
{
    int value=-99; socklen_t size=sizeof(value);
    CHECK(getsockopt(fd,level,option,&value,&size)==0 && size==sizeof(value));
    return value;
}
static void options(void)
{
    int socket6=socket(AF_INET6,SOCK_STREAM,0); CHECK(socket6>=0);
    CHECK(get_int(socket6,SOL_SOCKET,SO_TYPE)==SOCK_STREAM);
    CHECK(get_int(socket6,SOL_SOCKET,SO_ACCEPTCONN)==0);
    CHECK(get_int(socket6,IPPROTO_IPV6,IPV6_V6ONLY)==0);
    int one=1, size=16384;
    CHECK(setsockopt(socket6,IPPROTO_IPV6,IPV6_V6ONLY,&one,sizeof(one))==0);
    CHECK(get_int(socket6,IPPROTO_IPV6,IPV6_V6ONLY)==1);
    CHECK(setsockopt(socket6,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one))==0);
    CHECK(get_int(socket6,SOL_SOCKET,SO_REUSEADDR)==1);
    CHECK(setsockopt(socket6,SOL_SOCKET,SO_KEEPALIVE,&one,sizeof(one))==0);
    CHECK(get_int(socket6,SOL_SOCKET,SO_KEEPALIVE)==1);
    CHECK(setsockopt(socket6,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one))==0);
    CHECK(get_int(socket6,IPPROTO_TCP,TCP_NODELAY)==1);
    CHECK(get_int(socket6,IPPROTO_TCP,TCP_MAXSEG)>0);
    for(unsigned option=SO_SNDBUF;option<=SO_RCVBUF;option++) {
        CHECK(setsockopt(socket6,SOL_SOCKET,option,&size,sizeof(size))==0);
        CHECK(get_int(socket6,SOL_SOCKET,option)==32768);
        int zero=0; CHECK(setsockopt(socket6,SOL_SOCKET,option,&zero,sizeof(zero))==0);
        printf("NETWORK buffer minimum option=%u value=%d\n",option,get_int(socket6,SOL_SOCKET,option));
    }
    struct timeval timeout={0,20000}, actual={0}; socklen_t length=sizeof(actual);
    CHECK(setsockopt(socket6,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout))==0);
    CHECK(getsockopt(socket6,SOL_SOCKET,SO_SNDTIMEO,&actual,&length)==0 && actual.tv_sec==0 && actual.tv_usec==20000);
    struct sockaddr_in6 a6={.sin6_family=AF_INET6};
    CHECK(bind(socket6,(void *)&a6,sizeof(a6))==0);
    CHECK(setsockopt(socket6,IPPROTO_IPV6,IPV6_V6ONLY,&one,sizeof(one))==-1 && errno==EINVAL);
    length=sizeof(a6);CHECK(getsockname(socket6,(void *)&a6,&length)==0);
    int socket4=socket(AF_INET,SOCK_STREAM,0);CHECK(socket4>=0);
    struct sockaddr_in a4={.sin_family=AF_INET,.sin_port=a6.sin6_port};
    CHECK(bind(socket4,(void *)&a4,sizeof(a4))==0);
    CHECK(listen(socket6,4)==0 && get_int(socket6,SOL_SOCKET,SO_ACCEPTCONN)==1);
    CHECK(close(socket6)==0 && close(socket4)==0);
    socket6=socket(AF_INET6,SOCK_STREAM,0);socket4=socket(AF_INET,SOCK_STREAM,0);CHECK(socket6>=0 && socket4>=0);
    a6.sin6_port=0;CHECK(bind(socket6,(void *)&a6,sizeof(a6))==0);
    length=sizeof(a6);CHECK(getsockname(socket6,(void *)&a6,&length)==0);a4.sin_port=a6.sin6_port;
    CHECK(bind(socket4,(void *)&a4,sizeof(a4))==-1 && errno==EADDRINUSE);
    CHECK(close(socket6)==0 && close(socket4)==0);
    socket6=socket(AF_INET6,SOCK_STREAM,0);int other6=socket(AF_INET6,SOCK_STREAM,0);CHECK(socket6>=0 && other6>=0);
    a6.sin6_addr=in6addr_any;a6.sin6_port=0;CHECK(bind(socket6,(void *)&a6,sizeof(a6))==0);
    length=sizeof(a6);CHECK(getsockname(socket6,(void *)&a6,&length)==0);a6.sin6_addr=in6addr_loopback;
    CHECK(bind(other6,(void *)&a6,sizeof(a6))==-1 && errno==EADDRINUSE);
    CHECK(close(socket6)==0 && close(other6)==0);
    int refused=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK,0);CHECK(refused>=0);
    int unused=socket(AF_INET,SOCK_STREAM,0);CHECK(unused>=0);a4.sin_port=0;a4.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    CHECK(bind(unused,(void *)&a4,sizeof(a4))==0);length=sizeof(a4);CHECK(getsockname(unused,(void *)&a4,&length)==0);
    CHECK(connect(refused,(void *)&a4,sizeof(a4))==-1 && errno==EINPROGRESS);
    struct pollfd ready={.fd=refused,.events=POLLOUT};CHECK(poll(&ready,1,2000)==1 && (ready.revents&POLLERR));
    CHECK(get_int(refused,SOL_SOCKET,SO_ERROR)==ECONNREFUSED);
    CHECK(get_int(refused,SOL_SOCKET,SO_ERROR)==0);
    CHECK(close(refused)==0 && close(unused)==0);
    int udp=socket(AF_INET,SOCK_DGRAM|SOCK_NONBLOCK,0);CHECK(udp>=0);
    a4.sin_addr.s_addr=htonl(INADDR_LOOPBACK);a4.sin_port=0;
    CHECK(bind(udp,(void *)&a4,sizeof(a4))==0);length=sizeof(a4);CHECK(getsockname(udp,(void *)&a4,&length)==0);
    CHECK(connect(udp,(void *)&a4,sizeof(a4))==0);
    struct sockaddr_in peer;length=sizeof(peer);CHECK(getpeername(udp,(void *)&peer,&length)==0 && peer.sin_port==a4.sin_port);
    struct sockaddr disconnect={.sa_family=AF_UNSPEC};CHECK(connect(udp,&disconnect,sizeof(disconnect))==0);
    CHECK(getpeername(udp,(void *)&peer,&length)==-1 && errno==ENOTCONN);
    CHECK(close(udp)==0);
    puts("NETWORK PASS options: real options, V6ONLY port isolation and UDP peer");
}
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    addresses();
    options();
    puts("NETWORK PASS contract");
    return 0;
}
