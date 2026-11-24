#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fanotify.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../../common/raw_syscall.h"

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "CACHE NOWAIT FAIL line=%d %s errno=%d\n", \
            __LINE__, #condition, errno); exit(1); } } while (0)

enum { nr_preadv2 = 286, nr_pwritev2 = 287, nr_cachestat = 451, rwf_nowait = 8 };

struct cache_range { uint64_t off, len; };
struct cache_stats { uint64_t cached, dirty, writeback, evicted, recently_evicted; };

static long cache_stat(int fd, const struct cache_range *range,
                       struct cache_stats *stats, unsigned flags)
{
    return test_syscall6(nr_cachestat, fd, (uintptr_t)range, (uintptr_t)stats, flags, 0, 0);
}

static long nowait(int fd, struct iovec *iov, unsigned count, int64_t offset)
{
    return test_syscall6(nr_preadv2, fd, (uintptr_t)iov, count, offset, 0, rwf_nowait);
}

static struct cache_stats stats_for(int fd, uint64_t off, uint64_t length)
{
    const struct cache_range range = {off, length};
    struct cache_stats stats;
    CHECK(cache_stat(fd, &range, &stats, 0) == 0);
    CHECK(stats.recently_evicted <= stats.evicted);
    CHECK(stats.dirty <= stats.cached && stats.writeback <= stats.cached);
    return stats;
}

static void linux_cold_read(size_t page, unsigned char *out)
{
    const char *name = "/cache-nowait-direct.data";
    void *aligned = NULL;
    CHECK(posix_memalign(&aligned, page, page) == 0);
    memset(aligned, 0x73, page);
    int direct = open(name, O_WRONLY | O_CREAT | O_EXCL | O_DIRECT, 0600);
    CHECK(direct >= 0);
    CHECK(pwrite(direct, aligned, page, 0) == (ssize_t)page && close(direct) == 0);
    int fd = open(name, O_RDONLY);
    CHECK(fd >= 0 && stats_for(fd, 0, page).cached == 0);
    struct iovec first = {out, page};
    long cold = nowait(fd, &first, 1, 0);
    /* The fixed Linux kernel may submit a read and see it complete before
     * returning; the direct fixture proves initial cache absence either way. */
    CHECK(cold == -EAGAIN || (cold == (long)page && memcmp(out, aligned, page) == 0));
    CHECK(close(fd) == 0 && unlink(name) == 0);
    free(aligned);
}

static void fanotify_nowait(int fd, struct iovec *iov)
{
    int group = (int)syscall(SYS_fanotify_init, FAN_CLASS_CONTENT | FAN_NONBLOCK, O_RDONLY);
    CHECK(group >= 0);
    CHECK(syscall(SYS_fanotify_mark, group, FAN_MARK_ADD, (uint64_t)FAN_ACCESS_PERM,
        AT_FDCWD, "/cache-nowait.data") == 0);
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    CHECK(nowait(fd, iov, 1, -1) == -EAGAIN && lseek(fd, 0, SEEK_CUR) == 0);
    struct fanotify_event_metadata event;
    CHECK(read(group, &event, sizeof(event)) == -1 && errno == EAGAIN);
    CHECK(close(group) == 0 && nowait(fd, iov, 1, 0) == (long)iov->iov_len);
    puts("CACHE NOWAIT CASE fanotify-permission-no-queue PASS");
}

int main(void)
{
    struct utsname identity;
    CHECK(uname(&identity) == 0);
    int boaros = strstr(identity.version, "BoarOS") != NULL;
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    int fd = open("/cache-nowait.data", O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(page && page < 65536 && fd >= 0);
    unsigned char *data = malloc(3 * page);
    unsigned char *out = malloc(3 * page);
    CHECK(data && out);
    for (size_t i = 0; i < 3 * page; i++) data[i] = (unsigned char)(i * 13 + 29);
    memset(out, 0, 3 * page);
    CHECK(pwrite(fd, data, 3 * page, 0) == (ssize_t)(3 * page));

    const struct timespec old_times[2] = {{1, 0}, {2, 0}};
    CHECK(futimens(fd, old_times) == 0);
    struct iovec iov[] = {{out, page - 3}, {out + page - 3, page + 3}};
    CHECK(nowait(fd, iov, 2, 0) == (long)(2 * page));
    CHECK(memcmp(data, out, 2 * page) == 0 && lseek(fd, 0, SEEK_CUR) == 0);
    struct stat inode;
    CHECK(fstat(fd, &inode) == 0 && inode.st_atim.tv_sec > old_times[0].tv_sec);
    struct cache_stats stats = stats_for(fd, 0, 0);
    CHECK(stats.cached == 3 && stats.dirty == 3 && !stats.writeback && !stats.evicted);
    CHECK(fsync(fd) == 0 && posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM) == 0);
    stats = stats_for(fd, page - 1, 2);
    CHECK(stats.cached == 2 && !stats.dirty && !stats.writeback && !stats.evicted);
    puts("CACHE NOWAIT CASE warm-and-cache-counters PASS");

    CHECK(posix_fadvise(fd, page, page, POSIX_FADV_DONTNEED) == 0);
    stats = stats_for(fd, page, page);
    CHECK(!stats.dirty && !stats.writeback && !stats.evicted);
    long mixed = nowait(fd, iov, 2, 0);
    CHECK(lseek(fd, 0, SEEK_CUR) == 0);
    if (boaros) {
        CHECK(!stats.cached && mixed == (long)page);
        CHECK(nowait(fd, iov, 2, -1) == (long)page && lseek(fd, 0, SEEK_CUR) == (off_t)page);
        CHECK(nowait(fd, iov, 2, -1) == -EAGAIN && lseek(fd, 0, SEEK_CUR) == (off_t)page);
        CHECK(nowait(fd, iov, 2, page) == -EAGAIN);
        CHECK(stats_for(fd, page, page).cached == 0);
    } else {
        /* DONTNEED can retain a partially covered large folio. */
        CHECK(mixed == -EAGAIN || mixed == (long)page || mixed == (long)(2 * page));
        linux_cold_read(page, out);
        CHECK(lseek(fd, page, SEEK_SET) == (off_t)page);
    }
    CHECK(nowait(fd, iov, 2, 3 * page) == 0);
    CHECK(pread(fd, out, page, page) == (ssize_t)page && memcmp(out, data + page, page) == 0);
    CHECK(nowait(fd, iov, 2, -1) == (long)(2 * page) && lseek(fd, 0, SEEK_CUR) == (off_t)(3 * page));
    CHECK(nowait(fd, iov, 2, -1) == 0 && lseek(fd, 0, SEEK_CUR) == (off_t)(3 * page));
    puts("CACHE NOWAIT CASE cold-mixed-and-offset PASS");

    struct iovec broken[] = {{out, 9}, {(void *)1, 1}};
    CHECK(nowait(fd, broken + 1, 1, 0) == -EFAULT);
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    CHECK(nowait(fd, broken, 2, -1) == 9 && lseek(fd, 0, SEEK_CUR) == 9);
    CHECK(test_syscall6(nr_pwritev2, fd, (uintptr_t)iov, 2, 0, 0, rwf_nowait) == -EOPNOTSUPP);
    CHECK(test_syscall6(nr_preadv2, fd, (uintptr_t)iov, 2, 0, 0, 0x80000000U | rwf_nowait) == -EOPNOTSUPP);
    void *missing = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(missing != MAP_FAILED);
    struct iovec user_page = {missing, page};
    if (boaros) {
        CHECK(nowait(fd, &user_page, 1, 0) == -EAGAIN);
        int direct = open("/cache-nowait.data", O_RDONLY | O_DIRECT);
        CHECK(direct >= 0 && nowait(direct, iov, 2, 0) == -EOPNOTSUPP && close(direct) == 0);
        int memory = memfd_create("cache-nowait", 0);
        CHECK(memory >= 0 && nowait(memory, iov, 2, 0) == -EOPNOTSUPP);
        CHECK(close(memory) == 0);
    }
    CHECK(mprotect(missing, page, PROT_NONE) == 0);
    CHECK(nowait(fd, &user_page, 1, 0) == -EFAULT);
    CHECK(munmap(missing, page) == 0);
    if (boaros) fanotify_nowait(fd, iov);
    puts("CACHE NOWAIT CASE faults-and-unsupported-flags PASS");

    const struct cache_range range = {0, 0};
    CHECK(cache_stat(-1, (void *)1, (void *)1, 1) == -EBADF);
    CHECK(cache_stat(fd, (void *)1, (void *)1, 1) == -EFAULT);
    CHECK(cache_stat(fd, &range, (void *)1, 1) == -EINVAL);
    CHECK(cache_stat(fd, &range, (void *)1, 0) == -EFAULT);
    int ro = open("/cache-nowait.data", O_RDONLY);
    CHECK(ro >= 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        CHECK(setuid(65534) == 0);
        CHECK(cache_stat(ro, (void *)1, (void *)1, 1) == -EFAULT);
        CHECK(cache_stat(ro, &range, (void *)1, 1) == -EPERM);
        CHECK(cache_stat(fd, &range, &stats, 0) == 0);
        _exit(0);
    }
    int child_status;
    CHECK(waitpid(child, &child_status, 0) == child && WIFEXITED(child_status) &&
        WEXITSTATUS(child_status) == 0);
    CHECK(close(ro) == 0);
    CHECK(!stats_for(fd, 3 * page, 0).cached);
    CHECK(ftruncate(fd, page) == 0);
    CHECK(!stats_for(fd, page, 0).cached && !stats_for(fd, page, 0).evicted);
    CHECK(close(fd) == 0 && unlink("/cache-nowait.data") == 0);
    free(out);
    free(data);
    puts("CACHE NOWAIT CASE cachestat-validation-and-truncate PASS");
    puts("CACHE NOWAIT PASS");
    return 0;
}
