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
    CHECK(lseek(fd,100,SEEK_SET)==0);expect(pread(fd,&(uint64_t){0},8,0),ESPIPE);
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
    expect(write(fd,&value,8),EINVAL);expect(timerfd_settime(fd,0,(void *)(map+page),0),EFAULT);
    t.it_value.tv_nsec=1000000000;expect(timerfd_settime(-1,0,&t,0),EINVAL);
    CHECK(!munmap(map,(size_t)page*2)&&!close(fd));puts("EVENT CASE timer-fault-commit OK");
}
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);alarm(30);event_counter();event_vectors();timer_basic();timer_fault();puts("EVENT END 4");return 0;
}
