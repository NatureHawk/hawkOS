#pragma once
#include <stdint.h>
#include "header/sync.h"

#define ETH_MTU      1514      // 14-byte header + 1500 payload, no CRC
#define ETH_ALEN     6

// Driver for the Realtek RTL8139, the NIC QEMU emulates most faithfully.
//
// Receive is polled rather than interrupt driven -- the card's IRQ line is
// assigned by the PCI bus at runtime, and polling removes that moving part --
// but it is polled from two places: the timer interrupt moves frames off the
// card into a 512 KB queue every tick (rtl8139_drain), so the card's small
// ring never overflows while the network task is waiting for the CPU, and the
// network task takes them from the queue (rtl8139_poll).
int  rtl8139_init(void);
int  rtl8139_present(void);

const uint8_t* rtl8139_mac(void);

// Queues one frame for transmission. Returns 0 on success, -1 if all four
// transmit descriptors are still busy.
int  rtl8139_send(const void* frame, uint16_t len);

// Pulls the next received frame into buf. Returns its length, or 0 if the
// receive ring is empty.
uint16_t rtl8139_poll(uint8_t* buf, uint16_t cap);

// Moves everything the card has received into the software queue. Interrupts
// must be off; the timer interrupt calls it every tick.
void     rtl8139_drain(void);

// Woken (from the timer interrupt, inside rtl8139_drain) whenever frames have
// been added to the software queue. The network task blocks here instead of
// polling; read waitq_seq() before draining the queue and wait with
// waitq_wait_seq() so a frame arriving in between is not missed.
extern waitq_t rtl8139_rx_wq;

// Frames lost because the software queue itself was full.
uint32_t rtl8139_dropped(void);
