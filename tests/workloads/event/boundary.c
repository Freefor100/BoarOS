#define _GNU_SOURCE
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <poll.h>
static unsigned bad;
#define CHECK(x) do{if(!(x)){printf("EVENT BOUND FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);bad++;}}while(0)
static int error(long n,int e){return n==-1&&errno==e;}
static uint64_t get(int fd){uint64_t v=0;CHECK(read(fd,&v,8)==8);return v;}
static void put(int fd,uint64_t v){CHECK(write(fd,&v,8)==8);}
static int add(int fd,unsigned events){int ep=epoll_create1(0);struct epoll_event e={.events=events};CHECK(ep>=0&&!epoll_ctl(ep,EPOLL_CTL_ADD,fd,&e));return ep;}
int main(void)
{
 setvbuf(stdout,0,_IONBF,0);alarm(10);unsigned begin=bad;uint64_t n=1;int fd=eventfd(3,EFD_NONBLOCK);CHECK(fd>=0);
 CHECK(error(pwrite(fd,&n,8,0),ESPIPE));CHECK(get(fd)==3);close(fd);fd=timerfd_create(CLOCK_MONOTONIC,TFD_NONBLOCK);CHECK(error(pwrite(fd,&n,8,0),ESPIPE));close(fd);printf("EVENT BOUND positioned %s\n",bad==begin?"OK":"FAIL");
 begin=bad;fd=eventfd(7,EFD_NONBLOCK);CHECK(error(syscall(SYS_read,fd,(void *)UINTPTR_MAX,8),EFAULT));CHECK(get(fd)==7);CHECK(error(syscall(SYS_read,fd,(void *)UINTPTR_MAX,8),EFAULT));put(fd,7);CHECK(error(syscall(SYS_read,fd,&n,UINT64_MAX),EFAULT));CHECK(get(fd)==7);close(fd);printf("EVENT BOUND original-range %s\n",bad==begin?"OK":"FAIL");
 begin=bad;fd=eventfd(0,EFD_NONBLOCK);int ep=add(fd,EPOLLOUT|EPOLLET);struct epoll_event e;CHECK(epoll_wait(ep,&e,1,0)==1);put(fd,1);CHECK(epoll_wait(ep,&e,1,0)==0);close(ep);close(fd);fd=eventfd(2,EFD_SEMAPHORE|EFD_NONBLOCK);ep=add(fd,EPOLLIN|EPOLLET);CHECK(epoll_wait(ep,&e,1,0)==1&&get(fd)==1);CHECK(epoll_wait(ep,&e,1,0)==0);close(ep);close(fd);printf("EVENT BOUND directed-et %s\n",bad==begin?"OK":"FAIL");
 begin=bad;fd=eventfd(0,EFD_NONBLOCK);struct iovec empty={&n,0};CHECK(writev(fd,&empty,1)==0);CHECK(error(write(fd,&n,0),EINVAL));close(fd);printf("EVENT BOUND empty-vector %s\n",bad==begin?"OK":"FAIL");
 begin=bad;fd=timerfd_create(CLOCK_MONOTONIC,TFD_NONBLOCK);CHECK(error(syscall(SYS_write,fd,(void *)UINTPTR_MAX,8),EINVAL));CHECK(error(syscall(SYS_writev,fd,(void *)UINTPTR_MAX,1),EINVAL));CHECK(error(writev(fd,&empty,1),EINVAL));close(fd);printf("EVENT BOUND timer-write %s\n",bad==begin?"OK":"FAIL");
 return bad?1:0;
}
