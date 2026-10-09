#include <arch/riscv/trap.h>
#include <arch/riscv/thread.h>

    .section .text, "ax", @progbits
    .align 2
    .globl riscv_trap_entry
    .type riscv_trap_entry, @function

riscv_trap_entry:
    /* U-mode keeps current in sscratch and its own tp in tp. */
    csrrw tp, sscratch, tp
    bnez tp, .Luser_entry

    /* S-mode keeps sscratch zero; restore its kernel tp immediately. */
    csrrw tp, sscratch, tp
    addi sp, sp, -RISCV_TRAP_FRAME_SIZE

    sd t0, RISCV_TRAP_FRAME_T0(sp)
#if BOAROS_COST_DIAGNOSTICS
    sd t1, RISCV_TRAP_FRAME_T1(sp)
    rdtime t0
    la t1, kernel_cost_trap_timestamp
    sd t0, 0(t1)
#endif
    sd tp, RISCV_TRAP_FRAME_TP(sp)
    sd zero, RISCV_TRAP_FRAME_KERNEL_TP(sp)
    addi t0, sp, RISCV_TRAP_FRAME_SIZE
    sd t0, RISCV_TRAP_FRAME_SP(sp)
    j .Lsave_common

.Luser_entry:
    sd sp, RISCV_THREAD_STATE_USER_SP(tp)
    ld sp, RISCV_THREAD_STATE_KERNEL_SP(tp)
    addi sp, sp, -RISCV_TRAP_FRAME_SIZE

    sd t0, RISCV_TRAP_FRAME_T0(sp)
#if BOAROS_COST_DIAGNOSTICS
    sd t1, RISCV_TRAP_FRAME_T1(sp)
    rdtime t0
    la t1, kernel_cost_trap_timestamp
    sd t0, 0(t1)
#endif
    ld t0, RISCV_THREAD_STATE_USER_SP(tp)
    sd t0, RISCV_TRAP_FRAME_SP(sp)
    csrr t0, sscratch
    sd t0, RISCV_TRAP_FRAME_TP(sp)
    sd tp, RISCV_TRAP_FRAME_KERNEL_TP(sp)
    csrw sscratch, zero

.Lsave_common:
    sd ra, RISCV_TRAP_FRAME_RA(sp)
    sd gp, RISCV_TRAP_FRAME_GP(sp)

    /* The saved gp may be user-controlled; C must see the kernel value. */
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop

#if !BOAROS_COST_DIAGNOSTICS
    sd t1, RISCV_TRAP_FRAME_T1(sp)
#endif
    sd t2, RISCV_TRAP_FRAME_T2(sp)
    sd s0, RISCV_TRAP_FRAME_S0(sp)
    sd s1, RISCV_TRAP_FRAME_S1(sp)
    sd a0, RISCV_TRAP_FRAME_A0(sp)
    sd a1, RISCV_TRAP_FRAME_A1(sp)
    sd a2, RISCV_TRAP_FRAME_A2(sp)
    sd a3, RISCV_TRAP_FRAME_A3(sp)
    sd a4, RISCV_TRAP_FRAME_A4(sp)
    sd a5, RISCV_TRAP_FRAME_A5(sp)
    sd a6, RISCV_TRAP_FRAME_A6(sp)
    sd a7, RISCV_TRAP_FRAME_A7(sp)
    sd s2, RISCV_TRAP_FRAME_S2(sp)
    sd s3, RISCV_TRAP_FRAME_S3(sp)
    sd s4, RISCV_TRAP_FRAME_S4(sp)
    sd s5, RISCV_TRAP_FRAME_S5(sp)
    sd s6, RISCV_TRAP_FRAME_S6(sp)
    sd s7, RISCV_TRAP_FRAME_S7(sp)
    sd s8, RISCV_TRAP_FRAME_S8(sp)
    sd s9, RISCV_TRAP_FRAME_S9(sp)
    sd s10, RISCV_TRAP_FRAME_S10(sp)
    sd s11, RISCV_TRAP_FRAME_S11(sp)
    sd t3, RISCV_TRAP_FRAME_T3(sp)
    sd t4, RISCV_TRAP_FRAME_T4(sp)
    sd t5, RISCV_TRAP_FRAME_T5(sp)
    sd t6, RISCV_TRAP_FRAME_T6(sp)

    csrr t0, sstatus
    sd t0, RISCV_TRAP_FRAME_SSTATUS(sp)
    csrr t0, sepc
    sd t0, RISCV_TRAP_FRAME_SEPC(sp)
    csrr t0, scause
    sd t0, RISCV_TRAP_FRAME_SCAUSE(sp)
    csrr t0, stval
    sd t0, RISCV_TRAP_FRAME_STVAL(sp)

    mv a0, sp
    call riscv_trap_dispatch

    .size riscv_trap_entry, . - riscv_trap_entry

    .globl riscv_trap_return
    .type riscv_trap_return, @function
riscv_trap_return:
    call kernel_scheduler_switch_finish
    mv a0, sp
    call riscv_trap_return_prepare
    ld t0, RISCV_TRAP_FRAME_SSTATUS(sp)
    andi t1, t0, RISCV_SSTATUS_SIE
    bnez t1, .Lbad_return
    andi t1, t0, RISCV_SSTATUS_SPP
    bnez t1, .Lsupervisor_return

    ld t1, RISCV_TRAP_FRAME_KERNEL_TP(sp)
    beqz t1, .Lbad_return
    bne tp, t1, .Lbad_return
    ld t1, RISCV_THREAD_STATE_USER_MODE(tp)
    beqz t1, .Lbad_return
    csrw sscratch, tp
    j .Lprepare_return

.Lsupervisor_return:
    csrw sscratch, zero

.Lprepare_return:
#if BOAROS_COST_DIAGNOSTICS
    ld a0, RISCV_TRAP_FRAME_SSTATUS(sp)
    call kernel_cost_irq_return
#endif
    ld t0, RISCV_TRAP_FRAME_SEPC(sp)
    addi t1, sp, RISCV_TRAP_FRAME_SEPC
    sc.d zero, t0, (t1)
    csrw sepc, t0
    ld t0, RISCV_TRAP_FRAME_SSTATUS(sp)
    csrw sstatus, t0

    ld ra, RISCV_TRAP_FRAME_RA(sp)
    ld gp, RISCV_TRAP_FRAME_GP(sp)
    ld tp, RISCV_TRAP_FRAME_TP(sp)
#if !BOAROS_COST_DIAGNOSTICS
    ld t0, RISCV_TRAP_FRAME_T0(sp)
    ld t1, RISCV_TRAP_FRAME_T1(sp)
#endif
    ld t2, RISCV_TRAP_FRAME_T2(sp)
    ld s0, RISCV_TRAP_FRAME_S0(sp)
    ld s1, RISCV_TRAP_FRAME_S1(sp)
    ld a0, RISCV_TRAP_FRAME_A0(sp)
    ld a1, RISCV_TRAP_FRAME_A1(sp)
    ld a2, RISCV_TRAP_FRAME_A2(sp)
    ld a3, RISCV_TRAP_FRAME_A3(sp)
    ld a4, RISCV_TRAP_FRAME_A4(sp)
    ld a5, RISCV_TRAP_FRAME_A5(sp)
    ld a6, RISCV_TRAP_FRAME_A6(sp)
    ld a7, RISCV_TRAP_FRAME_A7(sp)
    ld s2, RISCV_TRAP_FRAME_S2(sp)
    ld s3, RISCV_TRAP_FRAME_S3(sp)
    ld s4, RISCV_TRAP_FRAME_S4(sp)
    ld s5, RISCV_TRAP_FRAME_S5(sp)
    ld s6, RISCV_TRAP_FRAME_S6(sp)
    ld s7, RISCV_TRAP_FRAME_S7(sp)
    ld s8, RISCV_TRAP_FRAME_S8(sp)
    ld s9, RISCV_TRAP_FRAME_S9(sp)
    ld s10, RISCV_TRAP_FRAME_S10(sp)
    ld s11, RISCV_TRAP_FRAME_S11(sp)
    ld t3, RISCV_TRAP_FRAME_T3(sp)
    ld t4, RISCV_TRAP_FRAME_T4(sp)
    ld t5, RISCV_TRAP_FRAME_T5(sp)
    ld t6, RISCV_TRAP_FRAME_T6(sp)
#if BOAROS_COST_DIAGNOSTICS
    la t0, kernel_cost_return_pending
    ld t1, 0(t0)
    beqz t1, .Lcost_return_saved
    rdtime t1
    la t0, kernel_cost_return_timestamp
    sd t1, 0(t0)
    la t0, kernel_cost_return_pending
    li t1, 1
    sd t1, 0(t0)
.Lcost_return_saved:
    ld t0, RISCV_TRAP_FRAME_T0(sp)
    ld t1, RISCV_TRAP_FRAME_T1(sp)
#endif
    ld sp, RISCV_TRAP_FRAME_SP(sp)
    sret

.Lbad_return:
    mv a0, sp
    tail riscv_trap_bad_return

    .size riscv_trap_return, . - riscv_trap_return
