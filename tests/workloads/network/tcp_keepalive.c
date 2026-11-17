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
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"KEEPALIVE FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);} } while(0)
static const int names[]={TCP_KEEPIDLE,TCP_KEEPINTVL,TCP_KEEPCNT};
static int get(int fd,int name)
{ int value=-1;socklen_t n=sizeof(value);CHECK(!getsockopt(fd,IPPROTO_TCP,name,&value,&n) && n==sizeof(value));return value; }
static void set(int fd,int name,int value)
{ CHECK(!setsockopt(fd,IPPROTO_TCP,name,&value,sizeof(value))); }
static void values(void)
{
    const int defaults[]={7200,75,9},limits[]={32767,32767,127};
    int fd=socket(AF_INET,SOCK_STREAM,0);CHECK(fd>=0);
    int dupfd=dup(fd);CHECK(dupfd>=0);
    for(unsigned i=0;i<3;i++) {
        CHECK(get(fd,names[i])==defaults[i]);
        int bad[]={0,-1,limits[i]+1};
        for(unsigned j=0;j<3;j++) {
            errno=0;CHECK(setsockopt(fd,IPPROTO_TCP,names[i],&bad[j],sizeof(int))==-1 && errno==EINVAL);
            CHECK(get(fd,names[i])==defaults[i]);
        }
        set(fd,names[i],limits[i]);CHECK(get(dupfd,names[i])==limits[i]);
        set(dupfd,names[i],1);CHECK(get(fd,names[i])==1);
    }
    int enabled=-1;socklen_t n=sizeof(enabled);CHECK(!getsockopt(fd,SOL_SOCKET,SO_KEEPALIVE,&enabled,&n) && !enabled);
    enabled=1;CHECK(!setsockopt(fd,SOL_SOCKET,SO_KEEPALIVE,&enabled,sizeof(enabled)));
    CHECK(get(fd,TCP_KEEPIDLE)==1 && !close(fd) && !close(dupfd));
    puts("KEEPALIVE CASE defaults bounds shared FD independent enable PASS");
}
static void lengths(void)
{
    int fd=socket(AF_INET,SOCK_STREAM,0);CHECK(fd>=0);
    long page=sysconf(_SC_PAGESIZE);CHECK(page>0);
    void *bad=mmap(NULL,(size_t)page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(bad!=MAP_FAILED);
    for(unsigned i=0;i<3;i++) {
        int value=0x12345678;
        set(fd,names[i],37);
        for(socklen_t n=0;n<4;n++) {
            errno=0;CHECK(setsockopt(fd,IPPROTO_TCP,names[i],&value,n)==-1 && errno==EINVAL);
            socklen_t length=n;unsigned char bytes[8];memset(bytes,0xa5,sizeof(bytes));
            CHECK(!getsockopt(fd,IPPROTO_TCP,names[i],bytes,&length) && length==n);
            int expected=37;CHECK(!memcmp(bytes,&expected,n));
            for(unsigned k=n;k<sizeof(bytes);k++)CHECK(bytes[k]==0xa5);
        }
        socklen_t length=0;CHECK(!getsockopt(fd,IPPROTO_TCP,names[i],bad,&length) && !length);
        length=99;errno=0;CHECK(getsockopt(fd,IPPROTO_TCP,names[i],bad,&length)==-1 && errno==EFAULT && length==4);
        errno=0;CHECK(getsockopt(fd,IPPROTO_TCP,names[i],&value,bad)==-1 && errno==EFAULT);
        errno=0;CHECK(setsockopt(fd,IPPROTO_TCP,names[i],bad,4)==-1 && errno==EFAULT);
        length=(socklen_t)-1;value=-1;CHECK(getsockopt(fd,IPPROTO_TCP,names[i],&value,&length)==-1 && errno==EINVAL);
        value=41;CHECK(setsockopt(fd,IPPROTO_TCP,names[i],&value,(socklen_t)-1)==-1 && errno==EINVAL && get(fd,names[i])==37);
    }
    CHECK(!munmap(bad,(size_t)page) && !close(fd));
    puts("KEEPALIVE CASE short lengths fault ordering negative lengths PASS");
}
static void inheritance(int family)
{
    struct sockaddr_storage storage={0};socklen_t size;
    if(family==AF_INET) {
        struct sockaddr_in *a=(void *)&storage;a->sin_family=AF_INET;a->sin_addr.s_addr=htonl(INADDR_LOOPBACK);size=sizeof(*a);
    } else {
        struct sockaddr_in6 *a=(void *)&storage;a->sin6_family=AF_INET6;a->sin6_addr=in6addr_loopback;size=sizeof(*a);
    }
    int listener=socket(family,SOCK_STREAM,0);CHECK(listener>=0);
    for(unsigned i=0;i<3;i++)set(listener,names[i],10+(int)i);
    CHECK(!bind(listener,(void *)&storage,size) && !listen(listener,1) && !getsockname(listener,(void *)&storage,&size));
    for(unsigned i=0;i<3;i++){CHECK(get(listener,names[i])==10+(int)i);set(listener,names[i],20+(int)i);}
    int client=socket(family,SOCK_STREAM,0);CHECK(client>=0 && !connect(client,(void *)&storage,size));
    int server=accept(listener,NULL,NULL);CHECK(server>=0);
    for(unsigned i=0;i<3;i++) {
        CHECK(get(server,names[i])==20+(int)i);set(listener,names[i],30+(int)i);
        CHECK(get(server,names[i])==20+(int)i);set(server,names[i],40+(int)i);
        CHECK(get(listener,names[i])==30+(int)i && get(server,names[i])==40+(int)i);
    }
    char byte;CHECK(!shutdown(client,SHUT_WR) && !recv(server,&byte,1,0));
    CHECK(!shutdown(server,SHUT_WR) && !recv(client,&byte,1,0));
    CHECK(!close(client) && !close(server) && !close(listener));
    puts(family==AF_INET?"KEEPALIVE CASE IPv4 listener inheritance PASS":"KEEPALIVE CASE IPv6 listener inheritance PASS");
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);alarm(30);values();lengths();inheritance(AF_INET);inheritance(AF_INET6);
    int udp=socket(AF_INET,SOCK_DGRAM,0),unixfd=socket(AF_UNIX,SOCK_STREAM,0);CHECK(udp>=0);
    if(getpid()==1)CHECK(unixfd>=0);
    for(unsigned i=0;i<3;i++) {
        int value=1;socklen_t size=sizeof(value);errno=0;
        CHECK(setsockopt(udp,IPPROTO_TCP,names[i],&value,size)==-1 && errno==ENOPROTOOPT);
        CHECK(getsockopt(udp,IPPROTO_TCP,names[i],&value,&size)==-1 && errno==EOPNOTSUPP /* Linux UDP rejects level */);
        if(unixfd>=0)CHECK(setsockopt(unixfd,IPPROTO_TCP,names[i],&value,size)==-1 && errno==EOPNOTSUPP);
    }
    CHECK(!close(udp));if(unixfd>=0)CHECK(!close(unixfd));puts("KEEPALIVE ALL PASS");
    if(getpid()==1)reboot(RB_POWER_OFF);
    return 0;
}
