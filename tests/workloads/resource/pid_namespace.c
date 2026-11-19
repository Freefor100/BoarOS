#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if(!(x)) { fprintf(stderr,"PIDNS FD FAIL %d %s errno=%d\n",__LINE__,#x,errno); exit(1); } } while(0)
static void same_inode(int a,int b)
{
    struct stat x,y;
    CHECK(!fstat(a,&x) && !fstat(b,&y));
    CHECK(x.st_ino==y.st_ino && x.st_dev==y.st_dev);
}
static void setns_result(const char *kind,int result,int allow_eperm)
{
    int denied=result==-1 && errno==EPERM;
    CHECK(result==0 || (allow_eperm && denied));
    printf("PIDNS SETNS %s %s\n",kind,denied?"permission-denied":"success");
}
static long status_number(const char *text,const char *field)
{
    const char *line=strstr(text,field);CHECK(line);
    char *end;long number=strtol(line+strlen(field),&end,10);
    CHECK(end>line+strlen(field));return number;
}
static void proc_status(int fd,pid_t pid,pid_t ppid)
{
    char text[4096];ssize_t n=read(fd,text,sizeof(text)-1);
    CHECK(n>0);text[n]=0;
    CHECK(status_number(text,"\nPid:\t")==pid);
    CHECK(status_number(text,"\nTgid:\t")==pid);
    CHECK(status_number(text,"\nPPid:\t")==ppid);
    CHECK(status_number(text,"\nNSpid:\t")==pid);
    CHECK(status_number(text,"\nNStgid:\t")==pid);
    CHECK(status_number(text,"\nNSpgid:\t")==getpgrp());
    CHECK(status_number(text,"\nNSsid:\t")==getsid(0));
}
static void fdinfo_pid(int target,pid_t expected)
{
    char path[80],text[1024];snprintf(path,sizeof(path),"/proc/self/fdinfo/%d",target);
    int fd=open(path,O_RDONLY);CHECK(fd>=0);
    ssize_t n=read(fd,text,sizeof(text)-1);CHECK(n>0);text[n]=0;CHECK(!close(fd));
    CHECK(status_number(text,"\nPid:\t")==expected);
    CHECK(status_number(text,"\nNSpid:\t")==expected);
}
static void proc_fsopen(int allow_eperm)
{
    int ctx=syscall(SYS_fsopen,"proc",1);
    if(ctx<0 && allow_eperm && errno==EPERM) {
        puts("PIDNS PROC fsopen permission-denied");return;
    }
    CHECK(ctx>=0);
    CHECK(!syscall(SYS_fsconfig,ctx,6,NULL,NULL,0));
    int tree=syscall(SYS_fsmount,ctx,1,0);CHECK(tree>=0 && !close(ctx));
    int fd=openat(tree,"self/status",O_RDONLY);CHECK(fd>=0 && !close(tree));
    proc_status(fd,getpid(),getppid());CHECK(!close(fd));
    puts("PIDNS PROC fsopen detached-view owner PASS");
}
static void pidfd_transport(void)
{
    int sockets[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
    int procdir=open("/proc/self",O_RDONLY|O_DIRECTORY);CHECK(procdir>=0);
    CHECK(!syscall(SYS_pidfd_send_signal,procdir,0,NULL,0) && !close(procdir));
    pid_t parent=getpid();int fd=syscall(SYS_pidfd_open,parent,0);CHECK(fd>=0);
    pid_t child=fork();CHECK(child>=0);
    if(!child) {
        CHECK(!close(sockets[0]));
        CHECK(!syscall(SYS_pidfd_send_signal,fd,0,NULL,0)); /* inherited OFD */
        CHECK(!close(fd));
        char byte;struct iovec iov={&byte,1};
        union {struct cmsghdr align;char bytes[CMSG_SPACE(sizeof(int))];} control;
        memset(&control,0,sizeof(control));
        struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=&control,.msg_controllen=sizeof(control)};
        CHECK(recvmsg(sockets[1],&msg,MSG_CMSG_CLOEXEC)==1 && byte=='p');
        struct cmsghdr *cmsg=CMSG_FIRSTHDR(&msg);
        CHECK(cmsg && cmsg->cmsg_level==SOL_SOCKET && cmsg->cmsg_type==SCM_RIGHTS && cmsg->cmsg_len==CMSG_LEN(sizeof(int)));
        int received;memcpy(&received,CMSG_DATA(cmsg),sizeof(received));
        fdinfo_pid(received,parent);
        CHECK(!syscall(SYS_pidfd_send_signal,received,0,NULL,0));
        CHECK(!close(received) && !close(sockets[1]));_exit(0);
    }
    CHECK(!close(sockets[1]));
    char byte='p';struct iovec iov={&byte,1};
    union {struct cmsghdr align;char bytes[CMSG_SPACE(sizeof(int))];} control;
    memset(&control,0,sizeof(control));
    struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=&control,.msg_controllen=sizeof(control)};
    struct cmsghdr *cmsg=CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level=SOL_SOCKET;cmsg->cmsg_type=SCM_RIGHTS;cmsg->cmsg_len=CMSG_LEN(sizeof(fd));
    memcpy(CMSG_DATA(cmsg),&fd,sizeof(fd));
    CHECK(sendmsg(sockets[0],&msg,0)==1 && !close(fd) && !close(sockets[0]));
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    puts("PIDNS PIDFD PASS fork-inherited SCM_RIGHTS signal-zero owner");
}
static void resource_identity(void)
{
    struct {uint32_t version;int32_t pid;} header={0x20080522,0};
    struct {uint32_t effective,permitted,inheritable;} self[2],named[2];
    CHECK(!syscall(SYS_capget,&header,self));
    header.pid=(int32_t)syscall(SYS_gettid);
    CHECK(!syscall(SYS_capget,&header,named) && !memcmp(self,named,sizeof(self)));
    CHECK(!syscall(SYS_capset,&header,named));
    header.pid=INT32_MAX;
    CHECK(syscall(SYS_capget,&header,named)==-1 && errno==ESRCH);
    CHECK(syscall(SYS_capset,&header,(void *)-1)==-1 && errno==EPERM);
    struct rlimit local,named_limit;
    CHECK(!getrlimit(RLIMIT_NOFILE,&local));
    CHECK(!syscall(SYS_prlimit64,getpid(),RLIMIT_NOFILE,NULL,&named_limit));
    CHECK(local.rlim_cur==named_limit.rlim_cur && local.rlim_max==named_limit.rlim_max);
    CHECK(syscall(SYS_prlimit64,INT32_MAX,RLIMIT_NOFILE,NULL,&named_limit)==-1 && errno==ESRCH);
    CHECK(syscall(SYS_prlimit64,INT32_MAX,RLIMIT_NOFILE,(void *)-1,NULL)==-1 && errno==EFAULT);
    puts("PIDNS RESOURCE PASS capget capset prlimit named-self and error-order");
}

int main(int argc,char **argv)
{
    int allow_eperm=argc==2 && !strcmp(argv[1],"--allow-eperm");
    CHECK(argc==1 || allow_eperm);
    proc_fsopen(allow_eperm);
    pidfd_transport();
    resource_identity();
    int status_fd=open("/proc/thread-self/status",O_RDONLY|O_CLOEXEC);
    CHECK(status_fd>=0);proc_status(status_fd,getpid(),getppid());CHECK(!close(status_fd));
    status_fd=open("/proc/self/status",O_RDONLY|O_CLOEXEC);CHECK(status_fd>=0);
    pid_t parent_ppid=getppid();
    int active=open("/proc/self/ns/pid",O_RDONLY|O_CLOEXEC);
    int children=open("/proc/thread-self/ns/pid_for_children",O_RDONLY|O_CLOEXEC);
    CHECK(active>=0 && children>=0);
    same_inode(active,children);
    CHECK(ioctl(active,0xb703U)==CLONE_NEWPID);
    char a[80],b[80];
    ssize_t n=readlink("/proc/self/ns/pid",a,sizeof(a));
    ssize_t m=readlink("/proc/thread-self/ns/pid_for_children",b,sizeof(b));
    CHECK(n>5 && n==m && !memcmp(a,b,n) && !memcmp(a,"pid:[",5));
    errno=0;CHECK(setns(active,CLONE_NEWUTS)==-1 && errno==EINVAL);
    pid_t before=getpid();
    int result=setns(active,CLONE_NEWPID);
    setns_result("direct",result,allow_eperm);
    CHECK(getpid()==before);
    int pidfd=syscall(SYS_pidfd_open,getpid(),0);
    CHECK(pidfd>=0);fdinfo_pid(pidfd,getpid());
    result=setns(pidfd,CLONE_NEWPID|CLONE_NEWUTS);
    setns_result("pidfd-combined",result,allow_eperm);
    CHECK(getpid()==before && !close(pidfd));
    int channel[2];CHECK(!pipe(channel));
    pid_t child=fork();CHECK(child>=0);
    if(!child) {
        close(channel[0]);
        proc_status(status_fd,before,parent_ppid);CHECK(!close(status_fd));
        int own=open("/proc/self/ns/pid",O_RDONLY);
        CHECK(own>=0);same_inode(own,active);CHECK(!close(own));
        CHECK(write(channel[1],"x",1)==1);
        _exit(0);
    }
    int child_pidfd=syscall(SYS_pidfd_open,child,0);CHECK(child_pidfd>=0);
    fdinfo_pid(child_pidfd,child);
    CHECK(!close(status_fd));
    close(channel[1]);CHECK(read(channel[0],a,1)==1);close(channel[0]);
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    fdinfo_pid(child_pidfd,-1);CHECK(!close(child_pidfd));
    same_inode(active,children);
    int copy=dup(active);CHECK(copy>=0 && !close(active));
    same_inode(copy,children);CHECK(!close(copy) && !close(children));
    puts("PIDNS PROC PASS status namespace-columns inherited-fd");
    puts("PIDNS FD PASS initial-view stable-inode setns inherited-owner");
    return 0;
}
