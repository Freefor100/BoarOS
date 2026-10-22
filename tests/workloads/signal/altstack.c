#define _GNU_SOURCE
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#define AUTO (1U << 31)
#if defined(__loongarch__)
#define MINIMUM 4096U
#else
#define MINIMUM 2048U
#endif
static unsigned failures;
#define CHECK(x) do { if (!(x)) { printf("ALTSTACK FAIL line=%d %s errno=%d\n", __LINE__, #x, errno); failures++; } } while (0)
static int error(long value, int code) { return value == -1 && errno == code; }
static int alt(const stack_t *input, stack_t *output) { return syscall(SYS_sigaltstack, input, output); }
static void abi(void)
{
    stack_t old, set, out;
    char *memory = mmap(0, 65536, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    CHECK(memory != MAP_FAILED);
    CHECK(!alt(0, &old) && old.ss_flags == SS_DISABLE && !old.ss_sp && !old.ss_size);
    set = (stack_t){.ss_sp=memory, .ss_size=MINIMUM};
    CHECK(!alt(&set, &out) && out.ss_flags == SS_DISABLE);
    CHECK(!alt(0, &out) && out.ss_sp == memory && out.ss_size == MINIMUM && !out.ss_flags);
    set.ss_size--;
    CHECK(error(alt(&set, 0), ENOMEM));
    set.ss_size=MINIMUM; set.ss_flags=SS_ONSTACK;
    CHECK(!alt(&set, 0));
    CHECK(!alt(0, &out) && !out.ss_flags);
    for (unsigned flags=0; flags<16; flags++) {
        set.ss_flags=(int)flags;
        int valid=flags==0 || flags==SS_ONSTACK || flags==SS_DISABLE;
        CHECK(valid ? !alt(&set, 0) : error(alt(&set, 0), EINVAL));
    }
    set.ss_flags=(int)AUTO;
    CHECK(!alt(&set, &set) && !set.ss_sp && set.ss_flags == SS_DISABLE);
    CHECK(!alt(0, &out) && (unsigned)out.ss_flags == AUTO);
    CHECK(error(alt((void *)UINTPTR_MAX, 0), EFAULT));
    CHECK(!alt(0, &out) && (unsigned)out.ss_flags == AUTO);
    set=(stack_t){.ss_sp=memory+16,.ss_size=32768};
    CHECK(error(alt(&set, (void *)UINTPTR_MAX), EFAULT));
    CHECK(!alt(0, &out) && out.ss_sp == set.ss_sp && out.ss_size == set.ss_size);
    /* Registration records an address; access is checked when delivering. */
    set=(stack_t){.ss_sp=(void *)UINTPTR_MAX,.ss_size=MINIMUM};
    CHECK(!alt(&set, 0));
    set.ss_flags=SS_DISABLE|(int)AUTO; set.ss_size=0;
    CHECK(!alt(&set, 0));
    CHECK(!alt(0, &out) && out.ss_flags == (int)(SS_DISABLE|AUTO) && !out.ss_sp && !out.ss_size);
    set.ss_flags=SS_DISABLE; CHECK(!alt(&set, 0));
    CHECK(!munmap(memory,65536));
}
int main(void)
{
    setvbuf(stdout,0,_IONBF,0); alarm(15);
    abi();
    printf("ALTSTACK END failures=%u\n",failures);
    return failures ? 1 : 0;
}
