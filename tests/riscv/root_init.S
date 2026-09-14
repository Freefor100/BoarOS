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

    # PID 1 starts with the console on fd 0/1/2: write(1) emits the marker
    # through the serial console.  read(0) blocks until input arrives, which
    # the serial-fed real-userland run covers; no probe here.
    la a1, .Lstdio_marker
    la t0, .Lstdio_marker_end
    sub a2, t0, a1
    addi a2, a2, -1
    li a0, 1
    li a7, 64
    ecall
    la t0, .Lstdio_marker_end
    la t1, .Lstdio_marker
    sub t0, t0, t1
    addi t0, t0, -1
    bne a0, t0, .Lfailure

    # Reach below the eagerly built argument image while remaining inside
    # the fixed stack VMA.  The first access must observe a zero-filled page;
    # the second must survive a store-page-fault retry and retain its value.
    mv s10, sp
    li t0, 0x20000
    sub sp, sp, t0
    ld t1, 0(sp)
    bnez t1, .Lfailure
    li t0, 0x1000
    sub t0, sp, t0
    li t1, 0x5a6b7c8d
    sd t1, 0(t0)
    ld t2, 0(t0)
    bne t1, t2, .Lfailure
    mv sp, s10

    # The raw Linux brk syscall returns the exact program break rather than
    # the libc wrapper's 0/-1 result.  Growth reserves a demand-zero heap;
    # shrinking below the third page withdraws that page before the next exec.
    li a0, 0
    li a7, 214
    ecall
    la t0, __program_break
    bltu a0, t0, .Lfailure
    mv s11, a0
    li t0, 0x2031
    add a0, s11, t0
    li a7, 214
    ecall
    li t0, 0x2031
    add t1, s11, t0
    bne a0, t1, .Lfailure
    addi t0, s11, 0
    ld t1, 0(t0)
    bnez t1, .Lfailure
    li t1, 0x2000
    add t0, s11, t1
    li t1, 0x11223344
    sd t1, 0(t0)
    li t0, 0x321
    add a0, s11, t0
    li a7, 214
    ecall
    li t0, 0x321
    add t1, s11, t0
    bne a0, t1, .Lfailure

    # Exercise the Linux anonymous-private mmap lifecycle in real U-mode.
    # The kernel chooses a top-down range, preserves a resident page through
    # PROT_NONE, rejects NOREPLACE overlap, and lets a hint refill a hole.
    li a0, 0
    li a1, 0x3000
    li a2, 3
    li a3, 0x22
    li a4, -1
    li a5, 0
    li a7, 222
    ecall
    bltz a0, .Lfailure
    mv s9, a0
    ld t0, 0(s9)
    bnez t0, .Lfailure
    li t0, 0x5a6b7c8d
    li t1, 0x1000
    add t3, s9, t1
    sd t0, 0(t3)

    addi a0, s9, 0
    li t1, 0x1000
    add a0, a0, t1
    li a1, 0x1000
    li a2, 0
    li a7, 226
    ecall
    bnez a0, .Lfailure
    addi a0, s9, 0
    add a0, a0, t1
    li a1, 0x1000
    li a2, 2
    li a7, 226
    ecall
    bnez a0, .Lfailure
    li t1, 0x1000
    add t3, s9, t1
    ld t2, 0(t3)
    li t0, 0x5a6b7c8d
    bne t2, t0, .Lfailure

    mv a0, s9
    li a1, 0x1000
    li a2, 3
    li a3, 0x100022
    li a4, -1
    li a5, 0
    li a7, 222
    ecall
    li t0, -17
    bne a0, t0, .Lfailure

    li t0, 0x1000
    add a0, s9, t0
    li a1, 0x1000
    li a2, 3
    li a3, 0x32
    li a4, -1
    li a5, 0
    li a7, 222
    ecall
    li t0, 0x1000
    add t1, s9, t0
    bne a0, t1, .Lfailure
    ld t2, 0(a0)
    bnez t2, .Lfailure

    mv a0, t1
    li a1, 0x1000
    li a7, 215
    ecall
    bnez a0, .Lfailure
    mv a0, s9
    li a1, 0x3000
    li a2, 1
    li a7, 226
    ecall
    li t0, -12
    bne a0, t0, .Lfailure

    li t0, 0x1000
    add a0, s9, t0
    li a1, 0x1000
    li a2, 3
    li a3, 0x22
    li a4, -1
    li a5, 0
    li a7, 222
    ecall
    li t0, 0x1000
    add t1, s9, t0
    bne a0, t1, .Lfailure
    ld t2, 0(a0)
    bnez t2, .Lfailure
    mv a0, s9
    li a1, 0x3000
    li a7, 215
    ecall
    bnez a0, .Lfailure

    # An absolute path ignores dirfd and receives the lowest free fd; the
    # console descriptors occupy 0/1/2.
    li a0, 7
    la a1, .Labsolute_path
    li a2, 0
    li a3, 0
    li a7, 56
    ecall
    li t0, 3
    bne a0, t0, .Lfailure
    mv s0, a0

    # A relative path resolves against cwd through AT_FDCWD.  Both
    # O_LARGEFILE and O_CLOEXEC are accepted for a read-only open.
    li a0, -100
    la a1, .Lrelative_path
    li a2, 0x88000
    li a3, 0
    li a7, 56
    ecall
    li t0, 4
    bne a0, t0, .Lfailure
    mv s1, a0

    # Two MAP_PRIVATE views initially observe the same file bytes.  A write
    # detaches only the modified view, the tail of the last file page is
    # zero-filled, and a whole page beyond EOF terminates a child with SIGBUS.
    li a0, 0
    li a1, 0x4000
    li a2, 3
    li a3, 2
    mv a4, s0
    li a5, 0
    li a7, 222
    ecall
    bltz a0, .Lfailure
    mv s8, a0
    li a0, 0
    li a1, 0x4000
    li a2, 3
    li a3, 2
    mv a4, s0
    li a5, 0
    li a7, 222
    ecall
    bltz a0, .Lfailure
    mv s9, a0

    lbu t0, 0(s8)
    li t1, 'A'
    bne t0, t1, .Lfailure
    lbu t0, 0(s9)
    bne t0, t1, .Lfailure
    li t0, 'Z'
    sb t0, 0(s8)
    lbu t1, 0(s8)
    bne t0, t1, .Lfailure
    lbu t1, 0(s9)
    li t0, 'A'
    bne t0, t1, .Lfailure
    li t0, 0x2327
    add t0, s8, t0
    lbu t1, 0(t0)
    li t2, 'D'
    bne t1, t2, .Lfailure
    lbu t1, 1(t0)
    bnez t1, .Lfailure

    li a0, 17
    li a1, 0
    li a2, 0
    li a3, 0
    li a4, 0
    li a7, 220
    ecall
    beqz a0, .Lmmap_bus_child
    bltz a0, .Lfailure
    mv s7, a0
    mv a0, s7
    la a1, .Lmmap_wait_status
    li a2, 0
    li a3, 0
    li a7, 260
    ecall
    bne a0, s7, .Lfailure
    la t0, .Lmmap_wait_status
    lw t1, 0(t0)
    # SIGBUS has a core-producing default action, so Linux wait status
    # carries WCOREDUMP in addition to the terminating signal number.
    li t2, 0x87
    bne t1, t2, .Lfailure

    mv a0, s8
    li a1, 0x4000
    li a7, 215
    ecall
    bnez a0, .Lfailure
    mv a0, s9
    li a1, 0x4000
    li a7, 215
    ecall
    bnez a0, .Lfailure

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

    # The read-only root gives every regular descriptor a read-only mode,
    # so writing to the /data descriptor reports EBADF.
    mv a0, s0
    la a1, .Lread_buffer
    li a2, 1
    li a7, 64
    ecall
    li t0, -9
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

    # fd 3 is at offset 5003 in a 9000-byte file.
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

    # lseek(SEEK_END) reports the exact size; the next read sees EOF.
    mv a0, s0
    li a1, 0
    li a2, 2
    li a7, 62
    ecall
    li t0, 9000
    bne a0, t0, .Lfailure
    mv a0, s0
    la a1, .Lread_buffer
    li a2, 1
    li a7, 63
    ecall
    bnez a0, .Lfailure

    # SEEK_SET repositions, and the descriptor's own offset drives reads.
    mv a0, s0
    li a1, 10
    li a2, 0
    li a7, 62
    ecall
    li t0, 10
    bne a0, t0, .Lfailure
    mv a0, s0
    la a1, .Lread_buffer
    li a2, 1
    li a7, 63
    ecall
    li t0, 1
    bne a0, t0, .Lfailure
    la t0, .Lread_buffer
    lbu t1, 0(t0)
    li t2, 'K'
    bne t1, t2, .Lfailure

    # The console is not seekable, negative targets are EINVAL, and the
    # failed calls leave the position untouched.
    li a0, 1
    li a1, 0
    li a2, 0
    li a7, 62
    ecall
    li t0, -29
    bne a0, t0, .Lfailure
    mv a0, s0
    li a1, -1
    li a2, 0
    li a7, 62
    ecall
    li t0, -22
    bne a0, t0, .Lfailure

    # fstat reports a regular mode and the exact file size for the ext4
    # descriptor, while newfstatat resolves the same file by name.
    mv a0, s0
    la a1, .Lstat_buffer
    li a7, 80
    ecall
    bnez a0, .Lfailure
    la t0, .Lstat_buffer
    lw t1, 16(t0)
    li t2, 0100000
    and t1, t1, t2
    bne t1, t2, .Lfailure
    la t0, .Lstat_buffer
    ld t1, 48(t0)
    li t2, 9000
    bne t1, t2, .Lfailure

    li a0, -100
    la a1, .Labsolute_path
    la a2, .Lstat_buffer
    li a3, 0
    li a7, 79
    ecall
    bnez a0, .Lfailure
    la t0, .Lstat_buffer
    ld t1, 48(t0)
    li t2, 9000
    bne t1, t2, .Lfailure

    # RISC-V dup3 duplicates the descriptor; both share one open-file
    # description, so reads advance the common offset.
    mv a0, s0
    li a1, 7
    li a2, 0
    li a7, 24
    ecall
    li t0, 7
    bne a0, t0, .Lfailure
    li a0, 7
    la a1, .Lread_buffer
    li a2, 1
    li a7, 63
    ecall
    li t0, 1
    bne a0, t0, .Lfailure
    la t0, .Lread_buffer
    lbu t1, 0(t0)
    li t2, 'L'
    bne t1, t2, .Lfailure

    # fcntl separates per-fd flags from the per-file status flags, and
    # F_SETFL accepts only the modifiable set.
    li a0, 7
    li a1, 3
    li a2, 0
    li a7, 25
    ecall
    bnez a0, .Lfailure
    li a0, 7
    li a1, 4
    li a2, 04000
    li a7, 25
    ecall
    bnez a0, .Lfailure
    li a0, 7
    li a1, 3
    li a7, 25
    ecall
    li t0, 04000
    bne a0, t0, .Lfailure
    li a0, 7
    li a1, 4
    li a2, 040000
    li a7, 25
    ecall
    li t0, -22
    bne a0, t0, .Lfailure
    li a0, 7
    li a1, 2
    li a2, 1
    li a7, 25
    ecall
    bnez a0, .Lfailure
    li a0, 7
    li a1, 1
    li a7, 25
    ecall
    li t0, 1
    bne a0, t0, .Lfailure

    # F_DUPFD_CLOEXEC honors its lower bound, and closing the duplicate
    # leaves the original descriptor working.
    li a0, 7
    li a1, 1030
    li a2, 9
    li a7, 25
    ecall
    li t0, 9
    bne a0, t0, .Lfailure
    li a0, 9
    li a1, 1
    li a7, 25
    ecall
    li t0, 1
    bne a0, t0, .Lfailure
    li a0, 7
    li a7, 57
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
    la a1, .Lmissing_path
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

    # Keep a normal descriptor at offset four across both execs.  fd 4 was
    # opened with O_CLOEXEC above and remains open until a successful exec.
    li a0, -100
    la a1, .Labsolute_path
    li a2, 0
    li a3, 0
    li a7, 56
    ecall
    li t0, 3
    bne a0, t0, .Lfailure
    mv s0, a0
    mv a0, s0
    la a1, .Lread_buffer
    li a2, 4
    li a7, 63
    ecall
    li t0, 4
    bne a0, t0, .Lfailure

    # Linux opens the executable before touching argv.  The invalid argv
    # must therefore not mask ENOENT, and failed exec must not close fd 1.
    la a0, .Lmissing_path
    li a1, 0x1000
    li a2, 0
    li a7, 221
    ecall
    li t0, -2
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
    li t2, 'D'
    bne t1, t2, .Lfailure

    la a0, .Lempty_path
    li a1, 0
    li a2, 0
    li a7, 221
    ecall
    li t0, -2
    bne a0, t0, .Lfailure
    li a0, 0x1000
    li a1, 0
    li a2, 0
    li a7, 221
    ecall
    li t0, -14
    bne a0, t0, .Lfailure
    la a0, .Lroot_path
    li a1, 0
    li a2, 0
    li a7, 221
    ecall
    li t0, -13
    bne a0, t0, .Lfailure
    la a0, .Lscript_path
    li a1, 0
    li a2, 0
    li a7, 221
    ecall
    li t0, -8
    bne a0, t0, .Lfailure
    la a0, .Lstage2_path
    la a1, .Lfault_argv
    li a2, 0
    li a7, 221
    ecall
    li t0, -14
    bne a0, t0, .Lfailure
    la a0, .Lstage2_path
    la a1, .Lstage2_argv
    li a2, 0x1000
    li a7, 221
    ecall
    li t0, -14
    bne a0, t0, .Lfailure

    la t0, .Lbig_string
    li t1, 0x20000
    li t2, 'x'
.Lfill_big_string:
    sb t2, 0(t0)
    addi t0, t0, 1
    addi t1, t1, -1
    bnez t1, .Lfill_big_string
    la a0, .Lstage2_path
    la a1, .Lbig_argv
    li a2, 0
    li a7, 221
    ecall
    li t0, -7
    bne a0, t0, .Lfailure

    # A regular file without execute bits is rejected before replacement.
    la a0, .Labsolute_path
    la a1, .Lstage2_argv
    la a2, .Lstage2_envp
    li a7, 221
    ecall
    li t0, -13
    bne a0, t0, .Lfailure

    # Set non-argument registers to sentinels.  stage2 checks that exec did
    # not leak them into the fresh image.
    li s0, 0x10
    li s1, 0x11
    li s2, 0x12
    li s3, 0x13
    li s4, 0x14
    li s5, 0x15
    li s6, 0x16
    li s7, 0x17
    li s8, 0x18
    li s9, 0x19
    li s10, 0x1a
    li s11, 0x1b
    li tp, 0x1c
    la a0, .Lstage2_path
    la a1, .Lstage2_argv
    la a2, .Lstage2_envp
    li a7, 221
    ecall
    j .Lfailure

.Lmmap_bus_child:
    li t0, 0x3000
    add t0, s8, t0
    lbu t1, 0(t0)
    j .Lfailure

.Lfailure:
    li a0, 0xe1
    li a7, 93
    ecall
    j .
    .size _start, . - _start

    .section .rodata, "a", @progbits
.Lstdio_marker:
    .asciz "BoarOS: root-init stdio write ok\n"
.Lstdio_marker_end:
.Labsolute_path:
    .asciz "/data"
.Lrelative_path:
    .asciz "data"
.Lmissing_path:
    .asciz "/missing"
.Lempty_path:
    .asciz ""
.Lroot_path:
    .asciz "/"
.Lscript_path:
    .asciz "/script"
.Lstage2_path:
    .asciz "stage2"
.Lstage2_arg0:
    .asciz "stage2"
.Lstage2_arg1:
    .asciz "hello"
.Lstage2_env0:
    .asciz "MODE=exec"
    .balign 8
.Lstage2_argv:
    .dword .Lstage2_arg0
    .dword .Lstage2_arg1
    .dword 0
.Lstage2_envp:
    .dword .Lstage2_env0
    .dword 0
.Lfault_argv:
    .dword 0x1000
    .dword 0
.Lbig_argv:
    .dword .Lbig_string
    .dword 0

    .section .bss, "aw", @nobits
    .balign 4096
    .skip 4000
.Lread_buffer:
    .skip 5000
    .balign 8
.Lstat_buffer:
    .skip 128
    .balign 4
.Lmmap_wait_status:
    .skip 4
    .balign 4096
.Lbig_string:
    .skip 0x20000
