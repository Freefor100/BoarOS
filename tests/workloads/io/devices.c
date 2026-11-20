#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/loop.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define CHECK(expr) do { if (!(expr)) { printf("DEVICES FAIL line %d (%s) errno=%d\n", __LINE__, #expr, errno); return 1; } } while (0)

static int full(void)
{
    char bytes[32], zeros[32] = {0};
    int p[2];
    CHECK(mknod("/full-alias", S_IFCHR | 0666, makedev(1, 7)) == 0);
    int fd = open("/full-alias", O_RDWR);
    CHECK(fd >= 0);
    memset(bytes, 0xa5, sizeof(bytes));
    CHECK(read(fd, bytes, sizeof(bytes)) == sizeof(bytes) && !memcmp(bytes, zeros, sizeof(bytes)));
    CHECK(write(fd, "x", 1) == -1 && errno == ENOSPC);
    CHECK(write(fd, "", 0) == -1 && errno == ENOSPC);
    struct pollfd pollfd = {fd, POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM, 0};
    CHECK(poll(&pollfd, 1, 0) == 1 && pollfd.revents == pollfd.events);
    CHECK(pipe(p) == 0);
    CHECK(splice(fd, NULL, p[1], NULL, sizeof(bytes), 0) == sizeof(bytes));
    memset(bytes, 0xa5, sizeof(bytes));
    CHECK(read(p[0], bytes, sizeof(bytes)) == sizeof(bytes) && !memcmp(bytes, zeros, sizeof(bytes)));
    CHECK(write(p[1], "x", 1) == 1);
    CHECK(splice(p[0], NULL, fd, NULL, 1, 0) == -1 && errno == EINVAL);
    CHECK(read(p[0], bytes, 1) == 1 && bytes[0] == 'x');
    CHECK(close(p[0]) == 0 && close(p[1]) == 0 && close(fd) == 0);
    puts("DEVICES full PASS");
    return 0;
}

static int tree(void)
{
    struct stat a, b;
    CHECK(mkdir("/view-a", 0755) == 0 && mkdir("/view-b", 0755) == 0);
    CHECK(mount("devtmpfs", "/view-a", "devtmpfs", 0, NULL) == 0);
    CHECK(mount("devtmpfs", "/view-b", "devtmpfs", 0, NULL) == 0);
    CHECK(stat("/view-a/full", &a) == 0 && S_ISCHR(a.st_mode) && a.st_rdev == makedev(1, 7));
    CHECK(stat("/view-b/full", &b) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino);
    int fd = open("/view-a/full", O_WRONLY);
    CHECK(fd >= 0 && write(fd, "x", 1) < 0 && errno == ENOSPC && close(fd) == 0);
    CHECK(stat("/view-a/vda", &a) == 0 && S_ISBLK(a.st_mode));
    unsigned long long size = 0;
    fd = open("/view-b/vda", O_RDONLY);
    CHECK(fd >= 0 && ioctl(fd, BLKGETSIZE64, &size) == 0 && size > 0 && close(fd) == 0);
    if (access("/partition-expected", F_OK) == 0) {
        CHECK(stat("/view-a/vdb1", &a) == 0 && S_ISBLK(a.st_mode));
        CHECK(stat("/view-b/vdb", &b) == 0 && S_ISBLK(b.st_mode));
        CHECK(major(a.st_rdev) == major(b.st_rdev) && minor(a.st_rdev) == minor(b.st_rdev) + 1);
        fd = open("/view-b/vdb1", O_RDONLY);
        CHECK(fd >= 0 && ioctl(fd, BLKGETSIZE64, &size) == 0 && size == 2 * 1024 * 1024);
        CHECK(close(fd) == 0);
        puts("DEVICES partition PASS");
    }
    CHECK(mkdir("/view-a/marker", 0700) == 0 && stat("/view-b/marker", &a) == 0);
    int ctl = open("/view-b/loop-control", O_RDWR);
    CHECK(ctl >= 0);
    CHECK(ioctl(ctl, LOOP_CTL_ADD, 91) == 91);
    CHECK(stat("/view-a/loop91", &a) == 0 && S_ISBLK(a.st_mode) && a.st_rdev == makedev(7, 91));
    DIR *dir = opendir("/view-b");
    CHECK(dir);
    struct dirent *ent; int found = 0;
    while ((ent = readdir(dir))) if (!strcmp(ent->d_name, "loop91")) found++;
    CHECK(closedir(dir) == 0 && found == 1);
    int path = open("/view-a/loop91", O_PATH);
    CHECK(path >= 0);
    fd = open("/view-b/loop91", O_RDONLY);
    CHECK(fd >= 0 && ioctl(ctl, LOOP_CTL_REMOVE, 91) < 0 && errno == EBUSY);
    CHECK(stat("/view-a/loop91", &b) == 0 && a.st_ino == b.st_ino && close(fd) == 0);
    CHECK(ioctl(ctl, LOOP_CTL_REMOVE, 91) == 0);
    CHECK(stat("/view-b/loop91", &b) < 0 && errno == ENOENT);
    CHECK(fstat(path, &b) == 0 && a.st_ino == b.st_ino);
    CHECK(ioctl(ctl, LOOP_CTL_ADD, 91) == 91);
    CHECK(stat("/view-b/loop91", &b) == 0 && a.st_ino != b.st_ino);
    CHECK(close(path) == 0);
    CHECK(rename("/view-a/loop91", "/view-a/old-loop") == 0);
    CHECK(mknod("/view-b/loop91", S_IFBLK | 0600, makedev(7, 91)) == 0);
    CHECK(stat("/view-a/loop91", &a) == 0 && ioctl(ctl, LOOP_CTL_REMOVE, 91) == 0);
    CHECK(stat("/view-b/loop91", &b) == 0 && a.st_ino == b.st_ino);
    CHECK(unlink("/view-b/loop91") == 0 && unlink("/view-b/old-loop") == 0);
    CHECK(close(ctl) == 0);
    CHECK(umount("/view-a") == 0 && umount("/view-b") == 0);
    CHECK(mount("devtmpfs", "/view-a", "devtmpfs", 0, NULL) == 0);
    CHECK(stat("/view-a/marker", &a) == 0 && S_ISDIR(a.st_mode));
    CHECK(umount("/view-a") == 0);
    puts("DEVICES devtmpfs PASS");
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int errors = full();
    errors += tree();
    if (!errors) puts("DEVICES PASS");
    sync();
    reboot(RB_POWER_OFF);
    return !!errors;
}
