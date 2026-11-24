#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#ifdef __GLIBC__
#include <sys/rseq.h>
#endif
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

#ifndef AT_RSEQ_FEATURE_SIZE
#define AT_RSEQ_FEATURE_SIZE 27
#define AT_RSEQ_ALIGN 28
#endif
#ifndef RSEQ_SIG
#if defined(__riscv)
#define RSEQ_SIG 0xf1401073
#elif defined(__loongarch__)
#define RSEQ_SIG 0x002a0010
#endif
#endif
#define CLASSIC_SIZE 32
#define UNREGISTER 1
enum {
    MEMBARRIER_CMD_QUERY = 0,
    MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ = 1 << 7,
    MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ = 1 << 8,
    MEMBARRIER_CMD_GET_REGISTRATIONS = 1 << 9,
    MEMBARRIER_CMD_FLAG_CPU = 1,
};
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "RSEQ FAIL line=%d %s errno=%d\n", __LINE__, #x, errno); \
    _exit(1); \
} } while (0)

struct classic_area {
    uint32_t cpu_start, cpu;
    uint64_t cs;
    uint32_t flags;
    uint32_t extension[3];
} __attribute__((aligned(CLASSIC_SIZE)));

static _Thread_local struct classic_area area;
static struct classic_area other;
extern int spin_sequence(struct classic_area *, atomic_int *, atomic_int *);
extern char sequence_abort[];

#define STRINGIFY_1(x) #x
#define STRINGIFY(x) STRINGIFY_1(x)

#if defined(__riscv)
#define SPIN_PUBLISH "lla t0, 4f\n sd t0, 8(a0)\n"
#define SPIN_START "li t0, 1\n fence rw,w\n sw t0, 0(a1)\n"
#define SPIN_LOOP "lw t0, 0(a2)\n beqz t0, 1b\n"
#define SPIN_RETURN(result) "li a0, " result "\n ret\n"
#elif defined(__loongarch__)
#define SPIN_PUBLISH "la.local $t0, 4f\n st.d $t0, $a0, 8\n"
#define SPIN_START "li.w $t0, 1\n dbar 0\n st.w $t0, $a1, 0\n"
#define SPIN_LOOP "ld.w $t0, $a2, 0\n beqz $t0, 1b\n"
#define SPIN_RETURN(result) "li.w $a0, " result "\n jr $ra\n"
#else
#error Add rseq test assembly for this target
#endif

__asm__(
    ".pushsection .text\n"
    ".balign 4\n .globl spin_sequence\n .type spin_sequence, @function\n"
    "spin_sequence:\n" SPIN_PUBLISH
    "0:\n" SPIN_START
    "1:\n" SPIN_LOOP
    "2:\n" SPIN_RETURN("0")
    ".balign 4\n .long " STRINGIFY(RSEQ_SIG) "\n"
    ".globl sequence_abort\n sequence_abort:\n" SPIN_RETURN("1")
    ".size spin_sequence, .-spin_sequence\n"
    ".popsection\n"
    ".pushsection .data.rel.ro\n .balign 32\n"
    "4:\n .long 0, 0\n .quad 0b, 2b - 0b, sequence_abort\n"
    ".popsection\n");

static long registration(void *pointer, unsigned length, unsigned flags, unsigned sig)
{
    return syscall(SYS_rseq, pointer, length, flags, sig);
}

static long barrier(int command, unsigned flags, int target)
{
    return syscall(SYS_membarrier, command, flags, target);
}

static int current_cpu(void)
{
    unsigned cpu;
    CHECK(!syscall(SYS_getcpu, &cpu, NULL, NULL));
    return (int)cpu;
}

static void check_cpu(struct classic_area *r)
{
    int cpu = current_cpu();
    CHECK(r->cpu_start == (unsigned)cpu && r->cpu == (unsigned)cpu);
}

static void check_invalid(void)
{
    CHECK(registration(&area, CLASSIC_SIZE - 1, 0, RSEQ_SIG) == -1 && errno == EINVAL);
    CHECK(registration((char *)&area + 8, CLASSIC_SIZE, 0, RSEQ_SIG) == -1 && errno == EINVAL);
    CHECK(registration(&area, CLASSIC_SIZE, 0x8000, RSEQ_SIG) == -1 && errno == EINVAL);
    CHECK(registration(&area, CLASSIC_SIZE, UNREGISTER, RSEQ_SIG) == -1 && errno == EINVAL);
    if (getauxval(AT_RSEQ_FEATURE_SIZE) == 20)
        CHECK(registration(&area, CLASSIC_SIZE + 1, 0, RSEQ_SIG) == -1 && errno == EINVAL);
    void *page = mmap(NULL, getpagesize(), PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(page != MAP_FAILED);
    CHECK(registration(page, CLASSIC_SIZE, 0, RSEQ_SIG) == -1 && errno == EFAULT);
    CHECK(!munmap(page, getpagesize()));
}

static void check_registration(void)
{
    area.cs = UINT64_MAX;
    area.flags = UINT32_MAX;
    memset(area.extension, 0xa5, sizeof(area.extension));
    CHECK(!registration(&area, CLASSIC_SIZE, 0, RSEQ_SIG));
    check_cpu(&area);
    CHECK(!area.cs && !area.flags);
    if (getauxval(AT_RSEQ_FEATURE_SIZE) == 20)
        CHECK(area.extension[0] == 0xa5a5a5a5 && area.extension[1] == 0xa5a5a5a5 &&
              area.extension[2] == 0xa5a5a5a5);
    CHECK(registration(&area, CLASSIC_SIZE, 0, RSEQ_SIG) == -1 && errno == EBUSY);
    CHECK(registration(&other, CLASSIC_SIZE, 0, RSEQ_SIG) == -1 && errno == EINVAL);
    CHECK(registration(&area, CLASSIC_SIZE, 0, RSEQ_SIG ^ 1) == -1 && errno == EPERM);
    CHECK(registration(&area, CLASSIC_SIZE, UNREGISTER, RSEQ_SIG ^ 1) == -1 && errno == EPERM);
    CHECK(registration(&other, CLASSIC_SIZE, UNREGISTER, RSEQ_SIG) == -1 && errno == EINVAL);
}

static void wait_child(pid_t child, int expected_signal)
{
    int status;
    CHECK(waitpid(child, &status, 0) == child);
    if (expected_signal)
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == expected_signal);
    else
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void check_fork_exec(const char *program)
{
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(registration(&area, CLASSIC_SIZE, 0, RSEQ_SIG) == -1 && errno == EBUSY);
        check_cpu(&area);
        execl(program, program, "--post-exec", (char *)NULL);
        CHECK(0);
    }
    wait_child(child, 0);
    check_cpu(&area);
}

static volatile sig_atomic_t signal_valid;
static atomic_int signal_release;

static void observe_signal(int sig, siginfo_t *info, void *arg)
{
    (void)info;
    ucontext_t *context = arg;
#if defined(__riscv)
    uintptr_t pc = context->uc_mcontext.__gregs[0];
#elif defined(__loongarch__)
    uintptr_t pc = context->uc_mcontext.__pc;
#endif
    signal_valid = sig == SIGALRM && pc == (uintptr_t)sequence_abort && !area.cs;
    atomic_store_explicit(&signal_release, 1, memory_order_release);
}

static void arm_signal(void)
{
    struct itimerval timer = {.it_value.tv_usec = 50000};
    CHECK(!setitimer(ITIMER_REAL, &timer, NULL));
}

static void use_fifo(int enable)
{
    struct sched_param priority = {.sched_priority = enable};
    CHECK(!syscall(SYS_sched_setscheduler, 0, enable ? SCHED_FIFO : SCHED_OTHER, &priority));
}

static void check_signal(void)
{
    struct sigaction action = {.sa_sigaction = observe_signal, .sa_flags = SA_SIGINFO};
    atomic_int ready = 0;
    CHECK(!sigaction(SIGALRM, &action, NULL));
    /* Hold the CPU until the timer interrupt so a separate task switch
     * cannot abort the same sequence before signal delivery. */
    use_fifo(1);
    arm_signal();
    CHECK(spin_sequence(&area, &ready, &signal_release) == 1);
    CHECK(signal_valid && ready && !area.cs);
    use_fifo(0);
    check_cpu(&area);

    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(!registration(&area, CLASSIC_SIZE, UNREGISTER, RSEQ_SIG));
        CHECK(!registration(&area, CLASSIC_SIZE, 0, RSEQ_SIG ^ 1));
        use_fifo(1);
        atomic_store(&signal_release, 0);
        arm_signal();
        spin_sequence(&area, &ready, &signal_release);
        _exit(2);
    }
    wait_child(child, SIGSEGV);
    puts("RSEQ PASS signal-frame-restart signature");
}

struct actor {
    int cpu, realtime, result, observed_cpu;
    atomic_int tid, ready, release, exited_sequence, done;
};

static void pin(int tid, int cpu)
{
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(cpu, &mask);
    CHECK(!sched_setaffinity(tid, sizeof(mask), &mask));
}

static void await(atomic_int *flag)
{
    for (int tries = 0; tries < 2000; tries++) {
        if (atomic_load_explicit(flag, memory_order_acquire))
            return;
        usleep(1000);
    }
    CHECK(0);
}

static void *worker(void *arg)
{
    struct actor *a = arg;
    pin(0, a->cpu);
    CHECK(!registration(&area, CLASSIC_SIZE, 0, RSEQ_SIG));
    check_cpu(&area);
    CHECK(current_cpu() == a->cpu);
    if (a->realtime)
        use_fifo(1);
    atomic_store(&a->tid, (int)syscall(SYS_gettid));
    a->result = spin_sequence(&area, &a->ready, &a->release);
    /* Sample before a syscall can re-enter the kernel and refresh TLS. */
    uint32_t cpu = area.cpu;
    uint32_t cpu_start = area.cpu_start;
    atomic_store_explicit(&a->exited_sequence, 1, memory_order_release);
    a->observed_cpu = current_cpu();
    CHECK(cpu == (unsigned)a->observed_cpu && cpu_start == cpu);
    CHECK(a->result != 1 || !area.cs);
    CHECK(!registration(&area, CLASSIC_SIZE, UNREGISTER, RSEQ_SIG));
    atomic_store_explicit(&a->done, 1, memory_order_release);
    return NULL;
}

static void check_remote(void)
{
    const int rseq = MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ;
    const int register_rseq = MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ;
    long query = barrier(MEMBARRIER_CMD_QUERY, 0, 0);
    CHECK(query >= 0 && (query & (rseq | register_rseq)) == (rseq | register_rseq));
    CHECK(barrier(rseq, 2, 0) == -1 && errno == EINVAL);
    CHECK(barrier(register_rseq, MEMBARRIER_CMD_FLAG_CPU, 0) == -1 && errno == EINVAL);
    CHECK(barrier(rseq, 0, 0) == -1 && errno == EPERM);
    CHECK(!barrier(register_rseq, 0, 0));
    CHECK(!barrier(register_rseq, 0, 0));
    long registrations = barrier(MEMBARRIER_CMD_GET_REGISTRATIONS, 0, 0);
    CHECK(registrations >= 0 && (registrations & register_rseq));
    CHECK(!barrier(rseq, MEMBARRIER_CMD_FLAG_CPU, 0x7fffffff));

    cpu_set_t possible;
    CHECK(!sched_getaffinity(0, sizeof(possible), &possible));
    int first = -1, second = -1;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (CPU_ISSET(cpu, &possible)) {
            if (first < 0) first = cpu;
            else { second = cpu; break; }
        }
    }
    CHECK(first >= 0);
    pin(0, first);
    struct actor local = {.cpu = first};
    pthread_t peer;
    CHECK(!pthread_create(&peer, NULL, worker, &local));
    await(&local.ready);
    await(&local.done);
    CHECK(!pthread_join(peer, NULL));
    CHECK(local.result == 1 && local.observed_cpu == first);
    puts("RSEQ PASS same-cpu-preemption");

    if (second < 0) {
        CHECK(!sched_setaffinity(0, sizeof(possible), &possible));
        puts("RSEQ PASS membarrier-registration");
        return;
    }
    for (int action = 0; action < 4; action++) {
        struct actor actor = {.cpu = second, .realtime = 1};
        pthread_t thread;
        CHECK(!pthread_create(&thread, NULL, worker, &actor));
        await(&actor.ready);
        if (action == 0) {
            CHECK(!barrier(rseq, MEMBARRIER_CMD_FLAG_CPU, 0x7fffffff));
            CHECK(!barrier(rseq, MEMBARRIER_CMD_FLAG_CPU, first));
            usleep(2000);
            CHECK(!atomic_load(&actor.exited_sequence));
            CHECK(!barrier(rseq, MEMBARRIER_CMD_FLAG_CPU, second));
        } else if (action == 1) {
            CHECK(!barrier(rseq, 0, 0));
        } else if (action == 2) {
            CHECK(!barrier(rseq, MEMBARRIER_CMD_FLAG_CPU, -7));
        } else {
            pin(atomic_load(&actor.tid), first);
        }
        await(&actor.done);
        CHECK(!pthread_join(thread, NULL));
        CHECK(actor.result == 1 && actor.observed_cpu == (action == 3 ? first : second));
    }
    CHECK(!sched_setaffinity(0, sizeof(possible), &possible));
    puts("RSEQ PASS membarrier-scoped membarrier-all migration");
}

#ifdef __GLIBC__
static void *check_glibc_thread(void *unused)
{
    (void)unused;
    struct classic_area *tls = (void *)((char *)__builtin_thread_pointer() + __rseq_offset);
    CHECK(__rseq_size >= 20);
    check_cpu(tls);
    unsigned length = __rseq_size > CLASSIC_SIZE ? __rseq_size : CLASSIC_SIZE;
    CHECK(registration(tls, length, 0, RSEQ_SIG) == -1 && errno == EBUSY);
    return NULL;
}
#endif

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc == 1) {
        CHECK(getauxval(AT_RSEQ_FEATURE_SIZE) >= 20);
        CHECK(getauxval(AT_RSEQ_ALIGN) >= CLASSIC_SIZE);
        printf("RSEQ ABI feature=%lu align=%lu\n",
               getauxval(AT_RSEQ_FEATURE_SIZE), getauxval(AT_RSEQ_ALIGN));
#ifdef __GLIBC__
        check_glibc_thread(NULL);
        pthread_t thread;
        CHECK(!pthread_create(&thread, NULL, check_glibc_thread, NULL));
        CHECK(!pthread_join(thread, NULL));
        puts("RSEQ PASS auxv glibc-main glibc-thread");
        CHECK(!setenv("GLIBC_TUNABLES", "glibc.pthread.rseq=0", 1));
        execl(argv[0], argv[0], "--raw", (char *)NULL);
        CHECK(0);
#else
        puts("RSEQ PASS auxv");
#endif
    }
#ifdef __GLIBC__
    CHECK(argc == 2 && __rseq_size == 0);
#else
    CHECK(argc == 1 || argc == 2);
#endif
    if (argc == 2 && !strcmp(argv[1], "--post-exec")) {
        CHECK(!registration(&area, CLASSIC_SIZE, 0, RSEQ_SIG));
        check_cpu(&area);
        return 0;
    }
    CHECK(argc == 1 || !strcmp(argv[1], "--raw"));
    check_invalid();
    check_registration();
    check_fork_exec(argv[0]);
    check_signal();
    check_remote();
    CHECK(!registration(&area, CLASSIC_SIZE, UNREGISTER, RSEQ_SIG));
    CHECK(!area.cpu_start && area.cpu == UINT32_MAX);
    CHECK(registration(&area, CLASSIC_SIZE, UNREGISTER, RSEQ_SIG) == -1 && errno == EINVAL);
    puts("RSEQ PASS registration fork-exec");
    puts("RSEQ PASS complete");
    return 0;
}
