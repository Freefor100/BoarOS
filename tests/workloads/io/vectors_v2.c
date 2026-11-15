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
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../../common/raw_syscall.h"

#ifndef RWF_NOAPPEND
#define RWF_NOAPPEND 0x20
#endif
#ifndef RWF_NOSIGNAL
#define RWF_NOSIGNAL 0x100
#endif
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"VECTORS2 FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1); } } while (0)
static unsigned cases;
static volatile sig_atomic_t pipes,limits;
static void handler(int sig) { if (sig==SIGPIPE) pipes++;else limits++; }
static void pass(const char *name) { printf("VECTORS2 CASE %s PASS\n",name);cases++; }
static long call(int writing,int fd,const struct iovec *iov,long count,int64_t off,uint64_t flags)
{ return test_syscall6(writing?287:286,fd,(long)iov,count,off,0x7abcdef,flags); }
static void exact(int fd,const char *expected,size_t size)
{
    char bytes[64]={0};CHECK(size<=sizeof(bytes));
    CHECK(pread(fd,bytes,size,0)==(ssize_t)size && !memcmp(bytes,expected,size));
}
static void positions(int fd)
{
    struct iovec iov[]={{"abcd",4},{NULL,0}};
    CHECK(call(1,fd,iov,2,0,0)==4 && lseek(fd,0,SEEK_CUR)==0);
    char bytes[4]={0};struct iovec out={bytes,4};
    CHECK(call(0,fd,&out,1,-1,0)==4 && !memcmp(bytes,"abcd",4) && lseek(fd,0,SEEK_CUR)==4);
    CHECK(lseek(fd,1,SEEK_SET)==1 && call(0,fd,&out,1,0,0)==4 && lseek(fd,0,SEEK_CUR)==1);
    CHECK(pwritev2(fd,iov,1,1,0)==4 && lseek(fd,0,SEEK_CUR)==1);
    CHECK(preadv2(fd,&out,1,1,0)==4 && !memcmp(bytes,"abcd",4));
    CHECK(lseek(fd,0,SEEK_SET)==0 && pwritev2(fd,iov,1,-1,0)==4 && lseek(fd,0,SEEK_CUR)==4);
    CHECK(ftruncate(fd,0)==0);
    int64_t far=(INT64_C(1)<<32)+17;
    CHECK(call(1,fd,iov,1,far,0)==4 && call(0,fd,&out,1,far,0)==4 && !memcmp(bytes,"abcd",4));
    CHECK(lseek(fd,0,SEEK_CUR)==4 && ftruncate(fd,0)==0);
    pass("positions-libc-native-width");
}
static void options(int fd)
{
    struct iovec iov={"ab",2};CHECK(lseek(fd,0,SEEK_SET)==0);
    CHECK(call(1,fd,&iov,1,0,RWF_DSYNC)==2);
    CHECK(call(1,fd,&iov,1,2,RWF_SYNC)==2 && lseek(fd,0,SEEK_CUR)==0);
    CHECK(call(1,fd,&iov,1,0,RWF_APPEND)==2);exact(fd,"ababab",6);
    CHECK(fcntl(fd,F_SETFL,O_APPEND)==0);
    CHECK(call(1,fd,&iov,1,0,0)==2);exact(fd,"abababab",8);
    struct iovec last={"XY",2};
    CHECK(call(1,fd,&last,1,0,RWF_NOAPPEND)==2 && lseek(fd,0,SEEK_CUR)==0);
    exact(fd,"XYababab",8);CHECK(fcntl(fd,F_GETFL)&O_APPEND);
    CHECK(call(1,fd,&last,1,-1,RWF_APPEND)==2 && lseek(fd,0,SEEK_CUR)==10);
    CHECK(call(1,fd,&iov,1,0,RWF_APPEND|RWF_NOAPPEND)==-EINVAL);
    CHECK(fcntl(fd,F_SETFL,0)==0);
    char bytes[2];struct iovec out={bytes,2};
    CHECK(call(0,fd,&out,1,0,RWF_HIPRI|RWF_DSYNC|RWF_SYNC|RWF_APPEND|RWF_NOSIGNAL)==2);
    CHECK(call(0,fd,&out,1,0,UINT64_C(1)<<32)==2);
    int only=open("/vectors-append-only",O_WRONLY|O_APPEND);CHECK(only>=0);
    CHECK(call(1,only,&iov,1,0,RWF_NOAPPEND)==-EPERM && call(1,only,&iov,1,0,0)==2);
    CHECK(fcntl(only,F_GETFL)&O_APPEND);CHECK(close(only)==0);
    only=open("/vectors-append-only",O_RDONLY|O_APPEND);CHECK(only>=0);
    CHECK(call(0,only,&out,1,0,RWF_NOAPPEND)==-EPERM && call(0,only,&out,1,0,0)==2);
    CHECK(close(only)==0);
    pass("per-call-append-sync");
}
static void vectors_faults(int fd)
{
    struct iovec *iov=calloc(1025,sizeof(*iov));CHECK(iov);
    char bytes[8]={0};
    for (unsigned n=8;n<=1024;n=n==8?9:1024) {
        iov[n-1]=(struct iovec){bytes,2};
        CHECK(call(0,fd,iov,n,0,0)==2);iov[n-1]=(struct iovec){0};
        if (n==1024) break;
    }
    CHECK(call(0,fd,iov,1025,0,0)==-EINVAL);
    struct iovec out={bytes,2};
    CHECK(call(0,-1,&out,1,-2,0)==-EINVAL && call(0,-1,&out,1,-1,0)==-EBADF);
    const unsigned unsupported[]={8,64,128,0x80000000U};
    for(unsigned i=0;i<sizeof(unsupported)/sizeof(*unsupported);i++) {
        long result=call(0,fd,&out,1,0,unsupported[i]);
        printf("VECTORS2 CAP flag=%u result=%ld\n",unsupported[i],result);
        CHECK(result==-EOPNOTSUPP || ((unsupported[i]==8 || unsupported[i]==128) && result==2) ||
              (unsupported[i]==8 && result==-EAGAIN));
    }
    CHECK(call(0,fd,(void *)1,1,0,0x80000000U)==-EFAULT);
    struct iovec split[]={{bytes,4},{(void *)1,4}};
    CHECK(call(0,fd,split,2,0,0)==4);
    split[0]=(struct iovec){"1234",4};
    CHECK(call(1,fd,split,2,0,RWF_SYNC)==4);exact(fd,"1234",4);
    CHECK(call(0,fd,NULL,0,0,0)==0);
    struct iovec tiny={bytes,1};
    int upper=memfd_create("upper",0);CHECK(upper>=0);
    for(int writing=0;writing<2;writing++) {
        CHECK(call(writing,fd,&tiny,1,INT64_MAX,0)==-EINVAL);
        CHECK(call(writing,fd,&tiny,1,INT64_MAX,UINT32_C(0x80000000))==-EINVAL);
        CHECK(lseek(upper,INT64_MAX,SEEK_SET)==INT64_MAX);
        CHECK(call(writing,upper,&tiny,1,-1,UINT32_C(0x80000000))==-EINVAL);
    }
    CHECK(close(upper)==0);
    free(iov);pass("vectors-errors-prefix");
}
static void signals_and_seals(int fd)
{
    struct sigaction sa={.sa_handler=handler};sigemptyset(&sa.sa_mask);
    CHECK(!sigaction(SIGPIPE,&sa,NULL) && !sigaction(SIGXFSZ,&sa,NULL));
    struct iovec iov={"a",1};
    for(unsigned socket=0;socket<2;socket++) {
        int pair[2];CHECK(socket?socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0:pipe(pair)==0);
        int output=socket?pair[0]:pair[1];CHECK(close(socket?pair[1]:pair[0])==0);
        sig_atomic_t old=pipes;
        CHECK(call(1,output,&iov,1,-1,RWF_NOSIGNAL)==-EPIPE && pipes==old);
        CHECK(call(1,output,&iov,1,-1,0)==-EPIPE && pipes==old+1 && close(output)==0);
    }
    struct rlimit old,zero={0,RLIM_INFINITY};CHECK(!getrlimit(RLIMIT_FSIZE,&old));
    zero.rlim_max=old.rlim_max;CHECK(!setrlimit(RLIMIT_FSIZE,&zero));
    CHECK(call(1,fd,&iov,1,0,RWF_NOSIGNAL)==-EFBIG && limits==1);
    CHECK(!setrlimit(RLIMIT_FSIZE,&old));
    int mem=memfd_create("vectors2",MFD_ALLOW_SEALING);CHECK(mem>=0);
    CHECK(!fcntl(mem,F_ADD_SEALS,F_SEAL_WRITE));
    CHECK(call(1,mem,&iov,1,0,RWF_APPEND|RWF_SYNC)==-EPERM && !close(mem));
    int event=eventfd(1,0);CHECK(event>=0);uint64_t value=0;struct iovec ev={&value,8};
    CHECK(call(0,event,&ev,1,-1,RWF_SYNC)==8 && value==1 && !close(event));
    pass("signal-targets-seals");
}
struct actor { int fd,cpu;pthread_barrier_t *gate; };
static void *append_worker(void *argument)
{
    struct actor *a=argument;cpu_set_t set;CPU_ZERO(&set);CPU_SET(a->cpu,&set);
    CHECK(!pthread_setaffinity_np(pthread_self(),sizeof(set),&set));
    int wait=pthread_barrier_wait(a->gate);CHECK(wait==0 || wait==PTHREAD_BARRIER_SERIAL_THREAD);
    unsigned char bytes[64];memset(bytes,a->cpu+1,sizeof(bytes));struct iovec iov={bytes,sizeof(bytes)};
    for(unsigned i=0;i<16;i++) CHECK(call(1,a->fd,&iov,1,0,RWF_APPEND)==sizeof(bytes));
    CHECK(sched_getcpu()==a->cpu);return NULL;
}
static void concurrent_append(int fd)
{
    cpu_set_t set;CHECK(!sched_getaffinity(0,sizeof(set),&set));
    struct actor actors[8];pthread_t threads[8];unsigned n=0;
    for(int i=0;i<CPU_SETSIZE && n<8;i++) if(CPU_ISSET(i,&set)) actors[n++]=(struct actor){.fd=fd,.cpu=i};
    CHECK(n && !ftruncate(fd,0) && lseek(fd,0,SEEK_SET)==0);
    pthread_barrier_t gate;CHECK(!pthread_barrier_init(&gate,NULL,n));
    for(unsigned i=0;i<n;i++) {actors[i].gate=&gate;CHECK(!pthread_create(&threads[i],NULL,append_worker,&actors[i]));}
    for(unsigned i=0;i<n;i++) CHECK(!pthread_join(threads[i],NULL));
    CHECK(!pthread_barrier_destroy(&gate) && lseek(fd,0,SEEK_CUR)==0 && !(fcntl(fd,F_GETFL)&O_APPEND));
    struct stat st;CHECK(!fstat(fd,&st) && st.st_size==(off_t)(n*16*64));
    unsigned seen[CPU_SETSIZE]={0};
    for(off_t pos=0;pos<st.st_size;pos+=64) {
        unsigned char bytes[64];CHECK(pread(fd,bytes,64,pos)==64 && bytes[0] && CPU_ISSET(bytes[0]-1,&set));
        for(unsigned i=1;i<64;i++) CHECK(bytes[i]==bytes[0]);
        seen[bytes[0]-1]++;
    }
    for(unsigned i=0;i<n;i++) CHECK(seen[actors[i].cpu]==16);
    pass("caller-cpus-atomic-append");
}
static void local_user_copy(int fd)
{
    size_t page=(size_t)sysconf(_SC_PAGESIZE),size=2*page;
    unsigned char *input=mmap(NULL,3*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    unsigned char *output=mmap(NULL,3*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(input!=MAP_FAILED && output!=MAP_FAILED);
    for(size_t i=0;i<size;i++)input[i]=(unsigned char)(i*17+3);
    CHECK(pwrite(fd,input,size,0)==(ssize_t)size);
    /* Cold destination faults return to the copy owner before page preparation. */
    CHECK(pread(fd,output,size,0)==(ssize_t)size && !memcmp(input,output,size));
    for(unsigned offset=0;offset<64;offset++) {
        CHECK(pread(fd,output+offset,129,0)==129 && !memcmp(input,output+offset,129));
    }
    CHECK(!mprotect(output+page,page,PROT_NONE));
    CHECK(pread(fd,output,size,0)==(ssize_t)page && !memcmp(input,output,page));
    CHECK(!mprotect(output,page,PROT_READ));
    errno=0;CHECK(pread(fd,output,32,0)==-1 && errno==EFAULT);
    CHECK(!mprotect(output,2*page,PROT_READ|PROT_WRITE));
    memset(output,0x91,page);
    pid_t child=fork();CHECK(child>=0);
    if(!child) {
        if(pread(fd,output,page,0)!=(ssize_t)page || memcmp(input,output,page))_exit(7);
        _exit(0);
    }
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    for(size_t i=0;i<page;i++)CHECK(output[i]==0x91);
    int shared_fd=open("/vectors-local-shared",O_RDWR|O_CREAT|O_TRUNC,0600);
    CHECK(shared_fd>=0 && !ftruncate(shared_fd,(off_t)size));
    unsigned char *shared=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_SHARED,shared_fd,0);
    CHECK(shared!=MAP_FAILED && pread(fd,shared,size,0)==(ssize_t)size);
    CHECK(!msync(shared,size,MS_SYNC));
    CHECK(pread(shared_fd,output,size,0)==(ssize_t)size && !memcmp(input,output,size));
    /* A completed snapshot revoked the old W PTEs. The next kernel store must
     * enter the same alias write preparation as a user-mode store. */
    CHECK(pread(fd,shared,size,0)==(ssize_t)size && !fsync(shared_fd));
    CHECK(!mprotect(shared,size,PROT_READ));errno=0;
    CHECK(pread(fd,shared,32,0)==-1 && errno==EFAULT);
    CHECK(!munmap(shared,size));
    unsigned char *private=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_PRIVATE,shared_fd,0);
    int pipefd[2];CHECK(private!=MAP_FAILED && !pipe(pipefd));
    CHECK(write(pipefd[1],"private",8)==8 && read(pipefd[0],private,8)==8 && !memcmp(private,"private",8));
    CHECK(pread(shared_fd,output,8,0)==8 && !memcmp(input,output,8));
    CHECK(!close(pipefd[0]) && !close(pipefd[1]) && !munmap(private,page));
    CHECK(!close(shared_fd) && !unlink("/vectors-local-shared"));
    CHECK(!pipe(pipefd) && !mprotect(input+page,page,PROT_NONE));
    CHECK(write(pipefd[1],input,size)==(ssize_t)page);
    CHECK(read(pipefd[0],output,page)==(ssize_t)page && !memcmp(input,output,page));
    CHECK(!close(pipefd[0]) && !close(pipefd[1]));
    char *name=(char *)input+page-sizeof("/vectors-v2.data");
    memcpy(name,"/vectors-v2.data",sizeof("/vectors-v2.data"));
    int opened=open(name,O_RDONLY);CHECK(opened>=0 && !close(opened));
    memset(input+page-8,'x',8);errno=0;
    CHECK(open((char *)input+page-8,O_RDONLY)==-1 && errno==EFAULT);
    CHECK(!munmap(input,3*page) && !munmap(output,3*page));
    int tail=open("/vectors-eof-tail",O_RDWR|O_CREAT|O_TRUNC,0600);
    CHECK(tail>=0 && write(tail,"short file",10)==10 && !fsync(tail));
    CHECK(!posix_fadvise(tail,0,0,POSIX_FADV_DONTNEED));
    unsigned char *mapped=mmap(NULL,page,PROT_READ,MAP_PRIVATE,tail,0);
    CHECK(mapped!=MAP_FAILED && !memcmp(mapped,"short file",10));
    for(size_t i=10;i<page;i++)CHECK(!mapped[i]);
    unsigned char *suffix=malloc(page);CHECK(suffix);memset(suffix,0xa7,page);
    CHECK(pread(tail,suffix,page,0)==10 && !memcmp(suffix,"short file",10));
    for(size_t i=10;i<page;i++)CHECK(suffix[i]==0xa7);
    free(suffix);CHECK(!munmap(mapped,page) && !close(tail) && !unlink("/vectors-eof-tail"));
    CHECK(!ftruncate(fd,0));
    pass("local-copy-fault-prefix-cow-string");
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    int fd=open("/vectors-v2.data",O_RDWR|O_CREAT|O_TRUNC,0600);CHECK(fd>=0);
    local_user_copy(fd);positions(fd);options(fd);vectors_faults(fd);signals_and_seals(fd);concurrent_append(fd);
    CHECK(!fsync(fd) && !close(fd) && !unlink("/vectors-v2.data"));
    printf("VECTORS2 PASS cases=%u\n",cases);return 0;
}
