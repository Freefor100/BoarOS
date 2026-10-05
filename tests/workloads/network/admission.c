#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"admission:%d %s errno=%d\n",__LINE__,#x,errno); exit(1); } } while(0)
static volatile sig_atomic_t pipes;
static void on_pipe(int sig) { (void)sig; pipes++; }
static void pair(int fds[2])
{
    int listener=socket(AF_INET,SOCK_STREAM,0); CHECK(listener>=0);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_addr={htonl(INADDR_LOOPBACK)}};
    CHECK(bind(listener,(void *)&address,sizeof(address))==0 && listen(listener,2)==0);
    socklen_t len=sizeof(address); CHECK(getsockname(listener,(void *)&address,&len)==0);
    fds[0]=socket(AF_INET,SOCK_STREAM,0); CHECK(fds[0]>=0 && connect(fds[0],(void *)&address,len)==0);
    fds[1]=accept(listener,0,0); CHECK(fds[1]>=0 && close(listener)==0);
}
static void result(const char *name, ssize_t value) { printf("ADMISSION %s=%ld errno=%d pipes=%d\n",name,(long)value,value<0?errno:0,pipes); }
static void failure(const char *name, ssize_t value, int error) { result(name,value); CHECK(value==-1 && errno==error); }
static void wait_blocked(pid_t child)
{
    char path[64], state[512]; snprintf(path,sizeof(path),"/proc/%d/stat",child);
    for (unsigned i=0;;i++) {
        int proc=open(path,O_RDONLY); CHECK(proc>=0); ssize_t n=read(proc,state,sizeof(state)-1);
        CHECK(n>0 && close(proc)==0); state[n]=0; char *end=strrchr(state,')'); CHECK(end);
        if(end[2]=='S') return;
        CHECK(i<100000); sched_yield();
    }
}
static void partial_shutdown(unsigned reset)
{
    int fds[2]; pair(fds); int small=0;
    CHECK(setsockopt(fds[0],SOL_SOCKET,SO_SNDBUF,&small,sizeof(small))==0);
    char *bytes=malloc(1024*1024); CHECK(bytes); memset(bytes,'p',1024*1024);
    pid_t child=fork(); CHECK(child>=0);
    if (!child) {
        pipes=0; CHECK(close(fds[1])==0);
        ssize_t n=write(fds[0],bytes,1024*1024); result("partial-shutdown",n);
        CHECK(n>0 && n<1024*1024 && pipes==0);
        if (reset) { failure("reset-after-prefix",send(fds[0],"x",1,MSG_NOSIGNAL),ECONNRESET);
            failure("reset-error-consumed",send(fds[0],"x",1,MSG_NOSIGNAL),EPIPE); }
        _exit(0);
    }
    wait_blocked(child);
    if (reset) { /* 对端丢弃未读数据触发真实RST。 */ CHECK(close(fds[1])==0); fds[1]=-1; }
    else CHECK(shutdown(fds[0],SHUT_WR)==0);
    int status; CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    CHECK(close(fds[0])==0 && (fds[1]<0 || close(fds[1])==0)); free(bytes);
}
int main(void)
{
    setvbuf(stdout,0,_IONBF,0); signal(SIGPIPE,on_pipe);
    int init=socket(AF_INET,SOCK_DGRAM,0); CHECK(init>=0);
    if (getpid()==1) { struct ifreq ifr={.ifr_name="lo"}; CHECK(ioctl(init,SIOCGIFFLAGS,&ifr)==0); ifr.ifr_flags|=IFF_UP; CHECK(ioctl(init,SIOCSIFFLAGS,&ifr)==0); }
    CHECK(close(init)==0);
    if (getpid()==1) { CHECK(mkdir("/proc",0755)==0 || errno==EEXIST); CHECK(mount("proc","/proc","proc",0,0)==0 || errno==EBUSY); }
    char *map=mmap(0,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); CHECK(map!=MAP_FAILED);
    memset(map,'a',4096); CHECK(mprotect(map+4096,4096,PROT_NONE)==0);
    void *outside=(void *)(uintptr_t)UINTPTR_MAX;
    failure("send-range-before-fd",send(-1,outside,1,MSG_NOSIGNAL),EFAULT);
    failure("write-fd-before-range",write(-1,outside,1),EBADF);
    int fd=socket(AF_INET,SOCK_STREAM,0); CHECK(fd>=0);
    failure("range-before-state",send(fd,outside,1,MSG_NOSIGNAL),EFAULT);
    failure("header-before-state",syscall(SYS_sendmsg,fd,(void *)(map+4096),MSG_NOSIGNAL),EFAULT);
    struct iovec invalid={outside,1}; struct msghdr invalid_message={.msg_iov=&invalid,.msg_iovlen=1};
    failure("iov-range-before-state",sendmsg(fd,&invalid_message,MSG_NOSIGNAL),EFAULT);
    failure("fresh-fault",send(fd,map+4096,1,MSG_NOSIGNAL), EPIPE);
    failure("fresh-empty",send(fd,map+4096,0,MSG_NOSIGNAL), EPIPE); CHECK(close(fd)==0);
    int fds[2]; pair(fds);
    failure("first-fault",send(fds[0],map+4096,1,MSG_NOSIGNAL), EFAULT);
    for (unsigned vector=0; vector<2; vector++) {
        struct iovec iov[]={{map,4096},{map+4096,4096}};
        ssize_t n=vector ? writev(fds[0],iov,2) : send(fds[0],map,8192,MSG_NOSIGNAL);
        result(vector ? "vector-prefix" : "fault-prefix",n);
        /* 两栈的内部接纳片段不同：只验证返回前缀与接收字节严格守恒。 */
        CHECK((n<0 && errno==EFAULT) || (n>0 && n<=4096));
        char data[4096]; size_t got=0, accepted=n<0?0:(size_t)n;
        while(got<accepted) { n=read(fds[1],data+got,accepted-got); CHECK(n>0); got+=(size_t)n; }
        CHECK(!memcmp(data,map,accepted));
        CHECK(recv(fds[1],data,1,MSG_DONTWAIT)==-1 && errno==EAGAIN);
    }
    CHECK(shutdown(fds[0],SHUT_WR)==0);
    failure("closed-fault",send(fds[0],map+4096,1,MSG_NOSIGNAL), EPIPE);
    failure("closed-empty",send(fds[0],map+4096,0,MSG_NOSIGNAL), EPIPE);
    failure("closed-signal",write(fds[0],map+4096,1), EPIPE);
    CHECK(pipes==1 && close(fds[0])==0 && close(fds[1])==0);
    pair(fds); int small=0; CHECK(setsockopt(fds[0],SOL_SOCKET,SO_SNDBUF,&small,sizeof(small))==0);
    CHECK(fcntl(fds[0],F_SETFL,O_NONBLOCK)==0);
    /* 不假定两边协议窗口相同；先确实填满，再验证无接纳量与fault的优先级。 */
    for(unsigned i=0;i<100000;i++) {
        ssize_t sent=send(fds[0],map,4096,MSG_NOSIGNAL);
        if(sent<0) { CHECK(errno==EAGAIN); struct pollfd ready={.fd=fds[0],.events=POLLOUT}; if(poll(&ready,1,20)==0) break; }
        CHECK(i<99999);
    }
    failure("full-range",send(fds[0],outside,1,MSG_NOSIGNAL),EFAULT);
    failure("full-fault",send(fds[0],map+4096,1,MSG_NOSIGNAL), EAGAIN);
    failure("full-valid",send(fds[0],map,1,MSG_NOSIGNAL), EAGAIN);
    pid_t child=fork(); CHECK(child>=0);
    if (!child) {
        CHECK(close(fds[1])==0 && fcntl(fds[0],F_SETFL,0)==0);
        for (;;) CHECK(write(fds[0],map,4096)>0);
    }
    wait_blocked(child);
    CHECK(kill(child,SIGKILL)==0); int status;
    CHECK(waitpid(child,&status,0)==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);
    CHECK(close(fds[0])==0 && close(fds[1])==0 && munmap(map,8192)==0);
    partial_shutdown(0);
    partial_shutdown(1);
    puts("NETWORK PASS admission"); return 0;
}
