#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/stat.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "GETDENTS FAIL line=%d errno=%d: %s\n", __LINE__, errno, #x); exit(1); } } while (0)
#define ENTRIES 300U
static unsigned char expected[32768], actual[32768];
static uint64_t number(const unsigned char *bytes, unsigned count)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < count; i++) value |= (uint64_t)bytes[i] << (8U * i);
    return value;
}
static size_t complete_prefix(size_t bytes, uint64_t *cookie)
{
    size_t complete = 0;
    while (complete + 19 <= bytes) {
        size_t length = number(expected + complete + 16, 2);
        CHECK(length >= 24 && length <= 280 && !(length & 7));
        if (length > bytes - complete) break;
        *cookie = number(expected + complete + 8, 8);
        complete += length;
    }
    return complete;
}
static long getdents(int fd, void *buffer, size_t size)
{
    return syscall(SYS_getdents64, fd, buffer, size);
}
static void filename(char *name, unsigned index)
{
    int prefix = sprintf(name, "%03u-", index);
    unsigned size = index == 151 ? 255 : 40;
    memset(name + prefix, 'a' + index % 26, size - (unsigned)prefix);
    name[size] = 0;
}
static void rewind_directory(int fd) { CHECK(lseek(fd, 0, SEEK_SET) == 0); }
static void check_suffix(int fd, size_t prefix, size_t size)
{
    long result = getdents(fd, actual, sizeof(actual));
    CHECK(result == (long)(size - prefix) && !memcmp(actual, expected + prefix, (size_t)result));
    CHECK(getdents(fd, actual, sizeof(actual)) == 0);
}
static void directory_cases(const char *path, const char *backend)
{
    CHECK(!mkdir(path, 0700));
    int fd = open(path, O_RDONLY | O_DIRECTORY); CHECK(fd >= 0);
    for (unsigned i = 0; i < ENTRIES; i++) {
        char name[256]; filename(name, i);
        int file = openat(fd, name, O_CREAT | O_EXCL | O_WRONLY, 0600);
        CHECK(file >= 0 && !close(file));
    }
    long result = getdents(fd, expected, sizeof(expected));
    CHECK(result > 16384 && result < (long)sizeof(expected));
    size_t size = (size_t)result;
    unsigned entries = 0, seen[ENTRIES] = {0};
    for (size_t offset = 0; offset < size;) {
        size_t length = number(expected + offset + 16, 2);
        CHECK(length >= 24 && length <= size - offset && !(length & 7));
        const char *name = (const char *)expected + offset + 19;
        CHECK(memchr(name, 0, length - 19));
        if (entries < 2) CHECK(!strcmp(name, entries ? ".." : "."));
        else {
            char *end; unsigned long index = strtoul(name, &end, 10);
            CHECK(index < ENTRIES && *end == '-' && !seen[index]++);
            CHECK(strlen(name) == (index == 151 ? 255U : 40U));
        }
        offset += length; entries++;
    }
    CHECK(entries == ENTRIES + 2);
    CHECK(!getdents(fd, actual, sizeof(actual)));
    off_t eof = lseek(fd, 0, SEEK_CUR); CHECK(eof >= 0);
    CHECK(!getdents(fd, actual, 0) && lseek(fd, 0, SEEK_CUR) == eof);
    printf("GETDENTS %s batch-long-name-eof PASS\n", backend);

    const size_t limits[] = {0, 1, 23, 24, 25, 47, 48, 63, 64, 280, 4095, 4096};
    for (unsigned i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
        rewind_directory(fd);
        uint64_t cookie = 0;
        size_t prefix = complete_prefix(limits[i], &cookie);
        errno = 0; result = getdents(fd, actual, limits[i]);
        CHECK(prefix ? result == (long)prefix : result == -1 && errno == EINVAL);
        CHECK(lseek(fd, 0, SEEK_CUR) == (off_t)cookie);
        if (prefix) CHECK(!memcmp(actual, expected, prefix));
        check_suffix(fd, prefix, size);
    }
    /* Seek to a saved opaque d_off, never an entry ordinal. */
    uint64_t cookie = number(expected + 8, 8);
    CHECK(lseek(fd, (off_t)cookie, SEEK_SET) == (off_t)cookie);
    CHECK(getdents(fd, actual, 24) == 24 && !memcmp(actual, expected + 24, 24));
    printf("GETDENTS %s short-buffer-cookie PASS\n", backend);

    long page = sysconf(_SC_PAGESIZE); CHECK(page >= 4096);
    unsigned char *mapping = mmap(NULL, (size_t)page * 2, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(mapping != MAP_FAILED && !mprotect(mapping + page, (size_t)page, PROT_NONE));
    for (unsigned i = 0; i < 10; i++) {
        size_t accessible = limits[i];
        rewind_directory(fd);
        uint64_t accepted = 0; size_t prefix = complete_prefix(accessible, &accepted);
        errno = 0; result = getdents(fd, mapping + page - accessible, 512);
        CHECK(prefix ? result == (long)prefix : result == -1 && errno == EFAULT);
        CHECK(lseek(fd, 0, SEEK_CUR) == (off_t)accepted);
        if (prefix) CHECK(!memcmp(mapping + page - accessible, expected, prefix));
        check_suffix(fd, prefix, size);
    }
    CHECK(!munmap(mapping, (size_t)page * 2));
    printf("GETDENTS %s efault-complete-prefix PASS\n", backend);

    rewind_directory(fd);
    int duplicate = dup(fd), independent = open(path, O_RDONLY | O_DIRECTORY);
    CHECK(duplicate >= 0 && independent >= 0);
    CHECK(getdents(duplicate, actual, 24) == 24 && !memcmp(actual, expected, 24));
    CHECK(getdents(fd, actual, 24) == 24 && !memcmp(actual, expected + 24, 24));
    CHECK(getdents(independent, actual, 24) == 24 && !memcmp(actual, expected, 24));
    CHECK(!close(duplicate) && !close(independent));
    rewind_directory(fd);
    int channel[2]; CHECK(!pipe(channel));
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        CHECK(!close(channel[0]));
        long count = getdents(fd, actual, 512); CHECK(count > 0);
        CHECK(write(channel[1], &count, sizeof(count)) == sizeof(count));
        CHECK(write(channel[1], actual, (size_t)count) == count);
        CHECK(!close(channel[1])); _exit(0);
    }
    CHECK(!close(channel[1]));
    long count; CHECK(read(channel[0], &count, sizeof(count)) == sizeof(count));
    CHECK(count > 0 && count <= 512);
    size_t received = 0;
    while (received < (size_t)count) {
        ssize_t bytes = read(channel[0], actual + received, (size_t)count - received);
        CHECK(bytes > 0); received += (size_t)bytes;
    }
    CHECK(!memcmp(actual, expected, received));
    int status; CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    CHECK(!close(channel[0]));
    check_suffix(fd, received, size);
    CHECK(!close(fd));
    printf("GETDENTS %s dup-fork-independent-offset PASS\n", backend);
}
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    directory_cases("/enum-ext4", "ext4");
    CHECK(!mkdir("/enum-memory", 0700));
    CHECK(!mount("tmpfs", "/enum-memory", "tmpfs", 0, ""));
    directory_cases("/enum-memory/items", "tmpfs");
    CHECK(!umount("/enum-memory"));
    puts("GETDENTS CONTRACT PASS");
    return 0;
}
