#include "abi.h"

#define THREAD_FLAGS (0x10f00 | 0x1000000 | 0x200000)
extern long abi_clone_entry(long, void *, int *, void (*)(void *), void *);
static unsigned char thread_stack[16384] __attribute__((aligned(16)));
static volatile int thread_tid;
static volatile int thread_ready;
static int thread_creates_session;
static long thread_session_result;
static int thread_gate[2];
struct session_info { int signo, error, code, pad, pid; unsigned uid; char rest[104]; };
struct session_time { long seconds, nanos; };
static volatile int orphan_handler_signal, orphan_handler_code;
static void orphan_handler(int signal, const struct session_info *info, void *context)
{
    (void)context;
    orphan_handler_signal = signal;
    orphan_handler_code = info->code;
}

static void pipe_make(int p[2]) { abi_require(SC2(59, p, 0) == 0); }
static void close_one(int fd) { abi_require(SC1(57, fd) == 0); }
static void send_data(int fd, const void *p, usize n) { abi_require(SC3(64, fd, p, n) == (long)n); }
static void receive(int fd, void *p, usize n) { abi_require(SC3(63, fd, p, n) == (long)n); }
static void token_send(int fd) { char c = 's'; send_data(fd, &c, 1); }
static void token_get(int fd) { char c; receive(fd, &c, 1); }
static long fork_one(void) { long p = SC5(220, 17, 0, 0, 0, 0); abi_require(p >= 0); return p; }
static int wait_one(long pid) { int status; abi_require(SC4(260, pid, &status, 0, 0) == pid); return status; }
static void record(const char *id, long result) { abi_record(id, result, -1, -1, 0, 0, 0); }
static void decimal(char *s, long value)
{
    char rev[24]; unsigned n = 0;
    do { rev[n++] = '0' + value % 10; value /= 10; } while (value);
    unsigned i = 0;
    while (n) s[i++] = rev[--n];
    s[i] = 0;
}
static long parse(const char *s) { long n = 0; while (*s) n = n * 10 + *s++ - '0'; return n; }
static int equal(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

/* The probe writes after exec, then stays alive until its parent tested EACCES. */
void abi_session_exec_probe(const unsigned long *sp)
{
    if (sp[0] != 7 || !equal((const char *)sp[2], "session-exec-probe")) return;
    long fd = parse((const char *)sp[3]), gate = parse((const char *)sp[4]);
    long pid = parse((const char *)sp[5]), pgid = parse((const char *)sp[6]);
    long sid = parse((const char *)sp[7]);
    long result[4] = {SC0(172) == pid, SC0(178) == pid,
        SC1(155, 0) == pgid, SC1(156, 0) == sid};
    send_data((int)fd, result, sizeof(result));
    token_get((int)gate);
    abi_exit(0);
}

static void basic_worker(int fd)
{
    long r[16], me = SC0(172), inherited = SC1(155, 0), sid = SC1(156, 0);
    r[0] = SC1(155, me) == inherited && SC1(156, me) == sid;
    r[1] = SC2(154, 0, 0);
    r[2] = SC1(155, me) == me;
    r[3] = SC0(157);
    r[4] = SC2(154, 0, inherited);
    long result = SC0(157);
    r[5] = result < 0 ? result : result == me;
    r[6] = SC1(156, 0) == me && SC1(155, 0) == me;
    r[7] = SC2(154, 0, 0);
    r[8] = SC0(157);
    int gate[2]; pipe_make(gate);
    long child = fork_one();
    if (!child) { token_get(gate[0]); abi_exit(0); }
    r[9] = SC2(154, child, 0);
    r[10] = SC1(155, child) == child && SC1(156, child) == me;
    r[11] = SC2(154, child, me);
    r[12] = SC2(154, child, 2147483647);
    r[13] = SC2(154, child, -1);
    r[14] = SC2(154, -1, -1);
    token_send(gate[1]);
    r[15] = wait_one(child);
    send_data(fd, r, sizeof(r)); abi_exit(0);
}

static void run_worker(void (*worker)(int), const char *const *names, unsigned count)
{
    int report[2]; pipe_make(report);
    long child = fork_one();
    if (!child) {
        close_one(report[0]);
        /* Linux PID 1 initially inherits numeric group/session zero. Establish
         * a real session so joining the inherited group is unambiguous. */
        abi_require(SC0(157) == SC0(172));
        long probe = fork_one();
        if (!probe) { worker(report[1]); abi_exit(98); }
        abi_exit(wait_one(probe) == 0 ? 0 : 98);
    }
    close_one(report[1]);
    long results[24]; receive(report[0], results, count * sizeof(long));
    close_one(report[0]); abi_require(wait_one(child) == 0);
    for (unsigned i = 0; i < count; i++) record(names[i], results[i]);
}

static void waiting_thread(void *unused)
{
    (void)unused;
    if (thread_creates_session) {
        long result = SC0(157);
        thread_session_result = result < 0 ? result : result == SC0(172);
    }
    thread_ready = 1;
    token_get(thread_gate[0]);
}

static int exec_report, exec_gate;
static long exec_pid, exec_pgid, exec_sid;
static void exec_thread(void *unused)
{
    (void)unused;
    char f[24], g[24], p[24], pg[24], s[24];
    decimal(f, exec_report); decimal(g, exec_gate); decimal(p, exec_pid);
    decimal(pg, exec_pgid); decimal(s, exec_sid);
    const char *argv[] = {"/init", "session-exec-probe", f, g, p, pg, s, 0};
    const char *envp[] = {0};
    SC3(221, argv[0], argv, envp);
    abi_exit(97);
}

static void exec_cases(int session_leader)
{
    int result[2], gate[2], ready[2]; pipe_make(result); pipe_make(gate); pipe_make(ready);
    long sid = SC1(156, 0);
    long child = fork_one();
    if (!child) {
        pipe_make(thread_gate);
        thread_tid = 0; thread_ready = 0;
        thread_creates_session = session_leader; thread_session_result = 0;
        long tid = abi_clone_entry(THREAD_FLAGS, thread_stack + sizeof(thread_stack),
            (int *)&thread_tid, waiting_thread, 0);
        abi_require(tid > 0);
        while (!thread_ready) SC0(124);
        long r[6] = {SC1(155, tid) == SC1(155, 0), SC1(156, tid) == SC1(156, 0),
            SC2(154, tid, 0), SC2(154, tid, -1), SC2(129, tid, 0), thread_session_result};
        send_data(result[1], r, sizeof(r));
        token_send(thread_gate[1]);
        while (thread_tid) SC0(124);
        if (!session_leader) abi_require(SC2(154, 0, 0) == 0);
        exec_report = result[1]; exec_gate = gate[0]; exec_pid = SC0(172);
        exec_pgid = exec_pid; exec_sid = session_leader ? exec_pid : sid;
        token_send(ready[1]); token_get(gate[0]);
        thread_tid = 0;
        abi_require(abi_clone_entry(THREAD_FLAGS, thread_stack + sizeof(thread_stack),
            (int *)&thread_tid, exec_thread, 0) > 0);
        for (;;) SC0(124);
    }
    long r[6]; receive(result[0], r, sizeof(r));
    const char *const names[2][13] = {
        {"session.thread-getpgid", "session.thread-getsid", "session.thread-setpgid", "session.thread-negative-pgid", "session.kill-thread-probe", "session.thread-no-setsid", "session.parent-before-exec", "session.exec-pid", "session.exec-tid", "session.exec-pgid", "session.exec-sid", "session.parent-after-exec", "session.exec-status"},
        {"session.leader-thread-getpgid", "session.leader-thread-getsid", "session.leader-thread-setpgid", "session.leader-thread-negative-pgid", "session.leader-kill-thread-probe", "session.thread-setsid", "session.leader-parent-before-exec", "session.leader-exec-pid", "session.leader-exec-tid", "session.leader-exec-pgid", "session.leader-exec-sid", "session.leader-parent-after-exec", "session.leader-exec-status"}
    };
    for (unsigned i = 0; i < 6; i++) record(names[session_leader][i], r[i]);
    token_get(ready[0]);
    record(names[session_leader][6], SC2(154, child, child));
    token_send(gate[1]); receive(result[0], r, 4 * sizeof(long));
    for (unsigned i = 0; i < 4; i++) record(names[session_leader][i + 7], r[i]);
    record(names[session_leader][11], SC2(154, child, child));
    token_send(gate[1]); record(names[session_leader][12], wait_one(child));
    close_one(result[0]); close_one(result[1]); close_one(gate[0]); close_one(gate[1]);
    close_one(ready[0]); close_one(ready[1]);
}

static void session_boundary_worker(int fd)
{
    long r[3], parent = SC0(173), self = SC0(172);
    abi_require(SC0(157) == self);
    r[0] = SC2(154, parent, 0);
    int pipefd[2]; pipe_make(pipefd);
    long child = fork_one();
    if (!child) { abi_require(SC0(157) == SC0(172)); token_send(pipefd[1]); token_get(thread_gate[0]); abi_exit(0); }
    token_get(pipefd[0]);
    r[1] = SC2(154, child, child);
    r[2] = SC1(156, child) == child;
    token_send(thread_gate[1]); abi_require(wait_one(child) == 0);
    send_data(fd, r, sizeof(r)); abi_exit(0);
}

/* Every stop is acknowledged by wait4 before severing the last parent link.
 * Block HUP/CONT so rt_sigtimedwait observes both kernel-generated signals. */
static void orphan_case(int separate_group)
{
    int report[2], parent_ready[2], bridge_gate[2], child_gate[2];
    pipe_make(report); pipe_make(parent_ready); pipe_make(bridge_gate); pipe_make(child_gate);
    long coordinator = fork_one();
    if (!coordinator) {
        long session = SC0(172);
        abi_require(SC0(157) == session);
        long bridge = fork_one();
        if (!bridge) {
            if (!separate_group) abi_require(SC2(154, 0, 0) == 0);
            long group = SC1(155, 0);
            long child = fork_one();
            if (!child) {
                if (separate_group) abi_require(SC2(154, 0, 0) == 0);
                unsigned long mask = 1UL | (1UL << 17);
                abi_require(SC4(135, 0, &mask, 0, 8) == 0);
                abi_require(SC2(129, SC0(172), 19) == 0);
                struct session_time timeout = {2, 0};
                struct session_info info;
                long r[7];
                if (separate_group == 2) {
                    struct { unsigned long handler, flags, mask; } action = {
                        (unsigned long)orphan_handler, 4, 0};
                    unsigned long hup = 1;
                    abi_require(SC4(134, 1, &action, 0, 8) == 0);
                    abi_require(SC4(135, 1, &hup, 0, 8) == 0);
                    r[0] = orphan_handler_signal; r[1] = orphan_handler_code;
                } else {
                    r[0] = SC4(137, &mask, &info, &timeout, 8); r[1] = info.code;
                }
                r[2] = SC4(137, &mask, &info, &timeout, 8); r[3] = info.code;
                r[4] = SC1(155, 0) == (separate_group ? SC0(172) : group);
                r[5] = SC1(156, 0) == session;
                r[6] = SC0(172);
                send_data(report[1], r, sizeof(r));
                token_get(child_gate[0]); abi_exit(0);
            }
            int status;
            abi_require(SC4(260, child, &status, 2, 0) == child && (status & 255) == 127);
            send_data(parent_ready[1], &child, sizeof(child));
            token_get(bridge_gate[0]); abi_exit(0);
        }
        long child; receive(parent_ready[0], &child, sizeof(child));
        token_send(bridge_gate[1]); abi_require(wait_one(bridge) == 0);
        /* The group survives reaping its original leader; a new member can join. */
        long r[3]; r[0] = SC1(155, bridge);
        long joiner = fork_one();
        if (!joiner) { token_get(bridge_gate[0]); abi_exit(0); }
        r[1] = SC2(154, joiner, separate_group ? child : bridge);
        r[2] = SC1(156, joiner) == session;
        token_send(bridge_gate[1]); abi_require(wait_one(joiner) == 0);
        send_data(parent_ready[1], r, sizeof(r));
        abi_exit(0);
    }
    long signals[7], membership[3];
    receive(report[0], signals, sizeof(signals));
    receive(parent_ready[0], membership, sizeof(membership));
    const char *const names[3][9] = {
        {"session.orphan-exit-hup", "session.orphan-exit-hup-code", "session.orphan-exit-cont", "session.orphan-exit-cont-code", "session.orphan-exit-pgid", "session.orphan-exit-sid", "session.dead-group-leader", "session.join-surviving-group", "session.join-surviving-sid"},
        {"session.orphan-reparent-hup", "session.orphan-reparent-hup-code", "session.orphan-reparent-cont", "session.orphan-reparent-cont-code", "session.orphan-reparent-pgid", "session.orphan-reparent-sid", "session.dead-parent", "session.join-orphan-group", "session.join-orphan-sid"},
        {"session.orphan-handler-hup", "session.orphan-handler-hup-code", "session.orphan-handler-cont", "session.orphan-handler-cont-code", "session.orphan-handler-pgid", "session.orphan-handler-sid", "session.handler-dead-parent", "session.join-handler-group", "session.join-handler-sid"}
    };
    for (unsigned i = 0; i < 6; i++) record(names[separate_group][i], signals[i]);
    for (unsigned i = 0; i < 3; i++) record(names[separate_group][i + 6], membership[i]);
    abi_require(wait_one(coordinator) == 0);
    const char *const sid_names[3][2] = {
        {"session.sid-survives-leader", "session.sid-only-no-task"},
        {"session.reparent-sid-survives-leader", "session.reparent-sid-only-no-task"},
        {"session.handler-sid-survives-leader", "session.handler-sid-only-no-task"}
    };
    record(sid_names[separate_group][0], SC1(156, signals[6]) == coordinator);
    record(sid_names[separate_group][1], SC1(156, coordinator));
    token_send(child_gate[1]); abi_require(wait_one(signals[6]) == 0);
    close_one(report[0]); close_one(report[1]); close_one(parent_ready[0]); close_one(parent_ready[1]);
    close_one(bridge_gate[0]); close_one(bridge_gate[1]); close_one(child_gate[0]); close_one(child_gate[1]);
}

static int proc_state(long pid, long *group, long *session)
{
    char path[80], digits[24], data[512];
    const char prefix[] = "/session-proc/";
    unsigned at = 0;
    while (prefix[at]) { path[at] = prefix[at]; at++; }
    decimal(digits, pid);
    for (unsigned i = 0; digits[i]; i++) path[at++] = digits[i];
    const char suffix[] = "/stat";
    for (unsigned i = 0; i < sizeof(suffix); i++) path[at++] = suffix[i];
    long fd = SC4(56, -100, path, 0, 0);
    abi_require(fd >= 0);
    long n = SC3(63, fd, data, sizeof(data)); close_one((int)fd);
    abi_require(n > 0);
    long pos = 0;
    while (pos < n && data[pos] != ')') pos++;
    abi_require(pos + 4 < n);
    int state = data[pos + 2]; pos += 4;
    for (unsigned field = 0; field < 3; field++) {
        long value = 0;
        while (pos < n && data[pos] == ' ') pos++;
        while (pos < n && data[pos] >= '0' && data[pos] <= '9')
            value = value * 10 + data[pos++] - '0';
        if (field == 1) *group = value;
        if (field == 2) *session = value;
    }
    return state;
}

static void zombie_worker(int report)
{
    abi_require(SC3(34, -100, "/session-proc", 0755) == 0);
    abi_require(SC5(40, "proc", "/session-proc", "proc", 0, 0) == 0);
    long inherited = SC1(155, 0), session = SC1(156, 0);
    long child = fork_one();
    if (!child) abi_exit(0);
    long group = 0, sid = 0;
    while (proc_state(child, &group, &sid) != 'Z') SC0(124);
    long r[10];
    r[0] = SC1(155, child) == inherited;
    r[1] = SC1(156, child) == session;
    r[2] = SC2(154, child, 0);
    r[3] = proc_state(child, &group, &sid) == 'Z' && group == child && sid == session;
    r[4] = SC2(129, child, 0);
    r[5] = SC2(129, -child, 0);
    r[6] = SC2(129, child, 10);
    r[7] = SC2(129, -child, 10);
    int status;
    long result = SC4(260, -child, &status, 0, 0);
    r[8] = result == child && status == 0;
    r[9] = SC1(155, child);
    abi_require(SC2(39, "/session-proc", 0) == 0);
    abi_require(SC3(35, -100, "/session-proc", 512) == 0);
    send_data(report, r, sizeof(r)); abi_exit(0);
}

static void group_signal_worker(int report)
{
    abi_require(SC2(154, 0, 0) == 0);
    unsigned long mask = 1UL << 9;
    abi_require(SC4(135, 0, &mask, 0, 8) == 0);
    struct session_time zero = {0, 0}, timeout = {2, 0};
    int first[2], second[2], gate[2]; pipe_make(first); pipe_make(second); pipe_make(gate);
    long children[2];
    for (unsigned i = 0; i < 2; i++) {
        children[i] = fork_one();
        if (!children[i]) {
            int fd = i ? second[1] : first[1];
            if (i) abi_require(SC2(154, 0, 0) == 0);
            token_send(fd);
            if (i) {
                token_get(gate[0]);
                long pending = SC4(137, &mask, 0, &zero, 8);
                send_data(fd, &pending, sizeof(pending));
                token_get(gate[0]);
            }
            long got = SC4(137, &mask, 0, &timeout, 8);
            send_data(fd, &got, sizeof(got)); abi_exit(0);
        }
    }
    token_get(first[0]); token_get(second[0]);
    long r[10];
    r[0] = SC2(129, 0, 10);
    r[1] = SC4(137, &mask, 0, &zero, 8);
    receive(first[0], &r[2], sizeof(long));
    token_send(gate[1]); receive(second[0], &r[3], sizeof(long));
    int status;
    r[4] = SC4(260, 0, &status, 0, 0) == children[0] && status == 0;
    r[5] = SC4(260, 0, &status, 1, 0);
    r[6] = SC4(260, -children[1], &status, 1, 0);
    r[7] = SC2(129, -children[1], 10);
    token_send(gate[1]); receive(second[0], &r[8], sizeof(long));
    r[9] = SC4(260, -children[1], &status, 0, 0) == children[1] && status == 0;
    send_data(report, r, sizeof(r)); abi_exit(0);
}

static void retained_group_worker(int report)
{
    long inherited = SC1(155, 0), self = SC0(172), r[4];
    abi_require(SC2(154, 0, 0) == 0);
    int gate[2]; pipe_make(gate);
    long child = fork_one();
    if (!child) { token_get(gate[0]); abi_exit(0); }
    abi_require(SC2(154, 0, inherited) == 0);
    r[0] = SC1(155, child) == self;
    r[1] = SC0(157);
    token_send(gate[1]); r[2] = wait_one(child);
    long result = SC0(157); r[3] = result < 0 ? result : result == self;
    send_data(report, r, sizeof(r)); abi_exit(0);
}

void abi_session_cases(void)
{
    record("session.getpgid-missing", SC1(155, -1));
    record("session.getsid-missing", SC1(156, -1));
    record("session.setpgid-missing", SC2(154, -1, 0));
    static const char *const basic_names[] = {
        "session.inherit", "session.create-group", "session.group-self", "session.setsid-group-leader",
        "session.rejoin-parent", "session.create-session", "session.session-self", "session.session-leader-setpgid",
        "session.setsid-again", "session.parent-move-child", "session.child-group-session", "session.parent-join-child",
        "session.nonexistent-group", "session.negative-pgid", "session.negative-precedence", "session.child-status"
    };
    run_worker(basic_worker, basic_names, 16);
    static const char *const retained_names[] = {"session.retained-group-member",
        "session.setsid-retained-group", "session.retained-member-exit", "session.setsid-after-group-release"};
    run_worker(retained_group_worker, retained_names, 4);
    static const char *const zombie_names[] = {"session.zombie-getpgid", "session.zombie-getsid",
        "session.zombie-setpgid", "session.zombie-proc", "session.zombie-kill-probe",
        "session.zombie-group-probe", "session.zombie-kill", "session.zombie-group-kill",
        "session.wait-group", "session.reaped-getpgid"};
    run_worker(zombie_worker, zombie_names, 10);
    exec_cases(0); exec_cases(1);
    pipe_make(thread_gate);
    static const char *const boundary[] = {"session.non-child", "session.cross-session-child", "session.cross-session-query"};
    run_worker(session_boundary_worker, boundary, 3);
    close_one(thread_gate[0]); close_one(thread_gate[1]);
    static const char *const group_names[] = {"session.kill-current-group", "session.kill-current-self",
        "session.kill-current-member", "session.kill-current-excludes-other", "session.wait-current-group",
        "session.wait-current-empty", "session.wait-other-live", "session.kill-other-group",
        "session.kill-other-member", "session.wait-other-group"};
    run_worker(group_signal_worker, group_names, 10);
    orphan_case(0); orphan_case(1); orphan_case(2);
}
