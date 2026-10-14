#include "abi.h"

void abi_exec_boundary_cases(void)
{
    static char argument[128];
    static const char *argv[1024];
    const char *envp[] = {0};
    struct { unsigned long current, maximum; } old, limit;
    abi_require(SC4(261, 0, 3, 0, &old) == 0);
    limit = old;
    limit.current = 131072;
    abi_require(SC4(261, 0, 3, &limit, 0) == 0);
    for (usize i = 0; i < sizeof(argument) - 1; i++) argument[i] = 'a';
    for (usize i = 0; i < 1023; i++) argv[i] = argument;
    long fd = abi_open("/boundary-exec", 2 | 64 | 512 | 0x80000);
    abi_require(fd >= 0 && SC3(64, fd, "old", 3) == 3);
    long pid = SC0(172);
    long ret = SC3(221, "/init", argv, envp);
    abi_record("boundary.exec-stack-limit", ret, -1, -1, 0, 0, 0);
    char content[3];
    int unchanged = SC0(172) == pid && SC3(25, fd, 1, 0) == 1 &&
        SC4(67, fd, content, sizeof(content), 0) == 3;
    abi_record("boundary.exec-failure-preserves", unchanged, abi_size(fd),
        abi_offset(fd), 0, content, sizeof(content));
    abi_require(SC4(261, 0, 3, &old, 0) == 0 && SC1(57, fd) == 0);
    abi_require(SC3(35, -100, "/boundary-exec", 0) == 0);
}

static void execute_instruction(volatile unsigned int *code)
{
#if defined(__loongarch__)
    *code = 0x4c000020; /* jirl zero, ra, 0 */
    __asm__ volatile("ibar 0" ::: "memory");
#else
    *code = 0x00008067; /* ret */
    __asm__ volatile("fence.i" ::: "memory");
#endif
    ((void (*)(void))code)();
}

__attribute__((noinline)) static void deep_stack(void)
{
    volatile unsigned int code[32768];
    /* The instruction resides below the initially committed headroom. */
    for (usize i = 0; i < sizeof(code) / sizeof(code[0]); i += ABI_PAGE_SIZE / 4)
        code[i] = 0;
    execute_instruction(&code[0]);
    __asm__ volatile("" :: "r"(&code[0]) : "memory");
}

void abi_stack_boundary_cases(void)
{
    const char *ids[] = {"stack.initial-execute", "stack.demand-execute"};
    for (unsigned i = 0; i < 2; i++) {
        long child = SC5(220, 17, 0, 0, 0, 0);
        abi_require(child >= 0);
        if (!child) {
            volatile unsigned int code __attribute__((aligned(16)));
            if (i) deep_stack();
            else execute_instruction(&code);
            abi_exit(42);
        }
        int status;
        abi_require(SC4(260, child, &status, 0, 0) == child);
        abi_record(ids[i], 0, -1, -1, status, 0, 0);
    }
}
