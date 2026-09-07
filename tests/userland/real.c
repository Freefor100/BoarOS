/* The first compiler-generated BoarOS user program: a static musl
 * binary exercising stdio, directory enumeration, regular-file reads,
 * descriptor duplication, and the clock ABI against the Linux surface. */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
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
    char line[8];
    ssize_t got = read(0, line, sizeof(line));
    if (got < 1) {
        return 28;
    }
    if (line[0] != 'g' || line[1] != 'o') {
        return 29;
    }
    if (write(1, "BoarOS: real userland console input ok\n", 40) != 40) {
        return 30;
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

    /* SA_RESTART must resume the same absolute sleep deadline through
     * restart_syscall rather than returning an EINTR to libc. */
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
    interrupted_pause.tv_nsec = 100000000L;
    user_signal_seen = 0;
    if (nanosleep(&interrupted_pause, 0) != 0 ||
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

    return 42;
}
