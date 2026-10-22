#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/select.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include "../../smp/composition_protocol.h"
#include <sys/timerfd.h>
#include <sys/uio.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x) do { if(!(x)){dprintf(2,"EVENT FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);} }while(0)
static void expect(ssize_t value,int error){CHECK(value==-1&&errno==error);}
static void put(int fd,uint64_t value){CHECK(write(fd,&value,8)==8);}
static uint64_t get(int fd){uint64_t value;CHECK(read(fd,&value,8)==8);return value;}
static void event_counter(void)
{
    int fd=eventfd(3,EFD_NONBLOCK|EFD_CLOEXEC);CHECK(fd>=0);CHECK(fcntl(fd,F_GETFD)==FD_CLOEXEC);
    CHECK((fcntl(fd,F_GETFL)&(O_ACCMODE|O_NONBLOCK))==(O_RDWR|O_NONBLOCK));
    struct stat s;CHECK(!fstat(fd,&s)&&(s.st_mode&0777)==0600);
    CHECK(lseek(fd,100,SEEK_SET)==0);expect(pread(fd,&(uint64_t){0},8,0),ESPIPE);expect(pwrite(fd,&(uint64_t){1},8,0),ESPIPE);
    int ep=epoll_create1(0);CHECK(ep>=0);struct epoll_event ev={.events=EPOLLIN|EPOLLONESHOT,.data.u64=42};CHECK(!epoll_ctl(ep,EPOLL_CTL_ADD,fd,&ev));
    put(fd,7);CHECK(epoll_wait(ep,&ev,1,0)==1&&ev.data.u64==42);CHECK(epoll_wait(ep,&ev,1,0)==0);CHECK(!close(ep));CHECK(get(fd)==10);uint64_t value;expect(read(fd,&value,8),EAGAIN);
    put(fd,UINT64_MAX-1);expect(write(fd,&(uint64_t){1},8),EAGAIN);expect(write(fd,&(uint64_t){UINT64_MAX},8),EINVAL);
    struct pollfd p={fd,POLLIN|POLLOUT,0};CHECK(poll(&p,1,0)==1&&(p.revents&POLLIN)&&!(p.revents&POLLOUT));
    CHECK(get(fd)==UINT64_MAX-1);put(fd,0);expect(read(fd,&value,8),EAGAIN);
    expect(read(fd,&value,7),EINVAL);expect(write(fd,&value,7),EINVAL);CHECK(!close(fd));
    fd=eventfd(2,EFD_NONBLOCK|EFD_SEMAPHORE);CHECK(fd>=0&&get(fd)==1&&get(fd)==1);expect(read(fd,&value,8),EAGAIN);CHECK(!close(fd));
    puts("EVENT CASE counter-flags-limit OK");
}
static void event_vectors(void)
{
    int fd=eventfd(0,EFD_NONBLOCK);CHECK(fd>=0);uint64_t value=0,part=0;
    struct iovec iov[2]={{&value,3},{(char *)&value+3,5}};
    put(fd,0x123456789);CHECK(readv(fd,iov,2)==8&&value==0x123456789);
    struct iovec writes[2]={{&part,4},{(char *)&part+4,4}};expect(writev(fd,writes,2),EINVAL);
    struct iovec two[2]={{&(uint64_t){2},8},{&(uint64_t){3},8}};CHECK(writev(fd,two,2)==16&&get(fd)==5);
    long page=sysconf(_SC_PAGESIZE);char *map=mmap(0,(size_t)page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(map!=MAP_FAILED);
    CHECK(!mprotect(map+page,(size_t)page,PROT_NONE));put(fd,11);expect(read(fd,map+page-3,8),EFAULT);expect(read(fd,&value,8),EAGAIN);
    expect(write(fd,map+page-3,8),EFAULT);expect(read(fd,&value,8),EAGAIN);CHECK(!munmap(map,(size_t)page*2));
    int alias=dup(fd);CHECK(alias>=0);CHECK(!close(fd));put(alias,9);CHECK(get(alias)==9);CHECK(!close(alias));
    puts("EVENT CASE vectors-fault-alias OK");
}
static uint64_t nsclock(clockid_t clock)
{ struct timespec t;CHECK(!clock_gettime(clock,&t));return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec; }
static void timer_basic(void)
{
    for(unsigned clock=0;clock<2;clock++) {
        int fd=timerfd_create((clockid_t)clock,TFD_NONBLOCK|TFD_CLOEXEC);CHECK(fd>=0);uint64_t ticks;
        expect(read(fd,&ticks,8),EAGAIN);struct itimerspec t={0},old;
        t.it_value.tv_nsec=20000000;CHECK(!timerfd_settime(fd,0,&t,&old)&&!old.it_value.tv_sec&&!old.it_value.tv_nsec);
        struct pollfd p={fd,POLLIN,0};CHECK(poll(&p,1,1000)==1&&(p.revents&POLLIN)&&get(fd)==1);
        uint64_t target=nsclock((clockid_t)clock)+20000000;t.it_value=(struct timespec){(time_t)(target/1000000000),(long)(target%1000000000)};
        CHECK(!timerfd_settime(fd,TFD_TIMER_ABSTIME|TFD_TIMER_CANCEL_ON_SET,&t,0));CHECK(poll(&p,1,1000)==1&&get(fd)==1);
        t=(struct itimerspec){.it_interval={0,10000000},.it_value={0,10000000}};
        CHECK(!timerfd_settime(fd,0,&t,0));uint64_t wake=nsclock(CLOCK_MONOTONIC)+55000000;
        struct timespec until={(time_t)(wake/1000000000),(long)(wake%1000000000)};CHECK(!clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&until,0));
        CHECK(!timerfd_gettime(fd,&old)&&old.it_interval.tv_nsec==10000000&&old.it_value.tv_nsec<=10000000);
        CHECK(get(fd)>=5);t=(struct itimerspec){0};CHECK(!timerfd_settime(fd,0,&t,0));expect(read(fd,&ticks,8),EAGAIN);CHECK(!close(fd));
    }
    puts("EVENT CASE timer-clocks-periodic OK");
}
static void timer_fault(void)
{
    int fd=timerfd_create(CLOCK_MONOTONIC,TFD_NONBLOCK);CHECK(fd>=0);
    long page=sysconf(_SC_PAGESIZE);char *map=mmap(0,(size_t)page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(map!=MAP_FAILED&&!mprotect(map+page,(size_t)page,PROT_NONE));
    struct itimerspec t={.it_value={0,10000000}};expect(timerfd_settime(fd,0,&t,(struct itimerspec *)(map+page)),EFAULT);
    struct pollfd p={fd,POLLIN,0};CHECK(poll(&p,1,1000)==1);CHECK(read(fd,map+page-3,8)==3);uint64_t value;expect(read(fd,&value,8),EAGAIN);
    struct itimerspec again={.it_value={0,1000000}};CHECK(!timerfd_settime(fd,0,&again,0));fd_set reads;FD_ZERO(&reads);FD_SET(fd,&reads);struct timespec timeout={1,0};CHECK(pselect(fd+1,&reads,0,0,&timeout,0)==1&&FD_ISSET(fd,&reads)&&get(fd)==1);
    expect(write(fd,&value,8),EINVAL);expect(timerfd_settime(fd,0,(void *)(map+page),0),EFAULT);
    t.it_value.tv_nsec=1000000000;expect(timerfd_settime(-1,0,&t,0),EINVAL);
    CHECK(!munmap(map,(size_t)page*2)&&!close(fd));puts("EVENT CASE timer-fault-commit OK");
}
#ifndef EVENT_SMOKE
#define EVENT_SMOKE 0
#endif
static unsigned cpus;
static struct composition_control *control;
static int control_fd=-1;
static void pin(unsigned cpu){uint64_t mask=UINT64_C(1)<<cpu;CHECK(!syscall(SYS_sched_setaffinity,0,sizeof(mask),&mask));}
static void gate(pthread_barrier_t *b){int r=pthread_barrier_wait(b);CHECK(!r||r==PTHREAD_BARRIER_SERIAL_THREAD);}
static void observe(void)
{
    if(access("/event-observer",F_OK))return;
    CHECK(!mkdir("/dev/shm",0700)||errno==EEXIST);CHECK(!mount("tmpfs","/dev/shm","tmpfs",0,0)||errno==EBUSY);
    control_fd=open("/dev/shm/smp-control",O_CREAT|O_EXCL|O_RDWR,0600);CHECK(control_fd>=0&&!ftruncate(control_fd,4096));
    control=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED,control_fd,0);CHECK(control!=MAP_FAILED);
    composition_store(&control->magic,COMPOSITION_MAGIC);while(!composition_load(&control->observer_ready))sched_yield();
}
static int epcreate(int fd)
{int ep=epoll_create1(0);CHECK(ep>=0);struct epoll_event e={.events=EPOLLIN,.data.u64=42};CHECK(!epoll_ctl(ep,EPOLL_CTL_ADD,fd,&e));return ep;}
static uint64_t receive(int fd,int ep)
{if(ep>=0){struct epoll_event e;CHECK(epoll_wait(ep,&e,1,-1)==1&&(e.events&EPOLLIN));}return get(fd);}
struct channel {int fd[2],ep[2];unsigned id,rounds;pthread_barrier_t *gate;uint64_t start[2],end[2],samples[1024];};
struct actor {struct channel *channel;unsigned side;};
static void *exchange(void *argument)
{
    struct actor *a=argument;struct channel *c=a->channel;unsigned side=a->side;pin((c->id+side)%cpus);gate(c->gate);c->start[side]=nsclock(CLOCK_MONOTONIC);
    for(unsigned i=0;i<c->rounds;i++) {
        uint64_t n=(uint64_t)i*4+c->id+1,t=nsclock(CLOCK_MONOTONIC);
        if(!side){put(c->fd[0],n);CHECK(receive(c->fd[1],c->ep[1])==n);c->samples[i]=nsclock(CLOCK_MONOTONIC)-t;}
        else {CHECK(receive(c->fd[0],c->ep[0])==n);put(c->fd[1],n);}
    }
    c->end[side]=nsclock(CLOCK_MONOTONIC);return 0;
}
static int compare(const void *a,const void *b){uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;return(x>y)-(x<y);}
static uint64_t cpu_us(struct rusage *u);
static void event_group(int epoll,int perf)
{
    struct channel *c=calloc(4,sizeof(*c));CHECK(c);pthread_t threads[8];struct actor actors[8];pthread_barrier_t b;CHECK(!pthread_barrier_init(&b,0,8));
    for(unsigned i=0;i<4;i++){c[i].id=i;c[i].rounds=perf?1024:32;c[i].gate=&b;for(unsigned j=0;j<2;j++){c[i].fd[j]=eventfd(0,0);CHECK(c[i].fd[j]>=0);c[i].ep[j]=epoll?epcreate(c[i].fd[j]):-1;}}
    struct rusage cpu_start;CHECK(!getrusage(RUSAGE_SELF,&cpu_start));
    for(unsigned i=0;i<8;i++){actors[i]=(struct actor){&c[i/2],i%2};CHECK(!pthread_create(&threads[i],0,exchange,&actors[i]));}
    for(unsigned i=0;i<8;i++)CHECK(!pthread_join(threads[i],0));
    uint64_t begin=UINT64_MAX,end=0,samples[4096];unsigned count=0;
    for(unsigned i=0;i<4;i++){for(unsigned j=0;j<2;j++){if(c[i].start[j]<begin)begin=c[i].start[j];if(c[i].end[j]>end)end=c[i].end[j];CHECK(!close(c[i].fd[j]));if(c[i].ep[j]>=0)CHECK(!close(c[i].ep[j]));}for(unsigned j=0;j<c[i].rounds;j++)samples[count++]=c[i].samples[j];}
    qsort(samples,count,sizeof(*samples),compare);struct rusage u;CHECK(!getrusage(RUSAGE_SELF,&u));
    if(perf)printf("EVENT-PERF name=eventfd-%s count=%u ns=%llu p50=%llu p95=%llu max=%llu ticks=0 cpu_us=%llu rss_kib=%ld\n",epoll?"epoll":"direct",count,(unsigned long long)(end-begin),(unsigned long long)samples[(count-1)/2],(unsigned long long)samples[(count*95-1)/100],(unsigned long long)samples[count-1],(unsigned long long)(cpu_us(&u)-cpu_us(&cpu_start)),u.ru_maxrss);
    else printf("EVENT CASE eventfd-%s OK\n",epoll?"epoll":"direct");
    CHECK(!pthread_barrier_destroy(&b));free(c);
}
struct timer_actor {int fd,ep;unsigned id,rounds;pthread_barrier_t *gate;uint64_t begin,end,ticks,samples[128];};
static void *timer_reader(void *argument)
{
    struct timer_actor *a=argument;pin(a->id%cpus);gate(a->gate);
    uint64_t target=nsclock(CLOCK_MONOTONIC)+2000000,seen=0;
    struct itimerspec setting={.it_interval={0,2000000},.it_value={(time_t)(target/1000000000),(long)(target%1000000000)}};
    CHECK(!timerfd_settime(a->fd,TFD_TIMER_ABSTIME,&setting,0));a->begin=nsclock(CLOCK_MONOTONIC);
    for(unsigned i=0;i<a->rounds;i++){uint64_t n=receive(a->fd,a->ep);CHECK(n);seen+=n;uint64_t now=nsclock(CLOCK_MONOTONIC),due=target+(seen-1)*2000000;CHECK(now>=due);a->samples[i]=now-due;}
    a->ticks=seen;a->end=nsclock(CLOCK_MONOTONIC);return 0;
}
static uint64_t cpu_us(struct rusage *u){return(uint64_t)u->ru_utime.tv_sec*1000000+u->ru_utime.tv_usec+(uint64_t)u->ru_stime.tv_sec*1000000+u->ru_stime.tv_usec;}
static void timer_group(int epoll,int perf)
{
    struct timer_actor a[4]={0};pthread_t threads[4];pthread_barrier_t b;CHECK(!pthread_barrier_init(&b,0,4));
    for(unsigned i=0;i<4;i++){a[i].fd=timerfd_create(CLOCK_MONOTONIC,0);CHECK(a[i].fd>=0);a[i].ep=epoll?epcreate(a[i].fd):-1;a[i].gate=&b;a[i].id=i;a[i].rounds=perf?128:4;}
    struct rusage before,after;CHECK(!getrusage(RUSAGE_SELF,&before));
    for(unsigned i=0;i<4;i++)CHECK(!pthread_create(&threads[i],0,timer_reader,&a[i]));
    for(unsigned i=0;i<4;i++)CHECK(!pthread_join(threads[i],0));
    CHECK(!getrusage(RUSAGE_SELF,&after));uint64_t begin=UINT64_MAX,end=0,ticks=0,samples[512];unsigned count=0;
    for(unsigned i=0;i<4;i++){if(a[i].begin<begin)begin=a[i].begin;if(a[i].end>end)end=a[i].end;ticks+=a[i].ticks;for(unsigned j=0;j<a[i].rounds;j++)samples[count++]=a[i].samples[j];CHECK(!close(a[i].fd));if(a[i].ep>=0)CHECK(!close(a[i].ep));}
    qsort(samples,count,sizeof(*samples),compare);
    if(perf)printf("EVENT-PERF name=timerfd-%s count=%u ns=%llu p50=%llu p95=%llu max=%llu ticks=%llu cpu_us=%llu rss_kib=%ld\n",epoll?"epoll":"direct",count,(unsigned long long)(end-begin),(unsigned long long)samples[(count-1)/2],(unsigned long long)samples[(count*95-1)/100],(unsigned long long)samples[count-1],(unsigned long long)ticks,(unsigned long long)(cpu_us(&after)-cpu_us(&before)),after.ru_maxrss);
    CHECK(!pthread_barrier_destroy(&b));
}
struct blocked {int fd,mode;unsigned cpu;uint64_t tid,value;ssize_t result;int error;};
static void *blocked_io(void *argument)
{
 struct blocked *b=argument;pin(b->cpu);__atomic_store_n(&b->tid,(uint64_t)syscall(SYS_gettid),__ATOMIC_RELEASE);
 if(b->mode==2){struct pollfd p={b->fd,POLLIN,0};b->result=ppoll(&p,1,0,0);}
 else b->result=b->mode==3?write(b->fd,&(uint64_t){1},8):read(b->fd,&b->value,8);
 b->error=errno;return 0;
}
static void asleep(struct blocked *b)
{
 uint64_t tid;while(!(tid=__atomic_load_n(&b->tid,__ATOMIC_ACQUIRE)))sched_yield();
 if(control){uint64_t next=composition_load(&control->request)+1;composition_store(&control->target_tid,tid);composition_store(&control->command,COMPOSITION_SOCKET_WAIT);composition_store(&control->request,next);while(composition_load(&control->reply)!=next)sched_yield();return;}
 char path[96],bytes[512];snprintf(path,sizeof(path),"/proc/self/task/%llu/stat",(unsigned long long)tid);
 for(unsigned i=0;i<100000;i++){int fd=open(path,O_RDONLY);CHECK(fd>=0);ssize_t n=read(fd,bytes,sizeof(bytes)-1);CHECK(n>0&&!close(fd));bytes[n]=0;char *p=strrchr(bytes,')');CHECK(p);if(p[2]=='S')return;sched_yield();}CHECK(0);
}
static volatile sig_atomic_t handled;
static void handler(int sig){(void)sig;handled++;}
static void lifecycle(void)
{
 if(!EVENT_SMOKE){
  struct sigaction action={.sa_handler=handler};sigemptyset(&action.sa_mask);CHECK(!sigaction(SIGUSR1,&action,0));
  for(unsigned cpu=0;cpu<cpus;cpu++)for(unsigned mode=0;mode<5;mode++){
   action.sa_flags=mode==4?SA_RESTART:0;CHECK(!sigaction(SIGUSR1,&action,0));
   int fd=eventfd(0,0),alias=dup(fd);CHECK(fd>=0&&alias>=0);if(mode==3)put(fd,UINT64_MAX-1);
   struct blocked b={.fd=fd,.mode=(int)mode,.cpu=cpu};pthread_t t;CHECK(!pthread_create(&t,0,blocked_io,&b));asleep(&b);
   if(!mode){CHECK(!close(fd));int replace=open("/data",O_RDONLY);CHECK(replace==fd);put(alias,19);CHECK(!pthread_join(t,0)&&b.result==8&&b.value==19);CHECK(!close(replace));}
   else if(mode==1||mode==2){CHECK(!pthread_kill(t,SIGUSR1));int joined=pthread_join(t,0);if(joined||b.result!=-1||b.error!=EINTR)dprintf(2,"EVENT interrupt cpu=%u mode=%u join=%d result=%ld error=%d\n",cpu,mode,joined,(long)b.result,b.error);CHECK(!joined&&b.result==-1&&b.error==EINTR);CHECK(!close(fd));}
   else if(mode==4){sig_atomic_t old=handled;CHECK(!pthread_kill(t,SIGUSR1));while(handled==old)sched_yield();asleep(&b);put(alias,23);CHECK(!pthread_join(t,0)&&b.result==8&&b.value==23);CHECK(!close(fd));}
   else {CHECK(get(alias)==UINT64_MAX-1);CHECK(!pthread_join(t,0)&&b.result==8&&get(alias)==1);CHECK(!close(fd));}
   CHECK(!close(alias));
  }
 }
 int fd=eventfd(1,EFD_NONBLOCK),alias=dup(fd);CHECK(fd>=0&&alias>=0&&!fcntl(alias,F_SETFD,FD_CLOEXEC));
 pid_t child=fork();CHECK(child>=0);if(!child){char a[24],b[24];snprintf(a,sizeof(a),"%d",fd);snprintf(b,sizeof(b),"%d",alias);execl("/init","/init","--exec-event",a,b,(char *)0);_exit(2);}int status;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));uint64_t value;expect(read(fd,&value,8),EAGAIN);CHECK(!close(fd)&&!close(alias));
 puts("EVENT CASE lifecycle-close-reuse-signal-exec OK");
}
int main(int argc,char **argv)
{
 if(argc==4&&!strcmp(argv[1],"--exec-event")){CHECK(fcntl(atoi(argv[2]),F_GETFD)==0);expect(fcntl(atoi(argv[3]),F_GETFD),EBADF);CHECK(get(atoi(argv[2]))==1);return 0;}
 setvbuf(stdout,0,_IONBF,0);alarm(30);uint64_t mask=0;CHECK(syscall(SYS_sched_getaffinity,0,sizeof(mask),&mask)>=0);cpus=__builtin_popcountll(mask);CHECK(cpus&&cpus<=4);
 int perf=!access("/event-perf",F_OK);if(!perf){CHECK(!mkdir("/proc",0755)||errno==EEXIST);CHECK(!mount("proc","/proc","proc",0,0)||errno==EBUSY);observe();event_counter();event_vectors();timer_basic();timer_fault();}
 event_group(0,perf);event_group(1,perf);timer_group(0,perf);timer_group(1,perf);
 if(!perf){puts("EVENT CASE timerfd-remote OK");lifecycle();}
 pin(0);if(control){uint64_t next=composition_load(&control->request)+1;composition_store(&control->command,COMPOSITION_STOP);composition_store(&control->request,next);while(composition_load(&control->reply)!=next)sched_yield();CHECK(!munmap(control,4096)&&!close(control_fd)&&!unlink("/dev/shm/smp-control"));}
 puts(perf?"EVENT-PERF END 4":"EVENT END 8");return 0;
}
