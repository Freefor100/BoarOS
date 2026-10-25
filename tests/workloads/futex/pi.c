#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { dprintf(2,"PI FAIL line=%d errno=%d: %s\n",__LINE__,errno,#c);exit(1); } } while (0)
#define ERROR(c,e) do { errno=0;CHECK((c)==-1 && errno==(e)); } while (0)
#define PRIVATE 128
#define CLOCKRT 256
#define OWNER_DIED UINT32_C(0x40000000)
#define WAITERS UINT32_C(0x80000000)
static long pi(uint32_t *word,int op,const struct timespec *timeout)
{return syscall(SYS_futex,word,op,0,timeout,0,0);}
static void raw_owner(void)
{
    uint32_t word=0,tid=(uint32_t)syscall(SYS_gettid);
    CHECK(!pi(&word,6|PRIVATE,0) && word==tid);
    ERROR(pi(&word,6|PRIVATE,0),EDEADLK);
    ERROR(pi(&word,8|PRIVATE,0),EDEADLK);
    CHECK(!pi(&word,7|PRIVATE,0) && !word);
    ERROR(pi(&word,7|PRIVATE,0),EPERM);
    CHECK(!pi(&word,8|PRIVATE,(void *)1) && word==tid);
    CHECK(!pi(&word,7|PRIVATE,(void *)1) && !word);
    word=OWNER_DIED;
    CHECK(!pi(&word,13|PRIVATE,0) && word==(OWNER_DIED|tid));
    CHECK(!pi(&word,7|PRIVATE,0) && !word);
    word=0x3fffffffU;
    ERROR(pi(&word,6|PRIVATE,0),ESRCH);
    CHECK(word==(0x3fffffffU|WAITERS));
    puts("PI CASE raw-owner-and-trylock PASS");
}
static void raw_errors(void)
{
    uint32_t word=0;
    struct timespec invalid={.tv_nsec=1000000000};
    ERROR(pi(&word,6|PRIVATE|CLOCKRT,0),ENOSYS);
    ERROR(pi(&word,8|PRIVATE|CLOCKRT,0),ENOSYS);
    ERROR(pi(&word,6|PRIVATE|CLOCKRT,&invalid),EINVAL);
    ERROR(pi((void *)1,6|PRIVATE,(void *)1),EFAULT);
    ERROR(pi((void *)1,6|PRIVATE,0),EINVAL);
    ERROR(pi((void *)1,7|PRIVATE,0),EFAULT);
    ERROR(pi((void *)0,6|PRIVATE,0),EFAULT);
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    uint32_t *p=mmap(0,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(p!=MAP_FAILED);*p=0;CHECK(!mprotect(p,page,PROT_READ));
    ERROR(pi(p,6|PRIVATE,0),EFAULT);CHECK(!munmap(p,page));
    struct timespec past={0};
    CHECK(!pi(&word,6|PRIVATE,&past));CHECK(!pi(&word,7|PRIVATE,0));
    CHECK(!pi(&word,13|PRIVATE|CLOCKRT,&past));CHECK(!pi(&word,7|PRIVATE,0));
    puts("PI CASE raw-clock-errors PASS");
}

static uint64_t stamp(void)
{struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static struct timespec after(clockid_t clock,uint64_t ns)
{struct timespec t;CHECK(!clock_gettime(clock,&t));uint64_t n=(uint64_t)t.tv_nsec+ns;t.tv_sec+=(time_t)(n/1000000000);t.tv_nsec=(long)(n%1000000000);return t;}
static void byte_send(int fd){CHECK(write(fd,"x",1)==1);}
static void byte_read(int fd){char c;CHECK(read(fd,&c,1)==1);}
static void wait_ok(pid_t child)
{int st;CHECK(waitpid(child,&st,0)==child && WIFEXITED(st) && !WEXITSTATUS(st));}
static long task_field(pid_t tid,unsigned field,char *state)
{
    char path[80],buf[1024];snprintf(path,sizeof(path),"/proc/self/task/%d/stat",tid);
    int fd=open(path,O_RDONLY);CHECK(fd>=0);ssize_t n=read(fd,buf,sizeof(buf)-1);CHECK(n>0&&!close(fd));buf[n]=0;
    char *p=strrchr(buf,')');CHECK(p&&p[1]==' ');*state=p[2];p+=4;
    for(unsigned f=4;f<field;f++){p=strchr(p,' ');CHECK(p);p++;}
    return strtol(p,0,10);
}
static void blocked(pid_t tid)
{
    uint64_t end=stamp()+2000000000;char state;
    do {task_field(tid,18,&state);if(state=='S')return;CHECK(stamp()<end);sched_yield();}while(1);
}
static void policy(int kind,int priority)
{struct sched_param p={.sched_priority=priority};CHECK(!syscall(SYS_sched_setscheduler,0,kind,&p));}
static void affinity(unsigned cpu)
{uint64_t mask=UINT64_C(1)<<cpu;CHECK(!syscall(SYS_sched_setaffinity,0,sizeof(mask),&mask));}
struct contender {uint32_t *word;int op,ready[2],result,error,priority,cpu_plus1;unsigned *counter,order;pid_t tid;struct timespec deadline;};
static void *contend(void *opaque)
{
    struct contender *c=opaque;if(c->cpu_plus1)affinity((unsigned)c->cpu_plus1-1);c->tid=(pid_t)syscall(SYS_gettid);
    if(c->priority)policy(SCHED_FIFO,c->priority);
    byte_send(c->ready[1]);errno=0;c->result=(int)pi(c->word,c->op,(c->deadline.tv_sec||c->deadline.tv_nsec)?&c->deadline:0);c->error=errno;
    if(!c->result){if(c->counter)c->order=__atomic_fetch_add(c->counter,1,__ATOMIC_SEQ_CST);CHECK(!pi(c->word,7|(c->op&PRIVATE),0));}
    if(c->priority)policy(SCHED_OTHER,0);
    return 0;
}
static pthread_t start(struct contender *c)
{pthread_t t;CHECK(!pipe(c->ready)&&!pthread_create(&t,0,contend,c));byte_read(c->ready[0]);return t;}
static void joined(pthread_t t,struct contender *c)
{CHECK(!pthread_join(t,0)&&!close(c->ready[0])&&!close(c->ready[1]));}
static volatile sig_atomic_t handled;
static void handler(int sig){CHECK(sig==SIGUSR1);handled++;}
static void contention(void)
{
    uint32_t word=0;CHECK(!pi(&word,6|PRIVATE,0));
    struct contender c={.word=&word,.op=8|PRIVATE};pthread_t t=start(&c);joined(t,&c);CHECK(c.result==-1&&c.error==EAGAIN);
    c=(struct contender){.word=&word,.op=7|PRIVATE};t=start(&c);joined(t,&c);CHECK(c.result==-1&&c.error==EPERM);
    c=(struct contender){.word=&word,.op=6|PRIVATE,.priority=30};t=start(&c);blocked(c.tid);
    char state;CHECK(task_field((pid_t)syscall(SYS_gettid),18,&state)==-31);
    struct sched_param base;CHECK(!syscall(SYS_sched_getparam,0,&base)&&!base.sched_priority);
    CHECK(!pi(&word,7|PRIVATE,0));joined(t,&c);CHECK(!c.result&&!word);
    CHECK(task_field((pid_t)syscall(SYS_gettid),18,&state)>=0);
    puts("PI CASE contention-donation-revoke PASS");
}
static void timeouts_and_signal(void)
{
    uint32_t word=0;CHECK(!pi(&word,6|PRIVATE,0));
    for(unsigned i=0;i<2;i++){
        struct contender c={.word=&word,.op=(i?6:13)|PRIVATE,.deadline=after(i?CLOCK_REALTIME:CLOCK_MONOTONIC,30000000)};
        pthread_t t=start(&c);joined(t,&c);CHECK(c.result==-1&&c.error==ETIMEDOUT);
        CHECK((word&~WAITERS)==(uint32_t)syscall(SYS_gettid));
    }
    struct sigaction sa={.sa_handler=handler};CHECK(!sigaction(SIGUSR1,&sa,0));
    struct contender c={.word=&word,.op=13|PRIVATE,.deadline=after(CLOCK_MONOTONIC,2000000000)};
    pthread_t t=start(&c);blocked(c.tid);CHECK(!syscall(SYS_tgkill,getpid(),c.tid,SIGUSR1));
    uint64_t end=stamp()+1000000000;while(!handled){CHECK(stamp()<end);sched_yield();}
    blocked(c.tid);CHECK(!pi(&word,7|PRIVATE,0));joined(t,&c);CHECK(!c.result&&handled==1&&!word);
    puts("PI CASE timeout-and-noninterruptible-restart PASS");
}
static void shared_fork(void)
{
    uint32_t *word=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);CHECK(word!=MAP_FAILED);
    int ready[2];CHECK(!pipe(ready)&&!pi(word,6,0));pid_t child=fork();CHECK(child>=0);
    if(!child){byte_send(ready[1]);CHECK(!pi(word,13,0));CHECK((*word&~WAITERS)==(uint32_t)syscall(SYS_gettid));CHECK(!pi(word,7,0));_exit(0);}
    byte_read(ready[0]);uint64_t end=stamp()+2000000000;while(!(__atomic_load_n(word,__ATOMIC_ACQUIRE)&WAITERS)){CHECK(stamp()<end);sched_yield();}
    CHECK(!pi(word,7,0));wait_ok(child);CHECK(!*word&&!munmap(word,4096)&&!close(ready[0])&&!close(ready[1]));
    puts("PI CASE shared-fork-handoff PASS");
}
struct robust_case {pthread_mutex_t mutex;int ready[2],release[2];};
static void *robust_owner(void *p)
{struct robust_case *c=p;CHECK(!pthread_mutex_lock(&c->mutex));byte_send(c->ready[1]);byte_read(c->release[0]);return 0;}
static void libc_robust(void)
{
    for(unsigned recover=0;recover<2;recover++){
        struct robust_case c;pthread_mutexattr_t a;CHECK(!pthread_mutexattr_init(&a));CHECK(!pthread_mutexattr_setprotocol(&a,PTHREAD_PRIO_INHERIT));
        CHECK(!pthread_mutexattr_setrobust(&a,PTHREAD_MUTEX_ROBUST));CHECK(!pthread_mutex_init(&c.mutex,&a));CHECK(!pthread_mutexattr_destroy(&a));
        CHECK(!pipe(c.ready)&&!pipe(c.release));pthread_t t;CHECK(!pthread_create(&t,0,robust_owner,&c));byte_read(c.ready[0]);byte_send(c.release[1]);CHECK(!pthread_join(t,0));
        CHECK(pthread_mutex_lock(&c.mutex)==EOWNERDEAD);if(recover)CHECK(!pthread_mutex_consistent(&c.mutex));CHECK(!pthread_mutex_unlock(&c.mutex));
        int r=pthread_mutex_lock(&c.mutex);CHECK(r==(recover?0:ENOTRECOVERABLE));if(!r)CHECK(!pthread_mutex_unlock(&c.mutex));
        CHECK(!pthread_mutex_destroy(&c.mutex)&&!close(c.ready[0])&&!close(c.ready[1])&&!close(c.release[0])&&!close(c.release[1]));
    }
    puts("PI CASE libc-robust-recovery PASS");
}

struct dying {uint32_t word;uint32_t *target;int ready[2],release[2];pid_t tid;};
static void *dying_owner(void *p)
{struct dying *d=p;d->tid=(pid_t)syscall(SYS_gettid);CHECK(!pi(d->target?d->target:&d->word,6|PRIVATE,0));byte_send(d->ready[1]);byte_read(d->release[0]);return 0;}
static void *dead_waiter(void *p)
{struct contender *c=p;c->tid=(pid_t)syscall(SYS_gettid);byte_send(c->ready[1]);CHECK(!pi(c->word,6|PRIVATE,0));CHECK((*c->word&OWNER_DIED)!=0);CHECK(!pi(c->word,7|PRIVATE,0));return 0;}
static void queued_owner_exit(void)
{
    struct dying d={0};CHECK(!pipe(d.ready)&&!pipe(d.release));pthread_t owner,waiter;CHECK(!pthread_create(&owner,0,dying_owner,&d));byte_read(d.ready[0]);
    struct contender c={.word=&d.word};CHECK(!pipe(c.ready)&&!pthread_create(&waiter,0,dead_waiter,&c));byte_read(c.ready[0]);blocked(c.tid);
    byte_send(d.release[1]);CHECK(!pthread_join(owner,0));joined(waiter,&c);CHECK(!d.word);
    CHECK(!close(d.ready[0])&&!close(d.ready[1])&&!close(d.release[0])&&!close(d.release[1]));
    puts("PI CASE queued-owner-exit PASS");
}
struct inversion {uint32_t word;int low_ready[2],low_go[2],medium_ready[2],medium_go[2];unsigned medium;};
static void *low(void *p)
{
    struct inversion *v=p;affinity(0);policy(SCHED_FIFO,10);CHECK(!pi(&v->word,6|PRIVATE,0));byte_send(v->low_ready[1]);byte_read(v->low_go[0]);
    /* 先让中优先级任务也进入就绪队列；继承后的holder应先被调度。 */
    CHECK(!__atomic_load_n(&v->medium,__ATOMIC_ACQUIRE));CHECK(!pi(&v->word,7|PRIVATE,0));policy(SCHED_OTHER,0);return 0;
}
static void *medium(void *p)
{
    struct inversion *v=p;affinity(0);policy(SCHED_FIFO,40);byte_send(v->medium_ready[1]);byte_read(v->medium_go[0]);
    __atomic_store_n(&v->medium,1,__ATOMIC_RELEASE);policy(SCHED_OTHER,0);return 0;
}
static void inversion(void)
{
    uint64_t saved=0;CHECK(syscall(SYS_sched_getaffinity,0,sizeof(saved),&saved)>0);affinity(0);
    struct inversion v={0};CHECK(!pipe(v.low_ready)&&!pipe(v.low_go)&&!pipe(v.medium_ready)&&!pipe(v.medium_go));
    pthread_t l,m;CHECK(!pthread_create(&l,0,low,&v));byte_read(v.low_ready[0]);
    struct contender high={.word=&v.word,.op=6|PRIVATE,.priority=80};pthread_t h=start(&high);blocked(high.tid);
    CHECK(!pthread_create(&m,0,medium,&v));byte_read(v.medium_ready[0]);
    policy(SCHED_FIFO,90);byte_send(v.medium_go[1]);byte_send(v.low_go[1]);joined(h,&high);CHECK(!high.result);policy(SCHED_OTHER,0);
    CHECK(!pthread_join(l,0)&&!pthread_join(m,0)&&v.medium==1&&!v.word);
    CHECK(!close(v.low_ready[0])&&!close(v.low_ready[1])&&!close(v.low_go[0])&&!close(v.low_go[1]));
    CHECK(!close(v.medium_ready[0])&&!close(v.medium_ready[1])&&!close(v.medium_go[0])&&!close(v.medium_go[1]));
    CHECK(!syscall(SYS_sched_setaffinity,0,sizeof(saved),&saved));puts("PI CASE priority-inversion-scheduling PASS");
}
struct chain {uint32_t first,second;int ready[2],go[2];pid_t tid;};
static void *chain_middle(void *p)
{
    struct chain *c=p;c->tid=(pid_t)syscall(SYS_gettid);CHECK(!pi(&c->second,6|PRIVATE,0));byte_send(c->ready[1]);byte_read(c->go[0]);
    CHECK(!pi(&c->first,6|PRIVATE,0));CHECK(!pi(&c->first,7|PRIVATE,0)&&!pi(&c->second,7|PRIVATE,0));return 0;
}
static void chain_cycle(void)
{
    struct chain c={0};CHECK(!pipe(c.ready)&&!pipe(c.go)&&!pi(&c.first,6|PRIVATE,0));pthread_t middle;
    CHECK(!pthread_create(&middle,0,chain_middle,&c));byte_read(c.ready[0]);byte_send(c.go[1]);blocked(c.tid);
    ERROR(pi(&c.second,6|PRIVATE,0),EDEADLK);
    struct contender high={.word=&c.second,.op=6|PRIVATE,.priority=50};pthread_t h=start(&high);blocked(high.tid);
    char state;CHECK(task_field(c.tid,18,&state)==-51&&task_field((pid_t)syscall(SYS_gettid),18,&state)==-51);
    CHECK(!pi(&c.first,7|PRIVATE,0));CHECK(!pthread_join(middle,0));joined(h,&high);CHECK(!high.result&&!c.first&&!c.second);
    CHECK(!close(c.ready[0])&&!close(c.ready[1])&&!close(c.go[0])&&!close(c.go[1]));puts("PI CASE chain-donation-cycle PASS");
}

static void *normal_wait(void *p)
{
    struct contender *c=p;c->tid=(pid_t)syscall(SYS_gettid);uint32_t expected=*c->word;byte_send(c->ready[1]);
    CHECK(!syscall(SYS_futex,c->word,PRIVATE,expected,0,0,0));return 0;
}
static void mixed(void)
{
    uint32_t word=0;CHECK(!pi(&word,6|PRIVATE,0));struct contender c={.word=&word};pthread_t t;
    CHECK(!pipe(c.ready)&&!pthread_create(&t,0,normal_wait,&c));byte_read(c.ready[0]);blocked(c.tid);
    struct contender other={.word=&word,.op=6|PRIVATE};pthread_t o=start(&other);joined(o,&other);CHECK(other.result==-1&&other.error==EINVAL);
    CHECK(syscall(SYS_futex,&word,1|PRIVATE,1,0,0,0)==1);joined(t,&c);
    c=(struct contender){.word=&word,.op=6|PRIVATE};t=start(&c);blocked(c.tid);
    ERROR(syscall(SYS_futex,&word,1|PRIVATE,1,0,0,0),EINVAL);
    CHECK(!pi(&word,7|PRIVATE,0));joined(t,&c);CHECK(!c.result&&!word);
    puts("PI CASE mixed-normal-pi-waiters PASS");
}
static void shared_alias(void)
{
    char name[]="/pi-alias-XXXXXX";int fd=mkstemp(name);CHECK(fd>=0&&!unlink(name)&&!ftruncate(fd,4096));
    uint32_t *a=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0),*b=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    CHECK(a!=MAP_FAILED&&b!=MAP_FAILED&&!close(fd));CHECK(!pi(a,6,0));
    struct contender c={.word=b,.op=13};pthread_t t=start(&c);blocked(c.tid);CHECK(!pi(a,7,0));joined(t,&c);CHECK(!c.result&&!*a&&!munmap(a,4096)&&!munmap(b,4096));

    struct dying d={0};d.target=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(d.target!=MAP_FAILED);
    CHECK(!pipe(d.ready)&&!pipe(d.release));pthread_t owner;CHECK(!pthread_create(&owner,0,dying_owner,&d));byte_read(d.ready[0]);
    c=(struct contender){.word=d.target,.op=6|PRIVATE};t=start(&c);blocked(c.tid);
    CHECK(!mprotect(d.target,4096,PROT_NONE));byte_send(d.release[1]);CHECK(!pthread_join(owner,0));joined(t,&c);CHECK(c.result==-1&&c.error==EFAULT);
    CHECK(!close(d.ready[0])&&!close(d.ready[1])&&!close(d.release[0])&&!close(d.release[1])&&!munmap(d.target,4096));
    puts("PI CASE shared-alias-owner-fault PASS");
}

static void waiter_order(void)
{
    uint32_t word=0;unsigned counter=0;CHECK(!pi(&word,6|PRIVATE,0));struct contender c[3];pthread_t t[3];
    for(unsigned i=0;i<3;i++){c[i]=(struct contender){.word=&word,.op=6|PRIVATE,.priority=(i==2?50:30),.counter=&counter};t[i]=start(&c[i]);blocked(c[i].tid);}
    struct sched_param p={.sched_priority=70};CHECK(!syscall(SYS_sched_setparam,c[0].tid,&p));
    char state;CHECK(task_field((pid_t)syscall(SYS_gettid),18,&state)==-71);
    pid_t child=fork();CHECK(child>=0);if(!child){CHECK(!syscall(SYS_sched_getparam,0,&p)&&p.sched_priority==0);_exit(0);}wait_ok(child);
    CHECK(!pi(&word,7|PRIVATE,0));for(unsigned i=0;i<3;i++){joined(t[i],&c[i]);CHECK(!c[i].result);}
    CHECK(c[0].order==0&&c[2].order==1&&c[1].order==2&&counter==3&&!word);
    puts("PI CASE repriority-order-fork-base PASS");
}
static void local_cpus(void)
{
    uint64_t saved=0;CHECK(syscall(SYS_sched_getaffinity,0,sizeof(saved),&saved)>0);
    for(unsigned cpu=0;cpu<4;cpu++)if(saved&(UINT64_C(1)<<cpu)){
        affinity(cpu);uint32_t word=0;for(unsigned i=0;i<16;i++){CHECK(!pi(&word,13|PRIVATE,0));CHECK(!pi(&word,7|PRIVATE,0));}
        CHECK(!pi(&word,6|PRIVATE,0));struct contender c={.word=&word,.op=13|PRIVATE,.priority=20,.cpu_plus1=(cpu?1:((saved&2)?2:1))};
        pthread_t t=start(&c);blocked(c.tid);char state;CHECK(task_field((pid_t)syscall(SYS_gettid),18,&state)==-21);
        CHECK(!pi(&word,7|PRIVATE,0));joined(t,&c);CHECK(!c.result&&!word);
    }
    CHECK(!syscall(SYS_sched_setaffinity,0,sizeof(saved),&saved));puts("PI CASE caller-cpu-affinity PASS");
}
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);alarm(15);mkdir("/proc",0755);CHECK(!mount("proc","/proc","proc",0,0)||errno==EBUSY);
    raw_owner();raw_errors();contention();timeouts_and_signal();shared_fork();libc_robust();queued_owner_exit();inversion();chain_cycle();mixed();shared_alias();waiter_order();local_cpus();
    puts("PI END 13");alarm(0);
    if(getpid()==1){sync();reboot(RB_POWER_OFF);}
    return 0;
}
