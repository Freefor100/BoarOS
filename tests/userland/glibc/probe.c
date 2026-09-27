#define _GNU_SOURCE

#include <gnu/libc-version.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef GLIBC_PROBE_THREADS
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#endif

static __thread int program_tls = 7;

static void emit(const char *message, size_t length)
{
    if (write(STDOUT_FILENO, message, length) != (ssize_t)length)
        _exit(95);
}

#define EMIT(message) emit(message, sizeof(message) - 1U)

__attribute__((constructor)) static void before_main(void)
{
    EMIT("GLIBC CONSTRUCTOR\n");
}

static void exit_marker(void)
{
    EMIT("GLIBC EXIT\n");
}

#ifdef GLIBC_PROBE_THREADS
static volatile sig_atomic_t signal_seen;

static void signal_handler(int number)
{
    if (number == SIGUSR1) signal_seen = 1;
}

struct thread_probe {
    int (*library_bump)(void);
    int value;
};

static void *run_thread(void *argument)
{
    struct thread_probe *probe = argument;

    program_tls = 19;
    if (probe->library_bump() != 32) return (void *)1;
    probe->value = program_tls;
    return (void *)0x1234;
}
#endif

int main(void)
{
    EMIT("GLIBC MAIN\n");
    if (atexit(exit_marker) != 0) return 10;
    if (strcmp(gnu_get_libc_version(), "2.44") != 0) return 11;
    if (program_tls != 7) return 12;
    program_tls = 13;
    if (program_tls != 13) return 13;
    EMIT("GLIBC BASE OK\n");

#ifdef GLIBC_PROBE_THREADS
    void *library = dlopen("/lib/libboaros-glibc-tls.so", RTLD_NOW);
    if (library == 0) return 20;
    int (*library_bump)(void) = dlsym(library, "library_bump");
    if (library_bump == 0 || library_bump() != 32 ||
        library_bump() != 33) return 21;
    EMIT("GLIBC DLOPEN TLS OK\n");

    struct thread_probe probe = {.library_bump = library_bump};
    pthread_t thread;
    void *joined = 0;
    if (pthread_create(&thread, 0, run_thread, &probe) != 0) return 22;
    if (pthread_join(thread, &joined) != 0) return 23;
    if (joined != (void *)0x1234 || probe.value != 19 ||
        program_tls != 13 || library_bump() != 34) return 24;
    EMIT("GLIBC PTHREAD TLS OK\n");

    struct sigaction action = {.sa_handler = signal_handler};
    if (sigemptyset(&action.sa_mask) != 0 ||
        sigaction(SIGUSR1, &action, 0) != 0 ||
        raise(SIGUSR1) != 0 || !signal_seen) return 25;
    EMIT("GLIBC SIGNAL OK\n");
    if (dlclose(library) != 0) return 26;
#endif

    EMIT("GLIBC RUNTIME OK\n");
    return 42;
}
