#define _GNU_SOURCE
#include <sched.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdatomic.h>
#include <signal.h>
#include <setjmp.h>
#include <ucontext.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CHECK(x) do {if(!(x)){printf("LA-SMP FAIL line=%d errno=%d\n",__LINE__,errno);fflush(NULL);exit(1);}}while(0)
static unsigned cpus,cases;static size_t page;
static _Thread_local unsigned tls,handled,expected_fault;
static _Thread_local sigjmp_buf fault_return;
static uint64_t now(void){struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static void pin(unsigned cpu){cpu_set_t mask;CPU_ZERO(&mask);CPU_SET(cpu,&mask);CHECK(!sched_setaffinity(0,sizeof(mask),&mask));unsigned actual=99;CHECK(!syscall(SYS_getcpu,&actual,NULL,NULL)&&actual==cpu);}
static void barrier(pthread_barrier_t *b){int r=pthread_barrier_wait(b);CHECK(!r||r==PTHREAD_BARRIER_SERIAL_THREAD);}
static void wait_for(atomic_uint *value,unsigned expected){uint64_t end=now()+5000000000U;while(atomic_load_explicit(value,memory_order_acquire)!=expected)CHECK(now()<end);}
static void record(const char *name){printf("LA-SMP CASE %s PASS\n",name);cases++;}
static void joined(pid_t pid){int s;CHECK(waitpid(pid,&s,0)==pid && WIFEXITED(s) && !WEXITSTATUS(s));}
static void handler(int sig,siginfo_t *info,void *context)
{
    (void)info;
    if(sig==SIGTRAP){((ucontext_t *)context)->uc_mcontext.__pc+=4;return;}
    if(sig==SIGSEGV||sig==SIGBUS){CHECK(expected_fault==(unsigned)sig);expected_fault=0;siglongjmp(fault_return,1);}
    CHECK(tls==0x73);
    if(sig==SIGUSR1){handled++;CHECK(!raise(SIGUSR2));}else {CHECK(sig==SIGUSR2);handled++;}
}
int la_fp_register_probe(uint64_t);
int la_simd_probe(const uint64_t [32][4],uint64_t [32][4],unsigned);
struct actor {unsigned cpu;pthread_barrier_t *gate;atomic_uint *entered;uint64_t cpu_time;};
static void *execution(void *opaque)
{
    struct actor *a=opaque;pin(a->cpu);tls=0x73;handled=0;barrier(a->gate);
    atomic_fetch_or_explicit(a->entered,1U<<a->cpu,memory_order_release);
    wait_for(a->entered,(1U<<cpus)-1);
    struct rusage before,after;CHECK(!getrusage(RUSAGE_THREAD,&before));
    CHECK(!la_fp_register_probe(UINT64_C(0x1234000000000000)+a->cpu*256));
    uint64_t input[32][4] __attribute__((aligned(32))),output[32][4] __attribute__((aligned(32)));
    for(unsigned width=2;width<=4;width*=2) {
        for(unsigned i=0;i<32;i++)for(unsigned j=0;j<4;j++)input[i][j]=0x735000+a->cpu*4096+i*4+j;
        CHECK(!la_simd_probe(input,output,width));
        for(unsigned i=0;i<32;i++)for(unsigned j=0;j<width;j++)CHECK(output[i][j]==input[i][j]);
    }
    for(unsigned i=0;i<32;i++)CHECK(!raise(SIGUSR1) && tls==0x73);
    CHECK(handled==64 && !getrusage(RUSAGE_THREAD,&after));
    a->cpu_time=(after.ru_utime.tv_sec-before.ru_utime.tv_sec)*1000000+after.ru_utime.tv_usec-before.ru_utime.tv_usec;
    CHECK(a->cpu_time>0);return NULL;
}
static void executors(void)
{
    pthread_barrier_t gate;CHECK(!pthread_barrier_init(&gate,NULL,cpus));atomic_uint entered=0;
    pthread_t threads[2];struct actor actors[2];
    for(unsigned i=0;i<cpus;i++){actors[i]=(struct actor){i,&gate,&entered,0};CHECK(!pthread_create(&threads[i],NULL,execution,&actors[i]));}
    for(unsigned i=0;i<cpus;i++){CHECK(!pthread_join(threads[i],NULL));}
    CHECK(!pthread_barrier_destroy(&gate));
    record("executors-tls-fpu-simd-signals-accounting");
}
struct remote {unsigned char *address;atomic_uint stage;};
static void fault_read(unsigned char *p,unsigned sig)
{
    expected_fault=sig;
    if(!sigsetjmp(fault_return,1)){volatile unsigned char byte=*p;(void)byte;CHECK(0);}
    CHECK(!expected_fault);
}
static void *remote_reader(void *opaque)
{
    struct remote *r=opaque;pin(cpus-1);CHECK(r->address[0]==17);atomic_store(&r->stage,1);
    wait_for(&r->stage,2);fault_read(r->address,SIGSEGV);atomic_store(&r->stage,3);
    wait_for(&r->stage,4);CHECK(r->address[0]==83);atomic_store(&r->stage,5);
    wait_for(&r->stage,6);fault_read(r->address,SIGSEGV);atomic_store(&r->stage,7);return NULL;
}
static void mappings(void)
{
    struct remote r={.address=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0)};
    CHECK(r.address!=MAP_FAILED);r.address[0]=17;pthread_t thread;CHECK(!pthread_create(&thread,NULL,remote_reader,&r));
    wait_for(&r.stage,1);CHECK(!mprotect(r.address,page,PROT_NONE));atomic_store(&r.stage,2);
    wait_for(&r.stage,3);CHECK(mmap(r.address,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==r.address);r.address[0]=83;atomic_store(&r.stage,4);
    wait_for(&r.stage,5);CHECK(!munmap(r.address,page));atomic_store(&r.stage,6);
    CHECK(!pthread_join(thread,NULL) && atomic_load(&r.stage)==7);record("shared-mm-protect-replace-unmap");
}
struct file_actor {unsigned cpu;pthread_barrier_t *gate;};
static void *files(void *opaque)
{
    struct file_actor *a=opaque;pin(a->cpu);barrier(a->gate);char directory[32],path[64];
    snprintf(directory,sizeof(directory),"/smp-%u",a->cpu);CHECK(!mkdir(directory,0700));
    for(unsigned i=0;i<16;i++) {
        snprintf(path,sizeof(path),"%s/item-%u",directory,i);int fd=open(path,O_CREAT|O_EXCL|O_RDWR,0600);CHECK(fd>=0);
        unsigned char data[4096],output[4096];memset(data,31+a->cpu,sizeof(data));
        CHECK(pwrite(fd,data,sizeof(data),0)==sizeof(data) && pread(fd,output,sizeof(output),0)==sizeof(output) && !memcmp(data,output,sizeof(data)));
        CHECK(!ftruncate(fd,page));unsigned char *mapped=mmap(NULL,page,PROT_READ,MAP_SHARED,fd,0);CHECK(mapped!=MAP_FAILED && mapped[0]==data[0]);
        CHECK(!unlink(path) && !ftruncate(fd,0));fault_read(mapped,SIGBUS);
        CHECK(!munmap(mapped,page) && !fsync(fd) && !close(fd));
    }
    int directory_fd=open(directory,O_RDONLY|O_DIRECTORY);CHECK(directory_fd>=0 && !fsync(directory_fd) && !close(directory_fd) && !rmdir(directory));return NULL;
}
static void filesystem(void)
{
    pthread_barrier_t gate;CHECK(!pthread_barrier_init(&gate,NULL,cpus));pthread_t threads[2];struct file_actor actors[2];
    for(unsigned i=0;i<cpus;i++){actors[i]=(struct file_actor){i,&gate};CHECK(!pthread_create(&threads[i],NULL,files,&actors[i]));}
    for(unsigned i=0;i<cpus;i++){CHECK(!pthread_join(threads[i],NULL));}
    CHECK(!pthread_barrier_destroy(&gate));
    record("ext4-parallel-files-mmap-orphans");
}
static atomic_uint turn;
static void *futex_peer(void *unused)
{
    (void)unused;pin(cpus-1);
    for(unsigned i=0;i<256;i++) {
        for(;;){unsigned value=atomic_load(&turn);if(value==1)break;long r=syscall(SYS_futex,&turn,128,0,NULL,NULL,0);CHECK(!r || errno==EAGAIN || errno==EINTR);}
        atomic_store(&turn,0);CHECK(syscall(SYS_futex,&turn,129,1,NULL,NULL,0)>=0);
    }return NULL;
}
static void synchronization(void)
{
    atomic_store(&turn,0);pthread_t thread;CHECK(!pthread_create(&thread,NULL,futex_peer,NULL));
    for(unsigned i=0;i<256;i++) {
        atomic_store(&turn,1);CHECK(syscall(SYS_futex,&turn,129,1,NULL,NULL,0)>=0);
        for(;;){unsigned value=atomic_load(&turn);if(value==0)break;long r=syscall(SYS_futex,&turn,128,1,NULL,NULL,0);CHECK(!r || errno==EAGAIN || errno==EINTR);}
    }CHECK(!pthread_join(thread,NULL));record("remote-futex-wait-wake");
}
static volatile sig_atomic_t virtual_signals,prof_signals;
static void timer_signal(int sig){if(sig==SIGVTALRM)virtual_signals++;else {CHECK(sig==SIGPROF);prof_signals++;}}
static void *timer_worker(void *unused)
{
    (void)unused;pin(cpus-1);struct rusage use;uint64_t value=1,end=now()+5000000000U;
    for(;;){for(unsigned i=0;i<100000;i++){value=(value<<3)^(value>>7)^17;__asm__ volatile("":"+r"(value));}CHECK(!getrusage(RUSAGE_THREAD,&use));if(use.ru_utime.tv_sec*1000000+use.ru_utime.tv_usec>60000)break;CHECK(now()<end);}
    return NULL;
}
static void timers(void)
{
    struct sigaction a={.sa_handler=timer_signal};sigemptyset(&a.sa_mask);CHECK(!sigaction(SIGVTALRM,&a,NULL)&&!sigaction(SIGPROF,&a,NULL));
    struct itimerval tick={.it_value={0,10000},.it_interval={0,10000}},zero={0};
    CHECK(!setitimer(ITIMER_VIRTUAL,&tick,NULL)&&!setitimer(ITIMER_PROF,&tick,NULL));pthread_t t;CHECK(!pthread_create(&t,NULL,timer_worker,NULL)&&!pthread_join(t,NULL));
    CHECK(virtual_signals && prof_signals && !setitimer(ITIMER_VIRTUAL,&zero,NULL)&&!setitimer(ITIMER_PROF,&zero,NULL));record("group-virtual-prof-timers");
}
int main(int argc,char **argv)
{
    if(argc==3 && !strcmp(argv[1],"--exec-child")) {
        unsigned expected=(unsigned)atoi(argv[2]),actual=99;cpu_set_t mask;
        CHECK(!sched_getaffinity(0,sizeof(mask),&mask) && CPU_ISSET(expected,&mask) && CPU_COUNT(&mask)==1);
        CHECK(!syscall(SYS_getcpu,&actual,NULL,NULL) && actual==expected);return 0;
    }
    page=(size_t)sysconf(_SC_PAGESIZE);CHECK(page==16384);cpu_set_t mask;
    for(unsigned i=0;i<2;i++){CPU_ZERO(&mask);CPU_SET(i,&mask);if(sched_setaffinity(0,sizeof(mask),&mask)){CHECK(errno==EINVAL);break;}cpus++;}
    CHECK(cpus);pin(0);printf("LA-SMP BEGIN cpus=%u\n",cpus);
    for(unsigned i=0;i<cpus;i++){pin(i);}
    pin(0);record("remote-affinity");
    struct sigaction action={.sa_sigaction=handler,.sa_flags=SA_SIGINFO|SA_NODEFER};sigemptyset(&action.sa_mask);
    int signals[]={SIGUSR1,SIGUSR2,SIGTRAP,SIGSEGV,SIGBUS};for(unsigned i=0;i<5;i++)CHECK(!sigaction(signals[i],&action,NULL));
    executors();mappings();synchronization();timers();filesystem();
    unsigned char *private=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(private!=MAP_FAILED);private[0]=17;
    pid_t child=fork();CHECK(child>=0);if(!child){pin(cpus-1);private[0]=83;char cpu[2]={(char)('0'+cpus-1),0};execl("/init","init","--exec-child",cpu,(char *)NULL);_exit(99);}joined(child);CHECK(private[0]==17 && !munmap(private,page));record("fork-cow-affinity-exec-exit");
    printf("LA-SMP END %u\n",cases);return 0;
}
