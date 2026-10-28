#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <mqueue.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char queue_name[64];
static char extra_name[80],root_restore_path[160];
static struct stat root_original;
static int root_changed;
static unsigned cases;
static void cleanup(void)
{
    if(root_changed&&geteuid()==0){chown(root_restore_path,root_original.st_uid,root_original.st_gid);chmod(root_restore_path,root_original.st_mode&07777);root_changed=0;}
    if(queue_name[0])mq_unlink(queue_name);
    if(extra_name[0])mq_unlink(extra_name);
}
static void failed(const char *expr,int line)
{int e=errno;cleanup();fprintf(stderr,"POSIX-MQ FAIL line=%d errno=%d %s\n",line,e,expr);exit(1);}
#define CHECK(x) do{if(!(x))failed(#x,__LINE__);}while(0)
#define ERROR(x,e) do{errno=0;CHECK((x)==-1&&errno==(e));}while(0)
static void pass(const char *name){cases++;printf("POSIX-MQ CASE %s PASS\n",name);}
static void child_done(pid_t child)
{int status;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));}
static mqd_t make(unsigned count,unsigned size,int flags)
{
    struct mq_attr a={.mq_maxmsg=count,.mq_msgsize=size};
    mqd_t q=mq_open(queue_name,O_CREAT|O_EXCL|O_RDWR|flags,0600,&a);CHECK(q!=(mqd_t)-1);return q;
}
static void removeq(mqd_t q){CHECK(!mq_unlink(queue_name)&&!mq_close(q));}
static void abi_priority_flags(void)
{
    CHECK(sizeof(struct mq_attr)==64&&sizeof(struct timespec)==16);
#if defined(__riscv)||defined(__loongarch__)
    CHECK(SYS_mq_open==180&&SYS_mq_unlink==181&&SYS_mq_timedsend==182&&SYS_mq_timedreceive==183&&SYS_mq_notify==184&&SYS_mq_getsetattr==185);
#endif
    mqd_t q=make(4,8192,O_NONBLOCK);CHECK(fcntl(q,F_GETFD)&FD_CLOEXEC);
    ERROR(mq_open(queue_name,3),EINVAL);
    char rawname[80];snprintf(rawname,sizeof(rawname),"%s-mode3",queue_name);
    struct mq_attr rawattr={.mq_maxmsg=1,.mq_msgsize=32};
    int mode3=(int)syscall(SYS_mq_open,rawname+1,O_CREAT|O_EXCL|3,0600,&rawattr);CHECK(mode3>=0);
    ERROR(mq_send(mode3,"x",1,0),EBADF);char tiny[32];ERROR(mq_receive(mode3,tiny,sizeof(tiny),0),EBADF);
    CHECK(!close(mode3)&&!syscall(SYS_mq_unlink,rawname+1));
    ERROR(mq_open(queue_name,O_CREAT|O_EXCL|O_RDWR,0600,0),EEXIST);
    int alias=dup(q);CHECK(alias>=0);struct mq_attr new={.mq_flags=0},old;
    CHECK(!mq_setattr(alias,&new,&old)&&(old.mq_flags&O_NONBLOCK));CHECK(!(fcntl(q,F_GETFL)&O_NONBLOCK));
    mqd_t independent=mq_open(queue_name,O_RDWR|O_NONBLOCK);CHECK(independent!=(mqd_t)-1);
    CHECK(!mq_getattr(independent,&old)&&(old.mq_flags&O_NONBLOCK));
    CHECK(!mq_send(q,"low",4,0)&&!mq_send(q,"first",6,20)&&!mq_send(q,"second",7,20));
    char input[8192],out[8192];memset(input,0x5a,sizeof(input));CHECK(!mq_send(q,input,sizeof(input),4));
    ERROR(mq_send(independent,"full",5,0),EAGAIN);unsigned priority;
    CHECK(mq_receive(q,out,sizeof(out),&priority)==6&&priority==20&&!strcmp(out,"first"));
    CHECK(mq_receive(q,out,sizeof(out),&priority)==7&&priority==20&&!strcmp(out,"second"));
    CHECK(mq_receive(q,out,sizeof(out),&priority)==8192&&priority==4&&!memcmp(input,out,sizeof(input)));
    CHECK(mq_receive(q,out,sizeof(out),&priority)==4&&priority==0&&!strcmp(out,"low"));
    ERROR(mq_receive(independent,out,sizeof(out),0),EAGAIN);
    ERROR(mq_receive(independent,out,8191,0),EMSGSIZE);ERROR(mq_send(q,"x",1,32768),EINVAL);
    struct timespec expired={0,0};ERROR(mq_timedreceive(q,out,sizeof(out),0,&expired),ETIMEDOUT);
    CHECK(!mq_timedsend(q,"ready",6,1,&expired));CHECK(mq_timedreceive(q,out,sizeof(out),0,&expired)==6);
    CHECK(!close(alias)&&!mq_close(independent));removeq(q);pass("native64-libc-priority-fifo-segments-ofd-flags-timeout");
}
static void poll_and_fault(void)
{
    mqd_t q=make(1,32,O_NONBLOCK);struct pollfd p={q,POLLIN|POLLOUT,0};
    CHECK(poll(&p,1,0)==1&&(p.revents&POLLOUT)&&!(p.revents&POLLIN));
    int ep=epoll_create1(EPOLL_CLOEXEC);CHECK(ep>=0);struct epoll_event event={.events=EPOLLIN,.data.u64=77};
    CHECK(!epoll_ctl(ep,EPOLL_CTL_ADD,q,&event));CHECK(!mq_send(q,"event",6,1));
    CHECK(poll(&p,1,0)==1&&(p.revents&POLLIN)&&!(p.revents&POLLOUT));
    CHECK(epoll_wait(ep,&event,1,0)==1&&event.data.u64==77&&(event.events&EPOLLIN));
    ERROR(syscall(SYS_mq_timedreceive,q,(void *)1,32,0,0),EFAULT);
    struct mq_attr attr;CHECK(!mq_getattr(q,&attr)&&!attr.mq_curmsgs);
    ERROR(syscall(SYS_mq_timedsend,q,(void *)1,1,0,0),EFAULT);CHECK(!mq_getattr(q,&attr)&&!attr.mq_curmsgs);
    ERROR(syscall(SYS_mq_timedsend,-1,(void *)1,9000,32768,(void *)1),EFAULT);
    ERROR(syscall(SYS_mq_open,"bad/name",O_CREAT|O_RDWR,0600,(void *)1),EFAULT);
    struct stat st;CHECK(!fstat(q,&st)&&S_ISREG(st.st_mode)&&st.st_size==80&&st.st_blksize==sysconf(_SC_PAGESIZE));
    struct statfs fs;CHECK(!fstatfs(q,&fs)&&fs.f_type==0x19800202&&(uint64_t)fs.f_bsize==(uint64_t)sysconf(_SC_PAGESIZE));
    ERROR(write(q,"x",1),EINVAL);ERROR(pwrite(q,"x",1,0),EINVAL);
    errno=0;CHECK(mmap(0,4096,PROT_READ,MAP_SHARED,q,0)==MAP_FAILED&&errno==ENODEV);
    CHECK(!close(ep));removeq(q);pass("poll-epoll-stat-write-mmap-usercopy");
}
static sigset_t notify_mask;
static siginfo_t take_signal(void)
{siginfo_t info;struct timespec timeout={5,0};CHECK(sigtimedwait(&notify_mask,&info,&timeout)==SIGUSR1);return info;}
static void notification_signal(void)
{
    mqd_t q=make(2,32,O_NONBLOCK);int alias=dup(q);CHECK(alias>=0);
    struct sigevent event={.sigev_notify=SIGEV_SIGNAL,.sigev_signo=SIGUSR1};
    event.sigev_value.sival_ptr=(void *)(uintptr_t)UINT64_C(0xabcdef0123456789);
    CHECK(!mq_notify(q,&event));ERROR(mq_notify(q,&event),EBUSY);
    pid_t child=fork();CHECK(child>=0);if(!child){CHECK(!close(alias));_exit(0);}child_done(child);
    CHECK(!mq_send(q,"signal",7,2));siginfo_t info=take_signal();
    CHECK(info.si_code==SI_MESGQ&&info.si_pid==getpid()&&info.si_uid==getuid()&&
        (uintptr_t)info.si_value.sival_ptr==UINT64_C(0xabcdef0123456789));
    char out[32];CHECK(mq_receive(q,out,sizeof(out),0)==7);
    event.sigev_notify=SIGEV_NONE;CHECK(!mq_notify(q,&event));ERROR(mq_notify(q,&event),EBUSY);
    CHECK(!close(alias));CHECK(!mq_notify(q,&event));CHECK(!mq_send(q,"none",5,0));
    CHECK(!mq_notify(q,&event));CHECK(!mq_notify(q,0));CHECK(!mq_notify(q,&event));
    removeq(q);pass("signal-real-sender-value64-childclose-owner-anyfd-none-single-registration");
}
static pthread_mutex_t callback_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t callback_cond=PTHREAD_COND_INITIALIZER;
static unsigned callback_count;
static void callback(union sigval value)
{
    CHECK(value.sival_int==73);CHECK(!pthread_mutex_lock(&callback_mutex));callback_count++;
    CHECK(!pthread_cond_signal(&callback_cond)&&!pthread_mutex_unlock(&callback_mutex));
}
static void notification_thread(void)
{
    mqd_t q=make(1,32,O_NONBLOCK);struct sigevent event={.sigev_notify=SIGEV_THREAD,.sigev_notify_function=callback};
    event.sigev_value.sival_int=73;CHECK(!mq_notify(q,&event));CHECK(!mq_send(q,"thread",7,0));
    struct timespec until;CHECK(!clock_gettime(CLOCK_REALTIME,&until));until.tv_sec+=5;
    CHECK(!pthread_mutex_lock(&callback_mutex));
    while(!callback_count)CHECK(!pthread_cond_timedwait(&callback_cond,&callback_mutex,&until));
    CHECK(callback_count==1&&!pthread_mutex_unlock(&callback_mutex));
    CHECK(!mq_notify(q,&event));int alias=dup(q);CHECK(alias>=0&&!close(alias));
    CHECK(!mq_notify(q,&event));CHECK(!mq_notify(q,0));removeq(q);pass("libc-sigev-thread-callback-and-removed-cookie");
}
static void creation_quota(void)
{
    struct rlimit original;CHECK(!getrlimit(RLIMIT_MSGQUEUE,&original));mqd_t q=make(1,32,0);
    struct rlimit low=original;low.rlim_cur=1;CHECK(!setrlimit(RLIMIT_MSGQUEUE,&low));
    mqd_t again=mq_open(queue_name,O_RDWR);CHECK(again!=(mqd_t)-1&&!mq_close(again));
    char name[80];snprintf(name,sizeof(name),"%s-quota",queue_name);struct mq_attr a={.mq_maxmsg=1,.mq_msgsize=1};
    ERROR(mq_open(name,O_CREAT|O_EXCL|O_RDWR,0600,&a),EMFILE);
    CHECK(!setrlimit(RLIMIT_MSGQUEUE,&original));CHECK(!mq_unlink(queue_name));
    CHECK(!mq_send(q,"orphan",7,0));char out[32];CHECK(mq_receive(q,out,sizeof(out),0)==7&&!strcmp(out,"orphan"));CHECK(!mq_close(q));
    q=make(1,32,0);removeq(q);pass("creation-realuid-quota-existing-open-unlinked-owner-lifetime");
}
static void numeric_user(void)
{CHECK(!setgroups(0,0)&&!setresgid(23457,23457,23457)&&!setresuid(23456,23456,23456));}
static int root_and_sticky(const char *first,const char *second,const char *path,const char *other,mqd_t q)
{
    if(geteuid()!=0)return 0;
    CHECK(!stat(first,&root_original));snprintf(root_restore_path,sizeof(root_restore_path),"%s",first);root_changed=1;
    CHECK(!fchmod(q,0666)&&!chmod(first,0700));struct stat st;CHECK(!stat(second,&st)&&(st.st_mode&07777)==0700);
    snprintf(extra_name,sizeof(extra_name),"%s-owner",queue_name);
    pid_t child=fork();CHECK(child>=0);
    if(!child){
        numeric_user();mqd_t existing=mq_open(queue_name,O_RDWR);CHECK(existing!=(mqd_t)-1&&!mq_close(existing));
        struct mq_attr attr={.mq_maxmsg=1,.mq_msgsize=32};
        ERROR(mq_open(extra_name,O_CREAT|O_EXCL|O_RDWR,0600,&attr),EACCES);
        ERROR(mq_unlink(queue_name),EACCES);ERROR(open(other,O_RDONLY),EACCES);_exit(0);
    }
    child_done(child);CHECK(!chown(first,23456,77)&&!chmod(first,03777));
    CHECK(!stat(second,&st)&&st.st_uid==23456&&st.st_gid==77&&(st.st_mode&07777)==03777);
    child=fork();CHECK(child>=0);
    if(!child){numeric_user();struct mq_attr attr={.mq_maxmsg=1,.mq_msgsize=32};
        mqd_t created=mq_open(extra_name,O_CREAT|O_EXCL|O_RDWR,0600,&attr);CHECK(created!=(mqd_t)-1&&!mq_close(created));_exit(0);}
    child_done(child);char child_path[180];snprintf(child_path,sizeof(child_path),"%s%s",second,extra_name);
    CHECK(!stat(child_path,&st)&&st.st_uid==23456&&st.st_gid==23457);CHECK(!mq_unlink(extra_name));extra_name[0]=0;
    CHECK(!chown(first,root_original.st_uid,root_original.st_gid)&&!chmod(first,01777));
    int pinned=open(other,O_PATH);CHECK(pinned>=0);CHECK(!fchown(q,23456,23457));
    child=fork();CHECK(child>=0);if(!child){numeric_user();CHECK(!unlink(other));_exit(0);}child_done(child);CHECK(!close(pinned));
    ERROR(stat(path,&st),ENOENT);
    CHECK(!chmod(first,root_original.st_mode&07777));root_changed=0;return 1;
}
static void filesystem_view(void)
{
    const char *first=getpid()==1?"/mqa":"/dev/mqueue";const char *second=first;
    if(getpid()==1){
        CHECK(!mkdir("/mqa",0755)&&!mkdir("/mqb",0755));
        CHECK(!mount("mqueue","/mqa","mqueue",0,0)&&!mount("mqueue","/mqb","mqueue",0,0));second="/mqb";
    }
    mqd_t q=make(2,32,O_NONBLOCK);CHECK(!mq_send(q,"view",5,1));
    char path[160],other[160],buffer[160];snprintf(path,sizeof(path),"%s%s",first,queue_name);
    snprintf(other,sizeof(other),"%s%s",second,queue_name);int fd=open(path,O_RDWR);CHECK(fd>=0);
    CHECK(read(fd,buffer,sizeof(buffer)-1)>0);CHECK(!memcmp(buffer,"QSIZE:5",7));
    CHECK(lseek(fd,0,SEEK_SET)==0);struct iovec iov[2]={{buffer,3},{buffer+3,sizeof(buffer)-4}};
    CHECK(readv(fd,iov,2)>7&&!memcmp(buffer,"QSIZE:5",7));
    CHECK(pread(fd,buffer,7,0)==7&&!memcmp(buffer,"QSIZE:5",7));
    CHECK(!fchmod(fd,0640));struct stat one,two;CHECK(!fstat(q,&one)&&!stat(other,&two));
    CHECK((one.st_mode&0777)==0640&&(two.st_mode&0777)==0640&&one.st_ino==two.st_ino);
    CHECK(mq_receive(fd,buffer,32,0)==5&&!memcmp(buffer,"view",5));
    char proc[80];snprintf(proc,sizeof(proc),"/proc/self/fd/%d",q);ssize_t n=readlink(proc,buffer,sizeof(buffer)-1);
    CHECK(n>0);buffer[n]=0;CHECK(!strcmp(buffer,queue_name));
    struct timespec times[2]={{123,4},{125,6}};CHECK(!utimensat(AT_FDCWD,other,times,0));
    CHECK(!fstat(q,&one)&&one.st_mtim.tv_sec==125&&one.st_mtim.tv_nsec==0);
    times[1].tv_sec=127;CHECK(!futimens(q,times));
    CHECK(!stat(other,&two)&&two.st_mtim.tv_sec==127&&!two.st_mtim.tv_nsec);
    if(!root_and_sticky(first,second,path,other,q))CHECK(!unlink(other));
    ERROR(mq_open(queue_name,O_RDWR),ENOENT);CHECK(!fstat(q,&one)&&!one.st_nlink);
    n=readlink(proc,buffer,sizeof(buffer)-1);CHECK(n>0);buffer[n]=0;
    char deleted[100];snprintf(deleted,sizeof(deleted),"%s (deleted)",queue_name);CHECK(!strcmp(buffer,deleted));
    snprintf(proc,sizeof(proc),"/proc/self/fd/%d",fd);n=readlink(proc,buffer,sizeof(buffer)-1);CHECK(n>0);buffer[n]=0;
    char mounted_deleted[180];snprintf(mounted_deleted,sizeof(mounted_deleted),"%s (deleted)",path);CHECK(!strcmp(buffer,mounted_deleted));
    if(getpid()==1)ERROR(umount(first),EBUSY);
    CHECK(!close(fd)&&!mq_close(q));
    if(getpid()==1)CHECK(!umount("/mqa")&&!umount("/mqb"));
    pass("shared-mount-namespace-state-read-readv-pread-mode-inode-proc-unlink");
}
int main(int argc,char **argv)
{
    setvbuf(stdout,0,_IONBF,0);CHECK(!atexit(cleanup));alarm(45);
    snprintf(queue_name,sizeof(queue_name),"/boaros-mq-%ld",(long)getpid());cleanup();
    if(getpid()==1){CHECK(!mkdir("/proc",0555)||errno==EEXIST);CHECK(!mount("proc","/proc","proc",0,0)||errno==EBUSY);}
    sigemptyset(&notify_mask);sigaddset(&notify_mask,SIGUSR1);CHECK(!pthread_sigmask(SIG_BLOCK,&notify_mask,0));
    puts("POSIX-MQ BEGIN");abi_priority_flags();poll_and_fault();notification_signal();notification_thread();creation_quota();
    if(argc==1||strcmp(argv[1],"--no-fs"))filesystem_view();
    printf("POSIX-MQ END %u\n",cases);alarm(0);if(getpid()==1){sync();reboot(RB_POWER_OFF);}return 0;
}
