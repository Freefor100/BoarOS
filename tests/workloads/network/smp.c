#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"SOCKET-SMP FAIL line=%d %s errno=%d\n",__LINE__,#x,errno); _exit(1); } } while (0)
#include "../../smp/composition_protocol.h"
static struct composition_control *control;
static int control_fd=-1;
static unsigned cpus;
static void command(unsigned action,uint64_t tid)
{
    uint64_t next=composition_load(&control->request)+1;
    composition_store(&control->target_tid,tid);composition_store(&control->command,action);composition_store(&control->request,next);
    while(composition_load(&control->reply)!=next)sched_yield();
}
static void observe_start(void)
{
    if(access("/socket-observer",F_OK))return;
    CHECK(!mkdir("/dev/shm",0700) || errno==EEXIST);
    CHECK(!mount("tmpfs","/dev/shm","tmpfs",0,0) || errno==EBUSY);
    control_fd=open("/dev/shm/smp-control",O_CREAT|O_EXCL|O_RDWR,0600);CHECK(control_fd>=0 && !ftruncate(control_fd,4096));
    control=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED,control_fd,0);CHECK(control!=MAP_FAILED);
    composition_store(&control->magic,COMPOSITION_MAGIC);
    while(!composition_load(&control->observer_ready))sched_yield();
}
static uint64_t ns(void) { struct timespec t; CHECK(!clock_gettime(CLOCK_MONOTONIC,&t)); return (uint64_t)t.tv_sec*1000000000U+t.tv_nsec; }
static void pin(unsigned cpu) { uint64_t mask=UINT64_C(1)<<cpu; CHECK(!syscall(SYS_sched_setaffinity,0,sizeof(mask),&mask)); }
static void barrier(pthread_barrier_t *b) { int r=pthread_barrier_wait(b); CHECK(!r || r==PTHREAD_BARRIER_SERIAL_THREAD); }
static void endpoints(int fds[2],int domain,int type)
{
    if (domain==AF_UNIX) { CHECK(!socketpair(domain,type,0,fds)); return; }
    union { struct sockaddr_in v4;struct sockaddr_in6 v6; } address={0},other;
    socklen_t length=domain==AF_INET?sizeof(address.v4):sizeof(address.v6);
    if (domain==AF_INET) { address.v4.sin_family=domain;address.v4.sin_addr.s_addr=htonl(INADDR_LOOPBACK); }
    else { address.v6.sin6_family=domain;address.v6.sin6_addr=in6addr_loopback; }
    int listener=socket(domain,type,0);CHECK(listener>=0);
    CHECK(!bind(listener,(void *)&address,length));CHECK(!getsockname(listener,(void *)&address,&length));
    fds[0]=socket(domain,type,0);CHECK(fds[0]>=0);
    if (type==SOCK_STREAM) CHECK(!listen(listener,8));
    CHECK(!connect(fds[0],(void *)&address,length));
    if (type==SOCK_STREAM) { fds[1]=accept(listener,0,0);CHECK(fds[1]>=0);CHECK(!close(listener)); }
    else { fds[1]=listener;socklen_t n=sizeof(other);CHECK(!getsockname(fds[0],(void *)&other,&n));CHECK(!connect(fds[1],(void *)&other,n)); }
}
static ssize_t transfer(int fd,void *bytes,size_t size,int writing,unsigned style)
{
    struct iovec iov[2]={{bytes,size/3},{(char *)bytes+size/3,size-size/3}};
    struct msghdr msg={.msg_iov=iov,.msg_iovlen=2};
    switch (style%4) {
    case 0:return writing?write(fd,bytes,size):read(fd,bytes,size);
    case 1:return writing?writev(fd,iov,2):readv(fd,iov,2);
    case 2:return writing?sendmsg(fd,&msg,MSG_NOSIGNAL):recvmsg(fd,&msg,0);
    default:return writing?sendto(fd,bytes,size,MSG_NOSIGNAL,0,0):recvfrom(fd,bytes,size,0,0,0);
    }
}
static void exact(int fd,void *bytes,size_t size,int writing,unsigned style)
{
    size_t done=0;while(done<size) { ssize_t n=transfer(fd,(char *)bytes+done,size-done,writing,style);if(n<0 && errno==EINTR)continue;CHECK(n>0 && (size_t)n<=size-done);done+=(size_t)n; }
}
struct channel { int fd[2],dgram,performance;unsigned id,rounds,pairs;pthread_barrier_t *gate;uint64_t start[2],end[2],samples[1024]; };
struct actor { struct channel *channel;unsigned side; };
static void *traffic(void *arg)
{
    struct actor *actor=arg;struct channel *c=actor->channel;unsigned side=actor->side;
    pin((c->id+side)%cpus);
    size_t block=c->performance && !c->dgram?65536:64;
    unsigned char *bytes=malloc(block);CHECK(bytes);memset(bytes,(int)(0x30+c->id),block);
    barrier(c->gate);c->start[side]=ns();
    for(unsigned i=0;i<c->rounds;i++) {
        unsigned style=c->performance?0:i%4;uint64_t start=ns();
        if(c->dgram) {
            if(!side) { CHECK(transfer(c->fd[0],bytes,block,1,style)==(ssize_t)block);memset(bytes,0,block);CHECK(transfer(c->fd[0],bytes,block,0,style)==(ssize_t)block); }
            else { memset(bytes,0,block);CHECK(transfer(c->fd[1],bytes,block,0,style)==(ssize_t)block);for(size_t j=0;j<block;j++)CHECK(bytes[j]==0x30+c->id);CHECK(transfer(c->fd[1],bytes,block,1,style)==(ssize_t)block); }
        } else if(!side) exact(c->fd[0],bytes,block,1,style);
        else { exact(c->fd[1],bytes,block,0,style); }
        if(side || c->dgram) for(size_t j=0;j<block;j++)CHECK(bytes[j]==0x30+c->id);
        if((c->dgram && !side) || (!c->dgram && side))c->samples[i]=ns()-start;
    }
    c->end[side]=ns();free(bytes);return 0;
}
static int compare(const void *a,const void *b) { uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;return (x>y)-(x<y); }
static void group(const char *name,int domain,int type,unsigned pairs,int performance)
{
    struct channel channels[4]={0};struct actor actors[8];pthread_t threads[8];pthread_barrier_t gate;
    CHECK(!pthread_barrier_init(&gate,0,pairs*2));
    unsigned rounds=performance?(type==SOCK_DGRAM?1024:256/pairs):16;
    for(unsigned i=0;i<pairs;i++) { struct channel *c=&channels[i];endpoints(c->fd,domain,type);c->id=i;c->dgram=type==SOCK_DGRAM;c->performance=performance;c->pairs=pairs;c->rounds=rounds;c->gate=&gate; }
    for(unsigned i=0;i<pairs*2;i++) { actors[i]=(struct actor){&channels[i/2],i%2};CHECK(!pthread_create(&threads[i],0,traffic,&actors[i])); }
    for(unsigned i=0;i<pairs*2;i++)CHECK(!pthread_join(threads[i],0));
    uint64_t start=UINT64_MAX,end=0,samples[4096];unsigned count=0;
    for(unsigned i=0;i<pairs;i++) { struct channel *c=&channels[i];for(unsigned side=0;side<2;side++) { if(c->start[side]<start)start=c->start[side];if(c->end[side]>end)end=c->end[side]; }for(unsigned j=0;j<rounds;j++)samples[count++]=c->samples[j];CHECK(!close(c->fd[0]) && !close(c->fd[1])); }
    CHECK(!pthread_barrier_destroy(&gate));qsort(samples,count,sizeof(*samples),compare);
    struct rusage usage;CHECK(!getrusage(RUSAGE_SELF,&usage));
    if(performance)printf("SOCKET-PERF name=%s pairs=%u bytes=%u count=%u ns=%llu p50=%llu p95=%llu max=%llu rss_kib=%ld\n",name,pairs,type==SOCK_DGRAM?4096U*64U*2U:16777216U,count,(unsigned long long)(end-start),(unsigned long long)samples[(count-1)/2],(unsigned long long)samples[(count*95-1)/100],(unsigned long long)samples[count-1],usage.ru_maxrss);
    else printf("SOCKET-SMP CASE %s OK\n",name);
}
struct blocked { int fd,peer;unsigned cpu,mode;uint64_t tid,entered;ssize_t result;int error; };
static void *blocked_reader(void *argument)
{
    struct blocked *b=argument;pin(b->cpu);__atomic_store_n(&b->tid,(uint64_t)syscall(SYS_gettid),__ATOMIC_RELEASE);
    char data[64]={0};__atomic_store_n(&b->entered,1,__ATOMIC_RELEASE);
    b->result=read(b->fd,data,sizeof(data));b->error=errno;
    if(b->mode==1)CHECK(b->result==4 && !memcmp(data,"held",4));
    return 0;
}
static void wait_sleep(struct blocked *b)
{
    while(!__atomic_load_n(&b->entered,__ATOMIC_ACQUIRE))sched_yield();
    if(control) { command(COMPOSITION_SOCKET_WAIT,b->tid);return; }
    char path[64],text[512];snprintf(path,sizeof(path),"/proc/self/task/%llu/stat",(unsigned long long)b->tid);
    for(unsigned i=0;i<100000;i++) { int fd=open(path,O_RDONLY);CHECK(fd>=0);ssize_t n=read(fd,text,sizeof(text)-1);CHECK(n>0 && !close(fd));text[n]=0;char *end=strrchr(text,')');CHECK(end);if(end[2]=='S')return;sched_yield(); }CHECK(0);
}
static void lifecycle(void)
{
    for(unsigned cpu=0;cpu<cpus;cpu++) {
        int fds[2];endpoints(fds,AF_UNIX,SOCK_STREAM);struct blocked b={.fd=fds[1],.peer=fds[0],.cpu=cpu,.mode=1};pthread_t thread;CHECK(!pthread_create(&thread,0,blocked_reader,&b));wait_sleep(&b);
        CHECK(!close(fds[1]));int replacement=open("/data",O_RDONLY);CHECK(replacement==fds[1]);CHECK(write(fds[0],"held",4)==4);CHECK(!pthread_join(thread,0));CHECK(!close(replacement) && !close(fds[0]));
        endpoints(fds,AF_UNIX,SOCK_STREAM);b=(struct blocked){.fd=fds[1],.cpu=cpu};CHECK(!pthread_create(&thread,0,blocked_reader,&b));wait_sleep(&b);CHECK(!pthread_cancel(thread));void *ret=0;CHECK(!pthread_join(thread,&ret) && ret==PTHREAD_CANCELED);CHECK(!close(fds[0]) && !close(fds[1]));
    }
    puts("SOCKET-SMP CASE blocking-close-reuse-cancel OK");
}
static void faults_and_sendfile(void)
{
    long page=sysconf(_SC_PAGESIZE);char *map=mmap(0,(size_t)page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(map!=MAP_FAILED);memset(map,'z',(size_t)page);CHECK(!mprotect(map+page,(size_t)page,PROT_NONE));
    for(unsigned cpu=0;cpu<cpus;cpu++) {
        pin(cpu);
        for(unsigned type=SOCK_STREAM;type<=SOCK_DGRAM;type++) {
            int fds[2];endpoints(fds,AF_UNIX,type);CHECK(send(fds[0],map+page,1,MSG_NOSIGNAL)==-1 && errno==EFAULT);
            CHECK(recv(fds[1],map,1,MSG_DONTWAIT)==-1 && errno==EAGAIN);
            struct timeval timeout={.tv_usec=2000};CHECK(!setsockopt(fds[1],SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)));uint64_t start=ns();CHECK(recv(fds[1],map,1,0)==-1 && errno==EAGAIN);CHECK(ns()-start>=1000000);
            if(type==SOCK_DGRAM) { CHECK(send(fds[0],map,0,0)==0);CHECK(recv(fds[1],map,0,0)==0);CHECK(send(fds[0],map,64,0)==64);struct iovec iov={map,8};struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1};CHECK(recvmsg(fds[1],&msg,MSG_TRUNC)==64 && (msg.msg_flags&MSG_TRUNC)); }
            CHECK(!close(fds[0]) && !close(fds[1]));
        }
        int fds[2];endpoints(fds,AF_INET,SOCK_STREAM);int input=open("/data",O_RDONLY);CHECK(input>=0);off_t offset=7;ssize_t n=sendfile(fds[0],input,&offset,63);CHECK(n>0 && n<=63);char want[64],got[64];CHECK(pread(input,want,(size_t)n,7)==n);exact(fds[1],got,(size_t)n,0,0);CHECK(!memcmp(want,got,(size_t)n) && offset==7+n);CHECK(!close(input) && !close(fds[0]) && !close(fds[1]));
    }
    pin(0);CHECK(!munmap(map,(size_t)page*2));puts("SOCKET-SMP CASE fault-empty-trunc-timeout-sendfile OK");
}
struct shared_actor { int fd,writing,dgram;unsigned id;pthread_barrier_t *gate;unsigned char *seen; };
static void *shared_traffic(void *arg)
{
    struct shared_actor *a=arg;pin(a->id%cpus);unsigned char bytes[64];barrier(a->gate);
    for(unsigned round=0;round<128;round++) {
        memset(bytes,'s',sizeof(bytes));
        if(a->writing && a->dgram) { uint32_t record=a->id*128+round;memcpy(bytes,&record,sizeof(record)); }
        if(a->dgram)CHECK(transfer(a->fd,bytes,sizeof(bytes),a->writing,round%4)==sizeof(bytes));
        else exact(a->fd,bytes,sizeof(bytes),a->writing,round%4);
        if(!a->writing) {
            unsigned start=0;
            if(a->dgram) { uint32_t record;memcpy(&record,bytes,sizeof(record));CHECK(record<512 && !__atomic_exchange_n(&a->seen[record],1,__ATOMIC_RELAXED));start=4; }
            for(unsigned j=start;j<sizeof(bytes);j++)CHECK(bytes[j]=='s');
        }
        /* At most four datagrams in flight: this tests shared ownership, not UDP loss under overload. */
        barrier(a->gate);
    }
    return 0;
}
static void shared_endpoints(void)
{
    for(unsigned domain=0;domain<2;domain++)for(unsigned dgram=0;dgram<2;dgram++) {
        int fds[2];endpoints(fds,domain?AF_UNIX:AF_INET,dgram?SOCK_DGRAM:SOCK_STREAM);
        unsigned char seen[512]={0};pthread_barrier_t gate;CHECK(!pthread_barrier_init(&gate,0,8));
        pthread_t threads[8];struct shared_actor actors[8];
        for(unsigned i=0;i<8;i++) { actors[i]=(struct shared_actor){fds[i/4],i<4,dgram,i%4,&gate,seen};CHECK(!pthread_create(&threads[i],0,shared_traffic,&actors[i])); }
        for(unsigned i=0;i<8;i++)CHECK(!pthread_join(threads[i],0));
        if(dgram)for(unsigned i=0;i<512;i++)CHECK(seen[i]==1);
        CHECK(!pthread_barrier_destroy(&gate) && !close(fds[0]) && !close(fds[1]));
    }
    puts("SOCKET-SMP CASE shared-readers-writers OK");
}
struct notification { int fd;unsigned cpu;pthread_barrier_t *gate; };
static void *notifier(void *argument) { struct notification *n=argument;pin(n->cpu);barrier(n->gate);CHECK(write(n->fd,"e",1)==1);return 0; }
static void epoll_notifications(void)
{
    for(unsigned cpu=0;cpu<cpus;cpu++) {
        int fds[2];endpoints(fds,AF_UNIX,SOCK_STREAM);int ep=epoll_create1(0),outer=epoll_create1(0);CHECK(ep>=0 && outer>=0);
        struct epoll_event event={.events=EPOLLIN|EPOLLONESHOT,.data.u64=77},out;
        CHECK(!epoll_ctl(ep,EPOLL_CTL_ADD,fds[1],&event));event.events=EPOLLIN;CHECK(!epoll_ctl(outer,EPOLL_CTL_ADD,ep,&event));
        pthread_barrier_t gate;CHECK(!pthread_barrier_init(&gate,0,2));struct notification n={fds[0],cpu,&gate};pthread_t thread;CHECK(!pthread_create(&thread,0,notifier,&n));barrier(&gate);
        CHECK(epoll_wait(outer,&out,1,2000)==1);CHECK(epoll_wait(ep,&out,1,2000)==1 && out.data.u64==77);CHECK(epoll_wait(ep,&out,1,0)==0);
        event.events=EPOLLIN|EPOLLONESHOT;CHECK(!epoll_ctl(ep,EPOLL_CTL_MOD,fds[1],&event));CHECK(epoll_wait(ep,&out,1,2000)==1);char data;CHECK(read(fds[1],&data,1)==1 && data=='e');CHECK(!epoll_ctl(ep,EPOLL_CTL_DEL,fds[1],0));CHECK(!pthread_join(thread,0));CHECK(!pthread_barrier_destroy(&gate));CHECK(!close(outer) && !close(ep) && !close(fds[0]) && !close(fds[1]));
    }
    puts("SOCKET-SMP CASE remote-epoll-oneshot-nested OK");
}
struct controlled_actor { int fd,mode,ep;unsigned cpu;uint64_t tid,go,done;void *buffer;ssize_t result; };
static void *controlled_actor(void *arg)
{
    struct controlled_actor *a=arg;pin(a->cpu);composition_store(&a->tid,(uint64_t)syscall(SYS_gettid));
    while(!composition_load(&a->go))sched_yield();
    char bytes[8]={0};
    if(a->mode==1) { a->result=read(a->fd,bytes,sizeof(bytes));CHECK(a->result==4 && !memcmp(bytes,"head",4)); }
    if(a->mode==2) { a->result=write(a->fd,a->buffer,1024);CHECK(a->result==1024); }
    if(a->mode==3)CHECK(write(a->fd,"event",5)==5);
    if(a->mode==4)CHECK(!mprotect(a->buffer,4096,PROT_NONE));
    if(a->mode==5)CHECK(!epoll_ctl(a->ep,EPOLL_CTL_DEL,a->fd,0));
    composition_store(&a->done,1);return 0;
}
static void held_start(struct controlled_actor *a,pthread_t *thread,unsigned mode)
{
    composition_store(&control->socket_entered,0);composition_store(&control->socket_release,0);
    composition_store(&control->socket_waited,0);composition_store(&control->socket_waiter_tid,0);
    CHECK(!pthread_create(thread,0,controlled_actor,a));while(!composition_load(&a->tid))sched_yield();
    composition_store(&control->socket_tid,a->tid);composition_store(&control->socket_mode,mode);composition_store(&a->go,1);
    while(!composition_load(&control->socket_entered))sched_yield();
}
static void controlled_lifetimes(void)
{
    if(!control)return;
    unsigned remote=cpus-1;int fds[2];pthread_t actor,waiter;
    endpoints(fds,AF_UNIX,SOCK_STREAM);CHECK(write(fds[0],"head",4)==4);
    struct controlled_actor a={.fd=fds[1],.mode=1,.cpu=remote};held_start(&a,&actor,1);
    CHECK(write(fds[0],"tail",4)==4 && !close(fds[1]));int replacement=open("/data",O_RDONLY);CHECK(replacement==fds[1]);
    composition_store(&control->socket_release,1);CHECK(!pthread_join(actor,0));CHECK(!close(replacement) && !close(fds[0]));
    puts("SOCKET-CONTROL reservation-append-last-close OK");
    composition_store(&control->socket_mode,0);
    endpoints(fds,AF_INET,SOCK_STREAM);void *page=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(page!=MAP_FAILED);memset(page,'p',4096);
    /* Create both stacks before holding a pin: musl may mprotect a new stack guard. */
    struct controlled_actor revoke={.mode=4,.cpu=0,.buffer=page};CHECK(!pthread_create(&waiter,0,controlled_actor,&revoke));
    while(!composition_load(&revoke.tid))sched_yield();
    a=(struct controlled_actor){.fd=fds[0],.mode=2,.cpu=remote,.buffer=page};held_start(&a,&actor,2);
    composition_store(&control->socket_waiter_tid,revoke.tid);composition_store(&revoke.go,1);
    CHECK(!pthread_join(actor,0) && !pthread_join(waiter,0));CHECK(composition_load(&control->socket_waited)==1);
    char received[1024];exact(fds[1],received,sizeof(received),0,0);for(unsigned i=0;i<sizeof(received);i++)CHECK(received[i]=='p');CHECK(!close(fds[0]) && !close(fds[1]) && !munmap(page,4096));
    puts("SOCKET-CONTROL tx-pin-remote-mprotect OK");
    composition_store(&control->socket_mode,0);
    endpoints(fds,AF_UNIX,SOCK_STREAM);int ep=epoll_create1(0);CHECK(ep>=0);struct epoll_event event={.events=EPOLLIN|EPOLLONESHOT,.data.u64=5};CHECK(!epoll_ctl(ep,EPOLL_CTL_ADD,fds[1],&event));
    a=(struct controlled_actor){.fd=fds[0],.mode=3,.cpu=remote};held_start(&a,&actor,3);
    struct controlled_actor removal={.fd=fds[1],.mode=5,.cpu=0,.ep=ep};CHECK(!pthread_create(&waiter,0,controlled_actor,&removal));while(!composition_load(&removal.tid))sched_yield();composition_store(&control->socket_waiter_tid,removal.tid);composition_store(&removal.go,1);
    while(!composition_load(&control->socket_waited))sched_yield();
    CHECK(!composition_load(&removal.done));
    composition_store(&control->socket_release,1);CHECK(!pthread_join(actor,0) && !pthread_join(waiter,0));CHECK(epoll_wait(ep,&event,1,0)==0);CHECK(!close(ep) && !close(fds[0]) && !close(fds[1]));
    composition_store(&control->socket_mode,0);puts("SOCKET-CONTROL callback-borrow-del-close OK");
}
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);signal(SIGPIPE,SIG_IGN);
    uint64_t mask=0;CHECK(syscall(SYS_sched_getaffinity,0,sizeof(mask),&mask)==sizeof(mask));cpus=(unsigned)__builtin_popcountll(mask);CHECK(cpus>=1 && cpus<=4);pin(0);
    if(getpid()==1) { CHECK(!mkdir("/proc",0555) || errno==EEXIST);CHECK(!mount("proc","/proc","proc",0,0) || errno==EBUSY); }
    int fd=socket(AF_INET,SOCK_DGRAM,0);CHECK(fd>=0);struct ifreq interface={.ifr_name="lo"};CHECK(!ioctl(fd,SIOCGIFFLAGS,&interface));interface.ifr_flags|=IFF_UP;CHECK(!ioctl(fd,SIOCSIFFLAGS,&interface));CHECK(!close(fd));
    observe_start();
    int performance=access("/performance",F_OK)==0;
    if(performance) {
        group("tcp-one",AF_INET,SOCK_STREAM,1,1);group("tcp-four",AF_INET,SOCK_STREAM,4,1);
        group("unix-stream-one",AF_UNIX,SOCK_STREAM,1,1);group("unix-stream-four",AF_UNIX,SOCK_STREAM,4,1);
        group("udp-four",AF_INET,SOCK_DGRAM,4,1);group("unix-dgram-four",AF_UNIX,SOCK_DGRAM,4,1);puts("SOCKET-PERF END 6");
    } else {
        group("ipv4-tcp",AF_INET,SOCK_STREAM,4,0);group("ipv6-tcp",AF_INET6,SOCK_STREAM,4,0);group("ipv4-udp",AF_INET,SOCK_DGRAM,4,0);group("ipv6-udp",AF_INET6,SOCK_DGRAM,4,0);group("unix-stream",AF_UNIX,SOCK_STREAM,4,0);group("unix-dgram",AF_UNIX,SOCK_DGRAM,4,0);
        lifecycle();faults_and_sendfile();shared_endpoints();epoll_notifications();controlled_lifetimes();puts("SOCKET-SMP END 10");
    }
    if(control) { command(COMPOSITION_STOP,0);CHECK(!munmap(control,4096) && !close(control_fd) && !unlink("/dev/shm/smp-control")); }
    sync();(void)reboot(RB_POWER_OFF);return 0;
}
