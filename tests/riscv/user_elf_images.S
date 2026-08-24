    .section .rodata.user_elf_images, "a", @progbits
    .balign 8
    .globl user_elf_program_start
user_elf_program_start:
    .incbin "build/riscv/tests/user/elf-probe-rv"
    .globl user_elf_program_end
user_elf_program_end:

    .balign 8
    .globl user_elf_fault_program_start
user_elf_fault_program_start:
    .incbin "build/riscv/tests/user/elf-text-fault-rv"
    .globl user_elf_fault_program_end
user_elf_fault_program_end:

    .balign 8
    .globl user_elf_guard_program_start
user_elf_guard_program_start:
    .incbin "build/riscv/tests/user/elf-guard-fault-rv"
    .globl user_elf_guard_program_end
user_elf_guard_program_end:
