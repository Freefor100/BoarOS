#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

static void continued(int signal_number)
{
    (void)signal_number;
    pid_t foreground = -1;
    static const char front[] = "TTY_GATE_FOREGROUND_RESUMED\n";
    static const char back[] = "TTY_GATE_BACKGROUND_RESUMED\n";
    /* 固定libc的raw syscall不取用户态锁，握手证明fg已经实际转交资格。 */
    if (syscall(SYS_ioctl, 0, TIOCGPGRP, &foreground) == 0 && foreground == getpgrp())
        (void)write(1, front, sizeof(front) - 1);
    else
        (void)write(1, back, sizeof(back) - 1);
}
int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "wait-foreground")) {
        char *end;
        long target = strtol(argv[2], &end, 10);
        if (*end || target <= 0 || target > INT32_MAX) return 2;
        char path[64];
        snprintf(path, sizeof(path), "/proc/%ld/stat", target);
        for (;;) {
            if (kill((pid_t)target, 0) && errno == ESRCH) return 5;
            char status[512];
            int fd = open(path, O_RDONLY);
            ssize_t n = fd < 0 ? -1 : read(fd, status, sizeof(status) - 1);
            if (fd >= 0) close(fd);
            if (n > 0) {
                status[n] = 0;
                char *tail = strrchr(status, ')');
                /* 实际原sleep已取得前台且进入睡眠，才允许宿主注入终端信号。 */
                if (tail && tail[1] == ' ' && tail[2] == 'S' &&
                    tcgetpgrp(0) == (pid_t)target) {
                    puts("TTY_GATE_SLEEP_READY");
                    return 0;
                }
            }
            sched_yield();
        }
    }
    if (argc != 2) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    int foreground = tcgetpgrp(0) == getpgrp();
    if (!strcmp(argv[1], "cpu")) {
        if (!foreground) return 3;
        puts("TTY_GATE_CPU");
        volatile uint64_t n = 0;
        for (;;) { n++; (void)n; }
    }
    if (!strcmp(argv[1], "sleep")) {
        if (!foreground) return 3;
        struct sigaction action = {.sa_handler = continued};
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGCONT, &action, NULL)) return 4;
        puts("TTY_GATE_SLEEP");
        for (;;) pause();
    }
    if (!strcmp(argv[1], "background-read")) {
        puts("TTY_GATE_BACKGROUND_READ");
        char byte;
        return read(0, &byte, 1) == 1 ? 0 : 5;
    }
    return 2;
}
