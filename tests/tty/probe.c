#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
struct tty_termios {
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t line, cc[19];
};
_Static_assert(sizeof(struct tty_termios) == 36, "kernel termios ABI");
static struct tty_termios original;
static unsigned char buffer[512];
static void fail(const char *what, long actual, long want)
{
    dprintf(1, "TTY_RECORD FAIL %s actual=%ld expected=%ld errno=%d\n", what, actual, want,
            errno);
    _exit(1);
}
static void check(long actual, long want, const char *what)
{
    if (actual != want)
        fail(what, actual, want);
}
static uint64_t monotonic(void)
{
    struct timespec t;
    check(clock_gettime(CLOCK_MONOTONIC, &t), 0, "clock");
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static void request(const char *name, const unsigned char *data, size_t n)
{
    char out[2048];
    size_t p = (size_t)snprintf(out, sizeof(out), "TTY_REQUEST %s ", name);
    for (size_t i = 0; i < n; i++)
        p += (size_t)snprintf(out + p, sizeof(out) - p, "%02x", data[i]);
    out[p++] = '\n';
    check(write(1, out, p), (long)p, "request write");
}
static void set_mode(int canonical, unsigned min, unsigned time)
{
    struct tty_termios t = original;
    t.iflag = canonical ? 0x100 : 0;
    t.oflag = 5;
    t.lflag = canonical ? 0x8003 : 0;
    t.cc[6] = (uint8_t)min;
    t.cc[5] = (uint8_t)time;
    check(ioctl(0, 0x5402, &t), 0, "TCSETS");
    check(ioctl(0, 0x540b, 0), 0, "TCFLSH");
}
static int available(void)
{
    int n = -1;
    check(ioctl(0, 0x541b, &n), 0, "FIONREAD");
    return n;
}
static void wait_count(int n)
{
    uint64_t limit = monotonic() + 3000000000ULL;
    while (available() != n) {
        if (monotonic() > limit)
            fail("input handshake", available(), n);
    }
}
static void record(const char *name, long n)
{
    dprintf(1, "TTY_RECORD %s %ld\n", name, n);
}
static void canonical(void)
{
    for (unsigned n = 63; n <= 65; n++) {
        set_mode(1, 1, 0);
        unsigned char input[101];
        memset(input, 'A', n);
        input[n] = '\n';
        char name[32];
        snprintf(name, sizeof(name), "line%u", n);
        request(name, input, n + 1);
        check(read(0, buffer, 128), n + 1, name);
        for (unsigned i = 0; i < n; i++)
            check(buffer[i], 'A', "line contents");
        check(buffer[n], '\n', "line delimiter");
        record(name, n + 1);
    }
    for (unsigned n = 63; n <= 65; n++) {
        set_mode(1, 1, 0);
        unsigned char input[66];
        memset(input, 'E', n);
        input[n] = 4;
        char name[32];
        snprintf(name, sizeof(name), "eof%u", n);
        request(name, input, n + 1);
        check(read(0, buffer, n), n, name);
        record(name, n);
        request("after_eof", (const unsigned char *)"X\n", 2);
        check(read(0, buffer, 128), 2, "EOF skipped at read capacity");
        check(buffer[0], 'X', "after eof contents");
    }
    set_mode(1, 1, 0);
    const unsigned char eof = 4;
    request("empty_eof", &eof, 1);
    check(read(0, buffer, 128), 0, "zero EOF");
    record("empty_eof", 0);
}
static void fault_prefix(int raw)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char *area =
        mmap(0, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (area == MAP_FAILED)
        fail("mmap", -1, 0);
    check(mprotect(area + page, page, PROT_NONE), 0, "guard page");
    const unsigned prefixes[] = {1, 8, 63};
    for (unsigned j = 0; j < 3; j++) {
        unsigned prefix = prefixes[j];
        set_mode(!raw, raw ? 65 : 1, 0);
        unsigned char input[101];
        memset(input, 'F', 100);
        input[100] = '\n';
        char name[32];
        snprintf(name, sizeof(name), "%s_fault%u", raw ? "raw" : "canon", prefix);
        request(name, input, raw ? 100 : 101);
        if (raw)
            wait_count(100);
        check(read(0, area + page - prefix, 128), prefix, name);
        if (raw) {
            struct tty_termios t;
            check(ioctl(0, 0x5401, &t), 0, "TCGETS after fault");
            t.cc[6] = 0;
            check(ioctl(0, 0x5402, &t), 0, "MIN0 noflush");
        }
        check(read(0, buffer, 128), raw ? 36 : 37, "consumed staging tail after fault");
        record(name, prefix);
    }
    set_mode(1, 1, 0);
    unsigned char input[101];
    memset(input, 'V', 100);
    input[100] = '\n';
    request("vector_fault", input, 101);
    struct iovec iov[2] = {{buffer, 8}, {area + page, 120}};
    check(readv(0, iov, 2), 8, "vector fault prefix");
    check(read(0, buffer, 128), 37, "vector staging tail");
    record("vector_fault", 8);
    check(munmap(area, 2 * page), 0, "munmap");
}
static void raw_reads(void)
{
    set_mode(0, 0, 0);
    check(read(0, buffer, 128), 0, "MIN0 TIME0");
    record("min0_time0", 0);
    set_mode(0, 0, 1);
    uint64_t start = monotonic();
    check(read(0, buffer, 128), 0, "MIN0 TIME1");
    uint64_t elapsed = monotonic() - start;
    if (elapsed < 50000000ULL || elapsed > 2000000000ULL)
        fail("VTIME elapsed", (long)elapsed, 100000000);
    dprintf(1, "TTY_TIMING min0_time1_ns %llu\n", (unsigned long long)elapsed);
    record("min0_time1", 0);
    set_mode(0, 1, 0);
    request("min1_time0", (const unsigned char *)"ABC", 3);
    wait_count(3);
    check(read(0, buffer, 128), 3, "MIN1 TIME0");
    record("min1_time0", 3);
    set_mode(0, 5, 1);
    request("min5_time1", (const unsigned char *)"AB", 2);
    check(read(0, buffer, 128), 2, "interbyte timeout");
    record("min5_time1", 2);
    for (unsigned minimum = 65; minimum <= 100; minimum += 35) {
        set_mode(0, minimum, 0);
        unsigned char input[100];
        memset(input, 'M', 100);
        char name[32];
        snprintf(name, sizeof(name), "min%u_count100", minimum);
        request(name, input, 100);
        wait_count(100);
        check(read(0, buffer, 128), 64, "MIN exceeds 64B kernel staging");
        struct tty_termios t;
        check(ioctl(0, 0x5401, &t), 0, "raw get");
        t.cc[6] = 0;
        check(ioctl(0, 0x5402, &t), 0, "raw MIN0");
        check(read(0, buffer, 128), 36, "raw remaining36");
        record(name, 64);
    }
    set_mode(0, 65, 0);
    unsigned char input[63];
    memset(input, 'P', 63);
    request("min65_first63", input, 63);
    wait_count(63);
    pid_t child = fork();
    if (child < 0)
        fail("fork", child, 0);
    if (!child) {
        wait_count(0);
        request("min65_continue2", (const unsigned char *)"QR", 2);
        _exit(0);
    }
    check(read(0, buffer, 128), 64, "MIN65 split ends on64 scratch");
    int status;
    check(waitpid(child, &status, 0), child, "wait split writer");
    check(status, 0, "split writer exit");
    wait_count(1);
    struct tty_termios t;
    check(ioctl(0, 0x5401, &t), 0, "raw remaining get");
    t.cc[6] = 0;
    check(ioctl(0, 0x5402, &t), 0, "raw MIN0 split");
    check(read(0, buffer, 128), 1, "split remaining1");
    check(buffer[0], 'R', "split remaining contents");
    record("min65_split", 64);
    set_mode(0, 1, 0);
    int flags = fcntl(0, F_GETFL);
    if (flags < 0)
        fail("get flags", flags, 0);
    check(fcntl(0, F_SETFL, flags | O_NONBLOCK), 0, "nonblock set");
    errno = 0;
    check(read(0, buffer, 128), -1, "nonblock empty");
    check(errno, EAGAIN, "nonblock errno");
    check(fcntl(0, F_SETFL, flags), 0, "nonblock restore");
    record("nonblock_eagain", EAGAIN);
}
static volatile sig_atomic_t caught;
static void catch_signal(int signal)
{
    (void)signal;
    caught++;
}
static void partial_signal(void)
{
    set_mode(0, 5, 0);
    caught = 0;
    struct sigaction action = {0}, old;
    action.sa_handler = catch_signal;
    sigemptyset(&action.sa_mask);
    check(sigaction(SIGUSR1, &action, &old), 0, "partial handler");
    request("partial_signal", (const unsigned char *)"AB", 2);
    wait_count(2);
    pid_t child = fork();
    if (child < 0)
        fail("signal fork", child, 0);
    if (!child) {
        wait_count(0);
        check(kill(getppid(), SIGUSR1), 0, "partial signal send");
        _exit(0);
    }
    check(read(0, buffer, 128), 2, "signal returns staged partial prefix");
    check(caught, 1, "partial signal handler");
    int status;
    check(waitpid(child, &status, 0), child, "partial sender reap");
    check(status, 0, "partial sender exit");
    check(sigaction(SIGUSR1, &old, NULL), 0, "partial handler restore");
    record("partial_signal", 2);
}

static int reader_notice;
static void *cancel_sibling(void *unused)
{
    (void)unused;
    for (;;) pause();
    return NULL;
}
static void *blocked_reader(void *unused)
{
    (void)unused;
    int tid = (int)syscall(SYS_gettid);
    check(write(reader_notice, &tid, sizeof(tid)), sizeof(tid), "reader TID handshake");
    unsigned char data[9];
    struct iovec vectors[9];
    for (unsigned i = 0; i < 9; i++)
        vectors[i] = (struct iovec){data + i, 1};
    /* More than eight descriptors exercises heap-owned imported vectors. */
    (void)readv(0, vectors, 9);
    fail("cancelled reader returned", 0, -1);
    return NULL;
}
static void cancel_blocked_group(void)
{
    set_mode(0, 1, 0);
    int notice[2];
    check(pipe(notice), 0, "cancel pipe");
    pid_t child = fork();
    if (child < 0)
        fail("cancel fork", child, 0);
    if (!child) {
        close(notice[0]);
        reader_notice = notice[1];
        pthread_t thread;
        check(pthread_create(&thread, NULL, cancel_sibling, NULL), 0, "cancel pthread create");
        /* Both kernels expose the leader stat; no nonleader procfs dependency. */
        blocked_reader(NULL);
    }
    close(notice[1]);
    int tid = 0;
    check(read(notice[0], &tid, sizeof(tid)), sizeof(tid), "reader notice");
    close(notice[0]);
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", tid);
    uint64_t limit = monotonic() + 3000000000ULL;
    for (;;) {
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            char stat[1024];
            ssize_t n = read(fd, stat, sizeof(stat) - 1);
            close(fd);
            if (n > 0) {
                stat[n] = 0;
                char *end = strrchr(stat, ')');
                if (end && end[1] == ' ' && end[2] == 'S')
                    break;
            }
        }
        if (monotonic() > limit)
            fail("blocked read ownership handshake", 0, 1);
    }
    check(kill(child, SIGKILL), 0, "kill blocked group");
    int status;
    check(waitpid(child, &status, 0), child, "wait blocked group");
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL)
        fail("blocked group signal status", status, SIGKILL);
    request("cancel_next_read", (const unsigned char *)"Z", 1);
    check(read(0, buffer, 1), 1, "read lock released after forced group exit");
    check(buffer[0], 'Z', "cancel next contents");
    record("forced_group_cancel", SIGKILL);
}

int main(void)
{
    if (ioctl(0, 0x5401, &original))
        fail("TCGETS", -1, 0);
    record("tcgets", 0);
    int sid = -1, pgrp = -1;
    check(ioctl(0, 0x5429, &sid), 0, "TIOCGSID");
    check(sid, getsid(0), "controlling SID");
    check(ioctl(0, 0x540f, &pgrp), 0, "TIOCGPGRP");
    check(pgrp, getpgrp(), "foreground group");
    int current = open("/dev/tty", O_RDWR);
    if (current < 0)
        fail("controlling tty reopen", -1, 0);
    check(close(current), 0, "ctty close");
    uint16_t winsize[4] = {24, 80, 0, 0};
    check(ioctl(0, 0x5414, winsize), 0, "TIOCSWINSZ");
    memset(winsize, 0, sizeof(winsize));
    check(ioctl(0, 0x5413, winsize), 0, "TIOCGWINSZ");
    check(winsize[0], 24, "rows");
    check(winsize[1], 80, "columns");
    record("winsize", 80);
    canonical();
    fault_prefix(0);
    fault_prefix(1);
    raw_reads();
    partial_signal();
    cancel_blocked_group();
    check(ioctl(0, 0x5402, &original), 0, "termios restore");
    dprintf(1, "TTY_PROBE_PASS\n");
    return 0;
}
