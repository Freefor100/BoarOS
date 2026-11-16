#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <sys/resource.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do {if(!(x)){fprintf(stderr,"SCHED-QUERY FAIL line=%d errno=%d %s\n",__LINE__,errno,#x);_exit(1);}}while(0)
#define ERROR(x,e) do {errno=0;CHECK((x)==-1&&errno==(e));}while(0)
struct attr {
    uint32_t size,policy;
    uint64_t flags;
    int32_t nice;
    uint32_t priority;
    uint64_t runtime,deadline,period;
    uint32_t util_min,util_max;
};
_Static_assert(sizeof(struct attr)==56,"native sched_attr size");
static unsigned cases;
static int boaros,host_safe;
static size_t page_size;
static void pass(const char *name){printf("SCHED-QUERY CASE %s PASS\n",name);cases++;}
static long attr_raw(uint64_t tid,void *out,uint64_t size,uint64_t flags)
{return syscall(SYS_sched_getattr,tid,out,size,flags);}
static struct attr attr_get(pid_t tid)
{struct attr a;memset(&a,0xa5,sizeof(a));CHECK(!attr_raw((uint32_t)tid,&a,sizeof(a),0));return a;}
static void policy(pid_t tid,int kind,int priority)
{struct sched_param p={.sched_priority=priority};CHECK(!syscall(SYS_sched_setscheduler,tid,kind,&p));}
static void state(struct attr a,unsigned kind,unsigned priority,unsigned reset)
{
    CHECK(a.size==56&&a.policy==kind&&a.flags==reset&&a.nice==0&&a.priority==priority);
    CHECK(!a.deadline&&!a.period);
    if(kind==SCHED_FIFO||kind==SCHED_RR)CHECK(!a.runtime);
    if(!host_safe)CHECK(!a.util_min&&!a.util_max);
    if(boaros&&kind==SCHED_OTHER)CHECK(a.runtime==10000000);
}
static void child_ok(pid_t pid)
{int status;CHECK(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&!WEXITSTATUS(status));}
static void send_byte(int fd){CHECK(write(fd,"x",1)==1);}
static void read_byte(int fd){char c;CHECK(read(fd,&c,1)==1);}
static void barrier(pthread_barrier_t *b)
{int e=pthread_barrier_wait(b);CHECK(!e||e==PTHREAD_BARRIER_SERIAL_THREAD);}
static uint64_t stamp(void)
{struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;}

static void sizes_and_order(void)
{
    struct attr full=attr_get(0);state(full,SCHED_OTHER,0,0);
    unsigned char bytes[80];
    for(unsigned size=48;size<=56;size++) {
        memset(bytes,0xa5,sizeof(bytes));CHECK(!attr_raw(0,bytes,size,0));
        struct attr expected=full;expected.size=size;
        /* Linux may select another fair-class slice between queries. */
        struct attr got={0};memcpy(&got,bytes,size);expected.runtime=got.runtime;
        CHECK(!memcmp(bytes,&expected,size));
        for(unsigned i=size;i<sizeof(bytes);i++)CHECK(bytes[i]==0xa5);
    }
    memset(bytes,0xa5,sizeof(bytes));CHECK(!attr_raw(0,bytes,sizeof(bytes),0));
    memcpy(&full,bytes,sizeof(full));CHECK(full.size==56);
    for(unsigned i=56;i<sizeof(bytes);i++)CHECK(!bytes[i]);
    for(unsigned size=0;size<48;size++)ERROR(attr_raw(0,bytes,size,0),EINVAL);
    ERROR(attr_raw(0,bytes,page_size+1,0),EINVAL);
    ERROR(attr_raw(0,0,56,0),EINVAL);
    ERROR(attr_raw(UINT32_MAX,bytes,56,0),EINVAL);
    ERROR(attr_raw(INT32_MAX,0,56,1),EINVAL);
    ERROR(attr_raw(INT32_MAX,(void *)1,47,1),EINVAL);
    ERROR(attr_raw(INT32_MAX,(void *)1,56,1),ESRCH);
    ERROR(attr_raw(0,(void *)1,56,1),EINVAL);
    ERROR(attr_raw(INT32_MAX,(void *)1,56,0),ESRCH);
    ERROR(attr_raw(0,(void *)1,56,0),EFAULT);
    CHECK(!attr_raw(UINT64_C(1)<<32,bytes,(UINT64_C(1)<<32)|56,UINT64_C(1)<<32));
    CHECK(!attr_raw((UINT64_C(1)<<32)|(uint32_t)syscall(SYS_gettid),bytes,56,0));
    unsigned char *large=mmap(0,page_size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(large!=MAP_FAILED);memset(large,0xa5,page_size);CHECK(!attr_raw(0,large,page_size,0));
    for(size_t i=56;i<page_size;i++)CHECK(!large[i]);
    CHECK(!munmap(large,page_size));pass("native-size-prefix-width-and-error-order");
}

static void output_fault(void)
{
    unsigned char *mapping=mmap(0,page_size*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(mapping!=MAP_FAILED&&!mprotect(mapping+page_size,page_size,PROT_NONE));
    unsigned char *out=mapping+page_size-32;memset(out,0xa5,32);
    struct attr expected=attr_get(0);ERROR(attr_raw(0,out,56,0),EFAULT);
    CHECK(!memcmp(out,&expected,24));
    uint64_t runtime;memcpy(&runtime,out+24,sizeof(runtime));
    CHECK(runtime<UINT64_C(1000000000));
    if(boaros)CHECK(runtime==10000000);
    out=mapping+page_size-64;memset(out,0xa5,64);
    ERROR(attr_raw(0,out,80,0),EFAULT);
    for(unsigned i=0;i<56;i++)CHECK(out[i]==0xa5);
    for(unsigned i=56;i<64;i++)CHECK(!out[i]);
    CHECK(!munmap(mapping,page_size*2));pass("output-fault-prefix-and-extension-order");
}

static void cpu_queries(void)
{
    cpu_set_t saved;CPU_ZERO(&saved);CHECK(syscall(SYS_sched_getaffinity,0,sizeof(saved),&saved)>0);
    unsigned visited=0;
    for(unsigned cpu=0;cpu<CPU_SETSIZE;cpu++)if(CPU_ISSET(cpu,&saved)) {
        cpu_set_t one,got;CPU_ZERO(&one);CPU_SET(cpu,&one);CHECK(!syscall(SYS_sched_setaffinity,0,sizeof(one),&one));
        for(unsigned i=0;i<16;i++) {
            unsigned actual=~0U,node=~0U;CHECK(!syscall(SYS_getcpu,&actual,&node,0)&&actual==cpu&&node==0);
            state(attr_get(0),SCHED_OTHER,0,0);
            struct sched_param p={.sched_priority=-1};struct timespec interval={-1,-1};
            CHECK(syscall(SYS_sched_getscheduler,0)==SCHED_OTHER);
            CHECK(!syscall(SYS_sched_getparam,0,&p)&&!p.sched_priority);
            CHECK(!syscall(SYS_sched_rr_get_interval,0,&interval)&&interval.tv_sec>=0&&interval.tv_nsec>=0);
            CPU_ZERO(&got);CHECK(syscall(SYS_sched_getaffinity,0,sizeof(got),&got)>0&&CPU_COUNT(&got)==1&&CPU_ISSET(cpu,&got));
            CHECK(syscall(SYS_sched_get_priority_max,SCHED_FIFO)==99&&syscall(SYS_sched_get_priority_min,SCHED_RR)==1);
            CHECK(!syscall(SYS_getcpu,&actual,&node,0)&&actual==cpu&&node==0);
        }
        printf("SCHED-QUERY CPU index=%u PASS\n",cpu);visited++;
        if(!boaros&&visited==8)break;
    }
    CHECK(visited&&(!boaros||visited<=8)&&!syscall(SYS_sched_setaffinity,0,sizeof(saved),&saved));
    struct sched_param p;struct timespec interval;cpu_set_t mask;
    ERROR(syscall(SYS_sched_getparam,INT32_MAX,0),EINVAL);
    ERROR(syscall(SYS_sched_getparam,INT32_MAX,(void *)1),ESRCH);
    ERROR(syscall(SYS_sched_getparam,0,(void *)1),EFAULT);
    ERROR(syscall(SYS_sched_getparam,-1,&p),EINVAL);
    ERROR(syscall(SYS_sched_getscheduler,-1),EINVAL);
    ERROR(syscall(SYS_sched_rr_get_interval,-1,&interval),EINVAL);
    ERROR(syscall(SYS_sched_rr_get_interval,INT32_MAX,(void *)1),ESRCH);
    ERROR(syscall(SYS_sched_rr_get_interval,0,0),EFAULT);
    ERROR(syscall(SYS_sched_getaffinity,INT32_MAX,1,&mask),EINVAL);
    ERROR(syscall(SYS_sched_getaffinity,INT32_MAX,sizeof(mask),(void *)1),ESRCH);
    ERROR(syscall(SYS_sched_getaffinity,-1,sizeof(mask),&mask),ESRCH);
    pass("affinity-getcpu-local-queries-and-existing-errors");
}

struct target {int ready[2],release[2];pid_t tid;};
static void *target_wait(void *p)
{struct target *t=p;t->tid=(pid_t)syscall(SYS_gettid);send_byte(t->ready[1]);read_byte(t->release[0]);return 0;}
static pthread_t target_start(struct target *t)
{pthread_t th;CHECK(!pipe(t->ready)&&!pipe(t->release)&&!pthread_create(&th,0,target_wait,t));read_byte(t->ready[0]);return th;}
static void target_finish(pthread_t th,struct target *t)
{send_byte(t->release[1]);CHECK(!pthread_join(th,0));CHECK(!close(t->ready[0])&&!close(t->ready[1])&&!close(t->release[0])&&!close(t->release[1]));}
static void lifetime(void)
{
    pid_t previous=0;unsigned reused=0;
    for(unsigned i=0;i<32;i++) {
        struct target t={0};pthread_t th=target_start(&t);state(attr_get(t.tid),SCHED_OTHER,0,0);
        if(t.tid==previous)reused++;
        previous=t.tid;
        target_finish(th,&t);
        /* clear_child_tid可先唤醒join；PID索引撤销才是查询转为ESRCH的边界。 */
        uint64_t end=stamp()+2000000000;
        for(;;) {
            struct attr out;errno=0;long result=attr_raw((uint32_t)t.tid,&out,56,0);
            if(result==-1){CHECK(errno==ESRCH);break;}
            CHECK(!result);state(out,SCHED_OTHER,0,0);CHECK(stamp()<end);sched_yield();
        }
    }
    pid_t child=fork();CHECK(child>=0);if(!child)_exit(0);
    siginfo_t info;CHECK(!waitid(P_PID,child,&info,WEXITED|WNOWAIT)&&info.si_pid==child);
    state(attr_get(child),SCHED_OTHER,0,0);
    struct sched_param p;struct timespec interval;cpu_set_t mask;
    CHECK(syscall(SYS_sched_getscheduler,child)==SCHED_OTHER&&!syscall(SYS_sched_getparam,child,&p)&&!p.sched_priority);
    CHECK(!syscall(SYS_sched_rr_get_interval,child,&interval)&&syscall(SYS_sched_getaffinity,child,sizeof(mask),&mask)>0);
    child_ok(child);struct attr out;ERROR(attr_raw((uint32_t)child,&out,56,0),ESRCH);
    printf("SCHED-QUERY TID reuse-observed=%u rounds=32\n",reused);pass("other-tid-exit-and-retained-zombie");
}

static void policies(void)
{
    for(unsigned i=0;i<3;i++) {
        policy(0,(int)i,i?20:0);state(attr_get(0),i,i?20:0,0);
        struct sched_param p;CHECK(!syscall(SYS_sched_getparam,0,&p)&&p.sched_priority==(i?20:0));
    }
    policy(0,SCHED_FIFO|SCHED_RESET_ON_FORK,20);state(attr_get(0),SCHED_FIFO,20,1);
    pid_t child=fork();CHECK(child>=0);if(!child){state(attr_get(0),SCHED_OTHER,0,0);_exit(0);}child_ok(child);
    state(attr_get(0),SCHED_FIFO,20,1);policy(0,SCHED_OTHER,0);pass("other-fifo-rr-and-reset-on-fork");
}

struct changing {pthread_barrier_t gate;pid_t tid;unsigned done,observed,version,ack;};
static void *change_policy(void *p)
{
    struct changing *c=p;barrier(&c->gate);
    while(!__atomic_load_n(&c->observed,__ATOMIC_ACQUIRE))sched_yield();
    for(unsigned i=0;i<512;i++) {
        for(unsigned rr=0;rr<2;rr++) {
            __atomic_add_fetch(&c->version,1,__ATOMIC_SEQ_CST);
            policy(c->tid,rr?SCHED_RR|SCHED_RESET_ON_FORK:SCHED_FIFO,rr?42:21);
            unsigned version=__atomic_add_fetch(&c->version,1,__ATOMIC_SEQ_CST);
            if(!i)while(__atomic_load_n(&c->ack,__ATOMIC_ACQUIRE)!=version)sched_yield();
        }
        sched_yield();
    }
    __atomic_store_n(&c->done,1,__ATOMIC_RELEASE);return 0;
}
static void changing_policy(void)
{
    struct target t={0};pthread_t target=target_start(&t),writer;
    struct changing c={.tid=t.tid};CHECK(!pthread_barrier_init(&c.gate,0,2));
    policy(t.tid,SCHED_FIFO,21);CHECK(!pthread_create(&writer,0,change_policy,&c));barrier(&c.gate);
    unsigned samples=0,stable=0,seen=0,mixed=0;
    do {
        unsigned before=__atomic_load_n(&c.version,__ATOMIC_SEQ_CST);
        struct attr a=attr_get(t.tid);
        unsigned after=__atomic_load_n(&c.version,__ATOMIC_SEQ_CST);
        int complete=(a.policy==SCHED_FIFO&&a.priority==21&&a.flags==0)||
            (a.policy==SCHED_RR&&a.priority==42&&a.flags==1);
        CHECK((a.policy==SCHED_FIFO||a.policy==SCHED_RR)&&(a.priority==21||a.priority==42)&&a.flags<=1);
        CHECK(a.size==56&&!a.nice&&!a.runtime&&!a.deadline&&!a.period);samples++;
        if(boaros)CHECK(complete);
        if(before==after&&!(before&1U)) {
            unsigned rr=before&&!(before&2U);
            state(a,rr?SCHED_RR:SCHED_FIFO,rr?42:21,rr);stable++;seen|=1U<<rr;
            if(before==2||before==4)__atomic_store_n(&c.ack,before,__ATOMIC_RELEASE);
        } else {
            /* Linux的RCU只保护目标存活，getter未取setter的rq锁，字段可跨版本。 */
            if(!complete)mixed++;
        }
        __atomic_store_n(&c.observed,1,__ATOMIC_RELEASE);
    }while(!__atomic_load_n(&c.done,__ATOMIC_ACQUIRE));
    CHECK(!pthread_join(writer,0)&&!pthread_barrier_destroy(&c.gate));
    CHECK(seen==3&&stable>=2);state(attr_get(t.tid),SCHED_RR,42,1);
    policy(t.tid,SCHED_OTHER,0);target_finish(target,&t);
    printf("SCHED-QUERY SNAPSHOT samples=%u stable=%u mixed=%u changes=1024\n",samples,stable,mixed);pass("query-during-policy-change");
}

#ifndef SCHED_CONTROL_ONLY
#define SCHED_CONTROL_ONLY 0
#endif
static long control_nice(int32_t);
static void control_attr(struct attr *);
static void control_policy(int,int);
struct pi_waiter {uint32_t *word;int ready[2];};
static void *donate(void *p)
{
    struct pi_waiter *w=p;policy(0,SCHED_FIFO,60);send_byte(w->ready[1]);
    CHECK(!syscall(SYS_futex,w->word,6|128,0,0,0,0));
    CHECK(!syscall(SYS_futex,w->word,7|128,0,0,0,0));policy(0,SCHED_OTHER,0);return 0;
}
static long own_effective_priority(void)
{
    char path[80],buf[1024];snprintf(path,sizeof(path),"/proc/self/task/%ld/stat",syscall(SYS_gettid));
    int fd=open(path,O_RDONLY);CHECK(fd>=0);ssize_t n=read(fd,buf,sizeof(buf)-1);CHECK(n>0&&!close(fd));buf[n]=0;
    char *p=strrchr(buf,')');CHECK(p&&p[1]==' ');p+=4;
    for(unsigned field=4;field<18;field++){p=strchr(p,' ');CHECK(p);p++;}return strtol(p,0,10);
}
static void pi_base_priority(void)
{
    policy(0,SCHED_FIFO,20);uint32_t word=0;CHECK(!syscall(SYS_futex,&word,6|128,0,0,0,0));
    struct pi_waiter w={.word=&word};pthread_t th;CHECK(!pipe(w.ready)&&!pthread_create(&th,0,donate,&w));read_byte(w.ready[0]);
    uint64_t end=stamp()+2000000000;
    while(own_effective_priority()!=-61){CHECK(stamp()<end);sched_yield();}
    unsigned base=20;
    if(SCHED_CONTROL_ONLY) {
        CHECK(!setpriority(PRIO_PROCESS,0,7)&&control_nice(0)==7);
        struct attr changed=attr_get(0);changed.priority=30;control_attr(&changed);base=30;
        CHECK(own_effective_priority()==-61);
    }
    state(attr_get(0),SCHED_FIFO,base,0);struct sched_param p;
    CHECK(!syscall(SYS_sched_getparam,0,&p)&&p.sched_priority==(int)base);
    CHECK(!syscall(SYS_futex,&word,7|128,0,0,0,0));CHECK(!pthread_join(th,0)&&!word);
    CHECK(!close(w.ready[0])&&!close(w.ready[1]));policy(0,SCHED_OTHER,0);if(SCHED_CONTROL_ONLY)CHECK(!setpriority(PRIO_PROCESS,0,0));pass("pi-effective-versus-base-priority");
}

#ifndef SCHED_CONTROL_ONLY
#define SCHED_CONTROL_ONLY 0
#endif
static long control_nice(int32_t who)
{errno=0;long n=getpriority(PRIO_PROCESS,who);CHECK(n!=-1||!errno);return n;}
static void control_attr(struct attr *a)
{CHECK(!syscall(SYS_sched_setattr,0,a,0));}
static void control_slice(pid_t tid,uint64_t expected)
{
    CHECK(attr_get(tid).runtime==expected);
    if(boaros) {
        struct timespec interval;
        CHECK(!syscall(SYS_sched_rr_get_interval,tid,&interval)&&!interval.tv_sec&&
            (uint64_t)interval.tv_nsec==expected);
    }
}
static void control_quantum(void)
{
    const uint64_t requests[]={1,99999,100000,2000000,100000000,UINT64_MAX};
    const uint64_t expected[]={100000,100000,100000,2000000,100000000,100000000};
    for(unsigned i=0;i<sizeof(requests)/sizeof(*requests);i++) {
        struct attr a=attr_get(0);a.runtime=requests[i];control_attr(&a);control_slice(0,expected[i]);
    }
    struct attr a=attr_get(0);a.runtime=2000000;control_attr(&a);
    struct sched_param p={0};CHECK(!syscall(SYS_sched_setparam,0,&p));control_slice(0,2000000);
    CHECK(!syscall(SYS_sched_setscheduler,0,SCHED_BATCH,&p));control_slice(0,2000000);
    CHECK(!setpriority(PRIO_PROCESS,0,1));control_slice(0,2000000);
    a=attr_get(0);a.policy=SCHED_OTHER;a.flags=16;a.runtime=UINT64_MAX;a.nice=19;control_attr(&a);
    CHECK(attr_get(0).policy==SCHED_BATCH&&control_nice(0)==1);control_slice(0,2000000);
    a=attr_get(0);a.policy=99;a.flags=8;a.runtime=3000000;control_attr(&a);control_slice(0,3000000);
    a=attr_get(0);a.runtime=0;a.flags=0;control_attr(&a);
    if(boaros)control_slice(0,10000000);
    control_policy(SCHED_OTHER,0);
}
static void control_policy(int kind,int n)
{struct attr a={.size=sizeof(a),.policy=(uint32_t)kind,.nice=n};control_attr(&a);}
static void control_abi(void)
{
    CHECK(control_nice(0)==0&&syscall(SYS_getpriority,PRIO_PROCESS,0)==20);
    CHECK(!setpriority(PRIO_PROCESS,0,-1));errno=0;CHECK(getpriority(PRIO_PROCESS,0)==-1&&!errno);
    CHECK(syscall(SYS_getpriority,PRIO_PROCESS,0)==21);
    CHECK(!setpriority(PRIO_PROCESS,0,100)&&control_nice(0)==19);
    CHECK(!setpriority(PRIO_PROCESS,0,-100)&&control_nice(0)==-20);
    CHECK(!setpriority(PRIO_PROCESS,0,0));CHECK(nice(1)==1);CHECK(!setpriority(PRIO_PROCESS,0,0));
    ERROR(syscall(SYS_getpriority,3,0),EINVAL);ERROR(syscall(SYS_setpriority,3,0,0),EINVAL);
    ERROR(syscall(SYS_getpriority,0,-1),ESRCH);
    unsigned char bytes[80]={0};struct attr *a=(void *)bytes;
    a->size=0;a->policy=SCHED_BATCH;a->nice=5;control_attr(a);
    struct attr got=attr_get(0);CHECK(got.policy==SCHED_BATCH&&got.nice==5);
    a->size=56;a->nice=7;a->policy=99;a->flags=8;control_attr(a);
    got=attr_get(0);CHECK(got.policy==SCHED_BATCH&&got.nice==7);
    *a=got;a->policy=SCHED_OTHER;a->nice=-20;a->flags=16;control_attr(a);
    got=attr_get(0);CHECK(got.policy==SCHED_BATCH&&got.nice==7);
    *a=got;a->nice=3;control_attr(a);CHECK(control_nice(0)==3); /* get-modify-set */
    a->size=47;ERROR(syscall(SYS_sched_setattr,0,a,0),E2BIG);CHECK(a->size==56&&control_nice(0)==3);
    a->size=80;bytes[79]=1;ERROR(syscall(SYS_sched_setattr,0,a,0),E2BIG);CHECK(a->size==56);
    bytes[79]=0;a->size=80;control_attr(a);
    a->size=48;a->flags=0x20;ERROR(syscall(SYS_sched_setattr,0,a,0),EINVAL);
    a->size=56;a->flags=UINT64_C(1)<<63;ERROR(syscall(SYS_sched_setattr,0,a,0),EINVAL);
    a->flags=0;ERROR(syscall(SYS_sched_setattr,0,a,1),EINVAL);ERROR(syscall(SYS_sched_setattr,0,(void *)1,0),EFAULT);
    if(boaros){a->flags=0x20;ERROR(syscall(SYS_sched_setattr,0,a,0),EOPNOTSUPP);a->flags=0;}
    control_policy(SCHED_OTHER,0);control_quantum();pass("nice-native-libc-and-attr-abi");
}
static void control_inherit(void)
{
    control_policy(SCHED_BATCH,6);
    struct attr inherited=attr_get(0);inherited.runtime=3000000;control_attr(&inherited);
    pid_t child=fork();CHECK(child>=0);
    if(!child){CHECK(control_nice(0)==6);execl("/sched_control","sched_control","--nice-exec",(char *)0);_exit(91);}child_ok(child);
    struct attr a={.size=56,.policy=SCHED_BATCH,.nice=-5,.flags=1,.runtime=3000000};control_attr(&a);
    child=fork();CHECK(child>=0);
    if(!child){struct attr got=attr_get(0);CHECK(got.policy==SCHED_BATCH&&got.nice==0&&!got.flags);if(boaros)control_slice(0,10000000);_exit(0);}child_ok(child);
    control_slice(0,3000000);
    a=(struct attr){.size=56,.policy=SCHED_FIFO,.priority=20,.flags=1,.runtime=UINT64_MAX};control_attr(&a);
    child=fork();CHECK(child>=0);
    if(!child){struct attr got=attr_get(0);CHECK(got.policy==SCHED_OTHER&&got.nice==0&&!got.priority&&!got.flags);if(boaros)control_slice(0,10000000);_exit(0);}child_ok(child);
    policy(0,SCHED_OTHER,0);control_slice(0,3000000);
    control_policy(SCHED_OTHER,0);pass("nice-fork-exec-reset");
}
static void control_permissions(void)
{
    pid_t child=fork();CHECK(child>=0);
    if(!child){struct rlimit limit={25,25};CHECK(!setrlimit(RLIMIT_NICE,&limit)&&!setuid(1001));
        CHECK(!setpriority(PRIO_PROCESS,0,-5));ERROR(setpriority(PRIO_PROCESS,0,-6),EACCES);
        limit=(struct rlimit){0,0};CHECK(!setrlimit(RLIMIT_NICE,&limit));CHECK(!setpriority(PRIO_PROCESS,0,5));
        ERROR(setpriority(PRIO_PROCESS,0,0),EACCES);
        struct attr a={.size=56,.policy=SCHED_OTHER,.nice=0};ERROR(syscall(SYS_sched_setattr,0,&a,0),EPERM);
        a.policy=SCHED_IDLE;a.nice=5;control_attr(&a);CHECK(attr_get(0).policy==SCHED_IDLE);
        a.policy=SCHED_BATCH;ERROR(syscall(SYS_sched_setattr,0,&a,0),EPERM);_exit(0);}
    child_ok(child);
    int ready[2],release[2];CHECK(!pipe(ready)&&!pipe(release));pid_t peers[2];
    for(unsigned i=0;i<2;i++) {
        peers[i]=fork();CHECK(peers[i]>=0);
        if(!peers[i]){CHECK(!setpgid(0,i?peers[0]:0));struct rlimit limit={i?0:21,i?0:21};
            CHECK(!setrlimit(RLIMIT_NICE,&limit)&&!setuid(1001));send_byte(ready[1]);read_byte(release[0]);_exit(0);}
        read_byte(ready[0]);
    }
    child=fork();CHECK(child>=0);
    if(!child){CHECK(!setpgid(0,peers[0]));struct rlimit limit={0,0};CHECK(!setrlimit(RLIMIT_NICE,&limit)&&!setuid(1001));
        ERROR(setpriority(PRIO_USER,0,-1),EACCES);CHECK(control_nice(peers[0])==-1&&control_nice(peers[1])==0&&control_nice(0)==0);
        errno=0;CHECK(getpriority(PRIO_PGRP,0)==-1&&!errno);
        CHECK(!setpriority(PRIO_PGRP,0,7)&&getpriority(PRIO_USER,0)==7);_exit(0);}
    child_ok(child);CHECK(control_nice(peers[0])==7&&control_nice(peers[1])==7);
    child=fork();CHECK(child>=0);
    if(!child){CHECK(!setuid(1002));ERROR(setpriority(PRIO_PROCESS,peers[0],8),EPERM);_exit(0);}child_ok(child);
    send_byte(release[1]);send_byte(release[1]);child_ok(peers[0]);child_ok(peers[1]);
    CHECK(!close(ready[0])&&!close(ready[1])&&!close(release[0])&&!close(release[1]));pass("nice-target-limits-group-partial-and-owner");
}
struct control_thread {pthread_barrier_t gate;_Atomic int tid,phase;int channel[2];unsigned cpu;};
static void *control_actor(void *arg)
{
    struct control_thread *t=arg;cpu_set_t set;CPU_ZERO(&set);CPU_SET(t->cpu,&set);CHECK(!sched_setaffinity(0,sizeof(set),&set));
    atomic_store(&t->tid,(int)syscall(SYS_gettid));barrier(&t->gate);read_byte(t->channel[0]);atomic_store(&t->phase,1);
    while(atomic_load(&t->phase)==1)__asm__ volatile("":::"memory");
    CHECK(control_nice(0)==4);control_slice(0,3000000);return NULL;
}
static void control_remote(void)
{
    cpu_set_t allowed;CHECK(!sched_getaffinity(0,sizeof(allowed),&allowed));unsigned first=CPU_SETSIZE,last=0;
    for(unsigned i=0;i<CPU_SETSIZE;i++)if(CPU_ISSET(i,&allowed)){if(first==CPU_SETSIZE)first=i;last=i;}
    CHECK(first!=CPU_SETSIZE);cpu_set_t one;CPU_ZERO(&one);CPU_SET(first,&one);CHECK(!sched_setaffinity(0,sizeof(one),&one));
    struct control_thread t={.cpu=last};CHECK(!pipe(t.channel)&&!pthread_barrier_init(&t.gate,0,2));pthread_t thread;
    CHECK(!pthread_create(&thread,0,control_actor,&t));barrier(&t.gate);int tid=atomic_load(&t.tid);
    CHECK(!setpriority(PRIO_PROCESS,tid,5)&&control_nice(tid)==5&&control_nice(0)==0);
    struct attr a={.size=56,.policy=SCHED_BATCH,.nice=6,.runtime=2000000};CHECK(!syscall(SYS_sched_setattr,tid,&a,0));
    CHECK(attr_get(tid).nice==6&&attr_get(tid).policy==SCHED_BATCH);
    control_slice(tid,2000000);
    send_byte(t.channel[1]);while(!atomic_load(&t.phase))sched_yield();
    CHECK(!setpriority(PRIO_PROCESS,tid,4));control_slice(tid,2000000);
    a=attr_get(tid);a.runtime=3000000;CHECK(!syscall(SYS_sched_setattr,tid,&a,0));control_slice(tid,3000000);
    CPU_ZERO(&one);CPU_SET(first,&one);CHECK(!sched_setaffinity(tid,sizeof(one),&one));
    atomic_store(&t.phase,2);CHECK(!pthread_join(thread,0)&&!pthread_barrier_destroy(&t.gate));
    /* clear_child_tid can finish pthread_join before the exiting task has
     * detached its PID. Until then its saved nice remains queryable. */
    uint64_t until=stamp()+2000000000;
    for(;;){errno=0;long raw=syscall(SYS_getpriority,PRIO_PROCESS,tid);
        if(raw==-1&&errno==ESRCH)break;
        CHECK(raw==16&&stamp()<until);sched_yield();}

    CHECK(!close(t.channel[0])&&!close(t.channel[1])&&!sched_setaffinity(0,sizeof(allowed),&allowed));
    pass("nice-blocked-running-migration-and-exit");
}
struct share_case {pthread_barrier_t gate;_Atomic int stop;unsigned cpu;int policy;uint64_t times[2];};
struct share_actor {struct share_case *test;unsigned index;};
static void *share_actor(void *arg)
{
    struct share_actor *actor=arg;struct share_case *t=actor->test;unsigned i=actor->index;
    cpu_set_t set;CPU_ZERO(&set);CPU_SET(t->cpu,&set);CHECK(!sched_setaffinity(0,sizeof(set),&set));
    control_policy(i?t->policy:SCHED_OTHER,i?5:0);barrier(&t->gate);
    struct timespec start,end;CHECK(!clock_gettime(CLOCK_THREAD_CPUTIME_ID,&start));
    uint64_t work=1;while(!atomic_load_explicit(&t->stop,memory_order_relaxed)){
        for(unsigned n=0;n<4096;n++)work=work*6364136223846793005ULL+1;
        __asm__ volatile(""::"r"(work):"memory");}
    CHECK(!clock_gettime(CLOCK_THREAD_CPUTIME_ID,&end));t->times[i]=(uint64_t)(end.tv_sec-start.tv_sec)*1000000000+end.tv_nsec-start.tv_nsec;return NULL;
}
static void control_shares(void)
{
    cpu_set_t set;CHECK(!sched_getaffinity(0,sizeof(set),&set));unsigned cpu=0;while(!CPU_ISSET(cpu,&set))cpu++;
    for(unsigned pass_no=0;pass_no<3;pass_no++){
        struct share_case t={.cpu=cpu,.policy=pass_no==0?SCHED_OTHER:pass_no==1?SCHED_BATCH:SCHED_IDLE};
        CHECK(!pthread_barrier_init(&t.gate,0,3));struct share_actor actors[2]={{&t,0},{&t,1}};pthread_t threads[2];
        for(unsigned i=0;i<2;i++)CHECK(!pthread_create(&threads[i],0,share_actor,&actors[i]));
        barrier(&t.gate);
        /* Readiness is the barrier; this fixed interval tests CPU allocation,
         * rather than using a delay as a concurrency handshake. */
        struct timespec until;CHECK(!clock_gettime(CLOCK_MONOTONIC,&until));until.tv_nsec+=400000000;
        if(until.tv_nsec>=1000000000){until.tv_sec++;until.tv_nsec-=1000000000;}
        CHECK(!clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&until,NULL));atomic_store(&t.stop,1);
        for(unsigned i=0;i<2;i++)CHECK(!pthread_join(threads[i],0));
        CHECK(!pthread_barrier_destroy(&t.gate));
        CHECK(t.times[0]>t.times[1]+t.times[1]/2&&t.times[1]>0);
        printf("SCHED-CONTROL SHARE policy=%d high_cpu_ns=%llu low_cpu_ns=%llu\n",t.policy,(unsigned long long)t.times[0],(unsigned long long)t.times[1]);
    }
    pass("nice-other-batch-idle-cpu-share");
}
static void controls(void)
{
    puts("SCHED-CONTROL BEGIN");control_abi();control_inherit();control_permissions();control_remote();control_shares();pi_base_priority();
    CHECK(control_nice(0)==0&&attr_get(0).policy==SCHED_OTHER);printf("SCHED-CONTROL END cases=%u\n",cases);
}

static const char rr_path[]="/proc/sys/kernel/sched_rr_timeslice_ms";
static void rr_write(const char *value)
{
    int fd=open(rr_path,O_WRONLY);CHECK(fd>=0);
    CHECK(write(fd,value,strlen(value))==(ssize_t)strlen(value)&&!close(fd));
}
static long rr_read(void)
{
    char text[32]={0};int fd=open(rr_path,O_RDONLY);CHECK(fd>=0);
    CHECK(read(fd,text,sizeof(text)-1)>0&&!close(fd));return strtol(text,NULL,10);
}
static void rr_sysctl(void)
{
    long saved=rr_read();CHECK(saved>0);
    struct attr original=attr_get(0);CHECK(original.policy==SCHED_OTHER);
    rr_write("50\n");CHECK(rr_read()==50);
    pid_t child=fork();CHECK(child>=0);
    if(!child) {
        policy(0,SCHED_RR,1);
        struct timespec interval;CHECK(!syscall(SYS_sched_rr_get_interval,0,&interval));
        CHECK(!interval.tv_sec&&interval.tv_nsec==50000000);
        struct sched_param param;CHECK(!syscall(SYS_sched_getparam,0,&param)&&param.sched_priority==1);
        pid_t nested=fork();CHECK(nested>=0);
        if(!nested){CHECK(syscall(SYS_sched_getscheduler,0)==SCHED_RR);_exit(0);}
        child_ok(nested);_exit(0);
    }
    child_ok(child);CHECK(syscall(SYS_sched_getscheduler,0)==SCHED_OTHER);
    for(unsigned i=0;i<2;i++) {
        const char *invalid=i?"x":"2147483648";
        int fd=open(rr_path,O_WRONLY);CHECK(fd>=0);
        ERROR(write(fd,invalid,strlen(invalid)),EINVAL);CHECK(!close(fd)&&rr_read()==50);
    }
    rr_write("1500");policy(0,SCHED_RR,1);
    struct timespec interval;CHECK(!syscall(SYS_sched_rr_get_interval,0,&interval));
    CHECK(interval.tv_sec==1&&interval.tv_nsec==500000000);
    policy(0,SCHED_OTHER,0);
    rr_write("2147483647");policy(0,SCHED_RR,1);
    CHECK(!syscall(SYS_sched_rr_get_interval,0,&interval));
    CHECK(interval.tv_sec==2147483&&interval.tv_nsec==647000000);
    policy(0,SCHED_OTHER,0);
    rr_write("0");CHECK(rr_read()==100);
    rr_write("-1");CHECK(rr_read()==100);
    child=fork();CHECK(child>=0);
    if(!child){CHECK(!setuid(1000));ERROR(open(rr_path,O_WRONLY),EACCES);_exit(0);}
    child_ok(child);
    char text[32];snprintf(text,sizeof(text),"%ld",saved);rr_write(text);
    CHECK(rr_read()==saved);pass("rr-timeslice-global-query-fork-normalization-validation-and-permissions");
}

int main(int argc,char **argv)
{
    if(argc==2&&!strcmp(argv[1],"--nice-exec")) {
        CHECK(control_nice(0)==6&&attr_get(0).policy==SCHED_BATCH);control_slice(0,3000000);return 0;
    }
    if(argc==2&&!strcmp(argv[1],"--rr-sysctl")) {
        setvbuf(stdout,0,_IONBF,0);rr_sysctl();return 0;
    }
    host_safe=argc==2&&!strcmp(argv[1],"--host-safe");CHECK(argc==1||host_safe);
    setvbuf(stdout,0,_IONBF,0);alarm(30);page_size=(size_t)sysconf(_SC_PAGESIZE);
    struct utsname uts;CHECK(!uname(&uts));boaros=strstr(uts.version,"BoarOS")!=0;
    if(getpid()==1){mkdir("/proc",0755);CHECK(!mount("proc","/proc","proc",0,0)||errno==EBUSY);}
    if(SCHED_CONTROL_ONLY){controls();alarm(0);return 0;}
    puts("SCHED-QUERY BEGIN");sizes_and_order();output_fault();cpu_queries();lifetime();
    if(host_safe)puts("SCHED-QUERY SKIP privileged-policy-and-pi host-safe");
    else {policies();changing_policy();pi_base_priority();}
    printf("SCHED-QUERY END cases=%u\n",cases);alarm(0);
    if(getpid()==1){sync();reboot(RB_POWER_OFF);}return 0;
}
