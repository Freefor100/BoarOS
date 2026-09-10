#include <arch/riscv/elf_image.h>
#include <arch/riscv/exec.h>
#include <arch/riscv/mm.h>
#include <kernel/errno.h>
#include <kernel/exec_image.h>
#include <kernel/heap.h>

enum riscv_exec_status riscv_exec_init(
    struct physical_page_allocator *allocator,
    const struct riscv_sv39_page_table *kernel_table)
{
    extern struct physical_page_allocator *riscv_exec_allocator;
    extern const struct riscv_sv39_page_table *riscv_exec_kernel_table;

    if (allocator == 0 || kernel_table == 0 ||
        kernel_table->state != RISCV_SV39_STATE_ACTIVE ||
        kernel_table->allocator != allocator) {
        return RISCV_EXEC_STATUS_INVALID_ARGUMENT;
    }
    if (riscv_exec_allocator != 0 || riscv_exec_kernel_table != 0) {
        return RISCV_EXEC_STATUS_ALREADY_INITIALIZED;
    }
    riscv_exec_allocator = allocator;
    riscv_exec_kernel_table = kernel_table;
    return RISCV_EXEC_STATUS_OK;
}

/* Kept as globals so both the image builder and the ABI entry share one
 * architecture binding without exposing it in the generic exec interface. */
struct physical_page_allocator *riscv_exec_allocator;
const struct riscv_sv39_page_table *riscv_exec_kernel_table;

static int64_t image_linux_error(enum riscv_elf_image_status status)
{
    switch (status) {
    case RISCV_ELF_IMAGE_STATUS_NO_MEMORY:
        return -KERNEL_ENOMEM;
    case RISCV_ELF_IMAGE_STATUS_IO:
        return -KERNEL_EIO;
    case RISCV_ELF_IMAGE_STATUS_MALFORMED:
    case RISCV_ELF_IMAGE_STATUS_WRONG_ARCH:
        return -KERNEL_ENOEXEC;
    case RISCV_ELF_IMAGE_STATUS_OK:
    case RISCV_ELF_IMAGE_STATUS_CLEANUP_REQUIRED:
        return 0;
    case RISCV_ELF_IMAGE_STATUS_ADDRESS_SPACE:
        return -KERNEL_ENOMEM;
    case RISCV_ELF_IMAGE_STATUS_INVALID_ARGUMENT:
        return -KERNEL_EINVAL;
    default:
        return 0;
    }
}

enum kernel_exec_image_status kernel_exec_image_prepare(
    const struct kernel_exec_image_request *request,
    struct kernel_heap *heap,
    struct kernel_exec_image *image,
    int64_t *linux_result)
{
    enum riscv_elf_image_status status;
    int64_t error;

    if (request == 0 || heap == 0 || image == 0 || linux_result == 0 ||
        image->mm.state != KERNEL_MM_EMPTY || image->arch_private != 0 ||
        riscv_exec_allocator == 0 || riscv_exec_kernel_table == 0 ||
        heap->page_allocator != riscv_exec_allocator) {
        return KERNEL_EXEC_IMAGE_STATUS_STATE;
    }
    status = riscv_elf_image_build(request,
                                   heap,
                                   riscv_exec_allocator,
                                   riscv_exec_kernel_table,
                                   image);
    error = image_linux_error(status);
    if (error != 0) {
        *linux_result = error;
        return KERNEL_EXEC_IMAGE_STATUS_LINUX_ERROR;
    }
    if (status == RISCV_ELF_IMAGE_STATUS_OK) {
        *linux_result = 0;
        return KERNEL_EXEC_IMAGE_STATUS_OK;
    }
    return status == RISCV_ELF_IMAGE_STATUS_CLEANUP_REQUIRED
               ? KERNEL_EXEC_IMAGE_STATUS_CLEANUP_REQUIRED
               : KERNEL_EXEC_IMAGE_STATUS_STATE;
}

enum kernel_exec_image_status kernel_exec_image_cleanup(
    struct kernel_heap *heap,
    struct kernel_exec_image *image)
{
    enum kernel_mm_status status;

    if (heap == 0 || image == 0 ||
        image->arch_private != 0 ||
        (image->mm.state != KERNEL_MM_EMPTY &&
         image->mm.state != KERNEL_MM_LIVE &&
         image->mm.state != KERNEL_MM_CLEANUP &&
         image->mm.state != KERNEL_MM_RELEASED &&
         image->mm.state != KERNEL_MM_MOVED)) {
        return KERNEL_EXEC_IMAGE_STATUS_STATE;
    }
    if (image->mm.state == KERNEL_MM_LIVE ||
        image->mm.state == KERNEL_MM_CLEANUP) {
        status = kernel_mm_release(&image->mm);
        if (status != KERNEL_MM_STATUS_OK) {
            return status == KERNEL_MM_STATUS_CLEANUP_REQUIRED
                       ? KERNEL_EXEC_IMAGE_STATUS_CLEANUP_REQUIRED
                       : KERNEL_EXEC_IMAGE_STATUS_STATE;
        }
    }
    return KERNEL_EXEC_IMAGE_STATUS_OK;
}
