#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>

/* Raw native ABI works with the older libc shipped in the guest toolchain. */
struct message { struct msghdr header; unsigned length; unsigned padding; };
_Static_assert(sizeof(struct message)==64,"native mmsghdr");
_Static_assert(offsetof(struct message,length)==56,"native msg_len");
#ifndef SYS_sendmmsg
#define SYS_sendmmsg 269
#endif
#ifndef SYS_recvmmsg
#define SYS_recvmmsg 243
#endif
#ifndef MSG_WAITFORONE
#define MSG_WAITFORONE 0x10000
#endif
#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0x40000000
#endif
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"MMSG FAIL line=%d %s errno=%d\n",__LINE__,#x,errno); exit(1); } } while (0)
static unsigned cases;
static void passed(const char *name) { ++cases; printf("MMSG CASE %s PASS\n",name); }
static long send_many(int fd,struct message *m,unsigned n,unsigned flags)
{ return syscall(SYS_sendmmsg,fd,m,n,flags); }
static long receive_many(int fd,struct message *m,unsigned n,unsigned flags,struct timespec *time)
{ return syscall(SYS_recvmmsg,fd,m,n,flags,time); }
static struct message message(void *data,size_t size,struct iovec *iov)
{
    *iov=(struct iovec){data,size};
    return (struct message){.header={.msg_iov=iov,.msg_iovlen=1},.length=0xdeadbeef};
}
static void close_pair(int p[2]) { CHECK(!close(p[0]) && !close(p[1])); }
static void validation(void)
{
    struct timespec bad={.tv_sec=-1},time={.tv_sec=1,.tv_nsec=123};
    CHECK(send_many(-1,0,0,0)==-1 && errno==EBADF);
    CHECK(receive_many(-1,0,0,0,0)==-1 && errno==EBADF);
#if defined(__riscv) || defined(__loongarch__)
    /* Fixed RV/LA Linux has CONFIG_COMPAT=n: MSG_CMSG_COMPAT is zero. */
    CHECK(send_many(-1,0,0,0x80000000U)==-1 && errno==EBADF);
    CHECK(receive_many(-1,0,0,0x80000000U,(void *)1)==-1 && errno==EFAULT);
#else
    /* The x86-64 host kernel enables CONFIG_COMPAT and reserves this bit. */
    CHECK(send_many(-1,0,0,0x80000000U)==-1 && errno==EINVAL);
    CHECK(receive_many(-1,0,0,0x80000000U,(void *)1)==-1 && errno==EINVAL);
#endif
    CHECK(receive_many(-1,0,0,0,&bad)==-1 && errno==EINVAL);
    CHECK(receive_many(-1,0,0,0,(void *)1)==-1 && errno==EFAULT);
    int p[2];CHECK(!pipe(p));
    CHECK(send_many(p[0],0,0,0)==-1 && errno==ENOTSOCK);
    CHECK(receive_many(p[0],0,0,0,0)==-1 && errno==ENOTSOCK);close_pair(p);
    CHECK(!socketpair(AF_UNIX,SOCK_DGRAM,0,p));
#if defined(__riscv) || defined(__loongarch__)
    struct message native={0};
    CHECK(send_many(p[0],&native,1,0x80000000U)==1 && !native.length);
    CHECK(receive_many(p[1],&native,1,0x80000000U|MSG_DONTWAIT,0)==1 && !native.length);
#endif
    CHECK(!send_many(p[0],(void *)1,0,0));
    CHECK(!receive_many(p[0],(void *)1,0,0,&time) && time.tv_sec==1 && time.tv_nsec==123);
    CHECK(receive_many(p[0],(void *)1,UINT_MAX,MSG_DONTWAIT,0)==-1 && errno==EFAULT);
    close_pair(p);passed("abi-fd-flags-zero-vlen-timeout-validation");
}
static void connected_batch(int p[2],int stream)
{
    struct iovec siov[3],riov[3];char sent[]="abc",received[3]={0};
    struct message senders[3],receivers[3];
    for(unsigned i=0;i<3;i++) { senders[i]=message(sent+i,1,&siov[i]); senders[i].header.msg_flags=MSG_EOR; receivers[i]=message(received+i,1,&riov[i]); }
    CHECK(send_many(p[0],senders,3,0)==3);
    for(unsigned i=0;i<3;i++)CHECK(senders[i].length==1);
    /* Protocol delivery can arrive after the first successful receive;
     * WAITFORONE legitimately returns that ready prefix. */
    for(unsigned completed=0;completed<3;) {
        long n=receive_many(p[1],receivers+completed,3-completed,MSG_WAITFORONE,0);
        CHECK(n>0 && (unsigned long)n<=3-completed);completed+=(unsigned)n;
    }
    CHECK(!memcmp(sent,received,3));
    for(unsigned i=0;i<3;i++)CHECK(receivers[i].length==1 && !receivers[i].header.msg_flags);
    CHECK(receive_many(p[1],receivers,3,MSG_DONTWAIT,0)==-1 && errno==EAGAIN);
    if(!stream) {
        siov[0].iov_len=0;CHECK(send_many(p[0],senders,1,0)==1 && !senders[0].length);
        CHECK(receive_many(p[1],receivers,3,MSG_WAITFORONE,0)==1 && !receivers[0].length);
    }
}
static void families(void)
{
    const int types[]={SOCK_STREAM,SOCK_DGRAM,SOCK_SEQPACKET};
    for(unsigned i=0;i<3;i++) { int p[2];CHECK(!socketpair(AF_UNIX,types[i],0,p));connected_batch(p,types[i]==SOCK_STREAM);close_pair(p); }
    passed("unix-stream-dgram-seqpacket-prefix-waitforone-empty");
    int udp[2];struct sockaddr_in address={.sin_family=AF_INET,.sin_addr={htonl(INADDR_LOOPBACK)}};
    udp[1]=socket(AF_INET,SOCK_DGRAM,0);udp[0]=socket(AF_INET,SOCK_DGRAM,0);CHECK(udp[0]>=0 && udp[1]>=0);
    CHECK(!bind(udp[1],(void *)&address,sizeof(address)));socklen_t n=sizeof(address);CHECK(!getsockname(udp[1],(void *)&address,&n));
    CHECK(!connect(udp[0],(void *)&address,n));connected_batch(udp,0);close_pair(udp);
    int listener=socket(AF_INET,SOCK_STREAM,0);CHECK(listener>=0);address.sin_port=0;
    CHECK(!bind(listener,(void *)&address,sizeof(address)) && !listen(listener,1));n=sizeof(address);CHECK(!getsockname(listener,(void *)&address,&n));
    int tcp[2];tcp[0]=socket(AF_INET,SOCK_STREAM,0);CHECK(tcp[0]>=0 && !connect(tcp[0],(void *)&address,n));tcp[1]=accept(listener,0,0);CHECK(tcp[1]>=0);
    connected_batch(tcp,1);close_pair(tcp);CHECK(!close(listener));passed("udp-tcp-loopback");
}
static void names_peek_trunc(void)
{
    int p[2];CHECK(!socketpair(AF_UNIX,SOCK_DGRAM,0,p));CHECK(send(p[0],"abcdef",6,0)==6);
    char data[2][2]={{0}};struct iovec iov[2];struct message m[2];
    for(int i=0;i<2;i++)m[i]=message(data[i],2,&iov[i]);
    CHECK(receive_many(p[1],m,2,MSG_PEEK|MSG_TRUNC|MSG_DONTWAIT,0)==2);
    for(int i=0;i<2;i++)CHECK(m[i].length==6 && (m[i].header.msg_flags&MSG_TRUNC) && !memcmp(data[i],"ab",2));
    CHECK(receive_many(p[1],m,2,MSG_DONTWAIT,0)==1 && m[0].length==2 && (m[0].header.msg_flags&MSG_TRUNC));
    close_pair(p);
    int sender=socket(AF_INET,SOCK_DGRAM,0),receiver=socket(AF_INET,SOCK_DGRAM,0);CHECK(sender>=0 && receiver>=0);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_addr={htonl(INADDR_LOOPBACK)}};socklen_t n=sizeof(address);
    CHECK(!bind(receiver,(void *)&address,n) && !getsockname(receiver,(void *)&address,&n));
    m[0]=message("xy",2,&iov[0]);m[0].header.msg_name=&address;m[0].header.msg_namelen=n;
    CHECK(send_many(sender,m,1,0)==1 && m[0].length==2);
    struct sockaddr_in source={0};m[0]=message(data[0],2,&iov[0]);m[0].header.msg_name=&source;m[0].header.msg_namelen=sizeof(source);
    CHECK(receive_many(receiver,m,1,0,0)==1 && m[0].length==2 && source.sin_family==AF_INET && source.sin_port && m[0].header.msg_namelen==sizeof(source));
    CHECK(!close(sender) && !close(receiver));passed("peek-trunc-name-input-output");
}
static void prefix_and_lazy_faults(void)
{
    int p[2];CHECK(!socketpair(AF_UNIX,SOCK_DGRAM,0,p));char data[2]={0};struct iovec iov[2];struct message m[2];
    m[0]=message("a",1,&iov[0]);m[1]=message("b",1,&iov[1]);m[1].header.msg_iov=(void *)1;
    CHECK(send_many(p[0],m,2,0)==1 && m[0].length==1 && m[1].length==0xdeadbeef);
    CHECK(recv(p[1],data,2,MSG_DONTWAIT)==1 && data[0]=='a');
    CHECK(recv(p[1],data,2,MSG_DONTWAIT)==-1 && errno==EAGAIN);
    CHECK(send(p[0],"a",1,0)==1 && send(p[0],"b",1,0)==1);
    m[0]=message(data,1,&iov[0]);m[1]=message(data+1,1,&iov[1]);m[1].header.msg_iov=(void *)1;
    CHECK(receive_many(p[1],m,2,MSG_DONTWAIT,0)==1 && m[0].length==1 && m[1].length==0xdeadbeef);
    /* The failed later entry remains pending, and is consumed before input. */
    CHECK(receive_many(p[1],(void *)1,1,MSG_DONTWAIT,0)==-1 && errno==EFAULT);
    CHECK(recv(p[1],data,2,MSG_DONTWAIT)==1 && data[0]=='b');
    CHECK(send(p[0],"c",1,0)==1 && send(p[0],"d",1,0)==1);
    CHECK(receive_many(p[1],m,2,MSG_DONTWAIT,0)==1);
    int error=0;socklen_t n=sizeof(error);CHECK(!getsockopt(p[1],SOL_SOCKET,SO_ERROR,&error,&n) && error==EFAULT);
    CHECK(recv(p[1],data,2,MSG_DONTWAIT)==1 && data[0]=='d');
    /* The first payload constructs the next header. Eager vector import fails. */
    struct iovec tail_iov={data,1};struct message tail={.header={.msg_iov=&tail_iov,.msg_iovlen=1}};
    memset(m,0,sizeof(m));m[0]=message(&m[1],sizeof(m[1]),&iov[0]);
    CHECK(send(p[0],&tail,sizeof(tail),0)==sizeof(tail) && send(p[0],"z",1,0)==1);
    CHECK(receive_many(p[1],m,2,MSG_DONTWAIT,0)==2 && m[0].length==sizeof(tail) && m[1].length==1 && data[0]=='z');
    close_pair(p);passed("completed-prefix-saved-error-lazy-entry-import");
}
static void timeout_points(void)
{
    int p[2];CHECK(!socketpair(AF_UNIX,SOCK_DGRAM,0,p));char byte;struct iovec iov[2];struct message m[2];
    for(int i=0;i<2;i++)m[i]=message(&byte,1,&iov[i]);
    struct timespec timeout={0};CHECK(send(p[0],"a",1,0)==1 && send(p[0],"b",1,0)==1);
    CHECK(receive_many(p[1],m,2,0,&timeout)==1 && !timeout.tv_sec && !timeout.tv_nsec);
    CHECK(recv(p[1],&byte,1,MSG_DONTWAIT)==1 && byte=='b');
    struct timeval socket_timeout={.tv_usec=30000};CHECK(!setsockopt(p[1],SOL_SOCKET,SO_RCVTIMEO,&socket_timeout,sizeof(socket_timeout)));
    timeout=(struct timespec){.tv_sec=1,.tv_nsec=456};struct timespec original=timeout;
    CHECK(receive_many(p[1],m,2,0,&timeout)==-1 && errno==EAGAIN && !memcmp(&timeout,&original,sizeof(timeout)));
    CHECK(send(p[0],"c",1,0)==1);timeout=original;
    CHECK(receive_many(p[1],m,2,0,&timeout)==1 && timeout.tv_sec==0 && timeout.tv_nsec>0 && timeout.tv_nsec<1000000000);
    close_pair(p);passed("timeout-after-entry-socket-timeout-remaining");
}
static int control_fd(struct msghdr *header)
{
    int result=-1,credentials=0;
    for(struct cmsghdr *c=CMSG_FIRSTHDR(header);c;c=CMSG_NXTHDR(header,c)) {
        CHECK(c->cmsg_level==SOL_SOCKET);
        if(c->cmsg_type==SCM_RIGHTS) { CHECK(c->cmsg_len==CMSG_LEN(sizeof(int)));memcpy(&result,CMSG_DATA(c),sizeof(result)); }
        if(c->cmsg_type==SCM_CREDENTIALS) { struct ucred cred;memcpy(&cred,CMSG_DATA(c),sizeof(cred));CHECK(cred.pid==getpid() && cred.uid==getuid() && cred.gid==getgid());credentials=1; }
    }
    CHECK(result>=0 && credentials);return result;
}
static void control_batch(void)
{
    for(int type=SOCK_DGRAM;type<=SOCK_SEQPACKET;type+=SOCK_SEQPACKET-SOCK_DGRAM) {
        int p[2],enabled=1;CHECK(!socketpair(AF_UNIX,type,0,p) && !setsockopt(p[1],SOL_SOCKET,SO_PASSCRED,&enabled,sizeof(enabled)));
        int original=open("/dev/null",O_RDONLY);CHECK(original>=0);
        union { struct cmsghdr align;char bytes[CMSG_SPACE(sizeof(int))]; } send_control[2];
        union { struct cmsghdr align;char bytes[128]; } recv_control[2];
        struct message m[2];struct iovec iov[2];char bytes[2]={'x','y'};
        for(int i=0;i<2;i++) { m[i]=message(bytes+i,1,&iov[i]);memset(&send_control[i],0,sizeof(send_control[i]));m[i].header.msg_control=send_control[i].bytes;m[i].header.msg_controllen=sizeof(send_control[i]);struct cmsghdr *c=CMSG_FIRSTHDR(&m[i].header);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(c),&original,sizeof(original)); }
        CHECK(send_many(p[0],m,2,0)==2);
        for(int i=0;i<2;i++) { m[i]=message(bytes+i,1,&iov[i]);m[i].header.msg_control=recv_control[i].bytes;m[i].header.msg_controllen=sizeof(recv_control[i]); }
        CHECK(receive_many(p[1],m,2,MSG_CMSG_CLOEXEC,0)==2);
        for(int i=0;i<2;i++) { int fd=control_fd(&m[i].header);CHECK(fcntl(fd,F_GETFD)&FD_CLOEXEC);char byte;CHECK(read(fd,&byte,1)==0 && !close(fd)); }
        CHECK(!close(original));close_pair(p);
    }
    passed("scm-rights-credentials-cloexec-per-entry-release");
}
static void output_faults(void)
{
    long page=sysconf(_SC_PAGESIZE);CHECK(page>0);
    char *region=mmap(0,(size_t)page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(region!=MAP_FAILED);
    struct message *m=(void *)(region+page-56);struct iovec iov;char byte=0;int p[2];CHECK(!socketpair(AF_UNIX,SOCK_DGRAM,0,p));
    *m=message("s",1,&iov);CHECK(!mprotect(region+page,(size_t)page,PROT_READ));
    CHECK(send_many(p[0],m,1,0)==-1 && errno==EFAULT && recv(p[1],&byte,1,MSG_DONTWAIT)==1 && byte=='s');
    CHECK(!mprotect(region+page,(size_t)page,PROT_READ|PROT_WRITE));*m=message(&byte,1,&iov);
    CHECK(send(p[0],"r",1,0)==1 && !mprotect(region+page,(size_t)page,PROT_READ));
    CHECK(receive_many(p[1],m,1,MSG_DONTWAIT,0)==-1 && errno==EFAULT && byte=='r');
    CHECK(recv(p[1],&byte,1,MSG_DONTWAIT)==-1 && errno==EAGAIN);
    CHECK(!mprotect(region+page,(size_t)page,PROT_READ|PROT_WRITE));
    struct timespec *time=(void *)(region+page);*time=(struct timespec){.tv_sec=1};struct message writable=message(&byte,1,&iov);
    CHECK(!mprotect(region+page,(size_t)page,PROT_READ) && send(p[0],"t",1,0)==1);
    CHECK(receive_many(p[1],&writable,1,0,time)==-1 && errno==EFAULT && writable.length==1 && byte=='t');
    CHECK(!munmap(region,(size_t)page*2));close_pair(p);passed("msg-len-send-recv-timeout-output-fault-consumption");
}
static void vector_limits(void)
{
    int p[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,p));
    struct message *m=calloc(1025,sizeof(*m));CHECK(m);
    for(unsigned i=0;i<1025;i++)m[i].length=0xdeadbeef;
    CHECK(send_many(p[0],m,1025,0)==1024);
    for(unsigned i=0;i<1024;i++)CHECK(!m[i].length);
    CHECK(m[1024].length==0xdeadbeef && !shutdown(p[0],SHUT_WR));
    /* recv does not share send's UIO_MAXIOV cap. EOF supplies zero bytes. */
    CHECK(receive_many(p[1],m,1025,0,0)==1025);
    for(unsigned i=0;i<1025;i++)CHECK(!m[i].length);
    free(m);close_pair(p);passed("send-vlen-clamp-receive-unsigned-vlen");
}
static void control_output_faults(void)
{
    long page=sysconf(_SC_PAGESIZE);CHECK(page>0);
    char *region=mmap(0,(size_t)page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(region!=MAP_FAILED);
    int original=open("/dev/null",O_RDONLY);CHECK(original>=0);
    int p[2],enabled=1;CHECK(!socketpair(AF_UNIX,SOCK_DGRAM,0,p) && !setsockopt(p[1],SOL_SOCKET,SO_PASSCRED,&enabled,sizeof(enabled)));
    for(int whole=0;whole<2;whole++) {
        union { struct cmsghdr align;char bytes[CMSG_SPACE(sizeof(int))]; } send_control={0};
        union { struct cmsghdr align;char bytes[128]; } recv_control={0};
        char byte=0;struct iovec siov,riov;struct message send_header=message("f",1,&siov);
        send_header.header.msg_control=send_control.bytes;send_header.header.msg_controllen=sizeof(send_control);
        struct cmsghdr *c=CMSG_FIRSTHDR(&send_header.header);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(c),&original,sizeof(original));
        CHECK(send_many(p[0],&send_header,1,0)==1);
        struct message *m=(void *)(region+(whole?0:page-56));*m=message(&byte,1,&riov);
        m->header.msg_control=recv_control.bytes;m->header.msg_controllen=sizeof(recv_control);
        CHECK(!mprotect(region+(whole?0:page),(size_t)page,PROT_READ));
        CHECK(receive_many(p[1],m,1,MSG_CMSG_CLOEXEC,0)==-1 && errno==EFAULT && byte=='f');
        /* Installed rights survive either outer-header or msg_len fault. */
        struct msghdr output={.msg_control=recv_control.bytes,
            .msg_controllen=CMSG_SPACE(sizeof(int))+CMSG_SPACE(sizeof(struct ucred))};
        int fd=control_fd(&output);CHECK(fcntl(fd,F_GETFD)&FD_CLOEXEC);CHECK(!close(fd));
        CHECK(recv(p[1],&byte,1,MSG_DONTWAIT)==-1 && errno==EAGAIN);
        CHECK(!mprotect(region+(whole?0:page),(size_t)page,PROT_READ|PROT_WRITE));
    }
    CHECK(!close(original) && !munmap(region,(size_t)page*2));close_pair(p);passed("scm-installed-rights-survive-msghdr-msg-len-faults");
}
struct blocked_batch { int fd,writing;uint64_t tid;char data[2];long result;int error; };
static void *blocked_receiver(void *arg)
{
    struct blocked_batch *batch=arg;struct message m[2];struct iovec iov[2];
    for(int i=0;i<2;i++)m[i]=message(batch->data+i,1,&iov[i]);
    __atomic_store_n(&batch->tid,(uint64_t)syscall(SYS_gettid),__ATOMIC_RELEASE);
    batch->result=batch->writing?send_many(batch->fd,m,2,MSG_NOSIGNAL):receive_many(batch->fd,m,2,0,0);batch->error=errno;
    return 0;
}
static void wait_blocked(uint64_t *tid)
{
    uint64_t id;while(!(id=__atomic_load_n(tid,__ATOMIC_ACQUIRE)))sched_yield();
    char path[80],text[512];snprintf(path,sizeof(path),"/proc/self/task/%llu/stat",(unsigned long long)id);
    for(unsigned i=0;i<1000000;i++) {
        int fd=open(path,O_RDONLY);CHECK(fd>=0);ssize_t n=read(fd,text,sizeof(text)-1);CHECK(n>0 && !close(fd));text[n]=0;
        char *end=strrchr(text,')');CHECK(end);if(end[2]=='S')return;sched_yield();
    }
    CHECK(0);
}
static void pinned_descriptor(void)
{
    int old[2],replacement[2];CHECK(!socketpair(AF_UNIX,SOCK_DGRAM,0,old) && !socketpair(AF_UNIX,SOCK_DGRAM,0,replacement));
    struct blocked_batch batch={.fd=old[1]};pthread_t thread;
    CHECK(send(old[0],"a",1,0)==1 && !pthread_create(&thread,0,blocked_receiver,&batch));
    wait_blocked(&batch.tid);
    /* S means this batch imported its first entry and is waiting on the old
     * socket. Replace the shared fd slot before releasing the second entry. */
    CHECK(dup2(replacement[1],old[1])==old[1]);
    CHECK(send(replacement[0],"x",1,0)==1 && send(old[0],"b",1,0)==1);
    CHECK(!pthread_join(thread,0) && batch.result==2 && batch.data[0]=='a' && batch.data[1]=='b');
    char byte;CHECK(recv(old[1],&byte,1,MSG_DONTWAIT)==1 && byte=='x');
    close_pair(old);close_pair(replacement);passed("batch-holds-original-ofd-across-fd-replacement");
}
static volatile sig_atomic_t signals_seen;
static void interrupted(int signal_number)
{ CHECK(signal_number==SIGUSR1);++signals_seen; }
static void signal_prefix(void)
{
    struct sigaction original,action={.sa_handler=interrupted};sigemptyset(&action.sa_mask);
    for(int writing=0;writing<2;writing++)for(int restart=0;restart<2;restart++) {
        action.sa_flags=restart?SA_RESTART:0;CHECK(!sigaction(SIGUSR1,&action,&original));
        int p[2];CHECK(!socketpair(AF_UNIX,SOCK_DGRAM,0,p));
        struct blocked_batch batch={.fd=writing?p[0]:p[1],.writing=writing,.data={'a','b'}};
        unsigned queued=0;char byte='q';
        if(writing) {
            int budget=4096;CHECK(!setsockopt(p[0],SOL_SOCKET,SO_SNDBUF,&budget,sizeof(budget)));
            while(send(p[0],&byte,1,MSG_DONTWAIT|MSG_NOSIGNAL)==1)CHECK(++queued<100000);
            CHECK(errno==EAGAIN && queued && recv(p[1],&byte,1,0)==1 && byte=='q');--queued;
        } else CHECK(send(p[0],"a",1,0)==1);
        sig_atomic_t before=signals_seen;pthread_t thread;
        CHECK(!pthread_create(&thread,0,blocked_receiver,&batch));wait_blocked(&batch.tid);
        CHECK(!pthread_kill(thread,SIGUSR1) && !pthread_join(thread,0));
        CHECK(batch.result==1 && signals_seen==before+1);
        if(writing) {
            unsigned first=0,total=0;
            while(recv(p[1],&byte,1,MSG_DONTWAIT)==1) { CHECK(byte=='q' || byte=='a');first+=byte=='a';++total; }
            CHECK(errno==EAGAIN && first==1 && total==queued+1);
        } else CHECK(batch.data[0]=='a');
        close_pair(p);CHECK(!sigaction(SIGUSR1,&original,0));
    }
    passed("signal-blocked-second-send-receive-prefix-with-and-without-restart");
}
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);
    validation();families();names_peek_trunc();prefix_and_lazy_faults();timeout_points();control_batch();output_faults();vector_limits();control_output_faults();pinned_descriptor();signal_prefix();
    printf("MMSG PASS cases=%u\n",cases);return 0;
}
