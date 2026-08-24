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
    li t0, 0x3fff7ff000
    sd zero, 0(t0)
    li a0, 0xee
    li a7, 93
    ecall
#else
    andi t0, sp, 15
    bnez t0, .Lfailure

    ld t0, 0(sp)
    li t1, 2
    bne t0, t1, .Lfailure
    ld t0, 8(sp)
    beqz t0, .Lfailure
    li t1, 'e'
    lbu t2, 0(t0)
    bne t1, t2, .Lfailure
    li t1, 'e'
    lbu t2, 8(t0)
    bne t1, t2, .Lfailure
    lbu t2, 9(t0)
    bnez t2, .Lfailure
    ld t0, 16(sp)
    beqz t0, .Lfailure
    li t1, 'h'
    lbu t2, 0(t0)
    bne t1, t2, .Lfailure
    lbu t2, 5(t0)
    bnez t2, .Lfailure
    ld t0, 24(sp)
    bnez t0, .Lfailure
    ld t0, 32(sp)
    beqz t0, .Lfailure
    li t1, 'M'
    lbu t2, 0(t0)
    bne t1, t2, .Lfailure
    li t1, 't'
    lbu t2, 8(t0)
    bne t1, t2, .Lfailure
    lbu t2, 9(t0)
    bnez t2, .Lfailure
    ld t0, 40(sp)
    bnez t0, .Lfailure

    addi t0, sp, 48
    li t5, 16
    li t6, 0
.Laux_loop:
    beqz t5, .Lfailure
    ld t1, 0(t0)
    ld t2, 8(t0)
    beqz t1, .Laux_done
    li t3, 6
    beq t1, t3, .Laux_pagesz
    li t3, 4
    beq t1, t3, .Laux_phent
    li t3, 5
    beq t1, t3, .Laux_phnum
    li t3, 9
    beq t1, t3, .Laux_entry
.Laux_next:
    addi t0, t0, 16
    addi t5, t5, -1
    j .Laux_loop
.Laux_pagesz:
    li t3, 4096
    bne t2, t3, .Lfailure
    ori t6, t6, 1
    j .Laux_next
.Laux_phent:
    li t3, 56
    bne t2, t3, .Lfailure
    ori t6, t6, 2
    j .Laux_next
.Laux_phnum:
    beqz t2, .Lfailure
    ori t6, t6, 4
    j .Laux_next
.Laux_entry:
    la t3, _start
    bne t2, t3, .Lfailure
    ori t6, t6, 8
    j .Laux_next
.Laux_done:
    bnez t2, .Lfailure
    li t0, 15
    bne t6, t0, .Lfailure

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
