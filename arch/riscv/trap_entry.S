    .section .text, "ax", @progbits
    .align 2
    .globl riscv_trap_entry
    .type riscv_trap_entry, @function

riscv_trap_entry:
    csrr a0, scause
    csrr a1, sepc
    csrr a2, stval
    csrr a3, sstatus
    tail riscv_trap_fatal

    .size riscv_trap_entry, . - riscv_trap_entry
