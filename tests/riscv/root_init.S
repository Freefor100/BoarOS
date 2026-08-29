    .section .text.start, "ax", @progbits
    .align 2
    .globl _start
    .type _start, @function
_start:
    andi t0, sp, 15
    bnez t0, .Lfailure

    ld t0, 0(sp)
    li t1, 1
    bne t0, t1, .Lfailure
    ld t0, 8(sp)
    beqz t0, .Lfailure
    li t1, '/'
    lbu t2, 0(t0)
    bne t1, t2, .Lfailure
    li t1, 'i'
    lbu t2, 1(t0)
    bne t1, t2, .Lfailure
    li t1, 'n'
    lbu t2, 2(t0)
    bne t1, t2, .Lfailure
    li t1, 'i'
    lbu t2, 3(t0)
    bne t1, t2, .Lfailure
    li t1, 't'
    lbu t2, 4(t0)
    bne t1, t2, .Lfailure
    lbu t2, 5(t0)
    bnez t2, .Lfailure
    ld t0, 16(sp)
    bnez t0, .Lfailure
    ld t0, 24(sp)
    bnez t0, .Lfailure

    # An absolute path ignores dirfd and receives the lowest free fd.
    li a0, 7
    la a1, .Labsolute_path
    li a2, 0
    li a3, 0
    li a7, 56
    ecall
    bnez a0, .Lfailure
    mv s0, a0

    # A relative path resolves against cwd through AT_FDCWD.  Both
    # O_LARGEFILE and O_CLOEXEC are accepted for a read-only open.
    li a0, -100
    la a1, .Lrelative_path
    li a2, 0x88000
    li a3, 0
    li a7, 56
    ecall
    li t0, 1
    bne a0, t0, .Lfailure
    mv s1, a0

    # The two descriptors have independent open-file descriptions.
    mv a0, s0
    la a1, .Lread_buffer
    li a2, 3
    li a7, 63
    ecall
    li t0, 3
    bne a0, t0, .Lfailure
    la t0, .Lread_buffer
    lbu t1, 0(t0)
    li t2, 'A'
    bne t1, t2, .Lfailure
    lbu t1, 1(t0)
    li t2, 'B'
    bne t1, t2, .Lfailure
    lbu t1, 2(t0)
    li t2, 'C'
    bne t1, t2, .Lfailure

    mv a0, s1
    la a1, .Lread_buffer
    li a2, 2
    li a7, 63
    ecall
    li t0, 2
    bne a0, t0, .Lfailure
    la t0, .Lread_buffer
    lbu t1, 0(t0)
    li t2, 'A'
    bne t1, t2, .Lfailure
    lbu t1, 1(t0)
    li t2, 'B'
    bne t1, t2, .Lfailure

    # The buffer begins 4000 bytes into a page, so this crosses three
    # mapped pages and requires more than one 4 KiB staging chunk.
    mv a0, s0
    la a1, .Lread_buffer
    li a2, 5000
    li a7, 63
    ecall
    li t0, 5000
    bne a0, t0, .Lfailure
    la t0, .Lread_buffer
    li t1, 0
.Lverify_large_read:
    lbu t2, 0(t0)
    addi t3, t1, 3
    li t4, 26
    remu t3, t3, t4
    addi t3, t3, 'A'
    bne t2, t3, .Lfailure
    addi t0, t0, 1
    addi t1, t1, 1
    li t2, 5000
    bltu t1, t2, .Lverify_large_read

    # A failed user copy reports EFAULT and does not commit the offset.
    mv a0, s1
    li a1, 0x1000
    li a2, 1
    li a7, 63
    ecall
    li t0, -14
    bne a0, t0, .Lfailure
    mv a0, s1
    la a1, .Lread_buffer
    li a2, 1
    li a7, 63
    ecall
    li t0, 1
    bne a0, t0, .Lfailure
    la t0, .Lread_buffer
    lbu t1, 0(t0)
    li t2, 'C'
    bne t1, t2, .Lfailure

    # fd 0 is at offset 5003 in a 9000-byte file.
    mv a0, s0
    la a1, .Lread_buffer
    li a2, 5000
    li a7, 63
    ecall
    li t0, 3997
    bne a0, t0, .Lfailure
    mv a0, s0
    la a1, .Lread_buffer
    li a2, 1
    li a7, 63
    ecall
    bnez a0, .Lfailure

    mv a0, s0
    li a7, 57
    ecall
    bnez a0, .Lfailure
    mv a0, s0
    li a7, 57
    ecall
    li t0, -9
    bne a0, t0, .Lfailure

    # Mutating flags are rejected by the read-only root, and a relative
    # path with a non-AT_FDCWD dirfd is not silently treated as root.
    li a0, -100
    la a1, .Labsolute_path
    li a2, 0100
    li a3, 0
    li a7, 56
    ecall
    li t0, -30
    bne a0, t0, .Lfailure
    li a0, 7
    la a1, .Lrelative_path
    li a2, 0
    li a3, 0
    li a7, 56
    ecall
    li t0, -9
    bne a0, t0, .Lfailure

    li a0, 0x2a
    li a7, 93
    ecall

.Lfailure:
    li a0, 0xe1
    li a7, 93
    ecall
    j .
    .size _start, . - _start

    .section .rodata, "a", @progbits
.Labsolute_path:
    .asciz "/data"
.Lrelative_path:
    .asciz "data"

    .section .bss, "aw", @nobits
    .balign 4096
    .skip 4000
.Lread_buffer:
    .skip 5000
