#pragma once
#include <stdint.h>

// Minimal PCI configuration-space access over the legacy 0xCF8/0xCFC port
// pair. Enough to find a device by vendor/device id and read the BARs and
// interrupt line out of its config header — which is all a driver for a
// single well-known NIC needs.
typedef struct {
    uint8_t  bus, slot, func;
    uint16_t vendor_id, device_id;
    uint8_t  irq_line;
    uint32_t bar[6];
} pci_device_t;

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
void     pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t val);

// Scans every bus/slot/function for the given vendor/device pair. Returns 0
// and fills `out` on a hit, -1 if the device is not present.
int  pci_find(uint16_t vendor_id, uint16_t device_id, pci_device_t* out);

// Sets the bus-master bit in the command register. A NIC that DMAs into
// host memory does nothing at all until this is on.
void pci_enable_bus_master(const pci_device_t* dev);

void pci_dump(void);
