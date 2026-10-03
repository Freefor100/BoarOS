#define _GNU_SOURCE
#include <errno.h>
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
