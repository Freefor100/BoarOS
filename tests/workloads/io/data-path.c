#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_FILES 4
#define MAX_REQUEST 65536
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "IO FAIL line=%d expression=%s errno=%d\n", __LINE__, #x, errno); exit(1); } } while (0)
static unsigned char buffer[MAX_REQUEST];
static unsigned files, request_bytes;
static uint64_t file_bytes;
static int cost_fd = -1;
static FILE *evidence;
static char result_path[1024];
struct result {
    int fd;
    uint64_t bytes, calls, start, end, sync_end, size_before, size_after, checksum;
    uint64_t growth[5];
    unsigned growth_count;
};

static uint64_t now(void)
{
    struct timespec t;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (uint64_t)t.tv_sec * 1000000000U + (uint64_t)t.tv_nsec;
}

static unsigned char pattern(unsigned id, uint64_t offset, unsigned epoch)
{
    uint32_t word = ((uint32_t)id * 0x9e3779b9U) ^ ((uint32_t)(offset / 4) * 0x85ebca6bU) ^ (epoch * 0xc2b2ae35U);
    return (unsigned char)(word >> ((offset % 4) * 8));
}

static void fill(unsigned id, uint64_t offset, size_t count, unsigned epoch)
{
    for (size_t j = 0; j < count; ++j) buffer[j] = pattern(id, offset + j, epoch);
}

static uint64_t check(unsigned id, uint64_t offset, size_t count, unsigned epoch)
{
    uint64_t checksum = 0;
    for (size_t j = 0; j < count; ++j) {
        CHECK(buffer[j] == pattern(id, offset + j, epoch));
        checksum += buffer[j];
    }
    return checksum;
}

static void exact(int fd, size_t count, int writing, uint64_t *calls)
{
    size_t done = 0;
    while (done < count) {
        ssize_t n = writing ? write(fd, buffer + done, count - done) : read(fd, buffer + done, count - done);
        if (calls) ++*calls;
        if (n < 0 && errno == EINTR) continue;
        CHECK(n > 0 && (size_t)n <= count - done);
        done += (size_t)n;
    }
}

static uint64_t size_of(int fd)
{
    struct stat s;
    CHECK(fstat(fd, &s) == 0 && S_ISREG(s.st_mode) && s.st_size >= 0);
    return (uint64_t)s.st_size;
}

static uint64_t verify(int fd, unsigned id, unsigned epoch)
{
    CHECK(size_of(fd) == file_bytes && lseek(fd, 0, SEEK_SET) == 0);
    uint64_t checksum = 0;
    for (uint64_t offset = 0; offset < file_bytes;) {
        size_t count = file_bytes - offset;
        if (count > sizeof(buffer)) count = sizeof(buffer);
        exact(fd, count, 0, NULL);
        checksum += check(id, offset, count, epoch);
        offset += count;
    }
    CHECK(read(fd, buffer, 1) == 0);
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    return checksum;
}

static void report_proc(const char *path, const char *name)
{
    FILE *input = fopen(path, "r"); CHECK(input);
    fprintf(evidence, "IO %s BEGIN\n", name);
    char chunk[4096]; size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), input)) != 0)
        CHECK(fwrite(chunk, 1, n, evidence) == n);
    CHECK(!ferror(input) && fclose(input) == 0);
    fprintf(evidence, "IO %s END\n", name);
}

static void stop_observation(void)
{
    if (cost_fd < 0) { evidence = fopen(result_path, "w"); CHECK(evidence); return; }
    uint64_t deadline = now() + 10000000000ULL;
    while (write(cost_fd, "end\n", 4) != 4) {
        CHECK(errno == EBUSY && now() < deadline);
        struct timespec pause = {.tv_nsec = 20000000};
        (void)nanosleep(&pause, NULL);
    }
    evidence = fopen(result_path, "w"); CHECK(evidence);
    report_proc("/proc/boaros_mem_stats", "MEM_PEAK");
    report_proc("/proc/boaros_cost", "COST");
    report_proc("/proc/meminfo", "MEM_AFTER");
    CHECK(close(cost_fd) == 0);
}

static void timed_out(int signal_number)
{
    (void)signal_number;
    const char message[] = "IO FAIL timeout\n";
    (void)write(2, message, sizeof(message) - 1);
    _exit(1);
}

int main(int argc, char **argv)
{
    signal(SIGALRM, timed_out);
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *config_path = argc > 1 ? argv[1] : "/io-config";
    char backend[16], operation[16], completion[16];
    FILE *config = fopen(config_path, "r"); CHECK(config);
    unsigned long long configured_bytes;
    unsigned timeout_seconds = 240;
    int fields = fscanf(config, "%15s %15s %15s %u %llu %u %u", backend, operation, completion,
                        &files, &configured_bytes, &request_bytes, &timeout_seconds);
    CHECK((fields == 6 || fields == 7) && timeout_seconds > 0 && timeout_seconds <= 3600 && fclose(config) == 0);
    alarm(timeout_seconds);
    file_bytes = configured_bytes;
    int memory = !strcmp(backend, "tmpfs"), append = !strcmp(operation, "append");
    int writing = append || !strcmp(operation, "overwrite"), hot = !strcmp(operation, "hot-read");
    int sync_mode = !strcmp(completion, "fsync") ? 1 : !strcmp(completion, "fdatasync") ? 2 : 0;
    CHECK((memory || !strcmp(backend, "ext4")) && (writing || hot || !strcmp(operation, "cold-read") || !strcmp(operation, "read")));
    CHECK((sync_mode || !strcmp(completion, "cache")) && (writing || !sync_mode));
    CHECK(!(memory && !strcmp(operation, "cold-read")));
    CHECK((files == 1 || files == 4) && file_bytes > 0 && file_bytes <= 64U * 1024U * 1024U);
    CHECK(request_bytes == 1024 || request_bytes == 4096 || request_bytes == 65536);
    int n = snprintf(result_path, sizeof(result_path), "%s.results", config_path);
    CHECK(n > 0 && (size_t)n < sizeof(result_path));
    const char *directory = argc > 2 ? argv[2] : memory ? "/memory" : "/data";
    if (memory && argc <= 2) CHECK(mount("tmpfs", directory, "tmpfs", 0, "size=384m") == 0);
    struct result results[MAX_FILES] = {{0}};
    for (unsigned id = 0; id < files; ++id) {
        char path[1024]; n = snprintf(path, sizeof(path), "%s/file-%u", directory, id);
        CHECK(n > 0 && (size_t)n < sizeof(path));
        if (memory) {
            int setup = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600); CHECK(setup >= 0);
            if (!append) for (uint64_t offset = 0; offset < file_bytes;) {
                size_t count = file_bytes - offset;
                if (count > sizeof(buffer)) count = sizeof(buffer);
                fill(id, offset, count, 0); exact(setup, count, 1, NULL); offset += count;
            }
            CHECK(close(setup) == 0);
        }
        int flags = writing ? O_RDWR : O_RDONLY;
        if (append) flags |= O_APPEND;
        results[id].fd = open(path, flags); CHECK(results[id].fd >= 0);
        results[id].size_before = size_of(results[id].fd);
        CHECK(results[id].size_before == (append ? 0 : file_bytes));
        if (hot) (void)verify(results[id].fd, id, 0);
    }
    if (access("/observe", F_OK) == 0) {
        if (mount("proc", "/proc", "proc", 0, NULL) < 0) CHECK(errno == EBUSY);
        cost_fd = open("/proc/boaros_cost_control", O_WRONLY | O_CLOEXEC); CHECK(cost_fd >= 0);
        CHECK(write(cost_fd, "begin\n", 6) == 6);
    }
    uint64_t start = now();
    /* 单任务逐请求轮转；每文件进展可核对，但不声称覆盖并发调度公平性。 */
    for (uint64_t offset = 0; offset < file_bytes;) {
        size_t count = file_bytes - offset;
        if (count > request_bytes) count = request_bytes;
        for (unsigned id = 0; id < files; ++id) {
            struct result *r = &results[id];
            if (!offset) r->start = now();
            if (writing) fill(id, offset, count, 1);
            exact(r->fd, count, writing, &r->calls);
            r->bytes += count;
            if (!writing) r->checksum += check(id, offset, count, 0);
            if (writing && (offset == 0 || r->bytes == file_bytes / 4 || r->bytes == file_bytes / 2 || r->bytes == file_bytes * 3 / 4 || r->bytes == file_bytes)) {
                uint64_t size = size_of(r->fd);
                CHECK(size == (append ? r->bytes : file_bytes));
                CHECK(r->growth_count < 5); r->growth[r->growth_count++] = size;
            }
            if (r->bytes == file_bytes) r->end = now();
        }
        offset += count;
    }
    uint64_t data_end = now();
    unsigned sync_calls = 0;
    if (sync_mode) for (unsigned id = 0; id < files; ++id) {
        CHECK((sync_mode == 1 ? fsync(results[id].fd) : fdatasync(results[id].fd)) == 0);
        ++sync_calls; results[id].sync_end = now();
    }
    uint64_t end = sync_mode ? now() : data_end;
    stop_observation();
    uint64_t verify_start = now();
    for (unsigned id = 0; id < files; ++id) {
        struct result *r = &results[id];
        r->size_after = size_of(r->fd); CHECK(r->size_after == file_bytes && r->bytes == file_bytes);
        if (writing) r->checksum = verify(r->fd, id, 1);
        else CHECK(read(r->fd, buffer, 1) == 0);
    }
    uint64_t verify_end = now(), cleanup_start = verify_end;
    unsigned cleanup_sync_calls = 0;
    if (writing && !sync_mode) for (unsigned id = 0; id < files; ++id) {
        CHECK(fsync(results[id].fd) == 0); ++cleanup_sync_calls;
    }
    uint64_t cleanup_end = now();
    for (unsigned id = 0; id < files; ++id) CHECK(close(results[id].fd) == 0);
    fprintf(evidence, "IO WINDOW start_ns=%llu data_end_ns=%llu end_ns=%llu elapsed_ns=%llu data_ns=%llu sync_ns=%llu bytes=%llu sync_calls=%u verify_ns=%llu cleanup_sync_ns=%llu cleanup_sync_calls=%u complete=1\n",
            (unsigned long long)start, (unsigned long long)data_end, (unsigned long long)end,
            (unsigned long long)(end - start), (unsigned long long)(data_end - start), (unsigned long long)(end - data_end),
            (unsigned long long)(file_bytes * files), sync_calls,
            (unsigned long long)(verify_end - verify_start), (unsigned long long)(cleanup_end - cleanup_start), cleanup_sync_calls);
    for (unsigned id = 0; id < files; ++id) {
        const struct result *r = &results[id];
        fprintf(evidence, "IO FILE id=%u bytes=%llu calls=%llu start_ns=%llu end_ns=%llu sync_end_ns=%llu size_before=%llu size_after=%llu checksum=%llu complete=1\n",
                id, (unsigned long long)r->bytes, (unsigned long long)r->calls, (unsigned long long)r->start,
                (unsigned long long)r->end, (unsigned long long)r->sync_end, (unsigned long long)r->size_before,
                (unsigned long long)r->size_after, (unsigned long long)r->checksum);
        for (unsigned j = 0; j < r->growth_count; ++j)
            fprintf(evidence, "IO SIZE id=%u sample=%u bytes=%llu\n", id, j, (unsigned long long)r->growth[j]);
    }
    if (memory && argc <= 2) CHECK(umount(directory) == 0);
    fputs("IO PASS all\n", evidence);
    CHECK(fflush(evidence) == 0 && fsync(fileno(evidence)) == 0 && fclose(evidence) == 0);
    puts("IO PASS all");
    return 0;
}
