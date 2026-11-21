#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr,"ipv4 multicast line %d: %s (errno=%d)\n",__LINE__,#expr,errno); \
    return 1; \
} } while (0)
#define FAIL(error,expr) do { errno=0; CHECK((expr)==-1 && errno==(error)); } while (0)

int main(int argc, char **argv)
{
    (void)argv;
    int forced_required=argc>1;
    unsigned loop=if_nametoindex("lo");
    CHECK(loop && loop<=INT_MAX);
    int a=socket(AF_INET,SOCK_DGRAM|SOCK_NONBLOCK,0);
    int b=socket(AF_INET,SOCK_DGRAM|SOCK_NONBLOCK,0);
    int sender=socket(AF_INET,SOCK_DGRAM,0);
    int tcp=socket(AF_INET,SOCK_STREAM,0);
    CHECK(a>=0 && b>=0 && sender>=0 && tcp>=0);
    int set=1, clear=0;
    CHECK(!setsockopt(a,SOL_SOCKET,SO_REUSEADDR,&set,sizeof(set)));
    CHECK(!setsockopt(b,SOL_SOCKET,SO_REUSEADDR,&set,sizeof(set)));
    struct sockaddr_in address={.sin_family=AF_INET};
    CHECK(!bind(a,(void *)&address,sizeof(address)));
    socklen_t length=sizeof(address);
    CHECK(!getsockname(a,(void *)&address,&length) && address.sin_port);
    CHECK(!bind(b,(void *)&address,sizeof(address)));
    address.sin_addr.s_addr=htonl(0xeffe0102);
    struct group_req req={.gr_interface=loop};
    *(struct sockaddr_in *)&req.gr_group=address;
    struct ip_mreqn indexed={.imr_multiaddr=address.sin_addr,.imr_ifindex=(int)loop};
    struct ip_mreq by_address={.imr_multiaddr=address.sin_addr,
        .imr_interface={htonl(INADDR_LOOPBACK)}};
    CHECK(!setsockopt(sender,SOL_IP,IP_MULTICAST_IF,&indexed,sizeof(indexed)));
    CHECK(!setsockopt(a,SOL_IP,MCAST_JOIN_GROUP,&req,sizeof(req)));
    FAIL(EADDRINUSE,setsockopt(a,SOL_IP,IP_ADD_MEMBERSHIP,&indexed,sizeof(indexed)));
    CHECK(!setsockopt(b,SOL_IP,IP_MULTICAST_ALL,&clear,sizeof(clear)));
    CHECK(sendto(sender,"a",1,0,(void *)&address,sizeof(address))==1);
    char got;
    CHECK(recv(a,&got,1,0)==1 && got=='a');
    FAIL(EAGAIN,recv(a,&got,1,0));
    FAIL(EAGAIN,recv(b,&got,1,0));
    CHECK(!setsockopt(b,SOL_IP,IP_ADD_MEMBERSHIP,&by_address,sizeof(by_address)));
    CHECK(!setsockopt(a,SOL_IP,IP_DROP_MEMBERSHIP,&indexed,sizeof(indexed)));
    CHECK(!setsockopt(a,SOL_IP,IP_MULTICAST_ALL,&clear,sizeof(clear)));
    CHECK(!setsockopt(sender,SOL_IP,IP_MULTICAST_TTL,&clear,sizeof(clear)));
    CHECK(!setsockopt(sender,SOL_IP,IP_MULTICAST_LOOP,&clear,sizeof(clear)));
    CHECK(sendto(sender,"b",1,0,(void *)&address,sizeof(address))==1);
    CHECK(recv(b,&got,1,0)==1 && got=='b');
    FAIL(EAGAIN,recv(a,&got,1,0));
    CHECK(!setsockopt(b,SOL_IP,MCAST_LEAVE_GROUP,&req,sizeof(req)));
    FAIL(EADDRNOTAVAIL,setsockopt(b,SOL_IP,MCAST_LEAVE_GROUP,&req,sizeof(req)));
    FAIL(EPROTO,setsockopt(tcp,SOL_IP,IP_ADD_MEMBERSHIP,&indexed,sizeof(indexed)));
    CHECK(!setsockopt(tcp,SOL_IP,MCAST_JOIN_GROUP,&req,sizeof(req)));
    CHECK(!setsockopt(tcp,SOL_IP,MCAST_LEAVE_GROUP,&req,sizeof(req)));

    int forced=1<<20;
    int result=setsockopt(sender,SOL_SOCKET,SO_SNDBUFFORCE,&forced,sizeof(forced));
    if (!result) {
        int actual;
        length=sizeof(actual);
        CHECK(!getsockopt(sender,SOL_SOCKET,SO_SNDBUF,&actual,&length));
        CHECK(actual==forced*2);
        forced=INT_MAX;
        CHECK(!setsockopt(sender,SOL_SOCKET,SO_SNDBUFFORCE,&forced,sizeof(forced)));
        CHECK(!getsockopt(sender,SOL_SOCKET,SO_SNDBUF,&actual,&length));
        CHECK(actual==INT_MAX-1);
        forced=-1;
        CHECK(!setsockopt(sender,SOL_SOCKET,SO_SNDBUFFORCE,&forced,sizeof(forced)));
        CHECK(!getsockopt(sender,SOL_SOCKET,SO_SNDBUF,&actual,&length));
        CHECK(actual>0 && actual<65536);
        pid_t child=fork();
        CHECK(child>=0);
        if(!child) {
            CHECK(!setgid(65534) && !setuid(65534));
            FAIL(EPERM,setsockopt(sender,SOL_SOCKET,SO_SNDBUFFORCE,&forced,sizeof(forced)));
            return 0;
        }
        int status;
        CHECK(waitpid(child,&status,0)==child && status==0);
    } else CHECK(!forced_required && errno==EPERM);
    CHECK(!close(a) && !close(b) && !close(sender) && !close(tcp));
    printf("IPV4 MULTICAST PASS udp delivery, both membership forms, TCP option policy, force=%s\n",
        result?"EPERM":"privileged/unprivileged");
    return 0;
}
