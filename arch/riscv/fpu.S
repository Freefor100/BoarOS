#include <arch/riscv/fpu.h>
#include <arch/riscv/trap.h>

    .section .text, "ax", @progbits
    .align 2

    /* Keep floating-point instructions local to the state routines. Older
     * assemblers reject floating-point registers in the typed .insn syntax. */
#ifdef BOAROS_RISCV_AS_EXTENSIONS
    .option arch, +f, +d
#endif

#define FPU_REGISTERS 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15, \
                      16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31

    .macro fpu_store reg, offset
#ifdef BOAROS_RISCV_AS_EXTENSIONS
    fsd f\reg, \offset(a0)
#else
    /* FSD: split the offset around the source register and x10 (a0). */
    .4byte 0x3027 | ((((\offset) >> 5) & 0x7f) << 25) | (\reg << 20) | (10 << 15) | (((\offset) & 0x1f) << 7)
#endif
    .endm

    .macro fpu_load reg, offset, base
#ifdef BOAROS_RISCV_AS_EXTENSIONS
    fld f\reg, \offset(x\base)
#else
    /* FLD: offset in bits 31:20, destination in bits 11:7. */
    .4byte 0x3007 | (((\offset) & 0xfff) << 20) | (\base << 15) | (\reg << 7)
#endif
    .endm

/* void riscv_fpu_state_save(struct riscv_fpu_state *state)
 *
 * Saves the live registers into `state` and marks it authoritative;
 * FS ends Clean so later user modification is tracked again. */
    .globl riscv_fpu_state_save
    .type riscv_fpu_state_save, @function
riscv_fpu_state_save:
    .irp reg, FPU_REGISTERS
    fpu_store \reg, 8*\reg
    .endr
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
    .irp reg, FPU_REGISTERS
    fpu_load \reg, 8*\reg, 10
    .endr
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
    .irp reg, FPU_REGISTERS
    fpu_load \reg, 0, 2
    .endr
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
