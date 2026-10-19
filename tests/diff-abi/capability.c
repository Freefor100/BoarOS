#include "abi.h"

#define CAP_VERSION_1 0x19980330U
#define CAP_VERSION_2 0x20071026U
#define CAP_VERSION_3 0x20080522U

struct cap_header { unsigned version; int pid; };
struct cap_data { unsigned effective, permitted, inheritable; };

static void cap_fill(struct cap_data *data)
{
    for (unsigned i = 0; i < 2; ++i)
        data[i] = (struct cap_data){0x5a5a5a5aU, 0x5a5a5a5aU,
                                    0x5a5a5a5aU};
}

static int cap_equal(const struct cap_data *a, const struct cap_data *b)
{
    for (unsigned i = 0; i < 2; ++i)
        if (a[i].effective != b[i].effective ||
            a[i].permitted != b[i].permitted ||
            a[i].inheritable != b[i].inheritable) return 0;
    return 1;
}

static void cap_target(const char *name, int tid,
                        const struct cap_data *expected)
{
    struct cap_header header = {CAP_VERSION_3, tid};
    struct cap_data data[2];
    cap_fill(data);
    long result = SC2(90, &header, data);
    abi_record(name, result, !result && cap_equal(data, expected), -1, 0, 0, 0);
}

struct cap_worker { int ready, release; long tid; };
static unsigned char cap_stack[16384] __attribute__((aligned(16)));
extern long abi_clone_entry(long, void *, int *, void (*)(void *), void *);

static void cap_worker(void *opaque)
{
    struct cap_worker *worker = opaque;
    worker->tid = SC0(178);
    __atomic_store_n(&worker->ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&worker->release, __ATOMIC_ACQUIRE)) SC0(124);
}

struct cap_departed_leader { int *tid; int ready, release; };
static void cap_departed_leader(void *opaque)
{
    struct cap_departed_leader *leader = opaque;
    for (;;) {
        int tid = __atomic_load_n(leader->tid, __ATOMIC_ACQUIRE);
        if (!tid) break;
        long result = SC6(98, leader->tid, 0, tid, 0, 0, 0);
        abi_require(!result || result == -11);
    }
    char byte = 1;
    abi_require(SC3(64, leader->ready, &byte, 1) == 1);
    abi_require(SC3(63, leader->release, &byte, 1) == 1);
}

static long cap_swap_kib(const char *data, long length)
{
    static const char key[] = "VmSwap:";
    for (long i = 0; i + (long)sizeof(key) < length; ++i) {
        if (i && data[i - 1] != '\n') continue;
        unsigned j = 0;
        while (j < sizeof(key) - 1 && data[i + j] == key[j]) ++j;
        if (j != sizeof(key) - 1) continue;
        long at = i + j, value = 0;
        while (at < length && (data[at] == ' ' || data[at] == '\t')) ++at;
        if (at == length || data[at] < '0' || data[at] > '9') return -1;
        while (at < length && data[at] >= '0' && data[at] <= '9')
            value = value * 10 + data[at++] - '0';
        while (at < length && (data[at] == ' ' || data[at] == '\t')) ++at;
        return at + 3 <= length && data[at] == 'k' && data[at + 1] == 'B' &&
               data[at + 2] == '\n' ? value : -1;
    }
    return -1;
}

static long cap_pid_max(const char *data, long length)
{
    if (length < 2 || data[length - 1] != '\n') return -1;
    long value = 0;
    for (long i = 0; i < length - 1; ++i) {
        if (data[i] < '0' || data[i] > '9') return -1;
        value = value * 10 + data[i] - '0';
        if (value > 0x7fffffffL) return -1;
    }
    return value > 0 ? value : -1;
}

void abi_capability_cases(void)
{
    struct cap_header header = {CAP_VERSION_3, 0};
    struct cap_data baseline[2], data[2];
    cap_fill(baseline);
    long result = SC2(90, &header, baseline);
    abi_record("capget.v3", result, -1, -1, 0, baseline, sizeof(baseline));
    header.version = CAP_VERSION_2;
    cap_fill(data);
    result = SC2(90, &header, data);
    abi_record("capget.v2", result, -1, -1, 0, data, sizeof(data));
    header.version = CAP_VERSION_1;
    cap_fill(data);
    result = SC2(90, &header, data);
    abi_record("capget.v1", result, -1, -1, 0, data, sizeof(data));

    abi_record("capget.null-header", SC2(90, 0, data), -1, -1, 0, 0, 0);
    abi_record("capget.both-null", SC2(90, 0, 0), -1, -1, 0, 0, 0);
    header = (struct cap_header){CAP_VERSION_3, -1};
    abi_record("capget.null-data-skips-pid", SC2(90, &header, 0),
               -1, -1, 0, 0, 0);
    header = (struct cap_header){0, -1};
    result = SC2(90, &header, 0);
    abi_record("capget.negotiate-null", result, header.version, -1, 0, 0, 0);
    header = (struct cap_header){0, -1};
    cap_fill(data);
    result = SC2(90, &header, data);
    abi_record("capget.negotiate-data", result, header.version, -1, 0,
               data, sizeof(data));
    header = (struct cap_header){CAP_VERSION_3, -1};
    abi_record("capget.negative-pid-before-data", SC2(90, &header, -1),
               -1, -1, 0, 0, 0);
    header.pid = 0x7fffffff;
    abi_record("capget.missing-pid-before-data", SC2(90, &header, -1),
               -1, -1, 0, 0, 0);
    header.pid = 0;
    abi_record("capget.data-fault", SC2(90, &header, -1), -1, -1, 0, 0, 0);
    cap_target("capget.self-tid", (int)SC0(178), baseline);

    long map = SC6(222, 0, 2 * ABI_PAGE_SIZE, 3, 0x22, -1, 0);
    abi_require(map >= 0);
    struct cap_header *mapped = (void *)map;
    *mapped = (struct cap_header){CAP_VERSION_3, 0};
    abi_require(!SC3(226, map, ABI_PAGE_SIZE, 1));
    cap_fill(data);
    result = SC2(90, mapped, data);
    abi_record("capget.readonly-valid-header", result,
               !result && cap_equal(data, baseline), -1, 0, 0, 0);
    abi_require(!SC3(226, map, ABI_PAGE_SIZE, 3));
    mapped->version = 0;
    abi_require(!SC3(226, map, ABI_PAGE_SIZE, 1));
    abi_record("capget.readonly-negotiate-data", SC2(90, mapped, data),
               mapped->version, -1, 0, 0, 0);
    abi_record("capget.readonly-negotiate-null", SC2(90, mapped, 0),
               mapped->version, -1, 0, 0, 0);
    abi_require(!SC3(226, map, ABI_PAGE_SIZE, 3));
    abi_require(!SC3(226, map + ABI_PAGE_SIZE, ABI_PAGE_SIZE, 0));

    unsigned *version = (void *)(map + ABI_PAGE_SIZE - sizeof(unsigned));
    *version = CAP_VERSION_3;
    abi_record("capget.version-only-null", SC2(90, version, 0),
               -1, -1, 0, 0, 0);
    abi_record("capget.pid-fault", SC2(90, version, data), -1, -1, 0, 0, 0);
    *version = 0;
    result = SC2(90, version, 0);
    abi_record("capget.version-only-negotiate-null", result, *version,
               -1, 0, 0, 0);
    *version = 0;
    result = SC2(90, version, data);
    abi_record("capget.version-only-negotiate-data", result, *version,
               -1, 0, 0, 0);

    header = (struct cap_header){CAP_VERSION_3, 0};
    struct cap_data *prefix = (void *)(map + ABI_PAGE_SIZE - sizeof(*prefix));
    *prefix = (struct cap_data){0x5a5a5a5aU, 0x5a5a5a5aU, 0x5a5a5a5aU};
    result = SC2(90, &header, prefix);
    abi_record("capget.output-prefix", result, -1, -1, 0,
               prefix, sizeof(*prefix));
    header.version = CAP_VERSION_1;
    result = SC2(90, &header, prefix);
    abi_record("capget.v1-page-end", result, -1, -1, 0,
               prefix, sizeof(*prefix));
    abi_require(!SC2(215, map, 2 * ABI_PAGE_SIZE));

    struct cap_worker worker = {0};
    int child_tid = 0;
    long tid = abi_clone_entry(0x10f00, cap_stack + sizeof(cap_stack),
                               &child_tid, cap_worker, &worker);
    abi_require(tid > 0);
    while (!__atomic_load_n(&worker.ready, __ATOMIC_ACQUIRE)) SC0(124);
    cap_target("capget.other-thread", (int)worker.tid, baseline);
    __atomic_store_n(&worker.release, 1, __ATOMIC_RELEASE);
    while (SC2(130, tid, 0) == 0) SC0(124);
    header = (struct cap_header){CAP_VERSION_3, (int)tid};
    abi_record("capget.exited-thread", SC2(90, &header, data),
               -1, -1, 0, 0, 0);

    int pipe[2];
    abi_require(!SC2(59, pipe, 0));
    long child = SC5(220, 17, 0, 0, 0, 0);
    abi_require(child >= 0);
    if (!child) {
        char byte;
        abi_require(SC3(63, pipe[0], &byte, 1) == 1);
        abi_exit(0);
    }
    cap_target("capget.other-process", (int)child, baseline);
    char byte = 1;
    abi_require(SC3(64, pipe[1], &byte, 1) == 1);
    unsigned char info[128];
    abi_require(!SC5(95, 1, child, info, 4 | 0x1000000, 0));
    cap_target("capget.zombie", (int)child, baseline);
    abi_require(SC4(260, child, 0, 0, 0) == child);
    header = (struct cap_header){CAP_VERSION_3, (int)child};
    abi_record("capget.reaped", SC2(90, &header, data), -1, -1, 0, 0, 0);
    abi_require(!SC1(57, pipe[0]) && !SC1(57, pipe[1]));

    int ready[2];
    abi_require(!SC2(59, pipe, 0) && !SC2(59, ready, 0));
    child = SC5(220, 17, 0, 0, 0, 0);
    abi_require(child >= 0);
    if (!child) {
        int leader_tid = (int)SC0(178);
        abi_require(SC1(96, &leader_tid) == leader_tid);
        struct cap_departed_leader leader = {&leader_tid, ready[1], pipe[0]};
        abi_require(abi_clone_entry(0x10f00, cap_stack + sizeof(cap_stack),
                                    &child_tid, cap_departed_leader, &leader) > 0);
        abi_exit(0);
    }
    abi_require(SC3(63, ready[0], &byte, 1) == 1);
    cap_target("capget.departed-group-leader", (int)child, baseline);
    abi_require(SC3(64, pipe[1], &byte, 1) == 1);
    abi_require(SC4(260, child, 0, 0, 0) == child);
    abi_require(!SC1(57, pipe[0]) && !SC1(57, pipe[1]) &&
                !SC1(57, ready[0]) && !SC1(57, ready[1]));

    const char *path = "/cap-proc";
    abi_require(!SC3(34, -100, path, 0700));
    abi_require(!SC5(40, "proc", path, "proc", 0, 0));
    long fd = abi_open("/cap-proc/self/status", 0);
    abi_require(fd >= 0);
    char status[4096];
    long length = SC3(63, fd, status, sizeof(status));
    abi_record("capget.proc-vmswap", length < 0 ? length : 0,
               length > 0 ? cap_swap_kib(status, length) : -1, -1, 0, 0, 0);
    abi_require(!SC1(57, fd));
    fd = abi_open("/cap-proc/sys/kernel/pid_max", 0);
    length = fd >= 0 ? SC3(63, fd, status, sizeof(status)) : fd;
    long maximum = length > 0 ? cap_pid_max(status, length) : -1;
    abi_record("capget.proc-pidmax", length < 0 ? length : 0,
               maximum > SC0(178), -1, 0, 0, 0);
    header = (struct cap_header){CAP_VERSION_3, (int)maximum};
    result = maximum > 0 ? SC2(90, &header, data) : length < 0 ? length : -1;
    abi_record("capget.pidmax-unused", result, -1, -1, 0, 0, 0);
    if (fd >= 0) abi_require(!SC1(57, fd));

    fd = abi_open("/cap-proc/sys/kernel", 0x10000);
    abi_require(fd >= 0);
    int listed = 0;
    while ((length = SC3(61, fd, status, sizeof(status))) > 0) {
        for (long at = 0; at + 19 < length;) {
            unsigned short record = (unsigned char)status[at + 16] |
                ((unsigned short)(unsigned char)status[at + 17] << 8);
            abi_require(record >= 20 && at + record <= length);
            static const char wanted[] = "pid_max";
            unsigned i = 0;
            while (i < sizeof(wanted) && at + 19 + i < at + record &&
                   status[at + 19 + i] == wanted[i]) ++i;
            if (i == sizeof(wanted) && status[at + 18] == 8) listed = 1;
            at += record;
        }
    }
    abi_record("capget.proc-pidmax-listed", length, listed, -1, 0, 0, 0);
    abi_require(!SC1(57, fd) && !SC2(39, path, 0) && !SC3(35, -100, path, 0x200));
}
