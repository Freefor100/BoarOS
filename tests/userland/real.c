/* The first compiler-generated BoarOS user program: a static musl
 * binary exercising stdio, directory enumeration, regular-file reads,
 * descriptor duplication, and the clock ABI against the Linux surface. */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

__attribute__((section(".rodata.unlink_test_far"), aligned(4096)))
const char unlink_far_page[8192] = "UNLINK_DEMAND_FAULT_PAGE_PAYLOAD";

/* Ten live double accumulators force the loop to hold FP state in the
 * registers across preemptions; every sum is an exact integer below
 * 2^53, so a switch that fails to save or restore FP state corrupts a
 * total instead of merely perturbing it. */
static int fp_worker(int slot)
{
    const unsigned long iterations = 2000000UL;
    const unsigned long long triangle =
        (unsigned long long)iterations * (iterations + 1) / 2;
    double a0 = 0.0, a1 = 0.0, a2 = 0.0, a3 = 0.0, a4 = 0.0;
    double a5 = 0.0, a6 = 0.0, a7 = 0.0, a8 = 0.0, a9 = 0.0;

    for (unsigned long i = 1; i <= iterations; i++) {
        double k = (double)i;
        a0 += k * 1.0;
        a1 += k * 2.0;
        a2 += k * 3.0;
        a3 += k * 4.0;
        a4 += k * 5.0;
        a5 += k * 6.0;
        a6 += k * 7.0;
        a7 += k * 8.0;
        a8 += k * 9.0;
        a9 += k * 10.0;
    }
    if ((unsigned long long)a0 != triangle ||
        (unsigned long long)a1 != 2 * triangle ||
        (unsigned long long)a2 != 3 * triangle ||
        (unsigned long long)a3 != 4 * triangle ||
        (unsigned long long)a4 != 5 * triangle ||
        (unsigned long long)a5 != 6 * triangle ||
        (unsigned long long)a6 != 7 * triangle ||
        (unsigned long long)a7 != 8 * triangle ||
        (unsigned long long)a8 != 9 * triangle ||
        (unsigned long long)a9 != 10 * triangle) {
        return slot * 100 + 1;
    }
    return 0;
}

static volatile sig_atomic_t user_signal_seen;
static volatile uintptr_t context_sp;
static volatile sig_atomic_t vfork_done;
static unsigned char vfork_stack[65536] __attribute__((aligned(16)));

_Static_assert(offsetof(ucontext_t, uc_mcontext) == 176U,
               "musl RISC-V ucontext alignment");
_Static_assert(sizeof(ucontext_t) == 960U, "musl RISC-V ucontext size");

static void context_handler(int sig, siginfo_t *info, void *context)
{
    ucontext_t *uc = context;

    if (sig == SIGUSR1 && info->si_signo == sig) {
        context_sp = uc->uc_mcontext.__gregs[REG_SP];
    }
}

static int vfork_worker(void *argument)
{
    struct timespec delay = {0, 20000000L};
    char *args[] = {"/missing-executable", 0};
    char *env[] = {0};

    /* Retiring a failed exec transaction must not complete vfork while
     * the child still uses the parent's MM. */
    if (execve(args[0], args, env) != -1 || errno != ENOENT) {
        return 2;
    }

    if (kill((pid_t)(uintptr_t)argument, SIGUSR1) != 0 ||
        nanosleep(&delay, 0) != 0) {
        return 1;
    }
    vfork_done = 1;
    return 0;
}

/* Keep registers live across the exact syscall boundary; libc fork may
 * itself use caller-saved FP registers before reaching ecall. */
static int check_fork_fp(void)
{
    register long a0 __asm__("a0") = SIGCHLD;
    register long a1 __asm__("a1") = 0;
    register long a7 __asm__("a7") = SYS_clone;
    uint64_t fp;
    unsigned long fcsr;
    unsigned long old_fcsr;
    const uint64_t sentinel = UINT64_C(0x3ff123456789abcd);

    __asm__ volatile("csrr %0, fcsr" : "=r"(old_fcsr));
    __asm__ volatile("fmv.d.x fs0, %4\n\tcsrwi fcsr, 1\n\tecall\n\t"
                     "fmv.x.d %0, fs0\n\tcsrr %1, fcsr"
                     : "=r"(fp), "=r"(fcsr), "+r"(a0), "+r"(a1)
                     : "r"(sentinel), "r"(a7)
                     : "fs0", "memory");
    __asm__ volatile("csrw fcsr, %0" : : "r"(old_fcsr));
    if (a0 == 0) {
        _exit(fp != sentinel || fcsr != 1U);
    }
    int status;
    return a0 < 0 || fp != sentinel || fcsr != 1U ||
           waitpid((pid_t)a0, &status, 0) != a0 ||
           !WIFEXITED(status) || WEXITSTATUS(status) != 0;
}

static void user_signal_handler(int signal_number)
{
    user_signal_seen = signal_number;
}

static void signal_parent_after_delay(pid_t parent, int signal_number)
{
    const struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000L };

    if (nanosleep(&delay, 0) != 0 || kill(parent, signal_number) != 0) {
        _exit(90);
    }
    _exit(0);
}

static int check_signal_context_and_lifecycle(void)
{
    struct sigaction action = {0};
    struct sigaction old_action;
    uintptr_t sp;
    int status;

    action.sa_sigaction = context_handler;
    action.sa_flags = SA_SIGINFO;
    if (sigaction(SIGUSR1, &action, &old_action) != 0) {
        return 1;
    }
    /* libc syscall does not move sp on this target; use raw ecall so the
     * compared value is exactly the interrupted register snapshot. */
    register long a0 __asm__("a0") = getpid();
    register long a1 __asm__("a1") = SIGUSR1;
    register long a7 __asm__("a7") = SYS_kill;
    __asm__ volatile("mv %0, sp\n\tecall"
                     : "=&r"(sp), "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    if (a0 != 0 || context_sp != sp ||
        sigaction(SIGUSR1, &old_action, 0) != 0) {
        return 2;
    }
    pid_t child = clone(vfork_worker, vfork_stack + sizeof(vfork_stack),
                        CLONE_VM | CLONE_VFORK | SIGCHLD,
                        (void *)(uintptr_t)getpid());
    if (child <= 0 || !vfork_done ||
        waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return 3;
    }
    for (int mode = 0; mode < 2; mode++) {
        action.sa_handler = mode == 0 ? SIG_IGN : user_signal_handler;
        action.sa_flags = mode == 0 ? 0 : SA_NOCLDWAIT | SA_RESTART;
        if (sigaction(SIGCHLD, &action, 0) != 0) {
            return 4;
        }
        child = fork();
        if (child < 0) {
            return 5;
        }
        if (child == 0) {
            _exit(12);
        }
        /* wait4 must wake and report ECHILD, never a zombie status. */
        if (waitpid(child, &status, 0) != -1 || errno != ECHILD) {
            return 6;
        }
    }
    action.sa_handler = SIG_DFL;
    action.sa_flags = 0;
    return sigaction(SIGCHLD, &action, 0) != 0;
}

static int check_pipe_waiters(void)
{
    static char fill[4096];

    for (int mode = 0; mode < 3; mode++) {
        int pipe_fd[2];
        int ready[2];
        pid_t children[2];
        char byte;

        if (pipe(pipe_fd) != 0 || pipe(ready) != 0) {
            return 1;
        }
        if (mode == 2) {
            for (int chunk = 0; chunk < 16; chunk++) {
                if (write(pipe_fd[1], fill, sizeof(fill)) != sizeof(fill)) {
                    return 6;
                }
            }
        }
        for (int index = 0; index < 2; index++) {
            children[index] = fork();
            if (children[index] < 0) {
                return 2;
            }
            if (children[index] == 0) {
                close(pipe_fd[mode == 2 ? 0 : 1]);
                close(ready[0]);
                if (write(ready[1], "r", 1) != 1) {
                    _exit(1);
                }
                if (mode == 2) {
                    if (write(pipe_fd[1], "x", 1) != -1 || errno != EPIPE) {
                        _exit(2);
                    }
                } else if (read(pipe_fd[0], &byte, 1) != mode) {
                    _exit(3);
                }
                _exit(0);
            }
        }
        close(ready[1]);
        for (int index = 0; index < 2; index++) {
            if (read(ready[0], &byte, 1) != 1) {
                return 3;
            }
        }
        close(ready[0]);
        if (mode == 1 && write(pipe_fd[1], "ab", 2) != 2) {
            return 4;
        }
        /* Keep the writer open in the data case: a missing progress wake
         * must not accidentally be repaired by the EOF wake. */
        if (mode != 1) {
            close(pipe_fd[mode == 2 ? 0 : 1]);
        }
        for (int index = 0; index < 2; index++) {
            int status;

            if (waitpid(children[index], &status, 0) != children[index] ||
                !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                return 5;
            }
        }
        if (mode != 2) {
            close(pipe_fd[0]);
        }
        if (mode != 0) {
            close(pipe_fd[1]);
        }
    }
    return 0;
}

static int check_poll_and_select(void)
{
    /* 1. Pipe poll: immediate nonblocking check on empty pipe and writable pipe */
    int p[2];
    if (pipe(p) != 0) {
        return 1;
    }
    struct pollfd pfds[2];
    pfds[0].fd = p[0];
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;
    pfds[1].fd = p[1];
    pfds[1].events = POLLOUT;
    pfds[1].revents = 0;

    int ret = poll(pfds, 2, 0);
    if (ret != 1 || pfds[0].revents != 0 || pfds[1].revents != POLLOUT) {
        return 2;
    }

    /* Write data: pipe reader becomes ready */
    if (write(p[1], "test", 4) != 4) {
        return 3;
    }
    ret = poll(pfds, 2, 0);
    if (ret != 2 || (pfds[0].revents & POLLIN) == 0 || (pfds[1].revents & POLLOUT) == 0) {
        return 4;
    }

    /* Drain pipe */
    char buf[16];
    if (read(p[0], buf, 4) != 4) {
        return 5;
    }

    /* Close writer: reader sees POLLHUP */
    if (close(p[1]) != 0) {
        return 6;
    }
    ret = poll(pfds, 1, 0);
    if (ret != 1 || (pfds[0].revents & POLLHUP) == 0) {
        return 7;
    }
    if (close(p[0]) != 0) {
        return 8;
    }

    /* 2. Negative fd and unallocated fd behavior */
    struct pollfd edge_fds[3];
    edge_fds[0].fd = -1;
    edge_fds[0].events = POLLIN;
    edge_fds[0].revents = 0x5a;
    edge_fds[1].fd = 200; /* unallocated fd */
    edge_fds[1].events = POLLIN;
    edge_fds[1].revents = 0;
    edge_fds[2].fd = -5;
    edge_fds[2].events = POLLOUT;
    edge_fds[2].revents = 0xa5;

    ret = poll(edge_fds, 3, 0);
    if (ret != 1 || edge_fds[0].revents != 0 || edge_fds[2].revents != 0 ||
        edge_fds[1].revents != POLLNVAL) {
        return 9;
    }

    /* 3. Timeout behavior: poll on empty pipe with 30ms timeout */
    if (pipe(p) != 0) {
        return 10;
    }
    pfds[0].fd = p[0];
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;

    struct timespec ts0, ts1;
    if (clock_gettime(CLOCK_MONOTONIC, &ts0) != 0) {
        return 11;
    }
    ret = poll(pfds, 1, 30);
    if (clock_gettime(CLOCK_MONOTONIC, &ts1) != 0) {
        return 12;
    }
    if (ret != 0 || pfds[0].revents != 0) {
        return 13;
    }
    long elapsed_ms = (ts1.tv_sec - ts0.tv_sec) * 1000 +
                      (ts1.tv_nsec - ts0.tv_nsec) / 1000000;
    if (elapsed_ms < 15) {
        return 14;
    }

    /* 4. Child wakes parent poll via pipe write */
    pid_t child = fork();
    if (child < 0) {
        return 15;
    }
    if (child == 0) {
        close(p[0]);
        struct timespec delay = { .tv_sec = 0, .tv_nsec = 20000000L };
        if (nanosleep(&delay, 0) != 0 || write(p[1], "w", 1) != 1) {
            _exit(1);
        }
        _exit(0);
    }
    close(p[1]);
    pfds[0].fd = p[0];
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;
    ret = poll(pfds, 1, 1000);
    if (ret != 1 || (pfds[0].revents & POLLIN) == 0) {
        return 16;
    }
    if (read(p[0], buf, 1) != 1) {
        return 17;
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        return 18;
    }
    close(p[0]);

    /* 5. Select & pselect basic, timeout, and EBADF */
    if (pipe(p) != 0) {
        return 19;
    }
    fd_set rfds, wfds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_SET(p[0], &rfds);
    FD_SET(p[1], &wfds);
    struct timeval tv = {0, 0};

    ret = select(p[1] + 1, &rfds, &wfds, NULL, &tv);
    if (ret != 1 || FD_ISSET(p[0], &rfds) || !FD_ISSET(p[1], &wfds)) {
        return 20;
    }

    if (write(p[1], "x", 1) != 1) {
        return 21;
    }
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_SET(p[0], &rfds);
    FD_SET(p[1], &wfds);
    ret = select(p[1] + 1, &rfds, &wfds, NULL, &tv);
    if (ret != 2 || !FD_ISSET(p[0], &rfds) || !FD_ISSET(p[1], &wfds)) {
        return 22;
    }

    /* Invalid fd triggers EBADF in select */
    FD_ZERO(&rfds);
    FD_SET(200, &rfds);
    errno = 0;
    ret = select(201, &rfds, NULL, NULL, &tv);
    if (ret != -1 || errno != EBADF) {
        return 23;
    }

    /* pselect timeout */
    struct timespec pts = { .tv_sec = 0, .tv_nsec = 10000000L };
    ret = pselect(0, NULL, NULL, NULL, &pts, NULL);
    if (ret != 0) {
        return 24;
    }
    close(p[0]);
    close(p[1]);

    /* 6. ppoll with temporary signal mask & EINTR */
    struct sigaction sa = {0};
    sa.sa_handler = user_signal_handler;
    if (sigaction(SIGUSR1, &sa, NULL) != 0) {
        return 25;
    }
    sigset_t block_mask, orig_mask, ppoll_mask;
    sigemptyset(&block_mask);
    sigaddset(&block_mask, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &block_mask, &orig_mask) != 0) {
        return 26;
    }
    sigemptyset(&ppoll_mask); /* SIGUSR1 unblocked during ppoll */

    if (pipe(p) != 0) {
        return 27;
    }
    child = fork();
    if (child < 0) {
        return 28;
    }
    if (child == 0) {
        signal_parent_after_delay(getppid(), SIGUSR1);
    }
    pfds[0].fd = p[0];
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;
    struct timespec poll_timeout = { .tv_sec = 2, .tv_nsec = 0 };
    user_signal_seen = 0;
    errno = 0;
    ret = ppoll(pfds, 1, &poll_timeout, &ppoll_mask);
    if (ret != -1 || errno != EINTR || user_signal_seen != SIGUSR1) {
        return 29;
    }

    /* Verify that SIGUSR1 is restored to blocked state */
    sigset_t current_mask;
    sigemptyset(&current_mask);
    if (sigprocmask(SIG_SETMASK, NULL, &current_mask) != 0 ||
        !sigismember(&current_mask, SIGUSR1)) {
        return 30;
    }
    /* Restore original mask */
    if (sigprocmask(SIG_SETMASK, &orig_mask, NULL) != 0) {
        return 31;
    }
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        return 32;
    }
    close(p[0]);
    close(p[1]);

    /* 7. Console stdout polling */
    struct pollfd pfd_stdout;
    pfd_stdout.fd = 1;
    pfd_stdout.events = POLLOUT;
    pfd_stdout.revents = 0;
    ret = poll(&pfd_stdout, 1, 0);
    if (ret != 1 || (pfd_stdout.revents & POLLOUT) == 0) {
        return 33;
    }

    /* 8. Regular file polling */
    int file_fd = open("/data", O_RDONLY);
    if (file_fd < 0) {
        return 34;
    }
    struct pollfd pfd_file;
    pfd_file.fd = file_fd;
    pfd_file.events = POLLIN;
    pfd_file.revents = 0;
    ret = poll(&pfd_file, 1, 0);
    close(file_fd);
    if (ret != 1 || (pfd_file.revents & POLLIN) == 0) {
        return 35;
    }

    /* 9. Broken pipe write-end readiness */
    int bp[2];
    if (pipe(bp) != 0) {
        return 101;
    }
    if (close(bp[0]) != 0) {
        return 102;
    }
    struct pollfd bp_pfd = { .fd = bp[1], .events = POLLOUT, .revents = 0 };
    ret = poll(&bp_pfd, 1, 0);
    if (ret != 1 || (bp_pfd.revents & POLLOUT) == 0 || (bp_pfd.revents & POLLERR) == 0) {
        return 103;
    }
    fd_set bp_wfds;
    FD_ZERO(&bp_wfds);
    FD_SET(bp[1], &bp_wfds);
    struct timeval bp_tv = {0, 0};
    ret = select(bp[1] + 1, NULL, &bp_wfds, NULL, &bp_tv);
    if (ret != 1 || !FD_ISSET(bp[1], &bp_wfds)) {
        return 104;
    }
    int bp_ep = epoll_create1(0);
    if (bp_ep < 0) {
        return 105;
    }
    struct epoll_event bp_ev = { .events = EPOLLOUT, .data.u32 = 77 };
    if (epoll_ctl(bp_ep, EPOLL_CTL_ADD, bp[1], &bp_ev) != 0) {
        return 106;
    }
    struct epoll_event bp_out;
    ret = epoll_wait(bp_ep, &bp_out, 1, 0);
    if (ret != 1 || (bp_out.events & EPOLLOUT) == 0 || (bp_out.events & EPOLLERR) == 0) {
        return 107;
    }
    close(bp_ep);
    /* Write to broken pipe must fail with EPIPE and raise SIGPIPE (SIGPIPE is ignored in real.c) */
    errno = 0;
    if (write(bp[1], "z", 1) != -1 || errno != EPIPE) {
        return 108;
    }
    close(bp[1]);

    /* 10. Raw ppoll & pselect6 timeout writeback ABI */
    struct timespec raw_ts = { .tv_sec = 0, .tv_nsec = 30000000L };
    long raw_ret = syscall(SYS_ppoll, NULL, 0, &raw_ts, NULL, 0);
    if (raw_ret != 0 || raw_ts.tv_sec != 0 || raw_ts.tv_nsec != 0) {
        return 109;
    }
    struct {
        void *ss;
        size_t ss_len;
    } raw_sigpack = { NULL, 0 };
    raw_ts.tv_sec = 0;
    raw_ts.tv_nsec = 30000000L;
    raw_ret = syscall(SYS_pselect6, 0, NULL, NULL, NULL, &raw_ts, &raw_sigpack);
    if (raw_ret != 0 || raw_ts.tv_sec != 0 || raw_ts.tv_nsec != 0) {
        return 110;
    }
    int ready_p[2];
    if (pipe(ready_p) != 0) {
        return 111;
    }
    if (write(ready_p[1], "q", 1) != 1) {
        return 112;
    }
    struct pollfd ready_pfd = { .fd = ready_p[0], .events = POLLIN, .revents = 0 };
    raw_ts.tv_sec = 5;
    raw_ts.tv_nsec = 0;
    raw_ret = syscall(SYS_ppoll, &ready_pfd, 1, &raw_ts, NULL, 0);
    if (raw_ret != 1 || raw_ts.tv_sec < 4) {
        return 113;
    }
    fd_set ready_rfds;
    FD_ZERO(&ready_rfds);
    FD_SET(ready_p[0], &ready_rfds);
    raw_ts.tv_sec = 5;
    raw_ts.tv_nsec = 0;
    raw_ret = syscall(SYS_pselect6, ready_p[0] + 1, &ready_rfds, NULL, NULL, &raw_ts, &raw_sigpack);
    if (raw_ret != 1 || raw_ts.tv_sec < 4) {
        return 114;
    }
    close(ready_p[0]);
    close(ready_p[1]);

    /* Interrupted raw SYS_ppoll and SYS_pselect6 timeout writeback */
    sigset_t poll_block_mask, poll_orig_mask, poll_wait_mask;
    sigemptyset(&poll_block_mask);
    sigaddset(&poll_block_mask, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &poll_block_mask, &poll_orig_mask) != 0) {
        return 116;
    }
    sigemptyset(&poll_wait_mask);

    int intr_p[2];
    if (pipe(intr_p) != 0) {
        return 117;
    }
    pid_t intr_child = fork();
    if (intr_child < 0) {
        return 118;
    }
    if (intr_child == 0) {
        close(intr_p[0]);
        close(intr_p[1]);
        signal_parent_after_delay(getppid(), SIGUSR1);
    }
    struct pollfd intr_pfd = { .fd = intr_p[0], .events = POLLIN, .revents = 0 };
    raw_ts.tv_sec = 5;
    raw_ts.tv_nsec = 0;
    user_signal_seen = 0;
    errno = 0;
    raw_ret = syscall(SYS_ppoll, &intr_pfd, 1, &raw_ts, &poll_wait_mask, sizeof(uint64_t));
    int intr_wait_status;
    waitpid(intr_child, &intr_wait_status, 0);
    close(intr_p[0]);
    close(intr_p[1]);
    if (raw_ret != -1 || errno != EINTR || user_signal_seen != SIGUSR1) {
        return 119;
    }
    if (raw_ts.tv_sec < 3 || raw_ts.tv_sec > 5 ||
        (raw_ts.tv_sec == 5 && raw_ts.tv_nsec != 0)) {
        return 120;
    }

    if (pipe(intr_p) != 0) {
        return 121;
    }
    intr_child = fork();
    if (intr_child < 0) {
        return 122;
    }
    if (intr_child == 0) {
        close(intr_p[0]);
        close(intr_p[1]);
        signal_parent_after_delay(getppid(), SIGUSR1);
    }
    fd_set intr_rfds;
    FD_ZERO(&intr_rfds);
    FD_SET(intr_p[0], &intr_rfds);
    raw_ts.tv_sec = 5;
    raw_ts.tv_nsec = 0;
    user_signal_seen = 0;
    errno = 0;
    struct {
        const sigset_t *ss;
        size_t ss_len;
    } intr_sigpack = { &poll_wait_mask, sizeof(uint64_t) };
    raw_ret = syscall(SYS_pselect6, intr_p[0] + 1, &intr_rfds, NULL, NULL, &raw_ts, &intr_sigpack);
    waitpid(intr_child, &intr_wait_status, 0);
    close(intr_p[0]);
    close(intr_p[1]);
    if (raw_ret != -1 || errno != EINTR || user_signal_seen != SIGUSR1) {
        return 123;
    }
    if (raw_ts.tv_sec < 3 || raw_ts.tv_sec > 5 ||
        (raw_ts.tv_sec == 5 && raw_ts.tv_nsec != 0)) {
        return 124;
    }

    /* Interrupted raw SYS_ppoll with NULL timeout */
    if (pipe(intr_p) != 0) {
        return 125;
    }
    intr_child = fork();
    if (intr_child < 0) {
        return 126;
    }
    if (intr_child == 0) {
        close(intr_p[0]);
        close(intr_p[1]);
        signal_parent_after_delay(getppid(), SIGUSR1);
    }
    intr_pfd.fd = intr_p[0];
    intr_pfd.events = POLLIN;
    intr_pfd.revents = 0;
    user_signal_seen = 0;
    errno = 0;
    raw_ret = syscall(SYS_ppoll, &intr_pfd, 1, NULL, &poll_wait_mask, sizeof(uint64_t));
    waitpid(intr_child, &intr_wait_status, 0);
    close(intr_p[0]);
    close(intr_p[1]);
    if (raw_ret != -1 || errno != EINTR || user_signal_seen != SIGUSR1) {
        return 127;
    }

    /* Boundary and invalid timeout checks */
    raw_ts.tv_sec = -1;
    raw_ts.tv_nsec = 0;
    errno = 0;
    raw_ret = syscall(SYS_ppoll, NULL, 0, &raw_ts, NULL, 0);
    if (raw_ret != -1 || errno != EINVAL) {
        return 128;
    }
    raw_ts.tv_sec = 0;
    raw_ts.tv_nsec = 1000000000L;
    errno = 0;
    raw_ret = syscall(SYS_ppoll, NULL, 0, &raw_ts, NULL, 0);
    if (raw_ret != -1 || errno != EINVAL) {
        return 129;
    }
    raw_ts.tv_sec = 0;
    raw_ts.tv_nsec = -1L;
    errno = 0;
    raw_ret = syscall(SYS_ppoll, NULL, 0, &raw_ts, NULL, 0);
    if (raw_ret != -1 || errno != EINVAL) {
        return 130;
    }
    raw_ts.tv_sec = 0;
    raw_ts.tv_nsec = 0;
    errno = 0;
    raw_ret = syscall(SYS_ppoll, NULL, 0, &raw_ts, NULL, 0);
    if (raw_ret != 0 || raw_ts.tv_sec != 0 || raw_ts.tv_nsec != 0) {
        return 131;
    }

    if (sigprocmask(SIG_SETMASK, &poll_orig_mask, NULL) != 0) {
        return 132;
    }

    /* 11. select total_bits with single fd in both readfds and writefds */
    int rw_fd = open("/test_total_bits.tmp", O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (rw_fd >= 0) {
        fd_set both_rfds, both_wfds;
        FD_ZERO(&both_rfds);
        FD_ZERO(&both_wfds);
        FD_SET(rw_fd, &both_rfds);
        FD_SET(rw_fd, &both_wfds);
        struct timeval both_tv = {0, 0};
        ret = select(rw_fd + 1, &both_rfds, &both_wfds, NULL, &both_tv);
        unlink("/test_total_bits.tmp");
        close(rw_fd);
        if (ret != 2 || !FD_ISSET(rw_fd, &both_rfds) || !FD_ISSET(rw_fd, &both_wfds)) {
            return 115;
        }
    }

    static const char poll_marker[] =
        "BoarOS: real userland poll/select checks ok\n";
    if (write(1, poll_marker, sizeof(poll_marker) - 1) !=
        (ssize_t)(sizeof(poll_marker) - 1)) {
        return 36;
    }

    return 0;
}

static int check_epoll(void)
{
    /* 1. epoll_create1 and flags */
    int epfd = epoll_create1(0);
    if (epfd < 0) {
        return 1;
    }
    int epfd_cloexec = epoll_create1(EPOLL_CLOEXEC);
    if (epfd_cloexec < 0) {
        close(epfd);
        return 2;
    }
    close(epfd_cloexec);

    errno = 0;
    int epfd_bad = epoll_create1(0x1234);
    if (epfd_bad != -1 || errno != EINVAL) {
        close(epfd);
        return 3;
    }

    /* 2. epoll_ctl error handling */
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.u64 = 0x1111ULL;

    errno = 0;
    if (epoll_ctl(-1, EPOLL_CTL_ADD, 0, &ev) != -1 || errno != EBADF) {
        close(epfd);
        return 4;
    }
    errno = 0;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, -1, &ev) != -1 || errno != EBADF) {
        close(epfd);
        return 5;
    }
    errno = 0;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, epfd, &ev) != -1 || errno != EINVAL) {
        close(epfd);
        return 6;
    }
    errno = 0;
    if (epoll_ctl(epfd, 999, 0, &ev) != -1 || errno != EINVAL) {
        close(epfd);
        return 7;
    }
    errno = 0;
    if (epoll_ctl(epfd, EPOLL_CTL_DEL, 0, NULL) != -1 || errno != ENOENT) {
        close(epfd);
        return 8;
    }

    /* O_NONBLOCK is a preserved status flag for regular files and
     * directories; it does not turn ordinary file reads into EAGAIN. */
    int reg_fd = open("/data", O_RDONLY | O_NONBLOCK);
    char reg_byte;
    if (reg_fd < 0) {
        close(epfd);
        return 803;
    }
    if ((fcntl(reg_fd, F_GETFL) & O_NONBLOCK) == 0 ||
        read(reg_fd, &reg_byte, 1) != 1 ||
        fcntl(reg_fd, F_SETFL, 0) != 0 ||
        (fcntl(reg_fd, F_GETFL) & O_NONBLOCK) != 0 ||
        fcntl(reg_fd, F_SETFL, O_NONBLOCK) != 0 ||
        (fcntl(reg_fd, F_GETFL) & O_NONBLOCK) == 0) {
        close(reg_fd);
        close(epfd);
        return 803;
    }
    errno = 0;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, reg_fd, &ev) != -1 || errno != EPERM) {
        close(reg_fd);
        close(epfd);
        return 801;
    }
    close(reg_fd);
    int dir_fd = open("/", O_RDONLY | O_DIRECTORY | O_NONBLOCK);
    if (dir_fd < 0) {
        close(epfd);
        return 804;
    }
    if ((fcntl(dir_fd, F_GETFL) & O_NONBLOCK) == 0) {
        close(dir_fd);
        close(epfd);
        return 804;
    }
    errno = 0;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, dir_fd, &ev) != -1 || errno != EPERM) {
        close(dir_fd);
        close(epfd);
        return 802;
    }
    close(dir_fd);

    /* 3. Empty epoll wait */
    struct epoll_event evs[4];
    memset(evs, 0, sizeof(evs));
    if (epoll_wait(epfd, evs, 4, 0) != 0) {
        close(epfd);
        return 9;
    }
    if (epoll_wait(epfd, evs, 4, 20) != 0) {
        close(epfd);
        return 10;
    }
    errno = 0;
    if (epoll_wait(epfd, evs, 0, 0) != -1 || errno != EINVAL) {
        close(epfd);
        return 11;
    }
    errno = 0;
    if (epoll_wait(epfd, NULL, 4, 0) != -1 || errno != EFAULT) {
        close(epfd);
        return 12;
    }

    /* 4. Pipe read readiness & Level Triggered (LT) mode */
    int p[2];
    if (pipe(p) != 0) {
        close(epfd);
        return 13;
    }
    ev.events = EPOLLIN;
    ev.data.u64 = 0x12345678ULL;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, p[0], &ev) != 0) {
        close(p[0]);
        close(p[1]);
        close(epfd);
        return 14;
    }
    errno = 0;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, p[0], &ev) != -1 || errno != EEXIST) {
        close(p[0]);
        close(p[1]);
        close(epfd);
        return 15;
    }

    if (epoll_wait(epfd, evs, 4, 0) != 0) {
        close(p[0]);
        close(p[1]);
        close(epfd);
        return 16;
    }

    if (write(p[1], "abcd", 4) != 4) {
        close(p[0]);
        close(p[1]);
        close(epfd);
        return 17;
    }

    int n = epoll_wait(epfd, evs, 4, 100);
    if (n != 1 || !(evs[0].events & EPOLLIN) || evs[0].data.u64 != 0x12345678ULL) {
        close(p[0]);
        close(p[1]);
        close(epfd);
        return 18;
    }

    /* LT test: data remains in pipe, next epoll_wait MUST also return 1 */
    n = epoll_wait(epfd, evs, 4, 0);
    if (n != 1 || !(evs[0].events & EPOLLIN) || evs[0].data.u64 != 0x12345678ULL) {
        close(p[0]);
        close(p[1]);
        close(epfd);
        return 19;
    }

    /* Partial read: 2 bytes read, 2 bytes unread */
    char buf[8];
    if (read(p[0], buf, 2) != 2) {
        close(p[0]);
        close(p[1]);
        close(epfd);
        return 20;
    }
    n = epoll_wait(epfd, evs, 4, 0);
    if (n != 1 || !(evs[0].events & EPOLLIN)) {
        close(p[0]);
        close(p[1]);
        close(epfd);
        return 21;
    }
    /* Drain remaining 2 bytes */
    if (read(p[0], buf, 2) != 2) {
        close(p[0]);
        close(p[1]);
        close(epfd);
        return 22;
    }
    if (epoll_wait(epfd, evs, 4, 0) != 0) {
        close(p[0]);
        close(p[1]);
        close(epfd);
        return 23;
    }

    /* 5. Child delayed write wakeup */
    pid_t child = fork();
    if (child < 0) {
        close(p[0]);
        close(p[1]);
        close(epfd);
        return 24;
    }
    if (child == 0) {
        close(p[0]);
        struct timespec delay = { .tv_sec = 0, .tv_nsec = 20000000L };
        nanosleep(&delay, NULL);
        if (write(p[1], "wake", 4) != 4) {
            _exit(1);
        }
        _exit(0);
    }
    n = epoll_wait(epfd, evs, 4, 2000);
    if (n != 1 || !(evs[0].events & EPOLLIN)) {
        close(p[0]);
        close(p[1]);
        close(epfd);
        return 25;
    }
    if (read(p[0], buf, 4) != 4) {
        close(p[0]);
        close(p[1]);
        close(epfd);
        return 26;
    }
    int child_status = 0;
    if (waitpid(child, &child_status, 0) != child || !WIFEXITED(child_status) ||
        WEXITSTATUS(child_status) != 0) {
        close(p[0]);
        close(p[1]);
        close(epfd);
        return 27;
    }

    /* 6. Edge Triggered (ET) mode */
    ev.events = EPOLLIN | EPOLLET;
    ev.data.u64 = 0x9999ULL;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, p[0], &ev) != 0) {
        close(p[0]);
        close(p[1]);
        close(epfd);
        return 28;
    }
    if (write(p[1], "abcd", 4) != 4) {
        close(p[0]);
        close(p[1]);
        close(epfd);
        return 29;
    }
    n = epoll_wait(epfd, evs, 4, 100);
    if (n != 1 || !(evs[0].events & EPOLLIN) || evs[0].data.u64 != 0x9999ULL) {
        close(p[0]);
        close(p[1]);
        close(epfd);
        return 30;
    }
    /* Partial read under ET */
    if (read(p[0], buf, 2) != 2) {
        close(p[0]);
        close(p[1]);
        close(epfd);
        return 31;
    }
    /* Under ET, unread data does NOT re-trigger without a new event edge */
    n = epoll_wait(epfd, evs, 4, 0);
    if (n != 0) {
        close(p[0]);
        close(p[1]);
        close(epfd);
        return 32;
    }
    /* New write edge arrives */
    if (write(p[1], "e", 1) != 1) {
        close(p[0]);
        close(p[1]);
        close(epfd);
        return 33;
    }
    n = epoll_wait(epfd, evs, 4, 100);
    if (n != 1 || !(evs[0].events & EPOLLIN)) {
        close(p[0]);
        close(p[1]);
        close(epfd);
        return 34;
    }
    /* Drain pipe: 2 bytes left from earlier + 1 new byte = 3 bytes */
    if (read(p[0], buf, 3) != 3) {
        close(p[0]);
        close(p[1]);
        close(epfd);
        return 35;
    }

    /* 7. EPOLLONESHOT mode */
    ev.events = EPOLLIN | EPOLLONESHOT;
    ev.data.u64 = 0x7777ULL;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, p[0], &ev) != 0) {
        close(p[0]);
        close(p[1]);
        close(epfd);
        return 36;
    }
    if (write(p[1], "x", 1) != 1) {
        close(p[0]);
        close(p[1]);
        close(epfd);
        return 37;
    }
    n = epoll_wait(epfd, evs, 4, 100);
    if (n != 1 || !(evs[0].events & EPOLLIN) || evs[0].data.u64 != 0x7777ULL) {
        close(p[0]);
        close(p[1]);
        close(epfd);
        return 38;
    }
    /* More data written while disarmed */
    if (write(p[1], "y", 1) != 1) {
        close(p[0]);
        close(p[1]);
        close(epfd);
        return 39;
    }
    n = epoll_wait(epfd, evs, 4, 0);
    if (n != 0) {
        close(p[0]);
        close(p[1]);
        close(epfd);
        return 40;
    }
    /* Re-arm via MOD */
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, p[0], &ev) != 0) {
        close(p[0]);
        close(p[1]);
        close(epfd);
        return 41;
    }
    n = epoll_wait(epfd, evs, 4, 0);
    if (n != 1 || !(evs[0].events & EPOLLIN)) {
        close(p[0]);
        close(p[1]);
        close(epfd);
        return 42;
    }
    /* Drain pipe (2 bytes) */
    if (read(p[0], buf, 2) != 2) {
        close(p[0]);
        close(p[1]);
        close(epfd);
        return 43;
    }

    /* 8. Deletion (EPOLL_CTL_DEL) */
    if (epoll_ctl(epfd, EPOLL_CTL_DEL, p[0], NULL) != 0) {
        close(p[0]);
        close(p[1]);
        close(epfd);
        return 44;
    }
    if (write(p[1], "z", 1) != 1) {
        close(p[0]);
        close(p[1]);
        close(epfd);
        return 45;
    }
    if (epoll_wait(epfd, evs, 4, 0) != 0) {
        close(p[0]);
        close(p[1]);
        close(epfd);
        return 46;
    }
    close(p[0]);
    close(p[1]);

    /* 9. epoll_pwait with signal mask & EINTR */
    struct sigaction sa = {0};
    sa.sa_handler = user_signal_handler;
    if (sigaction(SIGUSR1, &sa, NULL) != 0) {
        close(epfd);
        return 47;
    }
    sigset_t block_mask, orig_mask, wait_mask;
    sigemptyset(&block_mask);
    sigaddset(&block_mask, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &block_mask, &orig_mask) != 0) {
        close(epfd);
        return 48;
    }
    sigemptyset(&wait_mask); /* SIGUSR1 unblocked during epoll_pwait */

    child = fork();
    if (child < 0) {
        close(epfd);
        return 49;
    }
    if (child == 0) {
        signal_parent_after_delay(getppid(), SIGUSR1);
    }
    user_signal_seen = 0;
    errno = 0;
    n = epoll_pwait(epfd, evs, 4, 2000, &wait_mask);
    if (n != -1 || errno != EINTR || user_signal_seen != SIGUSR1) {
        close(epfd);
        return 50;
    }
    /* Verify SIGUSR1 remains blocked in parent */
    sigset_t current_mask;
    sigemptyset(&current_mask);
    if (sigprocmask(SIG_SETMASK, NULL, &current_mask) != 0 ||
        !sigismember(&current_mask, SIGUSR1)) {
        close(epfd);
        return 51;
    }
    if (sigprocmask(SIG_SETMASK, &orig_mask, NULL) != 0) {
        close(epfd);
        return 52;
    }
    if (waitpid(child, &child_status, 0) != child || !WIFEXITED(child_status) ||
        WEXITSTATUS(child_status) != 0) {
        close(epfd);
        return 53;
    }

    /* 10. Composability: poll() on epfd */
    int p2[2];
    if (pipe(p2) != 0) {
        close(epfd);
        return 54;
    }
    ev.events = EPOLLIN;
    ev.data.u64 = 0x42ULL;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, p2[0], &ev) != 0) {
        close(p2[0]);
        close(p2[1]);
        close(epfd);
        return 55;
    }
    struct pollfd pfd = { .fd = epfd, .events = POLLIN, .revents = 0 };
    if (poll(&pfd, 1, 0) != 0) {
        close(p2[0]);
        close(p2[1]);
        close(epfd);
        return 56;
    }
    if (write(p2[1], "ok", 2) != 2) {
        close(p2[0]);
        close(p2[1]);
        close(epfd);
        return 57;
    }
    if (poll(&pfd, 1, 100) != 1 || !(pfd.revents & POLLIN)) {
        close(p2[0]);
        close(p2[1]);
        close(epfd);
        return 58;
    }
    close(p2[0]);
    close(p2[1]);
    close(epfd);

    /* 11. fd reuse regression test: (fd, OFD) identity */
    int ep_reuse = epoll_create1(0);
    if (ep_reuse < 0) {
        return 60;
    }
    int p_a[2];
    if (pipe(p_a) != 0) {
        close(ep_reuse);
        return 61;
    }
    int oldfd = p_a[0];
    int keepfd = dup(oldfd);
    if (keepfd < 0) {
        close(p_a[0]); close(p_a[1]); close(ep_reuse);
        return 62;
    }
    struct epoll_event ev_a = { .events = EPOLLIN, .data.u32 = 0xaaaa };
    if (epoll_ctl(ep_reuse, EPOLL_CTL_ADD, oldfd, &ev_a) != 0) {
        close(keepfd); close(p_a[0]); close(p_a[1]); close(ep_reuse);
        return 63;
    }
    /* Close oldfd. OFD A is kept alive by keepfd. epoll interest (oldfd, OFD A) should remain. */
    close(oldfd);

    /* Create pipe B and ensure it reuses oldfd number */
    int p_b[2];
    if (pipe(p_b) != 0) {
        close(keepfd); close(p_a[1]); close(ep_reuse);
        return 64;
    }
    int p_b_read = p_b[0];
    if (p_b_read != oldfd) {
        if (dup2(p_b[0], oldfd) < 0) {
            close(p_b[0]); close(p_b[1]); close(keepfd); close(p_a[1]); close(ep_reuse);
            return 65;
        }
        close(p_b[0]);
        p_b_read = oldfd;
    }
    /* epoll ADD (oldfd, OFD B) must succeed and NOT return EEXIST */
    struct epoll_event ev_b = { .events = EPOLLIN, .data.u32 = 0xbbbb };
    if (epoll_ctl(ep_reuse, EPOLL_CTL_ADD, p_b_read, &ev_b) != 0) {
        close(p_b_read); close(p_b[1]); close(keepfd); close(p_a[1]); close(ep_reuse);
        return 66;
    }

    /* Trigger both A and B */
    if (write(p_a[1], "a", 1) != 1 || write(p_b[1], "b", 1) != 1) {
        close(p_b_read); close(p_b[1]); close(keepfd); close(p_a[1]); close(ep_reuse);
        return 67;
    }
    struct epoll_event reuse_evs[4];
    int r_cnt = epoll_wait(ep_reuse, reuse_evs, 4, 100);
    if (r_cnt != 2) {
        close(p_b_read); close(p_b[1]); close(keepfd); close(p_a[1]); close(ep_reuse);
        return 68;
    }
    int seen_a = 0, seen_b = 0;
    for (int i = 0; i < 2; i++) {
        if (reuse_evs[i].data.u32 == 0xaaaa) seen_a = 1;
        if (reuse_evs[i].data.u32 == 0xbbbb) seen_b = 1;
    }
    if (!seen_a || !seen_b) {
        close(p_b_read); close(p_b[1]); close(keepfd); close(p_a[1]); close(ep_reuse);
        return 69;
    }

    /* Drain B */
    char reuse_buf[4];
    if (read(p_b_read, reuse_buf, 1) != 1) {
        close(p_b_read); close(p_b[1]); close(keepfd); close(p_a[1]); close(ep_reuse);
        return 70;
    }
    /* DEL reused oldfd (OFD B) */
    if (epoll_ctl(ep_reuse, EPOLL_CTL_DEL, p_b_read, NULL) != 0) {
        close(p_b_read); close(p_b[1]); close(keepfd); close(p_a[1]); close(ep_reuse);
        return 71;
    }

    /* Trigger A again */
    if (write(p_a[1], "A", 1) != 1) {
        close(p_b_read); close(p_b[1]); close(keepfd); close(p_a[1]); close(ep_reuse);
        return 72;
    }
    r_cnt = epoll_wait(ep_reuse, reuse_evs, 4, 100);
    if (r_cnt != 1 || reuse_evs[0].data.u32 != 0xaaaa) {
        close(p_b_read); close(p_b[1]); close(keepfd); close(p_a[1]); close(ep_reuse);
        return 73;
    }
    /* Close keepfd: OFD A is released, registration for A must be torn down automatically */
    close(keepfd);
    close(p_a[1]);
    close(p_b_read);
    close(p_b[1]);
    close(ep_reuse);

    /* 12. Nested epoll, loop detection, and nesting limit */
    int ep_a = epoll_create1(0);
    int ep_b = epoll_create1(0);
    if (ep_a < 0 || ep_b < 0) {
        if (ep_a >= 0) close(ep_a);
        if (ep_b >= 0) close(ep_b);
        return 74;
    }
    struct epoll_event nest_ev = { .events = EPOLLIN, .data.u32 = 1 };
    /* ep_a watches ep_b */
    if (epoll_ctl(ep_a, EPOLL_CTL_ADD, ep_b, &nest_ev) != 0) {
        close(ep_a); close(ep_b);
        return 75;
    }
    /* ep_b watches ep_a: cycle -> ELOOP */
    errno = 0;
    if (epoll_ctl(ep_b, EPOLL_CTL_ADD, ep_a, &nest_ev) != -1 || errno != ELOOP) {
        close(ep_a); close(ep_b);
        return 76;
    }
    int ep_c = epoll_create1(0);
    /* ep_b watches ep_c */
    if (epoll_ctl(ep_b, EPOLL_CTL_ADD, ep_c, &nest_ev) != 0) {
        close(ep_a); close(ep_b); close(ep_c);
        return 77;
    }
    /* ep_c watches ep_a: cycle -> ELOOP */
    errno = 0;
    if (epoll_ctl(ep_c, EPOLL_CTL_ADD, ep_a, &nest_ev) != -1 || errno != ELOOP) {
        close(ep_a); close(ep_b); close(ep_c);
        return 78;
    }
    close(ep_a); close(ep_b); close(ep_c);

    /* Nesting chain limit: ep0 -> ep1 -> ep2 -> ep3 -> ep4 (depth 4) */
    int chain[6];
    for (int i = 0; i < 6; i++) {
        chain[i] = epoll_create1(0);
        if (chain[i] < 0) return 79;
    }
    for (int i = 0; i < 4; i++) {
        if (epoll_ctl(chain[i], EPOLL_CTL_ADD, chain[i+1], &nest_ev) != 0) {
            for (int j = 0; j < 6; j++) close(chain[j]);
            return 80;
        }
    }
    /* 5th nesting level exceeds EP_MAX_NESTS (4) -> ELOOP */
    errno = 0;
    if (epoll_ctl(chain[4], EPOLL_CTL_ADD, chain[5], &nest_ev) != -1 || errno != ELOOP) {
        for (int j = 0; j < 6; j++) close(chain[j]);
        return 81;
    }
    for (int j = 0; j < 6; j++) close(chain[j]);

    /* 13. maxevents > 1024 (e.g. 2048) on epoll_wait */
    int ep_large = epoll_create1(0);
    if (ep_large >= 0) {
        struct epoll_event large_evs[2];
        if (epoll_wait(ep_large, large_evs, 2048, 0) != 0) {
            close(ep_large);
            return 82;
        }
        close(ep_large);
    }

    static const char epoll_marker[] =
        "BoarOS: real userland epoll checks ok\n";
    if (write(1, epoll_marker, sizeof(epoll_marker) - 1) !=
        (ssize_t)(sizeof(epoll_marker) - 1)) {
        return 59;
    }

    return 0;
}

static int check_filesystem_rw(void)
{
    /* 1. File creation and exclusive open */
    int wfd = open("/testfile.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (wfd < 0) {
        return 1;
    }
    /* O_CREAT | O_EXCL on existing file must fail with EEXIST */
    errno = 0;
    int excl_fd = open("/testfile.txt", O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (excl_fd >= 0 || errno != EEXIST) {
        if (excl_fd >= 0) close(excl_fd);
        close(wfd);
        return 2;
    }

    /* 2. Write regular file and writev */
    const char msg1[] = "Hello BoarOS ext4 write!\n";
    size_t len1 = sizeof(msg1) - 1;
    if (write(wfd, msg1, len1) != (ssize_t)len1) {
        close(wfd);
        return 3;
    }
    struct iovec iov[2];
    iov[0].iov_base = (void *)"12345";
    iov[0].iov_len = 5;
    iov[1].iov_base = (void *)"67890";
    iov[1].iov_len = 5;
    if (writev(wfd, iov, 2) != 10) {
        close(wfd);
        return 4;
    }
    if (lseek(wfd, 0, SEEK_CUR) != (off_t)(len1 + 10)) {
        close(wfd);
        return 5;
    }
    if (close(wfd) != 0) {
        return 6;
    }

    /* 3. Read back and verify */
    int rfd = open("/testfile.txt", O_RDONLY);
    if (rfd < 0) {
        return 7;
    }
    struct stat st;
    if (fstat(rfd, &st) != 0 || st.st_size != (off_t)(len1 + 10)) {
        close(rfd);
        return 8;
    }
    char readbuf[64];
    memset(readbuf, 0, sizeof(readbuf));
    if (read(rfd, readbuf, sizeof(readbuf)) != (ssize_t)(len1 + 10)) {
        close(rfd);
        return 9;
    }
    if (memcmp(readbuf, msg1, len1) != 0 ||
        memcmp(readbuf + len1, "1234567890", 10) != 0) {
        close(rfd);
        return 10;
    }
    /* ftruncate on O_RDONLY fd must fail with EINVAL */
    errno = 0;
    if (ftruncate(rfd, 0) != -1 || errno != EINVAL) {
        close(rfd);
        return 11;
    }
    close(rfd);

    /* 4. Append mode */
    int afd = open("/testfile.txt", O_WRONLY | O_APPEND);
    if (afd < 0) {
        return 12;
    }
    if (write(afd, "+append", 7) != 7) {
        close(afd);
        return 13;
    }
    close(afd);

    rfd = open("/testfile.txt", O_RDONLY);
    if (rfd < 0) {
        return 14;
    }
    if (fstat(rfd, &st) != 0 || st.st_size != (off_t)(len1 + 10 + 7)) {
        close(rfd);
        return 15;
    }
    close(rfd);

    /* 5. Truncate */
    int rwfd = open("/testfile.txt", O_RDWR);
    if (rwfd < 0) {
        return 16;
    }
    if (ftruncate(rwfd, 5) != 0) {
        close(rwfd);
        return 17;
    }
    if (fstat(rwfd, &st) != 0 || st.st_size != 5) {
        close(rwfd);
        return 18;
    }
    memset(readbuf, 0, sizeof(readbuf));
    if (lseek(rwfd, 0, SEEK_SET) != 0 || read(rwfd, readbuf, sizeof(readbuf)) != 5 ||
        memcmp(readbuf, "Hello", 5) != 0) {
        close(rwfd);
        return 19;
    }
    close(rwfd);

    /* O_TRUNC on open */
    wfd = open("/testfile.txt", O_WRONLY | O_TRUNC);
    if (wfd < 0) {
        return 20;
    }
    if (fstat(wfd, &st) != 0 || st.st_size != 0) {
        close(wfd);
        return 21;
    }
    close(wfd);

    /* 6. Directory operations: mkdir / unlink / rmdir */
    if (mkdir("/testdir", 0755) != 0) {
        return 22;
    }
    /* Opening directory with O_WRONLY must fail with EISDIR */
    errno = 0;
    int dir_wfd = open("/testdir", O_WRONLY);
    if (dir_wfd >= 0 || errno != EISDIR) {
        if (dir_wfd >= 0) close(dir_wfd);
        return 23;
    }
    /* Create a file inside /testdir */
    int subfd = open("/testdir/sub.txt", O_CREAT | O_WRONLY, 0644);
    if (subfd < 0) {
        return 24;
    }
    if (write(subfd, "boar", 4) != 4) {
        close(subfd);
        return 25;
    }
    close(subfd);

    /* Non-empty rmdir should fail */
    errno = 0;
    if (rmdir("/testdir") != -1 || (errno != ENOTEMPTY && errno != EEXIST)) {
        return 26;
    }

    /* Unlink file inside directory */
    if (unlink("/testdir/sub.txt") != 0) {
        return 27;
    }
    /* Confirm file is gone */
    errno = 0;
    if (open("/testdir/sub.txt", O_RDONLY) >= 0 || errno != ENOENT) {
        return 28;
    }

    /* rmdir empty directory */
    if (rmdir("/testdir") != 0) {
        return 29;
    }
    /* Confirm directory is gone */
    errno = 0;
    if (open("/testdir", O_RDONLY) >= 0 || errno != ENOENT) {
        return 30;
    }

    /* 7. Unlink /testfile.txt */
    if (unlink("/testfile.txt") != 0) {
        return 31;
    }
    errno = 0;
    if (open("/testfile.txt", O_RDONLY) >= 0 || errno != ENOENT) {
        return 32;
    }

    /* Unlinking non-existent file must fail with ENOENT */
    errno = 0;
    if (unlink("/testfile.txt") != -1 || errno != ENOENT) {
        return 33;
    }

    /* 8. ETXTBSY tests: cannot open running executable (/init) for writing */
    errno = 0;
    int exec_wfd = open("/init", O_WRONLY);
    if (exec_wfd >= 0 || errno != ETXTBSY) {
        if (exec_wfd >= 0) close(exec_wfd);
        return 35;
    }
    errno = 0;
    int exec_rwfd = open("/init", O_RDWR);
    if (exec_rwfd >= 0 || errno != ETXTBSY) {
        if (exec_rwfd >= 0) close(exec_rwfd);
        return 36;
    }
    errno = 0;
    int exec_trfd = open("/init", O_RDONLY | O_TRUNC);
    if (exec_trfd >= 0 || errno != ETXTBSY) {
        if (exec_trfd >= 0) close(exec_trfd);
        return 37;
    }
    /* open /init for reading must succeed */
    int exec_rfd = open("/init", O_RDONLY);
    if (exec_rfd < 0) {
        return 38;
    }
    char elf_hdr[4];
    if (read(exec_rfd, elf_hdr, 4) != 4 || memcmp(elf_hdr, "\177ELF", 4) != 0) {
        close(exec_rfd);
        return 39;
    }
    close(exec_rfd);
    printf("BoarOS: fsrw stage etxtbsy ok\n");
    fflush(stdout);

    /* 9. Executable text denial on newly written file:
     * Copy /init to /test_exec, keep write fd open, fork+execve must fail with ETXTBSY.
     * Close write fd, fork+execve must succeed. */
    int src_fd = open("/init", O_RDONLY);
    if (src_fd < 0) {
        return 40;
    }
    int bin_wfd = open("/test_exec", O_CREAT | O_WRONLY | O_TRUNC, 0755);
    if (bin_wfd < 0) {
        close(src_fd);
        return 41;
    }
    char copy_buf[512];
    ssize_t nread;
    while ((nread = read(src_fd, copy_buf, sizeof(copy_buf))) > 0) {
        if (write(bin_wfd, copy_buf, (size_t)nread) != nread) {
            close(src_fd);
            close(bin_wfd);
            return 42;
        }
    }
    close(src_fd);
    printf("BoarOS: fsrw stage 9 copy ok\n");
    fflush(stdout);

    /* While bin_wfd is still open for write, fork a child to execve /test_exec */
    pid_t cpid = fork();
    if (cpid < 0) {
        close(bin_wfd);
        return 43;
    }
    if (cpid == 0) {
        char *exec_args[] = { "/test_exec", "child_exit", NULL };
        char *exec_env[] = { NULL };
        execve("/test_exec", exec_args, exec_env);
        /* execve should have failed with ETXTBSY! */
        _exit(errno == ETXTBSY ? 77 : 88);
    }
    int status = 0;
    if (waitpid(cpid, &status, 0) != cpid) {
        close(bin_wfd);
        return 44;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 77) {
        close(bin_wfd);
        return 45;
    }
    printf("BoarOS: fsrw stage 9 etxtbsy child ok\n");
    fflush(stdout);

    /* Now close the write descriptor */
    if (close(bin_wfd) != 0) {
        return 46;
    }

    /* Now execve should succeed! */
    cpid = fork();
    if (cpid < 0) {
        return 47;
    }
    if (cpid == 0) {
        char *exec_args[] = { "/test_exec", "child_exit", NULL };
        char *exec_env[] = { NULL };
        execve("/test_exec", exec_args, exec_env);
        _exit(99);
    }
    if (waitpid(cpid, &status, 0) != cpid) {
        return 48;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 42) {
        return 49;
    }
    printf("BoarOS: fsrw stage 9 exec child ok\n");
    fflush(stdout);

    /* Clean up /test_exec */
    unlink("/test_exec");
    printf("BoarOS: fsrw stage test-exec unlink ok\n");
    fflush(stdout);

    /* 10. Warm page-cache overwrite coherence:
     * Read file to populate page cache, overwrite from another fd, then read back from first fd. */
    int cache_wfd = open("/cache_test.txt", O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (cache_wfd < 0) {
        return 50;
    }
    if (write(cache_wfd, "INITIAL_CACHE_DATA_STRING_12345", 31) != 31) {
        close(cache_wfd);
        return 51;
    }
    /* Open for reading to populate warm cache */
    int cache_rfd = open("/cache_test.txt", O_RDONLY);
    if (cache_rfd < 0) {
        close(cache_wfd);
        return 52;
    }
    char cbuf[32];
    memset(cbuf, 0, sizeof(cbuf));
    if (read(cache_rfd, cbuf, 31) != 31 || memcmp(cbuf, "INITIAL_CACHE_DATA_STRING_12345", 31) != 0) {
        close(cache_wfd);
        close(cache_rfd);
        return 53;
    }
    /* Overwrite first 7 bytes via write fd */
    if (lseek(cache_wfd, 0, SEEK_SET) != 0 || write(cache_wfd, "UPDATED", 7) != 7) {
        close(cache_wfd);
        close(cache_rfd);
        return 54;
    }
    /* Read back from read fd: must observe "UPDATED_CACHE_DATA_STRING_12345" */
    if (lseek(cache_rfd, 0, SEEK_SET) != 0) {
        close(cache_wfd);
        close(cache_rfd);
        return 55;
    }
    memset(cbuf, 0, sizeof(cbuf));
    if (read(cache_rfd, cbuf, 31) != 31 || memcmp(cbuf, "UPDATED_CACHE_DATA_STRING_12345", 31) != 0) {
        close(cache_wfd);
        close(cache_rfd);
        return 56;
    }
    close(cache_wfd);
    close(cache_rfd);
    unlink("/cache_test.txt");
    printf("BoarOS: fsrw stage cache ok\n");
    fflush(stdout);

    /* 11. Multi-OFD size coherence:
     * fd1 write extends size, fd2 fstat observes immediately.
     * fd2 ftruncate shrinks size, fd1 fstat observes immediately. */
    int sofd1 = open("/size_test.txt", O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (sofd1 < 0) {
        return 57;
    }
    int sofd2 = open("/size_test.txt", O_RDWR);
    if (sofd2 < 0) {
        close(sofd1);
        return 58;
    }
    char hundred[100];
    memset(hundred, 'H', sizeof(hundred));
    if (write(sofd1, hundred, sizeof(hundred)) != 100) {
        close(sofd1);
        close(sofd2);
        return 59;
    }
    struct stat sst;
    if (fstat(sofd2, &sst) != 0 || sst.st_size != 100) {
        close(sofd1);
        close(sofd2);
        return 60;
    }
    if (ftruncate(sofd2, 40) != 0) {
        close(sofd1);
        close(sofd2);
        return 61;
    }
    if (fstat(sofd1, &sst) != 0 || sst.st_size != 40) {
        close(sofd1);
        close(sofd2);
        return 62;
    }
    close(sofd1);
    close(sofd2);
    unlink("/size_test.txt");

    /* 12. Truncate extend zero fill and file offset preservation:
     * Write 5 bytes, check lseek(CUR) == 5.
     * ftruncate to 8192 + 50.
     * Assert lseek(CUR) remains 5.
     * Read from 5 to 55: all bytes must be zero.
     * lseek to 8192, read 50 bytes: all bytes must be zero. */
    int trfd = open("/trunc_extend.txt", O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (trfd < 0) {
        return 63;
    }
    if (write(trfd, "12345", 5) != 5) {
        close(trfd);
        return 64;
    }
    if (lseek(trfd, 0, SEEK_CUR) != 5) {
        close(trfd);
        return 65;
    }
    if (ftruncate(trfd, 8192 + 50) != 0) {
        close(trfd);
        return 66;
    }
    if (lseek(trfd, 0, SEEK_CUR) != 5) {
        close(trfd);
        return 67;
    }
    if (fstat(trfd, &sst) != 0 || sst.st_size != 8192 + 50) {
        close(trfd);
        return 68;
    }
    char zbuf[50];
    memset(zbuf, 0xff, sizeof(zbuf));
    if (read(trfd, zbuf, sizeof(zbuf)) != sizeof(zbuf)) {
        close(trfd);
        return 69;
    }
    for (int zi = 0; zi < 50; zi++) {
        if (zbuf[zi] != 0) {
            close(trfd);
            return 70;
        }
    }
    if (lseek(trfd, 8192, SEEK_SET) != 8192) {
        close(trfd);
        return 71;
    }
    memset(zbuf, 0xff, sizeof(zbuf));
    if (read(trfd, zbuf, sizeof(zbuf)) != sizeof(zbuf)) {
        close(trfd);
        return 72;
    }
    for (int zi = 0; zi < 50; zi++) {
        if (zbuf[zi] != 0) {
            close(trfd);
            return 73;
        }
    }
    close(trfd);
    unlink("/trunc_extend.txt");

    /* 13a. Unlink unopened cached file:
     * Create file, write data, close file (cached in page cache).
     * Unlink must reclaim page-cache held inode without double free. */
    int uc_fd = open("/unlink_cached.txt", O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (uc_fd < 0) {
        return 74;
    }
    if (write(uc_fd, "CACHED_DATA", 11) != 11) {
        close(uc_fd);
        return 75;
    }
    if (close(uc_fd) != 0) {
        return 76;
    }
    if (unlink("/unlink_cached.txt") != 0) {
        return 77;
    }
    errno = 0;
    int uc_re = open("/unlink_cached.txt", O_RDONLY);
    if (uc_re >= 0 || errno != ENOENT) {
        if (uc_re >= 0) close(uc_re);
        return 78;
    }

    /* 13b. True unlink-but-open read/write/fstat lifecycle:
     * Open fd, write data, unlink file.
     * Subsequent open fails with ENOENT.
     * Original fd must continue to support fstat, lseek, read, and write!
     * Same pathname recreated while orphan is still open must yield a distinct inode! */
    int ufd = open("/unlink_live.txt", O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (ufd < 0) {
        return 79;
    }
    if (write(ufd, "abcdef", 6) != 6) {
        close(ufd);
        return 80;
    }
    struct stat ust_orig;
    memset(&ust_orig, 0, sizeof(ust_orig));
    if (fstat(ufd, &ust_orig) != 0 || ust_orig.st_size != 6 ||
        ust_orig.st_nlink != 1 || ust_orig.st_blocks <= 0) {
        close(ufd);
        return 81;
    }
    if (unlink("/unlink_live.txt") != 0) {
        close(ufd);
        return 82;
    }
    {
        struct stat unlinked_stat;

        if (fstat(ufd, &unlinked_stat) != 0 ||
            unlinked_stat.st_ino != ust_orig.st_ino ||
            unlinked_stat.st_nlink != 0 || unlinked_stat.st_size != 6) {
            close(ufd);
            return 86;
        }
    }
    errno = 0;
    int re_open = open("/unlink_live.txt", O_RDONLY);
    if (re_open >= 0 || errno != ENOENT) {
        if (re_open >= 0) close(re_open);
        close(ufd);
        return 120;
    }
    /* WHILE ufd IS STILL OPEN: Recreate /unlink_live.txt with same path.
     * New file must obtain a distinct inode from the live orphan! */
    int new_ufd = open("/unlink_live.txt", O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (new_ufd < 0) {
        close(ufd);
        return 121;
    }
    struct stat ust_new;
    memset(&ust_new, 0, sizeof(ust_new));
    if (fstat(new_ufd, &ust_new) != 0 || ust_new.st_ino == ust_orig.st_ino) {
        close(new_ufd);
        close(ufd);
        return 122;
    }
    if (write(new_ufd, "recreated", 9) != 9) {
        close(new_ufd);
        close(ufd);
        return 123;
    }
    char ubuf[16];
    memset(ubuf, 0, sizeof(ubuf));
    if (lseek(ufd, 0, SEEK_SET) != 0 || read(ufd, ubuf, 6) != 6 ||
        memcmp(ubuf, "abcdef", 6) != 0) {
        close(new_ufd);
        close(ufd);
        return 124;
    }
    if (lseek(ufd, 6, SEEK_SET) != 6 || write(ufd, "XYZ", 3) != 3) {
        close(new_ufd);
        close(ufd);
        return 125;
    }
    struct stat ust;
    memset(&ust, 0, sizeof(ust));
    if (fstat(ufd, &ust) != 0 || ust.st_size != 9 || ust.st_ino != ust_orig.st_ino) {
        close(new_ufd);
        close(ufd);
        return 126;
    }
    memset(ubuf, 0, sizeof(ubuf));
    if (lseek(new_ufd, 0, SEEK_SET) != 0 || read(new_ufd, ubuf, 9) != 9 ||
        memcmp(ubuf, "recreated", 9) != 0) {
        close(new_ufd);
        close(ufd);
        return 127;
    }
    if (close(ufd) != 0 || close(new_ufd) != 0 || unlink("/unlink_live.txt") != 0) {
        return 128;
    }
    printf("BoarOS: fsrw stage unlink-live ok\n");
    fflush(stdout);

    /* 13b. Multi-OFD unlink lifecycle:
     * fd1 and fd2 both open before unlink.
     * After unlink, both remain usable; close(fd1) leaves fd2 usable. */
    int m_fd1 = open("/unlink_multi.txt", O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (m_fd1 < 0) {
        return 133;
    }
    if (write(m_fd1, "HELLO", 5) != 5) {
        close(m_fd1);
        return 134;
    }
    int m_fd2 = open("/unlink_multi.txt", O_RDONLY);
    if (m_fd2 < 0) {
        close(m_fd1);
        return 135;
    }
    if (unlink("/unlink_multi.txt") != 0) {
        close(m_fd1);
        close(m_fd2);
        return 136;
    }
    char m_buf[16];
    memset(m_buf, 0, sizeof(m_buf));
    if (lseek(m_fd1, 0, SEEK_SET) != 0 || read(m_fd1, m_buf, 5) != 5 ||
        memcmp(m_buf, "HELLO", 5) != 0) {
        close(m_fd1);
        close(m_fd2);
        return 137;
    }
    memset(m_buf, 0, sizeof(m_buf));
    if (lseek(m_fd2, 0, SEEK_SET) != 0 || read(m_fd2, m_buf, 5) != 5 ||
        memcmp(m_buf, "HELLO", 5) != 0) {
        close(m_fd1);
        close(m_fd2);
        return 138;
    }
    if (close(m_fd1) != 0) {
        close(m_fd2);
        return 139;
    }
    memset(m_buf, 0, sizeof(m_buf));
    if (lseek(m_fd2, 0, SEEK_SET) != 0 || read(m_fd2, m_buf, 5) != 5 ||
        memcmp(m_buf, "HELLO", 5) != 0) {
        close(m_fd2);
        return 140;
    }
    if (close(m_fd2) != 0) {
        return 141;
    }
    printf("BoarOS: fsrw stage unlink-multi ok\n");
    fflush(stdout);

    /* 13c. Executable demand-paging across unlink:
     * Harness has placed /unlink_exec on the root disk.
     * Child execs /unlink_exec, handshakes with parent.
     * Parent unlinks /unlink_exec.
     * Child accesses far page, triggering demand page fault on unlinked inode. */
    int p_ready[2];
    int p_ack[2];
    if (pipe(p_ready) != 0 || pipe(p_ack) != 0) {
        return 146;
    }
    char rdy_arg[16];
    char ack_arg[16];
    snprintf(rdy_arg, sizeof(rdy_arg), "%d", p_ready[1]);
    snprintf(ack_arg, sizeof(ack_arg), "%d", p_ack[0]);
    pid_t ex_cpid = fork();
    if (ex_cpid < 0) {
        return 147;
    }
    if (ex_cpid == 0) {
        close(p_ready[0]);
        close(p_ack[1]);
        char *ex_args[] = { "/unlink_exec", "unlink_exec_child", rdy_arg, ack_arg, NULL };
        char *ex_env[] = { NULL };
        execve("/unlink_exec", ex_args, ex_env);
        _exit(129);
    }
    close(p_ready[1]);
    close(p_ack[0]);

    char rdy_buf[8];
    if (read(p_ready[0], rdy_buf, 6) != 6 || memcmp(rdy_buf, "READY\n", 6) != 0) {
        close(p_ready[0]);
        close(p_ack[1]);
        return 148;
    }
    close(p_ready[0]);

    if (unlink("/unlink_exec") != 0) {
        close(p_ack[1]);
        return 149;
    }
    errno = 0;
    int ex_ck = open("/unlink_exec", O_RDONLY);
    if (ex_ck >= 0 || errno != ENOENT) {
        if (ex_ck >= 0) close(ex_ck);
        close(p_ack[1]);
        return 150;
    }

    if (write(p_ack[1], "UNLINKED", 8) != 8) {
        close(p_ack[1]);
        return 151;
    }
    close(p_ack[1]);

    int ex_status = 0;
    if (waitpid(ex_cpid, &ex_status, 0) != ex_cpid) {
        return 152;
    }
    if (!WIFEXITED(ex_status) || WEXITSTATUS(ex_status) != 43) {
        return 153;
    }
    printf("BoarOS: fsrw stage unlink-exec ok\n");
    fflush(stdout);

    /* 14. Append mode atomic offset resolution:
     * Open O_APPEND, write 10 bytes, lseek to offset 0, write 3 bytes.
     * Bytes must be appended at offset 10, offset becomes 13. */
    int apfd = open("/append_lseek.txt", O_CREAT | O_RDWR | O_APPEND | O_TRUNC, 0644);
    if (apfd < 0) {
        return 83;
    }
    if (write(apfd, "0123456789", 10) != 10) {
        close(apfd);
        return 84;
    }
    if (lseek(apfd, 0, SEEK_CUR) != 10) {
        close(apfd);
        return 85;
    }
    if (lseek(apfd, 0, SEEK_SET) != 0) {
        close(apfd);
        return 86;
    }
    if (write(apfd, "ABC", 3) != 3) {
        close(apfd);
        return 87;
    }
    if (lseek(apfd, 0, SEEK_CUR) != 13) {
        close(apfd);
        return 88;
    }
    char apbuf[16];
    memset(apbuf, 0, sizeof(apbuf));
    if (lseek(apfd, 0, SEEK_SET) != 0 || read(apfd, apbuf, 13) != 13 ||
        memcmp(apbuf, "0123456789ABC", 13) != 0) {
        close(apfd);
        return 89;
    }
    close(apfd);
    unlink("/append_lseek.txt");

    /* 15. Create persistence marker file for host-side verification */
    int pfd = open("/persist.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (pfd < 0) {
        return 90;
    }
    static const char persist_data[] = "BoarOS-ext4-persisted-data";
    if (write(pfd, persist_data, sizeof(persist_data) - 1) != (ssize_t)(sizeof(persist_data) - 1)) {
        close(pfd);
        return 91;
    }
    if (close(pfd) != 0) {
        return 92;
    }

    static const char rw_marker[] = "BoarOS: real userland fs rw checks ok\n";
    if (write(1, rw_marker, sizeof(rw_marker) - 1) != (ssize_t)(sizeof(rw_marker) - 1)) {
        return 34;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "child_exit") == 0) {
        return 42;
    }
    if (argc > 3 && strcmp(argv[1], "unlink_exec_child") == 0) {
        int rdy_fd = 0, ack_fd = 0;
        for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++) rdy_fd = rdy_fd * 10 + (*p - '0');
        for (const char *p = argv[3]; *p >= '0' && *p <= '9'; p++) ack_fd = ack_fd * 10 + (*p - '0');
        if (write(rdy_fd, "READY\n", 6) != 6) {
            return 120;
        }
        char ack[10];
        if (read(ack_fd, ack, 8) != 8 || memcmp(ack, "UNLINKED", 8) != 0) {
            return 121;
        }
        if (memcmp(unlink_far_page, "UNLINK_DEMAND_FAULT_PAGE_PAYLOAD", 32) != 0) {
            return 122;
        }
        return 43;
    }

    printf("BoarOS: real userland stdio ok\n");

    DIR *dir = opendir("/");
    if (dir == 0) {
        return 1;
    }
    int saw_data = 0;
    int saw_init = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != 0) {
        if (strcmp(entry->d_name, "data") == 0) {
            saw_data = 1;
        }
        if (strcmp(entry->d_name, "init") == 0) {
            saw_init = 1;
        }
    }
    closedir(dir);
    if (!saw_data || !saw_init) {
        return 2;
    }

    int fd = open("/data", O_RDONLY);
    if (fd < 0) {
        return 3;
    }
    char buffer[16];
    ssize_t read_bytes = read(fd, buffer, sizeof(buffer));
    if (read_bytes != 16 || buffer[0] != 'A' || buffer[15] != 'P') {
        return 4;
    }
    if (lseek(fd, 0, SEEK_END) != 9000) {
        return 5;
    }
    struct stat status;
    if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_size != 9000) {
        return 6;
    }
    if (dup(fd) < 0) {
        return 7;
    }
    /* The duplicate shares one open-file description. */
    char shared[2];
    if (read(fd, shared, 1) != 0) {
        return 8;
    }
    if (write(1, "BoarOS: real userland file checks ok\n", 37) != 37) {
        return 9;
    }
    close(fd);

    int rw_err = check_filesystem_rw();
    if (rw_err != 0) {
        fprintf(stderr, "check_filesystem_rw failed: %d errno=%d\n", rw_err, errno);
        return 70 + rw_err;
    }

    /* Clocks: monotonic must advance, realtime must dominate it, and
     * gettimeofday must agree with clock_gettime on the same clock. */
    struct timespec mono_before;
    struct timespec mono_after;
    struct timespec realtime;
    if (clock_gettime(CLOCK_MONOTONIC, &mono_before) != 0) {
        return 10;
    }
    for (volatile unsigned long spin = 0; spin < 200000UL; spin++) {
    }
    if (clock_gettime(CLOCK_MONOTONIC, &mono_after) != 0) {
        return 11;
    }
    if (mono_after.tv_sec < mono_before.tv_sec ||
        (mono_after.tv_sec == mono_before.tv_sec &&
         mono_after.tv_nsec <= mono_before.tv_nsec)) {
        return 12;
    }
    if (clock_gettime(CLOCK_REALTIME, &realtime) != 0) {
        return 13;
    }
    /* The virt board's goldfish RTC must back CLOCK_REALTIME with a
     * plausible wall clock; boot-relative time would sit near zero. */
    if (realtime.tv_sec < (time_t)1577836800L) {
        return 14;
    }
    struct timeval day;
    if (gettimeofday(&day, 0) != 0) {
        return 15;
    }
    long realtime_seconds = (long)realtime.tv_sec;
    if (day.tv_sec < realtime_seconds || day.tv_sec > realtime_seconds + 1) {
        return 16;
    }
    if (write(1, "BoarOS: real userland clock checks ok\n", 38) != 38) {
        return 17;
    }

    /* Sleep: musl nanosleep routes to clock_nanosleep; the raw SYS_nanosleep
     * entry must behave identically.  Each call must sleep at least the
     * requested duration, and the kernel must not busy-wait the core. */
    if (clock_gettime(CLOCK_MONOTONIC, &mono_before) != 0) {
        return 18;
    }
    struct timespec pause = { .tv_sec = 0, .tv_nsec = 20000000L };
    if (nanosleep(&pause, 0) != 0) {
        return 19;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &mono_after) != 0) {
        return 20;
    }
    if (mono_after.tv_sec < mono_before.tv_sec ||
        (mono_after.tv_sec == mono_before.tv_sec &&
         (long)(mono_after.tv_nsec - mono_before.tv_nsec) < 20000000L)) {
        return 21;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &mono_before) != 0) {
        return 22;
    }
    pause.tv_nsec = 5000000L;
    if (syscall(SYS_nanosleep, &pause, 0) != 0) {
        return 23;
    }
    /* A NULL request pointer must fault the kernel copy, not the kernel. */
    pause.tv_nsec = 0;
    errno = 0;
    if (syscall(SYS_clock_nanosleep, CLOCK_REALTIME, 0, 0, 0) != -1L ||
        errno != EFAULT) {
        return 24;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &mono_after) != 0) {
        return 25;
    }
    if (mono_after.tv_sec < mono_before.tv_sec ||
        (mono_after.tv_sec == mono_before.tv_sec &&
         (long)(mono_after.tv_nsec - mono_before.tv_nsec) < 5000000L)) {
        return 26;
    }
    if (write(1, "BoarOS: real userland sleep checks ok\n", 38) != 38) {
        return 27;
    }

    /* Console input: stdin is the UART.  The harness feeds a line after
     * boot; the read must block until it arrives, then deliver it. */
    char line[8] = {0};
    ssize_t got = read(0, line, 1);
    if (got != 1 || line[1] != 0) {
        return 28;
    }
    got = read(0, line + 1, sizeof(line) - 1);
    if (got < 1 || line[0] != 'g' || line[1] != 'o') {
        return 29;
    }
    if (write(1, "BoarOS: real userland console input ok\n", 40) != 40) {
        return 30;
    }

    if (check_fork_fp()) {
        return 77;
    }

    /* Floating point: forked workers must each see their own FP register
     * image survive preemption, and the parent's FP execution must be
     * unaffected by the fork/wait cycle.  A runtime division pins the
     * rounding state: its bit pattern must be identical before and after. */
    volatile double one = 1.0;
    double third_before = one / 3.0;
    unsigned char before_bits[sizeof(double)];
    memcpy(before_bits, &third_before, sizeof(before_bits));
    pid_t first = fork();
    if (first < 0) {
        return 31;
    }
    if (first == 0) {
        _exit(fp_worker(1));
    }
    pid_t second = fork();
    if (second < 0) {
        return 32;
    }
    if (second == 0) {
        _exit(fp_worker(2));
    }
    int first_status = 0;
    int second_status = 0;
    if (waitpid(first, &first_status, 0) != first ||
        !WIFEXITED(first_status) || WEXITSTATUS(first_status) != 0) {
        return 33;
    }
    if (waitpid(second, &second_status, 0) != second ||
        !WIFEXITED(second_status) || WEXITSTATUS(second_status) != 0) {
        return 34;
    }
    volatile double one_again = 1.0;
    double third_after = one_again / 3.0;
    unsigned char after_bits[sizeof(double)];
    memcpy(after_bits, &third_after, sizeof(after_bits));
    if (memcmp(before_bits, after_bits, sizeof(before_bits)) != 0) {
        return 35;
    }
    static const char fp_marker[] = "BoarOS: real userland fp checks ok\n";
    if (write(1, fp_marker, sizeof(fp_marker) - 1) !=
        (ssize_t)(sizeof(fp_marker) - 1)) {
        return 36;
    }

    /* Signal delivery must build a Linux-compatible frame, return through
     * rt_sigreturn, and preserve blocked/pending state across the handler. */
    struct sigaction signal_action = {0};
    sigemptyset(&signal_action.sa_mask);
    signal_action.sa_handler = user_signal_handler;
    if (sigaction(SIGUSR1, &signal_action, 0) != 0) {
        return 37;
    }
    user_signal_seen = 0;
    if (kill(getpid(), SIGUSR1) != 0 || user_signal_seen != SIGUSR1) {
        return 38;
    }
    sigset_t signal_set;
    sigset_t signal_old;
    sigset_t signal_pending;
    sigemptyset(&signal_set);
    sigaddset(&signal_set, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &signal_set, &signal_old) != 0 ||
        kill(getpid(), SIGUSR1) != 0 ||
        sigpending(&signal_pending) != 0 ||
        !sigismember(&signal_pending, SIGUSR1) ||
        sigprocmask(SIG_SETMASK, &signal_old, 0) != 0 ||
        user_signal_seen != SIGUSR1) {
        return 39;
    }

    /* SIGCHLD's default disposition is ignored even after the lazy
     * disposition table has been allocated for another signal. */
    sigemptyset(&signal_pending);
    if (kill(getpid(), SIGCHLD) != 0 || sigpending(&signal_pending) != 0 ||
        sigismember(&signal_pending, SIGCHLD) != 0) {
        return 72;
    }

    /* Stop/continue events are observable through wait4, and a later
     * default SIGTERM remains a terminating child status. */
    pid_t stopped_child = fork();
    if (stopped_child < 0) {
        return 73;
    }
    if (stopped_child == 0) {
        raise(SIGSTOP);
        _exit(94);
    }
    int stopped_status = 0;
    if (waitpid(stopped_child, &stopped_status, WUNTRACED) !=
            stopped_child ||
        !WIFSTOPPED(stopped_status) || WSTOPSIG(stopped_status) != SIGSTOP ||
        kill(stopped_child, SIGCONT) != 0 ||
        waitpid(stopped_child, &stopped_status, WCONTINUED) !=
            stopped_child ||
        !WIFCONTINUED(stopped_status) ||
        kill(stopped_child, SIGTERM) != 0 ||
        waitpid(stopped_child, &stopped_status, 0) != stopped_child ||
        !WIFSIGNALED(stopped_status) ||
        WTERMSIG(stopped_status) != SIGTERM) {
        return 74;
    }

    /* Core-producing default actions carry the Linux wait status core bit. */
    pid_t core_child = fork();
    if (core_child < 0) {
        return 75;
    }
    if (core_child == 0) {
        raise(SIGQUIT);
        _exit(95);
    }
    int core_status = 0;
    if (waitpid(core_child, &core_status, 0) != core_child ||
        !WIFSIGNALED(core_status) || WTERMSIG(core_status) != SIGQUIT ||
        (core_status & 0x80) == 0) {
        return 76;
    }

    /* An interruptible nanosleep must expose EINTR and its remaining time
     * when the handler does not request SA_RESTART. */
    signal_action.sa_flags = 0;
    if (sigaction(SIGUSR1, &signal_action, 0) != 0) {
        return 45;
    }
    pid_t signal_child = fork();
    if (signal_child < 0) {
        return 46;
    }
    if (signal_child == 0) {
        signal_parent_after_delay(getppid(), SIGUSR1);
    }
    struct timespec interrupted_pause = {
        .tv_sec = 0,
        .tv_nsec = 100000000L,
    };
    struct timespec remaining_pause = {0};
    user_signal_seen = 0;
    errno = 0;
    if (nanosleep(&interrupted_pause, &remaining_pause) != -1 ||
        errno != EINTR || user_signal_seen != SIGUSR1 ||
        (remaining_pause.tv_sec == 0 && remaining_pause.tv_nsec == 0)) {
        return 47;
    }
    int signal_child_status = 0;
    if (waitpid(signal_child, &signal_child_status, 0) != signal_child ||
        !WIFEXITED(signal_child_status) ||
        WEXITSTATUS(signal_child_status) != 0) {
        return 48;
    }

    /* Handler delivery interrupts nanosleep even with SA_RESTART. A huge
     * valid duration must wait for that signal, not wrap into the past. */
    signal_action.sa_flags = SA_RESTART;
    if (sigaction(SIGUSR1, &signal_action, 0) != 0) {
        return 49;
    }
    signal_child = fork();
    if (signal_child < 0) {
        return 50;
    }
    if (signal_child == 0) {
        signal_parent_after_delay(getppid(), SIGUSR1);
    }
    interrupted_pause.tv_sec = 10000000000L;
    interrupted_pause.tv_nsec = 0;
    user_signal_seen = 0;
    if (nanosleep(&interrupted_pause, 0) != -1 || errno != EINTR ||
        user_signal_seen != SIGUSR1) {
        return 51;
    }
    if (waitpid(signal_child, &signal_child_status, 0) != signal_child ||
        !WIFEXITED(signal_child_status) ||
        WEXITSTATUS(signal_child_status) != 0) {
        return 52;
    }

    /* sigsuspend swaps in a temporary mask, wakes on the unblocked signal,
     * and restores the caller's mask before returning EINTR. */
    signal_action.sa_flags = 0;
    if (sigaction(SIGUSR1, &signal_action, 0) != 0) {
        return 53;
    }
    sigset_t suspend_mask;
    sigemptyset(&suspend_mask);
    if (sigprocmask(SIG_BLOCK, &signal_set, &signal_old) != 0) {
        return 78;
    }
    signal_child = fork();
    if (signal_child < 0) {
        return 54;
    }
    if (signal_child == 0) {
        signal_parent_after_delay(getppid(), SIGUSR1);
    }
    user_signal_seen = 0;
    errno = 0;
    if (sigsuspend(&suspend_mask) != -1 || errno != EINTR ||
        user_signal_seen != SIGUSR1) {
        return 55;
    }
    if (sigprocmask(SIG_SETMASK, 0, &signal_pending) != 0 ||
        !sigismember(&signal_pending, SIGUSR1) ||
        sigprocmask(SIG_SETMASK, &signal_old, 0) != 0) {
        return 79;
    }
    if (waitpid(signal_child, &signal_child_status, 0) != signal_child ||
        !WIFEXITED(signal_child_status) ||
        WEXITSTATUS(signal_child_status) != 0) {
        return 56;
    }

    static const char signal_marker[] =
        "BoarOS: real userland signal checks ok\n";
    if (write(1, signal_marker, sizeof(signal_marker) - 1) !=
        (ssize_t)(sizeof(signal_marker) - 1)) {
        return 40;
    }
    int lifecycle_result = check_signal_context_and_lifecycle();
    if (lifecycle_result != 0) {
        fprintf(stderr, "signal lifecycle check failed: %d\n", lifecycle_result);
        return 80;
    }

    /* pipe2 supplies two independent descriptors over one bounded ring. */
    int pipe_fds[2];
    static const char pipe_message[] = "pipe message";
    char pipe_buffer[sizeof(pipe_message)];
    if (pipe2(pipe_fds, 0) != 0 ||
        write(pipe_fds[1], pipe_message, sizeof(pipe_message)) !=
            (ssize_t)sizeof(pipe_message) ||
        read(pipe_fds[0], pipe_buffer, sizeof(pipe_buffer)) !=
            (ssize_t)sizeof(pipe_buffer) ||
        memcmp(pipe_buffer, pipe_message, sizeof(pipe_message)) != 0 ||
        close(pipe_fds[0]) != 0 || close(pipe_fds[1]) != 0) {
        return 41;
    }
    int nonblocking_fds[2];
    char one_byte;
    if (pipe2(nonblocking_fds, O_NONBLOCK) != 0 ||
        read(nonblocking_fds[0], &one_byte, 1) != -1 || errno != EAGAIN ||
        close(nonblocking_fds[0]) != 0 ||
        close(nonblocking_fds[1]) != 0) {
        return 42;
    }

    /* Closing the last writer turns buffered data into EOF, and a pipe has
     * FIFO stat/type semantics and cannot be repositioned. */
    int eof_fds[2];
    char eof_buffer[sizeof(pipe_message)];
    struct stat pipe_status;
    if (pipe(eof_fds) != 0 ||
        write(eof_fds[1], pipe_message, sizeof(pipe_message)) !=
            (ssize_t)sizeof(pipe_message) || close(eof_fds[1]) != 0 ||
        read(eof_fds[0], eof_buffer, sizeof(eof_buffer)) !=
            (ssize_t)sizeof(eof_buffer) ||
        memcmp(eof_buffer, pipe_message, sizeof(pipe_message)) != 0 ||
        read(eof_fds[0], &one_byte, 1) != 0 || close(eof_fds[0]) != 0) {
        return 57;
    }
    if (pipe(pipe_fds) != 0 || fstat(pipe_fds[0], &pipe_status) != 0 ||
        !S_ISFIFO(pipe_status.st_mode) || pipe_status.st_size != 0) {
        return 58;
    }
    errno = 0;
    if (lseek(pipe_fds[0], 0, SEEK_SET) != -1 || errno != ESPIPE ||
        close(pipe_fds[0]) != 0 || close(pipe_fds[1]) != 0) {
        return 59;
    }

    /* F_SETFL changes the shared open-file status used by the pipe reader. */
    if (pipe(pipe_fds) != 0) {
        return 67;
    }
    errno = 0;
    if (fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK) != 0) {
        return 60;
    }
    errno = 0;
    if (read(pipe_fds[0], &one_byte, 1) != -1) {
        return 69;
    }
    if (errno != EAGAIN) {
        return 70;
    }
    if (close(pipe_fds[0]) != 0 || close(pipe_fds[1]) != 0) {
        return 71;
    }

    /* A reader that starts empty must sleep until a later writer produces
     * data; closing the parent's writer also makes the EOF edge precise. */
    int blocking_fds[2];
    static const char delayed_message[] = "delayed pipe";
    char delayed_buffer[sizeof(delayed_message)];
    if (pipe(blocking_fds) != 0) {
        return 61;
    }
    pid_t pipe_child = fork();
    if (pipe_child < 0) {
        return 62;
    }
    if (pipe_child == 0) {
        const struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000L };

        close(blocking_fds[0]);
        if (nanosleep(&delay, 0) != 0 ||
            write(blocking_fds[1], delayed_message,
                  sizeof(delayed_message)) !=
                (ssize_t)sizeof(delayed_message)) {
            _exit(91);
        }
        _exit(0);
    }
    close(blocking_fds[1]);
    if (read(blocking_fds[0], delayed_buffer, sizeof(delayed_buffer)) !=
            (ssize_t)sizeof(delayed_buffer) ||
        memcmp(delayed_buffer, delayed_message, sizeof(delayed_message)) != 0 ||
        close(blocking_fds[0]) != 0) {
        return 63;
    }
    int pipe_child_status = 0;
    if (waitpid(pipe_child, &pipe_child_status, 0) != pipe_child ||
        !WIFEXITED(pipe_child_status) ||
        WEXITSTATUS(pipe_child_status) != 0) {
        return 64;
    }

    /* With the default disposition, writing without readers terminates the
     * child with SIGPIPE after the kernel has closed its pipe endpoints. */
    struct sigaction default_action = {0};
    sigemptyset(&default_action.sa_mask);
    default_action.sa_handler = SIG_DFL;
    pid_t sigpipe_child = fork();
    if (sigpipe_child < 0) {
        return 65;
    }
    if (sigpipe_child == 0) {
        int child_pipe_fds[2];

        if (sigaction(SIGPIPE, &default_action, 0) != 0 ||
            pipe(child_pipe_fds) != 0 || close(child_pipe_fds[0]) != 0) {
            _exit(92);
        }
        (void)write(child_pipe_fds[1], "x", 1);
        _exit(93);
    }
    int sigpipe_status = 0;
    if (waitpid(sigpipe_child, &sigpipe_status, 0) != sigpipe_child ||
        !WIFSIGNALED(sigpipe_status) ||
        WTERMSIG(sigpipe_status) != SIGPIPE) {
        return 66;
    }

    signal_action.sa_handler = user_signal_handler;
    if (sigaction(SIGPIPE, &signal_action, 0) != 0 ||
        pipe(pipe_fds) != 0 || close(pipe_fds[0]) != 0 ||
        write(pipe_fds[1], "x", 1) != -1 || errno != EPIPE ||
        user_signal_seen != SIGPIPE || close(pipe_fds[1]) != 0) {
        return 43;
    }
    static const char pipe_marker[] =
        "BoarOS: real userland pipe checks ok\n";
    if (write(1, pipe_marker, sizeof(pipe_marker) - 1) !=
        (ssize_t)(sizeof(pipe_marker) - 1)) {
        return 44;
    }

    if (check_pipe_waiters() != 0) {
        return 81;
    }

    int poll_result = check_poll_and_select();
    if (poll_result != 0) {
        fprintf(stderr, "poll/select check failed: %d errno=%d\n",
                poll_result, errno);
        return 82;
    }

    int epoll_result = check_epoll();
    if (epoll_result != 0) {
        fprintf(stderr, "epoll check failed: %d errno=%d\n",
                epoll_result, errno);
        return 83;
    }

    return 42;
}
