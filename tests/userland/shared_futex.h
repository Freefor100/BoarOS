#ifndef BOAROS_TEST_SHARED_FUTEX_H
#define BOAROS_TEST_SHARED_FUTEX_H

/* A forked process has a distinct MM but inherits the same anonymous
 * backing object. Polling WAKE's count avoids relying on scheduler timing. */
static int check_shared_futex_wake(void)
{
    uint32_t *word = mmap(0, 4096, PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    int ready[2];
    int status;
    char signal;
    pid_t child;
    long woken = 0;

    if (word == MAP_FAILED) return 1;
    if (pipe(ready) != 0) return 2;
    child = fork();
    if (child < 0) return 3;
    if (child == 0) {
        struct timespec timeout = {2, 0};
        long result;

        close(ready[0]);
        if (write(ready[1], "r", 1) != 1) _exit(11);
        result = syscall(SYS_futex, word, 0, 0, &timeout, 0, 0);
        _exit(result == 0 ? 0 : 12);
    }
    close(ready[1]);
    if (read(ready[0], &signal, 1) != 1) return 4;
    for (int attempt = 0; attempt < 10000 && woken == 0; attempt++) {
        woken = syscall(SYS_futex, word, 1, 1, 0, 0, 0);
        if (woken == 0) sched_yield();
    }
    if (waitpid(child, &status, 0) != child ||
        woken != 1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return 5;
    if (close(ready[0]) != 0 || munmap(word, 4096) != 0) return 6;
    return 0;
}

static int check_shared_futex_requeue(void)
{
    uint32_t *shared = mmap(0, 4096, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    uint32_t *target = mmap(0, 4096, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int ready[2];
    int status;
    char signal;
    pid_t child;
    long moved = 0;

    if (shared == MAP_FAILED || target == MAP_FAILED) return 1;
    if (pipe(ready) != 0) return 2;
    child = fork();
    if (child < 0) return 3;
    if (child == 0) {
        struct timespec timeout = {2, 0};
        long result;

        close(ready[0]);
        if (write(ready[1], "r", 1) != 1) _exit(21);
        result = syscall(SYS_futex, shared, 0, 0, &timeout, 0, 0);
        _exit(result == 0 ? 0 : 22);
    }
    close(ready[1]);
    if (read(ready[0], &signal, 1) != 1) return 4;
    for (int attempt = 0; attempt < 10000 && moved == 0; attempt++) {
        moved = syscall(SYS_futex, shared, 3, 0,
                        (void *)(uintptr_t)1, target, 0);
        if (moved == 0) sched_yield();
    }
    if (moved != 1 ||
        syscall(SYS_futex, shared, 1, 1, 0, 0, 0) != 0 ||
        munmap(shared, 4096) != 0 ||
        syscall(SYS_futex, target, 1, 1, 0, 0, 0) != 1 ||
        waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return 5;
    if (close(ready[0]) != 0 || munmap(target, 4096) != 0) return 6;
    return 0;
}

/* Exercise requeue to two offsets in the same shared object. The offsets
 * cover both bucket-local and cross-bucket movement in the current hash,
 * while the observable contract is independent of that hash. */
static int check_shared_futex_requeue_offsets(void)
{
    const size_t targets[] = {1U, 256U};

    for (size_t index = 0; index < 2U; index++) {
        uint32_t *words = mmap(0, 4096, PROT_READ | PROT_WRITE,
                               MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        int ready[2], status;
        char signal;
        pid_t child;
        long moved = 0;

        if (words == MAP_FAILED || pipe(ready) != 0) return 1;
        child = fork();
        if (child < 0) return 2;
        if (child == 0) {
            struct timespec timeout = {2, 0};
            long result;

            close(ready[0]);
            if (write(ready[1], "r", 1) != 1) _exit(41);
            result = syscall(SYS_futex, words, 0, 0, &timeout, 0, 0);
            _exit(result == 0 ? 0 : 42);
        }
        close(ready[1]);
        if (read(ready[0], &signal, 1) != 1) return 3;
        for (int attempt = 0; attempt < 10000 && moved == 0; attempt++) {
            moved = syscall(SYS_futex, words, 3, 0,
                            (void *)(uintptr_t)1, words + targets[index], 0);
            if (moved == 0) sched_yield();
        }
        if (moved != 1 ||
            syscall(SYS_futex, words, 1, 1, 0, 0, 0) != 0 ||
            syscall(SYS_futex, words + targets[index], 1, 1,
                    0, 0, 0) != 1 ||
            waitpid(child, &status, 0) != child ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 0)
            return 4 + (int)index;
        if (close(ready[0]) != 0 || munmap(words, 4096) != 0) return 6;
    }
    return 0;
}

struct shared_futex_unmap_arg {
    uint32_t *words;
    int release_fd;
    int done_fd;
};

static void *shared_futex_unmap_helper(void *argument)
{
    struct shared_futex_unmap_arg *arg = argument;
    char signal;
    int success = read(arg->release_fd, &signal, 1) == 1 &&
                  munmap(arg->words, 4096) == 0;

    signal = success ? 'u' : 'f';
    if (write(arg->done_fd, &signal, 1) != 1 || !success)
        return (void *)(uintptr_t)1;
    return 0;
}

/* Requeue within one object, then remove both MMs' final VMAs while the
 * waiter remains blocked. Its key must keep the object alive until timeout. */
static int check_shared_futex_last_mapping(void)
{
    uint32_t *words = mmap(0, 4096, PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    int ready[2], release[2], done[2], status;
    char signal;
    pid_t child;
    long moved = 0;

    if (words == MAP_FAILED || pipe(ready) != 0 ||
        pipe(release) != 0 || pipe(done) != 0) return 1;
    child = fork();
    if (child < 0) return 2;
    if (child == 0) {
        struct shared_futex_unmap_arg arg = {
            .words = words,
            .release_fd = release[0],
            .done_fd = done[1],
        };
        struct timespec timeout = {2, 0};
        pthread_t helper;
        void *helper_result = (void *)(uintptr_t)1;
        long result;

        close(ready[0]);
        close(release[1]);
        close(done[0]);
        if (pthread_create(&helper, 0, shared_futex_unmap_helper,
                           &arg) != 0) _exit(51);
        if (write(ready[1], "r", 1) != 1) _exit(52);
        errno = 0;
        result = syscall(SYS_futex, words, 0, 0, &timeout, 0, 0);
        int wait_errno = errno;
        if (pthread_join(helper, &helper_result) != 0) _exit(53);
        _exit(result == -1 && wait_errno == ETIMEDOUT &&
              helper_result == 0 ? 0 : 54);
    }
    close(ready[1]);
    close(release[0]);
    close(done[1]);
    if (read(ready[0], &signal, 1) != 1) return 3;
    for (int attempt = 0; attempt < 10000 && moved == 0; attempt++) {
        moved = syscall(SYS_futex, words, 3, 0,
                        (void *)(uintptr_t)1, words + 1, 0);
        if (moved == 0) sched_yield();
    }
    if (moved != 1 || munmap(words, 4096) != 0 ||
        write(release[1], "u", 1) != 1 ||
        read(done[0], &signal, 1) != 1 || signal != 'u' ||
        waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return 4;
    if (close(ready[0]) != 0 || close(release[1]) != 0 ||
        close(done[0]) != 0) return 5;
    return 0;
}

/* A private key on inherited shared memory and a new object at the same VA
 * must both stay isolated from the parent's shared key. */
static int check_shared_futex_isolation(void)
{
    for (int mode = 0; mode < 2; mode++) {
        uint32_t *word = mmap(0, 4096, PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        int ready[2], status = 0;
        char signal;
        pid_t child, waited = 0;
        long wake_result = 0;

        if (word == MAP_FAILED || pipe(ready) != 0) return 1;
        child = fork();
        if (child < 0) return 2;
        if (child == 0) {
            struct timespec timeout = {0, 200000000};
            long result;

            close(ready[0]);
            if (mode == 1 &&
                mmap(word, 4096, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED,
                     -1, 0) != word) _exit(31);
            if (write(ready[1], "r", 1) != 1) _exit(32);
            errno = 0;
            result = syscall(SYS_futex, word, mode == 0 ? 128 : 0,
                             0, &timeout, 0, 0);
            _exit(result == -1 && errno == ETIMEDOUT ? 0 : 33);
        }
        close(ready[1]);
        if (read(ready[0], &signal, 1) != 1) return 3;
        for (int attempt = 0; attempt < 100000; attempt++) {
            wake_result = syscall(SYS_futex, word, 1, 1, 0, 0, 0);
            if (wake_result != 0) break;
            waited = waitpid(child, &status, WNOHANG);
            if (waited != 0) break;
            sched_yield();
        }
        if (waited == 0 && waitpid(child, &status, 0) == child)
            waited = child;
        if (wake_result != 0 || waited != child ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 0)
            return 4 + mode;
        if (close(ready[0]) != 0 || munmap(word, 4096) != 0) return 6;
    }
    return 0;
}

#endif
