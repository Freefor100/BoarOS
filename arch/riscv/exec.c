#include <arch/riscv/exec.h>
#include <arch/riscv/mm.h>
#include <arch/riscv/user_elf.h>
#include <kernel/errno.h>
#include <kernel/exec_image.h>
#include <kernel/heap.h>

#include <stdint.h>

struct riscv_exec_private {
    struct riscv_sv39_user_space space;
};

static struct {
    struct physical_page_allocator *allocator;
    const struct riscv_sv39_page_table *kernel_table;
} riscv_exec_context;

enum riscv_exec_status riscv_exec_init(
    struct physical_page_allocator *allocator,
    const struct riscv_sv39_page_table *kernel_table)
{
    if (allocator == 0 || kernel_table == 0 ||
        kernel_table->state != RISCV_SV39_STATE_ACTIVE ||
        kernel_table->allocator != allocator) {
        return RISCV_EXEC_STATUS_INVALID_ARGUMENT;
    }
    if (riscv_exec_context.allocator != 0 ||
        riscv_exec_context.kernel_table != 0) {
        return RISCV_EXEC_STATUS_ALREADY_INITIALIZED;
    }
    riscv_exec_context.allocator = allocator;
    riscv_exec_context.kernel_table = kernel_table;
    return RISCV_EXEC_STATUS_OK;
}

static int64_t elf_linux_error(enum riscv_user_elf_status status)
{
    switch (status) {
    case RISCV_USER_ELF_STATUS_ARGUMENT_TOO_LARGE:
        return -KERNEL_E2BIG;
    case RISCV_USER_ELF_STATUS_NO_MEMORY:
        return -KERNEL_ENOMEM;
    case RISCV_USER_ELF_STATUS_IO:
        return -KERNEL_EIO;
    case RISCV_USER_ELF_STATUS_TRUNCATED:
    case RISCV_USER_ELF_STATUS_MALFORMED:
    case RISCV_USER_ELF_STATUS_WRONG_ARCH:
    case RISCV_USER_ELF_STATUS_UNSUPPORTED:
    case RISCV_USER_ELF_STATUS_INVALID_LAYOUT:
        return -KERNEL_ENOEXEC;
    case RISCV_USER_ELF_STATUS_OK:
    case RISCV_USER_ELF_STATUS_INVALID_ARGUMENT:
    case RISCV_USER_ELF_STATUS_ADDRESS_SPACE:
    case RISCV_USER_ELF_STATUS_CLEANUP_REQUIRED:
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
    struct riscv_exec_private *private;
    struct riscv_user_elf_request elf_request;
    struct riscv_user_elf_entry entry;
    enum kernel_heap_status heap_status;
    enum kernel_mm_status mm_status;
    enum riscv_user_elf_status elf_status;
    enum riscv_user_elf_status image_failure;

    if (request == 0 || heap == 0 || image == 0 || linux_result == 0 ||
        image->mm.state != KERNEL_MM_EMPTY || image->arch_private != 0 ||
        riscv_exec_context.allocator == 0 ||
        heap->page_allocator != riscv_exec_context.allocator) {
        return KERNEL_EXEC_IMAGE_STATUS_STATE;
    }
    heap_status = kernel_heap_allocate_zeroed(heap,
                                              1U,
                                              sizeof(*private),
                                              (void **)&private);
    if (heap_status == KERNEL_HEAP_STATUS_EMPTY) {
        *linux_result = -KERNEL_ENOMEM;
        return KERNEL_EXEC_IMAGE_STATUS_LINUX_ERROR;
    }
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        return KERNEL_EXEC_IMAGE_STATUS_STATE;
    }
    image->arch_private = private;
    elf_request.source = request->source;
    elf_request.executable = request->executable;
    elf_request.arguments = request->arguments;
    elf_request.argument_count = request->argument_count;
    elf_request.environment = request->environment;
    elf_request.environment_count = request->environment_count;
    elf_status = riscv_user_elf_load_detailed(
        &elf_request,
        riscv_exec_context.allocator,
        riscv_exec_context.kernel_table,
        &private->space,
        &entry,
        &image_failure);
    if (elf_status != RISCV_USER_ELF_STATUS_OK) {
        int64_t error = elf_linux_error(image_failure);

        if (error == 0 &&
            elf_status != RISCV_USER_ELF_STATUS_CLEANUP_REQUIRED) {
            return KERNEL_EXEC_IMAGE_STATUS_STATE;
        }
        if (error == 0) {
            return KERNEL_EXEC_IMAGE_STATUS_CLEANUP_REQUIRED;
        }
        *linux_result = error;
        return KERNEL_EXEC_IMAGE_STATUS_LINUX_ERROR;
    }
    mm_status = riscv_kernel_mm_create(&image->mm, &private->space);
    if (mm_status != KERNEL_MM_STATUS_OK) {
        if (mm_status == KERNEL_MM_STATUS_NO_MEMORY) {
            *linux_result = -KERNEL_ENOMEM;
            return KERNEL_EXEC_IMAGE_STATUS_LINUX_ERROR;
        }
        return mm_status == KERNEL_MM_STATUS_CLEANUP_REQUIRED
                   ? KERNEL_EXEC_IMAGE_STATUS_CLEANUP_REQUIRED
                   : KERNEL_EXEC_IMAGE_STATUS_STATE;
    }
    image->entry = (uintptr_t)entry.entry;
    image->stack_pointer = (uintptr_t)entry.stack_pointer;
    image->thread_pointer = 0U;
    *linux_result = 0;
    return KERNEL_EXEC_IMAGE_STATUS_OK;
}

enum kernel_exec_image_status kernel_exec_image_cleanup(
    struct kernel_heap *heap,
    struct kernel_exec_image *image)
{
    struct riscv_exec_private *private;
    int cleanup_required = 0;

    if (heap == 0 || image == 0) {
        return KERNEL_EXEC_IMAGE_STATUS_STATE;
    }
    if (image->mm.state == KERNEL_MM_LIVE ||
        image->mm.state == KERNEL_MM_CLEANUP) {
        if (kernel_mm_release(&image->mm) != KERNEL_MM_STATUS_OK) {
            cleanup_required = 1;
        }
    }
    private = image->arch_private;
    if (private != 0) {
        if (private->space.state == RISCV_SV39_USER_SPACE_LIVE ||
            private->space.state == RISCV_SV39_USER_SPACE_CLEANUP) {
            if (riscv_sv39_user_space_destroy(&private->space) !=
                RISCV_SV39_STATUS_OK) {
                cleanup_required = 1;
            }
        }
        if (!cleanup_required &&
            kernel_heap_release(heap, private) == KERNEL_HEAP_STATUS_OK) {
            image->arch_private = 0;
        } else if (!cleanup_required) {
            cleanup_required = 1;
        }
    }
    return cleanup_required ? KERNEL_EXEC_IMAGE_STATUS_CLEANUP_REQUIRED
                            : KERNEL_EXEC_IMAGE_STATUS_OK;
}
