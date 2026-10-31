#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { if(!(x)) { dprintf(2,"IOPRIO FAIL line=%d errno=%d: %s\n",__LINE__,errno,#x); _exit(1); } } while(0)
#define BE(n) ((2<<13)|(n))
#define CLONE_IO_VALUE 0x80000000U
#define KCMP_IO_VALUE 5
static long setprio(int which,int who,int priority){return syscall(30,which,who,priority);}
static long getprio(int which,int who){return syscall(31,which,who);}
static long cmp(pid_t a,pid_t b){return syscall(272,a,b,KCMP_IO_VALUE,0,0);}
static void sendbyte(int fd,char c){CHECK(write(fd,&c,1)==1);}
static void recvbyte(int fd,char c){char b;CHECK(read(fd,&b,1)==1 && b==c);}
static void joined(pid_t p){int s;CHECK(waitpid(p,&s,0)==p && WIFEXITED(s) && !WEXITSTATUS(s));}
struct gate {int ready,go;};
struct clone3_args {
    uint64_t flags,pidfd,child_tid,parent_tid,exit_signal,stack,stack_size,tls,set_tid,set_tid_size,cgroup;
};
static int child(void *p){struct gate *g=p;sendbyte(g->ready,'r');recvbyte(g->go,'x');return 0;}
static int lazy_child(void *p)
{
    struct gate *g=p;sendbyte(g->ready,'r');recvbyte(g->go,'s');
    CHECK(getprio(1,0)==0);sendbyte(g->ready,'s');recvbyte(g->go,'x');return 0;
}
static void lazy_context(void)
{
    pid_t owner=fork();CHECK(owner>=0);
    if(!owner) {
        int to[2],from[2];CHECK(!pipe(to)&&!pipe(from));char *stack=malloc(65536);CHECK(stack);
        struct gate g={from[1],to[0]};pid_t p=clone(lazy_child,stack+65536,CLONE_IO_VALUE|SIGCHLD,&g);CHECK(p>0);
        recvbyte(from[0],'r');CHECK(cmp(getpid(),p)==0);
        CHECK(setprio(1,0,BE(5))==0);long order=cmp(getpid(),p);CHECK(order==1||order==2);
        sendbyte(to[1],'s');recvbyte(from[0],'s');sendbyte(to[1],'x');joined(p);_exit(0);
    }
    joined(owner);
}
static void permission_checks(void)
{
    pid_t parent=getpid(),p=fork();CHECK(p>=0);
    if(!p) {
        CHECK(!setgid(12345) && !setuid(12345));
        errno=0;CHECK(setprio(1,parent,BE(1))==-1 && errno==EPERM);
        errno=0;CHECK(setprio(1,0,1<<13)==-1 && errno==EPERM);
        errno=0;CHECK(setprio(3,0,BE(6))==-1 && errno==EPERM);
        CHECK(!setprio(3,12345,BE(6)) && getprio(1,0)==BE(6) && getprio(3,0)==BE(6));
        _exit(0);
    }
    joined(p);
}
static void *thread_exec(void *unused)
{
    (void)unused;CHECK(!setprio(1,0,BE(7)));
    char *av[]={"ioprio","thread-exec",0};execv("/proc/self/exe",av);_exit(98);
}
static int shared_exec(void *opaque)
{
    struct gate *g=opaque;char ready[24],go[24];
    snprintf(ready,sizeof(ready),"%d",g->ready);snprintf(go,sizeof(go),"%d",g->go);
    char *av[]={"ioprio","shared-exec",ready,go,0};execv("/proc/self/exe",av);_exit(97);
}
int main(int argc,char **argv)
{
    setvbuf(stdout,0,_IONBF,0);
    if(argc==2) return getprio(1,0)==BE(!strcmp(argv[1],"thread-exec")?7:6)?0:1;
    if(argc==4 && !strcmp(argv[1],"shared-exec")) {
        sendbyte(atoi(argv[2]),'e');recvbyte(atoi(argv[3]),'x');
        return getprio(1,0)==BE(6)?0:1;
    }
    if(access("/proc/self/exe",F_OK)) {
        CHECK(!mkdir("/proc",0555)||errno==EEXIST);
        CHECK(!mount("proc","/proc","proc",0,0));
    }
    CHECK(getprio(1,0)==0);
    errno=0;CHECK(setprio(1,0,1)==-1 && errno==EINVAL);
    errno=0;CHECK(setprio(1,0,4<<13)==-1 && errno==EINVAL);
    errno=0;CHECK(getprio(99,0)==-1 && errno==EINVAL);
    errno=0;CHECK(getprio(1,999999)==-1 && errno==ESRCH);
    lazy_context();
    CHECK(setprio(1,0,BE(4))==0 && getprio(1,0)==BE(4));
    int to[2],from[2];CHECK(!pipe(to)&&!pipe(from));
    char *stack=malloc(65536);CHECK(stack);struct gate g={from[1],to[0]};
    pid_t p=clone(child,stack+65536,CLONE_IO_VALUE|SIGCHLD,&g);CHECK(p>0);
    recvbyte(from[0],'r');CHECK(cmp(getpid(),p)==0);
    CHECK(setprio(1,p,BE(2))==0 && getprio(1,0)==BE(2));
    sendbyte(to[1],'x');joined(p);free(stack);
    p=fork();CHECK(p>=0);
    if(!p){CHECK(getprio(1,0)==BE(2));sendbyte(from[1],'f');recvbyte(to[0],'x');_exit(0);}
    recvbyte(from[0],'f');long different=cmp(getpid(),p);CHECK(different==1||different==2);
    CHECK(setprio(1,p,BE(3))==0 && getprio(1,0)==BE(2) && getprio(1,p)==BE(3));
    sendbyte(to[1],'x');joined(p);
    CHECK(!close(to[0])&&!close(to[1])&&!close(from[0])&&!close(from[1]));
    p=fork();CHECK(p>=0);
    if(!p) {
        CHECK(setsid()>0 && getprio(2,0)==BE(2));
        CHECK(!setprio(2,0,BE(1)) && getprio(1,0)==BE(1));_exit(0);
    }
    joined(p);
    p=fork();CHECK(p>=0);
    if(!p) {
        pthread_t t;CHECK(!setprio(1,0,BE(3)) && !pthread_create(&t,0,thread_exec,0));
        (void)pthread_join(t,0);_exit(96);
    }
    joined(p);
    CHECK(!pipe(to)&&!pipe(from));stack=malloc(65536);CHECK(stack);
    g=(struct gate){from[1],to[0]};
    p=clone(shared_exec,stack+65536,CLONE_IO_VALUE|SIGCHLD,&g);CHECK(p>0);
    recvbyte(from[0],'e');CHECK(!cmp(getpid(),p));
    CHECK(!setprio(1,0,BE(6)));sendbyte(to[1],'x');joined(p);free(stack);
    CHECK(!close(to[0])&&!close(to[1])&&!close(from[0])&&!close(from[1]));
    CHECK(!pipe(to)&&!pipe(from));
    struct clone3_args args={.flags=CLONE_IO_VALUE,.exit_signal=SIGCHLD};
    p=(pid_t)syscall(435,&args,sizeof(args));CHECK(p>=0);
    if(!p) {sendbyte(from[1],'r');recvbyte(to[0],'x');CHECK(getprio(1,0)==BE(5));_exit(0);}
    recvbyte(from[0],'r');CHECK(!cmp(getpid(),p) && !setprio(1,0,BE(5)));
    sendbyte(to[1],'x');joined(p);
    CHECK(!close(to[0])&&!close(to[1])&&!close(from[0])&&!close(from[1]));
    permission_checks();
    p=fork();CHECK(p>=0);
    if(!p) {
        CHECK(!setprio(1,0,BE(6)));
        char *av[]={"ioprio","exec",0};execv("/proc/self/exe",av);_exit(99);
    }
    joined(p);
    puts("IOPRIO PASS");return 0;
}
