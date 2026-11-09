#include <kernel/vdso.h>

PHDRS
{
    image PT_LOAD FILEHDR PHDRS FLAGS(5);
    dynamic PT_DYNAMIC FLAGS(4);
}

SECTIONS
{
    . = KERNEL_VDSO_RESTORER_OFFSET;
    .text : { *(.text.sigreturn) *(.text .text.*) } :image
    .hash : { *(.hash) } :image
    .gnu.hash : { *(.gnu.hash) } :image
    .dynsym : { *(.dynsym) } :image
    .dynstr : { *(.dynstr) } :image
    .gnu.version : { *(.gnu.version) } :image
    .gnu.version_d : { *(.gnu.version_d) } :image
    .dynamic : { *(.dynamic) } :image :dynamic
    .rodata : { *(.rodata .rodata.*) } :image
    .got : { *(.got .got.*) } :image
    /DISCARD/ : { *(.comment .note.* .eh_frame* .riscv.attributes .gnu.attributes) }
    HIDDEN(__boaros_vvar = 1 << BOAROS_PAGE_SHIFT);
    ASSERT(__vdso_rt_sigreturn == KERNEL_VDSO_RESTORER_OFFSET, "sigreturn offset changed")
    ASSERT(. <= (1 << BOAROS_PAGE_SHIFT), "vDSO exceeds its page")
}

VERSION
{
#ifdef __riscv
    LINUX_4.15 {
#else
    LINUX_5.10 {
#endif
        global: __vdso_clock_gettime; __vdso_clock_getres; __vdso_gettimeofday;
                __vdso_rt_sigreturn;
        local: *;
    };
}
