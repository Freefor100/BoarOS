#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MFD_NOEXEC_SEAL
#define MFD_NOEXEC_SEAL 8
#define MFD_EXEC 16
#endif
#ifndef F_SEAL_EXEC
#define F_SEAL_EXEC 32
#endif
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"MEMFD:%d: %s (errno=%d)\n",__LINE__,#c,errno); exit(1); } } while (0)
#define FAIL(e,c) do { errno=0; CHECK((c)==-1 && errno==(e)); } while (0)
static int make(const char *name,unsigned flags)
{ return syscall(SYS_memfd_create,name,flags); }

static int pass_fd(int socket,int fd)
{
    char payload=1;
    struct iovec iov={.iov_base=&payload,.iov_len=1};
    union { struct cmsghdr align; char data[CMSG_SPACE(sizeof(int))]; } control={0};
    struct msghdr m={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control.data,.msg_controllen=sizeof(control.data)};
    if (fd>=0) {
        struct cmsghdr *c=CMSG_FIRSTHDR(&m);
        c->cmsg_len=CMSG_LEN(sizeof(int));c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;
        memcpy(CMSG_DATA(c),&fd,sizeof(fd));
        CHECK(sendmsg(socket,&m,0)==1);
        return fd;
    }
    CHECK(recvmsg(socket,&m,0)==1);
    struct cmsghdr *c=CMSG_FIRSTHDR(&m);
    CHECK(c && c->cmsg_len==CMSG_LEN(sizeof(int)) && c->cmsg_level==SOL_SOCKET && c->cmsg_type==SCM_RIGHTS);
    memcpy(&fd,CMSG_DATA(c),sizeof(fd));
    return fd;
}

int main(void)
{
    const long page=sysconf(_SC_PAGESIZE);
    CHECK(page>0);
    FAIL(EINVAL,make(NULL,0x80)); FAIL(EINVAL,make(NULL,MFD_EXEC|MFD_NOEXEC_SEAL)); FAIL(EFAULT,make(NULL,0));
    char long_name[251];memset(long_name,'n',250);long_name[250]=0;
    FAIL(EINVAL,make(long_name,0));
    long_name[249]=0;int default_fd=make(long_name,0);CHECK(default_fd>=0);
    CHECK(fcntl(default_fd,F_GET_SEALS)==F_SEAL_SEAL);
    FAIL(EPERM,fcntl(default_fd,F_ADD_SEALS,F_SEAL_GROW));
    CHECK(close(default_fd)==0);
    int fd=make("shared/test",MFD_CLOEXEC|MFD_ALLOW_SEALING);
    CHECK(fd>=0 && fcntl(fd,F_GETFD)==FD_CLOEXEC && fcntl(fd,F_GET_SEALS)==0);
    struct stat st;CHECK(fstat(fd,&st)==0 && S_ISREG(st.st_mode) && !st.st_nlink);
    char path[64],display[300];snprintf(path,sizeof(path),"/proc/self/fd/%d",fd);
    const char expected[]="/memfd:shared/test (deleted)";
    CHECK(readlink(path,display,sizeof(display))==(ssize_t)sizeof(expected)-1 && !memcmp(display,expected,sizeof(expected)-1));
    int reader=open(path,O_RDONLY);CHECK(reader>=0);
    FAIL(EPERM,fcntl(reader,F_ADD_SEALS,F_SEAL_GROW));
    CHECK(ftruncate(fd,page)==0);
    char *shared=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    CHECK(shared!=MAP_FAILED);
    memcpy(shared,"hello",6);
    FAIL(EBUSY,fcntl(fd,F_ADD_SEALS,F_SEAL_WRITE));
    CHECK(fcntl(fd,F_ADD_SEALS,F_SEAL_FUTURE_WRITE)==0);
    FAIL(EPERM,pwrite(fd,"x",1,0));
    FAIL(EPERM,fallocate(fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,0,page));
    errno=0;CHECK(mmap(NULL,page,PROT_WRITE,MAP_SHARED,fd,0)==MAP_FAILED && errno==EPERM);
    void *read_map=mmap(NULL,page,PROT_READ,MAP_SHARED,fd,0);
    CHECK(read_map!=MAP_FAILED);
    FAIL(EACCES,mprotect(read_map,page,PROT_READ|PROT_WRITE));

    int sv[2];CHECK(socketpair(AF_UNIX,SOCK_DGRAM,0,sv)==0);
    pid_t child=fork();CHECK(child>=0);
    if (!child) {
        CHECK(close(sv[0])==0 && close(fd)==0 && close(reader)==0);
        CHECK(munmap(read_map,page)==0);
        int recv=pass_fd(sv[1],-1);
        CHECK(fcntl(recv,F_GET_SEALS)==F_SEAL_FUTURE_WRITE);
        FAIL(EBUSY,fcntl(recv,F_ADD_SEALS,F_SEAL_WRITE));
        CHECK(munmap(shared,page)==0);
        char b[6];CHECK(pread(recv,b,sizeof(b),0)==sizeof(b) && !strcmp(b,"world"));
        char *private=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_PRIVATE,recv,0);
        CHECK(private!=MAP_FAILED && !strcmp(private,"world"));
        private[0]='W';
        CHECK(munmap(private,page)==0 && close(recv)==0 && close(sv[1])==0);
        _exit(0);
    }
    CHECK(close(sv[1])==0);
    memcpy(shared,"world",6);
    CHECK(munmap(shared,page)==0);
    pass_fd(sv[0],fd);
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    CHECK(fcntl(fd,F_ADD_SEALS,F_SEAL_WRITE|F_SEAL_GROW|F_SEAL_SHRINK)==0);
    FAIL(EPERM,ftruncate(fd,0));FAIL(EPERM,ftruncate(fd,2*page));
    CHECK(ftruncate(fd,page)==0 && fcntl(fd,F_ADD_SEALS,F_SEAL_SEAL)==0);
    FAIL(EPERM,fcntl(fd,F_ADD_SEALS,0));
    CHECK(close(sv[0])==0 && munmap(read_map,page)==0 && close(reader)==0 && close(fd)==0);

    fd=make("noexec",MFD_NOEXEC_SEAL);CHECK(fd>=0);
    CHECK(fcntl(fd,F_GET_SEALS)==F_SEAL_EXEC);
    CHECK(fstat(fd,&st)==0 && !(st.st_mode&0111));
    FAIL(EPERM,fchmod(fd,0700));CHECK(fchmod(fd,0600)==0);
    CHECK(write(fd,"x",1)==1 && close(fd)==0);
    fd=make("exec",MFD_EXEC|MFD_ALLOW_SEALING);CHECK(fd>=0);
    CHECK(fcntl(fd,F_ADD_SEALS,F_SEAL_EXEC)==0);
    CHECK(fcntl(fd,F_GET_SEALS)==(F_SEAL_EXEC|F_SEAL_SHRINK|F_SEAL_GROW|F_SEAL_WRITE|F_SEAL_FUTURE_WRITE));
    CHECK(close(fd)==0);
    puts("MEMFD PASS");
    return 0;
}
