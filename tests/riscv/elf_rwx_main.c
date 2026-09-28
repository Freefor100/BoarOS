static volatile unsigned long value = 1;

static long call3(long nr, long x, long y, long z)
{
    register long a0 __asm__("a0") = x;
    register long a1 __asm__("a1") = y;
    register long a2 __asm__("a2") = z;
    register long a7 __asm__("a7") = nr;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

void _start(void)
{
    static const char passed[] = "ELF RWX PASS\n";
    static const char failed[] = "ELF RWX FAIL\n";
    value = 42;
    if (value == 42) {
        call3(64, 1, (long)passed, sizeof(passed) - 1);
        call3(93, 0, 0, 0);
    } else {
        call3(64, 1, (long)failed, sizeof(failed) - 1);
        call3(93, 1, 0, 0);
    }
    for (;;) { }
}
