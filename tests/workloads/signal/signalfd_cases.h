#include <sys/signalfd.h>
#include <sys/epoll.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <fcntl.h>
static void signal_fd_cases(void)
{
    int signal=SIGRTMIN+4;sigset_t set,old;sigemptyset(&set);sigaddset(&set,signal);
    CHECK(!sigprocmask(SIG_BLOCK,&set,&old));
    CHECK(error(syscall(SYS_signalfd4,-1,&set,7,0),EINVAL));
    CHECK(error(syscall(SYS_signalfd4,-1,(void *)UINTPTR_MAX,8,~0U),EFAULT));
    CHECK(error(syscall(SYS_signalfd4,-1,&set,8,1),EINVAL));
    int fd=signalfd(-1,&set,SFD_NONBLOCK|SFD_CLOEXEC);CHECK(fd>=0);if(fd<0)return;
    struct signalfd_siginfo out[2];
    CHECK(error(read(fd,out,127),EINVAL));CHECK(error(read(fd,out,128),EAGAIN));
    CHECK(error(write(fd,out,128),EINVAL));CHECK(error(pread(fd,out,128,0),ESPIPE));CHECK(lseek(fd,123,SEEK_SET)==0);
    struct stat st;CHECK(!fstat(fd,&st) && (st.st_mode&0777)==0600);
    CHECK(!queued(signal,UINT64_C(0xabcdef1287654321)));CHECK(!queued(signal,2));
    struct iovec iov[]={{out,17},{(char *)out+17,239}};
    CHECK(readv(fd,iov,2)==256 && out[0].ssi_signo==(unsigned)signal && out[0].ssi_code==SI_QUEUE && out[0].ssi_pid==(unsigned)getpid() && out[0].ssi_ptr==UINT64_C(0xabcdef1287654321) && out[1].ssi_ptr==2);
    size_t page=(size_t)sysconf(_SC_PAGESIZE);char *map=mmap(0,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(map!=MAP_FAILED && !mprotect(map+page,page,PROT_NONE));
    CHECK(!queued(signal,3));CHECK(error(read(fd,map+page,128),EFAULT));CHECK(error(read(fd,out,128),EAGAIN));
    CHECK(!queued(signal,4) && !queued(signal,5));
    CHECK(read(fd,map+page-128,256)==128);CHECK(error(read(fd,out,128),EAGAIN));CHECK(!munmap(map,page*2));
    CHECK(signalfd(fd,&set,0)==fd && (fcntl(fd,F_GETFL)&O_NONBLOCK));
    CHECK(!queued(signal,6));int duplicate=dup(fd);CHECK(duplicate>=0 && read(duplicate,out,128)==128 && out[0].ssi_ptr==6);CHECK(!close(duplicate));
    int ep=epoll_create1(0);struct epoll_event event={.events=EPOLLIN|EPOLLET,.data.u64=7};CHECK(ep>=0 && !epoll_ctl(ep,EPOLL_CTL_ADD,fd,&event));
    CHECK(!queued(signal,7));CHECK(epoll_wait(ep,&event,1,0)==1 && event.data.u64==7);CHECK(read(fd,out,128)==128 && out[0].ssi_ptr==7);
    pid_t child=fork();CHECK(child>=0);if(!child) {
        /* The inherited OFD reads this child's pending set; the old epoll
         * subscription still belongs to the parent's sighand wait source. */
        if(queued(signal,8) || epoll_wait(ep,&event,1,0)!=0 || read(fd,out,128)!=128 || out[0].ssi_ptr!=8)_exit(1);
        if(epoll_ctl(ep,EPOLL_CTL_DEL,fd,0) || epoll_ctl(ep,EPOLL_CTL_ADD,fd,&event) || queued(signal,9) || epoll_wait(ep,&event,1,0)!=1 || read(fd,out,128)!=128)_exit(2);
        _exit(0);
    }
    int status;CHECK(waitpid(child,&status,0)==child && !status);
    CHECK(!close(ep) && !close(fd) && !sigprocmask(SIG_SETMASK,&old,0));
}
