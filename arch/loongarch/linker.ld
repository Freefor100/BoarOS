OUTPUT_ARCH(loongarch)
ENTRY(__firmware_entry)
PHDRS { text PT_LOAD FLAGS(5); rodata PT_LOAD FLAGS(4); data PT_LOAD FLAGS(6); }
SECTIONS {
 /* Match LA_DIRECT_BASE in boot_map.h. QEMU's ELF boot enters by PA. */
 KERNEL_VIRTUAL_BASE = 0xffff900000000000;
 . = KERNEL_VIRTUAL_BASE + 0x200000;
 __kernel_start = .;
 __firmware_entry = _start - KERNEL_VIRTUAL_BASE;
 .text : AT(ADDR(.text) - KERNEL_VIRTUAL_BASE) {
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
 .rodata : AT(ADDR(.rodata) - KERNEL_VIRTUAL_BASE) { *(.rodata .rodata.*) . = ALIGN(0x4000); } :rodata
 .data : AT(ADDR(.data) - KERNEL_VIRTUAL_BASE) { *(.data .data.*) . = ALIGN(0x4000); } :data
 .bss (NOLOAD) : AT(ADDR(.bss) - KERNEL_VIRTUAL_BASE) {
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
 ASSERT(__kernel_end - KERNEL_VIRTUAL_BASE <= 0x02000000, "kernel outgrows LA bootstrap mapping")
 /DISCARD/ : { *(.comment) *(.eh_frame .eh_frame.*) *(.note .note.*) }
}
