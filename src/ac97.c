// src/ac97.c — Intel ICH AC'97 PCM output
//
// Two I/O windows: the codec's mixer registers (NAM, BAR0) and the
// controller's bus-master DMA engine (NABM, BAR1). Only the PCM-out channel
// of the DMA engine is used.
//
// Playback is a ring of 32 buffer descriptors. The hardware walks it from
// CIV (the one playing) up to LVI (the last one it may play) and halts there
// with DCH set if nothing further has been queued. Queueing a buffer is
// filling the descriptor after LVI and moving LVI onto it; the ring is full
// when that descriptor is the one playing. A halted channel resumes when LVI
// moves, so an underrun costs a gap in the sound and nothing else.
#include <stdint.h>
#include "header/ac97.h"
#include "header/pci.h"
#include "header/io.h"
#include "header/task.h"
#include "header/sync.h"
#include "header/kstring.h"
#include "header/kprintf.h"
#include "header/irqctl.h"

#define AC97_VENDOR 0x8086
#define AC97_DEVICE 0x2415

// Mixer (NAM)
#define NAM_RESET        0x00
#define NAM_MASTER_VOL   0x02
#define NAM_PCM_VOL      0x18
#define NAM_EXT_ID       0x28
#define NAM_EXT_CTRL     0x2A
#define NAM_FRONT_RATE   0x2C

// Bus master (NABM), PCM-out box at 0x10
#define PO_BDBAR  0x10
#define PO_CIV    0x14
#define PO_LVI    0x15
#define PO_SR     0x16
#define PO_PICB   0x18
#define PO_CR     0x1B
#define GLOB_CNT  0x2C

#define CR_RPBM   0x01      // run
#define CR_RR     0x02      // reset registers
#define SR_DCH    0x01      // halted
#define SR_CLEAR  0x1C      // write-1-to-clear status bits

#define BDL_N       32
#define BUF_FRAMES  2048     // stereo frames per descriptor: ~46 ms at 44.1 kHz, 1.5 s a ring
#define BUF_SAMPLES (BUF_FRAMES * 2)

typedef struct __attribute__((packed)) {
    uint32_t addr;
    uint16_t samples;       // 16-bit samples, not frames and not bytes
    uint16_t flags;
} bdl_entry_t;

static bdl_entry_t bdl[BDL_N] __attribute__((aligned(8)));
static int16_t     bufs[BDL_N][BUF_SAMPLES] __attribute__((aligned(16)));

static uint16_t nam = 0, nabm = 0;
static int      present = 0;
static int      running = 0;        // the DMA engine has been started since the last stop
static uint32_t rate = 48000;

// The descriptor being filled and how far into it.
static int      fill_idx = 0;
static uint32_t fill_pos = 0;       // samples

// The controller's interrupt is not wired up, so a writer waiting for a free
// descriptor naps on this queue with a timeout; ac97_stop wakes it early.
static waitq_t  space_wq = WAITQ_INIT;

int ac97_present(void){ return present; }
uint32_t ac97_rate(void){ return rate; }

static void reset_channel(void){
    outb(nabm + PO_CR, 0);
    outb(nabm + PO_CR, CR_RR);
    for (int i = 0; i < 100000 && (inb(nabm + PO_CR) & CR_RR); i++) io_wait();
    outw(nabm + PO_SR, SR_CLEAR);
    running  = 0;
    fill_idx = 0;
    fill_pos = 0;
    outl(nabm + PO_BDBAR, (uint32_t)(uintptr_t)bdl);
}

int ac97_init(void){
    pci_device_t dev;
    if (pci_find(AC97_VENDOR, AC97_DEVICE, &dev) != 0){
        kprintf("[ac97] no AC'97 controller\n");
        return -1;
    }
    if (!(dev.bar[0] & 1u) || !(dev.bar[1] & 1u)){
        kprintf("[ac97] BARs are not I/O space\n");
        return -1;
    }
    nam  = (uint16_t)(dev.bar[0] & 0xFFFCu);
    nabm = (uint16_t)(dev.bar[1] & 0xFFFCu);
    pci_enable_bus_master(&dev);

    // Take the link out of cold reset, then reset the codec itself.
    outl(nabm + GLOB_CNT, 0x00000002);
    for (int i = 0; i < 20000; i++) io_wait();
    outw(nam + NAM_RESET, 0x0001);
    for (int i = 0; i < 20000; i++) io_wait();

    // Both at zero attenuation. The spec puts PCM-out's 0 dB at 0x0808 (with
    // up to +12 dB of gain below it), but QEMU reads the register as pure
    // attenuation from 0: 0x0808 played everything a quarter quieter, which
    // showed up as a 0.71x level against the source track. On a real codec
    // 0x0000 is +12 dB -- this driver is written for QEMU's.
    outw(nam + NAM_MASTER_VOL, 0x0000);
    outw(nam + NAM_PCM_VOL,    0x0000);

    memset(bdl, 0, sizeof(bdl));
    for (int i = 0; i < BDL_N; i++) bdl[i].addr = (uint32_t)(uintptr_t)bufs[i];
    reset_channel();

    present = 1;
    ac97_set_rate(44100);
    kprintf("[ac97] up: nam=%x nabm=%x irq=%u, %u Hz\n", nam, nabm, dev.irq_line, rate);
    return 0;
}

uint32_t ac97_set_rate(uint32_t hz){
    if (!present) return 0;
    if (inw(nam + NAM_EXT_ID) & 1u){                     // variable rate audio
        outw(nam + NAM_EXT_CTRL, (uint16_t)(inw(nam + NAM_EXT_CTRL) | 1u));
        outw(nam + NAM_FRONT_RATE, (uint16_t)hz);
        rate = inw(nam + NAM_FRONT_RATE);
        if (rate == 0) rate = 48000;
    } else {
        rate = 48000;
    }
    return rate;
}

void ac97_set_volume(int percent){
    if (!present) return;
    if (percent <= 0){ outw(nam + NAM_MASTER_VOL, 0x8000); return; }
    if (percent > 100) percent = 100;
    // 6 bits of attenuation in 1.5 dB steps; the top of the range is where
    // the ear can tell the steps apart, so map linearly onto the lower 32.
    uint16_t att = (uint16_t)((100 - percent) * 32 / 100);
    outw(nam + NAM_MASTER_VOL, (uint16_t)((att << 8) | att));
}

// Hands descriptor fill_idx to the hardware and starts or resumes it.
static void commit(void){
    bdl_entry_t* e = &bdl[fill_idx];
    e->samples = (uint16_t)fill_pos;
    e->flags   = 0;

    uint32_t f = irq_save();
    outb(nabm + PO_LVI, (uint8_t)fill_idx);
    if (!running){
        outb(nabm + PO_CR, CR_RPBM);
        running = 1;
    }
    irq_restore(f);

    fill_idx = (fill_idx + 1) % BDL_N;
    fill_pos = 0;
}

// The descriptor about to be filled is still owned by the hardware when it is
// the one playing. Only a running, un-halted channel can be playing it.
static int slot_busy(int idx){
    if (!running) return 0;
    if (inw(nabm + PO_SR) & SR_DCH) return 0;
    return inb(nabm + PO_CIV) == idx;
}

uint32_t ac97_write(const int16_t* samples, uint32_t frames){
    if (!present) return 0;
    uint32_t done = 0;
    while (done < frames){
        if (fill_pos == 0){
            while (slot_busy(fill_idx)) waitq_wait(&space_wq, 10);
        }
        uint32_t room = (BUF_SAMPLES - fill_pos) / 2;
        uint32_t take = frames - done;
        if (take > room) take = room;
        memcpy(&bufs[fill_idx][fill_pos], samples + done * 2, take * 4);
        fill_pos += take * 2;
        done     += take;
        if (fill_pos == BUF_SAMPLES) commit();
    }
    return done;
}

uint32_t ac97_pending(void){
    if (!present || !running) return fill_pos / 2;
    uint32_t f = irq_save();
    uint16_t sr  = inw(nabm + PO_SR);
    uint8_t  civ = inb(nabm + PO_CIV);
    uint8_t  lvi = inb(nabm + PO_LVI);
    uint16_t picb = inw(nabm + PO_PICB);
    irq_restore(f);

    uint32_t queued = fill_pos / 2;
    if (sr & SR_DCH) return queued;
    queued += picb / 2u;
    queued += (uint32_t)((lvi - civ) & (BDL_N - 1)) * BUF_FRAMES;
    return queued;
}

// Clearing the run bit halts the DMA engine where it is, with everything
// queued kept; setting it again carries on from the same sample.
void ac97_pause(int on){
    if (!present || !running) return;
    uint32_t f = irq_save();
    outb(nabm + PO_CR, on ? 0 : CR_RPBM);
    irq_restore(f);
}

void ac97_stop(void){
    if (!present) return;
    uint32_t f = irq_save();
    reset_channel();
    irq_restore(f);
    waitq_wake_all(&space_wq);
}
