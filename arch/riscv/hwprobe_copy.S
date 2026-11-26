    .section .text, "ax", @progbits
    .align 2

    /* a0=目标，a1=来源，a2=字节数（64的倍数）。两种循环每轮各执行八对加载和存储。 */
    .macro copy_loop load, store, stride
    add a3, a0, a2
1:
    .irp i, 0,1,2,3,4,5,6,7
    \load a4, (\i * \stride)(a1)
    \store a4, (\i * \stride)(a0)
    .endr
    addi a1, a1, (8 * \stride)
    addi a0, a0, (8 * \stride)
    bne a0, a3, 1b
    .endm

    .globl riscv_hwprobe_copy_words
    .type riscv_hwprobe_copy_words, @function
riscv_hwprobe_copy_words:
    copy_loop ld, sd, 8
    .globl riscv_hwprobe_words_end
riscv_hwprobe_words_end:
    li a0, 0
    ret
    .globl riscv_hwprobe_words_fault
riscv_hwprobe_words_fault:
    li a0, -1
    ret
    .size riscv_hwprobe_copy_words, . - riscv_hwprobe_copy_words

    .align 2
    .globl riscv_hwprobe_copy_bytes
    .type riscv_hwprobe_copy_bytes, @function
riscv_hwprobe_copy_bytes:
    copy_loop lbu, sb, 1
    li a0, 0
    ret
    .size riscv_hwprobe_copy_bytes, . - riscv_hwprobe_copy_bytes
