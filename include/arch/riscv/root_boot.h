#ifndef BOAROS_ARCH_RISCV_ROOT_BOOT_H
#define BOAROS_ARCH_RISCV_ROOT_BOOT_H

#include <arch/riscv/sv39.h>
#include <arch/riscv/virtio_mmio_block.h>
#include <kernel/dtb.h>
#include <kernel/elf64_source.h>
#include <kernel/exec_image.h>
#include <kernel/files.h>
#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/open_file.h>
#include <kernel/page_cache.h>
#include <kernel/physical_page.h>
#include <kernel/scheduler.h>
#include <kernel/vfs.h>

#include <stdint.h>

enum riscv_root_boot_status {
    RISCV_ROOT_BOOT_STATUS_OK = 0,
    RISCV_ROOT_BOOT_STATUS_NO_DEVICE,
    RISCV_ROOT_BOOT_STATUS_INVALID,
    RISCV_ROOT_BOOT_STATUS_HEAP,
    RISCV_ROOT_BOOT_STATUS_DEVICE,
    RISCV_ROOT_BOOT_STATUS_MOUNT,
    RISCV_ROOT_BOOT_STATUS_INIT,
    RISCV_ROOT_BOOT_STATUS_ELF,
    RISCV_ROOT_BOOT_STATUS_ADDRESS_SPACE,
    RISCV_ROOT_BOOT_STATUS_RESOURCES,
    RISCV_ROOT_BOOT_STATUS_SCHEDULER,
    RISCV_ROOT_BOOT_STATUS_CLEANUP,
};

enum riscv_root_boot_state {
    RISCV_ROOT_BOOT_EMPTY = 0,
    RISCV_ROOT_BOOT_LIVE,
    RISCV_ROOT_BOOT_CLEANUP,
    RISCV_ROOT_BOOT_FINISHED,
    RISCV_ROOT_BOOT_FAILED,
};

/* Identifies the first failing owner during normal PID 1 teardown.  Source
 * releases can both fail, so those two bits may be present together. */
enum riscv_root_finish_failure {
    RISCV_ROOT_FINISH_NONE = 0,
    RISCV_ROOT_FINISH_INTERPRETER_SOURCE = 1U << 0,
    RISCV_ROOT_FINISH_EXECUTABLE_SOURCE = 1U << 1,
    RISCV_ROOT_FINISH_UNMOUNT = 1U << 2,
    RISCV_ROOT_FINISH_PAGE_CACHE = 1U << 3,
    RISCV_ROOT_FINISH_DEVICE = 1U << 4,
    RISCV_ROOT_FINISH_HEAP_BASELINE = 1U << 5,
    RISCV_ROOT_FINISH_PAGE_BASELINE = 1U << 6,
};

struct riscv_root_boot {
    struct kernel_heap heap;
    struct kernel_page_cache page_cache;
    struct riscv_virtio_mmio_block device;
    struct kernel_vfs_mount mount;
    struct kernel_exec_image cleanup_image;
    struct kernel_elf64_source *cleanup_executable_source;
    struct kernel_elf64_source *cleanup_interpreter_source;
    struct kernel_open_file_description *cleanup_file_owner;
    struct kernel_open_file_description *cleanup_interpreter_file_owner;
    char *cleanup_path;
    struct kernel_files cleanup_files;
    struct kernel_fs_context cleanup_fs;
    uint32_t cleanup_device_owned;
    uint64_t baseline_pages;
    uint32_t finish_failure;
    int32_t finish_error;
    enum riscv_root_boot_status failure_status;
    enum riscv_root_boot_state state;
};

/* Scheduler initialization must precede this call. */
enum riscv_root_boot_status riscv_root_boot_start(
    struct riscv_root_boot *root,
    const struct dtb_boot_info *info,
    struct physical_page_allocator *allocator,
    const struct riscv_sv39_page_table *kernel_table);

/*
 * Retry cleanup after start returned CLEANUP.  A successful final retry returns
 * the original start failure and transitions root to FAILED.
 */
enum riscv_root_boot_status riscv_root_boot_cleanup(
    struct riscv_root_boot *root);

/* Call only after the scheduler has completely reaped PID 1. */
enum riscv_root_boot_status riscv_root_boot_finish(
    struct riscv_root_boot *root,
    const struct kernel_thread_completion *completion,
    struct kernel_heap_statistics *heap_statistics,
    uint64_t *available_pages);

#endif
