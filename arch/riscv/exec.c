#include <arch/riscv/exec.h>
#include <kernel/elf_image.h>
enum riscv_exec_status riscv_exec_init(struct physical_page_allocator *a,
    const struct riscv_sv39_page_table *table)
{ return (enum riscv_exec_status)kernel_exec_image_bind(a,table); }
