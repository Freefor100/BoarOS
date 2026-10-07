#ifndef BOAROS_PLATFORM_LOONGARCH_PCI_H
#define BOAROS_PLATFORM_LOONGARCH_PCI_H
#include <kernel/pci.h>
struct pci_host *la_virt_pci_host(void);
int la_virt_irq_initialize(void);
int la_virt_irq_register(uint32_t,void (*)(void *),void *);
void la_virt_irq_unregister(uint32_t,void *);
void la_virt_irq_dispatch(void);
int la_virt_irq_active(void);
#endif
