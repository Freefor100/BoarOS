#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL %d %s errno=%d\n", __LINE__, #expr, errno); failures++; } } while (0)
#define FUTEX_WAIT_BITSET_PRIVATE (9 | 128)
#define FUTEX_BITSET_MATCH_ANY 0xffffffffU

static int failures;
static const long long quantum = 33554432;
static const long long requested = 70000000;

static long slack(void)
{
    return syscall(SYS_prctl, PR_GET_TIMERSLACK, 0, 0, 0, 0);
}

static long setslack(unsigned long ns)
{
    return syscall(SYS_prctl, PR_SET_TIMERSLACK, ns, 0, 0, 0);
}

static long long mono(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000LL + t.tv_nsec;
}

static struct timespec ts(long long ns)
{
    return (struct timespec){ns / 1000000000, ns % 1000000000};
}

static long long target(void)
{
    return (mono() / quantum + 1) * quantum + 1000000;
}

static int status(pid_t child, int wanted)
{
    int result;
    return waitpid(child, &result, 0) == child && WIFEXITED(result) && WEXITSTATUS(result) == wanted;
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        CHECK(slack() == 70000);
        CHECK(setslack(17) == 0 && setslack(0) == 0 && slack() == 70000);
        return failures ? 2 : 0;
    }
    CHECK(slack() == 50000);
    CHECK(setslack(70000) == 0 && slack() == 70000);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        execl(argv[0], argv[0], "exec-check", (char *)0);
        _exit(99);
    }
    CHECK(status(child, 0));

    CHECK(mkdir("/proc", 0755) == 0 || errno == EEXIST);
    CHECK(mount("proc", "/proc", "proc", 0, 0) == 0 || errno == EBUSY);
    int fd = open("/proc/self/timerslack_ns", O_RDWR);
    CHECK(fd >= 0);
    char value[25] = "";
    CHECK(read(fd, value, sizeof(value)) == 6 && !strcmp(value, "70000\n"));
    CHECK(write(fd, "18446744073709551616\n", 21) == -1 && errno == ERANGE);
    CHECK(write(fd, "70000000\n", 9) == 9 && slack() == requested);
    CHECK(write(fd, "+0\n", 3) == 3 && slack() == 50000);
    CHECK(write(fd, "70000000\n", 9) == 9 && slack() == requested);
    close(fd);

    long long begin = target();
    struct timespec t = ts(begin);
    CHECK(clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, 0) == 0);
    printf("sleep overshoot=%lld\n", mono() - begin);
    CHECK(mono() >= begin + 20000000);

    int futex = 0;
    begin = target();
    t = ts(begin);
    CHECK(syscall(SYS_futex, &futex, FUTEX_WAIT_BITSET_PRIVATE, 0, &t, 0, FUTEX_BITSET_MATCH_ANY) == -1 && errno == ETIMEDOUT);
    printf("futex overshoot=%lld\n", mono() - begin);
    CHECK(mono() >= begin + 20000000);

    int ep = epoll_create1(0);
    struct epoll_event event;
    begin = mono();
    CHECK(epoll_wait(ep, &event, 1, 5) == 0);
    printf("epoll elapsed=%lld\n", mono() - begin);
    close(ep);
    begin = mono();
    CHECK(poll(0, 0, 5) == 0);
    printf("poll elapsed=%lld\n", mono() - begin);

    struct sched_param param = {.sched_priority=1};
    CHECK(syscall(SYS_sched_setscheduler, 0, SCHED_FIFO, &param) == 0);
    CHECK(slack() == 0 && setslack(70000) == 0 && slack() == 0);
    begin = target();
    t = ts(begin);
    CHECK(clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, 0) == 0);
    printf("RT overshoot=%lld\n", mono() - begin);
    CHECK(mono() < begin + 15000000);
    param.sched_priority = 0;
    CHECK(syscall(SYS_sched_setscheduler, 0, SCHED_OTHER, &param) == 0);
    CHECK(slack() == 50000);

    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_prctl, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EACCES),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog program = {.len = sizeof(filter) / sizeof(filter[0]), .filter = filter};
    CHECK(syscall(SYS_prctl, PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0);
    CHECK(syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &program) == 0);
    CHECK(slack() == -1 && errno == EACCES);
    CHECK(setslack(70000) == -1 && errno == EACCES);
    puts(failures ? "timer slack FAIL" : "timer slack PASS");
    fflush(stdout);
    reboot(RB_POWER_OFF);
    return failures != 0;
}
