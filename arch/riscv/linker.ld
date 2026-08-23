OUTPUT_ARCH(riscv)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x80200000;
    __kernel_start = .;

    .text :
    {
        __text_start = .;
        KEEP(*(.text.boot))
        *(.text .text.*)
        . = ALIGN(0x1000);
        __text_end = .;
    } :text

    .rodata :
    {
        __rodata_start = .;
        *(.srodata .srodata.*)
        *(.rodata .rodata.*)
        . = ALIGN(0x1000);
        __rodata_end = .;
    } :rodata

    .data :
    {
        __data_start = .;
        PROVIDE(__global_pointer$ = . + 0x800);
        *(.sdata .sdata.*)
        *(.data .data.*)
    } :data

    . = ALIGN(0x1000);
    .bss (NOLOAD) :
    {
        __bss_start = .;
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)

        . = ALIGN(16);
        __boot_stack_bottom = .;
        . += 0x1000;
        __boot_stack_top = .;
        __bss_end = .;
    } :data

    . = ALIGN(0x1000);
    __data_end = .;
    __kernel_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame .eh_frame.*)
        *(.note .note.*)
    }
}
