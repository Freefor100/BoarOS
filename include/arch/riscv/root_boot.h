#ifndef BOAROS_ARCH_RISCV_ROOT_BOOT_H
#define BOAROS_ARCH_RISCV_ROOT_BOOT_H

#include <arch/riscv/sv39.h>
#include <arch/riscv/virtio_mmio_block.h>
#include <kernel/dtb.h>
#include <kernel/files.h>
#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
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

struct riscv_root_boot {
    struct kernel_heap heap;
    struct kernel_page_cache page_cache;
    struct riscv_virtio_mmio_block device;
    struct kernel_vfs_mount mount;
    struct kernel_vfs_file cleanup_file;
    struct riscv_sv39_user_space cleanup_space;
    struct kernel_mm cleanup_mm;
    struct kernel_files cleanup_files;
    struct kernel_fs_context cleanup_fs;
    uint32_t cleanup_device_owned;
    uint64_t baseline_pages;
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
