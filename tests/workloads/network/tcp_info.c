#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"TCPINFO FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
static struct tcp_info info(int fd)
{
    struct tcp_info value;memset(&value,0xa5,sizeof(value));socklen_t n=sizeof(value);
    CHECK(!getsockopt(fd,IPPROTO_TCP,TCP_INFO,&value,&n));CHECK(n>=104);
    if(getpid()==1)CHECK(n==232);
    return value;
}
static void ipv6_case(void)
{
    struct sockaddr_in6 a={.sin6_family=AF_INET6,.sin6_addr=IN6ADDR_LOOPBACK_INIT};
    int listener=socket(AF_INET6,SOCK_STREAM,0);CHECK(listener>=0);
    socklen_t size=sizeof(a);CHECK(!bind(listener,(void *)&a,size) && !listen(listener,1));
    CHECK(!getsockname(listener,(void *)&a,&size));
    int client=socket(AF_INET6,SOCK_STREAM,0);CHECK(client>=0 && !connect(client,(void *)&a,size));
    int server=accept(listener,NULL,NULL);CHECK(server>=0 && info(client).tcpi_state==TCP_ESTABLISHED && info(server).tcpi_state==TCP_ESTABLISHED);
    char c;CHECK(send(client,"a",1,0)==1 && recv(server,&c,1,MSG_WAITALL)==1 && c=='a');
    CHECK(!shutdown(client,SHUT_WR) && !recv(server,&c,1,0));
    CHECK(!shutdown(server,SHUT_WR) && !recv(client,&c,1,0));
    CHECK(!close(client) && !close(server) && !close(listener));
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);alarm(30);
    int listener=socket(AF_INET,SOCK_STREAM,0);CHECK(listener>=0 && info(listener).tcpi_state==TCP_CLOSE);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    socklen_t n=sizeof(address);CHECK(!bind(listener,(void *)&address,n) && !listen(listener,3));
    CHECK(info(listener).tcpi_state==TCP_LISTEN && info(listener).tcpi_sacked==3);
    CHECK(!getsockname(listener,(void *)&address,&n));
    int client=socket(AF_INET,SOCK_STREAM,0);CHECK(client>=0 && !connect(client,(void *)&address,n));
    CHECK(info(listener).tcpi_unacked==1);
    int server=accept(listener,NULL,NULL);CHECK(server>=0 && !info(listener).tcpi_unacked);
    unsigned char initial[232];uint64_t initial_acked;uint32_t initial_delivered;
    n=sizeof(initial);CHECK(!getsockopt(client,IPPROTO_TCP,TCP_INFO,initial,&n) && n==sizeof(initial));
    memcpy(&initial_acked,initial+120,8);memcpy(&initial_delivered,initial+192,4);
    CHECK(initial_acked==1 && initial_delivered==1);
    n=sizeof(initial);CHECK(!getsockopt(server,IPPROTO_TCP,TCP_INFO,initial,&n) && n==sizeof(initial));
    memcpy(&initial_acked,initial+120,8);memcpy(&initial_delivered,initial+192,4);
    CHECK(initial_acked==0 && initial_delivered==1);
    struct tcp_info before=info(client);CHECK(before.tcpi_state==TCP_ESTABLISHED && before.tcpi_snd_mss>0 && before.tcpi_snd_cwnd>0);
    char payload[2048];memset(payload,42,sizeof(payload));CHECK(send(client,payload,sizeof(payload),0)==sizeof(payload));
    char buffer[sizeof(payload)];CHECK(recv(server,buffer,sizeof(buffer),MSG_WAITALL)==sizeof(buffer) && !memcmp(payload,buffer,sizeof(buffer)));
    struct tcp_info after=info(client);CHECK(after.tcpi_total_retrans==before.tcpi_total_retrans);
    CHECK(info(server).tcpi_rcv_mss>0);
    for(socklen_t length=0;length<=233;length++) {
        unsigned char bytes[240];memset(bytes,0xa5,sizeof(bytes));n=length;
        CHECK(!getsockopt(client,IPPROTO_TCP,TCP_INFO,bytes,&n));
        CHECK(n<=length && (getpid()!=1 || n==(length<232?length:232)));
        for(unsigned i=n;i<sizeof(bytes);i++)CHECK(bytes[i]==0xa5);
    }
    long page=sysconf(_SC_PAGESIZE);void *bad=mmap(NULL,page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(bad!=MAP_FAILED);
    n=0;CHECK(!getsockopt(client,IPPROTO_TCP,TCP_INFO,bad,&n) && !n);
    n=4;CHECK(getsockopt(client,IPPROTO_TCP,TCP_INFO,bad,&n)==-1 && errno==EFAULT && n==4);
    n=(socklen_t)-1;CHECK(getsockopt(client,IPPROTO_TCP,TCP_INFO,&after,&n)==-1 && errno==EINVAL);
    int value=1;CHECK(setsockopt(client,IPPROTO_TCP,TCP_INFO,&value,4)==-1 && errno==ENOPROTOOPT);
    CHECK(setsockopt(client,IPPROTO_TCP,TCP_INFO,bad,4)==-1 && errno==EFAULT);
    CHECK(setsockopt(client,IPPROTO_TCP,TCP_INFO,bad,0)==-1 && errno==EINVAL);
    CHECK(!munmap(bad,page));
    /* iperf save_tcpinfo does not check returned length. Validate the actual
     * extended snd_wnd consumer offset, not just an accepted TCP_INFO request. */
    unsigned char extended[256];memset(extended,0xa5,sizeof(extended));n=sizeof(extended);
    CHECK(!getsockopt(client,IPPROTO_TCP,TCP_INFO,extended,&n));
    printf("TCPINFO reported_length=%u modern_snd_wnd_offset=228 consumer_requires=232\n",n);
    if(getpid()==1)CHECK(n==232 && extended[232]==0xa5);
    uint32_t snd_wnd;memcpy(&snd_wnd,extended+228,sizeof(snd_wnd));CHECK(snd_wnd && snd_wnd!=0xa5a5a5a5U);
    uint64_t sent;memcpy(&sent,extended+200,sizeof(sent));CHECK(sent>=sizeof(payload));
    /* Trigger a real ACK of all client data before inspecting cumulative bytes. */
    CHECK(send(server,"a",1,0)==1 && recv(client,buffer,1,MSG_WAITALL)==1);
    n=sizeof(extended);CHECK(!getsockopt(client,IPPROTO_TCP,TCP_INFO,extended,&n));
    uint64_t acked;memcpy(&acked,extended+120,sizeof(acked));CHECK(acked>=sizeof(payload));
    n=sizeof(extended);CHECK(!getsockopt(server,IPPROTO_TCP,TCP_INFO,extended,&n));
    uint64_t received;memcpy(&received,extended+128,sizeof(received));CHECK(received==sizeof(payload));
    CHECK(!shutdown(client,SHUT_WR) && !recv(server,buffer,1,0));
    CHECK(info(server).tcpi_state==TCP_CLOSE_WAIT);
    n=sizeof(extended);CHECK(!getsockopt(server,IPPROTO_TCP,TCP_INFO,extended,&n));
    memcpy(&received,extended+128,sizeof(received));CHECK(received==sizeof(payload)+1); /* Linux sequence accounting includes FIN */
    CHECK(!shutdown(server,SHUT_WR) && !recv(client,buffer,1,0));
    n=sizeof(extended);CHECK(!getsockopt(client,IPPROTO_TCP,TCP_INFO,extended,&n));
    memcpy(&acked,extended+120,sizeof(acked));CHECK(acked==sizeof(payload)+2); /* active SYN and FIN */
    CHECK(!close(client) && !close(server) && !close(listener));
    ipv6_case();
    int udp=socket(AF_INET,SOCK_DGRAM,0);CHECK(udp>=0);n=sizeof(after);
    CHECK(getsockopt(udp,IPPROTO_TCP,TCP_INFO,&after,&n)==-1 && errno==EOPNOTSUPP);
    CHECK(!close(udp));
    puts("TCPINFO ALL PASS");if(getpid()==1)reboot(RB_POWER_OFF);return 0;
}
