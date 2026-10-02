#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/tcp.h>
#include <limits.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <signal.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <time.h>
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
    char byte;
    int after_error=recv(refused,&byte,1,MSG_DONTWAIT);
    printf("NETWORK refusal observed recv=%d errno=%d\n",after_error,errno);
    CHECK(after_error==0);
    CHECK(send(refused,"x",1,MSG_NOSIGNAL)==-1 && errno==EPIPE);
    CHECK(close(refused)==0 && close(unused)==0);
    int udp=socket(AF_INET,SOCK_DGRAM|SOCK_NONBLOCK,0);CHECK(udp>=0);
    a4.sin_addr.s_addr=htonl(INADDR_LOOPBACK);a4.sin_port=0;
    CHECK(bind(udp,(void *)&a4,sizeof(a4))==0);length=sizeof(a4);CHECK(getsockname(udp,(void *)&a4,&length)==0);
    CHECK(connect(udp,(void *)&a4,sizeof(a4))==0);
    struct sockaddr_in peer;length=sizeof(peer);CHECK(getpeername(udp,(void *)&peer,&length)==0 && peer.sin_port==a4.sin_port);
    struct sockaddr disconnect={.sa_family=AF_UNSPEC};CHECK(connect(udp,&disconnect,sizeof(disconnect))==0);
    CHECK(getpeername(udp,(void *)&peer,&length)==-1 && errno==ENOTCONN);
    length=sizeof(peer);CHECK(getsockname(udp,(void *)&peer,&length)==0 && peer.sin_port==0 && peer.sin_addr.s_addr==a4.sin_addr.s_addr);
    a4.sin_port=0;CHECK(bind(udp,(void *)&a4,sizeof(a4))==0);
    CHECK(close(udp)==0);
    puts("NETWORK PASS options: real options, V6ONLY port isolation and UDP peer");
}
static void data_and_shutdown(void)
{
    int udp=socket(AF_INET,SOCK_DGRAM,0), outsider=socket(AF_INET,SOCK_DGRAM,0);CHECK(udp>=0 && outsider>=0);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_addr={htonl(INADDR_LOOPBACK)}};
    CHECK(bind(udp,(void *)&address,sizeof(address))==0);socklen_t length=sizeof(address);
    CHECK(getsockname(udp,(void *)&address,&length)==0 && connect(udp,(void *)&address,sizeof(address))==0);
    CHECK(sendto(outsider,"wrong",5,0,(void *)&address,sizeof(address))==5);
    CHECK(write(udp,"connected",9)==9);
    char data[32];CHECK(recv(udp,data,sizeof(data),0)==9 && !memcmp(data,"connected",9));
    CHECK(recv(udp,data,sizeof(data),MSG_DONTWAIT)==-1 && errno==EAGAIN);
    CHECK((fcntl(udp,F_GETFL)&O_NONBLOCK)==0);
    struct iovec vector[]={{"one",3},{"two",3}};
    CHECK(writev(udp,vector,2)==6 && recv(udp,data,sizeof(data),0)==6 && !memcmp(data,"onetwo",6));
    CHECK(send(udp,"",0,0)==0 && recv(udp,data,sizeof(data),0)==0);
    struct msghdr message={.msg_iov=vector,.msg_iovlen=2};
    CHECK(sendmsg(udp,&message,0)==6);
    struct iovec output={data,sizeof(data)};struct sockaddr_in source;
    message=(struct msghdr){.msg_iov=&output,.msg_iovlen=1,.msg_name=&source,.msg_namelen=sizeof(source)};
    CHECK(recvmsg(udp,&message,0)==6 && !memcmp(data,"onetwo",6) && message.msg_namelen==sizeof(source) && message.msg_flags==0);
    CHECK(sendmsg(udp,&(struct msghdr){.msg_iov=vector,.msg_iovlen=2},0)==6);
    output.iov_len=3;message.msg_flags=0;CHECK(recvmsg(udp,&message,0)==3 && (message.msg_flags&MSG_TRUNC));
    CHECK(send(udp,"queued",6,0)==6 && shutdown(udp,SHUT_RD)==0);
    CHECK(recv(udp,data,sizeof(data),0)==6 && recv(udp,data,sizeof(data),0)==0);
    CHECK(close(udp)==0 && close(outsider)==0);
    int route=socket(AF_INET,SOCK_DGRAM,0);CHECK(route>=0);
    struct sockaddr_in wildcard={.sin_family=AF_INET};
    CHECK(bind(route,(void *)&wildcard,sizeof(wildcard))==0);
    CHECK(connect(route,(void *)&address,sizeof(address))==0);
    length=sizeof(wildcard);CHECK(getsockname(route,(void *)&wildcard,&length)==0 && wildcard.sin_addr.s_addr==htonl(INADDR_LOOPBACK));
    CHECK(close(route)==0);
    int listener=socket(AF_INET,SOCK_STREAM,0);CHECK(listener>=0);address.sin_port=0;
    CHECK(bind(listener,(void *)&address,sizeof(address))==0 && listen(listener,4)==0);
    length=sizeof(address);CHECK(getsockname(listener,(void *)&address,&length)==0);
    int client=socket(AF_INET,SOCK_STREAM,0);CHECK(client>=0 && connect(client,(void *)&address,sizeof(address))==0);
    int server=accept(listener,NULL,NULL);CHECK(server>=0);
    int minimal=0;CHECK(setsockopt(client,SOL_SOCKET,SO_SNDBUF,&minimal,sizeof(minimal))==0);
    pid_t reader=fork();CHECK(reader>=0);
    if(!reader) {
        CHECK(close(client)==0 && close(listener)==0);
        unsigned total=0;char chunk[1024];
        while(total<65536) {
            ssize_t got=read(server,chunk,sizeof(chunk));CHECK(got>0);
            for(ssize_t j=0;j<got;j++)CHECK(chunk[j]=='b');
            total+=(unsigned)got;
        }
        CHECK(write(server,"ok",2)==2 && close(server)==0);_exit(0);
    }
    char *bulk=malloc(65536);CHECK(bulk!=NULL);memset(bulk,'b',65536);
    CHECK(send(client,bulk,65536,MSG_NOSIGNAL)==65536);free(bulk);
    CHECK(read(client,data,sizeof(data))==2 && !memcmp(data,"ok",2));
    int child_status;CHECK(waitpid(reader,&child_status,0)==reader && child_status==0);
    int duplicate=dup(client);CHECK(duplicate>=0);
    CHECK(write(server,"abc",3)==3);memset(data,'z',sizeof(data));
    CHECK(recv(client,data,3,MSG_TRUNC)==3 && data[0]=='z' && data[2]=='z');
    CHECK(recv(client,data,sizeof(data),MSG_DONTWAIT)==-1 && errno==EAGAIN);
    void *guard=mmap(NULL,4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(guard!=MAP_FAILED);
    CHECK(write(server,"abc",3)==3 && recv(client,guard,3,MSG_TRUNC)==3 && munmap(guard,4096)==0);
    CHECK(send(client,"before FIN",10,0)==10 && shutdown(duplicate,SHUT_WR)==0);
    CHECK(recv(server,data,sizeof(data),0)==10 && !memcmp(data,"before FIN",10));
    CHECK(recv(server,data,sizeof(data),0)==0);
    struct pollfd ready={.fd=server,.events=POLLIN|POLLOUT|POLLRDHUP};
    CHECK(poll(&ready,1,2000)==1 && (ready.revents&POLLRDHUP) && (ready.revents&POLLOUT));
    CHECK(send(server,"reply",5,0)==5 && recv(client,data,sizeof(data),0)==5 && !memcmp(data,"reply",5));
    CHECK(send(client,"bad",3,MSG_NOSIGNAL)==-1 && errno==EPIPE);
    CHECK(shutdown(server,SHUT_WR)==0 && recv(client,data,sizeof(data),0)==0);
    CHECK(shutdown(client,99)==-1 && errno==EINVAL);
    CHECK(close(client)==0 && close(duplicate)==0 && close(server)==0 && close(listener)==0);
    int pair[2];CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);
    CHECK(write(pair[0],"queued",6)==6 && shutdown(pair[0],SHUT_WR)==0);
    CHECK(read(pair[1],data,sizeof(data))==6 && read(pair[1],data,sizeof(data))==0);
    CHECK(send(pair[0],"bad",3,MSG_NOSIGNAL)==-1 && errno==EPIPE);
    CHECK(write(pair[1],"response",8)==8 && read(pair[0],data,sizeof(data))==8);
    CHECK(shutdown(pair[0],SHUT_RD)==0);
    signal(SIGPIPE,SIG_IGN);CHECK(write(pair[1],"bad",3)==-1 && errno==EPIPE);
    CHECK(close(pair[0])==0 && close(pair[1])==0);
    CHECK(socketpair(AF_UNIX,SOCK_DGRAM,0,pair)==0);
    int budget=4096;CHECK(setsockopt(pair[0],SOL_SOCKET,SO_SNDBUF,&budget,sizeof(budget))==0);
    char packet[1000];memset(packet,1,sizeof(packet));unsigned filled=0;
    while(send(pair[0],packet,sizeof(packet),MSG_DONTWAIT)==sizeof(packet))CHECK(++filled<512);
    CHECK(errno==EAGAIN && filled>0);
    struct timeval timeout={0,20000};CHECK(setsockopt(pair[0],SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout))==0);
    struct timespec before,after;CHECK(clock_gettime(CLOCK_MONOTONIC,&before)==0);
    CHECK(send(pair[0],packet,sizeof(packet),0)==-1 && errno==EAGAIN);
    CHECK(clock_gettime(CLOCK_MONOTONIC,&after)==0);
    long long elapsed=(after.tv_sec-before.tv_sec)*1000000000LL+after.tv_nsec-before.tv_nsec;
    CHECK(elapsed>=15000000 && elapsed<2000000000);
    CHECK(recv(pair[1],packet,sizeof(packet),0)==sizeof(packet));
    CHECK(send(pair[0],packet,sizeof(packet),0)==sizeof(packet));
    CHECK(close(pair[0])==0 && close(pair[1])==0);
    puts("NETWORK PASS data: connected UDP boundaries/filter and shared-OFD half-close");
}
static void reset_and_accept_rollback(void)
{
    int listener=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK,0);CHECK(listener>=0);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_addr={htonl(INADDR_LOOPBACK)}};
    CHECK(bind(listener,(void *)&address,sizeof(address))==0 && listen(listener,1)==0);
    socklen_t length=sizeof(address);CHECK(getsockname(listener,(void *)&address,&length)==0);
    for(unsigned round=0;round<2;round++){
        int client=socket(AF_INET,SOCK_STREAM,0);CHECK(client>=0);
        CHECK(connect(client,(void *)&address,sizeof(address))==0);
        struct pollfd ready={.fd=listener,.events=POLLIN};CHECK(poll(&ready,1,2000)==1);
        if(!round){
            CHECK(accept(listener,(void *)1,&length)==-1 && errno==EFAULT);
            CHECK(accept(listener,NULL,NULL)==-1 && errno==EAGAIN);
        }else{
            int server=accept(listener,NULL,NULL);CHECK(server>=0);
            CHECK(write(client,"unread",6)==6);
            ready=(struct pollfd){.fd=server,.events=POLLIN};CHECK(poll(&ready,1,2000)==1);
            CHECK(close(server)==0);
            ready=(struct pollfd){.fd=client,.events=POLLERR};CHECK(poll(&ready,1,2000)==1 && (ready.revents&POLLERR));
            CHECK(get_int(client,SOL_SOCKET,SO_ERROR)==ECONNRESET);
            CHECK(get_int(client,SOL_SOCKET,SO_ERROR)==0);
            char byte;CHECK(recv(client,&byte,1,MSG_DONTWAIT)==0);
            CHECK(send(client,"bad",3,MSG_NOSIGNAL)==-1 && errno==EPIPE);
        }
        CHECK(close(client)==0);
    }
    CHECK(close(listener)==0);
    int stats=open("/proc/boaros_net_stats",O_RDONLY);
    if(stats>=0){char buffer[1024];ssize_t got=read(stats,buffer,sizeof(buffer)-1);CHECK(got>0);
        buffer[got]=0;CHECK(strstr(buffer,"counter_bits=16\n") && strstr(buffer,"tcp_write_calls="));CHECK(close(stats)==0);}
    else CHECK(errno==ENOENT);
    puts("NETWORK PASS lifecycle: reset differs from SYN refusal; failed accept releases backlog");
}
static void receive_state(void)
{
    char byte = 0;
    struct msghdr empty = {0};
    for (unsigned i = 0; i < 2; i++) {
        int fd = socket(i ? AF_INET6 : AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        CHECK(fd >= 0);
        CHECK(read(fd, &byte, 0) == 0 && readv(fd, NULL, 0) == 0);
        CHECK(recv(fd, &byte, 1, MSG_DONTWAIT) == -1 && errno == ENOTCONN);
        CHECK(recv(fd, &byte, 0, MSG_DONTWAIT) == -1 && errno == ENOTCONN);
        CHECK(recvmsg(fd, &empty, MSG_DONTWAIT) == -1 && errno == ENOTCONN);
        struct pollfd ready = {.fd = fd, .events = POLLIN | POLLOUT};
        CHECK(poll(&ready, 1, 0) == 1 && (ready.revents & POLLHUP));
        CHECK(close(fd) == 0);
    }
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listener >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(bind(listener, (void *)&address, sizeof(address)) == 0 && listen(listener, 1) == 0);
    socklen_t length = sizeof(address);
    CHECK(getsockname(listener, (void *)&address, &length) == 0);
    CHECK(recv(listener, &byte, 0, MSG_DONTWAIT) == -1 && errno == ENOTCONN);
    int client = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(client >= 0 && connect(client, (void *)&address, sizeof(address)) == 0);
    int server = accept(listener, NULL, NULL);
    CHECK(server >= 0);
    CHECK(recv(server, &byte, 0, MSG_DONTWAIT) == -1 && errno == EAGAIN);
    CHECK(recvmsg(server, &empty, MSG_DONTWAIT) == -1 && errno == EAGAIN);
    CHECK(send(client, "x", 1, MSG_NOSIGNAL) == 1);
    struct pollfd ready = {.fd = server, .events = POLLIN};
    CHECK(poll(&ready, 1, 2000) == 1);
    CHECK(recv(server, &byte, 0, MSG_DONTWAIT) == 0);
    CHECK(recvmsg(server, &empty, MSG_DONTWAIT) == 0);
    CHECK(recv(server, &byte, 1, MSG_DONTWAIT) == 1 && byte == 'x');
    CHECK(shutdown(client, SHUT_WR) == 0 && poll(&ready, 1, 2000) == 1);
    CHECK(recv(server, &byte, 0, MSG_DONTWAIT) == 0);
    CHECK(close(server) == 0 && close(client) == 0 && close(listener) == 0);
    puts("NETWORK PASS receive-state: fresh TCP errors and zero-length message state");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    addresses();
    receive_state();
    data_and_shutdown();
    options();
    reset_and_accept_rollback();
    puts("NETWORK PASS contract");
    return 0;
}
