#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <sys/random.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(c)                                                               \
    do {                                                                       \
        if (!(c)) {                                                            \
            dprintf(2, "rng failure line=%d errno=%d\n", __LINE__, errno);     \
            return 1;                                                          \
        }                                                                      \
    } while (0)
static volatile sig_atomic_t signal_seen;
static void on_signal(int signo) { signal_seen = signo; }

static int cancel_unready(void)
{
    int ready[2], status;
    char marker, bytes[16];
    struct sigaction action = {0};
    action.sa_handler = on_signal;
    CHECK(sigaction(SIGUSR1, &action, 0) == 0 && pipe(ready) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        struct timespec delay = {0, 30000000};
        close(ready[1]);
        if (read(ready[0], &marker, 1) != 1 || nanosleep(&delay, 0) ||
            kill(getppid(), SIGUSR1)) _exit(2);
        _exit(0);
    }
    CHECK(close(ready[0]) == 0 && write(ready[1], "r", 1) == 1);
    errno = 0;
    CHECK(getrandom(bytes, sizeof(bytes), 0) == -1 && errno == EINTR &&
          signal_seen == SIGUSR1);
    CHECK(close(ready[1]) == 0 && waitpid(child, &status, 0) == child &&
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
    puts("rng: unready wait interrupted");
    return 0;
}

int main(void)
{
    setvbuf(stdout, 0, _IONBF, 0);
    char mode = 0, bytes[64];
    int fd = open("/mode", O_RDONLY);
    CHECK(fd >= 0 && read(fd, &mode, 1) == 1 && !close(fd));
    if (mode == 'n') {
        CHECK(getrandom(bytes, sizeof(bytes), 0) == sizeof(bytes));
        puts("rng: ready");
        return 42;
    }
    errno = 0;
    CHECK(getrandom(bytes, sizeof(bytes), GRND_NONBLOCK) == -1 &&
          errno == EAGAIN);
    puts("rng: unready");
    if (mode == 'a') {
        CHECK(cancel_unready() == 0);
        puts("rng: absent boot ok");
        return 42;
    }
    if (mode == 's') {
        puts("rng: stop pending");
        return 42;
    }
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        if (getrandom(bytes, sizeof(bytes), 0) != sizeof(bytes))
            _exit(2);
        puts("rng: waiter ready");
        _exit(0);
    }
    volatile uint64_t sum = 0;
    for (unsigned i = 0; i < 100000; i++)
        sum += i;
    CHECK(sum == UINT64_C(4999950000));
    puts("rng: computation progressed");
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0);
    CHECK(getrandom(bytes, sizeof(bytes), GRND_NONBLOCK) == sizeof(bytes));
    puts("rng: delayed ready");
    return 42;
}
