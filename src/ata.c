// src/ata.c — ATA PIO driver, primary bus, master drive, LBA28
#include <stdint.h>
#include "header/ata.h"
#include "header/io.h"

#define ATA_DATA       0x1F0
#define ATA_SECCOUNT   0x1F2
#define ATA_LBA_LO     0x1F3
#define ATA_LBA_MID    0x1F4
#define ATA_LBA_HI     0x1F5
#define ATA_DRIVE_HEAD 0x1F6
#define ATA_STATUS     0x1F7
#define ATA_COMMAND    0x1F7

#define ATA_CMD_READ_PIO 0x20

#define STATUS_BSY 0x80
#define STATUS_DRQ 0x08
#define STATUS_ERR 0x01

static void wait_bsy_clear(void){
    while (inb(ATA_STATUS) & STATUS_BSY) { }
}

static int wait_drq(void){
    for (;;){
        uint8_t st = inb(ATA_STATUS);
        if (st & STATUS_ERR) return -1;
        if (st & STATUS_DRQ) return 0;
    }
}

int ata_read_sectors(uint32_t lba, uint8_t count, void* buf){
    uint16_t* out = (uint16_t*)buf;
    wait_bsy_clear();

    outb(ATA_DRIVE_HEAD, (uint8_t)(0xE0 | ((lba >> 24) & 0x0Fu)));
    outb(ATA_SECCOUNT, count);
    outb(ATA_LBA_LO,  (uint8_t)(lba & 0xFFu));
    outb(ATA_LBA_MID, (uint8_t)((lba >> 8) & 0xFFu));
    outb(ATA_LBA_HI,  (uint8_t)((lba >> 16) & 0xFFu));
    outb(ATA_COMMAND, ATA_CMD_READ_PIO);

    for (uint32_t s = 0; s < count; s++){
        wait_bsy_clear();
        if (wait_drq() != 0) return -1;
        for (int i = 0; i < 256; i++)
            out[s * 256u + (uint32_t)i] = inw(ATA_DATA);
    }
    return 0;
}
