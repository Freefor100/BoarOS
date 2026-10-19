#include "abi.h"

struct fsize_limit { unsigned long current, maximum; };
struct fsize_timespec { long sec, ns; };
static const unsigned long fsize_signal_mask = 1UL << 24;
static unsigned char fsize_stack[16384] __attribute__((aligned(16)));
extern long abi_clone_entry(long, void *, int *, void (*)(void *), void *);

static long fsize_get(struct fsize_limit *value)
{ return SC4(261, 0, 1, 0, value); }
static void fsize_set(unsigned long value)
{
    const struct fsize_limit limit = {value, ~0UL};
    abi_require(!SC4(261, 0, 1, &limit, 0));
}
static long fsize_take(void)
{
    const struct fsize_timespec zero = {0};
    int info[32] = {0};
    long result = SC4(137, &fsize_signal_mask, info, &zero, 8);
    if (result == -11) return 0;
    abi_require(result == 25 && info[0] == 25 && info[2] == 0 &&
                info[4] == SC0(172) && info[5] == 0);
    return 25;
}
static void fsize_name(char *name, const char *prefix, const char *suffix)
{
    while (*prefix) *name++ = *prefix++;
    *name++ = '.';
    while (*suffix) *name++ = *suffix++;
    *name = 0;
}
static void fsize_record(const char *prefix, const char *suffix, long result,
                         long fd, usize sample_length)
{
    char name[96]; unsigned char data[32];
    abi_require(sample_length <= sizeof(data));
    for (usize i = 0; i < sample_length; ++i) data[i] = 0xa5;
    if (sample_length)
        abi_require(SC4(67, fd, data, sample_length, 0) >= 0);
    fsize_name(name, prefix, suffix);
    abi_record(name, result, abi_size(fd), abi_offset(fd), fsize_take(),
               data, sample_length);
}
static long fsize_file(const char *path, usize seed_length)
{
    static const char seed[] = "abcdefghijklmnop";
    fsize_set(~0UL);
    long fd = abi_open(path, 2 | 0100 | 01000);
    abi_require(fd >= 0 && seed_length <= sizeof(seed) - 1);
    if (seed_length) abi_require(SC3(64, fd, seed, seed_length) == (long)seed_length);
    fsize_set(8);
    return fd;
}
static void fsize_close(long fd)
{ abi_require(!SC1(57, fd)); }

/* 每条记录独立观察内容、当前偏移和信号，避免只比较返回值。 */
static void fsize_file_operations(const char *path, const char *prefix)
{
    long fd = fsize_file(path, 0);
    long result = SC3(64, fd, "abcdef", 6);
    fsize_record(prefix, "write-before", result, fd, 6);
    result = SC3(64, fd, "GHIJ", 4);
    fsize_record(prefix, "write-cross", result, fd, 8);
    result = SC3(64, fd, "X", 1);
    fsize_record(prefix, "write-boundary", result, fd, 8);
    result = SC3(64, fd, (void *)-1, 0);
    fsize_record(prefix, "write-zero", result, fd, 8);
    fsize_close(fd);

    fd = fsize_file(path, 6);
    abi_require(SC3(62, fd, 3, 0) == 3);
    result = SC4(68, fd, "GHIJ", 4, 6);
    fsize_record(prefix, "pwrite-cross", result, fd, 8);
    result = SC4(68, fd, "X", 1, 8);
    fsize_record(prefix, "pwrite-boundary", result, fd, 8);
    fsize_close(fd);

    struct abi_iovec vectors[2] = {{"CDE", 3}, {"FGHIJ", 5}};
    fd = fsize_file(path, 4);
    result = SC3(66, fd, vectors, 2);
    fsize_record(prefix, "writev-cross", result, fd, 8);
    result = SC3(66, fd, vectors, 2);
    fsize_record(prefix, "writev-boundary", result, fd, 8);
    fsize_close(fd);

    fd = fsize_file(path, 6);
    abi_require(SC3(62, fd, 3, 0) == 3);
    result = SC5(70, fd, vectors, 2, 6, 0);
    fsize_record(prefix, "pwritev-cross", result, fd, 8);
    result = SC5(70, fd, vectors, 2, 8, 0);
    fsize_record(prefix, "pwritev-boundary", result, fd, 8);
    fsize_close(fd);

    fd = fsize_file(path, 6);
    long append = abi_open(path, 2 | 02000);
    abi_require(append >= 0 && SC3(62, append, 1, 0) == 1);
    result = SC3(64, append, "GHIJ", 4);
    fsize_record(prefix, "append-cross", result, append, 8);
    result = SC3(64, append, "X", 1);
    fsize_record(prefix, "append-boundary", result, append, 8);
    fsize_close(append); fsize_close(fd);

    fd = fsize_file(path, 16);
    abi_require(SC3(62, fd, 6, 0) == 6);
    result = SC3(64, fd, "WXYZ", 4);
    fsize_record(prefix, "overwrite-cross", result, fd, 16);
    result = SC2(46, fd, 12);
    fsize_record(prefix, "truncate-shrink-above", result, fd, 12);
    result = SC2(46, fd, 13);
    fsize_record(prefix, "truncate-grow-above", result, fd, 12);
    result = SC2(46, fd, 8);
    fsize_record(prefix, "truncate-to-limit", result, fd, 8);
    result = SC2(45, path, 9);
    fsize_record(prefix, "path-grow", result, fd, 8);
    result = SC2(45, path, 3);
    fsize_record(prefix, "path-shrink", result, fd, 3);
    fsize_close(fd);

    /* 映射和脏页先属于原文件；下调限额不追溯拒绝它们的写回。 */
    fsize_set(~0UL);
    fd = abi_open(path, 2 | 0100 | 01000);
    abi_require(fd >= 0 && !SC2(46, fd, ABI_PAGE_SIZE));
    long map = SC6(222, 0, ABI_PAGE_SIZE, 3, 1, fd, 0);
    abi_require(map >= 0);
    volatile unsigned char *memory = (void *)map;
    memory[0] = 'D'; memory[100] = 'P';
    fsize_set(8);
    memory[101] = 'Q';
    result = SC3(227, map, ABI_PAGE_SIZE, 4);
    long synced = SC1(82, fd);
    unsigned char sample[3] = {0};
    abi_require(SC4(67, fd, sample, 1, 0) == 1 &&
                SC4(67, fd, sample + 1, 2, 100) == 2);
    char name[96]; fsize_name(name, prefix, "lower-dirty-sync");
    abi_record(name, result, abi_size(fd), synced, fsize_take(), sample, sizeof(sample));
    abi_require(!SC2(215, map, ABI_PAGE_SIZE));
    fsize_close(fd);
    fsize_set(~0UL);
    abi_require(!SC3(35, -100, path, 0));
}

static void fsize_sendfile(void)
{
    fsize_set(~0UL);
    long source = abi_open("/fsize-source", 2 | 0100 | 01000);
    abi_require(source >= 0 && SC3(64, source, "0123456789ABCDEF", 16) == 16 &&
                SC3(62, source, 7, 0) == 7);
    long output = fsize_file("/fsize-output", 4), input_offset = 2;
    long result = SC4(71, output, source, &input_offset, 8);
    unsigned char sample[8];
    for (usize i = 0; i < sizeof(sample); ++i) sample[i] = 0xa5;
    abi_require(SC4(67, output, sample, 8, 0) >= 0);
    abi_record("fsize.sendfile-cross", result, abi_size(output), abi_offset(output),
               fsize_take(), sample, sizeof(sample));
    long offsets[] = {input_offset, abi_offset(source)};
    abi_record("fsize.sendfile-source-offset", 0, -1, -1, 0, offsets, sizeof(offsets));
    result = SC4(71, output, source, &input_offset, 1);
    abi_record("fsize.sendfile-boundary", result, abi_size(output), input_offset,
               fsize_take(), 0, 0);
    fsize_close(source); fsize_close(output);
    abi_require(!SC3(35, -100, "/fsize-source", 0) &&
                !SC3(35, -100, "/fsize-output", 0));
}

static void fsize_fallocate(void)
{
    long fd = fsize_file("/fsize-allocate", 4);
    long result = SC4(47, fd, 0, 6, 4);
    fsize_record("fsize", "fallocate-grow", result, fd, 4);
    result = SC4(47, fd, 0, 0, 0x7fffffffffffffffUL);
    fsize_record("fsize", "fallocate-max-before-limit", result, fd, 4);
    result = SC4(47, fd, 1, 4096, 4096);
    fsize_record("fsize", "fallocate-keep", result, fd, 4);
    result = SC4(47, fd, 3, 2, 4096);
    fsize_record("fsize", "fallocate-punch", result, fd, 4);
    fsize_close(fd);
    fd = fsize_file("/fsize-allocate", 16);
    result = SC4(47, fd, 0, 0, 12);
    fsize_record("fsize", "fallocate-existing", result, fd, 16);
    fsize_close(fd);
    abi_require(!SC3(35, -100, "/fsize-allocate", 0));
}

struct fsize_worker {
    int ready, release, done, mode, leader_alive;
    long fd, result, signal, extra_signal;
};
static void fsize_wait(int *word)
{
    while (!__atomic_load_n(word, __ATOMIC_ACQUIRE)) {
        long result = SC6(98, word, 0, 0, 0, 0, 0);
        abi_require(!result || result == -11 || result == -4);
    }
}
static void fsize_wake(int *word)
{
    __atomic_store_n(word, 1, __ATOMIC_RELEASE);
    abi_require(SC3(98, word, 1, 1) >= 0);
}
static void fsize_writer(void *argument)
{
    struct fsize_worker *work = argument;
    if (work->mode == 1) {
        const char *argv[] = {"/init", "fsize-exec", 0};
        const char *env[] = {0};
        SC3(221, argv[0], argv, env); abi_exit(92);
    }
    if (work->mode == 2) {
        fsize_wake(&work->ready);
        int alive;
        while ((alive = __atomic_load_n(&work->leader_alive, __ATOMIC_ACQUIRE))) {
            long result = SC6(98, &work->leader_alive, 0, alive, 0, 0, 0);
            abi_require(!result || result == -11 || result == -4);
        }
        struct fsize_limit value;
        SC1(94, !fsize_get(&value) && value.current == 8 && value.maximum == ~0UL ? 57 : 93);
        abi_exit(94);
    }
    fsize_set(8);
    work->result = SC3(64, work->fd, "X", 1);
    fsize_wake(&work->ready);
    fsize_wait(&work->release);
    work->signal = fsize_take();
    work->extra_signal = fsize_take();
    fsize_wake(&work->done);
}
void abi_fsize_exec_probe(void)
{
    struct fsize_limit value;
    abi_exit(!fsize_get(&value) && value.current == 8 && value.maximum == ~0UL ? 57 : 91);
}
static void fsize_lifecycle(void)
{
    long fd = fsize_file("/fsize-thread", 8);
    fsize_set(~0UL);
    struct fsize_worker work = {.fd = fd}; int tid_address = 0;
    long tid = abi_clone_entry(0x10f00, fsize_stack + sizeof(fsize_stack),
                               &tid_address, fsize_writer, &work);
    abi_require(tid > 0);
    fsize_wait(&work.ready);
    struct fsize_limit value;
    abi_require(!fsize_get(&value));
    abi_record("fsize.thread-shared", value.current == 8, -1, -1, 0, 0, 0);
    long parent_signal = fsize_take();
    fsize_wake(&work.release);
    fsize_wait(&work.done);
    while (SC2(130, tid, 0) == 0) SC0(124);
    long other_signals[] = {parent_signal, work.extra_signal};
    abi_record("fsize.thread-signal-owner", work.result, abi_size(fd), abi_offset(fd),
               work.signal, other_signals, sizeof(other_signals));
    fsize_close(fd); abi_require(!SC3(35, -100, "/fsize-thread", 0));
    long child = SC5(220, 17, 0, 0, 0, 0);
    abi_require(child >= 0);
    if (!child) {
        abi_require(!fsize_get(&value) && value.current == 8);
        fsize_set(3); abi_exit(57);
    }
    int status = 0;
    abi_require(SC4(260, child, &status, 0, 0) == child && !fsize_get(&value));
    abi_record("fsize.fork-isolation", value.current == 8, -1, -1, status, 0, 0);
    child = SC5(220, 17, 0, 0, 0, 0); abi_require(child >= 0);
    if (!child) {
        const char *argv[] = {"/init", "fsize-exec", 0};
        const char *env[] = {0};
        SC3(221, argv[0], argv, env); abi_exit(92);
    }
    abi_require(SC4(260, child, &status, 0, 0) == child);
    abi_record("fsize.exec-inherit", 0, -1, -1, status, 0, 0);
    child = SC5(220, 17, 0, 0, 0, 0); abi_require(child >= 0);
    if (!child) {
        work = (struct fsize_worker){.mode = 1};
        abi_require(abi_clone_entry(0x10f00, fsize_stack + sizeof(fsize_stack),
                                    &tid_address, fsize_writer, &work) > 0);
        for (;;) SC0(124);
    }
    abi_require(SC4(260, child, &status, 0, 0) == child);
    abi_record("fsize.nonleader-exec", 0, -1, -1, status, 0, 0);
    child = SC5(220, 17, 0, 0, 0, 0); abi_require(child >= 0);
    if (!child) {
        work = (struct fsize_worker){.mode = 2, .leader_alive = 1};
        abi_require(SC1(96, &work.leader_alive) > 0);
        abi_require(abi_clone_entry(0x10f00, fsize_stack + sizeof(fsize_stack),
                                    &tid_address, fsize_writer, &work) > 0);
        fsize_wait(&work.ready);
        SC1(93, 0); abi_exit(95);
    }
    abi_require(SC4(260, child, &status, 0, 0) == child);
    abi_record("fsize.leader-exit", 0, -1, -1, status, 0, 0);
}

void abi_fsize_cases(void)
{
    struct fsize_limit saved = {0}, value = {0};
    long result = fsize_get(&saved);
    abi_record("fsize.entry", result, -1, -1, 0, 0, 0);
    abi_require(!result);
    abi_record("fsize.default", saved.current == ~0UL && saved.maximum == ~0UL,
               -1, -1, 0, 0, 0);
    abi_record("fsize.output-fault", SC4(261, 0, 1, 0, (void *)-1), -1, -1, 0, 0, 0);
    abi_record("fsize.input-fault", SC4(261, 0, 1, (void *)-1, 0), -1, -1, 0, 0, 0);
    const struct fsize_limit invalid = {2, 1}, update = {42, ~0UL};
    abi_record("fsize.invalid-range", SC4(261, 0, 1, &invalid, 0), -1, -1, 0, 0, 0);
    result = SC4(261, 0, 1, &update, (void *)-1);
    abi_require(!fsize_get(&value));
    abi_record("fsize.commit-before-output-fault", result, value.current, -1, 0, 0, 0);
    unsigned long saved_mask;
    abi_require(!SC4(135, 0, &fsize_signal_mask, &saved_mask, 8));
    abi_require(!fsize_take());
    fsize_file_operations("/fsize-file", "fsize.ext4");
    abi_require(!SC3(34, -100, "/fsize-tmpfs", 0755) &&
                !SC5(40, "none", "/fsize-tmpfs", "tmpfs", 0,
                      "size=" ABI_TWO_PAGES_STRING ",nr_inodes=8"));
    fsize_file_operations("/fsize-tmpfs/data", "fsize.tmpfs");
    abi_require(!SC2(39, "/fsize-tmpfs", 0) &&
                !SC3(35, -100, "/fsize-tmpfs", 0x200));
    fsize_sendfile();
    fsize_fallocate();
    fsize_lifecycle();
    abi_require(!SC4(261, 0, 1, &saved, 0) && !fsize_get(&value));
    abi_record("fsize.restore", value.current == saved.current && value.maximum == saved.maximum,
               -1, -1, fsize_take(), 0, 0);
    abi_require(!SC4(135, 2, &saved_mask, 0, 8));
}
