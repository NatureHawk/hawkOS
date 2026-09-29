// src/rtl8139.c — Realtek RTL8139 Ethernet driver
//
// The card DMAs received frames into one flat ring buffer and reads
// transmitted frames straight out of host memory, so both buffers live in
// .bss: the kernel identity-maps all of RAM, which makes a virtual address
// its own physical address and removes any need for a translation step
// before handing a pointer to the hardware.
#include <stdint.h>
#include "header/rtl8139.h"
#include "header/irqctl.h"
#include "header/pci.h"
#include "header/io.h"
#include "header/kprintf.h"
#include "header/kstring.h"
#include "header/sync.h"

#define RTL_VENDOR 0x10EC
#define RTL_DEVICE 0x8139

// Register offsets from the I/O base.
#define REG_IDR0      0x00
#define REG_TSD0      0x10        // four transmit status registers, 4 bytes apart
#define REG_TSAD0     0x20        // four transmit buffer addresses
#define REG_RBSTART   0x30
#define REG_CMD       0x37
#define REG_CAPR      0x38
#define REG_IMR       0x3C
#define REG_ISR       0x3E
#define REG_TCR       0x40
#define REG_RCR       0x44
#define REG_CONFIG1   0x52

#define CMD_RESET     0x10
#define CMD_RX_ENABLE 0x08
#define CMD_TX_ENABLE 0x04
#define CMD_RX_EMPTY  0x01

// Accept broadcast, multicast, unicast-to-us and runt/error frames, with
// WRAP set so a frame that runs off the end of the ring is written past it
// contiguously instead of being split — which is why the buffer is
// over-allocated by a whole MTU below.
//
// Bits 11-12 pick the ring size: 10 is 32 KB. The original 8 KB ring held
// five full frames, and a server answering into a 32 KB TCP window sends
// twenty back to back -- so every burst overflowed the ring between two
// polls, and a video stream spent more time in retransmission than in
// transfer. (64 KB is the one size WRAP is not honoured for, hence 32.)
#define RCR_CONFIG    (0x0000068Fu | (2u << 11))

#define RX_BUF_LEN    32768
#define RX_BUF_PAD    16
#define RX_BUF_TOTAL  (RX_BUF_LEN + RX_BUF_PAD + 1536)

#define TX_DESCS      4

static uint8_t rx_buf[RX_BUF_TOTAL] __attribute__((aligned(16)));
static uint8_t tx_buf[TX_DESCS][2048] __attribute__((aligned(16)));

static uint16_t io_base   = 0;
static int      present   = 0;
static uint8_t  mac[ETH_ALEN];
static uint16_t rx_offset = 0;
static int      tx_next   = 0;

int rtl8139_present(void){ return present; }
const uint8_t* rtl8139_mac(void){ return mac; }

int rtl8139_init(void){
    pci_device_t dev;
    if (pci_find(RTL_VENDOR, RTL_DEVICE, &dev) != 0){
        kprintf("[rtl8139] no RTL8139 found on the PCI bus\n");
        return -1;
    }

    // BAR0 with bit 0 set is an I/O-space BAR; the base is the rest of it.
    if (!(dev.bar[0] & 1u)){
        kprintf("[rtl8139] BAR0 is memory-mapped, this driver needs the I/O BAR\n");
        return -1;
    }
    io_base = (uint16_t)(dev.bar[0] & 0xFFFCu);

    pci_enable_bus_master(&dev);

    outb(io_base + REG_CONFIG1, 0x00);          // power on, leave sleep mode

    outb(io_base + REG_CMD, CMD_RESET);
    for (int spin = 0; spin < 1000000; spin++)
        if (!(inb(io_base + REG_CMD) & CMD_RESET)) break;
    if (inb(io_base + REG_CMD) & CMD_RESET){
        kprintf("[rtl8139] reset did not complete\n");
        return -1;
    }

    memset(rx_buf, 0, sizeof(rx_buf));
    outl(io_base + REG_RBSTART, (uint32_t)(uintptr_t)rx_buf);

    outl(io_base + REG_RCR, RCR_CONFIG);
    outl(io_base + REG_TCR, 0x03000700);        // default IFG, 16-byte DMA burst

    // Receive is polled, so every interrupt source stays masked. The ISR is
    // still written back below in the poll loop to keep the card's status
    // bits from latching.
    outw(io_base + REG_IMR, 0x0000);

    outb(io_base + REG_CMD, CMD_RX_ENABLE | CMD_TX_ENABLE);

    for (int i = 0; i < ETH_ALEN; i++) mac[i] = inb(io_base + REG_IDR0 + i);

    rx_offset = 0;
    tx_next   = 0;
    present   = 1;

    kprintf("[rtl8139] up at io=%x irq=%u mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
            io_base, dev.irq_line, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 0;
}

// TSD bit 13 is the OWN flag, and its sense is the opposite of what the
// name suggests: the card SETS it once it has finished pulling a frame out
// of host memory, so a descriptor is free to reuse exactly when it reads 1.
// It comes out of reset set, which is what makes the first four sends work.
#define TSD_OWN (1u << 13)

int rtl8139_send(const void* frame, uint16_t len){
    if (!present || len > 1792) return -1;

    // Ethernet requires a 60-byte minimum payload on the wire; the card does
    // not pad for us, and a short ARP frame would otherwise be dropped.
    uint16_t send_len = len < 60 ? 60 : len;

    for (int attempt = 0; attempt < TX_DESCS; attempt++){
        int d = tx_next;
        tx_next = (d + 1) % TX_DESCS;

        if (!(inl(io_base + REG_TSD0 + d * 4) & TSD_OWN)) continue;   // still busy

        memcpy(tx_buf[d], frame, len);
        if (send_len > len) memset(tx_buf[d] + len, 0, send_len - len);

        outl(io_base + REG_TSAD0 + d * 4, (uint32_t)(uintptr_t)tx_buf[d]);
        outl(io_base + REG_TSD0  + d * 4, send_len);   // clears OWN, starts the DMA
        return 0;
    }
    return -1;   // all four descriptors in flight
}

// Takes the next frame off the card's ring into buf. Returns its length, or
// 0 for an empty ring or a bad frame (which is consumed either way).
static uint16_t hw_take(uint8_t* buf, uint16_t cap){
    if (inb(io_base + REG_CMD) & CMD_RX_EMPTY) return 0;

    // Each entry in the ring is a 2-byte status word, a 2-byte length, then
    // the frame. The length includes the 4-byte Ethernet CRC, which the
    // stack above has no use for.
    uint16_t status = (uint16_t)(rx_buf[rx_offset] | (rx_buf[rx_offset + 1] << 8));
    uint16_t length = (uint16_t)(rx_buf[rx_offset + 2] | (rx_buf[rx_offset + 3] << 8));

    uint16_t out = 0;
    if ((status & 0x0001) && length >= 4 && length <= 1600){
        uint16_t data_len = (uint16_t)(length - 4);
        out = data_len < cap ? data_len : cap;
        // WRAP is set, so a frame is contiguous even where it runs past the
        // end of the ring -- into the padding the buffer was sized with.
        memcpy(buf, &rx_buf[rx_offset + 4], out);
    }

    // Advance past this entry, dword-aligned as the card requires.
    rx_offset = (uint16_t)((rx_offset + length + 4 + 3) & ~3u);
    rx_offset %= RX_BUF_LEN;

    // CAPR trails the read pointer by 16 bytes; the card's own documentation
    // calls this out and getting it wrong makes the ring appear permanently
    // full after the first wrap.
    outw(io_base + REG_CAPR, (uint16_t)(rx_offset - 16));

    outw(io_base + REG_ISR, 0x0005);   // clear ROK / RER
    return out;
}

// ------------------------------------------------------ receive queue
//
// The card's own ring holds 32 KB, which a server fills in well under a
// tenth of a second. Emptying it from the network task alone meant that when
// the task waited its turn behind a busy one -- the video decoder, flat out
// -- frames were dropped, the sender backed off, and a stream that had
// bandwidth to spare starved. So the timer interrupt empties the ring into
// this much larger queue every 10 ms, whatever the scheduler is doing, and
// the network task takes frames from the queue at its own pace.
//
// Each entry is a 2-byte length and the frame. head is advanced only by the
// producer (the interrupt, or rtl8139_poll with interrupts off) and tail only
// by the consumer, so neither needs a lock.
#define SWQ_SIZE (512u * 1024u)

static uint8_t           swq[SWQ_SIZE];
static volatile uint32_t swq_head = 0, swq_tail = 0;
static uint32_t          swq_dropped = 0;
static uint8_t           take_buf[1600];

static void swq_put(const uint8_t* src, uint32_t n){
    uint32_t at = swq_head % SWQ_SIZE;
    uint32_t first = SWQ_SIZE - at;
    if (first > n) first = n;
    memcpy(&swq[at], src, first);
    if (n > first) memcpy(swq, src + first, n - first);
}

static void swq_get(uint32_t from, uint8_t* dst, uint32_t n){
    uint32_t at = from % SWQ_SIZE;
    uint32_t first = SWQ_SIZE - at;
    if (first > n) first = n;
    memcpy(dst, &swq[at], first);
    if (n > first) memcpy(dst + first, swq, n - first);
}

// Interrupts must be off: called from the timer interrupt, and by
// rtl8139_poll inside an irq_save.
waitq_t rtl8139_rx_wq = WAITQ_INIT;

// Returns non-zero if it added frames. Split from the public entry point so
// the network task's own drain inside rtl8139_poll does not raise the wake-up
// it is about to wait on: the task is the consumer and loops until the queue
// is empty, so a wake from there would only make it spin under load.
static int drain_queue(void){
    if (!present) return 0;
    int queued = 0;
    for (int guard = 0; guard < 256; guard++){
        if (inb(io_base + REG_CMD) & CMD_RX_EMPTY) break;
        uint16_t n = hw_take(take_buf, sizeof(take_buf));
        if (!n) continue;
        if (SWQ_SIZE - (swq_head - swq_tail) < (uint32_t)n + 2u){ swq_dropped++; continue; }
        uint8_t len[2] = { (uint8_t)n, (uint8_t)(n >> 8) };
        swq_put(len, 2);
        swq_head += 2;
        swq_put(take_buf, n);
        swq_head += n;
        queued = 1;
    }
    return queued;
}

void rtl8139_drain(void){
    if (drain_queue()) waitq_wake_all(&rtl8139_rx_wq);
}

uint32_t rtl8139_dropped(void){ return swq_dropped; }

uint16_t rtl8139_poll(uint8_t* buf, uint16_t cap){
    if (!present) return 0;

    uint32_t f = irq_save();
    drain_queue();
    irq_restore(f);

    if (swq_head == swq_tail) return 0;
    uint8_t len[2];
    swq_get(swq_tail, len, 2);
    uint16_t n = (uint16_t)(len[0] | (len[1] << 8));
    uint16_t out = n < cap ? n : cap;
    swq_get(swq_tail + 2, buf, out);
    swq_tail += 2u + n;
    return out;
}
