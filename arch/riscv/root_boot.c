#include <arch/riscv/direct_map.h>
#include <arch/riscv/exec.h>
#include <arch/riscv/memory_layout.h>
#include <arch/riscv/mm.h>
#include <arch/riscv/root_boot.h>
#include <arch/riscv/user_elf.h>
#include <kernel/files.h>
#include <kernel/fs_context.h>
#include <kernel/mm.h>

#include <stddef.h>
#include <stdint.h>

#define RISCV_ROOT_BOOT_EMPTY 0U
#define RISCV_ROOT_BOOT_LIVE UINT32_C(0x524f4f54)
#define RISCV_ROOT_BOOT_FINISHED UINT32_C(0x444f4e45)

static int direct_map_heap_address(const void *pointer,
                                   uint64_t *physical_address)
{
    return riscv_direct_map_va_to_pa((uint64_t)(uintptr_t)pointer,
                                     1U,
                                     physical_address) ==
           RISCV_DIRECT_MAP_STATUS_OK;
}

static int direct_map_dma_address(const void *pointer,
                                  uint64_t size,
                                  uint64_t *physical_address)
{
    return riscv_direct_map_va_to_pa((uint64_t)(uintptr_t)pointer,
                                     size,
                                     physical_address) ==
           RISCV_DIRECT_MAP_STATUS_OK;
}

static enum riscv_root_boot_status cleanup_start_failure(
    struct riscv_root_boot *root,
    struct kernel_vfs_file *file,
    struct riscv_sv39_user_space *space,
    struct kernel_mm *mm,
    struct kernel_files *files,
    struct kernel_fs_context *fs,
    enum riscv_root_boot_status failure)
{
    int cleanup_failed = 0;

    if ((files->state == KERNEL_FILES_LIVE ||
         files->state == KERNEL_FILES_CLEANUP) &&
        kernel_files_release(files) != KERNEL_FILES_STATUS_OK) {
        cleanup_failed = 1;
    }
    if ((fs->state == KERNEL_FS_CONTEXT_LIVE ||
         fs->state == KERNEL_FS_CONTEXT_CLEANUP) &&
        kernel_fs_context_release(fs) !=
            KERNEL_FS_CONTEXT_STATUS_OK) {
        cleanup_failed = 1;
    }
    if (file->private_data != 0 && kernel_vfs_close(file) != 0) {
        cleanup_failed = 1;
    }
    if ((mm->state == KERNEL_MM_LIVE ||
         mm->state == KERNEL_MM_CLEANUP) &&
        kernel_mm_release(mm) != KERNEL_MM_STATUS_OK) {
        cleanup_failed = 1;
    }
    if ((space->state == RISCV_SV39_USER_SPACE_LIVE ||
         space->state == RISCV_SV39_USER_SPACE_CLEANUP) &&
        riscv_sv39_user_space_destroy(space) != RISCV_SV39_STATUS_OK) {
        cleanup_failed = 1;
    }
    if (root->mount.private_data != 0 &&
        kernel_vfs_unmount(&root->mount) != 0) {
        cleanup_failed = 1;
    }
    if (root->device.page_allocator != 0 &&
        riscv_virtio_mmio_block_destroy(&root->device) !=
            RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK) {
        cleanup_failed = 1;
    }
    return cleanup_failed ? RISCV_ROOT_BOOT_STATUS_CLEANUP : failure;
}

enum riscv_root_boot_status riscv_root_boot_start(
    struct riscv_root_boot *root,
    const struct dtb_boot_info *info,
    struct physical_page_allocator *allocator,
    const struct riscv_sv39_page_table *kernel_table)
{
    static const char init_path[] = "/init";
    const struct kernel_exec_string arguments[1] = {
        {
            .bytes = init_path,
            .length = sizeof(init_path) - 1U,
        },
    };
    struct kernel_vfs_file file = {0};
    struct riscv_sv39_user_space space = {0};
    struct kernel_mm mm = {0};
    struct kernel_files files = {0};
    struct kernel_fs_context fs = {0};
    struct riscv_user_elf_request request = {0};
    struct riscv_user_elf_entry entry;
    enum riscv_virtio_mmio_block_status device_status;
    enum riscv_user_elf_status elf_status;
    enum kernel_mm_status mm_status;
    enum kernel_scheduler_status scheduler_status;
    enum riscv_root_boot_status failure;
    uint64_t mmio_address;
    uint32_t index;
    int found = 0;

    if (root == 0 || info == 0 || allocator == 0 ||
        kernel_table == 0 || root->state != RISCV_ROOT_BOOT_EMPTY ||
        root->device.page_allocator != 0 ||
        root->mount.private_data != 0 || info->timebase_frequency == 0U) {
        return RISCV_ROOT_BOOT_STATUS_INVALID;
    }
    if (kernel_heap_init(&root->heap,
                         allocator,
                         direct_map_heap_address) !=
        KERNEL_HEAP_STATUS_OK) {
        return RISCV_ROOT_BOOT_STATUS_HEAP;
    }
    if (riscv_exec_init(allocator, kernel_table) !=
        RISCV_EXEC_STATUS_OK) {
        return RISCV_ROOT_BOOT_STATUS_INIT;
    }
    root->baseline_pages = physical_page_available(allocator);

    for (index = 0U; index < info->virtio_mmio_count; index++) {
        if (info->virtio_mmio[index].base >
            UINT64_MAX - RISCV_KERNEL_MMIO_BASE) {
            return RISCV_ROOT_BOOT_STATUS_DEVICE;
        }
        mmio_address = RISCV_KERNEL_MMIO_BASE +
                       info->virtio_mmio[index].base;
        device_status = riscv_virtio_mmio_block_init(
            &root->device,
            (volatile void *)(uintptr_t)mmio_address,
            info->virtio_mmio[index].size,
            allocator,
            direct_map_dma_address,
            info->timebase_frequency);
        if (device_status == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK) {
            found = 1;
            break;
        }
        if (device_status != RISCV_VIRTIO_MMIO_BLOCK_STATUS_NOT_BLOCK) {
            return RISCV_ROOT_BOOT_STATUS_DEVICE;
        }
    }
    if (!found) {
        return RISCV_ROOT_BOOT_STATUS_NO_DEVICE;
    }

    if (kernel_vfs_mount_root_readonly(&root->mount,
                                       &root->device.block,
                                       &root->heap) != 0) {
        failure = RISCV_ROOT_BOOT_STATUS_MOUNT;
        goto fail;
    }
    if (kernel_vfs_open_executable(&root->mount, init_path, &file) != 0 ||
        kernel_vfs_file_read_source(&file, &request.source) != 0) {
        failure = RISCV_ROOT_BOOT_STATUS_INIT;
        goto fail;
    }
    request.arguments = arguments;
    request.argument_count = 1U;
    request.environment = 0;
    request.environment_count = 0U;
    request.executable = arguments[0];
    elf_status = riscv_user_elf_load(&request,
                                     allocator,
                                     kernel_table,
                                     &space,
                                     &entry);
    if (elf_status != RISCV_USER_ELF_STATUS_OK) {
        failure = elf_status == RISCV_USER_ELF_STATUS_IO
                      ? RISCV_ROOT_BOOT_STATUS_INIT
                      : RISCV_ROOT_BOOT_STATUS_ELF;
        goto fail;
    }
    if (kernel_vfs_close(&file) != 0) {
        failure = RISCV_ROOT_BOOT_STATUS_CLEANUP;
        goto fail;
    }

    mm_status = riscv_kernel_mm_create(&mm, &space);
    if (mm_status != KERNEL_MM_STATUS_OK) {
        failure = RISCV_ROOT_BOOT_STATUS_ADDRESS_SPACE;
        goto fail;
    }
    if (kernel_fs_context_create(&fs,
                                 &root->mount,
                                 &root->heap) !=
        KERNEL_FS_CONTEXT_STATUS_OK) {
        failure = RISCV_ROOT_BOOT_STATUS_RESOURCES;
        goto fail;
    }
    if (kernel_files_create(&files, &root->heap) !=
        KERNEL_FILES_STATUS_OK) {
        failure = RISCV_ROOT_BOOT_STATUS_RESOURCES;
        goto fail;
    }
    scheduler_status = kernel_user_thread_create(&mm,
                                                 &files,
                                                 &fs,
                                                 entry.entry,
                                                 entry.stack_pointer,
                                                 0U);
    if (scheduler_status != KERNEL_SCHEDULER_STATUS_OK) {
        failure = RISCV_ROOT_BOOT_STATUS_SCHEDULER;
        goto fail;
    }

    root->state = RISCV_ROOT_BOOT_LIVE;
    return RISCV_ROOT_BOOT_STATUS_OK;

fail:
    return cleanup_start_failure(root,
                                 &file,
                                 &space,
                                 &mm,
                                 &files,
                                 &fs,
                                 failure);
}

enum riscv_root_boot_status riscv_root_boot_finish(
    struct riscv_root_boot *root,
    const struct kernel_thread_completion *completion,
    struct kernel_heap_statistics *heap_statistics,
    uint64_t *available_pages)
{
    if (root == 0 || completion == 0 || heap_statistics == 0 ||
        available_pages == 0 || root->state != RISCV_ROOT_BOOT_LIVE ||
        completion->kind != KERNEL_THREAD_KIND_USER ||
        completion->tid != 1 || completion->tgid != 1) {
        return RISCV_ROOT_BOOT_STATUS_INVALID;
    }
    if (kernel_vfs_unmount(&root->mount) != 0 ||
        riscv_virtio_mmio_block_destroy(&root->device) !=
            RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK) {
        return RISCV_ROOT_BOOT_STATUS_CLEANUP;
    }

    kernel_heap_get_statistics(&root->heap, heap_statistics);
    *available_pages = physical_page_available(root->heap.page_allocator);
    if (heap_statistics->live_allocations != 0U ||
        heap_statistics->current_pages != 0U ||
        *available_pages != root->baseline_pages) {
        return RISCV_ROOT_BOOT_STATUS_CLEANUP;
    }
    root->state = RISCV_ROOT_BOOT_FINISHED;
    return RISCV_ROOT_BOOT_STATUS_OK;
}
