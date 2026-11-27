#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fanotify.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "EXT4 NAMESPACE FAIL %d: %s errno=%d\n", __LINE__, #x, errno); \
    if (getpid() == 1) reboot(RB_POWER_OFF); \
    exit(1); \
} } while (0)
#define FAIL(error, x) do { errno = 0; CHECK((x) == -1 && errno == (error)); } while (0)

static struct stat status(const char *path)
{
    struct stat st;
    CHECK(!lstat(path, &st));
    return st;
}

static int create(const char *path, char byte)
{
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0660);
    CHECK(fd >= 0 && write(fd, &byte, 1) == 1);
    return fd;
}

static long exchange(const char *first, const char *second, unsigned flags)
{
    return syscall(SYS_renameat2, AT_FDCWD, first, AT_FDCWD, second, flags);
}

static void concurrent_exchange(ino_t first, ino_t second)
{
    int start[2];
    CHECK(!pipe(start));
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        char ready;
        CHECK(!close(start[1]) && read(start[0], &ready, 1) == 1 && !close(start[0]));
        for (unsigned i = 0; i < 32; ++i) {
            ino_t a = status("left/tree").st_ino, b = status("left/peer").st_ino;
            CHECK((a == first || a == second) && (b == first || b == second));
        }
        _exit(0);
    }
    CHECK(!close(start[0]) && write(start[1], "x", 1) == 1 && !close(start[1]));
    for (unsigned i = 0; i < 32; ++i)
        CHECK(!exchange("left/tree", "left/peer", 2));
    int result;
    CHECK(waitpid(child, &result, 0) == child && WIFEXITED(result) && !WEXITSTATUS(result));
}

static void namespace_changes(void)
{
    CHECK(!mkdir("left", 0755) && !mkdir("right", 0755));
    int old = create("left/source", 's');
    int victim = create("right/victim", 'v');
    long page = sysconf(_SC_PAGESIZE);
    CHECK(page > 0 && !ftruncate(victim, page) && !fsync(victim));
    char *view = mmap(NULL, page, PROT_READ, MAP_SHARED, victim, 0);
    CHECK(view != MAP_FAILED && view[0] == 'v');
    CHECK(!link("left/source", "left/hard"));
    struct stat source = status("left/source");
    CHECK(source.st_nlink == 2 && status("left/hard").st_ino == source.st_ino);
    FAIL(EEXIST, exchange("left/source", "right/victim", 1));
    CHECK(!rename("left/source", "right/victim"));
    CHECK(status("right/victim").st_ino == source.st_ino);
    CHECK(!unlink("left/hard") && status("right/victim").st_nlink == 1);
    struct stat st;
    CHECK(!fstat(victim, &st) && st.st_nlink == 0 && view[0] == 'v');
    CHECK(!close(victim) && view[0] == 'v' && !munmap(view, page));
    CHECK(!close(old));
    FAIL(ENOENT, exchange("right/victim", "left/absent", 2));

    CHECK(!mkdir("left/tree", 0755));
    CHECK(!close(create("left/tree/child", 'c')));
    ino_t tree = status("left/tree").st_ino;
    CHECK(!exchange("left/tree", "right/victim", 2));
    CHECK(status("left/tree").st_ino == source.st_ino);
    CHECK(status("right/victim").st_ino == tree);
    CHECK(status("right/victim/..").st_ino == status("right").st_ino);
    CHECK(status("right/victim/child").st_size == 1);
    CHECK(!close(create("left/peer", 'p')));
    ino_t peer = status("left/peer").st_ino;
    CHECK(!exchange("left/tree", "left/peer", 2));
    CHECK(status("left/tree").st_ino == peer && status("left/peer").st_ino == source.st_ino);
    concurrent_exchange(peer, source.st_ino);
    CHECK(!mkdir("left/branch", 0755));
    CHECK(!rename("left/branch", "right/branch"));
    CHECK(status("right/branch/..").st_ino == status("right").st_ino);
    CHECK(!rmdir("right/branch"));
    CHECK(!unlink("right/victim/child") && !rmdir("right/victim"));
    CHECK(!unlink("left/peer") && !unlink("left/tree"));
    CHECK(!rmdir("left") && !rmdir("right"));
}

static void creation(void)
{
    CHECK(!mkfifo("fifo", 0600) && S_ISFIFO(status("fifo").st_mode));
    CHECK(!mknod("character", S_IFCHR | 0600, makedev(1, 3)));
    CHECK(S_ISCHR(status("character").st_mode) && status("character").st_rdev == makedev(1, 3));
    CHECK(!mknod("block", S_IFBLK | 0600, makedev(7, 1)));
    CHECK(S_ISBLK(status("block").st_mode) && status("block").st_rdev == makedev(7, 1));
    struct sockaddr_un address = {.sun_family = AF_UNIX, .sun_path = "local-socket"};
    int socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(socket_fd >= 0 && !bind(socket_fd, (void *)&address, sizeof(address)));
    CHECK(S_ISSOCK(status("local-socket").st_mode) && !close(socket_fd));

    char target[400], actual[400];
    memset(target, 't', sizeof(target));
    target[sizeof(target) - 1] = 0;
    CHECK(!symlink(target, "symlink") && readlink("symlink", actual, sizeof(actual)) == sizeof(target) - 1);
    CHECK(!memcmp(target, actual, sizeof(target) - 1));

    static const uint32_t acl[] = {2, 0x00070001, UINT32_MAX, 0x00040002, 1234,
        0x00050004, UINT32_MAX, 0x00050010, UINT32_MAX, 0x00000020, UINT32_MAX};
    CHECK(!mkdir("parent-acl", 0777));
    CHECK(!setxattr("parent-acl", "system.posix_acl_default", acl, sizeof(acl), 0));
    CHECK(!close(create("parent-acl/child", 'a')));
    CHECK(getxattr("parent-acl/child", "system.posix_acl_access", NULL, 0) == sizeof(acl));
    CHECK(!mkdir("parent-acl/directory", 0777));
    CHECK(getxattr("parent-acl/directory", "system.posix_acl_default", NULL, 0) == sizeof(acl));

    int fd = open("parent-acl", O_TMPFILE | O_RDWR, 0660);
    CHECK(fd >= 0 && write(fd, "tmp", 3) == 3 && !fsync(fd));
    struct stat st;
    CHECK(!fstat(fd, &st) && !st.st_nlink);
    CHECK(fgetxattr(fd, "system.posix_acl_access", NULL, 0) == sizeof(acl));
    CHECK(!linkat(fd, "", AT_FDCWD, "parent-acl/linked", AT_EMPTY_PATH));
    CHECK(status("parent-acl/linked").st_ino == st.st_ino);
    CHECK(!close(fd));
    fd = open(".", O_TMPFILE | O_EXCL | O_RDWR, 0600);
    CHECK(fd >= 0 && write(fd, "d", 1) == 1);
    FAIL(ENOENT, linkat(fd, "", AT_FDCWD, "exclusive", AT_EMPTY_PATH));
    CHECK(!close(fd));
}

static void file_handle(void)
{
    int fan = syscall(SYS_fanotify_init, FAN_CLASS_NOTIF | FAN_REPORT_DFID_NAME_TARGET | FAN_NONBLOCK,
                      O_RDONLY);
    CHECK(fan >= 0);
    CHECK(!syscall(SYS_fanotify_mark, fan, FAN_MARK_ADD | FAN_MARK_FILESYSTEM,
                   (uint64_t)FAN_CREATE, AT_FDCWD, "."));
    CHECK(!close(create("watched", 'w')));
    union {
        struct fanotify_event_metadata metadata;
        char bytes[256];
    } event;
    ssize_t length = read(fan, &event, sizeof(event));
    CHECK(length > 0 && event.metadata.mask == FAN_CREATE);
    CHECK(event.metadata.fd == FAN_NOFD && event.metadata.event_len <= length);
    struct file_handle *reported = NULL;
    for (size_t offset = event.metadata.metadata_len; offset < event.metadata.event_len;) {
        CHECK(event.metadata.event_len - offset >= sizeof(struct fanotify_event_info_header));
        struct fanotify_event_info_fid *info = (void *)(event.bytes + offset);
        CHECK(info->hdr.len >= sizeof(info->hdr) &&
              info->hdr.len <= event.metadata.event_len - offset);
        if (info->hdr.info_type == FAN_EVENT_INFO_TYPE_FID) {
            CHECK(info->hdr.len >= sizeof(*info) + sizeof(struct file_handle));
            reported = (void *)info->handle;
            CHECK(reported->handle_bytes <= info->hdr.len - sizeof(*info) - sizeof(*reported));
        }
        offset += info->hdr.len;
    }
    CHECK(reported);
    union {
        struct file_handle handle;
        char bytes[128];
    } exported = {.handle.handle_bytes = 120};
    int mount_id;
    CHECK(!name_to_handle_at(AT_FDCWD, "watched", &exported.handle, &mount_id, 0));
    CHECK(reported->handle_bytes == exported.handle.handle_bytes);
    CHECK(reported->handle_type == exported.handle.handle_type);
    CHECK(!memcmp(reported->f_handle, exported.handle.f_handle, reported->handle_bytes));
    CHECK(!close(fan));
}

int main(void)
{
    setbuf(stdout, NULL);
    CHECK(!mkdir("/namespace", 0777) && !chdir("/namespace"));
    namespace_changes();
    creation();
    file_handle();
    CHECK(!chdir("/"));
    sync();
    CHECK(!mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY, NULL));
    CHECK(!mount(NULL, "/", NULL, MS_REMOUNT, NULL));
    CHECK(status("/namespace/parent-acl/linked").st_size == 3);
    puts("EXT4 NAMESPACE ALL PASS");
    if (getpid() == 1) CHECK(!reboot(RB_POWER_OFF));
    return 0;
}
