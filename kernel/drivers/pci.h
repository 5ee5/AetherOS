#ifndef KERNEL_DRIVERS_PCI_H
#define KERNEL_DRIVERS_PCI_H

#include <stdbool.h>
#include <stdint.h>

struct pci_addr {
    uint8_t bus;
    uint8_t dev;
    uint8_t func;
    bool    valid;
};

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
void     pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t value);
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
void     pci_write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint16_t value);

/* Find the first device with the given class + subclass.  Returns addr.valid=false if not found. */
struct pci_addr pci_find_class(uint8_t class_code, uint8_t subclass);

/* Return the 64-bit BAR address for bar_index (0-5).  Returns 0 on error. */
uint64_t pci_read_bar(struct pci_addr addr, uint8_t bar_index);

/* Enable PCI bus mastering (needed for DMA). */
void pci_enable_busmaster(struct pci_addr addr);

#endif
