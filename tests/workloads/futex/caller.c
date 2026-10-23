#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../../smp/composition_protocol.h"

static unsigned cases, cpus;
static uint32_t words[1024] __attribute__((aligned(16384)));
static void expect(const char *name, long value, long expected)
{
    if (value == expected) return;
    printf("FUTEX CALLER FAIL case=%s value=%ld expected=%ld\n", name, value, expected);
    fflush(stdout); exit(1);
}
static void passed(const char *name)
{ cases++; printf("FUTEX CASE %s PASS\n", name); }
static long futex(uint32_t *a, unsigned op, unsigned value, uintptr_t fourth,
                   uint32_t *b, unsigned argument)
{
    long result = syscall(SYS_futex, a, op, value, fourth, b, argument);
    return result == -1 ? -errno : result;
}
static unsigned encode(unsigned op, unsigned cmp, int arg, int comparison)
{ return op << 28 | cmp << 24 | ((unsigned)arg & 4095) << 12 | ((unsigned)comparison & 4095); }
static void pin(unsigned cpu)
{
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu, &set);
    expect("affinity", sched_setaffinity(0, sizeof(set), &set), 0);
}
struct waiter { uint32_t *word; unsigned expected, mask, cpu; long result; };
static void *waiter(void *opaque)
{
    struct waiter *w = opaque;
    pin(w->cpu);
    w->result = futex(w->word, 9 | 128, w->expected, 0, 0, w->mask);
    return 0;
}
/* A successful same-key requeue observes actual kernel registration. */
static void queued(uint32_t *word, unsigned count)
{
    for (unsigned attempt = 0; attempt < 100000; attempt++) {
        long result = futex(word, 3 | 128, 0, INT_MAX, word, 0);
        if (result == count) return;
        expect("queued", result >= 0 && result < count, 1); sched_yield();
    }
    expect("queued-timeout", 0, 1);
}
static void start(pthread_t *thread, struct waiter *w)
{
    expect("pthread-create", pthread_create(thread, 0, waiter, w), 0);
    queued(w->word, 1);
}
static void join(pthread_t thread, struct waiter *w)
{
    expect("pthread-join", pthread_join(thread, 0), 0);
    expect("wait-result", w->result, 0);
}
static void abi(void)
{
    expect("cmp-mismatch", futex(words, 4 | 128, 0, 0, words + 1, 1), -EAGAIN);
    expect("cmp-match", futex(words, 4 | 128, 0, 0, words + 1, 0), 0);
    expect("requeue-same", futex(words, 3 | 128, 0, 0, words, 0), 0);
    expect("cmp-same", futex(words, 4 | 128, 0, 0, words, 0), 0);
    expect("requeue-narrow", futex(words, 3 | 128, 0, UINT64_C(0x100000000), words + 1, 0), 0);
    expect("cmp-narrow", futex(words, 4 | 128, 0, UINT64_C(0x100000000), words + 1, 0), 0);
    expect("cmp-negative", futex(words, 4 | 128, 0, UINT32_MAX, words + 1, 0), -EINVAL);
    expect("cmp-count-before-key", futex((void *)9, 4 | 128, UINT32_MAX, 0, words, 0), -EINVAL);
    expect("private-unmapped-wake", futex((void *)8, 1 | 128, 1, 0, 0, 0), 0);
    expect("shared-unmapped-wake", futex((void *)8, 1, 1, 0, 0, 0), -EFAULT);
    expect("invalid-op-private", futex(words, 5 | 128, 0, 0, (void *)8, encode(7, 0, 0, 0)), -ENOSYS);
    expect("invalid-op-shared", futex(words, 5, 0, 0, (void *)8, encode(7, 0, 0, 0)), -EFAULT);
    passed("abi");
}
static void atomics(void)
{
    static const unsigned ops[] = {0, 1, 2, 3, 4, 8, 9};
    static const uint32_t expected[] = {0xfffffffd, 0xfffffffa, 0xfffffffb, 2, 0xfffffffd, 0x80000000, 0};
    static const int args[] = {-3, -3, 1, -3, -1, 31, -1};
    words[1] = 7;
    for (unsigned i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        expect("atomic-op", futex(words, 5 | 128, 0, 0, words + 1, encode(ops[i], 2, args[i], 0)), 0);
        expect("atomic-value", words[1], expected[i]);
    }
    words[1] = 17;
    expect("invalid-op", futex(words, 5 | 128, 0, 0, words + 1, encode(7, 0, 0, 0)), -ENOSYS);
    expect("invalid-op-value", words[1], 17);
    expect("invalid-cmp", futex(words, 5 | 128, 0, 0, words + 1, encode(0, 7, 3, 0)), -ENOSYS);
    expect("invalid-cmp-side-effect", words[1], 3);
    void *readonly = mmap(0, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    expect("readonly-map", readonly == MAP_FAILED, 0);
    expect("wake-op-readonly", futex(words, 5 | 128, 0, 0, readonly, encode(0, 0, 1, 0)), -EFAULT);
    expect("readonly-unmap", munmap(readonly, 4096), 0);
    passed("atomics");
}
static void comparisons(void)
{
    for (unsigned cmp = 0; cmp < 6; cmp++) for (int old = -1; old <= 1; old++) {
        words[1] = (uint32_t)old;
        struct waiter w = {words + 1, (uint32_t)old, 1, 0, -99}; pthread_t thread;
        start(&thread, &w);
        int expected = cmp == 0 ? old == 0 : cmp == 1 ? old != 0 : cmp == 2 ? old < 0 :
                       cmp == 3 ? old <= 0 : cmp == 4 ? old > 0 : old >= 0;
        expect("comparison", futex(words, 5 | 128, 1, 1, words + 1, encode(1, cmp, 0, 0)), expected);
        expect("comparison-value", words[1], (uint32_t)old);
        if (!expected) expect("comparison-manual-wake", futex(words + 1, 1 | 128, 1, 0, 0, 0), 1);
        join(thread, &w);
        char name[40]; snprintf(name, sizeof(name), "compare-%u-old-%d", cmp, old); passed(name);
    }
}
static void requeues(unsigned offset)
{
    words[0] = words[offset] = 0;
    struct waiter a = {words, 0, 1, 0, -99}, b = {words, 0, 2, cpus > 1 ? 1U : 0U, -99};
    pthread_t first, second; start(&first, &a);
    expect("pthread-create-second", pthread_create(&second, 0, waiter, &b), 0); queued(words, 2);
    expect("same-key-two", futex(words, 3 | 128, 0, INT_MAX, words, 0), 2);
    expect("cmp-queued-mismatch", futex(words, 4 | 128, 1, 1, words + offset, 1), -EAGAIN);
    expect("cmp-wake-and-move", futex(words, 4 | 128, 1, 1, words + offset, 0), 2);
    expect("source-empty", futex(words, 1 | 128, 2, 0, 0, 0), 0);
    join(first, &a);
    expect("mask-retained", futex(words + offset, 10 | 128, 1, 0, 0, 1), 0);
    expect("target-wake", futex(words + offset, 10 | 128, 1, 0, 0, 2), 1);
    join(second, &b);
    passed(offset == 1 ? "requeue-near" : "requeue-offset");
}
static void wake_counts(int count)
{
    words[0] = 0;
    struct waiter w = {words, 0, 1, 0, -99}; pthread_t thread; start(&thread, &w);
    expect("legacy-wake-count", futex(words, 1 | 128, (unsigned)count, 0, 0, 0), 1);
    join(thread, &w); passed(count ? "wake-negative" : "wake-zero");
}
static void wake_both(void)
{
    words[0] = words[1] = 0;
    struct waiter a = {words, 0, 1, 0, -99}, b = {words + 1, 0, 1, cpus > 1 ? 1U : 0U, -99};
    pthread_t first, second; start(&first, &a); start(&second, &b);
    expect("wake-op-two", futex(words, 5 | 128, 1, 1, words + 1, encode(1, 0, 1, 0)), 2);
    expect("wake-op-two-value", words[1], 1); join(first, &a); join(second, &b); passed("wake-both");
}
static void shared(void)
{
    uint32_t *mapping = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    expect("shared-map", mapping == MAP_FAILED, 0);
    int ready[2]; expect("shared-pipe", pipe(ready), 0);
    pid_t child = fork(); expect("shared-fork", child < 0, 0);
    if (!child) {
        struct timespec timeout = {5, 0};
        close(ready[0]);
        if (write(ready[1], "r", 1) != 1) _exit(2);
        _exit(futex(mapping, 0, 0, (uintptr_t)&timeout, 0, 0) == 0 ? 0 : 3);
    }
    expect("shared-close-write", close(ready[1]), 0);
    char byte; expect("shared-ready", read(ready[0], &byte, 1), 1);
    long moved = 0;
    for (unsigned attempt = 0; attempt < 100000 && !moved; attempt++) {
        moved = futex(mapping, 4, 0, 1, mapping + 1, 0);
        if (!moved) sched_yield();
    }
    expect("shared-cmp-move", moved, 1);
    expect("shared-source-empty", futex(mapping, 1, 1, 0, 0, 0), 0);
    expect("shared-target-wake", futex(mapping + 1, 5, 0, 1, mapping + 1, encode(1, 0, 1, 0)), 1);
    int status; expect("shared-wait", waitpid(child, &status, 0), child); expect("shared-exit", status, 0);
    expect("shared-close-read", close(ready[0]), 0); expect("shared-unmap", munmap(mapping, 4096), 0); passed("shared");
}
static pthread_mutex_t robust_mutex;
static void *abandon(void *unused)
{ (void)unused; expect("robust-lock", pthread_mutex_lock(&robust_mutex), 0); return 0; }
static void robust(void)
{
    pthread_mutexattr_t attr; pthread_t thread;
    expect("robust-attr", pthread_mutexattr_init(&attr), 0);
    expect("robust-set", pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST), 0);
    expect("robust-init", pthread_mutex_init(&robust_mutex, &attr), 0);
    expect("robust-attr-destroy", pthread_mutexattr_destroy(&attr), 0);
    expect("robust-create", pthread_create(&thread, 0, abandon, 0), 0);
    expect("robust-join", pthread_join(thread, 0), 0);
    expect("robust-owner-died", pthread_mutex_lock(&robust_mutex), EOWNERDEAD);
    expect("robust-consistent", pthread_mutex_consistent(&robust_mutex), 0);
    expect("robust-unlock", pthread_mutex_unlock(&robust_mutex), 0);
    expect("robust-destroy", pthread_mutex_destroy(&robust_mutex), 0); passed("robust");
}
static struct composition_control *observer;
static int observer_fd;
static void observe_start(void)
{
    if (access("/futex-observer", F_OK)) return;
    expect("observer-mkdir", mkdir("/dev/shm", 0700) == 0 || errno == EEXIST, 1);
    expect("observer-mount", mount("tmpfs", "/dev/shm", "tmpfs", 0, 0) == 0 || errno == EBUSY, 1);
    observer_fd = open("/dev/shm/smp-control", O_CREAT | O_EXCL | O_RDWR, 0600);
    expect("observer-open", observer_fd >= 0, 1); expect("observer-truncate", ftruncate(observer_fd, 4096), 0);
    observer = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, observer_fd, 0);
    expect("observer-map", observer != MAP_FAILED, 1);
    composition_store(&observer->magic, COMPOSITION_MAGIC);
    while (!composition_load(&observer->observer_ready)) sched_yield();
}
static void observe_end(void)
{
    if (!observer) return;
    composition_store(&observer->command, COMPOSITION_STOP); composition_store(&observer->request, 1);
    while (composition_load(&observer->reply) != 1) sched_yield();
    expect("observer-unmap", munmap(observer, 4096), 0); expect("observer-close", close(observer_fd), 0);
    expect("observer-unlink", unlink("/dev/shm/smp-control"), 0);
}
static void *caller_cpu(void *argument)
{
    unsigned cpu = (unsigned)(uintptr_t)argument; pin(cpu);
    uint32_t pair[2] = {0};
    expect("cpu-cmp", futex(pair, 4 | 128, 0, 0, pair + 1, 1), -EAGAIN);
    expect("cpu-write", futex(pair, 5 | 128, 0, 0, pair + 1, encode(1, 0, 1, 0)), 0);
    expect("cpu-value", pair[1], 1);
    expect("cpu-wake", futex(pair, 1 | 128, 1, 0, 0, 0), 0);
    printf("FUTEX CPU %u PASS\n", cpu); return 0;
}
static void caller_cpus(void)
{
    for (unsigned cpu = 0; cpu < cpus; cpu++) {
        pthread_t thread; expect("cpu-create", pthread_create(&thread, 0, caller_cpu, (void *)(uintptr_t)cpu), 0);
        expect("cpu-join", pthread_join(thread, 0), 0);
    }
    passed("caller-cpus");
}
int main(void)
{
    setvbuf(stdout, 0, _IOLBF, 0);
    unsigned long available = 0;
    expect("getaffinity", syscall(SYS_sched_getaffinity, 0, sizeof(available), &available), sizeof(available));
    while (cpus < 4 && (available & (1UL << cpus))) cpus++;
    FILE *config = fopen("/account-futex", "r");
    if (config) { unsigned requested; if (fscanf(config, "%u", &requested) == 1 && requested && requested <= cpus) cpus = requested; fclose(config); }
    expect("cpu-count", cpus > 0, 1); pin(cpus - 1);
    printf("FUTEX CALLER CPUS %u\n", cpus);
    observe_start(); abi(); atomics(); comparisons(); requeues(1); requeues(256);
    wake_counts(0); wake_counts(-1); wake_both(); shared(); robust(); caller_cpus(); observe_end();
    expect("case-count", cases, 28);
    printf("FUTEX CALLER PASS cases=%u\n", cases); return 0;
}
