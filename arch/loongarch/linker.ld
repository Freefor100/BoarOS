OUTPUT_ARCH(loongarch)
ENTRY(_start)
PHDRS { text PT_LOAD FLAGS(5); rodata PT_LOAD FLAGS(4); data PT_LOAD FLAGS(6); }
SECTIONS {
 . = 0x9000000000200000;
 __kernel_start = .;
 .text : AT(ADDR(.text) - 0x9000000000000000) { KEEP(*(.text.boot)) *(.text .text.*) . = ALIGN(0x4000); } :text
 .rodata : AT(ADDR(.rodata) - 0x9000000000000000) { *(.rodata .rodata.*) . = ALIGN(0x4000); } :rodata
 .data : AT(ADDR(.data) - 0x9000000000000000) { *(.data .data.*) . = ALIGN(0x4000); } :data
 .bss (NOLOAD) : AT(ADDR(.bss) - 0x9000000000000000) {
   __bss_start = .; *(.bss .bss.*) *(COMMON)
   . = ALIGN(16); __boot_stack_bottom = .; . += 0x10000;
   __boot_stack_top = .; __bss_end = .;
 } :data
 . = ALIGN(0x4000); __kernel_end = .;
 /DISCARD/ : { *(.comment) *(.eh_frame .eh_frame.*) *(.note .note.*) }
}
