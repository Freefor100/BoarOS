#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "PERF CPU CLOCK FAIL line %d: %s, errno=%d\n", \
                __LINE__, #expression, errno); \
        exit(1); \
    } \
} while (0)

/* PERF_ATTR_SIZE_VER0 and the flags used by the original LTP constructor. */
struct perf_attr {
    uint32_t type, size;
    uint64_t config, period, sample_type, read_format, flags;
    uint32_t wakeup, bp_type;
    uint64_t config1;
};
_Static_assert(sizeof(struct perf_attr) == 64, "perf_event_attr v0");

static int open_clock(pid_t tid, unsigned disabled, unsigned id)
{
    struct perf_attr attr = {.type = 1, .size = sizeof(attr),
        .read_format = id ? 4 : 0, .flags = disabled | (1U << 5) | (1U << 6)};
    int fd = syscall(SYS_perf_event_open, &attr, tid, -1, -1, 8);
    CHECK(fd >= 0);
    CHECK(fcntl(fd, F_GETFD) == FD_CLOEXEC);
    return fd;
}

static uint64_t count(int fd)
{
    uint64_t value;
    CHECK(read(fd, &value, sizeof(value)) == sizeof(value));
    return value;
}

static uint64_t thread_time(void)
{
    struct timespec time;
    CHECK(!clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time));
    return (uint64_t)time.tv_sec * 1000000000 + time.tv_nsec;
}

static void burn(void)
{
    uint64_t until = thread_time() + 4000000;
    volatile unsigned work = 0;
    do {
        for (unsigned i = 0; i < 2048; i++) work += i;
    } while (thread_time() < until);
    (void)work;
}

static void wait_stopped(pid_t child)
{
    int status;
    CHECK(waitpid(child, &status, WUNTRACED) == child);
    CHECK(WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP);
}

static void remote_count(void)
{
    cpu_set_t allowed;
    CHECK(!sched_getaffinity(0, sizeof(allowed), &allowed));
    int first = -1, second = -1;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (!CPU_ISSET(cpu, &allowed)) continue;
        if (first == -1) first = cpu;
        else { second = cpu; break; }
    }
    CHECK(first >= 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(!raise(SIGSTOP));
        burn();
        CHECK(!raise(SIGSTOP));
        burn();
        _exit(0);
    }
    wait_stopped(child);
    int fd = open_clock(child, 1, 0);
    CHECK(!ioctl(fd, _IO('$', 0), 0));
    burn();
    CHECK(!count(fd));

    cpu_set_t pin;
    CPU_ZERO(&pin);
    CPU_SET(second < 0 ? first : second, &pin);
    CHECK(!sched_setaffinity(child, sizeof(pin), &pin));
    CHECK(!kill(child, SIGCONT));
    wait_stopped(child);
    uint64_t before = count(fd);
    CHECK(before >= 2000000);

    CPU_ZERO(&pin);
    CPU_SET(first, &pin);
    CHECK(!sched_setaffinity(child, sizeof(pin), &pin));
    CHECK(!kill(child, SIGCONT));
    int status;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && !WEXITSTATUS(status));
    uint64_t after = count(fd);
    CHECK(after >= before + 2000000);
    burn();
    CHECK(count(fd) == after && !close(fd));
}

int main(void)
{
    int fd = open_clock(0, 1, 0);
    int alias = dup(fd);
    CHECK(alias >= 0 && !count(fd));
    uint32_t short_read;
    CHECK(read(fd, &short_read, sizeof(short_read)) == -1 && errno == ENOSPC);
    CHECK(!ioctl(alias, _IO('$', 0), 0));
    burn();
    CHECK(!ioctl(fd, _IO('$', 1), 0));
    uint64_t frozen = count(fd);
    CHECK(frozen >= 2000000);
    burn();
    CHECK(count(alias) == frozen && !close(fd));
    CHECK(!ioctl(alias, _IO('$', 3), 0) && !count(alias) && !close(alias));

    fd = open_clock(0, 0, 1);
    uint64_t id, values[2];
    CHECK(!ioctl(fd, _IOR('$', 7, uint64_t), &id));
    CHECK(read(fd, values, sizeof(values)) == sizeof(values) && values[1] == id && id);
    CHECK(!close(fd));

    remote_count();
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(!setgid(65534) && !setuid(65534));
        fd = open_clock(0, 0, 0);
        burn();
        CHECK(count(fd) >= 2000000 && !close(fd));
        _exit(0);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    puts("PERF CPU CLOCK PASS");
    return 0;
}
