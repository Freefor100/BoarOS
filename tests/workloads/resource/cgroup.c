#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { dprintf(2,"CGROUP FAIL line=%d errno=%d %s\n",__LINE__,errno,#x); exit(1); } } while (0)
struct args { uint64_t flags,pidfd,ctid,ptid,signal,stack,stack_size,tls,set_tid,set_tid_size,cgroup; };
static const char *base="/tmp/cg-test";
static int dir, root, procs;
static void write_fd(int fd,const char *text)
{ CHECK(write(fd,text,strlen(text))==(ssize_t)strlen(text)); }
static void contents_at(int parent,const char *name,char *buf,size_t capacity)
{
    int fd=openat(parent,name,O_RDONLY);CHECK(fd>=0);
    ssize_t total=0,n;
    while ((n=read(fd,buf+total,capacity-1-(size_t)total))>0) { total+=n;CHECK((size_t)total<capacity-1); }
    CHECK(n==0);buf[total]=0;CHECK(!close(fd));
}
static int contains(int parent,pid_t id)
{
    char buf[4096];contents_at(parent,"cgroup.procs",buf,sizeof(buf));
    char wanted[32];snprintf(wanted,sizeof(wanted),"%d\n",id);
    char *p=strstr(buf,wanted);return p && (p==buf||p[-1]=='\n');
}
static void location(const char *expected)
{ char b[4096];int fd=open("/proc/self",O_RDONLY|O_DIRECTORY);CHECK(fd>=0);contents_at(fd,"cgroup",b,sizeof(b));CHECK(!close(fd));CHECK(!strcmp(b,expected)); }
static pid_t into(int fd)
{ struct args a={.flags=UINT64_C(0x200000000),.signal=SIGCHLD,.cgroup=(unsigned)fd};return syscall(435,&a,sizeof(a)); }
static void wait_ok(pid_t child)
{int s;CHECK(waitpid(child,&s,0)==child);CHECK(WIFEXITED(s)&&WEXITSTATUS(s)==0);}
static int ready[2],release_child[2];
static void *thread(void *unused)
{
    (void)unused;write_fd(ready[1],"R");char c;CHECK(read(release_child[0],&c,1)==1);
    char b[256];int fd=open("/proc/thread-self",O_RDONLY|O_DIRECTORY);CHECK(fd>=0);contents_at(fd,"cgroup",b,sizeof(b));CHECK(!close(fd));CHECK(!strcmp(b,"0::/boaros-test/a\n"));return 0;
}
static void *exec_after_leader(void *unused)
{
    (void)unused;
    for (;;) {
        char b[1024];int fd=open("/proc/self",O_RDONLY|O_DIRECTORY);CHECK(fd>=0);
        contents_at(fd,"stat",b,sizeof(b));CHECK(!close(fd));char *state=strrchr(b,')');CHECK(state);
        if (state[2]=='Z') break;
        sched_yield();
    }
    write_fd(ready[1],"R");char token;CHECK(read(release_child[0],&token,1)==1);
    char b[256];int fd=open("/proc/thread-self",O_RDONLY|O_DIRECTORY);CHECK(fd>=0);
    contents_at(fd,"cgroup",b,sizeof(b));CHECK(!close(fd));CHECK(!strcmp(b,"0::/boaros-test/a\n"));
    char *args[]={"cgroup","exec-child",0};execv("/proc/thread-self/exe",args);CHECK(0);return 0;
}
int main(int argc,char **argv)
{
    if (argc==2 && !strcmp(argv[1],"exec-child")) {location("0::/boaros-test/a\n");return 0;}
    mkdir("/proc",0755);if (mount("proc","/proc","proc",0,0)&&errno!=EBUSY)CHECK(0);
    mkdir("/tmp",01777);CHECK(!mkdir(base,0755));CHECK(!mount("none",base,"cgroup2",0,0));
    root=open(base,O_RDONLY|O_DIRECTORY);CHECK(root>=0);
    CHECK(!mkdirat(root,"boaros-test",0755));dir=openat(root,"boaros-test",O_PATH|O_DIRECTORY);CHECK(dir>=0);
    CHECK(!mkdirat(dir,"a",0755));int a=openat(dir,"a",O_PATH|O_DIRECTORY);CHECK(a>=0);
    procs=openat(a,"cgroup.procs",O_WRONLY|O_TRUNC);CHECK(procs>=0);
    char b[256];contents_at(a,"cgroup.type",b,sizeof(b));CHECK(!strcmp(b,"domain\n"));
    CHECK(!mkdir("/tmp/cg-alias",0755));CHECK(!mount("none","/tmp/cg-alias","cgroup2",0,0));
    int alias=open("/tmp/cg-alias/boaros-test/a",O_RDONLY|O_DIRECTORY);CHECK(alias>=0);
    struct stat x,y;CHECK(!fstat(a,&x)&&!fstat(alias,&y));CHECK(x.st_ino==y.st_ino&&x.st_dev==y.st_dev);
    CHECK(!mkdirat(a,"cached",0755));int old=openat(a,"cached",O_PATH|O_DIRECTORY);CHECK(old>=0);
    CHECK(!fstat(old,&x));CHECK(!unlinkat(alias,"cached",AT_REMOVEDIR));CHECK(!mkdirat(alias,"cached",0755));
    int fresh=openat(a,"cached",O_PATH|O_DIRECTORY);CHECK(fresh>=0);CHECK(!fstat(fresh,&y));CHECK(x.st_ino!=y.st_ino);
    errno=0;CHECK(into(old)==-1&&errno==ENOENT);CHECK(!close(old)&&!close(fresh));CHECK(!unlinkat(a,"cached",AT_REMOVEDIR));
    CHECK(!close(alias));CHECK(!umount("/tmp/cg-alias"));CHECK(!rmdir("/tmp/cg-alias"));
    CHECK(!mkdir("/tmp/cg-ro",0755));CHECK(!mount("none","/tmp/cg-ro","cgroup2",MS_RDONLY,0));
    errno=0;CHECK(open("/tmp/cg-ro/boaros-test/a/cgroup.procs",O_WRONLY)==-1&&errno==EROFS);
    CHECK(!umount("/tmp/cg-ro"));CHECK(!rmdir("/tmp/cg-ro"));
    int control=openat(a,"cgroup.type",O_RDONLY);CHECK(control>=0);
    errno=0;CHECK(mmap(0,4096,PROT_READ,MAP_PRIVATE,control,0)==MAP_FAILED&&errno==ENODEV);CHECK(!close(control));
    puts("CGROUP CASE hierarchy PASS");
    CHECK(!pipe(ready)&&!pipe(release_child));
    pid_t child=into(a);CHECK(child>=0);
    if (!child) { location("0::/boaros-test/a\n");CHECK(contains(a,getpid()));write_fd(ready[1],"R");char c;CHECK(read(release_child[0],&c,1)==1);_exit(0); }
    char token;CHECK(read(ready[0],&token,1)==1);CHECK(contains(a,child));
    errno=0;CHECK(unlinkat(dir,"a",AT_REMOVEDIR)==-1&&errno==EBUSY);
    write_fd(release_child[1],"X");wait_ok(child);CHECK(!contains(a,child));
    CHECK(!mkdirat(dir,"zombie",0755));int zombie=openat(dir,"zombie",O_PATH|O_DIRECTORY);CHECK(zombie>=0);
    child=into(zombie);CHECK(child>=0);if(!child)_exit(0);
    siginfo_t info={0};CHECK(!waitid(P_PID,child,&info,WEXITED|WNOWAIT));CHECK(info.si_pid==child);
    char path[64];snprintf(path,sizeof(path),"/proc/%d",child);int zproc=open(path,O_RDONLY|O_DIRECTORY);CHECK(zproc>=0);
    contents_at(zproc,"cgroup",b,sizeof(b));CHECK(!strcmp(b,"0::/boaros-test/zombie\n"));
    CHECK(!unlinkat(dir,"zombie",AT_REMOVEDIR));contents_at(zproc,"cgroup",b,sizeof(b));CHECK(!strcmp(b,"0::/boaros-test/zombie (deleted)\n"));
    CHECK(!close(zproc)&&!close(zombie));wait_ok(child);
    puts("CGROUP CASE atomic-clone-exit PASS");
    errno=0;CHECK(into(-1)==-1&&errno==EINVAL); // unsigned fd exceeds INT_MAX
    errno=0;CHECK(into(procs)==-1&&errno==EBADF);
    int ordinary=open("/tmp",O_RDONLY|O_DIRECTORY);CHECK(ordinary>=0);errno=0;CHECK(into(ordinary)==-1&&errno==EBADF);CHECK(!close(ordinary));
    struct args bad={.flags=UINT64_C(0x200001000),.signal=SIGCHLD,.cgroup=(unsigned)a,.pidfd=1};
    errno=0;CHECK(syscall(435,&bad,sizeof(bad))==-1&&errno==EFAULT);CHECK(!contains(a,getpid()));
    CHECK(!mkdirat(dir,"dead",0755));int dead=openat(dir,"dead",O_PATH|O_DIRECTORY);CHECK(dead>=0);CHECK(!unlinkat(dir,"dead",AT_REMOVEDIR));errno=0;CHECK(into(dead)==-1&&errno==ENOENT);CHECK(!close(dead));
    puts("CGROUP CASE fd-and-rollback PASS");
    child=fork();CHECK(child>=0);
    if (!child) {
        pthread_t t;CHECK(!pthread_create(&t,0,thread,0));CHECK(read(ready[0],&token,1)==1);
        write_fd(procs,"0");location("0::/boaros-test/a\n");
        pid_t nested=fork();CHECK(nested>=0);if(!nested){location("0::/boaros-test/a\n");_exit(0);}wait_ok(nested);
        write_fd(release_child[1],"X");CHECK(!pthread_join(t,0));_exit(0);
    }
    wait_ok(child);
    child=fork();CHECK(child>=0);
    if(!child){pthread_t t;CHECK(!pthread_create(&t,0,exec_after_leader,0));syscall(SYS_exit,0);__builtin_unreachable();}
    CHECK(read(ready[0],&token,1)==1);char pidtext[32];snprintf(pidtext,sizeof(pidtext),"%d",child);write_fd(procs,pidtext);
    CHECK(contains(a,child));write_fd(release_child[1],"X");wait_ok(child);
    puts("CGROUP CASE group-migration-inheritance PASS");
    child=fork();CHECK(child>=0);
    if (!child) {
        CHECK(!setgid(1000)&&!setuid(1000));
        errno=0;CHECK(into(a)==-1&&errno==EACCES);
        write_fd(procs,"0"); // root opener remains authorized after the caller drops uid
        location("0::/boaros-test/a\n");_exit(0);
    }
    wait_ok(child);
    child=fork();CHECK(child>=0);
    if(!child){CHECK(!ptrace(PTRACE_TRACEME,0,0,0));raise(SIGSTOP);CHECK(!setgid(1000)&&!setuid(1000));
        errno=0;CHECK(into(a)==-1&&errno==EACCES);_exit(0);}
    int trace_status;CHECK(waitpid(child,&trace_status,0)==child&&WIFSTOPPED(trace_status));
    CHECK(!ptrace(PTRACE_SETOPTIONS,child,0,PTRACE_O_TRACEFORK));CHECK(!ptrace(PTRACE_CONT,child,0,0));wait_ok(child);
    puts("CGROUP CASE delegation-opener PASS");
    CHECK(!close(procs));CHECK(!close(a));CHECK(!unlinkat(dir,"a",AT_REMOVEDIR));CHECK(!close(dir));
    CHECK(!unlinkat(root,"boaros-test",AT_REMOVEDIR));CHECK(!close(root));
    CHECK(!umount(base));CHECK(!rmdir(base));
    /* Preserve an unmounted hierarchy until shutdown, not until last umount. */
    CHECK(!mkdir(base,0755));CHECK(!mount("none",base,"cgroup2",0,0));
    root=open(base,O_RDONLY|O_DIRECTORY);CHECK(root>=0);CHECK(!mkdirat(root,"shutdown-owner",0755));CHECK(!close(root));
    CHECK(!umount(base));CHECK(!mount("none",base,"cgroup2",0,0));
    root=open(base,O_RDONLY|O_DIRECTORY);CHECK(root>=0);CHECK(!fstatat(root,"shutdown-owner",&x,0));CHECK(!close(root));
    CHECK(!umount(base));CHECK(!rmdir(base));
    close(ready[0]);close(ready[1]);close(release_child[0]);close(release_child[1]);
    puts("CGROUP END 5");return 0;
}
