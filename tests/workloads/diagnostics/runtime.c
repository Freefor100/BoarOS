#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static long milliseconds(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) _exit(90);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* A diagnostic timeout is reported separately from the real wait status. */
static void run(const char *name, char *const argv[], long budget)
{
    fflush(NULL);
    pid_t child = fork();
    if (child < 0) { printf("PROBE %s fork-error=%d\n", name, errno); return; }
    if (!child) {
        execv(argv[0], argv);
        dprintf(2, "PROBE exec-error=%d path=%s\n", errno, argv[0]);
        _exit(127);
    }
    long deadline = milliseconds() + budget;
    int status = 0, timed_out = 0;
    for (;;) {
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) break;
        if (result < 0) { printf("PROBE %s wait-error=%d\n", name, errno); return; }
        if (milliseconds() >= deadline) { timed_out = 1; kill(child, SIGKILL); }
        struct timespec delay = {0, 20000000};
        nanosleep(&delay, NULL);
    }
    printf("PROBE %s timeout=%d wait=%d\n", name, timed_out, status);
}

static void time_probe(void)
{
    struct timespec coarse = {0};
    errno = 0;
    int coarse_rc = clock_gettime(CLOCK_REALTIME_COARSE, &coarse);
    printf("PROBE clock-realtime-coarse result=%d errno=%d\n", coarse_rc, errno);
    FILE *f = tmpfile();
    if (!f) { perror("tmpfile"); return; }
    int fd = fileno(f);
    for (unsigned i = 0; i < 8; i++) {
        struct timespec before, after, times[2] = {{0, UTIME_NOW}, {0, UTIME_NOW}};
        struct stat st;
        time_t t = time(NULL);
        clock_gettime(CLOCK_REALTIME, &before);
        int rc = futimens(fd, times);
        fstat(fd, &st);
        clock_gettime(CLOCK_REALTIME, &after);
        printf("PROBE time-trace t=%ld before=%ld.%09ld now_rc=%d atime=%ld.%09ld mtime=%ld.%09ld after=%ld.%09ld\n",
            (long)t, before.tv_sec, before.tv_nsec, rc, st.st_atim.tv_sec, st.st_atim.tv_nsec,
            st.st_mtim.tv_sec, st.st_mtim.tv_nsec, after.tv_sec, after.tv_nsec);
        struct timespec delay = {0, 300000000};
        nanosleep(&delay, NULL);
    }
    fclose(f);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    mkdir("/dev", 0755); mkdir("/proc", 0755); mkdir("/tmp", 01777);
    mkdir("/bin", 0755); mkdir("/lib", 0755);
    mknod("/dev/null", S_IFCHR | 0666, makedev(1, 3));
    mknod("/dev/zero", S_IFCHR | 0666, makedev(1, 5));
    mknod("/dev/console", S_IFCHR | 0600, makedev(5, 1));
    if (mount("proc", "/proc", "proc", 0, NULL)) { perror("proc"); return 91; }
    symlink("/glibc/lib/ld-linux-riscv64-lp64d.so.1", "/lib/ld-linux-riscv64-lp64d.so.1");
    setenv("PATH", "/glibc/ltp/testcases/bin:/bin:/glibc", 1);
    setenv("LD_LIBRARY_PATH", "/glibc/lib", 1);
    if (chdir("/glibc")) return 92;
    char *install[] = {"/glibc/busybox", "--install", "-s", "/bin", NULL};
    run("install", install, 60000);
    symlink("/glibc/busybox", "/bin/sh");
    char *free_args[] = {"/glibc/busybox", "free", NULL};
    run("free", free_args, 5000);
    for (unsigned i = 0; i < 30; i++) {
        char *a[] = {"/glibc/entry-static.exe", "utime", NULL};
        char *b[] = {"/glibc/entry-dynamic.exe", "utime", NULL};
        run("utime-static", a, 5000);
        run("utime-dynamic", b, 5000);
    }
    time_probe();
    char *original_time[] = {"/time-probe", NULL};
    run("original-libc-time-probe", original_time, 10000);
    char *abort_args[] = {"/glibc/ltp/testcases/bin/abort01", NULL};
    run("abort01", abort_args, 10000);
    char *function_args[] = {"/bin/sh", "/glibc/ltp/testcases/bin/cgroup_fj_function.sh", NULL};
    run("cgroup-function-original-argv", function_args, 5000);
    char *subsystem_args[] = {"/bin/sh", "/glibc/ltp/testcases/bin/cgroup_fj_function.sh", "cpuset", NULL};
    run("cgroup-function-cpuset", subsystem_args, 10000);
    char *helper_args[] = {"/glibc/ltp/testcases/bin/cgroup_fj_proc", NULL};
    run("cgroup-helper-original-argv", helper_args, 3000);
    puts("PROBE complete");
    return 0;
}
