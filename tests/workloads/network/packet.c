#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <linux/filter.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { printf("PACKET FAIL %d %s errno=%d\n",__LINE__,#x,errno);return 1; } } while (0)
static const char marker[]="BoarOS packet frame";
static int frame(int fd, unsigned start, int index)
{
    unsigned char bytes[128];
    for (unsigned i=0;i<4;i++) {
        struct sockaddr_ll name; socklen_t n=sizeof(name);
        struct pollfd p={fd,POLLIN,0};
        CHECK(poll(&p,1,1000)==1 && (p.revents&POLLIN));
        ssize_t size=recvfrom(fd,bytes,sizeof(bytes),MSG_TRUNC,(void *)&name,&n);
        CHECK(size>28 && name.sll_family==AF_PACKET && name.sll_ifindex==index && name.sll_protocol==htons(ETH_P_IP));
        if(name.sll_pkttype!=PACKET_HOST)continue;
        CHECK((bytes[start]>>4)==4 && bytes[start+9]==17);
        CHECK((size_t)size==start+28+sizeof(marker) && !memcmp(bytes+start+28,marker,sizeof(marker)));
        return 0;
    }
    return 1;
}
int main(void)
{
    int lo=(int)if_nametoindex("lo");
    CHECK(lo>0);
    int raw=socket(AF_PACKET,SOCK_RAW|SOCK_CLOEXEC,0), cooked=socket(AF_PACKET,SOCK_DGRAM,0);
    CHECK(raw>=0 && cooked>=0);
    struct sockaddr_ll link={.sll_family=AF_PACKET,.sll_ifindex=lo,.sll_protocol=htons(ETH_P_IP)};
    CHECK(!bind(raw,(void *)&link,sizeof(link)) && !bind(cooked,(void *)&link,sizeof(link)));
    struct sockaddr_ll name; socklen_t len=sizeof(name);
    CHECK(!getsockname(raw,(void *)&name,&len) && name.sll_ifindex==lo);
    CHECK(connect(raw,(void *)&link,sizeof(link))<0 && errno==EOPNOTSUPP);
    int a=socket(AF_INET,SOCK_DGRAM,0), b=socket(AF_INET,SOCK_DGRAM,0);
    CHECK(a>=0 && b>=0);
    struct sockaddr_in dst={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    CHECK(!bind(b,(void *)&dst,sizeof(dst)));
    len=sizeof(dst);
    CHECK(!getsockname(b,(void *)&dst,&len));
    CHECK(sendto(a,marker,sizeof(marker),0,(void *)&dst,sizeof(dst))==sizeof(marker));
    CHECK(!frame(raw,14,lo) && !frame(cooked,0,lo));
    char message[sizeof(marker)];
    CHECK(recv(b,message,sizeof(message),MSG_DONTWAIT)==sizeof(message) && !memcmp(message,marker,sizeof(marker)));
    unsigned char bytes[24]={0}; bytes[12]=0x88; bytes[13]=0xb5;
    link.sll_protocol=htons(0x88b5);
    int tx=socket(AF_PACKET,SOCK_RAW,0); CHECK(tx>=0);
    CHECK(!bind(raw,(void *)&link,sizeof(link)));
    CHECK(sendto(tx,bytes,sizeof(bytes),0,(void *)&link,sizeof(link))==sizeof(bytes));
    len=sizeof(name);
    CHECK(recvfrom(raw,message,1,MSG_TRUNC|MSG_DONTWAIT,(void *)&name,&len)==sizeof(bytes));
    unsigned eth=if_nametoindex("eth0");
    if(eth) {
        /* Keep ordinary background Ethernet traffic out of the test queue. */
        struct sock_filter code[]={
            BPF_STMT(BPF_LD|BPF_H|BPF_ABS,12),
            BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,0x88b5,0,1),
            BPF_STMT(BPF_RET|BPF_K,UINT32_MAX),
            BPF_STMT(BPF_RET|BPF_K,0)
        };
        struct sock_fprog program={.len=sizeof(code)/sizeof(code[0]),.filter=code};
        CHECK(!setsockopt(raw,SOL_SOCKET,SO_ATTACH_FILTER,&program,sizeof(program)));
        link.sll_ifindex=(int)eth;
        link.sll_protocol=htons(ETH_P_ALL);
        CHECK(!bind(raw,(void *)&link,sizeof(link)));
        memset(bytes,0xff,6);
        CHECK(sendto(tx,bytes,sizeof(bytes),0,(void *)&link,sizeof(link))==sizeof(bytes));
        len=sizeof(name);
        CHECK(recvfrom(raw,message,1,MSG_TRUNC|MSG_DONTWAIT,(void *)&name,&len)==sizeof(bytes));
        CHECK(name.sll_pkttype==PACKET_OUTGOING && name.sll_ifindex==(int)eth &&
            name.sll_protocol==htons(0x88b5));
        puts("PACKET Ethernet transmit and filter PASS");
    }
    CHECK(!close(tx) && !close(raw) && !close(cooked) && !close(a) && !close(b));
    pid_t child=fork();
    CHECK(child>=0);
    if(!child) {
        if(setuid(65534)) _exit(2);
        _exit(socket(AF_PACKET,SOCK_RAW,0)==-1 && errno==EPERM?0:3);
    }
    int status;
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    puts("PACKET PASS");
    return 0;
}
