#include <arch/riscv/fpu.h>
#include <arch/riscv/trap.h>

    .section .text, "ax", @progbits
    .align 2

    /* The kernel march excludes F/D; these two functions are the only
     * code allowed to touch the FP unit. */
    .option arch, +f, +d

/* void riscv_fpu_state_save(struct riscv_fpu_state *state)
 *
 * Saves the live registers into `state` and marks it authoritative;
 * FS ends Clean so later user modification is tracked again. */
    .globl riscv_fpu_state_save
    .type riscv_fpu_state_save, @function
riscv_fpu_state_save:
    fsd f0, 0(a0)
    fsd f1, 8(a0)
    fsd f2, 16(a0)
    fsd f3, 24(a0)
    fsd f4, 32(a0)
    fsd f5, 40(a0)
    fsd f6, 48(a0)
    fsd f7, 56(a0)
    fsd f8, 64(a0)
    fsd f9, 72(a0)
    fsd f10, 80(a0)
    fsd f11, 88(a0)
    fsd f12, 96(a0)
    fsd f13, 104(a0)
    fsd f14, 112(a0)
    fsd f15, 120(a0)
    fsd f16, 128(a0)
    fsd f17, 136(a0)
    fsd f18, 144(a0)
    fsd f19, 152(a0)
    fsd f20, 160(a0)
    fsd f21, 168(a0)
    fsd f22, 176(a0)
    fsd f23, 184(a0)
    fsd f24, 192(a0)
    fsd f25, 200(a0)
    fsd f26, 208(a0)
    fsd f27, 216(a0)
    fsd f28, 224(a0)
    fsd f29, 232(a0)
    fsd f30, 240(a0)
    fsd f31, 248(a0)
    csrr t0, fcsr
    sd t0, RISCV_FPU_STATE_FCSR_OFFSET(a0)
    li t0, 1
    sd t0, RISCV_FPU_STATE_SAVED_OFFSET(a0)
    li t0, RISCV_SSTATUS_FS_MASK
    csrc sstatus, t0
    li t0, RISCV_SSTATUS_FS_CLEAN
    csrs sstatus, t0
    ret

    .size riscv_fpu_state_save, . - riscv_fpu_state_save

/* void riscv_fpu_state_restore(struct riscv_fpu_state *state)
 *
 * Reloads the registers from `state`; the memory image stays
 * authoritative for the next dispatch.  FS ends Clean. */
    .globl riscv_fpu_state_restore
    .type riscv_fpu_state_restore, @function
riscv_fpu_state_restore:
    fld f0, 0(a0)
    fld f1, 8(a0)
    fld f2, 16(a0)
    fld f3, 24(a0)
    fld f4, 32(a0)
    fld f5, 40(a0)
    fld f6, 48(a0)
    fld f7, 56(a0)
    fld f8, 64(a0)
    fld f9, 72(a0)
    fld f10, 80(a0)
    fld f11, 88(a0)
    fld f12, 96(a0)
    fld f13, 104(a0)
    fld f14, 112(a0)
    fld f15, 120(a0)
    fld f16, 128(a0)
    fld f17, 136(a0)
    fld f18, 144(a0)
    fld f19, 152(a0)
    fld f20, 160(a0)
    fld f21, 168(a0)
    fld f22, 176(a0)
    fld f23, 184(a0)
    fld f24, 192(a0)
    fld f25, 200(a0)
    fld f26, 208(a0)
    fld f27, 216(a0)
    fld f28, 224(a0)
    fld f29, 232(a0)
    fld f30, 240(a0)
    fld f31, 248(a0)
    ld t0, RISCV_FPU_STATE_FCSR_OFFSET(a0)
    csrw fcsr, t0
    li t0, RISCV_SSTATUS_FS_MASK
    csrc sstatus, t0
    li t0, RISCV_SSTATUS_FS_CLEAN
    csrs sstatus, t0
    ret

    .size riscv_fpu_state_restore, . - riscv_fpu_state_restore

/* void riscv_fpu_switch(struct riscv_fpu_state *previous,
 *                       struct riscv_fpu_state *next)
 *
 * Runs with interrupts off at every context switch, before the integer
 * context switch.  When the live state is Dirty it is saved into
 * `previous` and marked Clean; when `next` carries a saved image it is
 * reloaded from memory so the Dirty flag again tracks only user
 * modification (privileged spec table 14: restore completes as Clean).
 * Kernel threads never save (FS is never Dirty while only kernel code
 * ran) and never restore (saved stays zero).  The task exit path
 * passes a NULL `previous`: only the next image is reloaded. */
    .globl riscv_fpu_switch
    .type riscv_fpu_switch, @function
riscv_fpu_switch:
    addi sp, sp, -16
    sd ra, 0(sp)
    beqz a0, .Lswitch_restore
    csrr t0, sstatus
    srli t0, t0, 13
    andi t0, t0, 3
    li t1, 3
    bne t0, t1, .Lswitch_restore

    call riscv_fpu_state_save

.Lswitch_restore:
    ld t0, RISCV_FPU_STATE_SAVED_OFFSET(a1)
    beqz t0, .Lswitch_done
    mv a0, a1
    call riscv_fpu_state_restore

.Lswitch_done:
    ld ra, 0(sp)
    addi sp, sp, 16
    ret

    .size riscv_fpu_switch, . - riscv_fpu_switch

/* void riscv_fpu_reset_current(struct riscv_fpu_state *state)
 *
 * Resets the live registers and the memory image to the initial state
 * for an exec: the calling task keeps running afterwards and returns to
 * user space without a context switch, so the hardware registers must
 * be cleared here (FS=Initial does not imply zeroed registers). */
    .globl riscv_fpu_reset_current
    .type riscv_fpu_reset_current, @function
riscv_fpu_reset_current:
    addi sp, sp, -16
    sd zero, 0(sp)
    fld f0, 0(sp)
    fld f1, 0(sp)
    fld f2, 0(sp)
    fld f3, 0(sp)
    fld f4, 0(sp)
    fld f5, 0(sp)
    fld f6, 0(sp)
    fld f7, 0(sp)
    fld f8, 0(sp)
    fld f9, 0(sp)
    fld f10, 0(sp)
    fld f11, 0(sp)
    fld f12, 0(sp)
    fld f13, 0(sp)
    fld f14, 0(sp)
    fld f15, 0(sp)
    fld f16, 0(sp)
    fld f17, 0(sp)
    fld f18, 0(sp)
    fld f19, 0(sp)
    fld f20, 0(sp)
    fld f21, 0(sp)
    fld f22, 0(sp)
    fld f23, 0(sp)
    fld f24, 0(sp)
    fld f25, 0(sp)
    fld f26, 0(sp)
    fld f27, 0(sp)
    fld f28, 0(sp)
    fld f29, 0(sp)
    fld f30, 0(sp)
    fld f31, 0(sp)
    csrw fcsr, zero
    addi sp, sp, 16

    sd zero, 0(a0)
    sd zero, 8(a0)
    sd zero, 16(a0)
    sd zero, 24(a0)
    sd zero, 32(a0)
    sd zero, 40(a0)
    sd zero, 48(a0)
    sd zero, 56(a0)
    sd zero, 64(a0)
    sd zero, 72(a0)
    sd zero, 80(a0)
    sd zero, 88(a0)
    sd zero, 96(a0)
    sd zero, 104(a0)
    sd zero, 112(a0)
    sd zero, 120(a0)
    sd zero, 128(a0)
    sd zero, 136(a0)
    sd zero, 144(a0)
    sd zero, 152(a0)
    sd zero, 160(a0)
    sd zero, 168(a0)
    sd zero, 176(a0)
    sd zero, 184(a0)
    sd zero, 192(a0)
    sd zero, 200(a0)
    sd zero, 208(a0)
    sd zero, 216(a0)
    sd zero, 224(a0)
    sd zero, 232(a0)
    sd zero, 240(a0)
    sd zero, 248(a0)
    sd zero, RISCV_FPU_STATE_FCSR_OFFSET(a0)
    li t0, 1
    sd t0, RISCV_FPU_STATE_SAVED_OFFSET(a0)

    li t0, RISCV_SSTATUS_FS_MASK
    csrc sstatus, t0
    li t0, RISCV_SSTATUS_FS_INITIAL
    csrs sstatus, t0
    ret

    .size riscv_fpu_reset_current, . - riscv_fpu_reset_current
