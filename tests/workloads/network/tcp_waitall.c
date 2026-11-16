#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"WAITALL FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);} } while(0)
struct result { int entered,done,error; ssize_t bytes; volatile sig_atomic_t signals; };
static struct result *state;
static void caught(int signal) { (void)signal;state->signals++; }
static void pair(int p[2])
{
    int listener=socket(AF_INET,SOCK_STREAM,0);CHECK(listener>=0);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_addr={htonl(INADDR_LOOPBACK)}};
    CHECK(!bind(listener,(void *)&address,sizeof(address)) && !listen(listener,1));
    socklen_t size=sizeof(address);CHECK(!getsockname(listener,(void *)&address,&size));
    p[0]=socket(AF_INET,SOCK_STREAM,0);CHECK(p[0]>=0 && !connect(p[0],(void *)&address,size));
    p[1]=accept(listener,NULL,NULL);CHECK(p[1]>=0 && !close(listener));
    int one=1;CHECK(!setsockopt(p[0],IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one)));
}
static void available(int fd)
{ struct pollfd p={.fd=fd,.events=POLLIN};CHECK(poll(&p,1,5000)==1 && (p.revents&POLLIN)); }
static void close_pair(int p[2])
{
    /* Finish the two FIN exchanges in order, so this receive contract does
     * not accumulate simultaneous-close PCBs between its independent cases. */
    unsigned char byte;struct timeval timeout={0};
    CHECK(!fcntl(p[1],F_SETFL,0) && !setsockopt(p[1],SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)));
    CHECK(!shutdown(p[0],SHUT_WR) && !recv(p[1],&byte,1,0));
    CHECK(!shutdown(p[1],SHUT_WR) && !recv(p[0],&byte,1,0));
    CHECK(!close(p[0]) && !close(p[1]));
}
static ssize_t receive(int fd,unsigned char *data,int flags,int vector)
{
    if(!vector)return recv(fd,data,4,flags);
    struct iovec iov[3]={{data,1},{data+1,0},{data+1,3}};
    struct msghdr message={.msg_iov=iov,.msg_iovlen=3};
    ssize_t n=recvmsg(fd,&message,flags);CHECK(n<0 || !message.msg_flags);return n;
}
static void immediate(void)
{
    for(int vector=0;vector<2;vector++)for(int nonblock=0;nonblock<2;nonblock++) {
        int p[2];pair(p);unsigned char data[4];int flags=MSG_WAITALL;
        if(nonblock)CHECK(!fcntl(p[1],F_SETFL,O_NONBLOCK));else flags|=MSG_DONTWAIT;
        CHECK(receive(p[1],data,flags,vector)==-1 && errno==EAGAIN);
        CHECK(send(p[0],"ab",2,0)==2);available(p[1]);
        CHECK(receive(p[1],data,flags|MSG_PEEK,vector)==2 && !memcmp(data,"ab",2));
        CHECK(receive(p[1],data,flags,vector)==2 && !memcmp(data,"ab",2));
        CHECK(receive(p[1],data,flags,vector)==-1 && errno==EAGAIN);close_pair(p);
    }
    int p[2];pair(p);unsigned char data[4];CHECK(send(p[0],"abcd",4,0)==4);available(p[1]);
    CHECK(recv(p[1],(void *)1,4,MSG_WAITALL)==-1 && errno==EFAULT);
    CHECK(recv(p[1],data,4,MSG_WAITALL)==4 && !memcmp(data,"abcd",4));
    CHECK(!shutdown(p[0],SHUT_WR));CHECK(!recv(p[1],data,4,MSG_WAITALL));close_pair(p);
    puts("WAITALL CASE nonblock-peek-first-fault-eof PASS");
}
static void split_case(unsigned mode,int vector)
{
    long page=sysconf(_SC_PAGESIZE);CHECK(page>0);
    unsigned char *map=mmap(NULL,3*(size_t)page,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);CHECK(map!=MAP_FAILED);
    state=(struct result *)map;unsigned char *data=mode==4?map+2*page-2:map+page;
    if(mode==4)CHECK(!mprotect(map+2*page,page,PROT_NONE));
    int p[2];pair(p);
    if(mode>=6) {struct timeval timeout={.tv_usec=100000};CHECK(!setsockopt(p[1],SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)));}
    if(mode!=7 && mode!=8)CHECK(send(p[0],"ab",2,0)==2);
    pid_t child=fork();CHECK(child>=0);
    if(!child) {
        CHECK(!close(p[0]));
        struct sigaction action={.sa_handler=caught,.sa_flags=mode==5?SA_RESTART:0};CHECK(!sigaction(SIGUSR1,&action,NULL));
        __atomic_store_n(&state->entered,1,__ATOMIC_RELEASE);
        state->bytes=receive(p[1],data,MSG_WAITALL|((mode==1 || mode==2)?MSG_PEEK:0),vector);state->error=errno;
        __atomic_store_n(&state->done,1,__ATOMIC_RELEASE);_exit(0);
    }
    if(mode!=7 && mode!=8) {
        /* Observe the kernel's actual first copy, not a pre-syscall ready flag.
         * The next action cannot make a single-prefix implementation pass. */
        while(__atomic_load_n(data+1,__ATOMIC_ACQUIRE)!='b' && !__atomic_load_n(&state->done,__ATOMIC_ACQUIRE))sched_yield();
        CHECK(data[0]=='a' && data[1]=='b');
        if(mode<6)CHECK(!__atomic_load_n(&state->done,__ATOMIC_ACQUIRE));
    }
    if(mode<=2 || mode==4) {
        if(mode==2) {unsigned char old[2];CHECK(recv(p[1],old,2,0)==2 && !memcmp(old,"ab",2));}
        CHECK(send(p[0],"cd",2,0)==2);
    } else if(mode==3)CHECK(!shutdown(p[0],SHUT_WR));
    else if(mode==5)CHECK(!kill(child,SIGUSR1));
    else if(mode==8) {
        while(!__atomic_load_n(&state->entered,__ATOMIC_ACQUIRE))sched_yield();
        while(!__atomic_load_n(&state->done,__ATOMIC_ACQUIRE)) {
            if(kill(child,SIGUSR1))CHECK(errno==ESRCH && __atomic_load_n(&state->done,__ATOMIC_ACQUIRE));
            sched_yield();
        }
    }
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    ssize_t expected=mode<=2?4:mode==7 || mode==8?-1:2;
    CHECK(state->bytes==expected);
    if(mode==7)CHECK(state->error==EAGAIN);
    if(mode==8)CHECK(state->error==EINTR && state->signals);
    if(mode==5)CHECK(state->signals);
    if(mode<=2)CHECK(!memcmp(data,"abcd",4));
    if(mode==1 || mode==2 || mode==4) {
        char remaining[4];unsigned count=mode==1?4:2;
        CHECK(recv(p[1],remaining,count,MSG_DONTWAIT)==count && !memcmp(remaining,mode==1?"abcd":"cd",count));
    }
    close_pair(p);CHECK(!munmap(map,3*(size_t)page));
    printf("WAITALL CASE split-%u-%s PASS\n",mode,vector?"recvmsg":"recv");
}
static void truncate_receive(void)
{
    int p[2];pair(p);CHECK(send(p[0],"ab",2,0)==2);available(p[1]);
    state=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);CHECK(state!=MAP_FAILED);
    pid_t child=fork();CHECK(child>=0);
    if(!child) {
        CHECK(!close(p[0]));
        state->bytes=recv(p[1],(void *)1,4,MSG_TRUNC|MSG_WAITALL);
        __atomic_store_n(&state->done,1,__ATOMIC_RELEASE);_exit(0);
    }
    unsigned char byte;ssize_t queued;
    do {queued=recv(p[1],&byte,1,MSG_PEEK|MSG_DONTWAIT);sched_yield();}
    while(queued==1 && !__atomic_load_n(&state->done,__ATOMIC_ACQUIRE));
    CHECK(queued==-1 && errno==EAGAIN && !__atomic_load_n(&state->done,__ATOMIC_ACQUIRE));
    CHECK(send(p[0],"cd",2,0)==2);
    int status;CHECK(waitpid(child,&status,0)==child && !status && state->bytes==4);
    CHECK(!munmap(state,4096));close_pair(p);puts("WAITALL CASE trunc-skips-invalid-buffer PASS");
}
static void larger_than_window(void)
{
    const size_t size=131072;unsigned char *data=malloc(size);CHECK(data);
    for(size_t i=0;i<size;i++)data[i]=(unsigned char)(i%251);
    int p[2];pair(p);pid_t child=fork();CHECK(child>=0);
    if(!child) {
        CHECK(!close(p[0]));memset(data,0,size);
        struct iovec iov[2]={{data,123},{data+123,size-123}};
        struct msghdr message={.msg_iov=iov,.msg_iovlen=2};
        CHECK(recvmsg(p[1],&message,MSG_WAITALL)==(ssize_t)size);
        for(size_t i=0;i<size;i++)CHECK(data[i]==(unsigned char)(i%251));
        _exit(0);
    }
    for(size_t sent=0;sent<size;) {ssize_t n=send(p[0],data+sent,size-sent,0);CHECK(n>0);sent+=(size_t)n;}
    int status;CHECK(waitpid(child,&status,0)==child && !status);close_pair(p);free(data);
    puts("WAITALL CASE larger-than-receive-window PASS");
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);alarm(30);immediate();
    for(int vector=0;vector<2;vector++)for(unsigned mode=0;mode<9;mode++)split_case(mode,vector);
    truncate_receive();larger_than_window();
    puts("WAITALL ALL PASS");
    if(getpid()==1) {CHECK(!reboot(RB_POWER_OFF));for(;;)pause();}
    return 0;
}
