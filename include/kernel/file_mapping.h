#ifndef BOAROS_KERNEL_FILE_MAPPING_H
#define BOAROS_KERNEL_FILE_MAPPING_H

#include <stdint.h>

struct kernel_vfs_node;
struct kernel_lock_guard;
void kernel_vfs_node_lock(struct kernel_vfs_node *node, struct kernel_lock_guard *guard, int write);

/* MM owns the record; its source OFD keeps the inode alive. stop unlinks and
 * rejects new borrows; drain must run outside MM gate, inode I/O and raw locks.
 * The record and MM storage remain owned until drain returns. notify borrows a
 * cursor, drops metadata protection, then synchronously invokes the callback;
 * Controlled memory-file notification runs outside inode I/O; its callback may
 * acquire one MM gate and sleep. Legacy disk callbacks retain their single-CPU
 * inode-I/O contract and cannot acquire a concurrent MM gate. */
struct kernel_file_mapping {
    struct kernel_file_mapping *next;
    struct kernel_file_mapping **previous;
    struct kernel_vfs_node *node;
    void *owner;
    void (*truncate)(void *owner, struct kernel_vfs_node *node, uint64_t size);
    struct kernel_file_mapping *retired_next;
    uint32_t borrows;
};

void kernel_file_mapping_init(struct kernel_vfs_node *node);
/* First mapping privately prepares inode-owned metadata before VMA publication;
 * callers retain an inode/OFD owner. Published state lives until inode destroy.
 * prepare may allocate/sleep under inode read protection, never under raw. */
int kernel_file_mapping_prepare(struct kernel_vfs_node *node);
void kernel_file_mapping_destroy(struct kernel_vfs_node *node);
int kernel_file_mapping_empty(struct kernel_vfs_node *node);
int kernel_file_mapping_register(struct kernel_file_mapping *mapping);
void kernel_file_mapping_stop(struct kernel_file_mapping *mapping);
void kernel_file_mapping_drain(struct kernel_file_mapping *mapping);
void kernel_file_mapping_unregister(struct kernel_file_mapping *mapping);
void kernel_file_mapping_notify(struct kernel_vfs_node *node, uint64_t size);
/* I/O write owner serializes close/open; read owner serializes final fault
 * validation with close. An OFD owner covers waiting on the epoch queue. */
void kernel_file_mapping_close(struct kernel_vfs_node *node);
void kernel_file_mapping_open(struct kernel_vfs_node *node);
int kernel_file_mapping_epoch(struct kernel_vfs_node *node, uint64_t *epoch);
void kernel_file_mapping_wait(struct kernel_vfs_node *node);

#endif
