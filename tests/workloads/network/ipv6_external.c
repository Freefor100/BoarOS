#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/tcp.h>
#include <ifaddrs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#define CHECK(x) do { if(!(x)) {printf("IPV6 EXTERNAL FAIL %d %s errno=%d\n",__LINE__,#x,errno);exit(1);} } while(0)
static unsigned char data[65536], reply[65536];
static int endpoint(int type)
{
    int fd=socket(AF_INET6,type,0);CHECK(fd>=0);
    struct timeval timeout={10,0};
    CHECK(!setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)));
    CHECK(!setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout)));
    return fd;
}
#ifdef IPV6_STATIC_GLOBAL
struct in6_request {struct in6_addr address;unsigned prefix;int index;};
struct in6_route {struct in6_addr destination,source,gateway;unsigned type;unsigned short dst_len,src_len;
    unsigned metric;unsigned long info;unsigned flags;int index;};
static void configure_global(int fd,unsigned index,unsigned prefix,int remove)
{
    struct in6_request request={.prefix=prefix,.index=(int)index};
    CHECK(inet_pton(AF_INET6,"2001:db8:1::100",&request.address)==1);
    CHECK(!ioctl(fd,remove?0x8936:SIOCSIFADDR,&request));
    if(!remove) {
        CHECK(ioctl(fd,SIOCSIFADDR,&request)==-1 && errno==EEXIST);
        request.index+=256;CHECK(ioctl(fd,SIOCSIFADDR,&request)==-1 && errno==ENODEV);
        request.index-=256;request.prefix=129;CHECK(ioctl(fd,SIOCSIFADDR,&request)==-1 && errno==EINVAL);
    }
}
static void global_gateway(int fd,unsigned index,int remove)
{
    struct in6_route route={.flags=3,.index=(int)index};
    CHECK(inet_pton(AF_INET6,"fe80::2",&route.gateway)==1);
    CHECK(!ioctl(fd,remove?0x890c:0x890b,&route));
    if(!remove) {
        CHECK(ioctl(fd,0x890b,&route)==-1 && errno==EEXIST);
        route.dst_len=64;CHECK(ioctl(fd,0x890b,&route)==-1 && errno==EOPNOTSUPP);
    }
}
#endif
int main(void)
{
    setbuf(stdout,NULL);
    int fd=endpoint(SOCK_DGRAM);
    struct ifreq ifr={0};strcpy(ifr.ifr_name,"eth0");
    CHECK(!ioctl(fd,SIOCGIFINDEX,&ifr));unsigned index=ifr.ifr_ifindex;
    struct sockaddr_in6 local={.sin6_family=AF_INET6,.sin6_port=htons(19461),.sin6_scope_id=index};
    CHECK(inet_pton(AF_INET6,"fe80::5054:ff:fe12:3456",&local.sin6_addr)==1);
#ifdef IPV6_STATIC_GLOBAL
    long page=sysconf(_SC_PAGESIZE);CHECK(page>0);
    unsigned char *fault=mmap(NULL,(size_t)page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(fault!=MAP_FAILED && !munmap(fault+page,(size_t)page));
    CHECK(ioctl(fd,SIOCSIFADDR,fault+page-8)==-1 && errno==EFAULT);
    CHECK(ioctl(fd,0x890b,fault+page-8)==-1 && errno==EFAULT);
    CHECK(!seteuid(65534));
    CHECK(ioctl(fd,SIOCSIFADDR,fault+page-8)==-1 && errno==EPERM);
    CHECK(ioctl(fd,0x890b,fault+page-8)==-1 && errno==EPERM);
    CHECK(!seteuid(0) && !munmap(fault,(size_t)page));
    configure_global(fd,index,64,0);
    local.sin6_scope_id=0;CHECK(inet_pton(AF_INET6,"2001:db8:1::100",&local.sin6_addr)==1);
#endif
    int ready=0;
    for(unsigned i=0;i<60;i++) {
        if(!bind(fd,(void *)&local,sizeof(local))) {ready=1;break;}
        CHECK(errno==EADDRNOTAVAIL);usleep(100000);
    }
    CHECK(ready);puts("IPV6 EXTERNAL DAD READY");
    struct ifaddrs *interfaces=NULL;CHECK(!getifaddrs(&interfaces));unsigned found=0;
    for(struct ifaddrs *i=interfaces;i;i=i->ifa_next)
        if(i->ifa_addr && i->ifa_addr->sa_family==AF_INET6 && !strcmp(i->ifa_name,"eth0")) {
            struct sockaddr_in6 *a=(void *)i->ifa_addr;
#ifdef IPV6_STATIC_GLOBAL
            if(!memcmp(&a->sin6_addr,&local.sin6_addr,16)){CHECK(!a->sin6_scope_id);found++;}
            else CHECK(a->sin6_scope_id==index && IN6_IS_ADDR_LINKLOCAL(&a->sin6_addr));
#else
            CHECK(a->sin6_scope_id==index && !memcmp(&a->sin6_addr,&local.sin6_addr,16));found++;
#endif
        }
    freeifaddrs(interfaces);CHECK(found==1);puts("IPV6 EXTERNAL ADDRESS ENUM PASS");
    struct sockaddr_in6 peer={.sin6_family=AF_INET6,.sin6_port=htons(19460),.sin6_scope_id=index};
    CHECK(inet_pton(AF_INET6,"fe80::2",&peer.sin6_addr)==1);
#ifdef IPV6_STATIC_GLOBAL
    peer.sin6_scope_id=0;CHECK(inet_pton(AF_INET6,"2001:db8:1::2",&peer.sin6_addr)==1);
#endif
    for(unsigned i=0;i<sizeof(data);i++)data[i]=(unsigned char)(i*31U+i/251U);
    const unsigned lengths[]={64,1232,1452};
    for(unsigned i=0;i<3;i++) {
        unsigned n=lengths[i];
        CHECK(sendto(fd,data,n,0,(void *)&peer,sizeof(peer))==(int)n);
        struct sockaddr_in6 source={0};socklen_t size=sizeof(source);
        CHECK(recvfrom(fd,reply,sizeof(reply),0,(void *)&source,&size)==(int)n);
        CHECK(!memcmp(data,reply,n) && size==sizeof(source) && source.sin6_scope_id==peer.sin6_scope_id &&
              source.sin6_port==peer.sin6_port && !memcmp(&source.sin6_addr,&peer.sin6_addr,16));
    }
    CHECK(sendto(fd,data,1453,0,(void *)&peer,sizeof(peer))==-1 && errno==EMSGSIZE);
    close(fd);puts("IPV6 EXTERNAL UDP CONTENT PASS");
#ifdef IPV6_STATIC_GLOBAL
    fd=endpoint(SOCK_DGRAM);configure_global(fd,index,64,1);configure_global(fd,index,128,0);
    ready=0;
    for(unsigned i=0;i<60;i++) {
        if(!bind(fd,(void *)&local,sizeof(local))){ready=1;break;}
        CHECK(errno==EADDRNOTAVAIL);usleep(100000);
    }
    CHECK(ready);
    CHECK(sendto(fd,data,64,0,(void *)&peer,sizeof(peer))==-1 && errno==ENETUNREACH);
    global_gateway(fd,index,0);close(fd);
    puts("IPV6 EXTERNAL STATIC DEFAULT READY");
#endif
    fd=endpoint(SOCK_STREAM);CHECK(!connect(fd,(void *)&peer,sizeof(peer)));
#ifdef IPV6_STATIC_GLOBAL
    int listener=endpoint(SOCK_STREAM);struct sockaddr_in6 listen_address=local;listen_address.sin6_port=htons(19462);
    CHECK(!bind(listener,(void *)&listen_address,sizeof(listen_address)) && !listen(listener,1));
    struct sockaddr_in6 before={0},after={0};socklen_t name_size=sizeof(before);
    CHECK(!getsockname(fd,(void *)&before,&name_size));
    configure_global(fd,index,128,1);
    name_size=sizeof(after);CHECK(!getsockname(fd,(void *)&after,&name_size) && !memcmp(&before,&after,sizeof(before)));
    unsigned char tcp_state[256]={0};socklen_t state_size=sizeof(tcp_state);
    CHECK(!getsockopt(fd,IPPROTO_TCP,TCP_INFO,tcp_state,&state_size) && tcp_state[0]==1);
    int accepting=0;socklen_t option_size=sizeof(accepting);
    CHECK(!getsockopt(listener,SOL_SOCKET,SO_ACCEPTCONN,&accepting,&option_size) && accepting==1);
    configure_global(fd,index,128,0);
    int probe=endpoint(SOCK_DGRAM);ready=0;
    for(unsigned i=0;i<60;i++) {
        if(!bind(probe,(void *)&local,sizeof(local))){ready=1;break;}
        CHECK(errno==EADDRNOTAVAIL);usleep(100000);
    }
    CHECK(ready);close(probe);close(listener);
    puts("IPV6 EXTERNAL TCP ADDRESS READD PASS");
#endif
    for(unsigned at=0;at<sizeof(data);) {ssize_t n=send(fd,data+at,sizeof(data)-at,0);CHECK(n>0);at+=n;}
    CHECK(!shutdown(fd,SHUT_WR));
    for(unsigned at=0;at<sizeof(reply);) {ssize_t n=recv(fd,reply+at,sizeof(reply)-at,0);CHECK(n>0);at+=n;}
    CHECK(!memcmp(data,reply,sizeof(data)));CHECK(recv(fd,reply,1,0)==0);close(fd);
    puts("IPV6 EXTERNAL TCP CONTENT PASS");
#ifdef IPV6_STATIC_GLOBAL
    fd=endpoint(SOCK_DGRAM);global_gateway(fd,index,1);
    CHECK(sendto(fd,data,64,0,(void *)&peer,sizeof(peer))==-1 && errno==ENETUNREACH);
    configure_global(fd,index,128,1);close(fd);puts("IPV6 EXTERNAL STATIC CLEANUP PASS");
#endif
    puts("IPV6 EXTERNAL ALL PASS");
    if(getpid()==1) {sync();reboot(RB_POWER_OFF);}
    return 0;
}
