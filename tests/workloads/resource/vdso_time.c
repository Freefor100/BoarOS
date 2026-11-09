#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "VDSO TIME FAIL %d: %s\n", __LINE__, #condition); exit(1); \
} } while (0)

static const Elf64_Ehdr *image;
static void *symbol(const char *name)
{
    uintptr_t base = (uintptr_t)image;
    const Elf64_Phdr *ph = (const void *)(base + image->e_phoff);
    const Elf64_Dyn *dyn = NULL;
    const Elf64_Sym *syms = NULL;
    const char *names = NULL;
    const uint32_t *hash = NULL;
    const Elf64_Half *versions = NULL;
    const Elf64_Verdef *def = NULL;
    for (unsigned i = 0; i < image->e_phnum; i++)
        if (ph[i].p_type == PT_DYNAMIC) dyn = (const void *)(base + ph[i].p_vaddr);
    CHECK(dyn);
    for (; dyn->d_tag != DT_NULL; dyn++) switch (dyn->d_tag) {
    case DT_SYMTAB: syms = (const void *)(base + dyn->d_un.d_ptr); break;
    case DT_STRTAB: names = (const void *)(base + dyn->d_un.d_ptr); break;
    case DT_HASH: hash = (const void *)(base + dyn->d_un.d_ptr); break;
    case DT_VERSYM: versions = (const void *)(base + dyn->d_un.d_ptr); break;
    case DT_VERDEF: def = (const void *)(base + dyn->d_un.d_ptr); break;
    }
    CHECK(syms && names && hash && versions && def);
    for (uint32_t i = 1; i < hash[1]; i++) {
        if (strcmp(names + syms[i].st_name, name)) continue;
        while (def->vd_ndx != (versions[i] & 0x7fff) && def->vd_next)
            def = (const void *)((uintptr_t)def + def->vd_next);
        CHECK(def->vd_ndx == (versions[i] & 0x7fff));
        const Elf64_Verdaux *aux = (const void *)((uintptr_t)def + def->vd_aux);
#ifdef __riscv
        CHECK(!strcmp(names + aux->vda_name, "LINUX_4.15"));
#else
        CHECK(!strcmp(names + aux->vda_name, "LINUX_5.10"));
#endif
        return (void *)(base + syms[i].st_value);
    }
    CHECK(!"missing symbol");
    return NULL;
}

static int64_t ns(struct timespec value)
{ return value.tv_sec * INT64_C(1000000000) + value.tv_nsec; }

static volatile sig_atomic_t handled;
static void on_signal(int sig) { handled = sig; }

int main(void)
{
    image = (const void *)getauxval(AT_SYSINFO_EHDR);
    CHECK(image && !memcmp(image->e_ident, ELFMAG, SELFMAG) && image->e_type == ET_DYN);
    int (*get)(clockid_t, struct timespec *) = symbol("__vdso_clock_gettime");
    int (*res)(clockid_t, struct timespec *) = symbol("__vdso_clock_getres");
    int (*wall)(struct timeval *, struct timezone *) = symbol("__vdso_gettimeofday");
    CHECK(symbol("__vdso_rt_sigreturn"));
    const clockid_t clocks[] = {CLOCK_REALTIME, CLOCK_MONOTONIC,
        CLOCK_REALTIME_COARSE, CLOCK_MONOTONIC_COARSE, CLOCK_THREAD_CPUTIME_ID,
        CLOCK_BOOTTIME};
    for (unsigned i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
        struct timespec first, sample, last, expected;
        CHECK(syscall(SYS_clock_gettime, clocks[i], &first) == 0);
        CHECK(get(clocks[i], &sample) == 0);
        CHECK(syscall(SYS_clock_gettime, clocks[i], &last) == 0);
        CHECK(ns(first) <= ns(sample) && ns(sample) <= ns(last));
        CHECK(syscall(SYS_clock_getres, clocks[i], &expected) == 0);
        CHECK(res(clocks[i], &sample) == 0 && ns(sample) == ns(expected));
        CHECK(res(clocks[i], NULL) == 0);
    }
    CHECK(get(-1, &(struct timespec){0}) < 0);
    /* BoarOS has no suspend: BOOTTIME and MONOTONIC share the same origin.
     * Bracket reads rather than assuming successive queries have equal values. */
    struct timespec before_boot, boot, after_boot, resolution;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &before_boot));
    CHECK(!clock_gettime(CLOCK_BOOTTIME, &boot));
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &after_boot));
    CHECK(ns(before_boot) <= ns(boot) && ns(boot) <= ns(after_boot));
    CHECK(!clock_getres(CLOCK_BOOTTIME, &resolution));
    CHECK(resolution.tv_sec == 0 && resolution.tv_nsec == 1);
    CHECK(!syscall(SYS_clock_getres, CLOCK_BOOTTIME, NULL));
    CHECK(syscall(SYS_clock_gettime, CLOCK_BOOTTIME, (void *)1) == -1 && errno == EFAULT);
    CHECK(syscall(SYS_clock_getres, CLOCK_BOOTTIME, (void *)1) == -1 && errno == EFAULT);
    struct timeval now;
    struct timezone zone = {1, 1};
    CHECK(wall(&now, &zone) == 0 && now.tv_usec >= 0 && now.tv_usec < 1000000);
    CHECK(!zone.tz_minuteswest && !zone.tz_dsttime);

    size_t page = getauxval(AT_PAGESZ);
    void *data = (void *)((uintptr_t)image + page);
    CHECK(mprotect(data, page, PROT_READ | PROT_WRITE) == -1);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        struct timespec before, after, delay = {.tv_nsec = 40000000};
        CHECK(!get(CLOCK_MONOTONIC_COARSE, &before));
        CHECK(madvise((void *)image, page, MADV_DONTNEED) == 0);
        CHECK(madvise(data, page, MADV_DONTNEED) == 0);
        CHECK(nanosleep(&delay, NULL) == 0);
        CHECK(!get(CLOCK_MONOTONIC_COARSE, &after) && ns(after) > ns(before));
        struct sigaction action = {.sa_handler = on_signal};
        CHECK(sigaction(SIGUSR1, &action, NULL) == 0);
        CHECK(raise(SIGUSR1) == 0 && handled == SIGUSR1);
        _exit(0);
    }
    int status;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    puts("VDSO TIME PASS");
    return 0;
}
