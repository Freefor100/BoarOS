#include "init-config.h"
#include <arch/riscv/direct_map.h>
#include <arch/riscv/elf_image.h>
#include <arch/riscv/exec.h>
#include <arch/riscv/memory_layout.h>
#include <arch/riscv/mm.h>
#include <arch/riscv/root_boot.h>
#include <arch/riscv/virt_uart.h>
#include <kernel/elf64_source.h>
#include <kernel/exec_image.h>
#include <kernel/files.h>
#include <kernel/fs_context.h>
#include <kernel/mm.h>
#include <kernel/open_file.h>
#include <kernel/procfs.h>
#include <kernel/shm.h>

#include <stddef.h>
#include <stdint.h>

int riscv_root_boot_start_rng(struct riscv_root_boot *root,
    const struct dtb_boot_info *info, const struct dtb_irq_info *irq)
{
    for (uint32_t i = 0; i < info->virtio_mmio_count; i++) {
        uint32_t source = 0;
        for (uint32_t j = 0; j < irq->route_count; j++)
            if (irq->routes[j].base == info->virtio_mmio[i].base)
                source = irq->routes[j].source;
        if (!source) continue;
        int error = riscv_virtio_mmio_rng_start(&root->rng,
            (void *)(uintptr_t)(RISCV_KERNEL_MMIO_BASE + info->virtio_mmio[i].base),
            info->virtio_mmio[i].size, root->device.page_allocator,
            info->timebase_frequency, source);
        if (!error) return 0;
        if (root->rng.mmio) return error;
    }
    return 0;
}

struct riscv_virtio_mmio_block *riscv_root_boot_device(
    struct riscv_root_boot *root, uint32_t index)
{
    if (!root || index >= DTB_MAX_VIRTIO_MMIO_RANGES) return 0;
    return index == 0U ? &root->device : &root->extra_devices[index - 1U];
}

static int destroy_devices(struct riscv_root_boot *root)
{
    /* 先移除登记再销毁 DMA；有 mount claim 的设备必须保留。 */
    while (root->device_count) {
        struct riscv_virtio_mmio_block *device =
            riscv_root_boot_device(root, root->device_count - 1U);
        if (device->block.registered && kernel_block_unregister(&device->block))
            return RISCV_VIRTIO_MMIO_BLOCK_STATUS_STATE;
        int error = (int)riscv_virtio_mmio_block_destroy(device);
        if (error != RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK) return error;
        root->device_count--;
    }
    root->cleanup_device_owned = 0U;
    return RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK;
}

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
    root->cleanup_device_owned = root->device_count != 0U;
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
    if (riscv_virtio_mmio_rng_stop(&root->rng))
        return RISCV_ROOT_BOOT_STATUS_CLEANUP;
    if (kernel_network_stop(&root->network))
        return RISCV_ROOT_BOOT_STATUS_CLEANUP;
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
    if (root->mount.private_data != 0) {
        if (kernel_vfs_disk_cleanup_pending() != 0 ||
            kernel_procfs_unmount_children(&root->mount) != 0 ||
            kernel_vfs_unmount(&root->mount) != 0) {
            cleanup_failed = 1;
        }
    }
    /* The cache and block device remain owners of a live mount's I/O state. */
    if (root->mount.private_data == 0) {
        if ((root->page_cache.state == KERNEL_PAGE_CACHE_LIVE ||
             root->page_cache.state == KERNEL_PAGE_CACHE_CLEANUP) &&
            kernel_page_cache_destroy(&root->page_cache) !=
                KERNEL_PAGE_CACHE_STATUS_OK) {
            cleanup_failed = 1;
        }
        if (root->cleanup_device_owned != 0U) {
            if (destroy_devices(root) !=
                RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK) {
                cleanup_failed = 1;
            } else {
                root->cleanup_device_owned = 0U;
            }
        }
    }
    if (riscv_uart_tty_stop(&root->uart)) cleanup_failed = 1;
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

enum riscv_root_boot_status riscv_root_boot_start_with_irq(
    struct riscv_root_boot *root,
    const struct dtb_boot_info *info,
    const struct dtb_irq_info *irq,
    struct physical_page_allocator *allocator,
    const struct riscv_sv39_page_table *kernel_table)
{
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

    if (root == 0 || info == 0 || allocator == 0 ||
        kernel_table == 0 || root->state != RISCV_ROOT_BOOT_EMPTY || root->uart != 0 ||
        root->device.page_allocator != 0 || root->device_count != 0U ||
        info->virtio_mmio_count > DTB_MAX_VIRTIO_MMIO_RANGES ||
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
    if (kernel_shm_init(&root->heap, allocator) != KERNEL_SHM_STATUS_OK) {
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
            failure = RISCV_ROOT_BOOT_STATUS_DEVICE;
            goto fail;
        }
        mmio_address = RISCV_KERNEL_MMIO_BASE +
                       info->virtio_mmio[index].base;
        device_status = riscv_virtio_mmio_block_init(
            riscv_root_boot_device(root, root->device_count),
            (volatile void *)(uintptr_t)mmio_address,
            info->virtio_mmio[index].size,
            allocator,
            direct_map_dma_address,
            info->timebase_frequency);
        if (device_status == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK) {
            struct riscv_virtio_mmio_block *device =
                riscv_root_boot_device(root, root->device_count++);
            root->cleanup_device_owned = 1U;
            if (kernel_block_register(&device->block,
                    KERNEL_BLOCK_DEVICE_NUMBER(root->device_count - 1U)) != 0) {
                failure = RISCV_ROOT_BOOT_STATUS_DEVICE;
                goto fail;
            }
            continue;
        }
        if (device_status != RISCV_VIRTIO_MMIO_BLOCK_STATUS_NOT_BLOCK) {
            failure = RISCV_ROOT_BOOT_STATUS_DEVICE;
            goto fail;
        }
    }
    if (root->device_count == 0U) {
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
                kernel_elf64_source_create_interpreter(&root->heap,
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
    request.executable = (struct kernel_exec_string){init_path, sizeof(init_path) - 1U};
    request.arguments = init_arguments;
    request.argument_count = INIT_ARGUMENTS_COUNT;
    request.environment = init_environment;
    request.environment_count = INIT_ENVIRONMENT_COUNT;
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
    if (irq && irq->uart.registers.size) {
        if (irq->uart.registers.base > UINT64_MAX-RISCV_KERNEL_MMIO_BASE ||
            riscv_uart_tty_start(&root->uart, &root->heap, &irq->uart,
                (void *)(uintptr_t)(RISCV_KERNEL_MMIO_BASE+irq->uart.registers.base),
                info->timebase_frequency)) {
            failure = RISCV_ROOT_BOOT_STATUS_RESOURCES; goto fail;
        }
    }
    /* 运行期串口TTY必须先发布，初始OFD才能保存正确的实例。 */
    for (stdio_index = 0U; stdio_index < 3U; stdio_index++) {
        if (kernel_files_open_boot_console(&files,
                                      kernel_fs_context_root(&fs),
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
    int error;

    if (root == 0 || completion == 0 || heap_statistics == 0 ||
        available_pages == 0 || root->state != RISCV_ROOT_BOOT_LIVE ||
        completion->kind != KERNEL_THREAD_KIND_USER ||
        completion->tid != 1 || completion->tgid != 1) {
        return RISCV_ROOT_BOOT_STATUS_INVALID;
    }
    root->finish_failure = RISCV_ROOT_FINISH_NONE;
    root->finish_error = 0;
    int had_network = root->network != 0;
    error = kernel_network_stop(&root->network);
    if (error) {
        root->finish_failure = RISCV_ROOT_FINISH_NETWORK;
        root->finish_error = error;
        return RISCV_ROOT_BOOT_STATUS_CLEANUP;
    }
    error = riscv_virtio_mmio_rng_stop(&root->rng);
    if (error) {
        root->finish_failure = RISCV_ROOT_FINISH_RNG;
        root->finish_error = error;
        return RISCV_ROOT_BOOT_STATUS_CLEANUP;
    }
    if (had_network) {
        uint64_t block_irqs = 0;
        for (uint32_t i = 0; i < root->device_count; i++)
            block_irqs += riscv_root_boot_device(root, i)->statistics.interrupts;
        virt_uart_puts("BoarOS: mixed IRQ rng-bytes="); virt_uart_put_hex(root->rng.bytes);
        virt_uart_puts(" rng-errors="); virt_uart_put_hex(root->rng.errors);
        virt_uart_puts(" rng-timeouts="); virt_uart_put_hex(root->rng.timeouts);
        virt_uart_puts(" block-irqs="); virt_uart_put_hex(block_irqs); virt_uart_puts("\n");
    }
    if (root->cleanup_interpreter_source != 0) {
        error = (int)kernel_elf64_source_release(
            &root->cleanup_interpreter_source);
        if (error != KERNEL_ELF64_SOURCE_STATUS_OK) {
            root->finish_failure |= RISCV_ROOT_FINISH_INTERPRETER_SOURCE;
            root->finish_error = error;
        }
    }
    if (root->cleanup_executable_source != 0) {
        error = (int)kernel_elf64_source_release(
            &root->cleanup_executable_source);
        if (error != KERNEL_ELF64_SOURCE_STATUS_OK) {
            root->finish_failure |= RISCV_ROOT_FINISH_EXECUTABLE_SOURCE;
            root->finish_error = error;
        }
    }
    if (root->finish_failure != RISCV_ROOT_FINISH_NONE) {
        return RISCV_ROOT_BOOT_STATUS_CLEANUP;
    }
    kernel_page_cache_stop_worker(&root->page_cache);
    error = kernel_vfs_disk_cleanup_pending();
    if (!error) error = kernel_procfs_unmount_children(&root->mount);
    if (error == 0) error = kernel_vfs_unmount(&root->mount);
    if (error != 0) {
        root->finish_failure = RISCV_ROOT_FINISH_UNMOUNT;
        root->finish_error = error;
        return RISCV_ROOT_BOOT_STATUS_CLEANUP;
    }
    error = (int)kernel_page_cache_destroy(&root->page_cache);
    if (error != KERNEL_PAGE_CACHE_STATUS_OK) {
        root->finish_failure = RISCV_ROOT_FINISH_PAGE_CACHE;
        root->finish_error = error;
        return RISCV_ROOT_BOOT_STATUS_CLEANUP;
    }
    error = (int)destroy_devices(root);
    if (error != RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK) {
        root->finish_failure = RISCV_ROOT_FINISH_DEVICE;
        root->finish_error = error;
        return RISCV_ROOT_BOOT_STATUS_CLEANUP;
    }

    error = riscv_uart_tty_stop_report(&root->uart, &root->uart_statistics);
    if (error) {
        root->finish_failure = RISCV_ROOT_FINISH_UART;
        root->finish_error = error;
        return RISCV_ROOT_BOOT_STATUS_CLEANUP;
    }
    kernel_heap_get_statistics(&root->heap, heap_statistics);
    *available_pages = physical_page_available(root->heap.page_allocator);
    if (heap_statistics->live_allocations != 0U ||
        heap_statistics->current_pages != 0U) {
        root->finish_failure |= RISCV_ROOT_FINISH_HEAP_BASELINE;
    }
    if (*available_pages != root->baseline_pages) {
        root->finish_failure |= RISCV_ROOT_FINISH_PAGE_BASELINE;
    }
    if (root->finish_failure != RISCV_ROOT_FINISH_NONE) {
        return RISCV_ROOT_BOOT_STATUS_CLEANUP;
    }
    root->state = RISCV_ROOT_BOOT_FINISHED;
    return RISCV_ROOT_BOOT_STATUS_OK;
}

/* Module boot fixtures deliberately retain only the early polling console. */
enum riscv_root_boot_status riscv_root_boot_start(
    struct riscv_root_boot *root, const struct dtb_boot_info *info,
    struct physical_page_allocator *allocator,
    const struct riscv_sv39_page_table *kernel_table)
{
    return riscv_root_boot_start_with_irq(root, info, 0, allocator, kernel_table);
}
