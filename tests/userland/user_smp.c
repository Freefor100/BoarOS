#define _GNU_SOURCE
#include <assert.h>
#ifdef BOAROS_SMP_DYNAMIC
#include <dlfcn.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { dprintf(2,"USER-SMP FAIL line=%d errno=%d\n",__LINE__,errno); exit(1); } } while (0)
static uint64_t online;
static _Thread_local unsigned tls_value;
static _Thread_local sigjmp_buf fault_return;
static _Thread_local int expected_fault;
static atomic_uint signal_seen, nested_seen;
extern int user_smp_register_probe(unsigned);
static void record(const char *name) { printf("USER-SMP CASE %s OK\n",name); fflush(stdout); }
static void pin(unsigned cpu)
{
    uint64_t mask=UINT64_C(1)<<cpu, actual=0;
    CHECK(!syscall(SYS_sched_setaffinity,0,sizeof(mask),&mask));
    CHECK(syscall(SYS_sched_getaffinity,0,sizeof(actual),&actual)==sizeof(actual) && actual==mask);
}
static void wait_flag(atomic_uint *flag, unsigned value)
{ while (atomic_load_explicit(flag,memory_order_acquire)!=value) __asm__ volatile("" ::: "memory"); }
static void signal_handler(int number, siginfo_t *info, void *context)
{
    (void)info; (void)context;
    if (number==SIGSEGV || number==SIGBUS) {
        CHECK(number==expected_fault);
        siglongjmp(fault_return,1);
    }
    CHECK(tls_value==73);
    volatile double fp=1.25; fp+=2.5; CHECK(fp==3.75);
    if (number==SIGUSR1) {
        CHECK(!raise(SIGUSR2));
        atomic_store_explicit(&signal_seen,1,memory_order_release);
    } else atomic_store_explicit(&nested_seen,1,memory_order_release);
}
struct shared_run { atomic_uint started[2], done[2]; uint64_t checksum[2]; };
static void compute(struct shared_run *run,unsigned id)
{
    pin(id && (online&2) ? 1 : 0);
    atomic_store_explicit(&run->started[id],1,memory_order_release);
    wait_flag(&run->started[1-id],1);
    uint64_t value=UINT64_C(0x123456789abcdef);
    /* 不调用yield/syscall；两个显式资格的用户计算循环必须均有进展。 */
    for (unsigned i=0;i<10000000;i++) value=(value<<7)^(value>>3)^i;
    run->checksum[id]=value;
    atomic_store_explicit(&run->done[id],1,memory_order_release);
    wait_flag(&run->done[1-id],1);
}
static void process_overlap(void)
{
    struct shared_run *run=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_ANONYMOUS|MAP_SHARED,-1,0);
    CHECK(run!=MAP_FAILED);
    pid_t children[2];
    for (unsigned i=0;i<2;i++) {
        children[i]=fork(); CHECK(children[i]>=0);
        if (!children[i]) { compute(run,i); _exit(0); }
    }
    for (unsigned i=0;i<2;i++) { int status; CHECK(waitpid(children[i],&status,0)==children[i] && status==0); }
    CHECK(run->checksum[0]==run->checksum[1] && run->checksum[0]);
    CHECK(!munmap(run,4096)); record("process-affinity-overlap");
}
static atomic_uint receiver_started, receiver_done;
static void *receiver(void *unused)
{
    (void)unused; pin(online&2 ? 1 : 0); tls_value=73;
    CHECK(!user_smp_register_probe(0));
    atomic_store_explicit(&receiver_started,1,memory_order_release);
    wait_flag(&signal_seen,1);
    CHECK(atomic_load_explicit(&nested_seen,memory_order_acquire)==1);
    CHECK(tls_value==73 && !user_smp_register_probe(1));
    atomic_store_explicit(&receiver_done,1,memory_order_release);
    return (void *)(uintptr_t)73;
}
static void thread_signal(void)
{
    struct sigaction action={.sa_sigaction=signal_handler,.sa_flags=SA_SIGINFO|SA_NODEFER};
    sigemptyset(&action.sa_mask);
    CHECK(!sigaction(SIGUSR1,&action,0) && !sigaction(SIGUSR2,&action,0));
    pthread_t thread; CHECK(!pthread_create(&thread,0,receiver,0));
    wait_flag(&receiver_started,1);
    CHECK(!pthread_kill(thread,SIGUSR1));
    void *result; CHECK(!pthread_join(thread,&result) && result==(void *)(uintptr_t)73);
    CHECK(atomic_load_explicit(&receiver_done,memory_order_acquire));
    record("pthread-tls-registers-nested-signal");
}
struct mapping_case { unsigned char *page; atomic_uint ready, command, result; int file; };
static void *mapping_reader(void *opaque)
{
    struct mapping_case *test=opaque;
    pin(online&2 ? 1 : 0);
    CHECK(test->page[0]==19);
    atomic_store_explicit(&test->ready,1,memory_order_release);
    wait_flag(&test->command,1);
    expected_fault=test->file ? SIGBUS : SIGSEGV;
    if (!sigsetjmp(fault_return,1)) {
        if (test->file) { volatile unsigned char read=test->page[0]; (void)read; }
        else test->page[0]=27;
        CHECK(0); /* 撤权限/EOF后若旧翻译还能使用，必须失败。 */
    }
    atomic_store_explicit(&test->result,1,memory_order_release);
    return 0;
}
static void mapping_retirement(void)
{
    struct sigaction action={.sa_sigaction=signal_handler,.sa_flags=SA_SIGINFO};
    sigemptyset(&action.sa_mask);
    CHECK(!sigaction(SIGSEGV,&action,0) && !sigaction(SIGBUS,&action,0));
    for (unsigned file=0;file<2;file++) {
        int fd=-1;
        if (file) { fd=open("/smp-mapped",O_CREAT|O_RDWR|O_TRUNC,0600); CHECK(fd>=0 && !ftruncate(fd,8192)); }
        unsigned char *mapping=mmap(0,8192,PROT_READ|PROT_WRITE,
            file ? MAP_SHARED : MAP_PRIVATE|MAP_ANONYMOUS,fd,0);
        CHECK(mapping!=MAP_FAILED); mapping[4096]=19;
        struct mapping_case test={.page=mapping+4096,.file=file};
        pthread_t thread; CHECK(!pthread_create(&thread,0,mapping_reader,&test));
        wait_flag(&test.ready,1);
        if (file) CHECK(!fsync(fd) && !ftruncate(fd,4096));
        else CHECK(!mprotect(mapping+4096,4096,PROT_READ));
        atomic_store_explicit(&test.command,1,memory_order_release);
        CHECK(!pthread_join(thread,0) && atomic_load_explicit(&test.result,memory_order_acquire));
        if (file) { CHECK(!ftruncate(fd,8192) && mapping[4096]==0); CHECK(!close(fd)); }
        CHECK(!munmap(mapping,8192));
    }
    record("shared-mm-remote-permission-and-truncate");
}
static atomic_uint io_ready;
static int io_pipe[2];
static void *cancel_reader(void *unused)
{
    (void)unused; pin(online&2 ? 1 : 0);
    atomic_store_explicit(&io_ready,1,memory_order_release);
    char byte; (void)read(io_pipe[0],&byte,1); CHECK(0); return 0;
}
static void cancellation(void)
{
    CHECK(!pipe(io_pipe)); pthread_t thread; CHECK(!pthread_create(&thread,0,cancel_reader,0));
    wait_flag(&io_ready,1); CHECK(!pthread_cancel(thread));
    void *result; CHECK(!pthread_join(thread,&result) && result==PTHREAD_CANCELED);
    CHECK(!close(io_pipe[0]) && !close(io_pipe[1])); record("pthread-cancel-pipe-owner");
}
static void *exec_thread(void *unused)
{
    (void)unused; pin(online&2 ? 1 : 0);
    char *args[]={"/init","--exec-child",0}; execv(args[0],args); CHECK(0); return 0;
}
static void nonleader_exec(void)
{
    pid_t child=fork(); CHECK(child>=0);
    if (!child) { pin(online&2 ? 1 : 0); pthread_t thread; CHECK(!pthread_create(&thread,0,exec_thread,0)); for (;;) __asm__ volatile("" ::: "memory"); }
    int status; CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==23);
    record("nonleader-exec-remote-exit");
}
struct stop_case { atomic_uint ready,finished,progress; };
static void *stop_thread(void *opaque)
{
    struct stop_case *state=opaque; pin(online&2 ? 1 : 0);
    atomic_store_explicit(&state->ready,1,memory_order_release);
    while (!atomic_load_explicit(&state->finished,memory_order_acquire))
        atomic_fetch_add_explicit(&state->progress,1,memory_order_relaxed);
    return 0;
}
static void group_stop_continue(void)
{
    struct stop_case *state=mmap(0,4096,PROT_READ|PROT_WRITE,
        MAP_SHARED|MAP_ANONYMOUS,-1,0); CHECK(state!=MAP_FAILED);
    pid_t child=fork(); CHECK(child>=0);
    if (!child) {
        pthread_t thread; CHECK(!pthread_create(&thread,0,stop_thread,state));
        wait_flag(&state->finished,1); CHECK(!pthread_join(thread,0)); _exit(0);
    }
    wait_flag(&state->ready,1); CHECK(!kill(child,SIGSTOP));
    int status; CHECK(waitpid(child,&status,WUNTRACED)==child && WIFSTOPPED(status) && WSTOPSIG(status)==SIGSTOP);
    unsigned before=atomic_load_explicit(&state->progress,memory_order_relaxed);
    for (unsigned i=0;i<16;i++) CHECK(syscall(SYS_getpid)>0);
    CHECK(atomic_load_explicit(&state->progress,memory_order_relaxed)==before);
    CHECK(!kill(child,SIGCONT));
    while (atomic_load_explicit(&state->progress,memory_order_relaxed)==before) {}
    atomic_store_explicit(&state->finished,1,memory_order_release);
    CHECK(waitpid(child,&status,0)==child && status==0);
    CHECK(!munmap(state,4096)); record("group-stop-continue-remote-owner");
}
static uint64_t monotonic_ns(void)
{
    struct timespec now; CHECK(!clock_gettime(CLOCK_MONOTONIC,&now));
    return (uint64_t)now.tv_sec*1000000000U+now.tv_nsec;
}
static void set_policy(int policy,int priority)
{
    struct sched_param parameter={.sched_priority=priority};
    /* 原musl的sched_setscheduler是ENOSYS包装；验证真实内核syscall。 */
    CHECK(!syscall(SYS_sched_setscheduler,0,policy,&parameter));
}
static void reap(pid_t child)
{ int status; CHECK(waitpid(child,&status,0)==child && status==0); }
static void realtime_qualification(void)
{
    atomic_uint *seen=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_ANONYMOUS|MAP_SHARED,-1,0);
    CHECK(seen!=MAP_FAILED);
    for (unsigned cpu=0;cpu<(online==3 ? 2U : 1U);cpu++) {
        pin(cpu);
        /* 库代码的首次执行可真实缺页/睡眠；在FIFO非阻塞窗口之前
         * 完成fork返回与时钟/syscall代码的驻留准备，不把冷I/O当成yield。 */
        pid_t warm=fork(); CHECK(warm>=0);
        if (!warm) _exit(0);
        reap(warm); CHECK(monotonic_ns() && syscall(SYS_getpid)>0);
        for (unsigned block=0;block<2;block++) {
            atomic_store(seen,0); set_policy(SCHED_FIFO,40);
            pid_t child=fork(); CHECK(child>=0);
            if (!child) { atomic_store_explicit(seen,1,memory_order_release); _exit(0); }
            uint64_t until=monotonic_ns()+10000000U;
            do { CHECK(syscall(SYS_getpid)>0); } while (monotonic_ns()<until);
            /* 非阻塞服务不是yield：同级子任务仍不能进入用户态。 */
            CHECK(!atomic_load_explicit(seen,memory_order_acquire));
            if (!block) CHECK(!sched_yield());
            reap(child); /* 真正阻塞必须归还来源CPU资格。 */
            CHECK(atomic_load_explicit(seen,memory_order_acquire));
            set_policy(SCHED_OTHER,0);
        }
        atomic_store(seen,0); set_policy(SCHED_RR,40);
        pid_t child=fork(); CHECK(child>=0);
        if (!child) { atomic_store_explicit(seen,1,memory_order_release); _exit(0); }
        uint64_t until=monotonic_ns()+350000000U;
        while (!atomic_load_explicit(seen,memory_order_acquire) && monotonic_ns()<until)
            CHECK(syscall(SYS_getpid)>0);
        CHECK(atomic_load_explicit(seen,memory_order_acquire));
        reap(child); set_policy(SCHED_OTHER,0);
    }
    pin(0); CHECK(!munmap(seen,4096)); record("realtime-service-qualification");
}
static void remote_service_progress(void)
{
    if (online==3) {
        struct { atomic_uint ready,finished; } *shared=mmap(0,4096,
            PROT_READ|PROT_WRITE,MAP_ANONYMOUS|MAP_SHARED,-1,0);
        CHECK(shared!=MAP_FAILED);
        pid_t busy=fork(); CHECK(busy>=0);
        if (!busy) {
            pin(0); set_policy(SCHED_FIFO,80);
            atomic_store_explicit(&shared->ready,1,memory_order_release);
            wait_flag(&shared->finished,1); _exit(0);
        }
        pid_t service=fork(); CHECK(service>=0);
        if (!service) {
            pin(1); wait_flag(&shared->ready,1); set_policy(SCHED_FIFO,40);
            for (unsigned i=0;i<16;i++) CHECK(syscall(SYS_getpid)>0);
            atomic_store_explicit(&shared->finished,1,memory_order_release); _exit(0);
        }
        reap(service); reap(busy); CHECK(!munmap(shared,4096));
    }
    record("remote-service-forward-progress");
}
#ifdef BOAROS_SMP_DYNAMIC
static int (*dso_get)(void);
static void (*dso_set)(int);
static void *(*dso_address)(void);
static void *dso_thread(void *opaque)
{
    uintptr_t *address=opaque; pin(online&2 ? 1 : 0);
    CHECK(dso_get()==700); dso_set(730); *address=(uintptr_t)dso_address();
    CHECK(dso_get()==730); return 0;
}
static void dynamic_tls(void)
{
    void *module=dlopen("/lib/libboaros-tls.so",RTLD_NOW|RTLD_LOCAL); CHECK(module);
    dso_get=(int (*)(void))dlsym(module,"boaros_tls_get");
    dso_set=(void (*)(int))dlsym(module,"boaros_tls_set");
    dso_address=(void *(*)(void))dlsym(module,"boaros_tls_address");
    CHECK(dso_get && dso_set && dso_address && dso_get()==700); dso_set(710);
    uintptr_t own=(uintptr_t)dso_address(), other=0; pthread_t thread;
    CHECK(!pthread_create(&thread,0,dso_thread,&other) && !pthread_join(thread,0));
    CHECK(other && own!=other && dso_get()==710 && !dlclose(module));
    record("dynamic-musl-dlopen-tls");
}
#endif
int main(int argc,char **argv)
{
    if (argc==2 && !strcmp(argv[1],"--exec-child")) return 23;
    CHECK(syscall(SYS_sched_getaffinity,0,sizeof(online),&online)==sizeof(online) && (online==1 || online==3));
    printf("USER-SMP BEGIN cpus=%u\n",online==3 ? 2 : 1); fflush(stdout);
    pin(0);
    process_overlap(); thread_signal(); mapping_retirement(); cancellation(); nonleader_exec(); group_stop_continue(); realtime_qualification(); remote_service_progress();
#ifdef BOAROS_SMP_DYNAMIC
    dynamic_tls(); puts("USER-SMP END 9");
#else
    puts("USER-SMP END 8");
#endif
    fflush(stdout); CHECK(!fsync(1) || errno==EINVAL);
    sync(); (void)reboot(RB_POWER_OFF); return 42;
}
