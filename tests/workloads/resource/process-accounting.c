#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/acct.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL %d %s errno=%d\n", __LINE__, #expr, errno); failures++; } } while (0)

static int failures;

static long account(const char *path)
{
    return syscall(SYS_acct, path);
}

static void check_error(const char *path, int wanted)
{
    errno = 0;
    CHECK(account(path) == -1 && errno == wanted);
}

static int wait_status(pid_t child, int code)
{
    int status;
    return waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == code;
}

static void *worker(void *unused)
{
    (void)unused;
    unsigned char *memory = malloc(65536);
    if (!memory) return 0;
    volatile unsigned char *p = memory;
    for (size_t i = 0; i < 65536; i += 4096)
        p[i] = 1;
    free(memory);
    return 0;
}

static pid_t spawn_one(const char *name, char *program, int status)
{
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        char code[12];
        snprintf(code, sizeof(code), "%d", status);
        execl(program, program, name, code, (char *)0);
        _exit(99);
    }
    CHECK(wait_status(child, status));
    return child;
}

static unsigned count_record(int fd, pid_t pid, pid_t ppid, const char *comm, int code)
{
    struct acct_v3 record;
    unsigned matches = 0;
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    ssize_t n;
    while ((n = read(fd, &record, sizeof(record))) > 0) {
        CHECK(n == sizeof(record));
        if (n != sizeof(record)) break;
        printf("record pid=%u ppid=%u cmd=%s exit=%u v=%u flags=%u mem=%u\n",
            record.ac_pid, record.ac_ppid, record.ac_comm, record.ac_exitcode,
            record.ac_version, record.ac_flag, record.ac_mem);
        CHECK(record.ac_version == 3);
        if (record.ac_pid != (unsigned)pid || strcmp(record.ac_comm, comm)) continue;
        matches++;
        CHECK(record.ac_ppid == (unsigned)ppid);
        CHECK(record.ac_exitcode == (unsigned)code * 256);
        CHECK(record.ac_uid == 0 && record.ac_gid == 0);
        CHECK(record.ac_btime > 1700000000U && record.ac_btime <= (unsigned)time(0));
        CHECK(!strcmp(record.ac_comm, comm));
        CHECK(record.ac_mem > 0);
        CHECK(record.ac_etime >= 0.0f);
    }
    CHECK(n == 0);
    return matches;
}

static int nested_func(void *program)
{
    int file = open("/acct-inner", O_CREAT|O_TRUNC|O_RDWR, 0600);
    CHECK(file >= 0 && account("/acct-inner") == 0);
    pid_t pid = spawn_one("nested-worker", program, 22);
    CHECK(pid == 2);
    CHECK(account(0) == 0);
    CHECK(count_record(file, pid, 1, "nested-worker", 22) == 1);
    close(file);
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc == 3) {
        pthread_t thread;
        CHECK(prctl(PR_SET_NAME, argv[1]) == 0);
        CHECK(pthread_create(&thread, 0, worker, 0) == 0);
        CHECK(pthread_join(thread, 0) == 0);
        return failures ? 99 : atoi(argv[2]);
    }
    CHECK(account(0) == 0);
    check_error("/", EISDIR);
    check_error("/missing-path", ENOENT);
    check_error((char *)1, EFAULT);
    int old = open("/acct-old", O_CREAT|O_TRUNC|O_RDWR, 0600);
    int next = open("/acct-new", O_CREAT|O_TRUNC|O_RDWR, 0600);
    CHECK(old >= 0 && next >= 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        if (seteuid(65534)) _exit(97);
        if (account(0) != -1 || errno != EPERM) _exit(98);
        if (account("/acct-old") != -1 || errno != EPERM) _exit(99);
        _exit(0);
    }
    CHECK(wait_status(child, 0));
    CHECK(account("/acct-old") == 0);
    CHECK(ftruncate(old, 0) == 0);
    pid_t pid_a = spawn_one("first-worker", argv[0], 37);
    check_error("/missing-path", ENOENT);
    CHECK(account("/acct-new") == 0);
    pid_t pid_b = spawn_one("second-worker", argv[0], 39);
    static char clone_stack[65536];
    pid_t ns = clone(nested_func, clone_stack + sizeof(clone_stack),
                     CLONE_NEWPID | SIGCHLD, argv[0]);
    CHECK(ns >= 0 && wait_status(ns, 0));
    CHECK(account(0) == 0 && account(0) == 0);
    CHECK(count_record(old, pid_a, getpid(), "first-worker", 37) == 1);
    CHECK(count_record(next, pid_b, getpid(), "second-worker", 39) == 1);
    struct acct_v3 records[8] = {0};
    CHECK(pread(next, records, sizeof(records), 0) >= 256);
    CHECK(!strcmp(records[1].ac_comm, "nested-worker") && records[1].ac_ppid == (unsigned)ns);
    close(old);
    close(next);

    CHECK(mkdir("/mnt", 0755) == 0 || errno == EEXIST);
    CHECK(mount("tmpfs", "/mnt", "tmpfs", 0, "size=1m") == 0);
    int mounted = open("/mnt/file", O_CREAT|O_RDWR, 0600);
    CHECK(mounted >= 0);
    CHECK(account("/mnt/file") == 0);
    CHECK(umount("/mnt") == -1 && errno == EBUSY);
    pid_t pid_m = spawn_one("mnt-worker", argv[0], 41);
    CHECK(count_record(mounted, pid_m, getpid(), "mnt-worker", 41) == 1);
    close(mounted);
    CHECK(umount("/mnt") == 0);
    CHECK(mount("tmpfs", "/mnt", "tmpfs", 0, "size=1m") == 0);
    mounted = open("/mnt/file", O_CREAT|O_RDWR, 0600);
    CHECK(mounted >= 0 && account("/mnt/file") == 0);
    CHECK(umount2("/mnt", MNT_DETACH) == 0);
    (void)spawn_one("off-worker", argv[0], 43);
    CHECK(lseek(mounted, 0, SEEK_END) == 0);
    close(mounted);
    CHECK(account("/acct-old") == 0);
    puts(failures ? "process accounting FAIL" : "process accounting PASS");
    fflush(stdout);
    reboot(RB_POWER_OFF);
    return failures != 0;
}
