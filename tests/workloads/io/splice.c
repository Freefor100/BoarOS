#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/eventfd.h>
#include <sys/sysmacros.h>
#include <pthread.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "../../common/raw_syscall.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"SPLICE FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1); } } while(0)
static unsigned cases;static size_t page;static volatile sig_atomic_t broken,limited;
static long sp(int a,off_t *ao,int b,off_t *bo,size_t n,unsigned flags)
{ return test_syscall6(76,a,(long)ao,b,(long)bo,n,flags); }
static long tp(int a,int b,size_t n,unsigned flags)
{ return test_syscall6(77,a,b,n,flags,0,0); }
static long vm(int fd,const struct iovec *v,size_t n,unsigned flags)
{ return test_syscall6(75,fd,(long)v,n,flags,0,0); }
static void handler(int sig) { if(sig==SIGPIPE)broken++;else limited++; }
static void pass(const char *name) { printf("SPLICE CASE %s PASS\n",name);cases++; }
static void exact(int fd,const void *expected,size_t n)
{
    unsigned char *buf=malloc(n?n:1);CHECK(buf);size_t done=0;
    while(done<n) { ssize_t count=read(fd,buf+done,n-done);CHECK(count>0);done+=count; }
    CHECK(!memcmp(buf,expected,n));free(buf);
}
static void closepair(int *p) { CHECK(!close(p[0]) && !close(p[1])); }
static void errors(int fd)
{
    CHECK(sp(-1,NULL,-1,NULL,0,~0U)==0 && tp(-1,-1,0,~0U)==-EINVAL && tp(-1,-1,0,0)==0);
    CHECK(vm(-1,NULL,0,~0U)==-EINVAL && vm(-1,NULL,0,0)==-EBADF);
    CHECK(vm(fd,NULL,0,0)==0 && vm(fd,(void*)1,1,0)==-EFAULT);
    struct iovec v={"x",1};CHECK(vm(fd,&v,1,0)==-EBADF && vm(fd,&v,1025,0)==-EINVAL);
    CHECK(sp(-1,(void*)1,fd,NULL,1,0)==-EBADF && sp(fd,NULL,fd,NULL,1,0)==-EINVAL);
    int p[2];CHECK(!pipe2(p,O_NONBLOCK));
    CHECK(sp(p[0],(void*)1,fd,NULL,1,0)==-ESPIPE && sp(fd,NULL,p[1],(void*)1,1,0)==-ESPIPE);
    CHECK(sp(p[0],NULL,p[1],NULL,1,0)==-EINVAL && tp(p[0],p[1],1,0)==-EINVAL);
    CHECK(sp(p[0],NULL,fd,NULL,1,SPLICE_F_NONBLOCK)==-EAGAIN);
    CHECK(sp(p[1],NULL,fd,(void*)1,1,0)==-EFAULT); /* Output offset import precedes access mode. */
    CHECK(tp(p[0],fd,1,0)==-EINVAL);
    CHECK(close(p[1])==0 && sp(p[0],NULL,fd,NULL,1,0)==0 && close(p[0])==0);
    pass("abi-order-and-types");
}
static void pipe_links(void)
{
    int a[2],b[2];CHECK(!pipe2(a,O_NONBLOCK) && !pipe2(b,O_NONBLOCK));
    CHECK(write(a[1],"abcdefgh",8)==8 && tee(a[0],b[1],3,0)==3);exact(b[0],"abc",3);
    CHECK(sp(a[0],NULL,b[1],NULL,2,SPLICE_F_MOVE|SPLICE_F_MORE)==2);exact(b[0],"ab",2);
    CHECK(tp(a[0],b[1],6,0)==6);exact(a[0],"cdefgh",6);exact(b[0],"cdefgh",6);
    CHECK(tp(a[0],b[1],1,SPLICE_F_NONBLOCK)==-EAGAIN);
    char *data=malloc(page);CHECK(data);memset(data,0x2e,page);
    CHECK(fcntl(b[1],F_SETPIPE_SZ,page)==(int)page && write(a[1],data,page)==(ssize_t)page);
    CHECK(tp(a[0],b[1],page-1,0)==(long)page-1 && tp(a[0],b[1],1,SPLICE_F_NONBLOCK)==-EAGAIN);
    exact(b[0],data,page-1);CHECK(sp(a[0],NULL,b[1],NULL,page,0)==(long)page);exact(b[0],data,page);
    closepair(a);closepair(b);free(data);pass("pipe-move-tee-capacity");
}
static void user_pages(void)
{
    int p[2];CHECK(!pipe2(p,O_NONBLOCK));size_t size=3*page;
    char *data=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(data!=MAP_FAILED);
    for(size_t i=0;i<size;i++)data[i]=(char)(i*31+7);
    size_t length=2*page+16;char *expected=malloc(length);CHECK(expected);memcpy(expected,data+3,length);
    struct iovec v[]={{data+3,page-3},{data+page,page},{data+2*page,19}};
    CHECK(vmsplice(p[1],v,3,SPLICE_F_GIFT|SPLICE_F_NONBLOCK)==(ssize_t)length);
    CHECK(!munmap(data,size));exact(p[0],expected,length);free(expected);
    data=mmap(NULL,2*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(data!=MAP_FAILED);
    memset(data,0x4c,page);CHECK(!mprotect(data+page,page,PROT_NONE));
    struct iovec bad[]={{data,page},{data+page,1}};CHECK(vm(p[1],bad,2,0)==(long)page);exact(p[0],data,page);
    CHECK(vm(p[1],bad+1,1,0)==-EFAULT);
    CHECK(write(p[1],"xyz",3)==3);
    struct iovec out[]={{data,1},{data+page,2}};CHECK(vm(p[0],out,2,0)==-EFAULT);
    exact(p[0],"xyz",3); /* Faulted chunk is not consumed, even if user bytes were copied. */
    CHECK(!munmap(data,2*page));closepair(p);pass("vmsplice-pages-fault-and-lifetime");
}
static void files(int fd)
{
    int p[2];CHECK(!pipe(p));CHECK(pwrite(fd,"abcdefgh",8,0)==8 && lseek(fd,1,SEEK_SET)==1);
    CHECK(splice(fd,NULL,p[1],NULL,3,0)==3 && lseek(fd,0,SEEK_CUR)==4);exact(p[0],"bcd",3);
    off_t in=4;CHECK(sp(fd,&in,p[1],NULL,2,0)==2 && in==6 && lseek(fd,0,SEEK_CUR)==4);
    int target=memfd_create("splice-target",MFD_ALLOW_SEALING);CHECK(target>=0);off_t out=2;
    CHECK(sp(p[0],NULL,target,&out,2,0)==2 && out==4 && lseek(target,0,SEEK_CUR)==0);
    char bytes[8]={0};CHECK(pread(target,bytes,4,0)==4 && !memcmp(bytes,"\0\0ef",4));
    CHECK(fcntl(target,F_SETFL,O_APPEND)==0 && write(p[1],"q",1)==1);
    CHECK(sp(p[0],NULL,target,NULL,1,0)==-EINVAL);exact(p[0],"q",1);CHECK(fcntl(target,F_SETFL,0)==0);
    CHECK(fcntl(target,F_ADD_SEALS,F_SEAL_WRITE)==0 && write(p[1],"q",1)==1);
    CHECK(sp(p[0],NULL,target,NULL,1,0)==-EPERM);exact(p[0],"q",1);
    off_t *read_only=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(read_only!=MAP_FAILED);
    *read_only=0;CHECK(!mprotect(read_only,page,PROT_READ));
    CHECK(sp(fd,read_only,p[1],NULL,1,0)==-EFAULT && *read_only==0 && lseek(fd,0,SEEK_CUR)==4);exact(p[0],"a",1);
    CHECK(!munmap(read_only,page));
    in=3;CHECK(sp(fd,&in,p[1],NULL,1,0)==1 && write(p[1],"X",1)==1);exact(p[0],"dX",2);
    CHECK(pread(fd,bytes,8,0)==8 && !memcmp(bytes,"abcdefgh",8));
    int sparse=memfd_create("splice-sparse",0);CHECK(sparse>=0 && !ftruncate(sparse,3*page));
    char *mapped=mmap(NULL,3*page,PROT_READ,MAP_SHARED,sparse,0);CHECK(mapped!=MAP_FAILED);unsigned char resident[3];
    CHECK(!mincore(mapped,3*page,resident) && !(resident[0]&1));
    CHECK(sp(sparse,NULL,p[1],NULL,32,0)==32 && !mincore(mapped,3*page,resident) && !(resident[0]&1));
    char zeros[32]={0};exact(p[0],zeros,32);CHECK(!munmap(mapped,3*page) && !close(sparse));
    int control=open("/proc/sys/kernel/sched_rt_period_us",O_RDONLY);CHECK(control>=0);char expected[64]={0};
    ssize_t n=pread(control,expected,sizeof(expected),0);CHECK(n>0 && sp(control,NULL,p[1],NULL,n,0)==n);exact(p[0],expected,(size_t)n);CHECK(!close(control));
    CHECK(!mkfifo("/splice-fifo",0600));int fifo=open("/splice-fifo",O_RDWR|O_NONBLOCK);CHECK(fifo>=0);
    struct timespec old[2]={{1,0},{1,0}};CHECK(!futimens(fifo,old));off_t first=0;struct stat meta;
    CHECK(sendfile(fifo,fd,&first,1)==1 && !fstat(fifo,&meta) && meta.st_mtim.tv_sec==1);exact(fifo,"a",1);
    CHECK(!futimens(fifo,old));struct iovec one={"v",1};
    CHECK(vm(fifo,&one,1,0)==1 && !fstat(fifo,&meta) && meta.st_mtim.tv_sec==1);
    CHECK(sp(fifo,NULL,p[1],NULL,1,0)==1 && !fstat(fifo,&meta) && meta.st_atim.tv_sec==1);exact(p[0],"v",1);
    CHECK(write(p[1],"t",1)==1 && tp(p[0],fifo,1,0)==1 && !fstat(fifo,&meta) && meta.st_mtim.tv_sec==1);
    exact(p[0],"t",1);exact(fifo,"t",1);
    CHECK(!close(fifo) && !unlink("/splice-fifo"));
    CHECK(close(target)==0);closepair(p);pass("file-offsets-seals-and-nonmerge");
}
static void signals(int fd)
{
    struct sigaction act={0};act.sa_handler=handler;sigemptyset(&act.sa_mask);CHECK(!sigaction(SIGPIPE,&act,NULL) && !sigaction(SIGXFSZ,&act,NULL));
    int p[2],q[2];CHECK(!pipe(p) && !pipe(q));CHECK(write(p[1],"x",1)==1 && !close(q[0]));
    CHECK(sp(p[0],NULL,q[1],NULL,1,0)==-EPIPE && broken==1);exact(p[0],"x",1);
    struct iovec v={"x",1};CHECK(vm(q[1],&v,1,0)==-EPIPE && broken==2);
    CHECK(write(p[1],"q",1)==1);
    struct rlimit saved,zero;CHECK(!getrlimit(RLIMIT_FSIZE,&saved));zero=saved;zero.rlim_cur=0;CHECK(!setrlimit(RLIMIT_FSIZE,&zero));
    CHECK(sp(p[0],NULL,fd,NULL,1,0)==-EFBIG && limited==1);CHECK(!setrlimit(RLIMIT_FSIZE,&saved));exact(p[0],"q",1);
    closepair(p);CHECK(!close(q[1]));pass("epipe-fsize-and-consumption");
}
static void sockets(void)
{
    int s[2],p[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,s) && !pipe(p));
    CHECK(write(s[0],"stream",6)==6 && sp(s[1],NULL,p[1],NULL,6,0)==6);
    CHECK(sp(p[0],NULL,s[0],NULL,6,0)==6);exact(s[1],"stream",6);
    int enable=1;CHECK(!setsockopt(s[0],SOL_SOCKET,SO_PASSCRED,&enable,sizeof(enable)));
    CHECK(write(p[1],"cred",4)==4 && sp(p[0],NULL,s[0],NULL,4,0)==4);exact(s[1],"cred",4);
    int extra[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,extra));
    char control[CMSG_SPACE(sizeof(int))]={0};struct iovec v={"r",1};struct msghdr message={.msg_iov=&v,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof(control)};
    struct cmsghdr *c=CMSG_FIRSTHDR(&message);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(c),&extra[0],sizeof(int));
    CHECK(sendmsg(s[0],&message,0)==1 && !close(extra[0]) && sp(s[1],NULL,p[1],NULL,1,0)==1);exact(p[0],"r",1);CHECK(!close(extra[1]));
    CHECK(!shutdown(s[0],SHUT_WR) && sp(s[1],NULL,p[1],NULL,1,0)==0);
    closepair(s);closepair(p);
    CHECK(!socketpair(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK,0,s) && !pipe(p));
    size_t size=page+17;char *data=malloc(size+1),*copy=malloc(size+1);CHECK(data && copy);memset(data,0x37,size);
    CHECK(send(s[0],data,size,0)==(ssize_t)size && sp(s[1],NULL,p[1],NULL,size,0)==(long)size);
    CHECK(sp(p[0],NULL,s[0],NULL,size,0)==(long)size && recv(s[1],copy,size+1,0)==(ssize_t)size && !memcmp(copy,data,size));
    CHECK(recv(s[1],copy,1,MSG_DONTWAIT)==-1 && errno==EAGAIN);
    CHECK(send(s[0],NULL,0,0)==0 && sp(s[1],NULL,p[1],NULL,1,0)==0);
    CHECK(recv(s[1],copy,1,MSG_DONTWAIT)==-1 && errno==EAGAIN);
    CHECK(fcntl(p[1],F_SETPIPE_SZ,32*page)==(int)(32*page));struct iovec records[32];
    for(unsigned i=0;i<32;i++){data[i]=(char)i;records[i]=(struct iovec){data+i,1};}
    CHECK(vm(p[1],records,32,0)==32 && sp(p[0],NULL,s[0],NULL,32,0)==32);
    CHECK(recv(s[1],copy,64,0)==16 && !memcmp(copy,data,16));
    CHECK(recv(s[1],copy,64,0)==16 && !memcmp(copy,data+16,16));
    closepair(s);closepair(p);free(data);free(copy);
    int server=socket(AF_INET,SOCK_STREAM,0),client=socket(AF_INET,SOCK_STREAM,0);CHECK(server>=0 && client>=0);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};socklen_t length=sizeof(address);
    CHECK(!bind(server,(void*)&address,sizeof(address)) && !listen(server,1) && !getsockname(server,(void*)&address,&length));
    CHECK(!connect(client,(void*)&address,length));int peer=accept(server,NULL,NULL);CHECK(peer>=0 && !pipe(p));
    CHECK(write(client,"tcp",3)==3 && sp(peer,NULL,p[1],NULL,3,0)==3 && sp(p[0],NULL,client,NULL,3,0)==3);
    exact(peer,"tcp",3);CHECK(!close(peer) && !close(client) && !close(server));closepair(p);
    pass("stream-socket-roundtrip");
}
static void devices(void)
{
    CHECK(!mknod("/splice-zero",S_IFCHR|0600,makedev(1,5)) && !mknod("/splice-null",S_IFCHR|0600,makedev(1,3)));
    int zero=open("/splice-zero",O_RDWR),null=open("/splice-null",O_RDWR),event=eventfd(0,0),p[2];
    CHECK(zero>=0 && null>=0 && event>=0 && !pipe(p));
    CHECK(sp(null,NULL,p[1],NULL,1,0)==-EINVAL);
    CHECK(sp(p[0],NULL,event,NULL,1,SPLICE_F_NONBLOCK)==-EINVAL);
    off_t offset=7;CHECK(sp(zero,&offset,p[1],NULL,32,0)==32 && offset==7);
    char expected[32]={0};exact(p[0],expected,32);
    offset=9;CHECK(write(p[1],"discard",7)==7 && sp(p[0],NULL,null,&offset,7,0)==7 && offset==9);
    CHECK(!close(zero) && !close(null) && !close(event));closepair(p);
    CHECK(!unlink("/splice-zero") && !unlink("/splice-null"));pass("character-directions");
}
struct restart_call {int input,output,notice;long result;pthread_barrier_t start;};
static volatile sig_atomic_t restart_signals;
static void restart_handler(int signal) {(void)signal;restart_signals++;}
static void *restart_sender(void *pointer)
{
    struct restart_call *call=pointer;pthread_barrier_wait(&call->start);
    call->result=sp(call->input,NULL,call->output,NULL,4096,0);
    CHECK(write(call->notice,"r",1)==1);return NULL;
}
static void partial_restart(void)
{
    struct sigaction action={0};action.sa_handler=restart_handler;action.sa_flags=SA_RESTART;sigemptyset(&action.sa_mask);
    CHECK(!sigaction(SIGUSR1,&action,NULL));
    int p[2],s[2],notice[2];CHECK(!pipe(p) && !pipe(notice) && !socketpair(AF_UNIX,SOCK_STREAM,0,s));
    int size=1;CHECK(!setsockopt(s[0],SOL_SOCKET,SO_SNDBUF,&size,sizeof(size)) && fcntl(p[1],F_SETPIPE_SZ,64*page)==(int)(64*page));
    char *data=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(data!=MAP_FAILED);memset(data,0x69,page);
    struct iovec pieces[64];for(unsigned i=0;i<64;i++)pieces[i]=(struct iovec){data+64*i,64};
    CHECK(vm(p[1],pieces,64,0)==4096);
    struct restart_call call={.input=p[0],.output=s[0],.notice=notice[1]};CHECK(!pthread_barrier_init(&call.start,NULL,2));
    pthread_t sender;CHECK(!pthread_create(&sender,NULL,restart_sender,&call));pthread_barrier_wait(&call.start);
    struct pollfd input={s[1],POLLIN,0},output={s[0],POLLOUT,0};CHECK(poll(&input,1,2000)==1);
    struct timespec begin,now;CHECK(!clock_gettime(CLOCK_MONOTONIC,&begin));
    for(;;) {
        CHECK(poll(&output,1,0)>=0);if(!(output.revents&POLLOUT))break;
        sched_yield();CHECK(!clock_gettime(CLOCK_MONOTONIC,&now) && now.tv_sec-begin.tv_sec<3);
    }
    CHECK(!pthread_kill(sender,SIGUSR1));struct pollfd done={notice[0],POLLIN,0};
    CHECK(poll(&done,1,2000)==1 && !pthread_join(sender,NULL));
    CHECK(restart_signals==1 && call.result>0 && call.result<4096);
    exact(s[1],data,(size_t)call.result);exact(p[0],data,4096-(size_t)call.result);
    CHECK(!pthread_barrier_destroy(&call.start) && !munmap(data,page));closepair(p);closepair(s);closepair(notice);
    pass("partial-socket-signal-not-restarted");
}
struct endpoint { int p[2],ack[2],cpu;unsigned id;pthread_barrier_t start; };
static void *sender(void *pointer)
{
    struct endpoint *e=pointer;cpu_set_t set;CPU_ZERO(&set);CPU_SET(e->cpu,&set);CHECK(!pthread_setaffinity_np(pthread_self(),sizeof(set),&set));
    char *data=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(data!=MAP_FAILED);
    pthread_barrier_wait(&e->start);
    for(unsigned i=0;i<16;i++){ memset(data,e->id+1,64);memcpy(data,&i,sizeof(i));struct iovec v={data,64};CHECK(vm(e->p[1],&v,1,0)==64);char ack;CHECK(read(e->ack[0],&ack,1)==1); }
    CHECK(!munmap(data,page));return NULL;
}
static void parallel(void)
{
    cpu_set_t set;CHECK(!sched_getaffinity(0,sizeof(set),&set));int cpus[8],nr=0;
    for(int i=0;i<CPU_SETSIZE && nr<8;i++)if(CPU_ISSET(i,&set))cpus[nr++]=i;
    CHECK(nr);
    struct endpoint e[4];pthread_t workers[4];
    for(unsigned i=0;i<4;i++){e[i].cpu=cpus[i%nr];e[i].id=i;CHECK(!pipe(e[i].p) && !pipe(e[i].ack) && !pthread_barrier_init(&e[i].start,NULL,2));CHECK(!pthread_create(&workers[i],NULL,sender,&e[i]));}
    for(unsigned i=0;i<4;i++)pthread_barrier_wait(&e[i].start);
    for(unsigned n=0;n<16;n++)for(unsigned i=0;i<4;i++){char bytes[64];CHECK(read(e[i].p[0],bytes,64)==64);unsigned seq;memcpy(&seq,bytes,sizeof(seq));CHECK(seq==n && (unsigned char)bytes[63]==i+1);CHECK(write(e[i].ack[1],"a",1)==1);}
    for(unsigned i=0;i<4;i++){CHECK(!pthread_join(workers[i],NULL) && !pthread_barrier_destroy(&e[i].start));closepair(e[i].p);closepair(e[i].ack);}
    printf("SPLICE caller CPUs=%d\n",nr);pass("caller-cpus");
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);page=(size_t)sysconf(_SC_PAGESIZE);
    int fd=open("/splice-file",O_CREAT|O_TRUNC|O_RDWR,0600);CHECK(fd>=0);
    errors(fd);pipe_links();partial_restart();user_pages();files(fd);signals(fd);sockets();devices();parallel();
    CHECK(!fsync(fd) && !close(fd) && !unlink("/splice-file"));printf("SPLICE PASS cases=%u\n",cases);return 0;
}
