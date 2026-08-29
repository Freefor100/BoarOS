    .section .text.start, "ax", @progbits
    .align 2
    .globl _start
    .type _start, @function
_start:
    bnez tp, .Lfailure
    bnez s0, .Lfailure
    bnez s1, .Lfailure
    bnez s2, .Lfailure
    bnez s3, .Lfailure
    bnez s4, .Lfailure
    bnez s5, .Lfailure
    bnez s6, .Lfailure
    bnez s7, .Lfailure
    bnez s8, .Lfailure
    bnez s9, .Lfailure
    bnez s10, .Lfailure
    bnez s11, .Lfailure
    andi t0, sp, 15
    bnez t0, .Lfailure

#if ROOT_EXEC_STAGE == 2
    ld t0, 0(sp)
    li t1, 2
    bne t0, t1, .Lfailure
    ld t0, 8(sp)
    la t1, .Lstage2_name
    jal ra, .Lexpect_string
    ld t0, 16(sp)
    la t1, .Lhello
    jal ra, .Lexpect_string
    ld t0, 24(sp)
    bnez t0, .Lfailure
    ld t0, 32(sp)
    la t1, .Lenvironment
    jal ra, .Lexpect_string
    ld t0, 40(sp)
    bnez t0, .Lfailure
    addi s0, sp, 48
    la s1, .Lstage2_name
#else
    ld t0, 0(sp)
    li t1, 1
    bne t0, t1, .Lfailure
    ld t0, 8(sp)
    beqz t0, .Lfailure
    lbu t1, 0(t0)
    bnez t1, .Lfailure
    ld t0, 16(sp)
    bnez t0, .Lfailure
    ld t0, 24(sp)
    bnez t0, .Lfailure
    addi s0, sp, 32
    la s1, .Lstage3_path
#endif

    # Find AT_EXECFN and compare the pointed-to original caller path.
    li s2, 16
.Laux_loop:
    beqz s2, .Lfailure
    ld t0, 0(s0)
    ld t2, 8(s0)
    li t1, 31
    beq t0, t1, .Laux_execfn
    beqz t0, .Lfailure
    addi s0, s0, 16
    addi s2, s2, -1
    j .Laux_loop
.Laux_execfn:
    mv t0, t2
    mv t1, s1
    jal ra, .Lexpect_string

    li a7, 172
    ecall
    li t0, 1
    bne a0, t0, .Lfailure
    li a7, 178
    ecall
    li t0, 1
    bne a0, t0, .Lfailure

#if ROOT_EXEC_STAGE == 2
    # fd 1 was CLOEXEC and must be unreachable; fd 0 keeps offset four.
    li a0, 1
    la a1, .Lbuffer
    li a2, 1
    li a7, 63
    ecall
    li t0, -9
    bne a0, t0, .Lfailure
    li a0, 0
    la a1, .Lbuffer
    li a2, 1
    li a7, 63
    ecall
    li t0, 1
    bne a0, t0, .Lfailure
    la t0, .Lbuffer
    lbu t1, 0(t0)
    li t2, 'E'
    bne t1, t2, .Lfailure

    li s0, 0x20
    li s1, 0x21
    li s2, 0x22
    li s3, 0x23
    li s4, 0x24
    li s5, 0x25
    li s6, 0x26
    li s7, 0x27
    li s8, 0x28
    li s9, 0x29
    li s10, 0x2a
    li s11, 0x2b
    li tp, 0x2c
    la a0, .Lstage3_path
    li a1, 0
    li a2, 0
    li a7, 221
    ecall
    j .Lfailure
#else
    li a0, 0
    la a1, .Lbuffer
    li a2, 1
    li a7, 63
    ecall
    li t0, 1
    bne a0, t0, .Lfailure
    la t0, .Lbuffer
    lbu t1, 0(t0)
    li t2, 'F'
    bne t1, t2, .Lfailure

    # PID 1 has no userspace parent, and wait4 reports ECHILD before it
    # owns any children.
    li a7, 173
    ecall
    bnez a0, .Lfailure
    li a0, -1
    la a1, .Lwait_status
    li a2, 1
    li a3, 0
    li a7, 260
    ecall
    li t0, -10
    bne a0, t0, .Lfailure

    # wait4 rejects option bits outside the Linux wait mask before child
    # selection, and the pid_t value whose negation is unrepresentable is
    # reported as ESRCH.
    li a0, -1
    li a1, 0
    li a2, 4
    li a3, 0
    li a7, 260
    ecall
    li t0, -22
    bne a0, t0, .Lfailure
    li a0, 0x80000000
    li a1, 0
    li a2, 0
    li a3, 0
    li a7, 260
    ecall
    li t0, -3
    bne a0, t0, .Lfailure

    # This stage implements ordinary process clone only.  A sharing flag
    # must fail explicitly rather than silently changing its semantics.
    li a0, 0x111
    li a1, 0
    li a2, 0
    li a3, 0
    li a4, 0
    li a7, 220
    ecall
    li t0, -95
    bne a0, t0, .Lfailure

    li a0, 17
    li a1, 0
    li a2, 0
    li a3, 0
    li a4, 0
    li a7, 220
    ecall
    beqz a0, .Lclone_child
    bltz a0, .Lfailure
    mv s3, a0

    # Non-null rusage is outside the current accounting contract.
    mv a0, s3
    li a1, 0
    li a2, 1
    la a3, .Lwait_status
    li a7, 260
    ecall
    li t0, -95
    bne a0, t0, .Lfailure

    # Normal children are excluded by __WCLONE, and no child belongs to
    # process group 2.
    li a0, -1
    li a1, 0
    li a2, 0x80000001
    li a3, 0
    li a7, 260
    ecall
    li t0, -10
    bne a0, t0, .Lfailure
    li a0, -2
    li a1, 0
    li a2, 1
    li a3, 0
    li a7, 260
    ecall
    li t0, -10
    bne a0, t0, .Lfailure

    # The child has not produced an exit event yet, so WNOHANG returns 0.
    mv a0, s3
    li a1, 0
    li a2, 1
    li a3, 0
    li a7, 260
    ecall
    bnez a0, .Lfailure

    mv a0, s3
    la a1, .Lwait_status
    li a2, 0
    li a3, 0
    li a7, 260
    ecall
    bne a0, s3, .Lfailure
    la t0, .Lwait_status
    lw t1, 0(t0)
    li t2, 0x2500
    bne t1, t2, .Lfailure

    # The child wrote its private MM copy, while the inherited fd shared
    # one open-file description and therefore advanced the common offset.
    la t0, .Lfork_word
    ld t1, 0(t0)
    li t2, 0x33
    bne t1, t2, .Lfailure
    li a0, 0
    la a1, .Lbuffer
    li a2, 1
    li a7, 63
    ecall
    li t0, 1
    bne a0, t0, .Lfailure
    la t0, .Lbuffer
    lbu t1, 0(t0)
    li t2, 'H'
    bne t1, t2, .Lfailure

    # A child that clones once more and exits first leaves its live
    # grandchild reparented to PID 1.
    li a0, 17
    li a1, 0
    li a2, 0
    li a3, 0
    li a4, 0
    li a7, 220
    ecall
    beqz a0, .Lreparent_child
    bltz a0, .Lfailure
    mv s4, a0
    mv a0, s4
    la a1, .Lwait_status
    li a2, 0
    li a3, 0
    li a7, 260
    ecall
    bne a0, s4, .Lfailure
    la t0, .Lwait_status
    lw t1, 0(t0)
    li t2, 0x2600
    bne t1, t2, .Lfailure
    li a0, -1
    la a1, .Lwait_status
    li a2, 0
    li a3, 0
    li a7, 260
    ecall
    blez a0, .Lfailure
    la t0, .Lwait_status
    lw t1, 0(t0)
    li t2, 0x2700
    bne t1, t2, .Lfailure

    # pid == 0 selects children in the caller's process group.
    li a0, 17
    li a1, 0
    li a2, 0
    li a3, 0
    li a4, 0
    li a7, 220
    ecall
    beqz a0, .Lgroup_child
    bltz a0, .Lfailure
    li a0, 0
    la a1, .Lwait_status
    li a2, 0
    li a3, 0
    li a7, 260
    ecall
    blez a0, .Lfailure
    la t0, .Lwait_status
    lw t1, 0(t0)
    li t2, 0x2800
    bne t1, t2, .Lfailure

    # A user page fault is reported as a SIGSEGV-style wait status.
    li a0, 17
    li a1, 0
    li a2, 0
    li a3, 0
    li a4, 0
    li a7, 220
    ecall
    beqz a0, .Lfault_child
    bltz a0, .Lfailure
    mv s5, a0
    mv a0, s5
    la a1, .Lwait_status
    li a2, 0
    li a3, 0
    li a7, 260
    ecall
    bne a0, s5, .Lfailure
    la t0, .Lwait_status
    lw t1, 0(t0)
    li t2, 11
    bne t1, t2, .Lfailure

    # Linux reaps the zombie before copying status to userspace.  Therefore
    # an invalid status pointer returns EFAULT, but the same child cannot be
    # waited for a second time.
    li a0, 17
    li a1, 0
    li a2, 0
    li a3, 0
    li a4, 0
    li a7, 220
    ecall
    beqz a0, .Lefault_child
    bltz a0, .Lfailure
    mv s6, a0
    mv a0, s6
    li a1, 0x1000
    li a2, 0
    li a3, 0
    li a7, 260
    ecall
    li t0, -14
    bne a0, t0, .Lfailure
    mv a0, s6
    li a1, 0
    li a2, 1
    li a3, 0
    li a7, 260
    ecall
    li t0, -10
    bne a0, t0, .Lfailure

    # A reaped child is no longer waitable.
    li a0, -1
    li a1, 0
    li a2, 0
    li a3, 0
    li a7, 260
    ecall
    li t0, -10
    bne a0, t0, .Lfailure
    li a0, 0x2a
    li a7, 93
    ecall

.Lclone_child:
    li a7, 173
    ecall
    li t0, 1
    bne a0, t0, .Lfailure
    li t4, 50000000
.Lclone_delay:
    addi t4, t4, -1
    bnez t4, .Lclone_delay
    la t0, .Lfork_word
    li t1, 0x44
    sd t1, 0(t0)
    li a0, 0
    la a1, .Lbuffer
    li a2, 1
    li a7, 63
    ecall
    li t0, 1
    bne a0, t0, .Lfailure
    la t0, .Lbuffer
    lbu t1, 0(t0)
    li t2, 'G'
    bne t1, t2, .Lfailure
    li a0, 37
    li a7, 93
    ecall

.Lreparent_child:
    li a0, 17
    li a1, 0
    li a2, 0
    li a3, 0
    li a4, 0
    li a7, 220
    ecall
    beqz a0, .Lgrandchild
    bltz a0, .Lfailure
    li a0, 38
    li a7, 93
    ecall
.Lgrandchild:
    li t4, 5000000
.Lgrandchild_delay:
    addi t4, t4, -1
    bnez t4, .Lgrandchild_delay
    li a0, 39
    li a7, 93
    ecall

.Lgroup_child:
    li a0, 40
    li a7, 93
    ecall

.Lfault_child:
    li t0, 0x1000
    ld t1, 0(t0)
    j .Lfailure

.Lefault_child:
    li a0, 41
    li a7, 93
    ecall
#endif

.Lfailure:
    li a0, 0xe2
    li a7, 93
    ecall
    j .

# t0 is the actual address and t1 the expected NUL-terminated string.
.Lexpect_string:
    lbu t2, 0(t0)
    lbu t3, 0(t1)
    bne t2, t3, .Lfailure
    beqz t2, .Lstring_done
    addi t0, t0, 1
    addi t1, t1, 1
    j .Lexpect_string
.Lstring_done:
    ret
    .size _start, . - _start

    .section .rodata, "a", @progbits
.Lstage2_name:
    .asciz "stage2"
.Lhello:
    .asciz "hello"
.Lenvironment:
    .asciz "MODE=exec"
.Lstage3_path:
    .asciz "/stage3"

    .section .bss, "aw", @nobits
    .balign 16
.Lbuffer:
    .skip 16
.Lwait_status:
    .skip 8

    .section .data, "aw", @progbits
    .balign 8
.Lfork_word:
    .dword 0x33
