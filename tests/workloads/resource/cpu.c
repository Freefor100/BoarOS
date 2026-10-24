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
#include <sys/stat.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"CPU FAIL line=%d expr=%s errno=%d\n",__LINE__,#x,errno); _exit(1); } } while (0)
static unsigned cases;
static void pass(const char *name) { printf("CPU CASE %s PASS\n",name); cases++; }
static uint64_t stamp(clockid_t clock)
{ struct timespec t; CHECK(!clock_gettime(clock,&t)); return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
static void child_ok(pid_t child)
{ int s; CHECK(waitpid(child,&s,0)==child && WIFEXITED(s) && !WEXITSTATUS(s)); }
static volatile sig_atomic_t notifications;
static void noted(int signal) { (void)signal; notifications++; }
static void spin(void)
{ for (;;) __asm__ volatile("" ::: "memory"); }
static void getcpu_case(void)
{
    cpu_set_t original; CHECK(!sched_getaffinity(0,sizeof(original),&original));
    for(unsigned i=0;i<CPU_SETSIZE;i++) if(CPU_ISSET(i,&original)) {
        cpu_set_t one; CPU_ZERO(&one); CPU_SET(i,&one);
        CHECK(!sched_setaffinity(0,sizeof(one),&one));
        unsigned cpu=~0U,node=~0U;
        CHECK(!syscall(SYS_getcpu,&cpu,&node,(void *)1) && cpu==i && node==0);
        node=~0U; errno=0;
        CHECK(syscall(SYS_getcpu,(void *)1,&node,0)==-1 && errno==EFAULT && node==0);
        cpu=~0U; errno=0;
        CHECK(syscall(SYS_getcpu,&cpu,(void *)1,0)==-1 && errno==EFAULT && cpu==i);
        CHECK(!syscall(SYS_getcpu,0,0,(void *)1));
        printf("CPU EXEC index=%u PASS\n",i);
    }
    CHECK(!sched_setaffinity(0,sizeof(original),&original)); pass("getcpu-prefix-local");
}
static void limit_state(void)
{
    struct rlimit old; CHECK(!getrlimit(RLIMIT_CPU,&old));
    CHECK(old.rlim_cur==RLIM_INFINITY && old.rlim_max==RLIM_INFINITY);
    struct rlimit value={20,30},read;
    CHECK(!setrlimit(RLIMIT_CPU,&value) && !getrlimit(RLIMIT_CPU,&read));
    CHECK(read.rlim_cur==20 && read.rlim_max==30);
    struct rlimit invalid={31,30}; errno=0;
    CHECK(setrlimit(RLIMIT_CPU,&invalid)==-1 && errno==EINVAL);
    pid_t child=fork(); CHECK(child>=0);
    if(!child) { CHECK(!getrlimit(RLIMIT_CPU,&read) && read.rlim_cur==20 && read.rlim_max==30); value.rlim_cur=19; CHECK(!setrlimit(RLIMIT_CPU,&value)); _exit(0); }
    child_ok(child); CHECK(!getrlimit(RLIMIT_CPU,&read) && read.rlim_cur==20);
    CHECK(!setrlimit(RLIMIT_CPU,&old)); pass("limit-default-set-fork");
}
static void soft_limit(void)
{
    pid_t child=fork(); CHECK(child>=0);
    if(!child) {
        struct sigaction a={.sa_handler=noted}; CHECK(!sigaction(SIGXCPU,&a,0));
        struct rlimit value={1,2}; CHECK(!setrlimit(RLIMIT_CPU,&value));
        while(!notifications) __asm__ volatile("" ::: "memory");
        CHECK(notifications==1 && !getrlimit(RLIMIT_CPU,&value) && value.rlim_cur==2);
        uint64_t used=stamp(CLOCK_PROCESS_CPUTIME_ID);
        printf("CPU-METRIC soft-observed-ns=%llu\n",(unsigned long long)used);
        /* Linux quota uses PROF accounting, while this public clock samples
         * SCHED runtime; allow tick-accounting precision between the clocks. */
        CHECK(used>=900000000 && used<2100000000); _exit(0);
    }
    child_ok(child); pass("limit-soft-signal-and-update");
}
static void hard_limit(void)
{
    pid_t child=fork(); CHECK(child>=0);
    if(!child) { struct rlimit value={1,1}; CHECK(!setrlimit(RLIMIT_CPU,&value)); spin(); }
    int s; CHECK(waitpid(child,&s,0)==child && WIFSIGNALED(s) && WTERMSIG(s)==SIGKILL);
    pass("limit-hard-kill");
}
static void sleep_abi(void)
{
    struct timespec zero={0},remain={71,72};
    CHECK(clock_nanosleep(CLOCK_PROCESS_CPUTIME_ID,TIMER_ABSTIME,&zero,&remain)==0);
    CHECK(remain.tv_sec==71 && remain.tv_nsec==72);
    errno=0;
    CHECK(syscall(SYS_clock_nanosleep,CLOCK_THREAD_CPUTIME_ID,0,&zero,0)==-1 && errno==EOPNOTSUPP);
    CHECK(clock_nanosleep(CLOCK_THREAD_CPUTIME_ID,0,&zero,0)==EINVAL);
    clockid_t self=(clockid_t)((~(uint32_t)syscall(SYS_gettid)<<3)|6U);
    CHECK(clock_nanosleep(self,0,&zero,0)==EINVAL);
    CHECK(clock_nanosleep(CLOCK_PROCESS_CPUTIME_ID,0,(void *)1,0)==EFAULT);
    struct timespec invalid={0,1000000000}; CHECK(clock_nanosleep(CLOCK_PROCESS_CPUTIME_ID,0,&invalid,0)==EINVAL);
    pass("cpu-sleep-abi");
}
struct worker { pthread_barrier_t gate; unsigned stop; uint64_t check; };
static void *compute(void *p)
{
    struct worker *w=p; int e=pthread_barrier_wait(&w->gate); CHECK(!e || e==PTHREAD_BARRIER_SERIAL_THREAD);
    uint64_t value=7; while(!__atomic_load_n(&w->stop,__ATOMIC_ACQUIRE)) { value=value*6364136223846793005ULL+1; __asm__ volatile("" : "+r"(value)); }
    w->check=value; return 0;
}
static void cpu_sleep(void)
{
    for(unsigned kind=0;kind<2;kind++) {
        struct worker w={0}; pthread_t thread; CHECK(!pthread_barrier_init(&w.gate,0,2) && !pthread_create(&thread,0,compute,&w));
        clockid_t clock=CLOCK_PROCESS_CPUTIME_ID; if(kind) CHECK(!pthread_getcpuclockid(thread,&clock));
        int e=pthread_barrier_wait(&w.gate); CHECK(!e || e==PTHREAD_BARRIER_SERIAL_THREAD);
        uint64_t target=stamp(clock)+20000000; struct timespec request={target/1000000000,target%1000000000};
        CHECK(!clock_nanosleep(clock,TIMER_ABSTIME,&request,0)); CHECK(stamp(clock)>=target);
        __atomic_store_n(&w.stop,1,__ATOMIC_RELEASE); CHECK(!pthread_join(thread,0) && !pthread_barrier_destroy(&w.gate));
    }
    pass("cpu-sleep-process-and-other-thread");
}
static void *limit_thread(void *unused)
{
    (void)unused;struct rlimit value;
    CHECK(!getrlimit(RLIMIT_CPU,&value)&&value.rlim_cur==20&&value.rlim_max==30);
    value.rlim_cur=19;CHECK(!setrlimit(RLIMIT_CPU,&value));return 0;
}
static void *limit_exec(void *unused)
{
    (void)unused;execl("/init","cpu","--limit-exec",(char *)0);CHECK(0);return 0;
}
static void limit_lifetime(void)
{
    for(unsigned nonleader=0;nonleader<2;nonleader++) {
        pid_t child=fork();CHECK(child>=0);
        if(!child) {
            struct rlimit value={20,30};pthread_t thread;
            CHECK(!setrlimit(RLIMIT_CPU,&value)&&!pthread_create(&thread,0,limit_thread,0));
            CHECK(!pthread_join(thread,0)&&!getrlimit(RLIMIT_CPU,&value)&&value.rlim_cur==19);
            if(nonleader){CHECK(!pthread_create(&thread,0,limit_exec,0));pthread_join(thread,0);CHECK(0);}
            else limit_exec(0);
        }
        child_ok(child);
    }
    pass("limit-thread-and-exec");
}
static void sleep_interrupt(void)
{
    struct sigaction action={.sa_handler=noted,.sa_flags=SA_RESTART},old;
    CHECK(!sigaction(SIGALRM,&action,&old));
    for(unsigned mode=0;mode<3;mode++) {
        struct itimerval alarm={{0,0},{0,20000}};
        struct timespec request={60,0},remaining={71,72};
        if(mode==1){uint64_t target=stamp(CLOCK_PROCESS_CPUTIME_ID)+60000000000ULL;request=(struct timespec){target/1000000000,target%1000000000};}
        CHECK(!setitimer(ITIMER_REAL,&alarm,0));
        int result=clock_nanosleep(CLOCK_PROCESS_CPUTIME_ID,mode==1?TIMER_ABSTIME:0,&request,mode==2?(void *)1:&remaining);
        CHECK(result==(mode==2?EFAULT:EINTR));
        if(mode==0)CHECK(remaining.tv_sec>=59&&remaining.tv_sec<=60);
        if(mode==1)CHECK(remaining.tv_sec==71&&remaining.tv_nsec==72);
    }
    CHECK(!sigaction(SIGALRM,&old,0));pass("cpu-sleep-interrupt-and-output-fault");
}
static void wait_sleeping(pid_t pid,pid_t tid)
{
    char path[80],status[256];snprintf(path,sizeof(path),"/proc/%d/task/%d/stat",pid,tid);
    for(;;) {
        int fd=open(path,O_RDONLY);CHECK(fd>=0);ssize_t size=read(fd,status,sizeof(status)-1);CHECK(size>0&&!close(fd));status[size]=0;
        char *end=strrchr(status,')');CHECK(end&&end[1]==' ');if(end[2]=='S')return;sched_yield();
    }
}
struct exit_target {pthread_barrier_t gate;pid_t caller;};
static void *exit_clock_target(void *arg)
{
    struct exit_target *target=arg;int e=pthread_barrier_wait(&target->gate);CHECK(!e||e==PTHREAD_BARRIER_SERIAL_THREAD);
    wait_sleeping(getpid(),target->caller);return 0;
}
static void sleep_exit(void)
{
    struct sigaction action={.sa_handler=noted},old;CHECK(!sigaction(SIGALRM,&action,&old));
    struct exit_target target={.caller=syscall(SYS_gettid)};pthread_t thread;clockid_t clock;
    CHECK(!pthread_barrier_init(&target.gate,0,2)&&!pthread_create(&thread,0,exit_clock_target,&target));
    CHECK(!pthread_getcpuclockid(thread,&clock));int e=pthread_barrier_wait(&target.gate);CHECK(!e||e==PTHREAD_BARRIER_SERIAL_THREAD);
    struct itimerval alarm={{0,0},{0,100000}};struct timespec request={60,0};
    CHECK(!setitimer(ITIMER_REAL,&alarm,0));
    /* Target exit detaches the CPU deadline, while a later signal must still
     * interrupt the caller rather than turn target exit into timer expiry. */
    struct timespec remaining={0};
    CHECK(clock_nanosleep(clock,0,&request,&remaining)==EINTR);
    CHECK(remaining.tv_sec==60&&!remaining.tv_nsec);
    CHECK(!pthread_join(thread,0)&&!pthread_barrier_destroy(&target.gate)&&!sigaction(SIGALRM,&old,0));
    pid_t child=fork();CHECK(child>=0);
    if(!child){struct timespec duration={60,0};clock_nanosleep(CLOCK_PROCESS_CPUTIME_ID,0,&duration,0);CHECK(0);}
    wait_sleeping(child,child);CHECK(!kill(child,SIGKILL));int status;CHECK(waitpid(child,&status,0)==child&&WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);
    pass("cpu-sleep-target-exit-and-caller-kill");
}
static void absolute_stop_restart(void)
{
    struct timespec *request=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);CHECK(request!=MAP_FAILED);
    for(unsigned mode=0;mode<2;mode++) {
        clockid_t clock=mode?CLOCK_MONOTONIC:CLOCK_PROCESS_CPUTIME_ID;
        uint64_t target=stamp(clock)+60000000000ULL;*request=(struct timespec){target/1000000000,target%1000000000};
        pid_t child=fork();CHECK(child>=0);
        if(!child){alarm(3);CHECK(!clock_nanosleep(clock,TIMER_ABSTIME,request,0));_exit(0);}
        wait_sleeping(child,child);CHECK(!kill(child,SIGSTOP));int status;
        CHECK(waitpid(child,&status,WUNTRACED)==child&&WIFSTOPPED(status));
        *request=(struct timespec){0};CHECK(!kill(child,SIGCONT));child_ok(child);
    }
    CHECK(!munmap(request,4096));pass("absolute-sleep-stop-reloads-request");
}
int main(int argc,char **argv)
{
    if(argc==2&&!strcmp(argv[1],"--limit-exec")) {struct rlimit value;CHECK(!getrlimit(RLIMIT_CPU,&value)&&value.rlim_cur==19&&value.rlim_max==30);return 0;}
    setvbuf(stdout,0,_IONBF,0); alarm(15);
    mkdir("/proc",0755);CHECK(!mount("proc","/proc","proc",0,0)||errno==EBUSY);
    puts("CPU BEGIN"); getcpu_case(); limit_state(); soft_limit(); hard_limit(); sleep_abi(); cpu_sleep(); limit_lifetime(); sleep_interrupt(); sleep_exit(); absolute_stop_restart();
    printf("CPU END cases=%u\n",cases); alarm(0);
    if(getpid()==1) { sync(); reboot(RB_POWER_OFF); } return 0;
}
