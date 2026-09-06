OUTPUT_ARCH(riscv)

KERNEL_VIRTUAL_BASE = 0xffffffff80000000;
KERNEL_PHYSICAL_BASE = 0x80200000;
KERNEL_LOAD_OFFSET = KERNEL_VIRTUAL_BASE - KERNEL_PHYSICAL_BASE;

ENTRY(__kernel_physical_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = KERNEL_VIRTUAL_BASE;
    __kernel_start = .;
    __kernel_physical_start = KERNEL_PHYSICAL_BASE;

    .text : AT(ADDR(.text) - KERNEL_LOAD_OFFSET)
    {
        __text_start = .;
        KEEP(*(.text.boot))
        *(.text .text.*)
        . = ALIGN(0x1000);
        __text_end = .;
    } :text

    .rodata : AT(ADDR(.rodata) - KERNEL_LOAD_OFFSET)
    {
        __rodata_start = .;
        *(.srodata .srodata.*)
        *(.rodata .rodata.*)
        . = ALIGN(0x1000);
        __rodata_end = .;
    } :rodata

    .data : AT(ADDR(.data) - KERNEL_LOAD_OFFSET)
    {
        __data_start = .;
        PROVIDE(__global_pointer$ = . + 0x800);
        *(.sdata .sdata.*)
        *(.data .data.*)
    } :data

    . = ALIGN(0x1000);
    .bss (NOLOAD) : AT(ADDR(.bss) - KERNEL_LOAD_OFFSET)
    {
        __bss_start = .;
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)

        . = ALIGN(16);
        __boot_stack_bottom = .;
        . += 0x4000;
        __boot_stack_top = .;
        __bss_end = .;
    } :data

    . = ALIGN(0x1000);
    __data_end = .;
    __kernel_end = .;
    __kernel_physical_end = __kernel_end - KERNEL_LOAD_OFFSET;

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame .eh_frame.*)
        *(.note .note.*)
    }
}
