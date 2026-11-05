#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/klog.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { perror(#x); exit(1); } } while (0)
static void node(const char *path, unsigned major_number, unsigned minor_number)
{
    CHECK(mknod(path, S_IFCHR | 0600, makedev(major_number, minor_number)) == 0 || errno == EEXIST);
}
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    CHECK(mkdir("/dev", 0755) == 0 || errno == EEXIST);
    CHECK(mkdir("/proc", 0755) == 0 || errno == EEXIST);
    CHECK(mkdir("/tmp", 0777) == 0 || errno == EEXIST);
    CHECK(mount("proc", "/proc", "proc", 0, NULL) == 0);
    node("/dev/ttyS0", 4, 64); node("/dev/tty", 5, 0);
    node("/dev/console", 5, 1); node("/dev/null", 1, 3);
    node("/dev/zero", 1, 5);
    /* 控制显示级别，日志环继续保存；避免内核消息插入shell的ANSI编辑输出。 */
    CHECK(klogctl(6, NULL, 0) == 0);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        CHECK(setsid() == getpid());
        if (access("/probe-no-ctty", F_OK) != 0) {
            int fd = open("/dev/ttyS0", O_RDWR); CHECK(fd >= 0);
            CHECK(dup2(fd, 0) == 0 && dup2(fd, 1) == 1 && dup2(fd, 2) == 2);
            if (fd > 2) CHECK(close(fd) == 0);
        }
        CHECK(setenv("PATH", "/bin", 1) == 0);
        CHECK(setenv("LC_ALL", "C", 1) == 0);
        CHECK(setenv("TERM", "vt100", 1) == 0);
        CHECK(setenv("PS1", "BOAR_TTY$ ", 1) == 0);
        if (access("/probe", X_OK) == 0) execl("/probe", "probe", (char *)NULL);
        else execl("/busybox", "ash", "-i", (char *)NULL);
        perror("exec tty program"); _exit(127);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child);
    int fd = open("/tty-status", O_CREAT | O_WRONLY | O_TRUNC, 0600); CHECK(fd >= 0);
    char line[64]; int n = snprintf(line, sizeof(line), "wait_status=%d\n", status);
    CHECK(write(fd, line, (size_t)n) == n && fsync(fd) == 0 && close(fd) == 0);
    /* shell退出会hangup旧终端OFD；最终状态由普通文件核对，不依赖旧stdout。 */
    CHECK(klogctl(7, NULL, 0) == 0);
    sync();
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return 42;
}
