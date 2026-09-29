#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static int stage;
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "multi-mount failure stage=%d line=%d errno=%d: %s\n", stage, __LINE__, errno, #expr); return 1; } } while (0)
#define FAILS(expr, expected) do { errno = 0; CHECK((expr) == -1 && errno == (expected)); } while (0)

static int value(const char *path, const char *expected)
{
    char data[32] = {0};
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, data, strlen(expected));
    int error = close(fd);
    return !error && n == (ssize_t)strlen(expected) && !memcmp(data, expected, (size_t)n);
}
static int directory(const char *path)
{ return mkdir(path, 0755) == 0 || errno == EEXIST; }

int main(void)
{
    stage = 1;
    CHECK(directory("/second") && directory("/duplicate"));
    if (mknod("/arbitrary-disk", S_IFBLK | 0600, makedev(252, 16))) CHECK(errno == EEXIST);
    if (mknod("/same-device-alias", S_IFBLK | 0600, makedev(252, 16))) CHECK(errno == EEXIST);
    struct stat st;
    CHECK(stat("/arbitrary-disk", &st) == 0 && S_ISBLK(st.st_mode) && st.st_rdev == makedev(252, 16));
    FAILS(open("/arbitrary-disk", O_RDONLY), ENOTSUP);
#ifdef READ_ONLY_BOOT
    CHECK(mount("/arbitrary-disk", "/second", "ext4", MS_RDONLY, 0) == 0);
#else
    CHECK(mount("/arbitrary-disk", "/second", "ext4", 0, 0) == 0);
#endif
    FAILS(mount("/same-device-alias", "/duplicate", "ext4", 0, 0), EBUSY);
    stage = 2;
    CHECK(value("/identity", "ROOT") && value("/second/identity", "SECOND"));
#ifdef READ_ONLY_BOOT
    stage = 10;
    CHECK(value("/second/persistent", "PERSIST") && value("/second/hard", "PERSIST") &&
          value("/second/final", "FINISH"));
    CHECK(stat("/second/persistent", &st) == 0 && st.st_nlink == 2);
    int fd = open("/second/persistent", O_RDONLY);
    char c = 0;
    CHECK(fd >= 0 && pread(fd, &c, 1, 64) == 1 && c == 'M' && close(fd) == 0);
    FAILS(open("/second/new", O_CREAT | O_RDWR, 0600), EROFS);
    puts("BoarOS: multi-mount read-only persistence checks ok");
#else
    stage = 3;
    int fd = open("/second/persistent", O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(fd >= 0 && write(fd, "PERSIST", 7) == 7 && ftruncate(fd, 4096) == 0);
    CHECK(link("/second/persistent", "/second/hard") == 0 && fstat(fd, &st) == 0 && st.st_nlink == 2);
    CHECK(rename("/second/hard", "/second/renamed") == 0 && unlink("/second/renamed") == 0 &&
          fstat(fd, &st) == 0 && st.st_nlink == 1 && link("/second/persistent", "/second/hard") == 0);
    stage = 4;
    CHECK(directory("/second/memory") && mount("none", "/second/memory", "tmpfs", 0, "size=8192,nr_inodes=8") == 0);
    FAILS(link("/second/persistent", "/second/memory/link"), EXDEV);
    FAILS(rename("/second/persistent", "/second/memory/moved"), EXDEV);
    FAILS(umount("/second"), EBUSY);
    int ram = open("/second/memory/data", O_CREAT | O_RDWR, 0600);
    CHECK(ram >= 0 && ftruncate(ram, 4096) == 0 && write(ram, "RAM", 3) == 3);
    FAILS(umount("/second/memory"), EBUSY);
    volatile char *memory = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, ram, 0);
    CHECK(memory != MAP_FAILED && memory[0] == 'R');
    CHECK(unlink("/second/memory/data") == 0 && close(ram) == 0);
    FAILS(umount("/second/memory"), EBUSY);
    memory[0] = 'Z';
    CHECK(memory[0] == 'Z' && munmap((void *)memory, 4096) == 0 && umount("/second/memory") == 0);
    stage = 5;
    volatile char *mapped = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(mapped != MAP_FAILED && mapped[0] == 'P');
    mapped[64] = 'M';
    CHECK(msync((void *)mapped, 4096, MS_SYNC) == 0 && fsync(fd) == 0 && close(fd) == 0);
    FAILS(umount("/second"), EBUSY);
    CHECK(munmap((void *)mapped, 4096) == 0);
    int dir = open("/second", O_RDONLY | O_DIRECTORY);
    CHECK(dir >= 0);
    FAILS(umount("/second"), EBUSY);
    CHECK(close(dir) == 0 && umount("/second") == 0);
    stage = 6;
    CHECK(mount("/same-device-alias", "/second", "ext4", 0, 0) == 0 &&
          value("/second/persistent", "PERSIST") && value("/second/hard", "PERSIST"));
    CHECK(stat("/second/persistent", &st) == 0 && st.st_nlink == 2);
    fd = open("/second/persistent", O_RDONLY);
    char c = 0;
    CHECK(fd >= 0 && pread(fd, &c, 1, 64) == 1 && c == 'M' && close(fd) == 0);
    fd = open("/second/final", O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(fd >= 0 && write(fd, "FINISH", 6) == 6 && close(fd) == 0);
    puts("BoarOS: multi-mount write and remount checks ok");
#endif
    /* Deliberately retain the mount. Root teardown must stop its cache worker,
     * flush its journal and release its claim before resetting both devices. */
    return 42;
}
