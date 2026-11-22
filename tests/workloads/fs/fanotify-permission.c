#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/fanotify.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "FANOTIFY PERMISSION FAIL line=%d %s errno=%d\n", \
        __LINE__, #test, errno); exit(1); } } while (0)

static const char *name = "/fanotify-permission-file";

static int group_for(unsigned kind, unsigned mask)
{
    int group = (int)syscall(SYS_fanotify_init, kind | FAN_NONBLOCK,
                             O_RDONLY | O_CLOEXEC);
    CHECK(group >= 0);
    CHECK(syscall(SYS_fanotify_mark, group, FAN_MARK_ADD, (uint64_t)mask,
                  AT_FDCWD, name) == 0);
    return group;
}

static void wait_event(int group)
{
    struct pollfd ready = {.fd = group, .events = POLLIN};
    CHECK(poll(&ready, 1, 3000) == 1 && ready.revents == POLLIN);
}

static int get_event(int group, pid_t child, unsigned mask)
{
    struct fanotify_event_metadata event;
    wait_event(group);
    CHECK(read(group, &event, sizeof(event)) == sizeof(event));
    CHECK(event.event_len == sizeof(event) && event.vers == FANOTIFY_METADATA_VERSION);
    CHECK(event.mask == mask && event.pid == child && event.fd >= 0);
    CHECK(fcntl(event.fd, F_GETFD) == FD_CLOEXEC);
    return event.fd;
}

static void respond(int group, int fd, unsigned answer)
{
    struct fanotify_response response = {.fd = fd, .response = answer};
    CHECK(write(group, &response, sizeof(response)) == sizeof(response));
    CHECK(write(group, &response, sizeof(response)) == -1 && errno == ENOENT);
    CHECK(!close(fd));
}

static pid_t open_child(int high, int normal, int denied)
{
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid) return pid;
    if (high >= 0) CHECK(!close(high));
    if (normal >= 0) CHECK(!close(normal));
    int fd = open(name, O_RDONLY);
    if (denied) CHECK(fd == -1 && errno == EPERM);
    else CHECK(fd >= 0 && !close(fd));
    _exit(0);
}

static void reap(pid_t child, int signal)
{
    int status;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(signal ? WIFSIGNALED(status) && WTERMSIG(status) == signal :
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(void)
{
    int fd = open(name, O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(fd >= 0 && write(fd, "A", 1) == 1 && !close(fd));

    int normal = group_for(FAN_CLASS_CONTENT, FAN_OPEN_PERM | FAN_ACCESS_PERM);
    int high = group_for(FAN_CLASS_PRE_CONTENT, FAN_OPEN_PERM);
    pid_t child = open_child(high, normal, 0);
    int eventfd = get_event(high, child, FAN_OPEN_PERM);
    struct fanotify_event_metadata event;
    CHECK(read(normal, &event, sizeof(event)) == -1 && errno == EAGAIN);
    unsigned char payload;
    CHECK(read(eventfd, &payload, 1) == 1 && payload == 'A');
    respond(high, eventfd, FAN_ALLOW);
    respond(normal, get_event(normal, child, FAN_OPEN_PERM), FAN_ALLOW);
    reap(child, 0);
    CHECK(!close(high));

    child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(!close(normal));
        fd = open(name, O_RDONLY);
        CHECK(fd >= 0 && pread(fd, &payload, 1, 0) == -1 && errno == EPERM);
        CHECK(!close(fd));
        _exit(0);
    }
    respond(normal, get_event(normal, child, FAN_OPEN_PERM), FAN_ALLOW);
    respond(normal, get_event(normal, child, FAN_ACCESS_PERM), FAN_DENY);
    reap(child, 0);

    char copy_name[] = "/tmp/fan-permission-copy-XXXXXX";
    int copy = mkstemp(copy_name);
    CHECK(copy >= 0 && !unlink(copy_name));
    child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(!close(normal));
        fd = open(name, O_RDONLY);
        CHECK(fd >= 0);
        CHECK(syscall(SYS_copy_file_range, fd, NULL, copy, NULL, 1, 0) == -1 && errno == EPERM);
        CHECK(syscall(SYS_copy_file_range, fd, NULL, copy, NULL, 0, 0) == -1 && errno == EPERM);
        CHECK(lseek(fd, 0, SEEK_CUR) == 0 && lseek(copy, 0, SEEK_CUR) == 0);
        CHECK(!close(fd) && !close(copy));
        _exit(0);
    }
    respond(normal, get_event(normal, child, FAN_OPEN_PERM), FAN_ALLOW);
    respond(normal, get_event(normal, child, FAN_ACCESS_PERM), FAN_DENY);
    respond(normal, get_event(normal, child, FAN_ACCESS_PERM), FAN_DENY);
    reap(child, 0);
    CHECK(!close(copy));

    char directory[] = "/tmp/fan-permission-direct-XXXXXX";
    CHECK(mkdtemp(directory));
    CHECK(!syscall(SYS_fanotify_mark, normal, FAN_MARK_ADD,
        FAN_OPEN_PERM | FAN_ONDIR, AT_FDCWD, directory));
    child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(!close(normal));
        CHECK(open(directory, O_RDONLY | O_DIRECT) == -1 && errno == EPERM);
        CHECK(open(directory, O_RDONLY | O_DIRECT) == -1 && errno == EINVAL);
        _exit(0);
    }
    respond(normal, get_event(normal, child, FAN_OPEN_PERM), FAN_DENY);
    respond(normal, get_event(normal, child, FAN_OPEN_PERM), FAN_ALLOW);
    reap(child, 0);
    CHECK(!rmdir(directory));

    child = open_child(-1, normal, 1);
    wait_event(normal);
    CHECK(syscall(SYS_read, normal, 1, sizeof(event)) == -1 && errno == EFAULT);
    reap(child, 0);

    child = open_child(-1, normal, 1);
    wait_event(normal);
    CHECK(!kill(child, SIGKILL));
    reap(child, SIGKILL);
    CHECK(read(normal, &event, sizeof(event)) == -1 && errno == EAGAIN);

    child = open_child(-1, normal, 0);
    eventfd = get_event(normal, child, FAN_OPEN_PERM);
    CHECK(!close(eventfd) && !close(normal));
    reap(child, 0);
    CHECK(!unlink(name));
    puts("FANOTIFY PERMISSION PASS");
    return 0;
}
