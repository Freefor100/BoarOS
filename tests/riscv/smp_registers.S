    .section .text, "ax", @progbits
    .align 2
    .globl riscv_smp_gpr_wait
    .type riscv_smp_gpr_wait, @function
riscv_smp_gpr_wait:
    addi sp, sp, -112
    sd ra, 0(sp)
    sd s0, 8(sp)
    sd s1, 16(sp)
    sd s2, 24(sp)
    sd s3, 32(sp)
    sd s4, 40(sp)
    sd s5, 48(sp)
    sd s6, 56(sp)
    sd s7, 64(sp)
    sd s8, 72(sp)
    sd s9, 80(sp)
    sd s10, 88(sp)
    sd s11, 96(sp)
    li s0, 0x710
    li s1, 0x711
    li s2, 0x712
    li s3, 0x713
    li s4, 0x714
    li s5, 0x715
    li s6, 0x716
    li s7, 0x717
    li s8, 0x718
    li s9, 0x719
    li s10, 0x71a
    li s11, 0x71b
    /* 按真实ABI跨C park与另一hart恢复，保护context保存的整数寄存器。 */
    call riscv_smp_park_migration
    li t0, 0x710
    bne s0, t0, 1f
    li t0, 0x711
    bne s1, t0, 1f
    li t0, 0x712
    bne s2, t0, 1f
    li t0, 0x713
    bne s3, t0, 1f
    li t0, 0x714
    bne s4, t0, 1f
    li t0, 0x715
    bne s5, t0, 1f
    li t0, 0x716
    bne s6, t0, 1f
    li t0, 0x717
    bne s7, t0, 1f
    li t0, 0x718
    bne s8, t0, 1f
    li t0, 0x719
    bne s9, t0, 1f
    li t0, 0x71a
    bne s10, t0, 1f
    li t0, 0x71b
    bne s11, t0, 1f
    li a0, 1
    j 2f
1:
    li a0, 0
2:
    ld ra, 0(sp)
    ld s0, 8(sp)
    ld s1, 16(sp)
    ld s2, 24(sp)
    ld s3, 32(sp)
    ld s4, 40(sp)
    ld s5, 48(sp)
    ld s6, 56(sp)
    ld s7, 64(sp)
    ld s8, 72(sp)
    ld s9, 80(sp)
    ld s10, 88(sp)
    ld s11, 96(sp)
    addi sp, sp, 112
    ret
    .size riscv_smp_gpr_wait, . - riscv_smp_gpr_wait
