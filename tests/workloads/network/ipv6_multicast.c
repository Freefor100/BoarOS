#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#ifndef IPV6_MULTICAST_ALL
#define IPV6_MULTICAST_ALL 29
#endif
#define CHECK(x) do{if(!(x)){printf("IPV6 MULTICAST FAIL %d %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
static struct sockaddr_in6 host;
static int control;
static unsigned index6;
static void scalar(int fd,int option,int value)
{CHECK(!setsockopt(fd,IPPROTO_IPV6,option,&value,sizeof(value)));}
static int endpoint(int receive_group)
{
    int fd=socket(AF_INET6,SOCK_DGRAM,0);CHECK(fd>=0);
    struct timeval timeout={5,0};CHECK(!setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)));
    if(receive_group){int one=1;CHECK(!setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one)));
      struct sockaddr_in6 any={.sin6_family=AF_INET6,.sin6_port=htons(19462)};
      CHECK(!bind(fd,(void *)&any,sizeof(any)));}
    return fd;
}
static void membership(int fd,int remove)
{
    struct ipv6_mreq request={.ipv6mr_interface=index6};
    CHECK(inet_pton(AF_INET6,"ff02::1234",&request.ipv6mr_multiaddr)==1);
    CHECK(!setsockopt(fd,IPPROTO_IPV6,remove?IPV6_LEAVE_GROUP:IPV6_JOIN_GROUP,&request,sizeof(request)));
}
static unsigned char pattern(unsigned stage,unsigned at)
{return (unsigned char)(stage*19U+at*31U+at/7U);}
static void received(int fd,unsigned stage)
{
    unsigned char bytes[300];struct sockaddr_in6 source={0};socklen_t size=sizeof(source);
    CHECK(recvfrom(fd,bytes,sizeof(bytes),0,(void *)&source,&size)==257);
    for(unsigned i=0;i<257;i++)CHECK(bytes[i]==pattern(stage,i));
    struct in6_addr expected;CHECK(inet_pton(AF_INET6,"fe80::2",&expected)==1);
    CHECK(size==sizeof(source) && source.sin6_scope_id==index6 && source.sin6_port==htons(19460) &&
      !memcmp(&source.sin6_addr,&expected,16));
}
static void empty(int fd)
{unsigned char byte;CHECK(recv(fd,&byte,1,MSG_DONTWAIT)==-1 && (errno==EAGAIN || errno==EWOULDBLOCK));}
static void stage(unsigned number)
{
    unsigned char request[5]={'M','L','D',':',(unsigned char)number},reply[5]={0};
    CHECK(sendto(control,request,sizeof(request),0,(void *)&host,sizeof(host))==sizeof(request));
    CHECK(recv(control,reply,sizeof(reply),0)==sizeof(reply) && !memcmp(request,reply,sizeof(reply)));
    /* The peer injects group/query before this unicast response on one
     * Ethernet stream: reaching here is an ingress ordering barrier. */
}
int main(void)
{
    setbuf(stdout,NULL);control=endpoint(0);
    struct ifreq ifr={0};strcpy(ifr.ifr_name,"eth0");CHECK(!ioctl(control,SIOCGIFINDEX,&ifr));index6=ifr.ifr_ifindex;
    struct sockaddr_in6 local={.sin6_family=AF_INET6,.sin6_port=htons(19461),.sin6_scope_id=index6};
    CHECK(inet_pton(AF_INET6,"fe80::5054:ff:fe12:3456",&local.sin6_addr)==1);
    int ready=0;for(unsigned i=0;i<60;i++){
      if(!bind(control,(void *)&local,sizeof(local))){ready=1;break;}
      CHECK(errno==EADDRNOTAVAIL);usleep(100000);}
    CHECK(ready);
    host=(struct sockaddr_in6){.sin6_family=AF_INET6,.sin6_port=htons(19460),.sin6_scope_id=index6};
    CHECK(inet_pton(AF_INET6,"fe80::2",&host.sin6_addr)==1);
    int a=endpoint(1),b=endpoint(1);scalar(a,IPV6_MULTICAST_ALL,0);membership(a,0);
    int all=0;socklen_t length=sizeof(all);CHECK(!getsockopt(b,IPPROTO_IPV6,IPV6_MULTICAST_ALL,&all,&length) && all==1);
    int sender=endpoint(0);scalar(sender,IPV6_MULTICAST_IF,(int)index6);scalar(sender,IPV6_MULTICAST_HOPS,5);scalar(sender,IPV6_MULTICAST_LOOP,0);
    struct sockaddr_in6 group={.sin6_family=AF_INET6,.sin6_port=htons(19462),.sin6_scope_id=index6};
    CHECK(inet_pton(AF_INET6,"ff02::1234",&group.sin6_addr)==1);
    unsigned char outgoing[257];for(unsigned i=0;i<sizeof(outgoing);i++)outgoing[i]=pattern(99,i);
    CHECK(sendto(sender,outgoing,sizeof(outgoing),0,(void *)&group,sizeof(group))==sizeof(outgoing));close(sender);
    stage(1);received(a,1);received(b,1);empty(a);empty(b);
    puts("IPV6 MULTICAST DEFAULT ALL AND EXTERNAL CONTENT PASS");
    scalar(b,IPV6_MULTICAST_ALL,0);stage(2);received(a,2);empty(a);empty(b);
    int c=endpoint(1);scalar(c,IPV6_MULTICAST_ALL,0);membership(c,0);membership(a,1);
    stage(3);received(c,3);empty(c);empty(a);empty(b);
    membership(c,1);close(c);stage(4);empty(a);empty(b);
    membership(a,0);stage(5);received(a,5);empty(a);empty(b);
    close(a);stage(6);empty(b);close(b);close(control);
    puts("IPV6 MULTICAST SHARED MEMBERSHIP LEAVE AND LAST CLOSE PASS");
    puts("IPV6 MULTICAST ALL PASS");
    if(getpid()==1){sync();reboot(RB_POWER_OFF);}return 0;
}
