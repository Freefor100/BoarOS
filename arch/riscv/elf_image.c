#include <arch/riscv/elf_image.h>
#include <kernel/elf_image.h>
enum riscv_elf_image_status riscv_elf_image_build(
    const struct kernel_exec_image_request *request, struct kernel_heap *heap,
    struct physical_page_allocator *allocator,
    const struct riscv_sv39_page_table *table, struct kernel_exec_image *image)
{ return (enum riscv_elf_image_status)kernel_elf_image_build(request,heap,allocator,table,image); }
