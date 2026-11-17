#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
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
int main(void)
{
    setbuf(stdout,NULL);
    int fd=endpoint(SOCK_DGRAM);
    struct ifreq ifr={0};strcpy(ifr.ifr_name,"eth0");
    CHECK(!ioctl(fd,SIOCGIFINDEX,&ifr));unsigned index=ifr.ifr_ifindex;
    struct sockaddr_in6 local={.sin6_family=AF_INET6,.sin6_port=htons(19461),.sin6_scope_id=index};
    CHECK(inet_pton(AF_INET6,"fe80::5054:ff:fe12:3456",&local.sin6_addr)==1);
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
            CHECK(a->sin6_scope_id==index && !memcmp(&a->sin6_addr,&local.sin6_addr,16));found++;
        }
    freeifaddrs(interfaces);CHECK(found==1);puts("IPV6 EXTERNAL ADDRESS ENUM PASS");
    struct sockaddr_in6 peer={.sin6_family=AF_INET6,.sin6_port=htons(19460),.sin6_scope_id=index};
    CHECK(inet_pton(AF_INET6,"fe80::2",&peer.sin6_addr)==1);
    for(unsigned i=0;i<sizeof(data);i++)data[i]=(unsigned char)(i*31U+i/251U);
    const unsigned lengths[]={64,1232,1452};
    for(unsigned i=0;i<3;i++) {
        unsigned n=lengths[i];
        CHECK(sendto(fd,data,n,0,(void *)&peer,sizeof(peer))==(int)n);
        struct sockaddr_in6 source={0};socklen_t size=sizeof(source);
        CHECK(recvfrom(fd,reply,sizeof(reply),0,(void *)&source,&size)==(int)n);
        CHECK(!memcmp(data,reply,n) && size==sizeof(source) && source.sin6_scope_id==index &&
              source.sin6_port==peer.sin6_port && !memcmp(&source.sin6_addr,&peer.sin6_addr,16));
    }
    CHECK(sendto(fd,data,1453,0,(void *)&peer,sizeof(peer))==-1 && errno==EMSGSIZE);
    close(fd);puts("IPV6 EXTERNAL UDP CONTENT PASS");
    fd=endpoint(SOCK_STREAM);CHECK(!connect(fd,(void *)&peer,sizeof(peer)));
    for(unsigned at=0;at<sizeof(data);) {ssize_t n=send(fd,data+at,sizeof(data)-at,0);CHECK(n>0);at+=n;}
    CHECK(!shutdown(fd,SHUT_WR));
    for(unsigned at=0;at<sizeof(reply);) {ssize_t n=recv(fd,reply+at,sizeof(reply)-at,0);CHECK(n>0);at+=n;}
    CHECK(!memcmp(data,reply,sizeof(data)));CHECK(recv(fd,reply,1,0)==0);close(fd);
    puts("IPV6 EXTERNAL TCP CONTENT PASS");puts("IPV6 EXTERNAL ALL PASS");
    if(getpid()==1) {sync();reboot(RB_POWER_OFF);}
    return 0;
}
