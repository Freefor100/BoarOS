#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"CORK FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);} } while(0)
static void option(int fd,int name,int value)
{ CHECK(!setsockopt(fd,IPPROTO_TCP,name,&value,sizeof(value))); }
static void pair(int p[2])
{
    int listener=socket(AF_INET,SOCK_STREAM,0);CHECK(listener>=0);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_addr={htonl(INADDR_LOOPBACK)}};
    CHECK(!bind(listener,(void *)&address,sizeof(address)) && !listen(listener,1));
    socklen_t size=sizeof(address);CHECK(!getsockname(listener,(void *)&address,&size));
    p[0]=socket(AF_INET,SOCK_STREAM,0);CHECK(p[0]>=0 && !connect(p[0],(void *)&address,size));
    p[1]=accept(listener,NULL,NULL);CHECK(p[1]>=0 && !close(listener));
}
static void quiet(int fd)
{ struct pollfd p={.fd=fd,.events=POLLIN};CHECK(!poll(&p,1,20)); }
static void receive(int fd,const void *expected,size_t size)
{
    unsigned char bytes[4096];size_t total=0;
    while(total<size) {
        struct pollfd p={.fd=fd,.events=POLLIN};CHECK(poll(&p,1,2000)==1);
        size_t count=size-total<sizeof(bytes)?size-total:sizeof(bytes);
        ssize_t n=recv(fd,bytes,count,0);CHECK(n>0 && !memcmp(bytes,(const char *)expected+total,(size_t)n));
        total+=(size_t)n;
    }
}
static void close_pair(int p[2])
{
    char byte;CHECK(!shutdown(p[0],SHUT_WR) && !recv(p[1],&byte,1,0));
    CHECK(!shutdown(p[1],SHUT_WR) && !recv(p[0],&byte,1,0));
    CHECK(!close(p[0]) && !close(p[1]));
}
static void more(void)
{
    int p[2];pair(p);
    CHECK(send(p[0],"a",1,MSG_MORE)==1);quiet(p[1]);
    CHECK(!send(p[0],"",0,0));quiet(p[1]);
    struct iovec iov[2]={{"b",1},{"c",1}};
    struct msghdr message={.msg_iov=iov,.msg_iovlen=2};
    CHECK(sendmsg(p[0],&message,MSG_MORE)==2);quiet(p[1]);
    struct iovec vectors[2]={{"d",1},{"e",1}};
    struct mmsghdr batch[2]={{{.msg_iov=&vectors[0],.msg_iovlen=1},0},
                             {{.msg_iov=&vectors[1],.msg_iovlen=1},0}};
    CHECK(sendmmsg(p[0],batch,2,MSG_MORE)==2 && batch[0].msg_len==1 && batch[1].msg_len==1);quiet(p[1]);
    CHECK(send(p[0],"f",1,0)==1);receive(p[1],"abcdef",6);
    close_pair(p);puts("CORK CASE MSG_MORE recv/sendmsg and zero-send PASS");

    pair(p);option(p[0],TCP_NODELAY,1);
    CHECK(send(p[0],"e",1,MSG_MORE)==1);quiet(p[1]);
    option(p[0],TCP_NODELAY,1);receive(p[1],"e",1);
    close_pair(p);puts("CORK CASE MSG_MORE overrides NODELAY, explicit NODELAY push PASS");

    pair(p);CHECK(send(p[0],"timeout",7,MSG_MORE)==7);quiet(p[1]);
    /* Linux's probe/RTO and scheduling may exceed 200ms. Require finite
     * natural progress, not a wall-clock equality or a packet boundary. */
    receive(p[1],"timeout",7);close_pair(p);puts("CORK CASE MSG_MORE natural timeout PASS");

    pair(p);int mss=0;socklen_t len=sizeof(mss);CHECK(!getsockopt(p[0],IPPROTO_TCP,TCP_MAXSEG,&mss,&len) && mss>0);
    char *data=malloc((size_t)mss+1);CHECK(data);memset(data,'m',(size_t)mss+1);
    CHECK(send(p[0],data,(size_t)mss+1,MSG_MORE)==mss+1);
    receive(p[1],data,(size_t)mss+1);free(data);close_pair(p);puts("CORK CASE full MSS and tail progress PASS");

    pair(p);CHECK(send(p[0],"fault",5,MSG_MORE)==5);quiet(p[1]);
    CHECK(send(p[0],(void *)1,1,0)==-1 && errno==EFAULT);quiet(p[1]);
    CHECK(send(p[0],"!",1,0)==1);receive(p[1],"fault!",6);close_pair(p);
    pair(p);long page=sysconf(_SC_PAGESIZE);CHECK(page>0);
    size_t valid=16*(size_t)page;
    char *map=mmap(NULL,valid+(size_t)page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(map!=MAP_FAILED);
    memset(map,'x',valid);CHECK(!mprotect(map+valid,(size_t)page,PROT_NONE));
    int report[2];CHECK(!pipe(report));pid_t child=fork();CHECK(child>=0);
    if(!child) {
        CHECK(!close(p[0]) && !close(report[0]));char bytes[4096];uint64_t count=0;ssize_t n;
        while((n=recv(p[1],bytes,sizeof(bytes),0))>0) {
            for(ssize_t i=0;i<n;i++)CHECK(bytes[i]=='x');
            count+=(uint64_t)n;
        }
        CHECK(!n && write(report[1],&count,sizeof(count))==sizeof(count));_exit(0);
    }
    CHECK(!close(report[1]));ssize_t n=send(p[0],map,valid+(size_t)page,MSG_MORE);CHECK(n>0 && (size_t)n<=valid);
    CHECK(!shutdown(p[0],SHUT_WR));uint64_t count=0;CHECK(read(report[0],&count,sizeof(count))==sizeof(count) && count==(uint64_t)n);
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    CHECK(!close(report[0]) && !munmap(map,valid+(size_t)page));close_pair(p);
    puts("CORK CASE first fault and accepted prefix fault PASS");

    pair(p);CHECK(send(p[0],"fin",3,MSG_MORE)==3);quiet(p[1]);
    CHECK(!shutdown(p[0],SHUT_WR));receive(p[1],"fin",3);close_pair(p);
    pair(p);CHECK(send(p[0],"close",5,MSG_MORE)==5);quiet(p[1]);
    int duplicate=dup(p[0]);CHECK(duplicate>=0 && !close(p[0]));quiet(p[1]);
    CHECK(!close(duplicate));receive(p[1],"close",5);char byte;CHECK(!recv(p[1],&byte,1,0) && !close(p[1]));
    puts("CORK CASE shutdown/final close and dup lifetime PASS");
}
static int get_option(int fd,int name)
{ int value=-1;socklen_t size=sizeof(value);CHECK(!getsockopt(fd,IPPROTO_TCP,name,&value,&size) && size==sizeof(value));return value; }
static void cork(void)
{
    int p[2];pair(p);CHECK(!get_option(p[0],TCP_CORK));option(p[0],TCP_CORK,1);
    int duplicate=dup(p[0]);CHECK(duplicate>=0 && get_option(duplicate,TCP_CORK)==1);
    CHECK(send(p[0],"a",1,0)==1);quiet(p[1]);
    CHECK(send(duplicate,"b",1,0)==1);quiet(p[1]);CHECK(get_option(p[0],TCP_CORK)==1);
    option(duplicate,TCP_CORK,0);receive(p[1],"ab",2);CHECK(!close(duplicate));
    close_pair(p);puts("CORK CASE persistent setting and shared fd uncork PASS");

    pair(p);option(p[0],TCP_CORK,1);option(p[0],TCP_NODELAY,1);
    CHECK(send(p[0],"c",1,0)==1);quiet(p[1]);
    option(p[0],TCP_NODELAY,1);receive(p[1],"c",1);CHECK(get_option(p[0],TCP_CORK)==1);
    CHECK(send(p[0],"d",1,0)==1);quiet(p[1]);option(p[0],TCP_CORK,0);receive(p[1],"d",1);
    close_pair(p);puts("CORK CASE CORK precedence and NODELAY explicit push PASS");

    pair(p);option(p[0],TCP_CORK,1);CHECK(send(p[0],"timeout",7,0)==7);quiet(p[1]);
    receive(p[1],"timeout",7);CHECK(get_option(p[0],TCP_CORK)==1);
    CHECK(send(p[0],"again",5,0)==5);quiet(p[1]);option(p[0],TCP_CORK,0);receive(p[1],"again",5);
    close_pair(p);puts("CORK CASE timeout retains option and next tail corks PASS");

    pair(p);option(p[0],TCP_CORK,1);int mss=get_option(p[0],TCP_MAXSEG);CHECK(mss>0);
    char *bytes=malloc((size_t)mss+1);CHECK(bytes);memset(bytes,'M',(size_t)mss+1);
    CHECK(send(p[0],bytes,(size_t)mss+1,0)==mss+1);receive(p[1],bytes,(size_t)mss);
    quiet(p[1]);option(p[0],TCP_CORK,0);receive(p[1],bytes,1);free(bytes);close_pair(p);
    puts("CORK CASE full MSS passes, ACK does not release partial tail PASS");

    pair(p);option(p[0],TCP_CORK,1);CHECK(send(p[0],"fin",3,0)==3);quiet(p[1]);
    CHECK(!shutdown(p[0],SHUT_RD));quiet(p[1]);CHECK(!shutdown(p[0],SHUT_WR));
    receive(p[1],"fin",3);close_pair(p);
    pair(p);option(p[0],TCP_CORK,1);CHECK(send(p[0],"close",5,0)==5);quiet(p[1]);
    CHECK(!close(p[0]));receive(p[1],"close",5);char byte;CHECK(!recv(p[1],&byte,1,0) && !close(p[1]));
    puts("CORK CASE read shutdown preserves tail, write shutdown/final close push PASS");

    for(int family=AF_INET;family<=AF_INET6;family+=AF_INET6-AF_INET) {
        int listener=socket(family,SOCK_STREAM,0);CHECK(listener>=0);option(listener,TCP_CORK,1);
        struct sockaddr_storage address={0};socklen_t size;
        if(family==AF_INET) {
            struct sockaddr_in *a=(void *)&address;a->sin_family=AF_INET;a->sin_addr.s_addr=htonl(INADDR_LOOPBACK);size=sizeof(*a);
        } else {
            struct sockaddr_in6 *a=(void *)&address;a->sin6_family=AF_INET6;a->sin6_addr=in6addr_loopback;size=sizeof(*a);
        }
        CHECK(!bind(listener,(void *)&address,size) && !listen(listener,1) && !getsockname(listener,(void *)&address,&size));
        p[0]=socket(family,SOCK_STREAM,0);CHECK(p[0]>=0 && !connect(p[0],(void *)&address,size));
        p[1]=accept(listener,NULL,NULL);CHECK(p[1]>=0 && !close(listener) && get_option(p[1],TCP_CORK)==1);
        CHECK(send(p[1],"inherit",7,0)==7);quiet(p[0]);option(p[1],TCP_CORK,0);receive(p[0],"inherit",7);close_pair(p);
    }
    int udp=socket(AF_INET,SOCK_DGRAM,0),one=1;CHECK(udp>=0);
    CHECK(setsockopt(udp,IPPROTO_TCP,TCP_CORK,&one,sizeof(one))==-1 && errno==ENOPROTOOPT);CHECK(!close(udp));
    puts("CORK CASE IPv4/IPv6 listener inheritance and non-TCP rejection PASS");
}

int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);alarm(30);more();cork();
    puts("CORK ALL PASS");if(getpid()==1)reboot(RB_POWER_OFF);return 0;
}
