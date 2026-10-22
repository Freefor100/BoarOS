#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include "../../smp/composition_protocol.h"
#define CHECK(x) do { if(!(x)) { dprintf(2,"PIPE FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);_exit(1); } } while(0)
#ifndef PIPE_SMOKE
#define PIPE_SMOKE 0
#endif
static unsigned cpus;
static struct composition_control *control;
static int control_fd=-1;
static uint64_t now(void) { struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
static void pin(unsigned cpu) { uint64_t mask=UINT64_C(1)<<cpu;CHECK(!syscall(SYS_sched_setaffinity,0,sizeof(mask),&mask)); }
static void gate(pthread_barrier_t *b) { int r=pthread_barrier_wait(b);CHECK(!r||r==PTHREAD_BARRIER_SERIAL_THREAD); }
static void observe(void)
{
    if(access("/pipe-observer",F_OK))return;
    CHECK(!mkdir("/dev/shm",0700)||errno==EEXIST);
    CHECK(!mount("tmpfs","/dev/shm","tmpfs",0,0)||errno==EBUSY);
    control_fd=open("/dev/shm/smp-control",O_CREAT|O_EXCL|O_RDWR,0600);CHECK(control_fd>=0&&!ftruncate(control_fd,4096));
    control=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED,control_fd,0);CHECK(control!=MAP_FAILED);
    composition_store(&control->magic,COMPOSITION_MAGIC);
    while(!composition_load(&control->observer_ready))sched_yield();
}
static void ready(int fd,unsigned mode,int writing)
{
    if(mode==1) {
        struct pollfd p={fd,writing?POLLOUT:POLLIN,0};
        CHECK(ppoll(&p,1,0,0)==1&&(p.revents&(writing?POLLOUT:POLLIN)));
    } else if(mode==2) {
        fd_set set;FD_ZERO(&set);FD_SET(fd,&set);
        CHECK(pselect(fd+1,writing?0:&set,writing?&set:0,0,0,0)==1&&FD_ISSET(fd,&set));
    } else if(mode>=3) {
        struct epoll_event e;CHECK(epoll_wait((int)mode-3,&e,1,-1)==1&&(e.events&EPOLLIN));
    }
}
static void exact(int fd,void *bytes,size_t count,int writing,unsigned mode)
{
    size_t done=0;
    while(done<count) {
        if(mode)ready(fd,mode,writing);
        ssize_t n=writing?write(fd,(char *)bytes+done,count-done):read(fd,(char *)bytes+done,count-done);
        if(n<0&&errno==EINTR)continue;
        CHECK(n>0&&(size_t)n<=count-done);done+=(size_t)n;
    }
}
struct channel { int fds[2][2],ep;unsigned id,mode,rounds;size_t block;pthread_barrier_t *barrier;uint64_t start[2],end[2],samples[4096]; };
struct actor { struct channel *c;unsigned side; };
static void *transfer(void *argument)
{
    struct actor *a=argument;struct channel *c=a->c;unsigned side=a->side;pin((c->id+side)%cpus);
    unsigned char bytes[4096];memset(bytes,0x30+(int)c->id,c->block);
    gate(c->barrier);c->start[side]=now();
    for(unsigned i=0;i<c->rounds;i++) {
        uint64_t t=now();
        if(c->mode==1||c->mode==2) {
            if(!side) { exact(c->fds[0][1],bytes,c->block,1,0);exact(c->fds[1][0],bytes,c->block,0,c->mode); }
            else { exact(c->fds[0][0],bytes,c->block,0,c->mode);exact(c->fds[1][1],bytes,c->block,1,0); }
        } else exact(c->fds[0][side?0:1],bytes,c->block,!side,side&&c->mode==3?(unsigned)c->ep+3:0);
        if(side||c->mode==1||c->mode==2)for(size_t j=0;j<c->block;j++)CHECK(bytes[j]==0x30+c->id);
        if((side&&c->mode!=1&&c->mode!=2)||(!side&&(c->mode==1||c->mode==2)))c->samples[i]=now()-t;
    }
    c->end[side]=now();return 0;
}
static int compare(const void *a,const void *b) { uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;return (x>y)-(x<y); }
static void group(const char *name,unsigned pairs,unsigned mode,int performance)
{
    struct channel *channels=calloc(pairs,sizeof(*channels));CHECK(channels);
    struct actor actors[8];pthread_t threads[8];pthread_barrier_t barrier;CHECK(!pthread_barrier_init(&barrier,0,pairs*2));
    size_t block=mode==1||mode==2?64:4096;unsigned rounds=performance?4096/pairs:32;
    for(unsigned i=0;i<pairs;i++) {
        struct channel *c=&channels[i];c->id=i;c->mode=mode;c->rounds=rounds;c->block=block;c->barrier=&barrier;CHECK(!pipe(c->fds[0]));
        if(mode==1||mode==2)CHECK(!pipe(c->fds[1]));
        if(mode==3) { c->ep=epoll_create1(0);CHECK(c->ep>=0);struct epoll_event e={.events=EPOLLIN,.data.u64=i};CHECK(!epoll_ctl(c->ep,EPOLL_CTL_ADD,c->fds[0][0],&e)); }
    }
    for(unsigned i=0;i<pairs*2;i++) { actors[i]=(struct actor){&channels[i/2],i%2};CHECK(!pthread_create(&threads[i],0,transfer,&actors[i])); }
    for(unsigned i=0;i<pairs*2;i++)CHECK(!pthread_join(threads[i],0));
    uint64_t begin=UINT64_MAX,end=0,*samples=malloc((size_t)pairs*rounds*sizeof(*samples));CHECK(samples);unsigned count=0;
    for(unsigned i=0;i<pairs;i++) {
        struct channel *c=&channels[i];for(unsigned side=0;side<2;side++) { if(c->start[side]<begin)begin=c->start[side];if(c->end[side]>end)end=c->end[side]; }
        for(unsigned j=0;j<rounds;j++)samples[count++]=c->samples[j];
        CHECK(!close(c->fds[0][0])&&!close(c->fds[0][1]));
        if(mode==1||mode==2)CHECK(!close(c->fds[1][0])&&!close(c->fds[1][1]));
        if(mode==3)CHECK(!close(c->ep));
    }
    CHECK(!pthread_barrier_destroy(&barrier));qsort(samples,count,sizeof(*samples),compare);struct rusage usage;CHECK(!getrusage(RUSAGE_SELF,&usage));
    if(performance)printf("PIPE-PERF name=%s pairs=%u bytes=%u count=%u ns=%llu p50=%llu p95=%llu max=%llu rss_kib=%ld\n",name,pairs,(unsigned)(count*block*(mode==1||mode==2?2:1)),count,(unsigned long long)(end-begin),(unsigned long long)samples[(count-1)/2],(unsigned long long)samples[(count*95-1)/100],(unsigned long long)samples[count-1],usage.ru_maxrss);
    else printf("PIPE CASE %s OK\n",name);
    free(samples);free(channels);
}
struct blocked { int fd;unsigned cpu,mode;uint64_t tid;ssize_t result;int error,mask_ok;char bytes[64]; };
static void *reader(void *argument)
{
    struct blocked *b=argument;pin(b->cpu);sigset_t empty,blocked;sigemptyset(&empty);sigemptyset(&blocked);sigaddset(&blocked,SIGUSR1);
    if(b->mode>=3&&b->mode<=5)CHECK(!pthread_sigmask(SIG_BLOCK,&blocked,0));
    int ep=-1;if(b->mode==5) { ep=epoll_create1(0);CHECK(ep>=0);struct epoll_event event={.events=EPOLLIN};CHECK(!epoll_ctl(ep,EPOLL_CTL_ADD,b->fd,&event)); }
    __atomic_store_n(&b->tid,(uint64_t)syscall(SYS_gettid),__ATOMIC_RELEASE);
    if(b->mode==3) { struct pollfd f={b->fd,POLLIN,0};b->result=ppoll(&f,1,0,&empty); }
    else if(b->mode==4) { fd_set f;FD_ZERO(&f);FD_SET(b->fd,&f);b->result=pselect(b->fd+1,&f,0,0,0,&empty); }
    else if(b->mode==5||b->mode==6) { struct epoll_event e;b->result=epoll_pwait(b->mode==5?ep:b->fd,&e,1,b->mode==6?100:-1,&empty); }
    else b->result=read(b->fd,b->bytes,sizeof(b->bytes));
    b->error=errno;
    if(b->mode>=3&&b->mode<=5) { CHECK(!pthread_sigmask(SIG_SETMASK,0,&blocked));b->mask_ok=sigismember(&blocked,SIGUSR1); }
    if(ep>=0)CHECK(!close(ep));
    return 0;
}
static void asleep(struct blocked *b)
{
    uint64_t tid;while(!(tid=__atomic_load_n(&b->tid,__ATOMIC_ACQUIRE)))sched_yield();
    if(control) {
        uint64_t next=composition_load(&control->request)+1;
        composition_store(&control->target_tid,tid);composition_store(&control->command,COMPOSITION_SOCKET_WAIT);composition_store(&control->request,next);
        while(composition_load(&control->reply)!=next)sched_yield();
        return;
    }
    char path[96],bytes[512];snprintf(path,sizeof(path),"/proc/self/task/%llu/stat",(unsigned long long)tid);
    for(unsigned i=0;i<100000;i++) { int fd=open(path,O_RDONLY);CHECK(fd>=0);ssize_t n=read(fd,bytes,sizeof(bytes)-1);CHECK(n>0&&!close(fd));bytes[n]=0;char *p=strrchr(bytes,')');CHECK(p);if(p[2]=='S')return;sched_yield(); }CHECK(0);
}
static void handler(int sig) { (void)sig; }
static void lifecycle(void)
{
    struct sigaction action={.sa_handler=handler};sigemptyset(&action.sa_mask);CHECK(!sigaction(SIGUSR1,&action,0));
    for(unsigned cpu=0;cpu<cpus;cpu++)for(unsigned mode=0;mode<6;mode++) {
        int f[2];CHECK(!pipe(f));struct blocked b={.fd=f[0],.cpu=cpu,.mode=mode};pthread_t thread;CHECK(!pthread_create(&thread,0,reader,&b));asleep(&b);
        if(mode==0) {
            CHECK(!close(f[0]));int replacement=open("/pipe-data",O_RDONLY);CHECK(replacement==f[0]);CHECK(write(f[1],"held",4)==4);
            CHECK(!pthread_join(thread,0)&&b.result==4&&!memcmp(b.bytes,"held",4));CHECK(!close(replacement));
        } else if(mode==1) {
            CHECK(!close(f[1])&&!pthread_join(thread,0)&&b.result==0);CHECK(!close(f[0]));continue;
        } else { CHECK(!syscall(SYS_tgkill,getpid(),b.tid,SIGUSR1));CHECK(!pthread_join(thread,0)&&b.result==-1&&b.error==EINTR&&(mode<3||b.mask_ok));CHECK(!close(f[0])); }
        CHECK(!close(f[1]));
    }
    puts("PIPE CASE close-reuse-eof-signal OK");
}
static void boundaries(void)
{
    long page=sysconf(_SC_PAGESIZE);char *m=mmap(0,(size_t)page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(m!=MAP_FAILED);memset(m,'v',(size_t)page);CHECK(!mprotect(m+page,(size_t)page,PROT_NONE));
    CHECK(signal(SIGPIPE,SIG_IGN)!=SIG_ERR);
    for(unsigned cpu=0;cpu<cpus;cpu++) {
        pin(cpu);memset(m,'v',64);int f[2];CHECK(!pipe2(f,O_NONBLOCK));CHECK(write(f[1],m+page,1)==-1&&errno==EFAULT);
        CHECK(read(f[0],m,1)==-1&&errno==EAGAIN);struct iovec iov[2]={{m,31},{m+31,33}};CHECK(writev(f[1],iov,2)==64);memset(m,0,64);CHECK(readv(f[0],iov,2)==64);for(unsigned i=0;i<64;i++)CHECK(m[i]=='v');
        CHECK(!close(f[0])&&write(f[1],m,1)==-1&&errno==EPIPE&&!close(f[1]));
        CHECK(!pipe2(f,O_NONBLOCK));int input=open("/pipe-data",O_RDONLY);CHECK(input>=0);off_t offset=7;ssize_t n=sendfile(f[1],input,&offset,63);CHECK(n==63&&offset==70);char expected[64],actual[64];CHECK(pread(input,expected,63,7)==63&&read(f[0],actual,63)==63&&!memcmp(expected,actual,63));CHECK(!close(input)&&!close(f[0])&&!close(f[1]));
        CHECK(!mkfifo("/fifo",0600));int fifo=open("/fifo",O_RDWR|O_NONBLOCK);CHECK(fifo>=0);CHECK(write(fifo,"fifo",4)==4&&read(fifo,m,4)==4&&!memcmp(m,"fifo",4));CHECK(!close(fifo)&&!unlink("/fifo"));
        CHECK(!pipe(f));struct pollfd polls[3]={{f[0],POLLIN,0},{open("/dev/null",O_RDONLY),POLLIN,0},{open("/pipe-data",O_RDONLY),POLLIN,0}};CHECK(polls[1].fd>=0&&polls[2].fd>=0);struct timespec zero={0};CHECK(ppoll(polls,3,&zero,0)>=1&&(polls[2].revents&POLLIN));CHECK(!close(polls[1].fd)&&!close(polls[2].fd)&&!close(f[0])&&!close(f[1]));
        uint64_t t=now();struct timespec timeout={0,2000000};CHECK(ppoll(0,0,&timeout,0)==0&&now()-t>=1000000);
    }
    pin(0);CHECK(!munmap(m,(size_t)page*2));puts("PIPE CASE vectors-fault-fifo-sendfile-mixed-poll OK");
}
static void epoll_modes(void)
{
    const unsigned modes[]={0,EPOLLET,EPOLLONESHOT};
    for(unsigned cpu=0;cpu<cpus;cpu++)for(unsigned i=0;i<3;i++) {
        pin(cpu);int f[2];CHECK(!pipe(f));int ep=epoll_create1(0);CHECK(ep>=0);struct epoll_event e={.events=EPOLLIN|modes[i],.data.u64=7};CHECK(!epoll_ctl(ep,EPOLL_CTL_ADD,f[0],&e));CHECK(write(f[1],"a",1)==1);
        long page=sysconf(_SC_PAGESIZE);char *m=mmap(0,(size_t)page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(m!=MAP_FAILED&&!mprotect(m+page,(size_t)page,PROT_NONE));CHECK(epoll_wait(ep,(void *)(m+page-8),1,0)==-1&&errno==EFAULT);
        sigset_t mask;sigemptyset(&mask);CHECK(epoll_pwait(ep,&e,1,0,&mask)==1&&e.data.u64==7);CHECK(epoll_wait(ep,&e,1,0)==(modes[i]?0:1));
        e=(struct epoll_event){.events=EPOLLIN|modes[i],.data.u64=9};CHECK(!epoll_ctl(ep,EPOLL_CTL_MOD,f[0],&e));CHECK(epoll_wait(ep,&e,1,0)==1&&e.data.u64==9);CHECK(!epoll_ctl(ep,EPOLL_CTL_DEL,f[0],0));CHECK(epoll_wait(ep,&e,1,0)==0);CHECK(!close(ep)&&!close(f[0])&&!close(f[1])&&!munmap(m,(size_t)page*2));
    }
    if(!PIPE_SMOKE) {
    pin(0);int f[2];CHECK(!pipe(f));int ep=epoll_create1(0);CHECK(ep>=0);
    struct epoll_event event={.events=EPOLLIN|EPOLLONESHOT};CHECK(!epoll_ctl(ep,EPOLL_CTL_ADD,f[0],&event));
    struct blocked waiters[4]={0};pthread_t threads[4];
    for(unsigned cpu=0;cpu<cpus;cpu++) { waiters[cpu]=(struct blocked){.fd=ep,.cpu=cpu,.mode=6};CHECK(!pthread_create(&threads[cpu],0,reader,&waiters[cpu]));asleep(&waiters[cpu]); }
    CHECK(write(f[1],"s",1)==1);unsigned winners=0;
    for(unsigned cpu=0;cpu<cpus;cpu++) { CHECK(!pthread_join(threads[cpu],0)&&waiters[cpu].result>=0&&waiters[cpu].result<=1);winners+=(unsigned)waiters[cpu].result; }
    CHECK(winners==1&&!close(ep)&&!close(f[0])&&!close(f[1]));
    }
    puts("PIPE CASE epoll-modes-fault-rearm OK");
}
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);uint64_t mask=0;CHECK(syscall(SYS_sched_getaffinity,0,sizeof(mask),&mask)==0||mask);cpus=(unsigned)__builtin_popcountll(mask);CHECK(cpus>=1&&cpus<=4);
    unsigned char reference[128];for(unsigned i=0;i<sizeof(reference);i++)reference[i]=(unsigned char)i;
    int data=open("/pipe-data",O_CREAT|O_EXCL|O_RDWR,0600);CHECK(data>=0&&write(data,reference,sizeof(reference))==sizeof(reference)&&!fsync(data)&&!close(data));
    CHECK(!mkdir("/proc",0755)||errno==EEXIST);CHECK(!mount("proc","/proc","proc",0,0)||errno==EBUSY);
    int performance=!access("/pipe-perf",F_OK);if(!performance)observe();
    group("stream-one",1,0,performance);group("stream-four",4,0,performance);group("ppoll-four",4,1,performance);group("pselect-four",4,2,performance);group("epoll-four",4,3,performance);
    if(!performance) { if(!PIPE_SMOKE)lifecycle();boundaries();epoll_modes(); }
    pin(0);CHECK(!unlink("/pipe-data"));
    if(control) { uint64_t next=composition_load(&control->request)+1;composition_store(&control->command,COMPOSITION_STOP);composition_store(&control->request,next);while(composition_load(&control->reply)!=next)sched_yield();CHECK(!munmap(control,4096)&&!close(control_fd)&&!unlink("/dev/shm/smp-control")); }
    puts(performance?"PIPE-PERF END 5":PIPE_SMOKE?"PIPE END 7":"PIPE END 8");return 0;
}
