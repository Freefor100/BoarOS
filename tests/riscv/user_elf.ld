OUTPUT_ARCH(riscv)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x10000;

    .text : ALIGN(16)
    {
        KEEP(*(.text.start))
        *(.text .text.*)
        *(.rodata .rodata.*)
    } :text

    . = ALIGN(0x1000);

    .data : ALIGN(16)
    {
        KEEP(*(.data .data.*))
    } :data

    .bss (NOLOAD) : ALIGN(16)
    {
        KEEP(*(.bss .bss.*))
        *(COMMON)
    } :data

    __program_break = ALIGN(0x1000);

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.riscv.attributes)
    }
}
