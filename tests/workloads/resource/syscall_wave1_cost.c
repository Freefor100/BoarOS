#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/aio_abi.h>
#include <linux/membarrier.h>
#include <linux/futex.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

enum { SAMPLES = 180, CALLS = 256 };
static long timings[SAMPLES];
static int zero, null_device, sync_file, futex_word;
static char byte = 'x';
static char block[4096];

static long monotonic_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &now) != 0)
        abort();
    return now.tv_sec * 1000000000L + now.tv_nsec;
}

static long usage_us(void)
{
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage))
        abort();
    return (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000000L +
        usage.ru_utime.tv_usec + usage.ru_stime.tv_usec;
}

static int compare(const void *left, const void *right)
{
    long a = *(const long *)left, b = *(const long *)right;
    return (a > b) - (a < b);
}

static void bench(const char *name, long (*operation)(void), long expected)
{
    for (int i = 0; i < CALLS; i++)
        if (operation() != expected)
            abort();
    long usage = usage_us();
    long total = 0;
    for (int j = 0; j < SAMPLES; j++) {
        long start = monotonic_ns();
        for (int i = 0; i < CALLS; i++)
            if (operation() != expected)
                abort();
        timings[j] = monotonic_ns() - start;
        total += timings[j];
    }
    usage = usage_us() - usage;
    qsort(timings, SAMPLES, sizeof(timings[0]), compare);
    printf("COST %s n=%u median_ns=%ld p99_batch_mean_ns=%ld wall_ns=%ld cpu_us=%ld\n",
        name, SAMPLES * CALLS, timings[SAMPLES/2] / CALLS,
        timings[(SAMPLES * 99 + 99)/100-1] / CALLS, total, usage);
}

static long pid(void) { return syscall(SYS_getpid); }
static long tid(void) { return syscall(SYS_gettid); }
static long read_byte(void) { return syscall(SYS_read, zero, &byte, 1); }
static long write_byte(void) { return syscall(SYS_write, null_device, &byte, 1); }
static long clock_query(void)
{
    struct timespec now;
    return syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &now);
}
static long libc_clock_query(void)
{
    struct timespec now;
    return clock_gettime(CLOCK_MONOTONIC, &now);
}
static long wake(void) { return syscall(SYS_futex, &futex_word, FUTEX_WAKE_PRIVATE, 1, 0, 0, 0); }
static long open_close(void)
{
    long fd = syscall(SYS_openat, AT_FDCWD, "/dev/null", O_RDONLY, 0);
    return fd >= 0 ? syscall(SYS_close, fd) : fd;
}

static long range(unsigned flags)
{
    return syscall(84, sync_file, 0, sizeof(block), flags);
}
static long clean_range(void) { return range(7); }
static long memory_barrier(void) { return syscall(283, MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0, 0); }

static void dirty_range(void)
{
    enum { DIRTY_SAMPLES = 64 };
    long submit[DIRTY_SAMPLES], completed[DIRTY_SAMPLES];
    long total = 0, usage = usage_us();
    for (int i = 0; i < DIRTY_SAMPLES; i++) {
        block[0] = (char)i;
        if (pwrite(sync_file, block, sizeof(block), 0) != sizeof(block))
            abort();
        long start = monotonic_ns();
        if (range(2) != 0)
            abort();
        submit[i] = monotonic_ns() - start;
        if (range(4) != 0)
            abort();
        completed[i] = monotonic_ns() - start;
        total += completed[i];
    }
    usage = usage_us() - usage;
    qsort(submit, DIRTY_SAMPLES, sizeof(submit[0]), compare);
    qsort(completed, DIRTY_SAMPLES, sizeof(completed[0]), compare);
    printf("COST sync_file_range_dirty n=%u submit_median_ns=%ld complete_median_ns=%ld "
        "submit_p99_ns=%ld complete_p99_ns=%ld wall_ns=%ld cpu_us=%ld\n",
        DIRTY_SAMPLES, submit[DIRTY_SAMPLES/2], completed[DIRTY_SAMPLES/2],
        submit[DIRTY_SAMPLES-1], completed[DIRTY_SAMPLES-1], total, usage);
}

static void aio_cost(unsigned depth)
{
    enum { AIO_SAMPLES = 64, MAX_DEPTH = 32 };
    static _Alignas(16384) char direct[MAX_DEPTH][4096];
    struct iocb commands[MAX_DEPTH] = {0};
    struct iocb *list[MAX_DEPTH];
    struct io_event events[MAX_DEPTH];
    long submit[AIO_SAMPLES], completed[AIO_SAMPLES];
    aio_context_t ctx = 0;
    if (syscall(0, depth, &ctx))
        return;
    int fd = open("/aio-cost", O_RDWR | O_CREAT | O_TRUNC | O_DIRECT, 0600);
    if (fd < 0 || fallocate(fd, 0, 0, depth * sizeof(direct[0])))
        abort();
    for (unsigned i = 0; i < depth; i++) {
        commands[i].aio_lio_opcode = IOCB_CMD_PWRITE;
        commands[i].aio_fildes = fd;
        commands[i].aio_buf = (uintptr_t)direct[i];
        commands[i].aio_nbytes = sizeof(direct[i]);
        commands[i].aio_offset = i * sizeof(direct[i]);
        list[i] = &commands[i];
    }
    long total = 0, usage = usage_us();
    for (int i = 0; i < AIO_SAMPLES; i++) {
        direct[0][0] = (char)i;
        long start = monotonic_ns();
        if (syscall(2, ctx, depth, list) != (long)depth)
            abort();
        submit[i] = monotonic_ns() - start;
        if (syscall(4, ctx, depth, depth, events, NULL) != (long)depth)
            abort();
        completed[i] = monotonic_ns() - start;
        total += completed[i];
        for (unsigned j = 0; j < depth; j++)
            if (events[j].res != sizeof(direct[j]))
                abort();
    }
    usage = usage_us() - usage;
    qsort(submit, AIO_SAMPLES, sizeof(submit[0]), compare);
    qsort(completed, AIO_SAMPLES, sizeof(completed[0]), compare);
    printf("COST aio-direct-write depth=%u n=%u submit_median_ns=%ld complete_median_ns=%ld "
        "submit_p99_ns=%ld complete_p99_ns=%ld wall_ns=%ld cpu_us=%ld\n",
        depth, depth * AIO_SAMPLES, submit[AIO_SAMPLES/2], completed[AIO_SAMPLES/2],
        submit[AIO_SAMPLES-1], completed[AIO_SAMPLES-1], total, usage);
    if (syscall(1, ctx) || close(fd) || unlink("/aio-cost"))
        abort();
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    zero = open("/dev/zero", O_RDONLY);
    null_device = open("/dev/null", O_WRONLY);
    sync_file = open("/cost-file", O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (zero < 0 || null_device < 0 || sync_file < 0)
        abort();
    bench("getpid", pid, 1);
    bench("gettid", tid, 1);
    bench("read", read_byte, 1);
    bench("write", write_byte, 1);
    bench("openat-close", open_close, 0);
    bench("futex-wake", wake, 0);
    bench("raw-clock", clock_query, 0);
    bench("libc-clock", libc_clock_query, 0);
    if (range(0) == 0) {
        bench("sync_file_range_clean", clean_range, 0);
        dirty_range();
    }
    if (syscall(283, MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 0, 0) == 0)
        bench("membarrier-private", memory_barrier, 0);
    aio_cost(1);
    aio_cost(32);
    close(zero);
    close(null_device);
    close(sync_file);
    unlink("/cost-file");
    puts("COST END");
    sync();
    if (reboot(RB_POWER_OFF))
        perror("poweroff");
    return 0;
}
