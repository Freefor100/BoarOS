#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "PROC SYSTEM STATS FAIL line=%d %s\n", __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

enum { CPU_FIELDS = 10, DISK_FIELDS = 17 };
enum { USER, NICE, SYSTEM, IDLE, IOWAIT, IRQ };

struct system_stats {
    unsigned long long cpu[CPU_FIELDS];
    unsigned long long interrupts, switches, boot, tasks, runnable, blocked;
    unsigned cpu_count;
};

static struct system_stats read_system_stats(void)
{
    FILE *file = fopen("/proc/stat", "r");
    CHECK(file);
    struct system_stats result = {0};
    char *line = NULL;
    size_t capacity = 0;
    unsigned seen = 0;
    int last_cpu = -1;
    while (getline(&line, &capacity, file) >= 0) {
        if (!strncmp(line, "cpu", 3)) {
            char *cursor = line + 3;
            int aggregate = *cursor == ' ';
            if (aggregate) {
                CHECK(!result.cpu_count && !(seen & 1));
                seen |= 1;
            } else {
                CHECK(*cursor >= '0' && *cursor <= '9');
                long cpu = strtol(cursor, &cursor, 10);
                CHECK(cpu > last_cpu);
                last_cpu = (int)cpu;
                result.cpu_count++;
            }
            for (unsigned i = 0; i < CPU_FIELDS; i++) {
                char *end;
                unsigned long long value = strtoull(cursor, &end, 10);
                CHECK(end != cursor);
                if (aggregate) result.cpu[i] = value;
                cursor = end;
            }
            CHECK(!strcmp(cursor, "\n"));
        } else if (sscanf(line, "intr %llu", &result.interrupts) == 1) {
            seen |= 2;
        } else if (sscanf(line, "ctxt %llu", &result.switches) == 1) {
            seen |= 4;
        } else if (sscanf(line, "btime %llu", &result.boot) == 1) {
            seen |= 8;
        } else if (sscanf(line, "processes %llu", &result.tasks) == 1) {
            seen |= 16;
        } else if (sscanf(line, "procs_running %llu", &result.runnable) == 1) {
            seen |= 32;
        } else if (sscanf(line, "procs_blocked %llu", &result.blocked) == 1) {
            seen |= 64;
        } else if (!strncmp(line, "softirq ", 8)) {
            seen |= 128;
        }
    }
    free(line);
    CHECK(!ferror(file) && !fclose(file));
    CHECK(seen == 255 && result.cpu_count && result.runnable);
    return result;
}

static uint64_t clock_ns(clockid_t clock)
{
    struct timespec sample;
    CHECK(!clock_gettime(clock, &sample));
    return (uint64_t)sample.tv_sec * 1000000000ULL + sample.tv_nsec;
}

static void burn_cpu(void)
{
    uint64_t stop = clock_ns(CLOCK_THREAD_CPUTIME_ID) + 180000000;
    volatile uint64_t value = 1;
    do {
        for (unsigned i = 0; i < 20000; i++)
            value = value * 13 + 1;
    } while (clock_ns(CLOCK_THREAD_CPUTIME_ID) < stop);
}

static void *worker(void *unused)
{
    (void)unused;
    return NULL;
}

static void check_system_stats(void)
{
    struct system_stats before = read_system_stats();
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    CHECK(online > 0 && before.cpu_count == (unsigned long)online);
    uint64_t boot = (clock_ns(CLOCK_REALTIME) - clock_ns(CLOCK_BOOTTIME)) / 1000000000ULL;
    CHECK(before.boot + 2 >= boot && before.boot <= boot + 2);

    burn_cpu();
    struct system_stats active = read_system_stats();
    CHECK(active.cpu[USER] >= before.cpu[USER] + 10);
    CHECK(active.interrupts > before.interrupts);

    CHECK(!setpriority(PRIO_PROCESS, 0, 10));
    burn_cpu();
    struct system_stats niced = read_system_stats();
    CHECK(niced.cpu[NICE] >= active.cpu[NICE] + 10);

    pthread_t thread;
    CHECK(!pthread_create(&thread, NULL, worker, NULL));
    CHECK(!pthread_join(thread, NULL));
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) _exit(0);
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    CHECK(!nanosleep(&(struct timespec){.tv_nsec = 150000000}, NULL));
    struct system_stats after = read_system_stats();
    CHECK(after.cpu[IDLE] >= niced.cpu[IDLE] + 5);
    CHECK(after.tasks >= before.tasks + 2 && after.switches > before.switches);
    for (unsigned i = 0; i < CPU_FIELDS; i++) {
        if (i != IOWAIT) CHECK(after.cpu[i] >= before.cpu[i]);
    }
}

static void read_disk_stats(dev_t device, unsigned long long values[DISK_FIELDS])
{
    FILE *file = fopen("/proc/diskstats", "r");
    CHECK(file);
    char *line = NULL;
    size_t capacity = 0;
    int found = 0;
    while (getline(&line, &capacity, file) >= 0) {
        unsigned disk_major, disk_minor;
        char name[64];
        int offset;
        CHECK(sscanf(line, "%u %u %63s%n", &disk_major, &disk_minor, name, &offset) == 3);
        char *cursor = line + offset;
        unsigned long long fields[DISK_FIELDS];
        for (unsigned i = 0; i < DISK_FIELDS; i++) {
            char *end;
            fields[i] = strtoull(cursor, &end, 10);
            CHECK(end != cursor);
            cursor = end;
        }
        CHECK(!strcmp(cursor, "\n"));
        if (disk_major == major(device) && disk_minor == minor(device)) {
            CHECK(!found++);
            memcpy(values, fields, sizeof(fields));
        }
    }
    free(line);
    CHECK(!ferror(file) && !fclose(file) && found == 1);
}

static void check_disk_stats(void)
{
    enum { BYTES = 65536 };
    struct stat root;
    CHECK(!stat("/", &root));
    unsigned long long before[DISK_FIELDS], after[DISK_FIELDS];
    read_disk_stats(root.st_dev, before);
    void *buffer;
    CHECK(!posix_memalign(&buffer, BYTES, BYTES));
    memset(buffer, 0x43, BYTES);
    int fd = open("/tmp/proc-system-stats", O_CREAT | O_EXCL | O_RDWR | O_DIRECT, 0600);
    CHECK(fd >= 0);
    CHECK(write(fd, buffer, BYTES) == BYTES && !fsync(fd));
    CHECK(pread(fd, buffer, BYTES, 0) == BYTES);
    CHECK(!close(fd) && !unlink("/tmp/proc-system-stats"));
    free(buffer);
    read_disk_stats(root.st_dev, after);
    CHECK(after[0] > before[0] && after[2] >= before[2] + BYTES / 512);
    CHECK(after[4] > before[4] && after[6] >= before[6] + BYTES / 512);
    for (unsigned i = 0; i < DISK_FIELDS; i++) {
        if (i != 8) CHECK(after[i] >= before[i]);
    }
}

int main(void)
{
    DIR *directory = opendir("/proc");
    CHECK(directory);
    int stat_entry = 0, disks_entry = 0;
    for (struct dirent *entry; (entry = readdir(directory)); ) {
        stat_entry |= !strcmp(entry->d_name, "stat");
        disks_entry |= !strcmp(entry->d_name, "diskstats");
    }
    CHECK(!closedir(directory));
    CHECK(stat_entry && disks_entry);
    check_system_stats();
    check_disk_stats();
    puts("PROC SYSTEM STATS PASS");
    return 0;
}
