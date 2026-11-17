#define _GNU_SOURCE
#include <errno.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
#include <sched.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"SECCOMP FAIL %d: %s errno=%d\n",__LINE__,#x,errno);_exit(1);}}while(0)
static const char *self;
static int initial_mode;
static void nnp(void){CHECK(!prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0));}
static long filter(long nr,unsigned err,unsigned flags)
{
    struct sock_filter insns[]={
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,0),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,nr,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|err),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};
    struct sock_fprog p={4,insns};
    return syscall(SYS_seccomp,SECCOMP_SET_MODE_FILTER,flags,&p);
}
static void denied(long nr,int error)
{errno=0;CHECK(syscall(nr)==-1&&errno==error);}
struct worker {int ready[2],go[2];pthread_t thread;int mode,tid,nnp_before;};
static void *worker(void *opaque)
{
    struct worker *w=opaque;w->tid=syscall(SYS_gettid);
    w->nnp_before=prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0);
    if(w->mode==1){nnp();CHECK(!filter(SYS_getppid,EBUSY,0));}
    CHECK(write(w->ready[1],"r",1)==1);
    char command;CHECK(read(w->go[0],&command,1)==1);
    if(command=='s'||command=='e'){nnp();CHECK(!filter(SYS_getpid,EACCES,SECCOMP_FILTER_FLAG_TSYNC));}
    if(command=='e'){char *args[]={(char *)self,"--exec-check",0};execv(self,args);CHECK(0);}
    if(command=='c'||command=='s') {
        denied(SYS_getpid,EACCES);
        if(w->mode==2){denied(SYS_getppid,ENOENT);denied(SYS_getuid,EPERM);}
        CHECK(prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)==1);
    }
    if(command=='u') {
        CHECK(syscall(SYS_getpid)>0);
        CHECK(prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)==w->nnp_before);
    }
    return 0;
}
static void start(struct worker *w,int mode)
{
    *w=(struct worker){.mode=mode};CHECK(!pipe(w->ready)&&!pipe(w->go));
    CHECK(!pthread_create(&w->thread,0,worker,w));char c;CHECK(read(w->ready[0],&c,1)==1);
}
static void finish(struct worker *w,char command)
{
    CHECK(write(w->go[1],&command,1)==1);CHECK(!pthread_join(w->thread,0));
    for(unsigned i=0;i<2;i++){close(w->go[i]);close(w->ready[i]);}
}
static void propagation(void)
{
    struct worker a,b;start(&a,0);start(&b,0);
    finish(&a,'s');denied(SYS_getpid,EACCES);
    CHECK(prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)==1);finish(&b,'c');
    pid_t p=fork();CHECK(p>=0);if(!p){denied(SYS_getpid,EACCES);_exit(0);}
    int status;CHECK(waitpid(p,&status,0)==p&&WIFEXITED(status)&&!WEXITSTATUS(status));
}
static void failure(void)
{
    struct worker divergent,unfiltered;start(&divergent,1);start(&unfiltered,0);
    nnp();CHECK(filter(SYS_getpid,EACCES,SECCOMP_FILTER_FLAG_TSYNC)==divergent.tid);
    CHECK(syscall(SYS_getpid)>0&&syscall(SYS_getppid)>0);finish(&unfiltered,'u');
    finish(&divergent,'x');
    CHECK(!filter(SYS_getpid,EACCES,SECCOMP_FILTER_FLAG_TSYNC));denied(SYS_getpid,EACCES);
}
static void identical_not_ancestor(void)
{
    struct worker divergent;start(&divergent,1);
    nnp();CHECK(!filter(SYS_getppid,EBUSY,0));
    CHECK(filter(SYS_getpid,EACCES,SECCOMP_FILTER_FLAG_TSYNC)==divergent.tid);
    CHECK(syscall(SYS_getpid)>0);denied(SYS_getppid,EBUSY);finish(&divergent,'x');
}
static pthread_barrier_t barrier;
static void *concurrent(void *which)
{
    nnp();int ret=pthread_barrier_wait(&barrier);CHECK(!ret||ret==PTHREAD_BARRIER_SERIAL_THREAD);
    CHECK(!filter(which?SYS_getpid:SYS_getppid,which?EACCES:EBUSY,SECCOMP_FILTER_FLAG_TSYNC));
    return 0;
}
static void parallel_install(void)
{
    pthread_t a,b;CHECK(!pthread_barrier_init(&barrier,0,3));
    CHECK(!pthread_create(&a,0,concurrent,(void *)1));CHECK(!pthread_create(&b,0,concurrent,0));
    int ret=pthread_barrier_wait(&barrier);CHECK(!ret||ret==PTHREAD_BARRIER_SERIAL_THREAD);
    CHECK(!pthread_join(a,0)&&!pthread_join(b,0));CHECK(!pthread_barrier_destroy(&barrier));
    denied(SYS_getpid,EACCES);denied(SYS_getppid,EBUSY);
}
static void ancestor_chain(void)
{
    nnp();CHECK(!filter(SYS_getppid,ENOENT,0));
    struct worker a,b;start(&a,2);start(&b,2);
    CHECK(!filter(SYS_getuid,EPERM,0));
    CHECK(!filter(SYS_getpid,EACCES,SECCOMP_FILTER_FLAG_TSYNC));
    finish(&a,'c');finish(&b,'c');denied(SYS_getuid,EPERM);
}

static void thread_exec(void)
{
    struct worker a;start(&a,0);finish(&a,'e');CHECK(0);
}
static void *strict_worker(void *opaque)
{
    struct worker *w=opaque;w->tid=syscall(SYS_gettid);
    CHECK(!syscall(SYS_seccomp,SECCOMP_SET_MODE_STRICT,0,0));
    CHECK(syscall(SYS_write,w->ready[1],"r",1)==1);
    char c;CHECK(syscall(SYS_read,w->go[0],&c,1)==1);
    syscall(SYS_exit,0);__builtin_trap();
}
static void strict_peer(void)
{
    if(initial_mode){puts("SECCOMP TSYNC STRICT SKIP inherited host filter");return;}
    struct worker w={0};CHECK(!pipe(w.ready)&&!pipe(w.go));
    CHECK(!pthread_create(&w.thread,0,strict_worker,&w));
    char c;CHECK(read(w.ready[0],&c,1)==1);nnp();
    CHECK(filter(SYS_getpid,EACCES,SECCOMP_FILTER_FLAG_TSYNC)==w.tid);
    CHECK(syscall(SYS_getpid)>0);CHECK(write(w.go[1],"x",1)==1);
    for(unsigned i=0;i<100000;i++) {
        if(syscall(SYS_tgkill,getpid(),w.tid,0)==-1&&errno==ESRCH)return;
        sched_yield();
    }
    CHECK(0);
}
static int clone_gate[2];
static void *clone_leaf(void *error)
{
    char c;CHECK(read(clone_gate[0],&c,1)==1);
    denied(SYS_getpid,(int)(unsigned long)error);CHECK(prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)==1);return 0;
}
static void *clone_racer(void *error)
{
    int r=pthread_barrier_wait(&barrier);CHECK(!r||r==PTHREAD_BARRIER_SERIAL_THREAD);
    pthread_t t;CHECK(!pthread_create(&t,0,clone_leaf,error));CHECK(!pthread_join(t,0));return 0;
}
static void clone_publication(void)
{
    CHECK(!pipe(clone_gate));nnp();
    for(unsigned i=0;i<16;i++) {
        CHECK(!pthread_barrier_init(&barrier,0,2));
        pthread_t t;CHECK(!pthread_create(&t,0,clone_racer,(void *)(unsigned long)(101+i)));
        int r=pthread_barrier_wait(&barrier);CHECK(!r||r==PTHREAD_BARRIER_SERIAL_THREAD);
        CHECK(!filter(SYS_getpid,101+i,SECCOMP_FILTER_FLAG_TSYNC));
        CHECK(write(clone_gate[1],"x",1)==1);CHECK(!pthread_join(t,0));
        CHECK(!pthread_barrier_destroy(&barrier));
    }
    close(clone_gate[0]);close(clone_gate[1]);
}
static void run(void (*test)(void),const char *name)
{
    pid_t p=fork();CHECK(p>=0);if(!p){test();_exit(0);}
    int status;CHECK(waitpid(p,&status,0)==p&&WIFEXITED(status)&&!WEXITSTATUS(status));
    printf("SECCOMP TSYNC %s PASS\n",name);
}
int main(int argc,char **argv)
{
    if(argc>1&&!strcmp(argv[1],"--exec-check")) {
        denied(SYS_getpid,EACCES);CHECK(prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)==1);return 0;
    }
    self=argv[0];initial_mode=prctl(PR_GET_SECCOMP);
    setvbuf(stdout,0,_IONBF,0);alarm(40);
    run(propagation,"NNP/thread/fork");
    run(failure,"failure-TID/all-or-none/exit");
    run(identical_not_ancestor,"identical-independent-chains");
    run(parallel_install,"concurrent-install");
    run(ancestor_chain,"ancestor-upgrade");
    run(strict_peer,"strict-peer-failure");
    run(thread_exec,"nonleader-exec");
    run(clone_publication,"clone-publication");
    puts("SECCOMP ALL PASS");if(getpid()==1)reboot(RB_POWER_OFF);return 0;
}
