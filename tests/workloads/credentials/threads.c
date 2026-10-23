#define _GNU_SOURCE
#include <errno.h>
#include <grp.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned cpus;
static const char *self;
static void check(int okay,const char *name)
{if(!okay){printf("CREDENTIAL FAIL %s errno=%d\n",name,errno);fflush(stdout);_exit(99);}}
static void pin(unsigned cpu)
{cpu_set_t set;CPU_ZERO(&set);CPU_SET(cpu%cpus,&set);check(!sched_setaffinity(0,sizeof(set),&set),"affinity");}
struct worker {unsigned cpu;int raw;uid_t euid;gid_t egid;int count;gid_t groups[3];};
static pthread_barrier_t barrier;
static int release;
static void wait_release(void)
{
    while(!__atomic_load_n(&release,__ATOMIC_ACQUIRE)){
        long r=syscall(SYS_futex,&release,128,0,0,0,0);
        check(!r||errno==EAGAIN||errno==EINTR,"wait");
    }
}
static void *worker(void *opaque)
{
    struct worker *w=opaque;pin(w->cpu);
    if(w->raw)check(!syscall(SYS_setresuid,1000+w->cpu,1000+w->cpu,1000+w->cpu),"raw-setresuid");
    pthread_barrier_wait(&barrier);wait_release();
    w->euid=(uid_t)syscall(SYS_geteuid);w->egid=(gid_t)syscall(SYS_getegid);
    w->count=(int)syscall(SYS_getgroups,3,w->groups);
    return 0;
}
static void threads(int raw)
{
    pthread_t ids[3];struct worker w[3]={{0}};
    check(!pthread_barrier_init(&barrier,0,4),"barrier-init");
    for(unsigned i=0;i<3;i++){w[i].cpu=i+1;w[i].raw=raw;check(!pthread_create(&ids[i],0,worker,&w[i]),"pthread-create");}
    pin(0);pthread_barrier_wait(&barrier);
    if(raw)check(!syscall(SYS_geteuid),"raw-parent-unchanged");
    else{
        gid_t groups[3]={3002,3000,3001};
        check(!setgroups(3,groups),"libc-setgroups");
        check(!setresgid(2000,2001,2002),"libc-setresgid");
        check(!setresuid(1000,1001,1002),"libc-setresuid");
        check(!setresuid((uid_t)-1,1000,(uid_t)-1),"libc-setresuid-second");
    }
    __atomic_store_n(&release,1,__ATOMIC_RELEASE);check(syscall(SYS_futex,&release,129,INT32_MAX,0,0,0)>=0,"wake");
    for(unsigned i=0;i<3;i++){
        check(!pthread_join(ids[i],0),"pthread-join");
        check(w[i].euid==(raw?1000+i+1:1000),"thread-euid");
        check(w[i].egid==(raw?0:2001),"thread-egid");
        check(w[i].count==(raw?0:3),"thread-group-count");
        if(!raw)check(w[i].groups[0]==3000&&w[i].groups[1]==3001&&w[i].groups[2]==3002,"thread-groups");
    }
    check(!pthread_barrier_destroy(&barrier),"barrier-destroy");
}
static void fork_exec(void)
{
    check(!setresgid(2000,2000,2000),"exec-gid");
    check(!setresuid(1000,1000,1000),"exec-uid");
    check(!prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0),"exec-nnp");
    char *const argv[]={(char *)self,"--exec-probe",0};
    execv(self,argv);check(0,"exec");
}
static int departed_tid;
static _Thread_local volatile sig_atomic_t self_seen,self_bad;
static void self_handler(int sig,siginfo_t *info,void *context)
{
    (void)context;
    if(sig!=SIGUSR1 || info->si_code!=SI_TKILL || info->si_uid!=1000)self_bad=1;
    self_seen++;
}
static void *after_leader(void *unused)
{
    (void)unused;pin(cpus-1);
    for(;;) {
        int value=__atomic_load_n(&departed_tid,__ATOMIC_ACQUIRE);
        if(!value)break;
        long result=syscall(SYS_futex,&departed_tid,0,value,0,0,0);
        check(!result || errno==EAGAIN || errno==EINTR,"leader-clear-wait");
    }
    check(!syscall(SYS_setresuid,1000,1000,1000),"self-signal-drop-uid");
    pid_t group=getpid(),tid=(pid_t)syscall(SYS_gettid);
    for(unsigned i=0;i<32;i++) {
        check(!syscall(SYS_tgkill,group,tid,SIGUSR1),"self-signal-after-leader");
        check((unsigned)self_seen==i+1 && !self_bad,"self-signal-info");
    }
    _exit(0);
}
static void leader_exit(void)
{
    struct sigaction action={.sa_sigaction=self_handler,.sa_flags=SA_SIGINFO};
    sigemptyset(&action.sa_mask);check(!sigaction(SIGUSR1,&action,0),"self-signal-action");
    departed_tid=(int)syscall(SYS_gettid);
    check(syscall(SYS_set_tid_address,&departed_tid)==departed_tid,"leader-clear-register");
    pthread_t thread;check(!pthread_create(&thread,0,after_leader,0),"leader-child-thread");
    /* The worker observes the actual exit's clear_child_tid, not a ready flag. */
    syscall(SYS_exit,0);check(0,"leader-exit-returned");
}
static void run(const char *name,int mode)
{
    fflush(stdout);pid_t pid=fork();check(pid>=0,"fork");
    if(!pid){if(mode<2)threads(mode==0);else if(mode==2)fork_exec();else leader_exit();_exit(0);}
    int status=0;check(waitpid(pid,&status,0)==pid,"waitpid");
    check(WIFEXITED(status)&&WEXITSTATUS(status)==0,name);
    printf("CREDENTIAL CASE %s PASS\n",name);fflush(stdout);
}
int main(int argc,char **argv)
{
    self=argv[0];
    if(argc==2&&!strcmp(argv[1],"--exec-probe")){
        uid_t r,e,s;gid_t gr,ge,gs;
        check(!getresuid(&r,&e,&s)&&r==1000&&e==1000&&s==1000,"exec-uid-inherit");
        check(!getresgid(&gr,&ge,&gs)&&gr==2000&&ge==2000&&gs==2000,"exec-gid-inherit");
        check(prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)==1,"exec-nnp-inherit");return 0;
    }
    if(argc>1)cpus=(unsigned)strtoul(argv[1],0,0);
    else{cpu_set_t available;CPU_ZERO(&available);check(!sched_getaffinity(0,sizeof(available),&available),"get-affinity");
        for(unsigned i=0;i<4;i++)if(CPU_ISSET(i,&available))cpus=i+1;}
    check(cpus>=1&&cpus<=4,"cpu-count");
    printf("CREDENTIAL online=%u\n",cpus);fflush(stdout);
    run("raw-thread-local",0);run("libc-all-threads",1);run("fork-exec-inherit",2);run("self-signal-after-leader-exit",3);
    puts("CREDENTIAL END 4");return 0;
}
