    .section .text.boot, "ax", @progbits
    .align 2
    .globl _start
    .type _start, @function

_start:
    /* OpenSBI provides a0=hart ID and a1=DTB; no firmware stack is ours. */
    mv s0, a0
    mv s1, a1

    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop

    /* The first slice has no interrupt handlers, so establish that state. */
    csrw sie, zero
    csrci sstatus, 2

    la t0, __bss_start
    la t1, __bss_end
1:
    bgeu t0, t1, 2f
    sd zero, 0(t0)
    addi t0, t0, 8
    j 1b

2:
    la sp, __boot_stack_top
    andi sp, sp, -16

    /* All early traps are fatal and share the initialized kernel stack. */
    la t0, riscv_trap_entry
    csrw stvec, t0

    mv a0, s0
    mv a1, s1
    call kernel_main

3:
    wfi
    j 3b

    .size _start, . - _start
