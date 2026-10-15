#include <arch/riscv/trap.h>
    .section .text,"ax",@progbits
    .option norvc
    .globl mm_smp_store
    .globl mm_smp_store_pc
mm_smp_store:
    csrr t0, sstatus
    li t1, (1 << 18)
    csrs sstatus, t1
mm_smp_store_pc:
    sd a1, 0(a0)
    csrw sstatus, t0
    ret
    .globl mm_smp_load
mm_smp_load:
    csrr t0, sstatus
    li t1, (1 << 18)
    csrs sstatus, t1
    ld a0, 0(a0)
    csrw sstatus, t0
    ret

    /* 保存当前S栈，U trap使用该SP下方的新frame；不覆盖调用者C帧。 */
    .globl mm_smp_user
    .globl mm_smp_user_return
mm_smp_user:
    addi sp, sp, -64
    sd ra, 0(sp)
    csrr t0, sstatus
    sd t0, 8(sp)
    sd s0, 16(sp)
    sd s1, 24(sp)
    sd s2, 32(sp)
    mv s0, a0
    mv s1, a1
    mv s2, a2
    mv a0, sp
    la a1, mm_smp_user_return
    call mm_smp_user_enter
    li t0, RISCV_SSTATUS_SIE | RISCV_SSTATUS_SPIE | RISCV_SSTATUS_SPP
    csrc sstatus, t0
    csrw sepc, s0
    csrw sscratch, tp
    mv a0, s2
    mv sp, s1
    sret
mm_smp_user_return:
    ld t0, 8(sp)
    csrw sstatus, t0
    ld ra, 0(sp)
    ld s0, 16(sp)
    ld s1, 24(sp)
    ld s2, 32(sp)
    addi sp, sp, 64
    ret
