#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){dprintf(2,"PIDNS SOCKET FAIL %d %s errno=%d\n",__LINE__,#x,errno);_exit(1);}}while(0)
static int wire[2],foreign_fd=-1,nested;
static pid_t creator,foreign_pid;
static struct ucred peer(int fd)
{struct ucred cred;socklen_t size=sizeof(cred);CHECK(!getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&cred,&size)&&size==sizeof(cred));return cred;}
static void send_owner(int fd)
{
    char data='x',control[CMSG_SPACE(sizeof(struct ucred))+CMSG_SPACE(sizeof(int))]={0};
    struct iovec io={&data,1};struct msghdr message={.msg_iov=&io,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof(control)};
    struct cmsghdr *h=CMSG_FIRSTHDR(&message);h->cmsg_level=SOL_SOCKET;h->cmsg_type=SCM_CREDENTIALS;h->cmsg_len=CMSG_LEN(sizeof(struct ucred));
    struct ucred cred={getpid(),getuid(),getgid()};memcpy(CMSG_DATA(h),&cred,sizeof(cred));
    h=CMSG_NXTHDR(&message,h);CHECK(h);h->cmsg_level=SOL_SOCKET;h->cmsg_type=SCM_RIGHTS;h->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(h),&fd,sizeof(fd));
    CHECK(sendmsg(wire[1],&message,0)==1);
    CHECK(send(wire[1],"a",1,0)==1);
}
static void rejected_credentials(void)
{
    struct { uint32_t version; int32_t pid; } header={0x20080522,0};
    struct { uint32_t effective,permitted,inheritable; } caps[2]={{0,0,0},{0,0,0}};
    CHECK(!syscall(SYS_capset,&header,caps));
    char data='x',control[CMSG_SPACE(sizeof(struct ucred))]={0};
    struct iovec io={&data,1};struct msghdr message={.msg_iov=&io,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof(control)};
    struct cmsghdr *h=CMSG_FIRSTHDR(&message);h->cmsg_level=SOL_SOCKET;h->cmsg_type=SCM_CREDENTIALS;h->cmsg_len=CMSG_LEN(sizeof(struct ucred));
    struct ucred cred={0,getuid(),getgid()};memcpy(CMSG_DATA(h),&cred,sizeof(cred));
    CHECK(sendmsg(wire[1],&message,0)==-1&&errno==EPERM);
    cred.pid=getpid()+100000;memcpy(CMSG_DATA(h),&cred,sizeof(cred));
    CHECK(sendmsg(wire[1],&message,0)==-1&&errno==EPERM);
    cred.uid=(uid_t)-1;memcpy(CMSG_DATA(h),&cred,sizeof(cred));
    CHECK(sendmsg(wire[1],&message,0)==-1&&errno==EINVAL);
}
static int receive_owner_at(int socket,pid_t expected,int rights);
static int child(void *unused)
{
    (void)unused;
    if(nested)CHECK(getpid()==1);
    CHECK(peer(wire[1]).pid==(nested?0:creator));
    CHECK(receive_owner_at(wire[1],nested?0:creator,0)==-1);
    if(foreign_fd>=0){CHECK(peer(foreign_fd).pid==(nested?0:foreign_pid));CHECK(!close(foreign_fd));}
    int pair[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
    CHECK(peer(pair[0]).pid==getpid());rejected_credentials();send_owner(pair[0]);
    CHECK(!close(pair[0])&&!close(pair[1]));return 0;
}
static int receive_owner_at(int socket,pid_t expected,int rights)
{
    char data=0,control[256]={0};struct iovec io={&data,1};
    struct msghdr message={.msg_iov=&io,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof(control)};
    CHECK(recvmsg(socket,&message,MSG_CMSG_CLOEXEC)==1);
    int fd=-1,credentials=0;
    for(struct cmsghdr *h=CMSG_FIRSTHDR(&message);h;h=CMSG_NXTHDR(&message,h)){
        CHECK(h->cmsg_level==SOL_SOCKET);
        if(h->cmsg_type==SCM_CREDENTIALS){struct ucred cred;memcpy(&cred,CMSG_DATA(h),sizeof(cred));CHECK(cred.pid==expected&&cred.uid==getuid()&&cred.gid==getgid());credentials++;}
        else if(h->cmsg_type==SCM_RIGHTS){CHECK(rights&&fd<0);memcpy(&fd,CMSG_DATA(h),sizeof(fd));}
        else CHECK(0);
    }
    CHECK(credentials==1&&!(message.msg_flags&MSG_CTRUNC));
    CHECK(rights?(data=='x'&&fd>=0):(data=='a'&&fd==-1));return fd;
}
int main(int argc,char **argv)
{
    nested=argc==2&&!strcmp(argv[1],"--nested");creator=getpid();
    CHECK(!socketpair(AF_UNIX,SOCK_DGRAM,0,wire));int yes=1;
    CHECK(!setsockopt(wire[0],SOL_SOCKET,SO_PASSCRED,&yes,sizeof(yes)));
    CHECK(!setsockopt(wire[1],SOL_SOCKET,SO_PASSCRED,&yes,sizeof(yes)));
    pid_t old=0;
    for(unsigned i=0;i<2;i++){
        void *stack=NULL;pid_t pid;
        CHECK(send(wire[0],"a",1,0)==1);
        if(nested){stack=malloc(1U<<20);CHECK(stack);pid=clone(child,(char *)stack+(1U<<20),CLONE_NEWPID|SIGCHLD,NULL);}
        else {pid=fork();if(!pid)return child(NULL);}
        CHECK(pid>0);int status;CHECK(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&!WEXITSTATUS(status));free(stack);
        int fd=receive_owner_at(wire[0],pid,1);CHECK(receive_owner_at(wire[0],pid,0)==-1);
        struct ucred cred=peer(fd);CHECK(cred.pid==pid&&cred.uid==geteuid()&&cred.gid==getegid());
        if(foreign_fd>=0){CHECK(peer(foreign_fd).pid==old);CHECK(!close(foreign_fd));}
        foreign_fd=fd;old=pid;foreign_pid=pid;
    }
    CHECK(!close(foreign_fd)&&!close(wire[0])&&!close(wire[1]));
    puts(nested?"PIDNS SOCKET PASS nested sibling peercred SCM_CREDENTIALS SCM_RIGHTS exited-owner":"PIDNS SOCKET PASS peercred SCM_CREDENTIALS SCM_RIGHTS exited-owner");return 0;
}
