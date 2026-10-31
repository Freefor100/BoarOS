#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { \
    dprintf(2, "VDSO FAIL line=%d errno=%d: %s\n", __LINE__, errno, #x); \
    _exit(1); } } while (0)

static unsigned char *vdso;
static size_t page, vdso_length;
static unsigned char original[32];
static pthread_barrier_t barrier;
static _Thread_local volatile sig_atomic_t handled;

static void handler(int sig) { (void)sig; handled++; }

static void signal_roundtrip(void)
{
    sig_atomic_t prior = handled;
    CHECK(pthread_kill(pthread_self(), SIGUSR1) == 0);
    CHECK(handled == prior + 1);
}

static void same_bytes(void)
{
    CHECK(memcmp(vdso, original, sizeof(original)) == 0);
}

static void gate(void)
{
    int rc = pthread_barrier_wait(&barrier);
    CHECK(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD);
}

static void *reader(void *unused)
{
    (void)unused;
    same_bytes(); gate();
    CHECK(madvise(vdso, page, MADV_DONTNEED) == 0);
    gate(); same_bytes(); signal_roundtrip();
    return NULL;
}

static unsigned char *find_vdso(void)
{
    uintptr_t aux = getauxval(AT_SYSINFO_EHDR);
    char line[512]; unsigned long start, end;
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) {
        CHECK(mkdir("/proc", 0755) == 0 || errno == EEXIST);
        CHECK(mount("proc", "/proc", "proc", 0, NULL) == 0);
        maps = fopen("/proc/self/maps", "r");
    }
    CHECK(maps != NULL);
    while (fgets(line, sizeof(line), maps)) {
        if (strstr(line, "[vdso]") && sscanf(line, "%lx-%lx", &start, &end) == 2) {
            fclose(maps);
            CHECK(end >= start + page);
            vdso_length = end - start;
            CHECK(!aux || aux == (uintptr_t)start);
            printf("VDSO SOURCE %s pages=%zu\n", aux ? "auxv" : "proc-maps-no-auxv",
                   vdso_length / page);
            return (unsigned char *)start;
        }
    }
    fclose(maps);
    CHECK(0 && "vDSO mapping not found");
    return NULL;
}

int main(void)
{
    struct sigaction action = {0};
    pthread_t one, two;
    int status;
    page = (size_t)sysconf(_SC_PAGESIZE);
    vdso = find_vdso();
    CHECK(((uintptr_t)vdso & (page - 1)) == 0);
    memcpy(original, vdso, sizeof(original));
    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGUSR1, &action, NULL) == 0);

    CHECK(madvise(vdso, page, MADV_DONTNEED) == 0);
    same_bytes(); signal_roundtrip();
    puts("VDSO CASE discard-reload-sigreturn PASS");

    pid_t child = fork(); CHECK(child >= 0);
    if (child == 0) {
        CHECK(madvise(vdso, page, MADV_DONTNEED) == 0);
        same_bytes(); signal_roundtrip(); _exit(0);
    }
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    same_bytes(); signal_roundtrip();
    puts("VDSO CASE fork-discard-reload PASS");

    CHECK(pthread_barrier_init(&barrier, NULL, 3) == 0);
    CHECK(pthread_create(&one, NULL, reader, NULL) == 0);
    CHECK(pthread_create(&two, NULL, reader, NULL) == 0);
    gate(); gate();
    CHECK(pthread_join(one, NULL) == 0 && pthread_join(two, NULL) == 0);
    CHECK(pthread_barrier_destroy(&barrier) == 0);
    same_bytes(); signal_roundtrip();
    puts("VDSO CASE concurrent-discard-reload PASS");

    /* Linux special mappings cannot be split; protect their entire VMA. */
    CHECK(mprotect(vdso, vdso_length, PROT_NONE) == 0);
    CHECK(madvise(vdso, page, MADV_DONTNEED) == 0);
    CHECK(mprotect(vdso, vdso_length, PROT_READ | PROT_EXEC) == 0);
    same_bytes(); signal_roundtrip();
    puts("VDSO CASE protection-discard-reload PASS");
    puts("VDSO END 4");
    return 0;
}
