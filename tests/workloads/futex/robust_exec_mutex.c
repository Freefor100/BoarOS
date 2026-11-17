/* Process-shared mutex recovery as observed by an unrelated lock owner. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
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
};

static void *own(void *arg)
{
    struct scenario *s = arg;
    CHECK(!pthread_mutex_lock(s->mutex));
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

    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        struct scenario s = {action, exe, m};
        if (action == LEADER_EXEC) own(&s);
        pthread_t worker;
        CHECK(!pthread_create(&worker, NULL, own, &s));
        for (;;) pause();
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
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
    CHECK(!munmap(m, sizeof(*m)));
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--done")) return 0;
    int retained = argc == 2 && !strcmp(argv[1], "--expect-retained");
    CHECK(argc == 1 || retained);
    for (int pi = 0; pi < 2; ++pi)
        for (enum action a = LEADER_EXEC; a <= OWNER_EXIT; ++a)
            run(argv[0], pi, a, retained);
    printf("ROBUST MUTEX PASS 8: nonleader exec %s\n", retained ? "busy" : "owner dead");
    return 0;
}
