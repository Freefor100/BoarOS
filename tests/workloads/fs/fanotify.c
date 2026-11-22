#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/fanotify.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "FANOTIFY FAIL line=%d %s errno=%d\n", __LINE__, #test, errno); \
    exit(1); \
} } while (0)

static int notification(unsigned flags)
{
    return (int)syscall(SYS_fanotify_init, flags, O_RDONLY | O_CLOEXEC);
}

static int mark(int fd, unsigned flags, uint64_t mask, const char *name)
{
    return (int)syscall(SYS_fanotify_mark, fd, flags, mask, AT_FDCWD, name);
}

static void touch_open(const char *name)
{
    int fd = open(name, O_RDONLY);
    CHECK(fd >= 0 && !close(fd));
}

static void empty(int fd)
{
    char bytes[256];
    CHECK(read(fd, bytes, sizeof(bytes)) == -1 && errno == EAGAIN);
}

static void legacy(const char *name)
{
    uint64_t mask = FAN_OPEN | FAN_ACCESS | FAN_CLOSE_NOWRITE;
    int group = notification(FAN_NONBLOCK | FAN_CLOEXEC);
    CHECK(group >= 0 && fcntl(group, F_GETFD) == FD_CLOEXEC);
    CHECK(!mark(group, FAN_MARK_ADD, mask, name));
    int epoll = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ready = {.events = EPOLLIN, .data.u32 = 17};
    CHECK(epoll >= 0 && !epoll_ctl(epoll, EPOLL_CTL_ADD, group, &ready));
    empty(group);
    struct fanotify_event_metadata event;
    char payload;
    int fd = open(name, O_RDONLY);
    CHECK(fd >= 0 && read(fd, &payload, 1) == 1 && payload == 'A' && !close(fd));
    CHECK(epoll_wait(epoll, &ready, 1, 0) == 1 && ready.data.u32 == 17);
    CHECK(read(group, &event, sizeof(event) - 1) == -1 && errno == EINVAL);
    CHECK(read(group, &event, sizeof(event)) == sizeof(event));
    CHECK(event.event_len == sizeof(event) && event.metadata_len == sizeof(event) &&
          event.vers == FANOTIFY_METADATA_VERSION && event.mask == mask && event.pid == getpid());
    struct stat actual, expected;
    CHECK(event.fd >= 0 && fcntl(event.fd, F_GETFD) == FD_CLOEXEC);
    CHECK(!fstat(event.fd, &actual) && !stat(name, &expected) && actual.st_ino == expected.st_ino);
    CHECK(read(event.fd, &payload, 1) == 1 && payload == 'A' && !close(event.fd));
    empty(group);

    /* A failed copy consumes the event but returns the unused descriptor. */
    int first = dup(STDIN_FILENO);
    CHECK(first >= 0 && !close(first));
    touch_open(name);
    CHECK(syscall(SYS_read, group, 1, sizeof(event)) == -1 && errno == EFAULT);
    fd = dup(STDIN_FILENO);
    CHECK(fd == first && !close(fd));
    empty(group);

    struct rlimit saved, limited;
    CHECK(!getrlimit(RLIMIT_NOFILE, &saved));
    limited = saved;
    limited.rlim_cur = 32;
    CHECK(!setrlimit(RLIMIT_NOFILE, &limited));
    touch_open(name);
    int occupied[32];
    unsigned count = 0;
    while (count < sizeof(occupied) / sizeof(occupied[0])) {
        fd = dup(STDIN_FILENO);
        if (fd < 0) break;
        occupied[count++] = fd;
    }
    CHECK(fd < 0 && errno == EMFILE);
    CHECK(read(group, &event, sizeof(event)) == -1 && errno == EMFILE);
    while (count) CHECK(!close(occupied[--count]));
    CHECK(!setrlimit(RLIMIT_NOFILE, &saved));
    empty(group);
    CHECK(!mark(group, FAN_MARK_FLUSH, 0, NULL));
    touch_open(name);
    empty(group);
    CHECK(!close(epoll) && !close(group));
}

struct saved_handle {
    struct {
        unsigned bytes;
        int type;
        unsigned char data[128];
    } handle;
    struct statfs fs;
};

static void get_handle(const char *name, struct saved_handle *saved)
{
    int mount_id;
    saved->handle.bytes = sizeof(saved->handle.data);
    CHECK(!syscall(SYS_name_to_handle_at, AT_FDCWD, name, &saved->handle, &mount_id, 0));
    CHECK(!statfs(name, &saved->fs));
}

static void read_fid(int fd, const struct saved_handle *expected, uint64_t mask, pid_t pid)
{
    _Alignas(struct fanotify_event_metadata) unsigned char bytes[256];
    struct fanotify_event_metadata *event = (void *)bytes;
    size_t size = sizeof(*event) + sizeof(struct fanotify_event_info_fid) +
                  sizeof(unsigned) + sizeof(int) + expected->handle.bytes;
    size = (size + 3) & ~(size_t)3;
    CHECK(size <= sizeof(bytes) && read(fd, bytes, size) == (ssize_t)size);
    CHECK(event->event_len == size && event->metadata_len == sizeof(*event) &&
          event->vers == FANOTIFY_METADATA_VERSION && event->mask == mask &&
          event->pid == pid && event->fd == FAN_NOFD);
    struct fanotify_event_info_fid *info = (void *)(bytes + event->metadata_len);
    CHECK(info->hdr.info_type == FAN_EVENT_INFO_TYPE_FID &&
          info->hdr.len == size - sizeof(*event));
    CHECK(!memcmp(&info->fsid, &expected->fs.f_fsid, sizeof(info->fsid)));
    CHECK(!memcmp(info->handle, &expected->handle,
                  sizeof(unsigned) + sizeof(int) + expected->handle.bytes));
}

static void mark_scopes(const char *base, const char *file, const char *other,
                        const char *alias, const char *alias_file)
{
    uint64_t mask = FAN_OPEN | FAN_CLOSE_NOWRITE;
    struct saved_handle a, b;
    get_handle(file, &a);
    get_handle(other, &b);
    int group = notification(FAN_REPORT_FID | FAN_NONBLOCK);
    CHECK(group >= 0 && !mark(group, FAN_MARK_ADD, mask, file) &&
          !mark(group, FAN_MARK_ADD, mask, other));
    CHECK(!mark(group, FAN_MARK_ADD | FAN_MARK_MOUNT, mask, base));
    int first = open(file, O_RDONLY);
    int second = open(other, O_RDONLY);
    CHECK(first >= 0 && second >= 0 && !close(first) && !close(second));
    read_fid(group, &a, mask, getpid());
    read_fid(group, &b, mask, getpid());
    empty(group);
    CHECK(!close(group));

    CHECK(!mount(base, alias, NULL, MS_BIND, NULL));
    int mount_watch = notification(FAN_REPORT_FID | FAN_NONBLOCK);
    int fs_watch = notification(FAN_REPORT_FID | FAN_NONBLOCK);
    CHECK(mount_watch >= 0 && fs_watch >= 0);
    CHECK(!mark(mount_watch, FAN_MARK_ADD | FAN_MARK_MOUNT, FAN_OPEN, alias));
    CHECK(!mark(fs_watch, FAN_MARK_ADD | FAN_MARK_FILESYSTEM, FAN_OPEN, alias));
    touch_open(file);
    empty(mount_watch);
    read_fid(fs_watch, &a, FAN_OPEN, getpid());
    touch_open(alias_file);
    read_fid(mount_watch, &a, FAN_OPEN, getpid());
    read_fid(fs_watch, &a, FAN_OPEN, getpid());

    int legacy_watch = notification(FAN_NONBLOCK);
    CHECK(legacy_watch >= 0 && !mark(legacy_watch, FAN_MARK_ADD | FAN_MARK_MOUNT, FAN_OPEN, alias));
    touch_open(alias_file);
    CHECK(!umount2(alias, MNT_DETACH));
    read_fid(mount_watch, &a, FAN_OPEN, getpid());
    read_fid(fs_watch, &a, FAN_OPEN, getpid());
    struct fanotify_event_metadata event;
    CHECK(read(legacy_watch, &event, sizeof(event)) == sizeof(event) && event.fd >= 0);
    char payload;
    CHECK(read(event.fd, &payload, 1) == 1 && payload == 'A' && !close(event.fd));
    CHECK(!close(legacy_watch));
    touch_open(file);
    empty(mount_watch);
    read_fid(fs_watch, &a, FAN_OPEN, getpid());
    CHECK(!close(fs_watch) && !close(mount_watch));

    group = notification(FAN_REPORT_FID | FAN_NONBLOCK);
    CHECK(group >= 0 && !mark(group, FAN_MARK_ADD, FAN_OPEN, other));
    touch_open(other);
    CHECK(!unlink(other));
    read_fid(group, &b, FAN_OPEN, getpid());
    empty(group);
    CHECK(!close(group));
}

static void tmpfs_fid(const char *target, const char *file)
{
    CHECK(!mount("tmpfs", target, "tmpfs", 0, NULL));
    int fd = open(file, O_CREAT | O_WRONLY | O_EXCL, 0600);
    CHECK(fd >= 0 && !close(fd));
    struct saved_handle handle;
    get_handle(file, &handle);
    int group = notification(FAN_REPORT_FID | FAN_NONBLOCK);
    CHECK(group >= 0 && !mark(group, FAN_MARK_ADD, FAN_OPEN, file));
    touch_open(file);
    read_fid(group, &handle, FAN_OPEN, getpid());
    empty(group);
    CHECK(!close(group) && !unlink(file) && !umount(target));
}

static void restricted(const char *name)
{
    struct saved_handle a;
    get_handle(name, &a);
    int ready[2], complete[2];
    CHECK(!pipe(ready) && !pipe(complete));
    pid_t child = fork();
    CHECK(child >= 0);
    char token = 1;
    if (!child) {
        CHECK(!close(ready[0]) && !close(complete[1]));
        CHECK(!setgid(65534) && !setuid(65534));
        CHECK(notification(FAN_NONBLOCK) == -1 && errno == EPERM);
        int group = notification(FAN_REPORT_FID | FAN_NONBLOCK);
        CHECK(group >= 0);
        CHECK(mark(group, FAN_MARK_ADD | FAN_MARK_MOUNT, FAN_OPEN, name) == -1 && errno == EPERM);
        CHECK(!mark(group, FAN_MARK_ADD, FAN_OPEN, name));
        CHECK(write(ready[1], &token, 1) == 1 && read(complete[0], &token, 1) == 1);
        read_fid(group, &a, FAN_OPEN, 0);
        CHECK(!close(group));
        _exit(0);
    }
    CHECK(!close(ready[1]) && !close(complete[0]));
    CHECK(read(ready[0], &token, 1) == 1);
    touch_open(name);
    CHECK(write(complete[1], &token, 1) == 1 && !close(ready[0]) && !close(complete[1]));
    int status;
    CHECK(waitpid(child, &status, 0) == child && status == 0);
}

static void blocked_reader_exit(const char *name)
{
    int group = notification(FAN_CLOEXEC);
    CHECK(group >= 0 && !mark(group, FAN_MARK_ADD, FAN_OPEN, name));
    int ready[2];
    CHECK(!pipe(ready));
    pid_t child = fork();
    CHECK(child >= 0);
    char token = 1;
    if (!child) {
        CHECK(!close(ready[0]) && write(ready[1], &token, 1) == 1);
        struct fanotify_event_metadata event;
        (void)read(group, &event, sizeof(event));
        _exit(1);
    }
    CHECK(!close(ready[1]) && read(ready[0], &token, 1) == 1 && !close(ready[0]));
    char path[64], state[128];
    CHECK(snprintf(path, sizeof(path), "/proc/%d/stat", (int)child) > 0);
    int sleeping = 0;
    for (unsigned attempt = 0; attempt < 2000 && !sleeping; attempt++) {
        int proc = open(path, O_RDONLY);
        CHECK(proc >= 0);
        ssize_t got = read(proc, state, sizeof(state) - 1);
        CHECK(got > 0 && !close(proc));
        state[got] = 0;
        char *command_end = strrchr(state, ')');
        CHECK(command_end && command_end[1] == ' ' && command_end[2] != 'Z');
        sleeping = command_end[2] == 'S';
        if (!sleeping) CHECK(!nanosleep(&(struct timespec){.tv_nsec = 1000000L}, NULL));
    }
    CHECK(sleeping && !close(group) && !kill(child, SIGKILL));
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    touch_open(name);
}

static void drop_clean_pages(const char *name)
{
    long length = sysconf(_SC_PAGESIZE);
    int writer = open(name, O_RDWR);
    int drop = open("/proc/sys/vm/drop_caches", O_WRONLY);
    int pressure = open("/proc/sys/vm/vfs_cache_pressure", O_RDONLY);
    CHECK(length > 0 && writer >= 0 && drop >= 0 && pressure >= 0);
    char value[24];
    CHECK(read(pressure, value, sizeof(value)) > 0 && !close(pressure));
    CHECK(pwrite(writer, "B", 1, length - 1) == 1 && !fsync(writer));
    char *mapping = mmap(NULL, length, PROT_READ, MAP_SHARED, writer, 0);
    unsigned char resident;
    CHECK(mapping != MAP_FAILED);
    CHECK(!mincore(mapping, length, &resident) && (resident & 1));
    CHECK(write(drop, "1\n", 2) == 2);
    CHECK(!mincore(mapping, length, &resident) && !(resident & 1));
    CHECK(*(volatile char *)(mapping + length - 1) == 'B');
    CHECK(lseek(drop, 0, SEEK_SET) == 0 && write(drop, "1\n", 2) == 2);
    CHECK(!mincore(mapping, length, &resident) && (resident & 1));
    CHECK(!munmap(mapping, length) && !close(writer) && !close(drop));
}

int main(void)
{
    char base[] = "/tmp/fannot-base-XXXXXX", alias[] = "/tmp/fannot-alias-XXXXXX";
    CHECK(mkdtemp(base) && mkdtemp(alias) && !chmod(base, 0755));
    char a[128], b[128], aliased[128];
    CHECK(snprintf(a, sizeof(a), "%s/a", base) > 0);
    CHECK(snprintf(b, sizeof(b), "%s/b", base) > 0);
    CHECK(snprintf(aliased, sizeof(aliased), "%s/a", alias) > 0);
    int fd = open(a, O_CREAT | O_WRONLY | O_EXCL, 0644);
    CHECK(fd >= 0 && write(fd, "A", 1) == 1 && !close(fd));
    fd = open(b, O_CREAT | O_WRONLY | O_EXCL, 0644);
    CHECK(fd >= 0 && !close(fd));
    legacy(a);
    mark_scopes(base, a, b, alias, aliased);
    tmpfs_fid(alias, aliased);
    restricted(a);
    blocked_reader_exit(a);
    drop_clean_pages(a);
    CHECK(!unlink(a) && !rmdir(base) && !rmdir(alias));
    puts("FANOTIFY PASS fd-delivery faults scopes fid namespace privacy");
    return 0;
}
