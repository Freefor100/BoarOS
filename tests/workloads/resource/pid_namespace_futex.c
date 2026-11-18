#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){dprintf(2,"PIDNS PI FAIL %d %s errno=%d\n",__LINE__,#x,errno);_exit(1);}}while(0)
static struct shared {uint32_t source,target;pid_t outer;} *data;
static int start[2],ready[2];
static long fx(uint32_t *word,int op,uint32_t value,unsigned long arg,uint32_t *other,uint32_t expected)
{return syscall(SYS_futex,word,op,value,arg,other,expected);}
static void byte(int fd){CHECK(write(fd,"x",1)==1);}
static void receive(int fd){char c;CHECK(read(fd,&c,1)==1&&c=='x');}
static int child(void *mode)
{
    CHECK(getpid()==1);receive(start[0]);
    if(!mode) {
        CHECK(data->target==1);
        CHECK(fx(&data->target,FUTEX_TRYLOCK_PI,0,0,NULL,0)==-1&&errno==EDEADLK);
    } else if(mode==(void *)2) {
        uint32_t saved=__atomic_load_n(&data->target,__ATOMIC_SEQ_CST);
        __atomic_store_n(&data->target,FUTEX_WAITERS,__ATOMIC_SEQ_CST);
        long result=fx(&data->target,FUTEX_TRYLOCK_PI,0,0,NULL,0);int error=errno;
        __atomic_store_n(&data->target,saved,__ATOMIC_SEQ_CST);
        CHECK(result==-1&&error==EAGAIN);
    } else {
        byte(ready[1]);
        CHECK(!fx(&data->source,FUTEX_WAIT_REQUEUE_PI,0,0,&data->target,0));
        CHECK((data->target&FUTEX_TID_MASK)==(uint32_t)data->outer);
        CHECK(fx(&data->target,FUTEX_UNLOCK_PI,0,0,NULL,0)==-1&&errno==EPERM);
    }
    return 0;
}
static pid_t spawn(void *mode)
{
    void *stack=malloc(1U<<20);CHECK(stack);
    pid_t pid=clone(child,(char *)stack+(1U<<20),CLONE_NEWPID|SIGCHLD,mode);
    CHECK(pid>0);data->outer=pid;if(mode!=(void *)2)byte(start[1]);free(stack);return pid;
}
static void joined(pid_t pid){int s;CHECK(waitpid(pid,&s,0)==pid&&WIFEXITED(s)&&!WEXITSTATUS(s));}
static void *outer_waiter(void *unused)
{
    (void)unused;
    CHECK(!fx(&data->target,FUTEX_LOCK_PI,0,0,NULL,0));
    CHECK(!fx(&data->target,FUTEX_UNLOCK_PI,0,0,NULL,0));return NULL;
}

int main(void)
{
    /* Run as namespace PID 1, e.g. unshare -Urpf -- ./pid-namespace-futex. */
    CHECK(getpid()==1);
    data=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);CHECK(data!=MAP_FAILED);
    CHECK(!pipe(start)&&!pipe(ready));
    CHECK(!fx(&data->target,FUTEX_LOCK_PI,0,0,NULL,0)&&data->target==1);
    joined(spawn(NULL));CHECK(!fx(&data->target,FUTEX_UNLOCK_PI,0,0,NULL,0));
    puts("PIDNS PI PASS cross-namespace same-number collision is EDEADLK");
    pid_t pid=spawn((void *)1);receive(ready[0]);
    for(;;) {
        long moved=fx(&data->source,FUTEX_CMP_REQUEUE_PI,1,0,&data->target,0);
        CHECK(moved==0||moved==1);if(moved)break;sched_yield();
    }
    joined(pid);
    CHECK((data->target&FUTEX_TID_MASK)==(uint32_t)pid);
    puts("PIDNS PI PASS requeue writes requeuer-view TID; local unlock is EPERM");
    data->target=0;
    pid=spawn((void *)2);
    CHECK(!fx(&data->target,FUTEX_LOCK_PI,0,0,NULL,0));
    pthread_t waiter;CHECK(!pthread_create(&waiter,NULL,outer_waiter,NULL));
    while(!(__atomic_load_n(&data->target,__ATOMIC_SEQ_CST)&FUTEX_WAITERS))sched_yield();
    byte(start[1]);joined(pid);
    CHECK(!fx(&data->target,FUTEX_UNLOCK_PI,0,0,NULL,0));
    CHECK(!pthread_join(waiter,NULL));
    puts("PIDNS PI PASS existing invisible owner and zero TID without OWNER_DIED is EAGAIN");
    CHECK(!munmap(data,4096));
    return 0;
}
