#include <kernel/cpu_config.h>
#if BOAROS_MAX_CPUS > 1
    .section .text.boot, "ax", @progbits
    .align 2
    .globl riscv_secondary_entry
    .type riscv_secondary_entry, @function
riscv_secondary_entry:
    /* a1指向CPU0已发布的物理mailbox；没有BSS或共享启动栈操作。 */
    csrw sie, zero
    csrci sstatus, 2
    csrw sscratch, zero
    fence r, rw
    ld sp, 0(a1)
    ld t0, 8(a1)
    ld tp, 24(a1)
    ld t1, 32(a1)
    la t2, riscv_trap_entry
    csrw stvec, t2
    csrw satp, t0
    sfence.vma
    add sp, sp, t1
    add tp, tp, t1
    add t2, t2, t1
    csrw stvec, t2
    la t0, 1f
    add t0, t0, t1
    jr t0
1:
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop
    call riscv_smp_secondary_high
    unimp
    .size riscv_secondary_entry, . - riscv_secondary_entry
#endif
