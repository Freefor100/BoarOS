OUTPUT_ARCH(loongarch)
ENTRY(_start)
PHDRS { text PT_LOAD FLAGS(5); rodata PT_LOAD FLAGS(4); data PT_LOAD FLAGS(6); }
SECTIONS {
 . = 0x9000000000200000;
 __kernel_start = .;
 .text : AT(ADDR(.text) - 0x9000000000000000) {
   KEEP(*(.text.boot))
   /* Keep the syscall entry and accounting code together, before filesystem
    * or driver text growth can move individual parts of this path. */
   *(.text.la_trap_dispatch .text.la_trap_return_prepare)
   *(.text.kernel_cpu_account_trap_begin .text.kernel_cpu_account_trap_end)
   *(.text.kernel_syscall_dispatch .text.kernel_task_user_tgid)
   *trap_entry.o(.text .text.*)
   *(.text .text.*)
   . = ALIGN(0x4000);
 } :text
 .rodata : AT(ADDR(.rodata) - 0x9000000000000000) { *(.rodata .rodata.*) . = ALIGN(0x4000); } :rodata
 .data : AT(ADDR(.data) - 0x9000000000000000) { *(.data .data.*) . = ALIGN(0x4000); } :data
 .bss (NOLOAD) : AT(ADDR(.bss) - 0x9000000000000000) {
   __bss_start = .;
   /* 与RV相同：CPU本地状态、共享锁和调度器前缀集中在现有BSS首页。 */
   *(.bss.scheduler.cpu) . = ALIGN(64);
   *(.bss.scheduler.wait) . = ALIGN(64);
   *(.bss.scheduler.state)
   *(.bss .bss.*) *(COMMON)
   . = ALIGN(16); __boot_stack_bottom = .; . += 0x10000;
   __boot_stack_top = .; __bss_end = .;
 } :data
 . = ALIGN(0x4000); __kernel_end = .;
 /DISCARD/ : { *(.comment) *(.eh_frame .eh_frame.*) *(.note .note.*) }
}
