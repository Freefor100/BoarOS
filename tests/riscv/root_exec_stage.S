    .section .text.start, "ax", @progbits
#ifndef ROOT_FAULT_WAIT_STATUS
#define ROOT_FAULT_WAIT_STATUS 0x8b
#endif

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
    beq t0, t1, .Largc_one
    li t1, 2
    bne t0, t1, .Lfailure
    # The vfork-exec child image: argv[1] must be exactly "V"; it proves
    # the exec happened and exits 0x29 without running the full suite.
    ld t0, 16(sp)
    beqz t0, .Lfailure
    lbu t1, 0(t0)
    li t2, 'V'
    bne t1, t2, .Lfailure
    lbu t1, 1(t0)
    bnez t1, .Lfailure
    li a0, 0x29
    li a7, 93
    ecall
.Largc_one:
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

    # Every successful exec creates a fresh program break for the new ELF.
    li a0, 0
    li a7, 214
    ecall
    la t0, __program_break
    bltu a0, t0, .Lfailure
#if ROOT_EXEC_STAGE == 2
    # Leave a non-default exact break behind; stage3 must not inherit it.
    mv t1, a0
    li t0, 0x123
    add a0, t1, t0
    li a7, 214
    ecall
    li t0, 0x123
    add t1, t1, t0
    bne a0, t1, .Lfailure

    # fd 4 was CLOEXEC and must be unreachable; fd 3 keeps offset four.
    # The console descriptors survive exec unchanged on fd 0/1/2.
    la a1, .Lstage2_marker
    la t0, .Lstage2_marker_end
    sub a2, t0, a1
    addi a2, a2, -1
    li a0, 1
    li a7, 64
    ecall
    la t0, .Lstage2_marker_end
    la t1, .Lstage2_marker
    sub t0, t0, t1
    addi t0, t0, -1
    bne a0, t0, .Lfailure
    li a0, 4
    la a1, .Lbuffer
    li a2, 1
    li a7, 63
    ecall
    li t0, -9
    bne a0, t0, .Lfailure
    li a0, 3
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
    # Reserve three heap pages, commit the last, then shrink it away.  The
    # first clone checks independent exact-break state; a later child proves
    # that the released page faults before the parent regrows it as zero.
    li a0, 0
    li a7, 214
    ecall
    mv t0, a0
    la t1, .Lheap_base
    sd t0, 0(t1)
    li t2, 0x2000
    add t3, t0, t2
    la t1, .Lheap_fault_address
    sd t3, 0(t1)
    li t2, 0x2031
    add t4, t0, t2
    mv a0, t4
    li a7, 214
    ecall
    bne a0, t4, .Lfailure
    li t2, 0x55667788
    sd t2, 0(t3)
    li t2, 0x321
    add a0, t0, t2
    li a7, 214
    ecall
    li t2, 0x321
    add t3, t0, t2
    bne a0, t3, .Lfailure

    li a0, 3
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

    # WNOHANG may observe either a live or already exited child; a finite
    # user loop is not synchronization with the parent's scheduling.
    mv a0, s3
    la a1, .Lwait_status
    li a2, 1
    la a3, .Lrusage
    li a7, 260
    ecall
    beq a0, s3, .Lfirst_child_reaped
    bnez a0, .Lfailure
    la t0, .Lrusage
    ld t1, 0(t0)
    bnez t1, .Lfailure

    # A zero result leaves rusage untouched; the blocking wait then reaps.
    mv a0, s3
    la a1, .Lwait_status
    li a2, 0
    la a3, .Lrusage
    li a7, 260
    ecall
    bne a0, s3, .Lfailure
.Lfirst_child_reaped:
    la t0, .Lwait_status
    lw t1, 0(t0)
    li t2, 0x2500
    bne t1, t2, .Lfailure

    li a0, 0
    li a7, 214
    ecall
    la t0, .Lheap_base
    ld t0, 0(t0)
    li t1, 0x321
    add t0, t0, t1
    bne a0, t0, .Lfailure

    # The child wrote its private MM copy, while the inherited fd shared
    # one open-file description and therefore advanced the common offset.
    la t0, .Lfork_word
    ld t1, 0(t0)
    li t2, 0x33
    bne t1, t2, .Lfailure
    li a0, 3
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

    # Tick-resolution CPU time may be zero if execution did not intersect
    # a sampling tick. Exact charging is checked by scheduler cases.
    la t0, .Lrusage
    ld t1, 0(t0)
    ld t2, 8(t0)
    or t1, t1, t2
    bltz t2, .Lfailure
    # usec stays below one second for sub-minute children.
    li t2, 1000000
    bgeu t1, t2, .Lfailure

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

    # A user page fault is reported as a SIGSEGV-style wait status with the
    # core-producing default-action bit.
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
    li t2, ROOT_FAULT_WAIT_STATUS
    bne t1, t2, .Lfailure

    la t0, .Lheap_base
    ld t0, 0(t0)
    li t1, 0x2031
    add t2, t0, t1
    mv a0, t2
    li a7, 214
    ecall
    bne a0, t2, .Lfailure
    la t0, .Lheap_fault_address
    ld t0, 0(t0)
    ld t1, 0(t0)
    bnez t1, .Lfailure

    # Linux reaps the zombie before copying status to userspace.  Therefore
    # an invalid status pointer returns EFAULT, but the same child cannot be
    # waited for a second time.  A bad rusage pointer faults the same way.
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
    li a3, 0x1000
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

    # clone(17, custom_stack): the child starts on the supplied stack,
    # verifies its own sp lies inside the buffer, writes through it, and
    # exits with 0x27; the parent reaps and checks the status.
    li a0, 17
    la a1, .Lchild_stack
    li t0, 4096
    add a1, a1, t0
    li a2, 0
    li a3, 0
    li a4, 0
    li a7, 220
    ecall
    beqz a0, .Lcustom_stack_child
    bltz a0, .Lfailure
    la t0, .Lcustom_pid
    sd a0, 0(t0)
    ld a0, 0(t0)
    la a1, .Lwait_status
    li a2, 0
    li a3, 0
    li a7, 260
    ecall
    la t0, .Lcustom_pid
    ld t1, 0(t0)
    bne a0, t1, .Lfailure
    la t0, .Lwait_status
    lw t1, 0(t0)
    li t2, 0x2700
    bne t1, t2, .Lfailure

    # vfork: the child shares this address space, grows the shared break,
    # then execs a second-level stage3 with argv "V".  The parent resumes
    # from the clone when the exec commit retires the shared mm, reaps
    # status 0x2900, and observes the child's break change.
    li a0, 0x4111
    la a1, .Lchild_stack
    li t0, 4096
    add a1, a1, t0
    li a2, 0
    li a3, 0
    li a4, 0
    li a7, 220
    ecall
    beqz a0, .Lvfork_child
    bltz a0, .Lfailure
    la t0, .Lvfork_pid
    sd a0, 0(t0)
    ld a0, 0(t0)
    la a1, .Lwait_status
    li a2, 0
    li a3, 0
    li a7, 260
    ecall
    la t0, .Lvfork_pid
    ld t1, 0(t0)
    bne a0, t1, .Lfailure
    la t0, .Lwait_status
    lw t1, 0(t0)
    li t2, 0x2900
    bne t1, t2, .Lfailure
    li a0, 0
    li a7, 214
    ecall
    la t1, .Lheap_base
    ld t1, 0(t1)
    li t2, 0x3031
    add t1, t1, t2
    bne a0, t1, .Lfailure

    # times(153): child roll-up fields are non-negative and charged time
    # cannot exceed elapsed single-hart ticks; equality is valid without
    # sampled idle time. A finite spin need not intersect a sampling tick.
    li t4, 20000000
.Ltimes_spin:
    addi t4, t4, -1
    bnez t4, .Ltimes_spin
    la a0, .Ltms
    li a7, 153
    ecall
    bltz a0, .Lfailure
    la t0, .Ltms
    ld t1, 0(t0)
    ld t2, 8(t0)
    ld t3, 16(t0)
    ld t5, 24(t0)
    add t1, t1, t2
    bltz t3, .Lfailure
    bltz t5, .Lfailure
    add t2, t1, t3
    add t2, t2, t5
    bltu a0, t2, .Lfailure

    li a0, 0x2a
    li a7, 93
    ecall

.Lcustom_stack_child:
    # The child runs on the caller-supplied stack: verify sp lies inside
    # the buffer, then store through it below sp before exiting 0x27.
    la t0, .Lchild_stack
    li t1, 4096
    add t2, t0, t1
    bltu sp, t0, .Lcustom_stack_bad
    bgtu sp, t2, .Lcustom_stack_bad
    addi t3, sp, -32
    li t4, 0x5a5a5a5a5a5a5a5a
    sd t4, 0(t3)
    ld t5, 0(t3)
    bne t4, t5, .Lcustom_stack_bad
    li a0, 0x27
    li a7, 93
    ecall
.Lcustom_stack_bad:
    li a0, 0x28
    li a7, 93
    ecall

.Lvfork_child:
    # Running on the shared address space: grow the shared break so the
    # parent can observe the sharing after it resumes, then exec a
    # second-level stage3 with argv "V".
    la t0, .Lheap_base
    ld t0, 0(t0)
    li t1, 0x3031
    add a0, t0, t1
    li a7, 214
    ecall
    bltz a0, .Lcustom_stack_bad
    la a0, .Lstage3_path
    la a1, .Lvfork_argv
    li a2, 0
    li a7, 221
    ecall
    # exec must not return
    li a0, 0x2b
    li a7, 93
    ecall

.Lclone_child:
    li a0, 0
    li a7, 214
    ecall
    la t0, .Lheap_base
    ld t0, 0(t0)
    li t1, 0x321
    add t2, t0, t1
    bne a0, t2, .Lfailure
    li t1, 0x1231
    add t2, t0, t1
    mv a0, t2
    li a7, 214
    ecall
    bne a0, t2, .Lfailure
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
    li a0, 3
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
#ifdef ROOT_FAULT_INJECT_OOM
    li t0, 0x1000
#else
    la t0, .Lheap_fault_address
    ld t0, 0(t0)
#endif
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
.Lstage2_marker:
    .asciz "BoarOS: stage2 stdio write ok\n"
.Lstage2_marker_end:
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
.Lheap_base:
    .skip 8
.Lheap_fault_address:
    .skip 8
.Lrusage:
    .skip 144
.Ltms:
    .skip 32
.Lcustom_pid:
    .skip 8
.Lvfork_pid:
    .skip 8
.Lchild_stack:
    .skip 4096

    .section .data, "aw", @progbits
    .balign 8
.Lfork_word:
    .dword 0x33
.Lvfork_argv:
    .dword .Lstage3_path
    .dword .Lvfork_arg
    .dword 0
.Lvfork_arg:
    .asciz "V"
