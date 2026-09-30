#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Linked against the unchanged original image libc, not the host libc. */
int main(void)
{
    FILE *f = tmpfile();
    if (!f) return 1;
    for (unsigned i = 0; i < 8; i++) {
        struct timespec before, after, coarse = {0}, times[2] = {{0, UTIME_NOW}, {0, UTIME_NOW}};
        struct stat st;
        errno = 0;
        time_t t = time(NULL);
        int time_errno = errno;
        errno = 0;
        int coarse_rc = clock_gettime(CLOCK_REALTIME_COARSE, &coarse);
        int coarse_errno = errno;
        clock_gettime(CLOCK_REALTIME, &before);
        int rc = futimens(fileno(f), times);
        if (fstat(fileno(f), &st)) return 2;
        clock_gettime(CLOCK_REALTIME, &after);
        printf("PROBE original-libc-time t=%ld errno=%d coarse_rc=%d coarse_errno=%d before=%ld.%09ld now_rc=%d atime=%ld.%09ld mtime=%ld.%09ld after=%ld.%09ld\n",
            (long)t, time_errno, coarse_rc, coarse_errno, before.tv_sec, before.tv_nsec, rc,
            st.st_atim.tv_sec, st.st_atim.tv_nsec, st.st_mtim.tv_sec, st.st_mtim.tv_nsec,
            after.tv_sec, after.tv_nsec);
        struct timespec delay = {0, 300000000};
        nanosleep(&delay, NULL);
    }
    fclose(f);
    return 0;
}
