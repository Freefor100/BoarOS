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
    root->cleanup_file = *file;
    root->cleanup_space = *space;
    root->cleanup_mm = *mm;
    root->cleanup_files = *files;
    root->cleanup_fs = *fs;
    root->cleanup_device_owned = root->device.page_allocator != 0;
    root->failure_status = failure;
    root->state = RISCV_ROOT_BOOT_CLEANUP;
    return riscv_root_boot_cleanup(root);
}

enum riscv_root_boot_status riscv_root_boot_cleanup(
    struct riscv_root_boot *root)
{
    struct kernel_heap_statistics statistics;
    uint64_t available_pages;
    int cleanup_failed = 0;

    if (root == 0 || root->state != RISCV_ROOT_BOOT_CLEANUP) {
        return RISCV_ROOT_BOOT_STATUS_INVALID;
    }
    if ((root->cleanup_files.state == KERNEL_FILES_LIVE ||
         root->cleanup_files.state == KERNEL_FILES_CLEANUP) &&
        kernel_files_release(&root->cleanup_files) !=
            KERNEL_FILES_STATUS_OK) {
        cleanup_failed = 1;
    }
    if ((root->cleanup_fs.state == KERNEL_FS_CONTEXT_LIVE ||
         root->cleanup_fs.state == KERNEL_FS_CONTEXT_CLEANUP) &&
        kernel_fs_context_release(&root->cleanup_fs) !=
            KERNEL_FS_CONTEXT_STATUS_OK) {
        cleanup_failed = 1;
    }
    if (root->cleanup_file.private_data != 0 &&
        kernel_vfs_close(&root->cleanup_file) != 0) {
        cleanup_failed = 1;
    }
    if ((root->cleanup_mm.state == KERNEL_MM_LIVE ||
         root->cleanup_mm.state == KERNEL_MM_CLEANUP) &&
        kernel_mm_release(&root->cleanup_mm) != KERNEL_MM_STATUS_OK) {
        cleanup_failed = 1;
    }
    if ((root->cleanup_space.state == RISCV_SV39_USER_SPACE_LIVE ||
         root->cleanup_space.state == RISCV_SV39_USER_SPACE_CLEANUP) &&
        riscv_sv39_user_space_destroy(&root->cleanup_space) !=
            RISCV_SV39_STATUS_OK) {
        cleanup_failed = 1;
    }
    if (root->mount.private_data != 0 &&
        kernel_vfs_unmount(&root->mount) != 0) {
        cleanup_failed = 1;
    }
    if ((root->page_cache.state == KERNEL_PAGE_CACHE_LIVE ||
         root->page_cache.state == KERNEL_PAGE_CACHE_CLEANUP) &&
        kernel_page_cache_destroy(&root->page_cache) !=
            KERNEL_PAGE_CACHE_STATUS_OK) {
        cleanup_failed = 1;
    }
    if (root->cleanup_device_owned != 0U) {
        if (riscv_virtio_mmio_block_destroy(&root->device) !=
            RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK) {
            cleanup_failed = 1;
        } else {
            root->cleanup_device_owned = 0U;
        }
    }
    if (root->heap.page_allocator == 0) {
        cleanup_failed = 1;
    } else {
        kernel_heap_get_statistics(&root->heap, &statistics);
        available_pages = physical_page_available(root->heap.page_allocator);
        if (statistics.live_allocations != 0U ||
            statistics.current_pages != 0U ||
            available_pages != root->baseline_pages) {
            cleanup_failed = 1;
        }
    }
    if (cleanup_failed) {
        return RISCV_ROOT_BOOT_STATUS_CLEANUP;
    }
    root->state = RISCV_ROOT_BOOT_FAILED;
    return root->failure_status;
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
    uint32_t stdio_index;
    int64_t console_result;
    int found = 0;

    if (root == 0 || info == 0 || allocator == 0 ||
        kernel_table == 0 || root->state != RISCV_ROOT_BOOT_EMPTY ||
        root->device.page_allocator != 0 ||
        root->mount.private_data != 0 ||
        root->page_cache.state != KERNEL_PAGE_CACHE_EMPTY ||
        root->page_cache.record != 0 || info->timebase_frequency == 0U) {
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

    if (kernel_page_cache_init(&root->page_cache,
                               &root->heap,
                               allocator) !=
        KERNEL_PAGE_CACHE_STATUS_OK) {
        failure = RISCV_ROOT_BOOT_STATUS_RESOURCES;
        goto fail;
    }

    if (kernel_vfs_mount_root_readonly(&root->mount,
                                       &root->device.block,
                                       &root->heap,
                                       &root->page_cache) != 0) {
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
    mm_status = riscv_kernel_mm_create(&mm, &space);
    if (mm_status != KERNEL_MM_STATUS_OK) {
        failure = RISCV_ROOT_BOOT_STATUS_ADDRESS_SPACE;
        goto fail;
    }
    elf_status = riscv_user_elf_register_static_vmas(&request,
                                                      &mm,
                                                      &root->heap);
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
    /*
     * Bind PID 1 stdio to the console.  Bridge until a device filesystem
     * provides /dev/console; fork already inherits the descriptors and
     * exec only drops CLOEXEC ones.
     */
    for (stdio_index = 0U; stdio_index < 3U; stdio_index++) {
        if (kernel_files_open_console(&files,
                                      (int64_t)stdio_index,
                                      &console_result) !=
                KERNEL_FILES_STATUS_OK ||
            console_result != 0) {
            failure = RISCV_ROOT_BOOT_STATUS_RESOURCES;
            goto fail;
        }
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
        kernel_page_cache_destroy(&root->page_cache) !=
            KERNEL_PAGE_CACHE_STATUS_OK ||
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
