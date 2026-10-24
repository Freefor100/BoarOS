#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
static void check(int okay,const char *name)
{if(!okay){printf("NPROC FAIL %s errno=%d\n",name,errno);_exit(99);}}
static void limit(unsigned long soft)
{struct rlimit l;check(!getrlimit(RLIMIT_NPROC,&l),"get-limit");l.rlim_cur=soft;check(!setrlimit(RLIMIT_NPROC,&l),"set-limit");}
static void wait_ok(pid_t p)
{int s;check(waitpid(p,&s,0)==p&&!s,"child-wait");}
static void send_byte(int fd)
{check(write(fd,"x",1)==1,"handshake-send");}
static void take_byte(int fd)
{char c;check(read(fd,&c,1)==1&&c=='x',"handshake-take");}
static void accept_fork(void)
{pid_t p=fork();check(p>=0,"fork-allowed");if(!p)_exit(0);wait_ok(p);}
static void reject_fork(void)
{
    errno=0;pid_t p=fork();int error=errno;
    if(!p)_exit(98);
    if(p>0){int s;waitpid(p,&s,0);}
    check(p==-1&&error==EAGAIN,"fork-limit");
}
static void drop(unsigned uid)
{check(!syscall(SYS_setresuid,uid,uid,uid),"drop-uid");}
static void quota(void)
{limit(1);drop(20041);reject_fork();}
static void caps(uint64_t mask)
{
    struct {uint32_t version;int32_t pid;} header={0x20080522,0};
    struct {uint32_t effective,permitted,inheritable;} data[2]={{(uint32_t)mask,(uint32_t)mask,0},{(uint32_t)(mask>>32),(uint32_t)(mask>>32),0}};
    check(!syscall(SYS_capset,&header,data),"capset");
}
static void root_exempt(void)
{limit(0);caps(0);accept_fork();}
static void capability_exempt(void)
{
    for(unsigned i=0;i<2;i++){
        pid_t p=fork();check(p>=0,"cap-outer-fork");
        if(!p){limit(0);check(!syscall(SYS_setresuid,20042+i,0,0),"cap-real-uid");caps(UINT64_C(1)<<(i?21:24));accept_fork();_exit(0);}
        wait_ok(p);
    }
}
static void zombie(void)
{
    limit(2);drop(20044);
    pid_t p=fork();check(p>=0,"zombie-fork");if(!p)_exit(0);
    siginfo_t info={0};check(!waitid(P_PID,p,&info,WEXITED|WNOWAIT)&&info.si_pid==p,"zombie-observe");
    reject_fork();wait_ok(p);accept_fork();
}
static int ready[2],release_thread[2];
static pid_t worker_tid;
static void *worker(void *unused)
{(void)unused;worker_tid=(pid_t)syscall(SYS_gettid);send_byte(ready[1]);take_byte(release_thread[0]);return 0;}
static void threads(void)
{
    limit(2);drop(20045);check(!pipe(ready)&&!pipe(release_thread),"thread-pipes");
    pthread_t t;check(!pthread_create(&t,0,worker,0),"thread-create");take_byte(ready[0]);
    reject_fork();send_byte(release_thread[1]);check(!pthread_join(t,0),"thread-join");
    char path[80];snprintf(path,sizeof(path),"/proc/self/task/%d/stat",worker_tid);
    unsigned tries;
    for(tries=0;tries<10000;tries++){
        int fd=open(path,O_RDONLY);if(fd<0){check(errno==ENOENT||errno==ESRCH,"thread-gone-error");break;}
        check(!close(fd),"thread-proc-close");sched_yield();
    }
    check(tries<10000,"thread-logical-release");accept_fork();
    for(unsigned i=0;i<2;i++){check(!close(ready[i]),"thread-close-ready");check(!close(release_thread[i]),"thread-close-release");}
}
static void exec_same(const char *mode)
{execl("/proc/self/exe","nproc",mode,(char *)0);check(0,"exec-returned");}
static void deferred_exec_case(int boundary)
{
    unsigned uid=boundary?20047:20046;int hr[2],hg[2],ar[2],ag[2];
    check(!pipe(hr)&&!pipe(hg)&&!pipe(ar)&&!pipe(ag),"exec-pipes");
    pid_t holder=fork();check(holder>=0,"holder-fork");
    if(!holder){drop(uid);send_byte(hr[1]);take_byte(hg[0]);_exit(0);}
    take_byte(hr[0]);pid_t actor=fork();check(actor>=0,"actor-fork");
    if(!actor){
        limit(boundary?1:0);drop(uid);
        if(boundary)exec_same("--probe");
        char *const argv[]={"nproc","--probe",0};
        errno=0;check(execv("/proc/self/exe",argv)==-1&&errno==EAGAIN,"uid-exec-deferred");
        limit(1);errno=0;check(execv("/proc/self/exe",argv)==-1&&errno==EAGAIN,"uid-exec-still-over");
        send_byte(ar[1]);take_byte(ag[0]);exec_same("--probe");_exit(98);
    }
    if(!boundary){take_byte(ar[0]);send_byte(hg[1]);wait_ok(holder);send_byte(ag[1]);wait_ok(actor);}
    else {wait_ok(actor);send_byte(hg[1]);wait_ok(holder);}
    int *pipes[]={hr,hg,ar,ag};for(unsigned i=0;i<4;i++)for(unsigned j=0;j<2;j++)check(!close(pipes[i][j]),"exec-close");
}
static void deferred_exec(void){deferred_exec_case(0);}
static void transfer_boundary(void){deferred_exec_case(1);}
static void *nonleader(void *unused)
{(void)unused;exec_same("--fork-probe");return 0;}
static void nonleader_exec(void)
{
    limit(2);drop(20048);pthread_t t;check(!pthread_create(&t,0,nonleader,0),"nonleader-create");
    for(;;)pause();
}
struct scenario {const char *name;void(*run)(void);};
int main(int argc,char **argv)
{
    setvbuf(stdout,0,_IONBF,0);
    if(argc==2&&!strcmp(argv[1],"--probe"))return 0;
    if(argc==2&&!strcmp(argv[1],"--fork-probe")){accept_fork();return 0;}
    int proc=open("/proc/self/stat",O_RDONLY);
    if(proc<0){check(errno==ENOENT,"proc-probe");check(!mkdir("/proc",0555)||errno==EEXIST,"proc-mkdir");check(!mount("proc","/proc","proc",0,0),"proc-mount");}
    else check(!close(proc),"proc-close");
    const struct scenario cases[]={{"quota",quota},{"root-exempt",root_exempt},{"capability-exempt",capability_exempt},
        {"zombie",zombie},{"threads",threads},{"deferred-exec",deferred_exec},{"transfer-boundary",transfer_boundary},{"nonleader-exec",nonleader_exec}};
    unsigned failures=0;
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++){
        pid_t p=fork();check(p>=0,"outer-fork");if(!p){cases[i].run();_exit(0);}
        int s;check(waitpid(p,&s,0)==p,"outer-wait");
        printf("NPROC CASE %s %s status=%d\n",cases[i].name,s?"FAIL":"PASS",s);if(s)failures++;
    }
    printf("NPROC END cases=%zu failures=%u\n",sizeof(cases)/sizeof(cases[0]),failures);
    if(getpid()==1){sync();reboot(RB_POWER_OFF);}
    return failures?1:0;
}
