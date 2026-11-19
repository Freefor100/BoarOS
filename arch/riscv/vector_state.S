/* RVV state moves use whole-register operations, independent of user vl,
 * vtype and tail/mask policy. vstart must be zero for these transfers. */
#define RVV_CSR_VSTART 0x008
#define RVV_CSR_VCSR 0x00f
#define RVV_CSR_VL 0xc20
#define RVV_CSR_VTYPE 0xc21

    .section .text,"ax",@progbits
#ifdef BOAROS_RISCV_AS_EXTENSIONS
    .option push
    .option arch,+v
#endif

    /* Older assemblers do not have RVV 1.0 mnemonics. These are the same
     * whole-register instructions with t0 as the address register. */
    .macro rvv_store8 reg
#ifdef BOAROS_RISCV_AS_EXTENSIONS
    vs8r.v v\reg,(t0)
#else
    .word 0xe2828027 | (\reg << 7)
#endif
    .endm
    .macro rvv_load8 reg
#ifdef BOAROS_RISCV_AS_EXTENSIONS
    vl8re8.v v\reg,(t0)
#else
    .word 0xe2828007 | (\reg << 7)
#endif
    .endm

    .align 2
    .globl riscv_vector_save_hw
riscv_vector_save_hw:
    csrr t0,RVV_CSR_VSTART
    sd t0,0(a0)
    csrr t0,RVV_CSR_VL
    sd t0,8(a0)
    csrr t0,RVV_CSR_VTYPE
    sd t0,16(a0)
    csrr t0,RVV_CSR_VCSR
    sd t0,24(a0)
    csrw RVV_CSR_VSTART,zero
    ld t1,32(a0)
    slli t1,t1,3
    addi t0,a0,48
    rvv_store8 0
    add t0,t0,t1
    rvv_store8 8
    add t0,t0,t1
    rvv_store8 16
    add t0,t0,t1
    rvv_store8 24
    add t0,t0,t1
    ld t0,0(a0)
    csrw RVV_CSR_VSTART,t0
    li t0,0x600
    csrc sstatus,t0
    li t0,0x400
    csrs sstatus,t0
    ret
    .globl riscv_vector_restore_hw
riscv_vector_restore_hw:
    li t0,0x600
    csrs sstatus,t0
    csrw RVV_CSR_VSTART,zero
    ld t1,32(a0)
    slli t1,t1,3
    addi t0,a0,48
    rvv_load8 0
    add t0,t0,t1
    rvv_load8 8
    add t0,t0,t1
    rvv_load8 16
    add t0,t0,t1
    rvv_load8 24
    add t0,t0,t1
    ld t0,16(a0)
    ld t1,8(a0)
#ifdef BOAROS_RISCV_AS_EXTENSIONS
    vsetvl zero,t1,t0
#else
    .insn r 0x57, 7, 0x40, zero, t1, t0  /* vsetvl zero,t1,t0 */
#endif
    ld t0,24(a0)
    csrw RVV_CSR_VCSR,t0
    ld t0,0(a0)
    csrw RVV_CSR_VSTART,t0
    li t0,0x600
    csrc sstatus,t0
    li t0,0x400
    csrs sstatus,t0
    ret
#ifdef BOAROS_RISCV_AS_EXTENSIONS
    .option pop
#endif

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
