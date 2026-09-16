#include <arch/riscv/trap.h>
#include <kernel/stack.h>

#include <stdio.h>

/* Host-side probe uses the same build CPPFLAGS and source constants as the
 * kernel. The checker must not silently drift when the assembly frame or
 * allocation/guard/reserve policy changes. */
int main(void)
{
    printf("--stack-bytes %llu --guard-bytes %u "
           "--trap-frame-bytes %u --reserve-bytes %u\n",
           (unsigned long long)KERNEL_STACK_BYTES,
           KERNEL_STACK_GUARD_BYTES, (unsigned)RISCV_TRAP_FRAME_SIZE,
           KERNEL_STACK_MINIMUM_RESERVE);
    return 0;
}
