OUTPUT_ARCH(loongarch)
ENTRY(_start)
PHDRS { text PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
 . = 0x400000;
 .text : { *(.text .text.*) *(.rodata .rodata.*) } :text
 . = ALIGN(0x4000);
 .data : { *(.data .data.*) } :data
 .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) } :data
 /DISCARD/ : { *(.eh_frame .eh_frame.*) *(.note .note.*) *(.comment) }
}
