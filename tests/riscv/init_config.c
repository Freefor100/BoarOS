__asm__(".section .text.start,\"ax\"\n.global _start\n_start:\n mv a0,sp\n call probe\n1: j 1b\n");
static long call(long n, long x, long y, long z)
{
    register long a0 __asm__("a0") = x;
    register long a1 __asm__("a1") = y;
    register long a2 __asm__("a2") = z;
    register long a7 __asm__("a7") = n;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}
static int eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
void probe(const unsigned long *sp)
{
    const char **a = (void *)(sp + 1);
    const char **e = a + sp[0] + 1;
    int ok = sp[0] == 1 ? eq(a[0], "/init") && !e[0] :
        sp[0] == 2 && eq(a[0], "custom-zero") && eq(a[1], "arg with spaces") &&
        e[0] && eq(e[0], "MODE=probe") && e[1] && eq(e[1], "SECOND=") && !e[2];
    const char *path = sp[0] == 1 ? "/init" : "/entry";
    const unsigned long *aux = (void *)e;
    while (*aux) aux++;
    for (aux++; aux[0]; aux += 2) if (aux[0] == 31) ok = ok && eq((void *)aux[1], path);
    if (ok) call(64, 1, (long)(sp[0] == 1 ? "INIT DEFAULT PASS\n" : "INIT CUSTOM PASS!\n"), 18);
    call(93, ok ? 0 : 91, 0, 0);
}
