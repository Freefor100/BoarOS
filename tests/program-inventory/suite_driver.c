#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* One case per guest. Binary child output is encoded in separate records so
 * arbitrary test output cannot impersonate the driver protocol. */
#define CONFIG_LIMIT 65536U
#define VECTOR_LIMIT 128U
#define OUTPUT_LIMIT (1024U * 1024U)
static char configuration[CONFIG_LIMIT];
static char *cursor, *end;

static char *field(void)
{
    char *value = cursor;
    if (cursor >= end) return NULL;
    char *terminator = memchr(cursor, 0, (size_t)(end - cursor));
    if (!terminator) return NULL;
    cursor = terminator + 1;
    return value;
}

static int number(char *value, unsigned long maximum, unsigned long *result)
{
    char *tail;
    if (!value || !*value || *value < '0' || *value > '9') return -1;
    errno = 0;
    unsigned long parsed = strtoul(value, &tail, 10);
    if (errno || *tail || parsed > maximum) return -1;
    *result = parsed;
    return 0;
}

static int64_t now_ms(void)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time)) return -1;
    return (int64_t)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

#if !defined(SUITE_DRIVER_HOST_TEST) || defined(SUITE_DRIVER_ENV_TEST)
static void environment_result(const char *name, int result)
{
    printf("SUITE ENV %s %d %d\n", name, result, result ? errno : 0);
}

static void environment_mount(const char *name, const char *target,
                              const char *type, const char *options)
{
    int result = mount(type, target, type, 0, options);
    environment_result(name, result);
}

static void environment_directory(const char *name, const char *path)
{
    int result = mkdir(path, 01777);
    if (result && errno == EEXIST) result = 0;
    environment_result(name, result);
}

static void environment_loopback(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        environment_result("lo", -1);
        return;
    }
    struct ifreq interface = {0};
    memcpy(interface.ifr_name, "lo", sizeof("lo"));
    int result = ioctl(fd, SIOCGIFFLAGS, &interface);
    if (!result) {
        interface.ifr_flags |= IFF_UP;
        result = ioctl(fd, SIOCSIFFLAGS, &interface);
    }
    environment_result("lo", result);
    close(fd);
}
#endif

static void environment(void)
{
#if !defined(SUITE_DRIVER_HOST_TEST) || defined(SUITE_DRIVER_ENV_TEST)
    environment_mount("proc", "/proc", "proc", NULL);
    environment_mount("sysfs", "/sys", "sysfs", NULL);
    /* Linux auto-mounts devtmpfs before /init, hiding fixture /dev directories.
     * Recreate these mount points in the visible /dev before mounting them. */
    environment_directory("shm-dir", "/dev/shm");
    environment_mount("shm", "/dev/shm", "tmpfs", "mode=1777");
    environment_directory("mqueue-dir", "/dev/mqueue");
    environment_mount("mqueue", "/dev/mqueue", "mqueue", NULL);
    environment_loopback();
#endif
}

static int finish(int result)
{
#ifndef SUITE_DRIVER_HOST_TEST
    reboot(RB_POWER_OFF);
    return result == 0 ? 42 : result;
#else
    return result;
#endif
}

static int fail(const char *stage)
{
    printf("SUITE ERROR %s %d\n", stage, errno);
    return finish(1);
}

int main(int argc, char **argv)
{
    const char *case_path = "/case";
#ifdef SUITE_DRIVER_HOST_TEST
    if (argc != 2) return 2;
    case_path = argv[1];
#else
    (void)argc;
    (void)argv;
#endif
    setvbuf(stdout, NULL, _IONBF, 0);
    int fd = open(case_path, O_RDONLY);
    if (fd < 0) return fail("case-open");
    size_t used = 0;
    while (used < sizeof(configuration)) {
        ssize_t count = read(fd, configuration + used, sizeof(configuration) - used);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return fail("case-read");
        if (!count) break;
        used += (size_t)count;
    }
    close(fd);
    cursor = configuration;
    end = configuration + used;
    char *magic = field(), *id = field(), *timeout_string = field(), *cwd = field();
    unsigned long timeout_ms, env_count, arg_count;
    if (used == sizeof(configuration) || !magic || strcmp(magic, "SUITE1") ||
        !id || !*id || strspn(id, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") != strlen(id) ||
        !cwd || cwd[0] != '/' || number(timeout_string, 3600000, &timeout_ms) || !timeout_ms ||
        number(field(), VECTOR_LIMIT, &env_count)) {
        errno = EINVAL;
        return fail("case-format");
    }
    char *env[VECTOR_LIMIT + 1], *arguments[VECTOR_LIMIT + 1];
    for (unsigned long i = 0; i < env_count; i++) {
        env[i] = field();
        if (!env[i] || !strchr(env[i], '=')) { errno = EINVAL; return fail("case-env"); }
    }
    env[env_count] = NULL;
    if (number(field(), VECTOR_LIMIT, &arg_count) || !arg_count) {
        errno = EINVAL;
        return fail("case-argc");
    }
    for (unsigned long i = 0; i < arg_count; i++) {
        arguments[i] = field();
        if (!arguments[i]) { errno = EINVAL; return fail("case-argv"); }
    }
    arguments[arg_count] = NULL;
    if (cursor != end || arguments[0][0] != '/') { errno = EINVAL; return fail("case-tail"); }
    printf("SUITE BEGIN 1 %s\n", id);
    environment();
    int pipes[2][2], exec_pipe[2];
    if (pipe(pipes[0]) || pipe(pipes[1]) || pipe(exec_pipe)) return fail("pipe");
    if (fcntl(exec_pipe[1], F_SETFD, FD_CLOEXEC)) return fail("exec-cloexec");
    for (int i = 0; i < 2; i++)
        if (fcntl(pipes[i][0], F_SETFL, O_NONBLOCK)) return fail("nonblock");
    int64_t started = now_ms();
    if (started < 0) return fail("clock");
    pid_t child = fork();
    if (child < 0) return fail("fork");
    if (child == 0) {
        int failure_stage = 1; /* setup; stage 2 means the execve itself */
        close(exec_pipe[0]);
        for (int i = 0; i < 2; i++) {
            close(pipes[i][0]);
            if (dup2(pipes[i][1], i + 1) < 0) goto child_error;
            close(pipes[i][1]);
        }
        int input = open("/dev/null", O_RDONLY);
        if (input >= 0) { (void)dup2(input, 0); close(input); }
        /* Kernel init starts at /. Avoid requiring an otherwise unnecessary
         * chdir syscall before the workload can execute. */
        if (strcmp(cwd, "/") && chdir(cwd)) goto child_error;
        failure_stage = 2;
        execve(arguments[0], arguments, env);
child_error: {
            int error = errno;
            int failure[2] = {failure_stage, error};
            (void)write(exec_pipe[1], failure, sizeof(failure));
            dprintf(2, "driver %s errno=%d: %s\n",
                    failure_stage == 1 ? "setup" : "execve", error, strerror(error));
            _exit(127);
        }
    }
    close(exec_pipe[1]);
    for (int i = 0; i < 2; i++) close(pipes[i][1]);
    struct pollfd pollfds[2] = {{pipes[0][0], POLLIN, 0}, {pipes[1][0], POLLIN, 0}};
    size_t output[2] = {0, 0};
    int truncated[2] = {0, 0}, reaped = 0, timed_out = 0, status = 0;
    int64_t reaped_at = -1;
    while (!reaped || pollfds[0].fd >= 0 || pollfds[1].fd >= 0) {
        int64_t now = now_ms();
        if (now < 0) { (void)kill(child, SIGKILL); return fail("clock"); }
        if (!reaped && !timed_out && now - started >= (int64_t)timeout_ms) {
            timed_out = 1;
            if (kill(child, SIGKILL) && errno != ESRCH) return fail("kill");
        }
        if (!reaped) {
            pid_t waited = waitpid(child, &status, WNOHANG);
            if (waited == child) { reaped = 1; reaped_at = now; }
            else if (waited < 0 && errno != EINTR) return fail("wait");
        }
        /* A descendant retaining output must not hang this PID1 forever.
         * Isolated guests contain it; the incomplete output is not a pass. */
        if (reaped && now - reaped_at >= 1000) {
            if (pollfds[0].fd >= 0 || pollfds[1].fd >= 0) puts("SUITE DRAIN_TIMEOUT 1");
            break;
        }
        if (poll(pollfds, 2, 20) < 0 && errno != EINTR) return fail("poll");
        for (int i = 0; i < 2; i++) {
            if (pollfds[i].fd < 0) continue;
            if (pollfds[i].revents & POLLNVAL) { errno = EBADF; return fail("poll-fd"); }
            unsigned char bytes[256];
            ssize_t count = read(pollfds[i].fd, bytes, sizeof(bytes));
            if (!count) { close(pollfds[i].fd); pollfds[i].fd = -1; continue; }
            if (count < 0) {
                if (errno == EAGAIN || errno == EINTR) continue;
                return fail("output-read");
            }
            size_t emit = (size_t)count;
            if (emit > OUTPUT_LIMIT - output[i]) emit = OUTPUT_LIMIT - output[i];
            if (emit) {
                printf("SUITE DATA %c ", i ? 'E' : 'O');
                for (size_t n = 0; n < emit; n++) printf("%02x", bytes[n]);
                puts(".");
                output[i] += emit;
            }
            if (emit != (size_t)count && !truncated[i]) {
                printf("SUITE TRUNCATED %c\n", i ? 'E' : 'O');
                truncated[i] = 1;
            }
        }
    }
    for (int i = 0; i < 2; i++) if (pollfds[i].fd >= 0) close(pollfds[i].fd);
    int failure[2];
    ssize_t count = read(exec_pipe[0], failure, sizeof(failure));
    close(exec_pipe[0]);
    if (count == (ssize_t)sizeof(failure))
        printf("SUITE %s %d\n", failure[0] == 1 ? "SETUP" : "EXEC", failure[1]);
    else if (count != 0) { errno = EIO; return fail("exec-status"); }
    printf("SUITE WAIT %d\nSUITE TIMEOUT %d\nSUITE END %s\n", status, timed_out, id);
    return finish(0);
}
