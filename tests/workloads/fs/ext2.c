#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#ifndef FSOPEN_CLOEXEC
#define FSOPEN_CLOEXEC 1
#define FSMOUNT_CLOEXEC 1
#define FSCONFIG_SET_STRING 1
#define FSCONFIG_CMD_CREATE 6
#define MOVE_MOUNT_F_EMPTY_PATH 4
#endif
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1
#endif

#define DEVICE "/dev/vdb"
#define PRIMARY "/ext2-test"
#define ALIAS "/ext2-view"
#define CHECK(expr) do { if (!(expr)) { \
    dprintf(2, "EXT2 FAIL line=%d errno=%d: %s\n", __LINE__, errno, #expr); \
    _exit(1); \
} } while (0)

static void directory(const char *name)
{
    CHECK(!mkdir(name, 0755) || errno == EEXIST);
}

static void block_node(void)
{
    if (!access(DEVICE, F_OK)) return;
    directory("/sys");
    CHECK(!mount("sysfs", "/sys", "sysfs", 0, NULL) || errno == EBUSY);
    FILE *device = fopen("/sys/class/block/vdb/dev", "r");
    unsigned major, minor;
    if (device) CHECK(fscanf(device, "%u:%u", &major, &minor) == 2 && !fclose(device));
    else {
        /* The BoarOS VirtIO fixture has a fixed device number. */
        CHECK(!access("/sys/block/vdb/queue/logical_block_size", F_OK));
        major = 252;
        minor = 16;
    }
    CHECK(!mknod(DEVICE, S_IFBLK | 0600, makedev(major, minor)));
}

static void check_contents(int fd, const char *expected)
{
    char bytes[32] = {0};
    CHECK(pread(fd, bytes, sizeof(bytes), 0) == (ssize_t)strlen(expected));
    CHECK(!strcmp(bytes, expected));
}

static void check_type(void)
{
    struct statfs fs;
    CHECK(!statfs(PRIMARY, &fs) && fs.f_type == 0xef53);
    FILE *mounts = fopen("/proc/mounts", "r");
    char line[512];
    bool found = false;
    CHECK(mounts);
    while (fgets(line, sizeof(line), mounts))
        if (strstr(line, " " PRIMARY " ext2 ")) found = true;
    CHECK(!fclose(mounts) && found);
}

int main(void)
{
    block_node();
    directory("/proc");
    CHECK(!mount("proc", "/proc", "proc", 0, NULL) || errno == EBUSY);
    directory(PRIMARY);
    directory(ALIAS);
    CHECK(!mount(DEVICE, PRIMARY, "ext2", 0, NULL));
    check_type();
    CHECK(mount(DEVICE, ALIAS, "ext4", 0, NULL) == -1 && errno == EBUSY);
    CHECK(!mount(DEVICE, ALIAS, "ext2", 0, NULL));
    CHECK(!mkdir(PRIMARY "/old", 0750) && !mkdir(ALIAS "/new", 0750));
    int original = open(PRIMARY "/old/source", O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(original >= 0 && write(original, "source", 6) == 6 && !fsync(original));
    int victim = open(ALIAS "/new/target", O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(victim >= 0 && write(victim, "victim", 6) == 6 && !fsync(victim));
    CHECK(syscall(SYS_renameat2, AT_FDCWD, PRIMARY "/old/source", AT_FDCWD,
                  PRIMARY "/new/target", RENAME_NOREPLACE) == -1 && errno == EEXIST);
    CHECK(!rename(PRIMARY "/old/source", PRIMARY "/new/target"));
    struct stat info;
    CHECK(!fstat(victim, &info) && info.st_nlink == 0);
    check_contents(victim, "victim");
    check_contents(original, "source");
    CHECK(!close(victim) && !close(original));
    CHECK(!rmdir(PRIMARY "/old") && !umount(ALIAS) && !umount(PRIMARY));
    puts("EXT2 legacy mount, same-device ownership, rename and unlink lifetime PASS");

    int context = syscall(SYS_fsopen, "ext2", FSOPEN_CLOEXEC);
    CHECK(context >= 0);
    CHECK(!syscall(SYS_fsconfig, context, FSCONFIG_SET_STRING, "source", DEVICE, 0));
    CHECK(!syscall(SYS_fsconfig, context, FSCONFIG_CMD_CREATE, NULL, NULL, 0));
    int tree = syscall(SYS_fsmount, context, FSMOUNT_CLOEXEC, 0);
    CHECK(tree >= 0 && !close(context));
    CHECK(!syscall(SYS_move_mount, tree, "", AT_FDCWD, PRIMARY, MOVE_MOUNT_F_EMPTY_PATH));
    CHECK(!close(tree));
    check_type();
    int file = open(PRIMARY "/new/target", O_RDONLY);
    CHECK(file >= 0);
    check_contents(file, "source");
    CHECK(!unlink(PRIMARY "/new/target"));
    check_contents(file, "source");
    CHECK(!close(file) && !rmdir(PRIMARY "/new") && !umount(PRIMARY));
    puts("EXT2 new mount API and durable data PASS");
    fflush(stdout);
    if (getpid() == 1) {
        sync();
        CHECK(!reboot(RB_POWER_OFF));
    }
    return 0;
}
