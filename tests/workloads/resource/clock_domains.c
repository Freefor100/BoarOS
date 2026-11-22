#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/timex.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        dprintf(STDERR_FILENO, "CLOCK DOMAINS FAIL line=%d %s errno=%d\n", \
            __LINE__, #condition, errno); \
        _exit(1); \
    } \
} while (0)

static const int64_t second = 1000000000;
static struct timespec stamp(int64_t ns)
{
    return (struct timespec){ns / second, ns % second};
}

static int64_t now(clockid_t clock)
{
    struct timespec value;
    CHECK(syscall(SYS_clock_gettime, clock, &value) == 0);
    return value.tv_sec * second + value.tv_nsec;
}

static void set_tai(int offset)
{
    struct timex change = {.modes = ADJ_TAI, .constant = offset};
    CHECK(syscall(SYS_clock_adjtime, CLOCK_REALTIME, &change) >= 0 && change.tai == offset);
}

static void wall_step(int seconds)
{
    struct timex change = {.modes = ADJ_SETOFFSET, .time.tv_sec = seconds};
    CHECK(syscall(SYS_adjtimex, &change) >= 0);
}

static void reaped(pid_t child)
{
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
}

static void waiting(pid_t child)
{
    char path[64], status[512];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)child);
    int64_t deadline = now(CLOCK_MONOTONIC) + 3 * second;
    do {
        int fd = open(path, O_RDONLY);
        CHECK(fd >= 0);
        ssize_t n = read(fd, status, sizeof(status) - 1U);
        CHECK(close(fd) == 0 && n > 0);
        status[n] = '\0';
        const char *end = strrchr(status, ')');
        CHECK(end && end[1] == ' ' && end[2] != 'Z');
        if (end[2] == 'S' || end[2] == 'D') return;
        CHECK(nanosleep(&(struct timespec){.tv_nsec=1000000}, NULL) == 0);
    } while (now(CLOCK_MONOTONIC) < deadline);
    CHECK(0 && "child entered its clock wait");
}

static void clock_tai_domain(void)
{
    int64_t realtime = now(CLOCK_REALTIME);
    int64_t tai = now(CLOCK_TAI);
    CHECK(tai >= realtime + 100 * second && tai <= now(CLOCK_REALTIME) + 100 * second);
    struct timespec resolution;
    CHECK(syscall(SYS_clock_getres, CLOCK_TAI, &resolution) == 0 && resolution.tv_nsec > 0);
    CHECK(timerfd_create(CLOCK_TAI, 0) == -1 && errno == EINVAL);

    int fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK);
    struct itimerspec it = {.it_value = stamp(realtime + 35 * second)};
    CHECK(fd >= 0 && timerfd_settime(fd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &it, NULL) == 0);
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    CHECK(sigprocmask(SIG_BLOCK, &blocked, NULL) == 0);
    struct sigevent ev = {.sigev_notify = SIGEV_SIGNAL, .sigev_signo = SIGUSR1};
    timer_t timer;
    CHECK(timer_create(CLOCK_TAI, &ev, &timer) == 0);
    it = (struct itimerspec){.it_value = {.tv_sec = 25}};
    CHECK(timer_settime(timer, 0, &it, NULL) == 0);

    int pipefd[2];
    CHECK(pipe(pipefd) == 0);
    struct timespec expires = stamp(now(CLOCK_TAI) + 20 * second);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(close(pipefd[0]) == 0 && write(pipefd[1], "R", 1) == 1);
        CHECK(clock_nanosleep(CLOCK_TAI, TIMER_ABSTIME, &expires, NULL) == 0);
        CHECK(write(pipefd[1], "D", 1) == 1 && close(pipefd[1]) == 0);
        _exit(0);
    }
    char state;
    CHECK(close(pipefd[1]) == 0 && read(pipefd[0], &state, 1) == 1 && state == 'R');
    waiting(child);
    struct pollfd pipe_poll = {.fd=pipefd[0],.events=POLLIN};
    set_tai(60);
    CHECK(poll(&pipe_poll, 1, 0) == 0);
    CHECK(timer_gettime(timer, &it) == 0 && it.it_value.tv_sec > 31);
    set_tai(150);
    CHECK(poll(&pipe_poll, 1, 2000) == 1 && read(pipefd[0], &state, 1) == 1 && state == 'D');
    CHECK(close(pipefd[0]) == 0);
    reaped(child);
    CHECK(sigtimedwait(&blocked, NULL, &(struct timespec){.tv_sec=2}) == SIGUSR1);
    CHECK(timer_delete(timer) == 0);
    CHECK(sigprocmask(SIG_UNBLOCK, &blocked, NULL) == 0);

    uint64_t expirations;
    CHECK(read(fd, &expirations, sizeof(expirations)) == -1 && errno == EAGAIN);
    wall_step(1);
    CHECK(read(fd, &expirations, sizeof(expirations)) == -1 && errno == ECANCELED);
    CHECK(close(fd) == 0);
    wall_step(-1);
    puts("CLOCK DOMAINS PASS tai-retarget-and-cancel-isolation");
}

static void alarm_permissions(int rtc, int alarm_fd)
{
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(setuid(65534) == 0);
        CHECK(timerfd_create(CLOCK_BOOTTIME_ALARM, 0) == -1 && errno == EPERM);
        struct itimerspec it = {0};
        CHECK(timerfd_gettime(alarm_fd, &it) == 0);
        CHECK(timerfd_settime(alarm_fd, 0, &it, NULL) == -1 && errno == EPERM);
        struct sigevent ev = {.sigev_notify=SIGEV_NONE};
        timer_t timer;
        CHECK(timer_create(CLOCK_BOOTTIME_ALARM, &ev, &timer) == -1 &&
              errno == (rtc ? EPERM : EOPNOTSUPP));
        struct timespec duration = {.tv_nsec=1000000};
        CHECK(clock_nanosleep(CLOCK_BOOTTIME_ALARM, 0, &duration, NULL) ==
              (rtc ? EPERM : EOPNOTSUPP));
        _exit(0);
    }
    reaped(child);
}

static void alarm_domains(void)
{
    struct timespec resolution;
    int rtc = syscall(SYS_clock_getres, CLOCK_REALTIME_ALARM, &resolution) == 0;
    CHECK(rtc || errno == EINVAL);
    int clocks[] = {CLOCK_REALTIME_ALARM, CLOCK_BOOTTIME_ALARM};
    for (unsigned i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
        struct timespec sample;
        if (rtc)
            CHECK(syscall(SYS_clock_gettime, clocks[i], &sample) == 0 &&
                  syscall(SYS_clock_getres, clocks[i], NULL) == 0);
        else
            CHECK(syscall(SYS_clock_gettime, clocks[i], &sample) == -1 && errno == EINVAL);
    }
    int wall = timerfd_create(CLOCK_REALTIME_ALARM, TFD_NONBLOCK);
    int boot = timerfd_create(CLOCK_BOOTTIME_ALARM, TFD_NONBLOCK);
    CHECK(wall >= 0 && boot >= 0);
    alarm_permissions(rtc, wall);
    struct itimerspec it = {.it_value={.tv_sec=10}};
    CHECK(timerfd_settime(wall, 0, &it, NULL) == 0);
    CHECK(timerfd_settime(boot, 0, &it, NULL) == 0);
    wall_step(20);
    struct pollfd alarm_poll = {.fd=wall,.events=POLLIN};
    CHECK(poll(&alarm_poll, 1, 2000) == 1);
    uint64_t count;
    CHECK(read(wall, &count, sizeof(count)) == sizeof(count) && count == 1);
    CHECK(read(boot, &count, sizeof(count)) == -1 && errno == EAGAIN);
    it.it_value = stamp(now(CLOCK_REALTIME) + 15 * second);
    CHECK(timerfd_settime(wall, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &it, NULL) == 0);
    /* Returning to the creation offset produces no cancellation, even if
     * the wall clock stepped earlier while this descriptor was unregistered. */
    wall_step(-20);
    CHECK(read(wall, &count, sizeof(count)) == -1 && errno == EAGAIN);
    wall_step(1);
    CHECK(read(wall, &count, sizeof(count)) == -1 && errno == ECANCELED);
    CHECK(read(wall, &count, sizeof(count)) == -1 && errno == EAGAIN);
    CHECK(close(wall) == 0 && close(boot) == 0);
    wall_step(-1);
    printf("CLOCK DOMAINS RTC available=%d\n", rtc);
    puts("CLOCK DOMAINS PASS alarms");
}

static void noop_handler(int signo)
{
    (void)signo;
}

static void sleep_restart(int absolute, int new_offset)
{
    int channel[2];
    CHECK(pipe(channel) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(close(channel[0]) == 0);
        struct sigaction handler = {.sa_handler=noop_handler,.sa_flags=SA_RESTART};
        sigemptyset(&handler.sa_mask);
        CHECK(sigaction(SIGUSR2, &handler, NULL) == 0);
        struct timespec rem = {-1,-1}, delay = {.tv_sec=15};
        int flags = absolute ? TIMER_ABSTIME : 0;
        if (absolute) delay = stamp(now(CLOCK_TAI) + 15 * second);
        CHECK(write(channel[1], "R", 1) == 1);
        CHECK(clock_nanosleep(CLOCK_TAI, flags, &delay, &rem) == EINTR);
        if (absolute) CHECK(rem.tv_sec == -1 && rem.tv_nsec == -1);
        else CHECK(rem.tv_sec > 0 && rem.tv_sec <= 15);
        CHECK(write(channel[1], "R", 1) == 1);
        CHECK(clock_nanosleep(CLOCK_TAI, flags, &delay, NULL) == 0);
        CHECK(write(channel[1], "D", 1) == 1);
        _exit(0);
    }
    CHECK(close(channel[1]) == 0);
    char state;
    CHECK(read(channel[0], &state, 1) == 1 && state == 'R');
    waiting(child);
    CHECK(kill(child, SIGUSR2) == 0);
    CHECK(read(channel[0], &state, 1) == 1 && state == 'R');
    waiting(child);
    CHECK(kill(child, SIGSTOP) == 0);
    int status;
    CHECK(waitpid(child, &status, WUNTRACED) == child && WIFSTOPPED(status));
    set_tai(new_offset);
    CHECK(kill(child, SIGCONT) == 0);
    struct pollfd finished = {.fd=channel[0],.events=POLLIN};
    CHECK(poll(&finished, 1, 2000) == 1 && read(channel[0], &state, 1) == 1 && state == 'D');
    CHECK(close(channel[0]) == 0);
    reaped(child);
    printf("CLOCK DOMAINS PASS tai-signal-and-stop-restart absolute=%d\n", absolute);
}

static void boottime_periodic(void)
{
    int fd = timerfd_create(CLOCK_BOOTTIME, TFD_NONBLOCK);
    struct itimerspec it = {.it_value={.tv_nsec=5000000},.it_interval={.tv_nsec=5000000}};
    CHECK(fd >= 0 && timerfd_settime(fd, 0, &it, NULL) == 0);
    struct timespec end = stamp(now(CLOCK_BOOTTIME) + 25000000);
    CHECK(clock_nanosleep(CLOCK_BOOTTIME, TIMER_ABSTIME, &end, NULL) == 0);
    uint64_t value;
    CHECK(read(fd, &value, sizeof(value)) == sizeof(value) && value >= 5);
    CHECK(close(fd) == 0);
    puts("CLOCK DOMAINS PASS boottime-periodic");
}

int main(void)
{
    struct timex saved = {0};
    CHECK(syscall(SYS_adjtimex, &saved) >= 0);
    set_tai(100);
    clock_tai_domain();
    sleep_restart(0, 180);
    sleep_restart(1, 210);
    boottime_periodic();
    alarm_domains();
    set_tai(saved.tai);
    puts("CLOCK DOMAINS PASS");
    return 0;
}
