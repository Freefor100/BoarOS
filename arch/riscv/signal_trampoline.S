    .section .rodata.signal_trampoline, "a", @progbits
    .balign 4
    .globl arch_elf_trampoline_start
    .globl arch_elf_trampoline_end
arch_elf_trampoline_start:
    li a7, 139
    ecall
arch_elf_trampoline_end:
