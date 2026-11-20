#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/aio_abi.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { printf("AIO FAIL line %d %s errno %d\n",__LINE__,#x,errno); exit(1); } } while (0)
struct ring { unsigned id, nr, head, tail, magic, compat, incompat, size; struct io_event ev[]; };
static long submit(aio_context_t ctx, struct iocb *cb)
{
    return syscall(SYS_io_submit,ctx,1,&cb);
}
static struct io_event event(aio_context_t ctx)
{
    struct io_event e;
    struct timespec timeout={2,0};
    CHECK(syscall(SYS_io_getevents,ctx,1,1,&e,&timeout)==1);
    return e;
}
static long proc_value(const char *name)
{
    FILE *f=fopen(name,"r");
    long value;
    CHECK(f && fscanf(f,"%ld",&value)==1 && !fclose(f));
    return value;
}
static void direct_io(aio_context_t ctx)
{
    int fd=open("aio-direct",O_CREAT|O_TRUNC|O_DIRECT|O_RDWR,0600);
    CHECK(fd>=0);
    char *data;
    CHECK(!posix_memalign((void **)&data,4096,8192));
    memset(data,0x6d,4096);
    struct iocb cb={.aio_lio_opcode=IOCB_CMD_PWRITE,.aio_fildes=fd,
        .aio_buf=(uintptr_t)data,.aio_nbytes=4096};
    CHECK(submit(ctx,&cb)==1 && event(ctx).res==4096);
    struct iovec iov={data+4096,4096};
    cb.aio_lio_opcode=IOCB_CMD_PREADV;
    cb.aio_buf=(uintptr_t)&iov;
    cb.aio_nbytes=1;
    CHECK(submit(ctx,&cb)==1 && event(ctx).res==4096 && !memcmp(data,data+4096,4096));
    unsigned before_return=0;
    struct ring *ring=(void *)(uintptr_t)ctx;
    for (unsigned i=0;i<16;i++) {
        cb.aio_lio_opcode=i&1?IOCB_CMD_PREAD:IOCB_CMD_PWRITE;
        cb.aio_buf=(uintptr_t)(i&1?data+4096:data);
        cb.aio_nbytes=4096;
        unsigned tail=__atomic_load_n(&ring->tail,__ATOMIC_ACQUIRE);
        CHECK(submit(ctx,&cb)==1);
        before_return+=tail==__atomic_load_n(&ring->tail,__ATOMIC_ACQUIRE);
        CHECK(event(ctx).res==4096);
    }
    printf("AIO direct requests still pending after submit %u/16\n",before_return);
#ifdef BOAROS_AIO_WORKER
    CHECK(before_return>0);
#endif
    size_t page=sysconf(_SC_PAGESIZE);
    char *destination=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(destination!=MAP_FAILED);
    cb.aio_lio_opcode=IOCB_CMD_PREAD;
    cb.aio_buf=(uintptr_t)destination;
    CHECK(submit(ctx,&cb)==1);
    CHECK(mmap(destination,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==destination);
    memset(destination,0x28,page);
    CHECK(event(ctx).res==4096);
    for (size_t i=0;i<page;i++) CHECK(destination[i]==0x28);
    CHECK(!munmap(destination,page));
    cb.aio_lio_opcode=IOCB_CMD_FSYNC;
    cb.aio_buf=cb.aio_nbytes=0;
    CHECK(submit(ctx,&cb)==1 && !event(ctx).res);
    CHECK(!close(fd));
    if (getpid()!=1) CHECK(!unlink("aio-direct"));
    free(data);
}
static void pending_signal(aio_context_t ctx)
{
    sigset_t mask,old;
    sigemptyset(&mask);
    sigaddset(&mask,SIGUSR1);
    CHECK(!sigprocmask(SIG_BLOCK,&mask,&old));
    int fd=signalfd(-1,&mask,SFD_NONBLOCK);
    CHECK(fd>=0);
    struct iocb cb={.aio_lio_opcode=IOCB_CMD_POLL,.aio_fildes=fd,.aio_buf=POLLIN};
    CHECK(!raise(SIGUSR1));
    CHECK(submit(ctx,&cb)==1);
    CHECK(event(ctx).res&POLLIN);
    struct signalfd_siginfo info;
    CHECK(read(fd,&info,sizeof(info))==sizeof(info) && info.ssi_signo==SIGUSR1);
    CHECK(!close(fd) && !sigprocmask(SIG_SETMASK,&old,NULL));
}

static void pending_exit(int exec_child)
{
    aio_context_t ctx=0;
    int fd=eventfd(0,0);
    CHECK(fd>=0 && !syscall(SYS_io_setup,4,&ctx));
    struct iocb cb={.aio_lio_opcode=IOCB_CMD_POLL,.aio_fildes=fd,.aio_buf=POLLIN};
    CHECK(submit(ctx,&cb)==1);
    if (exec_child) {
        execl("/proc/self/exe","init","--exec-child",(char *)NULL);
        /* The minimal fixture also installs the executable directly at /init. */
        execl("/init","init","--exec-child",(char *)NULL);
        CHECK(0);
    }
    _exit(0);
}
static volatile sig_atomic_t caught;
static void catch_signal(int number)
{
    caught=number;
}
static void interrupted_pgetevents(void)
{
    int started[2];
    CHECK(!pipe(started));
    pid_t child=fork();
    CHECK(child>=0);
    if (!child) {
        aio_context_t ctx=0;
        struct sigaction action={.sa_handler=catch_signal,.sa_flags=SA_RESTART};
        sigemptyset(&action.sa_mask);
        CHECK(!sigaction(SIGUSR1,&action,NULL));
        sigset_t block,observed;
        sigemptyset(&block); sigaddset(&block,SIGUSR1);
        CHECK(!sigprocmask(SIG_BLOCK,&block,NULL));
        CHECK(!syscall(SYS_io_setup,1,&ctx));
        uint64_t empty=0;
        struct {uint64_t *address; size_t size;} mask={&empty,sizeof(empty)};
        struct io_event events[2];
        struct timespec timeout={0,300000000};
        CHECK(!raise(SIGUSR1));
        CHECK(syscall(SYS_io_pgetevents,ctx,1,1,events,&timeout,&mask)==-1 && errno==EINTR);
        CHECK(caught==SIGUSR1 && !sigprocmask(SIG_BLOCK,NULL,&observed) &&
            sigismember(&observed,SIGUSR1));
        CHECK(write(started[1],"r",1)==1);
        CHECK(syscall(SYS_io_pgetevents,ctx,1,1,events,&timeout,&mask)==0);
        CHECK(!sigprocmask(SIG_BLOCK,NULL,&observed) && sigismember(&observed,SIGUSR1));
        int ready=eventfd(1,0);
        CHECK(ready>=0);
        struct iocb cb={.aio_lio_opcode=IOCB_CMD_POLL,.aio_fildes=ready,.aio_buf=POLLIN};
        CHECK(submit(ctx,&cb)==1 && write(started[1],"r",1)==1);
        CHECK(syscall(SYS_io_pgetevents,ctx,2,2,events,&timeout,&mask)==1 && (events[0].res&POLLIN));
        CHECK(!sigprocmask(SIG_BLOCK,NULL,&observed) && sigismember(&observed,SIGUSR1));
        CHECK(!close(ready) && !syscall(SYS_io_destroy,ctx));
        _exit(0);
    }
    CHECK(!close(started[1]));
    char ack;
    int status;
    for (int i=0;i<2;i++) {
        CHECK(read(started[0],&ack,1)==1);
        usleep(20000);
        CHECK(!kill(child,SIGSTOP));
        CHECK(waitpid(child,&status,WUNTRACED)==child && WIFSTOPPED(status) && WSTOPSIG(status)==SIGSTOP);
        CHECK(!kill(child,SIGCONT));
    }
    CHECK(waitpid(child,&status,0)==child && !status);
    CHECK(!close(started[0]));
}

struct exiting_poll {aio_context_t ctx; int published; struct iocb cb;};
static int signalfd_child(void *arg)
{
    struct exiting_poll *poll=arg;
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask,SIGUSR1);
    int fd=signalfd(-1,&mask,0);
    if (fd<0) { __atomic_store_n(&poll->published,-1,__ATOMIC_RELEASE); return 1; }
    poll->cb=(struct iocb){.aio_lio_opcode=IOCB_CMD_POLL,.aio_fildes=fd,.aio_buf=POLLIN};
    int ok=submit(poll->ctx,&poll->cb)==1;
    __atomic_store_n(&poll->published,ok?1:-1,__ATOMIC_RELEASE);
    return !ok;
}
static void signalfd_source_exit(void)
{
    /* CLONE_VM shares the AIO context; the child's private signal table owns
     * the wait queue. Its teardown races context destruction in the parent. */
    size_t stack_size=128*1024;
    char *stack=mmap(NULL,stack_size,PROT_WRITE|PROT_READ,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(stack!=MAP_FAILED);
    for (int i=0;i<4;i++) {
        struct exiting_poll poll={0};
        CHECK(!syscall(SYS_io_setup,1,&poll.ctx));
        pid_t pid=clone(signalfd_child,stack+stack_size,CLONE_VM|SIGCHLD,&poll);
        CHECK(pid>0);
        while (!__atomic_load_n(&poll.published,__ATOMIC_ACQUIRE)) sched_yield();
        CHECK(poll.published==1 && !syscall(SYS_io_destroy,poll.ctx));
        int status;
        CHECK(waitpid(pid,&status,0)==pid && !status);
    }
    CHECK(!munmap(stack,stack_size));
}

struct blocked_reader {aio_context_t ctx; int ready;};
static void *wait_for_destroy(void *arg)
{
    struct blocked_reader *reader=arg;
    struct io_event event;
    __atomic_store_n(&reader->ready,1,__ATOMIC_RELEASE);
    long result=syscall(SYS_io_getevents,reader->ctx,1,1,&event,NULL);
    CHECK(result==-1 && errno==EINVAL);
    return NULL;
}
static void concurrent_destroy(void)
{
    for (unsigned i=0;i<8;i++) {
        struct blocked_reader reader={0};
        pthread_t thread;
        CHECK(!syscall(SYS_io_setup,1,&reader.ctx));
        CHECK(!pthread_create(&thread,NULL,wait_for_destroy,&reader));
        while (!__atomic_load_n(&reader.ready,__ATOMIC_ACQUIRE)) sched_yield();
        usleep(1000);
        CHECK(!syscall(SYS_io_destroy,reader.ctx));
        CHECK(!pthread_join(thread,NULL));
    }
}
int main(int argc,char **argv)
{
    if (argc==2 && !strcmp(argv[1],"--exec-child")) return 0;
    setbuf(stdout,NULL);
    aio_context_t ctx=0;
    CHECK(!syscall(SYS_io_setup,8,&ctx));
    struct ring *ring=(void *)(uintptr_t)ctx;
    CHECK(ring->nr>8 && ring->magic==0xa10a10a1 && ring->size==32 && !ring->head && !ring->tail);
    if (access("/proc/sys/fs/aio-nr",F_OK)) {
        mkdir("/proc",0555);
        CHECK(!mount("proc","/proc","proc",0,NULL));
    }
    long active=proc_value("/proc/sys/fs/aio-nr");
    CHECK(active>=8 && proc_value("/proc/sys/fs/aio-max-nr")>=8);
    int fd=open("aio-file",O_CREAT|O_TRUNC|O_RDWR,0600), efd=eventfd(0,0);
    CHECK(fd>=0 && efd>=0);
    char bytes[]="abc", dest[4]={0};
    struct iovec iov[2]={{bytes,1},{bytes+1,2}};
    struct iocb cb={.aio_data=42,.aio_lio_opcode=IOCB_CMD_PWRITEV,.aio_fildes=fd,
        .aio_buf=(uintptr_t)iov,.aio_nbytes=2,.aio_offset=7,.aio_flags=IOCB_FLAG_RESFD,.aio_resfd=efd};
    struct iocb *batch[]={&cb,NULL};
    CHECK(syscall(SYS_io_submit,ctx,2,batch)==1);
    uint64_t signals=0;
    CHECK(read(efd,&signals,8)==8 && signals==1);
    CHECK(ring->tail!=ring->head && ring->ev[ring->head].res==3 && ring->ev[ring->head].data==42);
    __atomic_store_n(&ring->head,(ring->head+1)%ring->nr,__ATOMIC_RELEASE);
    cb=(struct iocb){.aio_lio_opcode=IOCB_CMD_PREAD,.aio_fildes=fd,.aio_buf=(uintptr_t)dest,.aio_nbytes=3,.aio_offset=7};
    CHECK(submit(ctx,&cb)==1);
    struct io_event e=event(ctx);
    CHECK(e.obj==(uintptr_t)&cb && e.res==3 && !strcmp(bytes,dest) && !lseek(fd,0,SEEK_CUR));
    cb=(struct iocb){.aio_lio_opcode=IOCB_CMD_FDSYNC,.aio_fildes=fd};
    CHECK(submit(ctx,&cb)==1 && !event(ctx).res);
    int p[2]; CHECK(!pipe(p));
    cb=(struct iocb){.aio_lio_opcode=IOCB_CMD_POLL,.aio_fildes=p[0],.aio_buf=POLLIN};
    CHECK(submit(ctx,&cb)==1);
    CHECK(syscall(SYS_io_cancel,ctx,&cb,NULL)==-1 && errno==EINPROGRESS);
    CHECK(!event(ctx).res);
    CHECK(submit(ctx,&cb)==1 && !close(p[0]));
    CHECK(write(p[1],"x",1)==1 && (event(ctx).res&POLLIN));
    struct timespec timeout={0,0};
    CHECK(!syscall(SYS_io_pgetevents,ctx,0,1,&e,&timeout,NULL));
    pid_t child=fork(); CHECK(child>=0);
    if (!child) _exit(syscall(SYS_io_destroy,ctx)==-1 && errno==EINVAL ? 0:1);
    int status; CHECK(waitpid(child,&status,0)==child && !status);
    size_t length=ring->size+ring->nr*sizeof(struct io_event);
    void *new_base=mmap(NULL,length,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(new_base!=MAP_FAILED);
    CHECK(mremap(ring,length,length,MREMAP_MAYMOVE|MREMAP_FIXED,new_base)==new_base);
    ctx=(uintptr_t)new_base;
    direct_io(ctx);
    concurrent_destroy();
    interrupted_pgetevents();
    signalfd_source_exit();
    pending_signal(ctx);
    for (int i=0;i<2;i++) {
        child=fork(); CHECK(child>=0);
        if (!child) pending_exit(i);
        CHECK(waitpid(child,&status,0)==child && !status);
        CHECK(proc_value("/proc/sys/fs/aio-nr")==active);
    }
    aio_context_t invalid=0;
    CHECK(syscall(SYS_io_setup,0,&invalid)==-1 && errno==EINVAL);
    CHECK(syscall(SYS_io_setup,65537,&invalid)==-1 && errno==EAGAIN);
    struct {uint64_t *address,size;} mask;
    uint64_t blocked=0;
    mask.address=&blocked; mask.size=1;
    CHECK(syscall(SYS_io_pgetevents,ctx,0,1,&e,&timeout,&mask)==-1 && errno==EINVAL);
    mask.size=sizeof(blocked);
    CHECK(!syscall(SYS_io_pgetevents,ctx,0,1,&e,&timeout,&mask));
    CHECK(!syscall(SYS_io_destroy,ctx));
    CHECK(proc_value("/proc/sys/fs/aio-nr")==active-8);
    CHECK(!close(p[1]) && !close(fd) && !close(efd));
    if (getpid()!=1) CHECK(!unlink("aio-file"));
    puts("NATIVE AIO ALL PASS");
    if (getpid()==1) {
        sync();
        /* Linux reboots without unmounting the fixture's ext4 root. */
        (void)mount(NULL,"/",NULL,MS_REMOUNT|MS_RDONLY,NULL);
        reboot(RB_POWER_OFF);
    }
    return 0;
}
