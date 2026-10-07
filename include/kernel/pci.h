#ifndef BOAROS_KERNEL_PCI_H
#define BOAROS_KERNEL_PCI_H
#include <stdint.h>
#include <stddef.h>

enum pci_status { PCI_OK,PCI_INVALID,PCI_NOT_PRESENT,PCI_UNSUPPORTED,PCI_MALFORMED,PCI_NO_RESOURCE,PCI_STATE };
#define PCI_MAX_CLAIMS 64U
struct pci_host {
    void *context;
    uint32_t (*read)(void *,uint16_t,uint16_t,unsigned);
    void (*write)(void *,uint16_t,uint16_t,unsigned,uint32_t);
    volatile void *(*map)(void *,uint64_t,uint64_t);
    uint32_t (*interrupt_source)(void *,uint16_t,uint8_t);
    int (*register_irq)(void *,uint32_t,void (*)(void *),void *);
    void (*unregister_irq)(void *,uint32_t,void *);
    uint64_t memory_base,memory_size;
    struct { uint64_t address,size;const void *owner; } claims[PCI_MAX_CLAIMS];
};
struct pci_bar { uint64_t address,size; };
struct pci_function {
    struct pci_host *host;
    uint16_t bdf,command;
    uint32_t identity,saved_bars[6];
    struct pci_bar bars[6];
    uint8_t header_type,interrupt_pin,assigned;
};
struct pci_cap_region { uint32_t offset,length;uint8_t bar; };
struct pci_virtio_caps {
    struct pci_cap_region common,notify,isr,device;
    uint32_t notify_multiplier;
};
enum pci_status pci_function_probe(struct pci_host *,uint16_t,struct pci_function *);
enum pci_status pci_virtio_capabilities(const struct pci_function *,struct pci_virtio_caps *);
/* Disables decode while sizing BARs; failure restores all BAR/command owners. */
enum pci_status pci_function_assign(struct pci_function *);
/* Caller must first reset device/stop DMA and unregister its interrupt owner. */
enum pci_status pci_function_restore(struct pci_function *);
unsigned pci_host_claimed(const struct pci_host *);
#endif
