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
    state(attr_get(0),SCHED_FIFO,20,0);struct sched_param p;
    CHECK(!syscall(SYS_sched_getparam,0,&p)&&p.sched_priority==20);
    CHECK(!syscall(SYS_futex,&word,7|128,0,0,0,0));CHECK(!pthread_join(th,0)&&!word);
    CHECK(!close(w.ready[0])&&!close(w.ready[1]));policy(0,SCHED_OTHER,0);pass("pi-effective-versus-base-priority");
}

int main(int argc,char **argv)
{
    host_safe=argc==2&&!strcmp(argv[1],"--host-safe");CHECK(argc==1||host_safe);
    setvbuf(stdout,0,_IONBF,0);alarm(30);page_size=(size_t)sysconf(_SC_PAGESIZE);
    struct utsname uts;CHECK(!uname(&uts));boaros=strstr(uts.release,"boaros")!=0;
    if(getpid()==1){mkdir("/proc",0755);CHECK(!mount("proc","/proc","proc",0,0)||errno==EBUSY);}
    puts("SCHED-QUERY BEGIN");sizes_and_order();output_fault();cpu_queries();lifetime();
    if(host_safe)puts("SCHED-QUERY SKIP privileged-policy-and-pi host-safe");
    else {policies();changing_policy();pi_base_priority();}
    printf("SCHED-QUERY END cases=%u\n",cases);alarm(0);
    if(getpid()==1){sync();reboot(RB_POWER_OFF);}return 0;
}
