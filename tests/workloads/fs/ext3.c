#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/mount.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "EXT3 FAIL line=%d errno=%d %s\n", __LINE__, errno, #expr); \
    exit(1); \
} } while (0)

static const char point[] = "/ext3-check";
static const char payload[] = "/ext3-check/persist";
static const char content[] = "ext3 journal and block-mapped data\n";

static void block_device(char *path, size_t capacity)
{
    FILE *hint = fopen("/ext3-device", "r");
    char name[16], attribute[64];
    CHECK(hint && fscanf(hint, "%15s", name) == 1 && !fclose(hint));
    CHECK(!strncmp(name, "vd", 2) && strlen(name) == 3);
    CHECK(mkdir("/sys", 0755) == 0 || errno == EEXIST);
    CHECK(mount("sysfs", "/sys", "sysfs", 0, NULL) == 0);
    snprintf(attribute, sizeof(attribute), "/sys/class/block/%s/dev", name);
    FILE *numbers = fopen(attribute, "r");
    unsigned major = 252, minor = (unsigned)(name[2] - 'a') * 16;
    if (numbers)
        CHECK(fscanf(numbers, "%u:%u", &major, &minor) == 2 && !fclose(numbers));
    else
        CHECK(errno == ENOENT);
    CHECK(umount("/sys") == 0);
    CHECK(snprintf(path, capacity, "/dev/%s", name) > 0);
    CHECK(mknod(path, S_IFBLK | 0600, makedev(major, minor)) == 0 || errno == EEXIST);
}

static void check_type(void)
{
    struct statfs fs;
    CHECK(statfs(point, &fs) == 0 && fs.f_type == 0xef53);
    FILE *mounts = fopen("/proc/self/mountinfo", "r");
    CHECK(mounts);
    char *line = NULL;
    size_t capacity = 0;
    int found = 0;
    while (getline(&line, &capacity, mounts) > 0)
        if (strstr(line, " /ext3-check ") && strstr(line, " - ext3 "))
            found++;
    free(line);
    CHECK(fclose(mounts) == 0 && found == 1);
}

static void verify_content(void)
{
    char bytes[sizeof(content)];
    int fd = open(payload, O_RDONLY);
    CHECK(fd >= 0 && read(fd, bytes, sizeof(bytes)) == sizeof(bytes));
    CHECK(!memcmp(bytes, content, sizeof(content)) && close(fd) == 0);
}

int main(void)
{
    char disk[32];
    block_device(disk, sizeof(disk));
    CHECK(mkdir(point, 0755) == 0);
    CHECK(mount(disk, point, "ext3", MS_RDONLY, NULL) == 0);
    check_type();
    CHECK(open(payload, O_CREAT | O_WRONLY, 0600) == -1 && errno == EROFS);
    CHECK(mount(disk, point, "ext3", MS_REMOUNT, NULL) == 0);
    int fd = open(payload, O_CREAT | O_EXCL | O_WRONLY, 0600);
    CHECK(fd >= 0 && write(fd, content, sizeof(content)) == sizeof(content));
    CHECK(fsync(fd) == 0 && close(fd) == 0 && umount(point) == 0);

    int context = syscall(SYS_fsopen, "ext3", FSOPEN_CLOEXEC);
    CHECK(context >= 0);
    CHECK(syscall(SYS_fsconfig, context, FSCONFIG_SET_STRING, "source", disk, 0) == 0);
    CHECK(syscall(SYS_fsconfig, context, FSCONFIG_CMD_CREATE, NULL, NULL, 0) == 0);
    int tree = syscall(SYS_fsmount, context, FSMOUNT_CLOEXEC, 0);
    CHECK(tree >= 0 && close(context) == 0);
    CHECK(syscall(SYS_move_mount, tree, "", AT_FDCWD, point, MOVE_MOUNT_F_EMPTY_PATH) == 0);
    CHECK(close(tree) == 0);
    check_type();
    verify_content();
    int pick = syscall(SYS_fspick, AT_FDCWD, point, FSPICK_CLOEXEC);
    CHECK(pick >= 0);
    CHECK(syscall(SYS_fsconfig, pick, FSCONFIG_SET_FLAG, "ro", NULL, 0) == 0);
    CHECK(syscall(SYS_fsconfig, pick, FSCONFIG_CMD_RECONFIGURE, NULL, NULL, 0) == 0);
    CHECK(close(pick) == 0);
    CHECK(open(payload, O_WRONLY) == -1 && errno == EROFS);
    verify_content();
    CHECK(umount(point) == 0 && rmdir(point) == 0);
    puts("EXT3 PASS");
    return 0;
}
