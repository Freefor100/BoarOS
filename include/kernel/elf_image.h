#ifndef BOAROS_KERNEL_ELF_IMAGE_H
#define BOAROS_KERNEL_ELF_IMAGE_H

#include <arch/mmu.h>
#include <kernel/exec_image.h>

struct kernel_heap;

enum kernel_elf_image_status {
    KERNEL_ELF_IMAGE_STATUS_OK = 0,
    KERNEL_ELF_IMAGE_STATUS_INVALID_ARGUMENT,
    KERNEL_ELF_IMAGE_STATUS_NO_MEMORY,
    KERNEL_ELF_IMAGE_STATUS_ADDRESS_SPACE,
    KERNEL_ELF_IMAGE_STATUS_MALFORMED,
    KERNEL_ELF_IMAGE_STATUS_WRONG_ARCH,
    KERNEL_ELF_IMAGE_STATUS_IO,
    KERNEL_ELF_IMAGE_STATUS_STACK_LIMIT,
    KERNEL_ELF_IMAGE_STATUS_CLEANUP_REQUIRED,
};

/* Creates one fully described architecture user MM with demand-loaded ELF VMAs. */
enum kernel_elf_image_status kernel_elf_image_build(
    const struct kernel_exec_image_request *request,
    struct kernel_heap *heap,
    struct physical_page_allocator *allocator,
    const struct arch_mmu_page_table *kernel_table,
    struct kernel_exec_image *image);

int kernel_exec_image_bind(struct physical_page_allocator *, const struct arch_mmu_page_table *);
#endif
