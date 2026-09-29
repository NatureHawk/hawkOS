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

// ---- DMA -------------------------------------------------------------
//
// ata_read_sectors/ata_write_sectors above use PCI IDE bus-master DMA when the
// controller has it (class 01:01, BAR4) and fall back to PIO per chunk on any
// failure. The calls below pin a specific engine, for tests and benchmarks:
// the *_dma ones return -1 if there is no engine rather than falling back.
int ata_read_sectors_pio(uint32_t lba, uint8_t count, void* buf);
int ata_read_sectors_dma(uint32_t lba, uint8_t count, void* buf);
int ata_write_sectors_dma(uint32_t lba, uint8_t count, const void* buf);

int  ata_dma_available(void);        // controller found and not written off
int  ata_dma_enabled(void);          // available and not switched off
void ata_dma_enable(int on);         // `ata dma on|off`

typedef struct { uint32_t dma_ops, pio_ops, fallbacks, errors; } ata_stats_t;
void ata_stats(ata_stats_t* out);
