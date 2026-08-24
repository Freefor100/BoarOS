    .section .text.start, "ax", @progbits
    .align 2
    .globl _start
    .type _start, @function
_start:
#if defined(USER_ELF_TEXT_FAULT)
    la t0, _start
    sd zero, 0(t0)
    li a0, 0xef
    li a7, 93
    ecall
#elif defined(USER_ELF_GUARD_FAULT)
    li t0, 0x1008
    sub t0, sp, t0
    sd zero, 0(t0)
    li a0, 0xee
    li a7, 93
    ecall
#else
    andi t0, sp, 15
    bnez t0, .Lfailure

    addi sp, sp, -16
    li t0, 0x12345678
    sd t0, 0(sp)
    ld t1, 0(sp)
    bne t0, t1, .Lfailure
    addi sp, sp, 16

    la t0, user_elf_initialized
    ld t1, 0(t0)
    li t2, 0x1122334455667788
    bne t1, t2, .Lfailure

    la t0, user_elf_zeroed
    ld t1, 0(t0)
    bnez t1, .Lfailure
    li t1, 0x55aa
    sd t1, 0(t0)
    ld t2, 0(t0)
    bne t1, t2, .Lfailure

    li a0, 0x5a
    li a7, 93
    ecall

.Lfailure:
    li a0, 0xe0
    li a7, 93
    ecall
#endif
    j .
    .size _start, . - _start

    .section .data, "aw", @progbits
    .align 3
user_elf_initialized:
    .dword 0x1122334455667788

    .section .bss, "aw", @nobits
    .align 3
user_elf_zeroed:
    .skip 8
