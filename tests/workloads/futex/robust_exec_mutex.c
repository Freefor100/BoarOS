/* Process-shared mutex recovery as observed by an unrelated lock owner. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { dprintf(2, "robust mutex: line %d: %s\n", __LINE__, #x); _exit(1); } } while (0)

enum action { LEADER_EXEC, NONLEADER_EXEC, FAILED_EXEC, OWNER_EXIT };
struct scenario {
    enum action action;
    const char *exe;
    pthread_mutex_t *mutex;
    int tid_fd;
};

static void *own(void *arg)
{
    struct scenario *s = arg;
    CHECK(!pthread_mutex_lock(s->mutex));
    pid_t tid = syscall(SYS_gettid);
    CHECK(write(s->tid_fd, &tid, sizeof(tid)) == sizeof(tid));
    if (s->action == FAILED_EXEC) {
        execl("", "", (char *)NULL);
        CHECK(errno == ENOENT);
        CHECK(!pthread_mutex_unlock(s->mutex));
    } else if (s->action != OWNER_EXIT) {
        execl(s->exe, s->exe, "--done", (char *)NULL);
        CHECK(0);
    }
    _exit(0);
}

static void run(const char *exe, int pi, enum action action, int retained)
{
    pthread_mutex_t *m = mmap(NULL, sizeof(*m), PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(m != MAP_FAILED);
    pthread_mutexattr_t attr;
    CHECK(!pthread_mutexattr_init(&attr));
    CHECK(!pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED));
    CHECK(!pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST));
    CHECK(!pthread_mutexattr_setprotocol(&attr, pi ? PTHREAD_PRIO_INHERIT : PTHREAD_PRIO_NONE));
    CHECK(!pthread_mutex_init(m, &attr));
    CHECK(!pthread_mutexattr_destroy(&attr));

    int ids[2];
    CHECK(!pipe(ids));
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        alarm(10);
        close(ids[0]);
        struct scenario s = {action, exe, m, ids[1]};
        if (action == LEADER_EXEC) own(&s);
        pthread_t worker;
        CHECK(!pthread_create(&worker, NULL, own, &s));
        for (;;) pause();
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    pid_t old_tid, reused = -1;
    CHECK(read(ids[0], &old_tid, sizeof(old_tid)) == sizeof(old_tid));
    if (!retained && action == NONLEADER_EXEC) {
        /* clone3 set_tid in this namespace, not merely equal numbers in siblings. */
        struct {
            uint64_t flags, pidfd, child_tid, parent_tid, exit_signal, stack,
                     stack_size, tls, set_tid, set_tid_size, cgroup;
        } args = {.exit_signal = SIGCHLD, .set_tid = (uintptr_t)&old_tid, .set_tid_size = 1};
        reused = syscall(SYS_clone3, &args, sizeof(args));
        CHECK(reused >= 0);
        if (!reused) {
            alarm(10);
            close(ids[1]);
            char c;
            CHECK(syscall(SYS_gettid) == old_tid);
            CHECK(read(ids[0], &c, 1) == 1);
            _exit(0);
        }
        CHECK(reused == old_tid);
    }
    int expected = action == FAILED_EXEC ? 0 :
                   action == NONLEADER_EXEC && retained ? EBUSY : EOWNERDEAD;
    int actual = pthread_mutex_trylock(m);
    if (expected != actual) dprintf(2, "pi=%d case=%d: got %d expected %d\n", pi, action, actual, expected);
    CHECK(actual == expected);
    if (actual != EBUSY) {
        if (actual == EOWNERDEAD) CHECK(!pthread_mutex_consistent(m));
        CHECK(!pthread_mutex_unlock(m));
        CHECK(!pthread_mutex_destroy(m));
    }
    if (reused > 0) {
        CHECK(write(ids[1], "x", 1) == 1);
        CHECK(waitpid(reused, &status, 0) == reused && WIFEXITED(status) && !WEXITSTATUS(status));
    }
    close(ids[0]); close(ids[1]);
    CHECK(!munmap(m, sizeof(*m)));
}

/* glibc-specific observation: WAITERS on this mutex plus S on the only
 * contender is indirect evidence of contention, not a kernel queue probe. */
static void wait_sleeping(pid_t waiter, pthread_mutex_t *mutex)
{
    char path[64], buf[512];
    snprintf(path, sizeof(path), "/proc/%d/stat", waiter);
    for (;;) {
        FILE *f = fopen(path, "r");
        CHECK(f && fgets(buf, sizeof(buf), f));
        CHECK(!fclose(f));
        char *end = strrchr(buf, ')');
        CHECK(end && end[1] == ' ');
        if (end[2] == 'S' && ((unsigned)__atomic_load_n(&mutex->__data.__lock,
                __ATOMIC_RELAXED) & UINT32_C(0x80000000))) return;
        sched_yield();
    }
}

struct boundary {
    pthread_mutex_t lock;
    int held[2], release[2], ready[2], finish[2];
    const char *exe;
    int exec;
};

static void signal_fd(int fd) { CHECK(write(fd, "x", 1) == 1); }
static void await_fd(int fd) { char c; CHECK(read(fd, &c, 1) == 1); }

static void *worker(void *arg)
{
    struct boundary *b = arg;
    CHECK(!pthread_mutex_lock(&b->lock));
    signal_fd(b->held[1]);
    await_fd(b->release[0]);
    if (b->exec) {
        execl(b->exe, b->exe, "--done", (char *)NULL);
        CHECK(0);
    }
    return NULL;
}

static void reap(pid_t pid)
{
    int status;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status));
}

static void queued(const char *exe, int pi, int exec)
{
    struct boundary *b = mmap(NULL, sizeof(*b), PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(b != MAP_FAILED);
    b->exe = exe;
    b->exec = exec;
    pthread_mutexattr_t a;
    CHECK(!pthread_mutexattr_init(&a));
    CHECK(!pthread_mutexattr_setpshared(&a, PTHREAD_PROCESS_SHARED));
    CHECK(!pthread_mutexattr_setrobust(&a, PTHREAD_MUTEX_ROBUST));
    CHECK(!pthread_mutexattr_setprotocol(&a, pi ? PTHREAD_PRIO_INHERIT : PTHREAD_PRIO_NONE));
    CHECK(!pthread_mutex_init(&b->lock, &a));
    CHECK(!pthread_mutexattr_destroy(&a));
    CHECK(!pipe(b->held) && !pipe(b->release) && !pipe(b->ready) && !pipe(b->finish));
    pid_t owner = fork();
    CHECK(owner >= 0);
    if (!owner) {
        alarm(10);
        close(b->held[0]); close(b->release[1]);
        close(b->ready[0]); close(b->ready[1]); close(b->finish[1]);
        pthread_t t;
        CHECK(!pthread_create(&t, NULL, worker, b));
        CHECK(!pthread_join(t, NULL));
        await_fd(b->finish[0]); /* leader stays alive throughout recovery */
        _exit(0);
    }
    close(b->held[1]); close(b->release[0]); close(b->finish[0]);
    await_fd(b->held[0]);
    pid_t waiter = fork();
    CHECK(waiter >= 0);
    if (!waiter) {
        alarm(10);
        close(b->ready[0]); close(b->release[1]); close(b->finish[1]); close(b->held[0]);
        signal_fd(b->ready[1]);
        CHECK(pthread_mutex_lock(&b->lock) == EOWNERDEAD);
        CHECK(!pthread_mutex_consistent(&b->lock));
        CHECK(!pthread_mutex_unlock(&b->lock));
        _exit(0);
    }
    close(b->ready[1]);
    await_fd(b->ready[0]);
    wait_sleeping(waiter, &b->lock);
    signal_fd(b->release[1]);
    reap(waiter);
    CHECK(!pthread_mutex_trylock(&b->lock));
    CHECK(!pthread_mutex_unlock(&b->lock));
    if (!exec) signal_fd(b->finish[1]);
    reap(owner);
    CHECK(!pthread_mutex_destroy(&b->lock));
    close(b->held[0]); close(b->ready[0]); close(b->finish[1]); close(b->release[1]);
    CHECK(!munmap(b, sizeof(*b)));
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--done")) return 0;
    int retained = argc == 2 && !strcmp(argv[1], "--expect-retained");
    CHECK(argc == 1 || retained);
    alarm(30);
    for (int pi = 0; pi < 2; ++pi)
        for (enum action a = LEADER_EXEC; a <= OWNER_EXIT; ++a)
            run(argv[0], pi, a, retained);
    for (int pi = 0; pi < 2; ++pi) {
        queued(argv[0], pi, 0);
        if (!retained) queued(argv[0], pi, 1);
    }
    printf("ROBUST QUEUED PASS %d\n", retained ? 2 : 4);
    printf("ROBUST MUTEX PASS 8: nonleader exec %s\n", retained ? "busy" : "owner dead");
    return 0;
}
