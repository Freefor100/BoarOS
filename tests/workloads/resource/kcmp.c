#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pthread.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/sem.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#ifndef SYS_kcmp
#define SYS_kcmp 272
#endif
#define FILE_OBJECT 0
#define VM_OBJECT 1
#define FILES_OBJECT 2
#define FS_OBJECT 3
#define SIGHAND_OBJECT 4
#define IO_OBJECT 5
#define SEM_OBJECT 6
#define EPOLL_OBJECT 7
#define CHECK(x) do {if(!(x)){dprintf(2,"KCMP FAIL line=%d errno=%d %s\n",__LINE__,errno,#x);_exit(1);}}while(0)
#define ERROR(x,e) do {errno=0;CHECK((x)==-1&&errno==(e));}while(0)
static unsigned cases;
static int boaros;
static long compare(pid_t a,pid_t b,int type,unsigned long x,unsigned long y)
{return syscall(SYS_kcmp,a,b,type,x,y);}
static void pass(const char *name){printf("KCMP CASE %s PASS\n",name);cases++;}
static void token(int fd,char c){CHECK(write(fd,&c,1)==1);}
static void expect(int fd,char c){char b;CHECK(read(fd,&b,1)==1&&b==c);}
static void joined(pid_t pid){int s;CHECK(waitpid(pid,&s,0)==pid&&WIFEXITED(s)&&!WEXITSTATUS(s));}
static long different(pid_t a,pid_t b,int type,unsigned long x,unsigned long y)
{
    long r=compare(a,b,type,x,y);CHECK(r==1||r==2);
    CHECK(compare(b,a,type,y,x)==3-r);
    CHECK(compare(a,b,type,x,y)==r);return r;
}
static void self_abi(int fd)
{
    pid_t me=getpid();
    for(int type=VM_OBJECT;type<=SEM_OBJECT;type++)if(type!=IO_OBJECT)
        CHECK(!compare(me,me,type,UINT64_MAX,UINT64_MAX));
    ERROR(compare(0,me,999,0,0),ESRCH);ERROR(compare(me,-1,FILE_OBJECT,-1UL,-1UL),ESRCH);
    ERROR(compare(me,me,999,0,0),EINVAL);ERROR(compare(me,me,-1,0,0),EINVAL);
    ERROR(compare(me,me,FILE_OBJECT,UINT32_MAX,fd),EBADF);
    ERROR(compare(me,me,FILE_OBJECT,fd,UINT32_MAX),EBADF);
    CHECK(!compare(me,me,FILE_OBJECT,((unsigned long)1<<32)|(unsigned)fd,fd));
    CHECK(!compare(me,me,FILE_OBJECT,fd,((unsigned long)1<<32)|(unsigned)fd));
    if(boaros){ERROR(compare(me,me,IO_OBJECT,0,0),EOPNOTSUPP);ERROR(compare(me,me,EPOLL_OBJECT,0,UINT64_MAX),EOPNOTSUPP);}
    pass("abi-lookup-type-fd-width");
}
static void ofd(int fd)
{
    int copy=dup(fd),again=open("/kcmp-data",O_RDWR),third=open("/kcmp-data",O_RDWR);
    CHECK(copy>=0&&again>=0&&third>=0&&!compare(getpid(),getpid(),FILE_OBJECT,fd,copy));
    CHECK(lseek(copy,3,SEEK_SET)==3&&lseek(fd,0,SEEK_CUR)==3);
    CHECK(lseek(again,0,SEEK_CUR)==0);
    long ab=different(getpid(),getpid(),FILE_OBJECT,fd,again);
    long bc=different(getpid(),getpid(),FILE_OBJECT,again,third);
    long ac=different(getpid(),getpid(),FILE_OBJECT,fd,third);
    if(ab==bc)CHECK(ac==ab);
    CHECK(!close(copy)&&!close(again)&&!close(third));pass("real-ofd-dup-offset-stable-order");
}
static void fork_lazy(int fd)
{
    int to[2],from[2];CHECK(!pipe(to)&&!pipe(from));pid_t c=fork();CHECK(c>=0);
    if(!c){CHECK(!close(to[1])&&!close(from[0]));token(from[1],'r');expect(to[0],'s');
        struct sigaction action={.sa_handler=SIG_IGN};CHECK(!sigemptyset(&action.sa_mask)&&!sigaction(SIGUSR1,&action,0));
        token(from[1],'s');expect(to[0],'x');_exit(0);}
    CHECK(!close(to[0])&&!close(from[1]));expect(from[0],'r');
    CHECK(!compare(getpid(),c,FILE_OBJECT,fd,fd));
    for(int type=VM_OBJECT;type<=SIGHAND_OBJECT;type++)different(getpid(),c,type,0,0);
    CHECK(!compare(getpid(),c,SEM_OBJECT,0,0));
    long before=different(getpid(),c,SIGHAND_OBJECT,0,0);token(to[1],'s');expect(from[0],'s');
    CHECK(compare(getpid(),c,SIGHAND_OBJECT,0,0)==before);
    struct sigaction action={.sa_handler=SIG_IGN};CHECK(!sigemptyset(&action.sa_mask)&&!sigaction(SIGUSR2,&action,0));
    CHECK(compare(getpid(),c,SIGHAND_OBJECT,0,0)==before);
    token(to[1],'x');joined(c);CHECK(!close(to[1])&&!close(from[0]));pass("fork-empty-sighand-lazy-identity");
}
struct vfork_query_arguments {pid_t parent;int fd;};
static int vfork_compare(void *argument)
{
    const struct vfork_query_arguments *query=argument;
    pid_t me=(pid_t)syscall(SYS_getpid);int failed=0;
    failed|=compare(query->parent,me,VM_OBJECT,0,0)!=0;
    failed|=compare(query->parent,me,FILE_OBJECT,query->fd,query->fd)!=0;
    for(int type=FILES_OBJECT;type<=SIGHAND_OBJECT;type++){
        long r=compare(query->parent,me,type,0,0);failed|=r!=1&&r!=2;}
    return failed;
}
static void vfork_query(int fd)
{
    char *stack=malloc(65536);CHECK(stack);
    struct vfork_query_arguments query={getpid(),fd};
    /* Request the kernel sharing flags explicitly: musl ports without a vfork
     * assembly entry use fork semantics. C comparisons need a separate stack. */
    pid_t c=clone(vfork_compare,stack+65536,CLONE_VM|CLONE_VFORK|SIGCHLD,&query);
    CHECK(c>=0);joined(c);free(stack);pass("vfork-shared-vm-independent-tables");
}
struct child_gate {int ready,go;};
static int clone_wait(void *argument)
{struct child_gate *g=argument;token(g->ready,'r');expect(g->go,'x');return 0;}
static void shared_fs(void)
{
    int to[2],from[2];CHECK(!pipe(to)&&!pipe(from));char *stack=malloc(65536);CHECK(stack);
    struct child_gate gate={from[1],to[0]};pid_t c=clone(clone_wait,stack+65536,CLONE_FS|SIGCHLD,&gate);CHECK(c>=0);
    expect(from[0],'r');CHECK(!compare(getpid(),c,FS_OBJECT,0,0));different(getpid(),c,FILES_OBJECT,0,0);
    token(to[1],'x');joined(c);free(stack);
    CHECK(!close(to[0])&&!close(to[1])&&!close(from[0])&&!close(from[1]));pass("clone-fs-shared-object");
}
struct thread_gate {int ready,go;};
static void *thread_wait(void *argument)
{struct thread_gate *g=argument;pid_t tid=(pid_t)syscall(SYS_gettid);CHECK(write(g->ready,&tid,sizeof(tid))==sizeof(tid));expect(g->go,'x');return 0;}
static void thread_objects(int fd)
{
    int to[2],from[2];CHECK(!pipe(to)&&!pipe(from));struct thread_gate gate={from[1],to[0]};pthread_t thread;
    CHECK(!pthread_create(&thread,0,thread_wait,&gate));pid_t tid;CHECK(read(from[0],&tid,sizeof(tid))==sizeof(tid));
    CHECK(!compare(getpid(),tid,FILE_OBJECT,fd,fd));
    for(int type=VM_OBJECT;type<=SEM_OBJECT;type++)if(type!=IO_OBJECT)CHECK(!compare(getpid(),tid,type,0,0));
    int observer=(int)syscall(434,tid,0200U);CHECK(observer>=0);
    token(to[1],'x');CHECK(!pthread_join(thread,0));
    /* join observes CHILD_CLEARTID before Linux necessarily removes the TID. */
    struct pollfd departed={observer,POLLHUP,0};
    CHECK(poll(&departed,1,5000)==1&&(departed.revents&POLLHUP));
    ERROR(compare(getpid(),tid,VM_OBJECT,0,0),ESRCH);CHECK(!close(observer));
    CHECK(!close(to[0])&&!close(to[1])&&!close(from[0])&&!close(from[1]));pass("pthread-shared-resources-tid-retirement");
}
static void semaphore_undo(void)
{
    int id=semget(IPC_PRIVATE,1,0600|IPC_CREAT);CHECK(id>=0&&semctl(id,0,SETVAL,0)==0);
    int to[2],from[2],gone[2];CHECK(!pipe(to)&&!pipe(from)&&!pipe(gone));pid_t owner=fork();CHECK(owner>=0);
    if(!owner){CHECK(!close(gone[0]));struct sembuf op={0,1,SEM_UNDO};CHECK(!semop(id,&op,1));
        int child_ready[2];CHECK(!pipe(child_ready));char *stack=malloc(65536);CHECK(stack);struct child_gate gate={child_ready[1],to[0]};
        pid_t c=clone(clone_wait,stack+65536,CLONE_SYSVSEM|SIGCHLD,&gate);CHECK(c>=0);
        expect(child_ready[0],'r');CHECK(!compare(getpid(),c,SEM_OBJECT,0,0));CHECK(write(from[1],&c,sizeof(c))==sizeof(c));
        CHECK(!close(gone[1]));_exit(0);}
    CHECK(!close(gone[1]));pid_t sharer;CHECK(read(from[0],&sharer,sizeof(sharer))==sizeof(sharer));joined(owner);
    CHECK(semctl(id,0,GETVAL)==1);different(getpid(),sharer,SEM_OBJECT,0,0);
    token(to[1],'x');char byte;CHECK(read(gone[0],&byte,1)==0);CHECK(semctl(id,0,GETVAL)==0);if(getpid()==1)joined(sharer);
    CHECK(!semctl(id,0,IPC_RMID)&&!close(to[0])&&!close(to[1])&&!close(from[0])&&!close(from[1])&&!close(gone[0]));
    pass("sysvsem-empty-shared-last-owner-undo");
}
static void zombie_resources(int fd)
{
    int gate[2];CHECK(!pipe(gate));
    pid_t a=fork();CHECK(a>=0);if(!a){expect(gate[0],'x');_exit(0);}
    pid_t b=fork();CHECK(b>=0);if(!b){expect(gate[0],'x');_exit(0);}
    long sighand=different(a,b,SIGHAND_OBJECT,0,0);token(gate[1],'x');token(gate[1],'x');
    siginfo_t info;CHECK(!waitid(P_PID,a,&info,WEXITED|WNOWAIT)&&!waitid(P_PID,b,&info,WEXITED|WNOWAIT));
    for(int type=VM_OBJECT;type<=SEM_OBJECT;type++)if(type!=IO_OBJECT&&type!=SIGHAND_OBJECT)CHECK(!compare(a,b,type,0,0));
    CHECK(compare(a,b,SIGHAND_OBJECT,0,0)==sighand);
    ERROR(compare(a,b,FILE_OBJECT,fd,fd),EBADF);joined(a);ERROR(compare(a,b,999,0,0),ESRCH);joined(b);CHECK(!close(gate[0])&&!close(gate[1]));
    pass("zombie-null-resources-reaped-esrch");
}
static void permissions(int fd)
{
    int to[2],from[2];CHECK(!pipe(to)&&!pipe(from));pid_t target=fork();CHECK(target>=0);
    if(!target){CHECK(!setgroups(0,0)&&!setgid(1002)&&!setuid(1002));CHECK(!prctl(PR_SET_DUMPABLE,1));token(from[1],'r');expect(to[0],'x');_exit(0);}
    expect(from[0],'r');pid_t checker=fork();CHECK(checker>=0);
    if(!checker){CHECK(!setgroups(0,0)&&!setgid(1001)&&!setuid(1001));
        ERROR(compare(target,getpid(),999,fd,fd),EPERM);ERROR(compare(getpid(),target,FILE_OBJECT,UINT32_MAX,UINT32_MAX),EPERM);
        ERROR(compare(-1,target,999,0,0),ESRCH);_exit(0);}
    joined(checker);token(to[1],'x');joined(target);
    target=fork();CHECK(target>=0);
    if(!target){CHECK(!setgroups(0,0)&&!setgid(1001)&&!setuid(1001));CHECK(!prctl(PR_SET_DUMPABLE,0));token(from[1],'r');expect(to[0],'x');_exit(0);}
    expect(from[0],'r');checker=fork();CHECK(checker>=0);
    if(!checker){CHECK(!setgroups(0,0)&&!setgid(1001)&&!setuid(1001));ERROR(compare(getpid(),target,VM_OBJECT,0,0),EPERM);_exit(0);}
    joined(checker);token(to[1],'x');joined(target);
    target=fork();CHECK(target>=0);
    if(!target){CHECK(!setgroups(0,0)&&!setgid(1001)&&!setuid(1001));CHECK(!prctl(PR_SET_DUMPABLE,1));token(from[1],'r');expect(to[0],'x');_exit(0);}
    expect(from[0],'r');checker=fork();CHECK(checker>=0);
    if(!checker){
        CHECK(!setgroups(0,0)&&!setgid(1001)&&!setresuid(1001,0,0));
        CHECK(syscall(SYS_setfsuid,1002)==0&&syscall(SYS_setfsuid,UINT32_MAX)==1002);
        struct {uint32_t version;int32_t pid;} header={0x20080522,0};
        struct {uint32_t effective,permitted,inheritable;} data[2];
        CHECK(!syscall(SYS_capget,&header,data));data[0].effective=data[1].effective=0;
        CHECK(!syscall(SYS_capset,&header,data));different(getpid(),target,VM_OBJECT,0,0);_exit(0);
    }
    joined(checker);token(to[1],'x');joined(target);
    CHECK(!close(to[0])&&!close(to[1])&&!close(from[0])&&!close(from[1]));pass("both-target-realcreds-before-type-fd");
}
struct exec_gate {int ready,go;};
static void exec_image(int ready,int go)
{
    char a[24],b[24];snprintf(a,sizeof(a),"%d",ready);snprintf(b,sizeof(b),"%d",go);
    char *args[]={"/init","--exec",a,b,0};execv(args[0],args);_exit(2);
}
static void *exec_thread(void *argument)
{
    struct exec_gate *g=argument;pid_t tid=(pid_t)syscall(SYS_gettid);
    CHECK(write(g->ready,&tid,sizeof(tid))==sizeof(tid));expect(g->go,'e');exec_image(g->ready,g->go);return 0;
}
static void nonleader_exec(int fd)
{
    int to[2],from[2],clo=fcntl(fd,F_DUPFD_CLOEXEC,32);CHECK(clo>=32&&!pipe(to)&&!pipe(from));pid_t c=fork();CHECK(c>=0);
    if(!c){struct exec_gate gate={from[1],to[0]};pthread_t thread;CHECK(!pthread_create(&thread,0,exec_thread,&gate));CHECK(!pthread_join(thread,0));_exit(3);}
    pid_t oldtid;CHECK(read(from[0],&oldtid,sizeof(oldtid))==sizeof(oldtid));CHECK(!compare(c,oldtid,VM_OBJECT,0,0));
    token(to[1],'e');expect(from[0],'e');ERROR(compare(getpid(),oldtid,VM_OBJECT,0,0),ESRCH);
    CHECK(!compare(getpid(),c,FILE_OBJECT,fd,fd));ERROR(compare(getpid(),c,FILE_OBJECT,clo,clo),EBADF);
    different(getpid(),c,VM_OBJECT,0,0);token(to[1],'x');joined(c);
    CHECK(!close(clo)&&!close(to[0])&&!close(to[1])&&!close(from[0])&&!close(from[1]));pass("nonleader-exec-identity-ofd-cloexec");
}
int main(int argc,char **argv)
{
    setvbuf(stdout,0,_IONBF,0);
    if(argc==4&&!strcmp(argv[1],"--exec")){token(atoi(argv[2]),'e');expect(atoi(argv[3]),'x');return 0;}
    if(getpid()==1){CHECK(!mkdir("/proc",0555)||errno==EEXIST);CHECK(!mount("proc","/proc","proc",0,0));}
    struct utsname name;CHECK(!uname(&name));boaros=!strcmp(name.release,"0.1.0-boaros-dev");
    int fd=open("/kcmp-data",O_CREAT|O_RDWR|O_TRUNC,0600);CHECK(fd>=0&&write(fd,"same-ofd",8)==8);
    self_abi(fd);ofd(fd);fork_lazy(fd);vfork_query(fd);shared_fs();thread_objects(fd);semaphore_undo();permissions(fd);nonleader_exec(fd);zombie_resources(fd);
    CHECK(!close(fd)&&!unlink("/kcmp-data"));printf("KCMP END %u\n",cases);
    if(getpid()==1){sync();reboot(RB_POWER_OFF);}return 0;
}
