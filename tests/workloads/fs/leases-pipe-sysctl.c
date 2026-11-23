#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FILE_NAME "/lease-check"
#define ALIAS_NAME "/lease-check-link"
#define LEASE_SYSCTL "/proc/sys/fs/lease-break-time"
#define PIPE_SYSCTL "/proc/sys/fs/pipe-max-size"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s, errno=%d\n", \
    __func__, __LINE__, #x, errno); return 1; } } while (0)

static volatile sig_atomic_t notices;
static volatile sig_atomic_t last_fd;
static volatile sig_atomic_t last_code;

static void notice(int signum, siginfo_t *info, void *context)
{
    (void)signum;
    (void)context;
    last_fd = info->si_fd;
    last_code = info->si_code;
    notices++;
}

static int sysctl_set(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    const char *end = value;
    while (*end) end++;
    ssize_t written = write(fd, value, end - value);
    int saved = errno;
    close(fd);
    errno = saved;
    return written == end - value ? 0 : -1;
}

static long sysctl_get(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    char text[32] = {0};
    ssize_t count = read(fd, text, sizeof(text) - 1);
    close(fd);
    return count > 0 ? strtol(text, NULL, 10) : -1;
}

static int leases(void)
{
    CHECK(sigaction(SIGIO, &(struct sigaction){.sa_sigaction = notice, .sa_flags = SA_SIGINFO}, NULL) == 0);
    int fd = open(FILE_NAME, O_CREAT | O_RDWR | O_TRUNC, 0666);
    CHECK(fd >= 0 && write(fd, "hello", 5) == 5);
    CHECK(fcntl(fd, F_SETLEASE, F_RDLCK) < 0 && errno == EAGAIN);
    int opener = open(FILE_NAME, O_RDONLY);
    CHECK(opener >= 0 && fcntl(fd, F_SETLEASE, F_WRLCK) < 0 && errno == EAGAIN);
    CHECK(close(opener) == 0 && link(FILE_NAME, ALIAS_NAME) == 0);

    int pathfd = open(FILE_NAME, O_PATH);
    int alias = dup(fd);
    CHECK(pathfd >= 0 && alias >= 0);
    CHECK(fcntl(pathfd, F_GETLEASE) < 0 && errno == EBADF);
    CHECK(fcntl(fd, F_SETSIG, SIGIO) == 0 && fcntl(fd, F_SETLEASE, F_WRLCK) == 0);
    CHECK(close(fd) == 0 && fcntl(alias, F_GETLEASE) == F_WRLCK);
    CHECK(fcntl(alias,F_GETFL)&O_ASYNC);
    CHECK(fcntl(alias,F_SETFL,0)==0 && (fcntl(alias,F_GETFL)&O_ASYNC));
    CHECK(open(ALIAS_NAME, O_RDONLY | O_NONBLOCK) < 0 && errno == EAGAIN);
    CHECK(notices == 1 && last_fd == fd && last_code == POLL_MSG);
    CHECK(fcntl(alias, F_GETLEASE) == F_RDLCK);
    CHECK(open(FILE_NAME, O_RDWR | O_TRUNC | O_NONBLOCK) < 0 && errno == EAGAIN);
    struct stat st;
    CHECK(notices == 2 && fcntl(alias, F_GETLEASE) == F_UNLCK && fstat(alias, &st) == 0 && st.st_size == 5);
    CHECK(fcntl(alias, F_SETLEASE, F_UNLCK) == 0 && fcntl(alias, F_GETSIG) == 0);
    CHECK(!(fcntl(alias,F_GETFL)&O_ASYNC));
    CHECK(close(alias) == 0 && close(pathfd) == 0);

    fd = open(FILE_NAME, O_RDONLY);
    CHECK(fd >= 0 && fcntl(fd, F_SETLEASE, F_RDLCK) == 0);
    opener = open(ALIAS_NAME, O_RDONLY | O_NONBLOCK);
    CHECK(opener >= 0 && fcntl(opener, F_SETLEASE, F_RDLCK) == 0);
    CHECK(close(opener) == 0);
    opener = open(FILE_NAME, O_RDONLY | O_TRUNC | O_NONBLOCK);
    CHECK(opener >= 0 && fstat(fd, &st) == 0 && st.st_size == 0);
    CHECK(fcntl(fd, F_GETLEASE) == F_RDLCK && close(opener) == 0);

    CHECK(sysctl_get(LEASE_SYSCTL) == 45 && sysctl_set(LEASE_SYSCTL, "1\n") == 0);
    CHECK(sysctl_get(LEASE_SYSCTL) == 1);
    CHECK(signal(SIGIO,SIG_IGN)!=SIG_ERR);
    struct timespec start, end;
    CHECK(fcntl(fd, F_SETLEASE, F_WRLCK) == 0);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    opener = open(ALIAS_NAME, O_RDONLY);
    CHECK(opener >= 0 && close(opener) == 0 && clock_gettime(CLOCK_MONOTONIC, &end) == 0);
    CHECK((end.tv_sec - start.tv_sec) * 1000000000LL + end.tv_nsec - start.tv_nsec >= 800000000LL);
    CHECK(fcntl(fd, F_GETLEASE) == F_RDLCK);

    CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    CHECK(truncate(ALIAS_NAME, 1) == 0 && clock_gettime(CLOCK_MONOTONIC, &end) == 0);
    CHECK((end.tv_sec - start.tv_sec) * 1000000000LL + end.tv_nsec - start.tv_nsec >= 800000000LL);
    CHECK(fstat(fd, &st) == 0 && st.st_size == 1 && fcntl(fd, F_GETLEASE) == F_UNLCK);
    CHECK(sysctl_set(LEASE_SYSCTL, "0\n") == 0 && close(fd) == 0);

    CHECK(sigaction(SIGIO, &(struct sigaction){.sa_sigaction = notice, .sa_flags = SA_SIGINFO}, NULL) == 0);
    fd = open(FILE_NAME, O_RDWR);
    CHECK(fd >= 0 && fcntl(fd, F_SETLEASE, F_WRLCK) == 0);
    sigset_t pending, previous;
    sigemptyset(&pending); sigaddset(&pending, SIGIO);
    CHECK(sigprocmask(SIG_BLOCK, &pending, &previous) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        close(fd);
        opener = open(ALIAS_NAME, O_RDWR);
        _exit(opener >= 0 ? 0 : 5);
    }
    CHECK(sigtimedwait(&pending, NULL, &(struct timespec){.tv_sec = 3}) == SIGIO);
    int status;
    CHECK(waitpid(child, &status, WNOHANG) == 0);
    CHECK(close(fd) == 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    fd = open(FILE_NAME, O_RDWR);
    CHECK(fd >= 0 && fcntl(fd, F_SETLEASE, F_WRLCK) == 0);
    child = fork();
    CHECK(child >= 0);
    if (!child) {
        close(fd);
        opener = open(ALIAS_NAME, O_RDONLY);
        _exit(opener >= 0 ? 0 : 6);
    }
    CHECK(sigtimedwait(&pending, NULL, &(struct timespec){.tv_sec = 3}) == SIGIO);
    CHECK(waitpid(child, &status, WNOHANG) == 0);
    CHECK(sysctl_set(LEASE_SYSCTL, "1\n") == 0);
    CHECK(open(FILE_NAME, O_WRONLY | O_NONBLOCK) < 0 && errno == EAGAIN);
    CHECK(sigtimedwait(&pending, NULL, &(struct timespec){.tv_sec = 3}) == SIGIO);
    for (unsigned polls = 0;; ++polls) {
        pid_t found = waitpid(child, &status, WNOHANG);
        CHECK(found >= 0 && polls < 300);
        if (found == child) break;
        nanosleep(&(struct timespec){.tv_nsec = 10000000}, NULL);
    }
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(close(fd) == 0);
    CHECK(sigprocmask(SIG_SETMASK, &previous, NULL) == 0);
    CHECK(sysctl_set(LEASE_SYSCTL, "45\n") == 0);
    puts("BoarOS: lease OFD, break notification, expiry and wait passed");
    return 0;
}

static int pipe_sysctl(void)
{
    CHECK(sysctl_get(PIPE_SYSCTL) == 1048576);
    long page = sysconf(_SC_PAGESIZE);
    CHECK(page > 0);
    char limit[32];
    snprintf(limit, sizeof(limit), "%ld\n", page + 1);
    CHECK(sysctl_set(PIPE_SYSCTL, limit) == 0 && sysctl_get(PIPE_SYSCTL) == 2 * page);
    CHECK(sysctl_set(PIPE_SYSCTL, "4294967296\n") < 0 && errno == EINVAL);
    int pipefd[2];
    CHECK(pipe(pipefd) == 0 && fcntl(pipefd[0], F_GETPIPE_SZ) == 16 * page);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        int unprivileged[2];
        if (setuid(65534) || pipe(unprivileged) || fcntl(unprivileged[0], F_GETPIPE_SZ) != 2 * page)
            _exit(1);
        if (fcntl(unprivileged[1], F_SETPIPE_SZ, 4 * page) >= 0 || errno != EPERM)
            _exit(2);
        if (fcntl(pipefd[1], F_SETPIPE_SZ, 4 * page) != 4 * page)
            _exit(3);
        if (open(PIPE_SYSCTL, O_WRONLY) >= 0 || errno != EACCES)
            _exit(4);
        _exit(0);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(sysctl_set(PIPE_SYSCTL, "1048576\n") == 0);
    puts("BoarOS: pipe sysctl creation and resize limits passed");
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(20);
    if (mkdir("/proc", 0755) < 0 && errno != EEXIST) return 1;
    if (mount("proc", "/proc", "proc", 0, NULL) < 0 && errno != EBUSY) return 1;
    int generated = open("/proc/self/status", O_RDONLY);
    CHECK(generated >= 0 && fcntl(generated, F_GETLEASE) == F_UNLCK);
    CHECK(fcntl(generated, F_SETLEASE, F_RDLCK) < 0 && errno == EINVAL);
    CHECK(close(generated) == 0);
    int failed = leases() | pipe_sysctl();
    unlink(ALIAS_NAME);
    unlink(FILE_NAME);
    return failed ? 1 : 0;
}
