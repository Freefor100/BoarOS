#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"UNIX-NAMED FAIL line=%d %s errno=%d\n",__LINE__,#x,errno); _exit(1); } } while (0)
/* Separate file pages keep this check deterministic: the syscall first reads
 * each page when it copies the first byte, including with 16-KiB guest pages. */
_Alignas(16384) static const unsigned char cold_elf_send[16384] = {0x6b};
_Alignas(16384) static unsigned char cold_elf_receive[16384] = {0x3a};
#define OFFSET offsetof(struct sockaddr_un,sun_path)
static unsigned cpu_ids[4],cpus;
struct address { struct sockaddr_un un; socklen_t len; };
static struct address path(const char *name)
{ struct address a={.un.sun_family=AF_UNIX};CHECK(strlen(name)<sizeof(a.un.sun_path));strcpy(a.un.sun_path,name);a.len=OFFSET+strlen(name)+1;return a; }
static struct address abstract(const char *name,unsigned suffix)
{ struct address a={.un.sun_family=AF_UNIX};size_t n=strlen(name);CHECK(n+3<sizeof(a.un.sun_path));memcpy(a.un.sun_path+1,name,n);a.un.sun_path[n+2]=(char)suffix;a.len=OFFSET+n+3;return a; }
static int socket_at(int type,const struct address *a)
{ int fd=socket(AF_UNIX,type,0);CHECK(fd>=0);if(a)CHECK(!bind(fd,(const void *)&a->un,a->len));return fd; }
static void connect_to(int fd,const struct address *a) { CHECK(!connect(fd,(const void *)&a->un,a->len)); }
static void name_is(int fd,const struct address *a,int peer)
{ struct sockaddr_un got;memset(&got,0xa5,sizeof(got));socklen_t n=sizeof(got);CHECK(!(peer?getpeername(fd,(void *)&got,&n):getsockname(fd,(void *)&got,&n)));CHECK(n==a->len && !memcmp(&got,&a->un,n)); }
static void creds(int fd,pid_t pid,uid_t uid,gid_t gid)
{ struct ucred c={0};socklen_t n=sizeof(c);CHECK(!getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&c,&n));CHECK(n==sizeof(c) && c.pid==pid && c.uid==uid && c.gid==gid); }
static void wait_child(pid_t pid) { int status;CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0); }
static void exact(int fd,void *p,size_t n,int writing)
{ size_t done=0;while(done<n) { ssize_t r=writing?send(fd,(char *)p+done,n-done,MSG_NOSIGNAL):recv(fd,(char *)p+done,n-done,0);if(r<0 && errno==EINTR)continue;CHECK(r>0 && (size_t)r<=n-done);done+=(size_t)r; } }
static void pin(unsigned index)
{ cpu_set_t mask;CPU_ZERO(&mask);CPU_SET(cpu_ids[index%cpus],&mask);CHECK(!sched_setaffinity(0,sizeof(mask),&mask));cpu_set_t actual;CPU_ZERO(&actual);CHECK(!sched_getaffinity(0,sizeof(actual),&actual) && !memcmp(&actual,&mask,sizeof(mask))); }
static void affinity(void)
{ cpu_set_t mask;CPU_ZERO(&mask);CHECK(!sched_getaffinity(0,sizeof(mask),&mask));for(unsigned i=0;i<CPU_SETSIZE && cpus<4;i++)if(CPU_ISSET(i,&mask))cpu_ids[cpus++]=i;CHECK(cpus);pin(0); }
static uint64_t clock_ns(clockid_t c) { struct timespec t;CHECK(!clock_gettime(c,&t));return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec; }
static void barrier(pthread_barrier_t *b) { int r=pthread_barrier_wait(b);CHECK(!r || r==PTHREAD_BARRIER_SERIAL_THREAD); }
static void stream_names(void)
{
    for(unsigned abs=0;abs<2;abs++) {
        struct address a=abs?abstract("named-stream",1):path("stream"),client=abs?abstract("client-stream",2):path("client");
        int listener=socket_at(SOCK_STREAM,&a);CHECK(!listen(listener,2));name_is(listener,&a,0);
        int fd=socket_at(SOCK_STREAM,&client);connect_to(fd,&a);name_is(fd,&a,1);
        struct sockaddr_un got;socklen_t n=sizeof(got);int accepted=accept4(listener,(void *)&got,&n,SOCK_CLOEXEC|SOCK_NONBLOCK);CHECK(accepted>=0 && n==client.len && !memcmp(&got,&client.un,n));
        CHECK((fcntl(accepted,F_GETFL)&O_NONBLOCK) && (fcntl(accepted,F_GETFD)&FD_CLOEXEC));name_is(accepted,&a,0);name_is(accepted,&client,1);
        CHECK(send(fd,"stream",6,MSG_NOSIGNAL)==6);char bytes[8];CHECK(recv(accepted,bytes,8,0)==6 && !memcmp(bytes,"stream",6));
        CHECK(!shutdown(fd,SHUT_WR));CHECK(recv(accepted,bytes,8,0)==0);CHECK(send(accepted,"reply",5,MSG_NOSIGNAL)==5);CHECK(recv(fd,bytes,8,0)==5 && !memcmp(bytes,"reply",5));
        CHECK(!close(fd) && !close(accepted) && !close(listener));
        if(!abs) { struct stat st;CHECK(!lstat("stream",&st) && S_ISSOCK(st.st_mode));int stale=socket_at(SOCK_STREAM,0);CHECK(connect(stale,(void *)&a.un,a.len)==-1 && errno==ECONNREFUSED);CHECK(!close(stale) && !unlink("stream") && !unlink("client")); }
    }
    struct address a=abstract("exact",1),b=abstract("exact",2);int x=socket_at(SOCK_DGRAM,&a),y=socket_at(SOCK_DGRAM,&b);name_is(x,&a,0);name_is(y,&b,0);CHECK(!close(x) && !close(y));
    puts("UNIX-NAMED CASE stream-names-accept4-shutdown PASS");
}
static void inode_identity(void)
{
    for(int type=SOCK_STREAM;type<=SOCK_DGRAM;type++) {
        struct address a=path("old"),b=path("new"),alias=path("alias");int listener=socket_at(type,&a);if(type==SOCK_STREAM)CHECK(!listen(listener,4));
        struct stat old,now;CHECK(!lstat("old",&old) && S_ISSOCK(old.st_mode));CHECK(!link("old","alias") && !rename("old","new"));CHECK(!lstat("new",&now) && old.st_ino==now.st_ino && old.st_dev==now.st_dev);
        int client=socket_at(type,0);connect_to(client,&b);int peer=type==SOCK_STREAM?accept(listener,0,0):listener;CHECK(peer>=0);CHECK(send(client,"a",1,0)==1);char v;CHECK(recv(peer,&v,1,0)==1 && v=='a');if(peer!=listener)CHECK(!close(peer));CHECK(!close(client));
        CHECK(!unlink("new"));client=socket_at(type,0);connect_to(client,&alias);peer=type==SOCK_STREAM?accept(listener,0,0):listener;CHECK(peer>=0);CHECK(send(client,"b",1,0)==1 && recv(peer,&v,1,0)==1 && v=='b');
        /* Removing the last name must not detach established endpoints from their inode. */
        CHECK(!unlink("alias"));int replacement=socket_at(type,&a);if(type==SOCK_STREAM)CHECK(!listen(replacement,1));CHECK(send(client,"c",1,0)==1 && recv(peer,&v,1,0)==1 && v=='c');
        CHECK(!close(client));if(peer!=listener)CHECK(!close(peer));CHECK(!close(listener) && !close(replacement) && !unlink("old"));
    }
    puts("UNIX-NAMED CASE inode-rename-hardlink-unlink PASS");
}
static void dgram_names(void)
{
    struct address a=path("dgram-a"),b=path("dgram-b"),c=abstract("dgram-source",7);int one=socket_at(SOCK_DGRAM,&a),two=socket_at(SOCK_DGRAM,&b),sender=socket_at(SOCK_DGRAM,&c);connect_to(sender,&a);
    CHECK(send(sender,"a",1,0)==1);struct sockaddr_un from;socklen_t n=sizeof(from);char bytes[8];CHECK(recvfrom(one,bytes,8,0,(void *)&from,&n)==1 && bytes[0]=='a' && n==c.len && !memcmp(&from,&c.un,n));
    connect_to(sender,&b);CHECK(send(sender,"b",1,0)==1 && recv(two,bytes,8,0)==1 && bytes[0]=='b');CHECK(recv(one,bytes,8,MSG_DONTWAIT)==-1 && errno==EAGAIN);
    struct sockaddr sa={.sa_family=AF_UNSPEC};CHECK(!connect(sender,&sa,sizeof(sa)));CHECK(send(sender,"x",1,MSG_NOSIGNAL)==-1 && errno==ENOTCONN);CHECK(sendto(sender,"c",1,0,(void *)&a.un,a.len)==1 && recv(one,bytes,8,0)==1 && bytes[0]=='c');
    int auto_fd=socket_at(SOCK_DGRAM,0);struct sockaddr_un auto_name={.sun_family=AF_UNIX};CHECK(!bind(auto_fd,(void *)&auto_name,OFFSET));n=sizeof(auto_name);CHECK(!getsockname(auto_fd,(void *)&auto_name,&n) && n==OFFSET+6 && auto_name.sun_path[0]==0);
    CHECK(sendto(auto_fd,"u",1,0,(void *)&a.un,a.len)==1);socklen_t m=sizeof(from);CHECK(recvfrom(one,bytes,8,0,(void *)&from,&m)==1 && m==n && !memcmp(&from,&auto_name,n));CHECK(sendto(one,"r",1,0,(void *)&from,m)==1 && recv(auto_fd,bytes,8,0)==1 && bytes[0]=='r');
    CHECK(!close(auto_fd) && !close(sender) && !close(one) && !close(two) && !unlink("dgram-a") && !unlink("dgram-b"));puts("UNIX-NAMED CASE dgram-reconnect-unspec-source-autobind PASS");
}
static void credentials_dac(void)
{
    uid_t uid=getuid();gid_t gid=getgid();pid_t self=getpid();int pair[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));creds(pair[0],self,uid,gid);pid_t child=fork();CHECK(child>=0);if(!child) { if(!geteuid())CHECK(!setgid(65534) && !setuid(65534));creds(pair[1],self,uid,gid);_exit(0); }wait_child(child);CHECK(!close(pair[0]) && !close(pair[1]));
    struct address a=path("credential");int listener=socket_at(SOCK_STREAM,&a);child=fork();CHECK(child>=0);
    if(!child) { if(!geteuid())CHECK(!setgid(65534) && !setuid(65534));CHECK(!listen(listener,2));_exit(0); }wait_child(child);
    int fd=socket_at(SOCK_STREAM,0);connect_to(fd,&a);creds(fd,child,uid?uid:65534,uid?gid:65534);child=fork();CHECK(child>=0);
    if(!child) { int accepted=accept(listener,0,0);CHECK(accepted>=0);creds(accepted,self,uid,gid);CHECK(!close(accepted));_exit(0); }wait_child(child);CHECK(!close(fd) && !close(listener) && !unlink("credential"));
    /* The root guest exercises access using a different effective uid. */
    if(geteuid()==0) {
        a=path("dac");listener=socket_at(SOCK_STREAM,&a);CHECK(!listen(listener,2) && !chmod("dac",0600) && !chmod(".",0777));
        child=fork();CHECK(child>=0);if(!child) { CHECK(!setgid(65534) && !setuid(65534));int denied=socket_at(SOCK_STREAM,0);CHECK(connect(denied,(void *)&a.un,a.len)==-1 && errno==EACCES);CHECK(!close(denied));_exit(0); }wait_child(child);
        CHECK(!chmod("dac",0666));child=fork();CHECK(child>=0);if(!child) { CHECK(!setgid(65534) && !setuid(65534));int allowed=socket_at(SOCK_STREAM,0);connect_to(allowed,&a);creds(allowed,self,0,0);CHECK(!close(allowed));_exit(0); }
        int accepted=accept(listener,0,0);CHECK(accepted>=0);creds(accepted,child,65534,65534);wait_child(child);CHECK(!close(accepted) && !close(listener) && !unlink("dac") && !chmod(".",0700));
    }
    puts("UNIX-NAMED CASE credentials-snapshots-dac PASS");
}
/* Isolated initialized ELF pages enter the kernel without a user pre-touch. */
static const unsigned char cold_send[16384] __attribute__((aligned(16384)))={0x6b};
static unsigned char cold_receive[16384] __attribute__((aligned(16384)))={0x45};
static void faults(void)
{
    int unconnected=socket_at(SOCK_STREAM,0);char state_byte=0;
    CHECK(recv(unconnected,&state_byte,1,0)==-1 && errno==EINVAL);
    struct iovec state_iov={&state_byte,1};
    CHECK(readv(unconnected,&state_iov,1)==-1 && errno==EINVAL);
    struct address state_name=abstract("receive-state",0);
    CHECK(!bind(unconnected,(void *)&state_name.un,state_name.len) && !listen(unconnected,1));
    struct msghdr state_message={.msg_iov=&state_iov,.msg_iovlen=1};
    CHECK(recvmsg(unconnected,&state_message,MSG_DONTWAIT)==-1 && errno==EINVAL);
    CHECK(!close(unconnected));
    int pair[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
    CHECK(send(pair[0],cold_send,1,MSG_NOSIGNAL)==1);
    CHECK(recv(pair[1],cold_receive,1,0)==1 && cold_receive[0]==0x6b);
    CHECK(!close(pair[0]) && !close(pair[1]));
    size_t page=(size_t)sysconf(_SC_PAGESIZE);void *bad=mmap(0,page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(bad!=MAP_FAILED);
    struct address a=path("fault");int fd=socket_at(SOCK_DGRAM,0);CHECK(bind(fd,bad,a.len)==-1 && errno==EFAULT);struct stat st;CHECK(lstat("fault",&st)==-1 && errno==ENOENT);CHECK(!bind(fd,(void *)&a.un,a.len));
    int sender=socket_at(SOCK_DGRAM,0);CHECK(sendto(sender,bad,1,0,(void *)&a.un,a.len)==-1 && errno==EFAULT);char byte;CHECK(recv(fd,&byte,1,MSG_DONTWAIT)==-1 && errno==EAGAIN);
    CHECK(connect(sender,bad,a.len)==-1 && errno==EFAULT);CHECK(send(sender,"x",1,MSG_NOSIGNAL)==-1 && errno==ENOTCONN);
    int cold[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,cold));
    CHECK(send(cold[0],cold_elf_send,1,MSG_NOSIGNAL)==1);
    CHECK(recv(cold[1],cold_elf_receive,1,0)==1 && cold_elf_receive[0]==0x6b);
    CHECK(!close(cold[0]) && !close(cold[1]));
    CHECK(!close(sender) && !close(fd) && !unlink("fault") && !munmap(bad,page));puts("UNIX-NAMED CASE fault-no-sideeffects PASS");
}
static void wait_sleep(uint64_t *tid)
{
    uint64_t id;while(!(id=__atomic_load_n(tid,__ATOMIC_ACQUIRE)))sched_yield();
    char file[80],text[512];snprintf(file,sizeof(file),"/proc/self/task/%llu/stat",(unsigned long long)id);
    for(unsigned i=0;i<1000000;i++) { int fd=open(file,O_RDONLY);CHECK(fd>=0);ssize_t n=read(fd,text,sizeof(text)-1);CHECK(n>0 && !close(fd));text[n]=0;char *p=strrchr(text,')');CHECK(p);if(p[2]=='S')return;sched_yield(); }CHECK(0);
}
struct wake { int fd,mode,ep;pthread_barrier_t *gate;uint64_t tid; };
static void *wait_ready(void *arg)
{
    struct wake *w=arg;pin(1);__atomic_store_n(&w->tid,(uint64_t)syscall(SYS_gettid),__ATOMIC_RELEASE);barrier(w->gate);if(w->ep>=0) { struct epoll_event event;CHECK(epoll_wait(w->ep,&event,1,5000)==1 && (event.events&(w->mode?EPOLLOUT:EPOLLIN))); }
    else { struct pollfd p={w->fd,w->mode?POLLOUT:POLLIN,0};CHECK(poll(&p,1,5000)==1 && (p.revents&(w->mode?POLLOUT:POLLIN))); }return 0;
}
struct pending { int fd,error,result;struct address address;uint64_t tid; };
static void *pending_connect(void *arg)
{ struct pending *p=arg;pin(1);__atomic_store_n(&p->tid,(uint64_t)syscall(SYS_gettid),__ATOMIC_RELEASE);p->result=connect(p->fd,(void *)&p->address.un,p->address.len);p->error=errno;return 0; }
static void stream_space(void)
{
    struct address a=abstract("stream-space",9);int listener=socket_at(SOCK_STREAM,&a);CHECK(!listen(listener,2));int sender=socket_at(SOCK_STREAM|SOCK_NONBLOCK,0);connect_to(sender,&a);int receiver=accept(listener,0,0);CHECK(receiver>=0 && !close(listener));
    char bytes[4096];memset(bytes,'s',sizeof(bytes));size_t queued=0;ssize_t n;
    while((n=send(sender,bytes,sizeof(bytes),0))>0) { queued+=(size_t)n;CHECK(queued<16*1024*1024); }CHECK(n==-1 && errno==EAGAIN && queued);
    int ep=epoll_create1(0);CHECK(ep>=0);struct epoll_event event={.events=EPOLLOUT};CHECK(!epoll_ctl(ep,EPOLL_CTL_ADD,sender,&event) && epoll_wait(ep,&event,1,0)==0);
    pthread_barrier_t gate;CHECK(!pthread_barrier_init(&gate,0,2));struct wake w={.fd=sender,.mode=1,.ep=ep,.gate=&gate};pthread_t thread;CHECK(!pthread_create(&thread,0,wait_ready,&w));barrier(&gate);wait_sleep(&w.tid);
    size_t read_bytes=0;while(read_bytes<queued) { n=recv(receiver,bytes,sizeof(bytes),0);CHECK(n>0);for(ssize_t i=0;i<n;i++)CHECK(bytes[i]=='s');read_bytes+=(size_t)n; }CHECK(read_bytes==queued && !pthread_join(thread,0));
    CHECK(!epoll_ctl(ep,EPOLL_CTL_DEL,sender,0));event.events=EPOLLIN|EPOLLRDHUP;CHECK(!epoll_ctl(ep,EPOLL_CTL_ADD,receiver,&event));w=(struct wake){.fd=receiver,.mode=0,.ep=ep,.gate=&gate};CHECK(!pthread_create(&thread,0,wait_ready,&w));barrier(&gate);wait_sleep(&w.tid);CHECK(!close(sender));CHECK(!pthread_join(thread,0) && recv(receiver,bytes,1,0)==0);
    CHECK(!pthread_barrier_destroy(&gate) && !close(ep) && !close(receiver));
}
static void queue_notifications(void)
{
    for(unsigned ep=0;ep<2;ep++) {
        struct address a=abstract("queue",ep);int receiver=socket_at(SOCK_DGRAM|SOCK_NONBLOCK,&a),sender=socket_at(SOCK_DGRAM|SOCK_NONBLOCK,0);connect_to(sender,&a);char byte='q';unsigned queued=0;
        while(send(sender,&byte,1,0)==1) { CHECK(++queued<65536); }
        CHECK(errno==EAGAIN && queued>0);
        int eventfd=ep?epoll_create1(EPOLL_CLOEXEC):-1;CHECK(!ep || eventfd>=0);if(ep) { struct epoll_event e={.events=EPOLLOUT};CHECK(!epoll_ctl(eventfd,EPOLL_CTL_ADD,sender,&e));CHECK(epoll_wait(eventfd,&e,1,0)==0); }
        else { struct pollfd p={sender,POLLOUT,0};CHECK(poll(&p,1,0)==0); }
        pthread_barrier_t gate;CHECK(!pthread_barrier_init(&gate,0,2));struct wake w={.fd=sender,.mode=1,.ep=eventfd,.gate=&gate};pthread_t thread;CHECK(!pthread_create(&thread,0,wait_ready,&w));barrier(&gate);wait_sleep(&w.tid);
        for(unsigned i=0;i<queued;i++) { CHECK(recv(receiver,&byte,1,0)==1 && byte=='q'); }
        CHECK(!pthread_join(thread,0));CHECK(send(sender,&byte,1,0)==1 && recv(receiver,&byte,1,0)==1);
        CHECK(!pthread_barrier_destroy(&gate));if(ep)CHECK(!close(eventfd));CHECK(!close(sender) && !close(receiver));
    }
    /* Leave a reciprocal pair, wait on another sender's full destination, then
     * become reciprocal again while a writer is asleep. Neither transition
     * may lose the sender's poll/epoll notification. */
    struct address ta=abstract("topology-a",1),tb=abstract("topology-b",2);
    int left=socket_at(SOCK_DGRAM|SOCK_NONBLOCK,&ta),right=socket_at(SOCK_DGRAM|SOCK_NONBLOCK,&tb),third=socket_at(SOCK_DGRAM|SOCK_NONBLOCK,0);
    connect_to(left,&tb);connect_to(right,&ta);struct sockaddr unspec={.sa_family=AF_UNSPEC};
    CHECK(!connect(right,&unspec,sizeof(unspec)));
    char q='q';unsigned filled=0;
    while(sendto(third,&q,1,0,(void *)&tb.un,tb.len)==1)CHECK(++filled<65536);
    CHECK(errno==EAGAIN && filled);
    int topology_ep=epoll_create1(0);CHECK(topology_ep>=0);struct epoll_event topology_event={.events=EPOLLOUT};
    CHECK(!epoll_ctl(topology_ep,EPOLL_CTL_ADD,left,&topology_event) && epoll_wait(topology_ep,&topology_event,1,0)==0);
    pthread_barrier_t topology_gate;CHECK(!pthread_barrier_init(&topology_gate,0,2));
    struct wake topology_wait={.fd=left,.mode=1,.ep=topology_ep,.gate=&topology_gate};pthread_t topology_thread;
    CHECK(!pthread_create(&topology_thread,0,wait_ready,&topology_wait));barrier(&topology_gate);wait_sleep(&topology_wait.tid);
    CHECK(recv(right,&q,1,0)==1 && q=='q' && !pthread_join(topology_thread,0));
    CHECK(sendto(third,&q,1,0,(void *)&tb.un,tb.len)==1 && epoll_wait(topology_ep,&topology_event,1,0)==0);
    topology_wait.tid=0;CHECK(!pthread_create(&topology_thread,0,wait_ready,&topology_wait));barrier(&topology_gate);wait_sleep(&topology_wait.tid);
    connect_to(right,&ta);
    /* Linux does not notify the opposite endpoint solely on this connect;
     * drain a queued message to provide the space event on both kernels. */
    CHECK(recv(right,&q,1,0)==1 && q=='q' && !pthread_join(topology_thread,0));
    for(unsigned i=1;i<filled;i++)CHECK(recv(right,&q,1,0)==1 && q=='q');
    CHECK(!pthread_barrier_destroy(&topology_gate) && !close(topology_ep) && !close(third) && !close(left) && !close(right));
    struct address a=abstract("backlog",3);int listener=socket_at(SOCK_STREAM|SOCK_NONBLOCK,&a);CHECK(!listen(listener,1));int clients[64];unsigned count=0;
    for(;;) { int fd=socket_at(SOCK_STREAM|SOCK_NONBLOCK,0);if(!connect(fd,(void *)&a.un,a.len)) { CHECK(count<64);clients[count++]=fd;continue; }CHECK(errno==EAGAIN && count>0);CHECK(!close(fd));break; }
    struct pending pending={.fd=socket_at(SOCK_STREAM,0),.address=a};pthread_t connector;CHECK(!pthread_create(&connector,0,pending_connect,&pending));wait_sleep(&pending.tid);
    int first=accept4(listener,0,0,SOCK_NONBLOCK);CHECK(first>=0 && !close(first) && !close(clients[0]));CHECK(!pthread_join(connector,0) && !pending.result);
    for(unsigned i=1;i<=count;i++) { int fd=accept4(listener,0,0,SOCK_NONBLOCK),client=i<count?clients[i]:pending.fd;CHECK(fd>=0);char byte='z';CHECK(send(client,&byte,1,0)==1 && recv(fd,&byte,1,0)==1 && byte=='z');CHECK(!close(fd) && !close(client)); }
    CHECK(!close(listener));stream_space();puts("UNIX-NAMED CASE queue-space-poll-epoll-backlog PASS");
}
struct sender { int fd;struct address destination;unsigned id;pthread_barrier_t *gate; };
static void *many_sender(void *arg)
{ struct sender *a=arg;pin(a->id);barrier(a->gate);for(unsigned i=0;i<16;i++) { uint32_t data[2]={a->id,i};CHECK(sendto(a->fd,data,sizeof(data),0,(void *)&a->destination.un,a->destination.len)==sizeof(data)); }return 0; }
static void many_senders(void)
{
    struct address a=abstract("many",1);int receiver=socket_at(SOCK_DGRAM,&a);struct sender actors[4];pthread_t threads[4];pthread_barrier_t gate;CHECK(!pthread_barrier_init(&gate,0,5));
    for(unsigned i=0;i<4;i++) { struct address source=abstract("many-source",i);actors[i]=(struct sender){socket_at(SOCK_DGRAM,&source),a,i,&gate};CHECK(!pthread_create(&threads[i],0,many_sender,&actors[i])); }barrier(&gate);unsigned seen[4]={0};
    for(unsigned i=0;i<64;i++) { uint32_t data[2];struct sockaddr_un from;socklen_t n=sizeof(from);CHECK(recvfrom(receiver,data,sizeof(data),0,(void *)&from,&n)==sizeof(data) && data[0]<4 && data[1]==seen[data[0]]++);struct address expected=abstract("many-source",data[0]);CHECK(n==expected.len && !memcmp(&from,&expected.un,n)); }
    for(unsigned i=0;i<4;i++) { CHECK(!pthread_join(threads[i],0) && !close(actors[i].fd) && seen[i]==16); }
    CHECK(!pthread_barrier_destroy(&gate) && !close(receiver));puts("UNIX-NAMED CASE many-senders-source-order PASS");
}
struct perf_channel { int fd[2],dgram;unsigned id;pthread_barrier_t *gate;uint64_t start[2],end[2],cpu[2],samples[1024]; };
struct perf_actor { struct perf_channel *channel;unsigned side; };
static void *perf_actor(void *arg)
{
    struct perf_actor *actor=arg;struct perf_channel *c=actor->channel;unsigned side=actor->side;pin(c->id+side);
    unsigned rounds=c->dgram?1024:64;size_t size=c->dgram?64:65536;unsigned char *data=malloc(size);CHECK(data);memset(data,'A'+c->id,size);barrier(c->gate);
    c->cpu[side]=clock_ns(CLOCK_THREAD_CPUTIME_ID);c->start[side]=clock_ns(CLOCK_MONOTONIC);
    for(unsigned r=0;r<rounds;r++) {
        uint64_t start=clock_ns(CLOCK_MONOTONIC);
        if(!side) { if(c->dgram) { CHECK(send(c->fd[0],data,size,0)==(ssize_t)size);memset(data,0,size);CHECK(recv(c->fd[0],data,size,0)==(ssize_t)size); }else exact(c->fd[0],data,size,1); }
        else { memset(data,0,size);if(c->dgram)CHECK(recv(c->fd[1],data,size,0)==(ssize_t)size);else exact(c->fd[1],data,size,0);for(size_t i=0;i<size;i++)CHECK(data[i]=='A'+c->id);if(c->dgram)CHECK(send(c->fd[1],data,size,0)==(ssize_t)size); }
        if(c->dgram && !side)for(size_t i=0;i<size;i++)CHECK(data[i]=='A'+c->id);
        if((c->dgram && !side) || (!c->dgram && side))c->samples[r]=clock_ns(CLOCK_MONOTONIC)-start;
    }
    c->end[side]=clock_ns(CLOCK_MONOTONIC);c->cpu[side]=clock_ns(CLOCK_THREAD_CPUTIME_ID)-c->cpu[side];free(data);return 0;
}
static int compare(const void *a,const void *b) { uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;return (x>y)-(x<y); }
static void metric(const char *name,uint64_t bytes,uint64_t elapsed,uint64_t cpu,uint64_t *samples,unsigned count)
{ qsort(samples,count,sizeof(*samples),compare);struct rusage usage;CHECK(!getrusage(RUSAGE_SELF,&usage));printf("UNIX-NAMED PERF name=%s bytes=%llu count=%u wall_ns=%llu bytes_per_sec=%llu ops_per_sec=%llu p50_ns=%llu p95_ns=%llu max_ns=%llu thread_cpu_ns=%llu rss_kib=%ld\n",name,(unsigned long long)bytes,count,(unsigned long long)elapsed,(unsigned long long)(bytes*1000000000ULL/elapsed),(unsigned long long)((uint64_t)count*1000000000ULL/elapsed),(unsigned long long)samples[(count-1)/2],(unsigned long long)samples[(count*95-1)/100],(unsigned long long)samples[count-1],(unsigned long long)cpu,usage.ru_maxrss); }
static void perf_traffic(unsigned dgram)
{
    struct perf_channel channels[4]={0};struct perf_actor actors[8];pthread_t threads[8];pthread_barrier_t gate;CHECK(!pthread_barrier_init(&gate,0,8));
    for(unsigned i=0;i<4;i++) {
        struct address a=dgram?abstract("perf-server",i):path("perf-stream");struct perf_channel *c=&channels[i];c->id=i;c->dgram=dgram;c->gate=&gate;
        if(dgram) { struct address b=abstract("perf-client",i);c->fd[0]=socket_at(SOCK_DGRAM,&b);c->fd[1]=socket_at(SOCK_DGRAM,&a);connect_to(c->fd[0],&a);connect_to(c->fd[1],&b); }
        else { int listener=socket_at(SOCK_STREAM,&a);CHECK(!listen(listener,4));c->fd[0]=socket_at(SOCK_STREAM,0);connect_to(c->fd[0],&a);c->fd[1]=accept(listener,0,0);CHECK(c->fd[1]>=0 && !close(listener) && !unlink("perf-stream")); }
    }
    for(unsigned i=0;i<8;i++) { actors[i]=(struct perf_actor){&channels[i/2],i%2};CHECK(!pthread_create(&threads[i],0,perf_actor,&actors[i])); }for(unsigned i=0;i<8;i++)CHECK(!pthread_join(threads[i],0));
    uint64_t start=UINT64_MAX,end=0,cpu=0,samples[4096];unsigned count=0;
    for(unsigned i=0;i<4;i++) { struct perf_channel *c=&channels[i];for(unsigned j=0;j<2;j++) { if(c->start[j]<start)start=c->start[j];if(c->end[j]>end)end=c->end[j];cpu+=c->cpu[j];CHECK(!close(c->fd[j])); }for(unsigned j=0;j<(dgram?1024:64);j++)samples[count++]=c->samples[j]; }
    CHECK(!pthread_barrier_destroy(&gate));metric(dgram?"abstract-dgram-four":"pathname-stream-four",dgram?4096ULL*64*2:16ULL*1024*1024,end-start,cpu,samples,count);
}
static void perf_connections(void)
{
    struct address a=path("perf-connect");int listener=socket_at(SOCK_STREAM,&a);CHECK(!listen(listener,4));uint64_t samples[1024],cpu=clock_ns(CLOCK_THREAD_CPUTIME_ID),start=clock_ns(CLOCK_MONOTONIC);
    for(unsigned i=0;i<1024;i++) { uint64_t begin=clock_ns(CLOCK_MONOTONIC);int fd=socket_at(SOCK_STREAM,0);connect_to(fd,&a);int accepted=accept4(listener,0,0,SOCK_CLOEXEC);CHECK(accepted>=0 && !close(fd) && !close(accepted));samples[i]=clock_ns(CLOCK_MONOTONIC)-begin; }
    uint64_t end=clock_ns(CLOCK_MONOTONIC);cpu=clock_ns(CLOCK_THREAD_CPUTIME_ID)-cpu;CHECK(!close(listener) && !unlink("perf-connect"));metric("pathname-stream-connections",0,end-start,cpu,samples,1024);
}
int main(int argc,char **argv)
{
    setvbuf(stdout,0,_IONBF,0);signal(SIGPIPE,SIG_IGN);alarm(120);affinity();
    if(getpid()==1) { CHECK(!mkdir("/proc",0555) || errno==EEXIST);CHECK(!mount("proc","/proc","proc",0,0) || errno==EBUSY); }
    CHECK(!mkdir("/tmp",0777) || errno==EEXIST);char directory[]="/tmp/unix-named-XXXXXX";CHECK(mkdtemp(directory) && !chdir(directory));
    int performance=access("/performance",F_OK)==0 || (argc==2 && !strcmp(argv[1],"--performance"));
    if(performance) { perf_traffic(0);perf_traffic(1);perf_connections();puts("UNIX-NAMED PERF END 3"); }
    else { stream_names();inode_identity();dgram_names();credentials_dac();faults();queue_notifications();many_senders();puts("UNIX-NAMED END 7"); }
    CHECK(!chdir("/") && !rmdir(directory));alarm(0);if(getpid()==1) { sync();(void)reboot(RB_POWER_OFF); }return 0;
}
