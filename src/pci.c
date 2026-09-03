// src/pci.c — PCI configuration space via the 0xCF8/0xCFC port pair
#include <stdint.h>
#include "header/pci.h"
#include "header/io.h"
#include "header/kprintf.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static uint32_t cfg_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off){
    // Bit 31 enables the config cycle; the register offset must be
    // dword-aligned, so the low two bits are always zero.
    return 0x80000000u
         | ((uint32_t)bus  << 16)
         | ((uint32_t)slot << 11)
         | ((uint32_t)func << 8)
         | ((uint32_t)off & 0xFCu);
}

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off){
    outl(PCI_CONFIG_ADDRESS, cfg_addr(bus, slot, func, off));
    return inl(PCI_CONFIG_DATA);
}

void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t val){
    outl(PCI_CONFIG_ADDRESS, cfg_addr(bus, slot, func, off));
    outl(PCI_CONFIG_DATA, val);
}

int pci_find(uint16_t vendor_id, uint16_t device_id, pci_device_t* out){
    for (uint16_t bus = 0; bus < 256; bus++){
        for (uint8_t slot = 0; slot < 32; slot++){
            for (uint8_t func = 0; func < 8; func++){
                uint32_t id = pci_read32((uint8_t)bus, slot, func, 0x00);
                uint16_t ven = (uint16_t)(id & 0xFFFF);
                uint16_t dev = (uint16_t)(id >> 16);
                if (ven == 0xFFFF) continue;            // no device here
                if (ven != vendor_id || dev != device_id) continue;

                out->bus = (uint8_t)bus; out->slot = slot; out->func = func;
                out->vendor_id = ven;    out->device_id = dev;
                for (int b = 0; b < 6; b++)
                    out->bar[b] = pci_read32((uint8_t)bus, slot, func, (uint8_t)(0x10 + b * 4));
                out->irq_line = (uint8_t)(pci_read32((uint8_t)bus, slot, func, 0x3C) & 0xFF);
                return 0;
            }
        }
    }
    return -1;
}

void pci_enable_bus_master(const pci_device_t* dev){
    uint32_t cmd = pci_read32(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= (1u << 2) | (1u << 0);      // bus master + I/O space
    pci_write32(dev->bus, dev->slot, dev->func, 0x04, cmd);
}

void pci_dump(void){
    for (uint16_t bus = 0; bus < 4; bus++){
        for (uint8_t slot = 0; slot < 32; slot++){
            uint32_t id = pci_read32((uint8_t)bus, slot, 0, 0x00);
            if ((id & 0xFFFF) == 0xFFFF) continue;
            uint32_t cls = pci_read32((uint8_t)bus, slot, 0, 0x08);
            kprintf("[pci] %u:%u vendor=%x device=%x class=%x\n",
                    (unsigned)bus, (unsigned)slot,
                    (unsigned)(id & 0xFFFF), (unsigned)(id >> 16), (unsigned)(cls >> 16));
        }
    }
}
