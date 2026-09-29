#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { dprintf(2, "multi-disk failure line=%d errno=%d: %s\n", __LINE__, errno, #x); return 1; } } while (0)
static int token(void) { char c; return read(0, &c, 1) == 1 && c == 'g'; }
static int directory(const char *p) { return !mkdir(p,0755) || errno == EEXIST; }
static int verify(int fd, const char *value)
{
    char b[16] = {0};
    return pread(fd,b,strlen(value),0) == (ssize_t)strlen(value) && !memcmp(b,value,strlen(value));
}
int main(void)
{
    setvbuf(stdout, 0, _IONBF, 0);
    CHECK(directory("/second"));
    CHECK(!mknod("/disk-b", S_IFBLK|0600,makedev(252,16)) || errno == EEXIST);
    int phase = open("/phase", O_RDONLY);
    CHECK(!mount("/disk-b","/second","ext4",0,0));
    int a = open("/disk-a-data", phase >= 0 ? O_RDONLY : O_RDWR);
    int b = open("/second/disk-b-data",phase >= 0 ? O_RDONLY : O_RDWR);
    CHECK(a >= 0 && b >= 0);
    if (phase >= 0) {
        CHECK(verify(a,"A-FINAL"));
        CHECK(verify(b,"B-COLD") || verify(b,"B-FAULT") || verify(b,"B-FINAL"));
        CHECK(!close(phase) && !close(a) && !close(b) && !umount("/second"));
        puts("multi-disk: persistent readback ok");
        return 42;
    }
    CHECK(!fsync(b) && !fsync(a));
    int gate[2], start[2]; CHECK(!pipe(gate) && !pipe(start));
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        close(gate[0]);
        close(start[1]);
        char go;
        if (read(start[0], &go, 1) != 1) _exit(4);
        close(start[0]);
        int good = verify(b,"B-COLD");
        char c = good ? 'y' : 'n';
        if (write(gate[1],&c,1) != 1) _exit(2);
        _exit(good ? 0 : 3);
    }
    CHECK(!close(gate[1]) && !close(start[0]));
    puts("multi-disk: hold ready"); CHECK(token());
    CHECK(write(start[1], "g", 1) == 1 && !close(start[1]));
    /* Host sends this only after the secondary NBD has an unreplied READ. */
    CHECK(token());
    puts("multi-disk: parent token received");
    volatile uint64_t sum = 0;
    for (unsigned i=0;i<100000;i++) sum += i;
    CHECK(sum == UINT64_C(4999950000));
    puts("multi-disk: computation progressed");
    CHECK(verify(a,"A-COLD"));
    puts("multi-disk: A cold read ok");
    CHECK(pwrite(a,"A-HELD",6,0) == 6 && !fsync(a) && verify(a,"A-HELD"));
    puts("multi-disk: A progressed while B held");
    char c; int status;
    CHECK(read(gate[0],&c,1) == 1 && c == 'y' && !close(gate[0]));
    CHECK(waitpid(child,&status,0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    puts("multi-disk: fault ready"); CHECK(token());
    errno = 0;
    ssize_t written = pwrite(b,"B-FAULT",7,0);
    int write_error = written < 0 ? errno : 0;
    int sync_result = fsync(b), sync_error = sync_result < 0 ? errno : 0;
    CHECK((write_error == EIO || sync_error == EIO) && (written == 7 || written == -1));
    puts("multi-disk: B observed EIO");
    CHECK(pwrite(a,"A-FINAL",7,0) == 7 && !fsync(a) && verify(a,"A-FINAL"));
    puts("multi-disk: A healthy after B error");
    int recovered = 0;
    for (unsigned retry=0;retry<4;retry++) {
        if (pwrite(b,"B-FINAL",7,0) == 7 && !fsync(b)) { recovered=1; break; }
        CHECK(errno == EIO);
    }
    if (recovered) {
        CHECK(verify(b,"B-FINAL") && !close(b) && !umount("/second"));
        puts("multi-disk: B owner retry ok");
    } else {
        CHECK(!close(b));
        errno = 0;
        CHECK(umount("/second") == -1 && errno == EIO);
        errno = 0;
        CHECK(umount("/second") == -1 && errno == EIO);
        CHECK(directory("/spare"));
        errno = 0;
        CHECK(mount("/disk-b", "/spare", "ext4", 0, 0) == -1 && errno == EBUSY);
        puts("multi-disk: B failed unmount remains reachable and claimed");
    }
    phase = open("/phase",O_CREAT|O_WRONLY,0600);
    CHECK(phase >= 0 && write(phase,"done",4) == 4 && !fsync(phase) && !close(phase) && !close(a));
    if (!recovered) {
        /* Sticky journal errors keep the actual mount owner until reset. */
        puts("multi-disk: B isolated owner retained");
        (void)token();
        return 1;
    }
    puts("multi-disk: isolation ok");
    return 42;
}
