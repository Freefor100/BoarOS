#ifndef BOAROS_ARCH_RISCV_ELF_IMAGE_H
#define BOAROS_ARCH_RISCV_ELF_IMAGE_H

#include <arch/riscv/sv39.h>
#include <kernel/exec_image.h>

struct kernel_heap;

enum riscv_elf_image_status {
    RISCV_ELF_IMAGE_STATUS_OK = 0,
    RISCV_ELF_IMAGE_STATUS_INVALID_ARGUMENT,
    RISCV_ELF_IMAGE_STATUS_NO_MEMORY,
    RISCV_ELF_IMAGE_STATUS_ADDRESS_SPACE,
    RISCV_ELF_IMAGE_STATUS_MALFORMED,
    RISCV_ELF_IMAGE_STATUS_WRONG_ARCH,
    RISCV_ELF_IMAGE_STATUS_IO,
    RISCV_ELF_IMAGE_STATUS_CLEANUP_REQUIRED,
};

/* Creates one fully described RISC-V user MM with demand-loaded ELF VMAs. */
enum riscv_elf_image_status riscv_elf_image_build(
    const struct kernel_exec_image_request *request,
    struct kernel_heap *heap,
    struct physical_page_allocator *allocator,
    const struct riscv_sv39_page_table *kernel_table,
    struct kernel_exec_image *image);

#endif
