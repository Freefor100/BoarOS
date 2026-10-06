#include <kernel/elf_image.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
static struct physical_page_allocator *exec_allocator;
static const struct arch_mmu_page_table *exec_kernel_table;
int kernel_exec_image_bind(struct physical_page_allocator *allocator,
                           const struct arch_mmu_page_table *table)
{
    if (!allocator || !table || table->state!=ARCH_MMU_STATE_ACTIVE || table->allocator!=allocator) return 1;
    if (exec_allocator || exec_kernel_table) return 2;
    exec_allocator=allocator; exec_kernel_table=table; return 0;
}
static int64_t image_linux_error(enum kernel_elf_image_status status)
{
    switch (status) {
    case KERNEL_ELF_IMAGE_STATUS_NO_MEMORY:
        return -KERNEL_ENOMEM;
    case KERNEL_ELF_IMAGE_STATUS_IO:
        return -KERNEL_EIO;
    case KERNEL_ELF_IMAGE_STATUS_STACK_LIMIT:
        return -KERNEL_E2BIG;
    case KERNEL_ELF_IMAGE_STATUS_MALFORMED:
    case KERNEL_ELF_IMAGE_STATUS_WRONG_ARCH:
        return -KERNEL_ENOEXEC;
    case KERNEL_ELF_IMAGE_STATUS_OK:
    case KERNEL_ELF_IMAGE_STATUS_CLEANUP_REQUIRED:
        return 0;
    case KERNEL_ELF_IMAGE_STATUS_ADDRESS_SPACE:
        return -KERNEL_ENOMEM;
    case KERNEL_ELF_IMAGE_STATUS_INVALID_ARGUMENT:
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
    enum kernel_elf_image_status status;
    int64_t error;

    if (request == 0 || heap == 0 || image == 0 || linux_result == 0 ||
        image->mm.state != KERNEL_MM_EMPTY || image->arch_private != 0 ||
        exec_allocator == 0 || exec_kernel_table == 0 ||
        heap->page_allocator != exec_allocator) {
        return KERNEL_EXEC_IMAGE_STATUS_STATE;
    }
    status = kernel_elf_image_build(request,
                                   heap,
                                   exec_allocator,
                                   exec_kernel_table,
                                   image);
    error = image_linux_error(status);
    if (error != 0) {
        *linux_result = error;
        return KERNEL_EXEC_IMAGE_STATUS_LINUX_ERROR;
    }
    if (status == KERNEL_ELF_IMAGE_STATUS_OK) {
        *linux_result = 0;
        return KERNEL_EXEC_IMAGE_STATUS_OK;
    }
    return status == KERNEL_ELF_IMAGE_STATUS_CLEANUP_REQUIRED
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
