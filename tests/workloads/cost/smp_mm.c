/* Fixed-work MM consumers. Worker creation, prefaulting and start handshakes
 * precede the recorded spans; the same ELF is used for every CPU count. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(v) do { if(!(v)) { dprintf(2,"SMP-MM FAIL line=%d %s errno=%d\n",__LINE__,#v,errno);_exit(1); } } while(0)
enum { ACTORS=8, PARENTS=4, GETTIMES=65536, MAPS=1024, MAP_BYTES=65536, LIFECYCLES=32 };
static unsigned cpu_ids[ACTORS],cpus,smoke,windows;
static size_t page;
static const char *self;
struct actor {
    pthread_barrier_t *gate;
    unsigned id,rounds,cpu,completed;
    uint64_t begin,end,cpu_ns,child_cpu_ns,checksum,last_ns;
    struct timespec *result;
};

static uint64_t clock_ns(clockid_t clock)
{
    struct timespec t;CHECK(!clock_gettime(clock,&t));
    return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;
}
static unsigned pin(unsigned role)
{
    cpu_set_t mask,actual;CPU_ZERO(&mask);CPU_SET(cpu_ids[role%cpus],&mask);
    CHECK(!sched_setaffinity(0,sizeof(mask),&mask));CPU_ZERO(&actual);
    CHECK(!sched_getaffinity(0,sizeof(actual),&actual)&&!memcmp(&mask,&actual,sizeof(mask)));
    unsigned cpu=UINT32_MAX;CHECK(!syscall(SYS_getcpu,&cpu,NULL,NULL)&&cpu==cpu_ids[role%cpus]);return cpu;
}
static void gate(pthread_barrier_t *barrier)
{ int n=pthread_barrier_wait(barrier);CHECK(!n||n==PTHREAD_BARRIER_SERIAL_THREAD); }
static void exact(int fd,void *buffer,size_t count,int writing)
{
    char *p=buffer;
    while(count) {
        ssize_t n=writing?write(fd,p,count):read(fd,p,count);
        if(n<0&&errno==EINTR)continue;
        CHECK(n>0&&(size_t)n<=count);p+=n;count-=(size_t)n;
    }
}
static uint64_t work_checksum(unsigned id,unsigned rounds)
{ return (uint64_t)rounds*(rounds+1)/2*(id+1); }
static void report(const char *name,struct actor *a,unsigned count,uint64_t work,uint64_t bytes)
{
    uint64_t first=UINT64_MAX,last=0,cpu=0,child_cpu=0,checksum=0,completed=0;
    for(unsigned i=0;i<count;i++) {
        CHECK(a[i].begin&&a[i].end>=a[i].begin&&a[i].completed==a[i].rounds);
        CHECK(a[i].checksum==work_checksum(i,a[i].rounds));
        if(a[i].begin<first)first=a[i].begin;
        if(a[i].end>last)last=a[i].end;
        cpu+=a[i].cpu_ns;child_cpu+=a[i].child_cpu_ns;
        checksum+=a[i].checksum;completed+=a[i].completed;
        printf("SMP-MM ACTOR name=%s actor=%u cpu=%u affinity=%llx begin_ns=%llu end_ns=%llu cpu_ns=%llu child_cpu_ns=%llu completed=%u checksum=%llu last_ns=%llu\n",
            name,i,a[i].cpu,(unsigned long long)(UINT64_C(1)<<a[i].cpu),
            (unsigned long long)a[i].begin,(unsigned long long)a[i].end,
            (unsigned long long)a[i].cpu_ns,(unsigned long long)a[i].child_cpu_ns,
            a[i].completed,(unsigned long long)a[i].checksum,(unsigned long long)a[i].last_ns);
    }
    CHECK(last>first&&completed==work);
    printf("SMP-MM RESULT name=%s wall_ns=%llu cpu_ns=%llu actor_cpu_ns=%llu child_cpu_ns=%llu work=%llu bytes=%llu checks=%llu checksum=%llu workers=%u\n",
        name,(unsigned long long)(last-first),(unsigned long long)(cpu+child_cpu),
        (unsigned long long)cpu,(unsigned long long)child_cpu,(unsigned long long)work,
        (unsigned long long)bytes,(unsigned long long)completed,(unsigned long long)checksum,count);
    windows++;
}
static void prefault(struct actor *a)
{
    a->result=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(a->result!=MAP_FAILED);memset(a->result,0,page);
}
static void gettime_body(struct actor *a)
{
    uint64_t cpu=clock_ns(CLOCK_THREAD_CPUTIME_ID);a->begin=clock_ns(CLOCK_MONOTONIC);
    for(unsigned i=0;i<a->rounds;i++) {
        /* Bypass libc/vDSO so every iteration really copies a timespec through
         * the kernel into this actor's already writable private page. */
        a->result->tv_nsec=-1;
        CHECK(!syscall(SYS_clock_gettime,CLOCK_MONOTONIC,a->result));
        CHECK(a->result->tv_sec>=0&&a->result->tv_nsec>=0&&a->result->tv_nsec<1000000000);
        uint64_t value=(uint64_t)a->result->tv_sec*1000000000+(uint64_t)a->result->tv_nsec;
        CHECK(value>=a->last_ns);a->last_ns=value;
        a->checksum+=(uint64_t)(i+1)*(a->id+1);a->completed++;
    }
    a->end=clock_ns(CLOCK_MONOTONIC);a->cpu_ns=clock_ns(CLOCK_THREAD_CPUTIME_ID)-cpu;
}
static void *gettime_thread(void *opaque)
{
    struct actor *a=opaque;a->cpu=pin(a->id);prefault(a);gate(a->gate);gettime_body(a);
    CHECK(!munmap(a->result,page));a->result=NULL;return NULL;
}
static void *map_thread(void *opaque)
{
    struct actor *a=opaque;a->cpu=pin(a->id);gate(a->gate);
    uint64_t cpu=clock_ns(CLOCK_THREAD_CPUTIME_ID);a->begin=clock_ns(CLOCK_MONOTONIC);
    for(unsigned i=0;i<a->rounds;i++) {
        unsigned char *mapping=mmap(NULL,MAP_BYTES,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        CHECK(mapping!=MAP_FAILED);
        for(size_t at=0;at<MAP_BYTES;at+=page) {
            volatile uint64_t *value=(volatile uint64_t *)(mapping+at);
            uint64_t actual=*value;
            if(actual) {
                dprintf(2,"SMP-MM MAP FAIL actor=%u round=%u at=%zu virtual=%p expected=0x0 actual=0x%llx phase=zero\n",
                    a->id,i,at,(void *)value,(unsigned long long)actual);_exit(1);
            }
            *value=((uint64_t)(a->id+1)<<48)|((uint64_t)(i+1)<<16)|at;
        }
        for(size_t at=0;at<MAP_BYTES;at+=page) {
            volatile uint64_t *value=(volatile uint64_t *)(mapping+at);
            uint64_t expected=((uint64_t)(a->id+1)<<48)|((uint64_t)(i+1)<<16)|at,actual=*value;
            if(actual!=expected) {
                dprintf(2,"SMP-MM MAP FAIL actor=%u round=%u at=%zu virtual=%p expected=0x%llx actual=0x%llx phase=content\n",
                    a->id,i,at,(void *)value,(unsigned long long)expected,(unsigned long long)actual);_exit(1);
            }
        }
        CHECK(!munmap(mapping,MAP_BYTES));
        a->checksum+=(uint64_t)(i+1)*(a->id+1);a->completed++;
    }
    a->end=clock_ns(CLOCK_MONOTONIC);a->cpu_ns=clock_ns(CLOCK_THREAD_CPUTIME_ID)-cpu;return NULL;
}
static void shared_window(int mapping)
{
    unsigned total=mapping?(smoke?16:MAPS):(smoke?1024:GETTIMES);
    struct actor a[ACTORS]={0};pthread_t threads[ACTORS];pthread_barrier_t barrier;
    CHECK(!pthread_barrier_init(&barrier,NULL,ACTORS+1));
    for(unsigned i=0;i<ACTORS;i++) {
        a[i].id=i;a[i].rounds=total/ACTORS;a[i].gate=&barrier;
        CHECK(!pthread_create(&threads[i],NULL,mapping?map_thread:gettime_thread,&a[i]));
    }
    gate(&barrier);for(unsigned i=0;i<ACTORS;i++)CHECK(!pthread_join(threads[i],NULL));
    report(mapping?"map-shared":"clock-shared",a,ACTORS,total,
        (uint64_t)total*(mapping?MAP_BYTES:sizeof(struct timespec)));
    CHECK(!pthread_barrier_destroy(&barrier));
}
static void private_window(void)
{
    unsigned total=smoke?1024:GETTIMES;int ready[2],start[2],result[2];
    CHECK(!pipe(ready)&&!pipe(start)&&!pipe(result));pid_t children[ACTORS];
    for(unsigned i=0;i<ACTORS;i++) {
        children[i]=fork();CHECK(children[i]>=0);
        if(!children[i]) {
            CHECK(!close(ready[0])&&!close(start[1])&&!close(result[0]));
            struct actor a={.id=i,.rounds=total/ACTORS};a.cpu=pin(i);prefault(&a);
            char token='r';exact(ready[1],&token,1,1);exact(start[0],&token,1,0);CHECK(token=='s');
            gettime_body(&a);CHECK(!munmap(a.result,page));a.result=NULL;
            exact(result[1],&a,sizeof(a),1);CHECK(!close(ready[1])&&!close(start[0])&&!close(result[1]));_exit(0);
        }
    }
    CHECK(!close(ready[1])&&!close(start[0])&&!close(result[1]));char tokens[ACTORS];
    exact(ready[0],tokens,sizeof(tokens),0);for(unsigned i=0;i<ACTORS;i++)CHECK(tokens[i]=='r');
    memset(tokens,'s',sizeof(tokens));exact(start[1],tokens,sizeof(tokens),1);
    struct actor a[ACTORS]={0};unsigned seen=0;
    for(unsigned i=0;i<ACTORS;i++) {
        struct actor record;exact(result[0],&record,sizeof(record),0);
        CHECK(record.id<ACTORS&&!(seen&(1U<<record.id)));seen|=1U<<record.id;a[record.id]=record;
    }
    CHECK(seen==(1U<<ACTORS)-1);
    for(unsigned i=0;i<ACTORS;i++) {
        int status;CHECK(waitpid(children[i],&status,0)==children[i]&&WIFEXITED(status)&&!WEXITSTATUS(status));
    }
    CHECK(!close(ready[0])&&!close(start[1])&&!close(result[0]));
    report("clock-private",a,ACTORS,total,(uint64_t)total*sizeof(struct timespec));
}
static uint64_t usage_ns(const struct rusage *u)
{
    return (uint64_t)(u->ru_utime.tv_sec+u->ru_stime.tv_sec)*1000000000+
        (uint64_t)(u->ru_utime.tv_usec+u->ru_stime.tv_usec)*1000;
}
static void *lifecycle_thread(void *opaque)
{
    struct actor *a=opaque;a->cpu=pin(a->id);
    char *args[]={(char *)self,"--exec-child",NULL};gate(a->gate);
    uint64_t cpu=clock_ns(CLOCK_THREAD_CPUTIME_ID);a->begin=clock_ns(CLOCK_MONOTONIC);
    for(unsigned i=0;i<a->rounds;i++) {
        pid_t child=fork();CHECK(child>=0);
        if(!child) {execv(self,args);_exit(1);}
        int status;struct rusage usage;
        CHECK(wait4(child,&status,0,&usage)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==42);
        a->child_cpu_ns+=usage_ns(&usage);a->completed++;a->checksum+=(uint64_t)(i+1)*(a->id+1);
    }
    a->end=clock_ns(CLOCK_MONOTONIC);a->cpu_ns=clock_ns(CLOCK_THREAD_CPUTIME_ID)-cpu;return NULL;
}
static void lifecycle_window(void)
{
    unsigned total=smoke?4:LIFECYCLES;struct actor a[PARENTS]={0};pthread_t threads[PARENTS];pthread_barrier_t barrier;
    CHECK(!pthread_barrier_init(&barrier,NULL,PARENTS+1));
    for(unsigned i=0;i<PARENTS;i++) {
        a[i].id=i;a[i].rounds=total/PARENTS;a[i].gate=&barrier;
        CHECK(!pthread_create(&threads[i],NULL,lifecycle_thread,&a[i]));
    }
    gate(&barrier);for(unsigned i=0;i<PARENTS;i++)CHECK(!pthread_join(threads[i],NULL));
    report("fork-exec-wait",a,PARENTS,total,0);CHECK(!pthread_barrier_destroy(&barrier));
}
int main(int argc,char **argv)
{
    if(argc==2&&!strcmp(argv[1],"--exec-child")) {
        struct timespec t={.tv_nsec=-1};CHECK(syscall(SYS_getpid)==getpid());
        CHECK(!syscall(SYS_clock_gettime,CLOCK_MONOTONIC,&t)&&t.tv_sec>=0&&t.tv_nsec>=0&&t.tv_nsec<1000000000);return 42;
    }
    setvbuf(stdout,NULL,_IONBF,0);self=argv[0];CHECK(self&&*self&&access(self,X_OK)==0);
    unsigned requested=0,only_map=0;char profile[16]="formal";
    if(argc==1) {
        FILE *config=fopen("/smp-scale-cost","r");
        if(config) {int n=fscanf(config,"%u %15s",&requested,profile);CHECK(n==1||n==2);CHECK(!fclose(config));}
        else CHECK(errno==ENOENT);
        CHECK(!strcmp(profile,"formal")||!strcmp(profile,"smoke"));smoke=!strcmp(profile,"smoke");
    }
    for(int i=1;i<argc;i++) {
        if(!strcmp(argv[i],"--smoke"))smoke=1;
        else if(!strcmp(argv[i],"--only-map"))only_map=1;
        else {CHECK(!strcmp(argv[i],"--cpus")&&++i<argc);char *end;unsigned long n=strtoul(argv[i],&end,10);
            CHECK(*argv[i]&&!*end&&n<=ACTORS);requested=(unsigned)n;}
    }
    cpu_set_t allowed;CPU_ZERO(&allowed);CHECK(!sched_getaffinity(0,sizeof(allowed),&allowed));
    unsigned found=0;for(unsigned i=0;i<CPU_SETSIZE&&found<ACTORS;i++)if(CPU_ISSET(i,&allowed))cpu_ids[found++]=i;
    cpus=requested?requested:found;CHECK((cpus==1||cpus==2||cpus==4||cpus==8)&&cpus<=found);
    long size=sysconf(_SC_PAGESIZE);CHECK(size>0&&size<=MAP_BYTES&&MAP_BYTES%(unsigned long)size==0);page=(size_t)size;
    printf("SMP-MM BEGIN version=1 profile=%s cpus=%u actors=%u map_bytes=%u page_bytes=%zu cpu_ids=",smoke?"smoke":"formal",cpus,ACTORS,MAP_BYTES,page);
    for(unsigned i=0;i<cpus;i++)printf("%s%u",i?",":"",cpu_ids[i]);
    putchar('\n');
    if(only_map)shared_window(1);
    else {shared_window(0);shared_window(1);private_window();lifecycle_window();}
    CHECK(windows==(only_map?1U:4U));
    printf("SMP-MM END windows=%u profile=%s\n",windows,smoke?"smoke":"formal");return 0;
}
