/* The first compiler-generated BoarOS user program: a static musl
 * binary exercising stdio, directory enumeration, regular-file reads,
 * descriptor duplication, and the clock ABI against the Linux surface. */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

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

    return 42;
}
