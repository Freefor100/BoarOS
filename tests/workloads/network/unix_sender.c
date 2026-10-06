#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"unix-sender:%d %s errno=%d\n",__LINE__,#x,errno);exit(1);} } while(0)
static volatile sig_atomic_t pipe_signals;
static void on_pipe(int number){(void)number;pipe_signals++;}
static void size_option(int fd,int option,int value)
{ CHECK(setsockopt(fd,SOL_SOCKET,option,&value,sizeof(value))==0); }
static int option(int fd,int key)
{ int value=-1;socklen_t size=sizeof(value);CHECK(getsockopt(fd,SOL_SOCKET,key,&value,&size)==0 && size==sizeof(value));return value; }
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);signal(SIGPIPE,SIG_IGN);
    int pair[2];CHECK(socketpair(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK,0,pair)==0);
    printf("UNIX DEFAULT send=%d receive=%d\n",option(pair[0],SO_SNDBUF),option(pair[1],SO_RCVBUF));
    size_t length=65537;unsigned char *bytes=malloc(length),*received=malloc(length);CHECK(bytes && received);
    for(size_t i=0;i<length;i++)bytes[i]=(unsigned char)(i*17+9);
    size_option(pair[1],SO_RCVBUF,0);CHECK(option(pair[1],SO_RCVBUF)>0);
    CHECK(send(pair[0],bytes,length,0)==(ssize_t)length);
    CHECK(recv(pair[1],received,length,0)==(ssize_t)length && !memcmp(received,bytes,length));
    size_option(pair[0],SO_SNDBUF,4096);int send_limit=option(pair[0],SO_SNDBUF);CHECK(send_limit==8192);
    CHECK(send(pair[0],bytes,(size_t)send_limit-31,0)==-1 && errno==EMSGSIZE);
    CHECK(send(pair[0],bytes,(size_t)send_limit-32,0)==send_limit-32);
    CHECK(recv(pair[1],received,length,0)==send_limit-32 && !memcmp(received,bytes,(size_t)send_limit-32));
    size_option(pair[0],SO_SNDBUF,0);CHECK(option(pair[0],SO_SNDBUF)>0);
    unsigned queued=0;
    while(send(pair[0],bytes,0,0)==0){CHECK(++queued<100000);}
    CHECK(queued>0 && errno==EAGAIN);
    size_option(pair[1],SO_RCVBUF,1<<20);
    CHECK(send(pair[0],bytes,0,0)==-1 && errno==EAGAIN);
    for(unsigned i=0;i<queued;i++)CHECK(recv(pair[1],received,length,0)==0);
    CHECK(recv(pair[1],received,length,0)==-1 && errno==EAGAIN);
    struct pollfd ready={.fd=pair[0],.events=POLLOUT};CHECK(poll(&ready,1,0)==1 && (ready.revents&POLLOUT));
    long page=sysconf(_SC_PAGESIZE);CHECK(page>0);
    void *guard=mmap(0,(size_t)page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(guard!=MAP_FAILED);
    CHECK(send(pair[0],guard,1,0)==-1 && errno==EFAULT);
    CHECK(recv(pair[1],received,length,0)==-1 && errno==EAGAIN);
    CHECK(munmap(guard,(size_t)page)==0);
    size_option(pair[0],SO_SNDBUF,1<<20);
    CHECK(send(pair[0],bytes,length,0)==(ssize_t)length && close(pair[0])==0);
    CHECK(recv(pair[1],received,length,0)==(ssize_t)length && !memcmp(received,bytes,length));
    CHECK(close(pair[1])==0);free(bytes);free(received);
    for(int type=SOCK_STREAM;type<=SOCK_DGRAM;type++) {
        CHECK(socketpair(AF_UNIX,type|SOCK_NONBLOCK,0,pair)==0 && close(pair[1])==0);
        CHECK(write(pair[0],"x",1)==-1 && errno==(type==SOCK_STREAM ? EPIPE : ECONNREFUSED));
        CHECK(write(pair[0],"x",1)==-1 && errno==(type==SOCK_STREAM ? EPIPE : ENOTCONN));
        CHECK(close(pair[0])==0);
    }
    signal(SIGPIPE,on_pipe);
    for(int type=SOCK_STREAM;type<=SOCK_DGRAM;type++) {
        pipe_signals=0;
        CHECK(socketpair(AF_UNIX,type,0,pair)==0 && shutdown(pair[1],SHUT_RD)==0);
        CHECK(write(pair[0],"x",1)==-1 && errno==EPIPE && pipe_signals==(type==SOCK_STREAM));
        CHECK(close(pair[0])==0 && close(pair[1])==0);
    }
    puts("NETWORK PASS unix_sender");return 0;
}
