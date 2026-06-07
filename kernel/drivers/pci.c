#include "drivers/pci.h"

#include <stdint.h>

/* PCI configuration space via I/O ports 0xCF8 (address) / 0xCFC (data). */

static inline void outl(uint16_t port, uint32_t val)
{
    __asm__ volatile("outl %0, %1" :: "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t val;
    __asm__ volatile("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outw(uint16_t port, uint16_t val)
{
    __asm__ volatile("outw %0, %1" :: "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t val;
    __asm__ volatile("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static uint32_t cfg_addr(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset)
{
    return (1U << 31)
         | ((uint32_t)bus  << 16)
         | ((uint32_t)dev  << 11)
         | ((uint32_t)func << 8)
         | (offset & 0xfcU);
}

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset)
{
    outl(0xcf8, cfg_addr(bus, dev, func, offset));
    return inl(0xcfc);
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t value)
{
    outl(0xcf8, cfg_addr(bus, dev, func, offset));
    outl(0xcfc, value);
}

uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset)
{
    outl(0xcf8, cfg_addr(bus, dev, func, offset));
    return inw((uint16_t)(0xcfc + (offset & 2U)));
}

void pci_write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint16_t value)
{
    outl(0xcf8, cfg_addr(bus, dev, func, offset));
    outw((uint16_t)(0xcfc + (offset & 2U)), value);
}

struct pci_addr pci_find_class(uint8_t class_code, uint8_t subclass)
{
    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t dev = 0; dev < 32; ++dev) {
            /* Check function 0; skip if vendor == 0xFFFF (not present). */
            uint32_t id = pci_read32((uint8_t)bus, dev, 0, 0x00);
            if ((id & 0xffff) == 0xffff) continue;

            uint8_t max_func = 1;
            uint32_t hdr = pci_read32((uint8_t)bus, dev, 0, 0x0c);
            if (hdr & 0x800000U) max_func = 8; /* multi-function device */

            for (uint8_t func = 0; func < max_func; ++func) {
                uint32_t vid = pci_read32((uint8_t)bus, dev, func, 0x00);
                if ((vid & 0xffff) == 0xffff) continue;
                uint32_t cc = pci_read32((uint8_t)bus, dev, func, 0x08);
                uint8_t  cls = (uint8_t)(cc >> 24);
                uint8_t  sub = (uint8_t)(cc >> 16);
                if (cls == class_code && sub == subclass) {
                    return (struct pci_addr){ (uint8_t)bus, dev, func, true };
                }
            }
        }
    }
    return (struct pci_addr){ 0, 0, 0, false };
}

uint64_t pci_read_bar(struct pci_addr a, uint8_t bar_index)
{
    if (bar_index > 5) return 0;
    uint8_t offset = (uint8_t)(0x10 + bar_index * 4);
    uint32_t bar_lo = pci_read32(a.bus, a.dev, a.func, offset);

    /* Memory-mapped BAR; check for 64-bit. */
    if (bar_lo & 1U) return 0; /* I/O bar, not supported here */

    uint64_t addr = bar_lo & 0xfffffff0U;
    if (((bar_lo >> 1) & 3U) == 2 && bar_index < 5) {
        /* 64-bit BAR: upper half in next register. */
        uint32_t bar_hi = pci_read32(a.bus, a.dev, a.func, (uint8_t)(offset + 4));
        addr |= (uint64_t)bar_hi << 32;
    }
    return addr;
}

void pci_enable_busmaster(struct pci_addr a)
{
    uint16_t cmd = pci_read16(a.bus, a.dev, a.func, 0x04);
    pci_write16(a.bus, a.dev, a.func, 0x04, cmd | 0x04U);
}
