#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #x, errno); exit(1); } } while (0)
static int ready_pipe[2], release_pipe[2];
static pid_t worker_tid;
static void *worker(void *arg)
{
    (void)arg;
    worker_tid = (pid_t)syscall(SYS_gettid);
    CHECK(write(ready_pipe[1], "x", 1) == 1);
    char c; CHECK(read(release_pipe[0], &c, 1) == 1);
    return 0;
}
static void link_equal(const char *a, const char *b)
{
    char x[4096], y[4096];
    ssize_t nx = readlink(a, x, sizeof(x)), ny = readlink(b, y, sizeof(y));
    CHECK(nx > 0 && nx == ny && !memcmp(x, y, (size_t)nx));
}
int main(void)
{
    if (access("/proc/self", F_OK)) {
        if (mkdir("/proc", 0555) && errno != EEXIST) CHECK(0);
        CHECK(!mount("proc", "/proc", "proc", 0, 0));
    }
    CHECK(!pipe(ready_pipe) && !pipe(release_pipe));
    pthread_t thread; CHECK(!pthread_create(&thread, 0, worker, 0));
    char c; CHECK(read(ready_pipe[0], &c, 1) == 1);
    char dir[128], path[256], proc_fd[128];
    snprintf(dir, sizeof(dir), "/proc/%ld/task/%ld", (long)getpid(), (long)worker_tid);
    int dirfd = open(dir, O_RDONLY | O_DIRECTORY); CHECK(dirfd >= 0);
    const char *names[] = {"exe", "cwd", "root"};
    for (unsigned i = 0; i < 3; i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        snprintf(proc_fd, sizeof(proc_fd), "/proc/self/%s", names[i]);
        link_equal(path, proc_fd);
    }
    snprintf(path, sizeof(path), "%s/maps", dir);
    int fd = open(path, O_RDONLY); CHECK(fd >= 0 && read(fd, path, sizeof(path)) > 0); close(fd);
    snprintf(path, sizeof(path), "%s/fd/%d", dir, ready_pipe[0]);
    snprintf(proc_fd, sizeof(proc_fd), "/proc/self/fd/%d", ready_pipe[0]);
    link_equal(path, proc_fd);
    snprintf(path, sizeof(path), "%s/fdinfo/%d", dir, ready_pipe[0]);
    fd = open(path, O_RDONLY); CHECK(fd >= 0); ssize_t n = read(fd, path, sizeof(path) - 1);
    CHECK(n > 0); path[n] = 0; CHECK(strstr(path, "pos:") && strstr(path, "flags:")); close(fd);
    const char *dirs[] = {"fd", "fdinfo"};
    for (unsigned i = 0; i < 2; i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, dirs[i]);
        DIR *d = opendir(path); CHECK(d); int found = 0; struct dirent *entry;
        snprintf(proc_fd, sizeof(proc_fd), "%d", ready_pipe[0]);
        while ((entry = readdir(d))) if (!strcmp(entry->d_name, proc_fd)) found = 1;
        CHECK(found); CHECK(!closedir(d));
    }
    puts("PASS proc-thread resources and directory enumeration");
    CHECK(write(release_pipe[1], "x", 1) == 1); CHECK(!pthread_join(thread, 0));
    /* join wakes at clear_child_tid; wait for actual TID removal before
     * checking that the old directory cannot follow a later identity. */
    unsigned attempts;
    for (attempts = 0; attempts < 100000; attempts++) {
        if (syscall(SYS_tgkill, getpid(), worker_tid, 0) == -1 && errno == ESRCH) break;
        sched_yield();
    }
    CHECK(attempts < 100000);
    errno = 0; CHECK(openat(dirfd, "maps", O_RDONLY) == -1 && errno == ENOENT);
    close(dirfd); close(ready_pipe[0]); close(ready_pipe[1]); close(release_pipe[0]); close(release_pipe[1]);
    puts("PASS proc-thread old directory after exit");
    return 0;
}
