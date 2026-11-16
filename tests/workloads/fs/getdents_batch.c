#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* Same ELF and pre-created directory on both kernels. Creation and sync are
 * outside the timed enumeration; warmup is explicit, never called a cold run.
 * Usage: getdents_batch DIRECTORY ENTRIES BUFFER_BYTES ROUNDS [prepare] */
static uint64_t now(clockid_t clock)
{
    struct timespec value;
    assert(!clock_gettime(clock, &value));
    return (uint64_t)value.tv_sec * 1000000000 + value.tv_nsec;
}
static uint64_t number(const unsigned char *data, unsigned bytes)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < bytes; i++) value |= (uint64_t)data[i] << (8U * i);
    return value;
}
static unsigned scan(int fd, unsigned char *buffer, size_t capacity, unsigned expected,
    uint64_t *calls, uint64_t *bytes, unsigned char *seen)
{
    unsigned count = 0;
    memset(seen, 0, expected);
    assert(lseek(fd, 0, SEEK_SET) == 0);
    for (;;) {
        long size = syscall(SYS_getdents64, fd, buffer, capacity);
        assert(size >= 0); (*calls)++;
        if (!size) break;
        *bytes += (uint64_t)size;
        for (size_t at = 0; at < (size_t)size;) {
            size_t length = number(buffer + at + 16, 2);
            assert(length >= 24 && !(length & 7) && length <= (size_t)size - at);
            const char *name = (const char *)buffer + at + 19;
            assert(memchr(name, 0, length - 19));
            if (strcmp(name, ".") && strcmp(name, "..")) {
                char *end;
                unsigned long index = strtoul(name, &end, 10);
                assert(*end == 0 && index < expected);
                assert(!seen[index]++);
                count++;
            }
            at += length;
        }
    }
    assert(count == expected);
    return count;
}
int main(int argc, char **argv)
{
    assert(argc == 5 || argc == 6);
    unsigned entries = (unsigned)strtoul(argv[2], NULL, 10);
    size_t capacity = (size_t)strtoul(argv[3], NULL, 10);
    unsigned rounds = (unsigned)strtoul(argv[4], NULL, 10);
    assert(entries && entries <= 100000 && capacity >= 32 && capacity <= 1048576 && rounds);
    if (argc == 6) {
        assert(!strcmp(argv[5], "prepare"));
        assert(!mkdir(argv[1], 0700) || errno == EEXIST);
        int directory = open(argv[1], O_RDONLY | O_DIRECTORY); assert(directory >= 0);
        for (unsigned i = 0; i < entries; i++) {
            char name[32]; snprintf(name, sizeof(name), "%08u", i);
            int file = openat(directory, name, O_CREAT | O_EXCL | O_WRONLY, 0600);
            assert(file >= 0); assert(!close(file));
        }
        assert(!fsync(directory) && !close(directory));
    }
    int fd = open(argv[1], O_RDONLY | O_DIRECTORY); assert(fd >= 0);
    unsigned char *buffer = malloc(capacity), *seen = malloc(entries); assert(buffer && seen);
    /* Touch user pages and populate the filesystem cache before timing. */
    memset(buffer, 0, capacity);
    uint64_t calls = 0, bytes = 0;
    scan(fd, buffer, capacity, entries, &calls, &bytes, seen);
    calls = bytes = 0;
    uint64_t cpu = now(CLOCK_PROCESS_CPUTIME_ID), wall = now(CLOCK_MONOTONIC);
    for (unsigned i = 0; i < rounds; i++) scan(fd, buffer, capacity, entries, &calls, &bytes, seen);
    wall = now(CLOCK_MONOTONIC) - wall; cpu = now(CLOCK_PROCESS_CPUTIME_ID) - cpu;
    printf("GETDENTS warm entries=%llu rounds=%u buffer=%zu calls=%llu bytes=%llu wall_ns=%llu cpu_ns=%llu\n",
        (unsigned long long)entries * rounds, rounds, capacity,
        (unsigned long long)calls, (unsigned long long)bytes,
        (unsigned long long)wall, (unsigned long long)cpu);
    free(seen); free(buffer); assert(!close(fd));
    return 0;
}
