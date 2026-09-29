// src/ata.c — ATA PIO driver, primary bus, master drive, LBA28
//
// Reads and writes are the same shape: select the drive, program the LBA,
// issue the command, then move 256 words per sector through the data port.
// Writes add a cache flush at the end, without which the drive is entitled to
// report success and still lose the sector on power loss -- and QEMU is
// entitled not to update the backing file.
//
// Sector transfers use PCI bus-master DMA when the IDE controller offers it
// (second half of this file) and drop back to the PIO routines for any chunk
// DMA cannot or did not complete, so callers see the difference only in speed.
//
// Every wait here is bounded. The original spun on BSY forever, which is fine
// while nothing ever fails and a hard hang the first time there is no disk
// attached; the self-test runs in exactly that configuration.
#include <stdint.h>
#include "header/ata.h"
#include "header/io.h"
#include "header/pci.h"
#include "header/kstring.h"
#include "header/kprintf.h"

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

static int pio_read(uint32_t lba, uint8_t count, void* buf){
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

static int pio_write(uint32_t lba, uint8_t count, const void* buf){
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

// ------------------------------------------------------------------ DMA
//
// The PIIX-style IDE controller carries a bus-master engine whose registers
// sit in an I/O window named by BAR4: command at +0, status at +2 and the
// physical address of the Physical Region Descriptor table at +4. A transfer
// is: point the engine at a PRD table, issue READ/WRITE DMA to the drive, set
// the start bit, and wait for the engine to go idle with the interrupt bit
// raised. IRQ14 stays masked in the PIC, so completion is polled.
//
// The engine addresses physical memory and will not cross a 64K boundary
// within one PRD entry. Rather than allocate and hope, the bounce buffer is a
// static array aligned to 64K, which cannot straddle one, and the kernel is
// identity-mapped, so its virtual address is its physical address. Callers'
// buffers are copied through it: they may live anywhere, including on stacks.

#define ATA_CMD_READ_DMA  0xC8
#define ATA_CMD_WRITE_DMA 0xCA

#define BM_CMD    0
#define BM_STATUS 2
#define BM_PRD    4
#define BM_CMD_START 0x01
#define BM_CMD_READ  0x08      // engine writes to memory (drive -> host)
#define BM_ST_ACTIVE 0x01
#define BM_ST_ERROR  0x02
#define BM_ST_IRQ    0x04

#define DMA_CHUNK_SECTORS 64u
#define DMA_CHUNK_BYTES   (DMA_CHUNK_SECTORS * 512u)

typedef struct { uint32_t addr; uint16_t bytes; uint16_t flags; } prd_t;

static uint8_t dma_buf[DMA_CHUNK_BYTES] __attribute__((aligned(65536)));
// 8 bytes at 64-byte alignment can never cross a 64K boundary either.
static prd_t   dma_prd __attribute__((aligned(64)));

static int      dma_probed;
static uint16_t bm_base;            // 0 = no usable bus-master engine
static int      dma_enabled = 1;    // user switch (`ata dma off`)
static uint32_t st_dma_ops, st_pio_ops, st_fallbacks, st_errors;

// Reads and writes may arrive from any task, and the drive and the bounce
// buffer are single resources. Interrupts are off only for the instant it
// takes to test-and-set; the transfer itself runs with them on, so ticks keep
// counting and a competing task simply spins until the timer preempts it.
static volatile int ata_busy;

static int ata_lock(void){
    for (;;){
        uint32_t f;
        __asm__ __volatile__("pushf; pop %0; cli" : "=r"(f) :: "memory");
        if (!ata_busy){
            ata_busy = 1;
            if (f & 0x200u) __asm__ __volatile__("sti" ::: "memory");
            return 1;
        }
        if (!(f & 0x200u)) return 0;        // cannot be preempted to make progress: go unlocked
        __asm__ __volatile__("sti; pause" ::: "memory");
    }
}

static void ata_unlock(int held){ if (held) ata_busy = 0; }

static void dma_probe(void){
    if (dma_probed) return;
    dma_probed = 1;

    pci_device_t d;
    if (pci_find_class(0x01, 0x01, &d) != 0) return;
    // Bit 7: bus-master capable. Bit 0: primary channel in native mode, whose
    // task-file ports would not be the 0x1F0 block this driver programs.
    if (!(d.prog_if & 0x80) || (d.prog_if & 0x01)) return;
    if (!(d.bar[4] & 1u)) return;                       // must be an I/O BAR
    uint16_t base = (uint16_t)(d.bar[4] & 0xFFFCu);
    if (!base) return;

    pci_enable_bus_master(&d);
    bm_base = base;
    kprintf("[ata] IDE bus-master DMA at io %x (pci %x:%x)\n",
            (unsigned)base, (unsigned)d.vendor_id, (unsigned)d.device_id);
}

static void dma_stop(void){
    outb((uint16_t)(bm_base + BM_CMD), 0);
    outb((uint16_t)(bm_base + BM_STATUS), BM_ST_ERROR | BM_ST_IRQ);   // write-1-to-clear
}

// One transfer of at most DMA_CHUNK_SECTORS. 0 on success, -1 on any failure,
// after which the caller redoes the chunk with PIO.
static int dma_chunk(int write, uint32_t lba, uint8_t count, void* buf){
    uint16_t cmdp = (uint16_t)(bm_base + BM_CMD), stp = (uint16_t)(bm_base + BM_STATUS);
    uint32_t bytes = (uint32_t)count * 512u;
    uint8_t dir = write ? 0 : BM_CMD_READ;

    if (write) memcpy(dma_buf, buf, bytes);

    dma_prd.addr  = (uint32_t)dma_buf;
    dma_prd.bytes = (uint16_t)bytes;        // <= 32K, so never the 0 == 64K encoding
    dma_prd.flags = 0x8000;                 // end of table

    dma_stop();
    outl((uint16_t)(bm_base + BM_PRD), (uint32_t)&dma_prd);
    outb(cmdp, dir);

    if (issue(lba, count, write ? ATA_CMD_WRITE_DMA : ATA_CMD_READ_DMA) != 0){ dma_stop(); return -1; }
    outb(cmdp, (uint8_t)(dir | BM_CMD_START));

    int done = 0, bad = 0;
    for (uint32_t i = 0; i < ATA_SPIN; i++){
        uint8_t st = inb(stp);
        if (st & BM_ST_ERROR){ bad = 1; break; }
        if ((st & BM_ST_IRQ) && !(st & BM_ST_ACTIVE)){ done = 1; break; }
    }
    dma_stop();
    if (!done || bad) return -1;

    if (wait_bsy_clear() != 0) return -1;
    if (inb(ATA_STATUS) & (STATUS_ERR | STATUS_DF)) return -1;

    if (write){
        outb(ATA_COMMAND, ATA_CMD_FLUSH_CACHE);
        if (wait_bsy_clear() != 0) return -1;
        if (inb(ATA_STATUS) & (STATUS_ERR | STATUS_DF)) return -1;
    } else {
        memcpy(buf, dma_buf, bytes);
    }
    return 0;
}

static int dma_live(void){ return bm_base && dma_enabled; }

// The one path every public call funnels through. DMA where it is available,
// PIO for everything else, and PIO again for any chunk DMA fumbled. Three
// errors over the machine's life and the engine is written off for good.
static int rw(int write, uint32_t lba, uint8_t count, void* buf){
    if (count == 0) return 0;
    int held = ata_lock();
    dma_probe();

    int r = 0;
    if (!dma_live()){
        r = write ? pio_write(lba, count, buf) : pio_read(lba, count, buf);
        st_pio_ops++;
    } else {
        uint8_t* p = (uint8_t*)buf;
        while (count && r == 0){
            uint8_t n = count > DMA_CHUNK_SECTORS ? (uint8_t)DMA_CHUNK_SECTORS : count;
            if (dma_chunk(write, lba, n, p) == 0){
                st_dma_ops++;
            } else {
                st_errors++;
                st_fallbacks++;
                if (st_errors >= 3 && bm_base){
                    kprintf("[ata] DMA failing, falling back to PIO permanently\n");
                    bm_base = 0;
                }
                r = write ? pio_write(lba, n, p) : pio_read(lba, n, p);
                st_pio_ops++;
            }
            lba += n; count = (uint8_t)(count - n); p += (uint32_t)n * 512u;
        }
    }
    ata_unlock(held);
    return r;
}

int ata_read_sectors(uint32_t lba, uint8_t count, void* buf){ return rw(0, lba, count, buf); }
int ata_write_sectors(uint32_t lba, uint8_t count, const void* buf){ return rw(1, lba, count, (void*)buf); }

// Forced-path variants for tests and the benchmark: no fallback, so a result
// says which engine actually moved the bytes.
int ata_read_sectors_pio(uint32_t lba, uint8_t count, void* buf){
    int held = ata_lock();
    int r = pio_read(lba, count, buf);
    ata_unlock(held);
    return r;
}

int ata_read_sectors_dma(uint32_t lba, uint8_t count, void* buf){
    int held = ata_lock();
    dma_probe();
    int r = -1;
    if (bm_base){
        r = 0;
        uint8_t* p = (uint8_t*)buf;
        while (count && r == 0){
            uint8_t n = count > DMA_CHUNK_SECTORS ? (uint8_t)DMA_CHUNK_SECTORS : count;
            r = dma_chunk(0, lba, n, p);
            lba += n; count = (uint8_t)(count - n); p += (uint32_t)n * 512u;
        }
    }
    ata_unlock(held);
    return r;
}

int ata_write_sectors_dma(uint32_t lba, uint8_t count, const void* buf){
    int held = ata_lock();
    dma_probe();
    int r = -1;
    if (bm_base){
        r = 0;
        const uint8_t* p = (const uint8_t*)buf;
        while (count && r == 0){
            uint8_t n = count > DMA_CHUNK_SECTORS ? (uint8_t)DMA_CHUNK_SECTORS : count;
            r = dma_chunk(1, lba, n, (void*)p);
            lba += n; count = (uint8_t)(count - n); p += (uint32_t)n * 512u;
        }
    }
    ata_unlock(held);
    return r;
}

int ata_dma_available(void){
    int held = ata_lock();
    dma_probe();
    ata_unlock(held);
    return bm_base != 0;
}
int  ata_dma_enabled(void){ return dma_enabled && bm_base; }
void ata_dma_enable(int on){ dma_enabled = on != 0; }

void ata_stats(ata_stats_t* out){
    out->dma_ops = st_dma_ops; out->pio_ops = st_pio_ops;
    out->fallbacks = st_fallbacks; out->errors = st_errors;
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
