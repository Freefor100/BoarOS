#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/shm.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef SHM_STAT_ANY
#define SHM_STAT_ANY 15
#endif

#define MAXIMUM "/proc/sys/kernel/shmmax"
#define TOTAL "/proc/sys/kernel/shmall"
#define SEGMENTS "/proc/sys/kernel/shmmni"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "SHM LIMITS FAIL line=%u %s errno=%d\n", __LINE__, #condition, errno); \
        exit(1); \
    } \
} while (0)

static int write_limit(int fd, const char *value)
{
    ssize_t length = (ssize_t)strlen(value);
    return pwrite(fd, value, length, 0) == length;
}

static uint64_t read_limit(int fd)
{
    char text[32];
    ssize_t length = pread(fd, text, sizeof(text) - 1U, 0);
    CHECK(length > 0 && text[length - 1] == '\n');
    text[length - 1] = 0;
    char *end;
    uint64_t value = strtoull(text, &end, 10);
    CHECK(end != text && !*end);
    return value;
}

static int open_limit(const char *path)
{
    int fd = open(path, O_RDWR | O_CLOEXEC);
    CHECK(fd >= 0);
    return fd;
}

static int create_segment(key_t key, size_t length)
{
    return shmget(key, length, IPC_CREAT | IPC_EXCL | 0600);
}

int main(void)
{
    int initial = open("/proc/self/ns/ipc", O_RDONLY | O_CLOEXEC);
    CHECK(initial >= 0 && !unshare(CLONE_NEWIPC));
    int ns = open("/proc/self/ns/ipc", O_RDONLY | O_CLOEXEC);
    int maximum = open_limit(MAXIMUM), total = open_limit(TOTAL), segments = open_limit(SEGMENTS);
    CHECK(ns >= 0);
    long page = sysconf(_SC_PAGESIZE);
    CHECK(page > 0);
    char page_limit[32];
    CHECK(snprintf(page_limit, sizeof(page_limit), "%ld\n", 2 * page) > 0);
    CHECK(write_limit(maximum, page_limit) && write_limit(total, "3\n") && write_limit(segments, "130\n"));
    CHECK(read_limit(maximum) == (uint64_t)(2 * page));
    uint64_t limits[9];
    CHECK(syscall(SYS_shmctl, 0, IPC_INFO, limits) >= 0 &&
        limits[0] == (uint64_t)(2 * page) && limits[2] == 130 && limits[3] == 130 && limits[4] == 3);
    CHECK(create_segment(IPC_PRIVATE, 2 * page + 1) == -1 && errno == EINVAL);
    int first = create_segment(112233, page + 1);
    int second = create_segment(IPC_PRIVATE, 1);
    CHECK(first >= 0 && second >= 0 && create_segment(IPC_PRIVATE, 1) == -1 && errno == ENOSPC);
    unsigned char *mapping = shmat(first, NULL, 0);
    CHECK(mapping != (void *)-1);
    *mapping = 19;
    CHECK(!shmctl(first, IPC_RMID, NULL) && shmget(112233, 0, 0) == -1 && errno == ENOENT);
    CHECK(write_limit(total, "1\n") && write_limit(segments, "1\n"));
    CHECK(!shmctl(second, IPC_RMID, NULL) && *mapping == 19);
    CHECK(create_segment(IPC_PRIVATE, 1) == -1 && errno == ENOSPC);
    CHECK(!shmdt(mapping));
    int replacement = create_segment(112233, 1);
    struct shmid_ds info;
    CHECK(replacement >= 0 && replacement != first && shmctl(first, IPC_STAT, &info) == -1);
    CHECK(!shmctl(replacement, IPC_RMID, NULL));

    CHECK(write_limit(total, "140\n") && write_limit(segments, "130\n"));
    int ids[130];
    for (int i = 0; i < 130; i++) {
        ids[i] = create_segment(220000 + i, 1);
        CHECK(ids[i] >= 0);
    }
    CHECK(create_segment(IPC_PRIVATE, 1) == -1 && errno == ENOSPC);
    int highest = syscall(SYS_shmctl, 0, IPC_INFO, limits);
    CHECK(highest >= 129 && syscall(SYS_shmctl, highest, SHM_STAT_ANY, &info) == ids[129]);
    CHECK(syscall(SYS_shmctl, 32768 + highest, SHM_STAT_ANY, &info) == ids[129]);
    CHECK(shmget(220129, 0, 0) == ids[129]);
    for (int i = 0; i < 130; i++) CHECK(!shmctl(ids[i], IPC_RMID, NULL));
    CHECK(!write_limit(segments, "32769\n") && errno == EINVAL);
    CHECK(write_limit(segments, "32768\n") && read_limit(segments) == 32768);
    CHECK(write_limit(segments, "0\n") && create_segment(IPC_PRIVATE, 1) == -1 && errno == ENOSPC);

    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(!setgid(65534) && !setuid(65534));
        CHECK(open(SEGMENTS, O_WRONLY) == -1 && errno == EACCES);
        CHECK(!write_limit(segments, "3\n") && errno == EPERM);
        _exit(0);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));

    CHECK(!unshare(CLONE_NEWIPC));
    int separate = open_limit(SEGMENTS);
    CHECK(write_limit(separate, "7\n") && read_limit(separate) == 7 && read_limit(segments) == 0);
    CHECK(write_limit(segments, "8\n") && read_limit(segments) == 8 && read_limit(separate) == 7);
    CHECK(syscall(SYS_shmctl, 0, IPC_INFO, limits) == 0 && limits[2] == 7);
    CHECK(!setns(ns, CLONE_NEWIPC) && !close(ns));
    CHECK(syscall(SYS_shmctl, 0, IPC_INFO, limits) == 0 && limits[2] == 8);
    CHECK(!setns(initial, CLONE_NEWIPC) && !close(initial));
    char text[24];
    /* Linux can release the last namespace reference from its cleanup worker. */
    ssize_t length = pread(segments, text, sizeof(text), 0);
    for (unsigned i = 0; length >= 0 && i < 200; i++) {
        const struct timespec delay = {.tv_nsec = 1000000};
        nanosleep(&delay, NULL);
        length = pread(segments, text, sizeof(text), 0);
    }
    CHECK(length == -1 && errno == ENOENT);
    CHECK(!write_limit(segments, "9\n") && errno == ENOENT);
    CHECK(!close(separate) && !close(segments) && !close(total) && !close(maximum));
    puts("SHM LIMITS PASS");
    return 0;
}
