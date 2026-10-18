#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/uio.h>
#include <unistd.h>

static const char *stage;
static void diagnostic_text(const char *text)
{ (void)write(STDERR_FILENO, text, strlen(text)); }
static void diagnostic_number(unsigned value)
{
    char digits[16], *end = digits + sizeof(digits), *start = end;
    do { *--start = '0' + value % 10; value /= 10; } while (value);
    (void)write(STDERR_FILENO, start, (size_t)(end - start));
}
static int failure(unsigned line, const char *expression)
{
    unsigned error = (unsigned)errno;
    diagnostic_text("fat32 failure stage="); diagnostic_text(stage);
    diagnostic_text(" line="); diagnostic_number(line);
    diagnostic_text(" errno="); diagnostic_number(error);
    diagnostic_text(": "); diagnostic_text(expression); diagnostic_text("\n");
    return 1;
}
#define CHECK(expr) do { if (!(expr)) return failure(__LINE__, #expr); } while (0)
#define FAILS(expr, expected) do { errno = 0; CHECK((expr) == -1 && errno == (expected)); } while (0)
#define BASE "/fat-test/boaros-probe"
#define LONG BASE "/Long Unicode 文件😀.txt"
#define MOVED BASE "/Moved long name.txt"
static unsigned char expected[65536], actual[65536];

static int contents(int fd, const void *data, size_t n)
{
    return n <= sizeof(actual) && pread(fd, actual, n, 0) == (ssize_t)n &&
        !memcmp(actual, data, n);
}

static int named(const char *path, const void *data, size_t n)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    int same = contents(fd, data, n);
    return close(fd) == 0 && same;
}

int main(int argc, char **argv)
{
    const char *device = argc > 1 ? argv[1] : "/dev/vdb2";
    stage = "mount";
    CHECK(argc <= 2);
    struct stat node;
    if (stat(device, &node) < 0) {
        CHECK(errno == ENOENT && !strcmp(device, "/dev/vdb2"));
        CHECK(mkdir("/dev", 0755) == 0 || errno == EEXIST);
        CHECK(mknod(device, S_IFBLK | 0600, makedev(252, 18)) == 0 && stat(device, &node) == 0);
    }
    CHECK(S_ISBLK(node.st_mode));
    if (!strcmp(device, "/dev/vdb2")) CHECK(node.st_rdev == makedev(252, 18));
    CHECK(mkdir("/fat-test", 0755) == 0 || errno == EEXIST);
    CHECK(mount(device, "/fat-test", "vfat", 0, 0) == 0);
    /* 独占自己的目录；旧失败留下的文件必须由调用者检查，不能偷偷覆盖。 */
    CHECK(mkdir(BASE, 0755) == 0);
    CHECK(mkdir(BASE "/first", 0755) == 0 && mkdir(BASE "/second", 0755) == 0 &&
          mkdir(BASE "/first/sub", 0755) == 0);

    stage = "names-and-vectors";
    int shortfd = open(BASE "/SHORT.TXT", O_CREAT | O_EXCL | O_RDWR, 0644);
    CHECK(shortfd >= 0 && write(shortfd, "SHORT", 5) == 5 && fsync(shortfd) == 0 &&
          close(shortfd) == 0 && named(BASE "/short.txt", "SHORT", 5));
    int fd = open(LONG, O_CREAT | O_EXCL | O_RDWR, 0644);
    for (size_t j = 0; j < 32768; j++) expected[j] = (j * 37U + 19U) % 251;
    CHECK(fd >= 0 && write(fd, expected, 32768) == 32768 && lseek(fd, 0, SEEK_SET) == 0 &&
          read(fd, actual, 32768) == 32768 && !memcmp(actual, expected, 32768) &&
          lseek(fd, 0, SEEK_CUR) == 32768 && contents(fd, expected, 32768));
    DIR *dir = opendir(BASE);
    CHECK(dir != 0);
    struct dirent *entry;
    int saw_long = 0, saw_short = 0;
    errno = 0;
    while ((entry = readdir(dir))) {
        saw_long += !strcmp(entry->d_name, "Long Unicode 文件😀.txt");
        saw_short += !strcmp(entry->d_name, "SHORT.TXT");
    }
    CHECK(errno == 0 && saw_long == 1 && saw_short == 1 && closedir(dir) == 0);
    char left[] = "vector", right[] = "-tail!";
    struct iovec out[2] = {{left, 6}, {right, 6}};
    char in_left[5], in_right[7];
    struct iovec in[2] = {{in_left, sizeof(in_left)}, {in_right, sizeof(in_right)}};
    CHECK(lseek(fd, 7, SEEK_SET) == 7 && pwritev(fd, out, 2, 32765) == 12 &&
          lseek(fd, 0, SEEK_CUR) == 7 && preadv(fd, in, 2, 32765) == 12 &&
          lseek(fd, 0, SEEK_CUR) == 7 && !memcmp(in_left, "vecto", 5) &&
          !memcmp(in_right, "r-tail!", 7));
    memcpy(expected + 32765, "vector-tail!", 12);
    CHECK(contents(fd, expected, 32777));

    stage = "truncate-zero";
    CHECK(ftruncate(fd, 513) == 0 && ftruncate(fd, 32777) == 0);
    memset(expected + 513, 0, 32777 - 513);
    CHECK(contents(fd, expected, 32777) && pwrite(fd, "LAST", 4, 32773) == 4);
    memcpy(expected + 32773, "LAST", 4);
    CHECK(fsync(fd) == 0 && contents(fd, expected, 32777));

    stage = "rename-open-unlink";
    int old = open(MOVED, O_CREAT | O_EXCL | O_RDWR, 0644);
    struct stat original, renamed, replaced;
    CHECK(old >= 0 && write(old, "OLD", 3) == 3 && fstat(fd, &original) == 0 &&
          fstat(old, &replaced) == 0 && original.st_ino != replaced.st_ino);
    CHECK(rename(LONG, MOVED) == 0 && stat(MOVED, &renamed) == 0 &&
          renamed.st_ino == original.st_ino && contents(fd, expected, 32777) &&
          contents(old, "OLD", 3) && close(old) == 0);
    FAILS(access(LONG, F_OK), ENOENT);
    int alias = open(BASE "/moved LONG NAME.TXT", O_RDONLY);
    CHECK(alias >= 0 && fstat(alias, &renamed) == 0 && renamed.st_ino == original.st_ino &&
          contents(alias, expected, 32777) && close(alias) == 0);
    CHECK(unlink(MOVED) == 0 && contents(fd, expected, 32777));
    FAILS(access(MOVED, F_OK), ENOENT);
    int reused = open(MOVED, O_CREAT | O_EXCL | O_RDWR, 0644);
    CHECK(reused >= 0 && fstat(reused, &renamed) == 0 && renamed.st_ino != original.st_ino &&
          write(reused, "PERSIST", 7) == 7 && pwrite(fd, "LIVE", 4, 0) == 4 &&
          pread(fd, actual, 4, 0) == 4 && !memcmp(actual, "LIVE", 4) &&
          fsync(fd) == 0 && close(fd) == 0 && fsync(reused) == 0 && close(reused) == 0);

    stage = "dirfd-case-alias-rename";
    int sub = open(BASE "/FIRST/sub", O_RDONLY | O_DIRECTORY);
    CHECK(sub >= 0 && fstat(sub, &original) == 0 &&
          rename(BASE "/first/sub", BASE "/second/Moved directory") == 0);
    int child = openat(sub, "child.txt", O_CREAT | O_EXCL | O_RDWR, 0644);
    CHECK(child >= 0 && write(child, "CHILD", 5) == 5 && fsync(child) == 0 && close(child) == 0 &&
          named(BASE "/second/Moved directory/child.txt", "CHILD", 5) &&
          stat(BASE "/SECOND/moved DIRECTORY", &renamed) == 0 &&
          original.st_ino == renamed.st_ino);
    int parent = openat(sub, "..", O_RDONLY | O_DIRECTORY);
    CHECK(parent >= 0 && fstat(parent, &original) == 0 && stat(BASE "/second", &renamed) == 0 &&
          original.st_ino == renamed.st_ino && close(parent) == 0 && close(sub) == 0);

    stage = "shared-private-mapping";
    long page = sysconf(_SC_PAGESIZE);
    CHECK(page >= 4096 && page <= 32768);
    size_t length = (size_t)page * 2;
    fd = open(BASE "/MAPPED.BIN", O_CREAT | O_EXCL | O_RDWR, 0644);
    CHECK(fd >= 0 && ftruncate(fd, (off_t)length) == 0);
    volatile unsigned char *shared = mmap(0, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(shared != MAP_FAILED && shared[17] == 0);
    shared[17] = 'S';
    volatile unsigned char *private = mmap(0, length, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    CHECK(private != MAP_FAILED && private[17] == 'S');
    private[17] = 'P';
    shared[page + 19] = 'T';
    CHECK(private[17] == 'P' && shared[17] == 'S' &&
          msync((void *)shared, length, MS_SYNC) == 0 && fsync(fd) == 0 &&
          pread(fd, actual, 1, 17) == 1 && actual[0] == 'S' &&
          pread(fd, actual, 1, page + 19) == 1 && actual[0] == 'T' &&
          munmap((void *)private, length) == 0);
    stage = "mapped-truncate-tail";
    /* 只触碰保留的末页；截断后第二页已越过 EOF，访问它应产生 SIGBUS。 */
    shared[700] = 'X';
    CHECK(ftruncate(fd, 513) == 0 && shared[512] == 0 && shared[700] == 0);
    CHECK(msync((void *)shared, (size_t)page, MS_SYNC) == 0 && fstat(fd, &original) == 0 &&
          original.st_size == 513 && ftruncate(fd, (off_t)length) == 0 &&
          shared[700] == 0 && shared[page + 19] == 0);
    shared[page + 19] = 'R';
    CHECK(msync((void *)shared, length, MS_SYNC) == 0 && fsync(fd) == 0 && close(fd) == 0);
    FAILS(umount("/fat-test"), EBUSY);
    CHECK(munmap((void *)shared, length) == 0);
    memset(expected, 0, length);
    expected[17] = 'S'; expected[page + 19] = 'R';
    CHECK(named(BASE "/MAPPED.BIN", expected, length));

    stage = "readonly-remount";
    CHECK(umount("/fat-test") == 0 && mount(device, "/fat-test", "vfat", MS_RDONLY, 0) == 0 &&
          named(MOVED, "PERSIST", 7) && named(BASE "/short.txt", "SHORT", 5) &&
          named(BASE "/MAPPED.BIN", expected, length) &&
          named(BASE "/second/Moved directory/child.txt", "CHILD", 5));
    FAILS(open(BASE "/new", O_CREAT | O_RDWR, 0644), EROFS);
    FAILS(mkdir(BASE "/newdir", 0755), EROFS);
    FAILS(unlink(MOVED), EROFS);
    FAILS(rename(MOVED, BASE "/newname"), EROFS);
    FAILS(truncate(BASE "/MAPPED.BIN", 0), EROFS);

    stage = "cleanup-unmount";
    CHECK(umount("/fat-test") == 0 && mount(device, "/fat-test", "vfat", 0, 0) == 0);
    CHECK(unlink(MOVED) == 0 && unlink(BASE "/SHORT.TXT") == 0 &&
          unlink(BASE "/MAPPED.BIN") == 0 && unlink(BASE "/second/Moved directory/child.txt") == 0 &&
          rmdir(BASE "/second/Moved directory") == 0 && rmdir(BASE "/second") == 0 &&
          rmdir(BASE "/first") == 0 && rmdir(BASE) == 0 && umount("/fat-test") == 0);
    static const char success[] = "BoarOS: FAT32 user syscall checks ok\n";
    CHECK(write(STDOUT_FILENO, success, sizeof(success) - 1) == sizeof(success) - 1);
    return 0;
}
