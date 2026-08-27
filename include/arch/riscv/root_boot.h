#ifndef BOAROS_ARCH_RISCV_ROOT_BOOT_H
#define BOAROS_ARCH_RISCV_ROOT_BOOT_H

#include <arch/riscv/sv39.h>
#include <arch/riscv/virtio_mmio_block.h>
#include <kernel/dtb.h>
#include <kernel/heap.h>
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
    RISCV_ROOT_BOOT_STATUS_SCHEDULER,
    RISCV_ROOT_BOOT_STATUS_CLEANUP,
};

struct riscv_root_boot {
    struct kernel_heap heap;
    struct riscv_virtio_mmio_block device;
    struct kernel_vfs_mount mount;
    uint64_t baseline_pages;
    uint32_t state;
};

/* Scheduler initialization must precede this call. */
enum riscv_root_boot_status riscv_root_boot_start(
    struct riscv_root_boot *root,
    const struct dtb_boot_info *info,
    struct physical_page_allocator *allocator,
    const struct riscv_sv39_page_table *kernel_table);

/* Call only after the scheduler has completely reaped PID 1. */
enum riscv_root_boot_status riscv_root_boot_finish(
    struct riscv_root_boot *root,
    const struct kernel_thread_completion *completion,
    struct kernel_heap_statistics *heap_statistics,
    uint64_t *available_pages);

#endif
