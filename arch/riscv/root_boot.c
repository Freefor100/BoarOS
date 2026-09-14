#include <arch/riscv/direct_map.h>
#include <arch/riscv/elf_image.h>
#include <arch/riscv/exec.h>
#include <arch/riscv/memory_layout.h>
#include <arch/riscv/mm.h>
#include <arch/riscv/root_boot.h>
#include <kernel/elf64_source.h>
#include <kernel/exec_image.h>
#include <kernel/files.h>
#include <kernel/fs_context.h>
#include <kernel/mm.h>
#include <kernel/open_file.h>

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
    struct kernel_exec_image *image,
    struct kernel_elf64_source **executable_source,
    struct kernel_elf64_source **interpreter_source,
    struct kernel_open_file_description **file_owner,
    struct kernel_open_file_description **interpreter_file_owner,
    char **path_owner,
    struct kernel_files *files,
    struct kernel_fs_context *fs,
    enum riscv_root_boot_status failure)
{
    root->cleanup_image = *image;
    root->cleanup_executable_source = *executable_source;
    root->cleanup_interpreter_source = *interpreter_source;
    root->cleanup_file_owner = *file_owner;
    root->cleanup_interpreter_file_owner = *interpreter_file_owner;
    root->cleanup_path = *path_owner;
    root->cleanup_files = *files;
    root->cleanup_fs = *fs;
    *image = (struct kernel_exec_image){0};
    *executable_source = 0;
    *interpreter_source = 0;
    *file_owner = 0;
    *interpreter_file_owner = 0;
    *path_owner = 0;
    *files = (struct kernel_files){0};
    *fs = (struct kernel_fs_context){0};
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
    if ((root->cleanup_image.mm.state == KERNEL_MM_LIVE ||
         root->cleanup_image.mm.state == KERNEL_MM_CLEANUP) &&
        kernel_exec_image_cleanup(&root->heap,
                                  &root->cleanup_image) !=
            KERNEL_EXEC_IMAGE_STATUS_OK) {
        cleanup_failed = 1;
    }
    if (root->cleanup_interpreter_source != 0 &&
        kernel_elf64_source_release(&root->cleanup_interpreter_source) !=
            KERNEL_ELF64_SOURCE_STATUS_OK) {
        cleanup_failed = 1;
    }
    if (root->cleanup_executable_source != 0 &&
        kernel_elf64_source_release(&root->cleanup_executable_source) !=
            KERNEL_ELF64_SOURCE_STATUS_OK) {
        cleanup_failed = 1;
    }
    if (root->cleanup_file_owner != 0 &&
        kernel_open_file_release(&root->cleanup_file_owner) !=
            KERNEL_OPEN_FILE_STATUS_OK) {
        cleanup_failed = 1;
    }
    if (root->cleanup_interpreter_file_owner != 0 &&
        kernel_open_file_release(&root->cleanup_interpreter_file_owner) !=
            KERNEL_OPEN_FILE_STATUS_OK) {
        cleanup_failed = 1;
    }
    if (root->cleanup_path != 0) {
        (void)kernel_heap_release(&root->heap, root->cleanup_path);
        root->cleanup_path = 0;
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
    struct kernel_open_file_description *file_owner = 0;
    struct kernel_open_file_description *interpreter_owner = 0;
    struct kernel_elf64_source *executable_source = 0;
    struct kernel_elf64_source *interpreter_source = 0;
    struct kernel_exec_image image = {0};
    struct kernel_files files = {0};
    struct kernel_fs_context fs = {0};
    struct kernel_exec_image_request request = {0};
    enum riscv_virtio_mmio_block_status device_status;
    enum riscv_elf_image_status elf_status;
    enum kernel_open_file_status file_status;
    enum kernel_scheduler_status scheduler_status;
    enum riscv_root_boot_status failure;
    char *resolved_interpreter_path = 0;
    const char *interpreter_path;
    size_t interpreter_length = 0U;
    struct kernel_vfs_mount *resolved_mount = 0;
    int path_result;
    enum kernel_heap_status heap_status;
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

    if (kernel_vfs_mount_root(&root->mount,
                               &root->device.block,
                               &root->heap,
                               &root->page_cache) != 0) {
        failure = RISCV_ROOT_BOOT_STATUS_MOUNT;
        goto fail;
    }
    if (kernel_fs_context_create(&fs,
                                 &root->mount,
                                 &root->heap) !=
        KERNEL_FS_CONTEXT_STATUS_OK) {
        failure = RISCV_ROOT_BOOT_STATUS_RESOURCES;
        goto fail;
    }
    file_status = kernel_open_file_create_executable(&root->heap,
                                                     &root->mount,
                                                     init_path,
                                                     &file_owner,
                                                     &path_result);
    if (file_status != KERNEL_OPEN_FILE_STATUS_OK) {
        failure = file_status == KERNEL_OPEN_FILE_STATUS_NO_MEMORY
                      ? RISCV_ROOT_BOOT_STATUS_RESOURCES
                      : RISCV_ROOT_BOOT_STATUS_INIT;
        goto fail;
    }
    if (path_result != 0) {
        failure = RISCV_ROOT_BOOT_STATUS_INIT;
        goto fail;
    }
    {
        enum kernel_elf64_source_status source_status =
            kernel_elf64_source_create(&root->heap,
                                       &file_owner,
                                       BOAROS_PAGE_SIZE,
                                       KERNEL_ELF64_MACHINE_RISCV,
                                       &executable_source);

        if (source_status != KERNEL_ELF64_SOURCE_STATUS_OK) {
            failure = source_status == KERNEL_ELF64_SOURCE_STATUS_NO_MEMORY
                          ? RISCV_ROOT_BOOT_STATUS_RESOURCES
                          : source_status == KERNEL_ELF64_SOURCE_STATUS_IO
                                ? RISCV_ROOT_BOOT_STATUS_INIT
                                : RISCV_ROOT_BOOT_STATUS_ELF;
            goto fail;
        }
    }
    interpreter_path = kernel_elf64_source_interpreter(executable_source,
                                                       &interpreter_length);
    if (interpreter_path != 0) {
        heap_status = kernel_heap_allocate(&root->heap,
                                           KERNEL_FS_PATH_MAX,
                                           (void **)&resolved_interpreter_path);
        if (heap_status != KERNEL_HEAP_STATUS_OK) {
            failure = heap_status == KERNEL_HEAP_STATUS_EMPTY
                          ? RISCV_ROOT_BOOT_STATUS_RESOURCES
                          : RISCV_ROOT_BOOT_STATUS_CLEANUP;
            goto fail;
        }
        if (kernel_fs_context_resolve_kernel_path(
                &fs,
                KERNEL_FS_AT_FDCWD,
                interpreter_path,
                interpreter_length,
                resolved_interpreter_path,
                KERNEL_FS_PATH_MAX,
                &resolved_mount,
                &path_result) != KERNEL_FS_CONTEXT_STATUS_OK) {
            failure = RISCV_ROOT_BOOT_STATUS_INIT;
            goto fail;
        }
        if (path_result != 0 || resolved_mount != &root->mount) {
            failure = RISCV_ROOT_BOOT_STATUS_INIT;
            goto fail;
        }
        file_status = kernel_open_file_create_executable(
            &root->heap,
            resolved_mount,
            resolved_interpreter_path,
            &interpreter_owner,
            &path_result);
        if (file_status != KERNEL_OPEN_FILE_STATUS_OK) {
            failure = file_status == KERNEL_OPEN_FILE_STATUS_NO_MEMORY
                          ? RISCV_ROOT_BOOT_STATUS_RESOURCES
                          : RISCV_ROOT_BOOT_STATUS_INIT;
            goto fail;
        }
        if (path_result != 0) {
            failure = RISCV_ROOT_BOOT_STATUS_INIT;
            goto fail;
        }
        {
            enum kernel_elf64_source_status source_status =
                kernel_elf64_source_create(&root->heap,
                                           &interpreter_owner,
                                           BOAROS_PAGE_SIZE,
                                           KERNEL_ELF64_MACHINE_RISCV,
                                           &interpreter_source);

            if (source_status != KERNEL_ELF64_SOURCE_STATUS_OK) {
                failure = source_status ==
                                      KERNEL_ELF64_SOURCE_STATUS_NO_MEMORY
                              ? RISCV_ROOT_BOOT_STATUS_RESOURCES
                              : source_status ==
                                        KERNEL_ELF64_SOURCE_STATUS_IO
                                    ? RISCV_ROOT_BOOT_STATUS_INIT
                                    : RISCV_ROOT_BOOT_STATUS_ELF;
                goto fail;
            }
        }
        if (kernel_elf64_source_interpreter(interpreter_source, 0) != 0) {
            failure = RISCV_ROOT_BOOT_STATUS_ELF;
            goto fail;
        }
    }
    if (resolved_interpreter_path != 0) {
        (void)kernel_heap_release(&root->heap, resolved_interpreter_path);
    }
    resolved_interpreter_path = 0;
    request.executable_source = executable_source;
    request.interpreter_source = interpreter_source;
    request.executable = arguments[0];
    request.arguments = arguments;
    request.argument_count = 1U;
    request.environment = 0;
    request.environment_count = 0U;
    elf_status = riscv_elf_image_build(&request,
                                       &root->heap,
                                       allocator,
                                       kernel_table,
                                       &image);
    if (elf_status != RISCV_ELF_IMAGE_STATUS_OK) {
        failure = elf_status == RISCV_ELF_IMAGE_STATUS_NO_MEMORY
                      ? RISCV_ROOT_BOOT_STATUS_RESOURCES
                      : elf_status == RISCV_ELF_IMAGE_STATUS_IO
                            ? RISCV_ROOT_BOOT_STATUS_INIT
                            : elf_status ==
                                      RISCV_ELF_IMAGE_STATUS_ADDRESS_SPACE
                                  ? RISCV_ROOT_BOOT_STATUS_ADDRESS_SPACE
                                  : elf_status ==
                                            RISCV_ELF_IMAGE_STATUS_CLEANUP_REQUIRED
                                        ? RISCV_ROOT_BOOT_STATUS_CLEANUP
                                        : RISCV_ROOT_BOOT_STATUS_ELF;
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
    scheduler_status = kernel_user_thread_create(&image.mm,
                                                 &files,
                                                 &fs,
                                                 image.entry,
                                                 image.stack_pointer,
                                                 image.thread_pointer);
    if (scheduler_status != KERNEL_SCHEDULER_STATUS_OK) {
        failure = RISCV_ROOT_BOOT_STATUS_SCHEDULER;
        goto fail;
    }
    /* The MM now owns its source references; drop the preparation owners. */
    if (executable_source != 0 &&
        kernel_elf64_source_release(&executable_source) !=
            KERNEL_ELF64_SOURCE_STATUS_OK) {
        root->cleanup_executable_source = executable_source;
        executable_source = 0;
    }
    if (interpreter_source != 0 &&
        kernel_elf64_source_release(&interpreter_source) !=
            KERNEL_ELF64_SOURCE_STATUS_OK) {
        root->cleanup_interpreter_source = interpreter_source;
        interpreter_source = 0;
    }

    root->state = RISCV_ROOT_BOOT_LIVE;
    return RISCV_ROOT_BOOT_STATUS_OK;

fail:
    if (resolved_interpreter_path != 0) {
        (void)kernel_heap_release(&root->heap, resolved_interpreter_path);
        resolved_interpreter_path = 0;
    }
    return cleanup_start_failure(root,
                                 &image,
                                 &executable_source,
                                 &interpreter_source,
                                 &file_owner,
                                 &interpreter_owner,
                                 &resolved_interpreter_path,
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
    int cleanup_failed = 0;

    if (root == 0 || completion == 0 || heap_statistics == 0 ||
        available_pages == 0 || root->state != RISCV_ROOT_BOOT_LIVE ||
        completion->kind != KERNEL_THREAD_KIND_USER ||
        completion->tid != 1 || completion->tgid != 1) {
        return RISCV_ROOT_BOOT_STATUS_INVALID;
    }
    if (root->cleanup_interpreter_source != 0 &&
        kernel_elf64_source_release(&root->cleanup_interpreter_source) !=
            KERNEL_ELF64_SOURCE_STATUS_OK) {
        cleanup_failed = 1;
    }
    if (root->cleanup_executable_source != 0 &&
        kernel_elf64_source_release(&root->cleanup_executable_source) !=
            KERNEL_ELF64_SOURCE_STATUS_OK) {
        cleanup_failed = 1;
    }
    if (cleanup_failed) {
        return RISCV_ROOT_BOOT_STATUS_CLEANUP;
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
