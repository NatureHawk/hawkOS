// src/ata.c — ATA PIO driver, primary bus, master drive, LBA28
//
// Reads and writes are the same shape: select the drive, program the LBA,
// issue the command, then move 256 words per sector through the data port.
// Writes add a cache flush at the end, without which the drive is entitled to
// report success and still lose the sector on power loss -- and QEMU is
// entitled not to update the backing file.
//
// Every wait here is bounded. The original spun on BSY forever, which is fine
// while nothing ever fails and a hard hang the first time there is no disk
// attached; the self-test runs in exactly that configuration.
#include <stdint.h>
#include "header/ata.h"
#include "header/io.h"

#define ATA_DATA       0x1F0
#define ATA_ERROR      0x1F1
#define ATA_SECCOUNT   0x1F2
#define ATA_LBA_LO     0x1F3
#define ATA_LBA_MID    0x1F4
#define ATA_LBA_HI     0x1F5
#define ATA_DRIVE_HEAD 0x1F6
#define ATA_STATUS     0x1F7
#define ATA_COMMAND    0x1F7

#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_WRITE_PIO   0x30
#define ATA_CMD_FLUSH_CACHE 0xE7

#define STATUS_BSY 0x80
#define STATUS_DRDY 0x40
#define STATUS_DF  0x20
#define STATUS_DRQ 0x08
#define STATUS_ERR 0x01

// Generous enough that a real spinning disk servicing a seek is never
// mistaken for a dead one, small enough that a missing drive fails in well
// under a second rather than wedging the kernel.
#define ATA_SPIN 4000000u

static int wait_bsy_clear(void){
    for (uint32_t i = 0; i < ATA_SPIN; i++)
        if (!(inb(ATA_STATUS) & STATUS_BSY)) return 0;
    return -1;
}

static int wait_drq(void){
    for (uint32_t i = 0; i < ATA_SPIN; i++){
        uint8_t st = inb(ATA_STATUS);
        if (st & (STATUS_ERR | STATUS_DF)) return -1;
        if (st & STATUS_DRQ) return 0;
    }
    return -1;
}

// The spec requires a 400ns settle after selecting a drive before the status
// register means anything. Four reads of the alternate status port is the
// conventional way to spend it.
static void select_delay(void){
    for (int i = 0; i < 4; i++) (void)inb(ATA_STATUS);
}

static int issue(uint32_t lba, uint8_t count, uint8_t cmd){
    if (wait_bsy_clear() != 0) return -1;

    outb(ATA_DRIVE_HEAD, (uint8_t)(0xE0 | ((lba >> 24) & 0x0Fu)));
    select_delay();
    outb(ATA_SECCOUNT, count);
    outb(ATA_LBA_LO,  (uint8_t)(lba & 0xFFu));
    outb(ATA_LBA_MID, (uint8_t)((lba >> 8) & 0xFFu));
    outb(ATA_LBA_HI,  (uint8_t)((lba >> 16) & 0xFFu));
    outb(ATA_COMMAND, cmd);
    return 0;
}

int ata_read_sectors(uint32_t lba, uint8_t count, void* buf){
    uint16_t* out = (uint16_t*)buf;
    if (count == 0) return 0;
    if (issue(lba, count, ATA_CMD_READ_PIO) != 0) return -1;

    for (uint32_t s = 0; s < count; s++){
        if (wait_bsy_clear() != 0) return -1;
        if (wait_drq() != 0) return -1;
        for (int i = 0; i < 256; i++)
            out[s * 256u + (uint32_t)i] = inw(ATA_DATA);
    }
    return 0;
}

int ata_write_sectors(uint32_t lba, uint8_t count, const void* buf){
    const uint16_t* in = (const uint16_t*)buf;
    if (count == 0) return 0;
    if (issue(lba, count, ATA_CMD_WRITE_PIO) != 0) return -1;

    for (uint32_t s = 0; s < count; s++){
        if (wait_bsy_clear() != 0) return -1;
        if (wait_drq() != 0) return -1;
        for (int i = 0; i < 256; i++)
            outw(ATA_DATA, in[s * 256u + (uint32_t)i]);
        // A word-sized delay between sectors: some controllers need the bus
        // to settle before the next DRQ is asserted.
        select_delay();
    }

    // Without this the data can sit in the drive's write cache. QEMU in
    // particular may not push it to the backing file, which turns a
    // successful write into a file that is unchanged when the guest reboots.
    if (wait_bsy_clear() != 0) return -1;
    outb(ATA_COMMAND, ATA_CMD_FLUSH_CACHE);
    if (wait_bsy_clear() != 0) return -1;
    if (inb(ATA_STATUS) & (STATUS_ERR | STATUS_DF)) return -1;
    return 0;
}

int ata_present(void){
    if (wait_bsy_clear() != 0) return 0;
    outb(ATA_DRIVE_HEAD, 0xE0);
    select_delay();
    uint8_t st = inb(ATA_STATUS);
    // 0xFF is the floating bus with nothing driving it; 0x00 means no device
    // responded at all.
    return (st != 0xFF && st != 0x00);
}
