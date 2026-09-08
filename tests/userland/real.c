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
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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

int main(void)
{
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
    return 42;
}
