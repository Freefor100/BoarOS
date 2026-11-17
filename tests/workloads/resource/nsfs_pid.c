#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr,"NSFS FAIL %d: %s errno=%d\n",__LINE__,#c,errno); exit(1); } } while (0)
#define PARENT 0xb702U
#define PID_FROM 0x8004b706U
#define TGID_FROM 0x8004b707U
#define PID_IN 0x8004b708U
#define TGID_IN 0x8004b709U
static void *thread_case(void *arg)
{
    int fd=*(int *)arg, tid=(int)syscall(SYS_gettid);
    CHECK(tid!=getpid());
    CHECK(ioctl(fd,PID_FROM,tid)==tid && ioctl(fd,PID_IN,tid)==tid);
    CHECK(ioctl(fd,TGID_FROM,tid)==getpid() && ioctl(fd,TGID_IN,tid)==getpid());
    return (void *)(intptr_t)tid;
}
static void nested(int root)
{
    int ready[2],done[2]; CHECK(!pipe(ready) && !pipe(done));
    CHECK(!unshare(CLONE_NEWPID));
    int outer=getpid(),status;
    pid_t child=fork(); CHECK(child>=0);
    if (!child) {
        CHECK(!close(ready[0]) && !close(done[1]));
        int self=open("/proc/self/ns/pid",O_RDONLY); CHECK(self>=0 && getpid()==1);
        CHECK(ioctl(self,PARENT)==-1 && errno==EPERM);
        CHECK(ioctl(root,PID_FROM,outer)==-1 && errno==ESRCH);
        int global=ioctl(root,PID_IN,1); CHECK(global>1);
        CHECK(write(ready[1],&global,sizeof(global))==sizeof(global));
        char c; CHECK(read(done[0],&c,1)==1); _exit(0);
    }
    CHECK(!close(ready[1]) && !close(done[0]));
    int global; CHECK(read(ready[0],&global,sizeof(global))==sizeof(global) && global==child);
    int view=open("/proc/self/ns/pid_for_children",O_RDONLY); CHECK(view>=0);
    int parent=ioctl(view,PARENT); CHECK(parent>=0 && (fcntl(parent,F_GETFD)&FD_CLOEXEC));
    CHECK((fcntl(parent,F_GETFL)&O_ACCMODE)==O_RDONLY);
    struct stat a,b; CHECK(!fstat(root,&a) && !fstat(parent,&b) && a.st_ino==b.st_ino && a.st_dev==b.st_dev);
    /* A full descriptor table must not leak the parent snapshot or a slot. */
    struct rlimit saved,limited;
    CHECK(!getrlimit(RLIMIT_NOFILE,&saved)); limited=saved;
    if (limited.rlim_cur>64) limited.rlim_cur=64;
    CHECK(!setrlimit(RLIMIT_NOFILE,&limited));
    int fill[64],count=0,copy;
    while ((copy=dup(view))>=0) { CHECK(count<64); fill[count++]=copy; }
    CHECK(errno==EMFILE && ioctl(view,PARENT)==-1 && errno==EMFILE && count>0);
    CHECK(!close(fill[--count]));
    copy=ioctl(view,PARENT); CHECK(copy>=0 && !close(copy));
    while (count) CHECK(!close(fill[--count]));
    CHECK(!setrlimit(RLIMIT_NOFILE,&saved));
    CHECK(ioctl(view,PID_FROM,1)==child && ioctl(view,TGID_IN,child)==1);
    CHECK(ioctl(view,PID_IN,outer)==-1 && errno==ESRCH);
    CHECK(write(done[1],"x",1)==1 && waitpid(child,&status,0)==child && status==0);
    CHECK(ioctl(view,PID_FROM,1)==-1 && errno==ESRCH);
    CHECK(!close(parent) && !close(view));
    puts("NSFS nested parent FD and directed translations PASS");
}
int main(int argc,char **argv)
{
    int fd=open("/proc/self/ns/pid",O_RDONLY), uts=open("/proc/self/ns/uts",O_RDONLY);
    CHECK(fd>=0 && uts>=0);
    int path=open("/proc/self/ns/pid",O_PATH); CHECK(path>=0);
    CHECK(ioctl(path,PID_IN,getpid())==-1 && errno==EBADF && !close(path));
    CHECK(ioctl(fd,0xb7ffU,1)==-1 && errno==ENOTTY);
    unsigned commands[]={PID_FROM,TGID_FROM,PID_IN,TGID_IN};
    for (unsigned i=0;i<4;++i) {
        CHECK(ioctl(uts,commands[i],1)==-1 && errno==EINVAL);
        CHECK(ioctl(fd,commands[i],0)==-1 && errno==ESRCH);
        CHECK(ioctl(fd,commands[i],-1L)==-1 && errno==ESRCH);
        CHECK(ioctl(fd,commands[i],(1ULL<<32)|(unsigned)getpid())==getpid());
    }
    CHECK(ioctl(uts,PARENT)==-1 && errno==EINVAL);
    CHECK(ioctl(fd,PARENT)==-1 && errno==EPERM);
    pthread_t thread; void *tid;
    CHECK(!pthread_create(&thread,NULL,thread_case,&fd) && !pthread_join(thread,&tid));
    /* pthread_join observes clear_child_tid; final PID unhash may follow.
     * waitpid, rather than thread exit, is the deterministic reap boundary. */
    int status; pid_t child=fork(); CHECK(child>=0); if (!child) _exit(0);
    CHECK(waitpid(child,&status,0)==child && !status);
    CHECK(ioctl(fd,TGID_FROM,child)==-1 && errno==ESRCH);
    if (argc==2 && !strcmp(argv[1],"--nested")) nested(fd); else CHECK(argc==1);
    CHECK(!close(fd) && !close(uts));
    puts("NSFS PID PASS scalar TID TGID reap and errno");
}
