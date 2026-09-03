#pragma once
#include <stdint.h>

// PIO LBA28 read from the primary ATA bus, master drive. buf must hold
// count*512 bytes. Returns 0 on success, -1 on error.
int ata_read_sectors(uint32_t lba, uint8_t count, void* buf);
