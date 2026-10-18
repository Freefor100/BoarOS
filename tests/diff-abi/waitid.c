#include "abi.h"

#define P_ALL 0
#define P_PID 1
#define P_PGID 2
#define P_PIDFD 3

#define WNOHANG 0x00000001
#define WUNTRACED 0x00000002
#define WSTOPPED 0x00000002
#define WEXITED 0x00000004
#define WCONTINUED 0x00000008
#define WNOWAIT 0x01000000

#define __WNOTHREAD 0x20000000
#define __WALL 0x40000000
#define __WCLONE 0x80000000

#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGKILL 9

#define CLD_EXITED 1
#define CLD_KILLED 2
#define CLD_DUMPED 3
#define CLD_TRAPPED 4
#define CLD_STOPPED 5
#define CLD_CONTINUED 6

struct diff_siginfo {
    int si_signo;
    int si_errno;
    int si_code;
    int pad;
    int si_pid;
    unsigned int si_uid;
    int si_status;
    unsigned char tail[100];
};

struct diff_rusage {
    long ru_utime_sec, ru_utime_usec;
    long ru_stime_sec, ru_stime_usec;
    long ru_maxrss;
    long ru_ixrss;
    long ru_idrss;
    long ru_isrss;
    long ru_minflt;
    long ru_majflt;
    long ru_nswap;
    long ru_inblock;
    long ru_oublock;
    long ru_msgsnd;
    long ru_msgrcv;
    long ru_nsignals;
    long ru_nvcsw;
    long ru_nivcsw;
};

struct diff_sigaction {
    unsigned long handler;
    unsigned long flags;
    unsigned long mask;
};

extern long abi_clone_entry(long flags, void *stack, int *tid,
                            void (*fn)(void *), void *arg);

static void mem_set(void *dst, int val, usize n)
{
    unsigned char *d = dst;
    for (usize i = 0; i < n; i++) d[i] = (unsigned char)val;
}

static int mem_all(const void *src, int val, usize n)
{
    const unsigned char *s = src;
    for (usize i = 0; i < n; i++) {
        if (s[i] != (unsigned char)val) return 0;
    }
    return 1;
}

static void record_waitid(const char *name, long ret,
                          const struct diff_siginfo *info,
                          long expected_pid,
                          int expect_event)
{
    if (ret != 0) {
        abi_record(name, ret, -1, -1, 0, 0, 0);
        return;
    }
    int pad_ok = (info->pad == 0x5a5a5a5a);
    int tail_ok = mem_all(info->tail, 0x5a, sizeof(info->tail));
    int pid_ok = (info->si_pid == (expect_event ? (int)expected_pid : 0));
    abi_record(name, ret,
               info->si_signo,
               info->si_code,
               (pad_ok && tail_ok && pid_ok) ? 1 : 0,
               &info->si_status,
               sizeof(info->si_status));
}

static void close_fd(long fd)
{
    abi_require(SC1(57, fd) == 0);
}

static long child_held(int pipefd[2])
{
    abi_require(SC2(59, pipefd, 0) == 0);
    long child = CALL(220, 17, 0, 0, 0, 0, 0);
    abi_require(child >= 0);
    if (!child) {
        unsigned char byte = 0;
        abi_require(SC1(57, pipefd[1]) == 0);
        long n;
        do { n = SC3(63, pipefd[0], &byte, 1); } while (n == -4);
        abi_require(n == 1);
        abi_exit(byte);
    }
    abi_require(SC1(57, pipefd[0]) == 0);
    return child;
}

static void child_release(int pipefd[2], unsigned char exit_code)
{
    abi_require(SC3(64, pipefd[1], &exit_code, 1) == 1 && SC1(57, pipefd[1]) == 0);
}

struct thread_wait_arg {
    long child;
    volatile int ready;
    volatile int done;
    long result;
    struct diff_siginfo info;
};

static unsigned char helper_stack[16384] __attribute__((aligned(16)));

static void thread_wait_fn(void *arg)
{
    struct thread_wait_arg *t = arg;
    mem_set(&t->info, 0x5a, sizeof(t->info));
    t->ready = 1;
    t->result = SC5(95, P_PID, t->child, &t->info, WEXITED, 0);
    t->done = 1;
}

void abi_waitid_cases(void)
{
    struct diff_siginfo info;
    struct diff_rusage ru;
    long ret;

    /* 1. Invalid arguments */
    abi_record("waitid.invalid.which-negative", SC5(95, -1, 0, 0, WEXITED, 0), -1, -1, 0, 0, 0);
    abi_record("waitid.invalid.which-unknown", SC5(95, 4, 0, 0, WEXITED, 0), -1, -1, 0, 0, 0);
    abi_record("waitid.invalid.which-high", SC5(95, 99, 0, 0, WEXITED, 0), -1, -1, 0, 0, 0);
    abi_record("waitid.invalid.pid-zero", SC5(95, P_PID, 0, 0, WEXITED, 0), -1, -1, 0, 0, 0);
    abi_record("waitid.invalid.pid-negative", SC5(95, P_PID, -1, 0, WEXITED, 0), -1, -1, 0, 0, 0);
    abi_record("waitid.invalid.pgid-negative", SC5(95, P_PGID, -1, 0, WEXITED, 0), -1, -1, 0, 0, 0);
    abi_record("waitid.invalid.pidfd-negative", SC5(95, P_PIDFD, -1, 0, WEXITED, 0), -1, -1, 0, 0, 0);
    abi_record("waitid.invalid.pidfd-badfd", SC5(95, P_PIDFD, 99, 0, WEXITED, 0), -1, -1, 0, 0, 0);
    abi_record("waitid.invalid.options-zero", SC5(95, P_ALL, 0, 0, 0, 0), -1, -1, 0, 0, 0);
    abi_record("waitid.invalid.options-nohang-only", SC5(95, P_ALL, 0, 0, WNOHANG, 0), -1, -1, 0, 0, 0);
    abi_record("waitid.invalid.options-unknown", SC5(95, P_ALL, 0, 0, WEXITED | 0x80, 0), -1, -1, 0, 0, 0);
    abi_record("waitid.invalid.options-nowait-only", SC5(95, P_ALL, 0, 0, WNOWAIT, 0), -1, -1, 0, 0, 0);

    /* 2. No children */
    mem_set(&info, 0x5a, sizeof(info));
    mem_set(&ru, 0x5a, sizeof(ru));
    abi_record("waitid.empty.nohang-all", SC5(95, P_ALL, 0, &info, WEXITED | WNOHANG, &ru), -1, -1, 0, 0, 0);
    abi_record("waitid.empty.nohang-pid", SC5(95, P_PID, 9999, &info, WEXITED | WNOHANG, &ru), -1, -1, 0, 0, 0);
    abi_record("waitid.empty.nohang-pgid", SC5(95, P_PGID, 9999, &info, WEXITED | WNOHANG, &ru), -1, -1, 0, 0, 0);

    /* 3. Child running: WNOHANG checks */
    int pipefd[2];
    long child = child_held(pipefd);

    mem_set(&info, 0x5a, sizeof(info));
    mem_set(&ru, 0x5a, sizeof(ru));
    ret = SC5(95, P_ALL, 0, &info, WEXITED | WNOHANG, &ru);
    record_waitid("waitid.running.nohang-all", ret, &info, 0, 0);
    abi_record("waitid.running.nohang-ru-untouched", mem_all(&ru, 0x5a, sizeof(ru)), -1, -1, 0, 0, 0);

    mem_set(&info, 0x5a, sizeof(info));
    ret = SC5(95, P_PID, child, &info, WEXITED | WNOHANG, &ru);
    record_waitid("waitid.running.nohang-pid", ret, &info, 0, 0);

    mem_set(&info, 0x5a, sizeof(info));
    ret = SC5(95, P_PGID, 0, &info, WEXITED | WNOHANG, &ru);
    record_waitid("waitid.running.nohang-pgid-zero", ret, &info, 0, 0);

    mem_set(&info, 0x5a, sizeof(info));
    abi_record("waitid.running.nohang-mismatch-pid",
               SC5(95, P_PID, child + 999, &info, WEXITED | WNOHANG, &ru), -1, -1, 0, 0, 0);

    /* 4. Child stop & continue */
    abi_require(SC2(129, child, SIGSTOP) == 0);

    mem_set(&info, 0x5a, sizeof(info));
    mem_set(&ru, 0x5a, sizeof(ru));
    ret = SC5(95, P_PID, child, &info, WSTOPPED | WNOWAIT, &ru);
    record_waitid("waitid.stopped.nowait", ret, &info, child, 1);
    abi_record("waitid.stopped.nowait-ru-written", !mem_all(&ru, 0x5a, sizeof(ru)), -1, -1, 0, 0, 0);

    mem_set(&info, 0x5a, sizeof(info));
    ret = SC5(95, P_PID, child, &info, WSTOPPED, 0);
    record_waitid("waitid.stopped.consume", ret, &info, child, 1);

    mem_set(&info, 0x5a, sizeof(info));
    ret = SC5(95, P_PID, child, &info, WSTOPPED | WNOHANG, 0);
    record_waitid("waitid.stopped.after-consume", ret, &info, 0, 0);

    abi_require(SC2(129, child, SIGCONT) == 0);

    mem_set(&info, 0x5a, sizeof(info));
    mem_set(&ru, 0x5a, sizeof(ru));
    ret = SC5(95, P_PID, child, &info, WCONTINUED | WNOWAIT, &ru);
    record_waitid("waitid.continued.nowait", ret, &info, child, 1);
    abi_record("waitid.continued.nowait-ru-written", !mem_all(&ru, 0x5a, sizeof(ru)), -1, -1, 0, 0, 0);

    mem_set(&info, 0x5a, sizeof(info));
    ret = SC5(95, P_PID, child, &info, WCONTINUED, 0);
    record_waitid("waitid.continued.consume", ret, &info, child, 1);

    mem_set(&info, 0x5a, sizeof(info));
    ret = SC5(95, P_PID, child, &info, WCONTINUED | WNOHANG, 0);
    record_waitid("waitid.continued.after-consume", ret, &info, 0, 0);

    /* 5. Child exit: WNOWAIT, selection and consumption */
    child_release(pipefd, 37);

    mem_set(&info, 0x5a, sizeof(info));
    mem_set(&ru, 0x5a, sizeof(ru));
    ret = SC5(95, P_PID, child, &info, WEXITED | WNOWAIT, &ru);
    record_waitid("waitid.exit.nowait", ret, &info, child, 1);
    abi_record("waitid.exit.nowait-ru-written", !mem_all(&ru, 0x5a, sizeof(ru)), -1, -1, 0, 0, 0);

    /* Zombie exists, but WSTOPPED only should not match or reap zombie */
    mem_set(&info, 0x5a, sizeof(info));
    ret = SC5(95, P_PID, child, &info, WSTOPPED | WNOHANG, 0);
    record_waitid("waitid.exit.wstopped-only", ret, &info, 0, 0);

    /* Consume via wait4 */
    int status = -1;
    long wait4_rc = SC4(260, child, &status, 0, 0);
    abi_record("waitid.exit.consume-wait4", wait4_rc == child ? 0 : -1, -1, -1, status, 0, 0);

    mem_set(&info, 0x5a, sizeof(info));
    abi_record("waitid.exit.after-reap",
               SC5(95, P_PID, child, &info, WEXITED | WNOHANG, 0), -1, -1, 0, 0, 0);

    /* 6. Direct reap via waitid */
    long child2 = child_held(pipefd);
    child_release(pipefd, 42);

    mem_set(&info, 0x5a, sizeof(info));
    mem_set(&ru, 0x5a, sizeof(ru));
    ret = SC5(95, P_PID, child2, &info, WEXITED, &ru);
    record_waitid("waitid.exit.direct-reap", ret, &info, child2, 1);

    mem_set(&info, 0x5a, sizeof(info));
    abi_record("waitid.exit.direct-reap-after",
               SC5(95, P_PID, child2, &info, WEXITED | WNOHANG, 0), -1, -1, 0, 0, 0);

    /* 7. Child killed by signal */
    long child3 = child_held(pipefd);
    abi_require(SC2(129, child3, SIGKILL) == 0);
    close_fd(pipefd[1]);

    mem_set(&info, 0x5a, sizeof(info));
    ret = SC5(95, P_PID, child3, &info, WEXITED, 0);
    record_waitid("waitid.exit.killed", ret, &info, child3, 1);

    /* 8. NULL infop */
    long child4 = child_held(pipefd);
    child_release(pipefd, 43);
    abi_record("waitid.exit.null-infop", SC5(95, P_PID, child4, 0, WEXITED, 0), -1, -1, 0, 0, 0);
    mem_set(&info, 0x5a, sizeof(info));
    abi_record("waitid.exit.null-infop-after",
               SC5(95, P_PID, child4, &info, WEXITED | WNOHANG, 0), -1, -1, 0, 0, 0);

    /* 9. Fault handling and consumption order */
    /* Bad infop with normal wait: reaps event */
    long child5 = child_held(pipefd);
    child_release(pipefd, 44);
    abi_record("waitid.fault.infop-bad",
               SC5(95, P_PID, child5, (void *)-1, WEXITED, 0), -1, -1, 0, 0, 0);
    mem_set(&info, 0x5a, sizeof(info));
    abi_record("waitid.fault.infop-bad-after",
               SC5(95, P_PID, child5, &info, WEXITED | WNOHANG, 0), -1, -1, 0, 0, 0);

    /* Bad infop with WNOWAIT: does not reap event */
    long child6 = child_held(pipefd);
    child_release(pipefd, 45);
    abi_record("waitid.fault.infop-bad-nowait",
               SC5(95, P_PID, child6, (void *)-1, WEXITED | WNOWAIT, 0), -1, -1, 0, 0, 0);
    mem_set(&info, 0x5a, sizeof(info));
    ret = SC5(95, P_PID, child6, &info, WEXITED, 0);
    record_waitid("waitid.fault.infop-bad-nowait-after", ret, &info, child6, 1);

    /* Bad ru with normal wait: reaps event, infop untouched */
    long child7 = child_held(pipefd);
    child_release(pipefd, 46);
    mem_set(&info, 0x5a, sizeof(info));
    abi_record("waitid.fault.ru-bad",
               SC5(95, P_PID, child7, &info, WEXITED, (void *)-1), -1, -1, 0, 0, 0);
    abi_record("waitid.fault.ru-bad-infop-untouched",
               info.pad == 0x5a5a5a5a && mem_all(info.tail, 0x5a, sizeof(info.tail)), -1, -1, 0, 0, 0);
    mem_set(&info, 0x5a, sizeof(info));
    abi_record("waitid.fault.ru-bad-after",
               SC5(95, P_PID, child7, &info, WEXITED | WNOHANG, 0), -1, -1, 0, 0, 0);

    /* Bad infop with stopped child: stop event consumed */
    long child8 = child_held(pipefd);
    abi_require(SC2(129, child8, SIGSTOP) == 0);
    abi_record("waitid.fault.stopped-infop-bad",
               SC5(95, P_PID, child8, (void *)-1, WSTOPPED, 0), -1, -1, 0, 0, 0);
    mem_set(&info, 0x5a, sizeof(info));
    ret = SC5(95, P_PID, child8, &info, WSTOPPED | WNOHANG, 0);
    record_waitid("waitid.fault.stopped-infop-bad-after", ret, &info, 0, 0);

    /* wait4 with bad status pointer on stopped child: consumes stop event */
    abi_require(SC2(129, child8, SIGCONT) == 0);
    abi_require(SC4(260, child8, &status, WCONTINUED, 0) == child8);
    abi_require(SC2(129, child8, SIGSTOP) == 0);
    abi_record("wait4.fault.stopped-status-bad",
               SC4(260, child8, (int *)-1, WUNTRACED, 0), -1, -1, 0, 0, 0);
    abi_record("wait4.fault.stopped-status-bad-after",
               SC4(260, child8, &status, WUNTRACED | WNOHANG, 0), -1, -1, 0, 0, 0);

    abi_require(SC2(129, child8, SIGCONT) == 0);
    child_release(pipefd, 47);
    abi_require(SC4(260, child8, &status, 0, 0) == child8);

    /* 10. __WCLONE and __WALL */
    long child9 = child_held(pipefd);
    child_release(pipefd, 48);
    mem_set(&info, 0x5a, sizeof(info));
    abi_record("waitid.clone.wclone",
               SC5(95, P_PID, child9, &info, WEXITED | __WCLONE, 0), -1, -1, 0, 0, 0);
    mem_set(&info, 0x5a, sizeof(info));
    ret = SC5(95, P_PID, child9, &info, WEXITED | __WCLONE | __WALL, 0);
    record_waitid("waitid.clone.wall", ret, &info, child9, 1);

    /* 11. SA_NOCLDWAIT autoreap */
    struct diff_sigaction sa = {0}, old_sa = {0};
    sa.flags = 2; /* SA_NOCLDWAIT */
    abi_require(SC4(134, SIGCHLD, &sa, &old_sa, 8) == 0);
    long child10 = child_held(pipefd);
    child_release(pipefd, 49);
    /* Small pause to allow autoreap */
    for (int i = 0; i < 50; i++) SC0(124);
    mem_set(&info, 0x5a, sizeof(info));
    abi_record("waitid.cldwait.nocldwait",
               SC5(95, P_PID, child10, &info, WEXITED, 0), -1, -1, 0, 0, 0);
    abi_require(SC4(134, SIGCHLD, &old_sa, 0, 8) == 0);

    /* 12. Multithread WNOWAIT competition */
    long child11 = child_held(pipefd);
    child_release(pipefd, 55);

    struct thread_wait_arg targ = {
        .child = child11,
        .ready = 0,
        .done = 0,
        .result = -1,
    };
    int helper_tid = 0;
    long tid = abi_clone_entry(0x10f00, helper_stack + sizeof(helper_stack),
                               &helper_tid, thread_wait_fn, &targ);
    abi_require(tid > 0);

    /* Wait for helper thread to start waitid */
    while (!targ.ready) SC0(124);

    /* Main thread does waitid with WNOWAIT */
    struct diff_siginfo info_nowait;
    mem_set(&info_nowait, 0x5a, sizeof(info_nowait));
    long ret_nowait = SC5(95, P_PID, child11, &info_nowait, WEXITED | WNOWAIT, 0);

    /* Wait for helper thread to finish its reaping waitid */
    while (!targ.done) SC0(124);

    abi_record("waitid.multithread.nowait-compete",
               (ret_nowait == 0 && targ.result == 0) ? 0 : -1,
               -1, -1,
               targ.info.si_status, 0, 0);

    /* Wait for helper thread to exit */
    while (SC2(130, tid, 0) == 0) SC0(124);
}
