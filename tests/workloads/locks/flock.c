#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>

static void check(int ok,const char *step)
{
    if (ok) return;
    fprintf(stderr,"FLOCK FAIL %s errno=%d\n",step,errno);exit(1);
}
static void busy(int fd,int operation,const char *step)
{ errno=0;check(flock(fd,operation|LOCK_NB)==-1 && errno==EWOULDBLOCK,step); }
static void closed(int fd)
{ check(close(fd)==0,"close"); }

static void regular(const char *path)
{
    int a=open(path,O_CREAT|O_RDWR|O_TRUNC,0600),b=open(path,O_RDWR);
    check(a>=0 && b>=0,"regular opens");
    check(!flock(a,LOCK_SH) && !flock(b,LOCK_SH),"shared coexist");
    busy(a,LOCK_EX,"failed upgrade");
    check(!flock(b,LOCK_EX|LOCK_NB),"failed upgrade released old reader");
    busy(a,LOCK_SH,"exclusive excludes reader");
    check(!flock(b,LOCK_SH) && !flock(a,LOCK_SH),"downgrade permits readers");
    check(!flock(a,LOCK_UN) && !flock(b,LOCK_UN),"unlocks");
    check(!flock(a,LOCK_EX),"exclusive");
    struct flock bytes={.l_type=F_WRLCK,.l_whence=SEEK_SET,.l_len=1};
    check(!fcntl(b,F_SETLK,&bytes),"POSIX byte lock independent");
    bytes.l_type=F_UNLCK;check(!fcntl(b,F_SETLK,&bytes),"POSIX byte unlock");
    bytes.l_type=F_WRLCK;check(!fcntl(b,F_OFD_SETLK,&bytes),"OFD byte lock independent");
    bytes.l_type=F_UNLCK;check(!fcntl(b,F_OFD_SETLK,&bytes),"OFD byte unlock");
    int alias=dup(a);check(alias>=0,"dup");closed(a);
    busy(b,LOCK_EX,"dup retains owner");closed(alias);
    check(!flock(b,LOCK_EX|LOCK_NB),"last OFD close releases");
    closed(b);

    char link_path[256];check(snprintf(link_path,sizeof(link_path),"%s-link",path)>0,"link path");
    unlink(link_path);check(!link(path,link_path),"hardlink");
    a=open(path,O_RDWR);b=open(link_path,O_RDWR);check(a>=0 && b>=0,"hardlink opens");
    check(!flock(a,LOCK_EX),"hardlink lock");busy(b,LOCK_EX,"same inode alias conflicts");
    check(!unlink(path),"unlink old inode");
    int replacement=open(path,O_CREAT|O_RDWR|O_EXCL,0600);check(replacement>=0,"replace inode");
    check(!flock(replacement,LOCK_EX|LOCK_NB),"new inode independent");
    closed(replacement);closed(a);closed(b);check(!unlink(link_path),"unlink alias");

    int directory=open("/tmp",O_RDONLY|O_DIRECTORY);check(directory>=0,"directory open");
    check(!flock(directory,LOCK_EX|LOCK_NB),"read-only directory exclusive");closed(directory);
    int pathfd=open(path,O_PATH);check(pathfd>=0,"O_PATH open");errno=0;
    check(flock(pathfd,LOCK_UN)==-1 && errno==EBADF,"O_PATH rejected");closed(pathfd);
    errno=0;check(flock(-1,3)==-1 && errno==EINVAL,"command before descriptor");
    errno=0;check(flock(-1,LOCK_EX)==-1 && errno==EBADF,"invalid descriptor");
    check(!flock(-1,32),"legacy mandatory no-op");
}

static void fork_owner(const char *path)
{
    int owner=open(path,O_RDWR),observer=open(path,O_RDWR),handshake[2];
    check(owner>=0 && observer>=0 && !pipe(handshake),"fork opens");
    check(!flock(owner,LOCK_EX),"fork initial owner");
    pid_t child=fork();check(child>=0,"fork");
    if (!child) {
        closed(handshake[0]);
        check(!flock(owner,LOCK_SH),"inherited description converts");
        closed(owner);check(write(handshake[1],"s",1)==1,"fork notify");_exit(0);
    }
    closed(handshake[1]);char byte;check(read(handshake[0],&byte,1)==1,"fork conversion handshake");
    check(!flock(observer,LOCK_SH|LOCK_NB),"fork changed same OFD");
    busy(observer,LOCK_EX,"parent reference retains reader");
    closed(owner);check(!flock(observer,LOCK_EX|LOCK_NB),"parent last close releases");
    int status;check(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status),"fork status");
    closed(observer);closed(handshake[0]);
}

static void rights_owner(const char *path)
{
    int sockets[2];check(!socketpair(AF_UNIX,SOCK_DGRAM,0,sockets),"rights sockets");
    check(!flock(sockets[0],LOCK_EX|LOCK_NB) && !flock(sockets[1],LOCK_EX|LOCK_NB),"socket inodes independent");
    int a=open(path,O_RDWR),b=open(path,O_RDWR);check(a>=0 && b>=0,"rights opens");
    check(!flock(a,LOCK_EX),"rights initial lock");
    char byte='x',control[CMSG_SPACE(sizeof(int))];memset(control,0,sizeof(control));
    struct iovec iov={&byte,1};
    struct msghdr message={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof(control)};
    struct cmsghdr *header=CMSG_FIRSTHDR(&message);header->cmsg_level=SOL_SOCKET;header->cmsg_type=SCM_RIGHTS;header->cmsg_len=CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(header),&a,sizeof(a));check(sendmsg(sockets[0],&message,0)==1,"send rights");closed(a);
    busy(b,LOCK_EX,"queued rights retain OFD lock");
    memset(control,0,sizeof(control));check(recvmsg(sockets[1],&message,0)==1,"receive rights");
    header=CMSG_FIRSTHDR(&message);check(header && header->cmsg_type==SCM_RIGHTS,"received rights header");
    int received;memcpy(&received,CMSG_DATA(header),sizeof(received));
    check(!flock(received,LOCK_SH),"received same OFD converts");
    check(!flock(b,LOCK_SH|LOCK_NB),"received conversion visible");
    busy(b,LOCK_EX,"received OFD remains held");closed(received);
    check(!flock(b,LOCK_EX|LOCK_NB),"rights last close releases");
    closed(b);closed(sockets[0]);closed(sockets[1]);
}

static void anonymous_inodes(void)
{
    int p[2],q[2];check(!pipe(p) && !pipe(q),"pipe pairs");
    check(!flock(p[0],LOCK_EX|LOCK_NB),"pipe read endpoint lock");
    busy(p[1],LOCK_EX,"pipe endpoints share inode");
    check(!flock(q[0],LOCK_EX|LOCK_NB),"different pipe independent");
    closed(p[0]);check(!flock(p[1],LOCK_EX|LOCK_NB),"pipe last OFD release");
    closed(p[1]);closed(q[0]);closed(q[1]);

    int event=eventfd(0,0),other=eventfd(0,0),epoll=epoll_create1(0),timer=timerfd_create(CLOCK_MONOTONIC,0);
    check(event>=0 && other>=0 && epoll>=0 && timer>=0,"anon descriptors");
    check(!flock(event,LOCK_EX|LOCK_NB),"eventfd lock");
    busy(other,LOCK_EX,"eventfds share singleton inode");
    busy(epoll,LOCK_SH,"epoll shares singleton inode");
    busy(timer,LOCK_SH,"timerfd shares singleton inode");
    closed(event);check(!flock(epoll,LOCK_SH|LOCK_NB) && !flock(timer,LOCK_SH|LOCK_NB),"anon reader coexist");
    closed(other);closed(epoll);closed(timer);

    int first=(int)syscall(SYS_pidfd_open,getpid(),0),second=(int)syscall(SYS_pidfd_open,getpid(),0);
    check(first>=0 && second>=0,"pidfd opens");
    check(!flock(first,LOCK_EX|LOCK_NB),"pidfd lock");busy(second,LOCK_EX,"same target pidfs inode");
    closed(first);check(!flock(second,LOCK_EX|LOCK_NB),"pidfd release");closed(second);

    struct mq_attr attributes={.mq_maxmsg=2,.mq_msgsize=16};
    mq_unlink("/flock-api8");mqd_t mq=mq_open("/flock-api8",O_CREAT|O_RDWR|O_EXCL,0600,&attributes);
    mqd_t mq2=mq_open("/flock-api8",O_RDWR);check(mq!=(mqd_t)-1 && mq2!=(mqd_t)-1,"mqueue opens");
    check(!flock((int)mq,LOCK_EX|LOCK_NB),"mqueue lock");busy((int)mq2,LOCK_EX,"queue inode shared");
    check(!mq_close(mq),"mqueue owner close");check(!flock((int)mq2,LOCK_EX|LOCK_NB),"mqueue release");
    check(!mq_close(mq2) && !mq_unlink("/flock-api8"),"mqueue cleanup");
}

static void killed_waiter(const char *path)
{
    int holder=open(path,O_RDWR),handshake[2];
    check(holder>=0 && !pipe(handshake) && !flock(holder,LOCK_EX),"killed waiter holder");
    pid_t child=fork();check(child>=0,"killed waiter fork");
    if (!child) {
        closed(holder);closed(handshake[0]);int fd=open(path,O_RDWR);
        check(fd>=0 && write(handshake[1],"b",1)==1,"killed waiter armed");
        if (!flock(fd,LOCK_EX)) _exit(40);
        _exit(41);
    }
    closed(handshake[1]);char byte;check(read(handshake[0],&byte,1)==1,"killed waiter handshake");
    char proc[96],status[1024];snprintf(proc,sizeof(proc),"/proc/%ld/status",(long)child);
    /* After this handshake the child has no other blocking operation. State S
     * observes the actual flock park before SIGKILL, without timed sleeps. */
    for (;;) {
        int fd=open(proc,O_RDONLY);check(fd>=0,"killed waiter proc open");
        ssize_t count=read(fd,status,sizeof(status)-1);check(count>0,"killed waiter proc read");closed(fd);status[count]=0;
        char *state=strstr(status,"State:");check(state!=0,"killed waiter proc state");
        state+=6;while (*state==' ' || *state=='\t') state++;
        if (*state=='S' || *state=='D') break;
        check(*state!='Z',"killed waiter still live");sched_yield();
    }
    check(!kill(child,SIGKILL),"kill blocked flock");
    int result;check(waitpid(child,&result,0)==child && WIFSIGNALED(result) && WTERMSIG(result)==SIGKILL,"killed waiter status");
    closed(handshake[0]);closed(holder);
    int observer=open(path,O_RDWR);check(observer>=0 && !flock(observer,LOCK_EX|LOCK_NB),"killed waiter returned pin and queue");closed(observer);
}

int main(int argc,char **argv)
{
    const char *path=argc>1?argv[1]:"/tmp/flock-api8";
    regular(path);fork_owner(path);rights_owner(path);anonymous_inodes();killed_waiter(path);
    check(!unlink(path),"final unlink");puts("FLOCK-PASS");return 0;
}
