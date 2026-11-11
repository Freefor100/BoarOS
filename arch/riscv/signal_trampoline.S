    .section .rodata.vdso, "a", @progbits
    .balign 4
    .globl arch_elf_vdso_start
    .globl arch_elf_vdso_end
arch_elf_vdso_start:
    .incbin BOAROS_VDSO_IMAGE
arch_elf_vdso_end:
    .if (arch_elf_vdso_end - arch_elf_vdso_start) > (1 << BOAROS_PAGE_SHIFT)
    .error "vDSO ELF file exceeds its mapped page"
    .endif
