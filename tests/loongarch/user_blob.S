    .section .rodata.user_image, "a", @progbits
    .balign 8
    .globl la_user_elf_start
    .globl la_user_elf_end
la_user_elf_start:
    .incbin "build/loongarch/user-probe"
la_user_elf_end:
