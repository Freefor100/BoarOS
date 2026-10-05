#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define WORKERS 3
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_WAIT_PRIVATE (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_PRIVATE (FUTEX_WAKE | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAIT_BITSET_PRIVATE (FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG)
#define FUTEX_BITSET_MATCH_ANY UINT32_MAX

static volatile sig_atomic_t futex_signal_seen;
static volatile sig_atomic_t futex_signal_changes_word;
static volatile int *futex_signal_word;

static void futex_signal_handler(int signal_number)
{
    futex_signal_seen = signal_number;
    if (futex_signal_changes_word && futex_signal_word != 0)
        *futex_signal_word = 1;
}

struct futex_signal_case {
    int word;
    volatile int ready;
    int timed;
    long timeout_nanoseconds;
    int result;
    int error;
};

static void *futex_signal_waiter(void *opaque)
{
    struct futex_signal_case *test = opaque;
    struct timespec timeout = {
        .tv_sec = test->timeout_nanoseconds / 1000000000L,
        .tv_nsec = test->timeout_nanoseconds % 1000000000L,
    };

    test->ready = 1;
    errno = 0;
    test->result = (int)syscall(SYS_futex, &test->word,
                                FUTEX_WAIT_PRIVATE, 0,
                                test->timed ? &timeout : 0, 0, 0);
    test->error = errno;
    return 0;
}

static int wait_for_futex_case_ready(struct futex_signal_case *test)
{
    for (int tries = 0; tries < 10000 && !test->ready; tries++)
        sched_yield();
    return test->ready ? 0 : 1;
}

static int wait_for_futex_signal(void)
{
    for (int tries = 0; tries < 10000 && !futex_signal_seen; tries++)
        sched_yield();
    return futex_signal_seen ? 0 : 1;
}

static int run_futex_signal_case(int restart, int timed, int change_word,
                                 int expected_error)
{
    struct sigaction action = {0};
    struct futex_signal_case test = {
        .timed = timed,
        .timeout_nanoseconds = 1000000000L,
    };
    pthread_t thread;
    void *thread_result = 0;

    sigemptyset(&action.sa_mask);
    action.sa_handler = futex_signal_handler;
    action.sa_flags = restart ? SA_RESTART : 0;
    if (sigaction(SIGUSR1, &action, 0) != 0) return 1;
    futex_signal_seen = 0;
    futex_signal_changes_word = change_word;
    futex_signal_word = &test.word;
    if (pthread_create(&thread, 0, futex_signal_waiter, &test) != 0)
        return 2;
    if (wait_for_futex_case_ready(&test) != 0 ||
        pthread_kill(thread, SIGUSR1) != 0 || wait_for_futex_signal() != 0) {
        test.word = 1;
        syscall(SYS_futex, &test.word, FUTEX_WAKE_PRIVATE, 1, 0, 0, 0);
        pthread_join(thread, &thread_result);
        return 3;
    }
    if (!change_word) {
        if (restart && !timed) {
            /* handler标志只证明信号已进入，不能证明重启的WAIT已入队。
             * 保持期望值不变，直到WAKE明确选中等待者，避免合法EAGAIN。 */
            int woke = 0;
            for (int tries = 0; tries < 10000 && !woke; tries++) {
                long count = syscall(SYS_futex, &test.word, FUTEX_WAKE_PRIVATE, 1, 0, 0, 0);
                if (count < 0) break;
                woke = count == 1;
                if (!woke) sched_yield();
            }
            if (!woke) {
                test.word = 1;
                syscall(SYS_futex, &test.word, FUTEX_WAKE_PRIVATE, 1, 0, 0, 0);
                pthread_join(thread, &thread_result);
                return 3;
            }
        } else {
            test.word = 1;
            syscall(SYS_futex, &test.word, FUTEX_WAKE_PRIVATE, 1, 0, 0, 0);
        }
    }
    if (pthread_join(thread, &thread_result) != 0 || thread_result != 0)
        return 4;
    futex_signal_word = 0;
    if (expected_error == 0)
        return test.result == 0 ? 0 : 5;
    return test.result == -1 && test.error == expected_error ? 0 : 6;
}

static int check_futex_wake_signal_boundary(void)
{
    struct sigaction action = {0};
    struct futex_signal_case test = {0};
    struct timespec settle = { .tv_sec = 0, .tv_nsec = 20000000L };
    pthread_t thread;
    void *thread_result = 0;

    sigemptyset(&action.sa_mask);
    action.sa_handler = futex_signal_handler;
    action.sa_flags = SA_RESTART;
    if (sigaction(SIGUSR1, &action, 0) != 0) return 1;
    futex_signal_seen = 0;
    futex_signal_changes_word = 0;
    futex_signal_word = &test.word;
    if (pthread_create(&thread, 0, futex_signal_waiter, &test) != 0)
        return 2;
    if (wait_for_futex_case_ready(&test) != 0 || nanosleep(&settle, 0) != 0)
        return 3;

    /* Publish the changed condition, wake the waiter, then queue a signal
     * before it can run. The completed wake wins; the signal is still
     * delivered, and must not turn the successful wait into a retry. */
    test.word = 1;
    if (syscall(SYS_futex, &test.word, FUTEX_WAKE_PRIVATE, 1, 0, 0, 0) != 1 ||
        pthread_kill(thread, SIGUSR1) != 0 ||
        pthread_join(thread, &thread_result) != 0 || thread_result != 0 ||
        test.result != 0 || wait_for_futex_signal() != 0) return 4;
    futex_signal_word = 0;
    return 0;
}

static int check_futex_timeout_signal_boundary(void)
{
    struct sigaction action = {0};
    struct futex_signal_case test = {
        .timed = 1,
        .timeout_nanoseconds = 50000000L,
    };
    struct timespec near_deadline = {
        .tv_sec = 0,
        .tv_nsec = 40000000L,
    };
    pthread_t thread;
    void *thread_result = 0;

    sigemptyset(&action.sa_mask);
    action.sa_handler = futex_signal_handler;
    action.sa_flags = SA_RESTART;
    if (sigaction(SIGUSR1, &action, 0) != 0) return 1;
    futex_signal_seen = 0;
    futex_signal_changes_word = 0;
    futex_signal_word = &test.word;
    if (pthread_create(&thread, 0, futex_signal_waiter, &test) != 0)
        return 2;
    if (wait_for_futex_case_ready(&test) != 0 ||
        nanosleep(&near_deadline, 0) != 0 ||
        pthread_kill(thread, SIGUSR1) != 0 ||
        pthread_join(thread, &thread_result) != 0 || thread_result != 0 ||
        wait_for_futex_signal() != 0) return 3;
    futex_signal_word = 0;

    /* At the deadline boundary Linux permits either event to win. A caught
     * handler may expose EINTR; an already committed timeout exposes
     * ETIMEDOUT. No fresh timeout or unrelated result is valid. */
    return test.result == -1 &&
                   (test.error == EINTR || test.error == ETIMEDOUT)
               ? 0
               : 4;
}

static int check_timed_futex_stop_restart(void)
{
    struct timespec timeout = { .tv_sec = 0, .tv_nsec = 300000000L };
    struct timespec delay = { .tv_sec = 0, .tv_nsec = 60000000L };
    struct timespec stopped = { .tv_sec = 0, .tv_nsec = 100000000L };
    struct timespec before, after;
    int word = 0;
    int status = 0;
    pid_t controller = fork();

    if (controller < 0) return 1;
    if (controller == 0) {
        pid_t parent = getppid();

        if (nanosleep(&delay, 0) != 0 || kill(parent, SIGSTOP) != 0 ||
            nanosleep(&stopped, 0) != 0 || kill(parent, SIGCONT) != 0)
            _exit(1);
        _exit(0);
    }
    if (clock_gettime(CLOCK_MONOTONIC, &before) != 0) return 2;
    errno = 0;
    if (syscall(SYS_futex, &word, FUTEX_WAIT_PRIVATE, 0,
                &timeout, 0, 0) != -1 || errno != ETIMEDOUT) {
        kill(controller, SIGCONT);
        waitpid(controller, &status, 0);
        return 3;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &after) != 0 ||
        waitpid(controller, &status, 0) != controller ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) return 4;
    {
        int64_t elapsed_ns =
            (int64_t)(after.tv_sec - before.tv_sec) * INT64_C(1000000000) +
            after.tv_nsec - before.tv_nsec;

        if (elapsed_ns < INT64_C(250000000) ||
            elapsed_ns > INT64_C(400000000)) return 5;
    }
    return 0;
}

static int check_bitset_stop_restart(void)
{
    struct timespec delay = { .tv_sec = 0, .tv_nsec = 60000000L };
    struct timespec stopped = { .tv_sec = 0, .tv_nsec = 100000000L };
    struct timespec settled = { .tv_sec = 0, .tv_nsec = 20000000L };
    struct timespec deadline;
    int *word = mmap(0, 4096, PROT_READ | PROT_WRITE,
                     MAP_ANONYMOUS | MAP_SHARED, -1, 0);
    int status = 0;
    pid_t controller;

    if (word == MAP_FAILED) return 1;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) return 2;
    deadline.tv_nsec += 500000000L;
    deadline.tv_sec += deadline.tv_nsec / 1000000000L;
    deadline.tv_nsec %= 1000000000L;
    controller = fork();
    if (controller < 0) return 3;
    if (controller == 0) {
        pid_t parent = getppid();
        long unmatched, matched;

        if (nanosleep(&delay, 0) != 0 || kill(parent, SIGSTOP) != 0 ||
            nanosleep(&stopped, 0) != 0 || kill(parent, SIGCONT) != 0 ||
            nanosleep(&settled, 0) != 0) _exit(1);
        unmatched = syscall(SYS_futex, word, FUTEX_WAKE_BITSET,
                            1, 0, 0, 1);
        matched = syscall(SYS_futex, word, FUTEX_WAKE_BITSET,
                          1, 0, 0, 2);
        _exit(unmatched == 0 && matched == 1 ? 0 : 2);
    }
    errno = 0;
    long result = syscall(SYS_futex, word,
                          FUTEX_WAIT_BITSET | FUTEX_CLOCK_REALTIME,
                          0, &deadline, 0, 2);
    int saved_errno = errno;
    if (waitpid(controller, &status, 0) != controller ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
        munmap(word, 4096) != 0) return 4;
    return result == 0 && saved_errno == 0 ? 0 : 5;
}

static _Thread_local int executable_tls = 101;

static int deadline_after_ms(clockid_t clock, long milliseconds,
                             struct timespec *deadline)
{
    if (clock_gettime(clock, deadline) != 0) return errno;
    deadline->tv_nsec += milliseconds * 1000000L;
    deadline->tv_sec += deadline->tv_nsec / 1000000000L;
    deadline->tv_nsec %= 1000000000L;
    return 0;
}

static int wait_status(pid_t child, int expected)
{
    int status = 0;

    return waitpid(child, &status, 0) == child && WIFEXITED(status) &&
           WEXITSTATUS(status) == expected ? 0 : 1;
}

struct tls_gate {
    pthread_mutex_t mutex;
    pthread_cond_t ready_condition;
    pthread_cond_t release_condition;
    int ready;
    int release;
};

struct tls_worker {
    struct tls_gate *gate;
    int value;
    int initial;
    int observed;
    void *address;
};

static void *executable_tls_worker(void *opaque)
{
    struct tls_worker *worker = opaque;

    worker->initial = executable_tls;
    executable_tls = worker->value;
    worker->address = &executable_tls;
    if (pthread_mutex_lock(&worker->gate->mutex) != 0) return (void *)1;
    worker->gate->ready++;
    pthread_cond_signal(&worker->gate->ready_condition);
    while (!worker->gate->release) {
        if (pthread_cond_wait(&worker->gate->release_condition,
                              &worker->gate->mutex) != 0) {
            pthread_mutex_unlock(&worker->gate->mutex);
            return (void *)2;
        }
    }
    worker->observed = executable_tls;
    pthread_mutex_unlock(&worker->gate->mutex);
    return 0;
}

static int check_executable_tls(void)
{
    struct tls_gate gate = {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .ready_condition = PTHREAD_COND_INITIALIZER,
        .release_condition = PTHREAD_COND_INITIALIZER,
    };
    struct tls_worker workers[WORKERS];
    pthread_t threads[WORKERS];

    executable_tls = 109;
    for (int i = 0; i < WORKERS; i++) {
        workers[i] = (struct tls_worker){
            .gate = &gate,
            .value = 201 + i,
        };
        if (pthread_create(&threads[i], 0, executable_tls_worker,
                           &workers[i]) != 0) return 1;
    }
    pthread_mutex_lock(&gate.mutex);
    while (gate.ready != WORKERS)
        pthread_cond_wait(&gate.ready_condition, &gate.mutex);
    if (executable_tls != 109) {
        pthread_mutex_unlock(&gate.mutex);
        return 2;
    }
    for (int i = 0; i < WORKERS; i++) {
        if (workers[i].initial != 101 || workers[i].address == &executable_tls)
            return 3;
        for (int j = 0; j < i; j++)
            if (workers[i].address == workers[j].address) return 4;
    }
    gate.release = 1;
    pthread_cond_broadcast(&gate.release_condition);
    pthread_mutex_unlock(&gate.mutex);
    for (int i = 0; i < WORKERS; i++) {
        void *result = 0;
        if (pthread_join(threads[i], &result) != 0 || result != 0 ||
            workers[i].observed != workers[i].value) return 5;
    }
    return 0;
}

static pthread_mutex_t counter_mutex = PTHREAD_MUTEX_INITIALIZER;
static int shared_counter;

static void *counter_worker(void *opaque)
{
    (void)opaque;
    for (int i = 0; i < 240; i++) {
        if (pthread_mutex_lock(&counter_mutex) != 0) return (void *)1;
        int next = shared_counter + 1;
        if ((i & 7) == 0) sched_yield();
        shared_counter = next;
        if (pthread_mutex_unlock(&counter_mutex) != 0) return (void *)2;
    }
    return 0;
}

struct broadcast_gate {
    pthread_mutex_t mutex;
    pthread_cond_t ready_condition;
    pthread_cond_t release_condition;
    int ready;
    int release;
    int passed;
};

static void *broadcast_worker(void *opaque)
{
    struct broadcast_gate *gate = opaque;

    pthread_mutex_lock(&gate->mutex);
    gate->ready++;
    pthread_cond_signal(&gate->ready_condition);
    while (!gate->release)
        pthread_cond_wait(&gate->release_condition, &gate->mutex);
    gate->passed++;
    pthread_mutex_unlock(&gate->mutex);
    return 0;
}

static pthread_mutex_t timed_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_barrier_t round_barrier;
static int round_values[WORKERS];

static void *barrier_worker(void *opaque)
{
    size_t index = (size_t)opaque;
    for (int round = 1; round <= 4; round++) {
        round_values[index] = round;
        int result = pthread_barrier_wait(&round_barrier);
        if (result != 0 && result != PTHREAD_BARRIER_SERIAL_THREAD)
            return (void *)1;
        for (int i = 0; i < WORKERS; i++)
            if (round_values[i] != round) return (void *)2;
        result = pthread_barrier_wait(&round_barrier);
        if (result != 0 && result != PTHREAD_BARRIER_SERIAL_THREAD)
            return (void *)3;
    }
    return 0;
}

static void *timed_mutex_worker(void *opaque)
{
    int *result = opaque;
    struct timespec deadline;

    if (deadline_after_ms(CLOCK_REALTIME, 20, &deadline) != 0)
        *result = EINVAL;
    else
        *result = pthread_mutex_timedlock(&timed_mutex, &deadline);
    return 0;
}

static int check_synchronization(void)
{
    pthread_t threads[WORKERS];
    shared_counter = 0;
    for (int i = 0; i < WORKERS; i++)
        if (pthread_create(&threads[i], 0, counter_worker, 0) != 0) return 1;
    for (int i = 0; i < WORKERS; i++) {
        void *result = 0;
        if (pthread_join(threads[i], &result) != 0 || result != 0) return 2;
    }
    if (shared_counter != WORKERS * 240) return 3;

    struct broadcast_gate gate = {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .ready_condition = PTHREAD_COND_INITIALIZER,
        .release_condition = PTHREAD_COND_INITIALIZER,
    };
    for (int i = 0; i < WORKERS; i++)
        if (pthread_create(&threads[i], 0, broadcast_worker, &gate) != 0)
            return 4;
    pthread_mutex_lock(&gate.mutex);
    while (gate.ready != WORKERS)
        pthread_cond_wait(&gate.ready_condition, &gate.mutex);
    gate.release = 1;
    pthread_cond_broadcast(&gate.release_condition);
    sched_yield();
    pthread_mutex_unlock(&gate.mutex);
    for (int i = 0; i < WORKERS; i++)
        if (pthread_join(threads[i], 0) != 0) return 5;
    if (gate.passed != WORKERS) return 6;

    pthread_mutex_t condition_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
    struct timespec deadline;
    if (deadline_after_ms(CLOCK_REALTIME, 20, &deadline) != 0) return 7;
    pthread_mutex_lock(&condition_mutex);
    int timed_result = pthread_cond_timedwait(&condition, &condition_mutex,
                                               &deadline);
    if (timed_result != ETIMEDOUT ||
        pthread_mutex_unlock(&condition_mutex) != 0) return 8;

    timed_result = 0;
    pthread_mutex_lock(&timed_mutex);
    if (pthread_create(&threads[0], 0, timed_mutex_worker, &timed_result) != 0)
        return 9;
    if (pthread_join(threads[0], 0) != 0 || timed_result != ETIMEDOUT) return 10;
    pthread_mutex_unlock(&timed_mutex);
    if (pthread_barrier_init(&round_barrier, 0, WORKERS) != 0) return 11;
    for (size_t i = 0; i < WORKERS; i++)
        if (pthread_create(&threads[i], 0, barrier_worker, (void *)i) != 0)
            return 12;
    for (int i = 0; i < WORKERS; i++) {
        void *result;
        if (pthread_join(threads[i], &result) != 0 || result != 0) return 13;
    }
    if (pthread_barrier_destroy(&round_barrier) != 0) return 14;
    return 0;
}

struct cancel_gate {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int ready;
};

static void cancel_unlock(void *opaque)
{
    pthread_mutex_unlock(opaque);
}

static void *cancel_worker(void *opaque)
{
    struct cancel_gate *gate = opaque;

    pthread_mutex_lock(&gate->mutex);
    gate->ready = 1;
    pthread_cond_signal(&gate->condition);
    pthread_cleanup_push(cancel_unlock, &gate->mutex);
    for (;;) pthread_cond_wait(&gate->condition, &gate->mutex);
    pthread_cleanup_pop(1);
    return 0;
}

static int check_cancellation(void)
{
    struct cancel_gate gate = {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .condition = PTHREAD_COND_INITIALIZER,
    };
    pthread_t thread;
    void *result = 0;

    if (pthread_create(&thread, 0, cancel_worker, &gate) != 0) return 1;
    pthread_mutex_lock(&gate.mutex);
    while (!gate.ready) pthread_cond_wait(&gate.condition, &gate.mutex);
    pthread_mutex_unlock(&gate.mutex);
    if (pthread_cancel(thread) != 0 || pthread_join(thread, &result) != 0 ||
        result != PTHREAD_CANCELED) return 2;
    if (pthread_mutex_trylock(&gate.mutex) != 0) return 3;
    pthread_mutex_unlock(&gate.mutex);
    return 0;
}

typedef int (*tls_get_fn)(void);
typedef void (*tls_set_fn)(int);
typedef void *(*tls_address_fn)(void);

struct dso_gate {
    pthread_mutex_t mutex;
    pthread_cond_t ready_condition;
    pthread_cond_t release_condition;
    pthread_cond_t observed_condition;
    int ready;
    int loaded;
    int observed;
    tls_get_fn get;
    tls_set_fn set;
    tls_address_fn address;
};

struct dso_worker {
    struct dso_gate *gate;
    int index;
    int initial;
    int value;
    void *address;
};

static void *dso_tls_worker(void *opaque)
{
    struct dso_worker *worker = opaque;
    struct dso_gate *gate = worker->gate;

    pthread_mutex_lock(&gate->mutex);
    gate->ready++;
    pthread_cond_signal(&gate->ready_condition);
    while (!gate->loaded)
        pthread_cond_wait(&gate->release_condition, &gate->mutex);
    pthread_mutex_unlock(&gate->mutex);

    worker->initial = gate->get();
    gate->set(710 + worker->index);
    worker->address = gate->address();
    worker->value = gate->get();

    pthread_mutex_lock(&gate->mutex);
    gate->observed++;
    pthread_cond_signal(&gate->observed_condition);
    while (gate->loaded == 1)
        pthread_cond_wait(&gate->release_condition, &gate->mutex);
    pthread_mutex_unlock(&gate->mutex);
    return 0;
}

static int check_dlopen_tls(void)
{
    struct dso_gate gate = {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .ready_condition = PTHREAD_COND_INITIALIZER,
        .release_condition = PTHREAD_COND_INITIALIZER,
        .observed_condition = PTHREAD_COND_INITIALIZER,
    };
    struct dso_worker workers[WORKERS];
    pthread_t threads[WORKERS];
    void *handle;

    for (int i = 0; i < WORKERS; i++) {
        workers[i] = (struct dso_worker){ .gate = &gate, .index = i };
        if (pthread_create(&threads[i], 0, dso_tls_worker, &workers[i]) != 0)
            return 1;
    }
    pthread_mutex_lock(&gate.mutex);
    while (gate.ready != WORKERS)
        pthread_cond_wait(&gate.ready_condition, &gate.mutex);
    pthread_mutex_unlock(&gate.mutex);

    handle = dlopen("/lib/libboaros-tls.so", RTLD_NOW | RTLD_LOCAL);
    if (handle == 0) return 2;
    gate.get = (tls_get_fn)dlsym(handle, "boaros_tls_get");
    gate.set = (tls_set_fn)dlsym(handle, "boaros_tls_set");
    gate.address = (tls_address_fn)dlsym(handle, "boaros_tls_address");
    if (gate.get == 0 || gate.set == 0 || gate.address == 0) return 3;
    if (gate.get() != 700) return 4;
    gate.set(709);
    void *main_address = gate.address();

    pthread_mutex_lock(&gate.mutex);
    gate.loaded = 1;
    pthread_cond_broadcast(&gate.release_condition);
    while (gate.observed != WORKERS)
        pthread_cond_wait(&gate.observed_condition, &gate.mutex);
    if (gate.get() != 709) return 5;
    for (int i = 0; i < WORKERS; i++) {
        if (workers[i].initial != 700 || workers[i].value != 710 + i ||
            workers[i].address == main_address) return 6;
        for (int j = 0; j < i; j++)
            if (workers[i].address == workers[j].address) return 7;
    }
    gate.loaded = 2;
    pthread_cond_broadcast(&gate.release_condition);
    pthread_mutex_unlock(&gate.mutex);
    for (int i = 0; i < WORKERS; i++)
        if (pthread_join(threads[i], 0) != 0) return 8;
    if (dlclose(handle) != 0) return 9;
    return 0;
}

struct fd_case {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int ready;
    int read_fd;
    char byte;
    ssize_t result;
};

static void *blocked_reader(void *opaque)
{
    struct fd_case *test = opaque;

    pthread_mutex_lock(&test->mutex);
    test->ready = 1;
    pthread_cond_signal(&test->condition);
    pthread_mutex_unlock(&test->mutex);
    test->result = read(test->read_fd, &test->byte, 1);
    return 0;
}

static int check_shared_fd_pin(void)
{
    int original[2];
    int reused[2];
    pthread_t reader;
    struct fd_case test = {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .condition = PTHREAD_COND_INITIALIZER,
    };

    if (pipe(original) != 0) return 1;
    test.read_fd = original[0];
    if (pthread_create(&reader, 0, blocked_reader, &test) != 0) return 2;
    pthread_mutex_lock(&test.mutex);
    while (!test.ready) pthread_cond_wait(&test.condition, &test.mutex);
    pthread_mutex_unlock(&test.mutex);
    struct timespec pause = { .tv_sec = 0, .tv_nsec = 10000000L };
    nanosleep(&pause, 0);
    if (close(original[0]) != 0 || pipe(reused) != 0 ||
        reused[0] != original[0]) return 3;
    if (write(original[1], "P", 1) != 1 || pthread_join(reader, 0) != 0)
        return 4;
    if (test.result != 1 || test.byte != 'P') return 5;
    if (close(original[1]) != 0 || close(reused[0]) != 0 ||
        close(reused[1]) != 0) return 6;
    return 0;
}

static int bind_loopback_udp(struct sockaddr_in *address)
{
    struct ifreq interface = {0};
    socklen_t length = sizeof(*address);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    memcpy(interface.ifr_name, "lo", 3);
    if (ioctl(fd, SIOCGIFFLAGS, &interface) != 0) return -1;
    if ((interface.ifr_flags & IFF_UP) == 0) {
        interface.ifr_flags |= IFF_UP;
        if (ioctl(fd, SIOCSIFFLAGS, &interface) != 0) return -1;
    }
    *address = (struct sockaddr_in){
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    if (bind(fd, (const struct sockaddr *)address, sizeof(*address)) != 0 ||
        getsockname(fd, (struct sockaddr *)address, &length) != 0 ||
        length != sizeof(*address) || address->sin_port == 0)
        return -1;
    struct timeval timeout = {.tv_sec = 2};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout)) != 0) return -1;
    return fd;
}

static int check_shared_socket_pin(void)
{
    struct sockaddr_in address;
    struct fd_case readers[2] = {
        {.mutex = PTHREAD_MUTEX_INITIALIZER,
         .condition = PTHREAD_COND_INITIALIZER},
        {.mutex = PTHREAD_MUTEX_INITIALIZER,
         .condition = PTHREAD_COND_INITIALIZER},
    };
    pthread_t threads[2];
    struct timespec pause_time = {.tv_nsec = 10000000L};
    int receiver = bind_loopback_udp(&address);
    int sender = socket(AF_INET, SOCK_DGRAM, 0);
    if (receiver < 0 || sender < 0) return 1;
    char empty_followup = 0;
    if (sendto(sender, "", 0, 0, (struct sockaddr *)&address,
               sizeof(address)) != 0 ||
        read(receiver, &empty_followup, 1) != 0 ||
        sendto(sender, "r", 1, 0, (struct sockaddr *)&address,
               sizeof(address)) != 1 ||
        read(receiver, &empty_followup, 1) != 1 ||
        empty_followup != 'r') return 10;
    struct iovec empty_vector = {&empty_followup, 1};
    if (sendto(sender, "", 0, 0, (struct sockaddr *)&address,
               sizeof(address)) != 0 ||
        readv(receiver, &empty_vector, 1) != 0 ||
        sendto(sender, "s", 1, 0, (struct sockaddr *)&address,
               sizeof(address)) != 1 ||
        read(receiver, &empty_followup, 1) != 1 ||
        empty_followup != 's') return 11;
    for (int index = 0; index < 2; index++) {
        readers[index].read_fd = receiver;
        if (pthread_create(&threads[index], 0, blocked_reader,
                           &readers[index]) != 0) return 2;
    }
    for (int index = 0; index < 2; index++) {
        pthread_mutex_lock(&readers[index].mutex);
        while (!readers[index].ready)
            pthread_cond_wait(&readers[index].condition,
                              &readers[index].mutex);
        pthread_mutex_unlock(&readers[index].mutex);
    }
    nanosleep(&pause_time, 0);
    if (sendto(sender, "A", 1, 0, (struct sockaddr *)&address,
               sizeof(address)) != 1 ||
        sendto(sender, "B", 1, 0, (struct sockaddr *)&address,
               sizeof(address)) != 1) return 3;
    if (pthread_join(threads[0], 0) != 0 ||
        pthread_join(threads[1], 0) != 0 ||
        readers[0].result != 1 || readers[1].result != 1 ||
        readers[0].byte == readers[1].byte ||
        (readers[0].byte != 'A' && readers[0].byte != 'B') ||
        (readers[1].byte != 'A' && readers[1].byte != 'B')) return 4;

    readers[0] = (struct fd_case){
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .condition = PTHREAD_COND_INITIALIZER,
        .read_fd = receiver,
    };
    if (pthread_create(&threads[0], 0, blocked_reader, &readers[0]) != 0)
        return 5;
    pthread_mutex_lock(&readers[0].mutex);
    while (!readers[0].ready)
        pthread_cond_wait(&readers[0].condition, &readers[0].mutex);
    pthread_mutex_unlock(&readers[0].mutex);
    nanosleep(&pause_time, 0);
    if (close(receiver) != 0) return 6;
    int reused = socket(AF_INET, SOCK_DGRAM, 0);
    if (reused != receiver) return 7;
    if (sendto(sender, "C", 1, 0, (struct sockaddr *)&address,
               sizeof(address)) != 1 ||
        pthread_join(threads[0], 0) != 0 ||
        readers[0].result != 1 || readers[0].byte != 'C') return 8;
    if (close(reused) != 0 || close(sender) != 0) return 9;
    return 0;
}

static int check_socket_group_exit(void)
{
    int sender = socket(AF_INET, SOCK_DGRAM, 0);
    if (sender < 0) return 1;
    for (int round = 0; round < 8; round++) {
        int handshake[2];
        if (pipe(handshake) != 0) return 2;
        pid_t child = fork();
        if (child < 0) return 3;
        if (child == 0) {
            struct sockaddr_in address;
            struct fd_case readers[2] = {
                {.mutex = PTHREAD_MUTEX_INITIALIZER,
                 .condition = PTHREAD_COND_INITIALIZER},
                {.mutex = PTHREAD_MUTEX_INITIALIZER,
                 .condition = PTHREAD_COND_INITIALIZER},
            };
            pthread_t threads[2];
            close(handshake[0]);
            int receiver = bind_loopback_udp(&address);
            if (receiver < 0) _exit(11);
            for (int index = 0; index < 2; index++) {
                readers[index].read_fd = receiver;
                if (pthread_create(&threads[index], 0, blocked_reader,
                                   &readers[index]) != 0) _exit(12);
            }
            for (int index = 0; index < 2; index++) {
                pthread_mutex_lock(&readers[index].mutex);
                while (!readers[index].ready)
                    pthread_cond_wait(&readers[index].condition,
                                      &readers[index].mutex);
                pthread_mutex_unlock(&readers[index].mutex);
            }
            if (write(handshake[1], &address, sizeof(address)) !=
                sizeof(address)) _exit(13);
            for (;;) pause();
        }
        close(handshake[1]);
        struct sockaddr_in address;
        if (read(handshake[0], &address, sizeof(address)) !=
            sizeof(address)) return 4;
        close(handshake[0]);
        struct timespec pause_time = {.tv_nsec = 10000000L};
        nanosleep(&pause_time, 0);
        if (sendto(sender, "X", 1, 0, (struct sockaddr *)&address,
                   sizeof(address)) != 1 ||
            sendto(sender, "Y", 1, 0, (struct sockaddr *)&address,
                   sizeof(address)) != 1 ||
            kill(child, SIGKILL) != 0) return 5;
        int status = 0;
        if (waitpid(child, &status, 0) != child ||
            !WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL)
            return 6;
    }
    return close(sender) == 0 ? 0 : 7;
}

static int check_socket_pool_pressure(void)
{
    struct sockaddr_in tcp_address = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    socklen_t length = sizeof(tcp_address);
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0 ||
        bind(listener, (struct sockaddr *)&tcp_address,
             sizeof(tcp_address)) != 0 ||
        getsockname(listener, (struct sockaddr *)&tcp_address,
                    &length) != 0 ||
        listen(listener, 1) != 0) return 1;
    int client = socket(AF_INET, SOCK_STREAM, 0);
    if (client < 0 || connect(client, (struct sockaddr *)&tcp_address,
                              sizeof(tcp_address)) != 0) return 2;
    int accepted = accept(listener, 0, 0);
    if (accepted < 0) return 3;
    int flags = fcntl(client, F_GETFL);
    if (flags < 0 || fcntl(client, F_SETFL, flags | O_NONBLOCK) != 0)
        return 4;

    struct sockaddr_in udp_address;
    int receiver = bind_loopback_udp(&udp_address);
    int sender = socket(AF_INET, SOCK_DGRAM, 0);
    if (receiver < 0 || sender < 0) return 5;
    /* Bounded UDP receive pressure may drop packets; it must leave room for
     * a following TCP transfer. Raw TCP ERR_MEM retry is tested by the host pool fixture. */
    for (int attempts = 0; attempts < 8192; attempts++) {
        if (sendto(sender, "p", 1, 0, (struct sockaddr *)&udp_address,
                   sizeof(udp_address)) != 1) return 6;
    }
    struct pollfd ready = {.fd = client, .events = POLLOUT};
    if (poll(&ready, 1, 0) != 1 || !(ready.revents & POLLOUT)) return 8;
    if (write(client, "v", 1) != 1) return 9;
    char received;
    if (read(accepted, &received, 1) != 1 || received != 'v') return 10;
    if (close(receiver) != 0) return 11;
    if (fcntl(client, F_SETFL, flags) != 0 || write(client, "w", 1) != 1 ||
        read(accepted, &received, 1) != 1 || received != 'w') return 13;
    if (close(sender) != 0 || close(client) != 0 ||
        close(accepted) != 0 || close(listener) != 0) return 14;
    return 0;
}

static int leader_pipe_fd;

static void *last_thread_worker(void *opaque)
{
    (void)opaque;
    struct timespec pause = { .tv_sec = 0, .tv_nsec = 10000000L };
    nanosleep(&pause, 0);
    (void)write(leader_pipe_fd, "L", 1);
    return 0;
}

static void *exec_worker(void *opaque)
{
    char *expected = opaque;
    char *argv[] = { "/init", "execed", expected, 0 };
    char *envp[] = { 0 };
    unsigned long robust_head[3] = {0};
    void *observed = 0;
    size_t length = 0;

    robust_head[0] = (unsigned long)robust_head;
    if (syscall(SYS_set_robust_list, robust_head,
                sizeof(robust_head)) != 0)
        _exit(88);
    errno = 0;
    if (execve("/missing-robust-exec", argv, envp) != -1 ||
        errno != ENOENT ||
        syscall(SYS_get_robust_list, 0, &observed, &length) != 0 ||
        observed != robust_head || length != sizeof(robust_head))
        _exit(89);

    execve(argv[0], argv, envp);
    _exit(90);
}

static int blocked_group_pipe[2];
static pthread_mutex_t blocked_group_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t blocked_group_condition = PTHREAD_COND_INITIALIZER;
static int blocked_group_ready;

static void *exit_group_peer(void *opaque)
{
    (void)opaque;
    char byte;

    pthread_mutex_lock(&blocked_group_mutex);
    blocked_group_ready++;
    pthread_cond_signal(&blocked_group_condition);
    pthread_mutex_unlock(&blocked_group_mutex);
    (void)read(blocked_group_pipe[0], &byte, 1);
    return (void *)1;
}

static int check_thread_lifecycle(void)
{
    int marker_pipe[2];
    char marker = 0;
    pid_t child;

    if (pipe(marker_pipe) != 0) return 1;
    child = fork();
    if (child < 0) return 2;
    if (child == 0) {
        pthread_t worker;
        close(marker_pipe[0]);
        leader_pipe_fd = marker_pipe[1];
        if (pthread_create(&worker, 0, last_thread_worker, 0) != 0) _exit(91);
        pthread_exit(0);
    }
    close(marker_pipe[1]);
    if (read(marker_pipe[0], &marker, 1) != 1 || marker != 'L' ||
        close(marker_pipe[0]) != 0 || wait_status(child, 0) != 0) return 3;

    child = fork();
    if (child < 0) return 4;
    if (child == 0) {
        pthread_t worker;
        static char expected[24];
        snprintf(expected, sizeof(expected), "%ld", (long)getpid());
        if (pthread_create(&worker, 0, exec_worker, expected) != 0) _exit(92);
        (void)pthread_join(worker, 0);
        _exit(93);
    }
    if (wait_status(child, 23) != 0) return 5;

    child = fork();
    if (child < 0) return 6;
    if (child == 0) {
        pthread_t peers[2];
        blocked_group_ready = 0;
        if (pipe(blocked_group_pipe) != 0) _exit(94);
        for (int i = 0; i < 2; i++)
            if (pthread_create(&peers[i], 0, exit_group_peer, 0) != 0)
                _exit(95);
        pthread_mutex_lock(&blocked_group_mutex);
        while (blocked_group_ready != 2)
            pthread_cond_wait(&blocked_group_condition, &blocked_group_mutex);
        pthread_mutex_unlock(&blocked_group_mutex);
        struct timespec pause = { .tv_sec = 0, .tv_nsec = 10000000L };
        nanosleep(&pause, 0);
        syscall(SYS_exit_group, 27);
        _exit(96);
    }
    if (wait_status(child, 27) != 0) return 7;
    return 0;
}

static int check_raw_futex(void)
{
    int word = 1;
    struct timespec zero = {0};
    const struct {
        void *address;
        int operation;
        int value;
        const struct timespec *timeout;
        unsigned int bitset;
        int error;
    } cases[] = {
        { &word, FUTEX_WAIT_PRIVATE, 0, 0, 0, EAGAIN },
        { (void *)(uintptr_t)8, FUTEX_WAIT_PRIVATE, 0, 0, 0, EFAULT },
        { (char *)&word + 1, FUTEX_WAIT_PRIVATE, 1, 0, 0, EINVAL },
        { &word, FUTEX_WAIT_PRIVATE, 1, &zero, 0, ETIMEDOUT },
        { &word, FUTEX_WAIT_BITSET_PRIVATE, 1, &zero,
          FUTEX_BITSET_MATCH_ANY, ETIMEDOUT },
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        errno = 0;
        if (syscall(SYS_futex, cases[i].address, cases[i].operation,
                    cases[i].value, cases[i].timeout, 0,
                    cases[i].bitset) != -1 || errno != cases[i].error)
            return (int)i + 1;
    }
    /* Untimed FUTEX_WAIT follows ERESTARTSYS: a caught handler without
     * SA_RESTART exposes EINTR, while SA_RESTART retries until a wake. */
    if (run_futex_signal_case(0, 0, 0, EINTR) != 0) return 10;
    if (run_futex_signal_case(1, 0, 0, 0) != 0) return 11;

    /* A retried wait must compare the futex word again. The handler changes
     * it before sigreturn, so the retry observes EAGAIN rather than sleeping. */
    if (run_futex_signal_case(1, 0, 1, EAGAIN) != 0) return 12;

    /* Timed waits use restart-block semantics: a caught handler sees EINTR
     * even with SA_RESTART. */
    if (run_futex_signal_case(0, 1, 0, EINTR) != 0) return 13;
    if (run_futex_signal_case(1, 1, 0, EINTR) != 0) return 14;

    /* A stop/continue has no userspace handler, so restart_syscall resumes
     * the timed wait against its first absolute deadline. */
    if (check_timed_futex_stop_restart() != 0) return 15;
    if (check_futex_wake_signal_boundary() != 0) return 16;
    if (check_futex_timeout_signal_boundary() != 0) return 17;
    if (check_bitset_stop_restart() != 0) return 18;
    return 0;
}

struct robust_list_head_probe {
    void *next;
    long futex_offset;
    void *pending;
};

struct robust_entry_probe {
    void *next;
    uint32_t word;
};

enum robust_probe_mode {
    ROBUST_LIST_OWNER,
    ROBUST_PENDING_OWNER,
    ROBUST_OTHER_OWNER,
    ROBUST_CIRCULAR,
    ROBUST_BAD_HEAD,
    ROBUST_WAIT_OWNER,
    ROBUST_COW,
    ROBUST_PI_MARKER,
    ROBUST_PENDING_UNLOCK_WAKE,
    ROBUST_BAD_OFFSET,
};

struct robust_exit_probe {
    struct robust_list_head_probe head;
    struct robust_entry_probe entry;
    int registration_error;
    volatile long tid;
    volatile int go;
    int mode;
};

static volatile int robust_cow_go;

static void *robust_release_after_delay(void *opaque)
{
    struct robust_exit_probe *probe = opaque;
    struct timespec delay = {.tv_nsec = 20000000L};

    nanosleep(&delay, 0);
    probe->go = 1;
    return 0;
}

static void *robust_raw_exit_worker(void *opaque)
{
    struct robust_exit_probe *probe = opaque;
    long tid = syscall(SYS_gettid);

    probe->head.next = probe->mode == ROBUST_PENDING_OWNER ||
                        probe->mode == ROBUST_PENDING_UNLOCK_WAKE ?
                           (void *)&probe->head :
                       probe->mode == ROBUST_PI_MARKER ?
                           (void *)((uintptr_t)&probe->entry | 1U) :
                           (void *)&probe->entry;
    probe->head.futex_offset = probe->mode == ROBUST_BAD_OFFSET ? LONG_MAX :
                                (long)sizeof(probe->entry.next);
    probe->head.pending = probe->mode == ROBUST_PENDING_OWNER ||
                          probe->mode == ROBUST_PENDING_UNLOCK_WAKE ?
                              (void *)&probe->entry : 0;
    probe->entry.next = probe->mode == ROBUST_CIRCULAR ?
                                             (void *)&probe->entry :
                                             (void *)&probe->head;
    probe->entry.word = probe->mode == ROBUST_PENDING_UNLOCK_WAKE ? 0U :
                        (probe->mode == ROBUST_OTHER_OWNER ? 777U :
                                                               (uint32_t)tid) |
                            UINT32_C(0x80000000);
    if (syscall(SYS_set_robust_list,
                probe->mode == ROBUST_BAD_HEAD ? (void *)(uintptr_t)8U :
                                   (void *)&probe->head,
                sizeof(probe->head)) != 0)
        probe->registration_error = errno;
    probe->tid = tid;
    while ((probe->mode == ROBUST_WAIT_OWNER ||
            probe->mode == ROBUST_PENDING_UNLOCK_WAKE) && !probe->go)
        sched_yield();
    while (probe->mode == ROBUST_COW && !robust_cow_go) sched_yield();
    syscall(SYS_exit, 0);
    __builtin_unreachable();
}

static int robust_raw_exit_case(int mode, int *wait_result)
{
    struct robust_exit_probe local = {0};
    struct robust_exit_probe *probe = &local;
    pthread_t thread;

    if (mode == ROBUST_COW) {
        probe = mmap(0, 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (probe == MAP_FAILED) return 1;
        robust_cow_go = 0;
    }
    probe->mode = mode;
    if (pthread_create(&thread, 0, robust_raw_exit_worker, probe) != 0)
        return 1;
    for (int tries = 0; tries < 10000 && probe->tid == 0; tries++)
        sched_yield();
    if (probe->tid == 0) return 2;
    if (mode == ROBUST_COW) {
        pid_t child = fork();
        if (child < 0) return 6;
        if (child == 0) _exit(0);
        if (wait_status(child, 0) != 0) return 7;
        robust_cow_go = 1;
    }
    if (mode == ROBUST_WAIT_OWNER) {
        struct timespec timeout = {.tv_sec = 1};
        uint32_t expected = probe->entry.word;
        long result;

        probe->go = 1;
        result = syscall(SYS_futex, &probe->entry.word, FUTEX_WAIT,
                         expected, &timeout, 0, 0);
        if (wait_result != 0)
            *wait_result = result == 0 ? 1 :
                           result == -1 && errno == EAGAIN ? 0 : -1;
    }
    if (mode == ROBUST_PENDING_UNLOCK_WAKE) {
        struct timespec timeout = {.tv_sec = 1};
        pthread_t releaser;
        long result;

        if (pthread_create(&releaser, 0, robust_release_after_delay,
                           probe) != 0) return 9;
        result = syscall(SYS_futex, &probe->entry.word, FUTEX_WAIT,
                         0, &timeout, 0, 0);
        if (pthread_join(releaser, 0) != 0) return 10;
        if (wait_result != 0) *wait_result = result == 0 ? 1 : -1;
    }
    for (int tries = 0; tries < 10000; tries++) {
        errno = 0;
        if (syscall(SYS_tgkill, getpid(), probe->tid, 0) == -1 &&
            errno == ESRCH)
            break;
        if (tries == 9999) return 3;
        sched_yield();
    }
    if (probe->registration_error != 0)
        return 4;
    if (probe->entry.word !=
        (mode == ROBUST_PENDING_UNLOCK_WAKE ? 0U :
         mode == ROBUST_OTHER_OWNER ? UINT32_C(0x80000309) :
         mode == ROBUST_BAD_HEAD || mode == ROBUST_PI_MARKER ||
         mode == ROBUST_BAD_OFFSET ?
             ((uint32_t)probe->tid | UINT32_C(0x80000000)) :
                     UINT32_C(0xc0000000)))
        return 5;
    if (mode == ROBUST_COW && munmap(probe, 4096) != 0) return 8;
    return 0;
}

static int check_robust_raw_exit(void)
{
    int woken = 0;

    for (int mode = ROBUST_LIST_OWNER; mode <= ROBUST_BAD_HEAD; mode++) {
        int result = robust_raw_exit_case(mode, 0);
        if (result != 0) return 10 * mode + result;
    }
    if (robust_raw_exit_case(ROBUST_COW, 0) != 0) return 80;
    if (robust_raw_exit_case(ROBUST_PI_MARKER, 0) != 0) return 81;
    if (robust_raw_exit_case(ROBUST_BAD_OFFSET, 0) != 0) return 83;
    {
        int woken_pending = -1;
        if (robust_raw_exit_case(ROBUST_PENDING_UNLOCK_WAKE,
                                 &woken_pending) != 0 ||
            woken_pending != 1) return 82;
    }
    for (int tries = 0; tries < 4; tries++) {
        int observed = -1;
        int result = robust_raw_exit_case(ROBUST_WAIT_OWNER, &observed);
        if (result != 0 || observed < 0) return 60 + result;
        woken += observed;
    }
    return woken != 0 ? 0 : 70;
}

static int check_robust_registration(void)
{
    struct robust_list_head_probe head = {0};
    void *observed = (void *)(uintptr_t)1U;
    size_t length = 0U;
    long tid = syscall(SYS_gettid);

    errno = 0;
    if (syscall(SYS_set_robust_list, &head, sizeof(head)) != 0)
        return 1;
    if (syscall(SYS_get_robust_list, 0, &observed, &length) != 0 ||
        observed != &head || length != sizeof(head))
        return 2;
    observed = 0;
    if (syscall(SYS_get_robust_list, tid, &observed, &length) != 0 ||
        observed != &head || length != sizeof(head))
        return 6;
    observed = (void *)(uintptr_t)1U;
    errno = 0;
    if (syscall(SYS_get_robust_list, -1, &observed, &length) != -1 ||
        errno != ESRCH || observed != (void *)(uintptr_t)1U)
        return 7;
    length = 0U;
    errno = 0;
    if (syscall(SYS_get_robust_list, 0, (void *)(uintptr_t)8U,
                &length) != -1 || errno != EFAULT ||
        length != sizeof(head))
        return 8;
    observed = (void *)(uintptr_t)1U;
    errno = 0;
    if (syscall(SYS_get_robust_list, 0, &observed,
                (void *)(uintptr_t)8U) != -1 || errno != EFAULT ||
        observed != (void *)(uintptr_t)1U)
        return 9;
    errno = 0;
    if (syscall(SYS_set_robust_list, &head, sizeof(head) - 1U) != -1 ||
        errno != EINVAL)
        return 3;
    if (syscall(SYS_set_robust_list, 0, sizeof(head)) != 0)
        return 4;
    observed = (void *)(uintptr_t)1U;
    if (syscall(SYS_get_robust_list, 0, &observed, &length) != 0 ||
        observed != 0 || length != sizeof(head))
        return 5;
    if (syscall(SYS_set_robust_list, (void *)(uintptr_t)8U,
                sizeof(head)) != 0)
        return 10;
    if (syscall(SYS_get_robust_list, 0, &observed, &length) != 0 ||
        observed != (void *)(uintptr_t)8U)
        return 11;
    if (syscall(SYS_set_robust_list, 0, sizeof(head)) != 0)
        return 12;
    return 0;
}

struct robust_remote_probe {
    struct robust_list_head_probe head;
    volatile long tid;
    volatile int go;
    int error;
};

static void *robust_remote_worker(void *opaque)
{
    struct robust_remote_probe *probe = opaque;

    probe->head.next = &probe->head;
    if (syscall(SYS_set_robust_list, &probe->head,
                sizeof(probe->head)) != 0)
        probe->error = errno;
    probe->tid = syscall(SYS_gettid);
    while (!probe->go) sched_yield();
    return 0;
}

static int check_robust_remote_query(void)
{
    struct robust_remote_probe probe = {0};
    pthread_t thread;
    void *observed = 0;
    size_t length = 0;

    if (pthread_create(&thread, 0, robust_remote_worker, &probe) != 0)
        return 1;
    for (int tries = 0; tries < 10000 && probe.tid == 0; tries++)
        sched_yield();
    if (probe.tid == 0 || probe.error != 0) return 2;
    if (syscall(SYS_get_robust_list, probe.tid,
                &observed, &length) != 0 ||
        observed != &probe.head || length != sizeof(probe.head))
        return 3;
    probe.go = 1;
    if (pthread_join(thread, 0) != 0) return 4;
    errno = 0;
    if (syscall(SYS_get_robust_list, probe.tid,
                &observed, &length) != -1 || errno != ESRCH)
        return 5;
    return 0;
}

static void *robust_mutex_owner(void *opaque)
{
    pthread_mutex_t *mutex = opaque;

    return pthread_mutex_lock(mutex) == 0 ? 0 : (void *)1;
}

static int check_robust_mutex_protocol(void)
{
    pthread_mutexattr_t attr;
    pthread_mutex_t mutex;
    pthread_t thread;
    void *result = (void *)1;

    if (pthread_mutexattr_init(&attr) != 0 ||
        pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) != 0 ||
        pthread_mutex_init(&mutex, &attr) != 0)
        return 1;
    if (pthread_create(&thread, 0, robust_mutex_owner, &mutex) != 0 ||
        pthread_join(thread, &result) != 0 || result != 0)
        return 2;
    if (pthread_mutex_lock(&mutex) != EOWNERDEAD ||
        pthread_mutex_consistent(&mutex) != 0 ||
        pthread_mutex_unlock(&mutex) != 0 ||
        pthread_mutex_lock(&mutex) != 0 ||
        pthread_mutex_unlock(&mutex) != 0)
        return 3;
    if (pthread_create(&thread, 0, robust_mutex_owner, &mutex) != 0 ||
        pthread_join(thread, &result) != 0 || result != 0)
        return 4;
    if (pthread_mutex_lock(&mutex) != EOWNERDEAD ||
        pthread_mutex_unlock(&mutex) != 0 ||
        pthread_mutex_lock(&mutex) != ENOTRECOVERABLE)
        return 5;
    if (pthread_mutex_destroy(&mutex) != 0 ||
        pthread_mutexattr_destroy(&attr) != 0)
        return 6;
    return 0;
}

static int execed_mode(const char *expected)
{
    char *end = 0;
    long pid = strtol(expected, &end, 10);
    void *robust_head = (void *)(uintptr_t)1U;
    size_t robust_length = 0;

    if (expected[0] == 0 || end == 0 || *end != 0 || pid <= 0 ||
        getpid() != pid || syscall(SYS_gettid) != pid) return 22;
    if (syscall(SYS_get_robust_list, 0, &robust_head,
                &robust_length) != 0 || robust_head != 0 ||
        robust_length != 3U * sizeof(long)) return 24;
    return 23;
}

static int unix_dgram_fd;
static volatile int unix_dgram_started, unix_dgram_finished;
static ssize_t unix_dgram_result;
static void *unix_dgram_sender(void *argument)
{
    (void)argument;
    unsigned char bytes[32] = {0x7b};
    unix_dgram_started = 1;
    unix_dgram_result = write(unix_dgram_fd, bytes, sizeof(bytes));
    unix_dgram_finished = 1;
    return 0;
}
static int check_unix_datagram_wait(void)
{
    static unsigned char bytes[65536];
    int pair[2]; pthread_t writer;
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, pair)) return 1;
    unix_dgram_fd = pair[0];
    if (write(pair[0], bytes, 65520) != 65520) return 2;
    unix_dgram_started = unix_dgram_finished = 0;
    if (pthread_create(&writer, 0, unix_dgram_sender, 0)) return 3;
    while (!unix_dgram_started) sched_yield();
    for (unsigned i = 0; i < 16; i++) sched_yield();
    if (unix_dgram_finished) return 4;
    if (read(pair[1], bytes, 1) != 1 || pthread_join(writer, 0) || unix_dgram_result != 32 ||
        read(pair[1], bytes, sizeof(bytes)) != 32 || bytes[0] != 0x7b) return 5;
    if (write(pair[0], bytes, sizeof(bytes)) != sizeof(bytes)) return 6;
    unix_dgram_started = unix_dgram_finished = 0;
    if (pthread_create(&writer, 0, unix_dgram_sender, 0)) return 7;
    while (!unix_dgram_started) sched_yield();
    for (unsigned i = 0; i < 16; i++) sched_yield();
    void *result = 0;
    if (pthread_cancel(writer) || pthread_join(writer, &result) || result != PTHREAD_CANCELED) return 8;
    if (read(pair[1], bytes, 1) != 1 || fcntl(pair[1], F_SETFL, O_NONBLOCK)) return 9;
    if (read(pair[1], bytes, 1) != -1 || errno != EAGAIN) return 10;
    close(pair[0]); close(pair[1]);
    return 0;
}

static int run_check(const char *name, const char *marker,
                     int (*check)(void))
{
    int result = check();
    if (result != 0) {
        fprintf(stderr, "pthread check failed: %s:%d errno=%d\n",
                name, result, errno);
        return result;
    }
    puts(marker);
    return fflush(stdout) == 0 ? 0 : 1;
}

struct group_limit_case {
    struct rlimit nofile;
    struct rlimit stack;
    int result;
};

static void *group_limit_worker(void *opaque)
{
    struct group_limit_case *test = opaque;
    struct rlimit nofile = test->nofile;
    struct rlimit stack = test->stack;
    nofile.rlim_cur--;
    stack.rlim_cur -= 4096;
    if (setrlimit(RLIMIT_NOFILE, &nofile) != 0 ||
        setrlimit(RLIMIT_STACK, &stack) != 0 ||
        getrlimit(RLIMIT_NOFILE, &nofile) != 0 ||
        getrlimit(RLIMIT_STACK, &stack) != 0 ||
        nofile.rlim_cur != test->nofile.rlim_cur - 1 ||
        stack.rlim_cur != test->stack.rlim_cur - 4096)
        test->result = 1;
    return 0;
}

static int check_group_limits(void)
{
    struct group_limit_case test = {0};
    struct rlimit observed;
    pthread_t thread;
    int result = 0;

    if (getrlimit(RLIMIT_NOFILE, &test.nofile) != 0 ||
        getrlimit(RLIMIT_STACK, &test.stack) != 0 ||
        test.nofile.rlim_cur < 4 || test.stack.rlim_cur < 8192)
        return 1;
    if (pthread_create(&thread, 0, group_limit_worker, &test) != 0)
        return 2;
    if (pthread_join(thread, 0) != 0 || test.result != 0)
        result = 3;
    if (getrlimit(RLIMIT_NOFILE, &observed) != 0 ||
        observed.rlim_cur != test.nofile.rlim_cur - 1)
        result = 4;
    if (getrlimit(RLIMIT_STACK, &observed) != 0 ||
        observed.rlim_cur != test.stack.rlim_cur - 4096)
        result = 5;
    if (setrlimit(RLIMIT_NOFILE, &test.nofile) != 0 ||
        setrlimit(RLIMIT_STACK, &test.stack) != 0)
        return 6;
    errno = 0;
    if (getrlimit(RLIMIT_CORE, &observed) != -1 || errno != ENOTSUP)
        return 7;
    return result;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "execed") == 0)
        return execed_mode(argv[2]);

    if (run_check("executable TLS", "BoarOS: real pthread TLS checks ok",
                  check_executable_tls) != 0) return 1;
    if (run_check("synchronization",
                  "BoarOS: real pthread synchronization checks ok",
                  check_synchronization) != 0) return 2;
    if (run_check("cancellation",
                  "BoarOS: real pthread cancellation checks ok",
                  check_cancellation) != 0) return 3;
    if (run_check("dynamic TLS",
                  "BoarOS: real pthread dlopen TLS checks ok",
                  check_dlopen_tls) != 0) return 4;
    if (run_check("shared fd",
                  "BoarOS: real pthread shared fd checks ok",
                  check_shared_fd_pin) != 0) return 5;
    if (run_check("shared socket",
                  "BoarOS: real pthread shared socket checks ok",
                  check_shared_socket_pin) != 0) return 13;
    if (run_check("socket group exit",
                  "BoarOS: real pthread socket group exit checks ok",
                  check_socket_group_exit) != 0) return 14;
    if (run_check("socket pool pressure",
                  "BoarOS: real pthread socket pool pressure checks ok",
                  check_socket_pool_pressure) != 0) return 15;
    if (run_check("unix datagram wait", "BoarOS: real pthread unix datagram wait checks ok",
                  check_unix_datagram_wait) != 0) return 16;
    if (run_check("group limits",
                  "BoarOS: real pthread group limits checks ok",
                  check_group_limits) != 0) return 8;
    if (run_check("thread lifecycle",
                  "BoarOS: real pthread lifecycle checks ok",
                  check_thread_lifecycle) != 0) return 6;
    if (run_check("raw futex",
                  "BoarOS: real pthread futex ABI checks ok",
                  check_raw_futex) != 0) return 7;
    if (run_check("robust registration",
                  "BoarOS: real pthread robust registration checks ok",
                  check_robust_registration) != 0) return 9;
    if (run_check("robust remote query",
                  "BoarOS: real pthread robust remote query checks ok",
                  check_robust_remote_query) != 0) return 11;
    if (run_check("robust mutex protocol",
                  "BoarOS: real pthread robust mutex protocol checks ok",
                  check_robust_mutex_protocol) != 0) return 12;
    if (run_check("robust raw exit",
                  "BoarOS: real pthread robust raw exit checks ok",
                  check_robust_raw_exit) != 0) return 10;
    return 42;
}
