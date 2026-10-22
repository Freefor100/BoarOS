#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(v) do {if (!(v)) {dprintf(2,"ACCOUNT FAIL line=%d errno=%d\n",__LINE__,errno);exit(1);}} while(0)
static uint64_t now(void)
{
    struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));
    return (uint64_t)t.tv_sec*1000000000U+t.tv_nsec;
}
static void result(const char *name,uint64_t start,uint64_t check)
{printf("ACCOUNT COST %s ns=%llu check=%llu\n",name,(unsigned long long)(now()-start),(unsigned long long)check);fflush(stdout);}
static void sqlite(const char *database,const char *sql,const char *expected)
{
    int output[2];CHECK(!pipe(output));pid_t child=fork();CHECK(child>=0);
    if (!child) {
        CHECK(!close(output[0]) && dup2(output[1],1)==1 && !close(output[1]));
        execl("/sqlite3","sqlite3",database,sql,(char *)0);_exit(97);
    }
    CHECK(!close(output[1]));char buffer[128];size_t count=0;
    for (;;) {
        ssize_t n=read(output[0],buffer+count,sizeof(buffer)-1-count);CHECK(n>=0);
        if (!n) break;
        count+=(size_t)n;CHECK(count<sizeof(buffer)-1);
    }
    CHECK(!close(output[0]));int status;CHECK(waitpid(child,&status,0)==child && status==0);
    buffer[count]=0;CHECK(!strcmp(buffer,expected));
}
static int legacy_main(void)
{
    uint64_t start=now(),value=1;
    for (unsigned i=0;i<200000000;i++) {value=(value<<3)^(value>>7)^17;__asm__ volatile("" : "+r"(value));}
    CHECK(value==UINT64_C(0x36de7ac67e71a943));
    result("compute",start,value);
    CHECK(!mkdir("/tmp",01777) || errno==EEXIST);
    CHECK(!mount("tmpfs","/tmp","tmpfs",0,0));
    unsigned char bytes[64],out[64];for (unsigned i=0;i<sizeof(bytes);i++) bytes[i]=(unsigned char)(i+1);
    int fd=open("/tmp/small-write",O_CREAT|O_TRUNC|O_RDWR,0600);CHECK(fd>=0);
    start=now();
    for (unsigned i=0;i<10000;i++) CHECK(write(fd,bytes,sizeof(bytes))==sizeof(bytes));
    CHECK(lseek(fd,0,SEEK_SET)==0);
    for (unsigned i=0;i<10000;i++) CHECK(read(fd,out,sizeof(out))==sizeof(out) && !memcmp(bytes,out,sizeof(out)));
    CHECK(!close(fd));result("tmpfs-small-io",start,640000);
    start=now();
    sqlite(":memory:","WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<50000) SELECT sum(x) FROM n;","1250025000\n");
    result("sqlite-memory",start,1250025000);
    char sql[8192];size_t used=(size_t)snprintf(sql,sizeof(sql),
        "PRAGMA journal_mode=DELETE;PRAGMA synchronous=FULL;CREATE TABLE t(id INTEGER PRIMARY KEY,v INTEGER);"
        "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<5000) INSERT INTO t SELECT x,x FROM n;");
    for (unsigned i=0;i<40;i++) used+=(size_t)snprintf(sql+used,sizeof(sql)-used,"BEGIN;UPDATE t SET v=v+1 WHERE id=%u;COMMIT;",i+1);
    CHECK(used+80<sizeof(sql));strcpy(sql+used,"PRAGMA integrity_check;SELECT count(*),sum(v) FROM t;");
    start=now();sqlite("/account.db",sql,"delete\nok\n5000|12502540\n");result("sqlite-commit",start,12502540);
    CHECK(!unlink("/account.db"));sync();puts("ACCOUNT END 4");fflush(stdout);return 42;
}


/* 每种模式使用同一个ELF和固定工作量；配置、建线程和输出均在计时区外。 */
static const char *const timer_modes[] = {
    "off", "virtual-armed", "prof-armed", "virtual-periodic", "prof-periodic"
};
static const char *const timer_cases[] = {
    "getpid", "compute", "tmpfs-small-io", "compute-threads", "compute-processes"
};
static volatile sig_atomic_t delivered;
static sigset_t timer_signals;
static int timer_mode, expected_cpus = 1;
struct usage { uint64_t user, system; };
struct sample { uint64_t wall; struct usage self; };
struct worker {
    int index, ready, gate, done, process;
    uint64_t value, begin, end;
    unsigned cpu_mask, signals;
    struct usage usage;
};

static struct usage cpu_usage(int who)
{
    struct rusage u;
    CHECK(!getrusage(who, &u));
    return (struct usage){
        (uint64_t)u.ru_utime.tv_sec * 1000000000U + (uint64_t)u.ru_utime.tv_usec * 1000U,
        (uint64_t)u.ru_stime.tv_sec * 1000000000U + (uint64_t)u.ru_stime.tv_usec * 1000U
    };
}
static void timer_handler(int signal)
{
    (void)signal;
    ++delivered;
}
static void timer_stop(void)
{
    const struct itimerval zero = {0};
    CHECK(!setitimer(ITIMER_VIRTUAL, &zero, NULL));
    CHECK(!setitimer(ITIMER_PROF, &zero, NULL));
    struct timespec timeout = {0};
    for (;;) {
        int r = (int)syscall(SYS_rt_sigtimedwait, &timer_signals, NULL, &timeout, 8);
        if (r >= 0) continue;
        if (errno == EINTR) continue;
        CHECK(errno == EAGAIN);
        break;
    }
}
static void timer_prepare(void)
{
    CHECK(!pthread_sigmask(SIG_BLOCK, &timer_signals, NULL));
    timer_stop();
    delivered = 0;
    if (!timer_mode) return;
    struct itimerval value = {0};
    if (timer_mode <= 2) value.it_value.tv_sec = 60;
    else value.it_value.tv_usec = value.it_interval.tv_usec = 10000;
    CHECK(!setitimer(timer_mode & 1 ? ITIMER_VIRTUAL : ITIMER_PROF, &value, NULL));
}
static struct sample timer_start(void)
{
    struct sample s = {.self = cpu_usage(RUSAGE_SELF)};
    s.wall = now();
    CHECK(!pthread_sigmask(SIG_UNBLOCK, &timer_signals, NULL));
    return s;
}
static void timer_result(const char *name, struct sample start, uint64_t check,
                         unsigned workers, unsigned cpu_mask, int overlap, unsigned child_signals, struct usage child_usage)
{
    CHECK(!pthread_sigmask(SIG_BLOCK, &timer_signals, NULL));
    uint64_t wall = now() - start.wall;
    struct usage self = cpu_usage(RUSAGE_SELF);
    unsigned signals = (unsigned)delivered + child_signals;
    uint64_t user = self.user - start.self.user + child_usage.user;
    uint64_t system = self.system - start.self.system + child_usage.system;
    timer_stop();
    CHECK(timer_mode >= 3 || !signals);
    /* 短的getpid在宿主原生运行时可能不足一个周期，长负载必须实际递送。 */
    uint64_t charged = timer_mode & 1 ? user : user + system;
    CHECK(timer_mode < 3 || charged < 100000000U || signals);
    printf("ACCOUNT TIMER %s wall_ns=%llu user_ns=%llu system_ns=%llu signals=%u check=%llu workers=%u cpu_mask=%u overlap=%d\n",
           name, (unsigned long long)wall, (unsigned long long)user, (unsigned long long)system,
           signals, (unsigned long long)check, workers, cpu_mask, overlap);
    fflush(stdout);
}
static uint64_t compute(unsigned count)
{
    uint64_t value = 1;
    for (unsigned i = 0; i < count; ++i) {
        value = (value << 3) ^ (value >> 7) ^ 17;
        __asm__ volatile("" : "+r"(value));
    }
    return value;
}
static void pipe_bytes(int fd, void *buffer, size_t length, int writing)
{
    unsigned char *p = buffer;
    while (length) {
        ssize_t n = writing ? write(fd, p, length) : read(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        CHECK(n > 0);
        p += n;
        length -= (size_t)n;
    }
}
static void worker_run(struct worker *w)
{
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    CPU_SET(w->index % expected_cpus, &affinity);
    CHECK(!sched_setaffinity(0, sizeof(affinity), &affinity));
    if (w->process) timer_prepare();
    char byte = 1;
    pipe_bytes(w->ready, &byte, 1, 1);
    pipe_bytes(w->gate, &byte, 1, 0);
    if (w->process) CHECK(!pthread_sigmask(SIG_UNBLOCK, &timer_signals, NULL));
    struct usage begin = {0};
    if (w->process) begin = cpu_usage(RUSAGE_SELF);
    w->begin = now();
    w->value = compute(200000000U);
    if (w->process) CHECK(!pthread_sigmask(SIG_BLOCK, &timer_signals, NULL));
    w->end = now();
    if (w->process) {
        struct usage end = cpu_usage(RUSAGE_SELF);
        w->usage = (struct usage){end.user - begin.user, end.system - begin.system};
    }
    CHECK(w->value == UINT64_C(0x36de7ac67e71a943));
    CPU_ZERO(&affinity);
    CHECK(!sched_getaffinity(0, sizeof(affinity), &affinity));
    CHECK(CPU_COUNT(&affinity) == 1 && CPU_ISSET(w->index % expected_cpus, &affinity));
    w->cpu_mask = 1U << (w->index % expected_cpus);
    if (w->process) {
        w->signals = (unsigned)delivered;
        timer_stop();
    }
    pipe_bytes(w->done, &byte, 1, 1);
}
static void *thread_run(void *opaque)
{
    worker_run(opaque);
    return NULL;
}
static void parallel_compute(int processes)
{
    int ready[2], gate[2], done[2];
    CHECK(!pipe(ready) && !pipe(gate) && !pipe(done));
    struct worker *workers = mmap(NULL, sizeof(*workers) * 8, PROT_READ | PROT_WRITE,
                                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(workers != MAP_FAILED);
    pthread_t threads[8];
    pid_t children[8];
    /* 子进程各自配置定时器；线程仅共享父进程配置，handler只在父线程执行。 */
    CHECK(!pthread_sigmask(SIG_BLOCK, &timer_signals, NULL));
    timer_stop();
    delivered = 0;
    for (unsigned i = 0; i < 8; ++i) {
        workers[i] = (struct worker){.index = (int)i, .ready = ready[1], .gate = gate[0],
                                      .done = done[1], .process = processes};
        if (processes) {
            children[i] = fork();
            CHECK(children[i] >= 0);
            if (!children[i]) { worker_run(&workers[i]); _exit(0); }
        } else CHECK(!pthread_create(&threads[i], NULL, thread_run, &workers[i]));
    }
    char bytes[8] = {0};
    pipe_bytes(ready[0], bytes, sizeof(bytes), 0);
    if (!processes) timer_prepare();
    struct sample start = timer_start();
    pipe_bytes(gate[1], bytes, sizeof(bytes), 1);
    pipe_bytes(done[0], bytes, sizeof(bytes), 0);
    unsigned cpu_mask = 0, signals = 0;
    struct usage child_usage = {0};
    uint64_t value = 0;
    int overlap = 0;
    for (unsigned i = 0; i < 8; ++i) {
        if (processes) {
            int status;
            CHECK(waitpid(children[i], &status, 0) == children[i] && status == 0);
        } else CHECK(!pthread_join(threads[i], NULL));
        value += workers[i].value;
        cpu_mask |= workers[i].cpu_mask;
        signals += workers[i].signals;
        child_usage.user += workers[i].usage.user;
        child_usage.system += workers[i].usage.system;
        for (unsigned j = 0; j < i; ++j)
            if (workers[i].cpu_mask != workers[j].cpu_mask && workers[i].begin < workers[j].end &&
                workers[j].begin < workers[i].end) overlap = 1;
    }
    CHECK(value == UINT64_C(0xb6f3d633f38d4a18));
    CHECK(cpu_mask == (unsigned)((1U << expected_cpus) - 1) && (expected_cpus == 1 || overlap));
    timer_result(processes ? "compute-processes" : "compute-threads", start, value, 8, cpu_mask, overlap, signals, child_usage);
    CHECK(!munmap(workers, sizeof(*workers) * 8));
    CHECK(!close(ready[0]) && !close(ready[1]) && !close(gate[0]) && !close(gate[1]) &&
          !close(done[0]) && !close(done[1]));
}
static void timer_case(const char *name)
{
    if (!strcmp(name, "compute-threads") || !strcmp(name, "compute-processes")) {
        parallel_compute(!strcmp(name, "compute-processes"));
        return;
    }
    int fd = -1;
    unsigned char bytes[64], out[64];
    if (!strcmp(name, "tmpfs-small-io")) {
        CHECK(!mkdir("/tmp", 01777) || errno == EEXIST);
        CHECK(!mount("tmpfs", "/tmp", "tmpfs", 0, NULL));
        for (unsigned i = 0; i < sizeof(bytes); ++i) bytes[i] = (unsigned char)(i + 1);
        fd = open("/tmp/timer-small-write", O_CREAT | O_TRUNC | O_RDWR, 0600);
        CHECK(fd >= 0);
    }
    pid_t expected = getpid();
    timer_prepare();
    struct sample start = timer_start();
    uint64_t check;
    if (!strcmp(name, "getpid")) {
        for (unsigned i = 0; i < 1000000U; ++i) CHECK(syscall(SYS_getpid) == expected);
        check = 1000000;
    } else if (!strcmp(name, "compute")) {
        check = compute(1600000000U);
        CHECK(check == UINT64_C(0xed8eb42899304480));
    } else {
        for (unsigned i = 0; i < 80000U; ++i) CHECK(write(fd, bytes, sizeof(bytes)) == sizeof(bytes));
        CHECK(lseek(fd, 0, SEEK_SET) == 0);
        for (unsigned i = 0; i < 80000U; ++i)
            CHECK(read(fd, out, sizeof(out)) == sizeof(out) && !memcmp(bytes, out, sizeof(out)));
        check = 5120000;
    }
    timer_result(name, start, check, 1, 1, 0, 0, (struct usage){0});
    if (fd >= 0) {
        CHECK(!close(fd) && !unlink("/tmp/timer-small-write"));
        CHECK(!umount("/tmp"));
    }
}
static int timer_main(const char *mode, const char *only)
{
    for (timer_mode = 0; timer_mode < 5; ++timer_mode)
        if (!strcmp(mode, timer_modes[timer_mode])) break;
    CHECK(timer_mode < 5 && (expected_cpus == 1 || expected_cpus == 2 || expected_cpus == 4));
    sigemptyset(&timer_signals);
    sigaddset(&timer_signals, SIGVTALRM);
    sigaddset(&timer_signals, SIGPROF);
    struct sigaction action = {.sa_handler = timer_handler, .sa_flags = SA_RESTART};
    sigemptyset(&action.sa_mask);
    CHECK(!sigaction(SIGVTALRM, &action, NULL) && !sigaction(SIGPROF, &action, NULL));
    /* 单线程负载固定在CPU0；并行负载的worker按编号分到请求的CPU。 */
    if (getpid() == 1) {
        cpu_set_t affinity;
        CPU_ZERO(&affinity);CPU_SET(0, &affinity);
        CHECK(!sched_setaffinity(0, sizeof(affinity), &affinity));
    }
    printf("ACCOUNT TIMER BEGIN mode=%s cpus=%d\n", mode, expected_cpus);fflush(stdout);
    unsigned count = 0;
    for (unsigned i = 0; i < sizeof(timer_cases) / sizeof(timer_cases[0]); ++i)
        if (!only || !strcmp(only, timer_cases[i])) { timer_case(timer_cases[i]); ++count; }
    CHECK(count);
    sync();
    printf("ACCOUNT TIMER END %u\n", count);fflush(stdout);
    return 42;
}
#include "signal_stack.h"
int main(int argc, char **argv)
{
    FILE *stack_configuration=fopen("/account-altstack","r");
    if(stack_configuration)return stack_cost_main(stack_configuration);
    CHECK(errno==ENOENT);
    if (argc > 1) {
        const char *mode = NULL, *only = NULL;
        for (int i = 1; i < argc; ++i) {
            CHECK(i + 1 < argc);
            if (!strcmp(argv[i], "--timer-mode")) mode = argv[++i];
            else if (!strcmp(argv[i], "--workload")) only = argv[++i];
            else if (!strcmp(argv[i], "--cpus")) expected_cpus = atoi(argv[++i]);
            else CHECK(0);
        }
        CHECK(mode);
        return timer_main(mode, only);
    }
    FILE *configuration = fopen("/account-timer", "r");
    if (configuration) {
        char mode[32];
        CHECK(fscanf(configuration, "%31s %d", mode, &expected_cpus) == 2);
        CHECK(!fclose(configuration));
        return timer_main(mode, NULL);
    }
    CHECK(errno == ENOENT);
    return legacy_main();
}
