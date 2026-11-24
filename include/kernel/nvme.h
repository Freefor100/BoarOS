#ifndef BOAROS_KERNEL_NVME_H
#define BOAROS_KERNEL_NVME_H

#include <stdint.h>

struct kernel_heap;
struct physical_page_allocator;
struct pci_host;
struct kernel_open_file_description;
struct kernel_mm;
struct kernel_credentials;
struct kernel_char_device;

/* The boot owner starts optional PCI controllers and stops them after user
 * descriptors and mounts have been released. */
int kernel_nvme_bus_start(struct pci_host *, struct kernel_heap *,
                          struct physical_page_allocator *, uint32_t timebase);
int kernel_nvme_bus_stop(void);

int kernel_nvme_uring_command(struct kernel_open_file_description *,
    struct kernel_mm *, const struct kernel_credentials *, uint32_t cmd_op,
    const void *payload, uint64_t *result);

const struct kernel_char_device *kernel_nvme_char_device_lookup(uint64_t rdev);
int kernel_nvme_block_name(uint64_t number, char name[32]);
int kernel_nvme_visit_command_nodes(int (*)(void *, const char *, uint64_t), void *);

#endif
