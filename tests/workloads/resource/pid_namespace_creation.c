#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

/* Privileged guest-only workload, run in a fresh root with proc mounted.
 * Exact outer PID requests below deliberately use idle slots of that fixture. */
struct clone_args_local {
    uint64_t flags,pidfd,child_tid,parent_tid,exit_signal,stack,stack_size,tls,set_tid,set_tid_size,cgroup;
};
static void success(pid_t p)
{
    int status;
    assert(p>0 && waitpid(p,&status,0)==p && WIFEXITED(status) && !WEXITSTATUS(status));
}
static void field(int dir,const char *path,const char *field,const char *value)
{
    char buf[4096],line[128];int fd=openat(dir,path,O_RDONLY);assert(fd>=0);
    ssize_t n=read(fd,buf,sizeof(buf)-1);assert(n>0);buf[n]=0;assert(!close(fd));
    assert(snprintf(line,sizeof(line),"\n%s:\t%s\n",field,value)>0);
    assert(strstr(buf,line));
}
static int proc_view(void)
{
    int ctx=syscall(SYS_fsopen,"proc",1);assert(ctx>=0);
    assert(!syscall(SYS_fsconfig,ctx,6,NULL,NULL,0));
    int tree=syscall(SYS_fsmount,ctx,1,0);assert(tree>=0 && !close(ctx));return tree;
}
static void tree(void)
{
    assert(getpid()==1 && getppid()==0);
    int next[2],ready[2],view=proc_view();assert(!pipe(next) && !pipe(ready));
    int fd=-1; int32_t ids[]={1,73,313};
    struct clone_args_local a={.flags=CLONE_NEWPID|CLONE_PIDFD,.exit_signal=SIGCHLD,
        .pidfd=(uintptr_t)&fd,.set_tid=(uintptr_t)ids,.set_tid_size=3};
    assert(!mkdir("/cnscg",0755) && !mount("none","/cnscg","cgroup2",0,NULL));
    assert(!mkdir("/cnscg/leaf",0755));
    int cg=open("/cnscg/leaf",O_DIRECTORY|O_RDONLY);assert(cg>=0);
    a.flags|=UINT64_C(0x200000000);a.cgroup=INT32_MAX;
    errno=0;assert(syscall(SYS_clone3,&a,sizeof(a))==-1 && errno==EBADF);
    if(fd>=0){errno=0;assert(fcntl(fd,F_GETFD)==-1 && errno==EBADF);}
    a.cgroup=cg;fd=-1;
    pid_t p=syscall(SYS_clone3,&a,sizeof(a));assert(p>=0);
    if(!p){
        assert(getpid()==1 && getppid()==0);int local=proc_view();
        char group[128]={0};int membership=openat(local,"self/cgroup",O_RDONLY);assert(membership>=0);
        assert(read(membership,group,sizeof(group)-1)>0 && !strcmp(group,"0::/leaf\n") && !close(membership));
        field(local,"self/status","NSpid","1");field(view,"self/status","NSpid","73\t1");
        field(AT_FDCWD,"/proc/self/status","NSpid","313\t73\t1");
        char byte='x';assert(write(ready[1],&byte,1)==1 && read(next[0],&byte,1)==1);
        _exit(0);
    }
    assert(p==73 && fd>=0);
    char byte,path[80];assert(read(ready[0],&byte,1)==1);
    snprintf(path,sizeof(path),"self/fdinfo/%d",fd);
    field(view,path,"NSpid","73\t1");
    assert(!ptrace(PTRACE_SEIZE,p,0,0) && !ptrace(PTRACE_INTERRUPT,p,0,0));
    int status;assert(waitpid(p,&status,__WALL)==p && WIFSTOPPED(status));
    field(view,"73/status","TracerPid","1");
    assert(!ptrace(PTRACE_DETACH,p,0,0));assert(write(next[1],&byte,1)==1);
    struct pollfd pollfd={.fd=fd,.events=POLLIN};
    assert(poll(&pollfd,1,5000)==1 && (pollfd.revents&POLLIN));
    success(p);field(view,path,"Pid","-1");assert(!close(fd) && !close(view) && !close(cg));
    assert(!rmdir("/cnscg/leaf") && !umount("/cnscg") && !rmdir("/cnscg"));
}
static void thread_boundary(void)
{
    pid_t p=fork();assert(p>=0);
    if(!p){
        assert(!unshare(CLONE_NEWPID));
        struct rlimit limit={0,0};assert(!setrlimit(RLIMIT_NPROC,&limit));
        assert(!setresuid(20091,20091,20091));
        struct clone_args_local a={.flags=CLONE_VM|CLONE_SIGHAND|CLONE_THREAD};
        errno=0;assert(syscall(SYS_clone3,&a,sizeof(a))==-1 && errno==EINVAL);
        errno=0;assert(syscall(SYS_clone,a.flags,0,0,0,0)==-1 && errno==EINVAL);
        a.flags=CLONE_NEWPID;a.exit_signal=SIGCHLD;
        errno=0;assert(syscall(SYS_clone3,&a,sizeof(a))==-1 && errno==EAGAIN);
        _exit(0);
    }
    success(p);
}
static void traced_drain(void)
{
    enum { member_in_root = 325 };
    int ready[2],go[2],fd=-1;assert(!pipe(ready) && !pipe(go));
    struct clone_args_local a={.flags=CLONE_NEWPID|CLONE_PIDFD,.pidfd=(uintptr_t)&fd,.exit_signal=SIGCHLD};
    pid_t init=syscall(SYS_clone3,&a,sizeof(a));assert(init>=0);
    if(!init){
        assert(getpid()==1);
        int32_t nums[]={41,member_in_root};struct clone_args_local b={.set_tid=(uintptr_t)nums,.set_tid_size=2,.exit_signal=SIGCHLD};
        pid_t p=syscall(SYS_clone3,&b,sizeof(b));assert(p>=0);
        if(!p){for(;;)pause();}
        assert(p==41);char c='r';assert(write(ready[1],&c,1)==1 && read(go[0],&c,1)==1);_exit(19);
    }
    char c;assert(read(ready[0],&c,1)==1);
    assert(!ptrace(PTRACE_SEIZE,member_in_root,0,0) && !ptrace(PTRACE_INTERRUPT,member_in_root,0,0));
    int st;assert(waitpid(member_in_root,&st,__WALL)==member_in_root && WIFSTOPPED(st));
    assert(write(go[1],&c,1)==1);
    /* Leave the tracee's final wait report pending to its external tracer. */
    siginfo_t event={0};assert(!waitid(P_PID,member_in_root,&event,WEXITED|WNOWAIT|__WALL));
    assert(event.si_code==CLD_KILLED && event.si_status==SIGKILL);
    struct pollfd init_fd={.fd=fd,.events=POLLIN};
    assert(!poll(&init_fd,1,0));
    assert(waitpid(member_in_root,&st,__WALL)==member_in_root && WIFSIGNALED(st) && WTERMSIG(st)==SIGKILL);
    assert(waitpid(init,&st,0)==init && WIFEXITED(st) && WEXITSTATUS(st)==19);
    assert(poll(&init_fd,1,0)==1 && (init_fd.revents&POLLIN));
    assert(!close(fd));puts("PIDNS DRAIN PASS external tracer owns member wait before namespace init completion");
}

static int thread_ready[2],thread_go[2];
static void *stay(void *unused)
{
    (void)unused;char x='x';
    assert(write(thread_ready[1],&x,1)==1 && read(thread_go[0],&x,1)==1);
    return NULL;
}
static void explicit_thread(void)
{
    pid_t p=fork();assert(p>=0);
    if(!p){
        assert(!pipe(thread_ready) && !pipe(thread_go));pthread_t t;
        assert(!pthread_create(&t,NULL,stay,NULL));char x;assert(read(thread_ready[0],&x,1)==1);
        errno=0;assert(unshare(CLONE_NEWPID|CLONE_THREAD)==-1 && errno==EINVAL);
        assert(!unshare(CLONE_NEWPID));pid_t child=fork();assert(child>=0);
        if(!child)_exit(getpid()!=1);
        success(child);assert(write(thread_go[1],&x,1)==1 && !pthread_join(t,NULL));_exit(0);
    }
    success(p);
}
static void permissions(void)
{
    pid_t p=fork();assert(p>=0);
    if(!p){
        assert(!setresuid(20092,20092,20092));errno=0;
        assert(unshare(CLONE_NEWPID)==-1 && errno==EPERM);
        struct clone_args_local a={.flags=CLONE_NEWPID,.exit_signal=SIGCHLD};
        errno=0;assert(syscall(SYS_clone3,&a,sizeof(a))==-1 && errno==EPERM);
        errno=0;assert(syscall(SYS_clone,CLONE_NEWPID|SIGCHLD,0,0,0,0)==-1 && errno==EPERM);
        a.flags|=CLONE_VM|CLONE_SIGHAND|CLONE_THREAD;a.exit_signal=0;
        errno=0;assert(syscall(SYS_clone3,&a,sizeof(a))==-1 && errno==EINVAL);
        _exit(0);
    }
    success(p);
}
int main(void)
{
    permissions();explicit_thread();traced_drain();
    thread_boundary();
    pid_t original=getpid(),host=fork();assert(host>=0);
    if(!host){
        pid_t before=getpid();assert(!unshare(CLONE_NEWPID));
        assert(getpid()==before && getppid()==original);
        pid_t middle=fork();assert(middle>=0);
        if(!middle){tree();_exit(0);}
        success(middle);
        errno=0;assert(fork()==-1 && errno==ENOMEM);
        _exit(0);
    }
    success(host);
    puts("PIDNS CREATE PASS unshare stable caller, three-level set_tid, pidfd wait, dead child namespace");
    return 0;
}
