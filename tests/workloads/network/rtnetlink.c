#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <net/route.h>
#include <sys/ioctl.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"RTNETLINK FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
static unsigned char buffer[16384];
static int open_route(void)
{
    int fd=socket(AF_NETLINK,SOCK_RAW|SOCK_CLOEXEC,NETLINK_ROUTE);CHECK(fd>=0);
    struct sockaddr_nl a={.nl_family=AF_NETLINK};CHECK(!bind(fd,(void *)&a,sizeof(a)));
    socklen_t size=sizeof(a);CHECK(!getsockname(fd,(void *)&a,&size) && a.nl_pid && !a.nl_groups);
    return fd;
}
static void request(int fd,unsigned type,unsigned flags,unsigned sequence,unsigned bytes,int index)
{
    struct {struct nlmsghdr h;struct ifinfomsg info;} m={{.nlmsg_len=16+bytes,.nlmsg_type=type,.nlmsg_flags=flags,.nlmsg_seq=sequence},{.ifi_index=index}};
    struct sockaddr_nl kernel={.nl_family=AF_NETLINK};
    CHECK(sendto(fd,&m,m.h.nlmsg_len,0,(void *)&kernel,sizeof(kernel))==(ssize_t)m.h.nlmsg_len);
}
static unsigned receive(int fd,unsigned sequence,unsigned type,int dump)
{
    struct sockaddr_nl source={0};struct iovec iov={buffer,sizeof(buffer)};
    struct msghdr msg={.msg_name=&source,.msg_namelen=sizeof(source),.msg_iov=&iov,.msg_iovlen=1};
    ssize_t n=recvmsg(fd,&msg,0);CHECK(n>0 && !(msg.msg_flags&MSG_TRUNC) && !source.nl_pid);
    unsigned count=0,done=0;
    for(struct nlmsghdr *h=(void *)buffer;NLMSG_OK(h,n);h=NLMSG_NEXT(h,n)) {
        CHECK(h->nlmsg_seq==sequence);
        if(h->nlmsg_type==NLMSG_DONE) {CHECK(dump);done++;continue;}
        CHECK(h->nlmsg_type==type);count++;
        if(dump)CHECK(h->nlmsg_flags&NLM_F_MULTI);
    }
    CHECK(n==0 && (!dump || done==1));return count;
}
static void setup_default(void)
{
    int fd=socket(AF_INET,SOCK_DGRAM,0);CHECK(fd>=0);
    struct ifreq ifr={0};strcpy(ifr.ifr_name,"eth0");
    if(ioctl(fd,SIOCGIFADDR,&ifr)) {CHECK(errno==ENODEV);close(fd);return;}
    struct rtentry route={0};route.rt_dst.sa_family=AF_INET;route.rt_genmask.sa_family=AF_INET;
    struct sockaddr_in *gateway=(void *)&route.rt_gateway;gateway->sin_family=AF_INET;
    gateway->sin_addr.s_addr=htonl((ntohl(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr)&0xffffff00U)|1U);
    route.rt_flags=RTF_UP|RTF_GATEWAY;CHECK(!ioctl(fd,SIOCADDRT,&route));close(fd);
    puts("RTNETLINK configured real default via ioctl PASS");
}
static uint32_t route_attr(unsigned type)
{
    struct nlmsghdr *h=(void *)buffer;struct rtmsg *r=NLMSG_DATA(h);
    int n=RTM_PAYLOAD(h);
    for(struct rtattr *a=RTM_RTA(r);RTA_OK(a,n);a=RTA_NEXT(a,n))
        if(a->rta_type==type){uint32_t v;CHECK(RTA_PAYLOAD(a)==4);memcpy(&v,RTA_DATA(a),4);return v;}
    return 0;
}
static void lookup(int fd,const char *destination,unsigned selector,uint32_t value,int error)
{
    struct {struct nlmsghdr h;struct rtmsg rt;struct {uint16_t len,type;uint32_t value;} a[2];} q={
        .h={.nlmsg_len=36,.nlmsg_type=RTM_GETROUTE,.nlmsg_flags=NLM_F_REQUEST,.nlmsg_seq=40},
        .rt={.rtm_family=AF_INET,.rtm_dst_len=32,.rtm_flags=RTM_F_LOOKUP_TABLE},.a={{8,RTA_DST,0},{8,selector,value}}};
    CHECK(inet_pton(AF_INET,destination,&q.a[0].value)==1);
    if(selector)q.h.nlmsg_len+=8;
    if(selector==RTA_SRC)q.rt.rtm_src_len=32;
    struct sockaddr_nl kernel={.nl_family=AF_NETLINK};
    CHECK(sendto(fd,&q,q.h.nlmsg_len,0,(void *)&kernel,sizeof(kernel))==q.h.nlmsg_len);
    CHECK(receive(fd,40,error?NLMSG_ERROR:RTM_NEWROUTE,0)==1);
    if(error)CHECK(((struct nlmsgerr *)(buffer+16))->error==-error);
    else {CHECK(route_attr(RTA_DST)==q.a[0].value && route_attr(RTA_OIF));CHECK(!(((struct nlmsghdr *)buffer)->nlmsg_flags&NLM_F_MULTI));}
}
static void point_contract(void)
{
    int fd=open_route();lookup(fd,"127.0.0.1",0,0,0);
    CHECK(((struct rtmsg *)(buffer+16))->rtm_type==RTN_LOCAL && route_attr(RTA_TABLE)==RT_TABLE_LOCAL);
    lookup(fd,"127.0.0.2",0,0,0);CHECK(route_attr(RTA_PREFSRC)==htonl(0x7f000001));
    lookup(fd,"127.0.0.1",RTA_OIF,99999,ENODEV);
    lookup(fd,"127.0.0.1",RTA_IIF,1,EOPNOTSUPP);
    lookup(fd,"127.0.0.1",RTA_MARK,1,EOPNOTSUPP);
    lookup(fd,"127.0.0.1",RTA_SRC,htonl(0xc0000201),ENETUNREACH);
    lookup(fd,"127.0.0.1",RTA_SRC,htonl(0xe0000001),EINVAL);
    int control=socket(AF_INET,SOCK_DGRAM,0);CHECK(control>=0);
    struct ifreq ifr={0};strcpy(ifr.ifr_name,"eth0");
    if(ioctl(control,SIOCGIFADDR,&ifr))lookup(fd,"192.0.2.1",0,0,ENETUNREACH);
    else {
        uint32_t local=((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr;
        char text[INET_ADDRSTRLEN];CHECK(inet_ntop(AF_INET,&local,text,sizeof(text)));lookup(fd,text,0,0,0);
        CHECK(!ioctl(control,SIOCGIFINDEX,&ifr));unsigned eth_index=ifr.ifr_ifindex;
        uint32_t peer=htonl((ntohl(local)&0xffffff00U)|3U);CHECK(peer!=local);
        CHECK(inet_ntop(AF_INET,&peer,text,sizeof(text)));
        lookup(fd,text,0,0,0);
        CHECK(((struct rtmsg *)(buffer+16))->rtm_type==RTN_UNICAST);
        CHECK(route_attr(RTA_OIF)==eth_index && route_attr(RTA_PREFSRC)==local && !route_attr(RTA_GATEWAY));
        lookup(fd,"192.0.2.1",RTA_SRC,htonl(0x7f000001),EINVAL);
        lookup(fd,"192.0.2.1",0,0,0);CHECK(route_attr(RTA_GATEWAY));
        lookup(fd,"192.0.2.1",RTA_SRC,local,0);CHECK(route_attr(RTA_SRC)==local);
        CHECK(!ioctl(control,SIOCGIFINDEX,&ifr));lookup(fd,"192.0.2.1",RTA_OIF,ifr.ifr_ifindex,0);
        CHECK(route_attr(RTA_OIF)==(unsigned)ifr.ifr_ifindex);
        unsigned index=ifr.ifr_ifindex;
        CHECK(!ioctl(control,SIOCGIFFLAGS,&ifr));short saved_flags=ifr.ifr_flags;
        ifr.ifr_flags&=~IFF_UP;CHECK(!ioctl(control,SIOCSIFFLAGS,&ifr));
        lookup(fd,"192.0.2.1",0,0,ENETUNREACH);
        ifr.ifr_flags=saved_flags;CHECK(!ioctl(control,SIOCSIFFLAGS,&ifr));
        struct rtentry route={0};route.rt_dst.sa_family=AF_INET;route.rt_genmask.sa_family=AF_INET;
        struct sockaddr_in *gateway=(void *)&route.rt_gateway;gateway->sin_family=AF_INET;
        gateway->sin_addr.s_addr=htonl((ntohl(local)&0xffffff00U)|1U);route.rt_flags=RTF_UP|RTF_GATEWAY;
        CHECK(!ioctl(control,SIOCDELRT,&route));
        lookup(fd,"192.0.2.1",0,0,ENETUNREACH);
        lookup(fd,"192.0.2.1",RTA_OIF,index,ENETUNREACH);
        CHECK(!ioctl(control,SIOCADDRT,&route));
        lookup(fd,"192.0.2.1",RTA_SRC,htonl(0x7f000001),EINVAL);
        lookup(fd,"192.0.2.1",0,0,0);CHECK(route_attr(RTA_GATEWAY));

    }
    close(control);close(fd);puts("RTNETLINK point lookup PASS");
}
static void contract(void)
{
    int fd=open_route();
    struct sockaddr_nl a={0};socklen_t z=sizeof(a);CHECK(!getsockname(fd,(void *)&a,&z));
    int duplicate=socket(AF_NETLINK,SOCK_DGRAM,NETLINK_ROUTE);CHECK(duplicate>=0);
    CHECK(bind(duplicate,(void *)&a,sizeof(a))==-1 && errno==EADDRINUSE);close(duplicate);
    unsigned types[]={RTM_GETLINK,RTM_GETADDR,RTM_GETROUTE},sizes[]={16,8,12},replies[]={RTM_NEWLINK,RTM_NEWADDR,RTM_NEWROUTE};
    for(unsigned i=0;i<3;i++) {request(fd,types[i],NLM_F_REQUEST|NLM_F_DUMP,10+i,sizes[i],0);CHECK(receive(fd,10+i,replies[i],1)>0);}
    request(fd,RTM_GETLINK,NLM_F_REQUEST,20,16,1);CHECK(receive(fd,20,RTM_NEWLINK,0)==1);
    request(fd,RTM_GETLINK,NLM_F_REQUEST,21,16,0);CHECK(receive(fd,21,NLMSG_ERROR,0)==1);CHECK(((struct nlmsgerr *)(buffer+16))->error==-EINVAL);
    request(fd,RTM_GETADDR,NLM_F_REQUEST,22,8,0);CHECK(receive(fd,22,NLMSG_ERROR,0)==1);CHECK(((struct nlmsgerr *)(buffer+16))->error==-EOPNOTSUPP);
    request(fd,RTM_GETROUTE,NLM_F_REQUEST,23,12,0);CHECK(receive(fd,23,NLMSG_ERROR,0)==1);CHECK(((struct nlmsgerr *)(buffer+16))->error==-EOPNOTSUPP);
    request(fd,RTM_NEWLINK,NLM_F_REQUEST,24,16,1);CHECK(receive(fd,24,NLMSG_ERROR,0)==1);CHECK(((struct nlmsgerr *)(buffer+16))->error==-EOPNOTSUPP);
    request(fd,RTM_GETLINK,NLM_F_REQUEST|NLM_F_DUMP,25,1,0);
    struct iovec iov={buffer,1};struct msghdr m={.msg_iov=&iov,.msg_iovlen=1};
    CHECK(recvmsg(fd,&m,MSG_PEEK|MSG_TRUNC)>1 && (m.msg_flags&MSG_TRUNC));
    CHECK(receive(fd,25,RTM_NEWLINK,1)>0);
    request(fd,RTM_GETLINK,NLM_F_REQUEST|NLM_F_DUMP,26,16,0);m.msg_flags=0;
    CHECK(recvmsg(fd,&m,0)==1 && (m.msg_flags&MSG_TRUNC));
    CHECK(recv(fd,buffer,sizeof(buffer),MSG_DONTWAIT)==-1 && errno==EAGAIN);
    close(fd);puts("RTNETLINK contract PASS");
}
static void concurrent(void)
{
    int fd=open_route();pid_t children[4];
    for(unsigned i=0;i<4;i++) {children[i]=fork();CHECK(children[i]>=0);if(!children[i]) {request(fd,RTM_GETLINK,NLM_F_REQUEST|NLM_F_DUMP,100+i,16,0);_exit(0);}}
    for(unsigned i=0;i<4;i++){int status;CHECK(waitpid(children[i],&status,0)==children[i] && WIFEXITED(status) && !WEXITSTATUS(status));}
    unsigned seen=0;
    for(unsigned i=0;i<4;i++) {
        ssize_t n=recv(fd,buffer,sizeof(buffer),MSG_PEEK);CHECK(n>=16);
        unsigned seq=((struct nlmsghdr *)buffer)->nlmsg_seq;CHECK(seq>=100 && seq<104 && !(seen&(1U<<(seq-100))));seen|=1U<<(seq-100);
        CHECK(receive(fd,seq,RTM_NEWLINK,1)>0);
    }
    close(fd);puts("RTNETLINK concurrent shared-socket snapshots PASS");
}
static void iproute(void)
{
    if(access("/ip",X_OK))return;
    const char *commands[][7]={{"ip","link","show",NULL},{"ip","address","show",NULL},{"ip","route","show","table","all"},{"ip","-6","address","show",NULL},{"ip","-6","route","show",NULL},{"ip","route","get","127.0.0.1",NULL},{"ip","route","get","192.0.2.1",NULL},{"ip","route","get","192.0.2.1","oif","eth0",NULL}};
    int control=socket(AF_INET,SOCK_DGRAM,0);CHECK(control>=0);
    struct ifreq ifr={0};strcpy(ifr.ifr_name,"eth0");int network=!ioctl(control,SIOCGIFINDEX,&ifr);close(control);
    for(unsigned i=0;i<(network?8U:6U);i++) {
        char *args[8]={0};for(unsigned j=0;j<7;j++)args[j]=(char *)commands[i][j];
        pid_t p=fork();CHECK(p>=0);if(!p){execv("/ip",args);_exit(127);}
        int status;CHECK(waitpid(p,&status,0)==p && WIFEXITED(status) && !WEXITSTATUS(status));
    }
    puts("RTNETLINK iproute2 PASS");
}
int main(void)
{setvbuf(stdout,NULL,_IONBF,0);setup_default();contract();point_contract();concurrent();iproute();puts("RTNETLINK ALL PASS");sync();reboot(RB_POWER_OFF);return 0;}
