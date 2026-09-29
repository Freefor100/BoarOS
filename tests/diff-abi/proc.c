#include "abi.h"

#define PROC_CLONE_THREAD 0x10f00
#define PROC_CHILD_SETTID 0x1000000
#define PROC_CHILD_CLEARTID 0x200000
extern long abi_clone_entry(long flags, void *stack, int *tid,
                            void (*fn)(void *), void *arg);
static unsigned char proc_thread_stack[16384] __attribute__((aligned(16)));
static volatile int proc_thread_tid;
static volatile int proc_thread_ready;
static volatile int proc_thread_release;
struct proc_exec_result {
    long pid, tid, status_result, threads, exe_result;
    char exe[64];
};

static void proc_child_path(char *path, long pid)
{
    const char prefix[] = "/proc-probe/";
    unsigned i = 0;
    while (i < sizeof(prefix) - 1U) { path[i] = prefix[i]; i++; }
    char reverse[24];
    unsigned used = 0;
    do { reverse[used++] = '0' + pid % 10; pid /= 10; } while (pid);
    while (used) path[i++] = reverse[--used];
    path[i] = '\0';
}

static void proc_fd_path(char *path, long fd)
{
    const char prefix[] = "/proc-probe/self/fd/";
    unsigned i = 0;
    while (i < sizeof(prefix) - 1U) { path[i] = prefix[i]; i++; }
    char reverse[24];
    unsigned used = 0;
    do { reverse[used++] = '0' + fd % 10; fd /= 10; } while (fd);
    while (used) path[i++] = reverse[--used];
    path[i] = '\0';
}

static int proc_link_number(const char *text, long length,
                            const char *prefix, unsigned prefix_length)
{
    if (length <= (long)prefix_length + 1 ||
        text[length - 1] != ']') return 0;
    for (unsigned i = 0; i < prefix_length; i++)
        if (text[i] != prefix[i]) return 0;
    for (long i = prefix_length; i < length - 1; i++)
        if (text[i] < '0' || text[i] > '9') return 0;
    return 1;
}

static int proc_directory_has(long fd, const char *wanted)
{
    if (fd < 0) return 0;
    char entries[4096];
    long got = SC3(61, fd, entries, sizeof(entries));
    for (long at = 0; got > 0 && at + 19 < got;) {
        unsigned short reclen = (unsigned char)entries[at + 16] |
            ((unsigned short)(unsigned char)entries[at + 17] << 8);
        if (reclen < 20 || at + reclen > got) break;
        long i = 0;
        while (at + 19 + i < at + reclen &&
               entries[at + 19 + i] && wanted[i]) {
            if (entries[at + 19 + i] != wanted[i]) break;
            i++;
        }
        if (at + 19 + i < at + reclen &&
            !wanted[i] && !entries[at + 19 + i]) return 1;
        at += reclen;
    }
    return 0;
}

static long proc_directory_finish(long fd)
{
    char entries[4096];
    if (fd < 0) return -9;
    for (unsigned attempt = 0; attempt < 1024U; attempt++) {
        long got = SC3(61, fd, entries, sizeof(entries));
        if (got <= 0) return got;
    }
    return -1;
}

static void proc_fd_follow_stat(const char *case_name, const char *path,
                                long descriptor)
{
    struct abi_stat followed, original;
    abi_require(SC2(80, descriptor, &original) == 0);
    long result = SC4(79, -100, path, &followed, 0);
    int same = !result && followed.mode == original.mode &&
               followed.ino == original.ino &&
               followed.rdev == original.rdev;
    abi_record(case_name, result, same ? 0 : -1, -1, 0, 0, 0);
}

static long proc_status_fd_fields(long fd, char *state, long *threads)
{
    char data[4096];
    long length = SC3(63, fd, data, sizeof(data));
    if (length < 0) return length;
    *state = 0;
    *threads = 0;
    const char state_key[] = "State:\t";
    const char threads_key[] = "Threads:\t";
    for (long i = 0; i < length; i++) {
        if (i + (long)sizeof(state_key) < length) {
            unsigned j = 0;
            while (j < sizeof(state_key) - 1U &&
                   data[i + j] == state_key[j]) j++;
            if (j == sizeof(state_key) - 1U)
                *state = data[i + j];
        }
        if (i + (long)sizeof(threads_key) < length) {
            unsigned j = 0;
            while (j < sizeof(threads_key) - 1U &&
                   data[i + j] == threads_key[j]) j++;
            if (j == sizeof(threads_key) - 1U) {
                long count = 0;
                for (long at = i + j; at < length &&
                     data[at] >= '0' && data[at] <= '9'; at++)
                    count = count * 10 + data[at] - '0';
                *threads = count;
            }
        }
    }
    return 0;
}

static long proc_status_fields(const char *path, char *state, long *threads)
{
    long fd = abi_open(path, 0);
    if (fd < 0) return fd;
    long result = proc_status_fd_fields(fd, state, threads);
    abi_require(SC1(57, fd) == 0);
    return result;
}

static void proc_live_thread(void *unused)
{
    (void)unused;
    __atomic_store_n(&proc_thread_ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&proc_thread_release, __ATOMIC_ACQUIRE))
        SC0(124);
}

static void proc_group_thread(void *argument)
{
    volatile int *control = argument;
    __atomic_store_n(&control[0], 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&control[1], __ATOMIC_ACQUIRE))
        SC0(124);
}

void abi_proc_exec_probe(void)
{
    struct proc_exec_result result = {0};
    char state = 0;
    result.pid = SC0(172);
    result.tid = SC0(178);
    result.status_result = proc_status_fields("/proc-probe/self/status",
                                              &state, &result.threads);
    result.exe_result = SC4(78, -100, "/proc-probe/self/exe",
                            result.exe, sizeof(result.exe));
    long fd = abi_open("/proc-exec-result", 1 | 64 | 512);
    if (fd < 0) abi_exit(95);
    if (SC3(64, fd, &result, sizeof(result)) != sizeof(result)) abi_exit(96);
    abi_require(SC1(57, fd) == 0);
    abi_exit(0);
}

static void proc_exec_thread(void *unused)
{
    (void)unused;
    const char *argv[] = {"/init", "proc-exec-probe", 0};
    const char *env[] = {0};
    SC3(221, argv[0], argv, env);
    abi_exit(97);
}

static void proc_nonleader_exec_cases(void)
{
    long child = CALL(220, 17, 0, 0, 0, 0, 0);
    abi_require(child >= 0);
    if (!child) {
        long member = abi_clone_entry(PROC_CLONE_THREAD,
                                      proc_thread_stack + sizeof(proc_thread_stack),
                                      0, proc_exec_thread, 0);
        abi_require(member > 0);
        for (;;) SC0(124);
    }
    int status = 0;
    abi_require(SC4(260, child, &status, 0, 0) == child);
    abi_record("proc.nonleader-exec-wait", 0, -1, -1, status, 0, 0);
    long fd = abi_open("/proc-exec-result", 0);
    abi_require(fd >= 0);
    struct proc_exec_result result;
    abi_require(SC3(63, fd, &result, sizeof(result)) == sizeof(result));
    abi_require(SC1(57, fd) == 0);
    abi_record("proc.nonleader-exec-pid", result.pid == child ? 0 : -1,
               result.tid == child ? 0 : -1, -1, 0, 0, 0);
    abi_record("proc.nonleader-exec-status", result.status_result,
               result.threads, -1, 0, 0, 0);
    abi_record("proc.nonleader-exec-exe", result.exe_result, -1, -1, 0,
               result.exe, result.exe_result > 0 ? (usize)result.exe_result : 0);
    abi_require(SC3(35, -100, "/proc-exec-result", 0) == 0);
}

static void proc_thread_cases(void)
{
    proc_thread_tid = 73;
    proc_thread_ready = 0;
    proc_thread_release = 0;
    long member = abi_clone_entry(PROC_CLONE_THREAD | PROC_CHILD_SETTID |
                                  PROC_CHILD_CLEARTID,
                                  proc_thread_stack + sizeof(proc_thread_stack),
                                  (int *)&proc_thread_tid, proc_live_thread, 0);
    abi_require(member > 0);
    for (unsigned attempt = 0; !proc_thread_ready && attempt < 10000U;
         attempt++) SC0(124);
    abi_require(proc_thread_ready);
    char state = 0;
    long threads = 0;
    long result = proc_status_fields("/proc-probe/self/status", &state,
                                     &threads);
    abi_record("proc.thread-live", result, threads, -1, 0, 0, 0);
    proc_thread_release = 1;
    for (unsigned attempt = 0; proc_thread_tid && attempt < 10000U;
         attempt++) SC0(124);
    abi_require(!proc_thread_tid);
    result = proc_status_fields("/proc-probe/self/status", &state, &threads);
    abi_record("proc.thread-after-exit", result, threads, -1, 0, 0, 0);

    volatile int *control = (void *)CALL(222, 0, 4096, 3, 0x21, -1, 0);
    abi_require((long)control > 0);
    control[0] = 0; /* member entered */
    control[1] = 0; /* member may exit */
    control[2] = -1; /* leader clear_child_tid */
    control[3] = 73;
    control[4] = 0; /* parent opened status before leader exits */
    long child = CALL(220, 17, 0, 0, 0, 0, 0);
    abi_require(child >= 0);
    if (!child) {
        control[2] = (int)SC0(178);
        abi_require(SC1(96, &control[2]) == control[2]);
        long tid = abi_clone_entry(PROC_CLONE_THREAD |
                                   PROC_CHILD_CLEARTID,
                                   proc_thread_stack + sizeof(proc_thread_stack),
                                   (int *)&control[3], proc_group_thread,
                                   (void *)control);
        abi_require(tid > 0);
        while (!__atomic_load_n(&control[4], __ATOMIC_ACQUIRE)) SC0(124);
        abi_exit(0);
    }
    for (unsigned attempt = 0; !control[0] && attempt < 10000U;
         attempt++) SC0(124);
    abi_require(control[0]);
    char path[64];
    proc_child_path(path, child);
    unsigned path_length = 0;
    while (path[path_length]) path_length++;
    const char status_suffix[] = "/status";
    for (unsigned i = 0; i < sizeof(status_suffix); i++)
        path[path_length + i] = status_suffix[i];
    long held_status = abi_open(path, 0);
    abi_require(held_status >= 0);
    __atomic_store_n(&control[4], 1, __ATOMIC_RELEASE);
    for (unsigned attempt = 0; control[2] && attempt < 10000U;
         attempt++) SC0(124);
    abi_require(!control[2]);
    result = proc_status_fd_fields(held_status, &state, &threads);
    abi_record("proc.group-held-status", result, threads, -1, 0,
               &state, 1);
    abi_require(SC1(57, held_status) == 0);
    result = proc_status_fields(path, &state, &threads);
    abi_record("proc.group-leader-status", result, threads, -1, 0,
               &state, 1);
    const char exe_suffix[] = "/exe";
    for (unsigned i = 0; i < sizeof(exe_suffix); i++)
        path[path_length + i] = exe_suffix[i];
    char link[64];
    result = SC4(78, -100, path, link, sizeof(link));
    abi_record("proc.group-leader-exe", result, -1, -1, 0,
               link, result > 0 ? (usize)result : 0);
    const char cwd_suffix[] = "/cwd";
    for (unsigned i = 0; i < sizeof(cwd_suffix); i++)
        path[path_length + i] = cwd_suffix[i];
    result = SC4(78, -100, path, link, sizeof(link));
    abi_record("proc.group-leader-cwd", result, -1, -1, 0,
               link, result > 0 ? (usize)result : 0);
    const char fd_suffix[] = "/fd/0";
    for (unsigned i = 0; i < sizeof(fd_suffix); i++)
        path[path_length + i] = fd_suffix[i];
    result = SC4(78, -100, path, link, sizeof(link));
    abi_record("proc.group-leader-fd", result, -1, -1, 0,
               link, result > 0 ? (usize)result : 0);
    __atomic_store_n(&control[1], 1, __ATOMIC_RELEASE);
    int status = 0;
    abi_require(SC4(260, child, &status, 0, 0) == child);
    abi_require(status == 0);
    abi_require(SC2(215, control, 4096) == 0);
}

struct proc_stress_control { volatile int requested, finished, tid; long replacement; };
static struct proc_stress_control stress;
static void proc_reuse_writer(void *unused)
{
    (void)unused;
    for (int i = 1; i <= 256; i++) {
        while (__atomic_load_n(&stress.requested, __ATOMIC_ACQUIRE) != i) SC0(124);
        abi_require(SC1(57, 300) == 0);
        abi_require(SC3(24, stress.replacement, 300, 0) == 300);
        __atomic_store_n(&stress.finished, i, __ATOMIC_RELEASE);
    }
}

static void proc_reuse_stress(void)
{
    long a = abi_open("/proc-stress-a", 2 | 64 | 512);
    long b = abi_open("/proc-stress-b", 2 | 64 | 512);
    abi_require(a >= 0 && b >= 0 && SC3(64, a, "A", 1) == 1 && SC3(64, b, "B", 1) == 1);
    stress = (struct proc_stress_control){.replacement = b, .tid = 7};
    abi_require(abi_clone_entry(PROC_CLONE_THREAD | PROC_CHILD_SETTID | PROC_CHILD_CLEARTID,
        proc_thread_stack + sizeof(proc_thread_stack), (int *)&stress.tid, proc_reuse_writer, 0) > 0);
    for (int i = 1; i <= 256; i++) {
        int pipefd[2] = {-1, -1};
        if (i & 1) {
            abi_require(SC3(24, a, 300, 0) == 300);
        } else {
            abi_require(SC2(59, pipefd, 0) == 0 && SC3(64, pipefd[1], "P", 1) == 1);
            abi_require(SC3(24, pipefd[0], 300, 0) == 300 && SC1(57, pipefd[0]) == 0);
        }
        long held = abi_open("/proc-probe/self/fd/300", 0);
        abi_require(held >= 0);
        __atomic_store_n(&stress.requested, i, __ATOMIC_RELEASE);
        char link[64];
        long n = SC4(78, -100, "/proc-probe/self/fd/300", link, sizeof(link));
        abi_require(n > 0 || n == -2);
        long directory = abi_open("/proc-probe/self/fd", 65536);
        abi_require(directory >= 0 && proc_directory_finish(directory) == 0 && SC1(57, directory) == 0);
        while (__atomic_load_n(&stress.finished, __ATOMIC_ACQUIRE) != i) SC0(124);
        char byte = 0;
        abi_require(SC3(63, held, &byte, 1) == 1 && byte == ((i & 1) ? 'A' : 'P'));
        if (i & 1) abi_require(SC3(62, a, 0, 1) == 1);
        abi_require(SC1(57, held) == 0);
        if (!(i & 1)) abi_require(SC1(57, pipefd[1]) == 0);
        long fresh = abi_open("/proc-probe/self/fd/300", 0);
        abi_require(fresh >= 0 && SC3(63, fresh, &byte, 1) == 1 && byte == 'B');
        abi_require(SC1(57, fresh) == 0);
    }
    while (__atomic_load_n(&stress.tid, __ATOMIC_ACQUIRE)) SC0(124);
    abi_require(SC1(57, 300) == 0 && SC1(57, a) == 0 && SC1(57, b) == 0);
    abi_record("proc.fd-reuse-stress", 0, -1, -1, 0, 0, 0);
}

/* Catch missing/fictional memory fields without comparing host capacities. */
static unsigned long memory_field(const char *text, long length, const char *key)
{
    for (long i = 0; i < length; i++) {
        if (i && text[i - 1] != '\n') continue;
        unsigned j = 0;
        while (key[j] && i + j < length && text[i + j] == key[j]) j++;
        if (key[j]) continue;
        long k = i + j;
        while (k < length && (text[k] == ' ' || text[k] == '\t')) k++;
        if (k == length || text[k] < '0' || text[k] > '9') return ~0UL;
        unsigned long value = 0;
        while (k < length && text[k] >= '0' && text[k] <= '9')
            value = value * 10 + text[k++] - '0';
        return value;
    }
    return ~0UL;
}

static void proc_memory_cases(long fd)
{
    static char text[4096];
    long n = SC3(63, fd, text, sizeof(text));
    const char *keys[] = {"MemTotal:", "MemFree:", "MemAvailable:", "Cached:",
        "Buffers:", "Shmem:", "Dirty:", "Writeback:", "SReclaimable:"};
    int valid = n > 0;
    unsigned long total = memory_field(text, n, keys[0]);
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        unsigned long value = memory_field(text, n, keys[i]);
        if (value == ~0UL || value > total) valid = 0;
    }
    abi_record("proc.memory-fields", valid && total ? 0 : -1, -1, -1, 0, 0, 0);
    abi_require(SC3(62, fd, 0, 0) == 0);
    struct {
        long uptime;
        unsigned long loads[3], totalram, freeram, sharedram, bufferram;
        unsigned long totalswap, freeswap;
        unsigned short procs, pad;
        unsigned long totalhigh, freehigh;
        unsigned mem_unit;
    } info = {0};
    long result = SC1(179, &info);
    abi_record("sysinfo.shape", result == 0 && info.totalram > 0 &&
        info.freeram <= info.totalram && info.mem_unit && info.procs &&
        info.uptime >= 0 ? 0 : -1, -1, -1, 0, 0, 0);
    abi_record("sysinfo.fault", SC1(179, 1), -1, -1, 0, 0, 0);
}

/* Missing mount dispatch must not masquerade as a procfs consumer failure. */
void abi_proc_cases(void)
{
    abi_require(SC3(34, -100, "/proc-probe", 0755) == 0);
    long mounted = SC5(40, "proc", "/proc-probe", "proc", 0, 0);
    abi_record("proc.mount", mounted, -1, -1, 0, 0, 0);
    long root_directory = abi_open("/proc-probe", 0);
    abi_record("proc.root-list",
               proc_directory_has(root_directory, "meminfo") ? 0 : -1,
               -1, -1, 0, 0, 0);
    abi_record("proc.root-eof", proc_directory_finish(root_directory),
               -1, -1, 0, 0, 0);
    if (root_directory >= 0) abi_require(SC1(57, root_directory) == 0);
    long fd = abi_open("/proc-probe/meminfo", 0);
    abi_record("proc.meminfo-open", fd < 0 ? fd : 0, -1, -1, 0, 0, 0);
    proc_memory_cases(fd);
    proc_reuse_stress();
    long writable = abi_open("/proc-probe/meminfo", 1);
    abi_record("proc.meminfo-write-open", writable < 0 ? writable : 0,
               -1, -1, 0, 0, 0);
    abi_record("proc.meminfo-write",
               writable >= 0 ? SC3(64, writable, "x", 1) : -9,
               -1, -1, 0, 0, 0);
    if (writable >= 0) abi_require(SC1(57, writable) == 0);
    long uptime_fd = abi_open("/proc-probe/uptime", 0);
    abi_record("proc.uptime-open", uptime_fd < 0 ? uptime_fd : 0,
               -1, -1, 0, 0, 0);
    if (uptime_fd >= 0) {
        char uptime[80] = {0};
        long length = SC3(63, uptime_fd, uptime, sizeof(uptime));
        int shape = length >= 10 && length < (long)sizeof(uptime);
        long dot_count = 0, space_count = 0;
        for (long i = 0; i < length && shape; i++) {
            char c = uptime[i];
            if (c == '.') dot_count++;
            else if (c == ' ') space_count++;
            else if (c != '\n' && (c < '0' || c > '9')) shape = 0;
        }
        shape = shape && dot_count == 2 && space_count == 1 &&
                uptime[length - 1] == '\n';
        abi_record("proc.uptime-shape", shape ? 0 : -1,
                   -1, -1, 0, 0, 0);
        abi_require(SC1(57, uptime_fd) == 0);
    }
    char self_link[16] = {0};
    long self_length = SC4(78, -100, "/proc-probe/self",
                           self_link, sizeof(self_link));
    abi_record("proc.self-link", self_length, -1, -1, 0,
               self_link, self_length > 0 ? (usize)self_length : 0);
    long self_directory = abi_open("/proc-probe/self", 0);
    abi_record("proc.self-open", self_directory < 0 ? self_directory : 0,
               -1, -1, 0, 0, 0);
    if (self_directory >= 0) abi_require(SC1(57, self_directory) == 0);
    char object_link[40] = {0};
    long object_length = SC4(78, -100, "/proc-probe/self/exe",
                              object_link, sizeof(object_link));
    abi_record("proc.exe-link", object_length, -1, -1, 0,
               object_link, object_length > 0 ? (usize)object_length : 0);
    long executable = abi_open("/proc-probe/self/exe", 0);
    abi_record("proc.exe-open", executable < 0 ? executable : 0,
               -1, -1, 0, 0, 0);
    if (executable >= 0) {
        unsigned char elf[4] = {0};
        abi_record("proc.exe-magic", SC3(63, executable, elf, sizeof(elf)),
                   -1, -1, 0, elf, sizeof(elf));
        abi_require(SC1(57, executable) == 0);
    }
    object_length = SC4(78, -100, "/proc-probe/self/cwd",
                        object_link, sizeof(object_link));
    abi_record("proc.cwd-link", object_length, -1, -1, 0,
               object_link, object_length > 0 ? (usize)object_length : 0);
    object_length = SC4(78, -100, "/proc-probe/self/root",
                        object_link, sizeof(object_link));
    abi_record("proc.root-link", object_length, -1, -1, 0,
               object_link, object_length > 0 ? (usize)object_length : 0);
    char mounts_link[32] = {0};
    long mounts_length = SC4(78, -100, "/proc-probe/mounts",
                             mounts_link, sizeof(mounts_link));
    abi_record("proc.mounts-link", mounts_length, -1, -1, 0,
               mounts_link, mounts_length > 0 ? (usize)mounts_length : 0);
    long mounts_fd = abi_open("/proc-probe/self/mounts", 0);
    abi_record("proc.mounts-open", mounts_fd < 0 ? mounts_fd : 0,
               -1, -1, 0, 0, 0);
    int has_proc = 0;
    if (mounts_fd >= 0) {
        char mounts[4096];
        long got = SC3(63, mounts_fd, mounts, sizeof(mounts) - 1U);
        if (got > 0 && got < (long)sizeof(mounts)) {
            mounts[got] = '\0';
            const char *needle = " /proc-probe proc ";
            for (long i = 0; i + 18 <= got; i++) {
                int equal = 1;
                for (long j = 0; j < 18; j++)
                    if (mounts[i + j] != needle[j]) equal = 0;
                if (equal) {
                    has_proc = 1;
                    break;
                }
            }
        }
        abi_require(SC1(57, mounts_fd) == 0);
    }
    abi_record("proc.mounts-has-proc", has_proc ? 0 : -1,
               -1, -1, 0, 0, 0);
    long stat_fd = abi_open("/proc-probe/self/stat", 0);
    abi_record("proc.stat-open", stat_fd < 0 ? stat_fd : 0,
               -1, -1, 0, 0, 0);
    int stat_shape = 0;
    if (stat_fd >= 0) {
        char record[1024];
        long got = SC3(63, stat_fd, record, sizeof(record));
        if (got > 0 && got < (long)sizeof(record) && record[got - 1] == '\n') {
            long close = -1;
            for (long i = 0; i < got; i++) if (record[i] == ')') close = i;
            int fields = 2;
            if (close > 0 && record[close + 1] == ' ') {
                for (long i = close + 2; i < got; i++)
                    if (record[i] != ' ' && record[i] != '\n' &&
                        record[i - 1] == ' ') fields++;
                stat_shape = fields >= 24;
            }
        }
        abi_require(SC1(57, stat_fd) == 0);
    }
    abi_record("proc.stat-fields", stat_shape ? 0 : -1,
               -1, -1, 0, 0, 0);
    long status_fd = abi_open("/proc-probe/self/status", 0);
    abi_record("proc.status-open", status_fd < 0 ? status_fd : 0,
               -1, -1, 0, 0, 0);
    int status_shape = 0;
    if (status_fd >= 0) {
        char record[2048];
        long got = SC3(63, status_fd, record, sizeof(record));
        int name = 0, state = 0, pid = 0, rss = 0;
        for (long i = 0; i + 7 < got; i++) {
            if (record[i] == 'N' && record[i + 1] == 'a' &&
                record[i + 2] == 'm' && record[i + 3] == 'e' &&
                record[i + 4] == ':') name = 1;
            if (record[i] == 'S' && record[i + 1] == 't' &&
                record[i + 2] == 'a' && record[i + 3] == 't' &&
                record[i + 4] == 'e' && record[i + 5] == ':') state = 1;
            if (record[i] == 'P' && record[i + 1] == 'i' &&
                record[i + 2] == 'd' && record[i + 3] == ':') pid = 1;
            if (record[i] == 'V' && record[i + 1] == 'm' &&
                record[i + 2] == 'R' && record[i + 3] == 'S' &&
                record[i + 4] == 'S' && record[i + 5] == ':') rss = 1;
        }
        status_shape = name && state && pid && rss;
        abi_require(SC1(57, status_fd) == 0);
    }
    abi_record("proc.status-fields", status_shape ? 0 : -1,
               -1, -1, 0, 0, 0);
    char stdio_path[48];
    proc_fd_path(stdio_path, 0);
    char stdio_link[64] = {0};
    long stdio_length = SC4(78, -100, stdio_path,
                            stdio_link, sizeof(stdio_link));
    abi_record("proc.stdio-console-link", stdio_length, -1, -1, 0,
               stdio_link, stdio_length > 0 ? (usize)stdio_length : 0);
    proc_fd_follow_stat("proc.stdio-console-follow-stat", stdio_path, 0);
    long fd_directory = abi_open("/proc-probe/self/fd", 0);
    abi_record("proc.fd-dir", fd_directory < 0 ? fd_directory : 0,
               -1, -1, 0, 0, 0);
    long object_fd = SC4(56, -100, "/proc-fd-target", 0102, 0644);
    abi_require(object_fd >= 0);
    abi_require(SC3(64, object_fd, "abcdef", 6) == 6);
    abi_require(SC3(62, object_fd, 3, 0) == 3);
    char fd_path[48];
    proc_fd_path(fd_path, object_fd);
    const char *wanted_fd = fd_path + sizeof("/proc-probe/self/fd/") - 1;
    int listed = proc_directory_has(fd_directory, wanted_fd);
    abi_record("proc.fd-list-target", listed ? 0 : -1,
               -1, -1, 0, 0, 0);
    abi_record("proc.fd-eof", proc_directory_finish(fd_directory),
               -1, -1, 0, 0, 0);
    if (fd_directory >= 0) abi_require(SC1(57, fd_directory) == 0);
    char fd_link[64] = {0};
    long fd_link_size = SC4(78, -100, fd_path, fd_link, sizeof(fd_link));
    abi_record("proc.fd-link", fd_link_size, -1, -1, 0,
               fd_link, fd_link_size > 0 ? (usize)fd_link_size : 0);
    struct abi_stat fd_stat;
    long fd_stat_result = SC4(79, -100, fd_path, &fd_stat, 0x100);
    abi_record("proc.fd-link-mode", fd_stat_result,
               fd_stat_result ? -1 : (long)(fd_stat.mode & 0777U),
               -1, 0, 0, 0);
    proc_fd_follow_stat("proc.fd-follow-stat", fd_path, object_fd);
    long reopened = abi_open(fd_path, 0);
    abi_record("proc.fd-reopen", reopened < 0 ? reopened : 0,
               -1, -1, 0, 0, 0);
    char first = 0;
    abi_record("proc.fd-reopen-read",
               reopened >= 0 ? SC3(63, reopened, &first, 1) : -9,
               -1, -1, 0, &first, 1);
    abi_record("proc.fd-original-offset", abi_offset(object_fd),
               -1, -1, 0, 0, 0);
    if (reopened >= 0) abi_require(SC1(57, reopened) == 0);
    abi_require(SC1(57, object_fd) == 0);
    abi_record("proc.fd-closed-link",
               SC4(78, -100, fd_path, fd_link, sizeof(fd_link)),
               -1, -1, 0, 0, 0);
    abi_record("proc.fd-closed-follow-stat",
               SC4(79, -100, fd_path, &fd_stat, 0),
               -1, -1, 0, 0, 0);
    abi_require(SC3(35, -100, "/proc-fd-target", 0) == 0);
    long reused = SC4(56, -100, "/proc-fd-second", 0102, 0644);
    abi_require(reused >= 0);
    proc_fd_path(fd_path, reused);
    long reused_length = SC4(78, -100, fd_path, fd_link, sizeof(fd_link));
    abi_record("proc.fd-reused-link", reused_length, -1, -1, 0,
               fd_link, reused_length > 0 ? (usize)reused_length : 0);
    proc_fd_follow_stat("proc.fd-reused-follow-stat", fd_path, reused);
    abi_require(SC1(57, reused) == 0);
    abi_require(SC3(35, -100, "/proc-fd-second", 0) == 0);
    int pipe_fds[2] = {-1, -1};
    abi_require(SC2(59, pipe_fds, 0) == 0);
    proc_fd_path(fd_path, pipe_fds[0]);
    fd_link_size = SC4(78, -100, fd_path, fd_link, sizeof(fd_link));
    abi_record("proc.fd-pipe-link",
               proc_link_number(fd_link, fd_link_size, "pipe:[", 6U)
                    ? 0 : -1, -1, -1, 0, 0, 0);
    fd_stat_result = SC4(79, -100, fd_path, &fd_stat, 0x100);
    abi_record("proc.fd-pipe-mode", fd_stat_result,
               fd_stat_result ? -1 : (long)(fd_stat.mode & 0777U),
               -1, 0, 0, 0);
    proc_fd_follow_stat("proc.fd-pipe-follow-stat", fd_path, pipe_fds[0]);
    reopened = abi_open(fd_path, 0);
    abi_record("proc.fd-pipe-reopen", reopened < 0 ? reopened : 0,
               -1, -1, 0, 0, 0);
    abi_require(SC3(64, pipe_fds[1], "P", 1) == 1);
    char pipe_byte = 0;
    abi_record("proc.fd-pipe-read",
               reopened >= 0 ? SC3(63, reopened, &pipe_byte, 1) : -9,
               -1, -1, 0, &pipe_byte, 1);
    long both = abi_open(fd_path, 2);
    abi_record("proc.fd-pipe-rdwr", both < 0 ? both : 0,
               -1, -1, 0, 0, 0);
    abi_require(SC1(57, pipe_fds[0]) == 0);
    if (reopened >= 0) abi_require(SC3(64, pipe_fds[1], "Q", 1) == 1);
    pipe_byte = 0;
    abi_record("proc.fd-pipe-after-close",
               reopened >= 0 ? SC3(63, reopened, &pipe_byte, 1) : -9,
               -1, -1, 0, &pipe_byte, 1);
    abi_record("proc.fd-pipe-rdwr-write",
               both >= 0 ? SC3(64, both, "R", 1) : -9,
               -1, -1, 0, 0, 0);
    pipe_byte = 0;
    abi_record("proc.fd-pipe-rdwr-read",
               both >= 0 ? SC3(63, both, &pipe_byte, 1) : -9,
               -1, -1, 0, &pipe_byte, 1);
    if (both >= 0) abi_require(SC1(57, both) == 0);
    proc_fd_path(fd_path, pipe_fds[1]);
    long reopened_writer = abi_open(fd_path, 1);
    abi_record("proc.fd-pipe-wr-reopen",
               reopened_writer < 0 ? reopened_writer : 0,
               -1, -1, 0, 0, 0);
    abi_require(SC1(57, pipe_fds[1]) == 0);
    abi_record("proc.fd-pipe-wr-after-close",
               reopened_writer >= 0 ? SC3(64, reopened_writer, "T", 1) : -9,
               -1, -1, 0, 0, 0);
    pipe_byte = 0;
    abi_record("proc.fd-pipe-wr-delivered",
               reopened_writer >= 0 && reopened >= 0
                   ? SC3(63, reopened, &pipe_byte, 1) : -9,
               -1, -1, 0, &pipe_byte, 1);
    if (reopened_writer >= 0)
        abi_require(SC1(57, reopened_writer) == 0);
    if (reopened >= 0) abi_require(SC1(57, reopened) == 0);
    long socket_fd = SC3(198, 2, 2, 0);
    abi_require(socket_fd >= 0);
    proc_fd_path(fd_path, socket_fd);
    fd_link_size = SC4(78, -100, fd_path, fd_link, sizeof(fd_link));
    abi_record("proc.fd-socket-link",
               proc_link_number(fd_link, fd_link_size, "socket:[", 8U)
                    ? 0 : -1, -1, -1, 0, 0, 0);
    fd_stat_result = SC4(79, -100, fd_path, &fd_stat, 0x100);
    abi_record("proc.fd-socket-mode", fd_stat_result,
               fd_stat_result ? -1 : (long)(fd_stat.mode & 0777U),
               -1, 0, 0, 0);
    proc_fd_follow_stat("proc.fd-socket-follow-stat", fd_path, socket_fd);
    reopened = abi_open(fd_path, 0);
    abi_record("proc.fd-socket-reopen", reopened < 0 ? reopened : 0,
               -1, -1, 0, 0, 0);
    if (reopened >= 0) abi_require(SC1(57, reopened) == 0);
    abi_require(SC1(57, socket_fd) == 0);
    long epoll_fd = SC1(20, 0);
    abi_require(epoll_fd >= 0);
    proc_fd_path(fd_path, epoll_fd);
    fd_link_size = SC4(78, -100, fd_path, fd_link, sizeof(fd_link));
    const char epoll_link[] = "anon_inode:[eventpoll]";
    int epoll_match = fd_link_size == (long)sizeof(epoll_link) - 1;
    for (unsigned i = 0; epoll_match && i < sizeof(epoll_link) - 1; i++)
        if (fd_link[i] != epoll_link[i]) epoll_match = 0;
    abi_record("proc.fd-epoll-link", epoll_match ? 0 : -1,
               -1, -1, 0, 0, 0);
    fd_stat_result = SC4(79, -100, fd_path, &fd_stat, 0x100);
    abi_record("proc.fd-epoll-mode", fd_stat_result,
               fd_stat_result ? -1 : (long)(fd_stat.mode & 0777U),
               -1, 0, 0, 0);
    proc_fd_follow_stat("proc.fd-epoll-follow-stat", fd_path, epoll_fd);
    reopened = abi_open(fd_path, 0);
    abi_record("proc.fd-epoll-reopen", reopened < 0 ? reopened : 0,
               -1, -1, 0, 0, 0);
    if (reopened >= 0) abi_require(SC1(57, reopened) == 0);
    abi_require(SC1(57, epoll_fd) == 0);
    proc_thread_cases();
    proc_nonleader_exec_cases();
    long child = CALL(220, 17, 0, 0, 0, 0, 0);
    abi_require(child >= 0);
    if (!child) abi_exit(0);
    char child_path[48];
    proc_child_path(child_path, child);
    long child_dir = abi_open(child_path, 0);
    abi_require(child_dir >= 0);
    int zombie = 0;
    for (unsigned attempt = 0; attempt < 10000U; attempt++) {
        long child_stat = SC4(56, child_dir, "stat", 0, 0);
        if (child_stat >= 0) {
            char text[256];
            long got = SC3(63, child_stat, text, sizeof(text));
            for (long i = 0; i + 3 < got; i++)
                if (text[i] == ')' && text[i + 1] == ' ' &&
                    text[i + 2] == 'Z') zombie = 1;
            abi_require(SC1(57, child_stat) == 0);
        }
        if (zombie) break;
        SC0(124);
    }
    abi_record("proc.zombie-stat", zombie ? 0 : -1,
               -1, -1, 0, 0, 0);
    int child_status = 0;
    abi_require(SC4(260, child, &child_status, 0, 0) == child);
    long old_stat = SC4(56, child_dir, "stat", 0, 0);
    abi_record("proc.reaped-old-stat", old_stat < 0 ? old_stat : 0,
               -1, -1, 0, 0, 0);
    if (old_stat >= 0) abi_require(SC1(57, old_stat) == 0);
    long replacement = CALL(220, 17, 0, 0, 0, 0, 0);
    abi_require(replacement >= 0);
    if (!replacement) abi_exit(0);
    old_stat = SC4(56, child_dir, "stat", 0, 0);
    abi_record("proc.reused-old-stat", old_stat < 0 ? old_stat : 0,
               -1, -1, 0, 0, 0);
    if (old_stat >= 0) abi_require(SC1(57, old_stat) == 0);
    proc_child_path(child_path, replacement);
    long replacement_dir = abi_open(child_path, 0);
    abi_record("proc.replacement-dir",
               replacement_dir < 0 ? replacement_dir : 0,
               -1, -1, 0, 0, 0);
    if (replacement_dir >= 0) abi_require(SC1(57, replacement_dir) == 0);
    abi_require(SC4(260, replacement, &child_status, 0, 0) == replacement);
    abi_require(SC1(57, child_dir) == 0);
    if (fd >= 0) {
        char header[9] = {0};
        abi_record("proc.meminfo-read", SC3(63, fd, header, sizeof(header)),
                   -1, -1, 0, header, sizeof(header));
        abi_record("proc.meminfo-rewind", SC3(62, fd, 0, 0),
                   -1, -1, 0, 0, 0);
        char first[4] = {0}, second[5] = {0};
        struct abi_iovec iov[2] = {{first, sizeof(first)},
                                   {second, sizeof(second)}};
        abi_record("proc.meminfo-readv", SC3(65, fd, iov, 2),
                   -1, -1, 0, 0, 0);
        abi_record("proc.meminfo-readv-first", 0, -1, -1, 0,
                   first, sizeof(first));
        abi_record("proc.meminfo-readv-second", 0, -1, -1, 0,
                   second, sizeof(second));
        char positioned[9] = {0};
        abi_record("proc.meminfo-pread", SC4(67, fd, positioned,
                   sizeof(positioned), 0), -1, abi_offset(fd), 0,
                   positioned, sizeof(positioned));
        abi_record("proc.meminfo-seek-end", SC3(62, fd, 0, 2),
                   -1, -1, 0, 0, 0);
        abi_require(SC3(62, fd, 1048576, 0) == 1048576);
        abi_record("proc.meminfo-eof", SC3(63, fd, header, sizeof(header)),
                   -1, -1, 0, 0, 0);
        abi_require(SC3(62, fd, 0, 0) == 0);
        long fault_map = CALL(222, 0, 8192, 3, 0x22, -1, 0);
        abi_require(fault_map >= 0);
        abi_require(SC3(226, fault_map + 4096, 4096, 0) == 0);
        unsigned char *prefix = (void *)(fault_map + 4096 - 8);
        long partial = SC3(63, fd, prefix, 16);
        abi_record("proc.meminfo-fault-prefix", partial, -1,
                   abi_offset(fd), 0, prefix, partial > 0 && partial <= 8
                       ? (usize)partial : 0);
        char continuation[8] = {0};
        long continued = SC3(63, fd, continuation, sizeof(continuation));
        abi_record("proc.meminfo-fault-next", continued, -1,
                   abi_offset(fd), 0, continuation,
                   continued > 0 ? 1U : 0U);
        abi_require(SC2(215, fault_map, 8192) == 0);
        abi_require(SC3(62, fd, 0, 0) == 0);
        abi_record("proc.meminfo-fault-first",
                   SC3(63, fd, (void *)-1, 4), -1,
                   abi_offset(fd), 0, 0, 0);
        abi_record("proc.busy-unmount", SC2(39, "/proc-probe", 0),
                   -1, -1, 0, 0, 0);
        abi_require(SC1(57, fd) == 0);
    }
    abi_record("proc.unmount", SC2(39, "/proc-probe", 0), -1, -1, 0, 0, 0);
    abi_record("proc.readonly-silent-mount",
               SC5(40, "proc", "/proc-probe", "proc", 32769, 0),
               -1, -1, 0, 0, 0);
    abi_record("proc.stack-mount",
               SC5(40, "proc", "/proc-probe", "proc", 0, 0),
               -1, -1, 0, 0, 0);
    abi_record("proc.stack-unmount", SC2(39, "/proc-probe", 0),
               -1, -1, 0, 0, 0);
    abi_require(SC1(49, "/proc-probe") == 0);
    char cwd[32] = {0};
    abi_record("proc.cwd", SC2(17, cwd, sizeof(cwd)),
               -1, -1, 0, cwd, 12);
    abi_record("proc.cwd-busy-unmount", SC2(39, "/proc-probe", 0),
               -1, -1, 0, 0, 0);
    abi_require(SC1(49, "/") == 0);
    abi_require(SC3(35, -100, "/init", 0) == 0);
    char deleted_exe[40] = {0};
    long deleted_length = SC4(78, -100, "/proc-probe/self/exe",
                              deleted_exe, sizeof(deleted_exe));
    abi_record("proc.exe-deleted-link", deleted_length, -1, -1, 0,
               deleted_exe, deleted_length > 0 ? (usize)deleted_length : 0);
    long deleted_fd = abi_open("/proc-probe/self/exe", 0);
    abi_record("proc.exe-deleted-open", deleted_fd < 0 ? deleted_fd : 0,
               -1, -1, 0, 0, 0);
    if (deleted_fd >= 0) {
        unsigned char magic[4] = {0};
        abi_record("proc.exe-deleted-magic",
                   SC3(63, deleted_fd, magic, sizeof(magic)),
                   -1, -1, 0, magic, sizeof(magic));
        abi_require(SC1(57, deleted_fd) == 0);
    }
    abi_record("proc.readonly-unmount", SC2(39, "/proc-probe", 0),
               -1, -1, 0, 0, 0);
    abi_require(SC3(35, -100, "/proc-probe", 512) == 0);
}
