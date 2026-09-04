#pragma once
#include <stdint.h>

// PIO LBA28 on the primary ATA bus, master drive. buf must hold count*512
// bytes. Both return 0 on success, -1 on error or timeout — nothing here
// blocks indefinitely, so a missing or wedged drive fails the call instead of
// hanging the kernel.
int ata_read_sectors(uint32_t lba, uint8_t count, void* buf);
int ata_write_sectors(uint32_t lba, uint8_t count, const void* buf);

// True if anything is answering on the primary bus at all.
int ata_present(void);
