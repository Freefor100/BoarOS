#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define HOST "10.77.0.1"
#define GUEST "10.77.0.2"
#define CONTROL_PORT 18090
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"IPV4 FAIL line=%d expression=%s errno=%d\n",__LINE__,#x,errno);exit(1); } } while (0)
static unsigned char packet[65536], received[65536];

static struct sockaddr_in address(const char *ip, unsigned port)
{
    struct sockaddr_in result={.sin_family=AF_INET,.sin_port=htons((uint16_t)port)};
    CHECK(inet_pton(AF_INET,ip,&result.sin_addr)==1);return result;
}
static uint16_t checksum(const void *data,size_t size)
{
    const unsigned char *bytes=data;uint32_t value=0;
    while(size>1){value+=((unsigned)bytes[0]<<8)|bytes[1];bytes+=2;size-=2;}
    if(size)value+=(unsigned)*bytes<<8;
    while(value>>16)value=(value&65535)+(value>>16);
    return htons((uint16_t)~value);
}
static void timeout(int fd)
{
    struct timeval value={.tv_sec=8};
    CHECK(!setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&value,sizeof(value)));
}
static void receive_exact(int fd,void *buffer,size_t size)
{
    unsigned char *out=buffer;
    while(size) {
        ssize_t done=recv(fd,out,size,0);
        if(done<0 && errno==EINTR)continue;
        CHECK(done>0 && (size_t)done<=size);
        out+=(size_t)done;size-=(size_t)done;
    }
}
static int raw_socket(const char *ip)
{
    int fd=socket(AF_INET,SOCK_RAW,IPPROTO_ICMP);CHECK(fd>=0);timeout(fd);
    struct sockaddr_in local=address(ip,0);CHECK(!bind(fd,(void *)&local,sizeof(local)));return fd;
}
static int datagram(unsigned port)
{
    int fd=socket(AF_INET,SOCK_DGRAM,0);CHECK(fd>=0);timeout(fd);
    struct sockaddr_in local=address("0.0.0.0",port);CHECK(!bind(fd,(void *)&local,sizeof(local)));return fd;
}
static size_t echo_packet(unsigned id,unsigned sequence,size_t bytes)
{
    CHECK(bytes>=8 && bytes<=65515);memset(packet,0,bytes);
    packet[0]=ICMP_ECHO;packet[4]=(unsigned char)(id>>8);packet[5]=(unsigned char)id;
    packet[6]=(unsigned char)(sequence>>8);packet[7]=(unsigned char)sequence;
    for(size_t i=8;i<bytes;i++)packet[i]=(unsigned char)((i+sequence)%251);
    uint16_t sum=checksum(packet,bytes);memcpy(packet+2,&sum,2);return bytes;
}
static ssize_t echo_receive(int fd,unsigned type,unsigned id,unsigned sequence,size_t bytes)
{
    for(unsigned attempt=0;attempt<32;attempt++) {
        struct sockaddr_in source;socklen_t length=sizeof(source);
        ssize_t size=recvfrom(fd,received,sizeof(received),0,(void *)&source,&length);
        CHECK(size>=28 && length==sizeof(source) && source.sin_family==AF_INET && source.sin_port==0);
        unsigned header=(received[0]&15U)*4U;CHECK(header>=20 && header+8U<=(unsigned)size && received[9]==IPPROTO_ICMP);
        unsigned char *icmp=received+header;
        if(icmp[0]!=type || icmp[4]!=(unsigned char)(id>>8) || icmp[5]!=(unsigned char)id ||
            icmp[6]!=(unsigned char)(sequence>>8) || icmp[7]!=(unsigned char)sequence)continue;
        CHECK((size_t)size==header+bytes && !checksum(icmp,bytes));
        for(size_t i=8;i<bytes;i++)CHECK(icmp[i]==(unsigned char)((i+sequence)%251));
        return size;
    }
    CHECK(!"matching ICMP message missing");return -1;
}
static void configure(int external)
{
    int fd=socket(AF_INET,SOCK_DGRAM,0);CHECK(fd>=0);
    const char *names[]={"lo","eth0"};
    for(unsigned n=0;n<(external?2U:1U);n++) {
        struct ifreq iface={0};strcpy(iface.ifr_name,names[n]);
        if(!n || access("/reference-network-setup",F_OK)==0) {
            struct sockaddr_in local=address(n?GUEST:"127.0.0.1",0);
            memcpy(&iface.ifr_addr,&local,sizeof(local));CHECK(!ioctl(fd,SIOCSIFADDR,&iface));
            local=address(n?"255.255.255.0":"255.0.0.0",0);
            memcpy(&iface.ifr_netmask,&local,sizeof(local));CHECK(!ioctl(fd,SIOCSIFNETMASK,&iface));
        }
        CHECK(!ioctl(fd,SIOCGIFFLAGS,&iface));iface.ifr_flags|=IFF_UP;CHECK(!ioctl(fd,SIOCSIFFLAGS,&iface));
    }
    CHECK(!close(fd));
}
static void select_caller_cpu(void)
{
    cpu_set_t online,selected;CPU_ZERO(&online);CHECK(!sched_getaffinity(0,sizeof(online),&online));
    int count=CPU_COUNT(&online),cpu=-1;CHECK(count>0);
    for(int i=0;i<CPU_SETSIZE;i++)if(CPU_ISSET(i,&online)){cpu=i;if(i)break;}
    CHECK(cpu>=0);
    if(count>1){CPU_ZERO(&selected);CPU_SET(cpu,&selected);CHECK(!sched_setaffinity(0,sizeof(selected),&selected));}
    CHECK(sched_getcpu()==cpu);printf("IPV4 CALLER cpu=%d cpus=%d\n",cpu,count);
}
static void local_messages(void)
{
    int first=raw_socket("127.0.0.1"),second=raw_socket("127.0.0.1");
    struct sockaddr_in target=address("127.0.0.1",0);size_t size=echo_packet(0x4b31,1,64);
    CHECK(sendto(first,packet,size,0,(void *)&target,sizeof(target))==(ssize_t)size);
    struct pollfd ready={.fd=first,.events=POLLIN};CHECK(poll(&ready,1,8000)==1 && (ready.revents&POLLIN));
    unsigned char prefix[12],again[12];
    ssize_t total=recv(first,prefix,sizeof(prefix),MSG_PEEK|MSG_TRUNC);CHECK(total==84);
    CHECK(recv(first,again,sizeof(again),MSG_PEEK)==sizeof(again) && !memcmp(prefix,again,sizeof(prefix)));
    CHECK(echo_receive(first,ICMP_ECHO,0x4b31,1,size)==84);
    CHECK(echo_receive(first,ICMP_ECHOREPLY,0x4b31,1,size)==84);
    CHECK(echo_receive(second,ICMP_ECHOREPLY,0x4b31,1,size)==84);
    puts("IPV4 CASE raw-two-receivers-peek-trunc PASS");
    size=echo_packet(0x4b31,2,4096);
    CHECK(sendto(first,packet,size,0,(void *)&target,sizeof(target))==(ssize_t)size);
    CHECK(echo_receive(first,ICMP_ECHOREPLY,0x4b31,2,size)==4116);
    CHECK(echo_receive(second,ICMP_ECHOREPLY,0x4b31,2,size)==4116);
    CHECK(!close(first) && !close(second));puts("IPV4 CASE raw-large-loopback PASS");
    int sender=datagram(0),receiver=datagram(18093),one=1;
    CHECK(!setsockopt(sender,SOL_SOCKET,SO_BINDTODEVICE,"lo",3));
    target=address("127.255.255.255",18093);
    CHECK(sendto(sender,"local-broadcast",15,0,(void *)&target,sizeof(target))==-1 && errno==EACCES);
    CHECK(!setsockopt(sender,SOL_SOCKET,SO_BROADCAST,&one,sizeof(one)));
    CHECK(sendto(sender,"local-broadcast",15,0,(void *)&target,sizeof(target))==15);
    CHECK(recv(receiver,received,sizeof(received),0)==15 && !memcmp(received,"local-broadcast",15));
    CHECK(!close(sender) && !close(receiver));puts("IPV4 CASE broadcast-permission-receive PASS");
}
static void ready(const char *phase,int control)
{
    printf("IPV4 READY %s\n",phase);CHECK(!fflush(stdout));
    struct sockaddr_in source;socklen_t length=sizeof(source);char token;
    CHECK(recvfrom(control,&token,1,0,(void *)&source,&length)==1 && token=='G' && source.sin_addr.s_addr==inet_addr(HOST));
}
static void complete(const char *phase,int control)
{
    struct sockaddr_in peer=address(HOST,CONTROL_PORT);
    CHECK(sendto(control,phase,strlen(phase),0,(void *)&peer,sizeof(peer))==(ssize_t)strlen(phase));
}
static void host_echo(int control)
{
    int observer=raw_socket(GUEST);ready("host-echo",control);
    CHECK(echo_receive(observer,ICMP_ECHO,0x4b32,3,64)==84);
    char done;CHECK(recv(control,&done,1,0)==1 && done=='H');
    CHECK(!close(observer));puts("IPV4 CASE host-echo-observed-and-replied PASS");complete("host-echo",control);
}
static void external_udp(int control)
{
    int sender=datagram(18091),receiver=datagram(18092),one=1,ttl=37;
    CHECK(!setsockopt(sender,SOL_SOCKET,SO_BINDTODEVICE,"eth0",5));
    CHECK(!setsockopt(sender,IPPROTO_IP,IP_TTL,&ttl,sizeof(ttl)));
    struct sockaddr_in target=address("10.77.0.255",18091);
    ready("udp-out",control);
    CHECK(sendto(sender,"guest-broadcast",15,0,(void *)&target,sizeof(target))==-1 && errno==EACCES);
    CHECK(!setsockopt(sender,SOL_SOCKET,SO_BROADCAST,&one,sizeof(one)));
    CHECK(sendto(sender,"guest-broadcast",15,0,(void *)&target,sizeof(target))==15);
    char ack;CHECK(recv(control,&ack,1,0)==1 && ack=='U');complete("udp-out",control);
    ready("udp-in",control);
    struct sockaddr_in source;socklen_t length=sizeof(source);
    CHECK(recvfrom(receiver,received,sizeof(received),0,(void *)&source,&length)==14 &&
        !memcmp(received,"host-broadcast",14) && source.sin_addr.s_addr==inet_addr(HOST));
    CHECK(!close(sender) && !close(receiver));puts("IPV4 CASE udp-wire-broadcast PASS");complete("udp-in",control);
}
static void external_raw(int control)
{
    int fd=raw_socket(GUEST),ttl=37;CHECK(!setsockopt(fd,IPPROTO_IP,IP_TTL,&ttl,sizeof(ttl)));
    CHECK(!setsockopt(fd,SOL_SOCKET,SO_BINDTODEVICE,"eth0",5));ready("raw-wire",control);
    struct sockaddr_in peer=address(HOST,0);
    for(unsigned n=0;n<2;n++) {
        size_t size=echo_packet(0x4b33,4+n,n?4096:64);
        CHECK(sendto(fd,packet,size,0,(void *)&peer,sizeof(peer))==(ssize_t)size);
        CHECK(echo_receive(fd,ICMP_ECHOREPLY,0x4b33,4+n,size)>0);
    }
    CHECK(!close(fd));puts("IPV4 CASE raw-wire-fragmentation PASS");complete("raw-wire",control);
}
static int device_bound_socket(int type,const char *device,unsigned port)
{
    int fd=socket(AF_INET,type,0);CHECK(fd>=0);timeout(fd);
    CHECK(!setsockopt(fd,SOL_SOCKET,SO_BINDTODEVICE,device,strlen(device)+1));
    struct sockaddr_in local=address("0.0.0.0",port);CHECK(!bind(fd,(void *)&local,sizeof(local)));
    if(type==SOCK_STREAM)CHECK(!listen(fd,1));
    return fd;
}
static void device_name(int fd,const char *expected)
{
    char name[IFNAMSIZ];socklen_t length=sizeof(name);memset(name,0xa5,sizeof(name));
    CHECK(!getsockopt(fd,SOL_SOCKET,SO_BINDTODEVICE,name,&length) &&
        length==strlen(expected)+1 && !memcmp(name,expected,length));
}
static void bound_ports(int control)
{
    int udp_lo=device_bound_socket(SOCK_DGRAM,"lo",18094),udp_eth=device_bound_socket(SOCK_DGRAM,"eth0",18094);
    int tcp_lo=device_bound_socket(SOCK_STREAM,"lo",18095),tcp_eth=device_bound_socket(SOCK_STREAM,"eth0",18095);
    ready("bound-ports",control);
    int udp=datagram(0);struct sockaddr_in local=address("127.0.0.1",18094);
    CHECK(sendto(udp,"bound-udp-lo",12,0,(void *)&local,sizeof(local))==12);
    CHECK(recv(udp_lo,received,sizeof(received),0)==12 && !memcmp(received,"bound-udp-lo",12));
    struct sockaddr_in peer;socklen_t length=sizeof(peer);
    CHECK(recvfrom(udp_eth,received,sizeof(received),0,(void *)&peer,&length)==13 &&
        !memcmp(received,"bound-udp-eth",13) && peer.sin_addr.s_addr==inet_addr(HOST));
    CHECK(recv(udp_lo,received,sizeof(received),MSG_DONTWAIT)==-1 && errno==EAGAIN);
    CHECK(recv(udp_eth,received,sizeof(received),MSG_DONTWAIT)==-1 && errno==EAGAIN);
    int client=socket(AF_INET,SOCK_STREAM,0);CHECK(client>=0);timeout(client);local=address("127.0.0.1",18095);
    CHECK(!connect(client,(void *)&local,sizeof(local)) && send(client,"bound-tcp-lo",12,0)==12);
    length=sizeof(peer);int accepted_lo=accept(tcp_lo,(void *)&peer,&length);
    CHECK(accepted_lo>=0 && peer.sin_addr.s_addr==inet_addr("127.0.0.1"));timeout(accepted_lo);device_name(accepted_lo,"lo");
    receive_exact(accepted_lo,received,12);CHECK(!memcmp(received,"bound-tcp-lo",12));
    length=sizeof(peer);int accepted_eth=accept(tcp_eth,(void *)&peer,&length);
    CHECK(accepted_eth>=0 && peer.sin_addr.s_addr==inet_addr(HOST));timeout(accepted_eth);device_name(accepted_eth,"eth0");
    receive_exact(accepted_eth,received,13);CHECK(!memcmp(received,"bound-tcp-eth",13));
    CHECK(send(accepted_eth,received,13,0)==13);
    int fds[]={udp_lo,udp_eth,tcp_lo,tcp_eth,udp,client,accepted_lo,accepted_eth};
    for(unsigned i=0;i<sizeof(fds)/sizeof(fds[0]);i++)CHECK(!close(fds[i]));
    puts("IPV4 CASE bound-device-shared-port-isolation PASS");complete("bound-ports",control);
}
static void busybox_ping(const char *phase,int control,char *const extra[])
{
    ready(phase,control);int pipes[2];CHECK(!pipe(pipes));pid_t child=fork();CHECK(child>=0);
    if(!child) {
        CHECK(!close(pipes[0]));CHECK(dup2(pipes[1],1)==1 && dup2(pipes[1],2)==2);CHECK(!close(pipes[1]));
        char *args[18]={"/busybox","ping","-c","1","-w","5"};unsigned n=6;
        for(unsigned i=0;extra[i];i++){CHECK(n<16);args[n++]=extra[i];}args[n++]=HOST;args[n]=NULL;
        execv(args[0],args);_exit(127);
    }
    CHECK(!close(pipes[1]));char output[8192];size_t used=0;ssize_t got;
    while((got=read(pipes[0],output+used,sizeof(output)-1-used))>0){used+=(size_t)got;CHECK(used<sizeof(output)-1);}
    CHECK(!got && !close(pipes[0]));output[used]=0;int status;CHECK(waitpid(child,&status,0)==child);
    printf("IPV4 BUSYBOX %s BEGIN\n%sIPV4 BUSYBOX %s END\n",phase,output,phase);
    CHECK(WIFEXITED(status) && !WEXITSTATUS(status) && strstr(output,"1 packets transmitted, 1 packets received"));
    complete(phase,control);
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);alarm(90);int external=access("/external-ipv4-setup",F_OK)==0;
    configure(external);select_caller_cpu();local_messages();
    if(external) {
        int control=datagram(CONTROL_PORT);host_echo(control);external_udp(control);bound_ports(control);external_raw(control);
        char *plain[]={NULL},*ttl[]={"-t","37",NULL},*device[]={"-I","eth0",NULL},*source[]={"-I",GUEST,NULL};
        busybox_ping("ping-default",control,plain);busybox_ping("ping-ttl",control,ttl);
        busybox_ping("ping-device",control,device);busybox_ping("ping-source",control,source);
        CHECK(!close(control));puts("IPV4 CASE busybox-ping PASS");
    }
    alarm(0);puts("NETWORK PASS ipv4_basic");if(getpid()==1 && access("/reference-network-setup",F_OK)==0){sync();(void)reboot(RB_POWER_OFF);}return 0;
}
