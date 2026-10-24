#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/msg.h>
#include <sys/reboot.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int msg = -1, sem = -1, shm = -1;
static void *attached = (void *)-1;
union argument { int val; struct semid_ds *buf; };
static void cleanup(void)
{
    if (attached != (void *)-1) shmdt(attached);
    if (msg >= 0) msgctl(msg, IPC_RMID, 0);
    if (sem >= 0) semctl(sem, 0, IPC_RMID);
    if (shm >= 0) shmctl(shm, IPC_RMID, 0);
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "IPC-PROC FAIL line=%d errno=%d %s\n", __LINE__, errno, #x); exit(1); } } while (0)
static unsigned cases;
static void pass(const char *name) { cases++; printf("IPC-PROC CASE %s PASS\n", name); }
static int listing(const char *kind)
{
    char path[64]; snprintf(path, sizeof(path), "/proc/sysvipc/%s", kind);
    int fd = open(path, O_RDONLY | O_CLOEXEC); CHECK(fd >= 0);
    struct stat st; CHECK(!fstat(fd, &st) && S_ISREG(st.st_mode) && (st.st_mode & 0777) == 0444);
    return fd;
}
static char *contents(int fd, const char *prefix, size_t prefix_length)
{
    size_t capacity = 8192, used = prefix_length;
    char *text = malloc(capacity); CHECK(text && used < capacity);
    if (used) memcpy(text, prefix, used);
    for (;;) {
        if (used + 1025 > capacity) { capacity *= 2; CHECK(capacity <= 1024 * 1024); text = realloc(text, capacity); CHECK(text); }
        ssize_t size = read(fd, text + used, 1024); CHECK(size >= 0);
        if (!size) break;
        used += (size_t)size;
    }
    text[used] = 0;
    return text;
}
static int find(char *text, int id, uint64_t *values, unsigned fields)
{
    char *next, *line = strtok_r(text, "\n", &next);
    CHECK(line && strstr(line, "key"));
    int found = 0;
    while ((line = strtok_r(0, "\n", &next))) {
        char *at = line, *end;
        for (unsigned i = 0; i < fields; i++) {
            errno = 0;
            values[i] = i == 0 ? (uint64_t)strtoll(at, &end, 10) : strtoull(at, &end, i == 2 ? 8 : 10);
            CHECK(end != at && !errno); at = end;
        }
        while (*at == ' ' || *at == '\t') at++;
        CHECK(!*at);
        if (values[1] == (uint64_t)id) { found++; break; }
    }
    return found;
}
static int row(const char *kind, int id, uint64_t *values, unsigned fields)
{
    int fd = listing(kind); char *text = contents(fd, 0, 0); CHECK(!close(fd));
    int result = find(text, id, values, fields); free(text); return result;
}
int main(void)
{
    setvbuf(stdout, 0, _IONBF, 0); CHECK(!atexit(cleanup)); alarm(30);
    if (access("/proc/self/stat", F_OK)) {
        if (mkdir("/proc", 0755)) CHECK(errno == EEXIST);
        CHECK(!mount("proc", "/proc", "proc", 0, 0));
    }
    DIR *dir = opendir("/proc/sysvipc"); CHECK(dir);
    unsigned names = 0; struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, "shm")) names |= 1;
        if (!strcmp(entry->d_name, "sem")) names |= 2;
        if (!strcmp(entry->d_name, "msg")) names |= 4;
    }
    CHECK(names == 7 && !closedir(dir));
    long page = sysconf(_SC_PAGESIZE); CHECK(page > 0);
    msg = msgget(IPC_PRIVATE, 0600); sem = semget(IPC_PRIVATE, 2, 0600);
    key_t key = (key_t)(UINT32_C(0x90000000) | ((uint32_t)getpid() & UINT32_C(0x0fffffff)));
    shm = shmget(key, (size_t)page * 2, IPC_CREAT | IPC_EXCL | 0600);
    CHECK(msg >= 0 && sem >= 0 && shm >= 0);
    struct { long type; char text[3]; } message = {7, {'i', 'p', 'c'}};
    CHECK(!msgsnd(msg, &message, sizeof(message.text), 0));
    struct sembuf op = {0, 1, 0}; CHECK(!semop(sem, &op, 1));
    attached = shmat(shm, 0, 0); CHECK(attached != (void *)-1); *(volatile char *)attached = 42;
    struct msqid_ds m; struct semid_ds s; struct shmid_ds h;
    CHECK(!msgctl(msg, IPC_STAT, &m) && !semctl(sem, 0, IPC_STAT, (union argument){.buf = &s}) && !shmctl(shm, IPC_STAT, &h));
    uint64_t v[16];
    CHECK(row("msg", msg, v, 14) && !v[0] && v[2] == 0600 && v[3] == 3 && v[4] == 1 &&
        v[5] == (uint64_t)getpid() && !v[6] && v[7] == m.msg_perm.uid && v[8] == m.msg_perm.gid &&
        v[9] == m.msg_perm.cuid && v[10] == m.msg_perm.cgid && v[11] == (uint64_t)m.msg_stime &&
        v[12] == (uint64_t)m.msg_rtime && v[13] == (uint64_t)m.msg_ctime);
    CHECK(row("sem", sem, v, 10) && !v[0] && v[2] == 0600 && v[3] == 2 &&
        v[4] == s.sem_perm.uid && v[5] == s.sem_perm.gid && v[6] == s.sem_perm.cuid &&
        v[7] == s.sem_perm.cgid && v[8] == (uint64_t)s.sem_otime && v[9] == (uint64_t)s.sem_ctime);
    CHECK(row("shm", shm, v, 16) && v[0] == (uint64_t)(int64_t)key && v[2] == 0600 && v[3] == (uint64_t)page * 2 &&
        v[4] == (uint64_t)h.shm_cpid && v[5] == (uint64_t)h.shm_lpid && v[6] == h.shm_nattch &&
        v[7] == h.shm_perm.uid && v[8] == h.shm_perm.gid && v[9] == h.shm_perm.cuid && v[10] == h.shm_perm.cgid &&
        v[11] == (uint64_t)h.shm_atime && v[12] == (uint64_t)h.shm_dtime && v[13] == (uint64_t)h.shm_ctime &&
        v[14] >= (uint64_t)page && v[14] <= h.shm_segsz && !v[15]);
    pass("directory-and-real-descriptors");

    int fd = listing("msg"); char prefix[7]; CHECK(read(fd, prefix, sizeof(prefix)) == sizeof(prefix));
    char *text = contents(fd, prefix, sizeof(prefix));
    CHECK(find(text, msg, v, 14) && v[3] == 3 && v[4] == 1); free(text);
    int removed = msg; CHECK(!msgctl(msg, IPC_RMID, 0)); msg = -1;
    CHECK(!lseek(fd, 0, SEEK_SET)); text = contents(fd, 0, 0);
    CHECK(!find(text, removed, v, 14)); free(text);
    errno = 0; CHECK(lseek(fd, 0, SEEK_END) == -1 && errno == EINVAL); CHECK(!close(fd));
    pass("message-short-read-rewind");

    msg = msgget(IPC_PRIVATE, 0000); CHECK(msg >= 0);
    s.sem_perm.mode = 0; h.shm_perm.mode = 0;
    CHECK(!semctl(sem, 0, IPC_SET, (union argument){.buf = &s}) && !shmctl(shm, IPC_SET, &h));
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        CHECK(!setgroups(0, 0) && !setgid(1001) && !setuid(1001));
        CHECK(row("msg", msg, v, 14) && !v[2]);
        CHECK(row("sem", sem, v, 10) && !v[2]);
        CHECK(row("shm", shm, v, 16) && !v[2]);
        _exit(0);
    }
    int status; CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    pass("unprivileged-global-metadata");

    CHECK(!shmctl(shm, IPC_RMID, 0));
    CHECK(!shmctl(shm, IPC_STAT, &h));
    key_t current_key;
    memcpy(&current_key, &h.shm_perm, sizeof(current_key)); /* Native ipc_perm begins with key. */
    CHECK(current_key == IPC_PRIVATE);
    CHECK(row("shm", shm, v, 16) && !v[0] && (v[2] & 01000) && v[6] == 1 && *(volatile char *)attached == 42);
    CHECK(!shmdt(attached)); attached = (void *)-1;
    CHECK(!row("shm", shm, v, 16)); shm = -1;
    pass("removed-shm-attached-and-final-release");
    CHECK(!msgctl(msg, IPC_RMID, 0) && !semctl(sem, 0, IPC_RMID));
    CHECK(!row("msg", msg, v, 14) && !row("sem", sem, v, 10)); msg = sem = -1;
    pass("sem-and-msg-removal");
    printf("IPC-PROC END %u\n", cases); alarm(0);
    if (getpid() == 1) { sync(); reboot(RB_POWER_OFF); }
    return 0;
}
