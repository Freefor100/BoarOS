#ifndef BOAROS_USERLAND_FAULT_SIGNALS_H
#define BOAROS_USERLAND_FAULT_SIGNALS_H

static volatile sig_atomic_t fault_seen;
static volatile sig_atomic_t fault_signal, fault_code, fault_repair;
static void *volatile fault_address;
static void fault_handler(int signal, siginfo_t *info, void *context)
{
    ucontext_t *uc = context;
    void *address = fault_address ? fault_address : (void *)uc->uc_mcontext.__gregs[REG_PC];
    if (signal != fault_signal || info->si_code != fault_code || info->si_addr != address)
        _exit(80);
    fault_seen++;
    if (fault_repair == 1) {
        if (mmap(address, 4096, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != address) _exit(81);
    } else if (fault_repair == 2) {
        if (mprotect(address, 4096, PROT_READ | PROT_WRITE)) _exit(82);
    } else {
        /* 测试指令强制为四字节；handler 修改上下文后跳过 fault。 */
        uc->uc_mcontext.__gregs[REG_PC] += 4;
    }
}
static void *fault_sibling(void *argument)
{
    (void)argument;
    for (;;) pause();
    return 0;
}
static int fault_child(unsigned scenario)
{
    fault_seen = 0; fault_address = 0; fault_repair = 0;
    fault_signal = scenario == 2 || scenario == 9 ? SIGBUS :
                   scenario == 3 ? SIGILL : scenario == 4 ? SIGTRAP : SIGSEGV;
    fault_code = scenario == 2 || scenario == 9 ? BUS_ADRERR :
                 scenario == 3 ? ILL_ILLOPC : scenario == 4 ? TRAP_BRKPT :
                 scenario == 1 ? SEGV_ACCERR : SEGV_MAPERR;
    struct sigaction action = {.sa_sigaction = fault_handler, .sa_flags = SA_SIGINFO};
    sigemptyset(&action.sa_mask);
    if (scenario == 5 || scenario == 9) action.sa_handler = SIG_DFL;
    if (scenario == 7) action.sa_handler = SIG_IGN;
    if (sigaction(fault_signal, &action, 0)) return 1;
    if (scenario == 6) {
        sigset_t mask; sigemptyset(&mask); sigaddset(&mask, fault_signal);
        if (sigprocmask(SIG_BLOCK, &mask, 0)) return 2;
    }
    if (scenario == 8) {
        __asm__ volatile("mv sp, %0; sd zero, 0(%0)" :: "r"((uintptr_t)0x12345000) : "memory");
        __builtin_unreachable();
    }
    if (scenario == 3) {
        __asm__ volatile(".option push\n.option norvc\n.word 0\n.option pop" ::: "memory");
    } else if (scenario == 4) {
        __asm__ volatile(".option push\n.option norvc\nebreak\n.option pop" ::: "memory");
    } else if (scenario == 2 || scenario == 9) {
        int fd = open("/fault-signal", O_CREAT | O_RDWR | O_TRUNC, 0600);
        if (fd < 0) return 5;
        fault_address = mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (fault_address == MAP_FAILED) return 6;
        if (scenario == 9) {
            pthread_t sibling;
            if (pthread_create(&sibling, 0, fault_sibling, 0)) return 7;
        }
        unsigned long value;
        __asm__ volatile(".option push\n.option norvc\nld %0, 0(%1)\n.option pop"
            : "=r"(value) : "r"(fault_address) : "memory");
        (void)value;
        munmap(fault_address, 4096);
    } else {
        void *page = mmap(0, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED) return 8;
        fault_address = page;
        if (scenario != 1 && munmap(page, 4096)) return 9;
        fault_repair = scenario == 1 ? 2 : 1;
        *(volatile unsigned char *)page = 0x6b;
        if (*(volatile unsigned char *)page != 0x6b) { fprintf(stderr, "fault repaired value=%x\n", *(volatile unsigned char *)page); return 10; }
        if (munmap(page, 4096)) { fprintf(stderr, "fault repaired unmap errno=%d\n", errno); return 12; }
    }
    return fault_seen == 1 ? 0 : 11;
}
static int check_fault_signals(void)
{
    for (unsigned scenario = 0; scenario < 10; scenario++) {
        pid_t child = fork();
        if (child < 0) return 10 + scenario;
        if (!child) _exit(fault_child(scenario));
        int status;
        if (waitpid(child, &status, 0) != child) return 20 + scenario;
        if (scenario < 5) {
            if (!WIFEXITED(status) || WEXITSTATUS(status)) { fprintf(stderr, "fault scenario=%u wait=%x\n", scenario, status); return 30 + scenario; }
        } else {
            int signal = scenario == 9 ? SIGBUS : SIGSEGV;
            if (!WIFSIGNALED(status) || WTERMSIG(status) != signal) return 40 + scenario;
        }
    }
    unlink("/fault-signal");
    return 0;
}
#endif
