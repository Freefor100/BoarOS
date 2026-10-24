#define UNIX_CONTROL_MESSAGE_TYPE SOCK_SEQPACKET
#define UNIX_CONTROL_FIRST_TYPE SOCK_SEQPACKET
#define UNIX_CONTROL_LAST_TYPE SOCK_SEQPACKET
#define UNIX_CONTROL_LISTENER_TYPE SOCK_SEQPACKET
#define main unix_control_main
#include "unix_control.c"
#undef main
#include <poll.h>
#include <sys/epoll.h>
#include <sys/sendfile.h>
#undef CHECK
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"UNIXSEQ FAIL line=%d %s errno=%d\n",__LINE__,#x,errno); _exit(1); } } while (0)
#define NAME_OFFSET offsetof(struct sockaddr_un,sun_path)
static unsigned seq_cases;
static volatile sig_atomic_t pipe_signals;
static void seq_pass(const char *name) { ++seq_cases;printf("UNIXSEQ CASE %s PASS\n",name); }
static void pipe_signal(int signal_number) { CHECK(signal_number==SIGPIPE);++pipe_signals; }
static int seq_socket(int flags) { int fd=socket(AF_UNIX,SOCK_SEQPACKET|flags,0);CHECK(fd>=0);return fd; }
static struct sockaddr_un seq_name(const char *text,int abstract,socklen_t *length)
{
    struct sockaddr_un name={.sun_family=AF_UNIX};size_t n=strlen(text);CHECK(n+1<sizeof(name.sun_path));
    memcpy(name.sun_path+abstract,text,n+1);*length=(socklen_t)(NAME_OFFSET+n+1);return name;
}
static void seq_type(int fd)
{ int type=0;socklen_t length=sizeof(type);CHECK(!getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&length) && length==sizeof(type) && type==SOCK_SEQPACKET); }
static int seq_listener(const struct sockaddr_un *name,socklen_t length,int backlog)
{ int fd=seq_socket(SOCK_NONBLOCK);CHECK(!bind(fd,(const void *)name,length) && !listen(fd,backlog));return fd; }
static void seq_names(void)
{
    int pair[2];CHECK(!socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,0,pair));
    for(unsigned i=0;i<2;i++) { seq_type(pair[i]);CHECK((fcntl(pair[i],F_GETFL)&O_NONBLOCK) && (fcntl(pair[i],F_GETFD)&FD_CLOEXEC));peer_is(pair[i],getpid(),getuid(),getgid()); }close_pair(pair);
    for(int abstract=0;abstract<=1;abstract++) {
        socklen_t length,client_length;struct sockaddr_un name=seq_name("seq-listener",abstract,&length),client=seq_name("seq-client",abstract,&client_length),got;
        int listener=seq_listener(&name,length,2);passcred(listener,1);seq_type(listener);
        int fd=seq_socket(0);CHECK(!bind(fd,(void *)&client,client_length) && !connect(fd,(void *)&name,length));
        socklen_t n=sizeof(got);int peer=accept4(listener,(void *)&got,&n,SOCK_NONBLOCK|SOCK_CLOEXEC);CHECK(peer>=0 && n==client_length && !memcmp(&got,&client,n));seq_type(peer);
        CHECK((fcntl(peer,F_GETFL)&O_NONBLOCK) && (fcntl(peer,F_GETFD)&FD_CLOEXEC));n=sizeof(got);CHECK(!getsockname(peer,(void *)&got,&n) && n==length && !memcmp(&got,&name,n));
        n=sizeof(got);CHECK(!getpeername(peer,(void *)&got,&n) && n==client_length && !memcmp(&got,&client,n));peer_is(peer,getpid(),getuid(),getgid());
        int enabled=0;n=sizeof(enabled);CHECK(!getsockopt(peer,SOL_SOCKET,SO_PASSCRED,&enabled,&n) && enabled==1);
        CHECK(send(fd,"name",4,0)==4);struct received r=receive_control(peer,16,CONTROL_SIZE,0);CHECK(r.size==4 && !memcmp(r.data,"name",4));cred_is(&r,getpid(),getuid(),getgid());
        int wrong=socket(AF_UNIX,SOCK_STREAM,0);CHECK(wrong>=0 && connect(wrong,(void *)&name,length)==-1 && errno==(abstract?ECONNREFUSED:EPROTOTYPE) && !close(wrong));
        wrong=socket(AF_UNIX,SOCK_DGRAM,0);CHECK(wrong>=0 && connect(wrong,(void *)&name,length)==-1 && errno==(abstract?ECONNREFUSED:EPROTOTYPE) && !close(wrong));
        CHECK(!close(peer) && !close(fd) && !close(listener));if(!abstract)CHECK(!unlink("seq-listener") && !unlink("seq-client"));
    }
    int fd=seq_socket(0);struct sockaddr_un automatic={.sun_family=AF_UNIX};CHECK(!bind(fd,(void *)&automatic,NAME_OFFSET));socklen_t n=sizeof(automatic);
    CHECK(!getsockname(fd,(void *)&automatic,&n) && n==NAME_OFFSET+6 && !automatic.sun_path[0]);CHECK(!close(fd));
    seq_pass("socketpair-type-flags-path-abstract-autobind-peercred");
}
static void seq_state_flags(void)
{
    const int types[]={SOCK_STREAM,SOCK_DGRAM,SOCK_SEQPACKET};
    for(unsigned i=0;i<sizeof(types)/sizeof(*types);i++)for(unsigned listening=0;listening<2;listening++) {
        if(listening && types[i]==SOCK_DGRAM)continue;
        for(int how=SHUT_RD;how<=SHUT_RDWR;how++) {
            int fresh=socket(AF_UNIX,types[i],0);CHECK(fresh>=0);
            if(listening) { socklen_t length;struct sockaddr_un name=seq_name("shutdown-state",1,&length);CHECK(!bind(fresh,(void *)&name,length) && !listen(fresh,1)); }
            CHECK(shutdown(fresh,-1)==-1 && errno==EINVAL && shutdown(fresh,3)==-1 && errno==EINVAL);
            CHECK(!shutdown(fresh,how) && !close(fresh));
        }
    }

    int fd=seq_socket(SOCK_NONBLOCK);char byte;CHECK(send(fd,"x",1,0)==-1 && errno==ENOTCONN);CHECK(recv(fd,&byte,1,0)==-1 && errno==ENOTCONN);
    CHECK(listen(fd,1)==-1 && errno==EINVAL);CHECK(!close(fd));
    socklen_t length;struct sockaddr_un name=seq_name("state",1,&length);int listener=seq_listener(&name,length,1);
    CHECK(recv(listener,&byte,1,MSG_DONTWAIT)==-1 && errno==ENOTCONN);CHECK(send(listener,"x",1,0)==-1 && errno==ENOTCONN);CHECK(!close(listener));
    int p[2];pair_at(SOCK_SEQPACKET,p);CHECK(send(p[0],"x",1,MSG_OOB)==-1 && errno==EOPNOTSUPP);CHECK(recv(p[1],&byte,1,MSG_OOB|MSG_DONTWAIT)==-1 && errno==EOPNOTSUPP);
    CHECK(send(p[0],"a",1,MSG_MORE|MSG_EOR)==1 && send(p[0],"bc",2,0)==2);struct received r=receive_control(p[1],8,0,MSG_WAITALL);CHECK(r.size==1 && r.data[0]=='a' && !r.flags);
    r=receive_control(p[1],8,0,MSG_WAITALL);CHECK(r.size==2 && !memcmp(r.data,"bc",2) && !r.flags);
    CHECK(sendto(p[0],"z",1,0,(void *)&name,length)==1 && recv(p[1],&byte,1,0)==1 && byte=='z');close_pair(p);
    seq_pass("unconnected-listener-flags-more-eor-waitall-sendto");
}
static void seq_records(void)
{
    int p[2];pair_at(SOCK_SEQPACKET,p);CHECK(send(p[0],"abcdef",6,0)==6 && send(p[0],"NEXT",4,0)==4);
    struct received r=receive_control(p[1],2,0,MSG_PEEK);CHECK(r.size==2 && !memcmp(r.data,"ab",2) && r.flags==MSG_TRUNC);
    r=receive_control(p[1],2,0,MSG_PEEK|MSG_TRUNC);CHECK(r.size==6 && !memcmp(r.data,"ab",2) && r.flags==MSG_TRUNC);
    r=receive_control(p[1],2,0,0);CHECK(r.size==2 && !memcmp(r.data,"ab",2) && r.flags==MSG_TRUNC);r=receive_control(p[1],256,0,MSG_WAITALL);CHECK(r.size==4 && !memcmp(r.data,"NEXT",4) && !r.flags);
    CHECK(send(p[0],"keep",4,0)==4);char byte;CHECK(read(p[1],&byte,0)==0);struct iovec zero={&byte,0};CHECK(readv(p[1],&zero,1)==0);r=receive_control(p[1],8,0,0);CHECK(r.size==4 && !memcmp(r.data,"keep",4));
    int file=data_file();CHECK(send_control(p[0],"drop",4,&file,1,0,0)==4);r=receive_control(p[1],0,CONTROL_SIZE,MSG_PEEK);CHECK(r.size==0 && r.flags==MSG_TRUNC && r.nfds==1);release_received(&r);
    r=receive_control(p[1],0,CONTROL_SIZE,0);CHECK(r.size==0 && r.flags==MSG_TRUNC && r.nfds==1);release_received(&r);empty_queue(p[1]);
    CHECK(send(p[0],"discard",7,0)==7 && recv(p[1],&byte,0,0)==0);empty_queue(p[1]);
    passcred(p[1],1);CHECK(send_control(p[0],"",0,&file,1,0,0)==0 && !shutdown(p[0],SHUT_WR));
    r=receive_control(p[1],16,CONTROL_SIZE,0);CHECK(r.size==0 && r.nfds==1 && !r.flags);cred_is(&r,getpid(),getuid(),getgid());release_received(&r);
    r=receive_control(p[1],16,CONTROL_SIZE,0);CHECK(r.size==0 && !r.nfds && !r.ncreds && !r.flags);CHECK(!close(file));close_pair(p);
    seq_pass("record-peek-trunc-empty-zero-read-recv-eof-controls");
}
static void seq_vectors_faults(void)
{
    size_t page=(size_t)sysconf(_SC_PAGESIZE);void *bad=mmap(0,page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(bad!=MAP_FAILED);
    int p[2];pair_at(SOCK_SEQPACKET,p);struct iovec iov[]={{"ABC",3},{"def",3}};CHECK(writev(p[0],iov,2)==6);
    char left[2]={0},right[5]={0};struct iovec out[]={{left,2},{right,4}};CHECK(readv(p[1],out,2)==6 && !memcmp(left,"AB",2) && !memcmp(right,"Cdef",4));
    iov[1]=(struct iovec){bad,3};CHECK(writev(p[0],iov,2)==-1 && errno==EFAULT);empty_queue(p[1]);
    int small=4096;CHECK(!setsockopt(p[0],SOL_SOCKET,SO_SNDBUF,&small,sizeof(small)));size_t huge_size=1024*1024;void *huge=calloc(1,huge_size);CHECK(huge);
    CHECK(send(p[0],huge,huge_size,MSG_DONTWAIT)==-1 && errno==EMSGSIZE);empty_queue(p[1]);free(huge);
    CHECK(send(p[0],"abcdef",6,0)==6 && send(p[0],"safe",4,0)==4);out[1]=(struct iovec){bad,4};struct msghdr message={.msg_iov=out,.msg_iovlen=2};
    CHECK(recvmsg(p[1],&message,0)==-1 && errno==EFAULT && !memcmp(left,"ab",2));struct received r=receive_control(p[1],8,0,0);CHECK(r.size==4 && !memcmp(r.data,"safe",4));
    close_pair(p);CHECK(!munmap(bad,page));seq_pass("vectored-content-fault-atomic-send-consumed-receive-size-limit");
}
static void seq_import_order(void)
{
    size_t page=(size_t)sysconf(_SC_PAGESIZE);void *bad=mmap(0,page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(bad!=MAP_FAILED);
    int fd=seq_socket(0),invalid=-1;union control c={0};size_t used=add_rights(&c,0,&invalid,1);char byte='x';struct iovec iov={&byte,1};
    struct msghdr message={.msg_iov=&iov,.msg_iovlen=1,.msg_control=c.bytes,.msg_controllen=used};
    message.msg_control=bad;CHECK(syscall(SYS_sendmsg,fd,&message,MSG_OOB)==-1 && errno==EFAULT);message.msg_control=c.bytes;
    CHECK(syscall(SYS_sendmsg,fd,&message,MSG_OOB)==-1 && errno==ENOTCONN);
    struct msghdr incoming={.msg_iov=&iov,.msg_iovlen=1};CHECK(syscall(SYS_recvmsg,fd,&incoming,MSG_OOB)==-1 && errno==ENOTCONN);
    incoming.msg_iov=bad;CHECK(syscall(SYS_recvmsg,fd,&incoming,MSG_OOB)==-1 && errno==EFAULT);
    CHECK(recv(fd,&byte,1,MSG_OOB)==-1 && errno==ENOTCONN);
    CHECK(syscall(SYS_sendmsg,fd,bad,0)==-1 && errno==EFAULT);
    message.msg_iov=bad;CHECK(syscall(SYS_sendmsg,fd,&message,0)==-1 && errno==EFAULT);message.msg_iov=&iov;
    message.msg_control=bad;CHECK(syscall(SYS_sendmsg,fd,&message,0)==-1 && errno==EFAULT);message.msg_control=c.bytes;
    CHECK(syscall(SYS_sendmsg,fd,&message,0)==-1 && errno==ENOTCONN);((struct cmsghdr *)c.bytes)->cmsg_len=1;
    CHECK(syscall(SYS_sendmsg,fd,&message,0)==-1 && errno==ENOTCONN);CHECK(!close(fd));
    int p[2];pair_at(SOCK_SEQPACKET,p);(void)add_rights(&c,0,&invalid,1);
    CHECK(syscall(SYS_sendmsg,p[0],&message,MSG_OOB)==-1 && errno==EBADF);empty_queue(p[1]);
    CHECK(send(p[0],"unread",6,0)==6 && !close(p[1]));
    message.msg_control=bad;CHECK(syscall(SYS_sendmsg,p[0],&message,0)==-1 && errno==EFAULT);message.msg_control=NULL;message.msg_controllen=0;
    CHECK(syscall(SYS_sendmsg,p[0],&message,0)==-1 && errno==ECONNRESET);CHECK(syscall(SYS_sendmsg,p[0],&message,0)==-1 && errno==EPIPE);CHECK(!close(p[0]) && !munmap(bad,page));
    seq_pass("raw-header-iov-control-import-before-state-reset-and-fd-interpretation");
}
static void seq_final_header_fault(void)
{
    size_t page=(size_t)sysconf(_SC_PAGESIZE);struct msghdr *message=mmap(0,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(message!=MAP_FAILED);
    int p[2];pair_at(SOCK_SEQPACKET,p);int file=data_file();CHECK(send_control(p[0],"F",1,&file,1,0,0)==1);char byte=0;union control c={0};struct iovec iov={&byte,1};
    *message=(struct msghdr){.msg_iov=&iov,.msg_iovlen=1,.msg_control=c.bytes,.msg_controllen=CMSG_SPACE(sizeof(int))};CHECK(!mprotect(message,page,PROT_READ));
    CHECK(syscall(SYS_recvmsg,p[1],message,0)==-1 && errno==EFAULT && byte=='F');struct cmsghdr *header=(void *)c.bytes;
    CHECK(header->cmsg_level==SOL_SOCKET && header->cmsg_type==SCM_RIGHTS && header->cmsg_len==CMSG_LEN(sizeof(int)));int received;memcpy(&received,CMSG_DATA(header),sizeof(received));
    CHECK(read(received,&byte,1)==1 && byte=='0' && !close(received) && !close(file));empty_queue(p[1]);close_pair(p);CHECK(!munmap(message,page));
    seq_pass("final-msghdr-fault-payload-consumed-installed-right-survives");
}
static void seq_source_latebind(void)
{
    const int types[]={SOCK_STREAM,SOCK_DGRAM,SOCK_SEQPACKET};
    for(unsigned type=0;type<sizeof(types)/sizeof(*types);type++)for(unsigned abstract=0;abstract<=1;abstract++) {
        int p[2];CHECK(!socketpair(AF_UNIX,types[type],0,p) && send(p[0],"before",6,0)==6);socklen_t length;struct sockaddr_un name=seq_name("late-source",abstract,&length),from;
        CHECK(!bind(p[0],(void *)&name,length) && send(p[0],"after",5,0)==5 && !close(p[0]));
        /* A stream may combine writes, so bound each receive to the expected
         * prefix; both public receive entrances must export the sender name. */
        char text[8]={0};socklen_t n=sizeof(from);CHECK(recvfrom(p[1],text,6,0,(void *)&from,&n)==6 && !memcmp(text,"before",6));CHECK(n==length && !memcmp(&from,&name,n));
        memset(text,0,sizeof(text));memset(&from,0,sizeof(from));struct iovec iov={text,5};struct msghdr message={.msg_name=&from,.msg_namelen=sizeof(from),.msg_iov=&iov,.msg_iovlen=1};
        CHECK(recvmsg(p[1],&message,0)==5 && !memcmp(text,"after",5) && !message.msg_flags && !message.msg_controllen);CHECK(message.msg_namelen==length && !memcmp(&from,&name,length));
        CHECK(!close(p[1]));if(!abstract)CHECK(!unlink("late-source"));
    }
    seq_pass("source-current-name-after-send-latebind-and-sender-close");
}

static void seq_shutdown_reset(void)
{
    int p[2];pair_at(SOCK_SEQPACKET,p);CHECK(send(p[0],"queued",6,0)==6 && !shutdown(p[0],SHUT_WR));char bytes[16];CHECK(recv(p[1],bytes,sizeof(bytes),0)==6 && !memcmp(bytes,"queued",6));CHECK(recv(p[1],bytes,sizeof(bytes),MSG_DONTWAIT)==0);
    CHECK(send(p[1],"reply",5,0)==5 && recv(p[0],bytes,sizeof(bytes),0)==5 && !memcmp(bytes,"reply",5));
    sig_atomic_t before=pipe_signals;CHECK(send(p[0],"x",1,0)==-1 && errno==EPIPE);CHECK(write(p[0],"x",1)==-1 && errno==EPIPE);
    int file=data_file();off_t offset=0;CHECK(sendfile(p[0],file,&offset,1)==-1 && errno==EPIPE && offset==0);CHECK(pipe_signals==before && !close(file));close_pair(p);
    pair_at(SOCK_SEQPACKET,p);CHECK(send(p[0],"unread",6,0)==6 && !close(p[1]));CHECK(recv(p[0],bytes,sizeof(bytes),0)==-1 && errno==ECONNRESET);CHECK(recv(p[0],bytes,sizeof(bytes),0)==0 && !close(p[0]));
    socklen_t length;struct sockaddr_un name=seq_name("empty-child",1,&length);int listener=seq_listener(&name,length,1),client=seq_socket(0);CHECK(!connect(client,(void *)&name,length) && !close(listener));
    CHECK(recv(client,bytes,sizeof(bytes),0)==-1 && errno==ECONNRESET && recv(client,bytes,sizeof(bytes),0)==0 && !close(client));
    printf("UNIXSEQ ACTION sigpipe send=0 write=0 sendfile=0 total=%d\n",(int)pipe_signals);seq_pass("shutdown-drain-halfclose-reset-empty-child-no-sigpipe");
}
static void seq_backlog_accept(void)
{
    socklen_t length;struct sockaddr_un name=seq_name("backlog",1,&length);int listener=seq_listener(&name,length,0),first=seq_socket(SOCK_NONBLOCK),second=seq_socket(SOCK_NONBLOCK);
    CHECK(accept4(listener,0,0,0)==-1 && errno==EAGAIN);CHECK(!connect(first,(void *)&name,length));CHECK(connect(second,(void *)&name,length)==-1 && errno==EAGAIN);
    CHECK(accept4(listener,0,0,0x40000000)==-1 && errno==EINVAL);int peer=accept4(listener,0,0,SOCK_NONBLOCK);CHECK(peer>=0 && !close(peer) && !close(first));
    CHECK(!connect(second,(void *)&name,length));
    size_t page=(size_t)sysconf(_SC_PAGESIZE);void *bad=mmap(0,page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(bad!=MAP_FAILED);socklen_t n=sizeof(name);
    CHECK(accept(listener,bad,&n)==-1 && errno==EFAULT);CHECK(accept(listener,0,0)==-1 && errno==EAGAIN);char byte;CHECK(recv(second,&byte,1,0)==0);
    CHECK(!close(second) && !close(listener) && !munmap(bad,page));seq_pass("nonblock-backlog-accept-invalid-flags-address-fault-consumes-child");
}
struct seq_wake { int fd,ep;uint64_t tid;char byte;ssize_t result; };
static void *seq_receive_thread(void *argument)
{
    struct seq_wake *w=argument;pin(1);__atomic_store_n(&w->tid,(uint64_t)syscall(SYS_gettid),__ATOMIC_RELEASE);
    if(w->ep>=0) { struct epoll_event event;CHECK(epoll_wait(w->ep,&event,1,5000)==1 && (event.events&EPOLLIN)); }
    w->result=recv(w->fd,&w->byte,1,0);return NULL;
}
static void seq_wait_sleep(struct seq_wake *w)
{ struct blocked_control_sender sender={0};while(!(sender.tid=__atomic_load_n(&w->tid,__ATOMIC_ACQUIRE)))sched_yield();wait_control_sender_sleep(&sender); }
static void seq_caller_cpus(void)
{
    for(unsigned index=0;index<cpus;index++) {
        pin(index);int p[2];pair_at(SOCK_SEQPACKET,p);int file=data_file();uint32_t payload[]={index,cpu_ids[index]};
        CHECK(send_control(p[0],payload,sizeof(payload),&file,1,0,0)==sizeof(payload));
        struct received r=receive_control(p[1],sizeof(payload),CMSG_SPACE(sizeof(int)),MSG_CMSG_CLOEXEC);
        CHECK(r.size==sizeof(payload) && !memcmp(r.data,payload,sizeof(payload)) && r.nfds==1 && !r.ncreds && !(r.flags&MSG_CTRUNC));
        struct stat original,received;char content[16];CHECK(!fstat(file,&original) && !fstat(r.fds[0],&received) && original.st_dev==received.st_dev && original.st_ino==received.st_ino);
        CHECK((fcntl(r.fds[0],F_GETFD)&FD_CLOEXEC) && pread(r.fds[0],content,sizeof(content),0)==sizeof(content) && !memcmp(content,"0123456789abcdef",sizeof(content)));
        release_received(&r);CHECK(send(p[1],"ok",2,0)==2 && recv(p[0],content,sizeof(content),0)==2 && !memcmp(content,"ok",2));CHECK(!close(file));close_pair(p);
        printf("UNIXSEQ CPU index=%u affinity=%u rights=1 PASS\n",index,cpu_ids[index]);
    }
    pin(0);
}
static void seq_poll_epoll(void)
{
    int p[2];pair_at(SOCK_SEQPACKET,p);struct pollfd pollfd={.fd=p[1],.events=POLLIN|POLLOUT};CHECK(poll(&pollfd,1,0)==1 && pollfd.revents==POLLOUT);
    for(unsigned mode=0;mode<3;mode++) {
        int ep=epoll_create1(EPOLL_CLOEXEC);CHECK(ep>=0);struct epoll_event event={.events=EPOLLIN|(mode==1?EPOLLET:mode==2?EPOLLONESHOT:0),.data.u64=0x1234},out;
        CHECK(!epoll_ctl(ep,EPOLL_CTL_ADD,p[1],&event) && epoll_wait(ep,&out,1,0)==0);CHECK(send(p[0],"ab",2,0)==2 && send(p[0],"c",1,0)==1);
        CHECK(epoll_wait(ep,&out,1,1000)==1 && out.data.u64==0x1234 && (out.events&EPOLLIN));CHECK(epoll_wait(ep,&out,1,0)==(mode?0:1));
        char bytes[4];CHECK(recv(p[1],bytes,sizeof(bytes),0)==2 && !memcmp(bytes,"ab",2));
        if(mode==2)CHECK(!epoll_ctl(ep,EPOLL_CTL_MOD,p[1],&event) && epoll_wait(ep,&out,1,0)==1);
        CHECK(recv(p[1],bytes,sizeof(bytes),0)==1 && bytes[0]=='c' && epoll_wait(ep,&out,1,0)==0);
        if(mode==2)CHECK(!epoll_ctl(ep,EPOLL_CTL_MOD,p[1],&event));
        struct seq_wake wake={.fd=p[1],.ep=ep};pthread_t thread;CHECK(!pthread_create(&thread,0,seq_receive_thread,&wake));seq_wait_sleep(&wake);
        CHECK(send(p[0],"w",1,0)==1 && !pthread_join(thread,0) && wake.result==1 && wake.byte=='w');CHECK(!close(ep));
    }
    struct seq_wake wake={.fd=p[1],.ep=-1};pthread_t thread;CHECK(!pthread_create(&thread,0,seq_receive_thread,&wake));seq_wait_sleep(&wake);CHECK(!shutdown(p[0],SHUT_WR) && !pthread_join(thread,0) && wake.result==0);
    pollfd.events=POLLIN|POLLRDHUP;CHECK(poll(&pollfd,1,0)==1 && (pollfd.revents&(POLLIN|POLLRDHUP))==(POLLIN|POLLRDHUP));close_pair(p);
    seq_caller_cpus();seq_pass("poll-epoll-level-edge-oneshot-blocked-wake-eof");
}
static void seq_performance(unsigned mode)
{
    struct perf_channel *channels=calloc(PERF_PAIRS,sizeof(*channels));struct perf_actor actors[PERF_PAIRS*2];pthread_t threads[PERF_PAIRS*2];pthread_barrier_t gate;CHECK(channels && !pthread_barrier_init(&gate,0,PERF_PAIRS*2));
    for(unsigned i=0;i<PERF_PAIRS;i++) {
        struct perf_channel *c=&channels[i];c->id=i;c->type=SOCK_SEQPACKET;c->mode=mode;c->gate=&gate;c->nonce=0x5345515041434b54ULL+i;
        char text[32];snprintf(text,sizeof(text),"perf-%u",i);socklen_t length;struct sockaddr_un name=seq_name(text,1,&length);int listener=seq_listener(&name,length,1);c->fd[0]=seq_socket(0);
        CHECK(!connect(c->fd[0],(void *)&name,length));c->fd[1]=accept(listener,0,0);CHECK(c->fd[1]>=0 && !close(listener));
        c->file=data_file();CHECK(pwrite(c->file,&c->nonce,sizeof(c->nonce),0)==sizeof(c->nonce));struct stat st;CHECK(!fstat(c->file,&st));c->device=st.st_dev;c->inode=st.st_ino;
        if(mode==1) { passcred(c->fd[0],1);passcred(c->fd[1],1); }
    }
    for(unsigned i=0;i<PERF_PAIRS*2;i++) { actors[i]=(struct perf_actor){&channels[i/2],i%2};CHECK(!pthread_create(&threads[i],0,perf_actor,&actors[i])); }
    for(unsigned i=0;i<PERF_PAIRS*2;i++)CHECK(!pthread_join(threads[i],0));
    uint64_t start=UINT64_MAX,end=0,cpu=0,*samples=malloc(PERF_PAIRS*PERF_ROUNDS*sizeof(*samples));CHECK(samples);unsigned count=0;
    for(unsigned i=0;i<PERF_PAIRS;i++) {
        struct perf_channel *c=&channels[i];for(unsigned side=0;side<2;side++) { if(c->start[side]<start)start=c->start[side];if(c->end[side]>end)end=c->end[side];cpu+=c->cpu[side]; }
        memcpy(samples+count,c->samples,sizeof(c->samples));count+=PERF_ROUNDS;close_pair(c->fd);CHECK(!close(c->file));
    }
    CHECK(!pthread_barrier_destroy(&gate));qsort(samples,count,sizeof(*samples),compare_u64);const char *modes[]={"none","passcred","rights"};uint64_t elapsed=end-start;
    printf("UNIXSEQ PERF name=seqpacket-%s type=seqpacket mode=%s pairs=4 rounds_per_pair=1024 count=%u bytes=524288 cpus=%u wall_ns=%llu thread_cpu_ns=%llu p50_ns=%llu p95_ns=%llu max_ns=%llu ops_per_sec=%llu\n",modes[mode],modes[mode],count,cpus,(unsigned long long)elapsed,(unsigned long long)cpu,(unsigned long long)samples[(count-1)/2],(unsigned long long)samples[(count*95-1)/100],(unsigned long long)samples[count-1],(unsigned long long)((uint64_t)count*1000000000ULL/elapsed));
    free(samples);free(channels);
}
int main(int argc,char **argv)
{
    setvbuf(stdout,0,_IONBF,0);CHECK(signal(SIGPIPE,pipe_signal)!=SIG_ERR);alarm(120);affinity();
    if(getpid()==1) { CHECK(!mkdir("/proc",0555) || errno==EEXIST);CHECK(!mount("proc","/proc","proc",0,0) || errno==EBUSY); }
    CHECK(!mkdir("/tmp",0777) || errno==EEXIST);strcpy(workdir,"/tmp/unix-seqpacket-XXXXXX");CHECK(mkdtemp(workdir) && !chdir(workdir));unsigned before=fd_count();
    if((argc==2 && !strcmp(argv[1],"--performance")) || (argc==1 && !access("/performance",F_OK))) { for(unsigned mode=0;mode<3;mode++)seq_performance(mode);puts("UNIXSEQ PERF END 3"); }
    else {
        CHECK(argc==1 || (argc==2 && !strcmp(argv[1],"--correctness")));
        seq_names();seq_state_flags();seq_records();seq_vectors_faults();seq_import_order();seq_final_header_fault();seq_source_latebind();seq_shutdown_reset();seq_backlog_accept();seq_poll_epoll();
        rights_objects();source_lifetime();control_capacity();credentials_capacity();rights_limits();full_table();inflight_quota();exit_group_control_owner();peek_and_read();
        credentials_identity();fault_send_control();libc_control_fault();null_control();null_name_readonly();fault_receive_control();fault_receive_partial();fault_payload();malformed_control();concurrent_transfer();gc_actions();
        printf("UNIXSEQ END focus=%u control=%u\n",seq_cases,case_count);
    }
    unsigned after=fd_count();printf("UNIXSEQ RESOURCE fd_before=%u fd_after=%u\n",before,after);CHECK(before==after && !pipe_signals);CHECK(!chdir("/") && !rmdir(workdir));
    alarm(0);if(getpid()==1) { sync();(void)reboot(RB_POWER_OFF); }return 0;
}
