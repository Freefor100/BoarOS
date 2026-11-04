#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FD-EVENTS:%d %s errno=%d\n",__LINE__,#x,errno);exit(1); } } while (0)
static long close_range_raw(unsigned first,unsigned last,unsigned flags)
{ return syscall(436,first,last,flags); }
static int child_unshare(void *argument)
{
    int fd=*(int *)argument;
    if(close_range_raw((unsigned)fd,(unsigned)fd,2))return 11;
    errno=0;return fcntl(fd,F_GETFD)==-1 && errno==EBADF?0:12;
}
static void ranges(void)
{
    errno=0;CHECK(close_range_raw(5,4,0)==-1 && errno==EINVAL);
    errno=0;CHECK(close_range_raw(0,0,1)==-1 && errno==EINVAL);
    int base=open("/init",O_RDONLY);CHECK(base>=0);
    int a=fcntl(base,F_DUPFD,80),b=fcntl(base,F_DUPFD,81);CHECK(a==80 && b==81);
    CHECK(!close_range_raw(80,81,4));
    CHECK(fcntl(a,F_GETFD)==FD_CLOEXEC && fcntl(b,F_GETFD)==FD_CLOEXEC);
    void *stack=mmap(NULL,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(stack!=MAP_FAILED);
    pid_t child=clone(child_unshare,(char *)stack+65536,CLONE_FILES|SIGCHLD,&a);CHECK(child>=0);
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    CHECK(fcntl(a,F_GETFD)==FD_CLOEXEC);
    CHECK(!munmap(stack,65536));
    CHECK(!close_range_raw(80,UINT32_MAX,0));
    errno=0;CHECK(fcntl(a,F_GETFD)==-1 && errno==EBADF);
    CHECK(!close(base));puts("FD-EVENTS CASE close-range PASS");
}
static void permissions(void)
{
    const char *path="/api8-mode",*link="/api8-link";
    unlink(link);unlink(path);
    int fd=open(path,O_RDWR|O_CREAT|O_EXCL,0600);CHECK(fd>=0);
    CHECK(!syscall(452,fd,"",0640,0x1000));
    struct stat st;CHECK(!fstat(fd,&st) && (st.st_mode&0777)==0640);
    int opath=open(path,O_PATH);CHECK(opath>=0);
    CHECK(!syscall(452,opath,"",0604,0x1000));
    CHECK(!fstat(fd,&st) && (st.st_mode&0777)==0604);
    CHECK(!symlink(path,link));errno=0;
    CHECK(syscall(452,AT_FDCWD,link,0600,0x100)==-1 && errno==EOPNOTSUPP);
    errno=0;CHECK(syscall(452,AT_FDCWD,path,0600,1)==-1 && errno==EINVAL);
    CHECK(!close(opath) && !close(fd) && !unlink(link) && !unlink(path));
    int p[2],s[2];CHECK(!pipe(p) && !socketpair(AF_UNIX,SOCK_STREAM,0,s));
    CHECK(!syscall(452,p[0],"",0620,0x1000));
    CHECK(!fstat(p[1],&st) && (st.st_mode&0777)==0620);
    CHECK(!syscall(452,s[0],"",0640,0x1000));
    CHECK(!fstat(s[0],&st) && (st.st_mode&0777)==0640);
    int event=eventfd(0,0);CHECK(event>=0);errno=0;
    CHECK(syscall(452,event,"",0600,0x1000)==-1 && errno==EOPNOTSUPP);
    CHECK(!close(event) && !close(p[0]) && !close(p[1]) && !close(s[0]) && !close(s[1]));
    puts("FD-EVENTS CASE fchmodat2 PASS");
}
static volatile sig_atomic_t notifications;
static void notified(int signal) { (void)signal; notifications++; }
static void waits(void)
{
    int ep=epoll_create1(EPOLL_CLOEXEC),fd=eventfd(1,EFD_CLOEXEC);CHECK(ep>=0 && fd>=0);
    struct epoll_event add={.events=EPOLLIN|EPOLLONESHOT,.data.u64=0x1234},out;
    CHECK(!epoll_ctl(ep,EPOLL_CTL_ADD,fd,&add));
    struct timespec zero={0};
    CHECK(syscall(441,ep,&out,1,&zero,NULL,8)==1 && out.data.u64==0x1234);
    CHECK(syscall(441,ep,&out,1,&zero,NULL,8)==0);
    struct timespec bad={0,1000000000};errno=0;
    CHECK(syscall(441,ep,&out,1,&bad,NULL,8)==-1 && errno==EINVAL);
    errno=0;CHECK(syscall(441,-1,&out,1,(void *)1,NULL,8)==-1 && errno==EFAULT);
    struct timespec delay={0,1000001},begin,end;
    CHECK(!clock_gettime(CLOCK_MONOTONIC,&begin));
    CHECK(!syscall(441,ep,&out,1,&delay,NULL,8));
    CHECK(!clock_gettime(CLOCK_MONOTONIC,&end));
    CHECK((end.tv_sec-begin.tv_sec)*1000000000LL+end.tv_nsec-begin.tv_nsec>=1000001);
    struct sigaction action={.sa_handler=notified};sigemptyset(&action.sa_mask);
    CHECK(!sigaction(SIGUSR1,&action,NULL));
    sigset_t set,old,actual;sigemptyset(&set);sigaddset(&set,SIGUSR1);
    CHECK(!sigprocmask(SIG_BLOCK,&set,&old) && !raise(SIGUSR1));
    uint64_t mask=0;struct timespec second={1,0};errno=0;
    CHECK(syscall(441,ep,&out,1,&second,&mask,8)==-1 && errno==EINTR && notifications==1);
    CHECK(!sigprocmask(SIG_BLOCK,NULL,&actual) && sigismember(&actual,SIGUSR1)==1);
    CHECK(!sigprocmask(SIG_SETMASK,&old,NULL));
    CHECK(!close(fd) && !close(ep));puts("FD-EVENTS CASE epoll-pwait2 PASS");
}
static void instructions(void)
{
#if defined(__riscv)
    errno=0;CHECK(syscall(259,0,0,2)==-1 && errno==EINVAL);
    long page=sysconf(_SC_PAGESIZE);CHECK(page>0);
    int fd=open("/api8-code",O_RDWR|O_CREAT|O_TRUNC,0600);CHECK(fd>=0 && !ftruncate(fd,page));
    uint32_t *rw=mmap(NULL,(size_t)page,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    void *rx=mmap(NULL,(size_t)page,PROT_READ|PROT_EXEC,MAP_SHARED,fd,0);
    CHECK(rw!=MAP_FAILED && rx!=MAP_FAILED);
    rw[0]=0x00700513;rw[1]=0x00008067;
    CHECK(!syscall(259,rx,(char *)rx+8,0));CHECK(((int (*)(void))rx)()==7);
    rw[0]=0x00900513;
    CHECK(!syscall(259,rx,(char *)rx+8,1));CHECK(((int (*)(void))rx)()==9);
    CHECK(!munmap(rw,(size_t)page) && !munmap(rx,(size_t)page) && !close(fd) && !unlink("/api8-code"));
#else
    errno=0;CHECK(syscall(259,0,0,0)==-1 && errno==ENOSYS);
#endif
    puts("FD-EVENTS CASE instruction-sync PASS");
}
static void *remote(void *argument)
{
    unsigned cpu=(unsigned)(uintptr_t)argument;
    cpu_set_t set,actual;CPU_ZERO(&set);CPU_SET(cpu,&set);
    CHECK(!pthread_setaffinity_np(pthread_self(),sizeof(set),&set));
    CHECK(!pthread_getaffinity_np(pthread_self(),sizeof(actual),&actual) && CPU_COUNT(&actual)==1 && CPU_ISSET(cpu,&actual));
    CHECK(!close_range_raw(UINT32_MAX,UINT32_MAX,2|4));
    char path[64];snprintf(path,sizeof(path),"/tmp/api8-remote-%u",cpu);
    int fd=open(path,O_CREAT|O_EXCL|O_RDWR,0600);CHECK(fd>=0);
    CHECK(!syscall(452,fd,"",0640,0x1000) && !syscall(32,fd,2|4));
    int e=epoll_create1(0),event=eventfd(1,0);CHECK(e>=0 && event>=0);
    struct epoll_event add={.events=EPOLLIN,.data.u64=cpu},out;
    struct timespec zero={0};
    CHECK(!epoll_ctl(e,EPOLL_CTL_ADD,event,&add));
    CHECK(syscall(441,e,&out,1,&zero,NULL,8)==1 && out.data.u64==cpu);
    int pair[2];CHECK(!socketpair(AF_UNIX,SOCK_DGRAM,0,pair));
    char byte='x',received=0;struct iovec siov={&byte,1},riov={&received,1};
    struct mmsghdr send={.msg_hdr={.msg_iov=&siov,.msg_iovlen=1}},receive={.msg_hdr={.msg_iov=&riov,.msg_iovlen=1}};
    CHECK(syscall(269,pair[0],&send,1,0)==1 && send.msg_len==1);
    CHECK(syscall(243,pair[1],&receive,1,0,NULL)==1 && receive.msg_len==1 && received=='x');
    CHECK(!close(pair[0]) && !close(pair[1]) && !close(event) && !close(e) && !close(fd) && !unlink(path));
    printf("FD-EVENTS CPU %u PASS\n",cpu);return NULL;
}
static void callers(void)
{
    cpu_set_t set;CPU_ZERO(&set);CHECK(!sched_getaffinity(0,sizeof(set),&set));
    pthread_t threads[4];unsigned n=0;
    for(unsigned cpu=0;cpu<4;cpu++)if(CPU_ISSET(cpu,&set))CHECK(!pthread_create(&threads[n++],NULL,remote,(void *)(uintptr_t)cpu));
    for(unsigned i=0;i<n;i++)CHECK(!pthread_join(threads[i],NULL));
    CHECK(n>0);
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);ranges();permissions();waits();instructions();callers();
    puts("FD-EVENTS PASS");return 0;
}
