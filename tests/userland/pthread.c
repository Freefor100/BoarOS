#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define WORKERS 3
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAIT_PRIVATE (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)
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
        syscall(SYS_futex, &test.word, FUTEX_WAKE, 1, 0, 0, 0);
        pthread_join(thread, &thread_result);
        return 3;
    }
    if (!change_word) {
        test.word = 1;
        syscall(SYS_futex, &test.word, FUTEX_WAKE, 1, 0, 0, 0);
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
    if (syscall(SYS_futex, &test.word, FUTEX_WAKE, 1, 0, 0, 0) != 1 ||
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
        { &word, FUTEX_WAIT_BITSET_PRIVATE, 1, 0,
          FUTEX_BITSET_MATCH_ANY, ENOSYS },
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
    return 0;
}

static int execed_mode(const char *expected)
{
    char *end = 0;
    long pid = strtol(expected, &end, 10);

    if (expected[0] == 0 || end == 0 || *end != 0 || pid <= 0 ||
        getpid() != pid || syscall(SYS_gettid) != pid) return 22;
    return 23;
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
    if (run_check("thread lifecycle",
                  "BoarOS: real pthread lifecycle checks ok",
                  check_thread_lifecycle) != 0) return 6;
    if (run_check("raw futex",
                  "BoarOS: real pthread futex ABI checks ok",
                  check_raw_futex) != 0) return 7;
    return 42;
}
