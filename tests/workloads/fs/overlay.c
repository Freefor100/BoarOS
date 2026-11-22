#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fanotify.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

#ifndef AT_HANDLE_FID
#define AT_HANDLE_FID 0x200
#endif
#ifndef FSCONFIG_SET_STRING
#define FSCONFIG_SET_STRING 1
#define FSCONFIG_CMD_CREATE 6
#endif
#ifndef MOVE_MOUNT_F_EMPTY_PATH
#define MOVE_MOUNT_F_EMPTY_PATH 4
#endif

#define CHECK(test) do { if (!(test)) { \
    dprintf(2, "OVERLAY FAIL line=%d errno=%d %s\n", __LINE__, errno, #test); \
    _exit(1); \
} } while (0)

#define ROOT "/overlay-check"
#define LOWER ROOT "/lower"
#define UPPER ROOT "/upper"
#define WORK ROOT "/work"
#define VIEW ROOT "/view"
#define BIND ROOT "/bind"
#define OPTIONS "lowerdir=" LOWER ",upperdir=" UPPER ",workdir=" WORK
#define COPY_LENGTH (2 * 1024 * 1024)

static void create_file(const char *name, const char *bytes)
{
    int fd = open(name, O_CREAT | O_EXCL | O_WRONLY, 0640);
    CHECK(fd >= 0 && write(fd, bytes, strlen(bytes)) == (ssize_t)strlen(bytes) && !close(fd));
}

static void check_bytes(int fd, const char *expected)
{
    char content[32];
    size_t length = strlen(expected);
    CHECK(length < sizeof(content));
    CHECK(pread(fd, content, sizeof(content), 0) == (ssize_t)length &&
          !memcmp(content, expected, length));
}

static void check_file(const char *name, const char *expected)
{
    int fd = open(name, O_RDONLY);
    CHECK(fd >= 0);
    check_bytes(fd, expected);
    CHECK(!close(fd));
}

static void missing(const char *name)
{
    struct stat status;
    CHECK(lstat(name, &status) == -1 && errno == ENOENT);
}

static void join(pid_t child)
{
    int status;
    CHECK(waitpid(child, &status, 0) == child &&
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void populate(void)
{
    CHECK(!mkdir(ROOT, 0755) && !mkdir(LOWER, 0755) && !mkdir(UPPER, 0755) &&
          !mkdir(WORK, 0755) && !mkdir(VIEW, 0755) && !mkdir(BIND, 0755));
    create_file(LOWER "/alpha", "lower");
    CHECK(!setxattr(LOWER "/alpha", "user.copyup", "value", 5, XATTR_CREATE));
    create_file(LOWER "/shared", "physical-lower");
    create_file(LOWER "/removed", "hide");
    create_file(UPPER "/shared", "upper");
    create_file(UPPER "/only-upper", "upper-only");
    CHECK(!mkdir(LOWER "/opaque", 0755));
    create_file(LOWER "/opaque/child", "child");
    char data[16384];
    memset(data, 's', sizeof(data));
    int fd = open(LOWER "/space", O_CREAT | O_EXCL | O_WRONLY, 0640);
    CHECK(fd >= 0);
    for (size_t written = 0; written < COPY_LENGTH; written += sizeof(data))
        CHECK(write(fd, data, sizeof(data)) == sizeof(data));
    CHECK(!fsync(fd) && !close(fd));
}

static void merged_directory(const char *const *names, size_t count)
{
    unsigned seen = 0;
    DIR *directory = opendir(VIEW);
    CHECK(directory);
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) {
            CHECK(!errno);
            break;
        }
        size_t index = 0;
        while (index < count && strcmp(entry->d_name, names[index]))
            index++;
        CHECK(index < count && !(seen & (1U << index)));
        seen |= 1U << index;
    }
    CHECK(seen == (1U << count) - 1 && !closedir(directory));
}

static int notify_open(const char *name, unsigned mark_type)
{
    int fd = (int)syscall(SYS_fanotify_init, FAN_CLASS_NOTIF | FAN_REPORT_FID | FAN_NONBLOCK,
                          O_RDONLY);
    CHECK(fd >= 0);
    CHECK(!syscall(SYS_fanotify_mark, fd, FAN_MARK_ADD | mark_type,
                   FAN_OPEN | FAN_CLOSE_NOWRITE, AT_FDCWD, name));
    return fd;
}

struct file_identity {
    struct statfs fs;
    _Alignas(struct file_handle) unsigned char bytes[sizeof(struct file_handle) + 128];
};

static void file_identity(const char *name, struct file_identity *identity)
{
    struct file_handle *handle = (void *)identity->bytes;
    int mount_id;
    handle->handle_bytes = sizeof(identity->bytes) - sizeof(*handle);
    CHECK(!statfs(name, &identity->fs));
    CHECK(!name_to_handle_at(AT_FDCWD, name, handle, &mount_id, AT_HANDLE_FID));
    CHECK(handle->handle_bytes > 0);
}

static void received_open(int fd, const struct file_identity *expected)
{
    _Alignas(struct fanotify_event_metadata) unsigned char bytes[512];
    struct fanotify_event_metadata *event = (void *)bytes;
    const struct file_handle *expected_handle = (const void *)expected->bytes;
    ssize_t count = read(fd, bytes, sizeof(bytes));
    CHECK(count > 0 && FAN_EVENT_OK(event, count) && event->event_len == (uint32_t)count);
    CHECK(event->fd == FAN_NOFD && event->mask == (FAN_OPEN | FAN_CLOSE_NOWRITE) &&
          event->pid == getpid());
    CHECK(event->metadata_len == sizeof(*event) &&
          (size_t)count >= sizeof(*event) + sizeof(struct fanotify_event_info_fid) +
                           sizeof(struct file_handle));
    const struct fanotify_event_info_fid *info = (const void *)(bytes + event->metadata_len);
    const struct file_handle *handle = (const void *)info->handle;
    CHECK(info->hdr.info_type == FAN_EVENT_INFO_TYPE_FID &&
          sizeof(*event) + info->hdr.len <= (size_t)count);
    CHECK(!memcmp(&info->fsid, &expected->fs.f_fsid, sizeof(info->fsid)) &&
          handle->handle_bytes == expected_handle->handle_bytes &&
          handle->handle_type == expected_handle->handle_type &&
          sizeof(*info) + sizeof(*handle) + handle->handle_bytes <= info->hdr.len &&
          !memcmp(handle->f_handle, expected_handle->f_handle, handle->handle_bytes));
    CHECK(read(fd, bytes, sizeof(bytes)) == -1 && errno == EAGAIN);
    CHECK(!close(fd));
}

static void watched_layers(void)
{
    struct file_identity lower, upper, merged;
    file_identity(LOWER "/alpha", &lower);
    file_identity(UPPER "/shared", &upper);
    file_identity(VIEW "/alpha", &merged);

    /* The backing inode and merged inode have independent event identities. */
    int lower_group = notify_open(LOWER "/alpha", FAN_MARK_INODE);
    int upper_group = notify_open(UPPER "/shared", FAN_MARK_INODE);
    int overlay_group = notify_open(VIEW "/alpha", FAN_MARK_INODE);
    int mount_group = notify_open(LOWER, FAN_MARK_MOUNT);
    CHECK(!mount(VIEW, BIND, NULL, MS_BIND, NULL));
    check_file(BIND "/alpha", "lower");
    check_file(BIND "/shared", "upper");
    CHECK(!umount(BIND));
    received_open(lower_group, &lower);
    received_open(upper_group, &upper);
    received_open(overlay_group, &merged);
    char bytes[256];
    CHECK(read(mount_group, bytes, sizeof(bytes)) == -1 && errno == EAGAIN);
    CHECK(!close(mount_group));
}

static void raced_copy_up(void)
{
    int original = open(VIEW "/alpha", O_RDONLY);
    CHECK(original >= 0);
    const char *mapping = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, original, 0);
    CHECK(mapping != MAP_FAILED && mapping[0] == 'l');
    int ready[2], start[2];
    CHECK(!pipe(ready) && !pipe(start));
    pid_t workers[2];
    for (unsigned i = 0; i < 2; i++) {
        workers[i] = fork();
        CHECK(workers[i] >= 0);
        if (workers[i] == 0) {
            char ticket;
            CHECK(!close(ready[0]) && !close(start[1]));
            CHECK(write(ready[1], "R", 1) == 1 && read(start[0], &ticket, 1) == 1);
            int fd = open(VIEW "/alpha", O_RDWR);
            CHECK(fd >= 0);
            CHECK(pwrite(fd, i ? "R" : "L", 1, i ? 4 : 0) == 1);
            CHECK(!fsync(fd) && !close(fd));
            _exit(0);
        }
    }
    CHECK(!close(ready[1]) && !close(start[0]));
    for (unsigned i = 0; i < 2; i++) {
        char ticket;
        CHECK(read(ready[0], &ticket, 1) == 1);
    }
    CHECK(write(start[1], "GG", 2) == 2 && !close(start[1]) && !close(ready[0]));
    join(workers[0]);
    join(workers[1]);
    check_bytes(original, "LoweR");
    CHECK(!memcmp(mapping, "lower", 5) && !munmap((void *)mapping, 4096));
    CHECK(!close(original));
    check_file(LOWER "/alpha", "lower");
    check_file(UPPER "/alpha", "LoweR");
    struct stat source, copy;
    CHECK(!stat(LOWER "/alpha", &source) && !stat(UPPER "/alpha", &copy));
    CHECK(source.st_mode == copy.st_mode && source.st_uid == copy.st_uid &&
          source.st_gid == copy.st_gid);
    char attribute[5];
    CHECK(getxattr(UPPER "/alpha", "user.copyup", attribute, sizeof(attribute)) == sizeof(attribute) &&
          !memcmp(attribute, "value", sizeof(attribute)));
}

static void mount_with_context(void)
{
    int fs = (int)syscall(SYS_fsopen, "overlay", 1);
    CHECK(fs >= 0);
    /* Each path is resolved when supplied, even if CREATE has a different cwd. */
    CHECK(!chdir(ROOT));
    CHECK(!syscall(SYS_fsconfig, fs, FSCONFIG_SET_STRING, "lowerdir", "lower", 0));
    CHECK(!syscall(SYS_fsconfig, fs, FSCONFIG_SET_STRING, "upperdir", "upper", 0));
    CHECK(!syscall(SYS_fsconfig, fs, FSCONFIG_SET_STRING, "workdir", "work", 0));
    CHECK(!chdir("/"));
    CHECK(!syscall(SYS_fsconfig, fs, FSCONFIG_CMD_CREATE, NULL, NULL, 0));
    int mount_fd = (int)syscall(SYS_fsmount, fs, 1, 0);
    CHECK(mount_fd >= 0 && !close(fs));
    CHECK(!syscall(SYS_move_mount, mount_fd, "", AT_FDCWD, VIEW, MOVE_MOUNT_F_EMPTY_PATH));
    CHECK(!close(mount_fd));
}

static void no_space_for_copy(void)
{
    const off_t step = 65536;
    int filler = open(ROOT "/filler", O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(filler >= 0);
    struct statfs stats;
    CHECK(!fstatfs(filler, &stats));
    off_t free_bytes = (off_t)stats.f_bfree * stats.f_bsize;
    /* The ABI runner supplies a private 32 MiB ext4 fixture. */
    CHECK(free_bytes > COPY_LENGTH && free_bytes < 64 * 1024 * 1024);
    off_t length = free_bytes - step;
    while (fallocate(filler, 0, 0, length)) {
        CHECK(errno == ENOSPC && length > free_bytes - COPY_LENGTH / 2);
        length -= step;
    }
    CHECK(!fstatfs(filler, &stats) && (off_t)stats.f_bfree * stats.f_bsize < COPY_LENGTH);
    CHECK(open(VIEW "/space", O_RDWR) == -1 && errno == ENOSPC);
    missing(UPPER "/space");
    int fd = open(VIEW "/space", O_RDONLY);
    char byte;
    CHECK(fd >= 0 && read(fd, &byte, 1) == 1 && byte == 's' && !close(fd));
    CHECK(!close(filler) && !unlink(ROOT "/filler"));
    fd = open(VIEW "/space", O_RDWR);
    CHECK(fd >= 0 && pread(fd, &byte, 1, COPY_LENGTH - 1) == 1 && byte == 's');
    CHECK(!fsync(fd) && !close(fd));
}

int main(void)
{
    populate();
    CHECK(!mount("overlay", VIEW, "overlay", 0, OPTIONS));
    struct statfs status;
    CHECK(!statfs(VIEW, &status) && (unsigned long)status.f_type == 0x794c7630UL);
    check_file(VIEW "/alpha", "lower");
    check_file(VIEW "/shared", "upper");
    const char *const before[] = {".", "..", "alpha", "shared", "removed", "only-upper",
                                  "opaque", "space"};
    merged_directory(before, sizeof(before) / sizeof(*before));
    watched_layers();
    CHECK(!rename(VIEW "/only-upper", VIEW "/moved"));
    missing(VIEW "/only-upper");
    check_file(VIEW "/moved", "upper-only");
    CHECK(!rename(VIEW "/moved", VIEW "/only-upper"));
    create_file(VIEW "/rename-source", "new");
    create_file(VIEW "/rename-target", "old");
    int replaced=open(VIEW "/rename-target",O_RDONLY);
    CHECK(replaced>=0 && !rename(VIEW "/rename-source",VIEW "/rename-target"));
    check_file(VIEW "/rename-target","new");
    check_bytes(replaced,"old");
    CHECK(!close(replaced) && !unlink(VIEW "/rename-target"));
    raced_copy_up();
    no_space_for_copy();
    CHECK(!unlink(VIEW "/removed") && !unlink(VIEW "/shared"));
    CHECK(!unlink(VIEW "/opaque/child") && !rmdir(VIEW "/opaque") && !mkdir(VIEW "/opaque", 0710));
    if (!rename(VIEW "/opaque", VIEW "/opaque-moved")) {
        missing(VIEW "/opaque/child");
        CHECK(!rename(VIEW "/opaque-moved", VIEW "/opaque"));
    } else CHECK(errno == EXDEV);
    create_file(VIEW "/new", "created");
    int source=open(VIEW "/only-upper",O_RDONLY);
    int target=open(VIEW "/new",O_WRONLY|O_TRUNC);
    CHECK(source>=0 && target>=0);
    CHECK(syscall(SYS_copy_file_range,source,NULL,target,NULL,10,0)==10);
    CHECK(!fsync(target) && !close(source) && !close(target));
    CHECK(!umount(VIEW));

    mount_with_context();
    missing(VIEW "/removed");
    missing(VIEW "/shared");
    missing(VIEW "/opaque/child");
    check_file(LOWER "/removed", "hide");
    check_file(LOWER "/shared", "physical-lower");
    check_file(LOWER "/opaque/child", "child");
    check_file(UPPER "/new", "upper-only");
    const char *const after[] = {".", "..", "alpha", "only-upper", "opaque", "new", "space"};
    merged_directory(after, sizeof(after) / sizeof(*after));
    struct file_identity merged;
    file_identity(VIEW "/alpha", &merged);
    int group = notify_open(VIEW "/alpha", FAN_MARK_INODE);
    check_file(VIEW "/alpha", "LoweR");
    int directory = open(VIEW, O_DIRECTORY | O_RDONLY);
    CHECK(directory >= 0 && !umount2(VIEW, MNT_DETACH));
    missing(VIEW "/alpha");
    received_open(group, &merged);
    int fd = openat(directory, "new", O_RDONLY);
    CHECK(fd >= 0);
    check_bytes(fd, "upper-only");
    CHECK(!close(fd) && !close(directory));
    puts("OVERLAY PASS scope copy-up whiteouts remount detach");
    return 0;
}
