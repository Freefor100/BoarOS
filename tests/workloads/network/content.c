#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"network-content:%d %s errno=%d\n",__LINE__,#x,errno);exit(1); } }while(0)
static unsigned char pattern(unsigned id, size_t offset)
{ unsigned word=(id<<28)|(unsigned)(offset/4);return (unsigned char)(word>>((offset%4)*8)); }
static long long ns(void)
{ struct timespec t;CHECK(clock_gettime(CLOCK_MONOTONIC,&t)==0);return (long long)t.tv_sec*1000000000+t.tv_nsec; }
static void wait_ok(pid_t child)
{ int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0); }
static void tcp_content(unsigned workers, size_t bytes)
{
    int listener=socket(AF_INET6,SOCK_STREAM,0);CHECK(listener>=0);
    struct sockaddr_in6 address6={.sin6_family=AF_INET6};
    CHECK(bind(listener,(void *)&address6,sizeof(address6))==0 && listen(listener,8)==0);
    socklen_t length=sizeof(address6);CHECK(getsockname(listener,(void *)&address6,&length)==0);
    int ready[2],gate[2];CHECK(pipe(ready)==0 && pipe(gate)==0);
    pid_t server=fork();CHECK(server>=0);
    if(!server){
        close(ready[0]);close(ready[1]);close(gate[0]);close(gate[1]);pid_t readers[5];
        for(unsigned i=0;i<workers;i++){
            int peer=accept(listener,NULL,NULL);CHECK(peer>=0);readers[i]=fork();CHECK(readers[i]>=0);
            if(!readers[i]){
                close(listener);unsigned id;CHECK(read(peer,&id,sizeof(id))==sizeof(id));
                unsigned char data[8192];size_t done=0;unsigned long long checksum=0;
                while(done<bytes){ssize_t got=read(peer,data,sizeof(data));CHECK(got>0 && (size_t)got<=bytes-done);
                    for(ssize_t j=0;j<got;j++){CHECK(data[j]==pattern(id,done+(size_t)j));checksum+=data[j];}done+=(size_t)got;}
                CHECK(read(peer,data,1)==0 && write(peer,&checksum,sizeof(checksum))==sizeof(checksum));
                CHECK(shutdown(peer,SHUT_WR)==0 && close(peer)==0);_exit(0);
            }
            CHECK(close(peer)==0);
        }
        CHECK(close(listener)==0);for(unsigned i=0;i<workers;i++)wait_ok(readers[i]);_exit(0);
    }
    pid_t clients[5];
    for(unsigned id=1;id<=workers;id++){
        clients[id-1]=fork();CHECK(clients[id-1]>=0);
        if(!clients[id-1]){
            close(listener);close(ready[0]);close(gate[1]);int socket4=socket(AF_INET,SOCK_STREAM,0);CHECK(socket4>=0);
            struct sockaddr_in address={.sin_family=AF_INET,.sin_port=address6.sin6_port,.sin_addr={htonl(INADDR_LOOPBACK)}};
            CHECK(connect(socket4,(void *)&address,sizeof(address))==0);
            CHECK(write(ready[1],"r",1)==1);char c;CHECK(read(gate[0],&c,1)==1);close(ready[1]);close(gate[0]);
            CHECK(write(socket4,&id,sizeof(id))==sizeof(id));
            unsigned char data[8192];size_t done=0;unsigned long long expected=0;
            while(done<bytes){size_t chunk=bytes-done;if(chunk>sizeof(data))chunk=sizeof(data);
                for(size_t j=0;j<chunk;j++){data[j]=pattern(id,done+j);expected+=data[j];}
                size_t sent=0;while(sent<chunk){ssize_t n=write(socket4,data+sent,chunk-sent);CHECK(n>0);sent+=(size_t)n;}done+=chunk;}
            CHECK(shutdown(socket4,SHUT_WR)==0);unsigned long long checksum=0;size_t got=0;
            while(got<sizeof(checksum)){ssize_t n=read(socket4,(char *)&checksum+got,sizeof(checksum)-got);CHECK(n>0);got+=(size_t)n;}
            CHECK(checksum==expected && read(socket4,data,1)==0 && close(socket4)==0);
            printf("NETWORK WORKER id=%u bytes=%zu checksum=%llu complete=1\n",id,bytes,checksum);_exit(0);
        }
    }
    close(listener);close(ready[1]);close(gate[0]);char c;
    for(unsigned i=0;i<workers;i++)CHECK(read(ready[0],&c,1)==1);
    long long start=ns();for(unsigned i=0;i<workers;i++)CHECK(write(gate[1],"g",1)==1);
    for(unsigned i=0;i<workers;i++)wait_ok(clients[i]);
    wait_ok(server);
    printf("NETWORK FIXED tcp workers=%u bytes_per_worker=%zu elapsed_ns=%lld\n",workers,bytes,ns()-start);
    close(ready[0]);close(gate[1]);
}
static void udp_rr(void)
{
    int server=socket(AF_INET6,SOCK_DGRAM,0),client=socket(AF_INET6,SOCK_DGRAM,0);CHECK(server>=0 && client>=0);
    struct sockaddr_in6 address={.sin6_family=AF_INET6,.sin6_addr=IN6ADDR_LOOPBACK_INIT},peer=address;
    CHECK(bind(server,(void *)&address,sizeof(address))==0 && bind(client,(void *)&peer,sizeof(peer))==0);
    socklen_t length=sizeof(address);CHECK(getsockname(server,(void *)&address,&length)==0);length=sizeof(peer);
    CHECK(getsockname(client,(void *)&peer,&length)==0 && connect(client,(void *)&address,sizeof(address))==0 && connect(server,(void *)&peer,sizeof(peer))==0);
    int gate[2];CHECK(pipe(gate)==0);pid_t responder=fork();CHECK(responder>=0);
    if(!responder){close(client);close(gate[1]);char c;CHECK(read(gate[0],&c,1)==1);close(gate[0]);
        for(unsigned i=0;i<10000;i++){unsigned words[16];CHECK(read(server,words,sizeof(words))==sizeof(words));
            for(unsigned j=0;j<16;j++){CHECK(words[j]==i*16+j);words[j]^=0xa53c1987U;}
            CHECK(write(server,words,sizeof(words))==sizeof(words));}CHECK(close(server)==0);_exit(0);}
    close(server);close(gate[0]);long long start=ns();CHECK(write(gate[1],"g",1)==1);close(gate[1]);
    for(unsigned i=0;i<10000;i++){unsigned words[16];for(unsigned j=0;j<16;j++)words[j]=i*16+j;
        CHECK(write(client,words,sizeof(words))==sizeof(words) && read(client,words,sizeof(words))==sizeof(words));
        for(unsigned j=0;j<16;j++)CHECK(words[j]==((i*16+j)^0xa53c1987U));}
    wait_ok(responder);CHECK(close(client)==0);printf("NETWORK FIXED udp transactions=10000 bytes=64 elapsed_ns=%lld\n",ns()-start);
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    int control=socket(AF_INET,SOCK_DGRAM,0);CHECK(control>=0);
    struct ifreq interface={.ifr_name="lo"};CHECK(ioctl(control,SIOCGIFFLAGS,&interface)==0);
    interface.ifr_flags|=IFF_UP;CHECK(ioctl(control,SIOCSIFFLAGS,&interface)==0 && close(control)==0);
    tcp_content(1,16U*1024*1024);tcp_content(5,8U*1024*1024);udp_rr();
    puts("NETWORK PASS content");return 0;
}
