/* 单项监督持有直接子进程和自己的测试进程组；不扫描或按名称终止任务。 */
#ifdef CASE_HOST
#define _GNU_SOURCE
#include <errno.h>
#include <sys/syscall.h>
#include <unistd.h>
#define NR_WRITE SYS_write
#define NR_READ SYS_read
#define NR_CLOSE SYS_close
#define NR_PIPE SYS_pipe2
#define NR_PID SYS_getpid
#define NR_PGID SYS_getpgid
#define NR_SETPGID SYS_setpgid
#define NR_TIME SYS_clock_gettime
#define NR_SLEEP SYS_nanosleep
#define NR_FORK SYS_fork
#define NR_EXEC SYS_execve
#define NR_WAIT SYS_wait4
#define NR_KILL SYS_kill
#define NR_EXIT SYS_exit_group
#else
#define NR_WRITE 64
#define NR_READ 63
#define NR_CLOSE 57
#define NR_PIPE 59
#define NR_PID 172
#define NR_PGID 155
#define NR_SETPGID 154
#define NR_TIME 113
#define NR_SLEEP 101
#define NR_FORK 220
#define NR_EXEC 221
#define NR_WAIT 260
#define NR_KILL 129
#define NR_EXIT 94
#endif

static long call(long number, long x, long y, long z, long u, long v, long w)
{
#ifdef CASE_HOST
    long result = syscall(number, x, y, z, u, v, w);
    return result < 0 ? -errno : result;
#else
    register long a0 __asm__("a0") = x;
    register long a1 __asm__("a1") = y;
    register long a2 __asm__("a2") = z;
    register long a3 __asm__("a3") = u;
    register long a4 __asm__("a4") = v;
    register long a5 __asm__("a5") = w;
    register long a7 __asm__("a7") = number;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3),
                     "r"(a4), "r"(a5), "r"(a7) : "memory");
    return a0;
#endif
}

static void message(const char *text)
{
    long length = 0;
    while (text[length]) length++;
    while (length) {
        long done = call(NR_WRITE, 2, (long)text, length, 0, 0, 0);
        if (done == -4) continue;
        if (done <= 0) return;
        text += done;
        length -= done;
    }
}

static void number(long value)
{
    char buffer[24];
    unsigned long n = value < 0 ? (unsigned long)(-(value + 1)) + 1 : (unsigned long)value;
    int pos = 23;
    buffer[pos] = 0;
    do { buffer[--pos] = (char)('0' + n % 10); n /= 10; } while (n);
    if (value < 0) buffer[--pos] = '-';
    message(buffer + pos);
}

static long now(void)
{
    long time[2];
    long status = call(NR_TIME, 1, (long)time, 0, 0, 0, 0);
    return status < 0 ? status : time[0] * 1000 + time[1] / 1000000;
}

static void stop_group(long group, long child, int signal)
{
    /* group 是仍存活的监督者 PID；直接 child 只在 wait 回收前使用。 */
    call(NR_KILL, -group, signal, 0, 0, 0, 0);
    if (child > 0) call(NR_KILL, child, signal, 0, 0, 0, 0);
}

static int run(int argc, char **argv, char **envp)
{
    if (argc < 4 || argc > 27) return 125;
    long limit = 0;
    for (const char *s = argv[1]; *s; s++) {
        if (*s < '0' || *s > '9' || limit > 86400) return 125;
        limit = limit * 10 + *s - '0';
    }
    if (!argv[1][0] || limit > 86400) return 125;
    long start = now();
    if (start < 0) return 125;
    long group = call(NR_PID, 0, 0, 0, 0, 0, 0);
    long original_group = call(NR_PGID, 0, 0, 0, 0, 0, 0);
    int ready[2];
    if (group <= 0 || original_group <= 0 ||
        call(NR_PIPE, (long)ready, 0, 0, 0, 0, 0) < 0) return 125;
    if (call(NR_SETPGID, 0, group, 0, 0, 0, 0) < 0) {
        call(NR_CLOSE, ready[0], 0, 0, 0, 0, 0);
        call(NR_CLOSE, ready[1], 0, 0, 0, 0, 0);
        return 125;
    }
    long child = call(NR_FORK, 17, 0, 0, 0, 0, 0);
    if (child < 0) {
        call(NR_SETPGID, 0, original_group, 0, 0, 0, 0);
        call(NR_CLOSE, ready[0], 0, 0, 0, 0, 0);
        call(NR_CLOSE, ready[1], 0, 0, 0, 0, 0);
        return 125;
    }
    if (!child) {
        call(NR_CLOSE, ready[1], 0, 0, 0, 0, 0);
        char token;
        long received;
        do { received = call(NR_READ, ready[0], (long)&token, 1, 0, 0, 0); }
        while (received == -4);
        call(NR_CLOSE, ready[0], 0, 0, 0, 0, 0);
        if (received != 1) {
            call(NR_EXIT, 125, 0, 0, 0, 0, 0);
            for (;;) {}
        }
        char *arguments[32];
        arguments[0] = argv[2];
        arguments[1] = "sh";
        arguments[2] = "-c";
        arguments[3] = "exec \"$@\"";
        arguments[4] = "boaros-case";
#ifdef CASE_HOST
        arguments[1] = "-c";
        arguments[2] = "exec \"$@\"";
        arguments[3] = "boaros-case";
        unsigned base = 4;
#else
        unsigned base = 5;
#endif
        for (int i = 3; i < argc; i++) arguments[base++] = argv[i];
        arguments[base] = 0;
        long error = call(NR_EXEC, (long)argv[2], (long)arguments, (long)envp, 0, 0, 0);
        message("BOAROS-CASE EXEC-ERROR errno="); number(-error); message("\n");
        call(NR_EXIT, 127, 0, 0, 0, 0, 0);
        for (;;) {}
    }
    call(NR_CLOSE, ready[0], 0, 0, 0, 0, 0);
    /* 子进程继承隔离组但不是组长，仍可原样测试 setsid。监督者先退出该组。 */
    int setup_failed = call(NR_SETPGID, 0, original_group, 0, 0, 0, 0) < 0;
    if (!setup_failed) {
        char token = 1;
        long sent;
        do { sent = call(NR_WRITE, ready[1], (long)&token, 1, 0, 0, 0); }
        while (sent == -4);
        setup_failed = sent != 1;
    }
    call(NR_CLOSE, ready[1], 0, 0, 0, 0, 0);
    int status = 0, expired = 0, killed = 0, clock_failed = 0;
    long grace = 0, deadline_elapsed = -1;
    for (;;) {
        long waited = call(NR_WAIT, child, (long)&status, 1, 0, 0, 0);
        if (waited == child) break;
        if (waited == -4) continue;
        if (waited < 0) {
            message("BOAROS-CASE WAIT-ERROR errno="); number(-waited); message("\n");
            return 125;
        }
        long time = now();
        if (!expired && (time < 0 || (limit && time - start >= limit * 1000))) {
            expired = 1;
            clock_failed = time < 0;
            if (time >= start) deadline_elapsed = time - start;
            message("BOAROS-CASE TIMEOUT seconds="); number(limit);
            message(" command="); message(argv[3]); message("\n");
            /* wait 尚未回收 PID；即使子进程此刻退出，也不能误杀复用者。 */
            stop_group(group, child, clock_failed ? 9 : 15);
            grace = time + 2000;
        }
        if (expired && !killed && (time < 0 || time >= grace)) {
            stop_group(group, child, 9);
            killed = 1;
        }
        long delay[2] = {0, 20000000};
        call(NR_SLEEP, (long)delay, 0, 0, 0, 0, 0);
    }
    /* 回收直接 child 后不再使用其 PID；监督者仍保护隔离组的身份。 */
    if (!setup_failed) stop_group(group, 0, 9);
    if (setup_failed) return 125;
    if (expired) {
        message("BOAROS-CASE TIMEOUT-END wait_status="); number(status);
        /* 只保留数值诊断；child 已回收，不能再按其 PID 发信号或保活。 */
        message(" child_pid="); number(child);
        message(" test_pgid="); number(group);
        message(" deadline_elapsed_ms=");
        if (deadline_elapsed >= 0) number(deadline_elapsed); else message("unavailable");
        message(" total_elapsed_ms=");
        long end = now();
        if (end >= start) number(end - start); else message("unavailable");
        message("\n");
        return clock_failed ? 125 : 124;
    }
    return status & 127 ? 128 + (status & 127) : (status >> 8) & 255;
}

#ifdef CASE_HOST
int main(int argc, char **argv, char **envp) { return run(argc, argv, envp); }
#else
__asm__(".section .text.start,\"ax\"\n.global _start\n_start:\n"
        "mv a0,sp\nandi sp,sp,-16\ncall entry\nli a7,94\necall\n1: j 1b\n");
int entry(const unsigned long *sp)
{
    char **argv = (void *)(sp + 1);
    return run((int)sp[0], argv, argv + sp[0] + 1);
}
#endif
