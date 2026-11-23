#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define KEYS_PATH "/proc/sys/kernel/keys"
#define KC(op, a, b, c, d) syscall(SYS_keyctl, (long)(op), (long)(a), (long)(b), (long)(c), (long)(d))
#define ADD(name, bytes, ring) syscall(SYS_add_key, "user", name, payload, (long)(bytes), (long)(ring))
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "key quota FAIL line=%d %s errno=%d\n", __LINE__, #expr, errno); exit(1); \
} } while (0)
#define ERR(expr, code) CHECK((expr) == -1 && errno == (code))

enum { MAXKEYS, MAXBYTES, ROOT_MAXKEYS, ROOT_MAXBYTES, QUOTA_COUNT };
enum { GET_ID = 0, UPDATE = 2, CHOWN = 4, LINK = 8, READ = 11, THREAD = -1, PROCESS = -2 };
static const char *const names[] = {"maxkeys", "maxbytes", "root_maxkeys", "root_maxbytes"};
static char paths[QUOTA_COUNT][64];
static int saved[QUOTA_COUNT];
static pid_t owner;
static const char payload[32] = "key quota payload";

static int get_limit(unsigned field)
{
    char text[32];
    int fd = open(paths[field], O_RDONLY);
    CHECK(fd >= 0);
    ssize_t size = read(fd, text, sizeof(text) - 1);
    CHECK(size > 1 && text[size - 1] == '\n');
    text[size] = 0;
    char *end;
    long result = strtol(text, &end, 10);
    CHECK(end == text + size - 1 && result > 0 && result <= INT_MAX);
    CHECK(read(fd, text, 1) == 0 && close(fd) == 0);
    return (int)result;
}

static void set_limit(unsigned field, int value)
{
    char text[24];
    int size = snprintf(text, sizeof(text), "%d\n", value);
    int fd = open(paths[field], O_WRONLY);
    CHECK(fd >= 0 && write(fd, text, (size_t)size) == size && close(fd) == 0);
    CHECK(get_limit(field) == value);
}

static void restore_limits(void)
{
    if (getpid() != owner) return;
    for (unsigned field = 0; field < QUOTA_COUNT; field++) set_limit(field, saved[field]);
}

static void check_controls(void)
{
    DIR *dir = opendir(KEYS_PATH);
    CHECK(dir != NULL);
    unsigned found = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)))
        for (unsigned field = 0; field < QUOTA_COUNT; field++)
            if (!strcmp(entry->d_name, names[field])) found |= 1U << field;
    CHECK(found == (1U << QUOTA_COUNT) - 1 && closedir(dir) == 0);
    for (unsigned field = 0; field < QUOTA_COUNT; field++) {
        snprintf(paths[field], sizeof(paths[field]), "%s/%s", KEYS_PATH, names[field]);
        struct stat st;
        CHECK(stat(paths[field], &st) == 0 && S_ISREG(st.st_mode));
        CHECK((st.st_mode & 0777) == 0644 && st.st_uid == 0 && st.st_gid == 0);
        saved[field] = get_limit(field);
    }
    CHECK(atexit(restore_limits) == 0);
    for (unsigned field = 0; field < QUOTA_COUNT; field++) {
        set_limit(field, INT_MAX);
        int fd = open(paths[field], O_WRONLY);
        CHECK(fd >= 0);
        ERR(write(fd, "0\n", 2), EINVAL);
        ERR(write(fd, "-1\n", 3), EINVAL);
        ERR(write(fd, "2147483648\n", 11), EINVAL);
        ERR(write(fd, "xyz\n", 4), EINVAL);
        CHECK(close(fd) == 0 && get_limit(field) == INT_MAX);
    }
}

static void check_root_quota(void)
{
    long ring = KC(GET_ID, THREAD, 1, 0, 0);
    long other = KC(GET_ID, PROCESS, 1, 0, 0);
    CHECK(ring > 0 && other > 0);
    long key = ADD("root-key", 16, ring);
    CHECK(key > 0 && KC(CHOWN, key, 13244, -1, 0) == 0);
    set_limit(ROOT_MAXKEYS, 1);
    ERR(ADD("extra", 1, ring), EDQUOT);
    ERR(KC(CHOWN, key, 0, -1, 0), EDQUOT);
    set_limit(ROOT_MAXKEYS, INT_MAX);
    CHECK(KC(CHOWN, key, 0, -1, 0) == 0);
    set_limit(ROOT_MAXBYTES, 1);
    ERR(ADD("extra", 1, ring), EDQUOT);
    ERR(KC(UPDATE, key, payload, 17, 0), EDQUOT);
    ERR(KC(LINK, key, other, 0, 0), EDQUOT);
    CHECK(ADD("root-key", 16, ring) == key);
    CHECK(KC(UPDATE, key, payload, 8, 0) == 0);
    CHECK(KC(READ, key, NULL, 0, 0) == 8);
    set_limit(ROOT_MAXBYTES, INT_MAX);
    CHECK(KC(LINK, key, other, 0, 0) == 0 && ADD("extra", 1, ring) > 0);
    set_limit(ROOT_MAXKEYS, 1);
    set_limit(ROOT_MAXBYTES, 1);
}

static void check_user_quota(int bytes)
{
    set_limit(MAXKEYS, bytes ? INT_MAX : 2);
    set_limit(MAXBYTES, bytes ? 31 : INT_MAX);
    int fd = open(paths[MAXKEYS], O_WRONLY);
    CHECK(fd >= 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        CHECK(setuid(bytes ? 13243 : 13242) == 0);
        CHECK(get_limit(MAXKEYS) == (bytes ? INT_MAX : 2));
        ERR(open(paths[MAXKEYS], O_WRONLY), EACCES);
        ERR(write(fd, "1\n", 2), EPERM);
        long ring = KC(GET_ID, THREAD, 1, 0, 0);
        CHECK(ring > 0);
        /* The thread ring costs 5 bytes and this key and its link cost 26. */
        long key = ADD("bytes", 16, ring);
        CHECK(key > 0);
        ERR(ADD("other", 1, ring), EDQUOT);
        CHECK(ADD("bytes", 16, ring) == key);
        if (bytes) {
            ERR(KC(UPDATE, key, payload, 17, 0), EDQUOT);
            CHECK(KC(UPDATE, key, payload, 8, 0) == 0);
            CHECK(ADD("b", 1, ring) > 0);
        } else {
            CHECK(KC(UPDATE, key, payload, 17, 0) == 0);
        }
        _exit(0);
    }
    int status;
    CHECK(close(fd) == 0 && waitpid(child, &status, 0) == child && status == 0);
}

int main(void)
{
    CHECK(!mkdir("/proc", 0555) || errno == EEXIST);
    if (access("/proc/meminfo", R_OK)) CHECK(mount("proc", "/proc", "proc", 0, NULL) == 0);
    owner = getpid();
    check_controls();
    check_root_quota();
    check_user_quota(0);
    check_user_quota(1);
    restore_limits();
    owner = -1;
    puts("key quota PASS");
    return 0;
}
