/* RVV state moves use whole-register operations, independent of user vl,
 * vtype and tail/mask policy. vstart must be zero for these transfers. */
    .section .text,"ax",@progbits
    .option push
    .option arch,+v
    .align 2
    .globl riscv_vector_save_hw
riscv_vector_save_hw:
    csrr t0,vstart
    sd t0,0(a0)
    csrr t0,vl
    sd t0,8(a0)
    csrr t0,vtype
    sd t0,16(a0)
    csrr t0,vcsr
    sd t0,24(a0)
    csrw vstart,zero
    ld t1,32(a0)
    slli t1,t1,3
    addi t0,a0,48
    vs8r.v v0,(t0)
    add t0,t0,t1
    vs8r.v v8,(t0)
    add t0,t0,t1
    vs8r.v v16,(t0)
    add t0,t0,t1
    vs8r.v v24,(t0)
    add t0,t0,t1
    ld t0,0(a0)
    csrw vstart,t0
    li t0,0x600
    csrc sstatus,t0
    li t0,0x400
    csrs sstatus,t0
    ret
    .globl riscv_vector_restore_hw
riscv_vector_restore_hw:
    li t0,0x600
    csrs sstatus,t0
    csrw vstart,zero
    ld t1,32(a0)
    slli t1,t1,3
    addi t0,a0,48
    vl8re8.v v0,(t0)
    add t0,t0,t1
    vl8re8.v v8,(t0)
    add t0,t0,t1
    vl8re8.v v16,(t0)
    add t0,t0,t1
    vl8re8.v v24,(t0)
    add t0,t0,t1
    ld t0,16(a0)
    ld t1,8(a0)
    vsetvl zero,t1,t0
    ld t0,24(a0)
    csrw vcsr,t0
    ld t0,0(a0)
    csrw vstart,t0
    li t0,0x600
    csrc sstatus,t0
    li t0,0x400
    csrs sstatus,t0
    ret
    .option pop

    .globl riscv_vector_status_hw
riscv_vector_status_hw:
    csrr a0,sstatus
    ret
    .globl riscv_vector_off_hw
riscv_vector_off_hw:
    li t0,0x600
    csrc sstatus,t0
    ret
    .globl riscv_vector_probe_hw
riscv_vector_probe_hw:
    li t0,0x600
    csrs sstatus,t0
    csrr a0,0xc22
    csrc sstatus,t0
    ret
